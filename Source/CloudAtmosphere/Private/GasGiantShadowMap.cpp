#include "GasGiantShadowMap.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderCompilerCore.h"

// IsFeatureLevelSupported, GBlackVolumeTexture.
#include "RenderUtils.h"
#include "TextureResource.h"

bool FGasGiantShadowBakeCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

void FGasGiantShadowBakeCS::ModifyCompilationEnvironment(
	const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	OutEnvironment.SetDefine(TEXT("ATMO_BAKE_THREADS"), AtmoShadowBake::ThreadGroupSize);
}

// Entry point name must match the [numthreads] function in GasGiantShadowMap.usf.
// A mismatch fails at cook time as a missing entry point rather than anywhere
// that names the cause.
IMPLEMENT_GLOBAL_SHADER(
	FGasGiantShadowBakeCS,
	"/Plugin/CloudAtmosphere/Private/GasGiantShadowMap.usf",
	"MainShadowBakeCS",
	SF_Compute);

void FGasGiantShadowParams::ResolveTextures_RenderThread()
{
	check(IsInRenderingThread());

	FlowTexture = FlowResource ? FTextureRHIRef(FlowResource->GetRenderTargetTexture()) : FTextureRHIRef();
	MapTexture = MapResource ? FTextureRHIRef(MapResource->GetRenderTargetTexture()) : FTextureRHIRef();
	DetailTexture = DetailResource ? DetailResource->TextureRHI : FTextureRHIRef();
	StructureTexture = StructureResource ? StructureResource->TextureRHI : FTextureRHIRef();
}

