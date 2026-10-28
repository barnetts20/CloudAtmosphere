# Stage P1b: the cloud shadow bake, spread and only on change (F1)

**P1b2 (2026-10-28, 13:20): the bake's subsamples on quads.**

*Measured with P1b, flying fast and low (`stat gpu`):*
- `CloudAtmosphereShadowBake`: 7.48 ms avg / 12.03 max with `Amortize 0`, against 3.66 / 4.62 with `Amortize 1`.
- Graphics queue total: 27.47 → 22.65 ms avg, 38.37 → 32.63 max.
- `FramesPerLevel` 8 gave ~2.8 ms. The look was signed off.

*The fit:* the three points give about **2.4 ms + 5.1 ms ÷ FramesPerLevel**. `ProfileGPU` shows the level's quarter alone at 2.44 ms; coverage (0.03 ms) and the copies are negligible. The 2.4 ms floor is the latency of the longest rays: up to 256 dependent steps, each a chain of four subsamples' fetches. A quarter dispatch (512 warps, ~7 per SM) can't hide it.

*The change:* each texel's four subsamples now run on a quad of lanes, one subsample a lane. `QuadReadLaneAt` sums them in subsample order, with the same products and the same order as before.
- Every lane of the quad holds the same extinction, so the march's control flow stays quad-uniform. Only lane 0 writes.
- The work is four times the threads, with a quarter of the fetch chain per thread. DXIL per thread: 140 sample calls instead of 452, a third of the code.
- It is a new permutation, `ATMO_BAKE_QUAD`: 1D groups of 64, four groups to an 8×8 tile. It is compiled with `CFLAG_WaveOperations` only where the platform has wave operations, and chosen at run time only where the RHI has them. Otherwise the 8×8 path runs as before.
- `r.CloudAtmosphere.ShadowBake.QuadSubsamples 0` is the A/B.

*Expected:* the floor and the share both shorten. At `FramesPerLevel 4`, roughly 3.7 → 1.5–2 ms; it is not measured yet. The result should match the one-thread path to the last bit, or within float contraction (the same sum of the same products, in the same order). A look check confirms it.

*Compile risk:* this is the project's first use of wave operations. If the 5.7 names differ, look first at `RHISupportsWaveOperations`, `GRHISupportsWaveOperations` (`RHIGlobals.h`) and `CFLAG_WaveOperations`.

*Checked:* DXC SM 6.6, HLSL 2018 and 2021, all four permutations (deep deck × quad); SPIR-V (Vulkan 1.1) for the quad path.

---

The first fix of P1, from `Design/Performance.md` (catalogue item F1). **Only CloudAtmosphere changes**: `CloudAtmosphere_P1b.zip` holds full files, CRLF, cumulative from R1a, P1a's scopes included. VoxelPlugin and VoxelPlanets stay at `StageP1a2.zip`.

## The cost it goes after
At the Max preset, the `CloudAtmosphereShadowBake` row was **4.6–6.9 ms of GPU every frame**. One `Terrestrial.ShadowBake Level N` dispatch alone was ~6.2 ms: 256² texels, each marching up to 256 steps with 4 subsamples of the flow, structure and detail volumes. R1a rebaked one whole level every frame, in turn (each level every third frame), whether or not anything had moved.

## What changes
**The technique** is amortised shadow-map updates, as cloud and sky systems spread theirs over frames. Each level's rebake is spread over `FramesPerLevel` frames (default 4). Each frame bakes every 4th block of 8 rows. Interleaving the blocks keeps every frame's share about equally expensive, because the long chords near the disc's centre are spread across all frames. The rows go into a **staging slice**. The level's live slice is replaced only when the last share is in, by a copy in the same graph. So the march always reads either the whole previous bake or the whole new one, never a level half-baked around two cameras.

**What stays fixed while a bake is spread:**
- The light, the camera and the history weight are fixed when the bake starts. The level's bake is recorded (camera, light, time) only when it completes. So the march reads every level with the frame it was baked in, as before, and the reprojected history blend is unchanged.
- The field's clocks still advance between a bake's shares. Over 4 frames that is a fraction of a texel (the texels are about 8 / 4.7 / 2.3 km).

