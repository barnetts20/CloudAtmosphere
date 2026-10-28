#pragma once

#include "CoreMinimal.h"
#include "Containers/StaticArray.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "ShaderPermutation.h"
#include "RenderGraphResources.h"
#include "AtmosphereShadowBake.h"

class FRDGBuilder;
class FTextureResource;
class FTextureRenderTargetResource;

/** The cloud field's authored values, both models': TR_BuildField's packed
 *  pins, in its order, and the cloud material's extinction. The planet and the
 *  light extinction arrive beside them, as each pass shares them with its other
 *  uniforms; the field's clocks arrive packed, already wrapped.
 *
 *  ONE STRUCT FOR THE MARCH AND THE BAKE, filled by one packer and included in
 *  both passes' parameters, so the field the light sees is the field the eye
 *  sees. Names must match AtmosphereMarchPass.usf and TerrestrialShadowMap.usf;
 *  an addition to TR_BuildField's signature has to appear in all three. */
BEGIN_SHADER_PARAMETER_STRUCT(FTerrestrialFieldParameters, )

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
SHADER_PARAMETER(FVector4f, ShadowCascades)
SHADER_PARAMETER(FVector4f, CloudResponse)

// The material sets' rgb tint and amount in a, and what TR_CloudBeta solves
// the cloud's coefficient from. DeepMaterial: the depth below the base the
// deep material takes over across, in atmosphere fractions, and the deep
// floor's softness in fills.
SHADER_PARAMETER(FVector4f, CloudExtinction)
SHADER_PARAMETER(FVector4f, StormExtinction)
SHADER_PARAMETER(FVector4f, DeepExtinction)
SHADER_PARAMETER(FVector4f, DeepMaterial)

// Each noise layer's StretchBias and StretchFlatten, structure then detail.
SHADER_PARAMETER(FVector4f, NoiseStretch)
SHADER_PARAMETER(float, CloudOpticalDepth)

END_SHADER_PARAMETER_STRUCT()

/** Everything the cloud bake reads, flattened for the render thread.
 *
 *  Copied into a render command, so it holds no UObject -- the same split
 *  FFlowSimParams draws. The field comes from the packer the march shares,
 *  which is what keeps the band the light sees identical to the band the eye
 *  sees.
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

	FIntPoint MapSize = FIntPoint::ZeroValue;

	/** The map's slices, one per cascade. */
	int32 MapSlices = 0;

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

	/** Per level: what its previous bake was made with and how much of it the
	 *  next one keeps. Read only for levels in LevelMask. */
	TStaticArray<FAtmoShadowHistory, AtmoShadowBake::CascadeCount> History;

	// -- Spread bake (Stage P1b) -------------------------------------------
	//
	// A level's bake taken over RowStride requests: each bakes every
	// RowStride-th block of AtmoShadowBake::ThreadGroupSize rows from RowPhase
	// into the staging slice, and the last one (bResolveStaging) copies it over
	// the level's slice. The level's slice, and the camera and light the march
	// reads it with, stay the previous bake's until then. One level per spread
	// request. Stride 1 and no staging: the whole level, written in place.

	int32 RowStride = 1;
	int32 RowPhase = 0;

	/** Writes the staging slice instead of the level's. */
	bool bStaged = false;

	/** Copies the staging slice over the level's after this request's rows. */
	bool bResolveStaging = false;

	/** Stage P1b2: each texel's subsamples on a quad of lanes (ATMO_BAKE_QUAD),
	 *  where the platform has wave operations; otherwise ignored. */
	bool bQuadSubsamples = true;

	// -- Field --------------------------------------------------------------

	/** The gas giant's deep deck (TR_DEEP_DECK), or the terrestrial slab. */
	bool bDeepDeck = false;

	float PlanetRadius = 0.0f;
	float HeightScale = 0.0f;

	FTerrestrialFieldParameters Field{};

	/** The light ray's share of the solved coefficient. The albedo is a
	 *  property of the scattering site, not of the medium the light crossed,
	 *  and stays with the march. */
	float LightExtinctionFraction = 0.0f;

	// -- Resources ----------------------------------------------------------
	//
	// RESOURCES ON THE GAME THREAD, RHI HANDLES ON THE RENDER THREAD. A handle
	// read on the game thread can be one the render thread is replacing; the
	// resource object is stable until a release enqueued after this request.
	// ResolveTextures_RenderThread fills the handles from the resources.

	/** The sim's flow output. Read, never written. */
	FTextureRenderTargetResource* FlowResource = nullptr;

	/** The deck's noise volumes, from the actor's own properties. Either may be
	 *  null; the pass then binds black. */
	FTextureResource* DetailResource = nullptr;
	FTextureResource* StructureResource = nullptr;

	/** The bake's destination, pushed to the march as a single array
	 *  parameter. PITFALL: with fewer than AtmoShadowBake::CascadeCount slices
	 *  the inner cascades go unwritten and the march reads whatever the target
	 *  held, so IsUsable refuses it. */
	FTextureRenderTargetResource* MapResource = nullptr;

	/** One texel the coverage pass writes and the bake and the march read. */
	FTextureRenderTargetResource* CoverageResource = nullptr;

	/** Stage P1b: one slice the map's size and format, which a spread bake
	 *  fills before it replaces a level. Needed only when bStaged. */
	FTextureRenderTargetResource* StagingResource = nullptr;

	/** Render thread only, filled by ResolveTextures_RenderThread. */
	FTextureRHIRef FlowTexture;
	FTextureRHIRef DetailTexture;
	FTextureRHIRef StructureTexture;
	FTextureRHIRef MapTexture;
	FTextureRHIRef CoverageTexture;
	FTextureRHIRef StagingTexture;

	void ResolveTextures_RenderThread();

	/** Whether the bake has everything it needs. Checked before the render
	 *  command is enqueued, since a params struct is cheaper to reject on the
	 *  game thread than a dispatch is to unwind on the render thread. */
	bool IsUsable() const
	{
		return FlowResource
			&& MapResource
			&& CoverageResource
			&& MapSize.X > 0
			&& MapSize.X == MapSize.Y
			&& MapSlices >= AtmoShadowBake::CascadeCount
			&& PlanetRadius > 0.0f
			&& RowStride >= 1
			&& RowPhase >= 0 && RowPhase < RowStride
			&& (!bStaged || (StagingResource && FMath::CountBits(LevelMask) == 1));
	}
};

