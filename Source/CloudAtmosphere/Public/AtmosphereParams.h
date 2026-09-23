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
// is active; only the member LIST is common. That is why Motion, Phase and
// Terminator are shared even though a cloud band would be tuned nothing like a
// deck.
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

class AActor;

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

	/** Kernel size in SOURCE pixels. THE COST IS QUADRATIC IN THIS: the loop is
	 *  the disc inscribed in a (2r+1) square, two fetches a tap, so 6 is about 113
	 *  taps and 8 about 197. It mostly pays to hide the march's sampling noise
	 *  rather than to upsample, so anything that quiets the march lets this come
	 *  down -- the cheapest place in the chain to get frames back. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0", ClampMax = "16"))
	int32 BlurRadius = 6;

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

/** Opaque geometry casting into the deck shadow map: moons, hanging objects,
 *  terrain, a mesh inner surface. One orthographic depth capture per cascade,
 *  each sized and centred on that cascade, feeding the bake one occluder depth
 *  per texel ray.
 *
 *  PARKED. IsEnabled answers false whatever is authored, and the group is no
 *  longer exposed on the actor. Restoring the feature means returning bEnabled
 *  from IsEnabled and putting the EditAnywhere specifier back on
 *  APlanetAtmosphereActor::GasGiantOccluderShadows; nothing else was removed.
 *
 *  COST IS THE SCENE, NOT THE BAKE. Each enabled level runs the scene's depth
 *  pass for its own view every time it captures, while the bake gets CHEAPER on
 *  occluded rays because the march stops at the occluder. The cadences and the
 *  disc switch are the levers; nothing here changes the map's format or what the
 *  march does with it. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FGasGiantOccluderShadowParams
{
	GENERATED_BODY()

	/** Off destroys the capture components and their targets, and the bake runs
	 *  exactly as it does without this feature. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bEnabled = false;

	// Which levels capture. A level that is off binds nothing and reports
	// invalid, so a ray it would have covered falls through to the next coarser
	// level that is on -- which is how one level is isolated during bring-up.
	//
	// The disc is the most expensive of the three, being a planet-sized depth
	// pass, and only eclipse-scale occluders need it.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled"))
	bool bCaptureDisc = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled"))
	bool bCaptureStructure = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled"))
	bool bCaptureDetail = true;

	/** Renders the captures as LDR COLOUR instead of depth, so the targets show
	 *  what each capture actually frames. The depths are then meaningless and
	 *  every level reports invalid, so the bake ignores them and the deck
	 *  shadows alone remain.
	 *
	 *  This answers the first question any missing shadow raises -- whether the
	 *  capture is rendering at all and whether it is pointed at the planet --
	 *  which a depth target cannot, its values running to 1e8 and displaying as
	 *  flat white. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled"))
	bool bDebugColorCapture = false;

	// Frames between captures per level, 1 being every frame. A level holds its
	// last capture and the frame it rendered with between refreshes, so a stale
	// capture is placed correctly and only late.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled && bCaptureDisc", ClampMin = "1"))
	int32 DiscIntervalFrames = 8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled && bCaptureStructure", ClampMin = "1"))
	int32 StructureIntervalFrames = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled && bCaptureDetail", ClampMin = "1"))
	int32 DetailIntervalFrames = 1;

	/** How far the capture plane sits off the planet, as a multiple of the outer
	 *  shell. THE NEAR PLANE IS AT THE CAPTURE, so this is the ceiling on what
	 *  can cast: anything farther from the planet centre along the light is
	 *  clipped and casts nothing, and an object STRADDLING the plane loses its
	 *  near cap, which shrinks its shadow to whatever rim still sits below.
	 *
	 *  Raise it to cover moons and high orbits. The cost is depth range, not
	 *  resolution -- the capture's width does not change with it, because the
	 *  projection is orthographic. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled", ClampMin = "1.1"))
	float CaptureDistanceScale = 4.0f;

	/** Radius of the footprint each ray samples the capture over, in ATMOSPHERE
	 *  THICKNESSES. This is what EdgeInset erodes against, and it must match
	 *  GG_SHADOW_OCCLUDER_BLUR in GasGiantShadow.ush, which pushes the edge back
	 *  out by the same distance on the read side. Eroding and blurring by one
	 *  width leaves the edge where it was and only softens it.
	 *
	 *  A DISTANCE, NOT TEXELS. A texel spans an order of magnitude more ground
	 *  on the disc slice than on the detail slice, so a width in texels gives
	 *  each cascade a differently sized shadow and the walk between them steps
	 *  outward at every boundary.
	 *
	 *  PITFALL: the lattice is still the floor. Asking for less than a cascade
	 *  can resolve leaves that cascade at its own texel width, so the coarse
	 *  slices stay slightly wider however this is set. Closing that gap means
	 *  bringing the fade radii, which size the cascades, closer together. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "0.5"))
	float EdgeWidth = 0.002f;

	/** How much of the footprint must be covered before a texel shadows at all,
	 *  which pulls the edge inward.
	 *
	 *  EVERY STAGE SPREADS OUTWARD: the footprint is centred on the texel, the
	 *  texel is reconstructed across its neighbours, and the occluder term is
	 *  filtered again on the way out. Left uncorrected they leave a rim of
	 *  shadow outside the object, which reads as a halo when the view looks down
	 *  the light and the rest of the shadow hides behind the object.
	 *
	 *  HALF IS NEUTRAL, NOT INWARD. The footprint is centred on the texel, so
	 *  requiring half of it reproduces the true silhouette; below that the edge
	 *  dilates. Inward bias starts above 0.5 and is total at 1, where only a
	 *  fully covered texel shadows.
	 *
	 *  The trade is detachment where object meets surface -- the same bargain a
	 *  depth bias makes against shadow acne. Raise it until the halo goes, not
	 *  further. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "0.95"))
	float EdgeInset = 0.85f;

	/** Optical depth a fully covered texel adds, on the same scale the deck's
	 *  own thresholds use: 1 is 63% extinction, 3 is 95%, 5 is what saturated
	 *  cloud reads as. Higher goes darker than any cloud can, which is what
	 *  makes a solid object read solid rather than merely thick. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "50.0"))
	float Strength = 10.0f;

	/** How far behind a blocker its shadow decays to nothing, in atmosphere
	 *  thicknesses. Zero never decays.
	 *
	 *  The distance from blocker to receiver IS the light path between them, so
	 *  this reads as scattered light filling the shadow back in: darkest
	 *  directly under an object, gone once the deck is far enough below.
	 *
	 *  PITFALL: it grades along the LIGHT RAY, not from the object in space. At
	 *  the terminator a shadow stretched toward the night side fades along its
	 *  length while the object has not moved. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled", ClampMin = "0.0"))
	float FalloffDistance = 0.0f;

	/** View distance cap per capture, as a fraction of its FAR PLANE rather than
	 *  of its width: the plane sits well off the planet, so a cap measured
	 *  against a narrow level's own extent would cull the deck itself. 1 culls
	 *  exactly where the far plane does; 0 leaves the engine default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled", ClampMin = "0.0", ClampMax = "1.0"))
	float MaxViewDistanceScale = 1.0f;

	/** Excluded from every capture. For anything that renders opaque depth but
	 *  should not shadow the deck -- a skybox shell, a visual proxy for the
	 *  planet itself. The atmosphere actor hides its own children regardless. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled"))
	TArray<TObjectPtr<AActor>> HiddenActors;

	/** Whether the feature runs at all. THE ONLY GATE: the capture components and
	 *  the shadow target's slice count both hang off it, so a false here leaves
	 *  the bake and the reader exactly as they are without the feature. */
	bool IsEnabled() const
	{
		return false;
	}

	/** Frames between captures for a cascade index, 0 being the disc. */
	int32 GetIntervalFrames(int32 Level) const
	{
		if (Level <= 0)
		{
			return FMath::Max(DiscIntervalFrames, 1);
		}

		return FMath::Max(Level == 1 ? StructureIntervalFrames : DetailIntervalFrames, 1);
	}

	/** Whether a cascade index captures at all. */
	bool IsLevelEnabled(int32 Level) const
	{
		if (!IsEnabled())
		{
			return false;
		}

		if (Level <= 0)
		{
			return bCaptureDisc;
		}

		return Level == 1 ? bCaptureStructure : bCaptureDetail;
	}
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

	/** Distance along the light the receiver is lifted before the map is
	 *  sampled, in ATMOSPHERE THICKNESSES.
	 *
	 *  FOR THE OCCLUDER BAND ONLY. A captured surface holds its own depth in the
	 *  occluder slices, so an unbiased receiver reads the occluder's full peak
	 *  and shadows itself everywhere. The deck's crossings sit above the receiver
	 *  and need none, so with captures off this can be zero.
	 *
	 *  PITFALL: it must clear the occluder lattice's depth quantisation across
	 *  one texel -- texel width times slope, kilometres on the disc cascade, so
	 *  not a small number. Too much detaches shadows from the ground at the
	 *  terminator, where a lift along a grazing light travels far laterally.
	 *  Tune there, not at noon. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled", ClampMin = "0.0"))
	float ReceiverBias = 0.0f;

	/** Final multiplier on the optical depth read from the map, for art control
	 *  independent of the physical terms. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (EditCondition = "bEnabled", ClampMin = "0.0"))
	float Strength = 1.0f;

	/** The single vector parameter the march unpacks, four related scalars on
	 *  one Custom node pin. GGAtmo_BuildAtmo mirrors this layout. */
	FLinearColor Pack() const
	{
		return FLinearColor(bEnabled ? 1.0f : 0.0f, DirectFraction, ReceiverBias, Strength);
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

/** Where the clouds sit and how their columns are shaped. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FTerrestrialProfileParams
{
	GENERATED_BODY()

	/** Condensation level of an unlifted column, as a fraction of atmosphere
	 *  thickness. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CloudBase = 0.15f;

	/** Depth of a fully towering column before the pressure lid, as a fraction of
	 *  atmosphere thickness. PITFALL: keep CloudBase plus this below
	 *  1 - CeilingFalloff, or the ceiling thins every tall column. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001", ClampMax = "1.0"))
	float CloudThickness = 0.5f;

	/** Share of CloudThickness each end of the height profile ramps over. At 0.5
	 *  a full column has no flat core; at 0 both ends are hard. Also sets the
	 *  bake's step. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "0.5"))
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

	// -- Coverage and type ------------------------------------------------------

	/** Global coverage. 0.5 leaves the sim's weather as it is; lower clears the
	 *  sky, higher closes it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CloudCover = 0.5f;

	/** How strongly the sim's cloud field maps to coverage. Raise it until the
	 *  sim's cloudiest systems read as overcast. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float CoverageGain = 2.5f;

	/** Depth of a stratiform column as a fraction of a towering one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StratusDepth = 0.25f;

	/** Cloud type, 0 stratiform to 1 towering, is TypeBias + TypeAscent * rising
	 *  air + TypeTropical * tropicality. Type sets column depth and blends the
	 *  material from fair-weather to storm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float TypeBias = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float TypeAscent = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float TypeTropical = 0.3f;

	// -- Breakup --------------------------------------------------------------

	/** Multiplier on the structure layer's Erosion. Above 1 the noise cuts holes
	 *  through fully covered columns, giving broken fields at high coverage;
	 *  below 1 it smooths toward sheets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float ErosionGain = 1.0f;

	/** How far vertical motion moves the erosion: positive smooths rising air
	 *  into sheets and breaks sinking air into patches. The column's erosion is
	 *  Erosion * ErosionGain * (1 - ErosionAscent * rising air), floored at 0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float ErosionAscent = 0.0f;

	// -- Pressure -------------------------------------------------------------

	/** The sim pressure that reads as a full low or high. Raise it until the lid
	 *  varies across a system rather than switching at its edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.000001"))
	float PressureScale = 0.5f;

	/** Headroom as a multiple of CloudThickness, and how far pressure moves it:
	 *  positive gives a low more room than a high. THE MARCHED BAND IS BOUNDED BY
	 *  THIS, so it also sets how much of the shell gets fine-stepped. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001"))
	float CeilingDepth = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "-0.99", ClampMax = "0.99"))
	float CeilingPressure = 0.5f;

	/** How far tropicality and pressure move the base, multiples of
	 *  CloudThickness: a higher condensation level in the tropics, lower under
	 *  lows. Nothing else moves the base, which keeps it exact per column. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float BaseTropical = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float BasePressure = -0.1f;

	// -- Vertical warp ----------------------------------------------------------

	/** How far rising air stretches the noise vertically: towers drawn taller,
	 *  subsiding air pressed into sheets. Noise only; the bounds are unaffected. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "-0.9"))
	float WarpStretch = 0.5f;

	/** How far rising air lifts the noise, a multiple of CloudThickness. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float WarpShift = 0.2f;

	// -- March ----------------------------------------------------------------

	/** Bound on either surface's slope, in cloud depths per radian: the cone angle
	 *  for the entry search. Under-declaring it is the one way that search steps
	 *  over cloud, and the symptom is cloud missing on grazing rays. Raise it
	 *  first. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.1"))
	float CloudSlope = 60.0f;

	/** Optical depth through a full-depth column at density 1. Coverage and the
	 *  noise take most columns well under it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.1"))
	float CloudOpticalDepth = 40.0f;

	/** READOUT, not authored: the highest a column top can reach. Above
	 *  1 - CeilingFalloff the tallest columns are being capped. */
	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly)
	float SolvedTopMax = 0.0f;

	/** READOUT, not authored: the lowest a column base can fall, where the
	 *  marched band ends. */
	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly)
	float SolvedBaseMin = 0.0f;
};

