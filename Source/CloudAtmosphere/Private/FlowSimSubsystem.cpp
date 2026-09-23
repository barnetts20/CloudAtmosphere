#include "FlowSimSubsystem.h"

#include "GasGiantShadowMap.h"
#include "FlowSimulation.h"
#include "FlowSimShaders.h"
#include "FlowSimSettings.h"
#include "FlowSnapshot.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "Engine/VolumeTexture.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureRenderTarget2DArray.h"
#include "RenderingThread.h"

// TAutoConsoleVariable and FAutoConsoleCommandWithWorldAndArgs.
#include "HAL/IConsoleManager.h"

// UWorld::GetSubsystem, used by the console commands to find the right
// per-world instance.
#include "Engine/World.h"

// ---------------------------------------------------------------------------
// Console variables. The fastest debugging loop for a field like this is:
// change one thing, look, change it back.
// ---------------------------------------------------------------------------

static TAutoConsoleVariable<int32> CVarGasGiantDebugMode(
	TEXT("r.GasGiant.DebugMode"),
	-1,
	TEXT("Override the config's debug view. -1 uses the config.\n")
	TEXT("0 Vorticity, 1 Pressure, 2 Speed, 3 East, 4 North,\n")
	TEXT("5 Helmholtz residual, 6 Zonal profile error, 7 Vertical motion, 8 Froude, 9 Cloud, 10 Cloud formation ascent, 11 Noise displacement."),
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
// FConsoleCommandWithWorldAndArgsDelegate, because the subsystem is per world
// and the world handed back is the one the command was issued in -- the editor
// world from the editor console, the PIE world during play.
// ---------------------------------------------------------------------------

namespace
{
	UFlowSimSubsystem* FindSubsystem(UWorld* World)
	{
		return World ? World->GetSubsystem<UFlowSimSubsystem>() : nullptr;
	}

	/** Resolves an optional asset path argument, falling back to the project
	 *  setting. Both paths are logged, because "started against the wrong
	 *  config" and "did not start" look identical from the debug view. */
	UFlowSimConfig* ResolveConfig(const TArray<FString>& Args)
	{
		if (Args.Num() > 0)
		{
			UFlowSimConfig* Loaded = LoadObject<UFlowSimConfig>(nullptr, *Args[0]);

			if (!Loaded)
			{
				UE_LOG(LogFlowSim, Error, TEXT("No FlowSimConfig at '%s'."), *Args[0]);
			}

			return Loaded;
		}

		const UFlowSimSettings* Settings = GetDefault<UFlowSimSettings>();

		if (!Settings || Settings->DefaultConfig.IsNull())
		{
			UE_LOG(LogFlowSim, Error,
				TEXT("No config given and no DefaultConfig set in ")
				TEXT("Project Settings -> Plugins -> Gas Giant Sim."));
			return nullptr;
		}

		return Settings->DefaultConfig.LoadSynchronous();
	}
}

static FAutoConsoleCommandWithWorldAndArgs GFlowSimStartCmd(
	TEXT("FlowSim.Start"),
	TEXT("Start the flow sim. Optional argument is a FlowSimConfig asset path; ")
	TEXT("with none, uses the DefaultConfig from Project Settings."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (UFlowSimSubsystem* Sub = FindSubsystem(World))
			{
				if (UFlowSimConfig* Config = ResolveConfig(Args))
				{
					Sub->StartSimulation(Config);
					UE_LOG(LogFlowSim, Log, TEXT("Started against '%s'."), *Config->GetName());
				}
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GFlowSimStopCmd(
	TEXT("FlowSim.Stop"),
	TEXT("Stop stepping. State is kept, so FlowSim.Start resumes rather than reseeds."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>&, UWorld* World)
		{
			if (UFlowSimSubsystem* Sub = FindSubsystem(World))
			{
				Sub->StopSimulation();
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GFlowSimResetCmd(
	TEXT("FlowSim.Reset"),
	TEXT("Discard the field and reseed from the current config. Also the way to ")
	TEXT("pick up a changed grid size."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>&, UWorld* World)
		{
			if (UFlowSimSubsystem* Sub = FindSubsystem(World))
			{
				Sub->ResetSimulation();
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GFlowSimStepCmd(
	TEXT("FlowSim.Step"),
	TEXT("Advance N substeps while paused. Default 1."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (UFlowSimSubsystem* Sub = FindSubsystem(World))
			{
				Sub->StepOnce(Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 1);
			}
		}));

static FAutoConsoleCommandWithWorldAndArgs GFlowSimSaveCmd(
	TEXT("FlowSim.Save"),
	TEXT("Capture the live state into a FlowSnapshot asset. Argument is the ")
	TEXT("asset path. Blocks on the GPU; an authoring operation."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			if (Args.Num() == 0)
			{
				UE_LOG(LogFlowSim, Error, TEXT("FlowSim.Save needs a snapshot asset path."));
				return;
			}

			UFlowSimSubsystem* Sub = FindSubsystem(World);

			if (!Sub)
			{
				return;
			}

			UFlowSnapshot* Target = LoadObject<UFlowSnapshot>(nullptr, *Args[0]);

			if (!Target)
			{
				UE_LOG(LogFlowSim, Error,
					TEXT("No FlowSnapshot at '%s'. Create the asset first, then save into it."),
					*Args[0]);
				return;
			}

			Sub->SaveSnapshot(Target);
		}));

static FAutoConsoleCommandWithWorldAndArgs GFlowSimStatusCmd(
	TEXT("FlowSim.Status"),
	TEXT("Report step count, simulated time and spin-up progress."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>&, UWorld* World)
		{
			if (UFlowSimSubsystem* Sub = FindSubsystem(World))
			{
				UE_LOG(LogFlowSim, Display,
					TEXT("steps %d, simulated time %.2f, Courant %.3f, %s"),
					Sub->GetStepsCompleted(),
					Sub->GetSimulatedTime(),
					Sub->GetCourant(),
					Sub->IsSpinningUp() ? TEXT("spinning up") : TEXT("free running"));
			}
		}));

// ---------------------------------------------------------------------------

/** Peak angular rate the profile can reach in the fastest layer. The saturation
 *  caps the shaped term at 1 and the equatorial boost rides on top of it; each
 *  layer scales both. PITFALL: reading the shared profile alone under-reports a
 *  layer with JetScale above 1, and the Froude check then passes a regime the
 *  sim cannot balance. */
static float PeakRate(const UFlowSimConfig& Config)
{
	const int32 Layers = FMath::Clamp(Config.LayerCount, 1, 8);
	const bool bBanded = (Config.ZonalProfile == EFlowZonalProfile::Banded);

	float Peak = 0.0f;

	for (int32 i = 0; i < Layers; ++i)
	{
		const FFlowLayerProfile P = Config.LayerProfiles.IsValidIndex(i)
			? Config.LayerProfiles[i]
			: FFlowLayerProfile();

		const float Boost = bBanded ? FMath::Max(Config.EquatorialBoost * P.BoostScale, 0.0f) : 0.0f;

		Peak = FMath::Max(Peak, FMath::Abs(Config.JetStrength * P.JetScale) * (1.0f + Boost));
	}

	return FMath::Max(Peak, 1e-6f);
}

/** How many alternating shear zones the profile has from pole to pole: what
 *  the vorticity and pressure normalisations divide by. The three-cell profile
 *  has two jets' worth. */
static float ShearBands(const UFlowSimConfig& Config)
{
	return (Config.ZonalProfile == EFlowZonalProfile::Banded) ? Config.BandCount : 2.0f;
}

/** Gravity-wave speed from the deformation radius at 45 degrees. */
static float WaveSpeed(const UFlowSimConfig& Config)
{
	return FMath::Max(Config.DeformationRadius * Config.PlanetaryVorticity * 0.70710678f, 1e-3f);
}

void UFlowSimSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Simulation = new FFlowSimulation();
}

void UFlowSimSubsystem::Deinitialize()
{
	if (Simulation)
	{
		// The render thread owns the pooled allocations, so they are released
		// there, and the flush is what makes deleting the object afterwards safe.
		FFlowSimulation* Sim = Simulation;
		Simulation = nullptr;

		ENQUEUE_RENDER_COMMAND(FlowSimRelease)(
			[Sim](FRHICommandListImmediate&)
			{
				Sim->Release_RenderThread();
				delete Sim;
			});

		FlushRenderingCommands();
	}

	Super::Deinitialize();
}

bool UFlowSimSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game
		|| WorldType == EWorldType::PIE
		|| WorldType == EWorldType::Editor;
}

TStatId UFlowSimSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UFlowSimSubsystem, STATGROUP_Tickables);
}

void UFlowSimSubsystem::StartSimulation(UFlowSimConfig* InConfig)
{
	if (!InConfig)
	{
		UE_LOG(LogFlowSim, Warning, TEXT("StartSimulation called with a null config."));
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
	// resetting cannot diverge.
	ResetSimulation();
}

float UFlowSimSubsystem::GetCourant() const
{
	if (!Config)
	{
		return 0.0f;
	}

	const int32 W = FlowSimShader::GridLongitude(Config->GridLongitude);
	const float Step = Config->StepSize;

	return PeakRate(*Config) * Step * W / (2.0f * UE_PI);
}

void UFlowSimSubsystem::ReportInertSettings() const
{
	if (!Config)
	{
		return;
	}

	// Forcing with nowhere to sample from: amplitude, scale and lifetime all
	// dormant, and all switching on together when a volume is assigned.
	if (Config->ForcingAmplitude > 0.0f && !Config->ForcingVolume)
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("ForcingAmplitude is %.3f but no ForcingVolume is bound, so the ")
			TEXT("stochastic forcing is inactive. Assigning a volume will switch ")
			TEXT("amplitude, scale and lifetime on all at once."),
			Config->ForcingAmplitude);
	}

	if (Config->LayerCoupling > 0.0f && Config->LayerCount < 2)
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("LayerCoupling is %.3f but LayerCount is 1, so it does nothing."),
			Config->LayerCoupling);
	}

	if (Config->InitialState && Config->SpinUpSteps > 0)
	{
		UE_LOG(LogFlowSim, Log,
			TEXT("InitialState is bound, so SpinUpSteps (%d) is skipped. It still ")
			TEXT("matters when CREATING snapshots."),
			Config->SpinUpSteps);
	}

	if (Config->FilterLatitude <= 0.0f)
	{
		UE_LOG(LogFlowSim, Log,
			TEXT("FilterLatitude is 0, so the polar filter is disabled entirely ")
			TEXT("and FilterMaxHalfWidth has no effect."));
	}

	if (Config->DragRate <= 0.0f)
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("DragRate is 0: no Ekman convergence, so the vertical motion output ")
			TEXT("carries only gravity waves and the unbalanced flow, and the ")
			TEXT("stochastic forcing (scaled by drag) is off."));
	}
}

