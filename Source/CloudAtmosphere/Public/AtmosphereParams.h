// AtmosphereParams.h — the parameter sets the three-stage atmosphere post
// process is driven by, and the derivations that keep them consistent.
//
// THREE TIERS, SPLIT BY WHAT OWNS THE VALUE.
//
// ENVIRONMENT is what the planet does not choose: the star's colour, and the
// composite blur that runs on the finished image in a material both models
// share. One definition, one instance.
//
// COMMON is what both march materials declare and read identically -- the
// shell, the air, the march budget, the cloud lighting. One definition, ONE
// INSTANCE PER MODEL. The two models read the same parameters and want
// substantially different values for them: a gas giant's shell is one to two
// planet radii where a terrestrial's is a few percent, and the march budget
// that follows from that is not comparable. Two instances carry two sets of
// defaults, which a single struct cannot.
//
// MODEL params are what only one march material declares -- the cloud band, or
// the deck's field and per-band scattering.
//
// The seam is at Common: a generator can fill Environment and Common without
// knowing which model it is driving, then hand off to a model-specific pass.
//
// Cloud Beta stays in the model structs rather than in Common because the two
// models AUTHOR it differently, not merely value it differently. The
// terrestrial band writes an absolute extinction against a cloud with holes;
// the deck solves one from a total optical depth. Same shader parameter, two
// different inputs.
//
// RATIOS, NOT ABSOLUTES, WHEREVER ONE VALUE IS BOUNDED BY ANOTHER. A parameter
// expressed against the thing that constrains it stays valid when that thing is
// retuned. Expressed absolutely it silently goes out of range, and the failure
// shows up as a geometry or sampling artifact rather than as a bad value.
//
// The exception is noted on DetailRelief: it is deliberately NOT a ratio of the
// band relief, because summing them before the relief multiply makes the fine
// noise scale with band thickness, and noise stretched vertically but not
// horizontally becomes spikes.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "AtmosphereParams.generated.h"

class UGasGiantSimConfig;
class UVolumeTexture;

/** Which cloud model the march stage uses.
 *
 *  An enum rather than a bool: adding a case fails loudly at every switch,
 *  where a bool silently takes the false branch. */
UENUM(BlueprintType)
enum class EPlanetAtmosphereType : uint8
{
	Terrestrial,
	GasGiant
};

/** The star and the composite: what the planet does not choose.
 *
 *  ONE INSTANCE FOR BOTH MODELS. The star's colour is a system-level property
 *  that a cloud model has no say in, and the composite blur runs in a single
 *  material shared by both march paths -- a per-model copy of it would be a
 *  second value that can never reach a shader. */
 /** Stage 3 blur, which softens the limb against the scene behind it. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereCompositeParams
{
	GENERATED_BODY()

	/** Kernel size in SOURCE pixels. THE COST IS QUADRATIC IN THIS -- the loop is
	 *  the disc inscribed in a (2r+1) square, so 6 is about 113 taps and 8 is
	 *  about 197.
	 *
	 *  It is mostly paying to hide the march's sampling noise rather than to
	 *  upsample, so anything that quiets the march lets this come down, and this
	 *  is the cheapest place in the chain to get frames back. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0", ClampMax = "16"))
	int32 BlurRadius = 6;

	/** The Gaussian's width as a DIVISOR of the radius: sigma = radius /
	 *  falloff. Higher concentrates the weight at the centre; at 1 the edge taps
	 *  still carry about 0.6 and the kernel is close enough to a box that its
	 *  response hatches. 2 puts the edge at 0.14. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.25"))
	float BlurFalloffFactor = 2.0f;

	/** How hard a depth difference cuts a tap off, so the blur cannot drag
	 *  atmosphere across a silhouette. Measured against the centre depth, so it
	 *  is a relative tolerance and holds at any distance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float DepthSharpness = 5000.0f;

	/** Full-resolution pixels per source pixel: 2 for a half-per-axis buffer,
	 *  1 if the march runs at full resolution.
	 *
	 *  A PIPELINE FACT, NOT A LOOK CONTROL. It has to match how the postprocess
	 *  material is configured, and the only symptom of getting it wrong is that
	 *  the depth cutoff stops holding the silhouette at the distance it should. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1.0"))
	float DepthTapScale = 2.0f;

	/** Blur weight at the planet edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MaxBlurWeight = 0.5f;

	/** Blur weight everywhere else, as a fraction of MaxBlurWeight. A ratio so
	 *  the floor cannot exceed the peak. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinBlurFraction = 0.0f;

	/** MinW, derived. */
	float GetMinBlurWeight() const { return MaxBlurWeight * MinBlurFraction; }
};

/** The flow simulation both models read.
 *
 *  ONE SIM PER WORLD. The gas giant deck reads it for flow and the terrestrial
 *  band will read it for weather, so it sits on the environment rather than on
 *  either model: a per-model copy would be a second config the one subsystem
 *  cannot honour. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereSimulationParams
{
	GENERATED_BODY()

	/** Owns the flow render target the materials sample and the settings the
	 *  sim subsystem steps against. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	TObjectPtr<UGasGiantSimConfig> Config = nullptr;

	/** Start the sim on BeginPlay. Off when another actor already drives it:
	 *  the subsystem is per-world, so two planets starting it fight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bStartOnBeginPlay = true;
};

/** Where the shell sits.
 *
 *  Planet Center and Planet Radius come from the actor's transform, not from
 *  here. Everything below is a fraction of the radius, so resizing the planet
 *  moves the whole system together. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereGeometryParams
{
	GENERATED_BODY()

	/** Atmosphere top, as a fraction of planet radius above the surface. The
	 *  ceiling every other shell in the system is expressed against. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.001"))
	float HeightScale = 0.2f;

	/** Vertical offset applied to the atmosphere floor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float FloorOffset = 0.0f;

	/** Atmosphere outer radius in world units. */
	float GetAtmosphereRadius(float PlanetRadius) const
	{
		return PlanetRadius * (1.0f + HeightScale);
	}

	/** A shell of half a planet radius. The deck's world thickness is solved
	 *  from this, so it sets the deck's depth as much as the air's. */
	static FAtmosphereGeometryParams MakeGasGiantDefaults()
	{
		FAtmosphereGeometryParams Params;
		Params.HeightScale = 0.5f;
		return Params;
	}
};