/** Names must match the .usf's declarations exactly -- this file's own, and
 *  the format half that AtmosphereShadowBake.ush declares. An unbound one is a
 *  warning that is easy to scroll past. */
BEGIN_SHADER_PARAMETER_STRUCT(FTerrestrialShadowParameters, )

SHADER_PARAMETER(FIntPoint, ShadowMapSize)
SHADER_PARAMETER(FVector2f, ShadowInvMapSize)

SHADER_PARAMETER(FVector3f, ShadowLightDir)
SHADER_PARAMETER(FVector3f, ShadowCameraLocal)
SHADER_PARAMETER(int32, ShadowFirstLevel)
SHADER_PARAMETER(int32, ShadowRowStride)
SHADER_PARAMETER(int32, ShadowRowPhase)
SHADER_PARAMETER(int32, ShadowWriteSlice)
SHADER_PARAMETER(FVector3f, ShadowHistoryLightDir)
SHADER_PARAMETER(FVector3f, ShadowHistoryCameraLocal)
SHADER_PARAMETER(float, ShadowHistoryWeight)
SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float4>, ShadowHistory)

SHADER_PARAMETER(float, PlanetRadius)
SHADER_PARAMETER(float, HeightScale)
SHADER_PARAMETER_STRUCT_INCLUDE(FTerrestrialFieldParameters, Field)
SHADER_PARAMETER(float, LightExtinctionFraction)

SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, ShadowMapUAV)

SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray, FlowTarget)
SHADER_PARAMETER_SAMPLER(SamplerState, FlowTargetSampler)

SHADER_PARAMETER_TEXTURE(Texture3D, DetailVolume)
SHADER_PARAMETER_SAMPLER(SamplerState, DetailVolumeSampler)

SHADER_PARAMETER_TEXTURE(Texture3D, StructureVolume)
SHADER_PARAMETER_SAMPLER(SamplerState, StructureVolumeSampler)

SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, CoverageThreshold)

END_SHADER_PARAMETER_STRUCT()

/** The coverage pass: the field's pins and the flow in, one texel out. */
BEGIN_SHADER_PARAMETER_STRUCT(FTerrestrialCoverageParameters, )

SHADER_PARAMETER(float, PlanetRadius)
SHADER_PARAMETER(float, HeightScale)
SHADER_PARAMETER_STRUCT_INCLUDE(FTerrestrialFieldParameters, Field)

SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray, FlowTarget)
SHADER_PARAMETER_SAMPLER(SamplerState, FlowTargetSampler)

SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, CoverageUAV)

END_SHADER_PARAMETER_STRUCT()

/** The priority threshold covering CloudCover of the planet: MainCoverageCS,
 *  one group, ahead of the bake. Both models' priority is the same function,
 *  so it has no permutation. */
class FTerrestrialCoverageCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTerrestrialCoverageCS);

public:
	using FParameters = FTerrestrialCoverageParameters;
	SHADER_USE_PARAMETER_STRUCT(FTerrestrialCoverageCS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
};

class FTerrestrialShadowBakeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTerrestrialShadowBakeCS);

public:
	using FParameters = FTerrestrialShadowParameters;
	SHADER_USE_PARAMETER_STRUCT(FTerrestrialShadowBakeCS, FGlobalShader);

	/** The gas giant's deep deck. */
	class FDeepDeck : SHADER_PERMUTATION_BOOL("TR_DEEP_DECK");

	/** Stage P1b2: a texel's subsamples on a quad of lanes; compiled only for
	 *  platforms with wave operations. */
	class FQuadSubsamples : SHADER_PERMUTATION_BOOL("ATMO_BAKE_QUAD");

	using FPermutationDomain = TShaderPermutationDomain<FDeepDeck, FQuadSubsamples>;

	/** Whether this frame's bake can take the quad permutation: the platform
	 *  compiles it and the RHI runs wave operations. Render thread. */
	static bool SupportsQuadSubsamples();

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);
};

namespace TerrestrialShadow
{
	/** Adds the bake to the graph, or does nothing when Params is unusable.
	 *  Render thread; Params must already be resolved. */
	CLOUDATMOSPHERE_API void AddBakePass_RenderThread(
		FRDGBuilder& GraphBuilder, const FTerrestrialShadowParams& Params);
}