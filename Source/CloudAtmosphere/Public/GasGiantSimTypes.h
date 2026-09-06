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

	/** Laplacian(psi) - omega. Should be featureless once converged; see the
	 *  note in MainDebugVisCS for how to read a residual that is not. */
	Residual    UMETA(DisplayName = "Poisson residual"),

	/** Zonal mean vorticity minus the prescribed target. Shows whether the
	 *  nudge is winning against the drag. */
	ZonalError  UMETA(DisplayName = "Zonal profile error"),
};

/** Per-layer multipliers on the shared jet profile.
 *
 *  THIS IS THE VERTICAL WIND SHEAR, and the reason the stack exists at all.
 *  Taylor-Proudman makes the flow invariant along the rotation axis, so columns
 *  move together and a full 3D solve is mostly wasted work. What a stack of 2D
 *  layers buys over a single layer is layers that DISAGREE.
 *
 *  Multipliers rather than independent profiles, deliberately. Independent
 *  profiles would let each layer put its jets at different latitudes, and the
 *  bands the material draws come from the shared profile, so they would then
 *  register with none of them. */
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

	/** Scales the stochastic forcing amplitude. A MULTIPLIER, so 1 is neutral. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float ForcingScale = 1.0f;

	/** Scales the eddy drag. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	float DragScale = 1.0f;
};

/** Everything the sim needs, authored.
 *
 *  A DataAsset rather than component properties because at this stage the
 *  priority is watching the field rather than shipping a planet: the asset can
 *  be left open beside the debug target and edited while the sim runs, since
 *  every value here is re-read at the top of each frame. Nothing is latched at
 *  start except the grid dimensions.
 *
 *  The two render targets are AUTHORED ASSETS rather than transient textures
 *  created at runtime. That is the whole debugging story: point the sim at a
 *  render target asset, open it in the content browser, and watch. No material
 *  instance to plumb, no dynamic parameter to push, and any material can
 *  reference the flow target directly by asset path. */
UCLASS(BlueprintType)
class CLOUDATMOSPHERE_API UGasGiantSimConfig : public UDataAsset
{
	GENERATED_BODY()

public:
	// ------------------------------------------------------------------------
	// THE DEFAULTS BELOW ARE DERIVED FROM ONE ANOTHER, NOT CHOSEN INDEPENDENTLY.
	//
	// Almost every failure during bring-up was a parameter that was individually
	// reasonable and wrong in combination, so the relations are recorded here
	// rather than left to be rediscovered. Changing the profile means rederiving
	// the rest.
	//
	// With BandCount 5, JetStrength 0.18, EquatorialBoost 0.35:
	//
	//   peak rate               JetStrength * (1 + EquatorialBoost)   = 0.224
	//   zonal vorticity scale   peak of |2 mu R - (1-mu^2) R'|        = 3.40
	//   growth rate             JetStrength * BandCount * pi          = 2.83
	//   eddy turnover           1 / growth                            = 0.354
	//
	// and then:
	//
	//   PlanetaryVorticity  >  65, the Rayleigh-Kuo requirement. 100 gives 1.5x.
	//                          Scales linearly with JetStrength and QUADRATICALLY
	//                          with BandCount, which is the easy one to miss.
	//   NudgeRate           ~  8x growth, so the prescribed profile actually holds.
	//   DragRate            ~  1x growth, arresting the cascade at the band scale.
	//   ForcingAmplitude    ~  30% of the zonal vorticity scale: visible weather
	//                          with the bands still legible.
	//   ForcingDrift        ~  one feature per turnover, so forcing is stochastic
	//                          rather than frozen.
	//   StepRatio              0.0043, which lands Courant near 1.0 at this
	//                          profile -- deliberately above the 0.33 limit,
	//                          because the numerical diffusion is a real part
	//                          of the dissipation when DragRate is only 8% of
	//                          the growth rate.
	//   ForcingScale           feature well below the Rhines wavelength of 0.297,
	//                          so the inverse cascade has room to organise it.
	// ------------------------------------------------------------------------

	// -- Grid ---------------------------------------------------------------