/** The air itself: scattering, absorption and what bounced light survives.
 *
 *  Coefficients and scale heights are divided by atmosphere thickness in
 *  Atmo_BuildParams, so they are thickness-relative and survive a resize. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereAirScatteringParams
{
	GENERATED_BODY()

	/** RGB is the Rayleigh coefficient, A IS ITS SCALE HEIGHT as a fraction of
	 *  atmosphere thickness. Packed together because a coefficient without the
	 *  profile it applies to is not a quantity -- they are only ever authored as
	 *  a pair, and the shader reads them from one pin.
	 *
	 *  A scale height under about half the deck's top leaves no air above the
	 *  cloud, and the limb loses its halo. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor RayleighBeta = FLinearColor(0.896360f, 2.913294f, 4.0f, 0.1f);

	/** RGB the Mie coefficient, A ITS SCALE HEIGHT. Lower than Rayleigh's:
	 *  aerosol sits nearer the surface than the gas does. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor MieBeta = FLinearColor(1.0f, 0.83163f, 0.71612f, 0.05f);

	/** Mie asymmetry. Belongs to MieBeta and sits apart only because that
	 *  float4 has no channel left. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "-0.99", ClampMax = "0.99"))
	float MieG = 0.9f;

	/** RGB the absorber coefficient, A THE ALTITUDE ITS LAYER IS CENTRED ON.
	 *  Not a scale height like the two above: the absorber is a Lorentzian
	 *  layer, ozone-like, rather than a profile falling off from the ground. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor AbsorptionBeta = FLinearColor(0.05f, 0.05f, 0.05f, 0.15f);

	/** Half-width of that layer. Separate because the layer has two parameters
	 *  and only one alpha channel. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	float AbsorptionFalloff = 0.1f;

	/** RGB the ambient colour, A THE FLOOR its terminator falloff lerps from.
	 *
	 *  AT ZERO FLOOR THE TERM HAS ALMOST NO RANGE. Ambient stands in for light
	 *  that bounced several times, and behind an opaque planet there is none --
	 *  but gated to exactly zero it is swamped by direct light everywhere it is
	 *  not zero, so the whole control lives inside the terminator's width. The
	 *  floor is what buys it a night side, at the cost of the planet glowing
	 *  where nothing should reach it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor Ambient = FLinearColor(0.080328f, 0.080328f, 0.1f, 0.0f);

	/** Stronger and bluer than the terrestrial air, and a scale height that
	 *  keeps the air extending past the deck's crests rather than dying under
	 *  them -- below about half the deck's top there is no Rayleigh left above
	 *  the cloud and the limb reads as a hard edge. */
	static FAtmosphereAirScatteringParams MakeGasGiantDefaults()
	{
		FAtmosphereAirScatteringParams Params;

		Params.RayleighBeta = FLinearColor(5.291136f, 23.918262f, 32.0f, 0.4f);

		// Only the scale height moves. A deck whose peaks reach most of the way
		// up the shell needs aerosol that still exists above them, where a
		// terrestrial haze layer sits far below the cloud band.
		Params.MieBeta = FLinearColor(1.0f, 0.83163f, 0.71612f, 0.25f);

		// Near-black, with a floor to match. The deck is opaque to the limb, so
		// there is no lit surface under the air to bounce anything back up --
		// what the terrestrial value stands in for does not exist here.
		Params.Ambient = FLinearColor(0.0002f, 0.0002f, 0.0002f, 0.0002f);

		return Params;
	}
};

/** How cloud is lit, for whichever model is marching.
 *
 *  Cloud Beta and Cloud Absorption Beta are NOT here. The terrestrial band
 *  authors an absolute extinction; the deck solves one from a total optical
 *  depth. Each model owns its own input to the same shader parameter. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereCloudScatteringParams
{
	GENERATED_BODY()

	/** RGB the cloud ambient colour, A THE FLOOR, exactly as the air's. Its own
	 *  floor because the deck and the air go dark at different rates. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor Ambient = FLinearColor(0.04f, 0.04f, 0.05f, 0.0f);

	/** (forward g, backward g, lobe blend, isotropic multiple-scatter weight).
	 *  The isotropic term is what keeps the shadow side off black -- single
	 *  scattering has nothing to deliver there. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor PhaseParams = FLinearColor(0.9f, 0.1f, 0.5f, 0.5f);

	/** Five times the air's ambient and still near-black. The deck is the only
	 *  thing there is to bounce off, so multiple scattering inside it is the
	 *  whole of what this stands in for. */
	static FAtmosphereCloudScatteringParams MakeGasGiantDefaults()
	{
		FAtmosphereCloudScatteringParams Params;
		Params.Ambient = FLinearColor(0.001f, 0.001f, 0.001f, 0.001f);
		return Params;
	}
};

/** The march's step budget.
 *
 *  A BUDGET, NOT A QUALITY SETTING, and the two models spend it over different
 *  geometry. Cloud Steps crosses the terrestrial band, but on the gas giant it
 *  crosses only the deck segment the cone trace split off. Same name, same
 *  meaning, values that do not transfer. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereRaymarchParams
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1.0"))
	float AtmosphereSteps = 32.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1.0"))
	float AtmosphereLightSteps = 16.0f;

	/** Step growth with distance from the ray start. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float StepScaleFactor = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1.0"))
	float CloudSteps = 32.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1.0"))
	float CloudLightSteps = 32.0f;

	/** How many flow-field texels a shadow ray may cross in one step.
	 *
	 *  A LENGTH BOUND, NOT A BUDGET. The counts above divide a span, but how much
	 *  deck a shadow ray crosses depends on where the sun is -- an overhead ray
	 *  crosses a fraction of what a grazing one does. One count cannot serve
	 *  both, so it over-resolves the first and under-resolves the second.
	 *
	 *  The finest thing a shadow ray can see is one texel of the flow field,
	 *  since it reads nothing else. Step further and it samples one column out of
	 *  the several it crossed, with the jitter picking which -- which is not an
	 *  estimate with noise on it but a coin flip, and it prints as a scatter of
	 *  bright points that no composite blur resolves.
	 *
	 *  1 IS NYQUIST AGAINST THE FIELD AS RECONSTRUCTED, not against the grid. The
	 *  cubic B-spline is approximating rather than interpolating, so it band
	 *  limits the flow to something nearer two texels wide -- a step per texel is
	 *  already sampling the smoothed field twice per feature. That is why 2 shows
	 *  the flip and 0.5 buys almost nothing: below 1 the extra steps resolve
	 *  structure the reconstruction has already removed.
	 *
	 *  BandSharpness steepens the vorticity-to-altitude map and pushes the
	 *  effective feature back toward the raw texel, so a sharpened deck wants a
	 *  lower value. GG_FLOW_FILTER 0 does the same, harder.
	 *
	 *  IT SETS THE FLOOR THE BUDGET CAN FALL TO. At 1 with a 512 grid the bound
	 *  is about 19 steps, so CloudLightSteps above that is free and the scaling
	 *  saves down to it; at 2 the floor is 9.5 and the saving doubles. Raising
	 *  GridLongitude tightens the bound in proportion -- a finer sim has finer
	 *  structure, and resolving it costs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.25", ClampMax = "8.0"))
	float LightStepTexels = 1.0f;

	/** March pixels a VIEW step is allowed to span.
	 *
	 *  THE COUNTS ABOVE SIZE THE MARCH AGAINST THE DECK, THIS SIZES IT AGAINST
	 *  THE SCREEN. A step budget divided into a span knows nothing about where
	 *  the camera is, so the same detail is resolved at fifty planet radii as at
	 *  two -- and at fifty, a dozen samples land inside a single pixel. Nothing
	 *  finer than a pixel can be seen, so nothing finer needs marching.
	 *
	 *  IT ONLY EVER LENGTHENS THE STEP. Close in the footprint is smaller than
	 *  the planned step and this does nothing; far out it is many times larger
	 *  and the step follows it, which is where the saving is. The march is
	 *  capped by the thinnest feature on the ray either way, so a step can never
	 *  swallow the gradient it is integrating however far the camera gets.
	 *
	 *  1 is a step per pixel. Above about 2 the deck starts to band along the
	 *  step lattice at distance, since the jitter has less room to hide the
	 *  integration error; below 1 the extra samples land inside a pixel that has
	 *  already been decided. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.25", ClampMax = "4.0"))
	float ViewStepPixels = 1.0f;

	static FAtmosphereRaymarchParams MakeGasGiantDefaults()
	{
		FAtmosphereRaymarchParams Params;

		// The deck fills the disc, so the air segment above it is crossed by
		// every ray rather than only the ones that miss the planet, and the
		// deck segment gets a much finer march than the terrestrial band --
		// the whole image is deck, and the gradient holds every feature.
		Params.AtmosphereSteps = 64.0f;
		Params.StepScaleFactor = 3.0f;
		Params.CloudSteps = 64.0f;

		// A gas giant has no holes, so nearly every light sample accumulates
		// and takes the second fetch, where the terrestrial version leans on
		// Cloud_Density returning zero over most of the domain. The deck's
		// light budget stays high anyway: it is what shapes the gradient, and
		// the view steps above were halved to pay for it.
		Params.AtmosphereLightSteps = 8.0f;
		Params.CloudLightSteps = 32.0f;

		return Params;
	}
};

/** CATEGORY PATHS ON AN INLINED STRUCT'S MEMBERS ARE ABSOLUTE. A property
 *  carrying ShowOnlyInnerProperties hands its members to the panel directly,
 *  and each one lands in whatever category IT names -- the outer property's
 *  category is not a prefix. A bare name therefore surfaces at the panel root,
 *  outside the plugin's umbrella, and two structs that both say "Shell" merge
 *  into one group holding both models' properties.
 *
 *  So every member of an inlined struct spells its path out in full. The
 *  substructs below do not: they are declared as their own properties on the
 *  actor, which supplies their category.
 *
 *  The four common groups gathered by reference, so the apply path can take
 *  one argument and still read them by group.
 *
 *  NOT A UPROPERTY. The groups are declared separately on the actor -- one per
 *  panel group, twice over for the two models -- and this is only how they
 *  travel together. */
