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
#include "Algo/Sort.h"

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
	TEXT("5 Helmholtz residual, 6 Zonal profile error, 7 Vertical motion, 8 Froude, 9 Cloud, 10 Cloud formation ascent, 11 Noise displacement,\n")
	TEXT("12 Relative humidity, 13 Storm, 14 Layer top height."),
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
					TEXT("steps %d (%d last frame), simulated time %.4f, Courant %.3f, %s"),
					Sub->GetStepsCompleted(),
					Sub->GetStepsLastFrame(),
					Sub->GetSimulatedTime(),
					Sub->GetCourant(),
					Sub->IsSpinningUp() ? TEXT("spinning up") : TEXT("free running"));
			}
		}));

// ---------------------------------------------------------------------------
// The profile and the stack, on the CPU
// ---------------------------------------------------------------------------

namespace
{
	int32 LayerCountOf(const UFlowSimConfig& Config)
	{
		return FMath::Clamp(Config.LayerCount, 1, 8);
	}

	FFlowLayerProfile LayerOf(const UFlowSimConfig& Config, int32 Layer)
	{
		// Layers past the authored list take an unscaled copy of the shared
		// profile rather than zero, which would read as a sim bug.
		return Config.LayerProfiles.IsValidIndex(Layer) ? Config.LayerProfiles[Layer] : FFlowLayerProfile();
	}

	/** Share of the thermal shear a layer carries: 1 on top, 0 at the bottom. */
	float ShearShare(int32 Layer, int32 Layers)
	{
		return (Layers > 1) ? (float)(Layers - 1 - Layer) / (float)(Layers - 1) : 0.0f;
	}

	/** Mirrors GG_ZonalRate and SimThreeCellRate, for the reports. */
	float JetRate(const UFlowSimConfig& Config, float Mu, float Strength, float Boost)
	{
		if (Config.ZonalProfile == EFlowZonalProfile::ThreeCell)
		{
			const float A = FMath::Abs(Mu);
			const float Jet = (A - 0.71f) / 0.17f;
			const float Trades = Mu / 0.33f;
			const float Polar = (A - 0.97f) / 0.09f;

			return Strength * (FMath::Exp(-Jet * Jet) - 0.35f * FMath::Exp(-Trades * Trades) - 0.5f * FMath::Exp(-Polar * Polar));
		}

		const float K = Config.BandCount * UE_PI;
		const float Raw = (FMath::Cos(K * Mu) + 0.45f * FMath::Cos(K * 1.7f * Mu) + Config.Asymmetry * FMath::Sin(K * 0.6f * Mu))
			* 0.6897f - Config.WidthBias;

		float Saturated = Raw;
		const float Abs = FMath::Abs(Raw);

		if (Abs > 0.5f)
		{
			const float U = FMath::Clamp(Abs - 0.5f, 0.0f, 1.0f);
			const float U4 = U * U * U * U;
			Saturated = FMath::Sign(Raw) * (0.5f + U - (U4 * U * U - 3.0f * U4 * U + 2.5f * U4));
		}

		return Strength * (Saturated + Boost * FMath::Exp(-Mu * Mu * 12.0f));
	}

	/** Mirrors SimZonalRate: the layer's jets plus its share of the shear. */
	float LayerZonalRate(const UFlowSimConfig& Config, float Mu, int32 Layer)
	{
		const FFlowLayerProfile P = LayerOf(Config, Layer);
		const float Jets = JetRate(Config, Mu, Config.JetStrength * P.JetScale, Config.EquatorialBoost * P.BoostScale);

		float Shape = 0.0f;

		if (Config.ThermalShape == EFlowThermalShape::FollowJets)
		{
			Shape = JetRate(Config, Mu, 1.0f, Config.EquatorialBoost);
		}
		else
		{
			const float Offset = (FMath::Abs(FMath::Asin(FMath::Clamp(Mu, -1.0f, 1.0f))) - FMath::DegreesToRadians(Config.BaroclinicLatitude))
				/ FMath::Max(FMath::DegreesToRadians(Config.BaroclinicWidth), 1e-3f);

			Shape = FMath::Exp(-Offset * Offset);
		}

		return Jets + ShearShare(Layer, LayerCountOf(Config)) * Config.ThermalShear * Shape;
	}

