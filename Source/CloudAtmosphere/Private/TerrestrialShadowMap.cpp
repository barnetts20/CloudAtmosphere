#include "TerrestrialShadowMap.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderCompilerCore.h"

// IsFeatureLevelSupported, GBlackVolumeTexture.
#include "RenderUtils.h"
#include "TextureResource.h"
#include "RHIGlobals.h"

DECLARE_GPU_STAT(CloudAtmosphereShadowBake);   // P1a: its own row in stat gpu

bool FTerrestrialShadowBakeCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	if (!IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5))
	{
		return false;
	}

	// Stage P1b2: quad reads need wave operations.
	const FPermutationDomain Permutation(Parameters.PermutationId);

	return !Permutation.Get<FQuadSubsamples>() || RHISupportsWaveOperations(Parameters.Platform);
}

void FTerrestrialShadowBakeCS::ModifyCompilationEnvironment(
	const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	OutEnvironment.SetDefine(TEXT("ATMO_BAKE_THREADS"), AtmoShadowBake::ThreadGroupSize);

	const FPermutationDomain Permutation(Parameters.PermutationId);

	if (Permutation.Get<FQuadSubsamples>())
	{
		OutEnvironment.CompilerFlags.Add(CFLAG_WaveOperations);
	}
}

bool FTerrestrialShadowBakeCS::SupportsQuadSubsamples()
{
	return GRHISupportsWaveOperations && RHISupportsWaveOperations(GMaxRHIShaderPlatform);
}

// Entry point name must match the [numthreads] function in TerrestrialShadowMap.usf.
// A mismatch fails at cook time as a missing entry point rather than anywhere
// that names the cause.
IMPLEMENT_GLOBAL_SHADER(
	FTerrestrialShadowBakeCS,
	"/Plugin/CloudAtmosphere/Private/TerrestrialShadowMap.usf",
	"MainShadowBakeCS",
	SF_Compute);

bool FTerrestrialCoverageCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

IMPLEMENT_GLOBAL_SHADER(
	FTerrestrialCoverageCS,
	"/Plugin/CloudAtmosphere/Private/TerrestrialShadowMap.usf",
	"MainCoverageCS",
	SF_Compute);

void FTerrestrialShadowParams::ResolveTextures_RenderThread()
{
	check(IsInRenderingThread());

	FlowTexture = FlowResource ? FTextureRHIRef(FlowResource->GetRenderTargetTexture()) : FTextureRHIRef();
	MapTexture = MapResource ? FTextureRHIRef(MapResource->GetRenderTargetTexture()) : FTextureRHIRef();
	CoverageTexture = CoverageResource ? FTextureRHIRef(CoverageResource->GetRenderTargetTexture()) : FTextureRHIRef();
	StagingTexture = StagingResource ? FTextureRHIRef(StagingResource->GetRenderTargetTexture()) : FTextureRHIRef();
	DetailTexture = DetailResource ? DetailResource->TextureRHI : FTextureRHIRef();
	StructureTexture = StructureResource ? StructureResource->TextureRHI : FTextureRHIRef();
}

namespace TerrestrialShadow
{
	void AddBakePass_RenderThread(FRDGBuilder& GraphBuilder, const FTerrestrialShadowParams& Params)
	{
		check(IsInRenderingThread());

		// Resolved by the caller; a resource still initialising has no handle.
		if (!Params.IsUsable() || !Params.FlowTexture.IsValid() || !Params.MapTexture.IsValid()
			|| !Params.CoverageTexture.IsValid() || (Params.bStaged && !Params.StagingTexture.IsValid()))
		{
			return;
		}

		RDG_EVENT_SCOPE_STAT(GraphBuilder, CloudAtmosphereShadowBake, "CloudShadowBake");

		FRDGTextureRef Map = GraphBuilder.RegisterExternalTexture(
			CreateRenderTarget(Params.MapTexture, TEXT("Terrestrial.ShadowMap")));

		// Registered, so the graph puts it in a readable state like the map and coverage.
		FRDGTextureRef Flow = GraphBuilder.RegisterExternalTexture(
			CreateRenderTarget(Params.FlowTexture, TEXT("Terrestrial.Flow")));

		// Any address mode works: every face of the atlas carries its own gutter.
		FRHISamplerState* FlowSampler = TStaticSamplerState<SF_Bilinear, AM_Wrap, AM_Clamp, AM_Clamp>::GetRHI();

		// THE COVERAGE THRESHOLD FIRST: the bake below and this frame's march
		// read the texel it writes.
		FRDGTextureRef Coverage = GraphBuilder.RegisterExternalTexture(
			CreateRenderTarget(Params.CoverageTexture, TEXT("Terrestrial.Coverage")));
		{
			auto* CoverageP = GraphBuilder.AllocParameters<FTerrestrialCoverageCS::FParameters>();

			CoverageP->PlanetRadius = Params.PlanetRadius;
			CoverageP->HeightScale = Params.HeightScale;
			CoverageP->Field = Params.Field;
			CoverageP->FlowTarget = Flow;
			CoverageP->FlowTargetSampler = FlowSampler;
			CoverageP->CoverageUAV = GraphBuilder.CreateUAV(Coverage);

			TShaderMapRef<FTerrestrialCoverageCS> CoverageShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Terrestrial.Coverage"),
				CoverageShader, CoverageP, FIntVector(1, 1, 1));
		}