void UFlowSimSubsystem::ReportCourant() const
{
	if (!Config)
	{
		return;
	}

	// CONSEQUENCES, NOT CONTROLS. These fall out of the step with the profile,
	// the rotation and the grid; the substep rate falls out of the speed.
	const int32 W = FlowSimShader::GridLongitude(Config->GridLongitude);
	const float Step = Config->StepSize;
	const float C = WaveSpeed(*Config);

	const float Advective = GetCourant();
	const float Gravity = C * Step * W / (2.0f * UE_PI);
	const float Froude = PeakRate(*Config) / C;
	const float RotationPerStep = Config->PlanetaryVorticity * Step;

	UE_LOG(LogFlowSim, Log,
		TEXT("Step %.5f at TimeScale %.2f, %.1f substeps/frame at 60fps (cap %d). ")
		TEXT("Advective Courant %.3f, gravity-wave Courant %.3f (implicit), ")
		TEXT("Froude %.2f, Coriolis %.3f rad/step, wave speed %.3f."),
		Step, Config->TimeScale,
		Config->TimeScale / 60.0f / FMath::Max(Step, 1e-9f), Config->MaxSubstepsPerFrame,
		Advective, Gravity, Froude, RotationPerStep, C);

	if (Froude > 0.5f)
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("Froude %.2f: the peak flow is too fast for the gravity-wave speed ")
			TEXT("and will form hydraulic jumps. Raise DeformationRadius or ")
			TEXT("PlanetaryVorticity, or lower JetStrength."),
			Froude);
	}

	if (RotationPerStep > 0.5f)
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("Coriolis turns the flow %.2f rad per substep; the explicit ")
			TEXT("rotation and implicit pressure split loses accuracy, weakening ")
			TEXT("balanced jets and radiating gravity waves. ")
			TEXT("Lower StepSize."),
			RotationPerStep);
	}
}

