#include "FlowSimSubsystem.h"

#include "TerrestrialShadowMap.h"
#include "FlowSimulation.h"
#include "FlowSimShaders.h"
#include "FlowSimSettings.h"
#include "FlowSnapshot.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "TextureResource.h"
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

static TAutoConsoleVariable<int32> CVarFlowSimDebugMode(
	TEXT("r.FlowSim.DebugMode"),
	-1,
	TEXT("Override the config's debug view. -1 uses the config.\n")
	TEXT("0 Vorticity, 1 Pressure, 2 Speed, 3 East, 4 North,\n")
	TEXT("5 Helmholtz residual, 6 Zonal profile error, 7 Vertical motion, 8 Froude, 9 Cloud, 10 Cloud formation ascent, 11 Noise displacement,\n")
	TEXT("12 Relative humidity, 13 Storm, 14 Layer top height, 15 Column cloud, 16 Storm eye, 17 Storm cell health, 18 Storm genesis, 19 Storm cell gains."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarFlowSimDebugLayer(
	TEXT("r.FlowSim.DebugLayer"),
	-1,
	TEXT("Override the debug layer. -1 uses the config."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarFlowSimDebugScale(
	TEXT("r.FlowSim.DebugScale"),
	-1.0f,
	TEXT("Override the debug value scale. Negative uses the config."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarFlowSimPaused(
	TEXT("r.FlowSim.Paused"),
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
				TEXT("Project Settings -> Plugins -> Flow Sim."));
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
	TEXT("Stop stepping. The state is kept for the debug view; FlowSim.Start reseeds or restores."),
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
	TEXT("Discard the field and reseed from the current config, or restore its InitialState."),
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
	TEXT("Capture the live state into a FlowSnapshot asset: the asset path given, ")
	TEXT("or the running config's InitialState. Blocks on the GPU; an authoring operation."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Args, UWorld* World)
		{
			UFlowSimSubsystem* Sub = FindSubsystem(World);

			if (!Sub)
			{
				return;
			}

			UFlowSnapshot* Target = nullptr;

			if (Args.Num() > 0)
			{
				Target = LoadObject<UFlowSnapshot>(nullptr, *Args[0]);

				if (!Target)
				{
					UE_LOG(LogFlowSim, Error,
						TEXT("No FlowSnapshot at '%s'. Create the asset first, then save into it."),
						*Args[0]);
					return;
				}
			}
			else
			{
				const UFlowSimConfig* Running = Sub->GetConfig();
				Target = Running ? Running->InitialState.Get() : nullptr;

				if (!Target)
				{
					UE_LOG(LogFlowSim, Error,
						TEXT("The running config has no InitialState. Create a FlowSnapshot asset, ")
						TEXT("bind it as the config's InitialState, then save into it."));
					return;
				}
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

	/** The grid a config runs at: the dimensions rounded to what the solver
	 *  supports. */
	FIntVector GridOf(const UFlowSimConfig& Config)
	{
		return FIntVector(
			FlowSimShader::GridLongitude(Config.GridResolution),
			FlowSimShader::GridLatitude(Config.GridResolution),
			LayerCountOf(Config));
	}

	/** Share of the thermal shear a layer carries: 1 on top, 0 at the bottom. */
	float ShearShare(int32 Layer, int32 Layers)
	{
		return (Layers > 1) ? (float)(Layers - 1 - Layer) / (float)(Layers - 1) : 0.0f;
	}

	/** Mirrors SimZonalRate: the layer's jets plus its share of the shear. */
	float LayerZonalRate(const UFlowSimConfig& Config, const FFlowSimScales& Speeds, float Mu, int32 Layer)
	{
		const FFlowLayerProfile P = LayerOf(Config, Layer);
		const float Jets = FlowSimProfile::JetRate(Config, Mu, Speeds.JetStrength * P.JetScale, Config.EquatorialBoost * P.BoostScale);

		return Jets + ShearShare(Layer, LayerCountOf(Config)) * Speeds.ThermalShear * FlowSimProfile::ThermalShape(Config, Mu);
	}

	/** The layers' mean zonal angular rate at mu. */
	float MeanZonalRate(const UFlowSimConfig& Config, const FFlowSimScales& Speeds, float Mu)
	{
		const int32 Layers = LayerCountOf(Config);
		float Sum = 0.0f;

		for (int32 k = 0; k < Layers; ++k)
		{
			Sum += LayerZonalRate(Config, Speeds, Mu, k);
		}

		return Sum / (float)Layers;
	}

	/** The latitude nearest Requested, radians, where the layers' mean zonal
	 *  flow changes direction: the middle of the shear zone between two
	 *  opposite jets. Searched out to 45 degrees either side; Requested where
	 *  none lies within it. */
	float NearestFlowReversal(const UFlowSimConfig& Config, const FFlowSimScales& Speeds, float Requested)
	{
		const float Step = FMath::DegreesToRadians(0.25f);
		const float Limit = FMath::DegreesToRadians(85.0f);

		const auto Speed = [&Config, &Speeds](float Lat)
			{
				return MeanZonalRate(Config, Speeds, FMath::Sin(Lat)) * FMath::Cos(Lat);
			};

		for (int32 i = 0; i < 180; ++i)
		{
			for (const float Side : { -1.0f, 1.0f })
			{
				const float A = Requested + Side * (float)i * Step;
				const float B = A + Side * Step;

				if (FMath::Abs(A) > Limit || FMath::Abs(B) > Limit)
				{
					continue;
				}

				const float SpeedA = Speed(A);
				const float SpeedB = Speed(B);

				if (SpeedA == 0.0f)
				{
					return A;
				}

				if (SpeedA * SpeedB < 0.0f)
				{
					return A + (B - A) * SpeedA / (SpeedA - SpeedB);
				}
			}
		}

		return Requested;
	}

	/** The zonal flow a perpetual storm sits in: the layers' mean, sampled
	 *  along its latitude axis and weighted by a wind profile peaking at its
	 *  eyewall, so the two sides of its eyewall count most. Speed is eastward,
	 *  at the storm's centre; vorticity is relative, positive counterclockwise
	 *  seen from outside. */
	struct FPerpetualBackground
	{
		float Speed = 0.0f;
		float Vorticity = 0.0f;
	};

	FPerpetualBackground PerpetualBackground(const UFlowSimConfig& Config, const FFlowSimScales& Speeds,
		float CentreLat, float HalfHeight, float Collar)
	{
		const auto MeanRate = [&Config, &Speeds](float M)
			{
				return MeanZonalRate(Config, Speeds, M);
			};

		constexpr int32 Taps = 8;
		constexpr float Step = 1e-3f;

		double Speed = 0.0;
		double Vorticity = 0.0;
		double Weight = 0.0;

		for (int32 t = -Taps; t <= Taps; ++t)
		{
			const float R = FMath::Abs((float)t / (float)Taps);
			const float Inside = R / Collar;
			const float Outside = 1.0f - FMath::Clamp((R - Collar) / (1.0f - Collar), 0.0f, 1.0f);
			const float W = (R < Collar) ? Inside * Inside : Outside * Outside;

			const float Lat = FMath::Clamp(CentreLat + (float)t / (float)Taps * HalfHeight, -UE_HALF_PI + 1e-3f, UE_HALF_PI - 1e-3f);
			const float M = FMath::Sin(Lat);
			const float Hi = FMath::Min(M + Step, 1.0f);
			const float Lo = FMath::Max(M - Step, -1.0f);
			const float Rate = MeanRate(M);
			const float Slope = (MeanRate(Hi) - MeanRate(Lo)) / (Hi - Lo);

			// zeta = 2 mu R - (1 - mu^2) dR/dmu for an angular rate R(mu).
			Speed += W * Rate * FMath::Cos(Lat);
			Vorticity += W * (2.0f * M * Rate - (1.0f - M * M) * Slope);
			Weight += W;
		}

		FPerpetualBackground Out;
		Out.Speed = (float)(Speed / FMath::Max(Weight, 1e-6));
		Out.Vorticity = (float)(Vorticity / FMath::Max(Weight, 1e-6));
		return Out;
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
static float PeakRate(const UFlowSimConfig& Config, const FFlowSimScales& Speeds)
{
	const int32 Layers = LayerCountOf(Config);
	const bool bBanded = (Config.ZonalProfile == EFlowZonalProfile::Banded);

	float Peak = 0.0f;

	for (int32 i = 0; i < Layers; ++i)
	{
		const FFlowLayerProfile P = LayerOf(Config, i);

		const float Boost = bBanded ? FMath::Max(Config.EquatorialBoost * P.BoostScale, 0.0f) : 0.0f;

		Peak = FMath::Max(Peak, FMath::Abs(Speeds.JetStrength * P.JetScale) * (1.0f + Boost)
			+ ShearShare(i, Layers) * FMath::Abs(Speeds.ThermalShear));
	}

	return FMath::Max(Peak, 1e-6f);
}


/** Output scales against the speed root's physical scales: the constants the
 *  terrestrial baseline (Design/Baseline.json) keeps its scales at. */
namespace FlowSimOutput
{
	constexpr float Vorticity = 1.276773f;
	constexpr float Pressure = 2.930717f;
	constexpr float Divergence = 0.664986f;
}

void UFlowSimSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Simulation = new FFlowSimulation();

	PreActorTickHandle = FWorldDelegates::OnWorldPreActorTick.AddUObject(this, &UFlowSimSubsystem::OnPreActorTick);
}

void UFlowSimSubsystem::Deinitialize()
{
	FWorldDelegates::OnWorldPreActorTick.Remove(PreActorTickHandle);

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

	const int32 W = FlowSimShader::GridLongitude(Config->GridResolution);
	return PeakRate(*Config, Config->ResolveScales()) * CurrentStep * W / (2.0f * UE_PI);
}

void UFlowSimSubsystem::ReportInertSettings() const
{
	if (!Config)
	{
		return;
	}

	// Forcing with nowhere to sample from: amplitude, scale and lifetime all
	// dormant, and all switching on together when a volume is assigned.
	if (Config->EddySpeed > 0.0f && !Config->ForcingVolume)
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("EddySpeed is %.3f but no ForcingVolume is bound, so the ")
			TEXT("stochastic forcing is inactive. Assigning a volume will switch ")
			TEXT("amplitude, scale and lifetime on all at once."),
			Config->EddySpeed);
	}

	if (LayerCountOf(*Config) < 2)
	{
		if (Config->LayerCoupling > 0.0f)
		{
			UE_LOG(LogFlowSim, Warning,
				TEXT("LayerCoupling is %.3f but LayerCount is 1, so it does nothing."),
				Config->LayerCoupling);
		}

		if (Config->ShearSpeed != 0.0f)
		{
			UE_LOG(LogFlowSim, Warning,
				TEXT("ShearSpeed is %.3f but LayerCount is 1: a single layer has no ")
				TEXT("vertical shear, so it does nothing."),
				Config->ShearSpeed);
		}

		UE_LOG(LogFlowSim, Log,
			TEXT("LayerCount is 1: Stratification, UpperSaturation, GenesisShearRatio, StormCellTopShare, ")
			TEXT("StormCellInflow and the layers' DepthScale do nothing."));
	}

	if (Config->PerpetualStorms.Num() > FlowSimShader::MaxPerpetualStorms)
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("PerpetualStorms holds %d storms; only the first %d run."),
			Config->PerpetualStorms.Num(), FlowSimShader::MaxPerpetualStorms);
	}

	const int32 Perpetual = FMath::Min(Config->PerpetualStorms.Num(), FlowSimShader::MaxPerpetualStorms);

	if (FMath::Max(Config->MaxStormCells, 0) + Perpetual > FlowSimShader::MaxStormCells)
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("MaxStormCells %d and %d perpetual storms need more than %d slots; the hurricanes get %d."),
			Config->MaxStormCells, Perpetual, FlowSimShader::MaxStormCells, FlowSimShader::MaxStormCells - Perpetual);
	}

	if (Config->MaxStormCells <= 0 && Perpetual == 0)
	{
		UE_LOG(LogFlowSim, Log, TEXT("MaxStormCells is 0 with no perpetual storms: the storm cell and hurricane look settings do nothing."));
	}
	else
	{
		if (Config->StormCellPersistence >= 1.0f)
		{
			UE_LOG(LogFlowSim, Warning,
				TEXT("StormCellPersistence is 1: a cell never decays once its conditions fail or its ")
				TEXT("lifetime passes, and once every slot holds one no cell can spawn."));
		}

		if (Config->StormCellSustainRatio < 1.0f)
		{
			UE_LOG(LogFlowSim, Log,
				TEXT("StormCellSustainRatio %.2f holds a cell's storm below GenesisStorm, so a cell ")
				TEXT("lives only as long as the storm under it does."),
				Config->StormCellSustainRatio);
		}
	}

	if (Config->InitialState && Config->SpinUpTurnovers > 0.0f)
	{
		UE_LOG(LogFlowSim, Log,
			TEXT("InitialState is bound, so SpinUpTurnovers (%.1f) is skipped. It still ")
			TEXT("matters when CREATING snapshots."),
			Config->SpinUpTurnovers);
	}

	if (Config->FilterLatitude <= 0.0f)
	{
		UE_LOG(LogFlowSim, Log,
			TEXT("FilterLatitude is 0, so the polar filter is disabled entirely."));
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
	const int32 W = FlowSimShader::GridLongitude(Config->GridResolution);
	const float Step = Config->GetStepSize();
	const float Steps = FMath::Max(Config->SimSpeed, 0.0f) / 60.0f / Step;
	const float C = FlowSimProfile::WaveSpeed(*Config);
	const FFlowSimScales Speeds = Config->ResolveScales();

	const float Advective = PeakRate(*Config, Speeds) * Step * W / (2.0f * UE_PI);
	const float Gravity = C * Step * W / (2.0f * UE_PI);
	const float Froude = PeakRate(*Config, Speeds) / C;
	const float RotationPerStep = Config->PlanetaryVorticity * Step;

	UE_LOG(LogFlowSim, Log,
		TEXT("Speed %.4f: %.1f steps of %.6f per frame at 60fps. ")
		TEXT("Advective Courant %.3f, gravity-wave Courant %.3f (implicit), ")
		TEXT("Froude %.2f, Coriolis %.3f rad/step, wave speed %.3f."),
		Config->SimSpeed, Steps, Step,
		Advective, Gravity, Froude, RotationPerStep, C);

	// GRID DAMPING, per turnover: the implicit scheme's share at this step, and
	// the divergence damping's per-step fraction that makes up the rest.
	const float Implicit = Config->GetImplicitDampingRate(Step) * Speeds.Turnover;
	const float Fraction = Config->GetDivergenceDamping(Step);

	UE_LOG(LogFlowSim, Log,
		TEXT("Grid damping %.2f per turnover: implicit scheme %.2f at this step, ")
		TEXT("divergence damping %.5f of grid-scale divergence per step."),
		Config->GridDamping, Implicit, Fraction);

	if (Implicit > Config->GridDamping * 1.05f)
	{
		UE_LOG(LogFlowSim, Log,
			TEXT("The implicit scheme alone damps %.2f per turnover at this step, past ")
			TEXT("GridDamping %.2f. Lower ImplicitWeight toward 0.5 or the step for the authored rate."),
			Implicit, Config->GridDamping);
	}

	if (Fraction >= 0.449f)
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("GridDamping %.2f needs more divergence damping per step than the explicit ")
			TEXT("scheme allows at step %g, so it is capped. Lower GridDamping or the step."),
			Config->GridDamping, Step);
	}

	// The cells' two targets share one ceiling, SIM_CELL_TARGET_CEILING of the
	// Froude ceiling: the inflow gets what the vortex leaves.
	if (Config->MaxStormCells > 0 && Config->SpeedRoot > 0.0f && Config->StormCellInflow > 0.0f)
	{
		const float Room = 0.9f * FMath::Max(Config->SpeedRoot, 0.1f) * C;
		const float Wind = Speeds.CellWind;
		const float Inflow = FMath::Clamp(Config->StormCellInflow, 0.0f, 1.0f) * Wind;
		const float Left = FMath::Sqrt(FMath::Max(Room * Room - Wind * Wind, 0.0f));

		if (Left < Inflow)
		{
			UE_LOG(LogFlowSim, Warning,
				TEXT("Storm cell inflow target %.3f is cut to %.3f in a full-strength cell: its vortex ")
				TEXT("target %.3f leaves that much under the ceiling %.3f. Lower StormCellSpeed or raise SpeedRoot."),
				Inflow, Left, Wind, Room);
		}
	}

	if (Froude > 0.5f)
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("Froude %.2f: the peak flow is too fast for the gravity-wave speed ")
			TEXT("and will form hydraulic jumps. Raise DeformationRadius or ")
			TEXT("PlanetaryVorticity, or lower JetSpeed or ShearSpeed."),
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

	// THE BUDGET, as fractions of the speed root. The top layer's jet and shear
	// are summed though they peak at different latitudes, an upper bound; eddies
	// are their nominal equilibrium speed against the drag, and the storm cells
	// their target eyewall wind. Past the ceiling's knee the clip acts as a drag
	// on the zonal mean and clips eddy peaks.
	const FFlowLayerProfile Top = LayerOf(*Config, 0);

	const float TopWind = FMath::Abs(Config->JetSpeed * Top.JetScale) + (LayerCountOf(*Config) > 1 ? FMath::Abs(Config->ShearSpeed) : 0.0f);
	const float TopEddies = Config->EddySpeed * Top.EddyScale;
	const float Cells = Config->StormCellSpeed;

	UE_LOG(LogFlowSim, Log,
		TEXT("Speed root %.3f, turnover %.4f. Of the root: top layer's jets and shear %.2f, ")
		TEXT("its eddies %.2f, storm cells %.2f; the ceiling eases in from 0.7."),
		Speeds.Root, Speeds.Turnover, TopWind, TopEddies, Config->MaxStormCells > 0 ? Cells : 0.0f);

	// The turnover-authored lifetimes in days, 2 pi / PlanetaryVorticity, to
	// check against real weather.
	const float Day = 2.0f * UE_PI / FMath::Max(Config->PlanetaryVorticity, 0.1f);

	UE_LOG(LogFlowSim, Log,
		TEXT("A turnover is %.3f of a day; %.2f turnovers a second at SimSpeed %.4f. In days: cloud %.2f, ")
		TEXT("storm %.2f, storm cell %.2f, forcing pattern %.2f. Storm cell radius %.1f degrees."),
		Speeds.Turnover / Day, FMath::Max(Config->SimSpeed, 0.0f) / Speeds.Turnover, Config->SimSpeed,
		Speeds.CloudLifetime / Day, Speeds.StormLifetime / Day, Speeds.CellLifetime / Day,
		Speeds.ForcingLifetime / Day, FMath::RadiansToDegrees(Speeds.CellRadius));

	float PerpetualWind = 0.0f;

	for (int32 i = 0; i < FMath::Min(Config->PerpetualStorms.Num(), FlowSimShader::MaxPerpetualStorms); ++i)
	{
		PerpetualWind = FMath::Max(PerpetualWind, Config->PerpetualStorms[i].Wind);
	}

	if (TopWind + TopEddies > 0.7f || (Config->MaxStormCells > 0 && Cells > 0.7f) || PerpetualWind > 0.7f)
	{
		UE_LOG(LogFlowSim, Warning,
			TEXT("Winds past 0.7 of the speed root reach the ceiling, which then drags the ")
			TEXT("zonal mean and clips eddies and vortices instead of the flow settling. ")
			TEXT("Lower JetSpeed, ShearSpeed, EddySpeed, StormCellSpeed or a perpetual storm's Wind."));
	}
}

void UFlowSimSubsystem::ReportStack() const
{
	if (!Config)
	{
		return;
	}

	const int32 N = LayerCountOf(*Config);
	const FFlowSimStack Stack = BuildStack(*Config, FlowSimProfile::WaveSpeed(*Config));
	const FFlowSimScales Resolved = Config->ResolveScales();

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
	// Judged where the shear wind peaks: the zone's centre, or under FollowJets
	// the strongest jet between 10 and 80 degrees.
	float Lat = FMath::DegreesToRadians(FMath::Clamp(Config->BaroclinicLatitude, 10.0f, 80.0f));

	if (Config->ThermalShape == EFlowThermalShape::FollowJets)
	{
		float PeakWind = -1.0f;

		for (int32 Degree = 10; Degree <= 80; ++Degree)
		{
			const float Candidate = FMath::DegreesToRadians((float)Degree);
			const float Wind = FMath::Abs(FlowSimProfile::ThermalShape(*Config, FMath::Sin(Candidate))) * FMath::Cos(Candidate);

			if (Wind > PeakWind)
			{
				PeakWind = Wind;
				Lat = Candidate;
			}
		}
	}

	const float Shape = FMath::Abs(FlowSimProfile::ThermalShape(*Config, FMath::Sin(Lat)));
	const float ShearWind = FMath::Abs(Resolved.ThermalShear) * Shape * FMath::Cos(Lat);
	const float F = Config->PlanetaryVorticity * FMath::Sin(Lat);
	const float Beta = Config->PlanetaryVorticity * FMath::Cos(Lat);
	const float LocalRadius = FlowSimProfile::WaveSpeed(*Config) / FMath::Max(F, 1e-3f);
	const float Critical = 2.0f * Beta * LocalRadius * LocalRadius;
	const float Drive = ShearWind / FMath::Max(Critical, 1e-6f);

	UE_LOG(LogFlowSim, Log,
		TEXT("Thermal shear %.2f against a critical %.2f at %.0f degrees: %.2fx. ")
		TEXT("Storms grow from the shear above about 1."),
		ShearWind, Critical, FMath::RadiansToDegrees(Lat), Drive);

	// The balanced interfaces, from the same profile the balance pass
	// integrates: where a layer thins toward nothing, it has run into the top
	// or bottom of the stack.
	const int32 Rows = FlowSimShader::GridLatitude(Config->GridResolution);
	const float DMu = 2.0f / Rows;

	TArray<float> M;
	M.SetNumZeroed(N * Rows);

	for (int32 k = 0; k < N; ++k)
	{
		float Sum = 0.0f;

		for (int32 j = 1; j < Rows; ++j)
		{
			const float MuF = -1.0f + j * DMu;
			const float R = LayerZonalRate(*Config, Resolved, MuF, k);

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
			TEXT("clips. Lower ShearSpeed, widen DeformationRadius, or deepen that ")
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

	RunningGrid = Config ? GridOf(*Config) : FIntVector::ZeroValue;

	// Then hand back the start state, if there is one. Render commands run in
	// order, so the payload arrives after the reset flag and survives it.
	const bool bRestored = QueueInitialState();

	// An empty payload cancels one still pending from an earlier reset, which
	// the next initialisation would otherwise restore.
	if (!bRestored)
	{
		ENQUEUE_RENDER_COMMAND(FlowSimClearRestore)(
			[Sim](FRHICommandListImmediate&)
			{
				Sim->QueueRestore_RenderThread(TArray<float>());
			});
	}

	SpinUpTarget = (bRestored || !Config) ? 0 : Config->GetSpinUpSteps();

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

	const FIntVector Grid = GridOf(*Config);

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
	const FFlowSimScales Speeds = Config->ResolveScales();

	FFlowSnapshotProvenance Now;
	Now.BandCount = Config->BandCount;
	Now.JetStrength = Speeds.JetStrength;
	Now.EquatorialBoost = Config->EquatorialBoost;
	Now.Asymmetry = Config->Asymmetry;
	Now.WidthBias = Config->WidthBias;
	Now.PlanetaryVorticity = Config->PlanetaryVorticity;
	Now.ThermalShear = Speeds.ThermalShear;

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

	FFlowSimulation* Sim = Simulation;

	// THE READBACK MUST BE LOCKED ON THE RENDER THREAD. FRHIGPUBufferReadback::Lock
	// is render-thread-only on every RHI, flush or no flush. So the capture, wait
	// and copy happen inside one render command, and only the finished array
	// crosses back; the captures by reference are safe because the flush below
	// blocks until the command has run.
	TArray<float> Result;
	FIntVector Grid = FIntVector::ZeroValue;
	bool bSucceeded = false;

	ENQUEUE_RENDER_COMMAND(FlowSimCapture)(
		[Sim, Params, &Result, &Grid, &bSucceeded](FRHICommandListImmediate& RHICmdList) mutable
		{
			Params.ResolveTextures_RenderThread();

			FRHIGPUBufferReadback Readback(TEXT("FlowSim.SnapshotReadback"));
			bool bCaptured = false;

			{
				FRDGBuilder GraphBuilder(RHICmdList);

				bCaptured = Sim->AddCapturePass_RenderThread(GraphBuilder, Params, &Readback, Grid);

				GraphBuilder.Execute();
			}

			if (!bCaptured)
			{
				return;
			}

			RHICmdList.BlockUntilGPUIdle();

			if (!Readback.IsReady())
			{
				return;
			}

			// Sized by the grid the state is allocated at, which the capture
			// followed.
			const int32 Count = FFlowSimulation::StateFloats(Grid);
			const uint32 Bytes = (uint32)Count * sizeof(float);

			if (const void* Data = Readback.Lock(Bytes))
			{
				Result.SetNumUninitialized(Count);
				FMemory::Memcpy(Result.GetData(), Data, Bytes);
				bSucceeded = true;

				Readback.Unlock();
			}
		});

	FlushRenderingCommands();

	if (!bSucceeded)
	{
		UE_LOG(LogFlowSim, Error,
			TEXT("Snapshot readback failed. Is the sim initialised and running?"));
		return false;
	}

	Target->Grid = Grid;
	Target->State = MoveTemp(Result);

	Target->Provenance.BandCount = Config->BandCount;
	const FFlowSimScales Speeds = Config->ResolveScales();

	Target->Provenance.JetStrength = Speeds.JetStrength;
	Target->Provenance.EquatorialBoost = Config->EquatorialBoost;
	Target->Provenance.Asymmetry = Config->Asymmetry;
	Target->Provenance.WidthBias = Config->WidthBias;
	Target->Provenance.PlanetaryVorticity = Config->PlanetaryVorticity;
	Target->Provenance.ThermalShear = Speeds.ThermalShear;
	Target->SimulatedTime = (float)SimulatedTime;
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

bool UFlowSimSubsystem::PrepareTargets()
{
	if (!Config)
	{
		return false;
	}

	const int32 GridW = FlowSimShader::GridLongitude(Config->GridResolution);
	const int32 GridH = FlowSimShader::GridLatitude(Config->GridResolution);

	// The cube atlas the sim resamples its output onto; see FlowField.ush.
	const FIntPoint Atlas = FlowSimShader::AtlasSize(FlowSimShader::GridResolution(Config->GridResolution));

	// Flow, weather, noise phase A and noise phase B slices, one of each per
	// layer.
	const int32 Slices = 4 * LayerCountOf(*Config);

	if (!FlowTarget)
	{
		FlowTarget = NewObject<UTextureRenderTarget2DArray>(this, TEXT("FlowTarget"), RF_Transient);
	}

	UTextureRenderTarget2DArray* Flow = FlowTarget;

	if (Flow->SizeX != Atlas.X || Flow->SizeY != Atlas.Y || Flow->Slices != Slices
		|| Flow->OverrideFormat != PF_FloatRGBA || !Flow->bCanCreateUAV)
	{
		// bCanCreateUAV must be set BEFORE the resource is created, or the
		// texture comes back without UAV support and every dispatch that writes
		// it silently does nothing.
		Flow->bCanCreateUAV = true;
		Flow->OverrideFormat = PF_FloatRGBA;
		Flow->ClearColor = FLinearColor::Black;
		Flow->Init(Atlas.X, Atlas.Y, Slices, PF_FloatRGBA);
		Flow->UpdateResourceImmediate(true);

		UE_LOG(LogFlowSim, Log, TEXT("Flow target set to %dx%d x %d slices."), Atlas.X, Atlas.Y, Slices);
	}

	if (!Config->bDebugView)
	{
		return true;
	}

	if (!DebugTarget)
	{
		DebugTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("DebugTarget"), RF_Transient);
	}

	UTextureRenderTarget2D* Debug = DebugTarget;

	if (Debug->SizeX != GridW || Debug->SizeY != GridH || !Debug->bCanCreateUAV)
	{
		Debug->bCanCreateUAV = true;
		Debug->ClearColor = FLinearColor::Black;
		Debug->InitCustomFormat(GridW, GridH, PF_FloatRGBA, /*bForceLinearGamma*/ true);
		Debug->UpdateResourceImmediate(true);
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
	Out.GridSize = GridOf(*Config);

	const int32 Layers = Out.GridSize.Z;

	// Every wind from the speed root, every rate and lifetime from the turnover.
	const FFlowSimScales Scales = Config->ResolveScales();

	Out.JetParams = FVector4f(
		Config->BandCount,
		Scales.JetStrength,
		Config->EquatorialBoost,
		Config->Asymmetry);

	Out.WidthBias = Config->WidthBias;
	Out.ZonalProfile = (int32)Config->ZonalProfile;
	Out.JetLatitudeScale = FlowSimProfile::JetLatitudeScale(*Config);

	for (int32 i = 0; i < 8; ++i)
	{
		const FFlowLayerProfile P = LayerOf(*Config, i);

		// z scales the forcing rate, which the shader also multiplies by the
		// drag: EddyScale times DragScale leaves the layer's equilibrium eddy
		// speed at EddyScale times EddySpeed whatever its drag.
		Out.LayerProfile[i] = FVector4f(P.JetScale, P.BoostScale, P.EddyScale * P.DragScale, P.DragScale);
	}

	Out.DeltaTime = FMath::Clamp(Step, 0.0f, Config->GetSpinUpStep());
	Out.Turnover = Scales.Turnover;
	Out.Time = SimulatedTime;
	Out.PlanetaryVorticity = FMath::Max(Config->PlanetaryVorticity, 0.1f);
	Out.ImplicitWeight = Config->GetImplicitWeight(Out.DeltaTime);

	// -- Forcing ------------------------------------------------------------

	Out.ForcingChannel = FMath::Clamp(Config->ForcingChannel, 0, 3);

	Out.NudgeRate = Scales.NudgeRate;
	Out.ForcingAmplitude = Scales.EddySpeed;
	Out.ForcingFrequency = Scales.ForcingFrequency;
	Out.ForcingLifetime = Scales.ForcingLifetime;
	Out.DragRate = Scales.DragRate;
	Out.LayerCoupling = Scales.LayerCoupling;
	Out.DivergenceDamping = Config->GetDivergenceDamping(Out.DeltaTime);
	Out.FroudeCeiling = FMath::Max(Config->SpeedRoot, 0.1f);
	Out.ShockDamping = FMath::Max(Config->ShockDamping, 0.0f);
	Out.bSharpCentreVelocity = Config->bSharpCentreVelocity;

	Out.ThermalRelaxation = Scales.ThermalRelaxation;
	Out.ThermalParams = FVector4f(
		Scales.ThermalShear,
		Config->ThermalShape == EFlowThermalShape::FollowJets ? 1.0f : 0.0f,
		FMath::DegreesToRadians(FMath::Clamp(Config->BaroclinicLatitude, 0.0f, 90.0f)),
		FMath::DegreesToRadians(FMath::Max(Config->BaroclinicWidth, 1.0f)));

	// -- Moisture, cloud and storms -------------------------------------------

	Out.CondensationRate = Scales.CondensationRate;
	Out.EvaporationRate = Scales.EvaporationRate;
	Out.CloudLifetime = Scales.CloudLifetime;

	Out.MoistureParams = FVector4f(
		0.0f,
		FMath::Max(Config->SaturationPoleRatio, 0.0f),
		FMath::Clamp(Config->CondensationOnset, 0.0f, 0.99f),
		Scales.SurfaceEvaporation);

	Out.LatentHeating = FMath::Max(Config->LatentHeating, 0.0f);
	Out.AscentSmoothing = Scales.AscentSmoothing;
	Out.WindEvaporation = Scales.WindEvaporation;

	const float StormDecay = 1.0f / Scales.StormLifetime;

	Out.StormParams = FVector4f(
		FMath::Max(Config->StormAmount, 0.0f) * StormDecay,
		FMath::Clamp(Config->StormThreshold, 0.0f, 0.99f),
		FMath::Max(Config->StormSpin, 0.0f),
		StormDecay);

	// -- Storm cells ------------------------------------------------------------

	const float GenesisMin = FMath::DegreesToRadians(FMath::Clamp(Config->GenesisLatitudeMin, 0.0f, 90.0f));
	const float GenesisMax = FMath::DegreesToRadians(FMath::Clamp(Config->GenesisLatitudeMax, 0.0f, 90.0f));

	const float Wall = FMath::Clamp(Config->StormCellEyewall, 0.01f, 0.95f);

	Out.CellShape = FVector4f(
		Scales.CellRadius,
		FMath::Clamp(Config->StormCellEyeRatio, 0.0f, 0.9f) * Wall,
		Wall,
		FMath::Clamp(Config->StormCellEyeStrength, 0.0f, 1.0f));

	Out.CellVortex = FVector4f(
		FMath::Max(Config->StormCellFalloff, 0.1f),
		Scales.CellForcing,
		FMath::Clamp(Config->StormCellTopShare, -1.0f, 1.0f),
		Scales.CellWind);

	Out.CellDraft = FVector4f(
		FMath::Clamp(Config->StormCellDraft, -1.0f, 1.0f),
		FMath::Clamp(Config->StormCellEyeDraft, -1.0f, 1.0f),
		FMath::Clamp(Config->StormCellStorm + FMath::Max(Config->StormCellBandExcess, 0.0f), 0.0f, 1.0f),
		FMath::Clamp(Config->StormCellPressure, 0.0f, 2.0f));

	Out.CellLife = FVector4f(
		Scales.CellSpawnRate,
		Scales.CellGrowth,
		Scales.CellGrowth * (1.0f - FMath::Clamp(Config->StormCellPersistence, 0.0f, 1.0f)),
		Scales.CellLifetime);

	Out.CellMotion = FVector4f(
		Scales.CellDrift,
		Scales.CellFollow,
		FMath::Min(GenesisMin, GenesisMax),
		FMath::Max(GenesisMin, GenesisMax));

	Out.CellGenesis = FVector4f(
		Scales.GenesisShear,
		FMath::Clamp(Config->CondensationOnset + Config->GenesisHumidityMargin, 0.0f, 1.0f),
		FMath::Clamp(Config->GenesisStorm, 0.01f, 1.0f),
		FMath::Max(Config->GenesisSpin, 0.0f));

	// The lift's rate r that settles a full-intensity eyewall at the cover
	// against the cloud's decay alone: Cover = r / (r + 1 / CloudLifetime).
	const float CellCover = FMath::Clamp(Config->StormCellCloudCover, 0.0f, 0.99f);

	Out.CellCloud = FVector4f(
		CellCover / (1.0f - CellCover) / Out.CloudLifetime,
		0.0f,
		FMath::Clamp(Config->StormCellInflow, 0.0f, 1.0f),
		FMath::Clamp(Config->StormCellStorm, 0.0f, 1.0f));

	Out.CellWindBreadth = FMath::Clamp(Config->StormCellWindBreadth, 0.0f, 0.95f);
	Out.CellSustain = FMath::Clamp(FMath::Max(Config->StormCellSustainRatio, 0.0f) * Out.CellGenesis.Z, 0.0f, 1.0f);
	Out.CellEyeDepth = FMath::Clamp(Config->StormCellEyeDepth, 0.0f, 1.0f);
	Out.CellCoreFollow = Scales.CellCoreFollow;
	Out.CellEyeSoftness = FMath::Clamp(Config->StormCellEyeSoftness, 0.05f, 1.0f);


	// -- Perpetual storms -------------------------------------------------------
	//
	// The first cell slots. Each settles at the flow reversal nearest its
	// latitude, turns with the shear there and moves at the flow across it,
	// scaled by its steering, plus its drift, in closed form, so
	// FillCommonParameters places it at any time.
	const float Deformation = FMath::Max(Config->DeformationRadius, 0.01f);

	// The eyewall spans two grid columns at least, or the ring its wind is
	// measured on reads inside a cell or two and the gain pins.
	const float LeastRadius = FMath::Max(2.0f * UE_TWO_PI / (Wall * (float)FMath::Max(Out.GridSize.X, 1)), FMath::DegreesToRadians(0.5f));

	Out.PerpetualCount = FMath::Min(Config->PerpetualStorms.Num(), FlowSimShader::MaxPerpetualStorms);
	Out.PerpetualForcing = FMath::Max(Config->PerpetualStormForcing, 0.0f) / Scales.Turnover;

	for (int32 i = 0; i < Out.PerpetualCount; ++i)
	{
		const FFlowPerpetualStorm& Entry = Config->PerpetualStorms[i];
		const float Lat = NearestFlowReversal(*Config, Scales, FMath::DegreesToRadians(FMath::Clamp(Entry.Latitude, -85.0f, 85.0f)));
		const float HalfHeight = FMath::Clamp(Entry.Radius * Deformation,
			FMath::Min(LeastRadius, FMath::DegreesToRadians(45.0f)), FMath::DegreesToRadians(45.0f));

		// THE SPIN FOLLOWS THE SHEAR: positive turns counterclockwise seen from
		// outside, where the flow across the storm has positive vorticity.
		// PITFALL: a spin against the shear is torn apart by the jets.
		const FPerpetualBackground Background = PerpetualBackground(*Config, Scales, Lat, HalfHeight, Wall);
		const float Sense = (Background.Vorticity >= 0.0f) ? 1.0f : -1.0f;
		const float Cover = FMath::Clamp(Entry.Cover, 0.0f, 0.99f);

		Out.PerpetualShape[i] = FVector4f(
			Lat,
			FMath::DegreesToRadians(Entry.Longitude),
			HalfHeight,
			FMath::Clamp(Entry.Aspect, 1.0f, 4.0f));

		// Cover as a lift rate against the cloud's decay, as CellCloud.x; Lift
		// as the pressure drop, as CellDraft.w.
		Out.PerpetualLook[i] = FVector4f(
			Sense * FMath::Clamp(Entry.Wind, 0.0f, 0.9f) * Scales.Root,
			FMath::Clamp(Entry.Storm, 0.0f, 1.0f),
			FMath::Clamp(Entry.Lift, 0.0f, 2.0f),
			Cover / (1.0f - Cover) / Out.CloudLifetime);

		Out.PerpetualForm[i] = FVector4f(
			FMath::Clamp(Entry.Spiral, -1.0f, 1.0f),
			FMath::Clamp(Entry.Eye, 0.0f, 1.0f),
			0.0f,
			0.0f);

		// Between balanced jets of opposite direction the flow across it nets
		// to about zero and it holds its place; a stronger jet on one side
		// carries it that way.
		Out.PerpetualRate[i] = (double)(FMath::Clamp(Entry.Steering, 0.0f, 1.0f) * Background.Speed
			+ FMath::Clamp(Entry.Drift, -1.0f, 1.0f) * Scales.Root) / (double)FMath::Cos(Lat);
	}

	// The perpetual storms first, the hurricanes after them.
	Out.CellCount = FMath::Clamp(FMath::Max(Config->MaxStormCells, 0) + Out.PerpetualCount, 0, FlowSimShader::MaxStormCells);

	Out.StepIndex = StepsCompleted;

	Out.NoiseDriftRate = Config->GetNoiseDriftRate();
	Out.NoiseResetTime = Config->GetNoiseResetTime();

	Out.FilterLatitude = FMath::Clamp(Config->FilterLatitude, 0.0f, 1.0f);

	// -- Output normalisation -------------------------------------------------
	//
	// FROM THE SPEED ROOT, NOT A RUNNING MAXIMUM: a scale that chases the field
	// hides a drifting magnitude. Each is the physical scale of the fastest flow
	// the root allows, times a constant from FlowSimOutput, so "1" is the same
	// share of it in every regime. Each channel is soft-saturated or near unit
	// range at these scales.
	//
	//   Vorticity  the root over the deformation radius.
	//   Pressure   geostrophic: f times the root times the deformation radius,
	//              which is the root times the wave speed.
	//   Divergence vorticity times the Rossby number of the fastest flow, the
	//              root over the wave speed.
	//   Height     pressure over the density step an interface carries it by.
	const float Peak = PeakRate(*Config, Scales);
	const float Radius = FMath::Max(Config->DeformationRadius, 0.01f);
	const float ZetaScale = FMath::Max(FlowSimOutput::Vorticity * Scales.Root / Radius, 1e-4f);
	const float PressureScale = FMath::Max(FlowSimOutput::Pressure * Scales.Root * Scales.WaveSpeed, 1e-5f);
	const float DivScale = FMath::Max(FlowSimOutput::Divergence * (Scales.Root / Scales.WaveSpeed) * Scales.Root / Radius, 1e-4f);

	Out.OutputScales = FVector3f(PressureScale, ZetaScale, DivScale);
	Out.AtlasFaceSize = FlowSimShader::GridResolution(Config->GridResolution);

	// -- The stack ------------------------------------------------------------

	Out.Stack = BuildStack(*Config, FlowSimProfile::WaveSpeed(*Config));

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
			0.0f);
	}

	// -- Debug --------------------------------------------------------------

	const int32 ModeOverride = CVarFlowSimDebugMode.GetValueOnGameThread();
	const int32 LayerOverride = CVarFlowSimDebugLayer.GetValueOnGameThread();
	const float ScaleOverride = CVarFlowSimDebugScale.GetValueOnGameThread();

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
		case EFlowDebugMode::ColumnCloud:
		case EFlowDebugMode::CloudAscent:
		case EFlowDebugMode::Storm:
		case EFlowDebugMode::Eye:
		case EFlowDebugMode::CellHealth:
		case EFlowDebugMode::Genesis:     Out.DebugScale = 1.0f; break;
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

	if (Config->ForcingVolume)
	{
		Out.ForcingResource = Config->ForcingVolume->GetResource();
	}

	if (FlowTarget)
	{
		Out.FlowResource = FlowTarget->GameThread_GetRenderTargetResource();
	}

	if (Config->bDebugView && DebugTarget)
	{
		if (FTextureRenderTargetResource* Res = DebugTarget->GameThread_GetRenderTargetResource())
		{
			Out.DebugResource = Res;
			Out.DebugSize = FIntPoint(DebugTarget->SizeX, DebugTarget->SizeY);
		}
	}

	return Out.FlowResource != nullptr;
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

	// A frame without actor ticks steps here instead.
	if (!bSteppedThisFrame)
	{
		Advance(DeltaTime);
	}

	bSteppedThisFrame = false;

	// AFTER, AND NOT INSIDE. The bake reads the flow texture the step writes, and
	// render commands run in enqueue order. Outside StepSimulation because a
	// stopped or paused sim still has a deck to shadow.
	BakeShadowMap();
}

void UFlowSimSubsystem::OnPreActorTick(UWorld* InWorld, ELevelTick TickType, float DeltaTime)
{
	if (InWorld == GetWorld())
	{
		Advance(DeltaTime);
		bSteppedThisFrame = true;
	}
}

void UFlowSimSubsystem::Advance(float DeltaTime)
{
	ResolveClaims();

	// On the second frame rather than in Initialize: resolving a soft reference
	// during subsystem construction can run before the asset registry is
	// usable, and the first frame's atmospheres have bid by then.
	if (!bTriedAutoStart && ++FramesAdvanced > 1)
	{
		bTriedAutoStart = true;

		if (!bEverClaimed)
		{
			TryAutoStart();
		}
	}

	StepSimulation(DeltaTime);
}

void UFlowSimSubsystem::ClaimSimulation(const UObject* Claimant, UFlowSimConfig* InConfig, double Distance,
	UTextureRenderTarget2DArray* Keep)
{
	if (!Claimant || !InConfig)
	{
		return;
	}

	bEverClaimed = true;

	FSimClaim& Bid = Claims.AddDefaulted_GetRef();
	Bid.Claimant = Claimant;
	Bid.Config = InConfig;
	Bid.Distance = Distance;
	Bid.Keep = Keep;
}

void UFlowSimSubsystem::ReleaseClaim(const UObject* Claimant, bool bKeepField)
{
	Claims.RemoveAll([Claimant](const FSimClaim& Bid) { return Bid.Claimant.Get() == Claimant; });

	if (!IsOwner(Claimant))
	{
		return;
	}

	if (bKeepField)
	{
		KeepFlow(OwnerKeep.Get(), Claimant);
	}

	Owner.Reset();
	OwnerKeep.Reset();
	StopSimulation();
}

/** A challenger must be this much nearer than the owner to take the sim, so two
 *  planets at about the same distance do not trade it every frame. */
static constexpr double OwnerHandoverRatio = 0.8;

void UFlowSimSubsystem::ResolveClaims()
{
	TArray<FSimClaim> Bids = MoveTemp(Claims);
	Claims.Reset();

	Bids.RemoveAll([](const FSimClaim& Bid) { return !Bid.Claimant.IsValid() || !Bid.Config.IsValid(); });

	// NOBODY BID: the sim runs on as it is. An owner that went away is
	// forgotten, and the next bid takes over.
	if (Bids.Num() == 0)
	{
		if (!Owner.IsValid())
		{
			Owner.Reset();
		}

		return;
	}

	const FSimClaim* Current = Bids.FindByPredicate(
		[this](const FSimClaim& Bid) { return Owner.IsValid() && Bid.Claimant == Owner; });

	const FSimClaim* Nearest = &Bids[0];

	for (const FSimClaim& Bid : Bids)
	{
		if (Bid.Distance < Nearest->Distance)
		{
			Nearest = &Bid;
		}
	}

	const FSimClaim* Winner = (Current && Nearest->Distance >= Current->Distance * OwnerHandoverRatio)
		? Current : Nearest;

	const bool bNewOwner = !(Owner.IsValid() && Winner->Claimant == Owner);

	// THE OUTGOING OWNER KEEPS ITS FIELD, copied before the restart below
	// writes the next one. Render commands run in order, so the copy reads the
	// frame it last drew.
	if (bNewOwner && Owner.IsValid())
	{
		KeepFlow(OwnerKeep.Get(), Owner.Get());
	}

	Owner = Winner->Claimant;
	OwnerKeep = Winner->Keep;

	// A new owner starts from its config's snapshot, as does an owner whose
	// config changed: a planet type swap.
	if (bNewOwner || Config != Winner->Config.Get())
	{
		StartSimulation(Winner->Config.Get());
	}
}

UTextureRenderTarget2D* UFlowSimSubsystem::GetDebugTarget() const
{
	return (Config && Config->bDebugView) ? DebugTarget.Get() : nullptr;
}

bool UFlowSimSubsystem::TakeKeptClock(const UObject* Claimant, double& OutTime, UFlowSimConfig*& OutConfig)
{
	FKeptClock Kept;

	if (!KeptClocks.RemoveAndCopyValue(Claimant, Kept))
	{
		return false;
	}

	OutTime = Kept.Time;
	OutConfig = Kept.Config.Get();
	return true;
}

void UFlowSimSubsystem::KeepFlow(UTextureRenderTarget2DArray* Keep, const UObject* KeptFor)
{
	if (!Keep || !FlowTarget || !KeptFor)
	{
		return;
	}

	// THE CLOCK OF THE STATE COPIED, recorded here rather than by the planet:
	// an owner that stopped bidding without releasing kept its own clock at
	// its last tick, while the sim ran on.
	for (auto It = KeptClocks.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid())
		{
			It.RemoveCurrent();
		}
	}

	FKeptClock& Kept = KeptClocks.FindOrAdd(TWeakObjectPtr<const UObject>(KeptFor));
	Kept.Time = GetDisplayTime();
	Kept.Config = Config;

	const UTextureRenderTarget2DArray* Flow = FlowTarget;

	if (Keep->SizeX != Flow->SizeX || Keep->SizeY != Flow->SizeY || Keep->Slices != Flow->Slices
		|| Keep->OverrideFormat != PF_FloatRGBA)
	{
		Keep->OverrideFormat = PF_FloatRGBA;
		Keep->ClearColor = FLinearColor::Black;
		Keep->Init(Flow->SizeX, Flow->SizeY, Flow->Slices, PF_FloatRGBA);
		Keep->UpdateResourceImmediate(true);
	}

	FTextureRenderTargetResource* Source = FlowTarget->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* Dest = Keep->GameThread_GetRenderTargetResource();
	const int32 Slices = Flow->Slices;

	if (!Source || !Dest)
	{
		return;
	}

	ENQUEUE_RENDER_COMMAND(FlowSimKeep)(
		[Source, Dest, Slices](FRHICommandListImmediate& RHICmdList)
		{
			FTextureRHIRef SourceTexture = Source->GetRenderTargetTexture();
			FTextureRHIRef DestTexture = Dest->GetRenderTargetTexture();

			if (!SourceTexture.IsValid() || !DestTexture.IsValid())
			{
				return;
			}

			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGTextureRef From = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(SourceTexture, TEXT("FlowSim.Flow")));
			FRDGTextureRef To = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(DestTexture, TEXT("FlowSim.Kept")));

			FRHICopyTextureInfo CopyInfo;
			CopyInfo.NumSlices = Slices;

			AddCopyTexturePass(GraphBuilder, From, To, CopyInfo);

			GraphBuilder.Execute();
		});
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

	// A grid edit reallocates the state, so it goes through a reset and time,
	// steps, spin-up and the start state follow the new field.
	const FIntVector Grid = GridOf(*Config);

	if (Grid != RunningGrid)
	{
		UE_LOG(LogFlowSim, Display, TEXT("Grid changed to %dx%dx%d; resetting."), Grid.X, Grid.Y, Grid.Z);

		// The step's Courant numbers and the balance checks follow the grid.
		ReportCourant();
		ReportStack();
		ResetSimulation();
	}

	const int32 PauseOverride = CVarFlowSimPaused.GetValueOnGameThread();
	const bool bPaused = (PauseOverride >= 0) ? (PauseOverride != 0) : Config->bPaused;

	int32 Substeps = 0;
	int32 Due = 0;
	const float RunStep = Config->GetStepSize();
	float Step = RunStep;
	float Blend = StateBlend;

	// THE OUTPUT SHOWS SimulatedTime - Step + PendingTime, a blend of the last
	// two states. Spin-up and manual steps show the state they reach, so they
	// leave exactly one step owed and running resumes from what is on screen.
	// A pause holds spin-up too.
	if (!bPaused && StepsCompleted < SpinUpTarget)
	{
		// Spread over frames: one graph of hundreds of substeps hitches, and a
		// watchable spin-up says more than the converged state.
		Substeps = FMath::Min(
			FMath::Max(Config->MaxSpinUpStepsPerFrame, 1),
			SpinUpTarget - StepsCompleted);

		Step = Config->GetSpinUpStep();
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
		[Sim, Params, Substeps](FRHICommandListImmediate& RHICmdList) mutable
		{
			Params.ResolveTextures_RenderThread();

			FRDGBuilder GraphBuilder(RHICmdList);

			Sim->Enqueue_RenderThread(GraphBuilder, Params, Substeps);

			GraphBuilder.Execute();
		});
}

bool UFlowSimSubsystem::RequestShadowBake(const FTerrestrialShadowParams& InParams)
{
	if (!InParams.IsUsable())
	{
		return false;
	}

	ShadowRequests.Add(InParams);
	return true;
}

void UFlowSimSubsystem::BakeShadowMap()
{
	// CONSUMED, NOT HELD. A planet that stops asking stops baking on the next
	// tick, rather than leaving a map frozen at its last light direction.
	TArray<FTerrestrialShadowParams> Requests = MoveTemp(ShadowRequests);
	ShadowRequests.Reset();

	// Non-const, so the render command's copy can resolve its handles.
	for (FTerrestrialShadowParams& Params : Requests)
	{
		if (!Params.IsUsable())
		{
			continue;
		}

		ENQUEUE_RENDER_COMMAND(CloudShadowBake)(
			[Params](FRHICommandListImmediate& RHICmdList) mutable
			{
				Params.ResolveTextures_RenderThread();

				FRDGBuilder GraphBuilder(RHICmdList);

				TerrestrialShadow::AddBakePass_RenderThread(GraphBuilder, Params);

				GraphBuilder.Execute();
			});
	}
}