	/** Eigen-decomposition of a symmetric matrix by cyclic Jacobi rotations:
	 *  S = Q diag(Lambda) Q^T, eigenvectors in Q's columns. Exact to double
	 *  precision in a few sweeps at the stack's size. */
	void SymmetricEigen(int32 N, double S[8][8], double Lambda[8], double Q[8][8])
	{
		for (int32 i = 0; i < N; ++i)
		{
			for (int32 j = 0; j < N; ++j)
			{
				Q[i][j] = (i == j) ? 1.0 : 0.0;
			}
		}

		for (int32 Sweep = 0; Sweep < 64; ++Sweep)
		{
			double Off = 0.0;

			for (int32 p = 0; p < N; ++p)
			{
				for (int32 q = p + 1; q < N; ++q)
				{
					Off += S[p][q] * S[p][q];
				}
			}

			if (Off < 1e-24)
			{
				break;
			}

			for (int32 p = 0; p < N; ++p)
			{
				for (int32 q = p + 1; q < N; ++q)
				{
					if (FMath::Abs(S[p][q]) < 1e-300)
					{
						continue;
					}

					const double Theta = 0.5 * (S[q][q] - S[p][p]) / S[p][q];
					const double T = (Theta >= 0.0 ? 1.0 : -1.0) / (FMath::Abs(Theta) + FMath::Sqrt(Theta * Theta + 1.0));
					const double C = 1.0 / FMath::Sqrt(T * T + 1.0);
					const double Sn = T * C;

					for (int32 k = 0; k < N; ++k)
					{
						const double Skp = S[k][p];
						const double Skq = S[k][q];
						S[k][p] = C * Skp - Sn * Skq;
						S[k][q] = Sn * Skp + C * Skq;
					}

					for (int32 k = 0; k < N; ++k)
					{
						const double Spk = S[p][k];
						const double Sqk = S[q][k];
						S[p][k] = C * Spk - Sn * Sqk;
						S[q][k] = Sn * Spk + C * Sqk;
					}

					for (int32 k = 0; k < N; ++k)
					{
						const double Qkp = Q[k][p];
						const double Qkq = Q[k][q];
						Q[k][p] = C * Qkp - Sn * Qkq;
						Q[k][q] = Sn * Qkp + C * Qkq;
					}
				}
			}
		}

		for (int32 i = 0; i < N; ++i)
		{
			Lambda[i] = S[i][i];
		}
	}

	void PackMatrix(FVector4f Out[16], const double M[8][8], int32 N)
	{
		for (int32 i = 0; i < 16; ++i)
		{
			Out[i] = FVector4f::Zero();
		}

		for (int32 r = 0; r < N; ++r)
		{
			for (int32 c = 0; c < N; ++c)
			{
				const int32 Index = r * 8 + c;
				Out[Index >> 2][Index & 3] = (float)M[r][c];
			}
		}
	}

