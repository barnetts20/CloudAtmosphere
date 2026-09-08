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
	FLinearColor LightColor = FLinearColor(25.0f, 23.75f, 22.5f, 10.0f);

	// -- Composite ----------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Composite", meta = (ClampMin = "0.0"))
	float BlurFalloffFactor = 2.0f;

	/** Blur weight at the planet edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Composite", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MaxBlurWeight = 0.5f;

	/** Blur weight everywhere else, as a fraction of MaxBlurWeight. A ratio so
	 *  the floor cannot exceed the peak. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Composite", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinBlurFraction = 0.1f;

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

		// A shell of one planet radius. The deck's world thickness is solved
		// from this, so it sets the deck's depth as much as the air's.
		Params.AtmosphereHeightScale = 1.0f;

		// The deck fills the disc, so the air segment above it is crossed by
		// every ray rather than only the ones that miss the planet.
		Params.AtmosphereSteps = 128.0f;
		Params.StepScaleFactor = 4.0f;

		// The deck segment gets a finer march than the terrestrial band.
		Params.CloudSteps = 64.0f;

		// A gas giant has no holes, so nearly every light sample accumulates
		// and takes the second fetch, where the terrestrial version leans on
		// Cloud_Density returning zero over most of the domain.
		Params.CloudLightSteps = 16.0f;

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
 *  TWO ABSOLUTES, EVERYTHING ELSE DIMENSIONLESS. DeckTop and DeckBottom are
 *  fractions of atmosphere height; their difference is the gradient depth, and
 *  every relief amount and both carves are fractions of THAT.
 *
 *  RELIEF RIDES ON THE GRADIENT SO THE DECK CANNOT OUTGROW ITSELF. Containment
 *  is two-sided -- the deck top has to stay under the atmosphere ceiling and
 *  above its own floor -- and the floor binds first, because the gradient is a
 *  fraction of the shell while the headroom above is most of it. Expressed this
 *  way the floor check is a pure number, GetReliefBudget() <= 1, and it holds
 *  through any retune of either anchor. Against atmosphere height instead,
 *  every move of DeckTop needs relief retuned with it or the troughs punch
 *  through the floor and the solid-region skip fills them in.
 *
 *  The ceiling check is unchanged in form: GetTopMax() still returns an
 *  atmosphere fraction and still has to stay at or below 1. */
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
	float DeckTop = 0.25f;

	/** Where density reaches its peak. Below this the deck is solid, the
	 *  march skips six of its seven fetches and the inner cull is exact.
	 *
	 *  FLAT, NOT FOLLOWING RELIEF -- a pressure level rather than a cloud
	 *  surface. Every column reaches core density here however high its top
	 *  sits, so the gradient compresses in troughs and stretches over crests.
	 *
	 *  Relief is a fraction of the distance up to DeckTop, so keeping
	 *  GetReliefBudget() at or below 1 puts every column's top above this by
	 *  construction. Past 1 the deepest troughs become steps rather than
	 *  gradients and the solid-region skip fills them in. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shell", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DeckBottom = 0.15f;

	/** Where the mass sits inside the gradient, without moving either boundary.
	 *  1 is centred, below 1 pulls density toward the top.
	 *
	 *  FLOORED AT 0.5: the profile owes zero derivative at both ends, and the
	 *  exponent preserves it only above a half. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shell", meta = (ClampMin = "0.5", ClampMax = "4.0"))
	float DensityCurve = 1.5f;

	// -- Relief -------------------------------------------------------------
	//
	// Fractions of the gradient depth, all of them, moving the deck top around
	// DeckTop. 1 is the whole distance down to the floor, so the downward terms
	// summed -- GetReliefBudget() -- is what has to stay at or below 1.

	/** Height between a jet and a zone. Wants to be large: the point of driving
	 *  height from the flow is that bands are geometry rather than a pattern
	 *  painted on a sphere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief")
	float BandRelief = -0.75f;

	/** How far pressure lifts the deck. Anticyclones rise, cyclones sink. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief")
	float PressureLift = 0.15f;

	/** Added height of a convective tower, where the vortex gate is open. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief")
	float StormTowers = 0.25f;

	/** How far the flow's strain collapses band relief toward flat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief", meta = (ClampMin = "0.0"))
	float ReliefThinning = 0.0f;

	/** How deep the DETAIL layer carves the deck top.
	 *
	 *  NOT A RATIO OF BandRelief, deliberately. Summed before the relief
	 *  multiply, raising the band relief scales the fine noise by the same
	 *  factor, and noise stretched vertically but not horizontally becomes
	 *  spikes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief", meta = (ClampMin = "0.0"))
	float DetailRelief = 1.0f;

	/** How deep the PACKED layer carves it. Larger than the detail layer's:
	 *  this is the mid-level shaping that gives the deck its silhouette, and it
	 *  has to survive to a distance where the detail layer is long gone. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief", meta = (ClampMin = "0.0"))
	float StructureRelief = 0.75f;

	/** Vortex strength above which storm towers are allowed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VortexThreshold = 0.9f;

	// -- Detail -------------------------------------------------------------

	/** Horizontal feature size of the detail volume. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.01"))
	float DetailScale = 10.0f;

	/** Structure layer scale, as a multiple of DetailScale. Below 1 makes it the
	 *  COARSER layer, which is what its job wants: mid-level shaping that stays
	 *  resolvable from orbit while the detail layer tiles finely up close. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.01"))
	float StructureScaleRatio = 0.3f;

	/** DETAIL layer: vertical feature size against its horizontal one. Their
	 *  ratio IS the aspect of the resulting structure, so the ratio is the real
	 *  control and the absolute vertical rate is derived from it.
	 *
	 *  PITFALL: an exaggerated aspect does not read as tall noise. The UVW scale
	 *  swings by a large factor across the shell, so the pattern RESCALES with
	 *  sample altitude -- and since altitude within a step moves with step size,
	 *  which grows with distance, it swims as the camera pulls back. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.01"))
	float DetailAspect = 2.0f;

	/** STRUCTURE layer: the same, against its own horizontal scale.
	 *
	 *  SEPARATE FROM DetailAspect BECAUSE THE LAYERS WANT DIFFERENT SHAPES.
	 *  Detail is filaments that stretch along the streamlines; structure is
	 *  rounded mid-level shaping. Tying them means one horizontal scale change
	 *  retunes the other layer's silhouette. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.01"))
	float StructureAspect = 3.0f;

	/** How much of the coarse warp the detail layer inherits. A displacement
	 *  field with a large gradient IS strain, so inheriting one whole imposes
	 *  an order-one strain on the fine layer regardless of its own flow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DetailWarpInherit = 0.4f;

	/** How much the structure layer inherits. 0 is legitimate: this layer breaks
	 *  the flow into rounded shapes, and a shape dragged through the flow field
	 *  is no longer round. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StructureWarpInherit = 1.0f;

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
	float DetailFadeSpan = 0.33f;

	/** Structure layer: where it starts fading. An order of magnitude further out,
	 *  because this is the mid-level shaping that has to read from orbit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Level Of Detail", meta = (ClampMin = "0.0"))
	float StructureFadeNear = 0.33f;

	/** Structure layer: fade width, added to Near. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Level Of Detail", meta = (ClampMin = "0.0"))
	float StructureFadeSpan = 0.66f;

	/** Weights over the DETAIL volume's three Worley rungs: GBA, coarse to fine.
	 *
	 *  INDEPENDENT FIELDS ON SEPARATE SEEDS, each carrying its own octave stack
	 *  from its base scale down. Raising a rung adds a distinct pattern rather
	 *  than more of what the others already say.
	 *
	 *  Normalized shader-side by the weights' length, so contrast holds however
	 *  the balance is set and this is purely a look control. Strength lives in
	 *  DetailAmount. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail")
	FVector DetailWorleyWeights = FVector(1.0, 0.5, 0.25);

	/** How strongly the detail layer carves. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0"))
	float DetailAmount = 0.5f;

	/** The same three rungs for the STRUCTURE volume. Weighted toward the coarse
	 *  one it gives rounded billows; toward the fine one, cellular breakup. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail")
	FVector StructureWorleyWeights = FVector(1.0, 0.5, 0.25);

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
	float DetailErosion = 0.7f;

	/** How much the PACKED layer erodes the density. Separate because the two
	 *  layers survive to different distances: the structure one carries shape that
	 *  has to read from orbit, the detail one is micro variance that is gone
	 *  within a fraction of a planet radius. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float StructureErosion = 0.99f;

	/** How far down the gradient erosion reaches, as a fraction of it. 0 is
	 *  surface only, 1 reaches the solid region.
	 *
	 *  NORMALIZED AGAINST THE COLUMN'S OWN GRADIENT, so erosion finishes exactly
	 *  at DeckBottom for every column -- which is what makes the solid-region
	 *  skip exact. An absolute depth still has carve left at the floor in a
	 *  trough, where the gradient is compressed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ErosionDepth = 0.4f;

	// -- Flow ---------------------------------------------------------------

	/** Warp duration. A short advection whose only job is to carry the baked
	 *  volumes along the current flow -- the history lives in the sim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flow", meta = (ClampMin = "0.0"))
	float WarpTime = 0.05f;

	/** Detail layer warp, as a fraction of WarpTime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flow", meta = (ClampMin = "0.0"))
	float DetailWarpRatio = 0.4f;

	/** Shifts which band type dominates without retuning the sim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flow")
	float BandBias = -0.33f;

	/** Turbulence floor in band interiors. Real zone interiors are calmer than
	 *  their edges but not glass. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flow", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float TurbulenceFloor = 0.3f;

	/** Multiplies already-normalized vorticity, so 1 is neutral and the useful
	 *  range is roughly 0.5 to 3. Too high flattens the elevation to its
	 *  asymptote everywhere but the boundaries, turning the height field into
	 *  terraces joined by cliffs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flow", meta = (ClampMin = "0.0"))
	float BandSharpness = 0.75f;

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

	/** Micro variance. Only the alpha channel is read, as the filament network.
	 *  SAMPLER: wrap on all three axes, Linear Color -- these are scalar fields
	 *  and an sRGB decode curves them without erroring. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sources")
	TObjectPtr<UVolumeTexture> DetailVolume = nullptr;

	/** Mid-level shape. RGB are read as Perlin, Worley F1 and Ridged; G also
	 *  gates the storm towers.
	 *
	 *  A SEPARATE TEXTURE BECAUSE IT IS SAMPLED AT A DIFFERENT POSITION. Detail
	 *  inherits most of the flow warp so its filaments stretch along the
	 *  streamlines; structure inherits almost none so its shapes stay round.
	 *  One volume cannot serve both, whatever its channel layout. */
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
		return FLinearColor(DetailWorleyWeights.X, DetailWorleyWeights.Y,
			DetailWorleyWeights.Z, DetailAmount);
	}

	FLinearColor GetStructureNoise() const
	{
		return FLinearColor(StructureWorleyWeights.X, StructureWorleyWeights.Y,
			StructureWorleyWeights.Z, StructureAmount);
	}

	/** Gradient depth at a column with no relief, and the unit every relief
	 *  amount is a fraction of. The grain handle: widen it and the deck top
	 *  spreads over more march steps.
	 *
	 *  Relief scales with it, so widening to quiet the grain also raises the
	 *  bands. That is the deck getting deeper rather than a side effect -- the
	 *  alternative leaves relief fixed while the room it has to move in
	 *  changes, which is what puts troughs through the floor. */
	float GetGradientDepth() const
	{
		return DeckTop - DeckBottom;
	}

	/** How much of the gradient the downward relief terms claim between them.
	 *
	 *  AT OR BELOW 1 AND THE FLOOR IS SAFE: every column's top then sits at or
	 *  above DeckBottom, which is what keeps the solid-region skip exact. Past
	 *  1 only the rare joint minimum punches through, so it degrades rather
	 *  than breaks -- but it degrades invisibly, as troughs quietly filling in
	 *  with solid deck. */
	float GetReliefBudget() const
	{
		return 0.5f * FMath::Abs(BandRelief)
			+ FMath::Abs(PressureLift)
			+ FMath::Abs(DetailRelief)
			+ FMath::Abs(StructureRelief);
	}

	/** The highest the deck can reach, as an atmosphere fraction. Closed form,
	 *  and it must match GG_TopBounds in GasGiantFlow.ush -- every term in
	 *  GG_CloudTop appears, each at its most generous.
	 *
	 *  Elevation is in [0,1] so the band contributes half of BandRelief either
	 *  side of the base. Pressure is soft-saturated to [-1,1] sim-side so it
	 *  contributes the whole of PressureLift. The carves are subtractive and
	 *  belong to the lower bound only.
	 *
	 *  Keep this at or below 1: it is the deck's top against the atmosphere
	 *  ceiling. */
	float GetTopMax() const
	{
		return DeckTop + GetGradientDepth() * (
			0.5f * FMath::Abs(BandRelief)
			+ FMath::Abs(PressureLift)
			+ FMath::Max(StormTowers, 0.0f));
	}

	/** The lowest the deck top can fall, both carves included. Equals
	 *  DeckBottom exactly when GetReliefBudget() is 1. */
	float GetTopMin() const
	{
		return DeckTop - GetGradientDepth() * GetReliefBudget();
	}

	/** Atmosphere thickness in world units: the one absolute length the field
	 *  reads, and the unit every fraction above is in. The deck has no shell of
	 *  its own -- the anchors place it inside the air, so sizing the air does
	 *  not resize the deck and a thick deck does not force thick air. */
	float GetAtmosphereThickness(float PlanetRadius, float AtmosphereHeightScale) const
	{
		return PlanetRadius * AtmosphereHeightScale;
	}

	/** Profile, as the material expects it. x is world units. w is reserved and
	 *  read by nothing; the material feeds it a constant rather than a
	 *  parameter, so it cannot look live in the details panel. */
	FLinearColor GetProfile(float PlanetRadius, float AtmosphereHeightScale) const
	{
		return FLinearColor(
			GetAtmosphereThickness(PlanetRadius, AtmosphereHeightScale),
			DeckBottom,
			VortexThreshold,
			0.0f);
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
	 *  shape alone. */
	float GetDetailVertical() const { return DetailScale * DetailAspect; }

	float GetStructureVertical() const
	{
		return DetailScale * StructureScaleRatio * StructureAspect;
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
	FLinearColor ScatterPositive = FLinearColor(0.300517f, 0.033230f, 0.303819f, 1.0f);

	/** Band boundaries, where vorticity crosses zero. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bands")
	FLinearColor ScatterBase = FLinearColor(0.076171f, 0.483255f, 0.598958f, 1.0f);

	/** Where the band ramp saturates. Matching this to the inverse of the sim
	 *  debug view's DebugScale makes the material and the debug view agree
	 *  about where band boundaries are. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bands", meta = (ClampMin = "0.0"))
	float BandScale = 1.5f;

	// -- Extinction ---------------------------------------------------------
	//
	// AUTHORED AS TOTAL OPTICAL DEPTH, NOT AS A COEFFICIENT. Atmo_BuildParams
	// divides Cloud Beta by atmosphere thickness, and the deck's heights are
	// fractions of that same thickness, so a vertical ray from the deck top to
	// the surface accumulates
	//
	//     tau = ScatterX.a * CloudBeta * (DeckTop + DeckBottom) / 2
	//
	// -- the gradient integrates to half its span because the profile is
	// symmetric across it, plus the solid region from DeckBottom down.
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
	float DeckOpticalDepth = 100.0f;

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
	float LightExtinctionFraction = 0.25f;

	// -- Derivations --------------------------------------------------------

	/** Cloud Beta, solved so a ray crossing the deck at core density
	 *  accumulates DeckOpticalDepth.
	 *
	 *  The ScatterX.a multipliers ride on top, so they stay relative: at 1.0 a
	 *  band gets exactly the authored depth, and Neg against Pos is how much
	 *  more one band family absorbs than the other. */
	FLinearColor GetCloudBeta(float DeckTop, float DeckBottom) const
	{
		const float Path = FMath::Max(0.5f * (DeckTop + DeckBottom), KINDA_SMALL_NUMBER);

		return ExtinctionTint * (DeckOpticalDepth / Path);
	}

	/** Cloud Absorption Beta. Same solve, scaled down for the light ray. */
	FLinearColor GetCloudAbsorptionBeta(float DeckTop, float DeckBottom) const
	{
		return GetCloudBeta(DeckTop, DeckBottom) * LightExtinctionFraction;
	}

};