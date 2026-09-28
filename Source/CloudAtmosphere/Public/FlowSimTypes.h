#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "FlowSimTypes.generated.h"

class FTextureResource;
class FTextureRenderTargetResource;
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
	 *  peaking at 45, polar easterlies past about 65. BandCount, EquatorialBoost,
	 *  Asymmetry and WidthBias are unused. */
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

	/** Scales EquatorialBoost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float BoostScale = 1.0f;

	/** This layer's equilibrium eddy speed as a multiple of EddySpeed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer", meta = (ClampMin = "0.0"))
	float EddyScale = 1.0f;

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
UCLASS(BlueprintType, meta = (PrioritizeCategories = "Planet Time Quality Layers Rates Numerics Pipeline Debug"))
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds")
	EFlowZonalProfile ZonalProfile = EFlowZonalProfile::Banded;

	/** Latitude of the three cells' westerly jet, degrees. The profile stretches
	 *  in latitude about the equator with it, so the trades and the polar
	 *  easterlies move in proportion; past about 53 the polar easterlies leave
	 *  the pole. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ZonalProfile == EFlowZonalProfile::ThreeCell", EditConditionHides, ClampMin = "15.0", ClampMax = "75.0"))
	float JetLatitude = 45.0f;

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

	/** Equilibrium eddy speed the stochastic stirring sustains against the drag,
	 *  as a fraction of the speed root per unit slope of the forcing noise; each
	 *  layer scales it by its EddyScale. Divergence-free. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (ClampMin = "0.0"))
	float EddySpeed = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ZonalProfile == EFlowZonalProfile::Banded", EditConditionHides, ClampMin = "1.0"))
	float BandCount = 3.0f;

	/** Centre of the midlatitude zone, degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ThermalShape == EFlowThermalShape::Midlatitude", EditConditionHides, ClampMin = "0.0", ClampMax = "90.0"))
	float BaroclinicLatitude = 45.0f;

	/** Half-width of the midlatitude zone, degrees. Storms need it to span a
	 *  few deformation radii. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Winds", meta = (EditCondition = "ThermalShape == EFlowThermalShape::Midlatitude", EditConditionHides, ClampMin = "1.0", ClampMax = "90.0"))
	float BaroclinicWidth = 24.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Winds", meta = (EditCondition = "ZonalProfile == EFlowZonalProfile::Banded", EditConditionHides))
	float EquatorialBoost = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Winds", meta = (EditCondition = "ZonalProfile == EFlowZonalProfile::Banded", EditConditionHides))
	float Asymmetry = 0.5f;

	/** Positive widens the prograde zones. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Winds", meta = (EditCondition = "ZonalProfile == EFlowZonalProfile::Banded", EditConditionHides))
	float WidthBias = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Winds")
	EFlowThermalShape ThermalShape = EFlowThermalShape::Midlatitude;

	/** 2 * Omega. Sets the beta effect that arrests the inverse cascade into
	 *  jets, and with DeformationRadius the wave speed the speed root scales.
	 *  Every rate and lifetime is in turnovers, which shorten as it rises, so
	 *  beyond the Coriolis step it sets the tempo alone, as SimSpeed does.
	 *
	 *  PITFALL: also the explicit Coriolis step. Above about 0.5 radians of
	 *  rotation per step the split between explicit rotation and implicit
	 *  pressure radiates gravity waves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Winds", meta = (ClampMin = "0.1"))
	float PlanetaryVorticity = 24.0f;

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
	 *  fall geometrically. Cold air aloft holds little. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Moisture", meta = (EditCondition = "LayerCount > 1", EditConditionHides, ClampMin = "0.0", ClampMax = "1.0"))
	float UpperSaturation = 0.3f;

	/** How much the bottom layer's wind raises surface evaporation, as the gain
	 *  at the speed root: the moisture supply under a storm's own winds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Moisture", meta = (ClampMin = "0.0"))
	float WindEvaporationGain = 1.0f;

	/** How fast rising saturated air fills a column with cloud, per turnover. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Moisture", meta = (ClampMin = "0.0"))
	float CondensationRate = 0.5f;

	/** How fast sinking air evaporates cloud back into vapour, per turnover. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Moisture", meta = (ClampMin = "0.0"))
	float EvaporationRate = 0.3f;

	/** Time constant, in turnovers, of the low-pass the vertical motion
	 *  takes along the flow before anything reads it: condensation, latent heat,
	 *  the deck. What moves with the air holds; gravity waves and bores, which
	 *  move through it, average out instead of condensing cloud along their
	 *  crests and drawing travelling lines into the cloud field. Longer is
	 *  cleaner and makes cloud respond more slowly to new ascent; condensation
	 *  from passing waves goes too, so cloud amount falls (about half at 1.5)
	 *  and the deck's cover wants raising to match. 0 reads it raw. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Moisture", meta = (ClampMin = "0.0", ClampMax = "25.0"))
	float AscentSmoothing = 3.0f;

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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Storms", meta = (ClampMin = "0.0"))
	float StormAmount = 4.0f;

	/** How long a storm lasts once its drive is gone, in turnovers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Storms", meta = (ClampMin = "0.01"))
	float StormLifetime = 10.0f;

	/** How much normalised cyclonic vorticity raises the drive. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Storms", meta = (ClampMin = "0.0"))
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0"))
	float StormCellSpawnRate = 0.4f;

	/** Latitudes, degrees, between which cells form. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0", ClampMax = "90.0"))
	float GenesisLatitudeMin = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0", ClampMax = "90.0"))
	float GenesisLatitudeMax = 22.0f;

	/** Turnovers after which a cell decays whatever the conditions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.05"))
	float StormCellLifetime = 30.0f;

	/** Storm tracer a mature cell tops its eyewall up toward, as a multiple of
	 *  GenesisStorm (up to 1), on the stamp's profile, against the storm's
	 *  decay, so the storm settles a little below it. Zero leaves the storm to
	 *  the weather, so a cell lives only as long as its parent storm does. From
	 *  about 1.25 the cell keeps its own parent alive, and it ends when the
	 *  genesis window or humidity fails, or at StormCellLifetime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0"))
	float StormCellSustainRatio = 0.0f;

	/** Outer radius, in deformation radii, where every effect reaches zero;
	 *  the start log reports it in degrees. Held to 0.5 to 45 degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.01"))
	float StormCellRadius = 0.7f;

	/** Where the vectors peak, as a fraction of the radius. Outside the eye. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.01", ClampMax = "0.95"))
	float StormCellEyewall = 0.18f;

	/** Eyewall wind a mature cell holds on the bottom layer, as a fraction of
	 *  the speed root. The push is closed-loop: each step it closes part of the
	 *  gap between the flow's cyclonic wind and this profile, measured at the
	 *  eyewall and out in the band, so a cell settles here against drag and
	 *  the flow around it. A cell pushes at full strength from intensity 0.25.
	 *  Keep it under the ceiling's knee, 0.7, less the background wind the cell
	 *  rides on; StormCellWindBreadth widens the band of peak wind.
	 *  PITFALL: far past the ceiling the push runs pinned at its limit, the clip
	 *  flattens the whole vortex to the cap, and nothing regulates it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0"))
	float StormCellSpeed = 0.6f;

	/** How strongly a cell lifts every layer's cloud toward full cover at the
	 *  eyewall, scaled by the vector ramp elsewhere. A lift on top of the
	 *  weather already there, so a hurricane always holds more cloud than its
	 *  surroundings and is the last thing cover or erosion removes. Against the
	 *  cloud's decay alone a full-intensity eyewall settles at this cover,
	 *  whatever CloudLifetime; the lift's rate follows from the two. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0", ClampMax = "0.99"))
	float StormCellCloudCover = 0.75f;

	/** Speed difference between the top and bottom layers at which the window
	 *  closes, as a multiple of ShearSpeed's magnitude (at least 0.1): shear
	 *  tears a storm apart. Under 1 the thermal shear's peak closes it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0 && LayerCount > 1", EditConditionHides, ClampMin = "0.01"))
	float GenesisShearRatio = 2.5f;

	/** Bottom layer's relative humidity a cell needs to form, over
	 *  CondensationOnset; it weakens below 0.15 under that. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "-1.0", ClampMax = "1.0"))
	float GenesisHumidityMargin = 0.15f;

	/** Storm intensity a seed needs beneath it. A cell weakens once the storm
	 *  within half its radius falls below this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.01", ClampMax = "1.0"))
	float GenesisStorm = 0.2f;

	/** Normalised cyclonic vorticity at which a seed counts fully; below it the
	 *  seed is weighted down. Zero ignores spin. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0"))
	float GenesisSpin = 0.05f;

	/** Rate intensity grows while conditions hold, per turnover. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0"))
	float StormCellGrowth = 0.2f;

	/** How much of a cell outlasts its conditions: it decays at StormCellGrowth
	 *  times one less this once they fail or StormCellLifetime passes.
	 *  PITFALL: at 1 a cell never decays, and once every slot holds one none
	 *  can spawn. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0", ClampMax = "1.0"))
	float StormCellPersistence = 0.5f;

	/** Poleward-west drift on top of the steering flow, as a fraction of the
	 *  speed root. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0"))
	float StormCellDriftSpeed = 0.05f;

	/** Rate a cell is pulled toward the centre of the storm beneath it, per
	 *  turnover. Keeps it on its parent. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0"))
	float StormCellFollow = 0.2f;

	/** Rate a cell is pulled onto the core of the vortex in the flow, the
	 *  cyclonic vorticity peak within its eyewall, per turnover. Keeps the eye
	 *  on the centre of rotation as the vortex drifts; zero leaves the cell to
	 *  the steering flow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0"))
	float StormCellCoreFollow = 0.8f;

	/** How fast the vectors fall from the eyewall to the radius, as the power
	 *  of the remaining distance: 1 is linear, higher tightens the storm onto
	 *  its core. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.1"))
	float StormCellFalloff = 1.5f;

	/** Share of the span from the eyewall to the radius over which the wind
	 *  holds its peak before StormCellFalloff takes it to zero: the breadth of
	 *  the band of strongest winds, which a storm needs to read as a
	 *  hurricane. Shapes the vortex push only; the cloud, storm and draft
	 *  still peak at the eyewall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0", ClampMax = "0.95"))
	float StormCellWindBreadth = 0.0f;

	/** Vector strength at StormCellEyeRatio's radius, as a fraction of the
	 *  eyewall's. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0", ClampMax = "1.0"))
	float StormCellEyeStrength = 0.2f;

	/** Rate the flow relaxes toward the cell's vortex and inflow, per
	 *  turnover: higher spins a cell up faster and holds it tighter against
	 *  the flow around it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0"))
	float StormCellForcing = 0.15f;

	/** The top layer's share of the target wind; the bottom layer's is 1, and
	 *  those between are linear. Negative spins the top the other way, as a
	 *  storm's outflow does. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0 && LayerCount > 1", EditConditionHides, ClampMin = "-1.0", ClampMax = "1.0"))
	float StormCellTopShare = 0.0f;

	/** Inflow on the bottom layer and outflow on the top at the eyewall, as a
	 *  fraction of the target wind: the tangent of the spiral's inflow angle
	 *  (0.2 about 11 degrees, 0.4 about 22). Closed-loop like the vortex, on
	 *  the stamp's profile; the target gives way to the vortex's under the
	 *  speed ceiling. The storm's secondary circulation: it turns what the
	 *  vortex alone winds into rings into trailing spiral bands. Zero turns it
	 *  off. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0 && LayerCount > 1", EditConditionHides, ClampMin = "0.0", ClampMax = "1.0"))
	float StormCellInflow = 0.2f;


	/** Vertical motion in the eye, in W's units, carried with the eye tracer.
	 *  Negative sinks: in the sim the cloud the flow carries through the eye
	 *  evaporates and none condenses there, at a rate EvaporationRate times
	 *  this sets, so the eye clears and the flow winds what it clears; on the
	 *  output it adds to W. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "-1.0", ClampMax = "1.0"))
	float StormCellEyeDraft = -0.5f;

	/** Radius inside which the vectors fall linearly from StormCellEyeStrength
	 *  to zero at the centre, as a share of the eyewall's; between it and the
	 *  eyewall they rise to their peak. The eye itself is what the isobar
	 *  through the eyewall encloses. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricanes", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0", ClampMax = "0.9"))
	float StormCellEyeRatio = 0.44f;

	// -- Planet: hurricane look ---------------------------------------------
	//
	// How the cells read on the deck: storm, pressure and the eye's depth.

	/** Storm intensity raised across the whole storm, full from the centre
	 *  through the eyewall and easing to none at the radius, joined to the
	 *  sim's own by a smooth max. Storm deepens and darkens the cloud already
	 *  there without adding any, so a hurricane reads as storm throughout; with
	 *  the sim's storm tuned lower, the cells make the heaviest storm anywhere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Look", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0", ClampMax = "1.0"))
	float StormCellStorm = 1.0f;

	/** Pressure drop full through the eyewall and easing to none at the
	 *  radius, in the output's normalised units: the whole storm is a low. The
	 *  deck raises the lid and lowers the base under lows. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Look", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0", ClampMax = "2.0"))
	float StormCellPressure = 0.5f;

	/** Share of the deck's column depth a full-intensity eye removes at its
	 *  centre: 1 thins it to nothing, lower leaves a floor of cloud. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Look", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0", ClampMax = "1.0"))
	float StormCellEyeDepth = 0.8f;

	/** Share of the low's eye, in radius from the eyewall inward, that its rim
	 *  ramps over on a curve with no crease at either end. Higher starts the
	 *  descent gently from the eyewall and shrinks the fully clear floor; 1
	 *  ramps all the way to the centre. Lower gives a broad clear floor inside
	 *  a steeper wall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Planet|Hurricane Look", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.05", ClampMax = "1.0"))
	float StormCellEyeSoftness = 0.75f;

	/** Storm intensity the eyewall band adds over StormCellStorm, up to 1: the
	 *  band runs from 40% of peak vector strength inward to the eyewall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricane Look", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "0.0", ClampMax = "1.0"))
	float StormCellBandExcess = 0.0f;

	/** Vertical motion added with the vector strength, in W's units: rising
	 *  air makes the column towering and fills it in. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Hurricane Look", meta = (EditCondition = "MaxStormCells > 0", EditConditionHides, ClampMin = "-1.0", ClampMax = "1.0"))
	float StormCellDraft = 0.5f;

	// -- Planet: noise motion -----------------------------------------------

	/** Solid-body drift the noise carries on its own, as a fraction of the
	 *  speed root: an angular rate on the unit sphere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Noise Motion")
	float NoiseDriftSpeed = 0.3f;

	/** How long a displacement accumulates before it resets, in turnovers. The
	 *  noise's warp grows with the
	 *  flow's strain times the reset time, and strain scales with the root, so
	 *  this holds the winding per reset at any speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Planet|Noise Motion", meta = (ClampMin = "0.05"))
	float NoiseResetTurnovers = 3.0f;

	// -- Time ---------------------------------------------------------------

	/** Sim time per second of real time: THE SPEED HANDLE. The step is
	 *  StepSize whatever the speed, so speed sets the steps per frame and the
	 *  cost with it, and never the look. Zero freezes the sim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Time", meta = (ClampMin = "0.0"))
	float SimSpeed = 0.0025f;

	/** Substeps to run before the sim is considered ready. Skipped once
	 *  InitialState is bound. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Time", meta = (ClampMin = "0", ClampMax = "8192"))
	int32 SpinUpSteps = 300;

	// -- Quality ------------------------------------------------------------

	/** Edge of one face of the cube atlas the output is resampled onto, in
	 *  texels, rounded down to a power of two. The solver's grid follows: four
	 *  times it in longitude columns, twice it in latitude rows, so 64 runs at
	 *  256 x 128. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quality", meta = (ClampMin = "16", ClampMax = "512"))
	int32 GridResolution = 64;

	/** Sim time per step: a look and cost control, independent of speed.
	 *
	 *  PITFALL: THE WEATHER DEPENDS ON THE STEP, and no conversion of the
	 *  per-step settings removes that. The semi-Lagrangian interpolation
	 *  smooths once per step, and the solver splits grid-scale gravity waves
	 *  between pressure and divergence by an amount the step sets. Larger
	 *  steps give sharper, thicker cloud. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quality", meta = (ClampMin = "0.000001", ClampMax = "0.0086"))
	float StepSize = 1e-5f;

	/** Layers in the stack, 0 on top, coupled through their pressure. Two is
	 *  the smallest with baroclinic storms; one is a single shallow layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Quality", meta = (ClampMin = "1", ClampMax = "8"))
	int32 LayerCount = 2;

	// -- Layers -------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Layers")
	TArray<FFlowLayerProfile> LayerProfiles;

	/** Relaxation of each layer's velocity toward its neighbours', per
	 *  turnover: interfacial friction. Strong coupling erodes the shear storms
	 *  grow from. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Layers", meta = (EditCondition = "LayerCount > 1", EditConditionHides))
	float LayerCoupling = 0.01f;

	/** Density step at each interface, as a fraction of the surface's. Small
	 *  keeps the free surface nearly flat, so pressure systems are carried by
	 *  the interfaces. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Layers", meta = (EditCondition = "LayerCount > 1", EditConditionHides, ClampMin = "0.01", ClampMax = "1.0"))
	float Stratification = 0.1f;

	// -- Rates --------------------------------------------------------------
	//
	// EVERY RATE IS PER TURNOVER AND EVERY LIFETIME IN TURNOVERS: the time one
	// deformation radius takes to cross at the speed root. The speed root and
	// PlanetaryVorticity then change the tempo without changing how far
	// anything travels in its life.

	/** Relaxation of each layer's ZONAL-MEAN eastward velocity toward its
	 *  profile, per turnover. PITFALL: every nudge is an unbalanced push that
	 *  the flow answers with gravity waves, so strong nudging reads as
	 *  ripples. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Rates")
	float NudgeRate = 0.1f;

	/** Linear drag on the eddy part of the eastward velocity and all of the
	 *  northward, per turnover, scaled per layer. The energy sink that arrests
	 *  the cascade, and what turns flow into lows and out of highs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Rates")
	float DragRate = 0.15f;

	/** Rate interfaces relax toward their balanced heights, per turnover, by
	 *  moving mass between layers; column mass is untouched. On a single layer
	 *  its thickness relaxes instead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Rates", meta = (ClampMin = "0.0"))
	float ThermalRelaxation = 0.05f;

	/** How long one forcing pattern lives, in turnovers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Rates", meta = (ClampMin = "0.05"))
	float ForcingLifetime = 5.0f;

	/** Forcing volume tiles per deformation radius, so the stirring scales
	 *  with the eddies. The injection scale is the volume's own features
	 *  within a tile. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "Rates", meta = (ClampMin = "0.0001"))
	float ForcingFrequency = 0.05f;

	// -- Numerics -----------------------------------------------------------

	/** Weight of the implicit half of the gravity-wave terms. 0.5 is neutral;
	 *  above it gravity waves are damped, by an amount that grows with the
	 *  step, and that damping counts toward GridDamping. A stack runs at least
	 *  FlowSimStep::StackWeight at large steps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Numerics", meta = (ClampMin = "0.5", ClampMax = "1.0"))
	float ImplicitWeight = 0.6f;

	/** Rate, per turnover, at which grid-scale waves of the first internal
	 *  mode decay, whatever the step: the implicit scheme's own damping at the
	 *  step, and divergence damping making up the rest. Smooths W, ripples and
	 *  bores at the grid scale; longer waves lose less, as their scale squared.
	 *  Where the implicit scheme alone damps more (a large step, a high
	 *  ImplicitWeight) that wins, and the start log says so. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Numerics", meta = (ClampMin = "0.0"))
	float GridDamping = 2.0f;

	/** Gain of the compression-activated divergence damping: where a front
	 *  steepens, the grid-scale divergence a step removes grows by this times
	 *  the local compression per step. Higher widens and softens travelling
	 *  fronts more; 0 leaves only GridDamping. The total is capped at
	 *  the explicit scheme's stability bound, so any value is stable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Numerics", meta = (ClampMin = "0.0", ClampMax = "50.0"))
	float ShockDamping = 2.0f;

	/** Rebuild centre velocities to fourth order from the faces rather than as
	 *  a two-face average. Keeps a compact vortex from bleeding into a cross
	 *  along the grid axes. PITFALL: it also damps fast motion far less --
	 *  about twice the eddy speed, more W and more cloud at the same settings --
	 *  so the deck's coverage tuning does not carry over, and the faster flow
	 *  pulls the two noise phases apart until their crossfade reads as density
	 *  sliding under the clouds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Numerics")
	bool bSharpCentreVelocity = false;

	/** cos(latitude) below which the longitudinal filter engages. Its width
	 *  follows from the grid: FilterLatitude / cos(latitude) texels. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Numerics", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FilterLatitude = 0.9f;

	// -- Pipeline -----------------------------------------------------------
	//
	// Assets, targets and start state: what a machine or a session owns rather
	// than what a tune is.

	/** Band-limited tiling noise, read as a forcing streamfunction. Optional:
	 *  with none bound the forcing is exactly zero. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipeline")
	TObjectPtr<UVolumeTexture> ForcingVolume;

	/** The channel read, decoded from [0, 1] to [-1, 1]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipeline", meta = (ClampMin = "0", ClampMax = "3"))
	int32 ForcingChannel = 1;

	/** A captured state to start from. Empty means seed and spin up. A grid or
	 *  layout mismatch is refused and falls back to seeding. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipeline")
	TObjectPtr<UFlowSnapshot> InitialState;

	/** RGBA16F 2D array, the cube atlas with 4 * LayerCount slices; see
	 *  FlowField.ush. This is what the material samples. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipeline")
	TObjectPtr<UTextureRenderTarget2DArray> FlowTarget;

	/** Reconfigure the targets to match the grid if they do not already. A
	 *  mismatched target is refused, and a refused sim looks exactly like one
	 *  that runs and produces nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipeline")
	bool bAutoResizeTargets = true;

	/** Spin-up substeps per frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Pipeline", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxSpinUpStepsPerFrame = 8;

	// -- Debug --------------------------------------------------------------

	/** Any 2D render target. Sized to the grid it is one texel per cell. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	TObjectPtr<UTextureRenderTarget2D> DebugTarget;

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

	/** StepSize held to the solver's range. */
	float GetStepSize() const { return FMath::Clamp(StepSize, FlowSimStep::Min, FlowSimStep::SpinUp); }

	/** NoiseDriftSpeed as an angular rate, radians per unit sim time. */
	float GetNoiseDriftRate() const;

	/** NoiseResetTurnovers in sim time. */
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
	/** A layer's jets: SimThreeCellRate or GG_ZonalRate at a strength and boost. */
	float JetRate(const UFlowSimConfig& Config, float Mu, float Strength, float Boost);

	/** SimJetLatitudeScale: 45 degrees over JetLatitude. */
	float JetLatitudeScale(const UFlowSimConfig& Config);

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
 *  thread. */
