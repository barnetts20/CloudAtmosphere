#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "FlowSimTypes.generated.h"

class FTextureResource;
class FTextureRenderTargetResource;
class UVolumeTexture;
class UFlowSnapshot;

/** The zonal flow the nudge maintains. Mirrors SIM_PROFILE_* in FlowSim.usf. */
UENUM(BlueprintType)
enum class EFlowZonalProfile : uint8
{
	/** Alternating jets and zones, BandCount of them: a gas giant. */
	Banded      UMETA(DisplayName = "Banded"),

	/** Earth's three cells: easterly trades to about 25 degrees, a westerly jet
	 *  peaking at JetLatitude, polar easterlies past about 65. BandCount,
	 *  EquatorialBoost, Asymmetry, WidthBias and the layers' BoostScale are
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

	/** Storm intensity, 0 to 1, in red; storm cells' vector strength in blue
	 *  where it is larger. */
	Storm       UMETA(DisplayName = "Storm"),

	/** Height of the layer's top: the free surface on layer 0, an interface
	 *  below it. */
	LayerHeight UMETA(DisplayName = "Layer top height"),

	/** Cloud cover of the layer and every layer above it, 0 to 1: what the
	 *  terrestrial deck reads with its CloudLayer set to this layer. */
	ColumnCloud UMETA(DisplayName = "Column cloud"),

	/** The eye tracer, 0 to 1: clear air fed at storm cells' cores and carried
	 *  by the flow, which is how far their eyes thin the deck. */
	Eye         UMETA(DisplayName = "Storm eye"),

	/** Each storm cell's disc in its conditions: red the genesis window, green
	 *  the humidity, blue the parent storm, so a missing colour names what is
	 *  failing. The eyewall disc is the cell's favour in grey. */
	CellHealth  UMETA(DisplayName = "Storm cell health"),

	/** What a storm seed at each point is judged on, each against its gate:
	 *  red the genesis window, green the humidity, blue the storm. Bright where
	 *  all three pass, dim where any fails, dimmer near a live cell. */
	Genesis     UMETA(DisplayName = "Storm genesis"),

	/** Each storm cell's disc: red the debug layer's vortex gain, green its
	 *  inflow gain, as a share of the gain limit. Full brightness is pinned. */
	CellGains   UMETA(DisplayName = "Storm cell gains"),
};

/** Per-layer settings. Profile values are multipliers on the shared jet
 *  profile, so every layer's jets sit at the same latitudes. */
USTRUCT(BlueprintType)
struct FFlowLayerProfile
{
	GENERATED_BODY()

	/** Scales the jets, JetSpeed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float JetScale = 1.0f;

	/** Scales EquatorialBoost. Banded profile only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float BoostScale = 1.0f;

	/** This layer's equilibrium eddy speed as a multiple of EddySpeed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer", meta = (ClampMin = "0.0"))
	float EddyScale = 1.0f;

	/** Scales the drag, and the eddy forcing with it, so EddyScale still sets
	 *  the layer's eddy speed. The bottom layer carries the surface drag; layers
	 *  above it want little. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float DragScale = 1.0f;

	/** Relative depth of the layer in the stack, normalised over the layers, so
	 *  only the ratios matter. A thicker top layer leaves the interface more
	 *  room to rise toward the poles before it reaches the top. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer", meta = (ClampMin = "0.1"))
	float DepthScale = 1.0f;
};

/** A storm that never dies: a storm cell held in place, built by the same
 *  vortex, inflow, stamp and eye as the hurricanes, with its own size, wind and
 *  oval. It sits between two jets of opposite direction, the shear between
 *  them setting its spin, and moves with the flow across it (Steering) plus
 *  Drift, in closed form, so a snapshot carries none of it. The hurricane
 *  shape and look settings (eyewall, falloff, wind breadth, eye) shape it too. */
USTRUCT(BlueprintType)
struct FFlowPerpetualStorm
{
	GENERATED_BODY()

	/** Latitude asked for, degrees. The storm settles at the nearest latitude
	 *  where the zonal flow changes direction, the middle of the shear zone
	 *  between two opposite jets, within 45 degrees; the dump reports where. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm", meta = (ClampMin = "-85.0", ClampMax = "85.0"))
	float Latitude = -22.0f;

	/** Centre longitude at sim time zero, degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm", meta = (UIMin = "-180.0", UIMax = "180.0"))
	float Longitude = 0.0f;

	/** Half-height across latitude, in deformation radii, as StormCellRadius;
	 *  held large enough that its eyewall spans two grid columns. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm", meta = (ClampMin = "0.05"))
	float Radius = 1.0f;

	/** East-west half-length over the half-height: 1 is round. The wind falls
	 *  to 1 / Aspect of Wind at its east and west ends. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm", meta = (ClampMin = "1.0", ClampMax = "4.0"))
	float Aspect = 1.8f;

	/** Eyewall wind the flow is pushed toward, a fraction of the speed root, as
	 *  StormCellWind. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm", meta = (ClampMin = "0.0", ClampMax = "0.9"))
	float Wind = 0.5f;

	/** Inflow below and outflow above as a share of Wind, as StormCellInflow:
	 *  the vortex winds what it draws in into spiral arms. Negative reverses
	 *  it. Needs two layers or more. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float Spiral = 0.2f;

	/** Share of the flow across it that carries it: the zonal wind over its
	 *  latitude span, its eyewall's two sides weighted most. 0 holds it in
	 *  place; 1 lets an imbalance between the jets it straddles move it toward
	 *  the stronger's direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Steering = 1.0f;

	/** Eastward drift on top of the steering, a fraction of the speed root;
	 *  negative drifts west. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float Drift = 0.0f;

	/** Storm raised across the whole storm, as StormCellStorm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Storm = 0.8f;

	/** Cloud cover its eyewall settles at against the cloud's decay alone, as
	 *  StormCellCloudCover. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm", meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float Cover = 0.8f;

	/** Pressure drop across the whole storm, as StormCellPressure: the deck
	 *  raises its cloud top over it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float Lift = 0.5f;

	/** Share of a hurricane's eye it opens: 0 a calm core with no hole, 1 an
	 *  eye as deep as a hurricane's. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Storm", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Eye = 0.0f;
};

/** The layer profiles a new config starts with: a light top layer with the
 *  stronger eddies and a deeper, draggier bottom layer at the surface. */
inline TArray<FFlowLayerProfile> FlowSimDefaultLayers()
{
	FFlowLayerProfile Top;
	Top.EddyScale = 1.5f;
	Top.DragScale = 0.5f;
	Top.DepthScale = 1.5f;

	FFlowLayerProfile Bottom;
	Bottom.EddyScale = 0.05f;
	Bottom.DragScale = 1.5f;

	return { Top, Bottom };
}

/** Step limits in turnovers, so a step turns the flow by the same angle at any
 *  rotation rate; the subsystem steps in sim time, UFlowSimConfig converting. */
namespace FlowSimStep
{
	/** Smallest running step StepSize accepts. */
	static constexpr float Min = 1e-5f;

