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

/** Where the thermal shear sits in latitude. */
UENUM(BlueprintType)
enum class EFlowThermalShape : uint8
{
	/** One zone per hemisphere about BaroclinicLatitude: the equator-to-pole
	 *  temperature contrast of a terrestrial planet. */
	Midlatitude UMETA(DisplayName = "Midlatitude zone"),

	/** The jet profile itself, sign included: a banded gas giant, whose jets
	 *  decay with height. */
	FollowJets  UMETA(DisplayName = "Follow the jets"),
};

/** Which field the debug view renders. Mirrors SIM_DEBUG_* in FlowSim.usf. */
UENUM(BlueprintType)
enum class EFlowDebugMode : uint8
{
	Vorticity   UMETA(DisplayName = "Vorticity"),

	/** Montgomery potential less its zonal mean. Positive in highs in both
	 *  hemispheres. */
	Pressure    UMETA(DisplayName = "Pressure"),

	Speed       UMETA(DisplayName = "Speed"),
	East        UMETA(DisplayName = "Eastward velocity"),
	North       UMETA(DisplayName = "Northward velocity"),

	/** Rhs - (I - sL) psi for the vertical mode DebugLayer selects,
	 *  featureless once converged. */
	Residual    UMETA(DisplayName = "Helmholtz residual"),

	/** Zonal-mean eastward velocity minus the profile: whether the nudge holds. */
	ZonalError  UMETA(DisplayName = "Zonal profile error"),

	/** The layer's vertical motion: red rising, blue sinking. */
	Vertical    UMETA(DisplayName = "Vertical motion"),

	/** Speed over the wave speed DeformationRadius sets. */
	Froude      UMETA(DisplayName = "Froude number"),

	/** The layer's cloud tracer, 0 to 1. */
	Cloud       UMETA(DisplayName = "Cloud"),

	/** The vertical motion the cloud formed at, 0 to 1. */
	CloudAscent UMETA(DisplayName = "Cloud formation ascent"),

	/** Noise displacement magnitude, phase A, in radians. */
	NoiseDisplacement UMETA(DisplayName = "Noise displacement"),

	/** Relative humidity less CondensationOnset: red where rising air would
	 *  condense. */
	Humidity    UMETA(DisplayName = "Relative humidity"),

	/** Storm intensity, 0 to 1. Storm cells show as discs with a clear eye. */
	Storm       UMETA(DisplayName = "Storm"),

	/** Height of the layer's top: the free surface on layer 0, an interface
	 *  below it. */
	LayerHeight UMETA(DisplayName = "Layer top height"),
};

/** Per-layer settings. Profile values are multipliers on the shared jet
 *  profile, so every layer's jets sit at the same latitudes. */
USTRUCT(BlueprintType)
struct FFlowLayerProfile
{
	GENERATED_BODY()

	/** Scales JetStrength. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float JetScale = 1.0f;

	/** Scales EquatorialBoost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float BoostScale = 1.0f;

	/** Scales the stochastic forcing amplitude. A MULTIPLIER: 1 is neutral. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float ForcingScale = 1.0f;

	/** Scales the drag. The bottom layer carries the surface drag; layers above
	 *  it want little. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float DragScale = 1.0f;

	/** Relative depth of the layer in the stack. A thicker top layer leaves the
	 *  interface more room to rise toward the poles before it reaches the top. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer", meta = (ClampMin = "0.1"))
	float DepthScale = 1.0f;
};

namespace FlowSimStep
{
	/** Smallest running step StepSize accepts. */
	static constexpr float Min = 1e-6f;

	/** Spin-up step, and the largest running step: reaches a developed state
	 *  quickly. Its weather carries roughly 40% more thick cloud than 1e-5,
	 *  and relaxes to the running step's look over about a cloud lifetime
	 *  after spin-up ends. */
	static constexpr float SpinUp = 0.0086f;

	/** Steps a frame above which the log warns that the sim dominates frame
	 *  time. */
	static constexpr int32 WarnPerFrame = 64;

	/** Hang guard, not a budget: one frame's graph holds about a dozen passes
	 *  per step. Past it the sim runs slower than asked. */
	static constexpr int32 MaxPerFrame = 2048;