	/** Longitude columns. MUST BE EVEN: the polar fold in SimWrapCoord offsets
	 *  by exactly half the width, and an odd width lands the reflection half a
	 *  texel off, which shows as a faint discontinuity through both poles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "32", ClampMax = "2048"))
	int32 GridLongitude = 512;

	/** Latitude rows, in sin(latitude). Half the longitude count gives roughly
	 *  square cells in the tropics, which is where all the visible structure
	 *  is. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "16", ClampMax = "1024"))
	int32 GridLatitude = 256;

	/** Stack depth. One layer works and has no vertical shear. Three is the
	 *  first count that gives a top, a middle and a bottom. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid", meta = (ClampMin = "1", ClampMax = "8"))
	int32 LayerCount = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Grid")
	TArray<FGasGiantLayerProfile> LayerProfiles;

	// -- Jet profile --------------------------------------------------------
	//
	// These are the SAME parameters the material's band profile uses and they
	// must be kept identical between the two, because GasGiantJets.ush is
	// shared and the bands are drawn from the same shape the jets are
	// maintained at. Divergence here means bands that do not sit on their jets.

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile", meta = (ClampMin = "1.0"))
	float BandCount = 3.0f;

	/** Peak angular rate, radians per unit time on a unit sphere. Everything
	 *  in the sim is scaled against this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	float JetStrength = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	float EquatorialBoost = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	float Asymmetry = 0.5f;

	/** x Flatness, y WidthBias, z ReliefThinning (material only). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Jet Profile")
	FVector BandShape = FVector(0.0, 0.0, 0.0);

	// -- Physics ------------------------------------------------------------

	/** 2 * Omega. BETA IS NOT OPTIONAL and this is the number that supplies it.
	 *
	 *  Zero here means an isotropic inverse cascade, which merges eddies into
	 *  one hemispheric vortex rather than into jets. With NudgeRate high the
	 *  profile would still look correct while being maintained entirely by the
	 *  nudge, so this failure is invisible until the nudge comes down.
	 *
	 *  IT IS BANDCOUNT THAT SETS THE REQUIREMENT, NOT JETSTRENGTH ALONE.
	 *
	 *  The Rhines wavenumber sqrt(beta/U) is the finest banding a given
	 *  rotation rate can hold against a given wind speed. The prescribed
	 *  profile asks for BandCount*pi. Equating them:
	 *
	 *      PlanetaryVorticity >= JetStrength * (BandCount * pi)^2
	 *
	 *  which at BandCount 9 and JetStrength 0.1 is about 80. Set it below that
	 *  and the profile is asking for more bands than the rotation can support:
	 *  the inverse cascade pushes energy up to the scale beta does permit, the
	 *  jets go barotropically unstable, meander, roll up and merge into one or
	 *  two hemispheric vortices. Bands appear, hold for a while, then collapse.
	 *
	 *  The quadratic in BandCount is why this is easy to get wrong -- adding
	 *  two bands raises the requirement by half again. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics")
	float PlanetaryVorticity = 4.0f;

	/** Simulated time per second of real time. THE SPEED CONTROL, AND ONLY THAT.
	 *
	 *  Changing it changes how fast simulated time passes and nothing else. The
	 *  substep count per frame is unaffected, because the step size scales with
	 *  it -- see StepRatio. Zero freezes the sim without tearing it down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "0.0"))
	float TimeScale = 1.0f;

	/** Step size as a FRACTION OF TIMESCALE. Step = TimeScale * StepRatio.
	 *
	 *  Strictly proportional and deliberately unclamped, so substeps per frame
	 *  come to DeltaTime / StepRatio -- no TimeScale in it at all. The count is
	 *  therefore identical at every speed, which is what keeps slow motion
	 *  smooth instead of degrading into single-stepping, and it makes the cost
	 *  per frame a fixed, predictable number.
	 *
	 *  AUTHORED RATHER THAN DERIVED FROM A COURANT TARGET. Deriving it from a
	 *  Courant number reads well but inverts the dependency: the step would
	 *  then shrink whenever JetStrength or GridLongitude rose, so the substep
	 *  count and the frame cost would move whenever the profile was touched.
	 *  Authoring the ratio pins the cost and lets Courant float, which is the
	 *  right way round -- cost is a budget, Courant is a consequence.
	 *
	 *  Courant is reported at start and available from GetCourant(), and above
	 *  0.33 it stops being an accuracy figure and becomes a real dissipation
	 *  term. See the note there.
	 *
	 *  1/240 gives four substeps per frame at 60fps. Halve it for smoother
	 *  motion at double the cost. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "0.00001", ClampMax = "0.1"))
	float StepRatio = 0.0043f;

	/** Cap on substeps per frame, so a hitch does not cascade into a longer
	 *  hitch. Accumulated time beyond this is DISCARDED rather than carried,
	 *  because carrying it means a stall is followed by a burst of steps that
	 *  makes the next frame worse. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Physics", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxSubstepsPerFrame = 8;

	// -- Forcing ------------------------------------------------------------

	/** Relaxation of the ZONAL MEAN toward the prescribed profile, per unit
	 *  time. Not of the field: nudging the field erases every eddy each step.
	 *
	 *  Start high. Prescribed and emergent jets are the same code path with
	 *  different coefficients, and a high nudge holds the profile still, which
	 *  is what lets advection and the Poisson solve be validated separately
	 *  against known answers. Walk it down afterwards. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float NudgeRate = 1.0f;

	/** EQUILIBRIUM eddy vorticity sustained by the stochastic forcing.
	 *
	 *  This is what stops a well-damped run going laminar. Drag arrests the
	 *  inverse cascade, which is what keeps bands intact -- but drag with
	 *  nothing opposing it removes all the eddies too, and the result is clean
	 *  bands with no weather on them. A forced-dissipative balance is what
	 *  gives structure that persists without growing.
	 *
	 *  Expressed as an equilibrium rather than a rate, so it does not move when
	 *  DragRate is tuned. See MainForceCS.
	 *
	 *  Compare against the zonal vorticity scale, about
	 *  JetStrength * 0.69 * BandCount * pi -- 1.3 at BandCount 4 and
	 *  JetStrength 0.15. A third of that is visible weather that leaves the
	 *  bands legible; approaching it starts to break them up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float ForcingAmplitude = 2.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float ForcingScale = 0.25f;

	/** Drift through the forcing volume, in UVW per unit time. What
	 *  decorrelates the forcing so it is stochastic rather than static.
	 *
	 *  MUST BE COMPARED AGAINST THE EDDY TURNOVER TIME, not chosen small
	 *  because it is a drift. One forcing feature is 1/BasePeriod in UVW, so
	 *  the pattern refreshes every (1/BasePeriod)/|Drift| time units, and the
	 *  turnover is roughly 1/(JetStrength*BandCount*pi) -- about 0.5.
	 *
	 *  A drift of 0.03 refreshes every 17 units, thirty times slower than the
	 *  flow evolves, which is indistinguishable from frozen: the sim converges
	 *  to a fixed point with every structure pinned to a fixed longitude, and
	 *  reads as laminar no matter how strong the forcing is. Near 1.0 puts
	 *  refresh and turnover on the same timescale.
	 *
	 *  Components are mutually incommensurate so the path through the tiling
	 *  volume does not close and repeat. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	FVector ForcingDrift = FVector(0.001, 0.001, 0.0005);

	/** Linear drag on the eddy vorticity, per unit time.
	 *
	 *  THE ONLY LARGE-SCALE ENERGY SINK IN THE MODEL, and it has to be compared
	 *  against the instability growth rate, which is roughly
	 *  JetStrength * BandCount * pi. At BandCount 4 and JetStrength 0.15 that
	 *  is 1.88 per unit time, so a DragRate of 0.05 -- a 20 unit timescale --
	 *  is nearly forty times too slow to arrest anything.
	 *
	 *  The nudge cannot substitute for it. The nudge controls the ZONAL MEAN,
	 *  and a field that is mostly a single large eddy can carry a perfectly
	 *  correct zonal mean while looking nothing like bands. Eddy energy needs
	 *  its own sink.
	 *
	 *  Same order as the growth rate is the right starting point. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float DragRate = 1.5f;

	/** Relaxation between vertically adjacent layers. Weak on purpose: strong
	 *  coupling is the Taylor-Proudman limit, in which the stack collapses to
	 *  one layer and buys nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing")
	float LayerCoupling = 0.1f;

	// -- Polar filter -------------------------------------------------------

	/** cos(latitude) below which the longitudinal filter engages. 0.35 is
	 *  about 70 degrees. Above this nothing is filtered at all. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Polar Filter", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FilterLatitude = 0.9f;

	/** Bound on the filter width, so the innermost polar rows do not turn into
	 *  a loop over the whole grid. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Polar Filter", meta = (ClampMin = "1", ClampMax = "256"))
	int32 FilterMaxHalfWidth = 1;


	// -- Solver -------------------------------------------------------------

	/** Red-black sweeps per substep. Each is two dispatches.
	 *
	 *  Small because the solve is WARM STARTED from the previous step's psi,
	 *  which is a near-solution. If the residual view shows structure that is
	 *  spread evenly rather than concentrated at the poles, this is the number
	 *  to raise. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solver", meta = (ClampMin = "1", ClampMax = "128"))
	int32 PoissonIterations = 8;

	/** Sweeps for the one cold start at init, which has no previous psi to warm
	 *  start from. Off the frame budget, so it can afford to be generous.
	 *
	 *  ALSO AUTHORING-ONLY ONCE INITIALSTATE IS BOUND. A restored snapshot
	 *  carries its own psi, already consistent with the vorticity it was
	 *  captured beside, so no solve runs at all -- which is most of the reason
	 *  both fields are stored rather than just vorticity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solver", meta = (ClampMin = "1", ClampMax = "4096"))
	int32 InitPoissonIterations = 256;

	/** Over-relaxation. LEAVE AT 0 TO DERIVE IT FROM THE GRID.
	 *
	 *  The optimum is not a constant, it is a function of resolution, and it
	 *  approaches 2 as the grid grows:
	 *
	 *      w_opt = 2 / (1 + sqrt(1 - rho_jacobi^2))
	 *
	 *  which at 512x256 is about 1.981. A hand-picked 1.8 -- a reasonable rule
	 *  of thumb for a small grid -- is catastrophically off here, and the
	 *  sensitivity is not intuitive: the smoothest mode's error decays 0.7% per
	 *  substep at 1.8 against 14% at 1.981, so it accumulates to roughly 148x
	 *  the per-step injection instead of 9x.
	 *
	 *  That accumulated error is a large-scale streamfunction error, which is a
	 *  large-scale spurious VELOCITY, which advects everything into the lowest
	 *  wavenumber available. It presents as the field collapsing to a single
	 *  hemispheric mode -- indistinguishable, by eye, from a physical inverse
	 *  cascade that failed to arrest.
	 *
	 *  Derived rather than defaulted so it cannot go stale when the grid
	 *  changes. Set a positive value only to override deliberately.
	 *
	 *  AT OR ABOVE 2 THE ITERATION DIVERGES, immediately rather than gradually,
	 *  so a psi view that saturates on the first frame is almost always this. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Solver", meta = (ClampMin = "0.0", ClampMax = "1.99"))
	float Relaxation = 0.0f;

	// -- Forcing volume -----------------------------------------------------

	/** Band-limited tiling noise supplying the stochastic forcing.
	 *
	 *  ONE JOB. It used to seed the initial eddy streamfunction as well, and the
	 *  two wanted opposite things: a seed is differentiated twice, so a
	 *  Laplacian amplifies it by k^2 and it wants coarse and smooth, while
	 *  forcing goes straight into vorticity and wants fine. The seed half is
	 *  gone -- held sub-marginal the jets grow their own eddies from round-off
	 *  within a few tens of turnovers, so a seed only shortened spin-up, and a
	 *  baked start state removes spin-up altogether.
	 *
	 *  Optional. With none bound the forcing evaluates to exactly zero, which is
	 *  the correct state when the nudge is the energy source -- as it is at the
	 *  defaults below. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing Volume")
	TObjectPtr<UVolumeTexture> ForcingVolume;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing Volume", meta = (ClampMin = "0", ClampMax = "3"))
	int32 ForcingChannel = 1;

	/** True when the channel was baked with bBipolarOutput. MUST MATCH THE
	 *  RECIPE: a unipolar decode on a signed bake maps [-1,1] to [-3,1], which
	 *  as forcing is a constant vorticity source with noise riding on it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Forcing Volume")
	bool bForcingBipolar = true;

	// -- Start state --------------------------------------------------------

	/** A captured state to start from. Empty means seed and spin up.
	 *
	 *  When set and matching the grid, the whole seeding path is skipped: the
	 *  vorticity and streamfunction are uploaded directly and the sim is
	 *  running from the first frame. SpinUpSteps is ignored, because there is
	 *  nothing to spin up -- and InitPoissonIterations too, since psi arrives
	 *  already consistent with its vorticity.
	 *
	 *  This is what makes one sim serve many planets. Bake a library of states,
	 *  pick one at random, and a procedurally generated gas giant starts fully
	 *  developed with no spin-up cost and no two alike.
	 *
	 *  A grid mismatch is REFUSED and falls back to seeding, with a warning.
	 *  There is no resampling of a vorticity field that is cheaper or more
	 *  faithful than re-running the spin-up. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Start State")
	TObjectPtr<UGasGiantSnapshot> InitialState;

	// -- Spin-up ------------------------------------------------------------

	/** Substeps to run before the sim is considered ready.
	 *
	 *  AN AUTHORING PARAMETER NOW, NOT A RUNTIME ONE. It is what produces the
	 *  states that get captured into snapshots; once InitialState is bound it
	 *  is skipped entirely, because a restored state is already spun up.
	 *
	 *  Small even so, because the expensive part of a real spin-up is skipped:
	 *  what takes thousands of turnovers is the cascade organising jets out of
	 *  isotropic forcing, and the jets here are prescribed. What is left is
	 *  eddies equilibrating with jets that already exist, which is tens of
	 *  turnovers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Spin Up", meta = (ClampMin = "0", ClampMax = "8192"))
	int32 SpinUpSteps = 300;

	/** Spin-up substeps per frame.
	 *
	 *  Spread over frames rather than run in one graph, for two reasons. A
	 *  three-hundred-step graph is three thousand passes and will hitch. And
	 *  spreading it means the spin-up is WATCHABLE in the debug view, which is
	 *  where most of the diagnostic value is: seeing the seed organise, or fail
	 *  to, says far more than the converged state does. */
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

