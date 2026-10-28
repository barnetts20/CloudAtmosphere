# Stage P1e: profiling the atmosphere march (F3, measurement only)

This is F3's first step (`Design/Performance.md`): measure before changing. **It changes nothing in what is drawn** while `r.CloudAtmosphere.March.Ablate` is 0, which is the default. `CloudAtmosphere_P1e.zip` holds full files, CRLF, and is cumulative from R1a (it includes P1a, P1b and P1b2). VoxelPlugin and VoxelPlanets stay at `StageP1d.zip`.

## Why
- **The cost:** the march is one dispatch, `AtmosphereMarch 665x438`. It took 6.6 ms in a `ProfileGPU` taken inside a dense cloud layer, and 6.7–10 ms in fast, low flight. After F1 and F2 it is by far the largest single GPU cost.
- **The blind spot:** `ProfileGPU` stops at the dispatch. Inside it, each cloud step reads:
  - the flow column (4 atlas slices, up to 3 face slots);
  - the structure and detail volumes, twice each (the two flow-noise phases);
  - the cloud shadow map (up to 3 cascades);
  - the transmittance table.

  Each air step also reads the shadow map, and the flow column again for its light tint under cloud. On top of that the step lattice, the entry refine and the surface shadow all cost something. Without a shader profiler, the way to split those costs is to take each part out and time what is left. This is ablation, the usual method in that case.

## What it adds
**`r.CloudAtmosphere.March.Ablate`**, a bitmask that switches parts of the march off. Each switch is a uniform branch (`ATMO_ABLATE` over the `MarchAblate` uniform), only in the view's march pass:

| Bit | What is off |
|---|---|
| 1 | the detail volume's fetches (its mean instead) |
| 2 | the structure volume's fetches (its mean instead) |
| 4 | the cloud shadow map's read (no deck shadow) |
| 8 | the air samples' light-tint column read (neutral tint) |
| 16 | the entry refine (no substeps where a ray enters cloud) |
| 32 | half the cloud steps |
| 64 | the cloud shadow on geometry (the surface multiply) |
| 128 | half the air steps |

**`CloudAtmosphere.MarchSweep [frames=60] [rounds=3]`** steps through the bits above one at a time, then all four fetch bits together (1|2|4|8). For each setting it times the march pass's own GPU time: a pair of `RQT_AbsoluteTime` timestamps around the pass, read back a few frames later. That's the ocean's `voxel.Ocean.PerfSweep` technique. It logs one table: the median per setting across the rounds, the time saved against bit 0, and each round's median (to show the noise). It restores the cvar when it ends. The image changes while it runs; that's expected.

**Untouched:**
- the bake (`TerrestrialShadowMap.usf` compiles the switches out: `ATMO_ABLATE` is `false` there);
- the surface-sun, sky-table and reflection passes (`MarchAblate` is 0 for them).

## Files
- **New:** `Private/AtmosphereMarchSweep.h`, `Private/AtmosphereMarchSweep.cpp`. They hold the cvar, the timestamps and the sweep command.
- `Private/AtmosphereViewExtension.cpp`:
  - `MarchAblate` in the common parameters (0 by default);
  - the march pass sets it from the cvar;
  - the march pass is bracketed by the sweep's timestamps.
- `Shaders/Private/AtmosphereMarchPass.usf`:
  - `uint MarchAblate`;
  - `ATMO_ABLATE`;
  - the half-step switches.
- `Shaders/Private/AtmosphereMarch.ush`: the shadow read, the air tint, the refine and the surface-shadow switches.
- `Shaders/Private/TerrestrialDeck.ush`: the structure and detail switches, and the `ATMO_ABLATE` fallback.

## Checked
- **DXC SM 6.6, HLSL 2018 and 2021:** every entry point of `AtmosphereMarchPass.usf` in all three models (march, reflect, surface sun, sky table, irradiance, surface weather) compiles clean. So do both bake permutations.
- **The stub:** the shader check's engine stub now carries the view members the atmosphere reads. It is in the scratch tooling, not the plugin.
- **Not checked here (no toolchain):** the C++ build. The timestamp code is the ocean's (`VoxelOceanGPUTime.cpp`), which already builds in this project.

## The capture to take with it
1. Put the camera where F3 is to be judged: inside or just under the cloud layer, the view the 6.6 ms `ProfileGPU` was taken in. Hold it still. The march's cost depends on the view, so the camera must not move during the sweep.
2. Run `CloudAtmosphere.MarchSweep`. It takes about a minute: 10 settings × 3 rounds × ~70 frames.
3. Optionally, run it again at orbit and in fast, low flight. Holding still while hovering low is fine.

There's no need to paste anything: the table goes to the editor log, which I read from your disk.
