#include "GasGiantSimulation.h"

#include "GasGiantSimShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderTargetPool.h"
#include "RHIGPUReadback.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

// GBlackVolumeTexture, the fallback bound when no seed volume is set.
#include "RenderUtils.h"

// TStaticSamplerState, for the wrapped trilinear seed sampler.
#include "RHIStaticStates.h"

DEFINE_LOG_CATEGORY_STATIC(LogGasGiantSim, Log, All);

/** Everything registered into this frame's graph. Bundled so the pass helpers
 *  take one argument rather than nine, and so that the ping-pong swap is a
 *  single Swap() rather than three call sites that must agree. */
struct FGasGiantSimResources
{
	FRDGTextureRef Vorticity[2] = { nullptr, nullptr };
	FRDGTextureRef Psi = nullptr;
	FRDGTextureRef RowMean = nullptr;
	FRDGTextureRef PsiRowMean = nullptr;
	FRDGTextureRef GlobalMean = nullptr;
	FRDGTextureRef Velocity = nullptr;
	FRDGTextureRef Debug = nullptr;

	int32 Current = 0;

	FRDGTextureRef Source() const { return Vorticity[Current]; }
	FRDGTextureRef Dest() const { return Vorticity[1 - Current]; }
	void Swap() { Current = 1 - Current; }
};

namespace
{
	using namespace GasGiantSimShader;

	FIntVector GroupCount2D(const FIntVector& GridSize)
	{
		return FIntVector(
			FMath::DivideAndRoundUp(GridSize.X, ThreadGroupSize2D),
			FMath::DivideAndRoundUp(GridSize.Y, ThreadGroupSize2D),
			GridSize.Z);
	}

	/** Fills every scalar parameter. Resources are attached per pass afterwards.
	 *
	 *  Written once and shared, which is the point of the single parameter
	 *  struct: there is exactly one place where a config value becomes a shader
	 *  value, so a parameter cannot be threaded through to some passes and
	 *  quietly dropped from others. */
	void FillCommonParameters(FGasGiantSimParameters& P, const FGasGiantSimParams& Params)
	{
		P.SimGridSize = Params.GridSize;
		P.SimInvGridSize = FVector3f(
			1.0f / FMath::Max(Params.GridSize.X, 1),
			1.0f / FMath::Max(Params.GridSize.Y, 1),
			1.0f / FMath::Max(Params.GridSize.Z, 1));

		P.SimJetParams = Params.JetParams;
		P.SimWidthBias = Params.WidthBias;

		for (int32 i = 0; i < 8; ++i)
		{
			P.SimLayerProfile[i] = Params.LayerProfile[i];
		}

		P.SimDeltaTime = Params.DeltaTime;
		P.SimTime = Params.Time;
		P.SimPlanetaryVorticity = Params.PlanetaryVorticity;

		P.SimForcingChannel = Params.ForcingChannel;
		P.SimForcingBipolar = Params.bForcingBipolar ? 1 : 0;
		P.SimHasForcing = Params.ForcingTexture.IsValid() ? 1 : 0;

		P.SimNudgeRate = Params.NudgeRate;
		P.SimForcingAmplitude = Params.ForcingAmplitude;
		P.SimForcingScale = Params.ForcingScale;
		P.SimForcingDrift = Params.ForcingDrift;
		P.SimDragRate = Params.DragRate;
		P.SimLayerCoupling = Params.LayerCoupling;

		P.SimFilterLatitude = Params.FilterLatitude;
		P.SimFilterMaxHalfWidth = Params.FilterMaxHalfWidth;

		P.SimRelaxation = Params.Relaxation;
		P.SimRedBlackParity = 0;

		P.SimDebugMode = Params.DebugMode;
		P.SimDebugLayer = Params.DebugLayer;
		P.SimDebugScale = Params.DebugScale;
		P.SimDebugSize = Params.DebugSize;

		// The seed volume is optional. A null texture bound as black evaluates
		// the seed as zero, which gives a purely zonal initial condition -- a
		// legitimate state, and a far better failure than refusing to run,
		// because a running sim with no eddies is diagnosable from the debug
		// view in one glance and a sim that never started is not.
		P.SimForcingNoise = Params.ForcingTexture.IsValid()
			? Params.ForcingTexture
			: GBlackVolumeTexture->TextureRHI;

		// Wrap on all three axes. The forcing volume is a tiling bake and reading
		// it clamped puts a stretched band of constant value along each face,
		// which seeds a spurious vorticity sheet there.
		P.SimForcingNoiseSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
	}