		FTerrestrialShadowParameters* P = GraphBuilder.AllocParameters<FTerrestrialShadowParameters>();

		P->ShadowMapSize = Params.MapSize;
		P->ShadowInvMapSize = FVector2f(
			1.0f / static_cast<float>(Params.MapSize.X),
			1.0f / static_cast<float>(Params.MapSize.Y));

		P->ShadowLightDir = Params.LightDir;
		P->ShadowCameraLocal = Params.CameraLocal;

		P->PlanetRadius = Params.PlanetRadius;
		P->HeightScale = Params.HeightScale;
		P->Field = Params.Field;
		P->LightExtinctionFraction = Params.LightExtinctionFraction;

		P->ShadowMapUAV = GraphBuilder.CreateUAV(Map);

		P->FlowTarget = Flow;
		P->FlowTargetSampler = FlowSampler;
		P->CoverageThreshold = Coverage;

		// A missing volume binds black so the binding is complete. The actor
		// marks a layer without a volume asset unread (NoiseLevels); one whose
		// resource is not yet created reads black for those frames.
		P->DetailVolume = Params.DetailTexture.IsValid()
			? Params.DetailTexture
			: GBlackVolumeTexture->TextureRHI;

		P->StructureVolume = Params.StructureTexture.IsValid()
			? Params.StructureTexture
			: GBlackVolumeTexture->TextureRHI;

		// Wrap on all three axes, matching the march. Clamped, a tiling bake
		// reads a stretched band of constant value along each face.
		P->DetailVolumeSampler =
			TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();

		P->StructureVolumeSampler =
			TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();

		// Stage P1b: a spread bake writes the staging slice, whole for the
		// level's row blocks this request takes; the level's slice is left for
		// the march until the last request copies the staging slice over it.
		FRDGTextureRef Staging = nullptr;

		if (Params.bStaged)
		{
			Staging = GraphBuilder.RegisterExternalTexture(
				CreateRenderTarget(Params.StagingTexture, TEXT("Terrestrial.ShadowStaging")));

			P->ShadowMapUAV = GraphBuilder.CreateUAV(Staging);
		}

		P->ShadowRowStride = Params.RowStride;
		P->ShadowRowPhase = Params.RowPhase;
		P->ShadowWriteSlice = Params.bStaged ? 0 : -1;

		// One pass per level in the mask, one slice each. Only the index
		// crosses from here: the shader derives each level's extent and centre,
		// so the bake cannot disagree with the march about where a slice sits.
		// A spread bake's groups cover its share of the row blocks.
		const int32 RowBlocks = FMath::DivideAndRoundUp(Params.MapSize.Y, AtmoShadowBake::ThreadGroupSize);

		// Stage P1b2: the quad permutation's groups are 64 lanes, a band of two
		// rows of a tile each, so four groups to a tile row (TerrestrialShadowMap.usf).
		const bool bQuad = Params.bQuadSubsamples && FTerrestrialShadowBakeCS::SupportsQuadSubsamples();

		const int32 TileRows = FMath::DivideAndRoundUp(FMath::Max(RowBlocks - Params.RowPhase, 0), Params.RowStride);

		const FIntVector Groups(
			FMath::DivideAndRoundUp(Params.MapSize.X, AtmoShadowBake::ThreadGroupSize),
			bQuad ? TileRows * 4 : TileRows,
			1);

		FTerrestrialShadowBakeCS::FPermutationDomain Permutation;
		Permutation.Set<FTerrestrialShadowBakeCS::FDeepDeck>(Params.bDeepDeck);
		Permutation.Set<FTerrestrialShadowBakeCS::FQuadSubsamples>(bQuad);

		TShaderMapRef<FTerrestrialShadowBakeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel), Permutation);

		for (int32 Level = 0; Level < AtmoShadowBake::CascadeCount; ++Level)
		{
			if ((Params.LevelMask & (1u << Level)) == 0)
			{
				continue;
			}

			auto* LevelP = GraphBuilder.AllocParameters<FTerrestrialShadowBakeCS::FParameters>();
			*LevelP = *P;
			LevelP->ShadowFirstLevel = Level;

			// Copied even at weight 0, so the binding is always a written texture.
			const FAtmoShadowHistory& History = Params.History[Level];

			LevelP->ShadowHistory = AtmoShadowBake::AddHistoryCopy(GraphBuilder, Map, Level);
			LevelP->ShadowHistoryLightDir = History.LightDir;
			LevelP->ShadowHistoryCameraLocal = History.CameraLocal;
			LevelP->ShadowHistoryWeight = History.Weight;

			if (Groups.Y > 0)
			{
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("Terrestrial.ShadowBake Level %d (rows %d/%d%s)", Level, Params.RowPhase, Params.RowStride, bQuad ? TEXT(", quad") : TEXT("")),
					Shader, LevelP, Groups);
			}

			// The spread bake's last request: the staging slice replaces the
			// level's, in the same graph as its last rows, so the march reads
			// either the whole previous bake or the whole new one.
			if (Staging && Params.bResolveStaging)
			{
				FRHICopyTextureInfo Copy;
				Copy.Size = FIntVector(Params.MapSize.X, Params.MapSize.Y, 1);
				Copy.SourceSliceIndex = 0;
				Copy.DestSliceIndex = Level;
				Copy.NumSlices = 1;

				AddCopyTexturePass(GraphBuilder, Staging, Map, Copy);
			}
		}
	}
}