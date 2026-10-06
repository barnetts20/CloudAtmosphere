#pragma once

#include "CoreMinimal.h"
#include "FlowSimTypes.h"
#include "RenderGraphResources.h"

class FRDGBuilder;

/** The sim's log channel, shared by the solver and the subsystem that drives it. */
DECLARE_LOG_CATEGORY_EXTERN(LogFlowSim, Log, All);

/** The sim's persistent GPU state and the passes that advance it. Render thread
 *  only: everything arrives through FFlowSimParams; it never touches a UObject.
 *
 *  POOLED RATHER THAN TRANSIENT: RDG resources live for one graph, and the sim's
 *  state must survive between graphs.
 *
 *  THE STATE is the face velocities, layer thicknesses, tracers, noise
 *  displacements and storm cells. Everything else is rebuilt within a substep.
 *
 *  THE FACE PING-PONG IS TRACKED, NOT INFERRED. Predict, Filter and Correct each
 *  read and write every face, flipping the index three times per substep, so the
 *  live buffer alternates between substeps and every early-out path must leave
 *  the index consistent. The thicknesses need no ping-pong: Correct writes them
 *  once per substep from the modal amplitudes the solve leaves in PhiStar. */
class CLOUDATMOSPHERE_API FFlowSimulation
{
public:
	/** Floats per cell in a snapshot: u, v, phi, the four tracer channels
	 *  (cloud, cloud ascent, vapour, storm), both noise phases' displacements
	 *  (xyz each), then the low-passed ascent and the eye tracer. */
	static constexpr int32 StateFloatsPerCell = 15;

	/** The layout without the last two planes, which a restore still reads;
	 *  those two come back as zero. */
	static constexpr int32 LegacyStateFloatsPerCell = 13;

	/** Floats after the per-cell planes: every storm cell slot's two float4s
	 *  of state, then every slot's seed float4. */
	static constexpr int32 StateTrailingFloats = 12 * 32;

	/** The trailer without the seeds, which a restore still reads; the seeds
	 *  then come back as -1, the trait ranges' midpoints. */
	static constexpr int32 LegacyStateTrailingFloats = 8 * 32;

	/** Floats a snapshot of this grid holds. */
	static int32 StateFloats(const FIntVector& Grid, int32 FloatsPerCell = StateFloatsPerCell, int32 Trailing = StateTrailingFloats)
	{
		return Grid.X * Grid.Y * Grid.Z * FloatsPerCell + Trailing;
	}

	/** Which layout Num floats are for this grid, or 0 for none; bOutSeeds
	 *  says whether its trailer carries the cells' seeds. */
	static int32 FloatsPerCellOf(const FIntVector& Grid, int32 Num, bool* bOutSeeds = nullptr)
	{
		const bool bSeeds = Num == StateFloats(Grid);

		if (bOutSeeds)
		{
			*bOutSeeds = bSeeds;
		}

		return bSeeds ? StateFloatsPerCell
			: Num == StateFloats(Grid, StateFloatsPerCell, LegacyStateTrailingFloats) ? StateFloatsPerCell
			: Num == StateFloats(Grid, LegacyStateFloatsPerCell, LegacyStateTrailingFloats) ? LegacyStateFloatsPerCell
			: 0;
	}

	/** Discard all state. The next Enqueue rebuilds and re-seeds. */
	void RequestReset();

	/** Hand the next initialisation a captured state to upload instead of seeding.
	 *  Consumed once; an empty array cancels a pending one. Kept out of
	 *  FFlowSimParams, which is copied into a render command every frame. */
	void QueueRestore_RenderThread(TArray<float>&& InData);

	/** True when the state is the latest reset's: an Enqueue has run since. */
	bool HasState_RenderThread() const { return bInitialised && !bResetRequested && PooledPhi.IsValid(); }