	/** Spin-up step, and the largest running step: reaches a developed state
	 *  quickly. Its weather carries more thick cloud than a running step a
	 *  hundredth its size, and relaxes to the running step's look over about a
	 *  cloud lifetime after spin-up ends. */
	static constexpr float SpinUp = 0.044f;

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

/** The solver's numerics, frozen at their tuned values. */
namespace FlowSimNumerics
{
	/** Weight of the implicit half of the gravity-wave terms. 0.5 is neutral;
	 *  above it gravity waves are damped, by an amount that grows with the
	 *  step, and that damping counts toward GridDamping. A stack runs at least
	 *  FlowSimStep::StackWeight at large steps. */
	static constexpr float ImplicitWeight = 0.6f;

	/** Gain of the compression-activated divergence damping: where a front
	 *  steepens, the grid-scale divergence a step removes grows by this times
	 *  the local compression, capped at the explicit scheme's bound. */
	static constexpr float ShockDamping = 2.0f;

	/** Time constant, in turnovers, of the low-pass the vertical motion takes
	 *  along the flow before condensation, latent heat and the deck read it:
	 *  what moves with the air holds, while gravity waves and bores average out
	 *  instead of drawing travelling lines into the cloud. */
	static constexpr float AscentSmoothing = 1.527f;
}

/** The sim's speeds, rates and lengths, resolved from a config's authored
 *  fractions of the speed root, turnovers and deformation radii; see
 *  UFlowSimConfig::ResolveScales. */
struct FFlowSimScales
{
	/** First internal mode's wave speed, the speed root, and the eddy turnover
	 *  time DeformationRadius / Root. */
	float WaveSpeed = 1.0f;
	float Root = 1.0f;
	float Turnover = 1.0f;

	/** Angular rates for profiles that peak at 1. */
	float JetStrength = 0.0f;
	float ThermalShear = 0.0f;

	/** Speeds: eddies per unit forcing slope, the storm cells' vortex scale and
	 *  drift, and the shear that closes genesis. */
	float EddySpeed = 0.0f;
	float CellWind = 0.0f;
	float CellDrift = 0.0f;
	float GenesisShear = 1.0f;

	/** Surface evaporation gain per unit wind speed. */
	float WindEvaporation = 0.0f;

	/** Rates, per unit sim time. */
	float NudgeRate = 0.0f;
	float DragRate = 0.0f;
	float ThermalRelaxation = 0.0f;
	float LayerCoupling = 0.0f;
	float SurfaceEvaporation = 0.0f;
	float CondensationRate = 0.0f;
	float EvaporationRate = 0.0f;
	float CellSpawnRate = 0.0f;
	float CellGrowth = 0.0f;
	float CellFollow = 0.0f;
	float CellCoreFollow = 0.0f;
	float CellForcing = 0.0f;

	/** Lifetimes and time constants, in sim time. */
	float CloudLifetime = 1.0f;
	float AscentSmoothing = 0.0f;
	float StormLifetime = 1.0f;
	float CellLifetime = 1.0f;
	float ForcingLifetime = 1.0f;

	/** The storm cells' radius, radians, and the forcing volume's tiles per
	 *  planet radius. */
	float CellRadius = 0.1f;
	float ForcingFrequency = 0.25f;
};

/** Everything the sim needs, authored. Re-read at the top of each frame, so the
 *  asset can be edited while the sim runs; only the grid dimensions are latched.
 *
 *  EVERY WIND IS A FRACTION OF ONE SPEED, the root: SpeedRoot times the
 *  wave speed DeformationRadius and PlanetaryVorticity set. The ceiling caps
 *  faces at the root, easing in from 0.7 of it, so winds authored under 0.7
 *  are not clipped, and a config holds its look as the regime changes. The
 *  Rossby number of the fastest flow is SpeedRoot itself.
 *
 *  EVERY RATE AND LIFETIME IS IN TURNOVERS, DeformationRadius at the root,
 *  and the storm cells' radius and the forcing's scale are in deformation
 *  radii. The start log
 *  reports the wave speeds, the budget against 0.7, the lifetimes in days
 *  and the storm criterion. */
UCLASS(BlueprintType, meta = (PrioritizeCategories = "Planet Time Quality Layers Numerics Pipeline Debug"))
class CLOUDATMOSPHERE_API UFlowSimConfig : public UDataAsset
{
	GENERATED_BODY()

public:
	// -- Planet: winds ------------------------------------------------------
	//
	// Every wind is a fraction of the speed root. Each layer's target is the
	// jet profile plus its share of ShearSpeed: all of it on the top layer,
	// none on the bottom. The nudge holds the winds to it and the thermal
	// relaxation holds the interfaces at the heights in balance with it, which
	// is the temperature contrast storms draw on.

