#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

class FRDGBuilder;

namespace GasGiantShadow
{
	/** Thread group edge. 8x8 = 64, matching the sim's 2D kernels. Pushed to the
	 *  shader as GG_SHADOW_THREADS. */
	static constexpr int32 ThreadGroupSize = 8;

	/** Cascade count, sizing both the dispatch and the render target's slice
	 *  count. The shader decides what each level covers.
	 *
	 *  PITFALL: MUST MATCH GG_SHADOW_CASCADES in GasGiantShadow.ush, and is NOT
	 *  pushed as a define. The material reads that header too and never passes
	 *  through ModifyCompilationEnvironment, so a define set here would move the
	 *  bake without moving the march that reads it. Edit the pair together. */
	static constexpr int32 CascadeCount = 3;

	/** How far a depth capture reaches past the disc, as a multiple of the
	 *  planet's outer shell. ONLY A LOWER BOUND: the capture has to be at least
	 *  as wide as the shader's GG_SHADOW_EXTENT_MARGIN slice, and wider costs
	 *  resolution rather than correctness, so this does not have to track that
	 *  define exactly -- it must simply never be smaller. */
	static constexpr float CaptureExtentMargin = 1.02f;
}

/** One depth capture's placement, planet-local, as the bake reads it.
 *
 *  THE CAPTURE DESCRIBES ITSELF. U and V are the capture image's right and up
 *  axes, taken off the component's own transform rather than rebuilt from the
 *  light, so the bake cannot disagree with the capture about where a texel is
 *  and a mirrored basis is not expressible. Alignment with a cascade is a sizing
 *  convenience that makes the resample an identity; a mismatch costs resolution,
 *  never placement.
 *
 *  Render-thread safe: plain data plus an RHI handle, no UObject. */
struct CLOUDATMOSPHERE_API FGasGiantOccluderFrame
{
	FVector3f U = FVector3f(1.0f, 0.0f, 0.0f);
	FVector3f V = FVector3f(0.0f, 1.0f, 0.0f);

	/** Plane centre in (U, V), world units. */
	FVector2f Centre = FVector2f::ZeroVector;

	/** Half-width of the capture, world units. */
	float Extent = 0.0f;

	/** Capture plane's distance from the planet centre, along the light. */
	float PlaneDist = 0.0f;

	/** Far clip, world units from the plane. Background reads at or past it. */
	float Far = 0.0f;

	FTextureRHIRef DepthTexture;

	/** Set once the capture has actually rendered. A level that has never
	 *  captured must stay invalid: a cleared R32F target reads as depth zero,
	 *  which is an occluder sitting on the capture plane and shadows the whole
	 *  level. */
	bool bCaptured = false;

	bool IsUsable() const
	{
		return bCaptured && DepthTexture.IsValid() && Extent > 0.0f && Far > 0.0f;
	}

	// The three float4s the shader unpacks in AtmoOcc_MakeFrame. Changing a
	// layout here means changing it there; there is no binding that checks it.

	FVector4f PackU() const { return FVector4f(U.X, U.Y, U.Z, Extent); }

	FVector4f PackV() const { return FVector4f(V.X, V.Y, V.Z, PlaneDist); }

	FVector4f PackPlane() const
	{
		return FVector4f(Centre.X, Centre.Y, Far, IsUsable() ? 1.0f : 0.0f);
	}
};

/** Everything the shadow bake reads, flattened for the render thread.
 *
 *  Copied into a render command, so it holds no UObject -- the same split
 *  FGasGiantSimParams draws. Filled from the same deck groups and derivations
 *  ApplyGasGiantParams pushes to the material, which is what keeps the deck the
 *  light sees identical to the deck the eye sees.
 *
 *  NOT PART OF FGasGiantSimParams. That struct is the fluid solver's state and
 *  changes when the solver does; this changes when the deck or the light does.
 *  Sharing one would put the first unrelated member into a struct whose whole
 *  justification is that its members change together. */
struct CLOUDATMOSPHERE_API FGasGiantShadowParams
{
	// -- Map ----------------------------------------------------------------
	//
	// SQUARE. Cascade support is isotropic, so non-square texels would put the
	// bake's footprint and its entry back-off on the wrong scale along one axis.

	FIntPoint MapSize = FIntPoint(512, 512);

	// -- Frame --------------------------------------------------------------
	//
	// Planet-local. The light points TOWARD the star, matching the march.
	//
	// NO BASIS AND NO EXTENT. Both are derived from the light and the field by
	// GasGiantShadow.ush, which the bake and the march share -- a copy computed
	// here would be a second derivation that can disagree, and a map read in a
	// basis it was not written in gives smooth, plausible, misplaced shadows.

	FVector3f LightDir = FVector3f(0.0f, 0.0f, 1.0f);

