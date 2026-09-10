#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

class FRDGBuilder;

/** Everything the shadow bake reads, flattened for the render thread.
 *
 *  Copied into a render command, so it holds no UObject -- the same split
 *  FGasGiantSimParams draws. Filled from FGasGiantDeckParams' own getters, which
 *  is what keeps the deck the light sees identical to the deck the eye sees.
 *
 *  NOT PART OF FGasGiantSimParams. That struct is the fluid solver's state and
 *  changes when the solver does; this changes when the deck or the light does.
 *  Sharing one would put the first unrelated member into a struct whose whole
 *  justification is that its members change together. */
struct CLOUDATMOSPHERE_API FGasGiantShadowParams
{
	// -- Map ----------------------------------------------------------------

	FIntPoint MapSize = FIntPoint(512, 512);

	/** Half-width in world units. Set above the cull radius by
	 *  GasGiantShadow::ExtentMargin so the limb has texels outside the shell. */
	float Extent = 0.0f;

	// -- Frame --------------------------------------------------------------
	//
	// Planet-local. The light points TOWARD the star, matching the march.

	FVector3f LightDir = FVector3f(0.0f, 0.0f, 1.0f);
	FVector3f BasisU = FVector3f(1.0f, 0.0f, 0.0f);
	FVector3f BasisV = FVector3f(0.0f, 1.0f, 0.0f);

	FVector3f CameraLocal = FVector3f::ZeroVector;

	// -- Deck ---------------------------------------------------------------
	//
	// GG_BuildField's argument list, in its order. An addition there has to
	// appear here and in GasGiantShadowMap.usf.

	float PlanetRadius = 0.0f;
	float Time = 0.0f;
	float RotationWeight = 0.0f;

	FVector4f Scales = FVector4f::Zero();
	FVector4f Warps = FVector4f::Zero();
	FVector4f DetailNoise = FVector4f::Zero();
	FVector4f StructureNoise = FVector4f::Zero();

	float EdgeBias = 0.0f;
	float DeckSlope = 0.0f;

	FVector4f Crossfade = FVector4f::Zero();
	FVector4f Relief = FVector4f::Zero();
	FVector4f Profile = FVector4f::Zero();

	float BandSharpness = 0.0f;
	float ReliefThinning = 0.0f;
	float DetailVertical = 0.0f;
	float StructureVertical = 0.0f;
	float DetailErosion = 0.0f;
	float DetailRelief = 0.0f;
	float ErosionDepth = 0.0f;
	float DensityCurve = 0.0f;
	float StructureRelief = 0.0f;
	float StructureErosion = 0.0f;

	FVector4f FadeRanges = FVector4f::Zero();

	/** (ScatterNeg.a, ScatterPos.a, ScatterBase.a, BandScale). The per-band
	 *  extinction multiplier only. The albedo is a property of the scattering
	 *  site, not of the medium the light crossed, and stays per-pixel. */
	FVector4f ScatterAlphas = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);

	/** cloudAbsorptionBeta. Its fastest channel scales the accumulation so the
	 *  stored thresholds mean transmittance. */
	FVector3f AbsBeta = FVector3f::OneVector;

	// -- Resources ----------------------------------------------------------

	/** The sim's flow output. Read, never written. */
	FTextureRHIRef FlowTexture;

	/** The deck's noise volumes, from the actor's own properties -- these are
	 *  RHI handles rather than a second asset reference, since a compute pass
	 *  cannot reach a UObject. Either may be null; the pass binds black, which
	 *  is a defined value through the noise and gives an uncarved deck. */
	FTextureRHIRef DetailTexture;
	FTextureRHIRef StructureTexture;

	/** The bake's destination, pushed to the march as a material parameter. */
	FTextureRHIRef MapTexture;

	/** Whether the bake has everything it needs. Checked before the render
	 *  command is enqueued, since a params struct is cheaper to reject on the
	 *  game thread than a dispatch is to unwind on the render thread. */
	bool IsUsable() const
	{
		return FlowTexture.IsValid()
			&& MapTexture.IsValid()
			&& MapSize.X > 0
			&& MapSize.Y > 0
			&& Extent > 0.0f
			&& PlanetRadius > 0.0f;
	}
};

/** Names must match the declarations in GasGiantShadowMap.usf exactly. An
 *  unbound one is a warning that is easy to scroll past, which is the same
 *  silent-edit failure mode the checked material setters exist for. */