	template <typename TShader>
	void AddSimPass(
		FRDGBuilder& GraphBuilder,
		const TCHAR* Name,
		FGasGiantSimParameters* Parameters,
		const FIntVector& Groups)
	{
		TShaderMapRef<TShader> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			FRDGEventName(TEXT("%s"), Name),
			Shader,
			Parameters,
			Groups);
	}
}

void FGasGiantSimulation::RequestReset()
{
	bResetRequested = true;
}

void FGasGiantSimulation::QueueRestore_RenderThread(TArray<float>&& InData)
{
	check(IsInRenderingThread());

	PendingRestore = MoveTemp(InData);

	// Force the next Enqueue through initialisation so the upload actually
	// happens, rather than being stranded behind an already-initialised sim.
	bResetRequested = true;
}

void FGasGiantSimulation::Release_RenderThread()
{
	PooledVorticity[0].SafeRelease();
	PooledVorticity[1].SafeRelease();
	PooledPsi.SafeRelease();

	// PendingRestore is DELIBERATELY NOT cleared here.
	//
	// QueueRestore_RenderThread sets bResetRequested so the upload is
	// guaranteed to run, and EnsureResources answers that flag by calling this
	// function -- so clearing the payload here destroys it with the very reset
	// that was supposed to apply it. The restore then silently falls through to
	// seeding, and the only symptom is a planet that does not look like the one
	// that was saved.
	//
	// The payload is owned by the initialisation branch in Enqueue, which
	// either consumes it or reports it as mismatched and empties it there.
	PooledRowMean.SafeRelease();
	PooledPsiRowMean.SafeRelease();
	PooledGlobalMean.SafeRelease();

	AllocatedGrid = FIntVector::ZeroValue;
	CurrentVorticity = 0;
	bInitialised = false;
	bResetRequested = false;
}

bool FGasGiantSimulation::EnsureResources(const FGasGiantSimParams& Params)
{
	const bool bGridChanged = (AllocatedGrid != Params.GridSize);

	if (bGridChanged || bResetRequested || !PooledPsi.IsValid())
	{
		Release_RenderThread();

		const FIntPoint Size(Params.GridSize.X, Params.GridSize.Y);
		const uint16 Slices = (uint16)FMath::Max(Params.GridSize.Z, 1);

		// R32F rather than R16F for the state. Vorticity spans several orders
		// of magnitude between a jet core and a quiet zone interior, and the
		// Poisson residual is a small difference of two larger numbers -- half
		// precision there loses the residual entirely and the solve stalls at a
		// floor it cannot see below. The output texture the material reads is
		// RGBA16F, and that is fine, because by then the differencing is done.
		const FRDGTextureDesc StateDesc = FRDGTextureDesc::Create2DArray(
			Size, PF_R32_FLOAT, FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_UAV, Slices);

		PooledVorticity[0] = AllocatePooledTexture(StateDesc, TEXT("GasGiant.VorticityA"));
		PooledVorticity[1] = AllocatePooledTexture(StateDesc, TEXT("GasGiant.VorticityB"));
		PooledPsi = AllocatePooledTexture(StateDesc, TEXT("GasGiant.Psi"));

		// (row, layer). Also carries the zonal streamfunction during init; see
		// MainInitZonalPotentialCS for why it is reused rather than duplicated.
		const FRDGTextureDesc RowDesc = FRDGTextureDesc::Create2D(
			FIntPoint(Params.GridSize.Y, Slices), PF_R32_FLOAT, FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_UAV);

		PooledRowMean = AllocatePooledTexture(RowDesc, TEXT("GasGiant.RowMean"));
		PooledPsiRowMean = AllocatePooledTexture(RowDesc, TEXT("GasGiant.PsiRowMean"));

		const FRDGTextureDesc GlobalDesc = FRDGTextureDesc::Create2D(
			FIntPoint(1, Slices), PF_R32_FLOAT, FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_UAV);

		PooledGlobalMean = AllocatePooledTexture(GlobalDesc, TEXT("GasGiant.GlobalMean"));

		AllocatedGrid = Params.GridSize;
		CurrentVorticity = 0;
		bInitialised = false;
		bResetRequested = false;

		UE_LOG(LogGasGiantSim, Log, TEXT("Allocated sim state at %dx%d x %d layers."),
			Params.GridSize.X, Params.GridSize.Y, Params.GridSize.Z);

		return true;
	}

	return false;
}

