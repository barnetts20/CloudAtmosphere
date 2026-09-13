#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GasGiantSimTypes.generated.h"

class UVolumeTexture;
class UGasGiantSnapshot;
class UTextureRenderTarget2D;
class UTextureRenderTarget2DArray;

/** Which field the debug view renders. Mirrors GG_DEBUG_* in GasGiantSim.usf. */
UENUM(BlueprintType)
enum class EGasGiantDebugMode : uint8
{
	Vorticity   UMETA(DisplayName = "Vorticity"),
	Psi         UMETA(DisplayName = "Streamfunction"),
	Speed       UMETA(DisplayName = "Speed"),
	East        UMETA(DisplayName = "Eastward velocity"),
	North       UMETA(DisplayName = "Northward velocity"),

	/** Laplacian(psi) - omega, featureless once converged. MainDebugVisCS has the
	 *  note on reading one that is not. */
	Residual    UMETA(DisplayName = "Poisson residual"),

	/** Zonal mean vorticity minus the prescribed target: whether the nudge is
	 *  winning against the drag. */
	ZonalError  UMETA(DisplayName = "Zonal profile error"),
};

/** Per-layer multipliers on the shared jet profile. THIS IS THE VERTICAL WIND
 *  SHEAR and the reason the stack exists: Taylor-Proudman makes the flow
 *  invariant along the rotation axis, so a full 3D solve is mostly wasted work
 *  and what a stack of 2D layers buys is layers that DISAGREE.
 *
 *  Multipliers rather than independent profiles: independent ones would put each
 *  layer's jets at different latitudes, and the bands the material draws come
 *  from the shared profile, so they would register with none of them. */
USTRUCT(BlueprintType)
struct FGasGiantLayerProfile
{
	GENERATED_BODY()

	/** Scales JetStrength. Below 1 gives a slower deep layer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float JetScale = 1.0f;

	/** Scales EquatorialBoost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float BoostScale = 1.0f;

	/** Scales the stochastic forcing amplitude. A MULTIPLIER: 1 is neutral. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float ForcingScale = 1.0f;

	/** Scales the eddy drag. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float DragScale = 1.0f;
};

/** Everything the sim needs, authored. Every value is re-read at the top of each
 *  frame, so the asset can be edited while the sim runs; nothing is latched at
 *  start except the grid dimensions.
 *
 *  The two render targets are AUTHORED ASSETS rather than transient textures, so
 *  a material can reference the flow target by asset path and the debug target
 *  can be watched in the content browser while the sim runs. */
UCLASS(BlueprintType)
class CLOUDATMOSPHERE_API UGasGiantSimConfig : public UDataAsset
{
	GENERATED_BODY()

public:
	// THE PARAMETERS INTERACT. The jet profile sets a growth rate that NudgeRate,
	// DragRate and ForcingAmplitude are all scaled against, and the rotation rate
	// bounds how fine a banding that profile can hold. Retuning one of the four
	// usually means revisiting the others.

	// -- Grid ---------------------------------------------------------------