BEGIN_SHADER_PARAMETER_STRUCT(FGasGiantShadowParameters, )

SHADER_PARAMETER(FIntPoint, ShadowMapSize)
SHADER_PARAMETER(FVector2f, ShadowInvMapSize)

SHADER_PARAMETER(FVector3f, ShadowLightDir)
SHADER_PARAMETER(FVector3f, ShadowBasisU)
SHADER_PARAMETER(FVector3f, ShadowBasisV)
SHADER_PARAMETER(float, ShadowExtent)
SHADER_PARAMETER(FVector3f, ShadowCameraLocal)

SHADER_PARAMETER(float, ShadowPlanetRadius)
SHADER_PARAMETER(float, ShadowTime)
SHADER_PARAMETER(float, ShadowRotationWeight)
SHADER_PARAMETER(FVector4f, ShadowScales)
SHADER_PARAMETER(FVector4f, ShadowWarps)
SHADER_PARAMETER(FVector4f, ShadowDetailNoise)
SHADER_PARAMETER(FVector4f, ShadowStructureNoise)
SHADER_PARAMETER(float, ShadowEdgeBias)
SHADER_PARAMETER(float, ShadowDeckSlope)
SHADER_PARAMETER(FVector4f, ShadowCrossfade)
SHADER_PARAMETER(FVector4f, ShadowRelief)
SHADER_PARAMETER(FVector4f, ShadowProfile)
SHADER_PARAMETER(float, ShadowBandSharpness)
SHADER_PARAMETER(float, ShadowReliefThinning)
SHADER_PARAMETER(float, ShadowDetailVertical)
SHADER_PARAMETER(float, ShadowStructureVertical)
SHADER_PARAMETER(float, ShadowDetailErosion)
SHADER_PARAMETER(float, ShadowDetailRelief)
SHADER_PARAMETER(float, ShadowErosionDepth)
SHADER_PARAMETER(float, ShadowDensityCurve)
SHADER_PARAMETER(float, ShadowStructureRelief)
SHADER_PARAMETER(float, ShadowStructureErosion)
SHADER_PARAMETER(FVector4f, ShadowFadeRanges)
SHADER_PARAMETER(FVector4f, ShadowScatterAlphas)
SHADER_PARAMETER(FVector3f, ShadowAbsBeta)

SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ShadowMapUAV)

SHADER_PARAMETER_TEXTURE(Texture2DArray, ShadowFlowField)
SHADER_PARAMETER_SAMPLER(SamplerState, ShadowFlowSampler)

SHADER_PARAMETER_TEXTURE(Texture3D, ShadowDetailVolume)
SHADER_PARAMETER_SAMPLER(SamplerState, ShadowDetailSampler)

SHADER_PARAMETER_TEXTURE(Texture3D, ShadowStructureVolume)
SHADER_PARAMETER_SAMPLER(SamplerState, ShadowStructureSampler)

END_SHADER_PARAMETER_STRUCT()

class FGasGiantShadowBakeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGasGiantShadowBakeCS);

public:
	using FParameters = FGasGiantShadowParameters;
	SHADER_USE_PARAMETER_STRUCT(FGasGiantShadowBakeCS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

namespace GasGiantShadow
{
	/** Thread group edge. 8x8 = 64, matching the sim's 2D kernels. */
	static constexpr int32 ThreadGroupSize = 8;

	/** How far the map reaches past the cull radius. The limb is where the
	 *  encoded quantities vary fastest, and a map cut exactly at the shell puts
	 *  that variation against the clamp edge with nothing outside to blend
	 *  toward. */
	static constexpr float ExtentMargin = 1.02f;

	/** Fills BasisU and BasisV for a planet-local light direction.
	 *
	 *  ANCHORED TO THE SPIN AXIS rather than to an arbitrary perpendicular, so
	 *  the grid does not rotate between frames as the light moves. A rotating
	 *  lattice re-phases the reconstruction filter every frame, which shimmers
	 *  at the map's resolution limit even when nothing in the scene has moved.
	 *  Falls back to X when the light is along the pole. */
	CLOUDATMOSPHERE_API void BuildBasis(
		const FVector3f& LightDir, FVector3f& OutU, FVector3f& OutV);

	/** Adds the bake to the graph. The caller has already validated Params. */
	CLOUDATMOSPHERE_API void AddBakePass_RenderThread(
		FRDGBuilder& GraphBuilder, const FGasGiantShadowParams& Params);
}