	/** The zonal winds the nudge holds: a gas giant's alternating bands, or a
	 *  terrestrial planet's three cells. It also sets the storm cells' spin:
	 *  the shear of the band each sits in when banded, the hemisphere's when
	 *  three-cell. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds")
	EFlowZonalProfile ZonalProfile = EFlowZonalProfile::Banded;

	/** Latitude of the three cells' westerly jet, degrees. The profile stretches
	 *  in latitude about the equator with it, so the trades and the polar
	 *  easterlies move in proportion; past about 53 the polar easterlies leave
	 *  the pole. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ZonalProfile == EFlowZonalProfile::ThreeCell", EditConditionHides, ClampMin = "15.0", ClampMax = "75.0"))
	float JetLatitude = 45.0f;

	/** The easterly trades' strength against the westerly jet's: stronger
	 *  trades carry tropical weather west faster and shear it against the jet. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ZonalProfile == EFlowZonalProfile::ThreeCell", EditConditionHides, UIMin = "0.0", UIMax = "1.0"))
	float TradeWindStrength = 0.35f;

	/** The polar easterlies' strength against the westerly jet's: stronger is
	 *  a sharper polar front. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ZonalProfile == EFlowZonalProfile::ThreeCell", EditConditionHides, UIMin = "0.0", UIMax = "1.0"))
	float PolarEasterlyStrength = 0.5f;

	/** Rossby deformation radius at 45 degrees, in planet radii: the size eddies
	 *  settle at. On a stack it is the first internal mode's, the one weather
	 *  systems grow at; the stack's depth follows from it and Stratification. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (ClampMin = "0.01"))
	float DeformationRadius = 0.2f;

	/** THE SPEED ROOT, in Froude number against the first internal mode's wave
	 *  speed: every authored wind is a fraction of this times that speed. After
	 *  all forcing every face eases toward it from 0.7 of it, so the flow stays
	 *  short of the speeds where shallow water steepens into bores. About 0.6
	 *  is the top of the usable range; lower slows the whole system. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (ClampMin = "0.1", ClampMax = "1.0"))
	float SpeedRoot = 0.6f;

	/** The jets' peak eastward wind as a fraction of the speed root, on a layer
	 *  whose JetScale and BoostScale are 1. Negative reverses them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds")
	float JetSpeed = 0.35f;

	/** Peak eastward wind of the top layer over the bottom's, as a fraction of
	 *  the speed root. Storms grow from it past the criterion the start log
	 *  reports; well past that the interface reaches the top of the stack. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "LayerCount > 1", EditConditionHides))
	float ShearSpeed = 0.2f;

	/** How many times the band pattern repeats over sin(latitude) from pole
	 *  to pole: each repeat is a prograde and a retrograde jet. Fractional
	 *  values are allowed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ZonalProfile == EFlowZonalProfile::Banded", EditConditionHides, ClampMin = "1.0"))
	float BandCount = 3.0f;

	/** Centre of the midlatitude zone, degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ThermalShape == EFlowThermalShape::Midlatitude", EditConditionHides, ClampMin = "0.0", ClampMax = "90.0"))
	float BaroclinicLatitude = 45.0f;

	/** Half-width of the midlatitude zone, degrees. Storms need it to span a
	 *  few deformation radii. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ThermalShape == EFlowThermalShape::Midlatitude", EditConditionHides, ClampMin = "1.0", ClampMax = "90.0"))
	float BaroclinicWidth = 24.0f;

	/** Extra prograde wind in the equatorial jet, super-rotation, as a share of
	 *  the jets' strength; each layer scales it by its BoostScale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ZonalProfile == EFlowZonalProfile::Banded", EditConditionHides))
	float EquatorialBoost = 0.5f;

	/** Weight of an odd term that shifts the bands differently in each
	 *  hemisphere: 0 mirrors them about the equator. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ZonalProfile == EFlowZonalProfile::Banded", EditConditionHides))
	float Asymmetry = 0.5f;

	/** Positive widens the prograde zones. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ZonalProfile == EFlowZonalProfile::Banded", EditConditionHides))
	float WidthBias = 0.0f;

	/** Weight of a second, incommensurate cosine in the band shape: 0 spaces
	 *  the jets evenly at equal strength, higher makes widths and strengths
	 *  uneven, some bands merging. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ZonalProfile == EFlowZonalProfile::Banded", EditConditionHides, ClampMin = "0.0", UIMax = "0.8"))
	float JetIrregularity = 0.45f;

	/** The second cosine's frequency over the first: which bands are wide or
	 *  strong. PITFALL: near a whole ratio the pattern repeats. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ZonalProfile == EFlowZonalProfile::Banded", EditConditionHides, UIMin = "1.3", UIMax = "1.9"))
	float JetHarmonic = 1.7f;

	/** How far the jets' tops flatten into plateaus of even wind: the share
	 *  of the shape's range above which it saturates. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ZonalProfile == EFlowZonalProfile::Banded", EditConditionHides, ClampMin = "0.05", ClampMax = "0.95", UIMin = "0.2", UIMax = "0.7"))
	float JetFlatness = 0.5f;

	/** Latitude, degrees, at which EquatorialBoost falls to half: a narrow
	 *  equatorial jet or a broad superrotating belt. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ZonalProfile == EFlowZonalProfile::Banded", EditConditionHides, ClampMin = "0.5", ClampMax = "89.0", UIMin = "5.0", UIMax = "30.0"))
	float EquatorialJetWidth = 13.9065f;

	/** Where ShearSpeed sits in latitude: a midlatitude zone about
	 *  BaroclinicLatitude, or the jets' own profile. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds")
	EFlowThermalShape ThermalShape = EFlowThermalShape::Midlatitude;

	/** 2 * Omega. With DeformationRadius it sets the wave speed the speed root
	 *  scales. Every wind is a fraction of the root and every rate and
	 *  lifetime is in turnovers, so the jets' spacing, the eddies' size and the
	 *  Rossby number hold as it changes: beyond the Coriolis step it sets the
	 *  tempo alone, as SimSpeed does.
	 *
	 *  PITFALL: also the explicit Coriolis step. Above about 0.5 radians of
	 *  rotation per step the split between explicit rotation and implicit
	 *  pressure radiates gravity waves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (ClampMin = "0.1"))
	float PlanetaryVorticity = 24.0f;

	// -- Planet: flow rates -------------------------------------------------
	// How hard the flow is held to its profile, dragged and rebalanced.

	/** Relaxation of each layer's ZONAL-MEAN eastward velocity toward its
	 *  profile, per turnover. PITFALL: every nudge is an unbalanced push that
	 *  the flow answers with gravity waves, so strong nudging reads as
	 *  ripples. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Flow Rates")
	float NudgeRate = 0.1f;

	/** Linear drag on the eddy part of the eastward velocity and all of the
	 *  northward, per turnover, scaled per layer. The energy sink that arrests
	 *  the cascade, and what turns flow into lows and out of highs. The
	 *  forcing scales with it, so it sets how long eddies stay correlated and
	 *  not how fast they run; 0 turns the forcing off too. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Flow Rates")
	float DragRate = 0.15f;

	/** Rate interfaces relax toward their balanced heights, per turnover, by
	 *  moving mass between layers; column mass is untouched. On a single layer
	 *  its thickness relaxes instead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Flow Rates", meta = (ClampMin = "0.0"))
	float ThermalRelaxation = 0.05f;

	// -- Planet: stirring ---------------------------------------------------
	// Stochastic forcing against the drag, from ForcingVolume (Pipeline).

	/** Equilibrium eddy speed the stochastic stirring sustains against the drag,
	 *  as a fraction of the speed root per unit slope of the forcing noise; each
	 *  layer scales it by its EddyScale. Divergence-free. The speed reached also
	 *  scales with the forcing volume's gradient, so a volume with finer
	 *  features stirs harder. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Stirring", meta = (ClampMin = "0.0"))
	float EddySpeed = 0.15f;

	/** How long one forcing pattern lives, in turnovers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Stirring", meta = (ClampMin = "0.01", UIMin = "0.05"))
	float ForcingLifetime = 5.0f;

	/** Forcing volume tiles per deformation radius, so the stirring scales
	 *  with the eddies. The injection scale is the volume's own features
	 *  within a tile. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Stirring", meta = (ClampMin = "0.0", UIMin = "0.0001"))
	float ForcingFrequency = 0.05f;

	// -- Planet: moisture ---------------------------------------------------
	//
	// Vapour per layer, in units of the equator's surface saturation. The
	// surface evaporates into the bottom layer; rising air near saturation
	// condenses it into an advected cloud fraction, releasing latent heat, and
	// sinking air evaporates it. Rates are per turnover against vertical
	// motion normalised to (-1, 1).

	/** Relative humidity at which rising air starts to condense; it condenses
	 *  fully at saturation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Moisture", meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float CondensationOnset = 0.7f;

	/** Rate the surface moistens the bottom layer toward saturation, per
	 *  turnover. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Moisture", meta = (ClampMin = "0.0"))
	float SurfaceEvaporation = 0.2f;

	/** The bottom layer's saturation at the poles as a fraction of the
	 *  equator's, which is the vapour unit. 1 is uniform moisture. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Moisture", meta = (ClampMin = "0.0"))
	float SaturationPoleRatio = 0.25f;

	/** Share of a layer's depth moved up across the interface above it per
	 *  unit of vapour condensed: the latent heat that deepens lows under
	 *  condensing air. On a single layer it draws mass up into the layer.
	 *  PITFALL: a positive feedback; strong values run away into grid-scale
	 *  convection. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Moisture", meta = (ClampMin = "0.0"))
	float LatentHeating = 0.1f;

	/** How long cloud survives in still air before raining out, in turnovers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Moisture", meta = (ClampMin = "0.01"))
	float CloudLifetime = 30.0f;

	/** The top layer's saturation as a fraction of the bottom's; layers between
	 *  fall geometrically. Cold air aloft holds little; near the floor the upper
	 *  layers condense whatever vapour reaches them. PITFALL: at 0 condensation
	 *  forms cloud without spending vapour, and the upper layers stay overcast. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Moisture", meta = (EditCondition = "LayerCount > 1", EditConditionHides, ClampMin = "0.001", ClampMax = "1.0"))
	float UpperSaturation = 0.3f;

	/** How much the bottom layer's wind raises surface evaporation, as the gain
	 *  at the speed root: the moisture supply under a storm's own winds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Moisture", meta = (ClampMin = "0.0"))
	float WindEvaporationGain = 1.0f;

	/** How fast rising saturated air fills a column with cloud, per turnover. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Moisture", meta = (ClampMin = "0.0"))
	float CondensationRate = 0.5f;

	/** How fast sinking air evaporates cloud back into vapour, per turnover.
	 *  Also decays the storm tracer in sinking air, and with a negative
	 *  StormCellEyeDraft clears the storm cells' eyes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Moisture", meta = (ClampMin = "0.0"))
	float EvaporationRate = 0.3f;

	// -- Planet: storms -----------------------------------------------------
	//
	// An advected storm intensity, 0 to 1, grown where condensation is
	// intense and the air spins cyclonically. The deck draws it as storm cloud.

	/** Condensing ascent, W near saturation, below which nothing builds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Storms", meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float StormThreshold = 0.1f;

	/** Storm at full drive against its own decay, as odds: a column held at
	 *  full drive settles at StormAmount / (1 + StormAmount). The build rate is
	 *  this over StormLifetime, which is also the rate the cells hold their
	 *  storm up at. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Storms", meta = (ClampMin = "0.0"))
	float StormAmount = 4.0f;

	/** How long a storm lasts once its drive is gone, in turnovers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Storms", meta = (ClampMin = "0.01"))
	float StormLifetime = 10.0f;

	/** How much normalised cyclonic vorticity raises the drive. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Storms", meta = (ClampMin = "0.0"))
	float StormSpin = 2.0f;

	// -- Planet: hurricanes -------------------------------------------------
	//
	// Tracked tropical storms riding on the sim's own. Each spawns on a storm
	// with cyclonic spin in the genesis band, follows it, and fades once it is
	// gone. While alive it pushes the flow toward a vortex -- vectors tangent to
	// circles about its centre, rising from StormCellEyeStrength at the inner
	// ramp to a peak at the eyewall and falling to zero at the radius -- and
	// the flow carries the weather around it.
	//
	// PITFALL: the sim grid resolves the push and the atlas the band. Nothing
	// under about two grid cells survives: at GridResolution 128 the eyewall
	// wants to sit at least 1.4 degrees out, at 256 at least 0.7.

	/** Cells alive at once, up to FlowSimShader::MaxStormCells. Zero turns them
	 *  off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (ClampMin = "0", ClampMax = "32"))
	int32 MaxStormCells = 12;

	/** Spawn attempts per turnover, planet-wide. Each tests eight points in the
	 *  genesis band for a storm to seed on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (ClampMin = "0.0"))
	float StormCellSpawnRate = 0.4f;

	/** Equatorward edge of the band cells form in, degrees. Keep it a few
	 *  degrees off the equator, where the cells' rotation sense turns over. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float GenesisLatitudeMin = 8.0f;

	/** Poleward edge of the band cells form in, degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (ClampMin = "0.0", ClampMax = "90.0"))
	float GenesisLatitudeMax = 22.0f;

	/** Turnovers after which a cell decays whatever the conditions. It then
	 *  fades at its decay rate, StormCellGrowth times one less
	 *  StormCellPersistence, so it lives about ln(20) over that rate longer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (ClampMin = "0.01", UIMin = "0.05"))
	float StormCellLifetime = 30.0f;

	/** Storm tracer a mature cell tops its eyewall up toward, as a multiple of
	 *  the genesis storm (up to 1), on the stamp's profile, against the storm's
	 *  decay, so the storm settles a little below it. Zero leaves the storm to
	 *  the weather, so a cell lives only as long as its parent storm does. From
	 *  about 1.25 the cell keeps its own parent alive, and it ends when the
	 *  genesis window or humidity fails, or at StormCellLifetime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (ClampMin = "0.0"))
	float StormCellSustainRatio = 3.0f;

	/** Outer radius, in deformation radii, where every effect reaches zero;
	 *  the start log reports it in degrees. Held to 0.5 to 45 degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (ClampMin = "0.01"))
	float StormCellRadius = 0.7f;

	/** Where the vectors peak, as a fraction of the radius. Outside the eye. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (ClampMin = "0.01", ClampMax = "0.95"))
	float StormCellEyewall = 0.18f;

	/** Eyewall wind a mature cell holds on the bottom layer, as a fraction of
	 *  the speed root. The push is closed-loop: each step it closes part of the
	 *  gap between the flow's cyclonic wind and this profile, measured at the
	 *  eyewall and out in the band, so a cell settles here against drag and
	 *  the flow around it. A cell pushes at full strength from StormCellMaturity.
	 *  Keep it under the ceiling's knee, 0.7, less the background wind the cell
	 *  rides on; StormCellWindBreadth widens the band of peak wind.
	 *  PITFALL: far past the ceiling the push runs pinned at its limit, the clip
	 *  flattens the whole vortex to the cap, and nothing regulates it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (ClampMin = "0.0"))
	float StormCellWind = 0.6f;

	/** How strongly a cell lifts every layer's cloud toward full cover at the
	 *  eyewall, scaled by the vector ramp elsewhere. A lift on top of the
	 *  weather already there, so a hurricane always holds more cloud than its
	 *  surroundings and is the last thing cover or erosion removes. Against the
	 *  cloud's decay alone a full-intensity eyewall settles at this cover,
	 *  whatever CloudLifetime; the lift's rate follows from the two. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float StormCellCloudCover = 0.75f;

	/** Least distance between cells at spawn, in radii: 1 packs storms edge to
	 *  edge, 3 keeps them apart. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (ClampMin = "0.0", UIMin = "1.0", UIMax = "4.0"))
	float StormCellSpacing = 2.0f;

	// -- Planet: hurricane dynamics -----------------------------------------
	// How a cell forms, grows, decays and steers, and the shape of its push.

	/** Speed difference between the top and bottom layers at which the window
	 *  closes, as a multiple of ShearSpeed's magnitude (at least 0.1): shear
	 *  tears a storm apart. Under 1 the thermal shear's peak closes it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (EditCondition = "LayerCount > 1", EditConditionHides, ClampMin = "0.01"))
	float GenesisShearRatio = 2.5f;

	/** Bottom layer's relative humidity a cell needs to form, over
	 *  CondensationOnset. A live cell's favour ramps from StormCellDryTolerance
	 *  under that humidity to full at saturation, so it weakens anywhere short
	 *  of saturation and fails at the ramp's foot. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float GenesisHumidityMargin = 0.15f;

	/** How far under the genesis humidity a live cell survives: landfall and
	 *  dry intrusions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (UIMin = "0.0", UIMax = "0.5"))
	float StormCellDryTolerance = 0.15f;

	/** Storm intensity a seed needs beneath it, as a share of the storm a
	 *  column settles at under full drive, StormAmount / (1 + StormAmount). A
	 *  cell weakens once the storm within half its radius falls below it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (ClampMin = "0.0", UIMin = "0.05", UIMax = "1.0"))
	float GenesisStormRatio = 0.25f;

	/** Normalised cyclonic vorticity at which a seed counts fully; below it the
	 *  seed is weighted down. Zero ignores spin. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (ClampMin = "0.0"))
	float GenesisSpin = 0.05f;

	/** Rate intensity grows while conditions hold, per turnover. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (ClampMin = "0.0"))
	float StormCellGrowth = 0.2f;

	/** Intensity at which a cell pushes at full strength, so a young cell
	 *  spins up its parent storm while it has one: lower snaps a new storm to
	 *  full wind, higher winds it up over its growth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (ClampMin = "0.001", UIMin = "0.05", UIMax = "1.0"))
	float StormCellMaturity = 0.25f;

	/** How much of a cell outlasts its conditions: it decays at StormCellGrowth
	 *  times one less this once they fail or StormCellLifetime passes.
	 *  PITFALL: at 1 a cell never decays, and once every slot holds one none
	 *  can spawn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StormCellPersistence = 0.5f;

	/** Poleward-west drift on top of the steering flow, as a fraction of the
	 *  speed root. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (ClampMin = "0.0"))
	float StormCellDriftSpeed = 0.05f;

	/** Rate a cell is pulled toward the centre of the storm beneath it, per
	 *  turnover. Keeps it on its parent. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (ClampMin = "0.0"))
	float StormCellFollow = 0.2f;

	/** Rate a cell is pulled onto the core of the vortex in the flow, the
	 *  cyclonic vorticity peak within its eyewall, per turnover. Keeps the eye
	 *  on the centre of rotation as the vortex drifts; zero leaves the cell to
	 *  the steering flow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (ClampMin = "0.0"))
	float StormCellCoreFollow = 0.8f;

	/** How fast the vectors fall from the eyewall to the radius, as the power
	 *  of the remaining distance: 1 is linear, higher tightens the storm onto
	 *  its core. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (ClampMin = "0.1"))
	float StormCellFalloff = 1.5f;

	/** Share of the span from the eyewall to the radius over which the wind
	 *  holds its peak before StormCellFalloff takes it to zero: the breadth of
	 *  the band of strongest winds, which a storm needs to read as a
	 *  hurricane. Shapes the vortex push only; the cloud, storm and draft
	 *  still peak at the eyewall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (ClampMin = "0.0", ClampMax = "0.95"))
	float StormCellWindBreadth = 0.0f;

	/** Vector strength at StormCellEyeRatio's radius, as a fraction of the
	 *  eyewall's. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StormCellEyeStrength = 0.2f;

	/** Rate the flow relaxes toward the cell's vortex and inflow, per
	 *  turnover: higher spins a cell up faster and holds it tighter against
	 *  the flow around it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (ClampMin = "0.0"))
	float StormCellForcing = 0.15f;

	/** The top layer's share of the target wind; the bottom layer's is 1, and
	 *  those between are linear. Negative spins the top the other way, as a
	 *  storm's outflow does. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (EditCondition = "LayerCount > 1", EditConditionHides, ClampMin = "-1.0", ClampMax = "1.0"))
	float StormCellTopShare = 0.0f;

	/** Inflow on the bottom layer and outflow on the top at the eyewall, as a
	 *  fraction of the target wind: the tangent of the spiral's inflow angle
	 *  (0.2 about 11 degrees, 0.4 about 22). Closed-loop like the vortex, on
	 *  the stamp's profile; the target gives way to the vortex's under the
	 *  speed ceiling. The storm's secondary circulation: it turns what the
	 *  vortex alone winds into rings into trailing spiral bands. Zero turns it
	 *  off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (EditCondition = "LayerCount > 1", EditConditionHides, ClampMin = "0.0", ClampMax = "1.0"))
	float StormCellInflow = 0.2f;


	/** Vertical motion in the eye, in W's units, carried with the eye tracer.
	 *  Negative sinks: in the sim the cloud the flow carries through the eye
	 *  evaporates and none condenses there, at a rate EvaporationRate times
	 *  this sets, so the eye clears and the flow winds what it clears. Positive
	 *  rises and condenses cloud in the eye, which StormCellEyeDepth still
	 *  thins on the deck. On the output it adds to W either way. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float StormCellEyeDraft = -0.5f;

	/** Radius inside which the vectors fall linearly from StormCellEyeStrength
	 *  to zero at the centre, as a share of the eyewall's; between it and the
	 *  eyewall they rise to their peak. The eye itself is what the isobar
	 *  through the eyewall encloses. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Dynamics", meta = (ClampMin = "0.0", ClampMax = "0.9"))
	float StormCellEyeRatio = 0.44f;

	// -- Planet: hurricane look ---------------------------------------------
	//
	// How the cells read on the deck: storm, pressure and the eye's depth.

	/** Storm intensity raised across the whole storm, full from the centre
	 *  through the eyewall and easing to none at the radius, joined to the
	 *  sim's own by a smooth max. Storm deepens and darkens the cloud already
	 *  there without adding any, so a hurricane reads as storm throughout; with
	 *  the sim's storm tuned lower, the cells make the heaviest storm anywhere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Look", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StormCellStorm = 1.0f;

	/** Width of the smooth max joining a cell's storm to the sim's: higher
	 *  melts a hurricane into the storms around it, lower leaves a crease. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Look", meta = (ClampMin = "0.0", UIMin = "0.05", UIMax = "0.5"))
	float StormCellStormBlend = 0.25f;

	/** Pressure drop full through the eyewall and easing to none at the
	 *  radius, in the output's normalised units: the whole storm is a low. The
	 *  deck raises the lid and lowers the base under lows. Joined to the sim's
	 *  pressure before the output's soft saturation, so a deep drop rounds off
	 *  toward -1 rather than flattening into a plateau. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Look", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float StormCellPressure = 0.5f;

	/** Share of the deck's column depth a full-intensity eye removes at its
	 *  centre: 1 thins it to nothing, lower leaves a floor of cloud. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Look", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StormCellEyeDepth = 0.8f;

	/** Share of the low's eye, in radius from the eyewall inward, that its rim
	 *  ramps over on a curve with no crease at either end. Higher starts the
	 *  descent gently from the eyewall and shrinks the fully clear floor; 1
	 *  ramps all the way to the centre. Lower gives a broad clear floor inside
	 *  a steeper wall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Look", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float StormCellEyeSoftness = 0.75f;

	/** Rate the eye clears toward its source under a live cell, per turnover:
	 *  higher forms a crisp eye at once. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Look", meta = (ClampMin = "0.0"))
	float StormCellEyeRate = 3.14f;

	/** How long the eye lingers where its source has moved on, in turnovers.
	 *  The flow winds what is left, so longer draws long spiral streaks of
	 *  clear air behind a moving storm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Look", meta = (ClampMin = "0.01", UIMin = "0.2", UIMax = "5.0"))
	float StormCellEyeTrail = 1.2739f;

	/** Storm intensity the eyewall band adds over StormCellStorm, up to 1: the
	 *  band runs from 40% of peak vector strength inward to the eyewall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Look", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StormCellBandExcess = 0.0f;

	/** Vector strength, as a share of the eyewall's, where the band begins:
	 *  lower spreads it out into the spirals, higher tightens it to a ring. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Look", meta = (ClampMin = "0.0", ClampMax = "0.99", UIMin = "0.1", UIMax = "0.9"))
	float StormCellBandFloor = 0.4f;

	/** Vertical motion added with the vector strength, in W's units: rising
	 *  air makes the column towering and fills it in. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Look", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float StormCellDraft = 0.5f;

	// -- Planet: perpetual storms -------------------------------------------
	//
	// Authored storms that never die: the first cell slots, held in place. Cells
	// do not spawn within PerpetualStormClearance of one's reach, and a cell that drifts inside one
	// merges away. They share the hurricane shape and look settings.

	/** Up to FlowSimShader::MaxPerpetualStorms; entries past it are ignored.
	 *  They take the first cell slots; the hurricanes get MaxStormCells of the
	 *  rest. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Perpetual Storms", meta = (TitleProperty = "Latitude"))
	TArray<FFlowPerpetualStorm> PerpetualStorms;

	/** Rate the flow relaxes toward the perpetual storms' vortices and inflows,
	 *  per turnover: their StormCellForcing. Much lower than the stirring and
	 *  the gains pin, and the storm shows as cover without rotation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Perpetual Storms", meta = (ClampMin = "0.0"))
	float PerpetualStormForcing = 3.0f;

	/** Distance inside which no cell spawns near a perpetual storm, in
	 *  multiples of its reach. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Perpetual Storms", meta = (ClampMin = "0.0", UIMin = "0.5", UIMax = "3.0"))
	float PerpetualStormClearance = 1.5f;

	// -- Planet: noise motion -----------------------------------------------

	/** Solid-body drift the noise carries on its own, as a fraction of the
	 *  speed root: an angular rate on the unit sphere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Noise Motion")
	float NoiseDriftSpeed = 0.3f;

	/** How long a displacement accumulates before it resets, in turnovers. The
	 *  noise's warp grows with the
	 *  flow's strain times the reset time, and strain scales with the root, so
	 *  this holds the winding per reset at any speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Noise Motion", meta = (ClampMin = "0.1"))
	float NoiseResetTurnovers = 3.0f;

	// -- Time ---------------------------------------------------------------

	/** Sim time per second of real time: THE SPEED HANDLE. The step is
	 *  StepSize whatever the speed, so speed sets the steps per frame and the
	 *  cost with it, and never the look. Zero freezes the sim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Time", meta = (ClampMin = "0.0"))
	float SimSpeed = 0.0025f;

	/** Turnovers to run at the spin-up step before the sim is considered ready.
	 *  Skipped once InitialState is bound. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Time", meta = (ClampMin = "0.0", UIMax = "360.0"))
	float SpinUpTurnovers = 13.0f;

	// -- Quality ------------------------------------------------------------

	/** Edge of one face of the cube atlas the output is resampled onto, in
	 *  texels, rounded down to a power of two. The solver's grid follows: four
	 *  times it in longitude columns, twice it in latitude rows, so 64 runs at
	 *  256 x 128. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quality", meta = (ClampMin = "16", ClampMax = "512"))
	int32 GridResolution = 64;

	/** Turnovers per step: a look and cost control, independent of speed.
	 *
	 *  PITFALL: THE WEATHER DEPENDS ON THE STEP, and no conversion of the
	 *  per-step settings removes that. The semi-Lagrangian interpolation
	 *  smooths once per step, and the solver splits grid-scale gravity waves
	 *  between pressure and divergence by an amount the step sets. Larger
	 *  steps give sharper, thicker cloud. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quality", meta = (ClampMin = "0.00001", ClampMax = "0.044"))
	float StepSize = 1e-4f;

	/** Layers in the stack, 0 on top, coupled through their pressure. Two is
	 *  the smallest with baroclinic storms; one is a single shallow layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quality", meta = (ClampMin = "1", ClampMax = "8"))
	int32 LayerCount = 2;

	// -- Layers -------------------------------------------------------------

	/** Per-layer scales on the shared winds, eddies, drag and depth, resampled
	 *  over the stack: the first entry is the top layer, the last the bottom,
	 *  and layers between interpolate. A list LayerCount long maps one to one;
	 *  an empty one takes a profile's defaults. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layers")
	TArray<FFlowLayerProfile> LayerProfiles = FlowSimDefaultLayers();

	/** Relaxation of each layer's velocity toward its neighbours', per
	 *  turnover: interfacial friction. Strong coupling erodes the shear storms
	 *  grow from. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layers", meta = (EditCondition = "LayerCount > 1", EditConditionHides))
	float LayerCoupling = 0.01f;

	/** Density step at each interface, as a fraction of the surface's. Small
	 *  keeps the free surface nearly flat, so pressure systems are carried by
	 *  the interfaces. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layers", meta = (EditCondition = "LayerCount > 1", EditConditionHides, ClampMin = "0.01", ClampMax = "1.0"))
	float Stratification = 0.1f;

	// -- Numerics -----------------------------------------------------------

	/** Rate, per turnover, at which grid-scale waves of the first internal
	 *  mode decay, whatever the step: the implicit scheme's own damping at the
	 *  step, and divergence damping making up the rest. Smooths W, ripples and
	 *  bores at the grid scale; longer waves lose less, as their scale squared.
	 *  Where the implicit scheme alone damps more (a large step) that wins,
	 *  and the start log says so. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Numerics", meta = (ClampMin = "0.0"))
	float GridDamping = 2.0f;

	/** Polar longitudinal smoothing: each row is box-filtered over
	 *  FilterLatitude / cos(latitude) columns, so the filter first acts where
	 *  cos(latitude) falls below half of this (63 degrees at 0.9). Higher
	 *  filters wider and further from the poles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Numerics", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FilterLatitude = 0.9f;

	// -- Pipeline -----------------------------------------------------------
	//
	// Assets and start state: what a machine or a session owns rather than what
	// a tune is. The render targets are the subsystem's, created at run time.

	/** Band-limited tiling noise, read as a forcing streamfunction. Optional:
	 *  with none bound the forcing is exactly zero. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipeline")
	TObjectPtr<UVolumeTexture> ForcingVolume;

	/** The channel read, decoded from [0, 1] to [-1, 1]. Each channel is a
	 *  different noise octave, so the choice changes the eddies' scale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipeline", meta = (ClampMin = "0", ClampMax = "3"))
	int32 ForcingChannel = 1;

	/** A captured state to start from, restored whenever this config starts
	 *  or an atmosphere swaps to it. Empty means seed and spin up. A grid or
	 *  layout mismatch is refused and falls back to seeding. FlowSim.Save with
	 *  no argument captures into it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipeline")
	TObjectPtr<UFlowSnapshot> InitialState;

	/** Spin-up substeps per frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipeline", meta = (ClampMin = "1", UIMax = "64"))
	int32 MaxSpinUpStepsPerFrame = 8;

	// -- Debug --------------------------------------------------------------

	/** Draw the debug view, one texel per cell, into the subsystem's debug
	 *  target, which the atmosphere driving the sim shows as SimDebugView. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bDebugView = false;

	/** Field the debug view shows; r.FlowSim.DebugMode overrides it. */
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

