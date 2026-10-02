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
		OutEnvironment.SetDefine(TEXT("FLOWSIM_THREADS_2D"), ThreadGroupSize2D);
		OutEnvironment.SetDefine(TEXT("FLOWSIM_THREADS_1D"), ThreadGroupSize1D);
		OutEnvironment.SetDefine(TEXT("FLOWSIM_THREADS_LAYERS"), ThreadGroupSizeLayers);
		OutEnvironment.SetDefine(TEXT("FLOWSIM_THREADS_LINE"), ThreadGroupSizeLine);
		OutEnvironment.SetDefine(TEXT("FLOWSIM_LINE_MAX"), MaxGridLongitude);
		OutEnvironment.SetDefine(TEXT("FLOWSIM_COLUMN_MAX"), MaxGridLatitude);
		OutEnvironment.SetDefine(TEXT("FLOWSIM_MAX_CELLS"), MaxStormCells);
		OutEnvironment.SetDefine(TEXT("FLOWSIM_MAX_PERPETUAL"), MaxPerpetualStorms);
		OutEnvironment.SetDefine(TEXT("FLOWSIM_CELL_CONTROL_STRIDE"), CellControlStride);
		OutEnvironment.SetDefine(TEXT("FLOWSIM_SNAPSHOT_PLANES"), SnapshotPlanes);
		OutEnvironment.SetDefine(TEXT("FLOWSIM_ATLAS_GUTTER"), AtlasGutter);
		OutEnvironment.SetDefine(TEXT("FLOWSIM_FROUDE_KNEE"), *FString::Printf(TEXT("%.9gf"), FroudeKnee));
		OutEnvironment.SetDefine(TEXT("FLOWSIM_DAMPING_MAX"), *FString::Printf(TEXT("%.9gf"), DampingMax));

		// The Rhs pass reads the R32F UAV it also writes. R32F is in the
		// guaranteed typed-UAV-load set; every other UAV here is write-only, which
		// is why the column solve reads one spectrum and writes another.
		OutEnvironment.CompilerFlags.Add(CFLAG_AllowTypedUAVLoads);
	}
}

/** The name the shader reads the scalars under; FlowSimCommon.ush aliases each
 *  member from it. */
IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FFlowSimUniformParameters, "FlowSimUB");

// Entry point names must match FlowSim.usf. A mismatch fails at cook time as a
// missing entry point.

#define FLOWSIM_IMPLEMENT_SHADER(ClassName, EntryPoint) \
	IMPLEMENT_GLOBAL_SHADER(ClassName, "/Plugin/CloudAtmosphere/Private/FlowSim.usf", EntryPoint, SF_Compute)

FLOWSIM_IMPLEMENT_SHADER(FFlowSimInitBalanceCS, "MainInitBalanceCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimInitStateCS, "MainInitStateCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimReduceRowsCS, "MainReduceRowsCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimReduceGlobalCS, "MainReduceGlobalCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimReconstructCS, "MainReconstructCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimCellsCS, "MainCellsCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimCellFieldCS, "MainCellFieldCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimPredictCS, "MainPredictCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimFilterCS, "MainFilterCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimRhsCS, "MainRhsCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimHelmholtzForwardCS, "MainHelmholtzForwardCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimHelmholtzColumnCS, "MainHelmholtzColumnCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimHelmholtzInverseCS, "MainHelmholtzInverseCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimCorrectCS, "MainCorrectCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimCaptureCS, "MainCaptureCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimRestoreCS, "MainRestoreCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimDebugVisCS, "MainDebugVisCS")
FLOWSIM_IMPLEMENT_SHADER(FFlowSimResampleCS, "MainResampleCS")

#undef FLOWSIM_IMPLEMENT_SHADER