struct FAtmosphereCommonView
{
	const FAtmosphereGeometryParams& Geometry;
	const FAtmosphereAirScatteringParams& AirScattering;
	const FAtmosphereCloudScatteringParams& CloudScattering;
	const FAtmosphereRaymarchParams& Raymarch;
};

/** The terrestrial cloud band: its shell, its noise, and the extinction it is
 *  authored with.
 *
 *  Cloud Beta stays here rather than moving to Common because this model writes
 *  it directly, where the gas giant solves it. Ambient, phase and the step
 *  counts are in Common. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FTerrestrialCloudParams
{
	GENERATED_BODY()

	// -- Shell --------------------------------------------------------------
	//
	// Both fractions rather than absolutes, so the band stays inside the
	// atmosphere and the inner shell stays below the outer by construction.

	/** Cloud band top, as a fraction of the atmosphere height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shell", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CloudOuterFraction = 0.667f;

	/** Cloud band base, as a fraction of the band top. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shell", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CloudInnerFraction = 0.125f;

	// -- Shape --------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape")
	TObjectPtr<UVolumeTexture> CloudVolumeTexture = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape")
	FLinearColor AnimationWeights = FLinearColor(0.0f, 0.0f, 0.0f, 990.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape")
	float CloudCoverage = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape")
	float CloudDensityMultiplier = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape")
	float CloudHeightCurveMin = 0.3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape")
	float CloudHeightCurveMax = 0.7f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape")
	float CloudNoiseFrequency = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape")
	FLinearColor CloudNoiseWeights = FLinearColor(0.55f, 0.3f, 0.15f, 0.3f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape")
	FLinearColor CloudNoiseInvert = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);

	/** Detail frequency as a multiple of the base. A ratio because the detail
	 *  layer's job is to break up the shape the base produced, and that reading
	 *  only holds if the two stay in proportion. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape", meta = (ClampMin = "1.0"))
	float DetailFrequencyRatio = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape")
	FLinearColor DetailNoiseWeights = FLinearColor(0.2f, 0.3f, 0.3f, 0.2f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape")
	FLinearColor DetailNoiseInvert = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape")
	float DetailErodeStrength = 0.3f;

	// -- Lighting -----------------------------------------------------------

	/** Absolute extinction. The cloud has holes, so this is what makes the
	 *  solid parts read as solid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Lighting")
	FLinearColor CloudBeta = FLinearColor(150.0f, 145.3125f, 140.625f, 0.5f);

	/** Light-ray extinction, deliberately below Cloud Beta: light scattered
	 *  INTO the ray is what the multiple-scattering term stands in for, so the
	 *  full scattering coefficient would count that loss twice. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Lighting")
	FLinearColor CloudAbsorptionBeta = FLinearColor(25.0f, 25.0f, 25.0f, 0.0f);

	// -- Derivations --------------------------------------------------------

	/** Cloud Outer Height Scale, as the material expects it: a fraction of
	 *  planet radius. */
	float GetOuterHeightScale(float AtmosphereHeightScale) const
	{
		return AtmosphereHeightScale * CloudOuterFraction;
	}

	float GetInnerHeightScale(float AtmosphereHeightScale) const
	{
		return GetOuterHeightScale(AtmosphereHeightScale) * CloudInnerFraction;
	}

	float GetDetailNoiseFrequency() const
	{
		return CloudNoiseFrequency * DetailFrequencyRatio;
	}
};

