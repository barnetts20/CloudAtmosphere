#pragma once

#include "CoreMinimal.h"
#include "FlowSimTypes.h"
#include "RenderGraphResources.h"

class FRDGBuilder;

/** The sim's log channel, shared by the solver and the subsystem that drives it.
 *  Defined in FlowSimulation.cpp. */
DECLARE_LOG_CATEGORY_EXTERN(LogFlowSim, Log, All);

/** The sim's persistent GPU state and the passes that advance it. Render thread
 *  only: everything arrives through FFlowSimParams, and this class never touches
 *  a UObject.
 *
 *  POOLED RATHER THAN TRANSIENT, because RDG resources live for one graph and a
 *  simulation is defined by state that survives between them.
 *
 *  THE STATE is the face velocities and the geopotential. Everything else is
 *  rebuilt within a substep.
 *
 *  THE FACE PING-PONG IS TRACKED RATHER THAN INFERRED. Predict, Filter and
 *  Correct each read every face and write every face, so the faces are two
 *  textures with an index that flips three times per substep. An ODD number of
 *  flips means the live buffer alternates between substeps, which is why the
 *  index is a member and every early-out path has to leave it consistent.
 *
 *  The geopotential needs no ping-pong: the Helmholtz inverse transform writes
 *  it once per substep. */
class CLOUDATMOSPHERE_API FFlowSimulation
{
public:
	/** Floats per cell in a snapshot: u, v, phi. */
	static constexpr int32 StateFloatsPerCell = 3;

	/** Discard all state. The next Enqueue rebuilds and re-seeds. */
	void RequestReset();

	/** Hand the next initialisation a captured state to upload instead of
	 *  seeding. Consumed once. Not routed through FFlowSimParams, which is
	 *  copied into a render command every frame. */
	void QueueRestore_RenderThread(TArray<float>&& InData);

	/** Adds a pass copying the live state into a buffer, and enqueues a
	 *  readback. Editor-side capture path; the caller flushes and reads. */
	void AddCapturePass_RenderThread(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, class FRHIGPUBufferReadback* Readback);

	bool IsInitialised() const { return bInitialised; }

	/** Adds this frame's passes to the graph. Zero substeps is legal: the output
	 *  and debug passes still run, so a paused sim can be inspected. */
	void Enqueue_RenderThread(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, int32 NumSubsteps);

	/** Drops the pooled allocations. Called from the subsystem's teardown. */
	void Release_RenderThread();

private:
	/** Allocates the pooled state, or reallocates it if the grid changed.
	 *  Returns true when the caller must also seed. */
	bool EnsureResources(const FFlowSimParams& Params);

	void AddBalancePass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const struct FFlowSimResources& R);
	void AddInitPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const struct FFlowSimResources& R);
	void AddRestorePass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const struct FFlowSimResources& R);
	void AddReducePasses(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const struct FFlowSimResources& R);
	/** Centre, explicit and output fields of the current faces. bLatest writes
	 *  the output pair the resample blends toward, rather than the working pair. */
	void AddReconstructPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const struct FFlowSimResources& R, bool bLatest);
	void AddSubstep(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, struct FFlowSimResources& R);
	void AddDebugPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const struct FFlowSimResources& R);
	void AddResamplePass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const struct FFlowSimResources& R);

	TRefCountPtr<IPooledRenderTarget> PooledFace[2];
	TRefCountPtr<IPooledRenderTarget> PooledCentre;
	TRefCountPtr<IPooledRenderTarget> PooledExplicit;
	TRefCountPtr<IPooledRenderTarget> PooledPhi;
	TRefCountPtr<IPooledRenderTarget> PooledPhiStar;
	TRefCountPtr<IPooledRenderTarget> PooledRhs;
	TRefCountPtr<IPooledRenderTarget> PooledSpectrum[2];
	TRefCountPtr<IPooledRenderTarget> PooledCloud[2];

	/** Noise displacements, phase A slices then phase B. Flips with the cloud. */
	TRefCountPtr<IPooledRenderTarget> PooledNoise[2];
	/** The output on the sim's own grid, before the resample onto the atlas.
	 *  Doubles as the previous state's output once a frame's steps are done. */
	TRefCountPtr<IPooledRenderTarget> PooledLatLon;

	/** The latest state's centre fields and output, which the resample blends
	 *  toward from Centre and LatLon by FFlowSimParams::StateBlend. */
	TRefCountPtr<IPooledRenderTarget> PooledCentreLatest;
	TRefCountPtr<IPooledRenderTarget> PooledLatLonLatest;

	TRefCountPtr<IPooledRenderTarget> PooledRowMean;
	TRefCountPtr<IPooledRenderTarget> PooledPhiEq;
	TRefCountPtr<IPooledRenderTarget> PooledGlobalMean;

	/** Which of PooledFace holds the live faces. */
	int32 CurrentFace = 0;

	/** Which of PooledCloud and PooledNoise hold the live tracers. Flips once
	 *  per substep. */
	int32 CurrentCloud = 0;

	/** Grid the pooled state was allocated for. A change reallocates and
	 *  re-seeds. */
	FIntVector AllocatedGrid = FIntVector::ZeroValue;

	/** Consumed by the next initialisation, then emptied. */
	TArray<float> PendingRestore;

	bool bInitialised = false;
	bool bResetRequested = false;
};