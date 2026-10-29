#include "AtmosphereMarchSweep.h"

#include "CloudAtmosphere.h"
#include "Containers/Ticker.h"
#include "DynamicRHI.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopeLock.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include <atomic>

// ---------------------------------------------------------------------------
// The switches (AtmosphereMarchPass.usf, MarchAblate)
// ---------------------------------------------------------------------------

static TAutoConsoleVariable<int32> CVarAtmosphereMarchAblate(
	TEXT("r.CloudAtmosphere.March.Ablate"),
	0,
	TEXT("Stage P1e, measurement only: parts of the atmosphere march switched off, as bits.\n")
	TEXT("  1 the detail volume's fetches (its mean instead)\n")
	TEXT("  2 the structure volume's fetches (its mean instead)\n")
	TEXT("  4 the cloud shadow map's read (no deck shadow)\n")
	TEXT("  8 the air samples' light-tint column read (neutral tint)\n")
	TEXT(" 16 the entry refine (no substeps where a ray enters cloud)\n")
	TEXT(" 32 half the cloud steps\n")
	TEXT(" 64 the cloud shadow on geometry (the surface multiply)\n")
	TEXT("128 half the air steps\n")
	TEXT("256 the structure fetches taken twice (their cost, nothing drawn changes)\n")
	TEXT("512 the detail fetches taken twice (likewise)\n")
	TEXT("1024 the flow column read twice (likewise)\n")
	TEXT("2048 the cloud shadow read walking every cascade (R1a's walk; P1f reads only the levels that reach the result)\n")
	TEXT("4096 nothing (the sweep's control: a nonzero value alone)\n")
	TEXT("0 (default): the march as it is. CloudAtmosphere.MarchSweep steps through them."),
	ECVF_RenderThreadSafe);

namespace AtmosphereMarchSweep
{
	struct FSpan
	{
		uint32 Frame = 0;
		uint32 Ablate = 0;
		FRenderQueryRHIRef BeginQuery;
		FRenderQueryRHIRef EndQuery;
	};
}

namespace AtmosphereMarchSweepDetail
{
	using namespace AtmosphereMarchSweep;

	/** Frames a span waits before it's read, and after which an unread one is dropped. */
	constexpr uint32 ReadAfterFrames = 3;
	constexpr uint32 DropAfterFrames = 120;

	/** One frame's march passes, summed (every view: an editor can render more than one), and
	 *  the bits they ran with (a frame whose passes disagree is dropped). */
	struct FSample
	{
		uint32 Frame = 0;
		uint32 Ablate = 0;
		float Ms = 0.0f;
		int32 Passes = 0;
		bool bMixed = false;
	};

	// Never destructed: nothing of the RHI's is released after the RHI at exit.
	struct FState
	{
		FCriticalSection Lock;
		TArray<TSharedPtr<FSpan>> Pending;
		TArray<FRenderQueryRHIRef> Free;
		TMap<uint32, FSample> Sums;
		FCriticalSection DoneLock;
		TArray<FSample> Done;
	};

	static FState& State()
	{
		static FState* S = new FState();
		return *S;
	}

	static std::atomic<bool> GTiming{ false };

	static FRenderQueryRHIRef Allocate()
	{
		FState& S = State();
		if (S.Free.Num() > 0)
		{
			return S.Free.Pop(EAllowShrinking::No);
		}
		return RHICreateRenderQuery(RQT_AbsoluteTime);
	}

	static void AddTimestamp(FRDGBuilder& GraphBuilder, const FRenderQueryRHIRef& Query)
	{
		// No parameters: never culled, in order with the march pass.
		GraphBuilder.AddPass(RDG_EVENT_NAME("AtmosphereMarchSweep"), ERDGPassFlags::None,
			[Query](FRHICommandListImmediate& RHICmdList)
			{
				RHICmdList.EndRenderQuery(Query);
			});
	}