	/** Step above which a stack of layers runs at no less than StackWeight: its
	 *  layers' explicit pressure terms move at each other's speeds, and at
	 *  large steps that amplifies polar grid-scale noise below it. */
	static constexpr float StackLargeStep = 1e-3f;
	static constexpr float StackWeight = 0.75f;
}

/** Everything the sim needs, authored. Re-read at the top of each frame, so the
 *  asset can be edited while the sim runs; only the grid dimensions are latched.
 *
 *  THE REGIME IS SET BY THREE RATIOS, and every look control sits inside it:
 *    Rossby   JetStrength / PlanetaryVorticity. Low is Earth-like.
 *    Froude   peak speed / gravity-wave speed. Must stay below about 0.5.
 *    Size     DeformationRadius. The eddy scale.
 *  On a stack, the thermal shear against the deformation radius sets how
 *  readily the shear breaks into storms. The start log reports all of them. */
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

	/** Latitude rows, in sin(latitude). At most 1024, the tallest column the
	 *  Helmholtz solve holds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "16", ClampMax = "1024"))
	int32 GridLatitude = 256;

	/** Layers in the stack, 0 on top, coupled through their pressure. Two is
	 *  the smallest with baroclinic storms; one is a single shallow layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "1", ClampMax = "8"))
	int32 LayerCount = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	TArray<FFlowLayerProfile> LayerProfiles;

	// -- Jet profile --------------------------------------------------------

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

	/** 2 * Omega. Sets the Rossby number against JetStrength and the beta
	 *  effect that arrests the inverse cascade into jets.
	 *
	 *  PITFALL: also the explicit Coriolis step. Above about 0.5 radians of
	 *  rotation per step the split between explicit rotation and implicit
	 *  pressure radiates gravity waves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics")
	float PlanetaryVorticity = 24.0f;

	/** Rossby deformation radius at 45 degrees, in planet radii: the size eddies
	 *  settle at. On a stack it is the first internal mode's, the one weather
	 *  systems grow at; the stack's depth follows from it and Stratification. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "0.01"))
	float DeformationRadius = 0.2f;

	/** Density step at each interface, as a fraction of the surface's. Small
	 *  keeps the free surface nearly flat, so pressure systems are carried by
	 *  the interfaces. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float Stratification = 0.1f;

	/** Sim time per second of real time: THE SPEED HANDLE. The step is
	 *  StepSize whatever the speed, so speed sets the steps per frame and the
	 *  cost with it, and never the look. Zero freezes the sim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "0.0"))
	float SimSpeed = 0.0025f;

	/** Sim time per step: a look and cost control, independent of speed.
	 *
	 *  PITFALL: THE WEATHER DEPENDS ON THE STEP, and no conversion of the
	 *  per-step settings removes that. The semi-Lagrangian interpolation
	 *  smooths once per step, and the solver splits grid-scale gravity waves
	 *  between pressure and divergence by an amount the step sets. Larger
	 *  steps give sharper, thicker cloud. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "0.000001", ClampMax = "0.0086"))
	float StepSize = 1e-5f;

	float GetStepSize() const { return FMath::Clamp(StepSize, FlowSimStep::Min, FlowSimStep::SpinUp); }

	// -- Forcing ------------------------------------------------------------

	/** Relaxation of each layer's ZONAL-MEAN eastward velocity toward its
	 *  profile, per unit time. PITFALL: every nudge is an unbalanced push that
	 *  the flow answers with gravity waves, so strong nudging reads as
	 *  ripples. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float NudgeRate = 1.0f;

	/** Equilibrium eddy speed the stochastic forcing sustains against the drag,
	 *  per unit slope of the forcing noise. Divergence-free. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float ForcingAmplitude = 0.3f;

	/** Forcing noise frequency, in volume UVW per unit sphere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float ForcingScale = 0.25f;

	/** How long one forcing pattern lives, in sim time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing", meta = (ClampMin = "0.01"))
	float ForcingLifetime = 0.5f;

	/** Linear drag on the eddy part of the eastward velocity and all of the
	 *  northward, per unit time, scaled per layer. The energy sink that arrests
	 *  the cascade, and what turns flow into lows and out of highs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float DragRate = 1.5f;

	/** Relaxation of each layer's velocity toward its neighbours': interfacial
	 *  friction. Strong coupling erodes the shear storms grow from. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float LayerCoupling = 0.1f;

	/** Fraction of grid-scale divergence removed per step, scaled per row
	 *  against the grid spacing there. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float DivergenceDamping = 0.05f;

	// -- Thermal forcing ----------------------------------------------------
	//
	// Each layer's target is the jet profile plus its share of ThermalShear:
	// all of it on the top layer, none on the bottom. The nudge holds the
	// winds to it and the relaxation holds the interfaces at the heights in
	// balance with it, which is the temperature contrast storms draw on.

	/** Rate interfaces relax toward their balanced heights, per unit time, by
	 *  moving mass between layers; column mass is untouched. On a single layer
	 *  its thickness relaxes instead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thermal Forcing", meta = (ClampMin = "0.0"))
	float ThermalRelaxation = 0.5f;

	/** Vertical shear between the top and bottom layer, angular rate. Storms
	 *  grow once it exceeds about PlanetaryVorticity * DeformationRadius^2 at
	 *  the zone; well past that the interface reaches the top of the stack. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thermal Forcing")
	float ThermalShear = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thermal Forcing")
	EFlowThermalShape ThermalShape = EFlowThermalShape::Midlatitude;

	/** Centre of the midlatitude zone, degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thermal Forcing", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float BaroclinicLatitude = 45.0f;

	/** Half-width of the midlatitude zone, degrees. Storms need it to span a
	 *  few deformation radii. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Thermal Forcing", meta = (ClampMin = "1.0", ClampMax = "90.0"))
	float BaroclinicWidth = 24.0f;

	// -- Moisture -----------------------------------------------------------
	//
	// Vapour per layer, in units of the equator's surface saturation. The
	// surface evaporates into the bottom layer; rising air near saturation
	// condenses it into cloud, releasing latent heat.

	/** Saturation at the equator and at the poles, bottom layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Moisture", meta = (ClampMin = "0.0"))
	float SaturationEquator = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Moisture", meta = (ClampMin = "0.0"))
	float SaturationPole = 0.25f;

	/** The top layer's saturation as a fraction of the bottom's; layers between
	 *  fall geometrically. Cold air aloft holds little. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Moisture", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float UpperSaturation = 0.3f;

	/** Relative humidity at which rising air starts to condense; it condenses
	 *  fully at saturation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Moisture", meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float CondensationOnset = 0.7f;

	/** Rate the surface moistens the bottom layer toward saturation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Moisture", meta = (ClampMin = "0.0"))
	float SurfaceEvaporation = 2.0f;

	/** How much the bottom layer's wind speed raises surface evaporation, per
	 *  unit speed: the moisture supply under a storm's own winds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Moisture", meta = (ClampMin = "0.0"))
	float WindEvaporation = 1.0f;

	/** Share of a layer's depth moved up across the interface above it per
	 *  unit of vapour condensed: the latent heat that deepens lows under
	 *  condensing air. On a single layer it draws mass up into the layer.
	 *  PITFALL: a positive feedback; strong values run away into grid-scale
	 *  convection. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Moisture", meta = (ClampMin = "0.0"))
	float LatentHeating = 0.1f;

	// -- Cloud --------------------------------------------------------------
	//
	// An advected cloud fraction fed by condensation, cleared by sinking air,
	// and carried by the wind in between. Rates are per unit time against
	// vertical motion normalised to (-1, 1).

	/** How fast rising saturated air fills a column with cloud. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud", meta = (ClampMin = "0.0"))
	float CondensationRate = 5.0f;

	/** How fast sinking air evaporates cloud back into vapour. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud", meta = (ClampMin = "0.0"))
	float EvaporationRate = 3.0f;

	/** How long cloud survives in still air before raining out, in sim time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud", meta = (ClampMin = "0.001"))
	float CloudLifetime = 3.0f;

	// -- Storms -------------------------------------------------------------
	//
	// An advected storm intensity, 0 to 1, grown where condensation is
	// intense and the air spins cyclonically. The deck draws it as storm cloud.

	/** How fast a storm builds at full drive, per unit time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storms", meta = (ClampMin = "0.0"))
	float StormRate = 4.0f;

	/** Condensing ascent, W near saturation, below which nothing builds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storms", meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float StormThreshold = 0.1f;

	/** How much normalised cyclonic vorticity raises the drive. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storms", meta = (ClampMin = "0.0"))
	float StormSpin = 2.0f;

	/** How long a storm lasts once its drive is gone, in sim time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storms", meta = (ClampMin = "0.001"))
	float StormLifetime = 1.0f;

	// -- Storm cells ----------------------------------------------------------
	//
	// Tracked tropical storms: each spawns where the genesis conditions hold,
	// moves with the stack's mean flow, spins the flow into a compact vortex
	// and holds a storm disc with a clear eye while it lives.

	/** Cells alive at once, up to FlowSimShader::MaxStormCells. Zero turns them
	 *  off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm Cells", meta = (ClampMin = "0", ClampMax = "32"))
	int32 MaxStormCells = 12;

	/** Spawn attempts per unit sim time, planet-wide. An attempt holds only
	 *  where the genesis window and humidity allow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm Cells", meta = (ClampMin = "0.0"))
	float StormCellSpawnRate = 4.0f;

	/** Vortex radius, degrees of arc; the winds peak at about 0.7 of it. Wants
	 *  at least four grid cells, 3.5 degrees at 512 columns. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm Cells", meta = (ClampMin = "0.5", ClampMax = "30.0"))
	float StormCellRadius = 4.5f;

	/** Peak wind of a full-strength cell's vortex, in the jet's units. What the
	 *  flow reaches is less, drag working against it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm Cells", meta = (ClampMin = "0.0"))
	float StormCellWind = 2.0f;

	/** Rate the flow spins toward the vortex and the storm disc fills, per
	 *  unit time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm Cells", meta = (ClampMin = "0.0"))
	float StormCellSpinUp = 10.0f;

	/** Eye radius as a fraction of the vortex radius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm Cells", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float StormCellEye = 0.3f;

	/** Rates intensity grows while conditions hold and decays once they fail,
	 *  per unit time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm Cells", meta = (ClampMin = "0.0"))
	float StormCellGrowth = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm Cells", meta = (ClampMin = "0.0"))
	float StormCellDecay = 1.0f;

	/** Sim time after which a cell decays whatever the conditions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm Cells", meta = (ClampMin = "0.01"))
	float StormCellLifetime = 3.0f;

	/** Poleward-west drift on top of the steering flow, in the jet's units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm Cells", meta = (ClampMin = "0.0"))
	float StormCellDrift = 0.05f;

	/** The top layer's reversed share of the vortex: the outflow anticyclone. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm Cells", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StormCellOutflow = 0.5f;

	/** Latitudes, degrees, between which cells form. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm Cells", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float GenesisLatitudeMin = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm Cells", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float GenesisLatitudeMax = 22.0f;

	/** Speed difference between the top and bottom layers at which the window
	 *  closes: shear tears a storm apart. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm Cells", meta = (ClampMin = "0.01"))
	float GenesisShear = 1.0f;

	/** Bottom layer's relative humidity a cell needs to form; it weakens below
	 *  0.15 under this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm Cells", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float GenesisHumidity = 0.85f;

	// -- Noise coordinates --------------------------------------------------

	/** Solid-body drift the noise carries on its own, as a fraction of the
	 *  westerly jet's angular rate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise Coordinates")
	float NoiseDrift = 0.5f;

	/** How long a displacement accumulates before it resets, in jet turnover
	 *  times (1 / jet angular rate). */
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

	/** cos(latitude) below which the longitudinal filter engages. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Polar Filter", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FilterLatitude = 0.9f;

	/** Bound on the filter width, so the innermost polar rows do not turn into a
	 *  loop over the whole grid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Polar Filter", meta = (ClampMin = "1", ClampMax = "256"))
	int32 FilterMaxHalfWidth = 8;

	// -- Solver -------------------------------------------------------------

	/** Weight of the implicit half of the gravity-wave terms. 0.5 is neutral;
	 *  above it gravity waves are damped. A stack runs at least
	 *  FlowSimStep::StackWeight at large steps. */
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

	/** A captured state to start from. Empty means seed and spin up. A grid or
	 *  layout mismatch is refused and falls back to seeding. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start State")
	TObjectPtr<UFlowSnapshot> InitialState;

	// -- Spin-up ------------------------------------------------------------

	/** Substeps to run before the sim is considered ready. Skipped once
	 *  InitialState is bound. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spin Up", meta = (ClampMin = "0", ClampMax = "8192"))
	int32 SpinUpSteps = 300;

	/** Spin-up substeps per frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spin Up", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxSpinUpStepsPerFrame = 8;

	// -- Targets ------------------------------------------------------------

	/** RGBA16F 2D array, the cube atlas with 4 * LayerCount slices; see
	 *  FlowField.ush. This is what the material samples. */
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

	/** Layer to view; the vertical mode, for the residual. */
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