	/** The storm intensity genesis needs: GenesisStormRatio of the storm
	 *  tracer's equilibrium under full drive. */
	float GetGenesisStorm() const
	{
		const float Amount = FMath::Max(StormAmount, 0.0f);
		return FMath::Clamp(GenesisStormRatio * Amount / (1.0f + Amount), 0.01f, 1.0f);
	}

	/** StepSize held to the solver's range, in sim time. */
	float GetStepSize() const { return FMath::Clamp(StepSize, FlowSimStep::Min, FlowSimStep::SpinUp) * GetTurnover(); }

	/** The spin-up step in sim time, and the steps SpinUpTurnovers takes. */
	float GetSpinUpStep() const { return FlowSimStep::SpinUp * GetTurnover(); }
	int32 GetSpinUpSteps() const { return FMath::CeilToInt(FMath::Max(SpinUpTurnovers, 0.0f) / FlowSimStep::SpinUp); }

	/** NoiseDriftSpeed as an angular rate, radians per unit sim time. */
	float GetNoiseDriftRate() const;

	/** NoiseResetTurnovers in sim time, two spin-up steps at least. */
	float GetNoiseResetTime() const;

	/** One turnover in sim time: DeformationRadius over the speed root. */
	float GetTurnover() const;

	/** The speed root: SpeedRoot times the first internal mode's wave
	 *  speed. */
	float GetSpeedRoot() const;