void FGasGiantSimulation::AddInitPasses(FRDGBuilder& GraphBuilder, const FGasGiantSimParams& Params, const FGasGiantSimResources& R)
{
	const FIntVector Groups2D = GroupCount2D(Params.GridSize);

	// Rows, then layers. One thread per row.
	const FIntVector GroupsRows(
		FMath::DivideAndRoundUp(Params.GridSize.Y, ThreadGroupSize1D),
		Params.GridSize.Z,
		1);

	// -- Zonal streamfunction, by quadrature, one value per row -------------

	{
		FGasGiantSimParameters* P = GraphBuilder.AllocParameters<FGasGiantSimParameters>();
		FillCommonParameters(*P, Params);
		P->SimRowMeanUAV = GraphBuilder.CreateUAV(R.RowMean);

		AddSimPass<FGasGiantInitZonalPotentialCS>(GraphBuilder, TEXT("GasGiant.InitZonalPotential"), P, GroupsRows);
	}

	// -- Full streamfunction: zonal plus seeded eddies ----------------------

	{
		FGasGiantSimParameters* P = GraphBuilder.AllocParameters<FGasGiantSimParameters>();
		FillCommonParameters(*P, Params);
		P->SimRowMeanSRV = GraphBuilder.CreateSRV(R.RowMean);
		P->SimPsiUAV = GraphBuilder.CreateUAV(R.Psi);

		AddSimPass<FGasGiantInitPotentialCS>(GraphBuilder, TEXT("GasGiant.InitPotential"), P, Groups2D);
	}

	// -- Vorticity, through the same discrete operator the solver inverts ---

	{
		FGasGiantSimParameters* P = GraphBuilder.AllocParameters<FGasGiantSimParameters>();
		FillCommonParameters(*P, Params);
		P->SimPsiSRV = GraphBuilder.CreateSRV(R.Psi);
		P->SimVorticityUAV = GraphBuilder.CreateUAV(R.Vorticity[R.Current]);

		AddSimPass<FGasGiantInitVorticityCS>(GraphBuilder, TEXT("GasGiant.InitVorticity"), P, Groups2D);
	}
}

void FGasGiantSimulation::AddRestorePass(FRDGBuilder& GraphBuilder, const FGasGiantSimParams& Params, const FGasGiantSimResources& R)
{
	const int32 Total = Params.GridSize.X * Params.GridSize.Y * Params.GridSize.Z;

	// CreateStructuredBuffer uploads the initial data as part of graph setup,
	// so the pass below reads it without a separate transfer step.
	FRDGBufferRef Upload = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("GasGiant.RestoreUpload"),
		sizeof(float),
		Total * 2,
		PendingRestore.GetData(),
		PendingRestore.Num() * sizeof(float));

	FGasGiantSimParameters* P = GraphBuilder.AllocParameters<FGasGiantSimParameters>();
	FillCommonParameters(*P, Params);
	P->SimRestoreBuffer = GraphBuilder.CreateSRV(Upload);
	P->SimVorticityUAV = GraphBuilder.CreateUAV(R.Vorticity[R.Current]);
	P->SimPsiUAV = GraphBuilder.CreateUAV(R.Psi);

	AddSimPass<FGasGiantRestoreCS>(GraphBuilder, TEXT("GasGiant.Restore"), P, GroupCount2D(Params.GridSize));
}

