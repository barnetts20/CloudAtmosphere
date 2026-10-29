# Stage R2a: the final review's fixes

R2's fixes, in two zips, both cumulative from R1a (d74c5b4), P1 included:
- **`StageR2a.zip`:** VoxelPlugin + VoxelPlanets;
- **`CloudAtmosphere_R2a.zip`:** CloudAtmosphere.

`Design/R2_Review.md` has every finding and its status, and what blocks roadmap items 7 and 8.

## Look changes: please sign off
1. **K1, perpetual storms:** they moved about 5×10⁶ times too fast (`PerpetualBackground` never added up its weights), so they averaged into a smear. They now drift as designed. Look at a gas giant or a perpetual-storm planet.
2. **J4, the atmosphere's defaults:** a freshly placed atmosphere ran values that weren't its displayed preset (shadow maps at 1024², 16× any preset's texels). It now applies its preset's values in the constructor. Levels saved before this load exactly as they were (a saved-data version puts the old defaults back for them); a newly placed actor, or one re-saved after picking a preset, runs the preset. To move an old level onto its preset, re-pick it.
3. **G2, the far sphere and inland basins:** from altitude, every basin below sea level drew as open sea; the far sphere now culls them like the near rings. Look from orbit at a planet with inland basins (dry lowlands below sea level).

## The drain bug (the user's report)
**Fixed.** After a reseed, the new shore field's first face sampled the previous seed's terrain, which drained or culled open sea on that face for as long as the seed stayed. The old seed's field also drove the drain for the 1.5–2 s the new one took. Details in `Design/R2_Review.md`.
- **To confirm:** reseed several times with `voxel.Debug.OceanShore 1`, and look across the six faces from low orbit, then fly down to a coast. No face should show basins over open sea.
- **Expected change after a reseed:** the sea is open water everywhere (no drain, no basins) for the 1.5–2 s until the new field lands, as on a first spawn. A rebuild on the same seed (the climate's layers, say) keeps the old field shown until the new one lands, as before.

## What else changed
**VoxelPlugin:**
- **P1c's batch rate (A1, A7):** a busy commit now meshes at the busy cap when the GPU has headroom, and stays at the floor when it hasn't; the rate's state is reset with the pages; no banked credit.
- **Morph slack (A2):** with several terrains in a world, the morph collection takes the largest slack of the frame instead of the last terrain's.
- **Hidden terrains (A3):** cache ranges time out by frames the terrain is shown and drawn, so a hidden or parked terrain no longer frees ranges its pending copies still use.
- **Reset (A8):** the pages' render-thread inputs are released.
- Comments and cvar help (A4, A5, A6).

**VoxelPlanets, the land:**
- **Crash fix (B1/D1):** a terrestrial density with an ice shelf and no climate field dereferenced null.
- **The planet's build (B2, B3, B4, B6, B7, B9):** an empty ice shelf is baked once; the compositor is let go on a switch to Noise; the bake's GPU fields aren't released every tick while waiting, but once more when the terrain moves to the shelf's bake; the timeline's coast mark waits for the new shore field; the clamp warning logs once.
- **The noise registry (C1, C2, C9):** the exit hook is registered at module startup; packs made and not yet taken are kept up to 8, and for 120 s (pruned on every store, take and prewarm); packs are destroyed outside the lock.
- **The bake (C3, C4, C5, C6):** a river re-record uploads its fields; the erosion cvar is read once per baker; interrupted saves' temp files are deleted after 10 minutes; shader ternaries that read buffers became if/else.
- **The density (D3, D5):** the lattice-range check on the peaks', hills' and strata's detail.
- **The land cache (E3, E5, G4):** a floor on `voxel.Land.CacheTexelsPerFrame`; a failed dispatch invalidates the plan; an init serial the ocean keys on.
- `DetailSlopeDamping` is no longer editable (B10); comments.

**VoxelPlanets, the ocean:**
- the drain fixes (above), and the CPU shore build (`voxel.Ocean.ShoreFieldOnCpu 1`) now samples the terrain's heights too (H4); the fallback waits 30 s of ticks, not 1,800 frames (H5);
- pointer keys became weak pointers (F3, G3), with the basin margin and the density version in the probe's key;
- a weather key pins one map (F4); the perf sweep restores the user's cvars (F6);
- the rain rings and whitecap churn don't re-roll at the clock's hourly wrap (I1); the chart blend's `smoothstep` has its edges in order (I2, identical on D3D);
- the far sphere culls basins (G2, above); comments (G5, H7, I4).

**CloudAtmosphere:**
- **The shadow bake (J1, J2):** a still field bakes nothing again (the light-turn test and a non-spinning planet's key); a spread bake restarts on a field jump or a sim reset.
- **J3:** the climate and weather readbacks run even while the march can't draw, so a planet waiting on a climate snapshot no longer stalls.
- **J5:** a `MarchSweep` cut short by a game or editor world's teardown releases the mesher.
- **The flow sim (K1, K2, K3, K4):** the perpetual storms (above); a deferred restart invalidates the climate window; `FlowSim.Save` refuses a transient target; `PerpetualTime` is set.
- **L1:** a sky ray near the view centre no longer reads as a hit.
- Comments (K6, L2, L3).

## Please delete by hand
A zip can't delete files. Nothing includes these:
- `VoxelPlugin/Shaders/Private/VoxelLandBelts.ush`
- `VoxelPlugin/Shaders/Private/VoxelLandSeaIce.ush`

## Deferred (reasons in `Design/R2_Review.md`)
A5, B8, B12, C2's prewarm workers blocking pool threads (item 8), C4 (shader hash; the settings check's live cvar read), D2 (levees, a look change), E1/E2 and A6 (edits replay, for item 7), F7, I3, I5, J5's Console-priority restore, J6, K5.

## Checks run
- The harness runner: 58 pass, 0 fail, 9 skipped; outputs identical to P1j's.
- The shader check (dxc SM 6.6, HLSL 2018 and 2021, glslang) over every .usf and the material code.
- An independent second review of every fix.
- **Not run:** an engine build. Please build and play: a reseed or two, flying down to a coast; an old level with an atmosphere (it should look as before); a gas giant's storms.