void UFlowSimSubsystem::StopSimulation()
{
	bRunning = false;
}

void UFlowSimSubsystem::ResetSimulation()
{
	SimulatedTime = 0.0f;
	StepsCompleted = 0;
	StepAccumulator = 0.0f;

	if (!Simulation)
	{
		return;
	}

	FFlowSimulation* Sim = Simulation;

	ENQUEUE_RENDER_COMMAND(FlowSimReset)(
		[Sim](FRHICommandListImmediate&)
		{
			Sim->RequestReset();
		});

	// Then hand back the start state, if there is one. Render commands run in
	// order, so the payload arrives after the reset flag and survives it.
	const bool bRestored = QueueInitialState();

	SpinUpTarget = (bRestored || !Config) ? 0 : FMath::Max(Config->SpinUpSteps, 0);

	if (bRestored)
	{
		SimulatedTime = Config->InitialState->SimulatedTime;
		StepsCompleted = Config->InitialState->StepsCompleted;
	}
}

bool UFlowSimSubsystem::QueueInitialState()
{
	if (!Config || !Config->InitialState || !Simulation)
	{
		return false;
	}

	UFlowSnapshot* Snapshot = Config->InitialState;

	const FIntVector Grid(
		FlowSimShader::GridLongitude(Config->GridLongitude),
		FlowSimShader::GridLatitude(Config->GridLatitude),
		FMath::Clamp(Config->LayerCount, 1, 8));

	if (!Snapshot->IsValidFor(Grid))
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("InitialState '%s' does not match this solver's state at %dx%dx%d ")
			TEXT("(captured at %dx%dx%d, %d floats). Seeding instead."),
			*Snapshot->GetName(),
			Grid.X, Grid.Y, Grid.Z,
			Snapshot->Grid.X, Snapshot->Grid.Y, Snapshot->Grid.Z,
			Snapshot->State.Num());

		return false;
	}

	// Shape mismatch is a WARNING, not a refusal: the nudge re-registers the
	// zonal mean over a few hundred steps.
	FFlowSnapshotProvenance Now;
	Now.BandCount = Config->BandCount;
	Now.JetStrength = Config->JetStrength;
	Now.EquatorialBoost = Config->EquatorialBoost;
	Now.Asymmetry = Config->Asymmetry;
	Now.WidthBias = Config->WidthBias;
	Now.PlanetaryVorticity = Config->PlanetaryVorticity;

	if (!Snapshot->Provenance.MatchesShape(Now))
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("InitialState '%s' was captured under a different jet profile. ")
			TEXT("The nudge will re-register it over a few hundred steps."),
			*Snapshot->GetName());
	}

	TArray<float> Payload = Snapshot->State;

	FFlowSimulation* Sim = Simulation;

	ENQUEUE_RENDER_COMMAND(FlowSimQueueRestore)(
		[Sim, Payload = MoveTemp(Payload)](FRHICommandListImmediate&) mutable
		{
			Sim->QueueRestore_RenderThread(MoveTemp(Payload));
		});

	UE_LOG(LogFlowSim, Log, TEXT("Starting from snapshot '%s' at simulated time %.2f."),
		*Snapshot->GetName(), Snapshot->SimulatedTime);

	return true;
}

