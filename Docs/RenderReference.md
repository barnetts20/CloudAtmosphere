# Render reference

How the atmosphere's render path works and why: the compute passes, the march, the temporal resolve, the cloud shadow map, the transmittance table, the composite and the directional light, the switches left in production code, cost and pitfalls. Per-setting behaviour is in `Design/UsageGuide.md`; the cloud field (`TerrestrialDeck.ush`, `TerrestrialField.ush`) is in `Design/FieldReference.md`, and the weather sim, its ownership and the kept field in `Design/SimReference.md`. Section 7 is the plugin's flag register, sim included. Settings are named as in code; files and functions are cited without line numbers.

## 1. Overview

Everything draws as compute passes. `APlanetAtmosphereActor` fills the inputs every tick; `FAtmosphereViewExtension` (`AtmosphereViewExtension.cpp`) runs the per-view passes at `EPostProcessingPass::BeforeDOF`, ahead of depth of field and the upscaler, at internal resolution. The placement relies on TSR resolving what the march leaves, and on bloom and eye adaptation seeing the atmosphere.

### Frame order

| When | Pass | Entry | Runs |
|---|---|---|---|
| World pre-actor tick | Sim step | `FlowSim.usf` | `UFlowSimSubsystem::Advance`; writes the flow atlas the field reads |
| Actor tick | Transmittance bake | `AtmosphereTransmittance.usf` `MainTransmittanceCS` | Only when its inputs change (section 5) |
| Actor tick | Frame hand-off | — | `UpdateComputeMarch` → `SetFrame_GameThread` enqueues this frame's `FAtmosphereMarchParams` |
| Subsystem tick | Coverage threshold | `TerrestrialShadowMap.usf` `MainCoverageCS` | Every frame with clouds, one group |
| Subsystem tick | Shadow bake | `TerrestrialShadowMap.usf` `MainShadowBakeCS` | One dispatch per level in the request's `LevelMask` |
| Scene render, per view | March | `AtmosphereMarchPass.usf` `MainMarchCS` | One pixel per `CellSize` cell |
| Scene render, per view | Temporal resolve | `AtmosphereTemporal.usf` `MainResolveCS` | Every output pixel |
| Scene render, per view | Composite | `AtmosphereTemporal.usf` `MainCompositeCS` | Every output pixel |

`UpdateAtmosphere` runs, in order: `ClaimSimulation` and `RequestShadowBake` (clouds only), `UpdateTransmittanceTable`, `UpdateComputeMarch`. The shadow request is queued with `UFlowSimSubsystem::RequestShadowBake` and dispatched by `BakeShadowMap` from the subsystem's `Tick`, after the sim step: the bake reads the flow atlas the step writes, and the order relies on render commands running in enqueue order and on the subsystem, a tickable object, ticking after the actors. The coverage pass and the bake levels share one graph (`TerrestrialShadow::AddBakePass_RenderThread`), coverage first. `CommitShadowBake` records each baked level's camera and light before `FillMarchParams` reads them, so a frame's march reads every level against the camera and light its newest bake used.

GPU event names: `CloudAtmosphere` › `AtmosphereMarch WxH`, `TemporalResolve N`, `Composite`; `CloudShadowBake` › `Terrestrial.Coverage`, `Terrestrial.ShadowBake Level L`; `AtmosphereTransmittance` › `Atmosphere.TransmittanceBake`.

### State

