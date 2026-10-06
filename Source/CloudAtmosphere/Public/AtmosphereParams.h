// The parameter sets the atmosphere's passes are driven by, and the
// derivations that keep them consistent.
//
// ONE BUNDLE PER MODEL. Both models share every group's member list, and an
// actor carries one FAtmosphereModelParams per model, so a type change swaps the
// whole authored set. The pipeline and quality groups (Simulation, Raymarch,
// Sampling) are one instance; Simulation holds a config per model.
//
// RATIOS, NOT ABSOLUTES, WHEREVER ONE VALUE IS BOUNDED BY ANOTHER, so a value
// stays valid when its bound is retuned; an absolute one silently goes out of
// range, showing as a geometry or sampling artifact rather than a bad value.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "AtmosphereParams.generated.h"

class UFlowSimConfig;

class UVolumeTexture;

/** Which model the march draws. An enum rather than a bool: adding a case
 *  fails loudly at every switch, where a bool silently takes the false branch.
 *  Air only is the terrestrial model without its clouds, for moons and thin-air
 *  planets: no sim, no shadow bake, no cloud march. */
UENUM(BlueprintType)
enum class EPlanetAtmosphereType : uint8
{
	Terrestrial,
	GasGiant,
	AirOnly UMETA(DisplayName = "Air Only")
};

/** A performance tier for the actor's Advanced Graphics settings, applied by
 *  APlanetAtmosphereActor::ApplyGraphicsPreset. */
UENUM(BlueprintType)
enum class EAtmosphereGraphicsPreset : uint8
{
	Max,
	High,
	Mid,
	Low,
	Min
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

/** The sun's direct share on whatever opaque geometry the depth buffer holds:
 *  terrain, meshes, a mesh inner surface, other actors, evaluated once where
 *  the view ray stopped. The air dims and reddens it toward the terminator on
 *  every model. The cloud models also take the cloud and deck shadow there
 *  from the shadow map, whose depth is valid anywhere in the shell. The bake
 *  reads only CascadeRadii; Air Only reads neither it nor Strength.
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
	 *  independent of the physical terms. Not read on Air Only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled", ClampMin = "0.0"))
	float Strength = 1.0f;

	/** Half-widths of the shadow map's inner cascades around the camera, in
	 *  planet radii: X the mid field (level 1), Y the near field (level 2), each
	 *  held inside the one outside it. They size the map the clouds are lit
	 *  through as well as the one surfaces read. Narrower is sharper near the
	 *  camera and hands over to the coarser level sooner; wider spans lose
	 *  resolution. Zero collapses a level; X at 0 collapses the near field too,
	 *  since it is held inside the mid field. Only the near field carries the
	 *  detail layer's grain; match Y to the detail layer's FadeFar. Not read on
	 *  Air Only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	FVector2D CascadeRadii = FVector2D(0.6, 0.3);

	/** The surface shadow pin the march unpacks: the group's scalars, Z free. */
	FLinearColor Pack() const
	{
		return FLinearColor(bEnabled ? 1.0f : 0.0f, FMath::Clamp(DirectFraction, 0.0f, 1.0f), 0.0f, FMath::Max(Strength, 0.0f));
	}
};

// Cloud field parameter groups, both models'. The gas giant adds its deep deck.
//
// THE SIM IS THE WEATHER MAP, THE NOISE IS THE CLOUD. Coverage, cloud type and
// the pressure lid come from the sim per column; the structure layer's noise,
// shaped by a height profile, is what coverage erodes into individual clouds,
// and the detail layer erodes their edges. See TerrestrialDeck.ush.
//
// PACKED ON THE WAY OUT: each group reaches the march and the bake as a few
// float4 pins, packed in PackCloudField and unpacked once in TR_BuildField.
//
// THREE HEIGHT UNITS, each member stating its own. Atmosphere fractions, of the
// shell above the surface: CloudBase, CloudThickness, CeilingFalloff, DeepFill
// and the air's heights. Multiples or shares of CloudThickness: the lift and
// warp offsets, CeilingPressure, SurfaceSoftness and StratusDepth. Fills, of
// DeepFill: FloorRelief.