	/** Each speed times the root, the jet and shear rates that give their
	 *  profiles those peak winds, every rate and lifetime in sim time, and the
	 *  lengths from DeformationRadius. */
	FFlowSimScales ResolveScales() const;

	/** ImplicitWeight at a step: at least FlowSimStep::StackWeight for a stack
	 *  at a large step. */
	float GetImplicitWeight(float Step) const;

	/** Rate the implicit scheme alone damps the first internal mode's
	 *  grid-scale waves at a step, per unit sim time. */
	float GetImplicitDampingRate(float Step) const;

	/** Fraction of grid-scale divergence the divergence damping removes per
	 *  step: GridDamping less the implicit scheme's share, as a fraction of
	 *  that step. */
	float GetDivergenceDamping(float Step) const;

	virtual void Serialize(FArchive& Ar) override;

	/** Warns when the asset was saved at an older config version, whose values
	 *  load under the current meanings; nothing is converted. */
	virtual void PostLoad() override;
};

/** The zonal profiles on the CPU, mirroring FlowSim.usf, for the speed root and
 *  the start log. */
namespace FlowSimProfile
{
	/** A layer's jets: SimThreeCellRate or SimBandedRate at a strength and boost. */
	float JetRate(const UFlowSimConfig& Config, float Mu, float Strength, float Boost);

