#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "FlowSimTypes.generated.h"

class UVolumeTexture;
class UFlowSnapshot;
class UTextureRenderTarget2D;
class UTextureRenderTarget2DArray;

/** The zonal flow the nudge maintains. Mirrors SIM_PROFILE_* in FlowSim.usf. */
UENUM(BlueprintType)
enum class EFlowZonalProfile : uint8
{
	/** Alternating jets and zones, BandCount of them: a gas giant. */
	Banded      UMETA(DisplayName = "Banded"),

	/** Earth's three cells: easterly trades to about 25 degrees, a westerly jet
	 *  peaking at 45, polar easterlies past about 65. JetStrength is the jet's
	 *  peak rate; BandCount, EquatorialBoost, Asymmetry and WidthBias are
	 *  unused. */
	ThreeCell   UMETA(DisplayName = "Three cell (terrestrial)"),
};

/** Which field the debug view renders. Mirrors SIM_DEBUG_* in FlowSim.usf. */
UENUM(BlueprintType)
enum class EFlowDebugMode : uint8
{
	Vorticity   UMETA(DisplayName = "Vorticity"),

	/** Geopotential less its zonal mean. Positive in highs in both hemispheres. */
	Pressure    UMETA(DisplayName = "Pressure"),

	Speed       UMETA(DisplayName = "Speed"),
	East        UMETA(DisplayName = "Eastward velocity"),
	North       UMETA(DisplayName = "Northward velocity"),

	/** Rhs - (I - sL) phi, featureless once converged. MainDebugVisCS has the
	 *  note on reading one that is not. */
	Residual    UMETA(DisplayName = "Helmholtz residual"),

	/** Zonal-mean eastward velocity minus the profile: whether the nudge holds. */
	ZonalError  UMETA(DisplayName = "Zonal profile error"),

	/** -divergence: red rising, blue sinking. */
	Vertical    UMETA(DisplayName = "Vertical motion"),

	/** Speed over gravity-wave speed. */
	Froude      UMETA(DisplayName = "Froude number"),

	/** The cloud tracer, 0 to 1. */
	Cloud       UMETA(DisplayName = "Cloud"),

	/** The vertical motion the cloud formed at, 0 to 1. */
	CloudAscent UMETA(DisplayName = "Cloud formation ascent"),

	/** Noise displacement magnitude, phase A, in radians. */
	NoiseDisplacement UMETA(DisplayName = "Noise displacement"),
};

/** Per-layer multipliers on the shared jet profile: the vertical wind shear.
 *  Multipliers rather than independent profiles, so every layer's jets sit at
 *  the same latitudes. */
USTRUCT(BlueprintType)
struct FFlowLayerProfile
{
	GENERATED_BODY()

	/** Scales JetStrength. Below 1 gives a slower deep layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float JetScale = 1.0f;

	/** Scales EquatorialBoost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float BoostScale = 1.0f;

	/** Scales the stochastic forcing amplitude. A MULTIPLIER: 1 is neutral. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float ForcingScale = 1.0f;

	/** Scales the drag. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float DragScale = 1.0f;
};

/** Everything the sim needs, authored. Re-read at the top of each frame, so the
 *  asset can be edited while the sim runs; only the grid dimensions are latched.
 *
 *  THE REGIME IS SET BY THREE RATIOS, and every look control sits inside it:
 *    Rossby   JetStrength / PlanetaryVorticity. Low is Earth-like: flow slow
 *             against rotation, large balanced systems.
 *    Froude   peak speed / gravity-wave speed. Must stay below about 0.5, or the
 *             flow forms hydraulic jumps.
 *    Size     DeformationRadius. The eddy scale: small gives many narrow bands
 *             and small vortices, large a few big systems.
 *  The start log reports all three. */
UCLASS(BlueprintType)
class CLOUDATMOSPHERE_API UFlowSimConfig : public UDataAsset
{
	GENERATED_BODY()

public:
	// -- Grid ---------------------------------------------------------------