	/** Longitude columns. MUST BE EVEN: the polar fold in SimWrapCoord offsets by
	 *  exactly half the width, and an odd width lands the reflection half a texel
	 *  off, showing as a faint discontinuity through both poles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "32", ClampMax = "2048"))
	int32 GridLongitude = 512;

	/** Latitude rows, in sin(latitude). Half the longitude count gives roughly
	 *  square cells in the tropics, where the visible structure is. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "16", ClampMax = "1024"))
	int32 GridLatitude = 256;

	/** Stack depth. One layer works and has no vertical shear; two is the
	 *  smallest that produces any, which is what the deck reads. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "1", ClampMax = "8"))
	int32 LayerCount = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	TArray<FGasGiantLayerProfile> LayerProfiles;

	// -- Jet profile --------------------------------------------------------
	//
	// The SAME parameters the material's band profile uses, and they must match:
	// GasGiantJets.ush is shared, and the bands are drawn from the same shape the
	// jets are maintained at. Divergence means bands that do not sit on their
	// jets.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile", meta = (ClampMin = "1.0"))
	float BandCount = 3.0f;

	/** Peak angular rate, radians per unit time on a unit sphere. Everything in
	 *  the sim is scaled against this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	float JetStrength = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	float EquatorialBoost = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	float Asymmetry = 0.5f;

	/** Offsets the jet profile so zones and belts need not be the same width.
	 *  Positive widens the prograde zones. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	float WidthBias = 0.0f;

	// -- Physics ------------------------------------------------------------

	/** 2 * Omega. BETA IS NOT OPTIONAL and this supplies it: at zero the inverse
	 *  cascade is isotropic and merges eddies into one hemispheric vortex rather
	 *  than into jets. With NudgeRate high the profile still looks correct while
	 *  the nudge maintains it alone, so the failure hides until the nudge drops.
	 *
	 *  PITFALL: the requirement rises with the SQUARE of BandCount, not with
	 *  JetStrength alone. Too low and the jets go barotropically unstable,
	 *  meander, roll up and merge -- bands appear, hold, then collapse. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics")
	float PlanetaryVorticity = 4.0f;

	/** Simulated time per second of real time. THE SPEED CONTROL, AND ONLY THAT:
	 *  the substep count per frame is unaffected, since the step size scales with
	 *  it. Zero freezes the sim without tearing it down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "0.0"))
	float TimeScale = 1.0f;

	/** Step size as a FRACTION OF TIMESCALE: Step = TimeScale * StepRatio. Substeps
	 *  per frame come to DeltaTime / StepRatio with no TimeScale in it, so the
	 *  count is identical at every speed and the per-frame cost is pinned.
	 *
	 *  AUTHORED RATHER THAN DERIVED FROM A COURANT TARGET, which would invert the
	 *  dependency and move the frame cost whenever the profile was touched. Cost
	 *  is a budget, Courant a consequence: GetCourant() reports it, and above 0.33
	 *  it becomes a real dissipation term rather than an accuracy figure. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "0.00001", ClampMax = "0.1"))
	float StepRatio = 0.0043f;

	/** Cap on substeps per frame, so a hitch does not cascade into a longer one.
	 *  Time beyond this is DISCARDED rather than carried: carrying it means a
	 *  stall is followed by a burst of steps that makes the next frame worse. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxSubstepsPerFrame = 8;

	// -- Forcing ------------------------------------------------------------

	/** Relaxation of the ZONAL MEAN toward the prescribed profile, per unit time.
	 *  Not of the field: nudging the field erases every eddy each step. High holds
	 *  the profile still, which is what lets advection and the Poisson solve be
	 *  checked separately against known answers; walk it down afterwards. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float NudgeRate = 1.0f;

	/** EQUILIBRIUM eddy vorticity sustained by the stochastic forcing, and what
	 *  stops a well-damped run going laminar. Drag arrests the inverse cascade,
	 *  which keeps bands intact, but drag with nothing opposing it removes the
	 *  eddies too and leaves clean bands with no weather on them.
	 *
	 *  An equilibrium rather than a rate, so it does not move when DragRate is
	 *  tuned. Raise it toward the zonal vorticity scale and the bands start to
	 *  break up. See MainForceCS. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float ForcingAmplitude = 2.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float ForcingScale = 0.25f;

	/** Drift through the forcing volume, in UVW per unit time: what decorrelates
	 *  the forcing so it is stochastic rather than static.
	 *
	 *  MUST BE COMPARED AGAINST THE EDDY TURNOVER TIME, not chosen small because
	 *  it is a drift. Refresh much slower than the flow evolves is
	 *  indistinguishable from frozen -- the sim converges to a fixed point with
	 *  every structure pinned to a longitude, and reads as laminar however strong
	 *  the forcing is. Components are mutually incommensurate so the path through
	 *  the tiling volume does not close and repeat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	FVector ForcingDrift = FVector(0.001, 0.001, 0.0005);

	/** Linear drag on the eddy vorticity, per unit time. THE ONLY LARGE-SCALE
	 *  ENERGY SINK IN THE MODEL, and it has to be the same order as the
	 *  instability growth rate to arrest anything.
	 *
	 *  The nudge cannot substitute for it: the nudge controls the ZONAL MEAN, and
	 *  a field that is mostly one large eddy can carry a perfectly correct zonal
	 *  mean while looking nothing like bands. Eddy energy needs its own sink. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float DragRate = 1.5f;

	/** Relaxation between vertically adjacent layers. Weak on purpose: strong
	 *  coupling is the Taylor-Proudman limit, where the stack collapses to one
	 *  layer and buys nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float LayerCoupling = 0.1f;

	// -- Polar filter -------------------------------------------------------

	/** cos(latitude) below which the longitudinal filter engages, so higher
	 *  filters more of the grid: 0.9 reaches to about 26 degrees of latitude,
	 *  0.35 only to 70. Above it nothing is filtered. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Polar Filter", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FilterLatitude = 0.9f;

	/** Bound on the filter width, so the innermost polar rows do not turn into
	 *  a loop over the whole grid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Polar Filter", meta = (ClampMin = "1", ClampMax = "256"))
	int32 FilterMaxHalfWidth = 1;


	// -- Solver -------------------------------------------------------------

	/** Red-black sweeps per substep, each two dispatches. Small because the solve
	 *  is WARM STARTED from the previous step's psi, a near-solution. If the
	 *  residual view shows structure spread evenly rather than concentrated at the
	 *  poles, this is the number to raise. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solver", meta = (ClampMin = "1", ClampMax = "128"))
	int32 PoissonIterations = 8;

	/** Sweeps for the one cold start at init, which has no previous psi to warm
	 *  start from. Off the frame budget, so it can be generous. Ignored once
	 *  InitialState is bound: a restored snapshot carries psi already consistent
	 *  with its vorticity, which is most of why both fields are stored. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solver", meta = (ClampMin = "1", ClampMax = "4096"))
	int32 InitPoissonIterations = 256;

	/** Over-relaxation. LEAVE AT 0 TO DERIVE IT FROM THE GRID, since the optimum
	 *  is a function of resolution rather than a constant and approaches 2 as the
	 *  grid grows. Set a positive value only to override deliberately.
	 *
	 *  PITFALL: the sensitivity is not intuitive. A hand-picked value that is a
	 *  fine rule of thumb for a small grid leaves the smoothest mode's error
	 *  decaying an order of magnitude slower here, and the accumulated
	 *  streamfunction error is a large-scale spurious VELOCITY that advects
	 *  everything into the lowest wavenumber available. It presents as the field
	 *  collapsing to a single hemispheric mode, which by eye is indistinguishable
	 *  from an inverse cascade that failed to arrest. AT OR ABOVE 2 THE ITERATION
	 *  DIVERGES immediately, so a psi view that saturates on frame one is almost
	 *  always this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solver", meta = (ClampMin = "0.0", ClampMax = "1.99"))
	float Relaxation = 0.0f;

	// -- Forcing volume -----------------------------------------------------

	/** Band-limited tiling noise supplying the stochastic forcing. Optional: with
	 *  none bound the forcing evaluates to exactly zero, which is the correct
	 *  state when the nudge is the energy source. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing Volume")
	TObjectPtr<UVolumeTexture> ForcingVolume;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing Volume", meta = (ClampMin = "0", ClampMax = "3"))
	int32 ForcingChannel = 1;

	/** True when the channel was baked with bBipolarOutput. MUST MATCH THE RECIPE:
	 *  a unipolar decode on a signed bake maps [-1,1] to [-3,1], which as forcing
	 *  is a constant vorticity source with noise riding on it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing Volume")
	bool bForcingBipolar = true;

	// -- Start state --------------------------------------------------------

	/** A captured state to start from. Empty means seed and spin up.
	 *
	 *  Set and matching the grid, the seeding path is skipped entirely: vorticity
	 *  and streamfunction upload directly and the sim runs from the first frame,
	 *  with SpinUpSteps and InitPoissonIterations both ignored. This is what makes
	 *  one sim serve many planets -- bake a library of states, pick one at random,
	 *  and a generated planet starts fully developed with no two alike.
	 *
	 *  A grid mismatch is REFUSED and falls back to seeding, with a warning. No
	 *  resampling of a vorticity field is cheaper or more faithful than re-running
	 *  the spin-up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start State")
	TObjectPtr<UGasGiantSnapshot> InitialState;

	// -- Spin-up ------------------------------------------------------------

	/** Substeps to run before the sim is considered ready. AN AUTHORING PARAMETER,
	 *  NOT A RUNTIME ONE: it produces the states that get captured into snapshots,
	 *  and is skipped once InitialState is bound. Small because the expensive part
	 *  of a real spin-up -- the cascade organising jets out of isotropic forcing --
	 *  does not happen here, the jets being prescribed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spin Up", meta = (ClampMin = "0", ClampMax = "8192"))
	int32 SpinUpSteps = 300;

	/** Spin-up substeps per frame. Spread over frames rather than run in one
	 *  graph: a three-hundred-step graph is three thousand passes and will hitch,
	 *  and spreading it makes the spin-up WATCHABLE in the debug view, where
	 *  seeing the seed organise or fail to says more than the converged state. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spin Up", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxSpinUpStepsPerFrame = 8;

	// -- Targets ------------------------------------------------------------

	/** RGBA16F 2D array, sized (GridLongitude, GridLatitude, LayerCount).
	 *  RGB is tangent velocity as an angular rate, A is the streamfunction.
	 *  This is what the material samples. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Targets")
	TObjectPtr<UTextureRenderTarget2DArray> FlowTarget;

	/** Any 2D render target. Sized to the grid it is one texel per cell, which
	 *  is the intended setup. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Targets")
	TObjectPtr<UTextureRenderTarget2D> DebugTarget;

	/** Reconfigure the targets to match the grid if they do not already. On by
	 *  default: a mismatched target is refused, and a refused sim looks exactly
	 *  like a sim that is running and producing nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Targets")
	bool bAutoResizeTargets = true;

	// -- Debug --------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	EGasGiantDebugMode DebugMode = EGasGiantDebugMode::Vorticity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "0", ClampMax = "7"))
	int32 DebugLayer = 0;

	/** Value mapped to full colour. ZERO DERIVES IT PER MODE, which is usually
	 *  what is wanted: the seven fields differ in magnitude by two orders, so one
	 *  authored number is right for one of them and renders the rest black. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "0.0001"))
	float DebugScale = 0.0f;

	/** Halt stepping without tearing the state down. The debug view keeps
	 *  updating, so a frozen field can still be inspected in every mode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bPaused = false;
};

/** Flat, POD-ish snapshot handed to the render thread. Captured BY VALUE into a
 *  render command, so it holds no UObject: touching one from the render thread
 *  is a crash waiting for a garbage collection to schedule itself badly.
 *  Everything needed is copied here on the game thread, RHI references
 *  included. */
struct FGasGiantSimParams
{
	FIntVector GridSize = FIntVector(512, 256, 3);

