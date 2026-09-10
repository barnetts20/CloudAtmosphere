#include "AtmosphereTransmittance.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderCompilerCore.h"

#include "RenderUtils.h"

bool FAtmosphereTransmittanceCS::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
{
	return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
}

void FAtmosphereTransmittanceCS::ModifyCompilationEnvironment(
	const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	// The bake takes its size from here. The material's lookup takes it from
	// the .ush defaults, which is why the two must match.
	OutEnvironment.SetDefine(TEXT("ATMO_TRANSMITTANCE_THREADS"), AtmosphereTransmittance::ThreadGroupSize);
	OutEnvironment.SetDefine(TEXT("ATMO_TRANSMITTANCE_WIDTH"), AtmosphereTransmittance::Width);
	OutEnvironment.SetDefine(TEXT("ATMO_TRANSMITTANCE_HEIGHT"), AtmosphereTransmittance::Height);
}

// Entry point name must match the [numthreads] function in the .usf; a mismatch
// fails at cook time as a missing entry point.
IMPLEMENT_GLOBAL_SHADER(
	FAtmosphereTransmittanceCS,
	"/Plugin/CloudAtmosphere/Private/AtmosphereTransmittance.usf",
	"MainTransmittanceCS",
	SF_Compute);

namespace AtmosphereTransmittance
{
	static void AddBakePass_RenderThread(FRDGBuilder& GraphBuilder, const FAtmosphereTransmittanceParams& Params)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "AtmosphereTransmittance");

		FRDGTextureRef Table = GraphBuilder.RegisterExternalTexture(
			CreateRenderTarget(Params.Table, TEXT("Atmosphere.Transmittance")));

		FAtmosphereTransmittanceParameters* P =
			GraphBuilder.AllocParameters<FAtmosphereTransmittanceParameters>();

		P->TransmittancePlanetRadius = Params.PlanetRadius;
		P->TransmittanceAtmosphereRadius = Params.AtmosphereRadius;
		P->TransmittanceProfilePins = Params.ProfilePins;
		P->TransmittanceUAV = GraphBuilder.CreateUAV(Table);

		const FIntVector Groups(
			FMath::DivideAndRoundUp(Width, ThreadGroupSize),
			FMath::DivideAndRoundUp(Height, ThreadGroupSize),
			1);

		TShaderMapRef<FAtmosphereTransmittanceCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Atmosphere.TransmittanceBake"), Shader, P, Groups);
	}

	void RequestBake(const FAtmosphereTransmittanceParams& Params)
	{
		check(IsInGameThread());

		if (!Params.IsUsable())
		{
			return;
		}

		ENQUEUE_RENDER_COMMAND(AtmosphereTransmittanceBake)(
			[Params](FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);

				AddBakePass_RenderThread(GraphBuilder, Params);

				GraphBuilder.Execute();
			});
	}
}