/** The stack's vertical structure, derived from the config: every layer's
 *  depth and every coupling matrix, 8 x 8 row-major in 16 float4s. */
struct FFlowSimStack
{
	/** Mean geopotential of each layer. */
	float Depth[8] = {};

	/** Wave speed squared of each vertical mode, fastest first. */
	float ModeSpeedSq[8] = {};

	/** Layers to Montgomery potential, and back. */
	FVector4f Montgomery[16];
	FVector4f MontgomeryInverse[16];

	/** Modes to layers, layers to modes, modes to Montgomery potential. */
	FVector4f ModeToLayer[16];
	FVector4f LayerToMode[16];
	FVector4f ModeToMontgomery[16];

	/** The wave speed DeformationRadius sets: the first internal mode's, or the
	 *  single layer's. */
	float DesignSpeedSq = 1.0f;
};

/** Flat snapshot handed to the render thread. Captured BY VALUE into a render
 *  command, so it holds no UObject; RHI references are copied on the game
 *  thread. */
struct FFlowSimParams
{
	FIntVector GridSize = FIntVector(512, 256, 2);

	FVector4f JetParams = FVector4f(3.0f, 1.0f, 0.5f, 0.5f);
	int32 ZonalProfile = 0;
	float WidthBias = 0.0f;
	FVector4f LayerProfile[8] = {
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(),
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero() };

