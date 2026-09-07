// AtmosphereParams.h — the parameter sets the three-stage atmosphere post
// process is driven by, and the derivations that keep them consistent.
//
// SPLIT BY WHO READS THEM, NOT BY WHAT THEY MEAN. Shared holds what both march
// materials declare AND tune the same way: the air, the geometry, the light,
// the composite. Anything a terrestrial cloud and a gas giant deck would want
// at substantially different values is duplicated into the per-model struct so
// each carries its own defaults.
//
// Cloud Beta is the clearest case. Terrestrial wants ~150, absolute extinction
// against a cloud with holes. The gas giant deck is 100% coverage and the
// per-band alphas multiply this, so 150 goes opaque in one step and the useful
// value is near 1. One shared field cannot default to both.
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

/** The air, the geometry, the light and the composite. Both march materials
 *  declare every one of these and read them the same way. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereSharedParams
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

	// -- Light --------------------------------------------------------------

	/** RGB direction is the hue, RGB magnitude is the intensity. The march and
	 *  the directional light both derive from this, so they cannot disagree
	 *  about the star. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light")
	FLinearColor LightColor = FLinearColor(1.0f, 0.95f, 0.9f, 10.0f);

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

	// -- Raymarching --------------------------------------------------------
	//
	// Cloud Steps and Cloud Light Steps are NOT here. Terrestrial spends them
	// across the cloud band; the gas giant spends them across the deck segment
	// the cone trace split off, which is a different length and a different
	// budget.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Raymarching", meta = (ClampMin = "1.0"))
	float AtmosphereSteps = 32.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Raymarching", meta = (ClampMin = "1.0"))
	float AtmosphereLightSteps = 16.0f;

	/** Step growth with distance from the ray start. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Raymarching", meta = (ClampMin = "0.0"))
	float StepScaleFactor = 2.0f;

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

	/** Atmosphere outer radius in world units. */
	float GetAtmosphereRadius(float PlanetRadius) const
	{
		return PlanetRadius * (1.0f + AtmosphereHeightScale);
	}
};

