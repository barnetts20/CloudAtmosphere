// The parameter sets the two-stage atmosphere post process is driven by, and
// the derivations that keep them consistent.
//
// TIERS, SPLIT BY WHOSE QUESTION IT ANSWERS. A group is SHARED when the march
// or the flow reader asks it something no model owns -- how the air scatters,
// how far a step may run, how a noise layer travels, how the phase and the
// octaves behave. It is PER MODEL when the members themselves differ: what a
// cloud field's vertical profile is, what its material looks like, what a band
// even means.
//
// SHARING A GROUP DOES NOT MAKE TWO MODELS AGREE ON VALUES. An actor is one
// planet of one type, so a shared group is one property read by whichever model
// is active; only the member LIST is common. That is why Air, Ambient and Phase
// are shared even though a cloud band would be tuned nothing like a deck.
//
// A SHARED GROUP CARRIES NO MODEL PREFIX, on the struct or on the actor
// property. A per-model one carries both, so a second model's copy sits beside
// it rather than replacing it.
//
// RATIOS, NOT ABSOLUTES, WHEREVER ONE VALUE IS BOUNDED BY ANOTHER. A parameter
// expressed against the thing that constrains it stays valid when that thing is
// retuned; expressed absolutely it silently goes out of range, and the failure
// shows up as a geometry or sampling artifact rather than as a bad value. The
// exception is the noise layers' Relief, deliberately not a ratio of the band
// relief: summing them before the relief multiply would make the noise scale
// with band thickness, and noise stretched vertically but not horizontally
// becomes spikes.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "AtmosphereParams.generated.h"

class UFlowSimConfig;

class UVolumeTexture;

/** Which cloud model the march stage uses. An enum rather than a bool: adding a
 *  case fails loudly at every switch, where a bool silently takes the false
 *  branch. */
UENUM(BlueprintType)
enum class EPlanetAtmosphereType : uint8
{
	Terrestrial,
	GasGiant
};