struct FFlowSimParams
{
	FIntVector GridSize = FIntVector(512, 256, 2);

	FVector4f JetParams = FVector4f(3.0f, 1.0f, 0.5f, 0.5f);
	int32 ZonalProfile = 0;
	float WidthBias = 0.0f;

	/** 45 degrees over JetLatitude. */
	float JetLatitudeScale = 1.0f;
	FVector4f LayerProfile[8] = {
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(),
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero() };

	/** Per layer: x depth, y the Helmholtz scale of the mode in its slice,
	 *  z saturation factor; w unused. */
	FVector4f LayerState[8] = {
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(),
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero() };

	FFlowSimStack Stack;

	float DeltaTime = 0.0086f;

	/** Sim time at the start of the step, in double precision; the shader gets
	 *  it wrapped by each clock's period. */
	double Time = 0.0;
	float PlanetaryVorticity = 24.0f;
	float ImplicitWeight = 0.6f;

	int32 ForcingChannel = 1;

	float NudgeRate = 1.0f;
	float ForcingAmplitude = 0.3f;
	float ForcingFrequency = 0.25f;
	float ForcingLifetime = 0.5f;
	float DragRate = 1.5f;
	float LayerCoupling = 0.1f;
	/** Fraction of grid-scale divergence removed per step. */
	float DivergenceDamping = 0.0f;
	float FroudeCeiling = 0.6f;
	float ShockDamping = 2.0f;
	bool bSharpCentreVelocity = false;