/** The terrestrial cloud band: its shell, its noise, and its own lighting.
 *
 *  The lighting fields duplicate names that also exist on the gas giant struct.
 *  That is the point -- these are the values tuned for a cloud with holes, and
 *  they do not transfer to a solid deck. */
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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lighting")
	FLinearColor CloudAmbient = FLinearColor(0.04f, 0.04f, 0.05f, 0.0f);

	/** (forward g, backward g, lobe blend, isotropic multiple-scatter weight) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lighting")
	FLinearColor CloudPhaseParams = FLinearColor(0.9f, 0.1f, 0.5f, 0.5f);

	// -- Raymarching --------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Raymarching", meta = (ClampMin = "1.0"))
	float CloudSteps = 32.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Raymarching", meta = (ClampMin = "1.0"))
	float CloudLightSteps = 32.0f;

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
 *  Relief is (TopBase, BandRelief, PressureLift, StormTowers) in shell
 *  fractions, and GetTopMax below is the closed-form highest the deck can
 *  reach. Every derivation that has to stay inside the atmosphere goes through
 *  it. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FGasGiantDeckParams
{
	GENERATED_BODY()

	// -- Shell --------------------------------------------------------------

	/** Where the deck's highest possible point sits, as a fraction of the
	 *  atmosphere height. 1.0 puts it exactly at the atmosphere top.
	 *
	 *  A FRACTION AND NOT A THICKNESS, because the bound is on the deck's TOP,
	 *  not on its thickness, and the two differ by GetTopMax -- which changes
	 *  every time Relief is retuned. Authored as a thickness, raising the band
	 *  relief silently pushes the cull radius above the atmosphere shell, and
	 *  Atmo_Plan clips there first: the deck tops get sliced off exactly where
	 *  the features are tallest, which reads as a field bug. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shell", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float DeckTopFraction = 0.9f;

	/** Inverse depth: larger reaches core density in less depth. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shell", meta = (ClampMin = "0.0001"))
	float DensityRamp = 10.0f;

	/** Onset sharpness. 1 has zero derivative at the deck top and no crease; 2
	 *  is a plain exponential whose finite slope meeting the zero outside is a
	 *  C1 kink along the whole cloud top; above that it hardens further. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shell", meta = (ClampMin = "0.05"))
	float DensityCurve = 1.0f;

	/** Density the deck reaches. A SHAPE control, not an opacity one --
	 *  extinction comes from Cloud Beta and the per-band alphas. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Shell", meta = (ClampMin = "0.0"))
	float CoreDensity = 1.0f;

	// -- Relief -------------------------------------------------------------

	/** (TopBase, BandRelief, PressureLift, StormTowers), shell fractions.
	 *  BandRelief wants to be large: the point of driving height from the flow
	 *  is that bands are geometry rather than a pattern painted on a sphere. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief")
	FLinearColor Relief = FLinearColor(0.5f, 0.6f, 0.15f, 0.1f);

	/** How far the flow's strain collapses band relief toward flat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief", meta = (ClampMin = "0.0"))
	float ReliefThinning = 0.5f;

	/** How deep the detail carves the deck top, as a shell fraction.
	 *
	 *  NOT A RATIO OF Relief.y, deliberately. Summed before the relief multiply,
	 *  raising the band relief scales the fine noise by the same factor, and
	 *  noise stretched vertically but not horizontally becomes spikes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief", meta = (ClampMin = "0.0"))
	float DetailRelief = 0.12f;

	/** Vortex strength above which storm towers are allowed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Relief", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float VortexThreshold = 0.3f;

	// -- Detail -------------------------------------------------------------

	/** Horizontal feature size of the detail volume. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.01"))
	float DetailScale = 6.0f;

	/** Packed layer scale, as a multiple of DetailScale. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.01"))
	float PackedScaleRatio = 0.667f;

	/** Vertical feature size against the horizontal one. Their ratio IS the
	 *  aspect of the resulting structure, so the ratio is the real control and
	 *  the absolute is derived. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.01"))
	float DetailAspect = 0.5f;

	/** How much of the coarse warp the detail layer inherits. A displacement
	 *  field with a large gradient IS strain, so inheriting one whole imposes
	 *  an order-one strain on the fine layer regardless of its own flow. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float DetailWarpInherit = 0.8f;

	/** How much the packed layer inherits. 0 is legitimate: this layer breaks
	 *  the flow into rounded shapes, and a shape dragged through the flow field
	 *  is no longer round. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PackedWarpInherit = 0.1f;

	/** (Ridge, Fluff, Wisp, EdgeBias). xyz are renormalized by their sum in the
	 *  shader, so changing the balance does not change how much cloud there is.
	 *  w is how much detail survives in the flat band interiors. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail")
	FLinearColor DetailWeights = FLinearColor(0.4f, 0.8f, 0.3f, 0.6f);

	/** How much the detail erodes the density. Below 1: there is no lower cloud
	 *  shell, so a fully transparent column would let a ray run to the far side
	 *  of the planet, and the erosion floor is what makes that impossible. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float DetailErosion = 0.7f;

	/** Depth over which erosion falls off, as a multiple of the ramp depth
	 *  (1 / DensityRamp). A ratio so that sharpening the density onset pulls
	 *  the erosion band in with it, instead of stranding it below the deck. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Detail", meta = (ClampMin = "0.01"))
	float DetailDepthRatio = 1.0f;

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
	float BandBias = 0.0f;

	/** Turbulence floor in band interiors. Real zone interiors are calmer than
	 *  their edges but not glass. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flow", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float TurbulenceFloor = 0.3f;

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

	/** Bound on the deck's slope, shell fractions per radian. The cone angle
	 *  for the entry search: under-declaring it is the one way the search steps
	 *  over the surface, so raise it first if tangent-angle slicing appears. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Flow", meta = (ClampMin = "0.1"))
	float DeckSlope = 4.0f;

	// -- Sources ------------------------------------------------------------

	/** Four independent scalars, BGRA8. SAMPLER: wrap on all three axes, and
	 *  Linear Color -- these are not colours, and an sRGB decode curves every
	 *  one of them without erroring. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sources")
	TObjectPtr<UVolumeTexture> PackedDetail = nullptr;

	/** Owns the flow render target the material samples and the settings the
	 *  sim subsystem steps against. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sources")
	TObjectPtr<UGasGiantSimConfig> SimConfig = nullptr;

	/** Start the sim on BeginPlay. Off when another actor already drives it:
	 *  the subsystem is per-world, so two planets starting it fight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sources")
	bool bStartSimulationOnBeginPlay = true;

	// -- Raymarching --------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Raymarching", meta = (ClampMin = "1.0"))
	float CloudSteps = 64.0f;

	/** Lower than the terrestrial default on purpose. A gas giant has no holes,
	 *  so nearly every light sample accumulates and takes the second fetch,
	 *  where the terrestrial version leans on Cloud_Density returning zero over
	 *  most of the domain. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Raymarching", meta = (ClampMin = "1.0"))
	float CloudLightSteps = 8.0f;

	// -- Derivations --------------------------------------------------------

	/** The highest the deck can reach, in shell fractions. Closed form, and it
	 *  must match GG_TopBounds in GasGiantFlow.ush -- every term in GG_CloudTop
	 *  appears, each at its most generous.
	 *
	 *  Elevation is in [0,1] so the band contributes half of Relief.y either
	 *  side of the base. Pressure is soft-saturated to [-1,1] sim-side so it
	 *  contributes the whole of Relief.z. The detail carve is subtractive and
	 *  belongs to the lower bound only. */
	float GetTopMax() const
	{
		return Relief.R
			+ 0.5f * FMath::Abs(Relief.G)
			+ FMath::Abs(Relief.B)
			+ FMath::Max(Relief.A, 0.0f);
	}

	/** Shell thickness in world units: the one absolute length in the field.
	 *
	 *  Solved so the deck's highest possible point lands at DeckTopFraction of
	 *  the atmosphere height. Everything else in the deck is a shell fraction
	 *  and rides along. */
	float GetShellThickness(float PlanetRadius, float AtmosphereHeightScale) const
	{
		const float TopMax = FMath::Max(GetTopMax(), KINDA_SMALL_NUMBER);
		return PlanetRadius * AtmosphereHeightScale * DeckTopFraction / TopMax;
	}

	/** Profile, as the material expects it. x is world units. */
	FLinearColor GetProfile(float PlanetRadius, float AtmosphereHeightScale) const
	{
		return FLinearColor(
			GetShellThickness(PlanetRadius, AtmosphereHeightScale),
			DensityRamp,
			VortexThreshold,
			CoreDensity);
	}

	FLinearColor GetScales() const
	{
		return FLinearColor(
			DetailScale,
			DetailScale * PackedScaleRatio,
			DetailWarpInherit,
			PackedWarpInherit);
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

	float GetDetailVertical() const { return DetailScale * DetailAspect; }

	float GetDetailDepth() const
	{
		return DetailDepthRatio / FMath::Max(DensityRamp, KINDA_SMALL_NUMBER);
	}
};

