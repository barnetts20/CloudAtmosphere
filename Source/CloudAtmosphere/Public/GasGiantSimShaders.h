#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

/** ONE PARAMETER STRUCT FOR EVERY KERNEL IN THE SIM.
 *
 *  Eleven kernels sharing a struct rather than eleven structs, and this is a
 *  deliberate trade rather than laziness.
 *
 *  The kernels are stages of one pipeline over one set of resources. Their
 *  parameter lists overlap almost completely and, more to the point, they
 *  overlap in a way that CHANGES TOGETHER: adding a forcing term touches the
 *  forcing kernel and the debug kernel that visualises it, and adding a grid
 *  parameter touches all eleven. Eleven separate structs means eleven places to
 *  edit and ten places to forget, and a forgotten one fails as an unbound
 *  parameter warning that is easy to scroll past -- which is precisely the
 *  silent-edit failure mode this codebase has already been bitten by.
 *
 *  The cost is that each pass declares resources it does not use.
 *  FComputeShaderUtils::AddPass calls ClearUnusedGraphResources, which strips
 *  those before the graph sees them, so RDG neither transitions nor lifetime-
 *  extends anything a pass does not actually touch. The cost is therefore a few
 *  bytes of uniform buffer per dispatch and nothing else.
 *
 *  Names must match the declarations in GasGiantSim.usf exactly. */
BEGIN_SHADER_PARAMETER_STRUCT(FGasGiantSimParameters, )

// -- Grid ---------------------------------------------------------------
SHADER_PARAMETER(FIntVector, SimGridSize)
SHADER_PARAMETER(FVector3f, SimInvGridSize)

// -- Profile ------------------------------------------------------------
SHADER_PARAMETER(FVector4f, SimJetParams)
SHADER_PARAMETER(FVector4f, SimBandShape)
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

namespace GasGiantSimShader
{
	CLOUDATMOSPHERE_API bool ShouldCompile(const FGlobalShaderPermutationParameters& Parameters);
	CLOUDATMOSPHERE_API void ModifyEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);

	/** Thread group edge for the 2D kernels. 8x8 = 64, a full wave on AMD and
	 *  two on NVIDIA. */
	static constexpr int32 ThreadGroupSize2D = 8;

	/** Thread group for the 1D per-row reductions. */
	static constexpr int32 ThreadGroupSize1D = 64;
}

/** One class per entry point, all sharing FGasGiantSimParameters.
 *
 *  Written as a macro because the bodies are identical and a hand-written set
 *  of eleven would differ from each other eventually. If the macro ever trips
 *  over an engine change to SHADER_USE_PARAMETER_STRUCT, expanding it by hand
 *  is mechanical -- the contents are exactly what is written here. */
#define GG_DECLARE_SIM_SHADER(ClassName)                                                    \
	class ClassName : public FGlobalShader                                                  \
	{                                                                                       \
		DECLARE_GLOBAL_SHADER(ClassName);                                                   \
	public:                                                                                 \
		using FParameters = FGasGiantSimParameters;                                         \
		SHADER_USE_PARAMETER_STRUCT(ClassName, FGlobalShader);                              \
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& P)   \
		{                                                                                   \
			return GasGiantSimShader::ShouldCompile(P);                                     \
		}                                                                                   \
		static void ModifyCompilationEnvironment(                                           \
			const FGlobalShaderPermutationParameters& P, FShaderCompilerEnvironment& E)      \
		{                                                                                   \
			GasGiantSimShader::ModifyEnvironment(P, E);                                     \
		}                                                                                   \
	};

GG_DECLARE_SIM_SHADER(FGasGiantInitZonalPotentialCS)
GG_DECLARE_SIM_SHADER(FGasGiantInitPotentialCS)
GG_DECLARE_SIM_SHADER(FGasGiantInitVorticityCS)
GG_DECLARE_SIM_SHADER(FGasGiantVelocityCS)
GG_DECLARE_SIM_SHADER(FGasGiantAdvectCS)
GG_DECLARE_SIM_SHADER(FGasGiantReduceRowsCS)
GG_DECLARE_SIM_SHADER(FGasGiantReducePsiRowsCS)
GG_DECLARE_SIM_SHADER(FGasGiantReduceGlobalCS)
GG_DECLARE_SIM_SHADER(FGasGiantForceCS)
GG_DECLARE_SIM_SHADER(FGasGiantPolarFilterCS)
GG_DECLARE_SIM_SHADER(FGasGiantPoissonCS)
GG_DECLARE_SIM_SHADER(FGasGiantCaptureCS)
GG_DECLARE_SIM_SHADER(FGasGiantRestoreCS)
GG_DECLARE_SIM_SHADER(FGasGiantDebugVisCS)

#undef GG_DECLARE_SIM_SHADER