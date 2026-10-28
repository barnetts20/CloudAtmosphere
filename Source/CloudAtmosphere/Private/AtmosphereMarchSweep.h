#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FRDGBuilder;

/**
 * Stage P1e (catalogue F3, measurement only): what the atmosphere march's time is spent on.
 *
 * The march is one dispatch (6.6 ms at Max inside a cloud layer, 6.7-10 ms in fast flight), and
 * ProfileGPU can't see inside it. r.CloudAtmosphere.March.Ablate switches parts of it off, bit by
 * bit (AtmosphereMarchPass.usf: MarchAblate); CloudAtmosphere.MarchSweep steps through them with
 * the camera held still and logs the march's own GPU time for each (a pair of timestamps around the
 * march pass, read back a few frames later), so the costs come out of one command.
 *
 * Ablate 0 (the default) is the march as it was: every switch is a uniform branch that takes the
 * normal path. The sweep restores the cvar when it ends.
 */
namespace AtmosphereMarchSweep
{
    /** Render thread: the ablation bits the march runs with this frame. */
    uint32 GetAblate_RenderThread();

    /** One timed march pass (render thread). */
    struct FSpan;

    /** Graph setup, render thread: a timestamp before the march pass (null unless a sweep runs). */
    TSharedPtr<FSpan> Begin(FRDGBuilder& GraphBuilder, uint32 Ablate);

    /** Graph setup, render thread: the timestamp after it. */
    void End(FRDGBuilder& GraphBuilder, const TSharedPtr<FSpan>& Span);
}