	/** Behind State().Lock: the spans that have come back, to Done. */
	static void Poll()
	{
		FState& S = State();
		const uint32 Now = GFrameNumberRenderThread;

		for (int32 i = S.Pending.Num() - 1; i >= 0; i--)
		{
			FSpan& Span = *S.Pending[i];

			if (!Span.EndQuery.IsValid() || Now < Span.Frame + ReadAfterFrames)
			{
				if (Now > Span.Frame + DropAfterFrames)
				{
					S.Pending.RemoveAtSwap(i, EAllowShrinking::No);
				}
				continue;
			}

			uint64 BeginUs = 0;
			uint64 EndUs = 0;
			const bool bBegin = RHIGetRenderQueryResult(Span.BeginQuery.GetReference(), BeginUs, false);
			const bool bEnd = bBegin && RHIGetRenderQueryResult(Span.EndQuery.GetReference(), EndUs, false);

			if (!bEnd)
			{
				if (Now > Span.Frame + DropAfterFrames)
				{
					S.Pending.RemoveAtSwap(i, EAllowShrinking::No);
				}
				continue;
			}

			// RQT_AbsoluteTime reads in microseconds. Summed into the span's frame.
			FSample& Sum = S.Sums.FindOrAdd(Span.Frame);
			if (Sum.Passes > 0 && Sum.Ablate != Span.Ablate)
			{
				Sum.bMixed = true;
			}
			Sum.Frame = Span.Frame;
			Sum.Ablate = Span.Ablate;
			Sum.Ms += EndUs > BeginUs ? (float)((double)(EndUs - BeginUs) * 0.001) : 0.0f;
			Sum.Passes++;

			S.Free.Add(MoveTemp(Span.BeginQuery));
			S.Free.Add(MoveTemp(Span.EndQuery));
			S.Pending.RemoveAtSwap(i, EAllowShrinking::No);
		}

		// A frame is whole once it is old enough for no new span and none of its own is pending.
		TArray<uint32> Ready;
		for (const TPair<uint32, FSample>& It : S.Sums)
		{
			if (Now < It.Key + ReadAfterFrames + 1)
			{
				continue;
			}
			bool bWaiting = false;
			for (const TSharedPtr<FSpan>& P : S.Pending)
			{
				if (P->Frame == It.Key)
				{
					bWaiting = true;
					break;
				}
			}
			if (!bWaiting)
			{
				Ready.Add(It.Key);
			}
		}

		if (Ready.Num() > 0)
		{
			FScopeLock DoneLock(&S.DoneLock);
			for (const uint32 F : Ready)
			{
				const FSample Sum = S.Sums.FindAndRemoveChecked(F);
				if (!Sum.bMixed)
				{
					S.Done.Add(Sum);
				}
			}
			if (S.Done.Num() > 4096)
			{
				S.Done.RemoveAt(0, S.Done.Num() - 4096);
			}
		}
	}

	static void Drain(TArray<FSample>& Out)
	{
		FState& S = State();
		FScopeLock DoneLock(&S.DoneLock);
		Out.Append(S.Done);
		S.Done.Reset();
	}

	// -- The sweep (game thread) -----------------------------------------------

	struct FConfig
	{
		uint32 Ablate;
		const TCHAR* Name;
	};

	// P1f: the fetches' costs added rather than taken away (256, 512, 1024: the field and the
	// rays as they are), and the cascade walk's A/B (2048). Taking a noise layer away (1, 2)
	// changes the field, and with it how far the rays go: those two read as upper bounds.
	// 4096 switches nothing: whether a nonzero value alone moves the time (it should not).
	static const FConfig Configs[] = {
		{ 0u, TEXT("the march as it is") },
		{ 4096u, TEXT("nothing (a bit no switch reads: the control)") },
		{ 2048u, TEXT("cloud shadow read: every cascade (R1a's walk)") },
		{ 4u, TEXT("no cloud shadow read") },
		{ 256u, TEXT("structure fetches twice (+ their cost)") },
		{ 512u, TEXT("detail fetches twice (+ their cost)") },
		{ 1024u, TEXT("flow column read twice (+ its cost)") },
		{ 16u, TEXT("no entry refine") },
		{ 32u, TEXT("half the cloud steps") },
		{ 128u, TEXT("half the air steps") },
		{ 64u, TEXT("no shadow on geometry") },
		{ 1u, TEXT("no detail fetches (the field changes)") },
		{ 2u, TEXT("no structure fetches (the field changes)") },
		{ 0u, TEXT("the march as it is, again (drift)") },
	};
	constexpr int32 NumConfigs = UE_ARRAY_COUNT(Configs);

	/** Frames after a switch before samples count (the cvar reaches the render thread a frame
	 *  later, the GPU a few after that). */
	constexpr int32 WarmupFrames = 10;

	struct FSweep
	{
		bool bActive = false;
		int32 FramesPerSegment = 60;
		int32 Rounds = 3;
		int32 Round = 0;
		int32 Config = 0;
		int32 Frame = 0;
		int32 SavedAblate = 0;
		/** Other plugins' cvars held while the sweep runs (name, value before). */
		TArray<TPair<FString, FString>> Held;
		FTSTicker::FDelegateHandle Ticker;
		/** R2 (J5): the sweep ends with the world it measures (PIE stop, a map change). */
		FDelegateHandle WorldCleanup;
		/** Per config, every round's median, the passes a frame and the GPU frame's median. */
		TArray<float> Medians[NumConfigs];
		TArray<float> Passes[NumConfigs];
		TArray<float> FrameMs[NumConfigs];
		TArray<float> Segment;
		TArray<float> SegmentFrame;
		int32 SegmentPasses = 0;
	};

	static FSweep GSweep;