**On change:** a level is due only if one of these moved since its last bake:
- the field's clocks (the sim's display time or the spin time);
- the light (by more than ~0.02°);
- for an inner level, the camera (by more than one of the level's texels).

When no level is due, the request runs only the coverage pass the march reads, which is one thread group.
- With the clouds moving, that changes nothing: every level stays due, and the saving is the spreading.
- With the sim paused, or a still editor viewport, the bake costs nothing.

**Unchanged:**
- The first bake into a fresh target, model or field still bakes all three levels at once, in place, as before. That is a one-frame spike at spawn and on a field edit; S6 looks at spawn.
- The bake's per-texel march is untouched. With `Amortize 0`, or `FramesPerLevel 1`, the shader computes exactly what it did (stride 1, phase 0 is the identity).

**Memory:** one extra slice, the map's size and format (0.5 MB at 256², 8 MB at 1024²). It is made on the first spread bake.

## Expected saving
At Max, about **6.2 → ~1.6 ms of bake a frame** while clouds move (6.2 / 4, plus a slice copy and the history copy, each well under 0.1 ms). That is **~4.5 ms of GPU a frame**, and ~6 ms when nothing moves.

## The look trade, for your sign-off
**Cadence:**
- R1a refreshed each level every 3 frames (~50 ms at 60 fps).
- P1b refreshes it every 12 frames (~200 ms at 60 fps).

**How it shows:** `ShadowTemporalSmoothing` (0.1 s) still blends each rebake with its reprojected previous one. That blend is per world-second, so its fade is unchanged, but each step is larger.

**What to watch:**
- On ground under moving cloud shadows, at low altitude, whether they visibly *step* about 5 times a second rather than slide.
- Most of all with sped-up sim time.

**If it steps:**
- `r.CloudAtmosphere.ShadowBake.FramesPerLevel 2` halves the cadence (~3 ms saving).
- `1` is R1a's cadence (no GPU saving while clouds move, but still zero when they don't).

## Console variables
| Cvar | Default | What it does |
|---|---|---|
| `r.CloudAtmosphere.ShadowBake.Amortize` | 1 | 0 = R1a's schedule exactly (one whole level every frame, in place), for A/B. |
| `r.CloudAtmosphere.ShadowBake.FramesPerLevel` | 4 | Frames one level's rebake is spread over (1–16). The bake's GPU time per frame divides by about this. |
| `r.CloudAtmosphere.ShadowBake.MinInterval` | 0 | Least world seconds between two rebakes of one level. Frames with no level due bake nothing. A budget knob if slow clouds allow it. |
| `r.CloudAtmosphere.ShadowBake.CameraTexels` | 1 | Camera travel, in a level's texels, that makes an inner level due on a still field. |
| `r.CloudAtmosphere.ShadowBake.QuadSubsamples` | 1 | P1b2: each texel's 4 subsamples on a quad of lanes (wave operations required; falls back otherwise). 0 = one thread per texel, for A/B. |
| `r.CloudAtmosphere.ShadowBake.Log` | 0 | 1 = log each level as its rebake completes, with the seconds since its last one and its history weight. |

## Files
- `Shaders/Private/AtmosphereShadowBake.ush`: `ShadowRowStride`, `ShadowRowPhase`, `ShadowWriteSlice` uniforms.
- `Shaders/Private/AtmosphereShadowBakeLoop.ush`: the texel row from the group row's block (stride and phase); writes to `OutTexel` (staging or the level's slice). The texel's position math is unchanged.
- `Public/TerrestrialShadowMap.h`, `Private/TerrestrialShadowMap.cpp`:
  - `RowStride`/`RowPhase`/`bStaged`/`bResolveStaging`/`StagingResource` on the request, and the matching shader parameters;
  - the staged UAV, the share's group count and the resolving copy;
  - `IsUsable` refuses a staged request without a staging slice or with more than one level.
- `Public/PlanetAtmosphereActor.h`, `Private/PlanetAtmosphereActor.cpp`:
  - the cvars and the schedule in `FillShadowRequest`;
  - `CommitShadowBake(const FTerrestrialShadowParams&)`: records a spread bake's progress, and the level only when it completes;
  - `IsShadowLevelDue`, `PrepareShadowStaging`, `ShadowStaging`, `ShadowSpread`, `ShadowBakedClock`.

## Checked
- **Shader check:** DXC SM 6.6, HLSL 2018 and 2021, `MainShadowBakeCS` and `MainCoverageCS` in both permutations (`TR_DEEP_DECK` 0/1): clean. It uses the engine stubs plus `PI`/`LOOP`/`BRANCH`/`UNROLL`. glslang doesn't honour `#pragma once` on this include graph and fails the same way on the R1a baseline, so it says nothing here.
- **Stride 1, phase 0** maps every thread to the row it had, and `ShadowWriteSlice` −1 writes the level's own slice. So `Amortize 0` is R1a's output exactly.
- **Not checked here (no toolchain):** the C++ build.
  - The likeliest snag is the `FMath::CountTrailingZeros`/`CountBits` return types (cast explicitly).
  - Unity: the new statics are uniquely named.

## The capture to take with it
1. **GPU cost:** at spot B (fast and low over land) and spot D (5–10 km up), `stat gpu` with `r.CloudAtmosphere.ShadowBake.Amortize 0`, then `1`. The `CloudAtmosphereShadowBake` row is the number. Then `ProfileGPU` once with `Amortize 1`: the bake shows as `Terrestrial.ShadowBake Level N (rows k/4)`.
2. **The look:**
   - hover ~100 m over land under broken cloud, and toggle `Amortize 0`/`1` a few times;
   - watch the cloud shadows on the ground, and the clouds' own self-shadowing from orbit;
   - if they step, try `FramesPerLevel 2`.
3. **Optional:** `r.CloudAtmosphere.ShadowBake.Log 1` for a few seconds. Each level should report ~0.2 s between rebakes at 60 fps.
