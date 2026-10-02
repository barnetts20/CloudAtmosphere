# Status

The state of the CloudAtmosphere plugin and how work on it is run. State at 2026-10-01. Open work is in `Design/Todo.md`; what every setting does is in `Design/UsageGuide.md`.

## The plugin

Volumetric atmospheres and clouds for planets in Unreal Engine 5, drawn by compute passes from a scene view extension.

- **Three models** on one actor (`APlanetAtmosphereActor`):
  - **Terrestrial:** a slab of cloud over a surface.
  - **Gas giant:** the same slab over a deep deck that fills to the core, with a deep material and a floor.
  - **Air only:** the terrestrial air with no cloud, for moons and thin-air worlds.
- **One weather sim per world** (`UFlowSimSubsystem`): a multi-layer shallow-water model on a lat-lon grid, with moisture, storms, hurricanes (storm cells) and perpetual storms. The claiming planet nearest the camera drives it with its active model's config (`UFlowSimConfig`), restored from that config's snapshot. Other planets draw the weather they last kept (`Design/SimOwnership.md`).
- **The cloud field** turns the sim's weather into 3D cloud: coverage, type, lift, warp and two noise layers carried by the flow. It is lit through a three-cascade shadow map baked in the light's frame.
- **The march** spreads one sample per pixel cell over frames on a camera lattice. A temporal resolve reprojects the history with the planet's spin.

## Where things stand

**Phase C, the frozen core, is at its close-out.** A frozen core needs no further internal changes, so everything after it happens in a seeded procedural wrapper that only sets parameters. Done:

- Every look comparison and design call is decided, and the losing paths are stripped (decisions below; FlagAudit lists what flags remain and why).
- The core is correct and finite across the ranges a generator draws from. True limits are guarded at the push. Acceptable ranges are slider limits, documented for the harness.
- The three models.
- The final parameter surface:
  - one bundle per model on the actor;
  - the sim config grouped with nothing hidden;
  - a C++ and Blueprint runtime API;
  - tunes and presets that apply at runtime in a cooked build (`UAtmospherePreset`, `UAtmosphereTuneLibrary`);
  - sim config version 11.
- The pre-freeze review (`Design/Review_2026-10-01.md`) is triaged and its fixes have landed.
- Comment volume is within the guideline in every file.
- The usage guide.

Left before the freeze (`Todo.md`, tier 6):

