# Stage P1f: the cloud shadow read reads only the cascades that reach its result (F3, part 1)

**P1f2 (2026-10-28, 14:20): the sweep, made trustworthy.**

*Why:* P1f's sweep (inside the cloud layer) gave the march as 3.77 ms, against 7.33 ms with R1a's cascade walk (bit 2048). But every setting that *removes* work ran slower than the baseline, by +1.3 to +2.1 ms:

| Setting | Bits |
|---|---|
| no shadow read | 4 |
| no refine | 16 |
| half the steps | 32, 128 |
| no surface shadow | 64 |
| no noise layer | 1, 2 |

None of those can cost time. So the timing itself carried something besides the march.

*Two likely causes, both now taken out:*
1. **The terrain's brick mesher.** It runs on the async compute queue beside the march (P1c: its batch rate follows the GPU frame time, so it varies with the setting). The sweep was run 16 s after a spawn, with the coastal fields and the first fill still busy. Its batches share the GPU with the march, and the march's span carries them. The sweep now holds `voxel.Render.BrickMeshBatches` and `...Busy` at 0 while it runs, and restores them after.
2. **More than one march pass a frame** (an editor can render several views). Samples were per pass, so the median of a mix of views depended on the mix. Spans are now summed per frame, and a frame whose passes ran with different bits is dropped.

*Controls added:*
- bit **4096**, which switches nothing: a nonzero value alone shouldn't move the time;
- the baseline again at the end of each round, to show drift.

*The table now also shows* the whole GPU frame (the `stat unit` figure) and the passes per frame.

*What stands:* bit 2048 against bit 4 says the selective walk works. R1a's walk costs ~2.3 ms more than no read at all, and the new walk costs less than no read in that run; that last can't be right, so it is the sweep's problem above. P1f2's sweep gives the real figure.

---

The first F3 change, from P1e's measurement, plus a finer second measurement. Only CloudAtmosphere changes: `CloudAtmosphere_P1f.zip` is cumulative from R1a (P1a through P1e included). VoxelPlugin and VoxelPlanets stay at `StageP1d.zip`.

## What P1e measured
`CloudAtmosphere.MarchSweep`, inside a dense cloud layer, at Max. Medians of 3 rounds × 60 frames; the rounds spread by about ±0.8 ms.

| Bits | March (ms) | Saved | Off |
|---|---|---|---|
| 0 | 10.75 | – | nothing |
| 1 | 9.50 | 1.25 | the detail fetches |
| 2 | 4.85 | 5.91 | the structure fetches |
| 4 | 7.93 | **2.82** | the cloud shadow read |
| 8 | 10.81 | 0 | the air samples' light-tint read |
| 16 | 8.76 | 1.99 | the entry refine |
| 32 | 8.72 | 2.04 | half the cloud steps |
| 64 | 9.87 | 0.89 | the shadow on geometry |
| 128 | 9.53 | 1.22 | half the air steps |
| 1+2+4+8 | 2.42 | 8.33 | every noise, shadow and tint fetch |

**How to read it:**
- **The march is fetch-bound, not step-bound.** Halving the cloud steps saves 19%. Taking the fetches away leaves 2.4 ms of the 10.75.
- **Bits 1 and 2 change the field.** Without a noise layer the cloud is denser and more uniform, rays go opaque sooner, and fewer steps run. Their savings mix the fetch cost with the rays' length, so they are upper bounds. P1f measures the fetches by adding them instead (below).
- **Bit 4 changes nothing about the rays.** The light only sets what a sample receives. So **2.8 ms is the shadow read's own cost.**

## The change: the cascade walk reads only what reaches the result
`AtmoShadow_ReadTau` reads the map at every cloud and air sample. It read level 0, then each finer level that contains the point, and blended each over the one before by `smoothstep(ATMO_SHADOW_NEAR_BLEND, 1, Edge)`. Each level is a 4-tap slice with 4 node reconstructions, so the cost is up to 12 taps a sample.

**A level whose weight is exactly 0** (its edge measure at or inside `ATMO_SHADOW_NEAR_BLEND`, 0.75) replaces everything coarser: `lerp(Finer, Tau, 0)` is `Finer`, whatever `Tau` was. So the walk now:
- works out each level's coordinates and weight first (arithmetic, no fetch);
- starts at the finest level that wholly replaces the coarser ones;
- reads only that level and the finer ones after it, in the same order with the same operands.

**The result is the same, bit for bit.** Most of the view is inside level 2's inner three quarters (0.3 planet radii around the camera), so most samples now take one slice instead of three. That is the established cascade-selection rule of cascaded shadow maps: read the cascade that covers the point, and blend only in its border band.

- **Expected:** the 2.8 ms read falls by up to two thirds, so about −1.5 to −2 ms in this view. Less at the edge of the inner cascades.
- **Also faster:** the surface shadow (`AtmoShadow_SurfaceOptDepth`) and the ocean's surface-sun pass go through the same function.
- **R1a's walk** is kept as `AtmoShadow_ReadTauAllLevels`, behind bit 2048 of `r.CloudAtmosphere.March.Ablate` for the A/B.

## The second measurement
`r.CloudAtmosphere.March.Ablate` gains four bits:

| Bit | What it does |
|---|---|
| 256 | the structure fetches taken twice |
| 512 | the detail fetches taken twice |
| 1024 | the flow column read twice |
| 2048 | R1a's full cascade walk (the A/B) |

For 256, 512 and 1024 the second fetch is taken at a 3% higher frequency (other texels, the same locality) and scaled by 1e-20, so the field, the rays and the image are unchanged. **The added time is each fetch's own cost.** Taking a layer away could not measure that, because it changes the rays.

The sweep's list now runs:

| Bits | Setting |
|---|---|
| 0 | the march as it is |
| 2048, 4 | the shadow read: R1a's walk, and none |
| 256, 512, 1024 | the fetches twice |
| 16, 32, 128, 64 | the refine, the steps, the shadow on geometry |
| 1, 2 | the noise layers away (upper bounds) |

The table's column is now "vs base": minus is faster, plus slower.

## Files
- `Shaders/Private/AtmosphereShadowMap.ush`: `AtmoShadow_ReadTau` (the selective walk), `AtmoShadow_ReadTauAllLevels` (R1a's), and the `ATMO_ABLATE` fallback.
- `Shaders/Private/TerrestrialDeck.ush`: bits 256, 512 and 1024.
- `Source/CloudAtmosphere/Private/AtmosphereMarchSweep.cpp`: the cvar's help, the sweep's list, and the table's sign.

## Checked
- **DXC SM 6.6, HLSL 2018 and 2021:**
  - the march, reflect, surface-sun and sky-table entries, in all three models;
  - the bake in all four permutations (deep deck × quad).

  All compile clean.
- **Exactness, by construction:** a weight of exactly 0 discards the coarser levels in `lerp`. The slices read, their order and their operands are the original's.
- Not checked here (no toolchain): the C++ build.

## The capture to take with it
1. **The same view** (inside the cloud layer, camera still): `CloudAtmosphere.MarchSweep`. The rows to read:
   - 0 against 2048: what P1f saved;
   - 0 against 4: what the read still costs;
   - 256, 512 and 1024: what each fetch costs.

   The table goes to the log, which I read.
2. **The look:** nothing should change. A flick between `r.CloudAtmosphere.March.Ablate 0` and `2048` should show no difference at all.
