#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "AtmosphereShadowBake.h"

class FRDGBuilder;

/** Everything the terrestrial bake reads, flattened for the render thread.
 *
 *  Copied into a render command, so it holds no UObject -- the same split
 *  FFlowSimParams draws. Filled from the same field groups and derivations
 *  ApplyMarchParams pushes to the material, which is what keeps the band
 *  the light sees identical to the band the eye sees.
 *
 *  NOT PART OF FFlowSimParams. That struct is the fluid solver's state and
 *  changes when the solver does; this changes when the deck or the light does.
 *  Sharing one would put the first unrelated member into a struct whose whole
 *  justification is that its members change together. */
struct CLOUDATMOSPHERE_API FTerrestrialShadowParams
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
	// TerrestrialShadow.ush, which the bake and the march share -- a copy computed
	// here would be a second derivation that can disagree, and a map read in a
	// basis it was not written in gives smooth, plausible, misplaced shadows.

	FVector3f LightDir = FVector3f(0.0f, 0.0f, 1.0f);

	FVector3f CameraLocal = FVector3f::ZeroVector;

	/** The cascades this request bakes, bit per level. The rest keep what they
	 *  last held, read against the camera they were baked with. */
	uint32 LevelMask = (1u << AtmoShadowBake::CascadeCount) - 1u;

	// -- Deck ---------------------------------------------------------------
	//
	// TR_BuildField's arguments, in its order and under its names -- the same
	// names the material parameters carry. An addition there has to appear here
	// and in TerrestrialShadowMap.usf.

	float PlanetRadius = 0.0f;
	float HeightScale = 0.0f;
	float Time = 0.0f;

	// Packed as TR_BuildField documents.
	FVector4f CloudProfile = FVector4f::Zero();
	FVector4f CloudCurves = FVector4f::Zero();
	FVector4f CloudCoverage = FVector4f::Zero();
	FVector4f CloudType = FVector4f::Zero();
	FVector4f CloudLid = FVector4f::Zero();
	FVector4f CloudLift = FVector4f::Zero();
	FVector4f CloudMotion = FVector4f::Zero();
	FVector4f NoiseLevels = FVector4f::Zero();
	FVector4f StructureSampling = FVector4f::Zero();
	FVector4f StructureWarp = FVector4f::Zero();
	FVector4f DetailSampling = FVector4f::Zero();
	FVector4f DetailWarp = FVector4f::Zero();
	FVector4f CloudGenusStratus = FVector4f::Zero();
	FVector4f CloudGenusStratocumulus = FVector4f::Zero();
	FVector4f CloudGenusCumulus = FVector4f::Zero();
	FVector4f CloudGenusCirrus = FVector4f::Zero();

	// -- Extinction ---------------------------------------------------------
	//
	// Both material sets' rgb tint and amount in a, plus what TR_CloudBeta
	// solves the light ray's coefficient from. The albedo is a property of the
	// scattering site, not of the medium the light crossed, and stays per-pixel.

	FVector4f CloudExtinction = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
	FVector4f StormExtinction = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
	float CloudOpticalDepth = 0.0f;
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

	/** The bake's destination: AtmoShadowBake::CascadeCount slices, pushed to the
	 *  march as a single array parameter. Fewer slices leaves the inner cascades
	 *  unwritten and the march reads whatever the target held. */
	FTextureRHIRef MapTexture;

	// -- Occluders ----------------------------------------------------------

	/** One depth capture per cascade, each optional. NOT PART OF IsUsable: a
	 *  level with no capture disables itself and the deck still bakes, which is
	 *  the difference between a planet with no geometry shadows and a planet
	 *  with no shadows. */
	TStaticArray<FAtmoOccluderFrame, AtmoShadowBake::CascadeCount> Occluders;

	/** Footprint radius in capture texels. Under about 0.5 the taps stay inside
	 *  one texel and the edge is as hard as the lattice allows; above that it is
	 *  a penumbra width. */
	float OccluderSoftness = 0.5f;

	/** Coverage below this contributes nothing, pulling the shadow's edge inward
	 *  against the outward spread of the footprint and the two filters after
	 *  it. */
	float OccluderInset = 0.5f;

	/** Optical depth a fully covered texel adds, on the fastest-channel scale
	 *  the map's thresholds already use. */
	float OccluderStrength = 10.0f;

	/** How far behind a blocker that decays to nothing, in atmosphere
	 *  thicknesses. Zero never decays. */
	float OccluderFalloff = 0.0f;

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