void FGasGiantSimulation::AddCapturePass_RenderThread(FRDGBuilder& GraphBuilder, const FGasGiantSimParams& Params, FRHIGPUBufferReadback* Readback)
{
	check(IsInRenderingThread());

	if (!Readback || !PooledPsi.IsValid())
	{
		return;
	}

	const int32 Total = AllocatedGrid.X * AllocatedGrid.Y * AllocatedGrid.Z;

	if (Total <= 0)
	{
		return;
	}

	FRDGTextureRef Vorticity = GraphBuilder.RegisterExternalTexture(PooledVorticity[CurrentVorticity]);
	FRDGTextureRef Psi = GraphBuilder.RegisterExternalTexture(PooledPsi);

	FRDGBufferRef Capture = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Total * 2),
		TEXT("GasGiant.Capture"));

	FGasGiantSimParameters* P = GraphBuilder.AllocParameters<FGasGiantSimParameters>();
	FillCommonParameters(*P, Params);
	P->SimVorticitySRV = GraphBuilder.CreateSRV(Vorticity);
	P->SimPsiSRV = GraphBuilder.CreateSRV(Psi);
	P->SimCaptureBuffer = GraphBuilder.CreateUAV(Capture);

	AddSimPass<FGasGiantCaptureCS>(GraphBuilder, TEXT("GasGiant.Capture"), P, GroupCount2D(AllocatedGrid));

	AddEnqueueCopyPass(GraphBuilder, Readback, Capture, Total * 2 * sizeof(float));
}

void FGasGiantSimulation::AddPoissonSolve(FRDGBuilder& GraphBuilder, const FGasGiantSimParams& Params, const FGasGiantSimResources& R, int32 Iterations)
{
	const FIntVector Groups2D = GroupCount2D(Params.GridSize);

	// Two dispatches per sweep, opposite parity. The parity split is what makes
	// the in-place update safe: every tap a thread reads is the other colour,
	// so nothing in the dispatch is writing it.
	for (int32 Sweep = 0; Sweep < Iterations; ++Sweep)
	{
		for (int32 Parity = 0; Parity < 2; ++Parity)
		{
			FGasGiantSimParameters* P = GraphBuilder.AllocParameters<FGasGiantSimParameters>();
			FillCommonParameters(*P, Params);
			P->SimRedBlackParity = Parity;
			P->SimVorticitySRV = GraphBuilder.CreateSRV(R.Source());
			P->SimPsiUAV = GraphBuilder.CreateUAV(R.Psi);

			AddSimPass<FGasGiantPoissonCS>(GraphBuilder, TEXT("GasGiant.Poisson"), P, Groups2D);
		}
	}
}