	/** The stack's vertical structure. Layer k (0 on top) feels
	 *  M_k = sum_j A_kj phi_j with A_kj = 1 + Stratification * min(k, j): the
	 *  free surface, plus the density step of every interface above it. The
	 *  implicit operator is diag(depth) A, similar to the symmetric
	 *  D^1/2 A D^1/2, whose eigenvectors give the modes. Depths are scaled so
	 *  the mode DeformationRadius names runs at its wave speed. */
	FFlowSimStack BuildStack(const UFlowSimConfig& Config, float WaveSpeed)
	{
		const int32 N = LayerCountOf(Config);
		const double Eps = FMath::Clamp(Config.Stratification, 0.01f, 1.0f);

		double Share[8] = {};
		double Total = 0.0;

		for (int32 k = 0; k < N; ++k)
		{
			Share[k] = FMath::Max(LayerOf(Config, k).DepthScale, 0.1f);
			Total += Share[k];
		}

		double A[8][8] = {};
		double S[8][8] = {};

		for (int32 k = 0; k < N; ++k)
		{
			Share[k] /= Total;
		}

		for (int32 k = 0; k < N; ++k)
		{
			for (int32 j = 0; j < N; ++j)
			{
				A[k][j] = 1.0 + Eps * FMath::Min(k, j);
				S[k][j] = FMath::Sqrt(Share[k]) * A[k][j] * FMath::Sqrt(Share[j]);
			}
		}

		double Lambda[8] = {};
		double Q[8][8] = {};
		SymmetricEigen(N, S, Lambda, Q);

		// Fastest mode first, so slice 0 is the external mode.
		int32 Order[8];

		for (int32 m = 0; m < N; ++m)
		{
			Order[m] = m;
		}

		Algo::Sort(MakeArrayView(Order, N), [&Lambda](int32 L, int32 R) { return Lambda[L] > Lambda[R]; });

		const double Design = (N > 1) ? Lambda[Order[1]] : Lambda[Order[0]];
		const double Scale = (double)WaveSpeed * WaveSpeed / FMath::Max(Design, 1e-12);

		FFlowSimStack Out;
		Out.DesignSpeedSq = WaveSpeed * WaveSpeed;

		double Depth[8] = {};
		double R[8][8] = {};
		double RInv[8][8] = {};
		double AR[8][8] = {};
		double AInv[8][8] = {};
		double Speed[8] = {};

		for (int32 k = 0; k < N; ++k)
		{
			Depth[k] = Share[k] * Scale;
			Out.Depth[k] = (float)Depth[k];
		}

		for (int32 m = 0; m < N; ++m)
		{
			Speed[m] = Lambda[Order[m]] * Scale;
			Out.ModeSpeedSq[m] = (float)Speed[m];

			for (int32 k = 0; k < N; ++k)
			{
				R[k][m] = FMath::Sqrt(Depth[k]) * Q[k][Order[m]];
				RInv[m][k] = Q[k][Order[m]] / FMath::Sqrt(Depth[k]);
			}
		}

		// A R, and A^-1 = R Lambda^-1 R^-1 D.
		for (int32 k = 0; k < N; ++k)
		{
			for (int32 m = 0; m < N; ++m)
			{
				for (int32 j = 0; j < N; ++j)
				{
					AR[k][m] += A[k][j] * R[j][m];
					AInv[k][m] += R[k][j] / Speed[j] * RInv[j][m] * Depth[m];
				}
			}
		}

		PackMatrix(Out.Montgomery, A, N);
		PackMatrix(Out.MontgomeryInverse, AInv, N);
		PackMatrix(Out.ModeToLayer, R, N);
		PackMatrix(Out.LayerToMode, RInv, N);
		PackMatrix(Out.ModeToMontgomery, AR, N);

		return Out;
	}

	float MatrixEntry(const FVector4f M[16], int32 Row, int32 Col)
	{
		const int32 Index = Row * 8 + Col;
		return M[Index >> 2][Index & 3];
	}
}

/** Peak angular rate the profile can reach in the fastest layer, shear
 *  included. PITFALL: reading the shared profile alone under-reports a layer
 *  with JetScale above 1 or the top of a sheared stack, and the Froude check
 *  then passes a regime the sim cannot balance. */
