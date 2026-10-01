// The parameter sets the atmosphere's passes are driven by, and the
// derivations that keep them consistent.
//
// SHARED STRUCTS, SEPARATE INSTANCES. Both models run one cloud field, so they
// share every group's member list, but an actor carries one instance of each
// group per model: a type change swaps the whole authored set rather than
// reinterpreting one, and a terrestrial tune and a gas giant tune sit side by
// side. Only the pipeline groups (Simulation, Raymarch) are one instance, and
// Simulation holds a config per model.
//
// RATIOS, NOT ABSOLUTES, WHEREVER ONE VALUE IS BOUNDED BY ANOTHER. A parameter
// expressed against the thing that constrains it stays valid when that thing is
// retuned; expressed absolutely it silently goes out of range, and the failure
// shows up as a geometry or sampling artifact rather than as a bad value.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "AtmosphereParams.generated.h"

class UFlowSimConfig;

class UVolumeTexture;

/** Which cloud field the march samples. An enum rather than a bool: adding a
 *  case fails loudly at every switch, where a bool silently takes the false
 *  branch. */
UENUM(BlueprintType)
enum class EPlanetAtmosphereType : uint8
{
	Terrestrial,
	GasGiant
};

/** The flow simulation the cloud field reads as its weather map, a config per
 *  model. ONE SIM PER WORLD: the claiming planet nearest the camera drives it
 *  with its active model's config, restored from that config's InitialState,
 *  and the others draw the field they kept when they last drove it. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereSimulationParams
{
	GENERATED_BODY()

	/** The terrestrial model's sim: its settings and, as InitialState, the
	 *  snapshot a swap to the model restores. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<UFlowSimConfig> TerrestrialConfig = nullptr;

	/** The gas giant's. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<UFlowSimConfig> GasGiantConfig = nullptr;

	/** Bid for the sim every tick. Off, the planet never drives it and draws
	 *  the field it last kept, or none. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bClaimSimulation = true;
};

/** Cloud and deck shadows falling on whatever opaque geometry the depth buffer
 *  holds: terrain, meshes, a mesh inner surface, other actors.
 *
 *  A READ-SIDE FEATURE ENTIRELY. The map's optical depth is valid anywhere
 *  inside the shell, so a point on terrain reads the whole column above it as a
 *  deck sample reads its own. Nothing here changes the bake and no pass is
 *  added; the march evaluates it once, where the view ray stopped.
 *
 *  ONE STRUCT, ONE INSTANCE PER MODEL: both marches reach the map through one
 *  reader. A model without a map leaves its instance at the disabled default.
 *
 *  OUT OF SCOPE: translucent receivers, which write no depth, and the engine's
 *  lighting as opposed to its output -- this multiplies the lit result, so
 *  specular and indirect darken with direct sun. Reach for a light function when
 *  that distinction matters. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereSurfaceShadowParams
{
	GENERATED_BODY()

	/** Off multiplies by one and costs one scalar branch. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bEnabled = true;

	/** How much of a surface's brightness comes from the sun rather than from
	 *  sky and bounce. The shadow scales only that share, so a shadowed surface
	 *  floors at 1 - this rather than going to black.
	 *
	 *  THE DIAL THAT STOPS CLOUD SHADOWS READING AS HOLES IN THE WORLD. At 1
	 *  ambient is shadowed along with the sun and deep shade goes to the map's
	 *  ceiling; at 0 nothing happens. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "1.0"))
	float DirectFraction = 0.7f;

	/** Final multiplier on the optical depth read from the map, for art control
	 *  independent of the physical terms. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled", ClampMin = "0.0"))
	float Strength = 1.0f;

	/** Half-widths of the shadow map's inner cascades around the camera, in
	 *  planet radii: X the mid field (level 1), Y the near field (level 2), each
	 *  held inside the one outside it. They size the map the clouds are lit
	 *  through as well as the one surfaces read. Narrower is sharper near the
	 *  camera and hands over to the coarser level sooner; wider spans lose
	 *  resolution. Zero collapses a level. Only the near field carries the
	 *  detail layer's grain; match Y to the detail layer's FadeFar. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	FVector2D CascadeRadii = FVector2D(0.6, 0.3);

	/** The single vector parameter the march unpacks, the group's scalars on
	 *  one Custom node pin, Z free. Both fields' atmosphere builders mirror this
	 *  layout. */
	FLinearColor Pack() const
	{
		return FLinearColor(bEnabled ? 1.0f : 0.0f, DirectFraction, 0.0f, Strength);
	}
};