	/** Adds a pass copying the live state into a buffer and enqueues a readback
	 *  for the caller to flush and read. False, with nothing enqueued, without
	 *  HasState_RenderThread. OutGrid is the allocated grid the capture's layout follows. */
	bool AddCapturePass_RenderThread(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, class FRHIGPUBufferReadback* Readback, FIntVector& OutGrid);

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
	void AddRestorePass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const struct FFlowSimResources& R, int32 FloatsPerCell, bool bSeeds);
	void AddReducePasses(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const struct FFlowSimResources& R);
	/** Centre, explicit and output fields of the current faces. bLatest writes
	 *  the output pair the resample blends toward, rather than the working pair,
	 *  and no explicit field. */
	void AddReconstructPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const struct FFlowSimResources& R, bool bLatest);
	void AddCellsPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const struct FFlowSimResources& R);
	/** Every slot's traits from its seed under Params' ranges, without a step. */
	void AddCellTraitsPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const struct FFlowSimResources& R);
	/** The cells' streamfunction, potential and column terms for Predict. */
	void AddCellFieldPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const struct FFlowSimResources& R);
	void AddSubstep(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, struct FFlowSimResources& R);
	void AddDebugPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const struct FFlowSimResources& R);
	void AddResamplePass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const struct FFlowSimResources& R);

	TRefCountPtr<IPooledRenderTarget> PooledFace[2];
	TRefCountPtr<IPooledRenderTarget> PooledCentre;
	TRefCountPtr<IPooledRenderTarget> PooledExplicit;
	TRefCountPtr<IPooledRenderTarget> PooledPhi;

	/** phi* until the solve, then the modal amplitudes Correct reads. */
	TRefCountPtr<IPooledRenderTarget> PooledPhiStar;
	TRefCountPtr<IPooledRenderTarget> PooledRhs;
	TRefCountPtr<IPooledRenderTarget> PooledSpectrum[2];

	/** Cloud, cloud ascent, vapour and storm per layer. */
	TRefCountPtr<IPooledRenderTarget> PooledTracer[2];

	/** Storm cells: two float4 of state per slot, advanced in place, then two
	 *  float4 of vortex gains, two of inflow gains, one of health and one of the
	 *  pressure low per slot, rewritten every substep. */
	TRefCountPtr<FRDGPooledBuffer> PooledCells;

	/** The cells' fields Predict reads, rebuilt every substep: per layer on the
	 *  face rows, and one layer-independent column slice. */
	TRefCountPtr<IPooledRenderTarget> PooledCellFlow;
	TRefCountPtr<IPooledRenderTarget> PooledCellColumn;

	/** Noise displacements, phase A slices then phase B; flips with the tracers. */
	TRefCountPtr<IPooledRenderTarget> PooledNoise[2];

	/** The output on the sim's own grid, before the resample onto the atlas.
	 *  Doubles as the previous state's output once a frame's steps are done. */
	TRefCountPtr<IPooledRenderTarget> PooledLatLon;

	/** The latest state's centre fields and output, which the resample blends
	 *  toward from Centre and LatLon by FFlowSimParams::StateBlend. */
	TRefCountPtr<IPooledRenderTarget> PooledCentreLatest;
	TRefCountPtr<IPooledRenderTarget> PooledLatLonLatest;

	TRefCountPtr<IPooledRenderTarget> PooledRowMean;
	TRefCountPtr<IPooledRenderTarget> PooledMontgomeryEq;
	TRefCountPtr<IPooledRenderTarget> PooledGlobalMean;

	/** Which of PooledFace holds the live faces. */
	int32 CurrentFace = 0;

	/** Which of PooledTracer and PooledNoise hold the live tracers. Flips once
	 *  per substep. */
	int32 CurrentTracer = 0;

	/** Grid the pooled state was allocated for; a change reallocates and re-seeds. */
	FIntVector AllocatedGrid = FIntVector::ZeroValue;

	/** Consumed by the next initialisation, then emptied. */
	TArray<float> PendingRestore;

	/** The balance pass's inputs when it last ran. It reruns only when they
	 *  change; empty after a release. */
	TArray<float> BalanceKey;

	/** Every scalar parameter but the blend when the latest pair was last
	 *  written. A frame without steps rewrites it only when they change; empty
	 *  after a release or a seed. */
	TArray<uint8> LatestKey;

	bool bInitialised = false;
	bool bResetRequested = false;
};