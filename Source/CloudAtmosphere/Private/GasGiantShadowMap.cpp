#include "GasGiantShadowMap.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderCompilerCore.h"

// IsFeatureLevelSupported, GBlackVolumeTexture.
#include "RenderUtils.h"

bool FGasGiantShadowBakeCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

void FGasGiantShadowBakeCS::ModifyCompilationEnvironment(
	const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	OutEnvironment.SetDefine(TEXT("GG_SHADOW_THREADS"), GasGiantShadow::ThreadGroupSize);
}

// Entry point name must match the [numthreads] function in GasGiantShadowMap.usf.
// A mismatch fails at cook time as a missing entry point rather than anywhere
// that names the cause.
IMPLEMENT_GLOBAL_SHADER(
	FGasGiantShadowBakeCS,
	"/Plugin/CloudAtmosphere/Private/GasGiantShadowMap.usf",
	"MainShadowBakeCS",
	SF_Compute);

namespace GasGiantShadow
{
	void AddBakePass_RenderThread(FRDGBuilder& GraphBuilder, const FGasGiantShadowParams& Params)
	{
		check(IsInRenderingThread());

		if (!Params.IsUsable())
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

		// WRAP U, CLAMP V, matching the sampler the material reads the same
		// texture with. The sim grid is a cylinder; wrapping V joins the north
		// pole to the south, which reads as a simulation bug rather than a
		// sampler one.
		P->FlowTargetSampler = TStaticSamplerState<SF_Bilinear, AM_Wrap, AM_Clamp, AM_Clamp>::GetRHI();

		// A missing volume binds black rather than refusing the bake. Black is a
		// defined value through GG_PerlinWorley, so the deck comes out uncarved
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
		auto DepthOrBlack = [](const FGasGiantOccluderFrame& Frame) -> FRHITexture*
			{
				return Frame.IsUsable()
					? Frame.DepthTexture.GetReference()
					: GBlackTexture->TextureRHI.GetReference();
			};

		for (int32 Level = 0; Level < CascadeCount; ++Level)
		{
			const FGasGiantOccluderFrame& Frame = Params.Occluders[Level];

			P->OccluderU[Level] = Frame.PackU();
			P->OccluderV[Level] = Frame.PackV();
			P->OccluderPlane[Level] = Frame.PackPlane();
		}

		P->OccluderDepth0 = DepthOrBlack(Params.Occluders[0]);
		P->OccluderDepth1 = DepthOrBlack(Params.Occluders[1]);
		P->OccluderDepth2 = DepthOrBlack(Params.Occluders[2]);

		// One slice per cascade on Z. The shader derives each level's extent and
		// centre from its own slice index, so nothing about a level crosses from
		// here and the bake cannot disagree with the march about where a slice sits.
		const FIntVector Groups(
			FMath::DivideAndRoundUp(Params.MapSize.X, ThreadGroupSize),
			FMath::DivideAndRoundUp(Params.MapSize.Y, ThreadGroupSize),
			CascadeCount);

		TShaderMapRef<FGasGiantShadowBakeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("GasGiant.ShadowBake"), Shader, P, Groups);
	}
}