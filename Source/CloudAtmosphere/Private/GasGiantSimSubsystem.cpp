#include "GasGiantSimSubsystem.h"

#include "GasGiantShadowMap.h"
#include "GasGiantSimulation.h"
#include "GasGiantSimSettings.h"
#include "GasGiantSnapshot.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "Engine/VolumeTexture.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureRenderTarget2DArray.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"

// TAutoConsoleVariable and FAutoConsoleCommandWithWorldAndArgs.
#include "HAL/IConsoleManager.h"

// UWorld::GetSubsystem, used by the console commands to find the right
// per-world instance.
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogGasGiant, Log, All);

// ---------------------------------------------------------------------------
// Console commands. Present because the fastest debugging loop for a field
// like this is: change one thing, look, change it back. Going through a
// blueprint or a details panel for that is enough friction to discourage it.
// ---------------------------------------------------------------------------

static TAutoConsoleVariable<int32> CVarGasGiantDebugMode(
	TEXT("r.GasGiant.DebugMode"),
	-1,
	TEXT("Override the config's debug view. -1 uses the config.\n")
	TEXT("0 Vorticity, 1 Streamfunction, 2 Speed, 3 East, 4 North,\n")
	TEXT("5 Poisson residual, 6 Zonal profile error."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGasGiantDebugLayer(
	TEXT("r.GasGiant.DebugLayer"),
	-1,
	TEXT("Override the debug layer. -1 uses the config."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarGasGiantDebugScale(
	TEXT("r.GasGiant.DebugScale"),
	-1.0f,
	TEXT("Override the debug value scale. Negative uses the config."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarGasGiantPaused(
	TEXT("r.GasGiant.Paused"),
	-1,
	TEXT("Override pause. -1 uses the config, 0 runs, 1 freezes."),
	ECVF_RenderThreadSafe);

// ---------------------------------------------------------------------------
// Console commands.
//
// FConsoleCommandWithWorldAndArgsDelegate rather than a plain command, because
// the subsystem is per world and a static command has no other way to find the
// right one. The world it hands back is the one the command was issued in,
// which is the editor world when typed into the editor console and the PIE
// world when typed during play -- exactly the disambiguation wanted.
// ---------------------------------------------------------------------------

namespace
{
	UGasGiantSimSubsystem* FindSubsystem(UWorld* World)
	{
		return World ? World->GetSubsystem<UGasGiantSimSubsystem>() : nullptr;
	}

	/** Resolves an optional asset path argument, falling back to the project
	 *  setting. Both paths are logged, because "started against the wrong
	 *  config" and "did not start" look identical from the debug view. */
	UGasGiantSimConfig* ResolveConfig(const TArray<FString>& Args)
	{
		if (Args.Num() > 0)
		{
			UGasGiantSimConfig* Loaded = LoadObject<UGasGiantSimConfig>(nullptr, *Args[0]);

			if (!Loaded)
			{
				UE_LOG(LogGasGiant, Error, TEXT("No GasGiantSimConfig at '%s'."), *Args[0]);
			}

			return Loaded;
		}

		const UGasGiantSimSettings* Settings = GetDefault<UGasGiantSimSettings>();

		if (!Settings || Settings->DefaultConfig.IsNull())
		{
			UE_LOG(LogGasGiant, Error,
				TEXT("No config given and no DefaultConfig set in ")
				TEXT("Project Settings -> Plugins -> Gas Giant Sim."));
			return nullptr;
		}

		return Settings->DefaultConfig.LoadSynchronous();
	}
}

static FAutoConsoleCommandWithWorldAndArgs GGasGiantStartCmd(
	TEXT("GasGiant.Start"),
	TEXT("Start the flow sim. Optional argument is a GasGiantSimConfig asset path; ")
	TEXT("with none, uses the DefaultConfig from Project Settings."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (UGasGiantSimSubsystem* Sub = FindSubsystem(World))
			{
				if (UGasGiantSimConfig* Config = ResolveConfig(Args))
				{
					Sub->StartSimulation(Config);
					UE_LOG(LogGasGiant, Log, TEXT("Started against '%s'."), *Config->GetName());
				}
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GGasGiantStopCmd(
	TEXT("GasGiant.Stop"),
	TEXT("Stop stepping. State is kept, so GasGiant.Start resumes rather than reseeds."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>&, UWorld* World)
		{
			if (UGasGiantSimSubsystem* Sub = FindSubsystem(World))
			{
				Sub->StopSimulation();
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GGasGiantResetCmd(
	TEXT("GasGiant.Reset"),
	TEXT("Discard the field and reseed from the current config. Also the way to ")
	TEXT("pick up a changed grid size or seed volume."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>&, UWorld* World)
		{
			if (UGasGiantSimSubsystem* Sub = FindSubsystem(World))
			{
				Sub->ResetSimulation();
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GGasGiantStepCmd(
	TEXT("GasGiant.Step"),
	TEXT("Advance N substeps while paused. Default 1."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (UGasGiantSimSubsystem* Sub = FindSubsystem(World))
			{
				Sub->StepOnce(Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 1);
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GGasGiantSaveCmd(
	TEXT("GasGiant.Save"),
	TEXT("Capture the live state into a GasGiantSnapshot asset. Argument is the ")
	TEXT("asset path. Blocks on the GPU; an authoring operation."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (Args.Num() == 0)
			{
				UE_LOG(LogGasGiant, Error, TEXT("GasGiant.Save needs a snapshot asset path."));
				return;
			}

			UGasGiantSimSubsystem* Sub = FindSubsystem(World);

			if (!Sub)
			{
				return;
			}

			UGasGiantSnapshot* Target = LoadObject<UGasGiantSnapshot>(nullptr, *Args[0]);

			if (!Target)
			{
				UE_LOG(LogGasGiant, Error,
					TEXT("No GasGiantSnapshot at '%s'. Create the asset first, then save into it."),
					*Args[0]);
				return;
			}

			Sub->SaveSnapshot(Target);
		}));

static FAutoConsoleCommandWithWorldAndArgs GGasGiantStatusCmd(
	TEXT("GasGiant.Status"),
	TEXT("Report step count, simulated time and spin-up progress."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>&, UWorld* World)
		{
			if (UGasGiantSimSubsystem* Sub = FindSubsystem(World))
			{
				UE_LOG(LogGasGiant, Display,
					TEXT("steps %d, simulated time %.2f, Courant %.3f, %s"),
					Sub->GetStepsCompleted(),
					Sub->GetSimulatedTime(),
					Sub->GetCourant(),
					Sub->IsSpinningUp() ? TEXT("spinning up") : TEXT("free running"));
			}
		}));

// ---------------------------------------------------------------------------

/** Peak angular rate the profile can reach.
 *
 *  The saturation caps the shaped term at 1, and the equatorial boost is added
 *  AFTER it and so is not bounded by it. Conservative when the profile does not
 *  fully saturate, exact when it does -- which it does at the defaults. */
static float GasGiantPeakRate(const UGasGiantSimConfig& Config)
{
	return FMath::Max(Config.JetStrength * (1.0f + Config.EquatorialBoost), 1e-6f);
}

void UGasGiantSimSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Simulation = new FGasGiantSimulation();
}

void UGasGiantSimSubsystem::Deinitialize()
{
	if (Simulation)
	{
		// The render thread owns the pooled allocations, so they are released
		// there and the flush is what makes deleting the object afterwards
		// safe. Deleting from the game thread without this races a frame that
		// is already referencing them.
		FGasGiantSimulation* Sim = Simulation;
		Simulation = nullptr;

		ENQUEUE_RENDER_COMMAND(GasGiantRelease)(
			[Sim](FRHICommandListImmediate&)
			{
				Sim->Release_RenderThread();
				delete Sim;
			});

		FlushRenderingCommands();
	}

	Super::Deinitialize();
}

bool UGasGiantSimSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game
		|| WorldType == EWorldType::PIE
		|| WorldType == EWorldType::Editor;
}

TStatId UGasGiantSimSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGasGiantSimSubsystem, STATGROUP_Tickables);
}

void UGasGiantSimSubsystem::StartSimulation(UGasGiantSimConfig* InConfig)
{
	if (!InConfig)
	{
		UE_LOG(LogGasGiant, Warning, TEXT("StartSimulation called with a null config."));
		return;
	}

	Config = InConfig;
	bRunning = true;

	StepAccumulator = 0.0f;
	SimulatedTime = 0.0f;
	StepsCompleted = 0;
	PendingManualSteps = 0;

	ReportCourant();
	ReportInertSettings();

	// ResetSimulation owns the restore-or-seed decision, so starting and
	// resetting cannot diverge. They did: reset used to only clear the field,
	// which meant GasGiant.Reset reseeded even with a snapshot bound.
	ResetSimulation();
}

float UGasGiantSimSubsystem::GetCourant() const
{
	if (!Config)
	{
		return 0.0f;
	}

	const int32 W = FMath::Max(Config->GridLongitude & ~1, 32);
	const float Step = Config->TimeScale * Config->StepRatio;

	return GasGiantPeakRate(*Config) * Step * W / (2.0f * UE_PI);
}

void UGasGiantSimSubsystem::ReportInertSettings() const
{
	if (!Config)
	{
		return;
	}

	// Forcing with nowhere to sample from. SimSampleForcing returns exactly
	// zero when no volume is bound, so the amplitude, scale and drift are all
	// dormant -- and will all switch on together the moment a volume is
	// assigned, which is a surprising amount of change from one assignment.
	if (Config->ForcingAmplitude > 0.0f && !Config->ForcingVolume)
	{
		UE_LOG(LogGasGiant, Warning,
			TEXT("ForcingAmplitude is %.3f but no ForcingVolume is bound, so the ")
			TEXT("stochastic forcing is inactive. The nudge is the only energy ")
			TEXT("source. Assigning a volume will switch amplitude, scale and ")
			TEXT("drift on all at once."),
			Config->ForcingAmplitude);
	}

	// Forcing that never refreshes. The pattern has to move by about one
	// feature per eddy turnover to read as stochastic; far slower than that and
	// the sim converges to a fixed point with every structure pinned to a fixed
	// longitude, which looks laminar however strong the forcing is.
	if (Config->ForcingAmplitude > 0.0f && Config->ForcingVolume)
	{
		const float Growth = Config->JetStrength * Config->BandCount * UE_PI;
		const float DriftRate = (float)Config->ForcingDrift.Size();

		if (Growth > 0.0f && DriftRate > 0.0f && DriftRate < Growth * 0.05f)
		{
			UE_LOG(LogGasGiant, Warning,
				TEXT("ForcingDrift %.4f is far below the growth rate %.2f, so the ")
				TEXT("forcing is effectively frozen and the field will settle to a ")
				TEXT("fixed pattern. Near %.2f puts refresh on the turnover timescale."),
				DriftRate, Growth, Growth);
		}
	}

	// Vertical coupling with nothing to couple to.
	if (Config->LayerCoupling > 0.0f && Config->LayerCount < 2)
	{
		UE_LOG(LogGasGiant, Warning,
			TEXT("LayerCoupling is %.3f but LayerCount is 1, so it does nothing."),
			Config->LayerCoupling);
	}

	// Spin-up that will never run, and the solve that goes with it.
	if (Config->InitialState && Config->SpinUpSteps > 0)
	{
		UE_LOG(LogGasGiant, Log,
			TEXT("InitialState is bound, so SpinUpSteps (%d) and ")
			TEXT("InitPoissonIterations (%d) are skipped -- a restored state is ")
			TEXT("already spun up and its psi arrives consistent with its ")
			TEXT("vorticity. Both still matter when CREATING snapshots."),
			Config->SpinUpSteps, Config->InitPoissonIterations);
	}

	// The polar filter switched off entirely. Legal, and at a high Courant
	// number the numerical diffusion covers for it, but worth saying because
	// FilterMaxHalfWidth then looks like it should be doing something.
	if (Config->FilterLatitude <= 0.0f)
	{
		UE_LOG(LogGasGiant, Log,
			TEXT("FilterLatitude is 0, so the polar filter is disabled entirely ")
			TEXT("and FilterMaxHalfWidth has no effect."));
	}
	else if (Config->FilterLatitude > 0.5f)
	{
		const float Deg = FMath::RadiansToDegrees(FMath::Acos(Config->FilterLatitude));

		UE_LOG(LogGasGiant, Log,
			TEXT("FilterLatitude %.2f engages the longitudinal filter poleward of ")
			TEXT("%.1f degrees, which is most of the visible disc rather than just ")
			TEXT("the poles."),
			Config->FilterLatitude, Deg);
	}

	// The nudge only has leverage in proportion to how supercritical the jets
	// are. Above marginal there is no instability to maintain against, so
	// NudgeRate stops mattering -- and that reads as a regression rather than
	// as the supercriticality going away.
	const float Growth = Config->JetStrength * Config->BandCount * UE_PI;

	if (Growth > 0.0f && Config->NudgeRate > 0.0f && Config->NudgeRate < Growth * 0.01f)
	{
		UE_LOG(LogGasGiant, Log,
			TEXT("NudgeRate %.3f is under 1%% of the growth rate %.2f, so the ")
			TEXT("prescribed profile will not hold against the instability."),
			Config->NudgeRate, Growth);
	}
}

void UGasGiantSimSubsystem::ReportCourant() const
{
	if (!Config)
	{
		return;
	}

	// A CONSEQUENCE, NOT A CONTROL.
	//
	// StepRatio is what is authored, because it pins the substep count and
	// therefore the frame cost. Courant then falls out of it together with the
	// peak rate and the grid width, so it moves whenever the profile is
	// touched -- which is exactly why it is reported rather than authored.
	//
	// Semi-Lagrangian does not go UNSTABLE past 0.33, it goes DIFFUSIVE, and
	// that diffusion is a real energy sink. When DragRate is small it can be
	// most of the dissipation, which makes it a physics term wearing the
	// clothes of an accuracy setting. Worth naming at start, because nothing in
	// the output points back at the timestep.
	const float Step = Config->TimeScale * Config->StepRatio;
	const float Courant = GetCourant();

	UE_LOG(LogGasGiant, Log,
		TEXT("StepRatio %.5f -> step %.5f at TimeScale %.2f, Courant %.3f, ")
		TEXT("%.1f substeps/frame at 60fps."),
		Config->StepRatio, Step, Config->TimeScale, Courant,
		(1.0f / 60.0f) / FMath::Max(Config->StepRatio, 1e-9f));

	if (Courant > 0.33f)
	{
		UE_LOG(LogGasGiant, Log,
			TEXT("Courant is above 0.33, so numerical diffusion is a significant ")
			TEXT("energy sink and the look is tied to this TimeScale -- Courant ")
			TEXT("scales with it while DragRate does not. Intended at the defaults; ")
			TEXT("raise DragRate and lower StepRatio to decouple."));
	}
}

void UGasGiantSimSubsystem::StopSimulation()
{
	bRunning = false;
}

void UGasGiantSimSubsystem::ResetSimulation()
{
	SimulatedTime = 0.0f;
	StepsCompleted = 0;
	StepAccumulator = 0.0f;

	if (!Simulation)
	{
		return;
	}

	FGasGiantSimulation* Sim = Simulation;

	ENQUEUE_RENDER_COMMAND(GasGiantReset)(
		[Sim](FRHICommandListImmediate&)
		{
			Sim->RequestReset();
		});

	// Then hand back the start state, if there is one. Both are render
	// commands and run in order, so the payload arrives after the reset flag
	// and survives it.
	//
	// A restored state is already spun up by definition, so the spin-up budget
	// is zero and simulated time continues from where it was captured -- which
	// keeps the forcing drift continuous rather than snapping it back.
	const bool bRestored = QueueInitialState();

	SpinUpTarget = (bRestored || !Config) ? 0 : FMath::Max(Config->SpinUpSteps, 0);

	if (bRestored)
	{
		SimulatedTime = Config->InitialState->SimulatedTime;
		StepsCompleted = Config->InitialState->StepsCompleted;
	}
}

bool UGasGiantSimSubsystem::QueueInitialState()
{
	if (!Config || !Config->InitialState || !Simulation)
	{
		return false;
	}

	UGasGiantSnapshot* Snapshot = Config->InitialState;

	const FIntVector Grid(
		FMath::Max(Config->GridLongitude & ~1, 32),
		FMath::Max(Config->GridLatitude, 16),
		FMath::Clamp(Config->LayerCount, 1, 8));

	if (!Snapshot->IsValidFor(Grid))
	{
		UE_LOG(LogGasGiant, Warning,
			TEXT("InitialState '%s' was captured at %dx%dx%d but the config is ")
			TEXT("%dx%dx%d. Seeding instead -- a vorticity field cannot be ")
			TEXT("resampled onto a different grid any more cheaply than it can ")
			TEXT("be re-spun."),
			*Snapshot->GetName(),
			Snapshot->Grid.X, Snapshot->Grid.Y, Snapshot->Grid.Z,
			Grid.X, Grid.Y, Grid.Z);

		return false;
	}

	// Shape mismatch is a WARNING, not a refusal. The nudge will re-register
	// the zonal mean over a few hundred steps, so the state is usable -- it
	// just is not the state that was captured, and it drifts toward the new
	// profile while looking like neither. Worth saying out loud, because
	// nothing about the result points back at the snapshot.
	FGasGiantSnapshotProvenance Now;
	Now.BandCount = Config->BandCount;
	Now.JetStrength = Config->JetStrength;
	Now.EquatorialBoost = Config->EquatorialBoost;
	Now.Asymmetry = Config->Asymmetry;
	Now.BandShape = Config->BandShape;
	Now.PlanetaryVorticity = Config->PlanetaryVorticity;

	if (!Snapshot->Provenance.MatchesShape(Now))
	{
		UE_LOG(LogGasGiant, Warning,
			TEXT("InitialState '%s' was captured under a different jet profile. ")
			TEXT("Its eddies sit on jets this config does not have; the nudge ")
			TEXT("will re-register them over a few hundred steps."),
			*Snapshot->GetName());
	}

	// One flat payload, vorticity then psi, matching the shader's layout.
	TArray<float> Payload;
	Payload.Reserve(Snapshot->Vorticity.Num() + Snapshot->Psi.Num());
	Payload.Append(Snapshot->Vorticity);
	Payload.Append(Snapshot->Psi);

	FGasGiantSimulation* Sim = Simulation;

	ENQUEUE_RENDER_COMMAND(GasGiantQueueRestore)(
		[Sim, Payload = MoveTemp(Payload)](FRHICommandListImmediate&) mutable
		{
			Sim->QueueRestore_RenderThread(MoveTemp(Payload));
		});

	UE_LOG(LogGasGiant, Log, TEXT("Starting from snapshot '%s' at simulated time %.2f."),
		*Snapshot->GetName(), Snapshot->SimulatedTime);

	return true;
}

bool UGasGiantSimSubsystem::SaveSnapshot(UGasGiantSnapshot* Target)
{
	if (!Target || !Config || !Simulation)
	{
		UE_LOG(LogGasGiant, Error, TEXT("SaveSnapshot needs a target asset and a running sim."));
		return false;
	}

	FGasGiantSimParams Params;
	if (!BuildParams(Params))
	{
		return false;
	}

	const int32 Total = Params.GridSize.X * Params.GridSize.Y * Params.GridSize.Z;

	FGasGiantSimulation* Sim = Simulation;

	// THE READBACK MUST BE LOCKED ON THE RENDER THREAD.
	//
	// FRHIGPUBufferReadback::Lock goes through RHILockStagingBuffer, which is
	// render-thread-only on every RHI. Calling it from the game thread after a
	// flush looks reasonable -- the work is demonstrably finished by then --
	// and crashes inside the RHI regardless, because the thread is what is
	// being checked, not the state.
	//
	// So the whole capture, wait and copy happens inside one render command,
	// and only the finished float array crosses back. Result and bSucceeded are
	// captured by reference, which is safe precisely because the flush below
	// blocks until the command has run.
	TArray<float> Result;
	bool bSucceeded = false;

	ENQUEUE_RENDER_COMMAND(GasGiantCapture)(
		[Sim, Params, Total, &Result, &bSucceeded](FRHICommandListImmediate& RHICmdList)
		{
			FRHIGPUBufferReadback Readback(TEXT("GasGiant.SnapshotReadback"));

			{
				FRDGBuilder GraphBuilder(RHICmdList);

				Sim->AddCapturePass_RenderThread(GraphBuilder, Params, &Readback);

				GraphBuilder.Execute();
			}

			// Submits the command list and waits. Heavy-handed, and correct for
			// an authoring path: the alternative is polling a fence across
			// frames, which means the asset write has to survive the world being
			// torn down underneath it.
			RHICmdList.BlockUntilGPUIdle();

			if (!Readback.IsReady())
			{
				return;
			}

			const uint32 Bytes = (uint32)Total * 2u * sizeof(float);

			if (const void* Data = Readback.Lock(Bytes))
			{
				Result.SetNumUninitialized(Total * 2);
				FMemory::Memcpy(Result.GetData(), Data, Bytes);
				bSucceeded = true;
			}

			Readback.Unlock();
		});

	FlushRenderingCommands();

	if (!bSucceeded || Result.Num() != Total * 2)
	{
		UE_LOG(LogGasGiant, Error,
			TEXT("Snapshot readback failed. Is the sim initialised and running?"));
		return false;
	}

	Target->Grid = Params.GridSize;
	Target->Vorticity.SetNumUninitialized(Total);
	Target->Psi.SetNumUninitialized(Total);
	FMemory::Memcpy(Target->Vorticity.GetData(), Result.GetData(), Total * sizeof(float));
	FMemory::Memcpy(Target->Psi.GetData(), Result.GetData() + Total, Total * sizeof(float));

	Target->Provenance.BandCount = Config->BandCount;
	Target->Provenance.JetStrength = Config->JetStrength;
	Target->Provenance.EquatorialBoost = Config->EquatorialBoost;
	Target->Provenance.Asymmetry = Config->Asymmetry;
	Target->Provenance.BandShape = Config->BandShape;
	Target->Provenance.PlanetaryVorticity = Config->PlanetaryVorticity;
	Target->SimulatedTime = SimulatedTime;
	Target->StepsCompleted = StepsCompleted;

	Target->MarkPackageDirty();

	UE_LOG(LogGasGiant, Display,
		TEXT("Saved snapshot '%s': %dx%dx%d, %d steps, simulated time %.2f."),
		*Target->GetName(), Target->Grid.X, Target->Grid.Y, Target->Grid.Z,
		StepsCompleted, SimulatedTime);

	return true;
}

void UGasGiantSimSubsystem::StepOnce(int32 NumSteps)
{
	PendingManualSteps += FMath::Max(NumSteps, 1);
}

bool UGasGiantSimSubsystem::PrepareTargets() const
{
	if (!Config)
	{
		return false;
	}

	const int32 W = Config->GridLongitude;
	const int32 H = Config->GridLatitude;
	const int32 Slices = FMath::Clamp(Config->LayerCount, 1, 8);

	// -- Flow target --------------------------------------------------------

	UTextureRenderTarget2DArray* Flow = Config->FlowTarget;

	if (!Flow)
	{
		UE_LOG(LogGasGiant, Error,
			TEXT("No FlowTarget set. Create a Texture Render Target 2D Array asset, ")
			TEXT("set it here, and the sim will size it automatically."));
		return false;
	}

	const bool bFlowMismatch =
		Flow->SizeX != W ||
		Flow->SizeY != H ||
		Flow->Slices != Slices ||
		Flow->OverrideFormat != PF_FloatRGBA ||
		!Flow->bCanCreateUAV;

	if (bFlowMismatch)
	{
		if (!Config->bAutoResizeTargets)
		{
			UE_LOG(LogGasGiant, Error,
				TEXT("FlowTarget is %dx%dx%d, needs %dx%dx%d RGBA16F with bCanCreateUAV. ")
				TEXT("Enable bAutoResizeTargets or fix the asset."),
				Flow->SizeX, Flow->SizeY, Flow->Slices, W, H, Slices);
			return false;
		}

		// bCanCreateUAV must be set BEFORE the resource is created, or the
		// texture comes back without UAV support and every dispatch that writes
		// it silently does nothing. That failure presents as a black flow
		// texture with no warning anywhere, which is why it is set here rather
		// than left to the asset.
		Flow->bCanCreateUAV = true;
		Flow->OverrideFormat = PF_FloatRGBA;
		Flow->ClearColor = FLinearColor::Black;
		Flow->Init(W, H, Slices, PF_FloatRGBA);
		Flow->UpdateResourceImmediate(true);

		UE_LOG(LogGasGiant, Log, TEXT("Resized FlowTarget to %dx%d x %d slices."), W, H, Slices);
	}

	// -- Debug target -------------------------------------------------------

	if (UTextureRenderTarget2D* Debug = Config->DebugTarget)
	{
		const bool bDebugMismatch =
			Debug->SizeX != W ||
			Debug->SizeY != H ||
			!Debug->bCanCreateUAV;

		if (bDebugMismatch && Config->bAutoResizeTargets)
		{
			Debug->bCanCreateUAV = true;
			Debug->ClearColor = FLinearColor::Black;
			Debug->InitCustomFormat(W, H, PF_FloatRGBA, /*bForceLinearGamma*/ true);
			Debug->UpdateResourceImmediate(true);

			UE_LOG(LogGasGiant, Log, TEXT("Resized DebugTarget to %dx%d."), W, H);
		}
	}

	return true;
}

bool UGasGiantSimSubsystem::BuildParams(FGasGiantSimParams& Out) const
{
	if (!Config)
	{
		return false;
	}

	// Longitude must be even: the polar fold in SimWrapCoord offsets by exactly
	// half the width. Rounded down rather than refused, since the alternative
	// is a stalled sim over a parameter nobody would think to check.
	const int32 W = FMath::Max(Config->GridLongitude & ~1, 32);
	const int32 H = FMath::Max(Config->GridLatitude, 16);
	const int32 Slices = FMath::Clamp(Config->LayerCount, 1, 8);

	Out.GridSize = FIntVector(W, H, Slices);

	Out.JetParams = FVector4f(
		Config->BandCount,
		Config->JetStrength,
		Config->EquatorialBoost,
		Config->Asymmetry);

	Out.BandShape = FVector4f(
		(float)Config->BandShape.X,
		(float)Config->BandShape.Y,
		(float)Config->BandShape.Z,
		0.0f);

	for (int32 i = 0; i < 8; ++i)
	{
		// Layers past the authored list fall back to an unscaled copy of the
		// shared profile rather than to zero. Zero would give a layer with no
		// jets at all, which reads as a bug in the sim rather than as a missing
		// array entry.
		const FGasGiantLayerProfile P = Config->LayerProfiles.IsValidIndex(i)
			? Config->LayerProfiles[i]
			: FGasGiantLayerProfile();

		Out.LayerProfile[i] = FVector4f(P.JetScale, P.BoostScale, P.ForcingScale, P.DragScale);
	}

	// Step = TimeScale * StepRatio. Strictly proportional and deliberately
	// unclamped: a Courant cap here would make the step proportional below the
	// cap and constant above it, so the numerical character would change at a
	// threshold the speed control gives no sign of. Courant is reported at
	// start and available from GetCourant() instead.
	Out.DeltaTime = FMath::Max(Config->TimeScale * Config->StepRatio, 0.0f);
	Out.Time = SimulatedTime;
	Out.PlanetaryVorticity = Config->PlanetaryVorticity;

	Out.ForcingChannel = FMath::Clamp(Config->ForcingChannel, 0, 3);
	Out.bForcingBipolar = Config->bForcingBipolar;

	Out.NudgeRate = Config->NudgeRate;
	Out.ForcingAmplitude = Config->ForcingAmplitude;
	Out.ForcingScale = Config->ForcingScale;
	Out.ForcingDrift = FVector3f(Config->ForcingDrift);
	Out.DragRate = Config->DragRate;
	Out.LayerCoupling = Config->LayerCoupling;

	Out.FilterLatitude = FMath::Clamp(Config->FilterLatitude, 0.0f, 1.0f);
	Out.FilterMaxHalfWidth = FMath::Clamp(Config->FilterMaxHalfWidth, 1, 256);

	Out.PoissonIterations = FMath::Clamp(Config->PoissonIterations, 1, 128);
	Out.InitPoissonIterations = FMath::Clamp(Config->InitPoissonIterations, 1, 4096);

	// Optimal over-relaxation, derived from the grid rather than authored.
	//
	//   rho_jacobi = (cos(pi/N) + cos(pi/M)) / 2
	//   w_opt      = 2 / (1 + sqrt(1 - rho^2))
	//
	// which tends to 2 as the grid grows -- 1.981 at 512x256. Derived because
	// the value is resolution dependent and a hardcoded one silently detunes
	// the solver the moment somebody changes the grid, in a way that looks like
	// a physics failure rather than a solver setting.
	if (Config->Relaxation <= 0.0f)
	{
		const double RhoJacobi = 0.5 * (
			FMath::Cos(UE_DOUBLE_PI / (double)W) +
			FMath::Cos(UE_DOUBLE_PI / (double)H));

		const double Wopt = 2.0 / (1.0 + FMath::Sqrt(FMath::Max(1.0 - RhoJacobi * RhoJacobi, 0.0)));

		Out.Relaxation = (float)FMath::Clamp(Wopt, 0.1, 1.99);
	}
	else
	{
		// Hard clamp below 2. At or above it the SOR iteration diverges
		// immediately, and the symptom -- psi saturating on the first frame --
		// is far enough from the cause to be worth making unreachable.
		Out.Relaxation = FMath::Clamp(Config->Relaxation, 0.1f, 1.99f);
	}

	const int32 ModeOverride = CVarGasGiantDebugMode.GetValueOnGameThread();
	const int32 LayerOverride = CVarGasGiantDebugLayer.GetValueOnGameThread();
	const float ScaleOverride = CVarGasGiantDebugScale.GetValueOnGameThread();

	Out.DebugMode = (ModeOverride >= 0) ? ModeOverride : (int32)Config->DebugMode;
	Out.DebugLayer = FMath::Clamp((LayerOverride >= 0) ? LayerOverride : Config->DebugLayer, 0, Slices - 1);
	// DEBUG SCALE DERIVED PER MODE when left at zero, because the seven fields
	// differ in magnitude by two orders. Vorticity is O(3), streamfunction
	// O(0.01), the residual near zero -- so one authored number is right for
	// one of them and renders a genuine residual failure as solid black.
	//
	// From the profile rather than a running maximum: a max that chases the
	// field hides a drifting magnitude, which is one of the things the view
	// exists to reveal.
	if (ScaleOverride > 0.0f)
	{
		Out.DebugScale = ScaleOverride;
	}
	else if (Config->DebugScale > 0.0f)
	{
		Out.DebugScale = Config->DebugScale;
	}
	else
	{
		const float PeakRate = GasGiantPeakRate(*Config);
		const float ZetaScale = Config->JetStrength * 0.6897f * Config->BandCount * UE_PI;
		const float PsiScale = PeakRate / FMath::Max(Config->BandCount * UE_PI, 1.0f);

		switch (Config->DebugMode)
		{
		case EGasGiantDebugMode::Vorticity:   Out.DebugScale = ZetaScale + Config->ForcingAmplitude; break;
		case EGasGiantDebugMode::Psi:         Out.DebugScale = PsiScale * 2.0f; break;
		case EGasGiantDebugMode::Speed:
		case EGasGiantDebugMode::East:        Out.DebugScale = PeakRate; break;
			// Meridional flow is eddy only, with no zonal contribution at all, so
			// it is far smaller than the eastward component.
		case EGasGiantDebugMode::North:       Out.DebugScale = PeakRate * 0.15f; break;
			// Should be near zero. Scaled hard so a residual that is merely small
			// still reads, rather than rounding to black alongside a converged one.
		case EGasGiantDebugMode::Residual:    Out.DebugScale = ZetaScale * 0.01f; break;
		case EGasGiantDebugMode::ZonalError:  Out.DebugScale = ZetaScale * 0.05f; break;
		default:                              Out.DebugScale = ZetaScale; break;
		}

		Out.DebugScale = FMath::Max(Out.DebugScale, 1e-6f);
	}

	// -- Resource handles ---------------------------------------------------

	if (Config->ForcingVolume && Config->ForcingVolume->GetResource())
	{
		Out.ForcingTexture = Config->ForcingVolume->GetResource()->TextureRHI;
	}

	if (Config->FlowTarget)
	{
		if (FTextureRenderTargetResource* Res = Config->FlowTarget->GameThread_GetRenderTargetResource())
		{
			Out.FlowTexture = Res->GetRenderTargetTexture();
		}
	}

	if (Config->DebugTarget)
	{
		if (FTextureRenderTargetResource* Res = Config->DebugTarget->GameThread_GetRenderTargetResource())
		{
			Out.DebugTexture = Res->GetRenderTargetTexture();
			Out.DebugSize = FIntPoint(Config->DebugTarget->SizeX, Config->DebugTarget->SizeY);
		}
	}

	return Out.FlowTexture.IsValid();
}

void UGasGiantSimSubsystem::TryAutoStart()
{
	const UGasGiantSimSettings* Settings = GetDefault<UGasGiantSimSettings>();

	if (!Settings || Settings->DefaultConfig.IsNull())
	{
		return;
	}

	const UWorld* World = GetWorld();

	if (!World)
	{
		return;
	}

	const bool bEditorWorld = (World->WorldType == EWorldType::Editor);
	const bool bWanted = bEditorWorld ? Settings->bAutoStartInEditor : Settings->bAutoStartInGame;

	if (!bWanted)
	{
		return;
	}

	if (UGasGiantSimConfig* Loaded = Settings->DefaultConfig.LoadSynchronous())
	{
		UE_LOG(LogGasGiant, Log, TEXT("Auto-starting against '%s' in %s world."),
			*Loaded->GetName(), bEditorWorld ? TEXT("editor") : TEXT("game"));

		StartSimulation(Loaded);
	}
}

void UGasGiantSimSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Once, on the first tick rather than in Initialize: resolving a soft
	// object reference during subsystem construction can run before the asset
	// registry is usable, and a failed load there is silent.
	if (!bTriedAutoStart)
	{
		bTriedAutoStart = true;
		TryAutoStart();
	}

	StepSimulation(DeltaTime);

	// AFTER, AND NOT INSIDE. The bake reads the flow texture the step above
	// writes, and render commands run in enqueue order, so this ordering is what
	// keeps the shadow from being cast by a one-frame-stale field -- shadows
	// beside the lumps that cast them.
	//
	// Outside StepSimulation because every early-out in there is a reason the
	// SIM should not advance, not a reason the deck stops casting. A stopped,
	// paused or fully spun-up sim still has a deck, and the camera is still
	// moving, so the fades baked into the map are still changing.
	BakeShadowMap();
}

void UGasGiantSimSubsystem::StepSimulation(float DeltaTime)
{
	if (!bRunning || !Config || !Simulation)
	{
		return;
	}

	if (!PrepareTargets())
	{
		return;
	}

	const int32 PauseOverride = CVarGasGiantPaused.GetValueOnGameThread();
	const bool bPaused = (PauseOverride >= 0) ? (PauseOverride != 0) : Config->bPaused;

	// -- Decide how many substeps this frame --------------------------------

	int32 Substeps = 0;

	// Derived exactly as BuildParams derives it, so the accumulator and the
	// shader agree about how much time a substep is worth. Computed here rather
	// than read back from Params because the substep COUNT has to be known
	// before the params are built.
	const float StepSize = FMath::Max(Config->TimeScale * Config->StepRatio, 0.0f);

	if (StepsCompleted < SpinUpTarget)
	{
		// SPIN-UP IS SPREAD OVER FRAMES, not run in one graph.
		//
		// Three hundred substeps is three thousand passes, which will hitch
		// visibly and may trip the driver's timeout. Spreading it also makes
		// the spin-up WATCHABLE, and that is where most of the diagnostic value
		// is: seeing whether the seed organises tells you more than the
		// converged state does, because a converged-looking field can be
		// converged for the wrong reason.
		Substeps = FMath::Min(
			FMath::Max(Config->MaxSpinUpStepsPerFrame, 1),
			SpinUpTarget - StepsCompleted);
	}
	else if (PendingManualSteps > 0)
	{
		Substeps = FMath::Min(PendingManualSteps, FMath::Max(Config->MaxSubstepsPerFrame, 1));
		PendingManualSteps -= Substeps;
	}
	else if (!bPaused && StepSize > 0.0f)
	{
		StepAccumulator += DeltaTime * Config->TimeScale;

		Substeps = FMath::FloorToInt(StepAccumulator / StepSize);
		Substeps = FMath::Min(Substeps, FMath::Max(Config->MaxSubstepsPerFrame, 1));

		// Consume only what was taken, then DISCARD the rest of the bank if it
		// exceeded the cap. Carrying it means a hitch is followed by a burst of
		// catch-up steps that makes the next frame worse, and on a heavily
		// loaded frame that spirals. Simulated time falling behind real time
		// during a stall is the correct behaviour for a visual effect.
		StepAccumulator -= Substeps * StepSize;
		StepAccumulator = FMath::Min(StepAccumulator, StepSize);
	}

	FGasGiantSimParams Params;
	if (!BuildParams(Params))
	{
		return;
	}

	// Time advances on the game thread so the forcing drift is consistent with
	// the step count regardless of when the render thread gets to it.
	SimulatedTime += Substeps * StepSize;
	StepsCompleted += Substeps;

	FGasGiantSimulation* Sim = Simulation;

	// Zero substeps still enqueues. The debug view must keep updating on a
	// paused or fully spun-up-and-idle sim, or switching debug modes while
	// paused would appear to do nothing.
	ENQUEUE_RENDER_COMMAND(GasGiantStep)(
		[Sim, Params, Substeps](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			Sim->Enqueue_RenderThread(GraphBuilder, Params, Substeps);

			GraphBuilder.Execute();
		});
}

void UGasGiantSimSubsystem::RequestShadowBake(const FGasGiantShadowParams& InParams)
{
	ShadowRequests.Add(InParams);
}

void UGasGiantSimSubsystem::BakeShadowMap()
{
	// CONSUMED, NOT HELD. A planet that stops asking stops baking on the next
	// tick, rather than leaving a map frozen at whatever light direction it last
	// pushed -- which would look like a shadow that works and is wrong.
	TArray<FGasGiantShadowParams> Requests = MoveTemp(ShadowRequests);
	ShadowRequests.Reset();

	for (const FGasGiantShadowParams& Params : Requests)
	{
		// Rejected here rather than on the render thread: a params struct is
		// cheap to refuse on the game thread and expensive to unwind once a
		// graph is building.
		if (!Params.IsUsable())
		{
			continue;
		}

		ENQUEUE_RENDER_COMMAND(GasGiantShadowBake)(
			[Params](FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);

				GasGiantShadow::AddBakePass_RenderThread(GraphBuilder, Params);

				GraphBuilder.Execute();
			});
	}
}