/** Names must match the .usf's declarations exactly -- this file's own, and
 *  the format half that AtmosphereShadowBake.ush declares. An
 *  unbound one is a warning that is easy to scroll past, which is the same
 *  silent-edit failure mode the checked material setters exist for. */
BEGIN_SHADER_PARAMETER_STRUCT(FTerrestrialShadowParameters, )

SHADER_PARAMETER(FIntPoint, ShadowMapSize)
SHADER_PARAMETER(FVector2f, ShadowInvMapSize)

SHADER_PARAMETER(FVector3f, ShadowLightDir)
SHADER_PARAMETER(FVector3f, ShadowCameraLocal)
SHADER_PARAMETER(int32, ShadowFirstLevel)

SHADER_PARAMETER(float, PlanetRadius)
SHADER_PARAMETER(float, HeightScale)
SHADER_PARAMETER(float, Time)
SHADER_PARAMETER(FVector4f, CloudProfile)
SHADER_PARAMETER(FVector4f, CloudCurves)
SHADER_PARAMETER(FVector4f, CloudCoverage)
SHADER_PARAMETER(FVector4f, CloudType)
SHADER_PARAMETER(FVector4f, CloudLid)
SHADER_PARAMETER(FVector4f, CloudLift)
SHADER_PARAMETER(FVector4f, CloudMotion)
SHADER_PARAMETER(FVector4f, NoiseLevels)
SHADER_PARAMETER(FVector4f, StructureSampling)
SHADER_PARAMETER(FVector4f, StructureWarp)
SHADER_PARAMETER(FVector4f, DetailSampling)
SHADER_PARAMETER(FVector4f, DetailWarp)
SHADER_PARAMETER(FVector4f, CloudGenusStratus)
SHADER_PARAMETER(FVector4f, CloudGenusStratocumulus)
SHADER_PARAMETER(FVector4f, CloudGenusCumulus)
SHADER_PARAMETER(FVector4f, CloudGenusCirrus)

SHADER_PARAMETER(FVector4f, CloudExtinction)
SHADER_PARAMETER(FVector4f, StormExtinction)
SHADER_PARAMETER(float, CloudOpticalDepth)
SHADER_PARAMETER(float, LightExtinctionFraction)

SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, ShadowMapUAV)

SHADER_PARAMETER_TEXTURE(Texture2DArray, FlowTarget)
SHADER_PARAMETER_SAMPLER(SamplerState, FlowTargetSampler)

SHADER_PARAMETER_TEXTURE(Texture3D, DetailVolume)
SHADER_PARAMETER_SAMPLER(SamplerState, DetailVolumeSampler)

SHADER_PARAMETER_TEXTURE(Texture3D, StructureVolume)
SHADER_PARAMETER_SAMPLER(SamplerState, StructureVolumeSampler)

SHADER_PARAMETER_ARRAY(FVector4f, OccluderU, [AtmoShadowBake::CascadeCount])
SHADER_PARAMETER_ARRAY(FVector4f, OccluderV, [AtmoShadowBake::CascadeCount])
SHADER_PARAMETER_ARRAY(FVector4f, OccluderPlane, [AtmoShadowBake::CascadeCount])

SHADER_PARAMETER_TEXTURE(Texture2D, OccluderDepth0)
SHADER_PARAMETER_TEXTURE(Texture2D, OccluderDepth1)
SHADER_PARAMETER_TEXTURE(Texture2D, OccluderDepth2)

SHADER_PARAMETER_SAMPLER(SamplerState, OccluderDepthSampler)

SHADER_PARAMETER(float, OccluderSoftness)
SHADER_PARAMETER(float, OccluderInset)
SHADER_PARAMETER(float, OccluderStrength)
SHADER_PARAMETER(float, OccluderFalloff)

END_SHADER_PARAMETER_STRUCT()

// The captures are bound one name per level, and the shader walks them finest
// first through an unrolled chain, so a fourth cascade is not just a larger
// array. Caught here rather than as an unbound-parameter warning.
static_assert(AtmoShadowBake::CascadeCount == 3,
	"OccluderDepth0..2 and TRShadow_Occlusion are written out per cascade.");

class FTerrestrialShadowBakeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTerrestrialShadowBakeCS);

public:
	using FParameters = FTerrestrialShadowParameters;
	SHADER_USE_PARAMETER_STRUCT(FTerrestrialShadowBakeCS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

namespace TerrestrialShadow
{
	/** Adds the bake to the graph, or does nothing when Params is unusable.
	 *  Render thread. */
	CLOUDATMOSPHERE_API void AddBakePass_RenderThread(
		FRDGBuilder& GraphBuilder, const FTerrestrialShadowParams& Params);
}