| Owner | State | Lifetime |
|---|---|---|
| Actor | `ShadowTarget` (RGBA16F array, 3 slices of `ShadowResolution`²), `CoverageTarget` (1 texel R32F), `TransmittanceTable` (256 × 64 RGBA32F) | Transient; shadow and coverage freed under air only |
| Actor | Per level: `ShadowBakedCamera`, `ShadowBakedLight`, `ShadowBakeTime`; `ShadowLevelCursor`, `bShadowPrimed`, `ShadowFieldKey`, `ShadowType` | Rotation and history of the shadow map |
| Actor | `FieldClock` (sim time and spin time of the field drawn), `FlowTarget` (the sim's atlas or `KeptFlow`) | Set by `ClaimSimulation` each tick |
| Extension | `FAtmosphereMarchParams` (render-thread copy) | Replaced each tick |
| Extension | `FViewHistory` per view state (`View.State->GetViewKey()`): `Color`, `Tint`, `Age`, and the model, cell size, `CameraToClip`, camera origin, planet centre, rotation and spin angle it was resolved with, a frame counter | Dropped after `HistoryLifetime` (300) frames unrendered, on disable and on release |

The extension is created on the first tick `FillMarchParams` succeeds, enabled while the params are usable and the atmosphere is active, and released on the render thread (`ReleaseViewExtension`). One extension per atmosphere.

### Models

`PlanetType` picks the model; `FillMarchParams` maps it to the march's `ATMO_MODEL` permutation and `RequestShadowBake` to the bake's `TR_DEEP_DECK`.

| `PlanetType` | `ATMO_MODEL` | Bake | Bundle | Sim config | Sim, coverage, bake |
|---|---|---|---|---|---|
| Terrestrial | 0, the slab | `TR_DEEP_DECK` 0 | `Terrestrial` | `TerrestrialConfig` | Yes |
| GasGiant | 1, the deep deck | `TR_DEEP_DECK` 1 | `GasGiant` | `GasGiantConfig` | Yes |
| AirOnly | 2, air alone | — | `Terrestrial` | — | No |

`AtmosphereMarchPass.usf` derives `TR_DEEP_DECK` (`ATMO_MODEL == 1`) and `ATMO_FIELD_CLOUDS` (`ATMO_MODEL != 2`). Air only:

- skips the sim claim (releasing a held one, keeping its field), the coverage pass and the bake, and frees the shadow and coverage targets (`ReleaseCloudResources`);
- skips the field's packing; the march binds black in the flow, volume, shadow and coverage slots, and `IsUsable` needs only the transmittance table and blue noise;
- compiles out every cloud segment (both band radii at the planet radius), the shadow map reads and the surface shadow's cloud term (`ATMO_FIELD_CLOUDS` 0).

It keeps the air march, the transmittance table, the surface shadow's air dimming, the resolve, the composite and the directional light.

A model change takes effect on the next tick: the march's model differs from the history's, so every view's history restarts; `ShadowType` differs, so every shadow level rebakes; the bundle, and with it the air profile, the field and the sim config, swap.

### Compiled entries

`Tools/ShaderCheck/check.sh <shader dir> <FlowSimShaders.h> <out dir>` compiles every entry with DXC against stub engine headers and writes each one's DXIL, stripped of hashes, so two builds diff by code. It pushes what each pass's `ModifyCompilationEnvironment` pushes:

| Entry | Permutations | Defines |
|---|---|---|
| `MainMarchCS` | `ATMO_MODEL` 0, 1, 2 | `ATMO_MARCH_THREADS` 8, `ATMO_TRANSMITTANCE_WIDTH` 256, `ATMO_TRANSMITTANCE_HEIGHT` 64 |
| `MainShadowBakeCS` | `TR_DEEP_DECK` 0, 1 | `ATMO_BAKE_THREADS` 8 |
| `MainCoverageCS` | — | — |
| `MainResolveCS`, `MainCompositeCS` | — | `ATMO_TEMPORAL_THREADS` 8 |
| `MainTransmittanceCS` | — | `ATMO_TRANSMITTANCE_THREADS` 8, table size |
| 18 sim entries | — | `FLOWSIM_*` |

27 entries in all.

## 2. The march

`MainMarchCS` → `Atmo_MainMarchDepth` (`AtmosphereMarch.ush`). The march sees the cloud field only through the `Field_` interface of `TerrestrialField.ush`, which must be included first (`ATMO_FIELD_INCLUDED`).

### Ray setup

- **Space.** Camera-relative and world-oriented: the ray starts at the origin and `PlanetOffset` is the planet centre less this view's camera, formed in double on the render thread from each view's own origin. The field is planet-local through `Atmo_WorldToLocal(PlanetRotation)`, the rotation of the planet the atmosphere is attached to (`GetFieldFrame`), never the actor's own.
- **Ray.** `Atmo_RayDirection` inverts `RayViewToClip`, the jittered projection, so the march lands on the pixels the jittered depth buffer holds.
- **Scene depth.** `Atmo_SceneDepthAt` inverts device Z exactly (reverse-Z 0 is sky, `ATMO_NO_OCCLUDER_DISTANCE`), clamped to the view rect; `Atmo_SceneRayDistance` turns it into a ray distance, the march's bound. The view-axis depth is kept as the resolve's guide.
- **Parameters.** `TR_BUILD_FIELD`, `TR_BUILD_SCATTER` and `TR_BUILD_ATMO` expand over the pass's uniforms, which carry the builders' argument names. `Atmo_BuildParams` divides every air and cloud coefficient by the atmosphere thickness (coefficients are authored per thickness) and scales the profile by it. The cloud coefficient is grey, `TR_CloudBeta(F, CloudOpticalDepth)`; the light ray's is that times `LightExtinctionFraction` (1 − `SunlightPenetration`).
- **Band.** `cloudOuterRadius` is `TR_ShadowCullRadius` (planet radius + thickness × `TR_TopMax`), the radius the shadow map covers, so the lattice does not move with the camera. `cloudInnerRadius` is the lowest base (`TR_BaseMin`, held at the surface) for the slab; for the deep deck, where every column is full (`TR_DeckFull`, held above 0.1 planet radii).

### Terminus and segments

`Field_Terminus` decides where the ray stops before planning:

- **Slab:** the planet sphere clips the ray only where the depth buffer holds sky; geometry below the sphere keeps its own depth.
- **Deep deck:** a sphere `TR_DEEP_OPAQUE_DEPTH` (8) optical depths below `TR_DeckFull`, at the least extinction any material in use has (`TR_DeepTerminusRadius`), clips a ray that reaches it before the scene depth and marks it opaque.

`Atmo_Plan` (`AtmospherePlan.ush`) cuts the ray at the atmosphere and cloud shells into at most `ATMO_MAX_SEGMENTS` (5) segments `(start, end, band flag)`. `Field_SegmentIsCloud` alone decides cloud: the band flag, plus under the deep deck anything below the band.

### Air steps

The air segments' chords are summed first. `Atmo_ChordSteps` spreads `AtmosphereSteps` (N) over that chord L geometrically, the last step `ChordSpread` (S) times the first:

```
r = S^(1/(N-1))      first = L (r - 1) / (r^N - 1)      step(x) = first + (r - 1) x
```

x is the air chord already consumed, so one accumulator carries the progression across segments; a step trimmed at a segment boundary credits its trimmed length. The first step is `Atmo_LeadStep`, the distribution's chord to the pixel's lattice phase. Air ahead of a cloud segment stops at that segment's first lattice point.

### Cloud steps: the camera lattice

Cloud steps sit on a lattice anchored at the camera: step length is a function of distance alone, so rays agree wherever they overlap (`Atmo_CameraLattice`, `Atmo_LatticeStep`).

```
Floor f   = (cloudOuterRadius - cloudInnerRadius) / CloudSteps
Growth g  = exp(lerp(ln LatticeGrowth, ln LatticeGrowthFar, smoothstep(0, 4, a)))
            a = (|camera - centre| - cloudOuterRadius) / band depth
Knee k    = f / g
u(t)      = t / f                        t < k
          = 1/g + ln(t / k) / ln(1 + g)  t >= k
Cap       = f * ChordSpread / |dot(ray, up)|
```

- Steps are `f` long out to the knee, then grow in proportion to distance. Over a chord to distance D a ray takes about `1/g + ln(D g / f) / ln(1 + g)` cloud steps, up to `ATMO_MAX_ITER` (256) per segment.
- Each pixel's lattice is shifted by a phase in (0, 1] (`1 - ATMO_DRAW(svxy, 7919, shift)`), so neighbours share no boundaries; the air's lead step uses the same phase.
- A cloud segment widens back to the lattice point before its shell crossing (`Atmo_LatticeFloor`); only the terminus trims a cloud step. A lattice point under half a step away is skipped for the next.
- `ChordSpread` caps a step's rise through the band at that many floors, so a distant camera still crosses the deck in at least `CloudSteps / ChordSpread` steps.
- **Entry refinement.** The first coarse step that returns density is not accumulated. `RefineUntil = Atmo_LatticeDistance(L, Atmo_LatticeCoord(L, t) + ATMO_ENTRY_SPAN)` from that step's start `t`, so the step and the next (2 lattice steps) are re-marched in `ATMO_ENTRY_SUBSTEPS` (4) substeps each on the divided lattice: 8 substeps in place of 2 coarse samples plus the discarded one, up to 7 net extra. A clear coarse sample re-arms it. This divides the bias from where a surface falls in a step without moving any step boundary.

### Samples

Each step takes one sample, jittered within the step: `t + s/2 + min((ξ - 0.5) s, MaxDist - (t + s))`, ξ = `ATMO_DRAW(svxy, StepIndex, shift)`. The `min` keeps the step that ends at the terminus one-sided, so its slab does not reach behind the mesh the ray stopped at.

- **Draws.** `ATMO_DRAW` reads the blue noise tile at a texel offset by an R2 step in the draw index (`Atmo_TextureJitter`) and adds the frame's `JitterShift`. The lookup moves per index, not the value.
- **Gate.** A sample is shaded if it is above the planet radius or in a cloud segment: the surface bounds the air, not the deck. Below the surface, air density is the field's (`Field_AirDensity`: zero for the slab, held at the surface density for the deep deck).
- **Field read.** `Field_Sample(…, LocalPos, StepHalfHeight, F)`, where `StepHalfHeight` is the step's radial half-extent in atmosphere fractions, which the field integrates its profile across. The detail fade (`DetailFadeNear`/`DetailFadeFar`) and the pixel footprint are set per sample from its own jittered distance, in planet radii.

### Lighting per sample

`Atmo_LightMarch` reads, it does not march:

- **Planet shadow.** `Atmo_PlanetShadow`: the light ray's closest approach against the planet radius, softened by `ATMO_TERMINATOR_SOFTNESS` (1e-4 thicknesses) and ramped in across the terminator plane. The lobe shadow is its 6th power (`ATMO_LOBE_SHADOW_POWER`).
- **Air toward the light.** `Atmo_SunAir`: the transmittance table at the sample's radius and light cosine, times the betas. For a deep-deck sample below the surface, `Field_AirBuried` first moves the radius and cosine to where the light path leaves the surface sphere, so the table is read there, and adds the buried air below it at the surface's density.
- **Cloud toward the light.** `AtmoShadow_ReadTau` from the shadow map (section 4), jittered up to half a texel per read by a hash salted with the step and the frame's shift, divided by the fold constant `Fold.x` to give a material-weighted depth `τc`. Its ceiling is `Field_ShadowSaturation()` (5 on the slab, `TR_DEEP_SHADOW_SATURATION` 16 on the deep deck) over `max(Fold.y · ScatteringGlow^(OctaveCount-1), 1e-3)`, set on the slowest channel at the deepest octave.
- **Tint.** The receiver's own material colours the cloud that shadowed it: `Field_SampleLightTint` inside cloud, `Field_LightTint` (one coarse column read) outside, only where `τc > 0`. The light coefficient is `cloudAbsBeta · tint`.

With `a` = `OctaveAttenuation` (`ScatteringGlow`), `e` = `OctaveEccentricity` (1 − `ScatteringSpread`), `Sl` the lobe shadow and `Tair` the planet shadow times `Atmo_SunAir`:

```
w_o    = a^o · DualLobe(cosθ, ForwardG · e^o, BackwardG · e^o, ForwardWeight)      o < OctaveCount
Octave = Σ_o w_o · lerp(1, Sl, e^o) · Tair · exp(-cloudAbsBeta · tint · τc · a^o)
Cloud  = LightColor · (Octave + CloudAmbient · A_cloud · AmbientShare)
dL     = Cloud · albedo · (1 - exp(-σ s)),   σ = density · Field_Extinction · cloudScatBeta
```

Octave 0 is exact single scattering. The cloud step is energy-conserving: it scatters in what it takes out, scaled by albedo.

Air in-scatter per step, `Tsun = Tair · exp(-cloudAbsBeta · tint · τc)`:

```
direct  = (βR PR ρR + βM PM ρM · exp(-MieLobeDecay · τahead) · Sl) · Tsun · LightColor
ambient = βR ρR · AirAmbient · LightProduct · A_air · AmbientShare
weight  = s · (1 - exp(-τ)) / τ   per channel, τ the step's air optical depth
```

`τahead` is the mean-channel cloud optical depth already crossed by the view ray, so the haze's forward lobe dies behind cloud. The ambient factor `A = lerp(floor, 1, smoothstep(-AmbientTerminator, AmbientTerminator, cos sun elevation))` uses the sample's own up vector, with `AirAmbientFloor` and `CloudAmbientFloor`. `AmbientShare` is 1 except in the deep deck, where `Field_AmbientShare` buries it under the deck above the sample.

### Early exit

When the largest transmittance channel falls below `ATMO_OPAQUE_TRANSMITTANCE` (0.001) the ray stops and transmittance snaps to zero. A ray clipped by the terminus, the slab's planet sphere over sky or the deep deck's terminus (`Field_Terminus`'s `OutOpaque`, `bClippedOpaque`), is opaque by construction and snaps to zero too.

### Surface shadow

At the end of the ray, when `SurfaceShadow.bEnabled`, the depth buffer held geometry and the ray is not already opaque, the march folds sunlight on that geometry into the transmittance, so no other pass exists:

```
Sun = exp(-cloudAbsBeta · tint · τsurface · Strength) · Atmo_SunAir(surface)
T  *= 1 - DirectFraction · SunUp · (1 - Sun)
```

`τsurface` comes from `AtmoShadow_SurfaceOptDepth` at the surface point (`sceneDepth` along the ray), without lookup jitter, ceilinged at `ATMO_SHADOW_SATURATION / max(Fold.y, 1e-3)`, zero outside the cull radius. `SunUp` is the planet shadow at the surface, so the night side takes nothing. Air only keeps the air term. Since it multiplies the scene in the composite, specular and bounce darken with direct sun; translucency does not receive it.

### Outputs and depth

| Target | Format | Holds |
|---|---|---|
| `MarchColor` | RGBA16F | In-scattered light (unexposed, held under 60000), green transmittance |
| `MarchTint` | G16R16F | Red and blue transmittance |
| `MarchDepth` | G32R32F | Representative depth; view-axis guide depth, positive where the depth is the field's |

The representative depth is the ray distance the resolve reprojects by, the first that applies:

1. where the cloud's own mean transmittance crosses 0.5, interpolated inside the step as an exponential;
2. a ray clipped opaque by the terminus: the terminus;
3. the cloud's opacity-weighted mean distance, once its opacity passes `ATMO_DEPTH_MIN_OPACITY` (0.001);
4. the surface the ray stops on inside the atmosphere;
5. the air's in-scatter-weighted distance for a ray that leaves through the shell;
6. `ATMO_SKY_DEPTH` (−1), reprojected as a direction.

Air sets no half-transmittance depth: along the limb it alone can fall below half ahead of the cloud. The guide depth's sign is the cloud flag, set whichever depth was chosen: positive when the cloud's transmittance crossed half (`HalfDepth >= 0`), when its opacity passes `ATMO_DEPTH_MIN_OPACITY` (which every ray where rule 1 applies does), or when the deep deck's terminus clips the ray (`Field_TerminusIsField`), since that terminus moves with the field. A positive sign reprojects the depth with the field's spin. Threads past the view's edge write clear sky; a non-finite result is written as clear sky with a negative guide.

## 3. Sampling and temporal resolve

### Interleaving

Per frame the march runs one pixel of each N × N cell, N = `CellSize` (clamped to 1–16):

- **N per frame.** A view with valid history marches at N; a view state without valid history at `min(N, 2)`; a view without a state (a scene capture) at 1, every pixel, every frame, and resolves from that frame alone.
- **Rank.** `CellPixel` orders a cell's pixels by the R2 lattice's value, an ordered dither for any N, so consecutive frames' pixels sit far apart. The rank marched is `(Frame + R · ⌊Frame / N²⌋) mod N²`, R = 1 for an even N² and 2 for an odd one, so a pixel's revisits are an odd number of frames apart, never a multiple of the 8-frame jitter period the engine's sequence is relied on to have.
- **Draws.** Seeded by cell coordinate plus `NoiseOffset`, an R2 offset per rank into the noise tile, so each frame's marched set reads one contiguous window of the blue noise and stays blue at cell resolution. `JitterShift = frac(⌊Frame / N²⌋ · 0.618…)` moves every draw, the lattice phase included, by the golden ratio per sample the pixel has taken.

### Reprojection

The history is unjittered: each history pixel is its centre's ray (the resolve binds the unjittered projection as `RayViewToClip`), and the jittered samples accumulate into it.

| Matrix | Maps this frame's camera-relative point to the previous clip space | Used for |
|---|---|---|
| `PrevCameraToClip` | previous view rotation and unjittered projection, no translation | Sky, as a direction |
| `PrevPlanetToClip` | `PlanetDelta · PrevCameraToClip` | Depths not the field's |
| `PrevCloudToClip` | `CloudDelta · PrevCameraToClip` | Depths the field set |

Built in double in `Render_RenderThread`:

```
Turn        = Q_prev · Q_now⁻¹
Move        = Turn(camera_now - planet_now) - (camera_prev - planet_prev)
PlanetDelta = rotate(Turn) then translate(Move)
Spin        = Q_now · RotZ(unwind(spin_now - spin_prev)) · Q_now⁻¹
CloudDelta  = translate(-centre) · rotate(Spin) · translate(centre) · PlanetDelta
```

`spin` is `CloudMotion.w`, the field's spin angle `-½ PlanetaryVorticity · SpinRatio · SpinTime` (`PackCloudField`), wrapped in double; `centre` is the planet centre relative to this frame's camera. A point is reprojected as fixed to the planet through the planet's move and turn and the camera's move, and a cloud point through the field's spin about the planet's axis as well.

Each output pixel reprojects its ray at a representative depth: its own fresh depth when it was marched this frame, else the depth of the best-weighted fresh neighbour.

### Fill, clamp and blend (`MainResolveCS`)

The 4 × 4 fresh samples about the pixel give:

- **Fill:** bilinear over the inner four, each weight times the guide weight `exp(-100 · (|a - b| / max(min(a, b), 1))²) + 1e-3` on view-axis scene depth (`ATMO_GUIDE_SHARPNESS`).
- **Moments:** mean and standard deviation under a tent `saturate(1 - 0.5 · distance)` over all 16 taps, reaching two cells either side, twice the fill's reach, with the same guide weights, in exposed radiance compressed by its brightest channel (`Atmo_Compress`); transmittance unchanged. Every weight moves continuously with the pixel.

Samples past the view's edge or clamped at the image's edge are left out. Then:

```
motion   = saturate(|reprojection offset in pixels| / 2)                 ATMO_MOTION_PIXELS
gamma    = lerp(4, 1, motion)                                            ATMO_CLAMP_GAMMA(_MOTION)
history  = clamp(history, mean ± gamma σ)                                in compressed space
reject   = saturate(max over channels |history - clamped| / (gamma σ + 0.01))
age      = min of the 4 texels' counts × (1 - reject)                    total and own
cap      = 1 / FreshWeight
marched pixel:    alpha = max(1 / (age + 1), FreshWeight)   result = lerp(history, own, alpha)
other pixels:     share = lerp(0.1 · saturate(1 - ownAge), 0.5, motion)   result = lerp(history, fill, share)
no valid history: result = fill
```

The fill counts toward the total at its share; at rest it fades out once the pixel holds a sample of its own (`ownAge` ≥ 1), so the history converges to the pixel's own samples at full resolution. An unclamped history passes through without the compression round trip. History is invalid off-screen or behind the previous camera. Results are held under 60000.

History targets (full internal resolution): `Color` RGBA16F, `Tint` G16R16F, `Age` G16R16F (total samples, own samples).

### History lifetime

A history restarts on a camera cut, a gap in rendering of more than one frame, a model change or a new `CellSize`. A new internal resolution does not restart it: the extension relies on the view rect changing frame to frame under dynamic resolution and in the editor, and the resolve reads the history by UV at its own size. Disabling the extension (parking) frees every history.

### Debug views

`r.CloudAtmosphere.Temporal.Debug` (0–4) draws into its own target, `CloudAtmosphere.Debug`, composited opaque; the history keeps the real result.

| Value | Shows |
|---|---|
| 1 | Samples accumulated over their cap, grey; white when settled |
| 2 | Fresh blue, reprojected green, filled red |
| 3 | This frame's samples alone: the pixel's own, else the fill |
| 4 | Motion red, clamp rejection green |

`ATMO_DEBUG_TERM` (section 7) isolates march terms instead; the resolve and composite shape it as they shape the image.

## 4. The shadow map

One map per planet, shared by the march (cloud lighting) and the surface shadow. Format and reconstruction are model-independent (`AtmosphereShadowMap.ush`, `AtmosphereShadowBake.ush`, `AtmosphereShadowBakeLoop.ush`); the field supplies an `AtmoShadowFrame` (`TerrestrialShadow.ush`) and the bake hooks (`TerrestrialShadowMap.usf`).

### Cascades

Three levels, one slice each (`ATMO_SHADOW_CASCADES`, mirrored and asserted by `AtmoShadowBake::CascadeCount`):

| Level | Half-width | Centre | Detail layer |
|---|---|---|---|
| 0 | `CullRadius × 1.02` (`ATMO_SHADOW_EXTENT_MARGIN`), the whole disc | Planet centre | Mean erosion |
| 1 | `PlanetRadius × CascadeRadii.X`, held within level 0 | Camera, snapped to the level's texel grid | Mean erosion |
| 2 | `PlanetRadius × CascadeRadii.Y`, held within level 1 | Camera, snapped | Fetched (`TR_SHADOW_DETAIL_LEVEL` 2) |

`CullRadius` is `TR_ShadowCullRadius`, the march's `cloudOuterRadius`. A zero radius collapses its level: the bake writes the sentinel and the reader skips it.

- **Plane.** The map plane is perpendicular to the light; `AtmoShadow_Basis` puts V toward the spin axis so the lattice does not rotate as the light moves, (U, V, L) right-handed.
- **Disc warp.** `ATMO_SHADOW_DISC_WARP` 1 stores the disc against the angle from the sub-light point: plane radius `sin(A S) / sin(A)`, A = 90°, so ground resolution is uniform over the lit hemisphere. Every level windows the one warped disc; inner half-widths are `E_l / E_0 · sin(A) / A` in disc units.
- **Camera.** The bake's camera is `World->ViewLocationsRenderedLastFrame[0]`, planet-local, one frame stale; every viewport of a planet shares it.
- **Per-level frames.** Levels bake on different frames. Each records the camera and the light it was baked under (`ShadowBakedCamera`, `ShadowBakedLight`); the march receives them as `ShadowCamera1`, `ShadowCamera2` and `ShadowLight0`–`ShadowLight2` and reads each level in its own basis and centre (`AtmoShadow_MakeReader`). A zero light reads with the current light; a level never baked holds one, relying on the actor's members starting zeroed, as `UObject` memory does.

### Encoding

Inverted: each texel stores the four depths along its light ray at which fixed optical depths are crossed, so the nodes gather where the medium is dense.

| Channel | Optical depth | Transmittance |
|---|---|---|
| R | `ATMO_SHADOW_TAU_ENTRY` 0.02 | the silhouette |
| G | `ATMO_SHADOW_TAU0` 0.25 | 78% |
| B | `ATMO_SHADOW_TAU1` 1 | 37% |
| A | `ATMO_SHADOW_TAU2` 3 | 5% |

- Depths are offsets from the analytic entry into the cull sphere, in atmosphere thicknesses, for fp16.
- Optical depth is on the fastest channel: the bake folds `TR_CloudBeta · LightExtinctionFraction / thickness · (largest extinction tint in use)` into every depth (`BakeField_FoldConstant`), and the reader divides `Fold.x` back out (`Field_FoldConstant`). Each sample's density is weighted by its material's extinction amount (`TRShadow_Amount`); colour is applied at the receiver.
- `ATMO_SHADOW_NO_DECK` (60000), past every chord down to a `HeightScale` of 1e-4, marks a threshold never reached or a ray missing the shell; the target is cleared to it (`ShadowNoDeck`), so an unbaked texel reads lit.

### Reading

`AtmoShadow_ReadTau`: zero outside the cull sphere on its lit side. Otherwise each level's disc coordinate and depth (`AtmoShadow_LevelCoords`, in that level's basis) feed `AtmoShadow_TauSlice`, which reconstructs four texels' optical depth and blends them bilinearly. `AtmoShadow_TauFromNodes` is piecewise linear and monotone: below the entry node the first rate runs back to zero, past the last node the final rate continues up to the caller's ceiling. Levels are walked outermost inward; a finer level replaces the result where its square support contains the point, blending from `ATMO_SHADOW_NEAR_BLEND` (0.75) of its half-width to its edge.

### Baking

One dispatch per level in `LevelMask`, one thread per texel (`AtmoBake_Texel`):

1. **Ray.** The texel's disc point gives the ray's closest approach `Offset`; the ray runs along −L from the cull sphere entry.
2. **Terminus.** `BakeField_TerminusRadius`: the planet under the slab; under the deep deck, the radius at which the light is provably extinguished (light-side coefficient, least material extinction). A chord that meets it ends there, and its unreached thresholds go to the terminator plane (the chord's closest approach), so lit ground below the sphere is not self-shadowed and everything behind the planet is in its shadow.
3. **Entry.** `BakeField_Entry` → `TR_SlabEntry`, a cone trace of up to `TR_SLAB_ENTRY_PROBES` (24) column reads whose closing rate assumes surfaces rise at most `SlopePerTexel` × cloud depth per flow texel; it skips by geometry above `TR_TopMax` and below every base. The entry is backed off `ATMO_BAKE_ENTRY_BACKOFF` (1.5) texel footprints over the descent rate. It only sets where marching starts; the silhouette is the 0.02 crossing.
4. **Lattice.** Cells of `HeightStep = SpanTop · thickness / ATMO_BAKE_STEPS_PER_GRADIENT` (16), anchored at the shell entry. Each step's demand combines in quadrature the radial limit and half a flow texel of arc laterally, and is capped by an optical-depth allowance `ATMO_BAKE_MAX_DTAU · (1 + ATMO_BAKE_DTAU_GROWTH · τ)` (0.15, 1) at the last nonzero σ. The step is grown by doublings (up to 2⁸ cells) to the budget `remaining chord / remaining steps`, then halved (up to `ATMO_BAKE_MAX_HALVINGS` 6) toward the demand while the budget allows, and cut at the next multiple of its own length, so boundaries stay on the lattice. The budget outranks the demand: a ray that runs out of `ATMO_BAKE_MAX_STEPS` (256) loses the shadow behind it.
5. **Extinction.** `BakeField_Sigma` averages σ (not optical depth) over `ATMO_BAKE_SUBSAMPLES` (4) rotated-grid points across the warped texel footprint, at each step's far boundary; the trapezoid makes τ quadratic in the step and `AtmoBake_Crossing` solves each threshold to second order. No jitter: the map is shared by every pixel.
6. **Tail.** Unreached thresholds go to the terminator plane on a terminus chord, extrapolate at the last σ from the chord exit otherwise, or take the sentinel if no cloud was met.
7. **History** (below), then the write.

The hooks a field defines before including `AtmosphereShadowBakeLoop.ush`: `BakeFieldParams`, `BakeField_Params`, `BakeField_CullRadius`, `BakeField_CascadeExtent`, `BakeField_TopBound`, `BakeField_Entry`, `BakeField_TerminusRadius`, `BakeField_HeightStep`, `BakeField_LateralArc`, `BakeField_FoldConstant`, `BakeField_Sigma`. The field is built with the same `TR_BUILD_FIELD` and packer (`PackCloudField`) as the march, so the light sees the field the eye sees.

### Rotation and history

- **Rotation.** `ShadowLevelsPerFrame` levels per request, taken in turn from `ShadowLevelCursor`; all three when the map is not primed. One request per actor per frame (`ShadowBakeFrame`).
- **History.** Before a level's pass writes it, `AtmoShadowBake::AddHistoryCopy` copies its slice. The bake reprojects each ray into that previous bake (previous light basis and camera centre), interpolates four texels and blends `w = min(exp(-age / ShadowTemporalSmoothing), 0.9)`, age in world seconds since that level's last bake (`World->GetTimeSeconds()`, relied on to pause and dilate with the game). A node blends only where it and every weighted tap lie inside the chord; nodes are re-sorted after. Weight is zero on a fresh target, with smoothing at 0, or when the light turned more than about 2° (`HistoryLightCosine` 0.9994).
- **Full rebake.** `bShadowPrimed` clears, and the next request bakes every level without history, when: the target is recreated (`ShadowResolution` change), the model changes (`ShadowType`), the field key changes, the atmosphere wakes from parking, or air only frees the targets. The field key (`MakeShadowFieldKey`) is a CRC of every `FTerrestrialFieldParameters` pin except `CloudMotion`'s clock slots (drift, noise phase, spin), plus `WarpShift`, `CloudOpticalDepth`, the planet radius and `HeightScale`.

### Coverage pass

`MainCoverageCS`, one group of 256 threads ahead of the bake: `TR_Priority` of 4096 columns on a Fibonacci sphere (`TR_COVERAGE_SAMPLES`) into a 256-bin histogram, read from the top until `CloudCover` of the samples lie above, interpolated in the bin that reaches it, clamped to `[CoverageSoftness, 1 + CoverageSoftness]`. It writes `CoverageTarget`'s one texel (cleared to 2, above any priority); the bake and the march read it through `TR_BUILD_FIELD` as `CoverThreshold`. Both models' priority is one function, so it has no permutation.

## 5. Transmittance table

`AtmosphereTransmittance.h/.cpp`, `AtmosphereTransmittance.usf/.ush`. A 256 × 64 RGBA32F target per actor (`TransmittanceTable`), clamped on both axes, created on first use.

- **Contents.** rgb are the Rayleigh, Mie and absorber density integrals from a point to the atmosphere top, in thickness units: geometry and profile only. The march multiplies by thickness and the betas (`Atmo_SunAir`), so column depths and colours never need a rebake.
- **Mapping.** Bruneton's: width the light cosine through distance to the top, height the altitude through ρ; texels concentrate near the ground and the horizon. `AtmoT_UV` and `AtmoT_RMu` are exact inverses landing on texel centres at both ends. Below the horizon a ray reads the horizon's integral: the planet's occlusion is the analytic `Atmo_PlanetShadow`.
- **Bake.** 128 samples per texel at `t = L u³` (`ATMO_TRANSMITTANCE_STEPS`), finest where density is highest; the profile through `AtmoT_Profile`, the one derivation the march shares.
- **Rebake.** `UpdateTransmittanceTable` every tick compares `FAtmosphereTransmittanceParams`: the planet radius, the atmosphere radius (radius × (1 + `HeightScale`)), and the pins `RayleighScaleHeight`, `MieScaleHeight`, `AbsorptionAltitude`, `AbsorptionFalloff`. A change, or a recreated table, enqueues one bake (`AtmosphereTransmittance::RequestBake`). It runs under every model, air only included.

## 6. Composite and the directional light

### Composite

`MainCompositeCS` writes a new texture with the scene colour's description, inside the view rect only:

```
out.rgb = atmosphere.rgb · View.PreExposure + scene.rgb · (Tint.r, atmosphere.a, Tint.b)
```

The march's light is unexposed; scene colour carries the view's pre-exposure. There is no spatial filter: a pixel without settled history already holds the bilinear fill. Under a debug view the composite takes alpha alone, which the debug target writes as 0, so the view is opaque. The result returns as the pass's output, or is copied into `OverrideOutput` when one is given with a matching format.

The pass returns scene colour untouched when: no frame has arrived, the params are not usable, scene colour or scene depth is missing, a cloud resource handle is missing (clouds), the table or blue noise handle is missing, the view is a reflection capture, a planar reflection or not perspective, the feature level is below SM5, or the view rect is empty.

### Directional light

The actor owns `SunLightComponent` (movable, absolute rotation and scale). `UpdateLightFromRotation` runs every active tick, on edits and from `OrientToStar`:

```
LightProduct = LightColor · LightIntensity / max(R, G, B)
rotation     = (-LightDir).Rotation()        LightDir = root's relative rotation, as a world direction
colour       = LightProduct / |LightProduct|
intensity    = |LightProduct|
```

The march's `LightColor` is `LightProduct` itself, and the air's ambient is pre-multiplied by it (`AtmosphereAmbient`), so the clouds, the air and the scene light share one colour × intensity. `SetAtmosphereActive(false)` hides the light with the passes.

## 7. Flags

This is the plugin's flag register, render and sim alike. Every switch in production code is a separate path, untested unless someone tests it and drifting from the path that ships. A switch stays only with a written reason here. A look change goes in behind a define for comparison; once confirmed, the define goes.

### Register

| Flag | Kind | Where | Gates | Why it stays | Retires when |
|---|---|---|---|---|---|
| `ATMO_MODEL` | March permutation | `AtmosphereMarchPass.usf`, `FAtmosphereMarchCS::FModel` | Slab (0), deep deck (1), air alone (2); sets the two below | Three models | — |
| `TR_DEEP_DECK` | Bake permutation; derived in the march | `TerrestrialDeck.ush`, `TerrestrialField.ush`, `TerrestrialShadow.ush`, `TerrestrialShadowMap.usf`; `FTerrestrialShadowBakeCS::FDeepDeck` | The deep deck: no base, the fill, the floor, the terminus, buried air and ambient, the deep material, its shadow saturation | Two cloud models of one field; the slab compiles without the deep deck's code | — |
| `ATMO_FIELD_CLOUDS` | Derived from `ATMO_MODEL` | `TerrestrialField.ush`, `AtmosphereMarch.ush` | 0: no cloud segment, no shadow map read, no surface shadow cloud term | Air only binds no cloud resource | — |
| `ATMO_DEBUG_TERM`, `ATMO_DEBUG_SCALE` | Compile-time; `ATMO_DEBUG_TERM` 0 in shipping, `ATMO_DEBUG_SCALE` 1 by default | `AtmosphereMarch.ush` | March term views 1–12 (air Rayleigh, Mie, cloud direct, ambient, sun transmittance, lobe survival, step and hit counts, first density and altitude, cloud chord) | Dev tool | — |
| `r.CloudAtmosphere.Temporal.Debug` | Console variable | `AtmosphereViewExtension.cpp` | Resolve views, into their own target | Dev tool; history unaffected | — |
| `SurfaceShadow.bEnabled` | Setting | `FAtmosphereSurfaceShadowParams`, packed to `SurfaceShadow.x` | Surface shadow, one scalar branch | User feature | — |
| Noise layer presence | Packed | `PackCloudField` → `NoiseLevels.y`, `.w` | A layer without a volume is left out | An unset asset must not read black noise | — |
| `BlueNoise` unset | Asset | Actor, `FillMarchParams` | Nothing draws; one warning | Robustness | — |
| View state present | Engine | `Render_RenderThread` | History kept, or none (march at N = 1) | The engine decides which views have state | — |
| `PlanetType` | Setting | Actor | Model, permutations, bundle, sim config, sim and bake | Three models | — |
| `bClouds`, `bDeepDeck` | Transient | `FAtmosphereModelParams`, `SyncModelFlags` | Which groups a bundle shows | Panel only | — |
| `Simulation.bClaimSimulation` | Setting | `FAtmosphereSimulationParams` | Whether the planet bids for the sim | User feature | — |
| `ZonalProfile` (`SimZonalProfile`) | Sim setting | `UFlowSimConfig`, `FlowSim.usf` | Banded jets or three cells | Model choice | — |
| `ThermalShape` (`SimThermalParams.y`) | Sim setting | `UFlowSimConfig`, `FlowSim.usf` | Shear from a midlatitude zone or the jets | Authored choice | — |
| `SimHasForcing` | Sim uniform | `FlowSim.usf`, `FlowSimulation.cpp` | Stirring off with no forcing volume | A missing volume decodes to a constant source | — |
| `SimReconstructLatest` | Sim uniform | `FlowSim.usf`, `FlowSimulation.cpp` | Reconstruct writes the resample's latest pair only | Skips work the resample never reads | — |
| `SimRestoreFloatsPerCell` | Sim uniform | `FlowSim.usf`, `FlowSnapshot.h` (`LegacyFloatsPerCell` 13) | A 13-plane snapshot restores its carries as zero | Snapshots in the 13-plane layout | API-10's layout version replaces it |
| `bDebugView`, `DebugMode`, `DebugLayer`, `DebugScale`; `r.FlowSim.DebugMode`, `.DebugLayer`, `.DebugScale` | Sim settings, console variables | `UFlowSimConfig`, `FlowSimSubsystem.cpp` | The sim debug view and its overrides | Dev tool | — |
| `bPaused`, `r.FlowSim.Paused` | Sim setting, console variable | `UFlowSimConfig`, `FlowSimSubsystem.cpp` | Stepping and spin-up held | Dev tool | — |
| `bAutoStartInEditor`, `bAutoStartInGame` | Project settings | `UFlowSimSettings` | Whether the subsystem starts `DefaultConfig` itself | User settings | — |

### Not flags

- Numeric constants (below): one value on one path. Most are `#ifndef`, which only lets a test override them; `ATMO_TERMINATOR_SOFTNESS`, `ATMO_LOBE_SHADOW_POWER`, `ATMO_SHADOW_NO_DECK`, `ATMO_BAKE_MAX_DOUBLINGS` and the temporal constants (`ATMO_CLAMP_GAMMA(_MOTION)`, `ATMO_FILL_WEIGHT(_MOTION)`, `ATMO_MOTION_PIXELS`, `ATMO_GUIDE_SHARPNESS`) are plain defines.
- `ATMO_SHADOW_DISC_WARP` and `ATMO_SHADOW_LOOKUP_JITTER`: continuous weights, not branches.
- Thread counts, table size and `FLOWSIM_*` values pushed from C++; `ATMO_SHADOW_CASCADES` and `FLOW_ATLAS_BLEND`, plain defines.
- `ATMO_FIELD_INCLUDED`: the march's include contract, an `#error` without the field.
- Compile-time checks: `FLOWSIM_ATLAS_GUTTER` against `FLOW_ATLAS_GUTTER` (`FlowField.ush`), `FLOWSIM_MAX_PERPETUAL` against `FLOWSIM_MAX_CELLS` (`FlowSim.usf`), `CascadeCount == 3` (`AtmosphereShadowBake.h`).
- Values whose zero is the end of a continuous range (`FroudeCeiling`, `LayerCoupling`, `StormCellSustainRatio`, an empty `PerpetualStorms`): settings.

### Constants

Overridable with `#ifndef` unless listed as plain defines above; `MaxHistoryWeight`, `HistoryLightCosine` and `HistoryLifetime` are C++ constants.

| Constant | Value | File | Role |
|---|---|---|---|
| `ATMO_MAX_SEGMENTS` | 5 | `AtmospherePlan.ush` | Segment slots |
| `ATMO_MAX_ITER` | 256 | `AtmospherePlan.ush` | Steps per segment loop |
| `ATMO_CAMERA_LATTICE_FAR_ALTITUDE` | 4 | `AtmospherePlan.ush` | Band depths above the deck where `LatticeGrowthFar` applies |
| `ATMO_ENTRY_SUBSTEPS`, `ATMO_ENTRY_SPAN` | 4, 2 | `AtmospherePlan.ush` | Entry refinement |
| `ATMO_OPAQUE_TRANSMITTANCE` | 0.001 | `AtmosphereMarch.ush` | Early exit |
| `ATMO_DEPTH_MIN_OPACITY` | 0.001 | `AtmosphereMarch.ush` | Cloud opacity that sets a depth |
| `ATMO_TERMINATOR_SOFTNESS`, `ATMO_LOBE_SHADOW_POWER` | 1e-4, 6 | `AtmosphereMarch.ush` | Planet shadow |
| `ATMO_MS_MAX_OCTAVES` | 4 | `AtmosphereTypes.ush` | Octave slots (float4-packed) |
| `TR_DEEP_OPAQUE_DEPTH` | 8 | `TerrestrialDeck.ush` | Deep terminus, optical depths below full |
| `TR_DEEP_SHADOW_SATURATION`, `TR_DEEP_DIFFUSE_G` | 16, 0.5 | `TerrestrialField.ush` | Deep deck shadow ceiling, ambient burial |
| `ATMO_SHADOW_TAU_ENTRY`, `TAU0`, `TAU1`, `TAU2` | 0.02, 0.25, 1, 3 | `AtmosphereShadowMap.ush` | Stored crossings |
| `ATMO_SHADOW_SATURATION`, `ATMO_SHADOW_MIN_TAU_SCALE` | 5, 1e-3 | `AtmosphereShadowMap.ush` | Read ceiling |
| `ATMO_SHADOW_NO_DECK` | 60000 | `AtmosphereShadowMap.ush` | Sentinel, mirrored by `ShadowNoDeck` |
| `ATMO_SHADOW_EXTENT_MARGIN`, `ATMO_SHADOW_NEAR_BLEND` | 1.02, 0.75 | `AtmosphereShadowMap.ush` | Level 0 margin, cascade hand-over |
| `ATMO_SHADOW_LOOKUP_JITTER`, `ATMO_SHADOW_DISC_WARP` | 1, 1 | `AtmosphereShadowMap.ush` | Read jitter (texels), warp |
| `ATMO_BAKE_STEPS_PER_GRADIENT` | 16 | `AtmosphereShadowBake.ush` | Radial steps per ramp |
| `ATMO_BAKE_MAX_DTAU`, `ATMO_BAKE_DTAU_GROWTH` | 0.15, 1 | `AtmosphereShadowBake.ush` | Optical depth per step |
| `ATMO_BAKE_MAX_HALVINGS`, `ATMO_BAKE_MAX_DOUBLINGS` | 6, 8 | Bake headers | Step range about the cell |
| `ATMO_BAKE_MAX_STEPS`, `ATMO_BAKE_SUBSAMPLES`, `ATMO_BAKE_ENTRY_BACKOFF` | 256, 4, 1.5 | `AtmosphereShadowBake.ush` | Bake loop |
| `TR_SHADOW_DETAIL_LEVEL`, `TR_COVERAGE_SAMPLES` | 2, 4096 | `TerrestrialShadowMap.usf` | Detail fetch level, coverage samples |
| `TR_SLAB_ENTRY_PROBES`, `TR_CONE_RELAXATION` | 24, 0.9 | `TerrestrialDeck.ush` | Bake entry search |
| `MaxHistoryWeight`, `HistoryLightCosine` | 0.9, 0.9994 | `AtmosphereShadowBake.h` | Shadow history |
| `ATMO_CLAMP_GAMMA`, `_MOTION` | 4, 1 | `AtmosphereTemporal.usf` | Clamp width, σ |
| `ATMO_FILL_WEIGHT`, `_MOTION` | 0.1, 0.5 | `AtmosphereTemporal.usf` | Fill share |
| `ATMO_MOTION_PIXELS`, `ATMO_GUIDE_SHARPNESS` | 2, 100 | `AtmosphereTemporal.usf` | Motion scale, depth guide |
| `HistoryLifetime` | 300 frames | `AtmosphereViewExtension.cpp` | Unrendered history lifetime |
| `ATMO_TRANSMITTANCE_STEPS` | 128 | `AtmosphereTransmittance.usf` | Table samples per texel |

## 8. Cost

| Setting | What it scales |
|---|---|
| `CellSize` | March threads, `⌈W/N⌉ × ⌈H/N⌉`: about 1/N² of a full-resolution march. The resolve and composite run at full internal resolution whatever N. A pixel refreshes every N² frames. A frame without history marches at N ≤ 2; a view without state at N = 1. |
| `CloudSteps` | The lattice floor: steps inside the knee scale with it, those beyond with `ln`. Each cloud sample reads the field, the shadow map, the table and `OctaveCount` exponentials. |
| `LatticeGrowth`, `LatticeGrowthFar` | Cloud steps per ray about `1/g + ln(D g / f) / ln(1 + g)`: lower is finer and costlier, mostly in grazing views from in or near the deck. |
| `ChordSpread` | Cloud: higher allows longer steps from far cameras. Air: redistributes `AtmosphereSteps` without changing the count. |
| `AtmosphereSteps` | Air steps per ray. Each air sample also reads the shadow map and the table, and a coarse column where it is shadowed. |
| `OctaveCount` | One exponential per octave per cloud sample. |
| `ShadowResolution` | Bake texels per level, its square; memory 8 bytes per texel per slice, 3 slices (24 MB at 1024, 2 MB per slice at 512). Each texel marches up to 256 steps of 4 field samples. |
| `ShadowLevelsPerFrame` | Levels baked per frame, each with a slice copy for its history. |
| `ShadowTemporalSmoothing` | No cost. |

Fixed per frame: the coverage pass (4096 column reads, one group); the entry refinement (up to 7 net extra samples at each cloud entry); per view, history memory of 16 bytes per internal pixel and transient march targets of 20 bytes per marched cell. The transmittance bake (256 × 64 × 128 samples) runs only on a change.

## 9. Pitfalls

### View extension and resolve

- **Translucency is not in scene colour at `BeforeDOF`.** The extension relies on it merging at depth of field, after this pass, so it draws over the atmosphere unattenuated.
- **A history reprojected through the jittered projection moves under a still camera:** each lookup lands at the jitter's difference and re-interpolates pixel-scale detail. Rays jittered, history unjittered.
- **A revisit gap that is a multiple of 8,** the jitter period the engine's sequence is relied on to have, lands on the same sub-pixel jitter every visit. A gap of N² does for N a multiple of 4 and alternates between two jitters for other even N; N² − 1 does for every odd N. The rank rotation keeps every gap odd.
- **History is held by pointer** across the graph's execution, after other views of the family may have grown the map.
- **A resize is not a reset;** the history is read by UV.
- **Reprojected in world space, a camera riding a moving planet sees every pixel move,** and the clamp and fill run at their motion settings over the whole view. Reproject planet-fixed.
- **Clamp moments about the nearest fresh sample** are constant across a cell and stamp the cell grid into the image in motion.
- **A local named like a uniform hides it without a warning** (`AtmosphereTemporal.usf`).
- **One non-finite sample spreads** through the moments, clamp and fill into every pixel within two cells, and grows each frame. Light past half float is one too.
- **A limb ray given the sky depth reprojects as a direction** and trails behind a moving planet; the air's depth stands in.
- **Each atmosphere composites in turn** over the previous result through its own extension; nothing orders two atmospheres by depth.

### Ray setup and march

- **`CalcSceneDepth` is not used:** `AtmosphereDepth.ush` takes its epsilon to saturate depth as z / (1 + 1e-8 z) and inverts the projection exactly instead. Scene depth must be a true ray distance, or geometry sinks into the deck as the camera recedes.
- **The planet offset is formed in double;** formed from two float positions, the atmosphere steps against the scene as the camera moves.
- **Under a screen percentage the depth texels outside the view rect are treated as stale;** reads clamp to the rect.
- **A transmittance snap is required at the early exit,** or the residual leaks, coloured, on the rays that stopped.
- **Planet shadow as a hit test** draws a hard seam across the disc; branching on the terminator plane draws a line where it crosses low samples.
- **A Mie gate on the planet hit** steps the silhouette; scaling by the chord traversed leaves an opaque deck glowing.
- **Air weighted by step length** over-counts in-scatter once a step's optical depth nears 1, as on the limb.
- **At low cloud optical depth every octave sees full transmittance,** so thin cloud brightens by the sum of the weights.
- **The step plan:** a step cut at a shell is a sawtooth across the screen (rings about the camera); a per-ray chord budget jumps at the limb and shows the limb through opaque deck; steps placed by the field move with the weather. A shared lattice phase turns jitter noise into rings about the view.
- **The outer-shell crossing alone decides cloud;** any extra test on it suppresses cloud and draws a bright line at the limb.
- **A grazing ray dense enough to keep stepping but too thin to exit** stops at `ATMO_MAX_ITER`: a hard edge at the limb.
- **The detail fade is per sample, at the jittered distance;** per step it draws the lattice as rings.
- **Slab clip only over sky:** clipped where geometry was drawn, geometry below the sphere (a crater, a sea floor) draws as in-scatter over black.
- **Deep deck:** a long step through saturated deck takes one shadowed light read and the in-scatter vanishes in a ring; the terminus prevents it.
- **Blue noise** must be linear, uncompressed, without mips, read with `Load`. Advancing the value per index instead of moving the lookup gives every step the same pattern and rings about the view centre.

### Shadow map

- **A map read in a basis, radius or camera it was not written in gives smooth, plausible, misplaced shadows.** Basis, extents, centres and depths derive once, in `AtmosphereShadowMap.ush`, from the frame and the level's own camera and light.
- **(U, V, L) must be right-handed;** mirrored, it puts shadows on the wrong side of lumps.
- **Unsnapped cascade centres** slide the lattice under the medium and every crossing crawls.
- **A narrow hand-over band** shows the cascade seam as a ring.
- **Reconstruct four texels, then blend, with linear weights.** Blending nodes first creases the result on the lattice; smoothstep weights draw blocks along long shadows. No saturation snap: it draws a staircase along the shadow edge.
- **The surface read needs its radial gate;** a point behind the planet inside the disc reads the ceiling.
- **The fold constant must carry the march's thickness conversion,** or the field reads opaque everywhere. No debug remap in the bake: the march reads the map.
- **The bake reads its previous bake while writing it;** the slice copy exists for that.
- **A target with fewer than three slices** leaves inner levels unwritten; `IsUsable` refuses it.
- **`bCanCreateUAV` before the resource is created** (shadow target, transmittance table), or every dispatch silently writes nothing.
- **A pin added to `FTerrestrialFieldParameters` and missing from `MakeShadowFieldKey`** keeps a stale history.
- **Gradient-sized bake steps alone** let a dense deck put every threshold in one step; the optical-depth allowance prevents it. A constant-density crossing leaves rings where the subdivision changes; solve in optical depth per step.
- **`T / Step` can round just below a multiple,** cutting a zero step that spins the loop to its cap; the bias in the cut prevents it.
- **An under-declared slope steps over cloud** and leaves shadow holes under steep walls: raise `SlopePerTexel`.
- **The coverage pass binds no `CoverageThreshold`.** It builds the field through `TR_BUILD_FIELD`, which loads it; the priority path must not read `CoverThreshold`.

### Transmittance, light and actor

- **The table saturates at the horizon;** the planet's occlusion is analytic, and a table that held it too would darken those samples twice.
- **Clamp both axes:** wrapping blends the zenith column into the horizon and the ground row into the top.
- **Profile floors belong in `AtmoT_Profile`,** which both sides call, not in `Atmo_Density`.
- **Deep deck light below the surface** must take the buried air (`Field_AirBuried`); read at the sample's own radius, deep hollows brighten with depth.
- **The field's frame is the planet's.** Taken from the actor's rotation, aiming at a moving star turns the clouds and their spin axis with the light.
- **`SetAtmosphereActive` is the only off-switch;** hiding the actor or stopping its tick leaves it drawing.
- **One shadow map per planet, not per view:** a second viewport shares the first's camera-centred cascades.
- **Light is stored unexposed in half float:** the haze's forward lobe clips past `LightIntensity` about 1000 at `MieG` 0.95, about 40 at 0.99.

## 10. Checks

After a change to the render path:

1. `Tools/ShaderCheck/check.sh` clean on all 27 entries; diff the DXIL against the previous build where no change was meant.
2. **Still camera:** converges within a second or two at `CellSize` 4; `r.CloudAtmosphere.Temporal.Debug 1` settles white, 2 green with sparse blue.
3. **Moving camera:** no rings or terraces; ghosting only on disocclusion and fast flight through the deck, gone within a few frames of stopping. Watch the limb, eyewalls and mesh silhouettes; debug 4 shows where the clamp acts.
4. **Edge cases:** split screen, a scene capture (no history), parking and waking, a `PlanetType` switch each way including air only, editor delete and undo, screen percentage below 100.
