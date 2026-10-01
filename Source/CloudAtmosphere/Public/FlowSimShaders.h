#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "ShaderParameterMacros.h"

/** The sim's scalars, as one uniform buffer: built once per substep, and once
 *  for the frame's other passes, then shared by every pass that uses it
 *  rather than uploaded with each. The shader reads member Name through the
 *  alias SimName in FlowSimCommon.ush and FlowSim.usf.
 *
 *  PITFALL: A MEMBER NAMED LIKE ITS ALIAS BREAKS THE ENGINE'S PREPROCESSOR,
 *  which expands a self-referential macro twice when it is passed through
 *  another macro, as the stack matrices are through SIM_MATRIX. */
namespace FlowSimShader
{
	/** Perpetual storm slots: the uniform arrays' length, and the count
	 *  MainCellsCS updates on its first threads, so no more than
	 *  MaxStormCells. */
	static constexpr int32 MaxPerpetualStorms = 4;
}

BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FFlowSimUniformParameters, )

// -- Grid ---------------------------------------------------------------
SHADER_PARAMETER(FIntVector, GridSize)
SHADER_PARAMETER(FVector2f, InvGridSize)

// -- Profile ------------------------------------------------------------
SHADER_PARAMETER(FVector4f, JetParams)
SHADER_PARAMETER(float, WidthBias)
SHADER_PARAMETER(int32, ZonalProfile)
SHADER_PARAMETER(float, JetLatitudeScale)
SHADER_PARAMETER_ARRAY(FVector4f, LayerProfile, [8])

// -- Stack --------------------------------------------------------------
SHADER_PARAMETER_ARRAY(FVector4f, LayerState, [8])
SHADER_PARAMETER_ARRAY(FVector4f, MatMontgomery, [16])
SHADER_PARAMETER_ARRAY(FVector4f, MatMontgomeryInverse, [16])
SHADER_PARAMETER_ARRAY(FVector4f, MatModeToLayer, [16])
SHADER_PARAMETER_ARRAY(FVector4f, MatLayerToMode, [16])
SHADER_PARAMETER_ARRAY(FVector4f, MatModeToMontgomery, [16])

// -- Time, rotation and gravity waves -----------------------------------
SHADER_PARAMETER(float, DeltaTime)
SHADER_PARAMETER(uint32, ForcingCycle)
SHADER_PARAMETER(float, ForcingFraction)
SHADER_PARAMETER(float, NoiseClock)
SHADER_PARAMETER(float, PlanetaryVorticity)
SHADER_PARAMETER(float, WaveSpeedSq)
SHADER_PARAMETER(float, ImplicitWeight)

// -- Forcing volume -----------------------------------------------------
SHADER_PARAMETER(int32, ForcingChannel)
SHADER_PARAMETER(int32, HasForcing)

// -- Forcing ------------------------------------------------------------
SHADER_PARAMETER(float, NudgeRate)
SHADER_PARAMETER(float, ForcingAmplitude)
SHADER_PARAMETER(float, ForcingFrequency)
SHADER_PARAMETER(float, ForcingLifetime)
SHADER_PARAMETER(float, DragRate)
SHADER_PARAMETER(float, LayerCoupling)
SHADER_PARAMETER(float, DivergenceDamping)
SHADER_PARAMETER(float, FroudeCeiling)
SHADER_PARAMETER(float, ShockDamping)
SHADER_PARAMETER(int32, SharpCentre)
SHADER_PARAMETER(float, ThermalRelaxation)
SHADER_PARAMETER(FVector4f, ThermalParams)

// -- Moisture, cloud and storms -----------------------------------------
SHADER_PARAMETER(float, CondensationRate)
SHADER_PARAMETER(float, EvaporationRate)
SHADER_PARAMETER(float, CloudDecay)
SHADER_PARAMETER(FVector4f, MoistureParams)
SHADER_PARAMETER(float, LatentHeating)
SHADER_PARAMETER(float, AscentSmoothing)
SHADER_PARAMETER(FVector4f, StormParams)
SHADER_PARAMETER(float, WindEvaporation)