/** Per-band scattering for the deck, plus the shared cloud lighting the gas
 *  giant path tunes differently from the terrestrial one.
 *
 *  Alpha on each set MULTIPLIES CloudBeta rather than replacing it, so the deck
 *  stays on the same thickness-relative footing as the air: an absolute
 *  coefficient would change the deck's opacity on a resize while leaving the
 *  atmosphere's alone. RGB is single-scattering albedo, and it has no
 *  terrestrial equivalent.
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
	FLinearColor ScatterNegative = FLinearColor(0.9f, 0.9f, 0.9f, 1.0f);

	/** Anticyclonic bands. "Darker bands eat more light" is this alpha against
	 *  ScatterNegative's. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bands")
	FLinearColor ScatterPositive = FLinearColor(0.9f, 0.9f, 0.9f, 1.0f);

	/** Band boundaries, where vorticity crosses zero. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bands")
	FLinearColor ScatterBase = FLinearColor(0.9f, 0.9f, 0.9f, 1.0f);

	/** Where the band ramp saturates. Matching this to the inverse of the sim
	 *  debug view's DebugScale makes the material and the debug view agree
	 *  about where band boundaries are. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bands", meta = (ClampMin = "0.0"))
	float BandScale = 1.0f;

	// -- Lighting -----------------------------------------------------------
	//
	// Duplicated from the terrestrial struct because the values do not
	// transfer. 150 against a deck at 100% coverage is opaque in one step.

	/** Base extinction. The per-band alphas multiply this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lighting")
	FLinearColor CloudBeta = FLinearColor(1.0f, 1.0f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lighting")
	FLinearColor CloudAbsorptionBeta = FLinearColor(0.5f, 0.5f, 0.5f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lighting")
	FLinearColor CloudAmbient = FLinearColor(0.04f, 0.04f, 0.05f, 0.0f);

	/** (forward g, backward g, lobe blend, isotropic multiple-scatter weight).
	 *  The isotropic term is what keeps the shadow side off black -- single
	 *  scattering has nothing to deliver there. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lighting")
	FLinearColor CloudPhaseParams = FLinearColor(0.9f, 0.1f, 0.5f, 0.5f);
};