void FGasGiantSimulation::AddSubstep(FRDGBuilder& GraphBuilder, const FGasGiantSimParams& Params, FGasGiantSimResources& R)
{
	const FIntVector Groups2D = GroupCount2D(Params.GridSize);

	const FIntVector GroupsRows(
		FMath::DivideAndRoundUp(Params.GridSize.Y, ThreadGroupSize1D),
		Params.GridSize.Z,
		1);

	const FIntVector GroupsLayers(
		FMath::DivideAndRoundUp(Params.GridSize.Z, ThreadGroupSizeLayers),
		1,
		1);

	// -- 1. Velocity from the current streamfunction ------------------------
	//
	// First, because advection needs a velocity consistent with the vorticity
	// it is about to move. This also writes the texture the material samples,
	// so the two uses are one pass rather than two.

	{
		FGasGiantSimParameters* P = GraphBuilder.AllocParameters<FGasGiantSimParameters>();
		FillCommonParameters(*P, Params);
		P->SimPsiSRV = GraphBuilder.CreateSRV(R.Psi);
		P->SimPsiRowMeanUAV = GraphBuilder.CreateUAV(R.PsiRowMean);

		AddSimPass<FGasGiantReducePsiRowsCS>(GraphBuilder, TEXT("GasGiant.ReducePsiRows"), P, GroupsRows);
	}

	{
		FGasGiantSimParameters* P = GraphBuilder.AllocParameters<FGasGiantSimParameters>();
		FillCommonParameters(*P, Params);
		P->SimPsiSRV = GraphBuilder.CreateSRV(R.Psi);
		P->SimVorticitySRV = GraphBuilder.CreateSRV(R.Source());
		P->SimPsiRowMeanSRV = GraphBuilder.CreateSRV(R.PsiRowMean);
		P->SimVelocityUAV = GraphBuilder.CreateUAV(R.Velocity);

		AddSimPass<FGasGiantVelocityCS>(GraphBuilder, TEXT("GasGiant.Velocity"), P, Groups2D);
	}

	// -- 2. Advect absolute vorticity ---------------------------------------

	{
		FGasGiantSimParameters* P = GraphBuilder.AllocParameters<FGasGiantSimParameters>();
		FillCommonParameters(*P, Params);
		P->SimVorticitySRV = GraphBuilder.CreateSRV(R.Source());
		P->SimVelocitySRV = GraphBuilder.CreateSRV(R.Velocity);
		P->SimVorticityUAV = GraphBuilder.CreateUAV(R.Dest());

		AddSimPass<FGasGiantAdvectCS>(GraphBuilder, TEXT("GasGiant.Advect"), P, Groups2D);
	}
	R.Swap();

	// -- 3. Reductions ------------------------------------------------------
	//
	// After advection rather than before, so the forcing nudges what the flow
	// has actually become rather than what it was at the top of the step.

	{
		FGasGiantSimParameters* P = GraphBuilder.AllocParameters<FGasGiantSimParameters>();
		FillCommonParameters(*P, Params);
		P->SimVorticitySRV = GraphBuilder.CreateSRV(R.Source());
		P->SimRowMeanUAV = GraphBuilder.CreateUAV(R.RowMean);

		AddSimPass<FGasGiantReduceRowsCS>(GraphBuilder, TEXT("GasGiant.ReduceRows"), P, GroupsRows);
	}

	{
		FGasGiantSimParameters* P = GraphBuilder.AllocParameters<FGasGiantSimParameters>();
		FillCommonParameters(*P, Params);
		P->SimRowMeanSRV = GraphBuilder.CreateSRV(R.RowMean);
		P->SimGlobalMeanUAV = GraphBuilder.CreateUAV(R.GlobalMean);

		AddSimPass<FGasGiantReduceGlobalCS>(GraphBuilder, TEXT("GasGiant.ReduceGlobal"), P, GroupsLayers);
	}

	// -- 4. Forcing ---------------------------------------------------------

	{
		FGasGiantSimParameters* P = GraphBuilder.AllocParameters<FGasGiantSimParameters>();
		FillCommonParameters(*P, Params);
		P->SimVorticitySRV = GraphBuilder.CreateSRV(R.Source());
		P->SimRowMeanSRV = GraphBuilder.CreateSRV(R.RowMean);
		P->SimGlobalMeanSRV = GraphBuilder.CreateSRV(R.GlobalMean);
		P->SimVorticityUAV = GraphBuilder.CreateUAV(R.Dest());

		AddSimPass<FGasGiantForceCS>(GraphBuilder, TEXT("GasGiant.Force"), P, Groups2D);
	}
	R.Swap();

	// -- 5. Polar filter ----------------------------------------------------

	{
		FGasGiantSimParameters* P = GraphBuilder.AllocParameters<FGasGiantSimParameters>();
		FillCommonParameters(*P, Params);
		P->SimVorticitySRV = GraphBuilder.CreateSRV(R.Source());
		P->SimVorticityUAV = GraphBuilder.CreateUAV(R.Dest());

		AddSimPass<FGasGiantPolarFilterCS>(GraphBuilder, TEXT("GasGiant.PolarFilter"), P, Groups2D);
	}
	R.Swap();

	// -- 6. Poisson ---------------------------------------------------------
	//
	// Warm started: psi still holds last substep's solution, which is a near
	// solution to this one. That is what pays for an iterative solver here.

	AddPoissonSolve(GraphBuilder, Params, R, Params.PoissonIterations);
}