	static float MedianOf(TArray<float> Values)
	{
		if (Values.Num() == 0)
		{
			return 0.0f;
		}
		Values.Sort();
		return Values[Values.Num() / 2];
	}

	static void SetAblate(int32 Value)
	{
		if (IConsoleVariable* Var = CVarAtmosphereMarchAblate.AsVariable())
		{
			Var->Set(Value, ECVF_SetByConsole);
		}
	}

	/** Async compute the march overlaps, taken out of the timing while the sweep runs: the
	 *  terrain's brick mesher (VoxelPlugin), when it's loaded. Its batches run on the compute
	 *  queue beside the march and share the GPU with it, so the march's span would carry them. */
	static const TCHAR* const HeldVariables[] = {
		TEXT("voxel.Render.BrickMeshBatches"),
		TEXT("voxel.Render.BrickMeshBatchesBusy"),
	};

	static void HoldOthers()
	{
		for (const TCHAR* Name : HeldVariables)
		{
			if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
			{
				GSweep.Held.Emplace(Name, Var->GetString());
				Var->Set(TEXT("0"), ECVF_SetByConsole);
			}
		}
	}

	static void ReleaseOthers()
	{
		for (const TPair<FString, FString>& It : GSweep.Held)
		{
			if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(*It.Key))
			{
				Var->Set(*It.Value, ECVF_SetByConsole);
			}
		}
		GSweep.Held.Reset();
	}

	static void Finish(bool bComplete)
	{
		SetAblate(GSweep.SavedAblate);
		ReleaseOthers();
		GTiming.store(false);
		// (Called from Tick, which returns false: the ticker goes with that.)
		GSweep.Ticker.Reset();
		FWorldDelegates::OnWorldCleanup.Remove(GSweep.WorldCleanup);
		GSweep.WorldCleanup.Reset();
		GSweep.bActive = false;

		if (!bComplete)
		{
			UE_LOG(LogCloudAtmosphere, Warning, TEXT("CloudAtmosphere.MarchSweep: stopped (no march timings came back: is an atmosphere on screen?)."));
			return;
		}

		const float Base = MedianOf(GSweep.Medians[0]);
		FString Table = FString::Printf(
			TEXT("CloudAtmosphere.MarchSweep: the march's GPU time, median of %d rounds x %d frames (the camera should be still).\n")
			TEXT("    bits  march   vs base GPU frame  passes/frame  what is changed (minus: faster)\n"),
			GSweep.Rounds, GSweep.FramesPerSegment);

		for (int32 c = 0; c < NumConfigs; c++)
		{
			const float Ms = MedianOf(GSweep.Medians[c]);
			FString Rounds;
			for (const float R : GSweep.Medians[c])
			{
				Rounds += FString::Printf(TEXT(" %.2f"), R);
			}
			Table += FString::Printf(TEXT("    %4u  %6.2f  %+6.2f  %6.2f     %4.1f          %s (rounds:%s)\n"),
				Configs[c].Ablate, Ms, c == 0 ? 0.0f : Ms - Base, MedianOf(GSweep.FrameMs[c]), MedianOf(GSweep.Passes[c]),
				Configs[c].Name, *Rounds);
		}

		UE_LOG(LogCloudAtmosphere, Log, TEXT("%s"), *Table);
	}

	static bool Tick(float)
	{
		if (!GSweep.bActive)
		{
			return false;
		}

		TArray<FSample> Samples;
		Drain(Samples);

		const uint32 Want = Configs[GSweep.Config].Ablate;
		for (const FSample& S : Samples)
		{
			if (S.Ablate == Want && GSweep.Frame >= WarmupFrames)
			{
				GSweep.Segment.Add(S.Ms);
				GSweep.SegmentPasses += S.Passes;
			}
		}

		// The whole GPU frame alongside (the stat unit figure), for context.
		if (GSweep.Frame >= WarmupFrames)
		{
			const float FrameMs = (float)FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
			if (FrameMs > 0.0f)
			{
				GSweep.SegmentFrame.Add(FrameMs);
			}
		}

		GSweep.Frame++;

		// A segment ends with its frames counted, or after four times as many with too few
		// samples (no march on screen).
		const bool bFull = GSweep.Segment.Num() >= GSweep.FramesPerSegment;
		const bool bStuck = GSweep.Frame > WarmupFrames + 4 * GSweep.FramesPerSegment;

		if (!bFull && !bStuck)
		{
			return true;
		}

		if (bStuck && GSweep.Segment.Num() == 0)
		{
			Finish(false);
			return false;
		}

		GSweep.Medians[GSweep.Config].Add(MedianOf(GSweep.Segment));
		GSweep.Passes[GSweep.Config].Add((float)GSweep.SegmentPasses / (float)FMath::Max(GSweep.Segment.Num(), 1));
		GSweep.FrameMs[GSweep.Config].Add(MedianOf(GSweep.SegmentFrame));
		GSweep.Segment.Reset();
		GSweep.SegmentFrame.Reset();
		GSweep.SegmentPasses = 0;
		GSweep.Frame = 0;

		if (++GSweep.Config >= NumConfigs)
		{
			GSweep.Config = 0;
			if (++GSweep.Round >= GSweep.Rounds)
			{
				Finish(true);
				return false;
			}
		}

		SetAblate((int32)Configs[GSweep.Config].Ablate);
		return true;
	}

	static void Start(const TArray<FString>& Args)
	{
		if (GSweep.bActive)
		{
			UE_LOG(LogCloudAtmosphere, Warning, TEXT("CloudAtmosphere.MarchSweep: already running."));
			return;
		}

		GSweep = FSweep();
		GSweep.bActive = true;
		GSweep.FramesPerSegment = Args.Num() > 0 ? FMath::Clamp(FCString::Atoi(*Args[0]), 10, 1000) : 60;
		GSweep.Rounds = Args.Num() > 1 ? FMath::Clamp(FCString::Atoi(*Args[1]), 1, 10) : 3;
		GSweep.SavedAblate = CVarAtmosphereMarchAblate.GetValueOnGameThread();

		SetAblate((int32)Configs[0].Ablate);
		HoldOthers();
		GTiming.store(true);

		TArray<FSample> Stale;
		Drain(Stale);

		GSweep.Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
		// R2 (J5): a world torn down mid-sweep ends it at once, so the next world's terrain isn't
		// held unmeshed until the stuck timeout.
		// (Game and editor worlds only: an asset editor's preview world closing doesn't end it.)
		GSweep.WorldCleanup = FWorldDelegates::OnWorldCleanup.AddLambda([](UWorld* CleanedWorld, bool, bool)
			{
				if (!GSweep.bActive) return;
				if (CleanedWorld && !CleanedWorld->IsGameWorld() && CleanedWorld->WorldType != EWorldType::Editor) return;
				UE_LOG(LogCloudAtmosphere, Log, TEXT("CloudAtmosphere.MarchSweep: the world was torn down; sweep ended."));
				FTSTicker::GetCoreTicker().RemoveTicker(GSweep.Ticker);
				Finish(false);
			});

		UE_LOG(LogCloudAtmosphere, Log, TEXT("CloudAtmosphere.MarchSweep: %d configurations x %d frames x %d rounds; hold the camera still.%s"),
			NumConfigs, GSweep.FramesPerSegment, GSweep.Rounds,
			GSweep.Held.Num() > 0 ? TEXT(" The terrain's mesher is held for it (voxel.Render.BrickMeshBatches 0)") : TEXT(""));
	}
}