bool UFlowSimSubsystem::SaveSnapshot(UFlowSnapshot* Target)
{
	if (!Target || !Config || !Simulation)
	{
		UE_LOG(LogFlowSim, Error, TEXT("SaveSnapshot needs a target asset and a running sim."));
		return false;
	}

	FFlowSimParams Params;
	if (!BuildParams(Params))
	{
		return false;
	}

	const int32 Count = Params.GridSize.X * Params.GridSize.Y * Params.GridSize.Z
		* FFlowSimulation::StateFloatsPerCell;

	FFlowSimulation* Sim = Simulation;

	// THE READBACK MUST BE LOCKED ON THE RENDER THREAD. FRHIGPUBufferReadback::Lock
	// is render-thread-only on every RHI, flush or no flush. So the capture, wait
	// and copy happen inside one render command, and only the finished array
	// crosses back; the captures by reference are safe because the flush below
	// blocks until the command has run.
	TArray<float> Result;
	bool bSucceeded = false;

	ENQUEUE_RENDER_COMMAND(FlowSimCapture)(
		[Sim, Params, Count, &Result, &bSucceeded](FRHICommandListImmediate& RHICmdList)
		{
			FRHIGPUBufferReadback Readback(TEXT("FlowSim.SnapshotReadback"));

			{
				FRDGBuilder GraphBuilder(RHICmdList);

				Sim->AddCapturePass_RenderThread(GraphBuilder, Params, &Readback);

				GraphBuilder.Execute();
			}

			RHICmdList.BlockUntilGPUIdle();

			if (!Readback.IsReady())
			{
				return;
			}

			const uint32 Bytes = (uint32)Count * sizeof(float);

			if (const void* Data = Readback.Lock(Bytes))
			{
				Result.SetNumUninitialized(Count);
				FMemory::Memcpy(Result.GetData(), Data, Bytes);
				bSucceeded = true;
			}

			Readback.Unlock();
		});

	FlushRenderingCommands();

	if (!bSucceeded || Result.Num() != Count)
	{
		UE_LOG(LogFlowSim, Error,
			TEXT("Snapshot readback failed. Is the sim initialised and running?"));
		return false;
	}

	Target->Grid = Params.GridSize;
	Target->State = MoveTemp(Result);

	Target->Provenance.BandCount = Config->BandCount;
	Target->Provenance.JetStrength = Config->JetStrength;
	Target->Provenance.EquatorialBoost = Config->EquatorialBoost;
	Target->Provenance.Asymmetry = Config->Asymmetry;
	Target->Provenance.WidthBias = Config->WidthBias;
	Target->Provenance.PlanetaryVorticity = Config->PlanetaryVorticity;
	Target->SimulatedTime = SimulatedTime;
	Target->StepsCompleted = StepsCompleted;

	Target->MarkPackageDirty();

	UE_LOG(LogFlowSim, Display,
		TEXT("Saved snapshot '%s': %dx%dx%d, %d steps, simulated time %.2f."),
		*Target->GetName(), Target->Grid.X, Target->Grid.Y, Target->Grid.Z,
		StepsCompleted, SimulatedTime);

	return true;
}

