// The parameter sets the three-stage atmosphere post process is driven by, and
// the derivations that keep them consistent.
//
// TIERS, SPLIT BY WHAT OWNS THE VALUE. ENVIRONMENT is what the planet does not
// choose -- the composite blur and the flow sim, each running in one shared
// instance. The rest is per model: the terrestrial groups, bundled into an
// FAtmosphereCommonView for its two apply paths, and the gas giant groups, which
// ApplyGasGiantParams pushes directly and which never reach that view. Only
// FAtmosphereGeometryParams is instantiated by both, once each.
//
// THE GAS GIANT GROUPS ARE AUTHORITATIVE. They are laid out for how a gas giant
// is tuned rather than for what is shared, and the terrestrial model will be
// rebuilt on them.
//
// Cloud Beta sits in the model groups rather than anywhere shared because the
// two models AUTHOR it differently, not merely value it differently: the
// terrestrial band writes an absolute extinction against a cloud with holes, the
// deck solves one from a total optical depth. Same shader parameter, two
// different inputs.
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

class UGasGiantSimConfig;

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
	TObjectPtr<UGasGiantSimConfig> Config = nullptr;

	/** Start the sim on BeginPlay. Off when another actor already drives it: the
	 *  subsystem is per-world, so two planets starting it fight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	bool bStartOnBeginPlay = true;
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
	 *  profile it applies to is not a quantity: they are only ever authored as a
	 *  pair and the shader reads them from one pin. A scale height under about
	 *  half the deck's top leaves no air above the cloud and the limb loses its
	 *  halo. */
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

	/** Half-width of that layer. Separate because the layer has two parameters and
	 *  only one alpha channel. Floored, since AtmoT_Profile floors it for both the
	 *  march and the transmittance bake and a zero here would rely on that. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0001"))
	float AbsorptionFalloff = 0.1f;

	/** RGB the ambient colour, A THE FLOOR its terminator falloff lerps from. AT
	 *  ZERO FLOOR THE TERM HAS ALMOST NO RANGE: ambient stands in for light that
	 *  bounced several times and behind an opaque planet there is none, but gated
	 *  to exactly zero it is swamped by direct light everywhere it is not zero.
	 *  The floor buys it a night side, at the cost of the planet glowing where
	 *  nothing should reach it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor Ambient = FLinearColor(0.080328f, 0.080328f, 0.1f, 0.0f);
};

/** How cloud is lit. Cloud Beta and Cloud Absorption Beta are NOT here: the
 *  terrestrial band authors an absolute extinction and the deck solves one from
 *  a total optical depth, so each model owns its own input to the same shader
 *  parameter. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereCloudScatteringParams
{
	GENERATED_BODY()

	/** RGB the cloud ambient colour, A THE FLOOR, as the air's. Its own floor
	 *  because the deck and the air go dark at different rates. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor Ambient = FLinearColor(0.04f, 0.04f, 0.05f, 0.0f);

	/** (forward g, backward g, lobe blend, isotropic multiple-scatter weight). The
	 *  isotropic term keeps the shadow side off black, single scattering having
	 *  nothing to deliver there. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	FLinearColor PhaseParams = FLinearColor(0.9f, 0.1f, 0.5f, 0.5f);
};

/** The march's step budget. A BUDGET, NOT A QUALITY SETTING, and the two models
 *  spend it over different geometry: Cloud Steps crosses the terrestrial band,
 *  but on the gas giant only the deck segment the cone trace split off. Same
 *  name, same meaning, values that do not transfer. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereRaymarchParams
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1.0"))
	float AtmosphereSteps = 32.0f;

	/** Step growth with distance from the ray start. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float StepScaleFactor = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1.0"))
	float CloudSteps = 32.0f;

	/** March pixels a VIEW step is allowed to span. THE COUNTS ABOVE SIZE THE
	 *  MARCH AGAINST THE DECK, THIS AGAINST THE SCREEN: a budget divided into a
	 *  span knows nothing about where the camera is, so the same detail is
	 *  resolved at fifty planet radii as at two, where a dozen samples land inside
	 *  one pixel.
	 *
	 *  IT ONLY EVER LENGTHENS THE STEP -- close in the footprint is smaller than
	 *  the planned step and this does nothing. The march is capped by the thinnest
	 *  feature on the ray either way, so a step can never swallow the gradient it
	 *  is integrating. 1 is a step per pixel; above about 2 the deck bands along
	 *  the step lattice at distance, the jitter having less room to hide the
	 *  integration error. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.25", ClampMax = "4.0"))
	float ViewStepPixels = 1.0f;
};

/** The terrestrial groups bundled for ApplyCommonParams and
 *  ApplyTerrestrialParams.
 *
 *  BY VALUE, NOT A UPROPERTY: it is a call shape rather than authored state. The
 *  gas giant has no part in it -- ApplyGasGiantParams pushes its own groups
 *  under their own names. */
struct FAtmosphereCommonView
{
	FAtmosphereGeometryParams Geometry;
	FAtmosphereAirScatteringParams AirScattering;
	FAtmosphereCloudScatteringParams CloudScattering;
	FAtmosphereRaymarchParams Raymarch;
};

