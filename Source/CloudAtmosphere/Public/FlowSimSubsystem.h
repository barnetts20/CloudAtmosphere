#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Engine/EngineBaseTypes.h"
#include "GasGiantShadowMap.h"
#include "TerrestrialShadowMap.h"
#include "FlowSimTypes.h"
#include "FlowSimSubsystem.generated.h"

class FFlowSimulation;
class UFlowSnapshot;
class UWorld;

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
 *  classes marked with it, so without it the "Get Flow Sim Subsystem" node
 *  never appears in the palette and every BlueprintCallable member below is
 *  unreachable -- present in the class, impossible to call. */
UCLASS(BlueprintType)
class CLOUDATMOSPHERE_API UFlowSimSubsystem : public UTickableWorldSubsystem
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
	 *  first's camera-derived layer fades. */
	void RequestShadowBake(const FGasGiantShadowParams& InParams);

	/** The terrestrial field's bake. A SEPARATE QUEUE, not an overload sharing
	 *  one: the two params structs are separate types bound to separate shaders,
	 *  and they diverge as the fields do. */
	void RequestShadowBake(const FTerrestrialShadowParams& InParams);

	// -- Control ------------------------------------------------------------

	/** Begin stepping against this config. Always reseeds, or restores the
	 *  config's InitialState when it has one. */
	UFUNCTION(BlueprintCallable, Category = "Flow Sim")
	void StartSimulation(UFlowSimConfig* InConfig);

	UFUNCTION(BlueprintCallable, Category = "Flow Sim")
	void StopSimulation();

	/** Discard the field and re-seed, or re-upload InitialState if one is set. */
	UFUNCTION(BlueprintCallable, Category = "Flow Sim")
	void ResetSimulation();

	/** Capture the live state into a snapshot asset. BLOCKS on the GPU: it flushes
	 *  rendering, waits for the readback and copies a few megabytes. An authoring
	 *  operation, not a runtime one. */
	UFUNCTION(BlueprintCallable, Category = "Flow Sim")
	bool SaveSnapshot(UFlowSnapshot* Target);

	/** Advance exactly N substeps and then pause. */
	UFUNCTION(BlueprintCallable, Category = "Flow Sim")
	void StepOnce(int32 NumSteps = 1);

	UFUNCTION(BlueprintCallable, Category = "Flow Sim")
	bool IsSpinningUp() const { return StepsCompleted < SpinUpTarget; }

	UFUNCTION(BlueprintCallable, Category = "Flow Sim")
	bool IsRunning() const { return bRunning; }

	/** The config the sim steps against, whichever caller started it; null
	 *  before the first start. */
	UFUNCTION(BlueprintCallable, Category = "Flow Sim")
	UFlowSimConfig* GetConfig() const { return Config; }

	/** Simulated time elapsed. */
	UFUNCTION(BlueprintCallable, Category = "Flow Sim")
	float GetSimulatedTime() const { return SimulatedTime; }

	/** Sim time of the state the output shows, one step or less behind
	 *  GetSimulatedTime. Anything animated alongside the field clocks off this.
	 *  The sim steps before actors tick, so an actor reads the state this frame
	 *  renders.
	 *  PITFALL: READ BEFORE THE STEP, IT LAGS THE RENDERED FIELD BY A FRAME. At a
	 *  high SimSpeed the noise phases the renderer weights then no longer reach
	 *  zero where the sim resets them, and the whole field snaps. */
	UFUNCTION(BlueprintCallable, Category = "Flow Sim")
	float GetDisplayTime() const { return SimulatedTime - (1.0f - StateBlend) * CurrentStep; }

	UFUNCTION(BlueprintCallable, Category = "Flow Sim")
	int32 GetStepsCompleted() const { return StepsCompleted; }

	/** Steps the last frame took: the cost readout for SimSpeed. */
	UFUNCTION(BlueprintCallable, Category = "Flow Sim")
	int32 GetStepsLastFrame() const { return LastSubsteps; }

	/** Current Courant number: peak rate * step * GridLongitude / 2pi, at the
	 *  step the last frame took. Above 0.33 the numerical diffusion becomes a
	 *  real dissipation term, so it is worth watching when tuning DragRate. */
	UFUNCTION(BlueprintCallable, Category = "Flow Sim")
	float GetCourant() const;

	/** The params the next frame builds at the last frame's step, derived
	 *  values included: output scales, stack, implicit weight. False with no
	 *  config. For inspection; the render thread gets its own copy. */
	bool GetRunningParams(FFlowSimParams& OutParams) const
	{
		if (!Config)
		{
			return false;
		}

		BuildParams(OutParams, CurrentStep);
		return true;
	}