// Cloud field parameter groups, both models'. Each model has its own instance
// of every group (Terrestrial* and GasGiant* on the actor), so a type change
// swaps the whole authored set; the gas giant adds its deep deck.
//
// THE SIM IS THE WEATHER MAP, THE NOISE IS THE CLOUD. Coverage, cloud type and
// the pressure lid come from the sim per column; the structure layer's noise,
// shaped by a height profile, is what coverage erodes into individual clouds,
// and the detail layer erodes their edges. See TerrestrialDeck.ush.
//
// PACKED ON THE WAY OUT. Each group travels to the compute march and the bake as
// a few float4 pins, packed in PackCloudField and unpacked once in
// TR_BuildField; the members here keep their own names.
//
// CLOUD THICKNESS IS THE UNIT. Every height below except CloudBase is a
// multiple or share of it.

/** The planet the cloud field sits on. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FTerrestrialPlanetParams
{
	GENERATED_BODY()

	/** Atmosphere top, as a fraction of planet radius above the surface. The
	 *  ceiling every other shell in the system is expressed against. Floored so
	 *  the shadow map's no-deck sentinel (1000 thicknesses) lies past every
	 *  chord with half-float precision to spare. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.005"))
	float HeightScale = 0.2f;

	/** The field's rotation as a share of the sim's own, PlanetaryVorticity / 2
	 *  radians per unit sim time: 1 turns the clouds as fast as the planet the
	 *  sim's Coriolis assumes. The sim runs in the rotating frame, so this
	 *  rotates the sampling position. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float SpinRatio = 1.0f;
};

/** Where the clouds sit and how a column's height profile is shaped. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FTerrestrialShapeParams
{
	GENERATED_BODY()

	/** Condensation level of an unlifted column, as a fraction of atmosphere
	 *  thickness. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CloudBase = 0.15f;

	/** Depth of a fully towering column before the pressure lid, as a fraction of
	 *  atmosphere thickness, and the unit of every lift and warp offset. THE
	 *  MARCHED BAND IS BOUNDED BY THIS, so it also sets how much of the shell
	 *  gets fine-stepped. PITFALL: keep CloudBase plus this below
	 *  1 - CeilingFalloff, or the ceiling thins every tall column. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001", ClampMax = "1.0"))
	float CloudThickness = 0.5f;

	/** Share of CloudThickness each end of the height profile ramps over. At 0.5
	 *  a full column has no flat core. Also sets the bake's step, which is why it
	 *  stops short of 0: a near-zero ramp drives the bake into its step cap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.05", ClampMax = "0.5"))
	float SurfaceSoftness = 0.35f;

	/** Shape of the profile's top ramp. Below 0.5 it loses its C1 join. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001"))
	float TopCurve = 1.0f;

	/** Shape of the bottom ramp. Above 1 flattens cloud bases. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001"))
	float BottomCurve = 2.0f;

	/** Width of the band under the shell top across which density fades to zero,
	 *  as a fraction of atmosphere thickness, so the tallest towers cap softly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001", ClampMax = "0.5"))
	float CeilingFalloff = 0.2f;

	/** READOUT, not authored: the highest a column top can reach. Above
	 *  1 - CeilingFalloff the tallest columns are being capped. */
	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly)
	float SolvedTopMax = 0.0f;

	/** READOUT, not authored: the lowest a column base can fall, where the
	 *  marched band ends. */
	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly)
	float SolvedBaseMin = 0.0f;
};