static float PeakRate(const UFlowSimConfig& Config)
{
	const int32 Layers = LayerCountOf(Config);
	const bool bBanded = (Config.ZonalProfile == EFlowZonalProfile::Banded);

	float Peak = 0.0f;

	for (int32 i = 0; i < Layers; ++i)
	{
		const FFlowLayerProfile P = LayerOf(Config, i);

		const float Boost = bBanded ? FMath::Max(Config.EquatorialBoost * P.BoostScale, 0.0f) : 0.0f;

		Peak = FMath::Max(Peak, FMath::Abs(Config.JetStrength * P.JetScale) * (1.0f + Boost)
			+ ShearShare(i, Layers) * FMath::Abs(Config.ThermalShear));
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

/** Wave speed from the deformation radius at 45 degrees. */
static float WaveSpeed(const UFlowSimConfig& Config)
{
	return FMath::Max(Config.DeformationRadius * Config.PlanetaryVorticity * 0.70710678f, 1e-3f);
}

/** Implicit weight at a step: a stack at a large step runs at no less than
 *  FlowSimStep::StackWeight. */
static float ImplicitWeightAt(const UFlowSimConfig& Config, float Step)
{
	const float Authored = FMath::Clamp(Config.ImplicitWeight, 0.5f, 1.0f);

	return (LayerCountOf(Config) > 1 && Step > FlowSimStep::StackLargeStep)
		? FMath::Max(Authored, FlowSimStep::StackWeight)
		: Authored;
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

	SimulatedTime = 0.0f;
	StepsCompleted = 0;
	PendingManualSteps = 0;

	ReportCourant();
	ReportStack();
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
	return PeakRate(*Config) * CurrentStep * W / (2.0f * UE_PI);
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

	if (LayerCountOf(*Config) < 2)
	{
		if (Config->LayerCoupling > 0.0f)
		{
			UE_LOG(LogFlowSim, Warning,
				TEXT("LayerCoupling is %.3f but LayerCount is 1, so it does nothing."),
				Config->LayerCoupling);
		}

		if (Config->ThermalShear != 0.0f)
		{
			UE_LOG(LogFlowSim, Warning,
				TEXT("ThermalShear is %.3f but LayerCount is 1: a single layer has no ")
				TEXT("vertical shear, so it does nothing."),
				Config->ThermalShear);
		}
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

	if (Config->SurfaceEvaporation <= 0.0f)
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("SurfaceEvaporation is 0: nothing replaces the vapour that rains out, ")
			TEXT("so cloud and storms fade as the sky dries."));
	}
}

void UFlowSimSubsystem::ReportCourant() const
{
	if (!Config)
	{
		return;
	}

	// CONSEQUENCES, NOT CONTROLS. These fall out of the step with the profile,
	// the rotation and the grid; the step rate falls out of the speed.
	const int32 W = FlowSimShader::GridLongitude(Config->GridLongitude);
	const float Step = Config->GetStepSize();
	const float Steps = FMath::Max(Config->SimSpeed, 0.0f) / 60.0f / Step;
	const float C = WaveSpeed(*Config);

	const float Advective = PeakRate(*Config) * Step * W / (2.0f * UE_PI);
	const float Gravity = C * Step * W / (2.0f * UE_PI);
	const float Froude = PeakRate(*Config) / C;
	const float RotationPerStep = Config->PlanetaryVorticity * Step;

	UE_LOG(LogFlowSim, Log,
		TEXT("Speed %.4f: %.1f steps of %.6f per frame at 60fps. ")
		TEXT("Advective Courant %.3f, gravity-wave Courant %.3f (implicit), ")
		TEXT("Froude %.2f, Coriolis %.3f rad/step, wave speed %.3f."),
		Config->SimSpeed, Steps, Step,
		Advective, Gravity, Froude, RotationPerStep, C);

	if (Froude > 0.5f)
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("Froude %.2f: the peak flow is too fast for the gravity-wave speed ")
			TEXT("and will form hydraulic jumps. Raise DeformationRadius or ")
			TEXT("PlanetaryVorticity, or lower JetStrength or ThermalShear."),
			Froude);
	}

	if (RotationPerStep > 0.5f)
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("Coriolis turns the flow %.2f rad per substep; the explicit ")
			TEXT("rotation and implicit pressure split loses accuracy, weakening ")
			TEXT("balanced jets and radiating gravity waves. ")
			TEXT("Lower PlanetaryVorticity."),
			RotationPerStep);
	}
}