- **Profiles and tunes** (PROF-01 to PROF-03, the user's):
  - a terrestrial and a gas giant sim config, each with its snapshot, as the actor's defaults (retiring `GasGiantSimScratch`);
  - the gas giant baseline;
  - the shipping tune set.
  - API-09, coherent class defaults for a harness's new configs, follows from PROF-01's templates.
- **Docs:**
  - the sim, field and render references (DOCS-02 to DOCS-04);
  - the archive sweep (DOCS-06, PROF-04).

**Phase D, the seeded procedural harness, comes after the freeze.** It is not core. It covers:

- planet classes;
- seeded draws over the planet identity set within the range rules (`Design/HarnessReference.md`), after sweeping its first-estimate ranges;
- the sim's seeds and snapshot provenance (API-06, API-10, SIM-01);
- storm traits drawn per cell, their drivers, perpetual storm variation, the genesis window, channel flips and placement (PROC-01 to 03, 05, 07, 08);
- presets such as Earthlike and Peaceful (STORM-13), and master handles such as a storm amount (STORM-19);
- per-storm shape noise and storm populations (GG-14);
- rain and other game-side effects (REN-12);
- licensing (LIC-01).

The harness writes no shader and changes no struct.

**Phase E, planet integration, follows the harness.** It layers the atmosphere into the planet mesh after the voxel planet engine moves to compute:

- heightmap outputs to the atmosphere;
- a surface or ocean map for the sim, a weather readback for gameplay and audio, and implicit Coriolis if a gas giant needs it (DEC-10);
- many planets in one world (RUN-01 to RUN-09);
- air only as a distance level of detail (MODE-03).

### State worth knowing

- **Actor panel:** CloudAtmosphere comes first after the transform (a details customization registered by the module in editor builds). Its groups are Atmosphere, Model and Pipeline, in declaration order.
  - Model holds `Terrestrial` and `GasGiant`, one `FAtmosphereModelParams` each. Only the active model's bundle is shown.
  - Inside a bundle, groups the model doesn't read are hidden: Deep off the gas giant, and the cloud groups on air only.
  - A preset's `PlanetType` sets the same flags.
- **Sim config panel:** every handle is shown, grouped under Planet (Winds, Flow Rates, Stirring, Moisture, Storms, Hurricanes, Hurricane Dynamics, Hurricane Look, Perpetual Storms, Noise Motion), then Time, Quality, Layers, Numerics, Pipeline and Debug. Nothing sits under Advanced, because the details search skips collapsed advanced properties.
- **State and tunes:** nothing restores from a dump or tune on its own. The actor's saved properties and the sim config assets are the source of truth until the harness owns state (BUG-42). Tunes apply only when loaded.
  - `LoadParams` and `ApplyTune` leave Pipeline and the actor's Quality settings alone unless asked.
  - The sim config's Quality group always loads.
  - In a game world, sim sections go to the actor's runtime copy of its config (`GetWritableSimConfig`), so tune in the editor world to save.
- **Class defaults stay as they are.** Changing a default silently changes every saved asset that held the old one. Restart the editor after a constructor change before saving levels.
- **The user is hand-tuning** both models. `Design/Tunes/Hand_Terrestrial.json` and `Hand_GasGiant.json` are the current set, and the cloud noise pack is being converted to equalized noise (PROC-06).
- **Open with the user:** the terrestrial tune's `CoverageSoftness` of 0.75 caps coverage at about 74% and pins the threshold, so `CloudCover` barely acts (UsageGuide, Coverage).

## Decisions

All settled except DEC-10, which belongs to Phase E. The parameter pass's decisions are in `Design/ParamPass.md`.

| ID | Decision |
|---|---|
| DEC-01 | `TR_DEEP_DARKNESS` stays at 1; the 0 path is stripped. |
| DEC-02 | `TR_DEEP_AIR` stays at 1; the 0 path is stripped. The deep floor (`FloorRelief`) stays. |
| DEC-03 | The wind budget (`Design/Tunes/Winds_Budget_v5.json`): authored winds equal achieved winds under the speed ceiling's 0.7 knee. The shipping tunes are tuned on it (PROF-03). |
| DEC-04 | `SIM_CELL_PRESSURE_SOFT` stays at 1; the clamp path is stripped. |
| DEC-05 | Storm inflow stays the closed-loop momentum push: the gains view (debug CellGains) shows neither gain pinned on either model. |
| DEC-06 | Hard-coded constants: look and feel exposed, character exposed, numerics frozen and documented at the code (`Design/Constants.md`). |
| DEC-07 | The stamp's draft takes the strongest overlapping cell's, like storm, low and strength. |
| DEC-08 | `ShockDamping`, `ImplicitWeight` and `AscentSmoothing` are constants at their tuned values (`FlowSimNumerics`). `Stratification` and `LayerCoupling` stay handles. |
| DEC-09 | No band colouring in the core: the gas giant's palette is cloud, storm and the deep material. |
| DEC-10 | Deferred to Phase E: weather readback, implicit Coriolis and a surface or ocean map. |
| DEC-11 | One rotation: the actor's `SpinRatio` is a share of the config's `PlanetaryVorticity` / 2. `StepSize`, the spin-up step and the eye rates are in turnovers. |
| DEC-12 | Detail `MipBias` defaults to 0 and `CellSize` to 3. The temporal path freezes as it is, with fast-flight ghosting accepted. |
| DEC-13 | `MaxStormCells` 32 and `MaxPerpetualStorms` 8. The cells pass stays one 32-thread group. |
| DEC-14 | `.ush` include files count as shader files for comment volume (50%). |

## The docs

| Doc | Covers |
|---|---|
| `Design/UsageGuide.md` | Every user-exposed setting: what it does, its range, defaults and the current tune. Generated by `Tools/UsageGuide/gen.py` from a `CloudAtmosphere.DumpSchema` dump; regenerate after a parameter change. |
| `Design/HarnessReference.md` | For the procedural harness: each setting's class, a first-estimate draw range, the rules draws must keep, and how a change acts (resim, instant, reset) or what it costs. Generated by `Tools/UsageGuide/gen_harness.py`. |
| `Design/Todo.md` | Open work, by area, with priority, freeze class and tier. |
| `Design/FlowSimShallowWater.md` | The sim's equations, passes and storm cells (to be rewritten as the sim reference, DOCS-02). |
| `Design/SpeedScale.md` | The speed root and turnover units (folds into the sim reference). |
| `Design/PerpetualStorms.md` | Perpetual storms (folds into the sim reference). |
| `Design/SimOwnership.md` | One sim per world: claims, handover, kept fields, snapshots, authoring a profile. |
| `Design/TerrestrialClouds.md` | The cloud field (to be rewritten for both models, DOCS-03). |
| `Design/TemporalMarch.md` | The march, the camera lattice and the temporal resolve (with FlagAudit, DOCS-04). |
| `Design/FlagAudit.md` | Every compile-time flag left in production code and why. |
| `Design/Constants.md` | The hard-coded constants and which became handles. |
| `Design/ParamPass.md` | The parameter pass's decisions (D-1 to D-35). |
| `Design/Review_2026-10-01.md` | The pre-freeze review and its triage. |
| `Design/Tunes/` | Tunes in `DumpParams`' layout. `Hand_*` are current; the rest are archived in DOCS-06. |
| `Tools/ShaderCheck/` | The shader compile check (README there). |

## How the work is run

- **Deliverables are project files.** Stage full updated files with `project_write` from a `local_path` inside the working directory (scratchpad paths are refused). Also send a zip of the changed engine files, since the user copies them into the engine tree. Snippets only for a one-line change.
- **Project paths:**
  - Source and shader files sit flat at the project root, except files the project tool placed under `claude/`: `claude/FlowSimJets.ush`, `claude/FlowSimTypes.h` and `claude/AtmospherePreset.h` are the live copies. A root path that was deleted can't be created again.
  - The `claude/` copies of `AtmosphereViewExtension.h/.cpp`, `AtmosphereMarchPass.usf` and `AtmosphereTemporal.usf` are older duplicates of the root's.
  - Docs are under `Design/`, tunes under `Design/Tunes/`, tools under `Tools/`.
- **Engine source:** the user attaches a zip of the plugin's `Shaders/Private` and `Source` folders. Work on those files, which carry the true line endings, rather than on text read back from the project.
- **Comments** describe the code as it is: no history, and pitfalls phrased without history. Comment lines stay at or below 50% of code lines in `.cpp`, `.usf` and `.ush` files, and 100% in headers. UPROPERTY docs are the editor tooltips and the usage guide's source, so they say what the setting does, its units and its range.
- **Line endings are per file; preserve them** and check with `file` before staging.
  - LF: `AtmosphereParams.h`, `AtmospherePreset.h`, `AtmosphereShadowBake.h`, `AtmosphereTransmittance.h/.cpp`, `TerrestrialShadowMap.h/.cpp`, `FlowSimSettings.h`, `FlowSimShaders.h/.cpp`, `FlowSnapshot.h`, the docs, the JSON tunes and the tools.
  - CRLF: every other source and shader file.
- **Look changes go one at a time, behind a define,** for the user to compare. A confirmed variant becomes the default, and its switch is stripped once the old path isn't expected back. Every flag in production code needs a justification in `Design/FlagAudit.md`. The user dislikes new flags and handles, so a change that should just work lands without one.
- **Exact changes are checked for identical DXIL** before and after: diff the per-entry DXIL `check.sh` keeps.
- **Limits:** `ClampMin`/`ClampMax` and the push's guards go only at true limits: a singular point (a divide, |g| of 1), a fraction or weight outside [0, 1], a resource size. Acceptable ranges are `UIMin`/`UIMax`; going past them can be useful.
- **Renames and retirements** get a row in the dump loader (`AtmosphereParamDump.cpp`), so older tunes keep loading. A changed meaning raises the sim config version.
- **Build facts:**
  - A header or reflection change needs a full rebuild.
  - C4456 and C4458 (shadowing) are errors in the user's build.
  - A uniform buffer member must not share a name with its `Sim*` alias.
  - An HLSL local named like a uniform alias hides it without a warning.
  - A uniform array indexed in a flattened branch must have its index clamped.
  - Constants the sim's C++ and shaders share are `FlowSimShader` members (`FlowSimShaders.h`), pushed as `FLOWSIM_*` defines in `ModifyCompilationEnvironment`. A new one also goes in `check.sh`'s `SIM_DEFINES`, and a render pass's pushed define goes in that pass's list there.
- **Shader compile check:** `Tools/ShaderCheck/check.sh <shader dir> <FlowSimShaders.h> <out dir>`, with the shader dir being `Shaders/Private`. It compiles all 27 entries (the 18 sim passes, the march at each `ATMO_MODEL`, the bake with both `TR_DEEP_DECK` values, the coverage pass, resolve, composite and transmittance) and keeps each entry's DXIL for diffing. DXC comes from the GitHub release.
- **Before staging**, check that no local workspace path has leaked into an include.

## Pitfalls

- **Unsaved defaults.** Unreal saves only values that differ from the class default, so changing a default or reinterpreting a member silently changes every asset that held the old default.
- **Renames lose saved values** without a redirect. The tune restores them, and the loader row keeps older tunes loading.
- **Uniform names are the C++ member names.** A mismatch the shader uses fails the binding check. One it doesn't use is silently unbound.
- **Shared structs, per-model defaults.** A struct both models share takes each model's defaults in the actor constructor, not as member initialisers.
- **The field's pin list** is written in four places: `TR_BuildField` and its `TR_BUILD_FIELD` macro, the uniforms in `AtmosphereMarchPass.usf` and `TerrestrialShadowMap.usf`, and `FTerrestrialFieldParameters`. Change it in one commit.
- **The sim config version** never goes down: the engine refuses a package saved above the latest version.

## Easily lost

- **Orphan files** to delete in the engine tree:
  - Included by nothing: `AtmosphereOccluders.ush` and `AtmosphereComposite.ush`.
  - Retired shaders: `GasGiantJets.ush`, `GasGiantField.ush`, `GasGiantDeck.ush`, `GasGiantShadow.ush`, `GasGiantShadowMap.usf`, `GasGiantMarch.usf`, `TerrestrialMarch.usf`, `AtmosphereMarchPass.ush` and `AtmosphereNoise.ush`.
  - Retired source: `GasGiantShadowMap.h/.cpp`.
  - Unreferenced content: the three atmosphere materials and their instances.
- **Slot budget:** perpetual storms take the first of the 32 cell slots, up to 8; hurricanes get up to `MaxStormCells` of the rest.
- **Phase D notes:** start states come from a seeded pick in an authored snapshot library per model (RUN-01). If fast-flight air lag remains, an adaptive `CellSize` with RUN-03 is the next step.
