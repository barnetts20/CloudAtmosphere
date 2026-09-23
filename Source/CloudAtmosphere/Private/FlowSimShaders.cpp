#include "FlowSimShaders.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "ShaderCompilerCore.h"

// IsFeatureLevelSupported.
#include "RenderUtils.h"

namespace FlowSimShader
{
	bool ShouldCompile(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	void ModifyEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("GG_SIM_THREADS_2D"), ThreadGroupSize2D);
		OutEnvironment.SetDefine(TEXT("GG_SIM_THREADS_1D"), ThreadGroupSize1D);
		OutEnvironment.SetDefine(TEXT("GG_SIM_THREADS_LAYERS"), ThreadGroupSizeLayers);
		OutEnvironment.SetDefine(TEXT("GG_SIM_THREADS_LINE"), ThreadGroupSizeLine);
		OutEnvironment.SetDefine(TEXT("GG_SIM_LINE_MAX"), MaxGridLongitude);
		OutEnvironment.SetDefine(TEXT("GG_SIM_COLUMN_MAX"), MaxGridLatitude);

		// The Rhs pass reads the R32F UAV it also writes. R32F is in the
		// guaranteed typed-UAV-load set; every other UAV here is write-only, which
		// is why the column solve reads one spectrum and writes another.
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}
}

// Entry point names must match FlowSim.usf. A mismatch fails at cook time as a
// missing entry point.

#define GG_IMPLEMENT_SIM_SHADER(ClassName, EntryPoint) \
	IMPLEMENT_GLOBAL_SHADER(ClassName, "/Plugin/CloudAtmosphere/Private/FlowSim.usf", EntryPoint, SF_Compute)

GG_IMPLEMENT_SIM_SHADER(FFlowSimInitBalanceCS, "MainInitBalanceCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimInitStateCS, "MainInitStateCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimReduceRowsCS, "MainReduceRowsCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimReduceGlobalCS, "MainReduceGlobalCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimReconstructCS, "MainReconstructCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimPredictCS, "MainPredictCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimFilterCS, "MainFilterCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimRhsCS, "MainRhsCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimHelmholtzForwardCS, "MainHelmholtzForwardCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimHelmholtzColumnCS, "MainHelmholtzColumnCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimHelmholtzInverseCS, "MainHelmholtzInverseCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimCorrectCS, "MainCorrectCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimCaptureCS, "MainCaptureCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimRestoreCS, "MainRestoreCS")
GG_IMPLEMENT_SIM_SHADER(FFlowSimDebugVisCS, "MainDebugVisCS")

#undef GG_IMPLEMENT_SIM_SHADER