/** The planet the cloud field sits on. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmospherePlanetParams
{
	GENERATED_BODY()

	/** Atmosphere top, as a fraction of planet radius above the surface. The
	 *  ceiling every other shell in the system is expressed against. Below
	 *  about 0.005 the shadow map's depths, half floats in thicknesses, coarsen
	 *  toward the terminator. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001", UIMin = "0.005"))
	float HeightScale = 0.2f;

	/** The field's rotation as a share of the sim's own, PlanetaryVorticity / 2
	 *  radians per unit sim time: 1 turns the clouds as fast as the planet the
	 *  sim's Coriolis assumes. The sim runs in the rotating frame, so this
	 *  rotates the sampling position. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (UIMin = "0.0"))
	float SpinRatio = 1.0f;
};

/** Where the clouds sit and how a column's height profile is shaped. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FCloudShapeParams
{
	GENERATED_BODY()

	/** Condensation level of an unlifted column, as a fraction of atmosphere
	 *  thickness. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (UIMin = "0.0", UIMax = "1.0"))
	float CloudBase = 0.15f;

	/** Depth of a fully towering column before the pressure lid, as a fraction of
	 *  atmosphere thickness, and the unit of every lift and warp offset. THE
	 *  MARCHED BAND IS BOUNDED BY THIS, so it also sets how much of the shell
	 *  gets fine-stepped. PITFALL: keep CloudBase plus this below
	 *  1 - CeilingFalloff, or the ceiling thins every tall column. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001", UIMax = "1.0"))
	float CloudThickness = 0.5f;

	/** Share of CloudThickness each end of the height profile ramps over. At 0.5
	 *  a full column has no flat core. Also sets the bake's step, which is why it
	 *  stops short of 0: a near-zero ramp drives the bake into its step cap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001", ClampMax = "0.5", UIMin = "0.05"))
	float SurfaceSoftness = 0.35f;

	/** Shape of the profile's top ramp: higher flattens and lowers the top, as
	 *  BottomCurve does the base. Below 0.5 it loses its C1 join. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001"))
	float TopCurve = 1.0f;

	/** Shape of the bottom ramp. Above 1 flattens cloud bases. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001"))
	float BottomCurve = 2.0f;

	/** Width of the band under the shell top across which density fades to zero,
	 *  as a fraction of atmosphere thickness, so the tallest towers cap softly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001", UIMax = "0.5"))
	float CeilingFalloff = 0.2f;

	/** Bound on either cloud surface's slope, in cloud depths per flow texel:
	 *  the cone angle for the shadow bake's entry search. Per texel because the
	 *  surfaces are built on the flow, so their steepest walls narrow with the
	 *  grid. Under-declaring it is the one way that search steps over cloud,
	 *  and the symptom is shadow missing under steep cloud walls, such as a
	 *  small storm's eyewall or a sharp CoverageSoftness. Raise it first. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001", UIMin = "0.5", UIMax = "8.0"))
	float SlopePerTexel = 1.5f;

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
struct CLOUDATMOSPHERE_API FCloudCoverageParams
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

	/** The share of the planet covered, 0 clear sky to 1 every column holding
	 *  cloud. Columns rank by priority, their cloud amount raised by their
	 *  storm, and the threshold is solved each frame so this share lies above
	 *  it: hurricanes first, then storms, then plain cloud from systems' cores
	 *  outward. The same setting covers the same share whatever the sim's cloud
	 *  does, up to the share holding cloud above CoverageSoftness; the noise
	 *  breaks up what is covered. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CloudCover = 0.5f;

	/** The sim cloud at which a column's amount is 95% of the way to full.
	 *  Small makes the amount all but binary, so plain cloud ranks alike and
	 *  storm alone orders it; larger grades it from systems' cores to their
	 *  edges, which coverage then takes in that order. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.000001", UIMin = "0.001", UIMax = "1.0"))
	float CloudFull = 0.05f;

	/** How far storm raises a column's rank over plain cloud: rank is amount *
	 *  (1 + StormPriority * storm) / (1 + StormPriority), so plain cloud tops
	 *  out at 1 / (1 + StormPriority) and full storm reaches 1. 0 ranks by cloud
	 *  alone. Storm raises only cloud that exists. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float StormPriority = 1.0f;

	/** Half-width of the coverage threshold, in rank: how far a system's edge
	 *  ramps from clear to fully covered. Lower is crisper. PITFALL: the
	 *  threshold never falls below this, so above 0.5 no column reaches full
	 *  coverage and CloudCover loses its hold. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001", UIMax = "1.0"))
	float CoverageSoftness = 0.2f;

	/** How system edges thin out as coverage falls: 1 frays them into
	 *  scattered cores the noise picks, 0 fades them evenly. Inside a system,
	 *  where coverage is full, the structure layer's erosion alone decides how
	 *  much is cut clear. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CoverageFray = 0.5f;

	/** Coverage over which a column grows from no depth to its full depth:
	 *  wide thins system edges into low wisps, narrow stands scattered cloud
	 *  full height. PITFALL: below about 0.1 the edges are cliffs steeper than
	 *  SlopePerTexel bounds, and shadow goes missing under them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001", UIMin = "0.1", UIMax = "0.6"))
	float CoverageDepthRamp = 0.25f;

	/** Power on the sim's eye depth factor that a column's density scales by:
	 *  above 1 keeps more eyewall and clears the eye's floor sooner, below 1 a
	 *  hazier eye. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", UIMax = "4.0"))
	float EyeOpenPower = 1.0f;
};

/** The noise each genus draws: weights over the structure volume's four
 *  channels as the deck reads them, R smooth (Perlin), G billow (inverted
 *  Worley F1), B cellular (Worley F2 - F1), A fibrous (ridged Perlin); see
 *  Design/FieldReference.md for the bake. Each column blends the four by
 *  its type and altitude, and layered cloud turns cellular where the air
 *  sinks. Weights need not sum to one: the blend keeps the noise's contrast
 *  whatever their total. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FCloudGenusParams
{
	GENERATED_BODY()

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
};

/** Cloud type, 0 stratiform to 1 towering: TypeBias + TypeTropical *
 *  tropicality + TypeStorm * storm^TypeCurve. It sets column depth and the
 *  noise genus, not the material, which follows the storm index (Material). */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FCloudTypeParams
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
	 *  material is StormBalance's, not this. A term rather than a floor, so
	 *  storm grades into the cloud around it instead of turning every stormy
	 *  column into a full tower. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float TypeStorm = 0.5f;

	/** Exponent the storm term of type builds with. 1 is linear; higher lets
	 *  only the stormiest columns approach full towering storm cloud. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.1", UIMax = "8.0"))
	float TypeCurve = 2.0f;

	/** Depth of a stratiform column as a fraction of a towering one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StratusDepth = 0.25f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FCloudGenusParams Genus;

	/** How fast sinking air turns stratus into stratocumulus, per unit of the
	 *  sim's vertical motion. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float Subsidence = 3.0f;
};

/** How pressure, tropicality and formation altitude move a column's lid and
 *  base. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FCloudLiftParams
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
struct CLOUDATMOSPHERE_API FCloudWarpParams
{
	GENERATED_BODY()

	/** How far rising air stretches the noise vertically: towers drawn taller,
	 *  subsiding air pressed into sheets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (UIMin = "-0.9"))
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
struct CLOUDATMOSPHERE_API FCloudStructureLayerParams
{
	GENERATED_BODY()

	/** The typed cloud noise. SAMPLER: wrap on all three axes, Linear Color. No
	 *  volume leaves the layer out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<UVolumeTexture> Volume = nullptr;

	/** Horizontal frequency at CloudBase, in noise units per radian: higher is
	 *  smaller clouds. It grows with height at the rate Aspect sets, so the
	 *  base's height leaves the clouds' size alone. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (UIMin = "0.01"))
	float Scale = 6.0f;

	/** Vertical frequency over the horizontal one, at every height, measured
	 *  in decks: higher is flatter features. Every deck spans the features a
	 *  0.06 planet radii deep one would (TR_ASPECT_DEPTH), so CloudThickness
	 *  and HeightScale leave the noise's features alone. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", UIMin = "0.01"))
	float Aspect = 2.0f;

	/** How far the noise breaks the cloud up. Below 1 some of each column fills
	 *  whatever the noise; past 1 the shaping extrapolates, holes open that
	 *  survive any coverage, and from 1 / (1 + Breakup) up a fully covered
	 *  column is cut clear as readily as a thin one. 0 leaves smooth sheets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", UIMax = "2.0"))
	float Erosion = 0.85f;

	/** Share of the noise's range full erosion cuts away: the hole fraction a
	 *  thick system breaks up into. Low keeps thick decks solid, high breaks
	 *  them into separate masses with clear gaps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.2", UIMax = "0.8"))
	float Breakup = 0.5f;

	/** Share of the sim's carried noise displacement the layer follows: 1 moves
	 *  with the weather, 0 keeps only the sim's NoiseDriftSpeed drift. At 1,
	 *  high-turnover regions shear the noise into streaks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (UIMin = "0.0", UIMax = "1.0"))
	float FlowInherit = 0.9f;

	/** How far the crossfade between the noise's two phases leans to the one
	 *  the flow has drawn out less: each weighs by its stretch to the power
	 *  -StretchBias, from the sim's strain. Breaks strands up where a long
	 *  NoiseResetTurnovers lets the shear stretch them. 0 crossfades evenly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "8.0", UIMax = "4.0"))
	float StretchBias = 0.0f;

	/** How far the flow's stretch flattens the features: the vertical
	 *  frequency takes the stretch to this power. At 1 a feature thins
	 *  vertically as much as it narrows across the flow, so shear zones draw
	 *  sheets rather than tall narrow walls. 0 leaves the height alone. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", UIMax = "1.0"))
	float StretchFlatten = 0.0f;

	/** Mips added to the one the volume is read at from the pixel's footprint:
	 *  lower is sharper and shimmers more in motion, which the temporal resolve
	 *  partly averages. The volume needs its mips. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (UIMin = "-2.0", UIMax = "4.0"))
	float MipBias = 1.0f;
};

/** Edge erosion: wispy toward the base, billowy toward the top, fibrous in
 *  cirrus. Fades with distance to its mean, so distant cloud keeps the same
 *  average erosion without the grain. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FCloudDetailLayerParams
{
	GENERATED_BODY()

	/** The typed cloud noise, on its own asset so its features do not rhyme
	 *  with the structure's. No volume leaves the layer out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<UVolumeTexture> Volume = nullptr;

	/** Horizontal frequency at CloudBase, in noise units per radian: higher is
	 *  finer grain. It grows with height as the structure layer's does. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (UIMin = "0.01"))
	float Scale = 30.0f;

	/** Vertical frequency over the horizontal one, at every height, measured
	 *  in decks as for the structure layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", UIMin = "0.01"))
	float Aspect = 1.0f;

	/** How hard the edges are eaten, up to TR_DETAIL_EROSION_SCALE of the
	 *  density at 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Erosion = 0.6f;

	/** Share of a column's height over which the erosion turns from wispy at
	 *  the base to billowy: higher gives tall ragged bases, lower puts
	 *  cauliflower tops over almost the whole cloud. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001", UIMin = "0.05", UIMax = "0.6"))
	float BillowHeight = 0.25f;

	/** Share of the sim's carried noise displacement the layer follows. Lower
	 *  than the structure's, so sheared regions turn strandy and keep some
	 *  rounded detail. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (UIMin = "0.0", UIMax = "1.0"))
	float FlowInherit = 0.4f;

	/** The crossfade's lean to the less stretched phase, as for the structure
	 *  layer; it acts in proportion to this layer's FlowInherit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "8.0", UIMax = "4.0"))
	float StretchBias = 0.0f;

	/** The flattening by the flow's stretch, as for the structure layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", UIMax = "1.0"))
	float StretchFlatten = 0.0f;

	/** Mips added to the one the pixel's footprint reads, as for the
	 *  structure layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (UIMin = "-2.0", UIMax = "4.0"))
	float MipBias = 0.0f;

	/** Where the grain starts fading to the mean, in planet radii from the
	 *  camera: the unit of the shadow cascades' CascadeRadii. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float FadeNear = 0.0f;

	/** Where the grain reaches the mean, in planet radii; the fetch is skipped
	 *  beyond it. Only the near shadow cascade carries the grain, so keep
	 *  CascadeRadii.Y at or past this for the shadows to show it wherever the
	 *  clouds do. */
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
 *  Scatter is single-scattering albedo, non-negative: above 1 a channel
 *  scatters more light than the cloud intercepts, a brightening past physical
 *  cloud rather than an instability. CloudExtinction is an
 *  RGB tint on the solved extinction, 1 neutral; CloudOpticalDepth sets its
 *  amount. StormExtinction is a tint with the storm's amount in A, as a
 *  multiple of fair-weather cloud's. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FCloudMaterialParams
{
	GENERATED_BODY()

	/** Optical depth through a full-depth column of fair-weather cloud at
	 *  density 1: the clouds' opacity. Coverage and the noise take most columns
	 *  well under it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", UIMin = "0.1"))
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001", UIMax = "1.0"))
	float StormBlend = 0.15f;

	/** Fair-weather cloud's single-scattering albedo, per channel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor CloudScatter = FLinearColor(0.98f, 0.98f, 0.98f, 1.0f);

	/** Fair-weather cloud's extinction tint per channel, 1 neutral; its amount
	 *  is CloudOpticalDepth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor CloudExtinction = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);

	/** Storm cloud's single-scattering albedo, per channel: lower is darker
	 *  storm cloud. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor StormScatter = FLinearColor(0.9f, 0.92f, 0.95f, 1.0f);

	/** Storm cloud's extinction tint in RGB, and in A its opacity as a
	 *  multiple of fair-weather cloud's. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor StormExtinction = FLinearColor(1.0f, 1.0f, 1.0f, 2.0f);
};

/** How the march spreads its samples over pixels, frames and distance. For a
 *  performance tier, with Raymarch's step counts: one per actor, its lattice
 *  growth relative to the active model's cloud shell. */
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

	/** How much longer each cloud step is than the last, counted from the
	 *  camera in or under the deck and from the deck top above it, so any
	 *  camera above the deck steps it as one at the deck top. Lower is finer
	 *  and costlier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001", UIMax = "1.0"))
	float LatticeGrowth = 0.2f;
};

/** The deep deck beneath the slab (TR_DEEP_DECK). */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FCloudDeepParams
{
	GENERATED_BODY()

	/** How far below a column's base the deck reaches full density, as a
	 *  fraction of atmosphere thickness: coverage fills in and erosion, detail
	 *  and the eye's thinning fade out over it, on down through the core. The
	 *  marched band ends where every column is full, so a deeper fill
	 *  fine-steps further down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001", UIMin = "0.001", UIMax = "1.0"))
	float DeepFill = 0.1f;

	/** How far the floor rises into the fill, in fills: full-density mounds
	 *  shaped by the structure noise, from where the fill completes up to
	 *  FloorRelief fills above it at the highest noise; 1 reaches the base.
	 *  The floor only adds density, so the cloud above is unchanged. 0 leaves
	 *  no mounds but still a flat floor where the fill completes, so the deck
	 *  reaches full density there even where its top ramp has not. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FloorRelief = 0.5f;

	/** Depth of the floor's edge ramp, in fills: lower is harder mound edges. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001", UIMin = "0.01", UIMax = "1.0"))
	float FloorSoftness = 0.25f;

	/** Least absorption the sky's light meets as it fades into the deck below
	 *  a column's base, whatever the albedo: higher darkens the depths sooner.
	 *  Sunlight and albedo are unaffected. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0", UIMax = "0.2"))
	float Darkening = 0.01f;

	/** Depth below a column's base, in fills, over which the deck turns from
	 *  the cloud and storm material to the deep material: 0 switches at the
	 *  base. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", UIMax = "4.0"))
	float MaterialDepth = 0.25f;

	/** The deep material's single-scattering albedo, per channel, as the
	 *  cloud's. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor Scatter = FLinearColor(0.98f, 0.98f, 0.98f, 1.0f);

	/** The deep material's extinction tint in RGB, and in A its opacity as a
	 *  multiple of fair-weather cloud's. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor Extinction = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);
};

// The air, the cloud's lighting and the march's budget. THESE STRUCTS ARE DATA
// in authoring units; the helpers below and the field's builders derive what the
// shaders take. PITFALL: give no member a category. Members display in
// declaration order, and one that names a category sorts apart from its group.

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

	/** The column of Atmo_Density's absorber over the shell, by Simpson's rule on
	 *  64 intervals: a Lorentzian about AbsorptionAltitude, its half-width floored
	 *  as AtmoT_Profile floors it, times the Rayleigh profile. */
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
		return FLinearColor(FMath::Max(Depth.R, 0.0f) * Inv, FMath::Max(Depth.G, 0.0f) * Inv, FMath::Max(Depth.B, 0.0f) * Inv, 1.0f);
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001", UIMax = "1.0"))
	float AmbientTerminator = 0.15f;
};

/** The cloud's phase function: two Henyey-Greenstein lobes. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmospherePhaseParams
{
	GENERATED_BODY()

	/** Forward lobe asymmetry. What makes the rim bright near the sun. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "-0.99", ClampMax = "0.99", UIMin = "0.0"))
	float ForwardG = 0.9f;

	/** Backward lobe asymmetry, as a magnitude. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "-0.99", ClampMax = "0.99", UIMin = "0.0"))
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

	/** The light ray's extinction as a fraction of the view ray's, for march and bake. */
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

	/** Cloud steps at the lattice origin (the camera, or from above the deck
	 *  its top): the band's depth over this is the base step, growing with
	 *  distance past the origin by Sampling's LatticeGrowth, so a ray's count
	 *  varies. Capped by ATMO_MAX_ITER per segment. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "2", ClampMax = "256"))
	int32 CloudSteps = 128;

	/** Air: how much longer the last step is than the first, AtmosphereSteps
	 *  spread geometrically over the air the ray crosses; 1 is uniform, and the
	 *  count is exact, so this redistributes rather than adds. Cloud: the most a
	 *  step may rise through the band, in base steps, so a long step still
	 *  crosses the deck in several; higher is cheaper and coarser where steps
	 *  are long. Raised as needed so one crossing fits ATMO_MAX_ITER. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1.0", UIMax = "64.0"))
	float ChordSpread = 8.0f;

};

/** One model's look: every group the field, the air and the cloud's lighting
 *  read; the quality tier is the actor's. The actor holds one per model, and a
 *  group the model does not read is hidden. Declaration order is the
 *  panel's. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereModelParams
{
	GENERATED_BODY()

	/** THE MASTER SCALE: every height is a fraction of the shell it sets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FAtmospherePlanetParams Planet;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FAtmosphereAirParams Air;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FAtmosphereAmbientParams Ambient;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bClouds", EditConditionHides, HideEditConditionToggle))
	FCloudShapeParams Shape;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bClouds", EditConditionHides, HideEditConditionToggle))
	FCloudCoverageParams Coverage;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bClouds", EditConditionHides, HideEditConditionToggle))
	FCloudTypeParams Type;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bClouds", EditConditionHides, HideEditConditionToggle))
	FCloudLiftParams Lift;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bClouds", EditConditionHides, HideEditConditionToggle))
	FCloudWarpParams Warp;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bClouds", EditConditionHides, HideEditConditionToggle))
	FCloudStructureLayerParams StructureLayer;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bClouds", EditConditionHides, HideEditConditionToggle))
	FCloudDetailLayerParams DetailLayer;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bClouds && bDeepDeck", EditConditionHides, HideEditConditionToggle))
	FCloudDeepParams Deep;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bClouds", EditConditionHides, HideEditConditionToggle))
	FCloudMaterialParams Material;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bClouds", EditConditionHides, HideEditConditionToggle))
	FAtmospherePhaseParams Phase;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bClouds", EditConditionHides, HideEditConditionToggle))
	FAtmosphereMultipleScatteringParams MultipleScattering;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FAtmosphereSurfaceShadowParams SurfaceShadow;

	/** What the model draws, which decides the groups shown: clouds, and a
	 *  deep deck under them. Set by the actor from the model; transient, so
	 *  neither saves nor tunes carry them. */
	UPROPERTY(Transient)
	bool bClouds = true;

	UPROPERTY(Transient)
	bool bDeepDeck = false;
};