namespace GasGiantShadow
{
	void AddBakePass_RenderThread(FRDGBuilder& GraphBuilder, const FGasGiantShadowParams& Params)
	{
		check(IsInRenderingThread());

		// Resolved by the caller; a resource still initialising has no handle.
		if (!Params.IsUsable() || !Params.FlowTexture.IsValid() || !Params.MapTexture.IsValid())
		{
			return;
		}

		RDG_EVENT_SCOPE(GraphBuilder, "GasGiantShadowBake");

		FRDGTextureRef Map = GraphBuilder.RegisterExternalTexture(
			CreateRenderTarget(Params.MapTexture, TEXT("GasGiant.ShadowMap")));

		FGasGiantShadowParameters* P = GraphBuilder.AllocParameters<FGasGiantShadowParameters>();

		P->ShadowMapSize = Params.MapSize;
		P->ShadowInvMapSize = FVector2f(
			1.0f / static_cast<float>(Params.MapSize.X),
			1.0f / static_cast<float>(Params.MapSize.Y));

		P->ShadowLightDir = Params.LightDir;
		P->ShadowCameraLocal = Params.CameraLocal;

		P->PlanetRadius = Params.PlanetRadius;
		P->HeightScale = Params.HeightScale;
		P->Time = Params.Time;
		P->DeckTop = Params.DeckTop;
		P->CeilingFalloff = Params.CeilingFalloff;
		P->GradientThickness = Params.GradientThickness;
		P->DeckBackstop = Params.DeckBackstop;
		P->DensityCurve = Params.DensityCurve;
		P->BandSharpness = Params.BandSharpness;
		P->BandBias = Params.BandBias;
		P->HemisphereBlend = Params.HemisphereBlend;
		P->HemisphereVariance = Params.HemisphereVariance;
		P->BandRelief = Params.BandRelief;
		P->PressureRelief = Params.PressureRelief;
		P->VortexThreshold = Params.VortexThreshold;
		P->StormTowerRelief = Params.StormTowerRelief;
		P->ReliefThinning = Params.ReliefThinning;
		P->RotationWeight = Params.RotationWeight;
		P->WarpTime = Params.WarpTime;
		P->DeepShearRatio = Params.DeepShearRatio;
		P->TurbulenceFloor = Params.TurbulenceFloor;
		P->CrossfadePeriod = Params.CrossfadePeriod;
		P->EdgeBias = Params.EdgeBias;
		P->ErosionDepth = Params.ErosionDepth;
		P->StructureNoiseWeights = Params.StructureNoiseWeights;
		P->StructureScale = Params.StructureScale;
		P->StructureAspect = Params.StructureAspect;
		P->StructureRelief = Params.StructureRelief;
		P->StructureErosion = Params.StructureErosion;
		P->StructureFlowInherit = Params.StructureFlowInherit;
		P->StructureShearInherit = Params.StructureShearInherit;
		P->StructureFadeNear = Params.StructureFadeNear;
		P->StructureFadeSpan = Params.StructureFadeSpan;
		P->StructureBandMix = Params.StructureBandMix;
		P->StructureCrossfade = Params.StructureCrossfade;
		P->DetailNoiseWeights = Params.DetailNoiseWeights;
		P->DetailScale = Params.DetailScale;
		P->DetailAspect = Params.DetailAspect;
		P->DetailRelief = Params.DetailRelief;
		P->DetailErosion = Params.DetailErosion;
		P->DetailFlowInherit = Params.DetailFlowInherit;
		P->DetailShearInherit = Params.DetailShearInherit;
		P->DetailFadeNear = Params.DetailFadeNear;
		P->DetailFadeSpan = Params.DetailFadeSpan;
		P->DetailBandMix = Params.DetailBandMix;
		P->DetailCrossfade = Params.DetailCrossfade;
		P->DeckSlope = Params.DeckSlope;

		P->ExtinctionNegative = Params.ExtinctionNegative;
		P->ExtinctionPositive = Params.ExtinctionPositive;
		P->ExtinctionBase = Params.ExtinctionBase;
		P->BandScale = Params.BandScale;
		P->DeckOpticalDepth = Params.DeckOpticalDepth;
		P->LightExtinctionFraction = Params.LightExtinctionFraction;

		P->ShadowMapUAV = GraphBuilder.CreateUAV(Map);

		P->FlowTarget = Params.FlowTexture;

		// Any address mode works: every face of the atlas carries its own gutter.
		P->FlowTargetSampler = TStaticSamplerState<SF_Bilinear, AM_Wrap, AM_Clamp, AM_Clamp>::GetRHI();

		// A missing volume binds black so the binding is complete. It is never
		// weighted in: the actor zeroes a layer's amount when it has no volume.
		P->DetailVolume = Params.DetailTexture.IsValid()
			? Params.DetailTexture
			: GBlackVolumeTexture->TextureRHI;

		P->StructureVolume = Params.StructureTexture.IsValid()
			? Params.StructureTexture
			: GBlackVolumeTexture->TextureRHI;

		// Wrap on all three axes, matching the material. Clamped, a tiling bake
		// reads a stretched band of constant value along each face.
		P->DetailVolumeSampler =
			TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();

		P->StructureVolumeSampler =
			TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();

		// One pass per level in the mask, one slice each. Only the index
		// crosses from here: the shader derives each level's extent and centre,
		// so the bake cannot disagree with the march about where a slice sits.
		const FIntVector Groups(
			FMath::DivideAndRoundUp(Params.MapSize.X, AtmoShadowBake::ThreadGroupSize),
			FMath::DivideAndRoundUp(Params.MapSize.Y, AtmoShadowBake::ThreadGroupSize),
			1);

		TShaderMapRef<FGasGiantShadowBakeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		for (int32 Level = 0; Level < AtmoShadowBake::CascadeCount; ++Level)
		{
			if ((Params.LevelMask & (1u << Level)) == 0)
			{
				continue;
			}

			auto* LevelP = GraphBuilder.AllocParameters<FGasGiantShadowBakeCS::FParameters>();
			*LevelP = *P;
			LevelP->ShadowFirstLevel = Level;

			// Copied even at weight 0, so the binding is always a written texture.
			const FAtmoShadowHistory& History = Params.History[Level];

			LevelP->ShadowHistory = AtmoShadowBake::AddHistoryCopy(GraphBuilder, Map, Level);
			LevelP->ShadowHistoryLightDir = History.LightDir;
			LevelP->ShadowHistoryCameraLocal = History.CameraLocal;
			LevelP->ShadowHistoryWeight = History.Weight;

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("GasGiant.ShadowBake Level %d", Level), Shader, LevelP, Groups);
		}
	}
}