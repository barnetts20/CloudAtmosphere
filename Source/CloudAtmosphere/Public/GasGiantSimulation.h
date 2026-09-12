#pragma once

#include "CoreMinimal.h"
#include "GasGiantSimTypes.h"
#include "RenderGraphResources.h"

class FRDGBuilder;

/** The sim's persistent GPU state and the passes that advance it. Render thread
 *  only: everything arrives through FGasGiantSimParams, a flat copy made on the
 *  game thread, and this class never touches a UObject.
 *
 *  POOLED RATHER THAN TRANSIENT, because RDG resources live for one graph and a
 *  simulation is defined by state that survives between them. The vorticity, the
 *  streamfunction and the reduction buffers are allocated once and re-registered
 *  into each frame's graph; rebuilding from scratch each frame is not a
 *  simulation but an expensive procedural texture.
 *
 *  THE PING-PONG IS TRACKED RATHER THAN INFERRED. Advect, force and filter each
 *  read the whole vorticity field and write the whole vorticity field, so
 *  vorticity is two textures with an index that flips three times per substep.
 *  An ODD number of flips means the live buffer alternates between substeps,
 *  which is why the index is a member rather than recomputed from the frame
 *  number, and why every early-out path has to leave it consistent.
 *
 *  The streamfunction needs no ping-pong: red-black SOR updates in place, no
 *  thread in a sweep reading a texel another thread in that sweep writes. */
class CLOUDATMOSPHERE_API FGasGiantSimulation
{
public:
	/** Discard all state. The next Enqueue rebuilds and re-seeds. */
	void RequestReset();

	/** Hand the next initialisation a captured state to upload instead of seeding.
	 *  Consumed once, then dropped. Deliberately NOT routed through
	 *  FGasGiantSimParams, which is copied into a render command every frame: a
	 *  few megabytes used once at init would be paid for on every frame. */
	void QueueRestore_RenderThread(TArray<float>&& InData);

	/** Adds a pass copying the live state into Buffer, and enqueues a readback.
	 *  Editor-side capture path; the caller flushes and reads. */
	void AddCapturePass_RenderThread(FRDGBuilder& GraphBuilder, const FGasGiantSimParams& Params, class FRHIGPUBufferReadback* Readback);

	/** True once the initial condition has been constructed. */
	bool IsInitialised() const { return bInitialised; }

	/** Adds this frame's passes to the graph. NumSubsteps of zero is legal and
	 *  useful: it still runs the velocity pass and the debug view, so a paused sim
	 *  can be inspected in every mode without advancing it. */
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

	/** Grid the pooled state was allocated for. A change reallocates and re-seeds:
	 *  no resampling of a vorticity field onto a different grid is cheaper or more
	 *  faithful than starting over. */
	FIntVector AllocatedGrid = FIntVector::ZeroValue;

	/** Consumed by the next initialisation, then emptied. */
	TArray<float> PendingRestore;

	bool bInitialised = false;
	bool bResetRequested = false;
};