	/** Longitude columns. Rounded down to a power of two, which the Helmholtz
	 *  transform needs and which also keeps the width even for the polar fold. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "32", ClampMax = "2048"))
	int32 GridLongitude = 512;

	/** Latitude rows, in sin(latitude). Half the longitude count gives roughly
	 *  square cells in the tropics. At most 1024, the tallest column the
	 *  Helmholtz solve holds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "16", ClampMax = "1024"))
	int32 GridLatitude = 256;

	/** Stack depth. Two is the smallest with any vertical shear. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "1", ClampMax = "8"))
	int32 LayerCount = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	TArray<FFlowLayerProfile> LayerProfiles;

	// -- Jet profile --------------------------------------------------------
	//
	// The zonal flow the nudge maintains, and the one the sim starts balanced
	// on. See GasGiantJets.ush.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	EFlowZonalProfile ZonalProfile = EFlowZonalProfile::Banded;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile", meta = (ClampMin = "1.0"))
	float BandCount = 3.0f;

	/** Peak angular rate, radians per unit time on a unit sphere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	float JetStrength = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	float EquatorialBoost = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	float Asymmetry = 0.5f;

	/** Positive widens the prograde zones. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	float WidthBias = 0.0f;

	// -- Physics ------------------------------------------------------------

	/** 2 * Omega. Sets the Rossby number against JetStrength and, through its
	 *  variation with latitude, the beta effect that arrests the inverse cascade
	 *  into jets.
	 *
	 *  PITFALL: also the explicit Coriolis step. Its peak rotation per substep is
	 *  this times the step size; above about 0.5 radians the split between
	 *  explicit rotation and implicit pressure starts radiating gravity waves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics")
	float PlanetaryVorticity = 24.0f;

	/** Rossby deformation radius at 45 degrees, in planet radii: the scale where
	 *  rotation and stratification balance, and so the size eddies settle at.
	 *  The gravity-wave speed follows as c = Ld * PlanetaryVorticity * sin(45).
	 *  Large reduces the model to non-divergent flow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "0.01"))
	float DeformationRadius = 0.2f;

	/** Simulated time per second of real time. THE SPEED CONTROL, AND ONLY THAT:
	 *  it sets how many substeps run per frame, never their size, so the same
	 *  state evolves the same way at any speed and a snapshot baked fast plays
	 *  back unchanged. Cost scales with it. Zero freezes the sim without tearing
	 *  it down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "0.0"))
	float TimeScale = 2.0f;

	/** Simulated time per substep. PART OF THE PHYSICS: numerical diffusion,
	 *  divergence damping and the polar filter all act per substep, so changing
	 *  it changes the weather, and a snapshot must be played back at the step it
	 *  was baked at. The Courant numbers are consequences, reported at start.
	 *
	 *  PITFALL: a step that scales with speed makes TimeScale a physics
	 *  parameter too: fewer, larger steps diffuse less per unit time, so the
	 *  clouds stretch at high speed and collapse when it is lowered. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "0.00001", ClampMax = "0.1"))
	float StepSize = 0.0086f;

	/** Cap on substeps per frame. Time beyond it is DISCARDED rather than
	 *  carried, so a stall is not followed by a burst that makes the next frame
	 *  worse; past the cap the sim runs slower than TimeScale asks, still on
	 *  the same steps. Raise it, or use manual steps, to bake at high speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxSubstepsPerFrame = 8;

	// -- Forcing ------------------------------------------------------------

	/** Relaxation of the ZONAL-MEAN eastward velocity toward the profile, per
	 *  unit time. Not of the field, which would erase every eddy each step.
	 *  PITFALL: every nudge is an unbalanced push that the flow answers with
	 *  gravity waves, so strong nudging reads as ripples. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float NudgeRate = 1.0f;

	/** Equilibrium eddy speed the stochastic forcing sustains against the drag,
	 *  per unit slope of the forcing noise. Divergence-free, so it stirs without
	 *  pumping mass. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float ForcingAmplitude = 0.3f;

	/** Forcing noise frequency, in volume UVW per unit sphere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float ForcingScale = 0.25f;

	/** How long one forcing pattern lives, in sim time. Each is a fresh random
	 *  draw carried east with the jets, crossfaded into the next, so eddies are
	 *  born, released and travel. Short is restless stirring; long lets a
	 *  pattern hold eddies in place. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing", meta = (ClampMin = "0.01"))
	float ForcingLifetime = 0.5f;

	/** Linear drag on the eddy part of the eastward velocity and all of the
	 *  northward, per unit time. The large-scale energy sink that arrests the
	 *  cascade, AND what turns flow into lows and out of highs: the Ekman
	 *  convergence the vertical motion output reads. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float DragRate = 1.5f;

	/** Relaxation between vertically adjacent layers. Weak on purpose: strong
	 *  coupling collapses the stack to one layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float LayerCoupling = 0.1f;

	/** Fraction of grid-scale divergence removed per substep: damps
	 *  gravity-wave noise and leaves the rotational flow alone. Scaled per row
	 *  against the grid spacing there, so it is stable at every latitude and
	 *  step size; larger features are damped in proportion to the square of
	 *  their wavenumber. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float DivergenceDamping = 0.05f;

	/** Relaxation of the geopotential toward the profile's balanced state, per
	 *  unit time: jets maintained through their pressure gradient rather than
	 *  pushed directly, as a temperature contrast maintains them. Zero leaves the
	 *  nudge as the only driver. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing", meta = (ClampMin = "0.0"))
	float ThermalRelaxation = 0.0f;

	// -- Cloud --------------------------------------------------------------
	//
	// An advected cloud fraction standing in for moisture: it forms where air
	// rises, clears where air sinks, and is carried by the wind in between.
	// Rates are per unit time against vertical motion normalised to (-1, 1).
	// Written to the weather slice's second channel.

	/** How fast rising air fills a column with cloud. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud", meta = (ClampMin = "0.0"))
	float CondensationRate = 5.0f;

	/** How fast sinking air clears it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud", meta = (ClampMin = "0.0"))
	float EvaporationRate = 3.0f;

	/** How long cloud survives in still air before decaying, in sim time. Longer
	 *  carries cloud further from where it formed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud", meta = (ClampMin = "0.001"))
	float CloudLifetime = 2.0f;

	// -- Noise coordinates --------------------------------------------------
	//
	// Per layer, two displacement fields the cloud noise is sampled through:
	// advected with the flow and reset on phases half a period apart, so the
	// noise follows real trajectories and the reader crossfades the two.

	/** Solid-body drift the noise carries on its own, as a fraction of the
	 *  westerly jet's angular rate. The displacements hold only the flow's
	 *  departure from it, so matching the dominant flow keeps them small. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise Coordinates")
	float NoiseDrift = 0.5f;

	/** How long a displacement accumulates before it resets, in jet turnover
	 *  times (1 / jet angular rate). Longer follows the flow further and
	 *  stretches the noise more; 0.5 keeps the typical stretch under 2 to 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise Coordinates", meta = (ClampMin = "0.05"))
	float NoiseResetPeriod = 0.5f;

	/** The westerly jet's angular rate: JetStrength times layer 0's JetScale. */
	float GetJetRate() const
	{
		return JetStrength * (LayerProfiles.Num() > 0 ? LayerProfiles[0].JetScale : 1.0f);
	}