	FVector3f CameraLocal = FVector3f::ZeroVector;

	// -- Deck ---------------------------------------------------------------
	//
	// GG_BuildField's arguments, in its order and under its names -- the same
	// names the material parameters carry. An addition there has to appear here
	// and in GasGiantShadowMap.usf.

	float PlanetRadius = 0.0f;
	float HeightScale = 0.0f;
	float Time = 0.0f;
	float DeckTop = 0.0f;
	float CeilingFalloff = 0.0f;
	float GradientThickness = 0.0f;
	float DeckBackstop = 0.0f;
	float DensityCurve = 0.0f;
	float BandSharpness = 0.0f;
	float BandBias = 0.0f;
	float HemisphereBlend = 0.0f;
	float HemisphereVariance = 0.0f;
	float BandRelief = 0.0f;
	float PressureRelief = 0.0f;
	float VortexThreshold = 0.0f;
	float StormTowerRelief = 0.0f;
	float ReliefThinning = 0.0f;
	float RotationWeight = 0.0f;
	float WarpTime = 0.0f;
	float DeepShearRatio = 0.0f;
	float TurbulenceFloor = 0.0f;
	float CrossfadePeriod = 0.0f;
	float EdgeBias = 0.0f;
	float ErosionDepth = 0.0f;
	FVector4f StructureNoiseWeights = FVector4f::Zero();
	float StructureScale = 0.0f;
	float StructureAspect = 0.0f;
	float StructureRelief = 0.0f;
	float StructureErosion = 0.0f;
	float StructureFlowInherit = 0.0f;
	float StructureShearInherit = 0.0f;
	float StructureFadeNear = 0.0f;
	float StructureFadeSpan = 0.0f;
	float StructureBandMix = 0.0f;
	float StructureCrossfade = 0.0f;
	FVector4f DetailNoiseWeights = FVector4f::Zero();
	float DetailScale = 0.0f;
	float DetailAspect = 0.0f;
	float DetailRelief = 0.0f;
	float DetailErosion = 0.0f;
	float DetailFlowInherit = 0.0f;
	float DetailShearInherit = 0.0f;
	float DetailFadeNear = 0.0f;
	float DetailFadeSpan = 0.0f;
	float DetailBandMix = 0.0f;
	float DetailCrossfade = 0.0f;
	float DeckSlope = 0.0f;

	// -- Extinction ---------------------------------------------------------
	//
	// Each band's rgb tint and amount in a, plus what GG_DeckBeta solves the
	// light ray's coefficient from. The albedo is a property of the scattering
	// site, not of the medium the light crossed, and stays per-pixel.

	FVector4f ExtinctionNegative = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
	FVector4f ExtinctionPositive = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
	FVector4f ExtinctionBase = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
	float BandScale = 1.0f;
	float DeckOpticalDepth = 0.0f;
	float LightExtinctionFraction = 0.0f;

	// -- Resources ----------------------------------------------------------

	/** The sim's flow output. Read, never written. */
	FTextureRHIRef FlowTexture;

	/** The deck's noise volumes, from the actor's own properties -- these are
	 *  RHI handles rather than a second asset reference, since a compute pass
	 *  cannot reach a UObject. Either may be null; the pass binds black, which
	 *  is a defined value through the noise and gives an uncarved deck. */
	FTextureRHIRef DetailTexture;
	FTextureRHIRef StructureTexture;

	/** The bake's destination: GasGiantShadow::CascadeCount slices, pushed to the
	 *  march as a single array parameter. Fewer slices leaves the inner cascades
	 *  unwritten and the march reads whatever the target held. */
	FTextureRHIRef MapTexture;

	// -- Occluders ----------------------------------------------------------

	/** One depth capture per cascade, each optional. NOT PART OF IsUsable: a
	 *  level with no capture disables itself and the deck still bakes, which is
	 *  the difference between a planet with no geometry shadows and a planet
	 *  with no shadows. */
	TStaticArray<FGasGiantOccluderFrame, GasGiantShadow::CascadeCount> Occluders;

