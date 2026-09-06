#pragma once

#include "CoreMinimal.h"
#include "GasGiantSimTypes.h"
#include "RenderGraphResources.h"

class FRDGBuilder;

/** The sim's persistent GPU state and the passes that advance it.
 *
 *  Render thread only. Everything it needs arrives through FGasGiantSimParams,
 *  which is a flat copy made on the game thread; this class never touches a
 *  UObject. That is the same split FNoiseVolumeBaker already draws, and for the
 *  same reason.
 *
 *  WHY THE STATE IS POOLED RATHER THAN TRANSIENT.
 *
 *  RDG resources live for one graph. A simulation is defined by state that
 *  survives between graphs, so the vorticity, the streamfunction and the two
 *  reduction buffers are allocated once as pooled textures and re-registered
 *  into each frame's graph. The alternative -- rebuilding from scratch each
 *  frame -- is not a simulation, it is an expensive procedural texture.
 *
 *  THE PING-PONG, AND WHY IT IS TRACKED RATHER THAN INFERRED.
 *
 *  Three passes per substep read the whole vorticity field and write the whole
 *  vorticity field: advect, force and filter. Each therefore needs a distinct
 *  source and destination, so vorticity is two textures with an index that
 *  flips three times per substep. An odd number of flips means the "current"
 *  buffer alternates between substeps, which is why the index is a member
 *  rather than something recomputed from the frame number -- and why every
 *  early-out path below still has to leave it consistent.
 *
 *  The streamfunction needs no ping-pong: red-black SOR updates in place, since
 *  no thread in a sweep reads a texel that another thread in the same sweep
 *  writes. */
class CLOUDATMOSPHERE_API FGasGiantSimulation
{
public:
	/** Discard all state. The next Enqueue rebuilds and re-seeds. */
	void RequestReset();

	/** Hand the next initialisation a captured state to upload instead of
	 *  seeding. Consumed once, then dropped.
	 *
	 *  Deliberately NOT routed through FGasGiantSimParams. That struct is
	 *  copied into a render command every frame, and a few megabytes of state
	 *  used exactly once at init would be paid for on every frame forever. */
	void QueueRestore_RenderThread(TArray<float>&& InData);

	/** Adds a pass copying the live state into Buffer, and enqueues a readback.
	 *  Editor-side capture path; the caller flushes and reads. */
	void AddCapturePass_RenderThread(FRDGBuilder& GraphBuilder, const FGasGiantSimParams& Params, class FRHIGPUBufferReadback* Readback);

	/** True once the initial condition has been constructed. */
	bool IsInitialised() const { return bInitialised; }

	/** Adds this frame's passes to the graph.
	 *
	 *  NumSubsteps of zero is legal and useful: it still runs the velocity pass
	 *  and the debug view, so a paused sim can be inspected in every debug mode
	 *  and a mid-spin-up state can be examined without advancing it. */
	void Enqueue_RenderThread(FRDGBuilder& GraphBuilder, const FGasGiantSimParams& Params, int32 NumSubsteps);

	/** Drops the pooled allocations. Called from the subsystem's teardown. */
	void Release_RenderThread();

private:
	/** Allocates the pooled state, or reallocates it if the grid changed.
	 *  Returns true when the caller must also run the seeding passes. */
	bool EnsureResources(const FGasGiantSimParams& Params);

	void AddInitPasses(FRDGBuilder& GraphBuilder, const FGasGiantSimParams& Params, const struct FGasGiantSimResources& R);
	void AddRestorePass(FRDGBuilder& GraphBuilder, const FGasGiantSimParams& Params, const struct FGasGiantSimResources& R);
	void AddSubstep(FRDGBuilder& GraphBuilder, const FGasGiantSimParams& Params, struct FGasGiantSimResources& R);
	void AddPoissonSolve(FRDGBuilder& GraphBuilder, const FGasGiantSimParams& Params, const struct FGasGiantSimResources& R, int32 Iterations);
	void AddDebugPass(FRDGBuilder& GraphBuilder, const FGasGiantSimParams& Params, const struct FGasGiantSimResources& R);

	TRefCountPtr<IPooledRenderTarget> PooledVorticity[2];
	TRefCountPtr<IPooledRenderTarget> PooledPsi;
	TRefCountPtr<IPooledRenderTarget> PooledRowMean;
	TRefCountPtr<IPooledRenderTarget> PooledPsiRowMean;
	TRefCountPtr<IPooledRenderTarget> PooledGlobalMean;

	/** Which of PooledVorticity holds the live field. */
	int32 CurrentVorticity = 0;

	/** Grid the pooled state was allocated for. A change reallocates and
	 *  re-seeds, because there is no meaningful way to resample a vorticity
	 *  field onto a different grid that is cheaper than starting over. */
	FIntVector AllocatedGrid = FIntVector::ZeroValue;

	/** Consumed by the next initialisation, then emptied. */
	TArray<float> PendingRestore;

	bool bInitialised = false;
	bool bResetRequested = false;
};