	/** NoiseDrift as an angular rate, radians per unit sim time. */
	float GetNoiseDriftRate() const
	{
		return NoiseDrift * GetJetRate();
	}

	/** NoiseResetPeriod in sim time. */
	float GetNoiseResetTime() const
	{
		return FMath::Max(NoiseResetPeriod, 0.05f) / FMath::Max(FMath::Abs(GetJetRate()), 1e-3f);
	}

	// -- Polar filter -------------------------------------------------------

	/** cos(latitude) below which the longitudinal filter engages: 0.9 reaches
	 *  to about 26 degrees of latitude, 0.35 only to 70. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Polar Filter", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FilterLatitude = 0.9f;

	/** Bound on the filter width, so the innermost polar rows do not turn into a
	 *  loop over the whole grid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Polar Filter", meta = (ClampMin = "1", ClampMax = "256"))
	int32 FilterMaxHalfWidth = 8;

	// -- Solver -------------------------------------------------------------

	/** Weight of the implicit half of the gravity-wave terms. 0.5 is neutral;
	 *  above it gravity waves are damped, more strongly the higher. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solver", meta = (ClampMin = "0.5", ClampMax = "1.0"))
	float ImplicitWeight = 0.6f;

	// -- Forcing volume -----------------------------------------------------

	/** Band-limited tiling noise, read as a forcing streamfunction. Optional:
	 *  with none bound the forcing is exactly zero. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing Volume")
	TObjectPtr<UVolumeTexture> ForcingVolume;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing Volume", meta = (ClampMin = "0", ClampMax = "3"))
	int32 ForcingChannel = 1;

	/** True when the channel was baked signed. MUST MATCH THE RECIPE: a unipolar
	 *  decode of a signed bake biases the forcing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing Volume")
	bool bForcingBipolar = true;

	// -- Start state --------------------------------------------------------

	/** A captured state to start from. Empty means seed and spin up. A grid
	 *  mismatch is refused and falls back to seeding. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start State")
	TObjectPtr<UFlowSnapshot> InitialState;

	// -- Spin-up ------------------------------------------------------------

	/** Substeps to run before the sim is considered ready. Skipped once
	 *  InitialState is bound. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spin Up", meta = (ClampMin = "0", ClampMax = "8192"))
	int32 SpinUpSteps = 300;

	/** Spin-up substeps per frame. Spread over frames so the spin-up neither
	 *  hitches nor hides: watching the seed organise says more than the
	 *  converged state. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spin Up", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxSpinUpStepsPerFrame = 8;

	// -- Targets ------------------------------------------------------------

	/** RGBA16F 2D array, sized (GridLongitude, GridLatitude, 2 * LayerCount).
	 *  Slices [0, L) are flow, [L, 2L) weather; see FlowField.ush. This is what
	 *  the material samples. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Targets")
	TObjectPtr<UTextureRenderTarget2DArray> FlowTarget;

	/** Any 2D render target. Sized to the grid it is one texel per cell. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Targets")
	TObjectPtr<UTextureRenderTarget2D> DebugTarget;

	/** Reconfigure the targets to match the grid if they do not already. A
	 *  mismatched target is refused, and a refused sim looks exactly like one
	 *  that runs and produces nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Targets")
	bool bAutoResizeTargets = true;

	// -- Debug --------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	EFlowDebugMode DebugMode = EFlowDebugMode::Vorticity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "0", ClampMax = "7"))
	int32 DebugLayer = 0;

	/** Value mapped to full colour. ZERO DERIVES IT PER MODE: the fields differ
	 *  in magnitude by orders, so one number is right for one of them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "0.0"))
	float DebugScale = 0.0f;

	/** Halt stepping without tearing the state down. The debug view keeps
	 *  updating. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bPaused = false;
};

/** Flat snapshot handed to the render thread. Captured BY VALUE into a render
 *  command, so it holds no UObject; RHI references are copied on the game
 *  thread. */
struct FFlowSimParams
{
	FIntVector GridSize = FIntVector(512, 256, 3);

