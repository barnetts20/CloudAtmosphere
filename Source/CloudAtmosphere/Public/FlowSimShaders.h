#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

/** ONE PARAMETER STRUCT FOR EVERY KERNEL IN THE SIM. The kernels are stages of
 *  one pipeline over one set of resources, and their parameter lists overlap in
 *  a way that CHANGES TOGETHER: adding a forcing term touches the forcing kernel
 *  and the debug kernel that visualises it, and adding a grid parameter touches
 *  every one. A struct each means one place to edit and the rest to forget, and
 *  a forgotten one fails as an unbound-parameter warning that is easy to scroll
 *  past.
 *
 *  The cost is that each pass declares resources it does not use, but
 *  FComputeShaderUtils::AddPass calls ClearUnusedGraphResources, so RDG neither
 *  transitions nor lifetime-extends anything a pass does not touch. What is left
 *  is a few bytes of uniform buffer per dispatch.
 *
 *  Names must match the declarations in FlowSim.usf exactly. */
BEGIN_SHADER_PARAMETER_STRUCT(FFlowSimParameters, )

// -- Grid ---------------------------------------------------------------
SHADER_PARAMETER(FIntVector, SimGridSize)
SHADER_PARAMETER(FVector3f, SimInvGridSize)

// -- Profile ------------------------------------------------------------
SHADER_PARAMETER(FVector4f, SimJetParams)
SHADER_PARAMETER(float, SimWidthBias)
SHADER_PARAMETER_ARRAY(FVector4f, SimLayerProfile, [8])

// -- Time and rotation --------------------------------------------------
SHADER_PARAMETER(float, SimDeltaTime)
SHADER_PARAMETER(float, SimTime)
SHADER_PARAMETER(float, SimPlanetaryVorticity)

// -- Forcing volume -----------------------------------------------------
SHADER_PARAMETER(int32, SimForcingChannel)
SHADER_PARAMETER(int32, SimForcingBipolar)
SHADER_PARAMETER(int32, SimHasForcing)

// -- Forcing ------------------------------------------------------------
SHADER_PARAMETER(float, SimNudgeRate)
SHADER_PARAMETER(float, SimForcingAmplitude)
SHADER_PARAMETER(float, SimForcingScale)
SHADER_PARAMETER(FVector3f, SimForcingDrift)
SHADER_PARAMETER(float, SimDragRate)
SHADER_PARAMETER(float, SimLayerCoupling)

// -- Polar filter -------------------------------------------------------
SHADER_PARAMETER(float, SimFilterLatitude)
SHADER_PARAMETER(int32, SimFilterMaxHalfWidth)

// -- Solver -------------------------------------------------------------
SHADER_PARAMETER(float, SimRelaxation)
SHADER_PARAMETER(int32, SimRedBlackParity)

// -- Debug --------------------------------------------------------------
SHADER_PARAMETER(int32, SimDebugMode)
SHADER_PARAMETER(int32, SimDebugLayer)
SHADER_PARAMETER(float, SimDebugScale)
SHADER_PARAMETER(FIntPoint, SimDebugSize)

// -- Resources ----------------------------------------------------------
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float>, SimVorticitySRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float>, SimPsiSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, SimVelocitySRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SimRowMeanSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SimPsiRowMeanSRV)
SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, SimGlobalMeanSRV)

SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, SimVorticityUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, SimPsiUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float4>, SimVelocityUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, SimRowMeanUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, SimPsiRowMeanUAV)
SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, SimGlobalMeanUAV)
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

	// THE SHADER TAKES ITS GROUP SIZES FROM THESE. ModifyEnvironment pushes all
	// three as defines that FlowSim.usf's [numthreads] read, and
	// FlowSimulation.cpp sizes every dispatch from the same constants, so a
	// group size and its group count cannot disagree.

	/** Thread group edge for the 2D kernels. 8x8 = 64, a full wave on AMD and two
	 *  on NVIDIA. */
	static constexpr int32 ThreadGroupSize2D = 8;

	/** Thread group for the 1D per-row reductions, one thread per latitude row. */
	static constexpr int32 ThreadGroupSize1D = 64;

	/** Thread group for the per-layer reduction, one thread per layer. Small
	 *  because the stack is a handful of layers deep. */
	static constexpr int32 ThreadGroupSizeLayers = 8;
}

/** One class per entry point, all sharing FFlowSimParameters. A macro
 *  because the bodies are identical and a hand-written set would drift apart. If
 *  it ever trips over an engine change to SHADER_USE_PARAMETER_STRUCT, expanding
 *  it by hand is mechanical -- the contents are exactly what is written here. */
#define GG_DECLARE_SIM_SHADER(ClassName)                                                    \
	class ClassName : public FGlobalShader                                                  \
	{                                                                                       \
		DECLARE_GLOBAL_SHADER(ClassName);                                                   \
	public:                                                                                 \
		using FParameters = FFlowSimParameters;                                         \
		SHADER_USE_PARAMETER_STRUCT(ClassName, FGlobalShader);                              \
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)   \
		{                                                                                   \
			return FlowSimShader::ShouldCompile(P);                                     \
		}                                                                                   \
		static void ModifyCompilationEnvironment(                                           \
			const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& E)      \
		{                                                                                   \
			FlowSimShader::ModifyEnvironment(P, E);                                     \
		}                                                                                   \
	};

GG_DECLARE_SIM_SHADER(FFlowSimInitZonalPotentialCS)
GG_DECLARE_SIM_SHADER(FFlowSimInitPotentialCS)
GG_DECLARE_SIM_SHADER(FFlowSimInitVorticityCS)
GG_DECLARE_SIM_SHADER(FFlowSimVelocityCS)
GG_DECLARE_SIM_SHADER(FFlowSimAdvectCS)
GG_DECLARE_SIM_SHADER(FFlowSimReduceRowsCS)
GG_DECLARE_SIM_SHADER(FFlowSimReducePsiRowsCS)
GG_DECLARE_SIM_SHADER(FFlowSimReduceGlobalCS)
GG_DECLARE_SIM_SHADER(FFlowSimForceCS)
GG_DECLARE_SIM_SHADER(FFlowSimPolarFilterCS)
GG_DECLARE_SIM_SHADER(FFlowSimPoissonCS)
GG_DECLARE_SIM_SHADER(FFlowSimCaptureCS)
GG_DECLARE_SIM_SHADER(FFlowSimRestoreCS)
GG_DECLARE_SIM_SHADER(FFlowSimDebugVisCS)

#undef GG_DECLARE_SIM_SHADER