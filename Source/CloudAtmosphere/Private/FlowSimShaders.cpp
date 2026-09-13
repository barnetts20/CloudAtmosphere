#include "FlowSimShaders.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "ShaderCompilerCore.h"

// IsFeatureLevelSupported.
#include "RenderUtils.h"

namespace FlowSimShader
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
		OutEnvironment.SetDefine(TEXT("GG_SIM_THREADS_LAYERS"), ThreadGroupSizeLayers);

		// The SOR sweep reads its own UAV, and the polar filter and the cubic
		// interpolator both index far enough from the thread's own texel that
		// the compiler cannot prove the accesses are in range. Neither is a
		// correctness problem -- SimWrapCoord folds every index -- but the
		// bounds analysis is what would otherwise force scalarisation.
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}
}

// Entry point names must match the [numthreads] functions in FlowSim.usf.
// A mismatch here fails at cook time as a missing entry point rather than
// anywhere useful, so they are listed adjacent for comparison.

#define GG_IMPLEMENT_SIM_SHADER(ClassName, EntryPoint) \
	IMPLEMENT_GLOBAL_SHADER(ClassName, "/Plugin/CloudAtmosphere/Private/FlowSim.usf", EntryPoint, SF_Compute)

GG_IMPLEMENT_SIM_SHADER(FFlowSimInitZonalPotentialCS, "MainInitZonalPotentialCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimInitPotentialCS, "MainInitPotentialCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimInitVorticityCS, "MainInitVorticityCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimVelocityCS, "MainVelocityCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimAdvectCS, "MainAdvectCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimReduceRowsCS, "MainReduceRowsCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimReducePsiRowsCS, "MainReducePsiRowsCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimReduceGlobalCS, "MainReduceGlobalCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimForceCS, "MainForceCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimPolarFilterCS, "MainPolarFilterCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimPoissonCS, "MainPoissonCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimCaptureCS, "MainCaptureCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimRestoreCS, "MainRestoreCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimDebugVisCS, "MainDebugVisCS")

#undef GG_IMPLEMENT_SIM_SHADER