	/** Reconfigure the targets to match the grid if they do not already.
	 *
	 *  On by default because the alternative during bring-up is a silent
	 *  no-op: a mismatched target is refused, and a refused sim looks exactly
	 *  like a sim that is running and producing nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Targets")
	bool bAutoResizeTargets = true;

	// -- Debug --------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	EGasGiantDebugMode DebugMode = EGasGiantDebugMode::Vorticity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "0", ClampMax = "7"))
	int32 DebugLayer = 0;

	/** Value mapped to full colour. Everything shown is signed and O(1) against
	 *  PlanetaryVorticity, so start near that. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", meta = (ClampMin = "0.0001"))
	float DebugScale = 0.0f;

	/** Halt stepping without tearing the state down. The debug view keeps
	 *  updating, so a frozen field can still be inspected in every mode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	bool bPaused = false;
};

/** Flat, POD-ish snapshot handed to the render thread.
 *
 *  Deliberately plain, for exactly the reason FNoiseBakeDispatchParams is: it
 *  gets captured by value into a render command, and touching a UObject from
 *  the render thread is a crash waiting for a garbage collection to schedule
 *  itself badly. Everything the render thread needs is copied here on the game
 *  thread, including the seed volume's RHI reference. */
struct FGasGiantSimParams
{
	FIntVector GridSize = FIntVector(512, 256, 3);

	FVector4f JetParams = FVector4f(3.0f, 2.0f, 0.5f, 0.5f);
	FVector4f BandShape = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
	FVector4f LayerProfile[8];

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

	/** Held as RHI references so the render thread never dereferences a
	 *  UObject. Null seed is legal and evaluates as zero, which is a valid if
	 *  uninteresting state and is better than refusing to run. */
	FTextureRHIRef ForcingTexture;
	FTextureRHIRef FlowTexture;
	FTextureRHIRef DebugTexture;
};