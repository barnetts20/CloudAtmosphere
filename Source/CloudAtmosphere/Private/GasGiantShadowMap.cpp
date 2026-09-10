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

		P->ShadowPlanetRadius = Params.PlanetRadius;
		P->ShadowTime = Params.Time;
		P->ShadowRotationWeight = Params.RotationWeight;
		P->ShadowScales = Params.Scales;
		P->ShadowWarps = Params.Warps;
		P->ShadowDetailNoise = Params.DetailNoise;
		P->ShadowStructureNoise = Params.StructureNoise;
		P->ShadowEdgeBias = Params.EdgeBias;
		P->ShadowDeckSlope = Params.DeckSlope;
		P->ShadowCrossfade = Params.Crossfade;
		P->ShadowRelief = Params.Relief;
		P->ShadowProfile = Params.Profile;
		P->ShadowBandSharpness = Params.BandSharpness;
		P->ShadowReliefThinning = Params.ReliefThinning;
		P->ShadowDetailVertical = Params.DetailVertical;
		P->ShadowStructureVertical = Params.StructureVertical;
		P->ShadowDetailErosion = Params.DetailErosion;
		P->ShadowDetailRelief = Params.DetailRelief;
		P->ShadowErosionDepth = Params.ErosionDepth;
		P->ShadowDensityCurve = Params.DensityCurve;
		P->ShadowStructureRelief = Params.StructureRelief;
		P->ShadowStructureErosion = Params.StructureErosion;
		P->ShadowFadeRanges = Params.FadeRanges;
		P->ShadowScatterAlphas = Params.ScatterAlphas;
		P->ShadowAbsBeta = Params.AbsBeta;

		P->ShadowMapUAV = GraphBuilder.CreateUAV(Map);

		P->ShadowFlowField = Params.FlowTexture;

		// WRAP U, CLAMP V, matching the sampler the material reads the same
		// texture with. The sim grid is a cylinder; wrapping V joins the north
		// pole to the south, which reads as a simulation bug rather than a
		// sampler one.
		P->ShadowFlowSampler = TStaticSamplerState<SF_Bilinear, AM_Wrap, AM_Clamp, AM_Clamp>::GetRHI();

		// A missing volume binds black rather than refusing the bake. Black is a
		// defined value through GG_PerlinWorley, so the deck comes out uncarved
		// and the shadow is still broadly right -- diagnosable at a glance,
		// where a planet with no shadows at all looks like a broken pass.
		P->ShadowDetailVolume = Params.DetailTexture.IsValid()
			? Params.DetailTexture
			: GBlackVolumeTexture->TextureRHI;

		P->ShadowStructureVolume = Params.StructureTexture.IsValid()
			? Params.StructureTexture
			: GBlackVolumeTexture->TextureRHI;

		// Wrap on all three axes, matching the material. Clamped, a tiling bake
		// reads a stretched band of constant value along each face.
		P->ShadowDetailSampler =
			TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();

		P->ShadowStructureSampler =
			TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();

		const FIntVector Groups(
			FMath::DivideAndRoundUp(Params.MapSize.X, ThreadGroupSize),
			FMath::DivideAndRoundUp(Params.MapSize.Y, ThreadGroupSize),
			1);

		TShaderMapRef<FGasGiantShadowBakeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("GasGiant.ShadowBake"), Shader, P, Groups);
	}
}