/** How the noise travels with the flow. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FTerrestrialMotionParams
{
	GENERATED_BODY()

	/** Duration of the short advection through the flow that carries the noise
	 *  along the current wind. The history lives in the sim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float WarpTime = 0.2f;

	/** Length of the deep flow layer's step as a fraction of WarpTime: the
	 *  vertical wind shear each layer's ShearInherit follows. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float DeepShearRatio = 0.5f;

	/** How long a crossfade phase takes, in simulated time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.1"))
	float CrossfadePeriod = 10.0f;

	/** The planet's own rotation, radians per unit of simulated time. The sim
	 *  runs in the rotating frame, so this rotates the sampling position. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float RotationWeight = 0.1f;
};

/** The clouds' material: fair-weather cloud at type 0, storm cloud at type 1,
 *  blended by type. Scatter is single-scattering albedo; Extinction is RGB tint
 *  with the amount in A, multiplying the solved extinction, 1 neutral. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FTerrestrialCloudMaterialParams
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor CloudScatter = FLinearColor(0.98f, 0.98f, 0.98f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite)
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

/** The air: its colour, how it scatters, and how it shadows.
 *
 *  Coefficients are divided by atmosphere thickness in Atmo_BuildParams, so
 *  they are thickness-relative and survive a resize. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereLightingParams
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor RayleighBeta = FLinearColor(11.899769f, 24.921608f, 32.0f, 1.0f);

	/** As a fraction of atmosphere thickness. Under about half the deck's top
	 *  there is no Rayleigh above the cloud and the limb reads as a hard edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001"))
	float RayleighScaleHeight = 0.45f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor MieBeta = FLinearColor(10.0f, 8.93109f, 7.559319f, 1.0f);

	/** A deck whose peaks reach most of the way up the shell needs aerosol still
	 *  present above them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001"))
	float MieScaleHeight = 0.25f;

	/** Mie asymmetry. Rayleigh has no counterpart: its phase is fixed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "-0.99", ClampMax = "0.99"))
	float MieG = 0.95f;

	/** A Lorentzian layer, ozone-like, rather than a profile falling off from
	 *  the ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor AbsorptionBeta = FLinearColor(100.0f, 80.995651f, 87.670341f, 1.0f);

	/** Altitude the absorber layer is centred on, as a fraction of thickness. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float AbsorptionAltitude = 0.15f;

	/** Half-width of the absorber layer. Floored, as AtmoT_Profile floors it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001"))
	float AbsorptionFalloff = 0.1f;

	/** Stands in for light that bounced several times. Near-black, the deck being
	 *  opaque to the limb: no lit surface under the air to bounce anything up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor AtmosphereAmbient = FLinearColor(0.0001f, 0.0001f, 0.0001f, 1.0f);

	/** What the ambient's terminator falloff lerps from. At zero the term has
	 *  almost no range, being swamped by direct light everywhere it is not zero;
	 *  the floor buys it a night side. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float AtmosphereAmbientFloor = 0.0001f;
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
};

/** What the flow field does to a cloud field's shape: pressure, storms and the
 *  planet's own rotation. Every relief amount is a fraction of GradientThickness.
 *
 *  GAS GIANT ONLY: the terrestrial field reads the sim as a weather map and
 *  keeps its own handles in FTerrestrialProfileParams and
 *  FTerrestrialMotionParams.
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

	/** Vortex strength above which storm towers are allowed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
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

/** One noise layer. Each model has two with the same handles: Structure for the
 *  shape that reads from orbit, Detail for the variance gone within a fraction
 *  of a planet radius.
 *
 *  THE TERRESTRIAL FIELD READS A SUBSET. Its structure layer is the cloud shape
 *  coverage erodes, with Erosion as how much the noise shapes it; its detail
 *  layer erodes edges, with Erosion as how hard. Relief and BandMix are gas
 *  giant only. */
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

