#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

/** ONE PARAMETER STRUCT FOR EVERY KERNEL IN THE SIM. The kernels are stages of
 *  one pipeline over one set of resources, and their parameter lists change
 *  together. Each pass declares resources it does not use, but
 *  FComputeShaderUtils::AddPass clears unused graph resources, so RDG neither
 *  transitions nor lifetime-extends them.
 *
 *  Names must match the declarations in FlowSim.usf and FlowSimCommon.ush
 *  exactly. */
BEGIN_SHADER_PARAMETER_STRUCT(FFlowSimParameters, )

// -- Grid ---------------------------------------------------------------
SHADER_PARAMETER(FIntVector, SimGridSize)
SHADER_PARAMETER(FVector3f, SimInvGridSize)

// -- Profile ------------------------------------------------------------
SHADER_PARAMETER(FVector4f, SimJetParams)
SHADER_PARAMETER(float, SimWidthBias)
SHADER_PARAMETER(int32, SimZonalProfile)
SHADER_PARAMETER_ARRAY(FVector4f, SimLayerProfile, [8])

// -- Stack --------------------------------------------------------------
SHADER_PARAMETER_ARRAY(FVector4f, SimLayerState, [8])
SHADER_PARAMETER_ARRAY(FVector4f, SimMatMontgomery, [16])
SHADER_PARAMETER_ARRAY(FVector4f, SimMatMontgomeryInverse, [16])
SHADER_PARAMETER_ARRAY(FVector4f, SimMatModeToLayer, [16])
SHADER_PARAMETER_ARRAY(FVector4f, SimMatLayerToMode, [16])
SHADER_PARAMETER_ARRAY(FVector4f, SimMatModeToMontgomery, [16])

// -- Time, rotation and gravity waves -----------------------------------
SHADER_PARAMETER(float, SimDeltaTime)
SHADER_PARAMETER(uint32, SimForcingCycle)
SHADER_PARAMETER(float, SimForcingFraction)
SHADER_PARAMETER(float, SimNoiseClock)
SHADER_PARAMETER(float, SimPlanetaryVorticity)
SHADER_PARAMETER(float, SimWaveSpeedSq)
SHADER_PARAMETER(float, SimImplicitWeight)

// -- Forcing volume -----------------------------------------------------
SHADER_PARAMETER(int32, SimForcingChannel)
SHADER_PARAMETER(int32, SimForcingBipolar)
SHADER_PARAMETER(int32, SimHasForcing)

// -- Forcing ------------------------------------------------------------
SHADER_PARAMETER(float, SimNudgeRate)
SHADER_PARAMETER(float, SimForcingAmplitude)
SHADER_PARAMETER(float, SimForcingScale)
SHADER_PARAMETER(float, SimForcingLifetime)
SHADER_PARAMETER(float, SimDragRate)
SHADER_PARAMETER(float, SimLayerCoupling)
SHADER_PARAMETER(float, SimDivergenceDamping)
SHADER_PARAMETER(float, SimFroudeCeiling)
SHADER_PARAMETER(float, SimShockDamping)
SHADER_PARAMETER(int32, SimSharpCentre)
SHADER_PARAMETER(float, SimThermalRelaxation)
SHADER_PARAMETER(FVector4f, SimThermalParams)

// -- Moisture, cloud and storms -----------------------------------------
SHADER_PARAMETER(float, SimCondensationRate)
SHADER_PARAMETER(float, SimEvaporationRate)
SHADER_PARAMETER(float, SimCloudDecay)
SHADER_PARAMETER(FVector4f, SimMoistureParams)
SHADER_PARAMETER(float, SimLatentHeating)
SHADER_PARAMETER(float, SimAscentSmoothing)
SHADER_PARAMETER(FVector4f, SimStormParams)
SHADER_PARAMETER(float, SimWindEvaporation)