/** Which of the sim's cloud becomes cloud here. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FTerrestrialCoverageParams
{
	GENERATED_BODY()

	/** The sim layer the clouds are drawn from, -1 for the bottom. A layer's
	 *  cloud is combined with every layer's above it, so the bottom reads the
	 *  whole sky and 0 the top layer alone. Vertical motion, pressure and the
	 *  noise's displacement come from the same layer, so the cloud and its
	 *  breakup move together. The sim's Column cloud debug view at the same
	 *  layer shows exactly the cover read. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "-1", ClampMax = "7"))
	int32 CloudLayer = -1;

	/** How far down the ranking of cloud coverage reaches. Each column ranks by
	 *  its cloud amount raised by its storm, so hurricanes rank highest, then
	 *  storms, then plain cloud. 0 is clear sky, low values keep only
	 *  hurricanes and large storms, and the rest of the cloud comes in toward
	 *  1, where every column holding cloud is covered. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CloudCover = 0.8f;

	/** How far storm raises a column's rank over plain cloud: rank is amount *
	 *  (1 + StormPriority * storm) / (1 + StormPriority), so plain cloud tops
	 *  out at 1 / (1 + StormPriority) and full storm reaches 1. 0 ranks by cloud
	 *  alone. Storm raises only cloud that exists. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float StormPriority = 1.0f;

	/** Half-width of the coverage threshold, in rank: how far a system's edge
	 *  ramps from clear to fully covered. Lower is crisper. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001", ClampMax = "1.0"))
	float CoverageSoftness = 0.2f;

	/** How system edges thin out as coverage falls: 1 frays them into
	 *  scattered cores the noise picks, 0 fades them evenly. Inside a system,
	 *  where coverage is full, the structure layer's erosion alone decides how
	 *  much is cut clear. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CoverageFray = 0.5f;
};

/** Cloud type, 0 stratiform to 1 towering, and the noise each genus draws.
 *
 *  Type is TypeBias + TypeTropical * tropicality + TypeStorm * storm^TypeCurve.
 *  It sets column depth and the noise genus, not the material, which follows
 *  storm alone.
 *
 *  The genus colours are weights over the structure volume's four channels as
 *  the deck reads them, R smooth (Perlin), G billow (inverted Worley F1), B
 *  cellular (Worley F2 - F1), A fibrous (ridged Perlin); see
 *  Design/TerrestrialClouds.md for the bake. Each column blends the four genera
 *  by its type and altitude, and layered cloud turns cellular where the air
 *  sinks. Weights need not sum to one: the blend keeps the noise's contrast
 *  whatever their total. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FTerrestrialTypeParams
{
	GENERATED_BODY()

	/** A column's type before the weather moves it: 0 stratiform, 1 towering.
	 *  Type sets a column's depth, between StratusDepth and full, and its
	 *  genus blend. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float TypeBias = 0.8f;

	/** How far the tropics raise type toward towering cloud. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float TypeTropical = 0.3f;

	/** How far the sim's storm raises type toward towering cloud. The storm
	 *  material is StormBalance's, not this. A term rather than a floor, so storm grades into the cloud around it instead of turning every
	 *  stormy column into a full tower. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float TypeStorm = 0.5f;

	/** Exponent the storm term of type builds with. 1 is linear; higher lets
	 *  only the stormiest columns approach full towering storm cloud. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.1", ClampMax = "8.0"))
	float TypeCurve = 2.0f;

	/** Depth of a stratiform column as a fraction of a towering one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StratusDepth = 0.25f;

	/** Low layered cloud in still or rising air: sheets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor Stratus = FLinearColor(1.0f, 0.0f, 0.0f, 0.0f);

	/** Low layered cloud in sinking air: broken cells. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor Stratocumulus = FLinearColor(0.3f, 0.0f, 0.7f, 0.0f);

	/** Towering cloud at any altitude. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor Cumulus = FLinearColor(0.2f, 0.8f, 0.0f, 0.0f);

	/** High layered cloud: streaks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor Cirrus = FLinearColor(0.2f, 0.0f, 0.0f, 0.8f);

	/** How fast sinking air turns stratus into stratocumulus, per unit of the
	 *  sim's vertical motion. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float Subsidence = 3.0f;
};

/** How pressure, tropicality and formation altitude move a column's lid and
 *  base. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FTerrestrialLiftParams
{
	GENERATED_BODY()

	/** The sim pressure that reads as a full low or high. Raise it until the lid
	 *  varies across a system rather than switching at its edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.000001"))
	float PressureScale = 0.5f;

	/** How far pressure moves a column's depth, as a share of CloudThickness:
	 *  positive gives a low more room than a high. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "-0.99", ClampMax = "0.99"))
	float CeilingPressure = 0.5f;

	/** How far tropicality and pressure move the base, multiples of
	 *  CloudThickness: a higher condensation level in the tropics, lower under
	 *  lows. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float BaseTropical = 0.1f;

	/** How far pressure moves the base, a multiple of CloudThickness per unit
	 *  of the deck's pressure: negative lowers it under lows. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float BasePressure = -0.1f;

	/** Scales the sim's formation altitude -- where in its stack of layers the
	 *  cloud condensed, 0 at the bottom and 1 at the top, raised within its
	 *  layer by the ascent it condensed at -- to the deck's altitude, held to
	 *  [0, 1]. Higher lifts more of the cloud to full altitude. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float AltitudeGain = 3.0f;

	/** How far altitude lifts the base, multiples of CloudThickness, scaled by
	 *  1 - Type: towers stay rooted, stratiform cloud floats up to where it
	 *  formed. Widens the marched band by the same amount. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float AltitudeLift = 0.6f;
};

/** How rising air bends the noise. Noise only; the bounds are unaffected. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FTerrestrialWarpParams
{
	GENERATED_BODY()

	/** How far rising air stretches the noise vertically: towers drawn taller,
	 *  subsiding air pressed into sheets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "-0.9"))
	float WarpStretch = 0.5f;

	/** How far rising air lifts the noise, a multiple of CloudThickness. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float WarpShift = 0.2f;
};

/** The cloud shape coverage erodes. The volume's channels are noise TYPES
 *  (R Perlin, G Worley F1, B Worley F2 - F1, A ridged Perlin) that the genus
 *  weights blend; the sim carries the noise, and FlowInherit is the share of
 *  that carried displacement the layer follows. Applies at every range. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FTerrestrialStructureLayerParams
{
	GENERATED_BODY()

	/** The typed cloud noise. SAMPLER: wrap on all three axes, Linear Color. No
	 *  volume leaves the layer out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<UVolumeTexture> Volume = nullptr;

	/** Horizontal frequency at the planet's surface, in noise units per
	 *  radian: higher is smaller clouds. It grows with height by
	 *  exp(Aspect * HeightScale * height), height in atmosphere fractions. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.01"))
	float Scale = 6.0f;

	/** Vertical frequency over the horizontal one, at every height: higher is
	 *  flatter features. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.01"))
	float Aspect = 2.0f;

	/** How far the noise breaks the cloud up. Below 1 some of each column fills
	 *  whatever the noise; past 1 the shaping extrapolates, holes open that
	 *  survive any coverage, and from about half up a fully covered column is
	 *  cut clear as readily as a thin one. 0 leaves smooth sheets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float Erosion = 0.85f;

	/** Share of the sim's carried noise displacement the layer follows: 1 moves
	 *  with the weather, 0 stays fixed on the planet. At 1, high-turnover
	 *  regions shear the noise into streaks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FlowInherit = 0.9f;

	/** Mips added to the one the volume is read at from the pixel's footprint:
	 *  lower is sharper and shimmers more in motion, which the temporal resolve
	 *  partly averages. The volume needs its mips. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "-2.0", ClampMax = "4.0"))
	float MipBias = 1.0f;
};

/** Edge erosion: wispy toward the base, billowy toward the top, fibrous in
 *  cirrus. Fades with distance to its mean, so distant cloud keeps the same
 *  average erosion without the grain. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FTerrestrialDetailLayerParams
{
	GENERATED_BODY()

	/** The typed cloud noise, on its own asset so its features do not rhyme
	 *  with the structure's. No volume leaves the layer out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<UVolumeTexture> Volume = nullptr;

	/** Horizontal frequency at the surface, in noise units per radian: higher
	 *  is finer grain. It grows with height as the structure layer's does. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.01"))
	float Scale = 30.0f;

	/** Vertical frequency over the horizontal one, at every height, as for the
	 *  structure layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.01"))
	float Aspect = 1.0f;

	/** How hard the edges are eaten, up to TR_DETAIL_EROSION_SCALE of the
	 *  density at 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Erosion = 0.6f;

	/** Share of the sim's carried noise displacement the layer follows. Lower
	 *  than the structure's, so sheared regions turn strandy and keep some
	 *  rounded detail. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FlowInherit = 0.4f;

	/** Mips added to the one the pixel's footprint reads, as for the
	 *  structure layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "-2.0", ClampMax = "4.0"))
	float MipBias = 0.0f;

	/** Where the grain starts fading to the mean, and where it reaches it, in
	 *  planet radii from the camera: the unit of the shadow cascades'
	 *  CascadeRadii. The fetch is skipped beyond FadeFar. Only the near shadow
	 *  cascade carries the grain, so keep CascadeRadii.Y at or past FadeFar for
	 *  the shadows to show it wherever the clouds do. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float FadeNear = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float FadeFar = 0.3f;

	/** The noise's mean, what the layer settles to as it fades. 0.5 for an
	 *  equalized volume. PITFALL: cloud thickening or thinning across the fade
	 *  band means this is off from the volume's real mean. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FadeMean = 0.5f;
};

/** The clouds' material: fair-weather cloud at 0, storm cloud at 1, blended by
 *  the storm index StormBalance and StormBlend threshold, not by cloud type.
 *  Scatter is single-scattering albedo, held to [0, 1]. CloudExtinction is an
 *  RGB tint on the solved extinction, 1 neutral; CloudOpticalDepth sets its
 *  amount. StormExtinction is a tint with the storm's amount in A, as a
 *  multiple of fair-weather cloud's. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FTerrestrialCloudMaterialParams
{
	GENERATED_BODY()

	/** Optical depth through a full-depth column of fair-weather cloud at
	 *  density 1: the clouds' opacity. Coverage and the noise take most columns
	 *  well under it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.1"))
	float CloudOpticalDepth = 40.0f;

	/** How much of the cloud is storm material: 0 none, 1 all. Each column
	 *  ranks by a storm index, the larger of its raw sim cloud and its storm, so
	 *  storm cells rank highest, then thick system cores, and the threshold
	 *  sweeps down through that. Reads nothing the density does, so it changes
	 *  the storm/fair-weather balance without touching the deck's shape. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StormBalance = 0.5f;

	/** Half-width of the storm threshold, in storm index: how gradually cloud
	 *  scatter ramps into storm scatter from a system's edge toward its core.
	 *  Small is a crisp split; large, a long gradient. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001", ClampMax = "1.0"))
	float StormBlend = 0.15f;

	/** Fair-weather cloud's single-scattering albedo, per channel, held to
	 *  [0, 1]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor CloudScatter = FLinearColor(0.98f, 0.98f, 0.98f, 1.0f);

	/** Fair-weather cloud's extinction tint per channel, 1 neutral; its amount
	 *  is CloudOpticalDepth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor CloudExtinction = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);

	/** Storm cloud's single-scattering albedo, per channel, held to [0, 1]:
	 *  lower is darker storm cloud. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor StormScatter = FLinearColor(0.9f, 0.92f, 0.95f, 1.0f);

	/** Storm cloud's extinction tint in RGB, and in A its opacity as a
	 *  multiple of fair-weather cloud's. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor StormExtinction = FLinearColor(1.0f, 1.0f, 1.0f, 2.0f);
};

/** How the march spreads its samples over pixels, frames and distance. For a
 *  performance tier, with Raymarch's step counts. Each model has its own, the
 *  lattice growth being relative to its own cloud shell. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereSamplingParams
{
	GENERATED_BODY()

	/** One pixel of each CellSize square is marched per frame and the rest come
	 *  from history, so the march costs 1 / CellSize^2 of a full-resolution one
	 *  and a pixel refreshes every CellSize^2 frames. Larger is cheaper, slower
	 *  to settle and softer in motion. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1", ClampMax = "16"))
	int32 CellSize = 3;

	/** Least share a pixel's own new sample takes of its history. Lower averages
	 *  more frames and settles smoother; higher follows change sooner. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float FreshWeight = 0.15f;

	/** How much longer each cloud step is than the last, with the camera in or
	 *  under the deck and from four shell depths above it, blended between by
	 *  altitude. Lower is finer and costlier; equal values retire the blend. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001", ClampMax = "1.0"))
	float LatticeGrowth = 0.2f;

	/** The growth from four shell depths above the deck and beyond. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001", ClampMax = "1.0"))
	float LatticeGrowthFar = 0.02f;
};

/** The gas giant's deep deck, beneath the slab's groups (TR_DEEP_DECK). */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FGasGiantDeepParams
{
	GENERATED_BODY()

	/** How far below a column's base the deck reaches full density, as a
	 *  fraction of atmosphere thickness: coverage fills in and erosion, detail
	 *  and the eye's thinning fade out over it, on down through the core. The
	 *  marched band ends where every column is full, so a deeper fill
	 *  fine-steps further down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001", ClampMax = "1.0"))
	float DeepFill = 0.1f;

	/** How far the floor rises into the fill, in fills: full-density mounds
	 *  shaped by the structure noise, from where the fill completes up to
	 *  FloorRelief fills above it at the highest noise; 1 reaches the base.
	 *  The floor only adds density, so the cloud above is unchanged. 0 is no
	 *  floor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FloorRelief = 0.5f;
};

// Shared parameter groups: the air, the cloud's lighting and the march's
// budget.
//
// ONE STRUCT PER PANEL GROUP, declared on the actor as one property and inlined
// with ShowOnlyInnerProperties into the category it names. PITFALL: do not give
// a member here a category. A member that names one is pulled out of its
// struct's group, and category-less members then sort after every member that
// has one.
//
// NAMES MATCH THE STACK. Each member's name is its shader uniform and its
// shader term; these structs are data, and every derived quantity is computed
// once, in the field's builders, which the march and the shadow bake share.

/** The air: how much of it there is, its colour, and how it scatters.
 *
 *  EACH DEPTH IS A VERTICAL COLUMN, the optical depth per channel from the
 *  ground to the shell top, so its colour is the air's and its amount how
 *  thick the air reads. The scale heights only shape how the column is spread
 *  with altitude. The *Beta() helpers give the per-thickness coefficients the
 *  march takes, which Atmo_BuildParams divides by atmosphere thickness. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereAirParams
{
	GENERATED_BODY()

	/** Molecular scattering's column optical depth per channel: the sky's
	 *  colour and how thick it reads. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor RayleighDepth = FLinearColor(4.774597f, 9.999406f, 12.8395f, 1.0f);

	/** As a fraction of atmosphere thickness. Well under the cloud tops there
	 *  is no Rayleigh above them, and the limb reads as a hard edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001"))
	float RayleighScaleHeight = 0.45f;

	/** Aerosol haze's column optical depth per channel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor MieDepth = FLinearColor(2.454211f, 2.191878f, 1.855216f, 1.0f);

	/** As a fraction of atmosphere thickness. Cloud that reaches most of the
	 *  way up the shell needs haze still present above it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001"))
	float MieScaleHeight = 0.25f;

	/** Mie asymmetry. Rayleigh has no counterpart: its phase is fixed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "-0.99", ClampMax = "0.99"))
	float MieG = 0.95f;

	/** How fast the Mie forward lobe dies behind cloud, per optical depth.
	 *  Light that crossed a dense medium has lost its direction, and carrying the
	 *  full phase through puts the glow on top of the planet. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float MieLobeDecay = 2.0f;

	/** A Lorentzian layer, ozone-like, scaled by the Rayleigh profile, rather
	 *  than a profile falling off from the ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor AbsorptionDepth = FLinearColor(15.98955f, 12.95084f, 14.0181f, 1.0f);

	/** Altitude the absorber layer is centred on, as a fraction of thickness. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float AbsorptionAltitude = 0.15f;

	/** Half-width of the absorber layer. Floored, as AtmoT_Profile floors it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001"))
	float AbsorptionFalloff = 0.1f;

	FLinearColor RayleighBeta() const { return ToBeta(RayleighDepth, ExponentialColumn(RayleighScaleHeight)); }
	FLinearColor MieBeta() const { return ToBeta(MieDepth, ExponentialColumn(MieScaleHeight)); }
	FLinearColor AbsorptionBeta() const { return ToBeta(AbsorptionDepth, AbsorberColumn()); }

private:
	/** The column of exp(-h / H) over the shell, h and H in thicknesses, with
	 *  AtmoT_Profile's floor on H. */
	static float ExponentialColumn(float ScaleHeight)
	{
		const float H = FMath::Max(ScaleHeight, 1e-4f);
		return H * -FMath::Exp(-1.0f / H) + H;
	}

	/** The column of Atmo_Density's absorber over the shell, by Simpson's rule
	 *  on 64 intervals: a Lorentzian of half-width AbsorptionFalloff, floored as
	 *  AtmoT_Profile floors it, about AbsorptionAltitude, times the Rayleigh
	 *  profile. */
	float AbsorberColumn() const
	{
		constexpr int32 Intervals = 64;

		const float H = FMath::Max(RayleighScaleHeight, 1e-4f);
		const float W = FMath::Max(AbsorptionFalloff, 1e-4f);

		float Sum = 0.0f;

		for (int32 i = 0; i <= Intervals; ++i)
		{
			const float X = (float)i / Intervals;
			const float U = (AbsorptionAltitude - X) / W;
			const float Weight = (i == 0 || i == Intervals) ? 1.0f : ((i & 1) ? 4.0f : 2.0f);

			Sum += Weight * FMath::Exp(-X / H) / (1.0f + U * U);
		}

		return Sum / (3.0f * Intervals);
	}

	static FLinearColor ToBeta(const FLinearColor& Depth, float Column)
	{
		const float Inv = 1.0f / FMath::Max(Column, 1e-8f);
		return FLinearColor(Depth.R * Inv, Depth.G * Inv, Depth.B * Inv, 1.0f);
	}
};