// -- Storm cells --------------------------------------------------------
SHADER_PARAMETER(FVector4f, CellShape)
SHADER_PARAMETER(FVector4f, CellVortex)
SHADER_PARAMETER(FVector4f, CellDraft)
SHADER_PARAMETER(FVector4f, CellLife)
SHADER_PARAMETER(FVector4f, CellMotion)
SHADER_PARAMETER(FVector4f, CellGenesis)
SHADER_PARAMETER(FVector4f, CellCloud)
SHADER_PARAMETER(float, CellWindBreadth)
SHADER_PARAMETER(float, CellSustain)
SHADER_PARAMETER(float, CellEyeDepth)
SHADER_PARAMETER(float, CellCoreFollow)
SHADER_PARAMETER(float, CellEyeSoftness)
SHADER_PARAMETER(int32, CellCount)
SHADER_PARAMETER(int32, StepIndex)

// -- Perpetual storms ---------------------------------------------------
SHADER_PARAMETER(int32, PerpetualCount)
SHADER_PARAMETER(float, PerpetualForcing)
SHADER_PARAMETER_ARRAY(FVector4f, PerpetualShape, [FlowSimShader::MaxPerpetualStorms])
SHADER_PARAMETER_ARRAY(FVector4f, PerpetualLook, [FlowSimShader::MaxPerpetualStorms])
SHADER_PARAMETER_ARRAY(FVector4f, PerpetualForm, [FlowSimShader::MaxPerpetualStorms])

// -- Noise coordinates --------------------------------------------------
SHADER_PARAMETER(float, NoiseDriftRate)
SHADER_PARAMETER(float, NoiseResetTime)

// -- Polar filter -------------------------------------------------------
SHADER_PARAMETER(float, FilterLatitude)

// -- Output -------------------------------------------------------------
SHADER_PARAMETER(FVector3f, OutputScales)
SHADER_PARAMETER(int32, AtlasFaceSize)
SHADER_PARAMETER(float, StateBlend)

// -- Debug --------------------------------------------------------------
SHADER_PARAMETER(int32, DebugMode)
SHADER_PARAMETER(int32, DebugLayer)
SHADER_PARAMETER(float, DebugScale)
SHADER_PARAMETER(FIntPoint, DebugSize)

END_GLOBAL_SHADER_PARAMETER_STRUCT()

/** ONE PARAMETER STRUCT FOR EVERY KERNEL IN THE SIM. The kernels are stages of
 *  one pipeline over one set of resources, and their parameter lists change
 *  together. Each pass declares resources it does not use, but
 *  FComputeShaderUtils::AddPass clears unused graph resources, so RDG neither
 *  transitions nor lifetime-extends them.
 *
 *  Names must match the declarations in FlowSim.usf exactly. */
BEGIN_SHADER_PARAMETER_STRUCT(FFlowSimParameters, )

SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FFlowSimUniformParameters, FlowSimUB)

// -- Per pass -----------------------------------------------------------
SHADER_PARAMETER(int32, SimReconstructLatest)
SHADER_PARAMETER(uint32, SimRestoreFloatsPerCell)

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

	static_assert(MaxPerpetualStorms <= MaxStormCells, "MainCellsCS updates the perpetual storms on its cell threads.");

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
#define FLOWSIM_DECLARE_SHADER(ClassName)                                                    \
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

FLOWSIM_DECLARE_SHADER(FFlowSimInitBalanceCS)
FLOWSIM_DECLARE_SHADER(FFlowSimInitStateCS)
FLOWSIM_DECLARE_SHADER(FFlowSimReduceRowsCS)
FLOWSIM_DECLARE_SHADER(FFlowSimReduceGlobalCS)
FLOWSIM_DECLARE_SHADER(FFlowSimReconstructCS)
FLOWSIM_DECLARE_SHADER(FFlowSimCellsCS)
FLOWSIM_DECLARE_SHADER(FFlowSimCellFieldCS)
FLOWSIM_DECLARE_SHADER(FFlowSimPredictCS)
FLOWSIM_DECLARE_SHADER(FFlowSimFilterCS)
FLOWSIM_DECLARE_SHADER(FFlowSimRhsCS)
FLOWSIM_DECLARE_SHADER(FFlowSimHelmholtzForwardCS)
FLOWSIM_DECLARE_SHADER(FFlowSimHelmholtzColumnCS)
FLOWSIM_DECLARE_SHADER(FFlowSimHelmholtzInverseCS)
FLOWSIM_DECLARE_SHADER(FFlowSimCorrectCS)
FLOWSIM_DECLARE_SHADER(FFlowSimCaptureCS)
FLOWSIM_DECLARE_SHADER(FFlowSimRestoreCS)
FLOWSIM_DECLARE_SHADER(FFlowSimDebugVisCS)
FLOWSIM_DECLARE_SHADER(FFlowSimResampleCS)

#undef FLOWSIM_DECLARE_SHADER