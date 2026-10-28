#include "AtmosphereMarchSweep.h"

#include "CloudAtmosphere.h"
#include "Containers/Ticker.h"
#include "DynamicRHI.h"
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

	struct FSample
	{
		uint32 Ablate = 0;
		float Ms = 0.0f;
	};

	// Never destructed: nothing of the RHI's is released after the RHI at exit.
	struct FState
	{
		FCriticalSection Lock;
		TArray<TSharedPtr<FSpan>> Pending;
		TArray<FRenderQueryRHIRef> Free;
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

			// RQT_AbsoluteTime reads in microseconds.
			FSample Sample;
			Sample.Ablate = Span.Ablate;
			Sample.Ms = EndUs > BeginUs ? (float)((double)(EndUs - BeginUs) * 0.001) : 0.0f;

			{
				FScopeLock DoneLock(&S.DoneLock);
				S.Done.Add(Sample);
				if (S.Done.Num() > 4096)
				{
					S.Done.RemoveAt(0, S.Done.Num() - 4096);
				}
			}

			S.Free.Add(MoveTemp(Span.BeginQuery));
			S.Free.Add(MoveTemp(Span.EndQuery));
			S.Pending.RemoveAtSwap(i, EAllowShrinking::No);
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

	static const FConfig Configs[] = {
		{ 0u, TEXT("the march as it is") },
		{ 1u, TEXT("no detail fetches") },
		{ 2u, TEXT("no structure fetches") },
		{ 4u, TEXT("no cloud shadow read") },
		{ 8u, TEXT("no air light-tint read") },
		{ 16u, TEXT("no entry refine") },
		{ 32u, TEXT("half the cloud steps") },
		{ 64u, TEXT("no shadow on geometry") },
		{ 128u, TEXT("half the air steps") },
		{ 1u | 2u | 4u | 8u, TEXT("no noise, shadow or tint fetches") },
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
		FTSTicker::FDelegateHandle Ticker;
		/** Per config, every round's median. */
		TArray<float> Medians[NumConfigs];
		TArray<float> Segment;
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

	static void Finish(bool bComplete)
	{
		SetAblate(GSweep.SavedAblate);
		GTiming.store(false);
		// (Called from Tick, which returns false: the ticker goes with that.)
		GSweep.Ticker.Reset();
		GSweep.bActive = false;

		if (!bComplete)
		{
			UE_LOG(LogCloudAtmosphere, Warning, TEXT("CloudAtmosphere.MarchSweep: stopped (no march timings came back: is an atmosphere on screen?)."));
			return;
		}

		const float Base = MedianOf(GSweep.Medians[0]);
		FString Table = FString::Printf(
			TEXT("CloudAtmosphere.MarchSweep: the march's GPU time, median of %d rounds x %d frames (the camera should be still).\n")
			TEXT("    bits  ms      saved   what is off\n"),
			GSweep.Rounds, GSweep.FramesPerSegment);

		for (int32 c = 0; c < NumConfigs; c++)
		{
			const float Ms = MedianOf(GSweep.Medians[c]);
			FString Rounds;
			for (const float R : GSweep.Medians[c])
			{
				Rounds += FString::Printf(TEXT(" %.2f"), R);
			}
			Table += FString::Printf(TEXT("    %4u  %6.2f  %6.2f  %s (rounds:%s)\n"),
				Configs[c].Ablate, Ms, c == 0 ? 0.0f : Base - Ms, Configs[c].Name, *Rounds);
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
		GSweep.Segment.Reset();
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
		GTiming.store(true);

		TArray<FSample> Stale;
		Drain(Stale);

		GSweep.Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));

		UE_LOG(LogCloudAtmosphere, Log, TEXT("CloudAtmosphere.MarchSweep: %d configurations x %d frames x %d rounds; hold the camera still."),
			NumConfigs, GSweep.FramesPerSegment, GSweep.Rounds);
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