void UFlowSimSubsystem::StepOnce(int32 NumSteps)
{
	PendingManualSteps += FMath::Max(NumSteps, 1);
}

bool UFlowSimSubsystem::PrepareTargets() const
{
	if (!Config)
	{
		return false;
	}

	const int32 GridW = FlowSimShader::GridLongitude(Config->GridLongitude);
	const int32 GridH = FlowSimShader::GridLatitude(Config->GridLatitude);

	// The cube atlas the sim resamples its output onto; see FlowField.ush.
	const FIntPoint Atlas = FlowSimShader::AtlasSize(FlowSimShader::AtlasFaceSize(GridW));

	const int32 W = Atlas.X;
	const int32 H = Atlas.Y;

	// Flow, weather, noise phase A and noise phase B slices, one of each per
	// layer.
	const int32 Slices = 4 * FMath::Clamp(Config->LayerCount, 1, 8);

	// -- Flow target --------------------------------------------------------

	UTextureRenderTarget2DArray* Flow = Config->FlowTarget;

	if (!Flow)
	{
		UE_LOG(LogFlowSim, Error,
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
			UE_LOG(LogFlowSim, Error,
				TEXT("FlowTarget is %dx%dx%d, needs %dx%dx%d RGBA16F with bCanCreateUAV. ")
				TEXT("Enable bAutoResizeTargets or fix the asset."),
				Flow->SizeX, Flow->SizeY, Flow->Slices, W, H, Slices);
			return false;
		}

		// bCanCreateUAV must be set BEFORE the resource is created, or the
		// texture comes back without UAV support and every dispatch that writes
		// it silently does nothing.
		Flow->bCanCreateUAV = true;
		Flow->OverrideFormat = PF_FloatRGBA;
		Flow->ClearColor = FLinearColor::Black;
		Flow->Init(W, H, Slices, PF_FloatRGBA);
		Flow->UpdateResourceImmediate(true);

		UE_LOG(LogFlowSim, Log, TEXT("Resized FlowTarget to %dx%d x %d slices."), W, H, Slices);
	}

	// -- Debug target -------------------------------------------------------

	if (UTextureRenderTarget2D* Debug = Config->DebugTarget)
	{
		const bool bDebugMismatch =
			Debug->SizeX != GridW ||
			Debug->SizeY != GridH ||
			!Debug->bCanCreateUAV;

		if (bDebugMismatch && Config->bAutoResizeTargets)
		{
			Debug->bCanCreateUAV = true;
			Debug->ClearColor = FLinearColor::Black;
			Debug->InitCustomFormat(GridW, GridH, PF_FloatRGBA, /*bForceLinearGamma*/ true);
			Debug->UpdateResourceImmediate(true);

			UE_LOG(LogFlowSim, Log, TEXT("Resized DebugTarget to %dx%d."), GridW, GridH);
		}
	}

	return true;
}