/** Stage 3 blur, which softens the limb against the scene behind it.
 *
 *  ONE INSTANCE FOR BOTH MODELS: the composite runs in a single material shared
 *  by both march paths, so a per-model copy would be a second value that can
 *  never reach a shader. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereCompositeParams
{
	GENERATED_BODY()

	/** Kernel radius in SOURCE pixels near the planet and far from it, eased
	 *  between by camera distance. Near, it hides the march's dither, which is
	 *  large on screen; far, the dither is sub-pixel and a wide kernel only
	 *  softens the clouds. THE COST IS QUADRATIC IN THE RADIUS: the loop is the
	 *  disc inscribed in a (2r+1) square, two fetches a tap, so 4 is about 50
	 *  taps and 8 about 200. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "16.0"))
	float MaxBlurRadius = 4.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "16.0"))
	float MinBlurRadius = 1.0f;

	/** Camera distance from the planet centre, in planet radii, at which the
	 *  radius starts easing from MaxBlurRadius, and over how far it reaches
	 *  MinBlurRadius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float BlurFadeStart = 1.25f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001"))
	float BlurFadeSpan = 1.25f;

	/** The Gaussian's width as a DIVISOR of the radius: sigma = radius / falloff.
	 *  Higher concentrates weight at the centre. At 1 the edge taps still carry
	 *  about 0.6, close enough to a box that its response hatches; 2 puts the edge
	 *  at 0.14. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.25"))
	float BlurFalloffFactor = 2.0f;

	/** How hard a depth difference cuts a tap off, so the blur cannot drag
	 *  atmosphere across a silhouette. Measured against the centre depth, so the
	 *  tolerance is relative and holds at any distance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float DepthSharpness = 5000.0f;

	/** Full-resolution pixels per source pixel: 2 for a half-per-axis buffer, 1 if
	 *  the march runs at full resolution. A PIPELINE FACT, NOT A LOOK CONTROL --
	 *  it has to match how the postprocess material is configured, and the only
	 *  symptom of getting it wrong is the depth cutoff no longer holding the
	 *  silhouette at the distance it should. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1.0"))
	float DepthTapScale = 2.0f;

	/** How far toward the blurred result the composite goes. 0 leaves the
	 *  atmosphere buffer untouched, 1 is the full kernel.
	 *
	 *  UNIFORM ACROSS THE SCREEN. A radial weight -- more blur toward the limb,
	 *  where rays are worst sampled -- blurs by where a pixel IS rather than by
	 *  what it needs. The march's step-length bounds are what hold sampling error
	 *  even, and they do it directly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BlurWeight = 1.0f;
};

/** The flow simulation both models read. ONE SIM PER WORLD: the gas giant deck
 *  reads it for flow and the terrestrial band will read it for weather, so it
 *  sits on the environment rather than on either model -- a per-model copy would
 *  be a second config the one subsystem cannot honour. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereSimulationParams
{
	GENERATED_BODY()

	/** Owns the flow render target the materials sample and the settings the sim
	 *  subsystem steps against. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<UFlowSimConfig> Config = nullptr;

	/** Start the sim on BeginPlay. Off when another actor already drives it: the
	 *  subsystem is per-world, so two planets starting it fight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bStartOnBeginPlay = true;
};

/** Cloud and deck shadows falling on whatever opaque geometry the depth buffer
 *  holds: terrain, meshes, a mesh inner surface, other actors.
 *
 *  A READ-SIDE FEATURE ENTIRELY. The map's optical depth is valid anywhere
 *  inside the shell, so a point on terrain reads the whole column above it as a
 *  deck sample reads its own. Nothing here changes the bake and no pass is
 *  added; the march evaluates it once, where the view ray stopped.
 *
 *  COMMON, NOT PER MODEL: both marches reach the map through one reader. A model
 *  without a map leaves the group at its disabled default.
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
	 *  resolution. Zero collapses a level. Terrestrial only: the gas giant sizes
	 *  its cascades from its noise layers' fades. */
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

/** Where the shell sits. Planet Center and Planet Radius come from the actor's
 *  transform, not from here; everything below is a fraction of the radius, so
 *  resizing the planet moves the whole system together. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereGeometryParams
{
	GENERATED_BODY()

	/** Atmosphere top, as a fraction of planet radius above the surface. The
	 *  ceiling every other shell in the system is expressed against. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001"))
	float HeightScale = 0.2f;

	/** Atmosphere outer radius in world units. */
	float GetAtmosphereRadius(float PlanetRadius) const
	{
		return PlanetRadius * (1.0f + HeightScale);
	}
};


// Terrestrial parameter groups.
//
// THE SIM IS THE WEATHER MAP, THE NOISE IS THE CLOUD. Coverage, cloud type and
// the pressure lid come from the sim per column; the structure layer's noise,
// shaped by a height profile, is what coverage erodes into individual clouds,
// and the detail layer erodes their edges. See TerrestrialDeck.ush.
//
// PACKED ON THE WAY OUT. Each group travels to the material and the bake as a
// few float4 pins, packed in ApplyTerrestrialModelParams and unpacked once in
// TR_BuildField; the members here keep their own names.
//
// CLOUD THICKNESS IS THE UNIT. Every height below except CloudBase is a
// multiple or share of it.