	float ThermalRelaxation = 0.5f;

	/** x shear, y shape (0 midlatitude, 1 jets), z zone latitude, w zone
	 *  half-width, radians. */
	FVector4f ThermalParams = FVector4f::Zero();

	float CondensationRate = 5.0f;
	float EvaporationRate = 3.0f;
	float CloudLifetime = 3.0f;

	/** x unused, y saturation at the poles as a fraction of the equator's,
	 *  z condensation onset, w surface evaporation. */
	FVector4f MoistureParams = FVector4f(0.0f, 0.25f, 0.7f, 2.0f);
	float WindEvaporation = 1.0f;
	float LatentHeating = 0.1f;
	float AscentSmoothing = 0.3f;

	/** x rate, y threshold, z spin, w decay rate. */
	FVector4f StormParams = FVector4f(4.0f, 0.1f, 2.0f, 1.0f);

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
	float CellEyeDepth = 0.8f;

	/** Rate a cell is pulled onto its vortex's core. */
	float CellCoreFollow = 8.0f;

	/** Share of the low's eye its rim ramps over. */
	float CellEyeSoftness = 0.75f;
	int32 CellCount = 0;

	/** Steps completed before the frame's first; seeds the cells' spawns. */
	int32 StepIndex = 0;

	/** Radians per unit sim time, and sim time. */
	float NoiseDriftRate = 0.0f;
	float NoiseResetTime = 1.0f;


	float FilterLatitude = 0.9f;

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