/** The gas giant deck's field: shape, transport, and the volumes it reads.
 *
 *  THREE ABSOLUTES, EVERYTHING ELSE DIMENSIONLESS. DeckTop, DeckBackstop and
 *  GradientThickness are fractions of atmosphere height; every relief amount
 *  and both carves are fractions of GradientThickness.
 *
 *  THE GRADIENT HANGS FROM EACH COLUMN'S OWN TOP rather than stretching between
 *  two anchors, so relief moves the profile instead of deforming it and every
 *  column shades alike. DeckBackstop is a backstop under it: the gradient stops
 *  there, which keeps the marched band fixed however deep relief cuts.
 *
 *  RELIEF IS BOUNDED IN NEITHER DIRECTION. Downward it meets the backstop;
 *  upward the deck hangs from DeckTop, so the peaks land on it whatever the
 *  relief amounts are. GetTopMin() says how far the deepest troughs fall, and
 *  GetDeckBase() where an unrelieved column sits -- both tuning readouts rather
 *  than limits. The only constraint left is DeckTop itself, at or below 1. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FGasGiantDeckParams
{
	GENERATED_BODY()

	// -- Shell --------------------------------------------------------------
	//
	// Measured from the planet surface. DeckTop must stay at or below 1:
	// above it Atmo_Plan clips the tallest columns, slicing the tops off
	// exactly where features are tallest, which reads as a field bug.

	/** THE CEILING the deck hangs from, as a fraction of atmosphere thickness.
	 *  Relief works DOWNWARD from here, so the cloud tops stay put and
	 *  GradientThickness spends itself on depth.
	 *
	 *  How much of the relief hangs below it is CeilingReserve. The shader is
	 *  given GetDeckBase(), never this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Shape", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DeckTop = 1.0f;

	/** How much of the relief's upward reach is reserved BELOW DeckTop.
	 *
	 *  1 hangs the whole theoretical maximum below it, so no column can ever
	 *  exceed DeckTop. That bound is the JOINT worst case -- every up term at
	 *  its extreme on the same column at the same moment -- which essentially
	 *  never happens, so the visible tops sit well under DeckTop and sink
	 *  further as GradientThickness grows.
	 *
	 *  0 puts DeckTop at the unrelieved base, so the tops climb with thickness
	 *  instead.
	 *
	 *  BETWEEN THEM IS WHERE THE TOPS HOLD STILL. Reserve roughly what the up
	 *  terms actually attain together and the general cloud tops land near
	 *  DeckTop at any thickness, while the rare joint maxima -- storm towers
	 *  over a pressure high -- poke above it, which is what those features are
	 *  for.
	 *
	 *  GetTopMax() is then above DeckTop and IS the cull radius, so it is the
	 *  number that has to stay at or below 1. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Shape", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CeilingReserve = 0.6f;

	/** How far the density gradient reaches below a column's own top, and the
	 *  unit every relief amount is a fraction of.
	 *
	 *  THE GRAIN HANDLE. Widen it and the deck top spreads over more march
	 *  steps. Relief scales with it, so widening to quiet the grain also raises
	 *  the bands -- that is the deck getting deeper, not a side effect.
	 *
	 *  UNIFORM ACROSS THE DECK, which is the point: the gradient is what the
	 *  surface shading reads, and a span measured to a flat floor instead
	 *  stretches over crests and collapses in troughs, so the same material
	 *  shades soft in one place and hard in another. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Shape", meta = (ClampMin = "0.0001", ClampMax = "1.0"))
	float GradientThickness = 0.3f;

	/** Backstop under the gradient: no column's density ramp reaches below this,
	 *  however low relief takes its top.
	 *
	 *  ALSO THE MARCHED BAND'S LOWER EDGE, which is why the clamp exists. A
	 *  freely following floor drags that edge down to GetTopMin() minus the
	 *  thickness and spends the same step budget over nearly twice the span.
	 *
	 *  THE TRADE IS AUTHORED HERE. Columns topping out above this plus
	 *  GradientThickness get the full uniform span; the rest compress toward a
	 *  step. Lowering it buys uniformity in the troughs and widens the fine band
	 *  one for one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Shape", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DeckBackstop = 0.3f;

	/** Where the mass sits inside the gradient, without moving either boundary.
	 *  1 is centred, below 1 pulls density toward the top.
	 *
	 *  ABOVE 0.5 THE ONSET IS C1. At 0.5 the slope at the deck top goes finite
	 *  instead of zero, which creases along the whole top; below it the top
	 *  hardens into an edge. Both are allowed -- a sharp cloud top is a real
	 *  discontinuity, not a bug -- so this is a look control rather than a
	 *  bounded one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Shape", meta = (ClampMin = "0.0001", ClampMax = "4.0"))
	float DensityCurve = 0.5f;

	// -- Relief -------------------------------------------------------------
	//
	// Fractions of GradientThickness, all of them, moving the deck top around
	// GetDeckBase(). 1 is one whole gradient. NOTHING HERE NEEDS WATCHING: the
	// base is solved so the upward terms land on DeckTop, and the downward ones
	// meet the backstop. Raising an amount deepens the deck rather than
	// pushing it through the shell.
	//
	// THEY SPLIT ACROSS TWO PANEL GROUPS, and the split is where the shape
	// comes from rather than what it is. Band, pressure and towers are relief
	// the SIMULATION produces, so they group with the rest of the flow; the two
	// layer amounts are relief the noise VOLUMES produce, so they group with
	// the layer that carves it. Same unit either way.

	/** Height between a jet and a zone. Wants to be large: the point of driving
	 *  height from the flow is that bands are geometry rather than a pattern
	 *  painted on a sphere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Flow")
	float BandRelief = -0.5f;

	/** How far pressure lifts the deck. Anticyclones rise, cyclones sink. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Flow")
	float PressureRelief = 0.1f;

	/** Added height of a convective tower, where VortexThreshold's gate is
	 *  open. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Flow")
	float StormTowerRelief = 0.1f;

	/** How far the flow's strain collapses band relief toward flat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Flow", meta = (ClampMin = "0.0"))
	float ReliefThinning = 0.0f;

	/** How deep the DETAIL layer carves the deck top. ONE-SIDED: it only
	 *  removes, because its features are fine enough that centring them reads
	 *  as high-frequency material pushing up out of the surface rather than as
	 *  the surface being broken up.
	 *
	 *  NEGATIVE FLIPS IT into a one-sided build instead.
	 *
	 *  NOT A RATIO OF BandRelief, deliberately. Summed before the relief
	 *  multiply, raising the band relief scales the fine noise by the same
	 *  factor, and noise stretched vertically but not horizontally becomes
	 *  spikes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Carve")
	float DetailRelief = 0.25f;

	/** How far the PACKED layer moves the deck top, SIGNED about the noise's
	 *  median -- its features are large enough that a one-sided carve lowers the
	 *  mean deck as it is turned up, so texture amount and deck altitude stop
	 *  being separate controls.
	 *
	 *  NEGATIVE MIRRORS THE NOISE, turning its billows into pits.
	 *
	 *  Larger than the detail layer's: this is the mid-level shaping that gives
	 *  the deck its silhouette, and it has to survive to a distance where the
	 *  detail layer is long gone. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Carve")
	float StructureRelief = 0.5f;

	/** Vortex strength above which storm towers are allowed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Flow", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VortexThreshold = 0.9f;

	// -- Detail -------------------------------------------------------------

	/** Horizontal feature size of the DETAIL volume, in noise units per radian.
	 *  Higher tiles finer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Scale", meta = (ClampMin = "0.01"))
	float DetailScale = 24.0f;

	/** Horizontal feature size of the STRUCTURE volume, same unit, authored
	 *  independently. Below DetailScale makes it the coarser layer, which is
	 *  what its job wants: mid-level shaping that stays resolvable from orbit
	 *  while the detail layer tiles finely up close.
	 *
	 *  INDEPENDENT RATHER THAN A RATIO, so the two layers can take different
	 *  noise assets and be retuned separately. Tied to DetailScale, retuning the
	 *  fine layer silently moves the deck's silhouette -- the shape that has to
	 *  survive to orbit follows a control that only matters up close. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Scale", meta = (ClampMin = "0.01"))
	float StructureScale = 1.2f;

	/** DETAIL layer: vertical feature size against its horizontal one. Their
	 *  ratio IS the aspect of the resulting structure, so the ratio is the real
	 *  control and the absolute vertical rate is derived from it.
	 *
	 *  1 IS ISOTROPIC at any shell thickness -- above it features are taller
	 *  than they are wide, below it flatter.
	 *
	 *  PITFALL: an exaggerated aspect does not read as tall noise. The UVW scale
	 *  swings by a large factor across the shell, so the pattern RESCALES with
	 *  sample altitude -- and since altitude within a step moves with step size,
	 *  which grows with distance, it swims as the camera pulls back. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Scale", meta = (ClampMin = "0.01"))
	float DetailAspect = 3.0f;

	/** STRUCTURE layer: the same, against its own horizontal scale.
	 *
	 *  SEPARATE FROM DetailAspect BECAUSE THE LAYERS WANT DIFFERENT SHAPES.
	 *  Detail is filaments that stretch along the streamlines; structure is
	 *  rounded mid-level shaping. Tying them means one horizontal scale change
	 *  retunes the other layer's silhouette. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Scale", meta = (ClampMin = "0.01"))
	float StructureAspect = 8.0f;

	/** How much of the coarse warp the detail layer inherits. A displacement
	 *  field with a large gradient IS strain, so inheriting one whole imposes
	 *  an order-one strain on the fine layer regardless of its own flow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Warp", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DetailWarpInherit = 0.4f;

	/** How much the structure layer inherits. 0 is legitimate: this layer breaks
	 *  the flow into rounded shapes, and a shape dragged through the flow field
	 *  is no longer round. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Warp", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StructureWarpInherit = 0.7f;

	// -- Level of detail ----------------------------------------------------
	//
	// Distances in PLANET RADII from the camera to the sample. Each layer is
	// full strength below Near, fades to nothing at Near + Span, and is skipped
	// entirely beyond that.
	//
	// THE UNIT IS THE PLANET, NOT THE ATMOSPHERE. Both volumes are sampled
	// against a unit direction, so a feature's world size goes with the planet
	// radius and has nothing to do with how thick the air is. Measured in
	// atmosphere thicknesses instead, retuning AtmosphereHeightScale moves
	// every fade with it -- which reads as the LOD breaking rather than as the
	// shell changing.
	//
	// A FADE RANGE BELONGS WITH ITS SCALE. Tiling frequency scales with the
	// layer's scale, so the distance at which it starts aliasing goes as one
	// over that scale: raising DetailScale needs a proportionally tighter fade,
	// and lowering it lets the fade relax. Retuned independently, a scale
	// change either leaves visible tiling or throws away structure that was
	// still resolvable.
	//
	// Left as separate handles while the scales are still being explored. Once
	// they settle, the durable form is Near = constant / scale.

	/** Detail layer: where it starts fading. Small, because this layer is the
	 *  finer of the two and tiles aggressively -- which is affordable exactly
	 *  because it is gone within a fraction of a planet radius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Fade", meta = (ClampMin = "0.0"))
	float DetailFadeNear = 0.0f;

	/** Detail layer: fade width, added to Near. Additive rather than a
	 *  multiple, so the transition width is independent of where it starts and
	 *  a tight fade close in is authorable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Fade", meta = (ClampMin = "0.0"))
	float DetailFadeSpan = 0.2f;

	/** Structure layer: where it starts fading. An order of magnitude further out,
	 *  because this is the mid-level shaping that has to read from orbit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Fade", meta = (ClampMin = "0.0"))
	float StructureFadeNear = 0.2f;

	/** Structure layer: fade width, added to Near. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Fade", meta = (ClampMin = "0.0"))
	float StructureFadeSpan = 0.6f;

	/** Worley rung weights coarse to fine in RGB, layer amount in A.
	 *
	 *  RGB IS A DIRECTION, NOT THREE LEVELS. The rungs are renormalized by their
	 *  LENGTH shader-side rather than their sum, so only the balance between
	 *  them carries meaning -- which is exactly what a colour picker navigates.
	 *  Dragging the wheel sweeps the whole spectrum continuously; three sliders
	 *  can only walk one axis at a time.
	 *
	 *  A stays independent of that normalization and is the layer's overall
	 *  amount. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Noise")
	FLinearColor DetailNoiseWeights = FLinearColor(1.0f, 0.5f, 0.25f, 1.0f);

	/** Same layout for the structure layer. Its own set, because the two layers
	 *  are different sizes doing different jobs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Noise")
	FLinearColor StructureNoiseWeights = FLinearColor(1.0f, 0.5f, 0.25f, 1.0f);

	/** How much of either layer survives in the flat band interiors, against
	 *  full strength at the edges where a real gas giant's billows live. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Carve", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float EdgeBias = 1.0f;

	/** How much the DETAIL layer erodes the density. Below 1: there is no lower
	 *  cloud shell, so a fully transparent column would let a ray run to the far
	 *  side of the planet, and the erosion floor is what makes that
	 *  impossible. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Carve", meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float DetailErosion = 0.9f;

	/** How much the PACKED layer erodes the density. Separate because the two
	 *  layers survive to different distances: the structure one carries shape that
	 *  has to read from orbit, the detail one is micro variance that is gone
	 *  within a fraction of a planet radius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Carve", meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float StructureErosion = 0.9f;

	/** How far down the gradient erosion reaches, as a fraction of it. 0 is
	 *  surface only, 1 reaches the saturated region.
	 *
	 *  NORMALIZED AGAINST THE COLUMN'S OWN GRADIENT, so erosion finishes exactly
	 *  where that column saturates. An absolute depth still has carve left at
	 *  the floor of a column the backstop compressed, which is where the
	 *  solid-region skip takes over. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Carve", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ErosionDepth = 1.0f;

	// -- Flow ---------------------------------------------------------------

	/** Warp duration. A short advection whose only job is to carry the baked
	 *  volumes along the current flow -- the history lives in the sim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Warp", meta = (ClampMin = "0.0"))
	float WarpTime = 0.075f;

	/** Detail layer warp, as a fraction of WarpTime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Warp", meta = (ClampMin = "0.0"))
	float DetailWarpRatio = 1.0f;

	/** Shifts which band type dominates without retuning the sim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Flow")
	float BandBias = 0.3f;

	/** Turbulence floor in band interiors. Real zone interiors are calmer than
	 *  their edges but not glass. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Warp", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float TurbulenceFloor = 0.5f;

	// -- Crossfade ----------------------------------------------------------
	//
	// The warp anchors to a fixed direction and integrates for a fixed time, so
	// the noise WOBBLES where the flow changes but never travels: it cannot,
	// because the volumes are resampled every frame and have nowhere to keep a
	// position. Two phases half a period apart, each ramping its displacement
	// and resetting under zero weight, give it somewhere to go.
	//
	// A SECOND VOLUME FETCH PER LAYER IT IS ENABLED ON. The flow loop is shared,
	// so what it costs is the texture read rather than the advection.

	/** How long a phase takes to run its ramp, in SIMULATED seconds. The actor
	 *  divides by the sim's TimeScale before pushing, so the noise keeps pace
	 *  with the flow it is meant to be carried by however the sim speed moves.
	 *
	 *  IT IS A SPEED CONTROL, NOT A DURATION. How far a phase travels is fixed
	 *  by the warp -- GG_CROSSFADE_SPAN times WarpTime times the layer's
	 *  inheritance -- so the period only sets how long that takes. Travelling at
	 *  the flow's own rate would want a period of GG_CROSSFADE_SPAN * WarpTime,
	 *  which at any sane warp is a fraction of a second and dissolves far too
	 *  often to hide. Everything usable is far slower than the flow, so this is
	 *  an art control rather than a physical one.
	 *
	 *  THE GHOSTING TRADE LIVES HERE. Each phase is sheared by a different
	 *  amount and the blend superimposes them, so long periods separate the two
	 *  further and ghost harder while short ones dissolve more often. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Warp", meta = (ClampMin = "0.1"))
	float CrossfadePeriod = 20.0f;

	/** ON BY DEFAULT: the detail layer is where pinned noise reads as pinned.
	 *  Its features are small enough that a few seconds of not travelling shows,
	 *  and it is the layer the fades remove at distance anyway. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Warp")
	bool bCrossfadeDetail = true;

	/** OFF BY DEFAULT: structure features are large, and their apparent motion
	 *  already comes from the flow the sim advects. Turning it on doubles this
	 *  layer's fetches -- including at distance, where it is the only layer left
	 *  and the detail fade has already stopped paying for one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Warp")
	bool bCrossfadeStructure = false;

	/** Multiplies already-normalized vorticity, so 1 is neutral and the useful
	 *  range is roughly 0.5 to 3. Too high flattens the elevation to its
	 *  asymptote everywhere but the boundaries, turning the height field into
	 *  terraces joined by cliffs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Flow", meta = (ClampMin = "0.0"))
	float BandSharpness = 1.0f;

	/** The planet's own rotation, radians per unit of simulated time. The sim
	 *  runs in the rotating frame, so this rotates the sampling position rather
	 *  than the flow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Flow")
	float RotationWeight = 0.0f;

	// The sim slices the deck reads are GG_FLOW_LAYER and GG_DEEP_FLOW_LAYER
	// in GasGiantFlow.ush. Both must stay under GasGiantSimConfig::LayerCount.

	/** Bound on the deck's slope, GRADIENT DEPTHS per radian -- the same unit as
	 *  the relief that produces the slope, so it stays in step through an anchor
	 *  retune instead of silently becoming an under-declaration. The cone angle
	 *  for the entry search: under-declaring it is the one way the search steps
	 *  over the surface, so raise it first if tangent-angle slicing appears.
	 *
	 *  Grouped with the step budgets rather than with the shape it describes,
	 *  since the search is the only thing that reads it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Raymarch", meta = (ClampMin = "0.1"))
	float DeckSlope = 8.0f;

	// -- Sources ------------------------------------------------------------

	/** Micro variance. R is a Perlin FBM, GBA a Worley octave ladder on separate
	 *  seeds, all median-centred and equalized per channel.
	 *
	 *  SAMPLER: wrap on all three axes, Linear Color -- these are scalar fields
	 *  and an sRGB decode curves them without erroring. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Noise")
	TObjectPtr<UVolumeTexture> DetailVolume = nullptr;

	/** Mid-level shape, same channel layout as the detail volume. G doubles as
	 *  the storm-tower gate, so it stays the coarsest rung.
	 *
	 *  A SEPARATE ASSET FOR DIFFERENT SEEDS, not because one texture could not
	 *  serve both positions -- the two layers are sampled at different points
	 *  and would be two fetches either way. Shared seeds would make the layers
	 *  rhyme, and coincident features across two scales read as a repeat rather
	 *  than as depth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Surface|Noise")
	TObjectPtr<UVolumeTexture> StructureVolume = nullptr;

	// The sim that drives the flow lives on the Environment set, since the
	// subsystem is per-world and the terrestrial band will read the same one.

	// -- Derivations --------------------------------------------------------

	/** Crossfade, as the material expects it. The enables are pushed as 0 or 1
	 *  and read as a branch, so a disabled layer costs its single fetch. */
	FLinearColor GetCrossfade() const
	{
		return FLinearColor(
			CrossfadePeriod,
			bCrossfadeDetail ? 1.0f : 0.0f,
			bCrossfadeStructure ? 1.0f : 0.0f,
			0.0f);
	}

	/** Ladder weights plus the layer's amount, as the material expects them. */
	FLinearColor GetDetailNoise() const
	{
		return DetailNoiseWeights;
	}

	FLinearColor GetStructureNoise() const
	{
		return StructureNoiseWeights;
	}

	/** How far the UPWARD relief terms can carry a column above the base, in
	 *  gradients. Every term in GG_CloudTop at its most generous: Elevation is
	 *  in [0,1] so the band contributes half of BandRelief either side of the
	 *  base; pressure is soft-saturated to [-1,1] sim-side so it contributes
	 *  the whole of PressureRelief; the structure carve is signed, so half of it
	 *  lands in each bound; the detail carve is one-sided and lands in whichever
	 *  bound its sign points at. */
	float GetUpBudget() const
	{
		return 0.5f * FMath::Abs(BandRelief)
			+ FMath::Abs(PressureRelief)
			+ 0.5f * FMath::Abs(StructureRelief)
			+ FMath::Max(-DetailRelief, 0.0f)
			+ FMath::Max(StormTowerRelief, 0.0f);
	}

	/** The matching downward reach. Feeds GetTopMin; nothing bounds it. */
	float GetReliefBudget() const
	{
		return 0.5f * FMath::Abs(BandRelief)
			+ FMath::Abs(PressureRelief)
			+ FMath::Max(DetailRelief, 0.0f)
			+ 0.5f * FMath::Abs(StructureRelief);
	}

	/** The unrelieved deck altitude the shader builds every column from: far
	 *  enough below DeckTop that the tallest column lands exactly on it.
	 *
	 *  DERIVED, SO THE CEILING IS WHAT IS AUTHORED. Anchoring the base instead
	 *  puts DeckTop at neither the peaks nor the mean: relief is a fraction of
	 *  GradientThickness, so the peaks climb as the deck deepens and every
	 *  thickness needs the shell realigned by hand. Hung from the ceiling, the
	 *  cloud tops stay where they were put and thickness spends itself
	 *  downward, which is the direction that has room. */
	float GetDeckBase() const
	{
		return DeckTop - GradientThickness * GetUpBudget() * CeilingReserve;
	}

	/** The highest the deck can reach, and the cull radius the march plans
	 *  against. Equals DeckTop only at CeilingReserve 1; below that it sits
	 *  above by the unreserved remainder. MUST MATCH GG_TopBounds, which
	 *  derives it the same way from the base the shader was given.
	 *
	 *  KEEP THIS AT OR BELOW 1. Past it the cull radius leaves the atmosphere,
	 *  the plan clamps to the shell, and the tallest columns are sliced flat. */
	float GetTopMax() const
	{
		return GetDeckBase() + GradientThickness * GetUpBudget();
	}

	/** The lowest a column top can fall. Below DeckBackstop its gradient has been
	 *  clamped to a step; below DeckBackstop + GradientThickness it is compressed.
	 *  Also the altitude under which every column is saturated, which the shader
	 *  counts its opaque terminus down from. */
	float GetTopMin() const
	{
		return GetDeckBase() - GradientThickness * GetReliefBudget();
	}

	/** Where the gradient bottoms out at a column with no relief. Matches
	 *  GG_DeckFloor in GasGiantFlow.ush, which does the same clamp per column. */
	float GetNominalFloor() const
	{
		const float Base = GetDeckBase();

		return FMath::Min(FMath::Max(Base - GradientThickness, DeckBackstop), Base);
	}

	/** Atmosphere thickness in world units: the one absolute length the field
	 *  reads, and the unit every fraction above is in. The deck has no shell of
	 *  its own -- the anchors place it inside the air, so sizing the air does
	 *  not resize the deck and a thick deck does not force thick air. */
	float GetAtmosphereThickness(float PlanetRadius, float AtmosphereHeightScale) const
	{
		return PlanetRadius * AtmosphereHeightScale;
	}

	/** Profile, as the material expects it. x is world units; the other three
	 *  are atmosphere fractions and a threshold. */
	FLinearColor GetProfile(float PlanetRadius, float AtmosphereHeightScale) const
	{
		return FLinearColor(
			GetAtmosphereThickness(PlanetRadius, AtmosphereHeightScale),
			DeckBackstop,
			VortexThreshold,
			GradientThickness);
	}

	/** Relief, as the material expects it. x is the base the other three move
	 *  around, so it belongs in the same float4 as they do. */
	FLinearColor GetRelief() const
	{
		return FLinearColor(GetDeckBase(), BandRelief, PressureRelief, StormTowerRelief);
	}

	FLinearColor GetScales() const
	{
		return FLinearColor(
			DetailScale,
			StructureScale,
			DetailWarpInherit,
			StructureWarpInherit);
	}

	FLinearColor GetWarps() const
	{
		return FLinearColor(
			WarpTime,
			WarpTime * DetailWarpRatio,
			BandBias,
			TurbulenceFloor);
	}

	/** Noise units the shell spans, per layer. Each aspect rides on its own
	 *  layer's horizontal scale, so retuning one scale leaves the other layer's
	 *  shape alone.
	 *
	 *  SCALED BY ATMOSPHERE HEIGHT, WHICH IS WHAT MAKES THE ASPECT A RATIO.
	 *  Heights reaching the field are fractions of shell thickness while the
	 *  horizontal scale is against planet radius, so a vertical rate authored
	 *  bare means a different world shape at every shell size -- 1 would be
	 *  isotropic at a shell of one radius, forty times stretched at a
	 *  hundredth of one. Folding the height scale in here makes 1 isotropic
	 *  everywhere, and a shell retune stops restretching the noise. */
	float GetDetailVertical(float AtmosphereHeightScale) const
	{
		return DetailScale * DetailAspect * AtmosphereHeightScale;
	}

	float GetStructureVertical(float AtmosphereHeightScale) const
	{
		return StructureScale * StructureAspect * AtmosphereHeightScale;
	}

	/** (DetailNear, DetailFar, StructureNear, StructureFar), as the shader wants
	 *  it. Planet radii. */
	FLinearColor GetFadeRanges() const
	{
		return FLinearColor(
			DetailFadeNear,
			DetailFadeNear + DetailFadeSpan,
			StructureFadeNear,
			StructureFadeNear + StructureFadeSpan);
	}

};