private:
	/** Steps the sim ahead of every actor's tick; Tick steps instead on a frame
	 *  that has no actor ticks. */
	void OnPreActorTick(UWorld* InWorld, ELevelTick TickType, float DeltaTime);

	/** Auto-start on first use, then StepSimulation. Once per frame. */
	void Advance(float DeltaTime);

	/** The sim's half of the frame. Every early-out here is a reason the field
	 *  should not advance, which is why the bake is not inside it. */
	void StepSimulation(float DeltaTime);

	FDelegateHandle PreActorTickHandle;

	/** Set by OnPreActorTick, cleared by Tick. */
	bool bSteppedThisFrame = false;

	/** Drains ShadowRequests into one render command each. */
	void BakeShadowMap();

	/** This frame's bakes, one per planet. Cleared on consumption rather than keyed
	 *  by requester: each request names its own destination, so there is nothing to
	 *  match up and nothing to leave stale. */
	TArray<FGasGiantShadowParams> ShadowRequests;

	TArray<FTerrestrialShadowParams> TerrestrialShadowRequests;

	/** Builds the flat render-thread snapshot at a step. Returns false if the config is unusable, having
	 *  already logged why. */
	bool BuildParams(FFlowSimParams& OutParams, float Step) const;

	/** Checks the render targets against the grid, reconfiguring them when
	 *  bAutoResizeTargets is set. Returns false if they remain unusable. */
	bool PrepareTargets() const;

	UPROPERTY(Transient)
	TObjectPtr<UFlowSimConfig> Config;

	FFlowSimulation* Simulation = nullptr;

	/** Hands the render thread the InitialState payload, if there is one worth
	 *  handing over. Returns true when a restore was queued, so the caller can
	 *  skip spin-up. */
	bool QueueInitialState();

	/** Consults UFlowSimSettings and starts if this world type wants it. Run
	 *  from the first Tick rather than Initialize: the world is not reliably ready
	 *  to resolve a soft object reference that early. */
	void TryAutoStart();

	/** Logs any setting that is authored but currently has no effect. An inert
	 *  parameter is the failure mode this system hides best: nothing errors, the
	 *  value sits in the details panel looking applied, and the only symptom is
	 *  that changing it does nothing. */
	void ReportInertSettings() const;

	/** Logs the speed, the steps it takes per frame at 60 fps, and the Courant
	 *  numbers at that step. Reported, never enforced. */
	void ReportCourant() const;

	/** Logs the stack's mode wave speeds, how far the thermal shear is past the
	 *  point storms grow at, and warns when the balanced interfaces reach the
	 *  edge of the stack. */
	void ReportStack() const;

	bool bTriedAutoStart = false;

	/** The step the last frame took, which the Courant number and a snapshot's
	 *  parameters are read at. */
	float CurrentStep = 1e-5f;

	/** Sim time owed past the latest state, under one step. */
	float PendingTime = 0.0f;

	/** Where the output sits between the last two states; see
	 *  FFlowSimParams::StateBlend. */
	float StateBlend = 1.0f;

	int32 LastSubsteps = 0;

	/** 0 normal, 1 past FlowSimStep::WarnPerFrame, 2 at the hang guard. Logged
	 *  when it changes. */
	int32 StepLoadLevel = 0;

	float SimulatedTime = 0.0f;
	int32 StepsCompleted = 0;

	/** Substeps to run before free-running. Set from SpinUpSteps at start. */
	int32 SpinUpTarget = 0;

	/** Manual steps queued by StepOnce, honoured even while paused. */
	int32 PendingManualSteps = 0;

	bool bRunning = false;
};