// -- Storm cells --------------------------------------------------------
SHADER_PARAMETER(FVector4f, SimCellShape)
SHADER_PARAMETER(FVector4f, SimCellVortex)
SHADER_PARAMETER(FVector4f, SimCellDraft)
SHADER_PARAMETER(FVector4f, SimCellLife)
SHADER_PARAMETER(FVector4f, SimCellMotion)
SHADER_PARAMETER(FVector4f, SimCellGenesis)
SHADER_PARAMETER(FVector4f, SimCellCloud)
SHADER_PARAMETER(float, SimCellWindBreadth)
SHADER_PARAMETER(float, SimCellSustain)
SHADER_PARAMETER(float, SimCellEyeDepth)
SHADER_PARAMETER(float, SimCellCoreFollow)
SHADER_PARAMETER(float, SimCellEyeLow)
SHADER_PARAMETER(float, SimCellEyeSoftness)
SHADER_PARAMETER(int32, SimCellCount)
SHADER_PARAMETER(int32, SimStepIndex)

// -- Noise coordinates --------------------------------------------------
SHADER_PARAMETER(float, SimNoiseDriftRate)
SHADER_PARAMETER(float, SimNoiseResetTime)

// -- Polar filter -------------------------------------------------------
SHADER_PARAMETER(float, SimFilterLatitude)
SHADER_PARAMETER(int32, SimFilterMaxHalfWidth)

// -- Output -------------------------------------------------------------
SHADER_PARAMETER(FVector3f, SimOutputScales)
SHADER_PARAMETER(int32, SimAtlasFaceSize)
SHADER_PARAMETER(float, SimStateBlend)

// -- Debug --------------------------------------------------------------
SHADER_PARAMETER(int32, SimDebugMode)
SHADER_PARAMETER(int32, SimDebugLayer)
SHADER_PARAMETER(float, SimDebugScale)
SHADER_PARAMETER(FIntPoint, SimDebugSize)

// -- Resources ----------------------------------------------------------
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float2>, SimFaceSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, SimCentreSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, SimExplicitSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float>, SimPhiSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float>, SimPhiStarSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float>, SimRhsSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float2>, SimSpectrumSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, SimTracerSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, SimNoiseSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, SimLatLonSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, SimCentreLatestSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, SimLatLonLatestSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, SimRowMeanSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SimMontgomeryEqSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SimGlobalMeanSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float2>, SimCellFlowSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, SimCellColumnSRV)

SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float2>, SimFaceUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, SimCentreUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, SimExplicitUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, SimPhiUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, SimPhiStarUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, SimRhsUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float2>, SimSpectrumUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, SimTracerUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, SimNoiseUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, SimLatLonUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, SimRowMeanUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, SimMontgomeryEqUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, SimGlobalMeanUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, SimOutputUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SimDebugUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float2>, SimCellFlowUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SimCellColumnUAV)

SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, SimCellSRV)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, SimCellUAV)
SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, SimRestoreBuffer)
SHADER_PARAMETER(uint32, SimRestoreFloatsPerCell)
SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, SimCaptureBuffer)

SHADER_PARAMETER_TEXTURE(Texture3D, SimForcingNoise)
SHADER_PARAMETER_SAMPLER(SamplerState, SimForcingNoiseSampler)

END_SHADER_PARAMETER_STRUCT()

