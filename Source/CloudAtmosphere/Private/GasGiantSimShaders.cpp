#include "GasGiantSimShaders.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "ShaderCompilerCore.h"

// IsFeatureLevelSupported.
#include "RenderUtils.h"

namespace GasGiantSimShader
{
	bool ShouldCompile(const FGlobalShaderPermutationParameters& Parameters)
	{
		// SM5 and up. Everything here is a plain compute dispatch with typed
		// UAV loads on R32F and RGBA16F, which is baseline for that feature
		// level and above.
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	void ModifyEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("GG_SIM_THREADS_2D"), ThreadGroupSize2D);
		OutEnvironment.SetDefine(TEXT("GG_SIM_THREADS_1D"), ThreadGroupSize1D);

		// The SOR sweep reads its own UAV, and the polar filter and the cubic
		// interpolator both index far enough from the thread's own texel that
		// the compiler cannot prove the accesses are in range. Neither is a
		// correctness problem -- SimWrapCoord folds every index -- but the
		// bounds analysis is what would otherwise force scalarisation.
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}
}

// Entry point names must match the [numthreads] functions in GasGiantSim.usf.
// A mismatch here fails at cook time as a missing entry point rather than
// anywhere useful, so they are listed adjacent for comparison.

#define GG_IMPLEMENT_SIM_SHADER(ClassName, EntryPoint) \
	IMPLEMENT_GLOBAL_SHADER(ClassName, "/Plugin/CloudAtmosphere/Private/GasGiantSim.usf", EntryPoint, SF_Compute)

GG_IMPLEMENT_SIM_SHADER(FGasGiantInitZonalPotentialCS, "MainInitZonalPotentialCS")
GG_IMPLEMENT_SIM_SHADER(FGasGiantInitPotentialCS, "MainInitPotentialCS")
GG_IMPLEMENT_SIM_SHADER(FGasGiantInitVorticityCS, "MainInitVorticityCS")
GG_IMPLEMENT_SIM_SHADER(FGasGiantVelocityCS, "MainVelocityCS")
GG_IMPLEMENT_SIM_SHADER(FGasGiantAdvectCS, "MainAdvectCS")
GG_IMPLEMENT_SIM_SHADER(FGasGiantReduceRowsCS, "MainReduceRowsCS")
GG_IMPLEMENT_SIM_SHADER(FGasGiantReducePsiRowsCS, "MainReducePsiRowsCS")
GG_IMPLEMENT_SIM_SHADER(FGasGiantReduceGlobalCS, "MainReduceGlobalCS")
GG_IMPLEMENT_SIM_SHADER(FGasGiantForceCS, "MainForceCS")
GG_IMPLEMENT_SIM_SHADER(FGasGiantPolarFilterCS, "MainPolarFilterCS")
GG_IMPLEMENT_SIM_SHADER(FGasGiantPoissonCS, "MainPoissonCS")
GG_IMPLEMENT_SIM_SHADER(FGasGiantCaptureCS, "MainCaptureCS")
GG_IMPLEMENT_SIM_SHADER(FGasGiantRestoreCS, "MainRestoreCS")
GG_IMPLEMENT_SIM_SHADER(FGasGiantDebugVisCS, "MainDebugVisCS")

#undef GG_IMPLEMENT_SIM_SHADER