bool UFlowSimSubsystem::BuildParams(FFlowSimParams& Out) const
{
	if (!Config)
	{
		return false;
	}

	// Rounded to what the solver supports rather than refused.
	const int32 W = FlowSimShader::GridLongitude(Config->GridLongitude);
	const int32 H = FlowSimShader::GridLatitude(Config->GridLatitude);
	const int32 Layers = FMath::Clamp(Config->LayerCount, 1, 8);

	Out.GridSize = FIntVector(W, H, Layers);

	Out.JetParams = FVector4f(
		Config->BandCount,
		Config->JetStrength,
		Config->EquatorialBoost,
		Config->Asymmetry);

	Out.WidthBias = Config->WidthBias;
	Out.ZonalProfile = (int32)Config->ZonalProfile;

	for (int32 i = 0; i < 8; ++i)
	{
		// Layers past the authored list take an unscaled copy of the shared
		// profile rather than zero, which would read as a sim bug.
		const FFlowLayerProfile P = Config->LayerProfiles.IsValidIndex(i)
			? Config->LayerProfiles[i]
			: FFlowLayerProfile();

		Out.LayerProfile[i] = FVector4f(P.JetScale, P.BoostScale, P.ForcingScale, P.DragScale);
	}

	// A fixed step, independent of speed; the Courant numbers are reported
	// rather than enforced.
	Out.DeltaTime = FMath::Max(Config->StepSize, 0.0f);
	Out.Time = SimulatedTime;
	Out.PlanetaryVorticity = Config->PlanetaryVorticity;

	// -- Gravity waves and the Helmholtz solve --------------------------------

	const float C = WaveSpeed(*Config);

	Out.WaveSpeedSq = C * C;
	Out.ImplicitWeight = FMath::Clamp(Config->ImplicitWeight, 0.5f, 1.0f);

	const float ImplicitStep = Out.ImplicitWeight * Out.DeltaTime * C;
	Out.HelmholtzScale = ImplicitStep * ImplicitStep;

	// -- Forcing ------------------------------------------------------------

	Out.ForcingChannel = FMath::Clamp(Config->ForcingChannel, 0, 3);
	Out.bForcingBipolar = Config->bForcingBipolar;

	Out.NudgeRate = Config->NudgeRate;
	Out.ForcingAmplitude = Config->ForcingAmplitude;
	Out.ForcingScale = Config->ForcingScale;
	Out.ForcingLifetime = FMath::Max(Config->ForcingLifetime, 0.01f);
	Out.DragRate = Config->DragRate;
	Out.LayerCoupling = Config->LayerCoupling;
	Out.DivergenceDamping = FMath::Clamp(Config->DivergenceDamping, 0.0f, 0.5f);
	Out.ThermalRelaxation = FMath::Max(Config->ThermalRelaxation, 0.0f);

	Out.CondensationRate = FMath::Max(Config->CondensationRate, 0.0f);
	Out.EvaporationRate = FMath::Max(Config->EvaporationRate, 0.0f);
	Out.CloudLifetime = FMath::Max(Config->CloudLifetime, 1e-3f);

	Out.NoiseDriftRate = Config->GetNoiseDriftRate();
	Out.NoiseResetTime = Config->GetNoiseResetTime();

	Out.FilterLatitude = FMath::Clamp(Config->FilterLatitude, 0.0f, 1.0f);
	Out.FilterMaxHalfWidth = FMath::Clamp(Config->FilterMaxHalfWidth, 1, 256);

	// -- Output normalisation -------------------------------------------------
	//
	// FROM THE PROFILE, NOT A RUNNING MAXIMUM: a scale that chases the field
	// hides a drifting magnitude. Each channel is soft-saturated or near unit
	// range at these scales.
	//
	//   Vorticity  the zonal profile's peak shear.
	//   Pressure   geostrophic: f times a streamfunction of the jet speed over
	//              the band scale.
	//   Divergence vorticity times the Rossby number, which is how much of the
	//              flow is unbalanced.
	const float Peak = PeakRate(*Config);
	const float ZetaScale = FMath::Max(Config->JetStrength * 0.6897f * ShearBands(*Config) * UE_PI, 1e-4f);
	const float PsiScale = Peak / FMath::Max(ShearBands(*Config) * UE_PI, 1.0f);
	const float PressureScale = FMath::Max(Config->PlanetaryVorticity * 0.70710678f * PsiScale, 1e-5f);
	const float Rossby = FMath::Clamp(Peak / FMath::Max(Config->PlanetaryVorticity, 1e-4f), 0.01f, 1.0f);
	const float DivScale = FMath::Max(ZetaScale * Rossby, 1e-4f);

	Out.OutputScales = FVector3f(PressureScale, ZetaScale, DivScale);
	Out.AtlasFaceSize = FlowSimShader::AtlasFaceSize(W);

	// -- Debug --------------------------------------------------------------

	const int32 ModeOverride = CVarGasGiantDebugMode.GetValueOnGameThread();
	const int32 LayerOverride = CVarGasGiantDebugLayer.GetValueOnGameThread();
	const float ScaleOverride = CVarGasGiantDebugScale.GetValueOnGameThread();

	Out.DebugMode = (ModeOverride >= 0) ? ModeOverride : (int32)Config->DebugMode;
	Out.DebugLayer = FMath::Clamp((LayerOverride >= 0) ? LayerOverride : Config->DebugLayer, 0, Layers - 1);

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
		switch ((EFlowDebugMode)Out.DebugMode)
		{
		case EFlowDebugMode::Vorticity:   Out.DebugScale = ZetaScale; break;
		case EFlowDebugMode::Pressure:    Out.DebugScale = PressureScale * 2.0f; break;
		case EFlowDebugMode::Speed:
		case EFlowDebugMode::East:        Out.DebugScale = Peak; break;
			// Meridional flow is eddy only, far smaller than the eastward.
		case EFlowDebugMode::North:       Out.DebugScale = Peak * 0.15f; break;
			// Scaled hard so a residual that is merely small still reads.
		case EFlowDebugMode::Residual:    Out.DebugScale = PressureScale * 0.01f; break;
		case EFlowDebugMode::ZonalError:  Out.DebugScale = Peak * 0.05f; break;
		case EFlowDebugMode::Vertical:    Out.DebugScale = DivScale; break;
			// Saturates at Froude 1; the jump threshold is half way up.
		case EFlowDebugMode::Froude:      Out.DebugScale = 1.0f; break;
		case EFlowDebugMode::Cloud:
		case EFlowDebugMode::CloudAscent: Out.DebugScale = 1.0f; break;
			// A quarter radian, about the displacement at mid-life.
		case EFlowDebugMode::NoiseDisplacement: Out.DebugScale = 0.25f; break;
		default:                          Out.DebugScale = ZetaScale; break;
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

void UFlowSimSubsystem::TryAutoStart()
{
	const UFlowSimSettings* Settings = GetDefault<UFlowSimSettings>();

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

	if (UFlowSimConfig* Loaded = Settings->DefaultConfig.LoadSynchronous())
	{
		UE_LOG(LogFlowSim, Log, TEXT("Auto-starting against '%s' in %s world."),
			*Loaded->GetName(), bEditorWorld ? TEXT("editor") : TEXT("game"));

		StartSimulation(Loaded);
	}
}

void UFlowSimSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// On the first tick rather than in Initialize: resolving a soft reference
	// during subsystem construction can run before the asset registry is usable.
	if (!bTriedAutoStart)
	{
		bTriedAutoStart = true;
		TryAutoStart();
	}

	StepSimulation(DeltaTime);

	// AFTER, AND NOT INSIDE. The bake reads the flow texture the step writes, and
	// render commands run in enqueue order. Outside StepSimulation because a
	// stopped or paused sim still has a deck to shadow.
	BakeShadowMap();
}

void UFlowSimSubsystem::StepSimulation(float DeltaTime)
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

	int32 Substeps = 0;

	// Derived exactly as BuildParams derives it, so the accumulator and the
	// shader agree about how much time a substep is worth.
	const float StepSize = FMath::Max(Config->StepSize, 0.0f);

	if (StepsCompleted < SpinUpTarget)
	{
		// Spread over frames: one graph of hundreds of substeps hitches, and a
		// watchable spin-up says more than the converged state.
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

		// Consume what was taken and DISCARD the rest beyond one step: carrying
		// it turns a hitch into a burst that makes the next frame worse.
		StepAccumulator -= Substeps * StepSize;
		StepAccumulator = FMath::Min(StepAccumulator, StepSize);
	}

	FFlowSimParams Params;
	if (!BuildParams(Params))
	{
		return;
	}

	SimulatedTime += Substeps * StepSize;
	StepsCompleted += Substeps;

	FFlowSimulation* Sim = Simulation;

	// Zero substeps still enqueues, so the output and debug views keep updating
	// on a paused sim.
	ENQUEUE_RENDER_COMMAND(FlowSimStep)(
		[Sim, Params, Substeps](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			Sim->Enqueue_RenderThread(GraphBuilder, Params, Substeps);

			GraphBuilder.Execute();
		});
}

