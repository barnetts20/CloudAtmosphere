#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "GasGiantShadowMap.h"
#include "GasGiantSimTypes.h"
#include "GasGiantSimSubsystem.generated.h"

class FGasGiantSimulation;
class UGasGiantSnapshot;

/** Game-thread driver for the flow sim.
 *
 *  WHY A WORLD SUBSYSTEM AND NOT A SCENE VIEW EXTENSION.
 *
 *  A view extension runs inside the render pipeline, once per view. A planet's
 *  weather is world state, not view state: one planet has one flow field no
 *  matter how many viewports, reflection captures or PIE windows are looking at
 *  it, and a view extension would step the sim once for each of them. The
 *  result would be a sim whose rate depends on how many things are rendering,
 *  which is the kind of bug that only appears when someone opens a second
 *  viewport.
 *
 *  The cost is that the work is enqueued from the game tick rather than
 *  scheduled inside the render graph the scene is already building, so it lands
 *  in its own command list. At sub-millisecond that is not worth the
 *  correctness risk.
 *
 *  WHY THE CONFIG IS RE-READ EVERY TICK.
 *
 *  Because the point of this stage is watching the field. Every value in the
 *  config takes effect on the next frame, so the asset can be left open beside
 *  the debug target and tuned live. Only the grid dimensions are latched, and
 *  changing those reallocates and re-seeds rather than trying to resample a
 *  vorticity field onto a different grid. */
 /** BlueprintType is load-bearing, not decorative. K2Node_GetSubsystem only
  *  offers classes marked with it, so without it the "Get Gas Giant Sim
  *  Subsystem" node does not appear in the palette at all and every
  *  BlueprintCallable member below is unreachable -- present in the class,
  *  impossible to call. */
UCLASS(BlueprintType)
class CLOUDATMOSPHERE_API UGasGiantSimSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	// -- UWorldSubsystem ----------------------------------------------------

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// -- FTickableGameObject ------------------------------------------------

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual bool IsTickableInEditor() const override { return true; }
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

	// -- Shadow bake --------------------------------------------------------

	/** Queue one deck shadow map for this frame. Called by each gas giant every
	 *  tick; the request is consumed by the next Tick and not retained.
	 *
	 *  HOSTED HERE FOR ORDERING, NOT BECAUSE IT IS SIM STATE. The bake reads the
	 *  flow texture this subsystem writes, and being on the same tick is what
	 *  puts the write before the read. Nothing else about it is shared: it holds
	 *  no state across frames, has no substeps, and runs whether or not the sim
	 *  is running.
	 *
	 *  ONE MAP PER PLANET, NOT PER VIEW. The map reaches the march as a material
	 *  parameter, which has no view dimension. A second viewport therefore
	 *  shares the first's camera-derived layer fades. That is an LOD mismatch in
	 *  the secondary view, not a wrong shadow, and it is a property of the
	 *  delivery rather than of this pass. */
	void RequestShadowBake(const FGasGiantShadowParams& InParams);

	// -- Control ------------------------------------------------------------

	/** Begin stepping against this config. Safe to call again with a different
	 *  config; only a grid change forces a reseed. */
	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	void StartSimulation(UGasGiantSimConfig* InConfig);

	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	void StopSimulation();

	/** Discard the field and re-seed, or re-upload InitialState if one is set. */
	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	void ResetSimulation();

	/** Capture the live state into a snapshot asset.
	 *
	 *  BLOCKS on the GPU. An authoring operation, not a runtime one -- it
	 *  flushes rendering, waits for the readback and copies a few megabytes
	 *  back. Calling it per frame would stall the pipeline every frame. */
	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	bool SaveSnapshot(UGasGiantSnapshot* Target);

	/** Advance exactly N substeps and then pause. The single most useful thing
	 *  in here while bringing the solver up: watching one advection step at a
	 *  time in the residual view localises a discretisation bug in minutes. */
	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	void StepOnce(int32 NumSteps = 1);

	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	bool IsSpinningUp() const { return StepsCompleted < SpinUpTarget; }

	/** Simulated time elapsed. */
	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	float GetSimulatedTime() const { return SimulatedTime; }

	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	int32 GetStepsCompleted() const { return StepsCompleted; }

	/** Current Courant number: peak rate * step * GridLongitude / 2pi.
	 *
	 *  A consequence of StepRatio, the profile and the grid, not a control.
	 *  Above 0.33 the numerical diffusion becomes a real dissipation term, so
	 *  this is worth watching when tuning DragRate. */
	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	float GetCourant() const;

private:
	/** The sim's half of Tick. Every early-out in here is a reason the field
	 *  should not advance, which is why the bake is not inside it. */
	void StepSimulation(float DeltaTime);

	/** Drains ShadowRequests into one render command each. */
	void BakeShadowMap();

	/** This frame's bakes, one per planet. Cleared on consumption rather than
	 *  keyed by requester: each request already names its own destination, so
	 *  there is nothing to match up and nothing to leave stale. */
	TArray<FGasGiantShadowParams> ShadowRequests;

	/** Builds the flat render-thread snapshot. Returns false if the config is
	 *  unusable, having already logged why. */
	bool BuildParams(FGasGiantSimParams& OutParams) const;

	/** Checks the render targets against the grid, reconfiguring them when
	 *  bAutoResizeTargets is set. Returns false if they remain unusable.
	 *
	 *  Validation before any dispatch, and all of it before any of it, for the
	 *  reason UNoiseBakeManifest validates a whole set before baking any of it:
	 *  a half-configured run is worse than a refused one, because it produces
	 *  output that looks like a result. */
	bool PrepareTargets() const;

	UPROPERTY(Transient)
	TObjectPtr<UGasGiantSimConfig> Config;

	FGasGiantSimulation* Simulation = nullptr;

	/** Hands the render thread the InitialState payload, if there is one worth
	 *  handing over. Returns true when a restore was queued, so the caller can
	 *  skip spin-up. */
	bool QueueInitialState();

	/** Consults UGasGiantSimSettings and starts if this world type wants it.
	 *  Run from the first Tick rather than Initialize, because the world is not
	 *  reliably ready to resolve a soft object reference that early. */
	void TryAutoStart();

	/** Logs any setting that is authored but currently has no effect.
	 *
	 *  An inert parameter is the failure mode this system produces most often
	 *  and hides best: nothing errors, the value sits in the details panel
	 *  looking applied, and the only symptom is that changing it does nothing.
	 *  Cheaper to state at start than to rediscover. */
	void ReportInertSettings() const;

	/** Logs the step size, Courant number and the TimeScale above which the sim
	 *  goes diffusive. Reported, never enforced -- see StepRatio. */
	void ReportCourant() const;

	bool bTriedAutoStart = false;

	/** Real time banked toward the next fixed substep. */
	float StepAccumulator = 0.0f;

	float SimulatedTime = 0.0f;
	int32 StepsCompleted = 0;

	/** Substeps to run before free-running. Set from SpinUpSteps at start. */
	int32 SpinUpTarget = 0;

	/** Manual steps queued by StepOnce, honoured even while paused. */
	int32 PendingManualSteps = 0;

	bool bRunning = false;
};