	/** SimJetLatitudeScale: 45 degrees over JetLatitude. */
	float JetLatitudeScale(const UFlowSimConfig& Config);

	/** SimJetShape: the banded shape's irregularity, harmonic, normalisation
	 *  and equatorial Gaussian's coefficient. */
	FVector4f JetShape(const UFlowSimConfig& Config);

	/** SimJetForm: the saturation's knee and width, and the three cells'
	 *  trade and polar strengths. */
	FVector4f JetForm(const UFlowSimConfig& Config);

	/** SimThermalShape: the thermal shear's latitude shape, peaking at 1. */
	float ThermalShape(const UFlowSimConfig& Config, float Mu);

	/** Wave speed of the first internal mode, from the deformation radius at
	 *  45 degrees. */
	float WaveSpeed(const UFlowSimConfig& Config);
}

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
 *  thread. UFlowSimSubsystem::BuildParams sets every member, so the defaults
 *  are zero rather than a second, stale set of values. */
struct FFlowSimParams
{
	FIntVector GridSize = FIntVector::ZeroValue;

	FVector4f JetParams = FVector4f::Zero();
	int32 ZonalProfile = 0;
	float WidthBias = 0.0f;

	/** 45 degrees over JetLatitude. */
	float JetLatitudeScale = 0.0f;
	FVector4f JetShape = FVector4f::Zero();
	FVector4f JetForm = FVector4f::Zero();
	FVector4f LayerProfile[8] = {
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(),
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero() };