void UFlowSimSubsystem::ReportStack() const
{
	if (!Config)
	{
		return;
	}

	const int32 N = LayerCountOf(*Config);
	const FFlowSimStack Stack = BuildStack(*Config, WaveSpeed(*Config));

	FString Speeds;

	for (int32 m = 0; m < N; ++m)
	{
		Speeds += FString::Printf(TEXT("%s%.2f"), m ? TEXT(", ") : TEXT(""), FMath::Sqrt(Stack.ModeSpeedSq[m]));
	}

	UE_LOG(LogFlowSim, Log, TEXT("%d layer(s); mode wave speeds %s."), N, *Speeds);

	if (N < 2)
	{
		return;
	}

	// Storms grow once the shear exceeds about beta times twice the square of
	// the local deformation radius (two equal layers, the classic criterion).
	const float Lat = FMath::DegreesToRadians(FMath::Clamp(Config->BaroclinicLatitude, 10.0f, 80.0f));
	const float F = Config->PlanetaryVorticity * FMath::Sin(Lat);
	const float Beta = Config->PlanetaryVorticity * FMath::Cos(Lat);
	const float LocalRadius = WaveSpeed(*Config) / FMath::Max(F, 1e-3f);
	const float Critical = 2.0f * Beta * LocalRadius * LocalRadius;
	const float Drive = FMath::Abs(Config->ThermalShear) * FMath::Cos(Lat) / FMath::Max(Critical, 1e-6f);

	UE_LOG(LogFlowSim, Log,
		TEXT("Thermal shear %.2f against a critical %.2f at %.0f degrees: %.2fx. ")
		TEXT("Storms grow from the shear above about 1."),
		FMath::Abs(Config->ThermalShear) * FMath::Cos(Lat), Critical, Config->BaroclinicLatitude, Drive);

	// The balanced interfaces, from the same profile the balance pass
	// integrates: where a layer thins toward nothing, it has run into the top
	// or bottom of the stack.
	const int32 Rows = FlowSimShader::GridLatitude(Config->GridLatitude);
	const float DMu = 2.0f / Rows;

	TArray<float> M;
	M.SetNumZeroed(N * Rows);

	for (int32 k = 0; k < N; ++k)
	{
		float Sum = 0.0f;

		for (int32 j = 1; j < Rows; ++j)
		{
			const float MuF = -1.0f + j * DMu;
			const float R = LayerZonalRate(*Config, MuF, k);

			M[k * Rows + j] = M[k * Rows + j - 1] - DMu * MuF * R * (Config->PlanetaryVorticity + R);
			Sum += M[k * Rows + j];
		}

		for (int32 j = 0; j < Rows; ++j)
		{
			M[k * Rows + j] -= Sum / Rows;
		}
	}

	float Thinnest = 1.0f;
	int32 ThinLayer = 0;

	for (int32 k = 0; k < N; ++k)
	{
		for (int32 j = 0; j < Rows; ++j)
		{
			float Phi = 0.0f;

			for (int32 l = 0; l < N; ++l)
			{
				Phi += MatrixEntry(Stack.MontgomeryInverse, k, l) * M[l * Rows + j];
			}

			const float Relative = Phi / FMath::Max(Stack.Depth[k], 1e-6f);

			if (Relative < Thinnest)
			{
				Thinnest = Relative;
				ThinLayer = k;
			}
		}
	}

	if (Thinnest < -0.75f)
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("At balance, layer %d thins to %.0f%% of its depth: the interface ")
			TEXT("nearly reaches the edge of the stack, which the thickness floor ")
			TEXT("clips. Lower ThermalShear, widen DeformationRadius, or deepen that ")
			TEXT("layer with DepthScale."),
			ThinLayer, 100.0f * (1.0f + Thinnest));
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
	CurrentStep = Config ? Config->GetStepSize() : 1e-5f;
	PendingTime = CurrentStep;
	StateBlend = 1.0f;

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

	// Said every time: a missing or broken InitialState reference falls back to
	// a seed, which is otherwise indistinguishable from a restore gone wrong.
	if (!bRestored && Config)
	{
		UE_LOG(LogFlowSim, Display, TEXT("Reseeding '%s' (InitialState %s); %d spin-up steps."),
			*Config->GetName(),
			Config->InitialState ? TEXT("refused, see above") : TEXT("not bound"),
			SpinUpTarget);
	}

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
		LayerCountOf(*Config));

	if (!Snapshot->IsValidFor(Grid))
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("InitialState '%s' does not match this solver's state at %dx%dx%d ")
			TEXT("(captured at %dx%dx%d, %d floats; the solver needs %d). ")
			TEXT("Seeding instead."),
			*Snapshot->GetName(),
			Grid.X, Grid.Y, Grid.Z,
			Snapshot->Grid.X, Snapshot->Grid.Y, Snapshot->Grid.Z,
			Snapshot->State.Num(), FFlowSimulation::StateFloats(Grid));

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
	Now.ThermalShear = Config->ThermalShear;

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
	if (!BuildParams(Params, CurrentStep))
	{
		return false;
	}

	const int32 Count = FFlowSimulation::StateFloats(Params.GridSize);

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
	Target->Provenance.ThermalShear = Config->ThermalShear;
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
	const int32 Slices = 4 * LayerCountOf(*Config);

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