	FVector4f JetParams = FVector4f(3.0f, 1.0f, 0.5f, 0.5f);
	int32 ZonalProfile = 0;
	float WidthBias = 0.0f;
	FVector4f LayerProfile[8] = {
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(),
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero() };

	float DeltaTime = 0.0086f;
	float Time = 0.0f;
	float PlanetaryVorticity = 24.0f;

	/** c^2, and the derived solver constants. */
	float WaveSpeedSq = 1.0f;
	float ImplicitWeight = 0.6f;
	float HelmholtzScale = 0.0f;

	int32 ForcingChannel = 1;
	bool bForcingBipolar = true;

	float NudgeRate = 1.0f;
	float ForcingAmplitude = 0.3f;
	float ForcingScale = 0.25f;
	float ForcingLifetime = 0.5f;
	float DragRate = 1.5f;
	float LayerCoupling = 0.1f;
	float DivergenceDamping = 0.05f;
	float ThermalRelaxation = 0.0f;

	float CondensationRate = 5.0f;
	float EvaporationRate = 3.0f;
	float CloudLifetime = 2.0f;

	/** Radians per unit sim time, and sim time. */
	float NoiseDriftRate = 0.0f;
	float NoiseResetTime = 1.0f;

	float FilterLatitude = 0.9f;
	int32 FilterMaxHalfWidth = 8;

	/** Output normalisation: x pressure, y vorticity, z divergence. */
	FVector3f OutputScales = FVector3f(1.0f, 1.0f, 1.0f);

	/** Face edge of the cube atlas FlowTexture holds, in texels. */
	int32 AtlasFaceSize = 128;

	int32 DebugMode = 0;
	int32 DebugLayer = 0;
	float DebugScale = 1.0f;
	FIntPoint DebugSize = FIntPoint::ZeroValue;

	/** A null forcing texture is legal and evaluates as zero. */
	FTextureRHIRef ForcingTexture;
	FTextureRHIRef FlowTexture;
	FTextureRHIRef DebugTexture;
};