/** How much light a cloud field removes, and where inside its gradient the mass
 *  that removes it sits. The TOTAL is the model's, since what it is measured
 *  across differs; these two do not. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereExtinctionParams
{
	GENERATED_BODY()

	/** Where the mass sits inside the gradient, without moving either boundary. 1
	 *  is centred, below 1 pulls density toward the top. DeckOpticalDepth is solved
	 *  against the curve's mean, so this redistributes mass without changing how
	 *  much light the deck removes. ABOVE 0.5 THE ONSET IS C1: at 0.5 the slope at
	 *  the deck top goes finite and creases along the whole top, below it the top
	 *  hardens into an edge. A look control rather than a bounded one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001", ClampMax = "4.0"))
	float DensityCurve = 1.0f;

	/** Light-ray extinction as a fraction of the view ray's. Below 1, since light
	 *  scattered INTO the ray is what multiple scattering stands in for and the
	 *  full coefficient counts that loss twice. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float LightExtinctionFraction = 0.5f;
};

/** The deck's phase function and its ambient. */
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

	/** The deck's ambient colour, with its own floor since the deck and the air go
	 *  dark at different rates. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (HideAlphaChannel))
	FLinearColor CloudAmbient = FLinearColor(0.0001f, 0.0001f, 0.0001f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float CloudAmbientFloor = 0.001f;
};

/** Octaves after Wrenninge: octave i sees the deck toward the light at
 *  Attenuation^i of its optical depth, weighs Contribution^i, and uses the phase
 *  with its g scaled by Eccentricity^i. Later octaves reach deeper and scatter
 *  more broadly -- the glow inside thick cloud, and a softer rim. One exp per
 *  octave per deck sample. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereMultipleScatteringParams
{
	GENERATED_BODY()

	/** Octaves including single scattering. 1 is single scattering only. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1", ClampMax = "4"))
	int32 OctaveCount = 3;

	/** Optical-depth factor per octave. Lower lets later octaves reach deeper. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OctaveAttenuation = 0.6f;

	/** Weight factor per octave. Thin cloud brightens by the sum of the weights,
	 *  every octave seeing it at full transmittance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OctaveContribution = 0.6f;

	/** Phase anisotropy factor per octave. Lower makes later octaves more isotropic
	 *  and releases them from the lobe shadow in proportion. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float OctaveEccentricity = 0.6f;
};

/** The day-night transition. Four controls tuned against each other: softness
 *  shapes the planet shadow both the air and the deck are read through, the
 *  ambient width gates both ambients, and the lobe terms keep the forward peaks
 *  from leaking past it. */
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

	/** Width of the ambient terminator, in cosine of sun elevation; 0.15 is about
	 *  9 degrees either side. Ambient applied unconditionally washes the night
	 *  side, which reads as the star shining through the planet. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001", ClampMax = "1.0"))
	float AmbientTerminator = 0.15f;

	/** How fast the air's Mie forward lobe dies behind deck, per optical depth.
	 *  Light that crossed a dense medium has lost its direction, and carrying the
	 *  full phase through puts the glow on top of the planet. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float MieLobeDecay = 2.0f;

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

	/** Steps across the air segments. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1.0"))
	float AtmosphereSteps = 64.0f;

	/** Steps across the cloud a ray actually crosses, shared across every cloud
	 *  segment on it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1.0"))
	float CloudSteps = 128.0f;

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