	FVector4f JetParams = FVector4f(3.0f, 2.0f, 0.5f, 0.5f);
	float WidthBias = 0.0f;
	FVector4f LayerProfile[8] = {
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(),
		FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero() };

	float DeltaTime = 0.0043f;
	float Time = 0.0f;
	float PlanetaryVorticity = 4.0f;

	int32 ForcingChannel = 1;
	bool bForcingBipolar = true;

	float NudgeRate = 1.0f;
	float ForcingAmplitude = 2.5f;
	float ForcingScale = 0.25f;
	FVector3f ForcingDrift = FVector3f(0.001f, 0.001f, 0.0005f);
	float DragRate = 1.5f;
	float LayerCoupling = 0.1f;

	float FilterLatitude = 0.9f;
	int32 FilterMaxHalfWidth = 1;

	int32 PoissonIterations = 8;
	int32 InitPoissonIterations = 256;
	float Relaxation = 1.98f;

	int32 DebugMode = 0;
	int32 DebugLayer = 0;
	float DebugScale = 0.0f;
	FIntPoint DebugSize = FIntPoint::ZeroValue;

	/** RHI references, so the render thread never dereferences a UObject. A null
	 *  forcing texture is legal and evaluates as zero, which is a valid if
	 *  uninteresting state and better than refusing to run. */
	FTextureRHIRef ForcingTexture;
	FTextureRHIRef FlowTexture;
	FTextureRHIRef DebugTexture;
};