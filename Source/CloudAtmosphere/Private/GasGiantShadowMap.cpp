#include "GasGiantShadowMap.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderCompilerCore.h"

// IsFeatureLevelSupported.
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
	void BuildBasis(const FVector3f& LightDir, FVector3f& OutU, FVector3f& OutV)
	{
		const FVector3f L = LightDir.GetSafeNormal();

		// The spin axis, projected off the light. Degenerate only when the light
		// is along the pole, where any perpendicular is as stable as any other.
		const FVector3f Axis = FVector3f(0.0f, 0.0f, 1.0f);

		FVector3f U = Axis - L * FVector3f::DotProduct(Axis, L);

		if (U.SizeSquared() < UE_KINDA_SMALL_NUMBER)
		{
			const FVector3f Fallback(1.0f, 0.0f, 0.0f);
			U = Fallback - L * FVector3f::DotProduct(Fallback, L);
		}

		OutU = U.GetSafeNormal();
		OutV = FVector3f::CrossProduct(L, OutU);
	}

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
		P->ShadowBasisU = Params.BasisU;
		P->ShadowBasisV = Params.BasisV;
		P->ShadowExtent = Params.Extent;
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

		P->ShadowMapUAV = GraphBuilder.CreateUAV(Map);

		P->ShadowFlowField = Params.FlowTexture;

		// WRAP U, CLAMP V, matching the sampler the material reads the same
		// texture with. The sim grid is a cylinder; wrapping V joins the north
		// pole to the south, which reads as a simulation bug rather than a
		// sampler one.
		P->ShadowFlowSampler = TStaticSamplerState<SF_Bilinear, AM_Wrap, AM_Clamp, AM_Clamp>::GetRHI();

		const FIntVector Groups(
			FMath::DivideAndRoundUp(Params.MapSize.X, ThreadGroupSize),
			FMath::DivideAndRoundUp(Params.MapSize.Y, ThreadGroupSize),
			1);

		TShaderMapRef<FGasGiantShadowBakeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("GasGiant.ShadowBake"), Shader, P, Groups);
	}
}