	/** Whether the bake has everything it needs. Checked before the render
	 *  command is enqueued, since a params struct is cheaper to reject on the
	 *  game thread than a dispatch is to unwind on the render thread. */
	bool IsUsable() const
	{
		return FlowTexture.IsValid()
			&& MapTexture.IsValid()
			&& MapSize.X > 0
			&& MapSize.X == MapSize.Y
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
SHADER_PARAMETER(FVector3f, ShadowCameraLocal)

SHADER_PARAMETER(float, PlanetRadius)
SHADER_PARAMETER(float, HeightScale)
SHADER_PARAMETER(float, Time)
SHADER_PARAMETER(float, DeckTop)
SHADER_PARAMETER(float, CeilingFalloff)
SHADER_PARAMETER(float, GradientThickness)
SHADER_PARAMETER(float, DeckBackstop)
SHADER_PARAMETER(float, DensityCurve)
SHADER_PARAMETER(float, BandSharpness)
SHADER_PARAMETER(float, BandBias)
SHADER_PARAMETER(float, HemisphereBlend)
SHADER_PARAMETER(float, HemisphereVariance)
SHADER_PARAMETER(float, BandRelief)
SHADER_PARAMETER(float, PressureRelief)
SHADER_PARAMETER(float, VortexThreshold)
SHADER_PARAMETER(float, StormTowerRelief)
SHADER_PARAMETER(float, ReliefThinning)
SHADER_PARAMETER(float, RotationWeight)
SHADER_PARAMETER(float, WarpTime)
SHADER_PARAMETER(float, DeepShearRatio)
SHADER_PARAMETER(float, TurbulenceFloor)
SHADER_PARAMETER(float, CrossfadePeriod)
SHADER_PARAMETER(float, EdgeBias)
SHADER_PARAMETER(float, ErosionDepth)
SHADER_PARAMETER(FVector4f, StructureNoiseWeights)
SHADER_PARAMETER(float, StructureScale)
SHADER_PARAMETER(float, StructureAspect)
SHADER_PARAMETER(float, StructureRelief)
SHADER_PARAMETER(float, StructureErosion)
SHADER_PARAMETER(float, StructureFlowInherit)
SHADER_PARAMETER(float, StructureShearInherit)
SHADER_PARAMETER(float, StructureFadeNear)
SHADER_PARAMETER(float, StructureFadeSpan)
SHADER_PARAMETER(float, StructureBandMix)
SHADER_PARAMETER(float, StructureCrossfade)
SHADER_PARAMETER(FVector4f, DetailNoiseWeights)
SHADER_PARAMETER(float, DetailScale)
SHADER_PARAMETER(float, DetailAspect)
SHADER_PARAMETER(float, DetailRelief)
SHADER_PARAMETER(float, DetailErosion)
SHADER_PARAMETER(float, DetailFlowInherit)
SHADER_PARAMETER(float, DetailShearInherit)
SHADER_PARAMETER(float, DetailFadeNear)
SHADER_PARAMETER(float, DetailFadeSpan)
SHADER_PARAMETER(float, DetailBandMix)
SHADER_PARAMETER(float, DetailCrossfade)
SHADER_PARAMETER(float, DeckSlope)

SHADER_PARAMETER(FVector4f, ExtinctionNegative)
SHADER_PARAMETER(FVector4f, ExtinctionPositive)
SHADER_PARAMETER(FVector4f, ExtinctionBase)
SHADER_PARAMETER(float, BandScale)
SHADER_PARAMETER(float, DeckOpticalDepth)
SHADER_PARAMETER(float, LightExtinctionFraction)

SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, ShadowMapUAV)

SHADER_PARAMETER_TEXTURE(Texture2DArray, FlowTarget)
SHADER_PARAMETER_SAMPLER(SamplerState, FlowTargetSampler)

SHADER_PARAMETER_TEXTURE(Texture3D, DetailVolume)
SHADER_PARAMETER_SAMPLER(SamplerState, DetailVolumeSampler)

SHADER_PARAMETER_TEXTURE(Texture3D, StructureVolume)
SHADER_PARAMETER_SAMPLER(SamplerState, StructureVolumeSampler)

SHADER_PARAMETER_ARRAY(FVector4f, OccluderU, [GasGiantShadow::CascadeCount])
SHADER_PARAMETER_ARRAY(FVector4f, OccluderV, [GasGiantShadow::CascadeCount])
SHADER_PARAMETER_ARRAY(FVector4f, OccluderPlane, [GasGiantShadow::CascadeCount])

SHADER_PARAMETER_TEXTURE(Texture2D, OccluderDepth0)
SHADER_PARAMETER_TEXTURE(Texture2D, OccluderDepth1)
SHADER_PARAMETER_TEXTURE(Texture2D, OccluderDepth2)

SHADER_PARAMETER_SAMPLER(SamplerState, OccluderDepthSampler)

END_SHADER_PARAMETER_STRUCT()

// The captures are bound one name per level, and the shader walks them finest
// first through an unrolled chain, so a fourth cascade is not just a larger
// array. Caught here rather than as an unbound-parameter warning.
static_assert(GasGiantShadow::CascadeCount == 3,
	"OccluderDepth0..2 and GGShadow_Occlusion are written out per cascade.");

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
	/** Adds the bake to the graph, or does nothing when Params is unusable.
	 *  Render thread. */
	CLOUDATMOSPHERE_API void AddBakePass_RenderThread(
		FRDGBuilder& GraphBuilder, const FGasGiantShadowParams& Params);
}