	/** Per layer: x depth, y the Helmholtz scale of the mode in its slice,
	 *  z saturation factor; w unused. */
	FVector4f LayerState[8] = {
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(),
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero() };

	FFlowSimStack Stack;

	float DeltaTime = 0.0f;

	/** One turnover in sim time: the unit the eye tracer's rates are in. */
	float Turnover = 0.0f;

	/** Sim time at the start of the step, in double precision; the shader gets
	 *  it wrapped by each clock's period. */
	double Time = 0.0;

	/** The step clock: step AnchorStep fell at AnchorTime, and each since is
	 *  DeltaTime on. A step's time depends on its index alone, not on how
	 *  frames grouped the steps, which replay needs. */
	double AnchorTime = 0.0;
	int32 AnchorStep = 0;

	/** Sim time at the start of step Index. */
	double TimeAt(int32 Index) const
	{
		return AnchorTime + (double)(Index - AnchorStep) * (double)DeltaTime;
	}
	float PlanetaryVorticity = 0.0f;
	float ImplicitWeight = 0.0f;

	int32 ForcingChannel = 0;

	float NudgeRate = 0.0f;
	float ForcingAmplitude = 0.0f;
	float ForcingFrequency = 0.0f;
	float ForcingLifetime = 0.0f;
	float DragRate = 0.0f;
	float LayerCoupling = 0.0f;
	/** Fraction of grid-scale divergence removed per step. */
	float DivergenceDamping = 0.0f;
	float FroudeCeiling = 0.0f;
	float ShockDamping = 0.0f;