namespace FlowSimShader
{
	CLOUDATMOSPHERE_API bool ShouldCompile(const FGlobalShaderPermutationParameters& Parameters);
	CLOUDATMOSPHERE_API void ModifyEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);

	// THE SHADER TAKES ITS GROUP SIZES FROM THESE. ModifyEnvironment pushes them
	// as defines that FlowSim.usf's [numthreads] read, and FlowSimulation.cpp
	// sizes every dispatch from the same constants.

	/** Thread group edge for the 2D kernels. 8x8 = 64. */
	static constexpr int32 ThreadGroupSize2D = 8;

	/** Thread group for the per-row reductions, one thread per latitude row. */
	static constexpr int32 ThreadGroupSize1D = 64;

	/** Thread group for the per-layer kernels, one thread per layer. */
	static constexpr int32 ThreadGroupSizeLayers = 8;

	/** Thread group for the Helmholtz transforms and column solve, one group per
	 *  row or wavenumber. Must divide MaxGridLatitude. */
	static constexpr int32 ThreadGroupSizeLine = 256;

	/** Widest row the transform holds in group shared memory. The sim needs a
	 *  power of two no wider than this. */
	static constexpr int32 MaxGridLongitude = 2048;

	/** Tallest column the latitude solve holds in group shared memory. */
	static constexpr int32 MaxGridLatitude = 1024;

	/** Storm cell slots, and the thread group of the pass that advances them.
	 *  PITFALL: UFlowSnapshot::CellFloats is sized from this too. */
	static constexpr int32 MaxStormCells = 32;

	/** Gutter texels around each atlas face. PITFALL: must equal
	 *  FLOW_ATLAS_GUTTER in FlowField.ush, which the materials read without
	 *  this define, or every face reads its neighbour's tile. */
	static constexpr int32 AtlasGutter = 4;

	/** The atlas: six faces with their gutters, packed 3 x 2. */
	inline FIntPoint AtlasSize(int32 FaceSize)
	{
		const int32 Tile = FaceSize + 2 * AtlasGutter;
		return FIntPoint(3 * Tile, 2 * Tile);
	}

	/** Range of UFlowSimConfig::GridResolution: 64 x 32 up to the transform's
	 *  and the column solve's limits. */
	static constexpr int32 MinGridResolution = 16;
	static constexpr int32 MaxGridResolution = 512;

	static_assert(4 * MaxGridResolution <= MaxGridLongitude && 2 * MaxGridResolution <= MaxGridLatitude,
		"The largest grid resolution exceeds the Helmholtz solve's limits.");

	/** The atlas face edge for a requested resolution: the power of two at or
	 *  below it, in range. Powers of two keep the columns radix-2 for the
	 *  transform and even for the polar fold. */
	inline int32 GridResolution(int32 Requested)
	{
		const uint32 Clamped = (uint32)FMath::Clamp(Requested, MinGridResolution, MaxGridResolution);
		return (int32)(1u << FMath::FloorLog2(Clamped));
	}

	/** Longitude columns: four atlas faces span the equator, so the output
	 *  matches the grid there. */
	inline int32 GridLongitude(int32 Resolution)
	{
		return 4 * GridResolution(Resolution);
	}

	/** Latitude rows, pole to pole. */
	inline int32 GridLatitude(int32 Resolution)
	{
		return 2 * GridResolution(Resolution);
	}
}

/** One class per entry point, all sharing FFlowSimParameters. A macro because
 *  the bodies are identical and a hand-written set would drift apart. */
#define GG_DECLARE_SIM_SHADER(ClassName)                                                    \
	class ClassName : public FGlobalShader                                                  \
	{                                                                                       \
		DECLARE_GLOBAL_SHADER(ClassName);                                                   \
	public:                                                                                 \
		using FParameters = FFlowSimParameters;                                             \
		SHADER_USE_PARAMETER_STRUCT(ClassName, FGlobalShader);                              \
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)   \
		{                                                                                   \
			return FlowSimShader::ShouldCompile(P);                                         \
		}                                                                                   \
		static void ModifyCompilationEnvironment(                                           \
			const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& E)      \
		{                                                                                   \
			FlowSimShader::ModifyEnvironment(P, E);                                         \
		}                                                                                   \
	};

GG_DECLARE_SIM_SHADER(FFlowSimInitBalanceCS)
GG_DECLARE_SIM_SHADER(FFlowSimInitStateCS)
GG_DECLARE_SIM_SHADER(FFlowSimReduceRowsCS)
GG_DECLARE_SIM_SHADER(FFlowSimReduceGlobalCS)
GG_DECLARE_SIM_SHADER(FFlowSimReconstructCS)
GG_DECLARE_SIM_SHADER(FFlowSimCellsCS)
GG_DECLARE_SIM_SHADER(FFlowSimCellFieldCS)
GG_DECLARE_SIM_SHADER(FFlowSimPredictCS)
GG_DECLARE_SIM_SHADER(FFlowSimFilterCS)
GG_DECLARE_SIM_SHADER(FFlowSimRhsCS)
GG_DECLARE_SIM_SHADER(FFlowSimHelmholtzForwardCS)
GG_DECLARE_SIM_SHADER(FFlowSimHelmholtzColumnCS)
GG_DECLARE_SIM_SHADER(FFlowSimHelmholtzInverseCS)
GG_DECLARE_SIM_SHADER(FFlowSimCorrectCS)
GG_DECLARE_SIM_SHADER(FFlowSimCaptureCS)
GG_DECLARE_SIM_SHADER(FFlowSimRestoreCS)
GG_DECLARE_SIM_SHADER(FFlowSimDebugVisCS)
GG_DECLARE_SIM_SHADER(FFlowSimResampleCS)

#undef GG_DECLARE_SIM_SHADER