/** The terrestrial cloud band: its shell, its noise, and the extinction it is
 *  authored with. Cloud Beta stays here because this model writes it directly
 *  where the gas giant solves it; ambient, phase and the step counts are in the
 *  shared groups. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FTerrestrialCloudParams
{
	GENERATED_BODY()

	// -- Shell ---------------------------------------------------------------
	// Both fractions rather than absolutes, so the band stays inside the
	// atmosphere and the inner shell below the outer by construction.

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
	 *  layer breaks up the shape the base produced, which only reads that way if
	 *  the two stay in proportion. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape", meta = (ClampMin = "1.0"))
	float DetailFrequencyRatio = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape")
	FLinearColor DetailNoiseWeights = FLinearColor(0.2f, 0.3f, 0.3f, 0.2f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape")
	FLinearColor DetailNoiseInvert = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Shape")
	float DetailErodeStrength = 0.3f;

	// -- Lighting -----------------------------------------------------------

	/** Absolute extinction. The cloud has holes, so this makes the solid parts
	 *  read as solid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud|Lighting")
	FLinearColor CloudBeta = FLinearColor(150.0f, 145.3125f, 140.625f, 0.5f);

	/** Light-ray extinction, deliberately below Cloud Beta: light scattered INTO
	 *  the ray is what multiple scattering stands in for, so the full coefficient
	 *  would count that loss twice. */
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
struct CLOUDATMOSPHERE_API FGasGiantAtmosphereLightingParams
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

	/** READOUT, not authored: the highest every relief term together could reach,
	 *  before the ceiling. Above 1 - CeilingFalloff the tallest features are being
	 *  capped by the band, past 1 by the excess shown. */
	UPROPERTY(VisibleAnywhere, Transient, BlueprintReadOnly)
	float SolvedTopMax = 0.0f;
};

/** How the simulation shapes the deck: bands, pressure and storms. Every relief
 *  amount is a fraction of GradientThickness. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FGasGiantFlowParams
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

/** How the noise layers travel with the flow. Each layer's FlowInherit and
 *  ShearInherit say how much of it that layer follows. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FGasGiantMotionParams
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

/** Controls both noise layers' carves share. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FGasGiantSurfaceParams
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

/** One noise layer carving the deck. The deck has two with the same handles:
 *  Structure for the mid-level shape that reads from orbit, Detail for the micro
 *  variance gone within a fraction of a planet radius. Where they behave
 *  differently it is by construction rather than by handle -- the structure
 *  volume's G gates storm towers, and the detail relief's sidedness is
 *  GG_DETAIL_RELIEF_CENTRED. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FGasGiantNoiseLayerParams
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

	static FGasGiantNoiseLayerParams MakeStructureDefaults()
	{
		FGasGiantNoiseLayerParams Out;
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

	static FGasGiantNoiseLayerParams MakeDetailDefaults()
	{
		FGasGiantNoiseLayerParams Out;
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

/** How much light the deck removes. AUTHORED AS TOTAL OPTICAL DEPTH, NOT AS A
 *  COEFFICIENT: the deck's heights are fractions of atmosphere thickness, so a
 *  coefficient is solved per frame from the depth wanted, where an absolute one
 *  would mean something different after every resize and anchor move.
 *
 *  PITFALL: too low and the deck never saturates, a PERFORMANCE bug as much as a
 *  visual one -- the march's transmittance early-out is dead code until a ray
 *  can go opaque, so every ray burns its full step budget. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FGasGiantExtinctionParams
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

	/** Total optical depth from the deck top to the surface at core density, down
	 *  an unrelieved column, at any DensityCurve. Below about 8 the sky shows
	 *  through. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.1"))
	float DeckOpticalDepth = 2000.0f;

	/** Light-ray extinction as a fraction of the view ray's. Below 1, since light
	 *  scattered INTO the ray is what multiple scattering stands in for and the
	 *  full coefficient counts that loss twice. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float LightExtinctionFraction = 0.5f;
};

/** The deck's phase function and its ambient. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FGasGiantPhaseParams
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
struct CLOUDATMOSPHERE_API FGasGiantMultipleScatteringParams
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
struct CLOUDATMOSPHERE_API FGasGiantTerminatorParams
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
struct CLOUDATMOSPHERE_API FGasGiantRaymarchParams
{
	GENERATED_BODY()

	/** Steps across the air segments. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1.0"))
	float AtmosphereSteps = 64.0f;

	/** Steps across the deck band. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1.0"))
	float CloudSteps = 128.0f;

	/** Step growth with distance from the ray start. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float StepScaleFactor = 2.0f;

	/** March pixels a view step may span. The step counts size the march against
	 *  the deck, this against the screen, and it only ever lengthens the step.
	 *  Above about 2 the deck bands along the step lattice at distance. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.25", ClampMax = "4.0"))
	float ViewStepPixels = 2.0f;

	/** Bound on the deck's slope, in gradient depths per radian: the cone angle for
	 *  the entry search. Under-declaring it is the one way that search steps over
	 *  the surface, so raise it first if tangent-angle slicing appears. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.1"))
	float DeckSlope = 8.0f;
};