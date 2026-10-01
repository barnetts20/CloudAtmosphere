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

// Both material sets' rgb tint and amount in a, and what TR_CloudBeta solves
// the cloud's coefficient from.
SHADER_PARAMETER(FVector4f, CloudExtinction)
SHADER_PARAMETER(FVector4f, StormExtinction)
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

	/** Per level: what its previous bake was made with and how much of it the
	 *  next one keeps. Read only for levels in LevelMask. */
	TStaticArray<FAtmoShadowHistory, AtmoShadowBake::CascadeCount> History;

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

	/** The bake's destination: AtmoShadowBake::CascadeCount slices, pushed to the
	 *  march as a single array parameter. Fewer slices leaves the inner cascades
	 *  unwritten and the march reads whatever the target held. */
	FTextureRenderTargetResource* MapResource = nullptr;

	/** Render thread only, filled by ResolveTextures_RenderThread. */
	FTextureRHIRef FlowTexture;
	FTextureRHIRef DetailTexture;
	FTextureRHIRef StructureTexture;
	FTextureRHIRef MapTexture;

	void ResolveTextures_RenderThread();

	/** Whether the bake has everything it needs. Checked before the render
	 *  command is enqueued, since a params struct is cheaper to reject on the
	 *  game thread than a dispatch is to unwind on the render thread. */
	bool IsUsable() const
	{
		return FlowResource
			&& MapResource
			&& MapSize.X > 0
			&& MapSize.X == MapSize.Y
			&& PlanetRadius > 0.0f;
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
SHADER_PARAMETER(FVector3f, ShadowHistoryLightDir)
SHADER_PARAMETER(FVector3f, ShadowHistoryCameraLocal)
SHADER_PARAMETER(float, ShadowHistoryWeight)
SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<float4>, ShadowHistory)

SHADER_PARAMETER(float, PlanetRadius)
SHADER_PARAMETER(float, HeightScale)
SHADER_PARAMETER_STRUCT_INCLUDE(FTerrestrialFieldParameters, Field)
SHADER_PARAMETER(float, LightExtinctionFraction)

SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, ShadowMapUAV)

SHADER_PARAMETER_TEXTURE(Texture2DArray, FlowTarget)
SHADER_PARAMETER_SAMPLER(SamplerState, FlowTargetSampler)

SHADER_PARAMETER_TEXTURE(Texture3D, DetailVolume)
SHADER_PARAMETER_SAMPLER(SamplerState, DetailVolumeSampler)

SHADER_PARAMETER_TEXTURE(Texture3D, StructureVolume)
SHADER_PARAMETER_SAMPLER(SamplerState, StructureVolumeSampler)

END_SHADER_PARAMETER_STRUCT()

class FTerrestrialShadowBakeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTerrestrialShadowBakeCS);

public:
	using FParameters = FTerrestrialShadowParameters;
	SHADER_USE_PARAMETER_STRUCT(FTerrestrialShadowBakeCS, FGlobalShader);

	/** The gas giant's deep deck. */
	class FDeepDeck : SHADER_PERMUTATION_BOOL("TR_DEEP_DECK");
	using FPermutationDomain = TShaderPermutationDomain<FDeepDeck>;

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