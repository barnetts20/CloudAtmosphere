#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Engine/EngineBaseTypes.h"
#include "TerrestrialShadowMap.h"
#include "FlowSimTypes.h"
#include "FlowSimSubsystem.generated.h"

class FFlowSimulation;
class UFlowSnapshot;
class UTextureRenderTarget2D;
class UTextureRenderTarget2DArray;
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
 *  ONE SIM, MANY PLANETS. Atmospheres claim the sim every tick with their
 *  active config; the nearest drives it, and the others draw the copy of the
 *  field they kept when they last did. Planets are assumed far enough apart
 *  that only one on screen needs live weather.
 *
 *  THE RENDER TARGETS ARE THIS SUBSYSTEM'S, created at run time and sized to
 *  the running config's grid, so no asset has to match the sim.
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

	/** Queue one cloud shadow map for this frame. Called by each planet every
	 *  tick; the request is consumed by the next Tick and not retained. False
	 *  when it is unusable and dropped.
	 *
	 *  HOSTED HERE FOR ORDERING, NOT BECAUSE IT IS SIM STATE. The bake reads the
	 *  flow texture this subsystem writes, and sharing a tick is what puts the
	 *  write before the read. It holds no state across frames, has no substeps,
	 *  and runs whether or not the sim is running.
	 *
	 *  ONE MAP PER PLANET, NOT PER VIEW: it is baked once a frame, so a second
	 *  viewport shares the first's camera-derived layer fades. */
	bool RequestShadowBake(const FTerrestrialShadowParams& InParams);

	// -- Ownership ----------------------------------------------------------

	/** Bids for the sim this frame with the claimant's active config. Called by
	 *  each claiming atmosphere every tick; the bids are resolved as the next
	 *  frame starts. The nearest claimant drives the sim, and keeps it until
	 *  another is nearer by a margin. A new owner, or the owner's config
	 *  changing, restarts the sim from that config's InitialState, after the
	 *  outgoing owner's field is copied into the Keep target it bid with.
	 *
	 *  AN OWNER'S CONFIG WINS: FlowSim.Start is overridden on the next frame
	 *  while an atmosphere claims the sim. */
	void ClaimSimulation(const UObject* Claimant, UFlowSimConfig* InConfig, double Distance,
		UTextureRenderTarget2DArray* Keep);

	/** Moves the running sim from From to To without a restart, when Claimant
	 *  drives it under From: a config replaced by its runtime copy. */
	void AdoptConfig(const UObject* Claimant, UFlowSimConfig* From, UFlowSimConfig* To);

	/** Gives up the sim if Claimant drives it, stopping the sim until the next
	 *  bid; bKeepField copies its field into its Keep target first. */
	void ReleaseClaim(const UObject* Claimant, bool bKeepField);

	bool IsOwner(const UObject* Claimant) const { return Claimant && Owner.Get() == Claimant; }

	/** The atlas the sim writes and the field reads; null before the first
	 *  frame the sim steps. */
	UTextureRenderTarget2DArray* GetFlowTarget() const { return FlowTarget; }

	/** The debug view, one texel per cell, while the config's bDebugView is on;
	 *  null otherwise. */
	UTextureRenderTarget2D* GetDebugTarget() const;

	/** The clock a claimant's field was kept at, once, after the copy: the
	 *  display time and the config it was simulated under. False when nothing
	 *  was kept for it since the last call. */
	bool TakeKeptClock(const UObject* Claimant, double& OutTime, UFlowSimConfig*& OutConfig);

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
	float GetSimulatedTime() const { return (float)SimulatedTime; }

	/** Sim time of the state the output shows, one step or less behind
	 *  GetSimulatedTime. Anything animated alongside the field clocks off this.
	 *  The sim steps before actors tick, so an actor reads the state this frame
	 *  renders.
	 *  PITFALL: READ BEFORE THE STEP, IT LAGS THE RENDERED FIELD BY A FRAME. At a
	 *  high SimSpeed the noise phases the renderer weights then no longer reach
	 *  zero where the sim resets them, and the whole field snaps.
	 *  Double: a float stops resolving a frame's advance within days of sim
	 *  time, so reduce any phase from it before narrowing. */
	UFUNCTION(BlueprintCallable, Category = "Flow Sim")
	double GetDisplayTime() const { return SimulatedTime - (1.0 - StateBlend) * CurrentStep; }

	UFUNCTION(BlueprintCallable, Category = "Flow Sim")
	int64 GetStepsCompleted() const { return StepsCompleted; }

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

	/** Resolves the claims, auto-starts if nobody claims, then StepSimulation.
	 *  Once per frame. */
	void Advance(float DeltaTime);

	/** One atmosphere's bid; see ClaimSimulation. */
	struct FSimClaim
	{
		TWeakObjectPtr<const UObject> Claimant;
		TWeakObjectPtr<UFlowSimConfig> Config;
		double Distance = 0.0;
		TWeakObjectPtr<UTextureRenderTarget2DArray> Keep;
	};

	/** The bids made since the last frame started. */
	TArray<FSimClaim> Claims;

	/** The atmosphere driving the sim, and where its field is kept when it
	 *  stops. */
	TWeakObjectPtr<const UObject> Owner;
	TWeakObjectPtr<UTextureRenderTarget2DArray> OwnerKeep;

	/** True once any atmosphere has claimed the sim: the auto-start is for
	 *  worlds with none. */
	bool bEverClaimed = false;

	/** Frames advanced, so the auto-start waits a frame for the first bids. */
	int32 FramesAdvanced = 0;

	/** Picks this frame's owner from the last frame's bids, keeps the outgoing
	 *  owner's field and restarts the sim on a change of owner or config. */
	void ResolveClaims();

	/** Copies the flow atlas into Keep, sizing Keep to match, and records the
	 *  clock the copy holds for KeptFor. */
	void KeepFlow(UTextureRenderTarget2DArray* Keep, const UObject* KeptFor);

	/** Kept clocks not yet taken: the display time and the config. */
	struct FKeptClock
	{
		double Time = 0.0;
		TWeakObjectPtr<UFlowSimConfig> Config;
	};

	TMap<TWeakObjectPtr<const UObject>, FKeptClock> KeptClocks;

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
	TArray<FTerrestrialShadowParams> ShadowRequests;

	/** Builds the flat render-thread snapshot at a step. Returns false if the config is unusable, having
	 *  already logged why. */
	bool BuildParams(FFlowSimParams& OutParams, float Step) const;

	/** Creates the render targets on first use and sizes them to the grid.
	 *  Returns false without a config. */
	bool PrepareTargets();

	UPROPERTY(Transient)
	TObjectPtr<UFlowSimConfig> Config;

	/** The cube atlas the sim resamples its output onto, RGBA16F with
	 *  4 * LayerCount slices; see FlowField.ush. */
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2DArray> FlowTarget;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> DebugTarget;

	FFlowSimulation* Simulation = nullptr;

	/** Hands the render thread the InitialState payload, if there is one worth
	 *  handing over. Returns true when a restore was queued, so the caller can
	 *  skip spin-up. */
	bool QueueInitialState();

	/** Consults UFlowSimSettings and starts if this world type wants it. Run
	 *  from the second frame rather than Initialize: the world is not reliably
	 *  ready to resolve a soft object reference that early, and the first
	 *  frame's atmospheres have bid by then. */
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

	/** Double precision: a float stops resolving a small step within hours. */
	double SimulatedTime = 0.0;
	int64 StepsCompleted = 0;

	/** The step clock, FFlowSimParams::AnchorTime and AnchorStep at ClockStep:
	 *  re-anchored at the current time when the step changes, and at zero
	 *  ClockStep after a reset or restore. */
	double ClockAnchorTime = 0.0;
	int64 ClockAnchorStep = 0;
	float ClockStep = 0.0f;

	/** The grid the running state was reset at. A config grid that differs
	 *  resets the sim, since the state is reallocated either way. */
	FIntVector RunningGrid = FIntVector::ZeroValue;

	/** Substeps to run before free-running. Set from SpinUpTurnovers at start. */
	int32 SpinUpTarget = 0;

	/** Manual steps queued by StepOnce, honoured even while paused. */
	int32 PendingManualSteps = 0;

	bool bRunning = false;
};