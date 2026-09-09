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
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereEnvironmentParams
{
	GENERATED_BODY()

	// -- Light --------------------------------------------------------------

	/** RGB direction is the hue, RGB magnitude is the intensity. The march and
	 *  the directional light both derive from this, so they cannot disagree
	 *  about the star.
	 *
	 *  Light DIRECTION is not here: it comes from the actor's rotation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light")
	FLinearColor LightColor = FLinearColor(30.0f, 28.5f, 27.0f, 10.0f);

	// -- Composite ----------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Composite", meta = (ClampMin = "0.0"))
	float BlurFalloffFactor = 2.0f;

	/** Blur weight at the planet edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Composite", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MaxBlurWeight = 0.5f;

	/** Blur weight everywhere else, as a fraction of MaxBlurWeight. A ratio so
	 *  the floor cannot exceed the peak. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Composite", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinBlurFraction = 0.0f;

	/** MinW, derived. */
	float GetMinBlurWeight() const { return MaxBlurWeight * MinBlurFraction; }
};

/** The shell, the air, the march budget and the cloud lighting: everything both
 *  march materials declare and read the same way.
 *
 *  ONE INSTANCE PER MODEL, and nothing here is shared between them. This is one
 *  definition of what each parameter MEANS, with a separate value per model.
 *
 *  Defaults come from the two factories at the bottom, not from a details-panel
 *  edit: the member initialisers below are the terrestrial set, and the gas
 *  giant factory states its deltas against them. A member added without a
 *  matching delta therefore takes the terrestrial value on both models, which
 *  is silent. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereCommonParams
{
	GENERATED_BODY()

	// -- Geometry -----------------------------------------------------------
	//
	// Planet Center and Planet Radius come from the actor's transform, not from
	// here. Everything below is a fraction of the radius, so resizing the
	// planet moves the whole system together.

	/** Atmosphere top, as a fraction of planet radius above the surface. The
	 *  ceiling every other shell in the system is expressed against. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry", meta = (ClampMin = "0.001"))
	float AtmosphereHeightScale = 0.2f;

	/** Vertical offset applied to the atmosphere floor. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Geometry")
	float AtmosphereFloorOffset = 0.0f;

	// -- Air scattering -----------------------------------------------------
	//
	// Coefficients and scale heights are divided by atmosphere thickness in
	// Atmo_BuildParams, so they are thickness-relative and survive a resize.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air Scattering")
	FLinearColor RayleighBeta = FLinearColor(0.896360f, 2.913294f, 4.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air Scattering")
	float RayleighHeight = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air Scattering")
	FLinearColor MieBeta = FLinearColor(1.0f, 0.83163f, 0.71612f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air Scattering")
	float MieHeight = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air Scattering", meta = (ClampMin = "-0.99", ClampMax = "0.99"))
	float MieG = 0.9f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air Scattering")
	FLinearColor AtmosphereAbsorptionBeta = FLinearColor(0.05f, 0.05f, 0.05f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air Scattering")
	float AtmosphereAbsorptionHeight = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air Scattering")
	float AtmosphereAbsorptionFalloff = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Air Scattering")
	FLinearColor AtmosphereAmbient = FLinearColor(0.080328f, 0.080328f, 0.1f, 0.0f);

	// -- Cloud lighting -----------------------------------------------------
	//
	// Cloud Beta and Cloud Absorption Beta are NOT here. The terrestrial band
	// authors an absolute extinction; the deck solves one from a total optical
	// depth. Each model owns its own input to the same shader parameter.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Lighting")
	FLinearColor CloudAmbient = FLinearColor(0.04f, 0.04f, 0.05f, 0.0f);

	/** (forward g, backward g, lobe blend, isotropic multiple-scatter weight).
	 *  The isotropic term is what keeps the shadow side off black -- single
	 *  scattering has nothing to deliver there. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Lighting")
	FLinearColor CloudPhaseParams = FLinearColor(0.9f, 0.1f, 0.5f, 0.5f);

	// -- Raymarching --------------------------------------------------------
	//
	// A BUDGET, NOT A QUALITY SETTING, and the two models spend it over
	// different geometry. Cloud Steps crosses the terrestrial band, but on the
	// gas giant it crosses only the deck segment the cone trace split off.
	// Same name, same meaning, values that do not transfer.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Raymarching", meta = (ClampMin = "1.0"))
	float AtmosphereSteps = 32.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Raymarching", meta = (ClampMin = "1.0"))
	float AtmosphereLightSteps = 16.0f;

	/** Step growth with distance from the ray start. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Raymarching", meta = (ClampMin = "0.0"))
	float StepScaleFactor = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Raymarching", meta = (ClampMin = "1.0"))
	float CloudSteps = 32.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Raymarching", meta = (ClampMin = "1.0"))
	float CloudLightSteps = 32.0f;

	// -- Derivations --------------------------------------------------------

	/** Atmosphere outer radius in world units. */
	float GetAtmosphereRadius(float PlanetRadius) const
	{
		return PlanetRadius * (1.0f + AtmosphereHeightScale);
	}

	// -- Defaults -----------------------------------------------------------

	/** The member initialisers above, unmodified. */
	static FAtmosphereCommonParams MakeTerrestrialDefaults()
	{
		return FAtmosphereCommonParams();
	}

	/** Deltas against the terrestrial set. Everything not listed is identical
	 *  today and free to diverge -- carrying two instances is what makes that
	 *  a value edit rather than a code change. */
	static FAtmosphereCommonParams MakeGasGiantDefaults()
	{
		FAtmosphereCommonParams Params;

		// A shell of a third of a planet radius. The deck's world thickness is
		// solved from this, so it sets the deck's depth as much as the air's.
		Params.AtmosphereHeightScale = 0.3f;

		// Stronger and bluer than the terrestrial air, and a scale height that
		// keeps the air extending past the deck's crests rather than dying
		// under them -- below about half the deck's top there is no Rayleigh
		// left above the cloud and the limb reads as a hard edge.
		Params.RayleighBeta = FLinearColor(5.291138f, 23.918282f, 32.0f, 1.0f);
		Params.RayleighHeight = 0.2f;

		// The deck fills the disc, so the air segment above it is crossed by
		// every ray rather than only the ones that miss the planet.
		Params.AtmosphereSteps = 128.0f;
		Params.StepScaleFactor = 3.0f;

		// The deck segment gets a much finer march than the terrestrial band:
		// the whole image is deck, and the gradient is where every feature is.
		Params.CloudSteps = 128.0f;

		// A gas giant has no holes, so nearly every light sample accumulates
		// and takes the second fetch, where the terrestrial version leans on
		// Cloud_Density returning zero over most of the domain. Both light
		// budgets are cut hard to pay for the view steps above.
		Params.AtmosphereLightSteps = 4.0f;
		Params.CloudLightSteps = 12.0f;

		return Params;
	}
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shell", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CloudOuterFraction = 0.667f;

	/** Cloud band base, as a fraction of the band top. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shell", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CloudInnerFraction = 0.125f;

	// -- Shape --------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape")
	TObjectPtr<UVolumeTexture> CloudVolumeTexture = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape")
	FLinearColor AnimationWeights = FLinearColor(0.0f, 0.0f, 0.0f, 990.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape")
	float CloudCoverage = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape")
	float CloudDensityMultiplier = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape")
	float CloudHeightCurveMin = 0.3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape")
	float CloudHeightCurveMax = 0.7f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape")
	float CloudNoiseFrequency = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape")
	FLinearColor CloudNoiseWeights = FLinearColor(0.55f, 0.3f, 0.15f, 0.3f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape")
	FLinearColor CloudNoiseInvert = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);

	/** Detail frequency as a multiple of the base. A ratio because the detail
	 *  layer's job is to break up the shape the base produced, and that reading
	 *  only holds if the two stay in proportion. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape", meta = (ClampMin = "1.0"))
	float DetailFrequencyRatio = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape")
	FLinearColor DetailNoiseWeights = FLinearColor(0.2f, 0.3f, 0.3f, 0.2f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape")
	FLinearColor DetailNoiseInvert = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shape")
	float DetailErodeStrength = 0.3f;

	// -- Lighting -----------------------------------------------------------

	/** Absolute extinction. The cloud has holes, so this is what makes the
	 *  solid parts read as solid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lighting")
	FLinearColor CloudBeta = FLinearColor(150.0f, 145.3125f, 140.625f, 0.5f);

	/** Light-ray extinction, deliberately below Cloud Beta: light scattered
	 *  INTO the ray is what the multiple-scattering term stands in for, so the
	 *  full scattering coefficient would count that loss twice. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lighting")
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
 *  RELIEF IS THEREFORE UNBOUNDED BELOW. Only the ceiling check remains:
 *  GetTopMax() returns an atmosphere fraction and has to stay at or below 1.
 *  GetTopMin() says how far the deepest troughs fall, which is a tuning readout
 *  rather than a limit. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FGasGiantDeckParams
{
	GENERATED_BODY()

	// -- Shell --------------------------------------------------------------
	//
	// Measured from the planet surface. GetTopMax() must stay at or below 1:
	// above it Atmo_Plan clips the tallest columns, slicing the tops off
	// exactly where features are tallest, which reads as a field bug.

	/** Base of the deck top, before relief moves it. Relief adds and subtracts
	 *  around this, so the highest point is GetTopMax() rather than this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shell", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DeckTop = 0.8f;

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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shell", meta = (ClampMin = "0.0001", ClampMax = "1.0"))
	float GradientThickness = 0.4f;

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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shell", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DeckBackstop = 0.3f;

	/** Where the mass sits inside the gradient, without moving either boundary.
	 *  1 is centred, below 1 pulls density toward the top.
	 *
	 *  ABOVE 0.5 THE ONSET IS C1. At 0.5 the slope at the deck top goes finite
	 *  instead of zero, which creases along the whole top; below it the top
	 *  hardens into an edge. Both are allowed -- a sharp cloud top is a real
	 *  discontinuity, not a bug -- so this is a look control rather than a
	 *  bounded one. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shell", meta = (ClampMin = "0.0001", ClampMax = "4.0"))
	float DensityCurve = 1.0f;

	// -- Relief -------------------------------------------------------------
	//
	// Fractions of GradientThickness, all of them, moving the deck top around
	// DeckTop. 1 is one whole gradient. Nothing bounds the downward terms --
	// they meet the backstop -- so only GetTopMax() has to be watched.

	/** Height between a jet and a zone. Wants to be large: the point of driving
	 *  height from the flow is that bands are geometry rather than a pattern
	 *  painted on a sphere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief")
	float BandRelief = -0.5f;

	/** How far pressure lifts the deck. Anticyclones rise, cyclones sink. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief")
	float PressureLift = 0.1f;

	/** Added height of a convective tower, where the vortex gate is open. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief")
	float StormTowers = 0.1f;

	/** How far the flow's strain collapses band relief toward flat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief", meta = (ClampMin = "0.0"))
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief")
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief")
	float StructureRelief = 0.5f;

	/** Vortex strength above which storm towers are allowed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VortexThreshold = 0.9f;

	// -- Detail -------------------------------------------------------------

	/** Horizontal feature size of the detail volume. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.01"))
	float DetailScale = 24.0f;

	/** Structure layer scale, as a multiple of DetailScale. Below 1 makes it the
	 *  COARSER layer, which is what its job wants: mid-level shaping that stays
	 *  resolvable from orbit while the detail layer tiles finely up close. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.01"))
	float StructureScaleRatio = 0.05f;

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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.01"))
	float DetailAspect = 3.0f;

	/** STRUCTURE layer: the same, against its own horizontal scale.
	 *
	 *  SEPARATE FROM DetailAspect BECAUSE THE LAYERS WANT DIFFERENT SHAPES.
	 *  Detail is filaments that stretch along the streamlines; structure is
	 *  rounded mid-level shaping. Tying them means one horizontal scale change
	 *  retunes the other layer's silhouette. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.01"))
	float StructureAspect = 8.0f;

	/** How much of the coarse warp the detail layer inherits. A displacement
	 *  field with a large gradient IS strain, so inheriting one whole imposes
	 *  an order-one strain on the fine layer regardless of its own flow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DetailWarpInherit = 0.4f;

	/** How much the structure layer inherits. 0 is legitimate: this layer breaks
	 *  the flow into rounded shapes, and a shape dragged through the flow field
	 *  is no longer round. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0", ClampMax = "1.0"))
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Level Of Detail", meta = (ClampMin = "0.0"))
	float DetailFadeNear = 0.0f;

	/** Detail layer: fade width, added to Near. Additive rather than a
	 *  multiple, so the transition width is independent of where it starts and
	 *  a tight fade close in is authorable. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Level Of Detail", meta = (ClampMin = "0.0"))
	float DetailFadeSpan = 0.2f;

	/** Structure layer: where it starts fading. An order of magnitude further out,
	 *  because this is the mid-level shaping that has to read from orbit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Level Of Detail", meta = (ClampMin = "0.0"))
	float StructureFadeNear = 0.2f;

	/** Structure layer: fade width, added to Near. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Level Of Detail", meta = (ClampMin = "0.0"))
	float StructureFadeSpan = 0.4f;

	// Weights over each volume's three Worley rungs, coarse to fine.
	//
	// INDEPENDENT FIELDS ON SEPARATE SEEDS, each carrying its own octave stack
	// from its base scale down. Raising a rung adds a distinct pattern rather
	// than more of what the others already say.
	//
	// Normalized shader-side by the weights' length, so contrast holds however
	// the balance is set and these are purely look controls. Strength lives in
	// the Amount beside them.
	//
	// Six scalars rather than two vectors, so each field's name in the details
	// panel is the name of the material parameter it feeds.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail")
	float DetailWorleyCoarse = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail")
	float DetailWorleyMid = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail")
	float DetailWorleyFine = 0.25f;

	/** How strongly the detail layer carves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0"))
	float DetailAmount = 1.0f;

	/** Weighted toward the coarse rung the structure layer gives rounded
	 *  billows; toward the fine one, cellular breakup. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail")
	float StructureWorleyCoarse = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail")
	float StructureWorleyMid = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail")
	float StructureWorleyFine = 0.25f;

	/** How strongly the structure layer shapes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0"))
	float StructureAmount = 1.0f;

	/** How much of either layer survives in the flat band interiors, against
	 *  full strength at the edges where a real gas giant's billows live. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float EdgeBias = 1.0f;

	/** How much the DETAIL layer erodes the density. Below 1: there is no lower
	 *  cloud shell, so a fully transparent column would let a ray run to the far
	 *  side of the planet, and the erosion floor is what makes that
	 *  impossible. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float DetailErosion = 0.9f;

	/** How much the PACKED layer erodes the density. Separate because the two
	 *  layers survive to different distances: the structure one carries shape that
	 *  has to read from orbit, the detail one is micro variance that is gone
	 *  within a fraction of a planet radius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float StructureErosion = 0.9f;

	/** How far down the gradient erosion reaches, as a fraction of it. 0 is
	 *  surface only, 1 reaches the saturated region.
	 *
	 *  NORMALIZED AGAINST THE COLUMN'S OWN GRADIENT, so erosion finishes exactly
	 *  where that column saturates. An absolute depth still has carve left at
	 *  the floor of a column the backstop compressed, which is where the
	 *  solid-region skip takes over. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ErosionDepth = 1.0f;

	// -- Flow ---------------------------------------------------------------

	/** Warp duration. A short advection whose only job is to carry the baked
	 *  volumes along the current flow -- the history lives in the sim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flow", meta = (ClampMin = "0.0"))
	float WarpTime = 0.075f;

	/** Detail layer warp, as a fraction of WarpTime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flow", meta = (ClampMin = "0.0"))
	float DetailWarpRatio = 1.0f;

	/** Shifts which band type dominates without retuning the sim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flow")
	float BandBias = -0.33f;

	/** Turbulence floor in band interiors. Real zone interiors are calmer than
	 *  their edges but not glass. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flow", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float TurbulenceFloor = 0.5f;

	/** Multiplies already-normalized vorticity, so 1 is neutral and the useful
	 *  range is roughly 0.5 to 3. Too high flattens the elevation to its
	 *  asymptote everywhere but the boundaries, turning the height field into
	 *  terraces joined by cliffs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flow", meta = (ClampMin = "0.0"))
	float BandSharpness = 1.0f;

	/** The planet's own rotation, radians per unit of simulated time. The sim
	 *  runs in the rotating frame, so this rotates the sampling position rather
	 *  than the flow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flow")
	float RigidRate = 0.0f;

	/** Which sim layer drives the coarse warp and the band signal. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flow", meta = (ClampMin = "0"))
	int32 FlowLayer = 0;

	/** Which sim layer advects the detail. A different layer from FlowLayer is
	 *  what produces genuine vertical wind shear. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flow", meta = (ClampMin = "0"))
	int32 DeepFlowLayer = 1;

	/** Bound on the deck's slope, GRADIENT DEPTHS per radian -- the same unit as
	 *  the relief that produces the slope, so it stays in step through an anchor
	 *  retune instead of silently becoming an under-declaration. The cone angle
	 *  for the entry search: under-declaring it is the one way the search steps
	 *  over the surface, so raise it first if tangent-angle slicing appears. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flow", meta = (ClampMin = "0.1"))
	float DeckSlope = 8.0f;

	// -- Sources ------------------------------------------------------------

	/** Micro variance. R is a Perlin FBM, GBA a Worley octave ladder on separate
	 *  seeds, all median-centred and equalized per channel.
	 *
	 *  SAMPLER: wrap on all three axes, Linear Color -- these are scalar fields
	 *  and an sRGB decode curves them without erroring. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sources")
	TObjectPtr<UVolumeTexture> DetailVolume = nullptr;

	/** Mid-level shape, same channel layout as the detail volume. G doubles as
	 *  the storm-tower gate, so it stays the coarsest rung.
	 *
	 *  A SEPARATE ASSET FOR DIFFERENT SEEDS, not because one texture could not
	 *  serve both positions -- the two layers are sampled at different points
	 *  and would be two fetches either way. Shared seeds would make the layers
	 *  rhyme, and coincident features across two scales read as a repeat rather
	 *  than as depth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sources")
	TObjectPtr<UVolumeTexture> StructureVolume = nullptr;

	/** Owns the flow render target the material samples and the settings the
	 *  sim subsystem steps against. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sources")
	TObjectPtr<UGasGiantSimConfig> SimConfig = nullptr;

	/** Start the sim on BeginPlay. Off when another actor already drives it:
	 *  the subsystem is per-world, so two planets starting it fight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sources")
	bool bStartSimulationOnBeginPlay = true;

	// -- Derivations --------------------------------------------------------

	/** Ladder weights plus the layer's amount, as the material expects them. */
	FLinearColor GetDetailNoise() const
	{
		return FLinearColor(DetailWorleyCoarse, DetailWorleyMid, DetailWorleyFine, DetailAmount);
	}

	FLinearColor GetStructureNoise() const
	{
		return FLinearColor(StructureWorleyCoarse, StructureWorleyMid,
			StructureWorleyFine, StructureAmount);
	}

	/** How far the downward relief terms take a column top below DeckTop,
	 *  in gradients. Feeds GetTopMin; nothing bounds it. */
	float GetReliefBudget() const
	{
		return 0.5f * FMath::Abs(BandRelief)
			+ FMath::Abs(PressureLift)
			+ FMath::Max(DetailRelief, 0.0f)
			+ 0.5f * FMath::Abs(StructureRelief);
	}

	/** The highest the deck can reach, as an atmosphere fraction. Closed form,
	 *  and it must match GG_TopBounds in GasGiantFlow.ush -- every term in
	 *  GG_CloudTop appears, each at its most generous.
	 *
	 *  Elevation is in [0,1] so the band contributes half of BandRelief either
	 *  side of the base. Pressure is soft-saturated to [-1,1] sim-side so it
	 *  contributes the whole of PressureLift. The structure carve is signed, so
	 *  half of it lands in each bound; the detail carve is one-sided and lands
	 *  in whichever bound its sign points at.
	 *
	 *  Keep this at or below 1: it is the deck's top against the atmosphere
	 *  ceiling. */
	float GetTopMax() const
	{
		return DeckTop + GradientThickness * (
			0.5f * FMath::Abs(BandRelief)
			+ FMath::Abs(PressureLift)
			+ 0.5f * FMath::Abs(StructureRelief)
			+ FMath::Max(-DetailRelief, 0.0f)
			+ FMath::Max(StormTowers, 0.0f));
	}

	/** The lowest a column top can fall. Below DeckBackstop its gradient has been
	 *  clamped to a step; below DeckBackstop + GradientThickness it is compressed.
	 *  Also the altitude under which every column is saturated, which the shader
	 *  counts its opaque terminus down from. */
	float GetTopMin() const
	{
		return DeckTop - GradientThickness * GetReliefBudget();
	}

	/** Where the gradient bottoms out at a column with no relief. Matches
	 *  GG_DeckFloor in GasGiantFlow.ush, which does the same clamp per column. */
	float GetNominalFloor() const
	{
		return FMath::Min(FMath::Max(DeckTop - GradientThickness, DeckBackstop), DeckTop);
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
		return FLinearColor(DeckTop, BandRelief, PressureLift, StormTowers);
	}

	FLinearColor GetScales() const
	{
		return FLinearColor(
			DetailScale,
			DetailScale * StructureScaleRatio,
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

	FLinearColor GetLayers() const
	{
		return FLinearColor(
			static_cast<float>(FlowLayer),
			static_cast<float>(DeepFlowLayer),
			DeckSlope,
			0.0f);
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
		return DetailScale * StructureScaleRatio * StructureAspect * AtmosphereHeightScale;
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bands")
	FLinearColor ScatterNegative = FLinearColor(0.241319f, 0.0f, 0.001502f, 1.0f);

	/** Anticyclonic bands. "Darker bands eat more light" is this alpha against
	 *  ScatterNegative's. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bands")
	FLinearColor ScatterPositive = FLinearColor(0.129427f, 0.124283f, 0.348958f, 1.0f);

	/** Band boundaries, where vorticity crosses zero. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bands")
	FLinearColor ScatterBase = FLinearColor(0.331597f, 0.273795f, 0.193569f, 1.0f);

	/** Where the band ramp saturates. Matching this to the inverse of the sim
	 *  debug view's DebugScale makes the material and the debug view agree
	 *  about where band boundaries are. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bands", meta = (ClampMin = "0.0"))
	float BandScale = 2.0f;

	// -- Terminator -----------------------------------------------------------
	//
	// Four scalars that shape the day-night transition, tuned against each
	// other rather than individually. They reach the shader as one LobeParams
	// float4.

	/** Width of the planet-shadow falloff, as a fraction of atmosphere
	 *  thickness. The geometric shadow of a sphere has a hard boundary; a real
	 *  terminator does not, because a grazing sun ray crosses progressively more
	 *  air before it arrives. This stands in for that without marching it.
	 *
	 *  WIDE, because this is what shapes the terminator. Narrowing it to stop
	 *  the forward lobe leaking makes the terminator hard AND exposes the
	 *  smoothstep's own endpoints as edges along the light's tangent cone.
	 *  LobeShadowPower is the control for the leak. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terminator", meta = (ClampMin = "0.0001"))
	float TerminatorSoftness = 0.35f;

	/** Width of the ambient terminator, in cosine of sun elevation. 0.15 is
	 *  about 9 degrees either side of the geometric terminator.
	 *
	 *  AMBIENT IS STARLIGHT THAT BOUNCED, NOT A FLOOR. Both ambient terms stand
	 *  in for light that scattered several times before arriving, and behind an
	 *  opaque planet there is none. Applied unconditionally they wash the night
	 *  side at a fixed brightness, which reads as the star shining through the
	 *  planet -- and on a deck opaque to the limb, nothing breaks it up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terminator", meta = (ClampMin = "0.0001", ClampMax = "1.0"))
	float AmbientTerminator = 0.15f;

	/** How fast the Mie forward lobe dies behind deck: optical depths of deck
	 *  for an e-fold, inverted, so 2 leaves the lobe at 2% one optical depth in.
	 *
	 *  THE LOBE BELONGS TO SINGLE SCATTERING. Light that has crossed a dense
	 *  medium has bounced several times and lost its direction, so the sharp
	 *  forward peak washes out to something near isotropic. Carrying the full
	 *  phase through the deck makes the glow sit on top of the planet rather
	 *  than in front of it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terminator", meta = (ClampMin = "0.0"))
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terminator", meta = (ClampMin = "0.0"))
	float LobeShadowPower = 6.0f;

	// -- Extinction ---------------------------------------------------------
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Extinction", meta = (ClampMin = "0.1"))
	float DeckOpticalDepth = 40.0f;

	/** Per-channel tint on that depth. Wavelength-dependent extinction, on top
	 *  of the albedo in the scatter sets. Neutral at (1,1,1). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Extinction")
	FLinearColor ExtinctionTint = FLinearColor(1.0f, 0.969f, 0.938f, 1.0f);

	/** Light-ray extinction as a fraction of the view ray's.
	 *
	 *  Below 1 on purpose: light scattered INTO the ray is what the
	 *  multiple-scattering term stands in for, so the full coefficient would
	 *  count that loss twice and the deck would read as flat black on the
	 *  shadow side. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Extinction", meta = (ClampMin = "0.0", ClampMax = "1.0"))
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