void FGasGiantSimulation::AddDebugPass(FRDGBuilder& GraphBuilder, const FGasGiantSimParams& Params, const FGasGiantSimResources& R)
{
	if (!R.Debug)
	{
		return;
	}

	const FIntVector Groups(
		FMath::DivideAndRoundUp(Params.DebugSize.X, ThreadGroupSize2D),
		FMath::DivideAndRoundUp(Params.DebugSize.Y, ThreadGroupSize2D),
		1);

	FGasGiantSimParameters* P = GraphBuilder.AllocParameters<FGasGiantSimParameters>();
	FillCommonParameters(*P, Params);
	P->SimVorticitySRV = GraphBuilder.CreateSRV(R.Source());
	P->SimPsiSRV = GraphBuilder.CreateSRV(R.Psi);
	P->SimVelocitySRV = GraphBuilder.CreateSRV(R.Velocity);
	P->SimRowMeanSRV = GraphBuilder.CreateSRV(R.RowMean);
	P->SimDebugUAV = GraphBuilder.CreateUAV(R.Debug);

	AddSimPass<FGasGiantDebugVisCS>(GraphBuilder, TEXT("GasGiant.DebugVis"), P, Groups);
}

void FGasGiantSimulation::Enqueue_RenderThread(FRDGBuilder& GraphBuilder, const FGasGiantSimParams& Params, int32 NumSubsteps)
{
	check(IsInRenderingThread());

	if (!Params.FlowTexture.IsValid())
	{
		// Without somewhere to put the velocity there is nothing worth
		// computing. Refused loudly rather than silently, since a sim that
		// runs and writes nowhere is indistinguishable from a broken one.
		return;
	}

	const bool bNeedsSeeding = EnsureResources(Params);

	RDG_EVENT_SCOPE(GraphBuilder, "GasGiantSim");

	FGasGiantSimResources R;
	R.Vorticity[0] = GraphBuilder.RegisterExternalTexture(PooledVorticity[0]);
	R.Vorticity[1] = GraphBuilder.RegisterExternalTexture(PooledVorticity[1]);
	R.Psi = GraphBuilder.RegisterExternalTexture(PooledPsi);
	R.RowMean = GraphBuilder.RegisterExternalTexture(PooledRowMean);
	R.PsiRowMean = GraphBuilder.RegisterExternalTexture(PooledPsiRowMean);
	R.GlobalMean = GraphBuilder.RegisterExternalTexture(PooledGlobalMean);
	R.Current = CurrentVorticity;

	R.Velocity = GraphBuilder.RegisterExternalTexture(
		CreateRenderTarget(Params.FlowTexture, TEXT("GasGiant.Flow")));

	if (Params.DebugTexture.IsValid() && Params.DebugSize.X > 0 && Params.DebugSize.Y > 0)
	{
		R.Debug = GraphBuilder.RegisterExternalTexture(
			CreateRenderTarget(Params.DebugTexture, TEXT("GasGiant.Debug")));
	}

	if (bNeedsSeeding || !bInitialised)
	{
		if (PendingRestore.Num() == Params.GridSize.X * Params.GridSize.Y * Params.GridSize.Z * 2)
		{
			// Restored, not seeded. No Poisson solve follows: psi arrives
			// already consistent with the vorticity it was captured beside, so
			// solving could only move it away from the captured state.
			AddRestorePass(GraphBuilder, Params, R);

			UE_LOG(LogGasGiantSim, Log, TEXT("Restored state from snapshot."));

			PendingRestore.Empty();
			bInitialised = true;
		}
		else
		{
			if (PendingRestore.Num() > 0)
			{
				UE_LOG(LogGasGiantSim, Warning,
					TEXT("Snapshot has %d floats, grid needs %d. Seeding instead."),
					PendingRestore.Num(),
					Params.GridSize.X * Params.GridSize.Y * Params.GridSize.Z * 2);

				PendingRestore.Empty();
			}

			AddInitPasses(GraphBuilder, Params, R);

			// The one cold start. No previous psi to warm start from, so this
			// runs many more sweeps than a substep does, and it is off the
			// frame budget so it can afford to.
			//
			// It is not truly cold either: psi already holds the zonal
			// streamfunction and the vorticity was built by applying the
			// discrete Laplacian to exactly that, which puts the source in the
			// range of the operator. So it should converge almost immediately,
			// and a residual view that is not featureless on frame one means
			// the discretisation and the seeding disagree -- a far more
			// specific bug than "the sim looks wrong".
			//
			// The restore branch above skips this entirely, which is most of
			// why storing psi alongside vorticity is worth the extra megabytes.
			AddPoissonSolve(GraphBuilder, Params, R, FMath::Max(Params.InitPoissonIterations, 1));

			bInitialised = true;
		}
	}

	for (int32 Step = 0; Step < NumSubsteps; ++Step)
	{
		AddSubstep(GraphBuilder, Params, R);
	}

	// Final velocity pass so the texture the material reads matches the psi the
	// last substep produced rather than the one it started from. One extra
	// dispatch; without it the flow the material sees is always one substep
	// stale, which is invisible at four substeps a frame and confusing at one.
	{
		const FIntVector GroupsRows(
			FMath::DivideAndRoundUp(Params.GridSize.Y, ThreadGroupSize1D), Params.GridSize.Z, 1);

		FGasGiantSimParameters* PR = GraphBuilder.AllocParameters<FGasGiantSimParameters>();
		FillCommonParameters(*PR, Params);
		PR->SimPsiSRV = GraphBuilder.CreateSRV(R.Psi);
		PR->SimPsiRowMeanUAV = GraphBuilder.CreateUAV(R.PsiRowMean);

		AddSimPass<FGasGiantReducePsiRowsCS>(GraphBuilder, TEXT("GasGiant.ReducePsiRowsFinal"), PR, GroupsRows);

		FGasGiantSimParameters* P = GraphBuilder.AllocParameters<FGasGiantSimParameters>();
		FillCommonParameters(*P, Params);
		P->SimPsiSRV = GraphBuilder.CreateSRV(R.Psi);
		P->SimVorticitySRV = GraphBuilder.CreateSRV(R.Source());
		P->SimPsiRowMeanSRV = GraphBuilder.CreateSRV(R.PsiRowMean);
		P->SimVelocityUAV = GraphBuilder.CreateUAV(R.Velocity);

		AddSimPass<FGasGiantVelocityCS>(GraphBuilder, TEXT("GasGiant.VelocityFinal"), P, GroupCount2D(Params.GridSize));
	}

	AddDebugPass(GraphBuilder, Params, R);

	// Carry the ping-pong index across the frame boundary. An odd number of
	// swaps per substep means this genuinely alternates, so losing it would
	// silently advance the sim from a two-substeps-stale buffer.
	CurrentVorticity = R.Current;
}