/** The planet the terrestrial field sits on. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FTerrestrialPlanetParams
{
	GENERATED_BODY()

	/** Atmosphere top, as a fraction of planet radius above the surface. The
	 *  ceiling every other shell in the system is expressed against. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001"))
	float HeightScale = 0.2f;

	/** The planet's own rotation, radians per unit of simulated time. The sim
	 *  runs in the rotating frame, so this rotates the sampling position. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float SpinRate = 0.1f;
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float TypeBias = 0.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float TypeTropical = 0.3f;

	/** How far the sim's storm deepens and darkens cloud. A term rather than a
	 *  floor, so storm grades into the cloud around it instead of turning every
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float BasePressure = -0.1f;

	/** Maps the sim's formation ascent, the vertical motion a cloud condensed
	 *  at, to an altitude in [0, 1]. The ascent in cloud typically runs 0.1 to
	 *  0.4, so about 3 spans it. */
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

	/** Horizontal frequency, in noise units per radian: higher is smaller
	 *  clouds. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.01"))
	float Scale = 6.0f;

	/** Vertical frequency against the horizontal one, over the shell: higher
	 *  is flatter features. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.01"))
	float Aspect = 2.0f;

	/** How far the noise breaks the cloud up. Below 1 some of each column fills
	 *  whatever the noise; past 1 the shaping extrapolates, holes open that
	 *  survive any coverage, and from about half up a fully covered column is
	 *  cut clear as readily as a thin one. 0 leaves smooth sheets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float Erosion = 0.85f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FlowInherit = 1.0f;

	/** Offset to the mip the volume is read at, from the march pixel's
	 *  footprint. 1 accounts for the half-resolution march; lower is sharper
	 *  and shimmers in motion. The volume needs its mips. */
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.01"))
	float Scale = 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.01"))
	float Aspect = 1.0f;

	/** How hard the edges are eaten, up to TR_DETAIL_EROSION_SCALE of the
	 *  density at 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Erosion = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FlowInherit = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "-2.0", ClampMax = "4.0"))
	float MipBias = 1.0f;

	/** Where the grain starts fading to the mean, and over how far, in noise
	 *  features from the camera: multiples of 1 / Scale planet radii, since the
	 *  distance a layer starts aliasing at goes as its feature size. The fetch is
	 *  skipped beyond. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float FadeStart = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float FadeLength = 9.0f;

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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor CloudScatter = FLinearColor(0.98f, 0.98f, 0.98f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor CloudExtinction = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor StormScatter = FLinearColor(0.9f, 0.92f, 0.95f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor StormExtinction = FLinearColor(1.0f, 1.0f, 1.0f, 2.0f);
};

// Gas giant parameter groups.
//
// ONE STRUCT PER PANEL GROUP, declared on the actor as one property and inlined
// with ShowOnlyInnerProperties into the category it names. PITFALL: do not give
// a member here a category. A member that names one is pulled out of its
// struct's group, and category-less members then sort after every member that
// has one.
//
// NAMES MATCH THE STACK. Each member's name is its material parameter, its
// Custom node pin and its shader term; the noise layers' members take their
// layer's prefix there (StructureScale, DetailScale), and bools drop their b.
// These structs are data -- every derived quantity is computed once, in
// GG_BuildField and GG_DeckBeta, which the march and the shadow bake share. The
// Solved readouts are the exception, filled by the actor for display and never
// pushed.
//
// UNITS. HeightScale is a fraction of planet radius; every deck height is a
// fraction of atmosphere thickness and every relief amount a fraction of
// GradientThickness. Fade distances are planet radii.

/** The air: how much of it there is, its colour, and how it scatters.
 *
 *  EACH DEPTH IS A VERTICAL COLUMN, the optical depth per channel from the
 *  ground to the shell top, so its colour is the air's and its amount how
 *  thick the air reads. The scale heights only shape how the column is spread
 *  with altitude. The *Beta() helpers give the per-thickness coefficients the
 *  material takes, which Atmo_BuildParams divides by atmosphere thickness. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereAirParams
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor RayleighDepth = FLinearColor(4.774597f, 9.999406f, 12.8395f, 1.0f);

	/** As a fraction of atmosphere thickness. Under about half the deck's top
	 *  there is no Rayleigh above the cloud and the limb reads as a hard edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001"))
	float RayleighScaleHeight = 0.45f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor MieDepth = FLinearColor(2.454211f, 2.191878f, 1.855216f, 1.0f);

	/** A deck whose peaks reach most of the way up the shell needs aerosol still
	 *  present above them. */
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

	/** The air's ambient, as a ratio of the star's light per channel. Near-black
	 *  under a deck opaque to the limb, with no lit surface under the air to
	 *  bounce anything up. */
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float CloudAmbientFloor = 0.001f;

	/** Width of the ambient terminator, in cosine of sun elevation; 0.15 is about
	 *  9 degrees either side. Ambient applied unconditionally washes the night
	 *  side, which reads as the star shining through the planet. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001", ClampMax = "1.0"))
	float AmbientTerminator = 0.15f;
};

/** The deck's vertical profile: where it hangs and how density ramps into it.
 *  THE GRADIENT HANGS FROM EACH COLUMN'S OWN TOP rather than stretching between
 *  two anchors, so relief moves the profile instead of deforming it and every
 *  column shades alike. DeckBackstop stops it below, which keeps the marched
 *  band fixed however deep relief cuts. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FGasGiantProfileParams
{
	GENERATED_BODY()

	/** Where an unrelieved column's top sits, as a fraction of atmosphere
	 *  thickness. Relief shapes the deck around it and never moves it as a whole,
	 *  so every relief control is independent of deck altitude. PITFALL: keep it
	 *  below 1 - CeilingFalloff, since a typical top inside the ceiling band thins
	 *  the whole deck and DeckOpticalDepth stops being exact. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DeckTop = 0.85f;

	/** Width of the band under the shell top across which density fades to zero,
	 *  as a fraction of atmosphere thickness. WHAT LETS RARE FEATURES REACH THE
	 *  SHELL: a storm tower that would cross it flattens into a soft cap instead
	 *  of being cut, so the deck never sits lower to make room for its tallest
	 *  outlier. Wider gives rounder domes, narrower flatter caps. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001", ClampMax = "0.5"))
	float CeilingFalloff = 0.05f;

	/** How far the density gradient reaches below a column's own top, and the unit
	 *  every relief amount is a fraction of. THE GRAIN HANDLE: widen it and the
	 *  deck top spreads over more march steps. Relief scales with it, so widening
	 *  also raises the bands -- the deck getting deeper, not a side effect. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001", ClampMax = "1.0"))
	float GradientThickness = 0.3f;

	/** Backstop under the gradient: no column's density ramp reaches below this,
	 *  however low relief takes its top. ALSO THE MARCHED BAND'S LOWER EDGE --
	 *  columns topping out above this plus GradientThickness get the full uniform
	 *  span and the rest compress toward a step, so lowering it buys uniformity in
	 *  the troughs and widens the fine band one for one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DeckBackstop = 0.3f;

	/** Bound on the deck's slope, in gradient depths per radian: the cone angle for
	 *  the entry search. Under-declaring it is the one way that search steps over
	 *  the surface, so raise it first if tangent-angle slicing appears. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.1"))
	float DeckSlope = 8.0f;

	/** READOUT, not authored: the highest every relief term together could reach,
	 *  before the ceiling. Above 1 - CeilingFalloff the tallest features are being
	 *  capped by the band, past 1 by the excess shown. */
	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly)
	float SolvedTopMax = 0.0f;

	/** Total optical depth from the deck top to the surface at core density, down
	 *  an unrelieved column, at any DensityCurve. Below about 8 the sky shows
	 *  through. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.1"))
	float DeckOpticalDepth = 2000.0f;

	/** Where the mass sits inside the gradient, without moving either boundary. 1
	 *  is centred, below 1 pulls density toward the top. DeckOpticalDepth is solved
	 *  against the curve's mean, so this redistributes mass without changing how
	 *  much light the deck removes. ABOVE 0.5 THE ONSET IS C1: at 0.5 the slope at
	 *  the deck top goes finite and creases along the whole top, below it the top
	 *  hardens into an edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001", ClampMax = "4.0"))
	float DensityCurve = 1.0f;
};

/** What the flow field does to a cloud field's shape: pressure, storms and the
 *  planet's own rotation. Every relief amount is a fraction of GradientThickness.
 *
 *  GAS GIANT ONLY: the terrestrial field reads the sim as a weather map and
 *  keeps its own handles in its Lift and Planet groups.
 *
 *  THE HEMISPHERE PAIR IS HERE RATHER THAN THERE. The sim's channels are
 *  rotation senses and which sense is cyclonic flips at the equator, so every
 *  reader of those channels needs the flip handled -- FlowField.ush's
 *  FlowReadParams takes both, whatever field supplies them. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereFlowParams
{
	GENERATED_BODY()

	/** Half-width of the equatorial blend, in DEGREES of latitude. The sim's
	 *  channels are rotation senses, and which sense is cyclonic flips at the
	 *  equator, so a band either side of it has no clear sense at all and the
	 *  band and pressure relief wash out across it. This is how wide that band
	 *  reads. Cannot be zero: a hard switch seams along the equator. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.05", ClampMax = "45.0"))
	float HemisphereBlend = 3.0f;

	/** How far local vorticity widens that band, as a multiple of it at full
	 *  normalized vorticity. The sign flip is exactly the equator and stays so;
	 *  this varies only the WIDTH, which is what keeps a vortex straddling the
	 *  equator from being bisected along a fixed latitude however strong it is.
	 *  0 gives a band of constant width, which reads as a perfect annulus. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "8.0"))
	float HemisphereVariance = 2.0f;

	/** How far pressure lifts the deck. Anticyclones rise, cyclones sink. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float PressureRelief = 0.1f;

	/** Vortex strength above which storm towers are allowed. Below 1: the gate
	 *  ramps from here to 1, and equal ends make it undefined. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float VortexThreshold = 0.9f;

	/** How far the strongest vortices move the deck, where VortexThreshold's gate
	 *  is open and the structure volume's storm channel peaks. Follows the pressure
	 *  sign -- towers over anticyclones, funnels into cyclones, negative swapping
	 *  them -- and is centred on zero, so it never moves the deck as a body. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float StormTowerRelief = 0.1f;

	/** How far the flow's strain collapses band relief toward flat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float ReliefThinning = 0.0f;

	/** The planet's own rotation, radians per unit of simulated time. The sim runs
	 *  in the rotating frame, so this rotates the sampling position, not the
	 *  flow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float RotationWeight = 0.0f;
};

/** How a BANDED planet reads the flow: where zones and belts sit and how far
 *  apart they stand. Gas giant only -- a field without banded material has no
 *  use for any of it, and the band coordinate it produces means something
 *  different wherever it exists at all. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FGasGiantBandShapeParams
{
	GENERATED_BODY()

	/** Multiplies already-normalized vorticity, so 1 is neutral and the useful
	 *  range is roughly 0.5 to 3. Too high flattens elevation to its asymptote
	 *  everywhere but the boundaries: terraces joined by cliffs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float BandSharpness = 1.0f;

	/** Shifts which band type dominates without retuning the sim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float BandBias = 0.3f;

	/** Height of zones above belts, a fraction of GradientThickness: each moves
	 *  half of it from DeckTop, zones up and belts down, meeting at DeckTop on the
	 *  band boundaries. Positive lifts the anticyclonic zones. Bands are geometry
	 *  rather than a pattern painted on a sphere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float BandRelief = 0.3f;
};

/** How the gas giant's noise layers travel with the flow. Each layer's
 *  FlowInherit and ShearInherit say how much of it that layer follows. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereMotionParams
{
	GENERATED_BODY()

	/** Warp duration: a short advection through the top flow layer, whose only job
	 *  is to carry the volumes along the current flow. The history lives in the
	 *  sim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float WarpTime = 0.1f;

	/** Length of the deep flow layer's step, as a fraction of WarpTime: the
	 *  vertical wind shear between the two sim layers. Each layer's ShearInherit
	 *  says how much of it that layer follows. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float DeepShearRatio = 1.0f;

	/** How much of the warp survives in band interiors, against full strength
	 *  at the edges where the shear lives. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float TurbulenceFloor = 0.5f;

	/** How long a crossfade phase takes, in SIMULATED seconds -- the clock the
	 *  flow runs on, so the noise keeps pace at any sim speed. A SPEED CONTROL,
	 *  NOT A DURATION, since how far a phase travels is fixed by the warp: long
	 *  periods separate the phases further and ghost harder in shear, short ones
	 *  show the dissolve more often. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.1"))
	float CrossfadePeriod = 10.0f;
};

/** Controls both gas giant noise layers' carves share. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereCarveParams
{
	GENERATED_BODY()

	/** How much of either layer survives in flat band interiors, against full
	 *  strength at the edges where a real gas giant's billows live. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float EdgeBias = 1.0f;

	/** How far down the gradient erosion reaches, as a fraction of it: 0 surface
	 *  only, 1 into the saturated region. Normalized per column, so erosion
	 *  finishes exactly where that column saturates. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ErosionDepth = 1.0f;
};

/** One gas giant noise layer: Structure for the shape that reads from orbit,
 *  Detail for the variance gone within a fraction of a planet radius. The
 *  terrestrial layers have their own structs. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereNoiseLayerParams
{
	GENERATED_BODY()

	/** R a Perlin FBM, GBA a Worley octave ladder on separate seeds, all
	 *  median-centred and equalized per channel. The two layers take separate
	 *  assets so their features do not rhyme. SAMPLER: wrap on all three axes,
	 *  Linear Color -- an sRGB decode curves these scalar fields without
	 *  erroring. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<UVolumeTexture> Volume = nullptr;

	/** Worley rung weights coarse to fine in RGB, the layer's amount in A. RGB IS
	 *  A DIRECTION, NOT THREE LEVELS: the rungs are renormalized by their length
	 *  shader-side, so only the balance carries meaning, which is what a colour
	 *  picker navigates. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor NoiseWeights = FLinearColor(1.0f, 0.5f, 0.25f, 1.0f);

	/** Horizontal feature size, in noise units per radian. Higher tiles finer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.01"))
	float Scale = 1.0f;

	/** Vertical feature size against the horizontal one. 1 is isotropic at any
	 *  shell thickness. PITFALL: an exaggerated aspect does not read as tall
	 *  noise -- the UVW scale swings across the shell, so the pattern rescales with
	 *  sample altitude and swims as step size grows with distance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.01"))
	float Aspect = 1.0f;

	/** How far the layer moves the deck top, a fraction of GradientThickness.
	 *  Signed about the noise median for structure, one-sided for detail unless
	 *  GG_DETAIL_RELIEF_CENTRED; negative mirrors. Independent of BandRelief. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float Relief = 0.5f;

	/** How much the layer erodes the density. Below 1, since there is no lower
	 *  cloud shell and a transparent column would let a ray run to the far side of
	 *  the planet. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float Erosion = 0.9f;

	/** How much of the top-layer warp the layer follows. A displacement field with
	 *  a large gradient IS strain, so 1 imposes an order-one strain regardless of
	 *  the layer's own flow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FlowInherit = 0.5f;

	/** How much of the deep-layer shear the layer follows, on top of FlowInherit's
	 *  share of the top-layer warp. See DeepShearRatio. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ShearInherit = 0.0f;

	/** Where the layer starts fading, in planet radii from the camera. A FADE
	 *  BELONGS WITH ITS SCALE: the distance at which a layer starts aliasing goes
	 *  as one over its Scale, so retune them together. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float FadeNear = 0.0f;

	/** Fade width, added to FadeNear. The layer is skipped entirely beyond. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float FadeSpan = 0.5f;

	/** How far the layer's shapes carry material across a band boundary; at 1 a
	 *  full-strength shape moves the boundary by about its own width. SIGNED:
	 *  positive gives rising shapes the Positive band, negative the Negative. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float BandMix = 0.0f;

	/** Two phases of the warp dissolved into each other, so the noise travels
	 *  instead of wobbling in place. A second volume fetch. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bCrossfade = false;

	static FAtmosphereNoiseLayerParams MakeStructureDefaults()
	{
		FAtmosphereNoiseLayerParams Out;
		Out.Scale = 1.0f;
		Out.Aspect = 12.0f;
		Out.Relief = 0.5f;
		Out.Erosion = 0.9f;
		Out.FlowInherit = 0.7f;
		Out.ShearInherit = 0.7f;
		Out.FadeNear = 0.15f;
		Out.FadeSpan = 0.5f;
		Out.bCrossfade = false;
		return Out;
	}

	static FAtmosphereNoiseLayerParams MakeDetailDefaults()
	{
		FAtmosphereNoiseLayerParams Out;
		Out.Scale = 12.0f;
		Out.Aspect = 3.0f;
		Out.Relief = 0.15f;
		Out.Erosion = 0.9f;
		Out.FlowInherit = 0.3f;
		Out.ShearInherit = 0.0f;
		Out.FadeNear = 0.0f;
		Out.FadeSpan = 0.15f;
		Out.bCrossfade = true;
		return Out;
	}
};

/** Per-band material: what each band scatters, what it removes, and how sharply
 *  bands meet. Negative is cyclonic, Positive anticyclonic, Base the boundaries
 *  where vorticity crosses zero.
 *
 *  Scatter is single-scattering albedo. Extinction is RGB tint with the amount in
 *  A, multiplying the deck's solved extinction, 1 neutral -- "darker bands eat
 *  more light" is one amount against another. The view ray applies each band's
 *  tint and amount where it samples; the light ray takes the amounts from the
 *  shadow map and the tint of the band it arrives at. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FGasGiantBandParams
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor ScatterNegative = FLinearColor(0.11422f, 0.200265f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor ExtinctionNegative = FLinearColor(1.0f, 0.969f, 0.938f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor ScatterPositive = FLinearColor(0.136704f, 1.0f, 0.994174f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor ExtinctionPositive = FLinearColor(1.0f, 0.969f, 0.938f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor ScatterBase = FLinearColor(1.0f, 0.0f, 0.127569f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor ExtinctionBase = FLinearColor(1.0f, 0.969f, 0.938f, 1.0f);

	/** Where the band ramp saturates. Matching the inverse of the sim debug view's
	 *  DebugScale makes the two agree about where boundaries are. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float BandScale = 2.0f;
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

	/** The per-octave factors the material takes. */
	float OctaveAttenuation() const { return FMath::Clamp(ScatteringGlow, 0.0f, 1.0f); }
	float OctaveEccentricity() const { return 1.0f - FMath::Clamp(ScatteringSpread, 0.0f, 1.0f); }
};

/** The gas giant's planet shadow: softness shapes the shadow the air and the
 *  deck are read through, and the lobe power keeps the forward peaks from
 *  leaking past it. The terrestrial planet's shadow is a fixed step. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereTerminatorParams
{
	GENERATED_BODY()

	/** Width of the planet-shadow falloff, as a fraction of atmosphere thickness,
	 *  standing in for a grazing sun ray crossing progressively more air without
	 *  marching it. WIDE, because this is what shapes the terminator: narrowing it
	 *  to stop the forward lobe leaking makes the terminator hard and exposes the
	 *  smoothstep's endpoints. LobeShadowPower is the control for the leak. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001"))
	float TerminatorSoftness = 0.35f;

	/** Exponent on the planet shadow for the ANISOTROPIC terms only. The forward
	 *  lobe is single-scattered direct light, genuinely hard-shadowed behind a
	 *  planet, and a residual few percent times a lobe gain near 95x washes the
	 *  disc. A power on the same smoothstep, so no new endpoint appears; 6 takes
	 *  10% residual to 1e-6. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float LobeShadowPower = 6.0f;
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