	/** Per layer: x depth, y the Helmholtz scale of the mode in its slice,
	 *  z saturation factor, w height output scale. */
	FVector4f LayerState[8] = {
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(),
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero() };

	FFlowSimStack Stack;

	float DeltaTime = 0.0086f;
	float Time = 0.0f;
	float PlanetaryVorticity = 24.0f;
	float ImplicitWeight = 0.6f;

	int32 ForcingChannel = 1;
	bool bForcingBipolar = true;

	float NudgeRate = 1.0f;
	float ForcingAmplitude = 0.3f;
	float ForcingScale = 0.25f;
	float ForcingLifetime = 0.5f;
	float DragRate = 1.5f;
	float LayerCoupling = 0.1f;
	float DivergenceDamping = 0.05f;

	float ThermalRelaxation = 0.5f;

	/** x shear, y shape (0 midlatitude, 1 jets), z zone latitude, w zone
	 *  half-width, radians. */
	FVector4f ThermalParams = FVector4f::Zero();

	float CondensationRate = 5.0f;
	float EvaporationRate = 3.0f;
	float CloudLifetime = 3.0f;

	/** x saturation at the equator, y at the poles, z condensation onset,
	 *  w surface evaporation. */
	FVector4f MoistureParams = FVector4f(1.0f, 0.25f, 0.7f, 2.0f);
	float WindEvaporation = 1.0f;
	float LatentHeating = 0.1f;

	/** x rate, y threshold, z spin, w decay rate. */
	FVector4f StormParams = FVector4f(4.0f, 0.1f, 2.0f, 1.0f);

	/** See SimCellShape, SimCellLife, SimCellMotion and SimCellGenesis in
	 *  FlowSim.usf. */
	FVector4f CellShape = FVector4f::Zero();
	FVector4f CellLife = FVector4f::Zero();
	FVector4f CellMotion = FVector4f::Zero();
	FVector4f CellGenesis = FVector4f::Zero();

	/** Steps completed before the frame's first; seeds the cells' spawns. */
	int32 StepIndex = 0;

	/** Radians per unit sim time, and sim time. */
	float NoiseDriftRate = 0.0f;
	float NoiseResetTime = 1.0f;

	float FilterLatitude = 0.9f;
	int32 FilterMaxHalfWidth = 8;

	/** Where the output sits between the state before the frame's last step
	 *  (0) and after it (1). */
	float StateBlend = 1.0f;

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