bool UFlowSimSubsystem::BuildParams(FFlowSimParams& Out, float Step) const
{
	if (!Config)
	{
		return false;
	}

	// Rounded to what the solver supports rather than refused.
	const int32 W = FlowSimShader::GridLongitude(Config->GridLongitude);
	const int32 H = FlowSimShader::GridLatitude(Config->GridLatitude);
	const int32 Layers = LayerCountOf(*Config);

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
		const FFlowLayerProfile P = LayerOf(*Config, i);

		Out.LayerProfile[i] = FVector4f(P.JetScale, P.BoostScale, P.ForcingScale, P.DragScale);
	}

	Out.DeltaTime = FMath::Clamp(Step, 0.0f, FlowSimStep::SpinUp);
	Out.Time = SimulatedTime;
	Out.PlanetaryVorticity = Config->PlanetaryVorticity;
	Out.ImplicitWeight = ImplicitWeightAt(*Config, Out.DeltaTime);

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
	Out.ThermalParams = FVector4f(
		Config->ThermalShear,
		Config->ThermalShape == EFlowThermalShape::FollowJets ? 1.0f : 0.0f,
		FMath::DegreesToRadians(FMath::Clamp(Config->BaroclinicLatitude, 0.0f, 90.0f)),
		FMath::DegreesToRadians(FMath::Max(Config->BaroclinicWidth, 1.0f)));

	// -- Moisture, cloud and storms -------------------------------------------

	Out.CondensationRate = FMath::Max(Config->CondensationRate, 0.0f);
	Out.EvaporationRate = FMath::Max(Config->EvaporationRate, 0.0f);
	Out.CloudLifetime = FMath::Max(Config->CloudLifetime, 1e-3f);

	Out.MoistureParams = FVector4f(
		FMath::Max(Config->SaturationEquator, 0.0f),
		FMath::Max(Config->SaturationPole, 0.0f),
		FMath::Clamp(Config->CondensationOnset, 0.0f, 0.99f),
		FMath::Max(Config->SurfaceEvaporation, 0.0f));

	Out.LatentHeating = FMath::Max(Config->LatentHeating, 0.0f);
	Out.WindEvaporation = FMath::Max(Config->WindEvaporation, 0.0f);

	Out.StormParams = FVector4f(
		FMath::Max(Config->StormRate, 0.0f),
		FMath::Clamp(Config->StormThreshold, 0.0f, 0.99f),
		FMath::Max(Config->StormSpin, 0.0f),
		1.0f / FMath::Max(Config->StormLifetime, 1e-3f));

	// -- Storm cells ------------------------------------------------------------

	const float GenesisMin = FMath::DegreesToRadians(FMath::Clamp(Config->GenesisLatitudeMin, 0.0f, 90.0f));
	const float GenesisMax = FMath::DegreesToRadians(FMath::Clamp(Config->GenesisLatitudeMax, 0.0f, 90.0f));

	const float Eye = FMath::Clamp(Config->StormCellEye, 0.0f, 0.9f);

	Out.CellShape = FVector4f(
		FMath::DegreesToRadians(FMath::Clamp(Config->StormCellRadius, 0.5f, 45.0f)),
		Eye,
		FMath::Clamp(Config->StormCellEyewall, Eye + 0.01f, 0.95f),
		FMath::Clamp(Config->StormCellEyeStrength, 0.0f, 1.0f));

	Out.CellVortex = FVector4f(
		FMath::Max(Config->StormCellFalloff, 0.1f),
		FMath::Max(Config->StormCellForcing, 0.0f),
		FMath::Clamp(Config->StormCellTopShare, -1.0f, 1.0f),
		FMath::Max(Config->StormCellWind, 0.0f));

	Out.CellDraft = FVector4f(
		FMath::Clamp(Config->StormCellDraft, -1.0f, 1.0f),
		FMath::Clamp(Config->StormCellEyeDraft, -1.0f, 1.0f),
		FMath::Clamp(Config->StormCellBandStorm, 0.0f, 1.0f),
		FMath::Clamp(Config->StormCellBandPressure, 0.0f, 2.0f));

	Out.CellLife = FVector4f(
		FMath::Max(Config->StormCellSpawnRate, 0.0f),
		FMath::Max(Config->StormCellGrowth, 0.0f),
		FMath::Max(Config->StormCellDecay, 0.0f),
		FMath::Max(Config->StormCellLifetime, 0.01f));

	Out.CellMotion = FVector4f(
		FMath::Max(Config->StormCellDrift, 0.0f),
		FMath::Max(Config->StormCellFollow, 0.0f),
		FMath::Min(GenesisMin, GenesisMax),
		FMath::Max(GenesisMin, GenesisMax));

	Out.CellGenesis = FVector4f(
		FMath::Max(Config->GenesisShear, 0.01f),
		FMath::Clamp(Config->GenesisHumidity, 0.0f, 1.0f),
		FMath::Clamp(Config->GenesisStorm, 0.01f, 1.0f),
		FMath::Max(Config->GenesisSpin, 0.0f));

	Out.CellCloud = FVector4f(
		FMath::Clamp(Config->StormCellCanopy, 0.0f, 1.0f),
		FMath::Clamp(Config->StormCellCanopyRadius, 0.05f, 1.0f),
		FMath::Clamp(Config->StormCellFeed, 0.0f, 1.0f),
		FMath::Clamp(Config->StormCellFeedFloor, 0.0f, 1.0f));

	Out.CellPatch = FVector4f(
		FMath::Clamp(Config->StormCellFeedFill, 0.0f, 1.0f),
		FMath::Max(Config->StormCellPatchScale, 0.5f),
		FMath::Max(Config->StormCellFeedRate, 0.0f),
		FMath::Clamp(Config->StormCellInflow, 0.0f, 1.0f));

	Out.CellCount = FMath::Clamp(Config->MaxStormCells, 0, FlowSimShader::MaxStormCells);

	Out.StepIndex = StepsCompleted;

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
	//   Height     pressure over the density step an interface carries it by.
	const float Peak = PeakRate(*Config);
	const float ZetaScale = FMath::Max(Config->JetStrength * 0.6897f * ShearBands(*Config) * UE_PI, 1e-4f);
	const float PsiScale = Peak / FMath::Max(ShearBands(*Config) * UE_PI, 1.0f);
	const float PressureScale = FMath::Max(Config->PlanetaryVorticity * 0.70710678f * PsiScale, 1e-5f);
	const float Rossby = FMath::Clamp(Peak / FMath::Max(Config->PlanetaryVorticity, 1e-4f), 0.01f, 1.0f);
	const float DivScale = FMath::Max(ZetaScale * Rossby, 1e-4f);

	Out.OutputScales = FVector3f(PressureScale, ZetaScale, DivScale);
	Out.AtlasFaceSize = FlowSimShader::AtlasFaceSize(W);

	// -- The stack ------------------------------------------------------------

	Out.Stack = BuildStack(*Config, WaveSpeed(*Config));

	const float ImplicitStep = Out.ImplicitWeight * Out.DeltaTime;
	const float Stratification = FMath::Clamp(Config->Stratification, 0.01f, 1.0f);

	for (int32 k = 0; k < 8; ++k)
	{
		if (k >= Layers)
		{
			Out.LayerState[k] = FVector4f(1.0f, 0.0f, 1.0f, 1.0f);
			continue;
		}

		const float Saturation = (Layers > 1)
			? FMath::Pow(FMath::Clamp(Config->UpperSaturation, 0.0f, 1.0f), ShearShare(k, Layers))
			: 1.0f;

		Out.LayerState[k] = FVector4f(
			Out.Stack.Depth[k],
			ImplicitStep * ImplicitStep * Out.Stack.ModeSpeedSq[k],
			Saturation,
			(k == 0) ? PressureScale : PressureScale / Stratification);
	}

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
			// Already normalised and soft-saturated.
		case EFlowDebugMode::Vertical:    Out.DebugScale = 1.0f; break;
			// Saturates at Froude 1; the jump threshold is half way up.
		case EFlowDebugMode::Froude:      Out.DebugScale = 1.0f; break;
		case EFlowDebugMode::Cloud:
		case EFlowDebugMode::CloudAscent:
		case EFlowDebugMode::Storm:       Out.DebugScale = 1.0f; break;
			// A quarter radian, about the displacement at mid-life.
		case EFlowDebugMode::NoiseDisplacement: Out.DebugScale = 0.25f; break;
			// From dry at zero onset to saturated at full red.
		case EFlowDebugMode::Humidity:    Out.DebugScale = FMath::Max(1.0f - Config->CondensationOnset, 0.05f); break;
		case EFlowDebugMode::LayerHeight: Out.DebugScale = (Out.DebugLayer == 0) ? PressureScale : PressureScale / Stratification; break;
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
	int32 Due = 0;
	const float RunStep = Config->GetStepSize();
	float Step = RunStep;
	float Blend = StateBlend;

	// THE OUTPUT SHOWS SimulatedTime - Step + PendingTime, a blend of the last
	// two states. Spin-up and manual steps show the state they reach, so they
	// leave exactly one step owed and running resumes from what is on screen.
	if (StepsCompleted < SpinUpTarget)
	{
		// Spread over frames: one graph of hundreds of substeps hitches, and a
		// watchable spin-up says more than the converged state.
		Substeps = FMath::Min(
			FMath::Max(Config->MaxSpinUpStepsPerFrame, 1),
			SpinUpTarget - StepsCompleted);

		Step = FlowSimStep::SpinUp;
		PendingTime = RunStep;
		Blend = 1.0f;
	}
	else if (PendingManualSteps > 0)
	{
		Substeps = FMath::Min(PendingManualSteps, FlowSimStep::WarnPerFrame);
		PendingManualSteps -= Substeps;
		PendingTime = RunStep;
		Blend = 1.0f;
	}
	else if (!bPaused && Config->SimSpeed > 0.0f)
	{
		// Speed times frame time, the frame time clamped so a slow frame asks
		// for no more than a tenth of a second's worth.
		PendingTime += Config->SimSpeed * FMath::Min(DeltaTime, 0.1f);

		// A frame at the hang guard drops the excess rather than owing it.
		Due = FMath::FloorToInt(PendingTime / Step);
		Substeps = FMath::Min(Due, FlowSimStep::MaxPerFrame);
		PendingTime -= Due * Step;
		Blend = FMath::Clamp(PendingTime / Step, 0.0f, 1.0f);
	}

	// Said once per change of state, not per frame; the warning clears at half
	// its threshold so a speed near it does not repeat it.
	const int32 WarnBelow = (StepLoadLevel > 0) ? FlowSimStep::WarnPerFrame / 2 : FlowSimStep::WarnPerFrame;
	const int32 Level = (Due > FlowSimStep::MaxPerFrame) ? 2
		: (Substeps > WarnBelow) ? 1 : 0;

	if (Level != StepLoadLevel && !bPaused && StepsCompleted >= SpinUpTarget)
	{
		StepLoadLevel = Level;

		if (Level == 2)
		{
			UE_LOG(LogFlowSim, Warning,
				TEXT("SimSpeed %.4f needs more than %d steps a frame; running slower than asked."),
				Config->SimSpeed, FlowSimStep::MaxPerFrame);
		}
		else if (Level == 1)
		{
			UE_LOG(LogFlowSim, Warning,
				TEXT("SimSpeed %.4f takes %d steps a frame; the sim now dominates frame time."),
				Config->SimSpeed, Substeps);
		}
		else
		{
			UE_LOG(LogFlowSim, Log, TEXT("SimSpeed %.4f takes %d steps a frame."),
				Config->SimSpeed, Substeps);
		}
	}

	FFlowSimParams Params;
	if (!BuildParams(Params, Step))
	{
		return;
	}

	Params.StateBlend = Blend;

	CurrentStep = Step;
	StateBlend = Blend;
	LastSubsteps = Substeps;
	SimulatedTime += Substeps * Step;
	StepsCompleted += Substeps;

	FFlowSimulation* Sim = Simulation;

	// Zero substeps still enqueues, so the output and debug views keep updating
	// on a paused sim.
	ENQUEUE_RENDER_COMMAND(FlowSimAdvance)(
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