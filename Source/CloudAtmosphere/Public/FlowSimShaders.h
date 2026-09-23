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

// -- Time, rotation and gravity waves -----------------------------------
SHADER_PARAMETER(float, SimDeltaTime)
SHADER_PARAMETER(float, SimTime)
SHADER_PARAMETER(float, SimPlanetaryVorticity)
SHADER_PARAMETER(float, SimWaveSpeedSq)
SHADER_PARAMETER(float, SimImplicitWeight)
SHADER_PARAMETER(float, SimHelmholtzScale)

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
SHADER_PARAMETER(float, SimThermalRelaxation)

// -- Cloud tracer -------------------------------------------------------
SHADER_PARAMETER(float, SimCondensationRate)
SHADER_PARAMETER(float, SimEvaporationRate)
SHADER_PARAMETER(float, SimCloudDecay)

// -- Polar filter -------------------------------------------------------
SHADER_PARAMETER(float, SimFilterLatitude)
SHADER_PARAMETER(int32, SimFilterMaxHalfWidth)

// -- Output -------------------------------------------------------------
SHADER_PARAMETER(FVector3f, SimOutputScales)

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
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float2>, SimCloudSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float2>, SimRowMeanSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SimPhiEqSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SimGlobalMeanSRV)

SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float2>, SimFaceUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, SimCentreUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, SimExplicitUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, SimPhiUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, SimPhiStarUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, SimRhsUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float2>, SimSpectrumUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float2>, SimCloudUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, SimRowMeanUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, SimPhiEqUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, SimGlobalMeanUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, SimOutputUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, SimDebugUAV)

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

	/** Longitude columns for a requested width: the power of two at or below it,
	 *  within [32, MaxGridLongitude]. The transform is radix-2. */
	inline int32 GridLongitude(int32 Requested)
	{
		const uint32 Clamped = (uint32)FMath::Clamp(Requested, 32, MaxGridLongitude);
		return (int32)(1u << FMath::FloorLog2(Clamped));
	}

	inline int32 GridLatitude(int32 Requested)
	{
		return FMath::Clamp(Requested, 16, MaxGridLatitude);
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

#undef GG_DECLARE_SIM_SHADER