	float ThermalRelaxation = 0.0f;

	/** x shear, y shape (0 midlatitude, 1 jets), z zone latitude, w zone
	 *  half-width, radians. */
	FVector4f ThermalParams = FVector4f::Zero();

	float CondensationRate = 0.0f;
	float EvaporationRate = 0.0f;
	float CloudLifetime = 0.0f;

	/** x unused, y saturation at the poles as a fraction of the equator's,
	 *  z condensation onset, w surface evaporation. */
	FVector4f MoistureParams = FVector4f::Zero();
	float WindEvaporation = 0.0f;
	float LatentHeating = 0.0f;
	float AscentSmoothing = 0.0f;

	/** x rate, y threshold, z spin, w decay rate. */
	FVector4f StormParams = FVector4f::Zero();

	/** See SimCellShape through SimCellGenesis in FlowSim.usf. */
	FVector4f CellShape = FVector4f::Zero();
	FVector4f CellVortex = FVector4f::Zero();
	FVector4f CellDraft = FVector4f::Zero();
	FVector4f CellLife = FVector4f::Zero();
	FVector4f CellMotion = FVector4f::Zero();
	FVector4f CellGenesis = FVector4f::Zero();
	FVector4f CellCloud = FVector4f::Zero();

	/** Share of the span past the eyewall the vortex's wind holds its peak. */
	float CellWindBreadth = 0.0f;

	/** Storm tracer a mature cell holds its eyewall at, at least. */
	float CellSustain = 0.0f;

	/** Share of the deck's depth a full-intensity eye removes. */
	float CellEyeDepth = 0.0f;

	/** Rate a cell is pulled onto its vortex's core. */
	float CellCoreFollow = 0.0f;

	/** Share of the low's eye its rim ramps over. */
	float CellEyeSoftness = 0.0f;

	/** The band's floor and the storm blend's width; the eye tracer's rise and
	 *  decay, per turnover; the intensity of full push; the spawn spacing, in
	 *  radii; the dry tolerance; and the perpetual storms' clearance, in reaches. */
	float CellBandFloor = 0.0f;
	float CellStormBlend = 0.0f;
	float CellEyeRate = 0.0f;
	float CellEyeDecay = 0.0f;
	float CellMaturity = 0.0f;
	float CellSpacing = 0.0f;
	float CellDryTolerance = 0.0f;
	float PerpetualClearance = 0.0f;
	int32 CellCount = 0;

	/** Steps completed before the frame's first; seeds the cells' spawns. */
	int32 StepIndex = 0;

	/** Perpetual storms, up to FlowSimShader::MaxPerpetualStorms: see
	 *  SimPerpetualShape, SimPerpetualLook and SimPerpetualForm in FlowSim.usf, with Shape's y
	 *  the longitude at time zero; PerpetualRate is each longitude's angular
	 *  rate. PerpetualTime is the time they are placed at: the time a step
	 *  reaches, or the output's outside the steps. */
	int32 PerpetualCount = 0;
	float PerpetualForcing = 0.0f;
	FVector4f PerpetualShape[8] = { FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(),
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero() };
	FVector4f PerpetualLook[8] = { FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(),
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero() };
	FVector4f PerpetualForm[8] = { FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(),
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero() };
	double PerpetualRate[8] = {};
	double PerpetualTime = 0.0;

	/** Radians per unit sim time, and sim time. */
	float NoiseDriftRate = 0.0f;
	float NoiseResetTime = 0.0f;


	float FilterLatitude = 0.0f;

	/** Where the output sits between the state before the frame's last step
	 *  (0) and after it (1). */
	float StateBlend = 1.0f;

	/** Output normalisation: x pressure, y vorticity, z divergence. */
	FVector3f OutputScales = FVector3f::ZeroVector;

	/** Face edge of the cube atlas FlowTexture holds, in texels. */
	int32 AtlasFaceSize = 0;

	int32 DebugMode = 0;
	int32 DebugLayer = 0;
	float DebugScale = 0.0f;
	FIntPoint DebugSize = FIntPoint::ZeroValue;

	/** Resources on the game thread, resolved to RHI handles by
	 *  ResolveTextures_RenderThread: a handle read on the game thread can be one
	 *  the render thread is replacing. A null forcing volume is legal and
	 *  evaluates as zero. */
	FTextureResource* ForcingResource = nullptr;
	FTextureRenderTargetResource* FlowResource = nullptr;
	FTextureRenderTargetResource* DebugResource = nullptr;

	/** Render thread only. */
	FTextureRHIRef ForcingTexture;
	FTextureRHIRef FlowTexture;
	FTextureRHIRef DebugTexture;

	void ResolveTextures_RenderThread();
};