static FAutoConsoleCommand GAtmosphereMarchSweepCmd(
	TEXT("CloudAtmosphere.MarchSweep"),
	TEXT("Stage P1e: times the atmosphere march with each part switched off in turn (r.CloudAtmosphere.March.Ablate) and logs a table. ")
	TEXT("Args: [frames per segment = 60] [rounds = 3]. Hold the camera still."),
	FConsoleCommandWithArgsDelegate::CreateStatic(&AtmosphereMarchSweepDetail::Start));

namespace AtmosphereMarchSweep
{
	uint32 GetAblate_RenderThread()
	{
		return (uint32)FMath::Max(CVarAtmosphereMarchAblate.GetValueOnRenderThread(), 0);
	}

	TSharedPtr<FSpan> Begin(FRDGBuilder& GraphBuilder, uint32 Ablate)
	{
		using namespace AtmosphereMarchSweepDetail;

		if (!GTiming.load(std::memory_order_relaxed))
		{
			return nullptr;
		}

		FScopeLock Lock(&State().Lock);
		Poll();

		TSharedPtr<FSpan> Span = MakeShared<FSpan>();
		Span->Frame = GFrameNumberRenderThread;
		Span->Ablate = Ablate;
		Span->BeginQuery = Allocate();

		if (!Span->BeginQuery.IsValid())
		{
			return nullptr;
		}

		AddTimestamp(GraphBuilder, Span->BeginQuery);
		State().Pending.Add(Span);
		return Span;
	}

	void End(FRDGBuilder& GraphBuilder, const TSharedPtr<FSpan>& Span)
	{
		using namespace AtmosphereMarchSweepDetail;

		if (!Span.IsValid() || Span->EndQuery.IsValid())
		{
			return;
		}

		FScopeLock Lock(&State().Lock);
		FRenderQueryRHIRef Query = Allocate();

		if (!Query.IsValid())
		{
			return;
		}

		AddTimestamp(GraphBuilder, Query);
		Span->EndQuery = MoveTemp(Query);
	}
}