/** Light that bounced several times, for the air and the cloud, and where it
 *  fades across the terminator. Each has its own floor, since the cloud and the
 *  air go dark at different rates. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereAmbientParams
{
	GENERATED_BODY()

	/** The air's ambient, as a ratio of the star's light per channel. Only a
	 *  lit surface under the air bounces much up: near-black over an opaque
	 *  deck, brighter over a terrestrial surface. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor AirAmbient = FLinearColor(3.333333e-6f, 3.508772e-6f, 3.703704e-6f, 1.0f);

	/** What the ambient's terminator falloff lerps from. At zero the term has
	 *  almost no range, being swamped by direct light everywhere it is not zero;
	 *  the floor buys it a night side. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float AirAmbientFloor = 0.0001f;

	/** The cloud's ambient, as a ratio of the star's light per channel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor CloudAmbient = FLinearColor(0.0001f, 0.0001f, 0.0001f, 1.0f);

	/** The cloud's night-side share of its ambient, as AirAmbientFloor is the
	 *  air's. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float CloudAmbientFloor = 0.001f;

	/** Width of the ambient terminator, in cosine of sun elevation; 0.15 is about
	 *  9 degrees either side. Ambient applied unconditionally washes the night
	 *  side, which reads as the star shining through the planet. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001", ClampMax = "1.0"))
	float AmbientTerminator = 0.15f;
};

/** The cloud's phase function: two Henyey-Greenstein lobes. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmospherePhaseParams
{
	GENERATED_BODY()

	/** Forward lobe asymmetry. What makes the rim bright near the sun. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float ForwardG = 0.9f;

	/** Backward lobe asymmetry, as a magnitude. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float BackwardG = 0.1f;

	/** Share of the forward lobe in the blend. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ForwardWeight = 0.5f;
};

/** Octaves after Wrenninge: octave i sees the cloud toward the light at
 *  ScatteringGlow^i of its optical depth, weighs ScatteringGlow^i, and uses the
 *  phase with its g scaled by (1 - ScatteringSpread)^i. Later octaves reach
 *  deeper and scatter more broadly -- the glow inside thick cloud, and a softer
 *  rim. One exp per octave per cloud sample. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereMultipleScatteringParams
{
	GENERATED_BODY()

	/** How far sunlight gets into cloud: the light ray sees 1 - this of the view
	 *  ray's extinction, octave 0's depth factor, which the shadow map's depths
	 *  carry too. 0 is the full extinction; above it stands in for light
	 *  scattered forward back into the ray, which the full coefficient counts as
	 *  lost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SunlightPenetration = 0.5f;

	/** How much light the later octaves carry: each sees this much less of the
	 *  depth toward the light and weighs this much less. Higher glows brighter
	 *  inside thick cloud; thin cloud brightens by the sum of the weights, every
	 *  octave seeing it at full transmittance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ScatteringGlow = 0.6f;

	/** How much broader each later octave's phase is. Higher makes them more
	 *  isotropic and releases them from the lobe shadow in proportion; 0 keeps
	 *  every octave as forward-peaked as the first. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ScatteringSpread = 0.4f;

	/** Octaves including single scattering. 1 is single scattering only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1", ClampMax = "4"))
	int32 OctaveCount = 3;

	/** The light ray's extinction as a fraction of the view ray's: the value the
	 *  march and the bake read. */
	float LightExtinctionFraction() const
	{
		return 1.0f - FMath::Clamp(SunlightPenetration, 0.0f, 1.0f);
	}

	/** The per-octave factors the march takes. */
	float OctaveAttenuation() const { return FMath::Clamp(ScatteringGlow, 0.0f, 1.0f); }
	float OctaveEccentricity() const { return 1.0f - FMath::Clamp(ScatteringSpread, 0.0f, 1.0f); }
};

/** The march's budget, for a performance tier. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereRaymarchParams
{
	GENERATED_BODY()

	/** Steps across the air segments. Capped by ATMO_MAX_ITER per segment. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "2", ClampMax = "256"))
	int32 AtmosphereSteps = 64;

	/** Steps across the cloud a ray actually crosses, shared across every cloud
	 *  segment on it. Capped by ATMO_MAX_ITER per segment. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "2", ClampMax = "256"))
	int32 CloudSteps = 128;

	/** How much longer a class's last step is than its first, the counts above
	 *  being spread geometrically across the chord that class actually occupies.
	 *  1 is uniform; raising it moves samples toward the NEAR END OF THE CLOUD,
	 *  which is the face the camera sees from either side of the band.
	 *
	 *  THE COUNTS ARE EXACT, so this redistributes rather than adds: a ray costs
	 *  what the two counts name however it is angled, and the only variance left
	 *  is a step per segment boundary and rays that end early on transmittance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1.0", ClampMax = "64.0"))
	float ChordSpread = 8.0f;

};
