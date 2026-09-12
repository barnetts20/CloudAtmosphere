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
 *  A WORLD SUBSYSTEM AND NOT A SCENE VIEW EXTENSION. A view extension runs once
 *  per view, and a planet's weather is world state: one planet has one flow
 *  field however many viewports, reflection captures or PIE windows are looking
 *  at it, so a view extension would step the sim once for each and the sim's
 *  rate would depend on how many things are rendering. The cost is that the work
 *  is enqueued from the game tick rather than scheduled inside the render graph
 *  the scene is already building, landing in its own command list.
 *
 *  THE CONFIG IS RE-READ EVERY TICK, so every value takes effect on the next
 *  frame and the asset can be tuned live beside the debug target. Only the grid
 *  dimensions are latched; changing those reallocates and re-seeds.
 *
 *  PITFALL: BlueprintType is load-bearing. K2Node_GetSubsystem only offers
 *  classes marked with it, so without it the "Get Gas Giant Sim Subsystem" node
 *  never appears in the palette and every BlueprintCallable member below is
 *  unreachable -- present in the class, impossible to call. */
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
	 *  flow texture this subsystem writes, and sharing a tick is what puts the
	 *  write before the read. It holds no state across frames, has no substeps,
	 *  and runs whether or not the sim is running.
	 *
	 *  ONE MAP PER PLANET, NOT PER VIEW: the map reaches the march as a material
	 *  parameter, which has no view dimension, so a second viewport shares the
	 *  first's camera-derived layer fades. An LOD mismatch in the secondary view
	 *  rather than a wrong shadow, and a property of the delivery. */
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

	/** Capture the live state into a snapshot asset. BLOCKS on the GPU: it flushes
	 *  rendering, waits for the readback and copies a few megabytes. An authoring
	 *  operation, not a runtime one. */
	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	bool SaveSnapshot(UGasGiantSnapshot* Target);

	/** Advance exactly N substeps and then pause. Watching one advection step at a
	 *  time in the residual view is what localises a discretisation bug. */
	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	void StepOnce(int32 NumSteps = 1);

	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	bool IsSpinningUp() const { return StepsCompleted < SpinUpTarget; }

	/** Simulated time elapsed. */
	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	float GetSimulatedTime() const { return SimulatedTime; }

	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	int32 GetStepsCompleted() const { return StepsCompleted; }

	/** Current Courant number: peak rate * step * GridLongitude / 2pi. A
	 *  consequence of StepRatio, the profile and the grid rather than a control.
	 *  Above 0.33 the numerical diffusion becomes a real dissipation term, so it
	 *  is worth watching when tuning DragRate. */
	UFUNCTION(BlueprintCallable, Category = "Gas Giant")
	float GetCourant() const;

private:
	/** The sim's half of Tick. Every early-out here is a reason the field should
	 *  not advance, which is why the bake is not inside it. */
	void StepSimulation(float DeltaTime);

	/** Drains ShadowRequests into one render command each. */
	void BakeShadowMap();

	/** This frame's bakes, one per planet. Cleared on consumption rather than keyed
	 *  by requester: each request names its own destination, so there is nothing to
	 *  match up and nothing to leave stale. */
	TArray<FGasGiantShadowParams> ShadowRequests;

	/** Builds the flat render-thread snapshot. Returns false if the config is
	 *  unusable, having already logged why. */
	bool BuildParams(FGasGiantSimParams& OutParams) const;

	/** Checks the render targets against the grid, reconfiguring them when
	 *  bAutoResizeTargets is set. Returns false if they remain unusable. All of
	 *  the validation before any of the dispatches: a half-configured run is worse
	 *  than a refused one, because it produces output that looks like a result. */
	bool PrepareTargets() const;

	UPROPERTY(Transient)
	TObjectPtr<UGasGiantSimConfig> Config;

	FGasGiantSimulation* Simulation = nullptr;

	/** Hands the render thread the InitialState payload, if there is one worth
	 *  handing over. Returns true when a restore was queued, so the caller can
	 *  skip spin-up. */
	bool QueueInitialState();

	/** Consults UGasGiantSimSettings and starts if this world type wants it. Run
	 *  from the first Tick rather than Initialize: the world is not reliably ready
	 *  to resolve a soft object reference that early. */
	void TryAutoStart();

	/** Logs any setting that is authored but currently has no effect. An inert
	 *  parameter is the failure mode this system hides best: nothing errors, the
	 *  value sits in the details panel looking applied, and the only symptom is
	 *  that changing it does nothing. */
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