/** Per-band scattering and extinction for the deck.
 *
 *  Alpha on each set MULTIPLIES the base extinction rather than replacing it,
 *  so the deck stays on the same footing as the air. RGB is single-scattering
 *  albedo, and it has no terrestrial equivalent.
 *
 *  All three equal gives a single-material deck. Split Negative from Positive
 *  only once relief is visible -- before that, a colour difference and a
 *  geometry failure look alike. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FGasGiantScatterParams
{
	GENERATED_BODY()

	/** rgb albedo, a extinction multiplier. Cyclonic bands. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Cloud Scattering|Bands")
	FLinearColor ScatterNegative = FLinearColor(0.062275f, 0.070836f, 0.241319f, 1.0f);

	/** Anticyclonic bands. "Darker bands eat more light" is this alpha against
	 *  ScatterNegative's. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Cloud Scattering|Bands")
	FLinearColor ScatterPositive = FLinearColor(0.055407f, 0.321422f, 0.348958f, 1.0f);

	/** Band boundaries, where vorticity crosses zero. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Cloud Scattering|Bands")
	FLinearColor ScatterBase = FLinearColor(0.331597f, 0.103566f, 0.288032f, 1.0f);

	/** Where the band ramp saturates. Matching this to the inverse of the sim
	 *  debug view's DebugScale makes the material and the debug view agree
	 *  about where band boundaries are. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Flow", meta = (ClampMin = "0.0"))
	float BandScale = 3.0f;

	// -- Terminator -----------------------------------------------------------
	//
	// Four scalars that shape the day-night transition, tuned against each
	// other rather than individually. They reach the shader as one LobeParams
	// float4.
	//
	// THEIR OWN GROUP, not filed under either scattering set, because only one
	// of the four is a cloud term. The softness shapes the planet shadow both
	// the air and the deck are read through; the ambient width gates both
	// ambient terms; the lobe decay is the AIR's Mie lobe dying behind deck.
	// Under either heading, three of the four would be in the wrong place.

	/** Width of the planet-shadow falloff, as a fraction of atmosphere
	 *  thickness. The geometric shadow of a sphere has a hard boundary; a real
	 *  terminator does not, because a grazing sun ray crosses progressively more
	 *  air before it arrives. This stands in for that without marching it.
	 *
	 *  WIDE, because this is what shapes the terminator. Narrowing it to stop
	 *  the forward lobe leaking makes the terminator hard AND exposes the
	 *  smoothstep's own endpoints as edges along the light's tangent cone.
	 *  LobeShadowPower is the control for the leak. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Terminator", meta = (ClampMin = "0.0001"))
	float TerminatorSoftness = 0.35f;

	/** Width of the ambient terminator, in cosine of sun elevation. 0.15 is
	 *  about 9 degrees either side of the geometric terminator.
	 *
	 *  AMBIENT IS STARLIGHT THAT BOUNCED, NOT A FLOOR. Both ambient terms stand
	 *  in for light that scattered several times before arriving, and behind an
	 *  opaque planet there is none. Applied unconditionally they wash the night
	 *  side at a fixed brightness, which reads as the star shining through the
	 *  planet -- and on a deck opaque to the limb, nothing breaks it up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Terminator", meta = (ClampMin = "0.0001", ClampMax = "1.0"))
	float AmbientTerminator = 0.15f;

	/** How fast the Mie forward lobe dies behind deck: optical depths of deck
	 *  for an e-fold, inverted, so 2 leaves the lobe at 2% one optical depth in.
	 *
	 *  THE LOBE BELONGS TO SINGLE SCATTERING. Light that has crossed a dense
	 *  medium has bounced several times and lost its direction, so the sharp
	 *  forward peak washes out to something near isotropic. Carrying the full
	 *  phase through the deck makes the glow sit on top of the planet rather
	 *  than in front of it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Terminator", meta = (ClampMin = "0.0"))
	float MieLobeDecay = 2.0f;

	/** Exponent applied to the planet shadow for the ANISOTROPIC terms only.
	 *
	 *  THE FORWARD LOBE AND THE ISOTROPIC TERM WANT DIFFERENT SHADOWS. The lobe
	 *  is single-scattered direct light: it arrives along one path, and behind a
	 *  planet that path is blocked, so it really is hard-shadowed. The isotropic
	 *  term stands in for light that bounced several times, which genuinely
	 *  wraps around the terminator. Sharing one factor forces a choice between a
	 *  hard terminator and a glow.
	 *
	 *  The dual lobe at (0.9, 0.1, 0.5) returns about 7.6 head-on against an
	 *  isotropic 1/4pi -- a 95x multiplier in exactly the backlit geometry where
	 *  the falloff is widest, so a few percent of residual shadow becomes a wash
	 *  across the disc. The phase parameters are the gain on that leak, not its
	 *  cause.
	 *
	 *  A power on the same smoothstep rather than a narrower one, so no new
	 *  endpoint appears anywhere. 6 takes 10% residual to 1e-6. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Terminator", meta = (ClampMin = "0.0"))
	float LobeShadowPower = 6.0f;

	// -- Extinction ---------------------------------------------------------
	//
	// DeckOpticalDepth and BandScale are declared here and shown with the DECK,
	// under Shape and Flow. The rest of this struct shows under Gas Giant Cloud
	// Scattering: the band albedos and the extinction are what the deck's
	// material IS, and they tune against the air's scattering next door rather
	// than against the shape. The optical depth is meaningless apart from the
	// shell it is measured across, and BandScale sets how sharply the band ramp
	// saturates, which reads as a flow control however it is consumed. Category
	// paths are absolute, so a property can sit in the struct that solves with
	// it and the group that tunes with it.
	//
	// AUTHORED AS TOTAL OPTICAL DEPTH, NOT AS A COEFFICIENT. Atmo_BuildParams
	// divides Cloud Beta by atmosphere thickness, and the deck's heights are
	// fractions of that same thickness, so a vertical ray down a column with no
	// relief accumulates
	//
	//     tau = ScatterX.a * CloudBeta * (DeckTop + Floor) / 2
	//
	// -- the gradient integrates to half its span because the profile is
	// symmetric across it, plus the saturated region from that column's floor
	// down.
	//
	// THE DECK'S DENSITY PEAKS AT EXACTLY 1, which is why no density term
	// appears above. A separate peak-density control would divide back out of
	// this solve and change nothing on screen.
	//
	// An absolute CloudBeta means something different after every resize and
	// every anchor move. Solving for it from the depth wanted is the only form
	// that survives both.
	//
	// PITFALL: too low and the deck never saturates, which is a PERFORMANCE bug
	// as much as a visual one. Both early-outs -- the main march's transmittance
	// test and the light march's saturation cutoff -- are dead code until a ray
	// can actually go opaque, so every ray burns its full step budget and
	// spawns a full light march. A deck you can see the sky through is a deck
	// costing several times what it should.

	/** Total optical depth from the deck top to the surface at core density.
	 *  30 is transmittance 1e-13 at the base, saturating within a few percent
	 *  of the deck depth. Below about 8 the sky starts showing through. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Deck|Shape", meta = (ClampMin = "0.1"))
	float DeckOpticalDepth = 100.0f;

	/** Per-channel tint on that depth. Wavelength-dependent extinction, on top
	 *  of the albedo in the scatter sets. Neutral at (1,1,1). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Cloud Scattering|Extinction")
	FLinearColor ExtinctionTint = FLinearColor(1.0f, 0.969f, 0.938f, 1.0f);

	/** Light-ray extinction as a fraction of the view ray's.
	 *
	 *  Below 1 on purpose: light scattered INTO the ray is what the
	 *  multiple-scattering term stands in for, so the full coefficient would
	 *  count that loss twice and the deck would read as flat black on the
	 *  shadow side. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Gas Giant Cloud Scattering|Extinction", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float LightExtinctionFraction = 0.5f;

	// -- Derivations --------------------------------------------------------

	/** LobeParams, as the material expects it. */
	FLinearColor GetLobeParams() const
	{
		return FLinearColor(TerminatorSoftness, AmbientTerminator,
			MieLobeDecay, LobeShadowPower);
	}

	/** Cloud Beta, solved so a ray down a column with no relief accumulates
	 *  DeckOpticalDepth at core density.
	 *
	 *  Floor is that column's own gradient base, from GetNominalFloor(): the
	 *  gradient hangs from the top, so the path depends on the thickness rather
	 *  than on where the backstop sits, except where the backstop cut the
	 *  gradient short.
	 *
	 *  The ScatterX.a multipliers ride on top, so they stay relative: at 1.0 a
	 *  band gets exactly the authored depth, and Neg against Pos is how much
	 *  more one band family absorbs than the other. */
	FLinearColor GetCloudBeta(float Top, float Floor) const
	{
		const float Path = FMath::Max(0.5f * (Top + Floor), KINDA_SMALL_NUMBER);

		return ExtinctionTint * (DeckOpticalDepth / Path);
	}

	/** Cloud Absorption Beta. Same solve, scaled down for the light ray. */
	FLinearColor GetCloudAbsorptionBeta(float Top, float Floor) const
	{
		return GetCloudBeta(Top, Floor) * LightExtinctionFraction;
	}

};