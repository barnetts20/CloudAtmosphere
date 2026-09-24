#include "TerrestrialShadowMap.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderCompilerCore.h"

// IsFeatureLevelSupported, GBlackVolumeTexture.
#include "RenderUtils.h"

bool FTerrestrialShadowBakeCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

void FTerrestrialShadowBakeCS::ModifyCompilationEnvironment(
	const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	OutEnvironment.SetDefine(TEXT("ATMO_BAKE_THREADS"), AtmoShadowBake::ThreadGroupSize);
}

// Entry point name must match the [numthreads] function in TerrestrialShadowMap.usf.
// A mismatch fails at cook time as a missing entry point rather than anywhere
// that names the cause.
IMPLEMENT_GLOBAL_SHADER(
	FTerrestrialShadowBakeCS,
	"/Plugin/CloudAtmosphere/Private/TerrestrialShadowMap.usf",
	"MainShadowBakeCS",
	SF_Compute);

namespace TerrestrialShadow
{
	void AddBakePass_RenderThread(FRDGBuilder& GraphBuilder, const FTerrestrialShadowParams& Params)
	{
		check(IsInRenderingThread());

		if (!Params.IsUsable())
		{
			return;
		}

		RDG_EVENT_SCOPE(GraphBuilder, "TerrestrialShadowBake");

		FRDGTextureRef Map = GraphBuilder.RegisterExternalTexture(
			CreateRenderTarget(Params.MapTexture, TEXT("Terrestrial.ShadowMap")));

		FTerrestrialShadowParameters* P = GraphBuilder.AllocParameters<FTerrestrialShadowParameters>();

		P->ShadowMapSize = Params.MapSize;
		P->ShadowInvMapSize = FVector2f(
			1.0f / static_cast<float>(Params.MapSize.X),
			1.0f / static_cast<float>(Params.MapSize.Y));

		P->ShadowLightDir = Params.LightDir;
		P->ShadowCameraLocal = Params.CameraLocal;

		P->PlanetRadius = Params.PlanetRadius;
		P->HeightScale = Params.HeightScale;
		P->Time = Params.Time;
		P->CloudProfile = Params.CloudProfile;
		P->CloudCurves = Params.CloudCurves;
		P->CloudCoverage = Params.CloudCoverage;
		P->CloudType = Params.CloudType;
		P->CloudLid = Params.CloudLid;
		P->CloudLift = Params.CloudLift;
		P->CloudMotion = Params.CloudMotion;
		P->NoiseLevels = Params.NoiseLevels;
		P->StructureSampling = Params.StructureSampling;
		P->StructureWarp = Params.StructureWarp;
		P->DetailSampling = Params.DetailSampling;
		P->DetailWarp = Params.DetailWarp;
		P->CloudGenusStratus = Params.CloudGenusStratus;
		P->CloudGenusStratocumulus = Params.CloudGenusStratocumulus;
		P->CloudGenusCumulus = Params.CloudGenusCumulus;
		P->CloudGenusCirrus = Params.CloudGenusCirrus;
		P->ShadowCascades = Params.ShadowCascades;
		P->CloudResponse = Params.CloudResponse;

		P->CloudExtinction = Params.CloudExtinction;
		P->StormExtinction = Params.StormExtinction;
		P->CloudOpticalDepth = Params.CloudOpticalDepth;
		P->LightExtinctionFraction = Params.LightExtinctionFraction;

		P->ShadowMapUAV = GraphBuilder.CreateUAV(Map);

		P->FlowTarget = Params.FlowTexture;

		// WRAP U, CLAMP V, matching the sampler the material reads the same
		// texture with. The sim grid is a cylinder; wrapping V joins the north
		// pole to the south, which reads as a simulation bug rather than a
		// sampler one.
		P->FlowTargetSampler = TStaticSamplerState<SF_Bilinear, AM_Wrap, AM_Clamp, AM_Clamp>::GetRHI();

		// A missing volume binds black rather than refusing the bake. Black is a
		// defined value through the noise, so the cloud comes out uneroded
		// and the shadow is still broadly right -- diagnosable at a glance,
		// where a planet with no shadows at all looks like a broken pass.
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

		// -- Occluders ------------------------------------------------------
		//
		// POINT, NOT BILINEAR. Filtering across a depth discontinuity invents
		// intermediate depths that belong to no surface, and they read as a
		// shadow ramp trailing off every silhouette.
		P->OccluderDepthSampler =
			TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

		// Black stands in for a missing or not-yet-rendered capture so the
		// binding is always complete. It must never be READ: black is depth 0,
		// an occluder on the capture plane, which would shadow the whole level.
		// PackPlane's Valid short-circuits the lookup before any fetch.
		auto DepthOrBlack = [](const FAtmoOccluderFrame& Frame) -> FRHITexture*
			{
				return Frame.IsUsable()
					? Frame.DepthTexture.GetReference()
					: GBlackTexture->TextureRHI.GetReference();
			};

		for (int32 Level = 0; Level < AtmoShadowBake::CascadeCount; ++Level)
		{
			const FAtmoOccluderFrame& Frame = Params.Occluders[Level];

			P->OccluderU[Level] = Frame.PackU();
			P->OccluderV[Level] = Frame.PackV();
			P->OccluderPlane[Level] = Frame.PackPlane();
		}

		P->OccluderSoftness = Params.OccluderSoftness;
		P->OccluderInset = Params.OccluderInset;
		P->OccluderStrength = Params.OccluderStrength;
		P->OccluderFalloff = Params.OccluderFalloff;

		P->OccluderDepth0 = DepthOrBlack(Params.Occluders[0]);
		P->OccluderDepth1 = DepthOrBlack(Params.Occluders[1]);
		P->OccluderDepth2 = DepthOrBlack(Params.Occluders[2]);

		// One pass per level in the mask, one slice each. Only the index
		// crosses from here: the shader derives each level's extent and centre,
		// so the bake cannot disagree with the march about where a slice sits.
		const FIntVector Groups(
			FMath::DivideAndRoundUp(Params.MapSize.X, AtmoShadowBake::ThreadGroupSize),
			FMath::DivideAndRoundUp(Params.MapSize.Y, AtmoShadowBake::ThreadGroupSize),
			1);

		TShaderMapRef<FTerrestrialShadowBakeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

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

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("Terrestrial.ShadowBake Level %d", Level), Shader, LevelP, Groups);
		}
	}
}