void UFlowSimSubsystem::RequestShadowBake(const FGasGiantShadowParams& InParams)
{
	ShadowRequests.Add(InParams);
}

void UFlowSimSubsystem::RequestShadowBake(const FTerrestrialShadowParams& InParams)
{
	TerrestrialShadowRequests.Add(InParams);
}

void UFlowSimSubsystem::BakeShadowMap()
{
	// CONSUMED, NOT HELD. A planet that stops asking stops baking on the next
	// tick, rather than leaving a map frozen at its last light direction.
	TArray<FGasGiantShadowParams> Requests = MoveTemp(ShadowRequests);
	ShadowRequests.Reset();

	for (const FGasGiantShadowParams& Params : Requests)
	{
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

	TArray<FTerrestrialShadowParams> TerrestrialRequests = MoveTemp(TerrestrialShadowRequests);
	TerrestrialShadowRequests.Reset();

	for (const FTerrestrialShadowParams& Params : TerrestrialRequests)
	{
		if (!Params.IsUsable())
		{
			continue;
		}

		ENQUEUE_RENDER_COMMAND(TerrestrialShadowBake)(
			[Params](FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);

				TerrestrialShadow::AddBakePass_RenderThread(GraphBuilder, Params);

				GraphBuilder.Execute();
			});
	}
}