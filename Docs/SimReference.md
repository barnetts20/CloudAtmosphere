# Flow sim reference

How the weather sim works and why, for whoever maintains it. Per-setting descriptions are in [`Design/UsageGuide.md`](UsageGuide.md); ownership, claims and the kept fields are in [`Design/SimOwnership.md`](SimOwnership.md). The cloud field that reads the sim's output is in [`Design/FieldReference.md`](FieldReference.md), the render pipeline in [`Design/RenderReference.md`](RenderReference.md). Code is cited by file and function.

| Area | Code |
|---|---|
| Config, constants, scales | `Public/FlowSimTypes.h`, `Private/FlowSimTypes.cpp` |
| Uniform buffer, shader constants | `Public/FlowSimShaders.h`, `Private/FlowSimShaders.cpp` |
| Stepping, stack, params, start log | `Private/FlowSimSubsystem.cpp` |
| Pass graph, pooled state | `Private/FlowSimulation.cpp` |
| Snapshot asset | `Public/FlowSnapshot.h` |
| Every compute pass (`Main*CS`) | `Shaders/Private/FlowSim.usf` |
| Grid, topology, operators | `Shaders/Private/FlowSimCommon.ush` |
| Banded jet profile | `Shaders/Private/FlowSimJets.ush` |
| The atlas the cloud field reads | `Shaders/Private/FlowField.ush` |

## 1. Overview

A semi-Lagrangian, semi-implicit (SLSI) stack of rotating shallow-water layers on a longitude × sin(latitude) grid of the unit sphere, coupled through their pressure. Each layer carries advected tracers (cloud, formation ascent, vapour, storm) and two noise displacement phases. On top of the flow sit tracked **storm cells** (hurricanes), which spawn on the sim's storms and push the flow toward a vortex, and authored **perpetual storms**, which are storm cells held in closed-form positions.

Vertical motion, cloud, moisture and storms are outputs of the dynamics, not inferences from a streamfunction:

- Vertical motion comes from convergence, so nothing depends on a hemisphere sign convention.
- Drag makes air spiral into lows and out of highs (Ekman convergence) in both hemispheres.
- The Montgomery potential is the pressure field.
- `DeformationRadius` sets the eddy size: small gives many narrow systems, large a few big ones.
- Vertical shear held by thermal forcing (`ShearSpeed`, two layers or more) is the energy source of baroclinic weather.

Each frame the sim resamples its output onto a cube atlas of 4 slices per layer (flow, weather, two noise phases), which the cloud field reads through `FlowField.ush` (section 9; [`Design/FieldReference.md`](FieldReference.md)).

One sim runs per world, driven by the nearest claiming atmosphere; other planets draw the field they kept. See [`Design/SimOwnership.md`](SimOwnership.md).

## 2. Units

### The speed root and the turnover

Three authored values set every scale (`FlowSimProfile::WaveSpeed`, `UFlowSimConfig::GetSpeedRoot`, `GetTurnover`):

```
c  = DeformationRadius · PlanetaryVorticity · sin 45°     first internal mode's wave speed (floored 1e-3)
U  = SpeedRoot · c                                         the speed root (SpeedRoot floored 0.1)
T  = DeformationRadius / U = √2 / (SpeedRoot · PlanetaryVorticity)     one turnover, sim time
Ld = c / f(45°) = DeformationRadius                        deformation radius, planet radii
```

`U` is the cap of the speed ceiling (section 4), so every authored wind is a fraction of the fastest the flow may move. `SpeedRoot` is that cap in Froude number against `c`, and also the Rossby number of the fastest flow, `U / (f₄₅ Ld)`. On a stack `c` is the first internal mode's speed; the external mode is faster (section 5). On one layer `c` is the layer's.

`PlanetaryVorticity` is `2Ω` (floored 0.1 by `GetPlanetaryVorticity`); `f = PlanetaryVorticity · μ` with `μ = sin(latitude)`.

### Authored values to sim units

`UFlowSimConfig::ResolveScales` returns most of these as `FFlowSimScales`, which `UFlowSimSubsystem::BuildParams` copies into `FFlowSimParams`; `GetStepSize`, `GetNoiseResetTime`, `GetNoiseDriftRate`, `GetDivergenceDamping` and `BuildParams` (perpetual storms) convert the rest.

| Kind | Settings | Conversion |
|---|---|---|
| Jet wind | `JetSpeed` | `JetStrength = JetSpeed · U / max|rate·cos|` of the unit-strength profile with `EquatorialBoost` (`FlowSimProfile::PeakWind`, 512 rows): the peak eastward wind on a layer with `JetScale` and `BoostScale` 1 |
| Shear | `ShearSpeed` | `ThermalShear = ShearSpeed · U / max|shape·cos|` of the thermal shape |
| Speeds | `EddySpeed`, `StormCellWind`, `StormCellDriftSpeed`, perpetual `Wind`, `Drift` | `× U` |
| Genesis shear | `GenesisShearRatio` | `GenesisShearRatio · max(|ShearSpeed|, 0.1) · U` (floored 0.01) |
| Wind evaporation | `WindEvaporationGain` | `÷ U`: the gain at wind speed `U` |
| Noise drift | `NoiseDriftSpeed` | `× U`, an angular rate (`GetNoiseDriftRate`) |
| Rates | `NudgeRate`, `DragRate`, `ThermalRelaxation`, `LayerCoupling`, `SurfaceEvaporation`, `CondensationRate`, `EvaporationRate`, `StormCellSpawnRate`, `StormCellGrowth`, `StormCellFollow`, `StormCellCoreFollow`, `StormCellForcing`, `PerpetualStormForcing`, `GridDamping` | `÷ T` |
| Lifetimes | `CloudLifetime`, `StormLifetime`, `StormCellLifetime`, `ForcingLifetime`, `FlowSimNumerics::AscentSmoothing`, `NoiseResetTurnovers` (at least `2 · FlowSimStep::SpinUp`), `StepSize`, `SpinUpTurnovers` | `× T` |
| Eye tracer | `StormCellEyeRate`, `StormCellEyeTrail` | per turnover; the shader divides `Δt` by `T` (`MainPredictCS`) |
| Lengths | `StormCellRadius`, perpetual `Radius` | `× DeformationRadius`, radians; cell radius held to 0.5°–45° |
| Forcing scale | `ForcingFrequency` | `÷ DeformationRadius`: volume tiles per planet radius |
| Dimensionless | thresholds, onsets, shares, `StormAmount`, `LatentHeating`, `Stratification`, saturation ratios | unchanged |

Because rates and lifetimes are in turnovers and winds in fractions of `U`, a config keeps its balance when `DeformationRadius`, `PlanetaryVorticity` or `SpeedRoot` change: `PlanetaryVorticity` mostly sets tempo, `DeformationRadius` the size of systems against the planet and grid, `SpeedRoot` how close the winds run to the cap.

### The speed budget

The ceiling leaves faces untouched below `0.7 U` (`FlowSimShader::FroudeKnee`) and eases them toward `U` above it. Authored winds belong under the knee, so the ceiling catches outliers rather than shaping the flow. The start log (`ReportCourant`) reports, as fractions of `U`, the top layer's peak zonal wind (jets, boost and its shear share, `LayerPeakWind`), its eddies (`EddySpeed · EddyScale`) and the cells' `StormCellWind`, and warns when the top layer's wind plus eddies, `StormCellWind` or any perpetual `Wind` passes 0.7. Past the knee the ceiling drags the zonal mean and clips eddy and vortex peaks.

### Output scales

`BuildParams` normalises the output against the root's physical scales, each times a constant in `FlowSimOutput` that sets the cloud field's calibration:

| Channel | Scale |
|---|---|
| Pressure | `2.930717 · U · c` |
| Vorticity | `1.276773 · U / Ld` |
| Divergence (and W) | `0.664986 · (U / c) · U / Ld` |

"1" is then the same share of the fastest flow in every regime.

## 3. Grid, state and the step

### Grid

`GridResolution` is rounded down to a power of two in [16, 512] (`FlowSimShader::GridResolution`); it is the atlas face edge `F`. The sim grid is `4F` longitude columns by `2F` latitude rows, `LayerCount` (1–8) layers, layer 0 on top. The limits come from the Helmholtz solve: `N_lon` a power of two up to 2048 (radix-2 FFT in group shared memory), `N_lat` up to 1024 (column solve). A grid change reallocates the state and resets (`StepSimulation` → `ResetSimulation` → `QueueInitialState`): `InitialState` is restored if it matches the new grid, otherwise the sim seeds and spins up.

Topology (`FlowSimCommon.ush`):

- Rows are equal steps in `μ`: equal-area cells, so a zonal mean is a plain row mean, and a Laplacian with no cross terms.
- Cell-centred, so the poles are cell **faces** where every flux carries `1 − μ² = 0`; no operator needs a polar boundary condition.
- Longitude wraps; latitude folds over the poles with a half-turn in longitude (`SimWrapCoord`: row −1 is row 0 at `λ + π`). Only scalars and Cartesian vectors are read through the fold.

**C-grid for the gravity step.** `u` on west faces, `v` on south faces, scalars at centres. Row 0's `v` is the south pole and is zero; the north pole face is not stored. `div(grad)` is then exactly the compact five-point Laplacian (`SimLaplacianCoeffs`), so the Helmholtz operator and the velocity correction are consistent by construction.

**Cartesian for advection.** Reconstruct builds a 3D centre velocity from the faces; advection backtraces along great circles (`SimBacktrace`), samples bicubically in index space (`SimSampleCubic4`) and parallel-transports vectors to the arrival point (`SimTransport`, Rodrigues), which supplies the sphere's metric terms with no pole singularity.

**Centre velocity** is fourth order from the two faces either side along each component's own axis, `(9(a + b) − (a′ + b′)) / 16`, zero past the polar faces (`SimCentreComponents`).

### State and resources

Pooled render targets and one structured buffer, owned by `FFlowSimulation` (32-bit throughout; the atlas is the only 16-bit target).

| Resource | Format, size | Contents |
|---|---|---|
| `Face[2]` | RG32F, `N_lon × N_lat × L` | `u` west face, `v` south face; ping-pong, flips three times a substep |
| `Phi` | R32F | Thickness anomaly `φ`; written by Correct |
| `Tracer[2]` | RGBA32F | Cloud `q`, `q ×` formation ascent, vapour, storm; flips once a substep |
| `Noise[2]` | RGBA32F, `2L` slices | Phase A then phase B displacement xyz; A's w the low-passed ascent, B's w the eye tracer; flips with `Tracer` |
| `Cells` | 256 float4 | Storm cell state and control (below) |
| `Centre` | RGBA32F | Cartesian centre `V` in xyz, divergence in w |
| `Explicit` | RGBA32F | The explicit half of the step (section 4) |
| `PhiStar` | R32F | `φ*` from Predict; after the solve, modal amplitudes |
| `Rhs` | R32F | Filtered `φ*`, then the modal right-hand side |
| `Spectrum[2]` | RG32F | Row spectra (wavenumber, row, mode) |
| `RowMean` | RG32F, (row, layer) | Zonal means of `u` and `φ` |
| `MontgomeryEq` | R32F, (row, layer) | Balanced Montgomery potential, global mean removed |
| `GlobalMean` | R32F, (1, layer) | Area mean of `φ` |
| `CellFlow` | RG32F, `N_lon × (N_lat + 1) × L` | Cells' streamfunction at corners, velocity potential at centres |
| `CellColumn` | RGBA32F, `N_lon × N_lat` | Storm floor, eye source, cloud feed, near flag |
| `LatLon` | RGBA32F, `4L` slices | Output on the sim grid; holds the previous state's after a frame's steps |
| `CentreLatest`, `LatLonLatest` | as `Centre`, `LatLon` | The latest state's, written after the frame's steps |

The state proper is the faces, `φ`, the tracers, both noise phases (w channels included) and the cells' state entries; everything else is rebuilt within a substep.

### Cell buffer

`FlowSimShader::CellStateStride` 2, `CellControlStride` 6, `MaxStormCells` 32: 64 state entries then 192 control entries (`CellBufferSize` 256 float4, `SIM_CELL_BUFFER_SIZE`).

| Entry | Contents |
|---|---|
| `2s` | Centre (unit vector) xyz; intensity w, 0 for a free slot |
| `2s + 1` | Age (sim time) x; last step's move yzw |
| `64 + 6s + 0, 1` | Vortex gain per layer: layer `l` in component `l & 3` of entry `l >> 2` |
| `64 + 6s + 2, 3` | Inflow gain per layer, same packing |
| `64 + 6s + 4` | Health (`SIM_CELL_HEALTH`): window, humidity favour, parent favour, favour |
| `64 + 6s + 5` | Low (`SIM_CELL_CORE`): mean pressure on the eyewall ring, least pressure inside it, the measuring ring in radii, the eased rotation sense |

Perpetual storms take slots `0 … PerpetualCount − 1`; hurricanes the next `MaxStormCells`, up to 32 in all (`BuildParams`). MainCellsCS zeroes every control block each step and rewrites the live ones; slots past the cell count are cleared.

### Step order

`FFlowSimulation::Enqueue_RenderThread` builds one graph a frame.

| # | Pass | When | Does |
|---|---|---|---|
| 0 | `MainInitBalanceCS` | when its inputs change (`BalanceKeyOf`) | Balanced Montgomery potential per layer |
| 0 | `MainInitStateCS` or `MainRestoreCS` | on start, reset or grid change | Seed balanced, or upload a snapshot |
| 1 | `MainReduceRowsCS`, `MainReduceGlobalCS` | each substep | Zonal means of `u`, `φ`; global mean of `φ` |
| 2 | `MainReconstructCS` | each substep | Centre `V` and divergence, the explicit field, the lat-lon output (incl. W's low-pass) |
| 3 | `MainCellsCS` | each substep | One group, a thread per slot: place perpetual storms, spawn, move, grow, measure lows and gains |
| 4 | `MainCellFieldCS` | each substep when `MaxStormCells` > 0 or any perpetual storm is configured, live or not; otherwise skipped and Predict reads no cell fields | Cells' `ψ`, `χ` and column terms for Predict |
| 5 | `MainPredictCS` | each substep | Advection and every explicit term: `V*`, `φ*`, tracers, noise |
| 6 | `MainFilterCS` | each substep | Polar box filter on `u`, `v`, `φ*` |
| 7 | `MainRhsCS` | each substep | `φ* − αΔt D ∇·V*`, projected onto the modes; a thread per column |
| 8 | `MainHelmholtzForwardCS`, `…ColumnCS`, `…InverseCS` | each substep | Row FFT, tridiagonal solve per wavenumber and mode, inverse FFT |
| 9 | `MainCorrectCS` | each substep | `V^{n+1}`, `φ^{n+1}` from the modal amplitudes |
| 10 | Reduce, `MainReconstructCS` (latest) | after steps, or when a parameter changed (`LatestKeyOf`) | `CentreLatest`, `LatLonLatest`; no explicit field |
| 11 | `MainResampleCS` | every frame | Blend previous and latest onto the atlas, add the cells' stamp |
| 12 | `MainDebugVisCS` | with `bDebugView` | Debug view on the grid |

Each substep gets its own uniform buffer at its own time; the passes after the loop use the frame's. With zero substeps (paused, or a low `SimSpeed` frame) the resample still runs, the latest reconstruct reruns only if a parameter changed (`LatestKeyOf`), and the debug pass runs whenever the debug view is on, so a paused sim stays inspectable.

### Noise phases

Two displacement fields per layer carry the cloud field's noise with the flow. Predict advects each: the coordinate `normalize(rotZ(d(departure) + departure, NoiseDriftRate · Δt))`, less the arrival point. A phase resets to zero at the step its clock wraps (`SimNoiseResets`; `NoiseClock = t / NoiseResetTime mod 2`, phase B half a period behind), which is where the reader's crossfade weight for it is zero (`Flow_NoiseWeightA`). The reset period in turnovers bounds the winding per reset, since warp grows as strain (∝ `U / Ld`) times the period.

### Time control

`UFlowSimSubsystem::StepSimulation`:

- **The step is set apart from the speed.** Every running step is `StepSize` turnovers, held to [`FlowSimStep::Min` 1e-5, `SpinUp` 0.044] (`GetStepSize`). `SimSpeed` is sim time per real second: each frame owes `SimSpeed · min(frame time, 0.1 s)`, takes the whole steps that covers, and carries the remainder. Speed sets the steps per frame and the cost, never the look.
- **Output blend.** The remainder's share of a step is `StateBlend`; the resample blends the previous and latest states by it, and storm cells are drawn at `latest − (1 − StateBlend) · last move` (`SimCellDisplayCentre`). `GetDisplayTime` is `SimulatedTime − (1 − StateBlend) · step`; the field's noise and spin clocks run off it.
- **Limits.** Above 64 steps a frame (`WarnPerFrame`) the log warns once; above 2048 (`MaxPerFrame`) the excess is dropped and the sim runs slower than asked.
- **Clock.** A step's time is its index from an anchor, `AnchorTime + (i − AnchorStep) · Δt`, so it does not depend on how frames grouped the steps; the anchor resets when the step size changes. The forcing cycle and noise clock are wrapped in double on the CPU (`FillCommonParameters`).
- **Spin-up.** Without a restored snapshot the sim first runs `ceil(SpinUpTurnovers / 0.044)` steps at the spin-up step, `MaxSpinUpStepsPerFrame` a frame, shown at blend 1. A pause holds it.
- **Manual steps.** `FlowSim.Step N` queues `N` steps at the running step, up to 64 a frame, shown at blend 1; a paused sim stays paused.
- **Pause.** `bPaused` or `r.FlowSim.Paused` stops stepping; output and debug passes keep running.

## 4. Equations and numerics

### Equations

Per layer `k`, non-dimensional on the unit sphere; `V` tangent velocity, `φ` thickness anomaly in geopotential units, `D_k` the layer's mean depth (so the layer is `D_k + φ` deep), `M_k` its Montgomery potential (section 5):

```
DV/Dt = −f k̂×V − ∇M_k + nudge + stirring − r_k V′ + κ(V̄_nbr − V) + cell pushes + divergence damping
Dφ/Dt = −D_k ∇·V − φ ∇·V + F_{k+1} − F_k
```

- `r_k = DragRate · DragScale_k`; `V′` is `u` less its zonal mean, and all of `v`. The zonal mean of `u` belongs to the nudge: dragging it too settles the two at a compromise below the profile.
- Nudge: `NudgeRate · (u_target − ū)` on `u` only, where `u_target = R_k(μ) cos φ` (`SimZonalRate`).
- `κ = LayerCoupling`, toward the mean of the genuine vertical neighbours (`SimCouplingDelta`).
- `F_i` is the diabatic mass flux up across interface `i` (section 6).

### The semi-implicit step

Only the linear gravity terms (`−∇M`, `−D ∇·V`) are implicit, off-centred with weight `α`. Coriolis is explicit, an exact rotation of `V` about the local vertical by `−f Δt` (`SimAdvectVelocity`), energy-conserving at any `f Δt`.

```
Reconstruct   E = ( V − (1−α)Δt ∇M ,  φ − (1−α)Δt D ∇·V − Δt φ ∇·V )        at centres
Predict       V* = face component of Rotate_f(Transport(E.xyz at departure)) + explicit terms, then the ceiling
              φ* = E.w at departure + Δt (F_in − F_out) − ⟨φ⟩,  floored at −0.9 D
Filter        polar box filter on u, v, φ*
Rhs           R_k = φ*_k − αΔt D_k ∇·V*_k,   R̂ = R⁻¹ R
Helmholtz     (I − s_m L) ψ_m = R̂_m,   s_m = (αΔt c_m)²
Correct       V^{n+1} = V* − αΔt ∇(A R ψ),   φ^{n+1} = R ψ
```

The explicit gradient in `E` is the C-grid's own (centre average of the face gradients). Subtracting the layer's global-mean `φ` each step corrects mass drift.

`α` is `FlowSimNumerics::ImplicitWeight` (0.6), raised to `FlowSimStep::StackWeight` (0.75) on a stack when the step exceeds `StackLargeStep` (0.0102 turnovers), which includes spin-up (`GetImplicitWeight`).

### Helmholtz solve

Direct, not iterative. Every coefficient of `I − sL` depends on latitude alone, so an FFT along each row separates the problem into one tridiagonal system in latitude per zonal wavenumber `m`:

```
−s A_lo p_{j−1} + (1 + s(A_lo + A_hi) + s C_lon · 4 sin²(π m / N)) p_j − s A_hi p_{j+1} = R̂_j
```

solved by parallel cyclic reduction in group shared memory (`MainHelmholtzColumnCS`), stable without pivoting because the system is strictly diagonally dominant. `A_lo` vanishes on the south polar row and `A_hi` on the north, so neither end couples outside the column. Exact to float precision at any `c Δt`. Each mode solves in its own slice at its own `s_m` (`LayerState[m].y`).

### Polar filter

Each row is box-filtered in longitude over `FilterLatitude / cos φ` columns (`SimFilterHalfWidth`): half-width `round((FilterLatitude / cos φ − 1) / 2)`, zero where `cos φ ≥ FilterLatitude`, so it first acts where `cos φ` falls to half of `FilterLatitude` (63° at 0.9). It removes the modes the converging columns cannot support, which are also the stiffest in the solve. `v` filters at its face's latitude. A running mean is conservative in the row, so it does not disturb the zonal means the nudge acts on. `FilterLatitude` 0 disables it.

### Grid damping

`GridDamping` is the rate, per turnover, at which the first internal mode's grid-scale waves decay, whatever the step. Two mechanisms supply it (`UFlowSimConfig::GetImplicitDampingRate`, `GetDivergenceDamping`):

```
implicit:   |g|² = (1 + (1−α)² A) / (1 + α² A),   A = (c Δt)² S_eq,   S_eq = 2/Δλ² + 2/Δμ²
            rate_imp = ln(1 / |g|²) / (2 Δt)
divergence: fraction = min(1 − exp(−max(GridDamping / T − rate_imp, 0) · Δt), 0.45)
```

`S_eq` is the Laplacian's diagonal at the equator (`EquatorStiffness`, `SimRowStiffness`). At small steps divergence damping carries nearly all of it; at large steps the implicit scheme alone may exceed the rate, which the start log reports. The cap 0.45 (`FlowSimShader::DampingMax`) is the explicit scheme's stability bound; the log warns when it binds. Longer waves lose less, as their scale squared.

Divergence damping is applied per face as a gradient of the divergence scaled per row (`SimDampEast`, `SimDampNorth`), so the row's grid-scale mode loses exactly the fraction:

```
Δu = f_face · ∂δ/(cos φ ∂λ) / S_row        Δv = f_face · cos φ ∂δ/∂μ / max(S_row, S_row−1)
```

A single coefficient tuned at the equator would be thousands of times too strong in the polar rows.

### Bore suppression

A shallow-water flow near its wave speed steepens into bores that travel as sharp lines. Two structural guards, each set by one dimensionless value:

- **Speed ceiling** (`SimSpeedCeiling`, end of Predict, every face): with `s` the speed from the face component and the across-face component of the centre average,

  ```
  s′ = s                                   s ≤ k,  k = 0.7 U
  s′ = k + (U − k) tanh((s − k)/(U − k))   above
  ```

  applied as the factor `s′/s`, C1 at the knee. The uniform `SpeedRoot` carries the setting; the cap is `SpeedRoot · c = U`.
- **Compression damping** (`SimFaceDamping`): the face's damping fraction is `min(fraction + ShockDamping · max(−min(δ₁, δ₂), 0) · Δt, 0.45)`, von Neumann–Richtmyer style: it engages only where a front compresses, in proportion. `ShockDamping` is `FlowSimNumerics::ShockDamping` (2).

The ascent low-pass (section 6) keeps whatever gravity waves remain out of the cloud.

### Stirring

A divergence-free stochastic forcing against the drag (`SimForcingSlope`). `ForcingVolume`'s channel `ForcingChannel`, decoded to [−1, 1], is a streamfunction; the velocity is `k̂ × ∇ψ` (east gets `−slope` along north, north `+slope` along east), by central difference one volume texel either side. Two phases half a `ForcingLifetime` apart, each a fresh pattern at a hashed offset per (cycle, phase, layer), carried east at the row's zonal-mean angular rate for the cycle's age, crossfaded by triangle weights renormalised to keep the variance. The rate is `EddySpeed · U · DragRate · EddyScale · DragScale`, so with drag `DragRate · DragScale` the equilibrium eddy speed per unit noise slope is `EddySpeed · EddyScale · U` whatever the drag. No volume bound, or `DragRate` 0, is no stirring.

### Frozen constants

| Constant | Value | Role |
|---|---|---|
| `FlowSimNumerics::ImplicitWeight` | 0.6 | `α`; 0.5 neutral, above damps gravity waves |
| `FlowSimNumerics::ShockDamping` | 2 | Compression damping gain |
| `FlowSimNumerics::AscentSmoothing` | 1.527 turnovers | Along-flow low-pass of W |
| `FlowSimStep::Min`, `SpinUp` | 1e-5, 0.044 turnovers | Step range; spin-up step |
| `FlowSimStep::StackLargeStep`, `StackWeight` | 0.0102 turnovers, 0.75 | `α` floor on a stack at large steps |
| `FlowSimStep::WarnPerFrame`, `MaxPerFrame` | 64, 2048 | Steps-per-frame warning, hang guard |
| `FlowSimShader::FroudeKnee` | 0.7 | Share of `U` below which faces are untouched |
| `FlowSimShader::DampingMax` | 0.45 | Damping fraction cap |
| `SIM_CLOUD_ASCENT_FLOOR` | 0.05 | Cloud below which formation altitude fades instead of dividing |
| `SIM_STORM_HEMISPHERE` | 20 | Sharpness of the storm tracer's sense through the equator, and of a three-cell seed's |
| `SIM_SHEAR_SENSE_EASE` | 0.5 | Background vorticity, in jet rates, over which a banded cell's sense eases |
| Thickness floor | `φ ≥ −0.9 D` | A layer may thin, never invert |
| `FlowSimOutput` | 1.276773, 2.930717, 0.664986 | Output scale constants (section 2) |
| Three-cell shape | jet 0.71 ± 0.17, trades 0.33, polar 0.97 ± 0.09 in `μ` | `SimThreeCellRate`, `FlowSimProfile::JetRate` |
| Banded asymmetry frequency | 0.6 | Odd term's frequency, `SimJetRaw` |

The storm cell constants are listed in section 7.

## 5. The layer stack

### Montgomery potential and modes

Layer `k` feels the free surface plus the density step of every interface above it (`BuildStack` in `FlowSimSubsystem.cpp`):

```
M_k = Σ_j A_kj φ_j,     A_kj = 1 + ε · min(k, j),     ε = Stratification (0.01–1)
```

Interface `i` is the top of layer `i`, at height `ζ_i = Σ_{j≥i} φ_j` above the bottom. A small `ε` keeps the free surface nearly flat, so pressure systems ride on the interfaces.

The implicit operator is `diag(D) A`, similar to the symmetric `D^½ A D^½`, decomposed by cyclic Jacobi in double precision. Its eigenvectors are the vertical modes, sorted fastest (external) first; `R` maps modes to layers. The depths are `DepthScale` shares (normalised) scaled so the first internal mode's speed is exactly `c` (the only mode's, on one layer). The subsystem builds `A`, `A⁻¹`, `R`, `R⁻¹` and `A R` every frame into 8×8 row-major uniforms. One layer is one mode and reduces exactly to single-layer shallow water.

### Zonal profile

Each layer's target angular rate is the jet profile plus its share of the thermal shear (`SimZonalRate`), the same on the CPU (`LayerZonalRate`) and GPU:

```
R_k(μ) = jets_k(μ) + share_k · ThermalShear · shape(μ),     share_k = (L − 1 − k)/(L − 1): 1 on top, 0 at the bottom
```

- **Banded** (`SimBandedRate`, `FlowSimJets.ush`): `cos(Kμ) + JetIrregularity · cos(K · JetHarmonic · μ) + Asymmetry · sin(0.6 Kμ)` with `K = BandCount · π`, scaled by `1/(1 + JetIrregularity)`, less `WidthBias`, saturated by a C2 quintic above the knee `1 − JetFlatness` (width `2 · JetFlatness`, topping out at 1), plus `EquatorialBoost · BoostScale · exp(−μ² ln2 / sin²(EquatorialJetWidth))`, times `JetStrength · JetScale`.
- **Three cell** (`SimThreeCellRate`): Gaussians in `μ`, a westerly jet less `TradeWindStrength` trades and `PolarEasterlyStrength` easterlies, latitudes stretched by `45° / JetLatitude`.
- **Thermal shape** (`SimThermalShape`): Midlatitude, `exp(−((|φ| − BaroclinicLatitude)/BaroclinicWidth)²)`; FollowJets, the jet profile at unit strength.

The profile is a function of `μ`, so jets are equal-area and do not crowd the poles.

### Thermal forcing and balance

`MainInitBalanceCS` integrates each layer's gradient-wind balance on the C-grid's own faces, one thread per layer:

```
M_j − M_{j−1} = −Δμ · μ_f · R(μ_f) · (PlanetaryVorticity + R(μ_f))
```

which is exactly what Correct's discrete north gradient measures, so the balanced state holds with nothing to adjust. The global mean is removed. `φ_eq = A⁻¹ M_eq` gives each layer's balanced thickness, and the vertical shear lands in the interfaces. The pass reruns whenever its inputs (grid, jets, shear, layer profiles, rotation) change.

`ThermalRelaxation` relaxes the interfaces toward balance by moving mass between neighbouring layers (`SimInterfaceFlux`), with latent heat as a second term (section 6):

```
F_i = ThermalRelaxation · (ζ_i − ζ_i^eq) + LatentHeating · D_i · condensed_i       mass from layer i into i − 1
dφ_k/dt += F_{k+1} − F_k
```

Each column's total is untouched. On a single layer the flux is open below: `φ` relaxes toward `φ_eq` and latent heat draws mass up into the layer.

The seed (`MainInitStateCS`) is the balanced zonal flow on the faces, `φ_eq`, no cloud or storm, vapour at `CondensationOnset` of saturation, zero noise and empty cells.

### Vertical motion

Ascent at the middle of layer `k`, normalised by half its depth (`SimLayerVerticalMotion`): the lift of every layer below, half its own convergence, and its own flow across the sloping surfaces below, the slantwise ascent of air riding over a colder layer.

```
w_k = SoftSat( [ −½ h_k δ_k + Σ_{j>k} ( −h_j δ_j + (V_k − V_j) · ∇φ_j ) ] / (½ h_k) / DivergenceScale )
```

with `h = D + φ` and `SoftSat(x) = x / √(1 + x²)`. On the bottom layer it is `−δ`, normalised.

### Layer profiles

`LayerProfiles` is resampled over the stack: the first entry is the top layer, the last the bottom, those between interpolate; an empty list is unscaled (`LayerOf`). Per layer the shader gets `(JetScale, BoostScale, EddyScale · DragScale, DragScale)`, and in `LayerState` the depth, the mode slice's Helmholtz scale and the saturation factor `UpperSaturation^share_k`.

### Checks in the start log

`ReportStack`:

- The modes' wave speeds.
- **Storm criterion.** Two equal layers grow baroclinic waves once the shear wind exceeds about `2 β Ld²`, `β = PlanetaryVorticity · cos φ`, `Ld = c / f` there. Judged at `BaroclinicLatitude` (held to 10°–80°), or under FollowJets at the strongest shape between 10° and 80°; the log gives the ratio.
- **Thinning.** It integrates the same balance on the CPU and warns when a balanced layer thins below a quarter of its depth: the interface nearly reaches the edge of the stack, where the thickness floor clips it.

## 6. Moisture and storms

### Tracers

Four channels per layer, advected together, sampled bicubically and clamped to the four nearest centres' range so the cubic cannot make negative cloud (`SimSampleBounded4`):

| Channel | Meaning |
|---|---|
| x | Cloud fraction `q`, [0, 1] |
| y | `q` times the formation ascent, the mean W the cloud condensed at; never above `q` |
| z | Vapour, in units of the equator's bottom-layer saturation |
| w | Storm intensity, [0, 1] |

Every tracer source integrates in closed form (`SimRelax`): exact where the rate is linear in the value and never past its zero, at any step.

### Saturation and condensation

```
q_sat(μ, k) = UpperSaturation^share_k · lerp(SaturationPoleRatio, 1, cos²φ)          (SimSaturation)
C = CondensationRate · ( max(W, 0) · onset(RH) + max(RH − 1, 0) )                   (SimCondensation)
onset = saturate((RH − CondensationOnset) / (1 − CondensationOnset))
```

`C` is per unit time as a fraction of saturation. Rising air condenses near saturation; any excess condenses outright. In `MainPredictCS`:

- Cloud forms at `C (1 − q)` and is lost at `EvaporationRate · max(−W, 0) + 1 / CloudLifetime` (sinking evaporation, rain-out).
- The formation channel gains `C (1 − q) · max(W, 0)` and loses at the same loss rate.
- Vapour loses `C · q_sat` and regains cloud evaporated by sinking air, `EvaporationRate · max(−W, 0) · q · q_sat`; rained-out cloud is lost.
- The surface evaporates into the bottom layer at `SurfaceEvaporation · (1 + WindEvaporation · |V|) · max(q_sat − vapour, 0)`, `|V|` the centre speed: a storm's own winds feed its moisture.
- The diabatic flux carries vapour with the mass it moves: mass rising in from below brings the lower layer's vapour, mass sinking in from above the upper layer's (upwind), over the layer's thickness.

### Latent heat

The second term of `F_i`: a layer's condensation lifts `LatentHeating · D_i` of mass per unit vapour condensed across the interface above it, which lowers pressure under the condensing air, deepens the convergence feeding it and raises pressure aloft. The condensation it reads is smoothed over the centre (½) and its four neighbours (⅛ each), which has no response to the grid-scale checkerboard (`SimCondensedSmooth`).

### Ascent low-pass

Everything that reads W (condensation, latent heat, the output, the deck) reads it low-passed along the flow. Reconstruct eases the value carried to each centre toward this step's, `W = lerp(carried, w_k, 1 − exp(−Δt/τ))`, `τ = 1.527` turnovers, and writes it to the weather slice; Predict carries that from the departure point in noise phase A's w. What moves with the air (fronts, lows, storm updraughts) holds; gravity waves and bores pass each parcel briefly and average out. Snapshots store the carried value.

### Storm tracer

Storms grow where condensation is intense, more so in cyclonic spin, with the hemisphere's sense on either profile (`SimSense(μ, 20)`), so a banded planet's storms favour its belts:

```
intensity = C / CondensationRate · (1 + StormSpin · max(ζ̂ · sense(μ), 0))
drive     = saturate((intensity − StormThreshold) / (1 − StormThreshold))
dS/dt     = a · drive · (1 − S) − S · (1/StormLifetime + EvaporationRate · max(−W, 0)),   a = StormAmount / StormLifetime
```

`ζ̂` is the normalised vorticity the output carries. A column held at full drive settles at `StormAmount / (1 + StormAmount)`. The genesis storm is `GenesisStormRatio` of that (`GetGenesisStorm`, held to [0.01, 1]).

## 7. Storm cells

A hurricane is a few grid cells across at game resolution, below the deformation radius. Spun up to a fixed target at a high rate it reorganises the planet's weather; left to the storm tracer the flow shears it into streaks. So a storm cell is a **tracked centre riding on a storm the sim made**, pushing the flow toward a vortex closed-loop, and the flow then winds the weather around it.

### Slots and traits

32 slots (`FlowSimShader::MaxStormCells`); perpetual storms first, then up to `MaxStormCells` hurricanes. `SimCellTraitsOf` gives every slot its radius, aspect, wind, sense, storm, cloud lift, pressure, inflow, eye share and closing share per step (`1 − exp(−forcing · Δt)`). Hurricanes take the config's; perpetual storms their own (section 8). Every per-texel read goes through `SimCellReach`, which returns the slot's centre, sense, intensity and distance `R` in radii (across its ellipse for a perpetual storm), using `atan2` since `acos` loses small angles.

The shape settings shared by every slot: radius `R` (`StormCellRadius`), eyewall `StormCellEyewall` (held just outside the eye ramp, `SimCellWall`), eye ramp `max(StormCellEyeRatio · eyewall, StormCellEyeStrength · one grid cell)` (at most 0.9 of the eyewall), `StormCellEyeStrength`, `StormCellFalloff`, `StormCellWindBreadth`, `StormCellTopShare`.

### Rotation sense

- Three cell: the hemisphere's, `SoftSat(40 μ)` (`SIM_CELL_HEMISPHERE_SHARPNESS`: 0.8 of full at 2°, 0.96 at 5°), so a cell drifting along the equator fades rather than flipping.
- Banded: the shear of its band, `SoftSat(ζ_bg / (0.5 · |JetStrength|))` with `ζ_bg = 2μR̄ − (1 − μ²) dR̄/dμ` of the layers' mean profile (`SimSpinSense`): cells alternate band to band and ease through each jet core.

MainCellsCS stores the sign and the eased magnitude; the magnitude scales the vortex's targets and units, so the loop asks only for what the eased push delivers.

### Spawn

In `MainCellsCS`, one spawn a step: the first free hurricane slot draws with chance `StormCellSpawnRate · Δt · free / hurricane slots`, hashed from `StepIndex · 32 + slot`.

1. Draw 8 points (`SIM_CELL_SPAWN_DRAWS`) uniform in latitude within `GenesisLatitudeMin`–`Max`, either hemisphere, any longitude; keep the best **seed score** (`SimCellSeedScore`): the bottom layer's column storm, if at least the genesis storm, times `saturate(cyclonic ζ̂ / GenesisSpin)` (1 when `GenesisSpin` is 0); or, with `GenesisMoisture` above 0, at least `genesis storm · GenesisMoisture · saturate((RH − RH_g)/(1 − RH_g))`.
2. Refine onto the storm's peak: three six-point ring searches from half a radius, halving, scoring `storm · (1 + cyclonic)`.
3. Hold only where, at the point, the genesis window exceeds 0.5 (`SIM_GENESIS_WINDOW_GATE`), the bottom layer's RH exceeds `RH_g = CondensationOnset + GenesisHumidityMargin`, no live cell is within `StormCellSpacing` radii and no perpetual storm within `PerpetualStormClearance` of its reach.

A new cell starts at intensity 0.1 with its health written, so the health view shows it from its first step.

### Conditions and health

```
window   = band(φ) · saturate(1 − |V_top − V_bottom| / GenesisShear)          (SimGenesisWindow)
band     = fades over 4° (0.07 rad) equatorward and 8° (0.14 rad) poleward of the genesis latitudes
humidity favour = saturate((RH − F) / (1 − F)),   F = RH_g − StormCellDryTolerance
parent favour   = saturate(max(parent / genesis storm, GenesisMoisture · saturate((RH − RH_g)/(1 − RH_g))))
favour   = window · humidity favour · parent favour,   0 past StormCellLifetime
```

A live cell reads its window and RH as means over a six-point ring at half its radius (`SimCellConditions`), which cancel its own vortex and inflow; a seed reads them at its point. `parent` is the strongest column storm at the centre, on the half-radius ring and on the eyewall ring, where the cell's own inflow converges and feeds it. Health is stored per slot for the Storm cell health view.

### Motion

Each step a live cell moves by (`MainCellsCS`):

- **Steering**: the layers' centre velocity over the half-radius ring, each layer weighted by the magnitude of its vortex share, so the cell follows the levels its vortex lives in.
- **Drift**: `StormCellDriftSpeed · U` poleward and west.
- **Parent follow**: `StormCellFollow` times the offset to the storm-weighted centroid of the centre and the half-radius ring.
- **Core follow**: `StormCellCoreFollow` times the offset to the bottom layer's vortex core, the centroid weighted by cyclonic vorticity squared over the centre and three rings out to the eyewall (at least two grid cells). The flow's vortex drifts on its own, poleward and west, and the one-sided push never drags it back; the pull keeps the push, the stamp and the eye on the flow's centre of rotation.

The move is a great-circle step along the summed velocity.

### Intensity, lifetime, merging

```
dI/dt = StormCellGrowth · favour · (1 − I) − StormCellGrowth · (1 − StormCellPersistence) · (1 − favour) · I
```

Favour is zero inside a stronger slot's reach (a hurricane's radius, a perpetual storm's reach; a perpetual storm is always the stronger), so cells that meet merge. Below 0.05 (`SIM_CELL_FREE`) the slot frees. Past `StormCellLifetime` a cell fades over about `ln 20 / (StormCellGrowth · (1 − StormCellPersistence))`; at `StormCellPersistence` 1 it never decays, and once every slot is full none can spawn (start-log warning).

### Closed-loop gains

`SimCellWriteControl`, about the new centre, for every live slot:

- **Rings.** The eyewall ring sits at the eyewall, or two grid cells out where the eyewall is finer (`SIM_CELL_RING_CELLS`), at most 0.9. The band ring sits halfway from the wind's hold to the radius, no closer than the first, at most 0.9. Each is six points; the full ring cancels the cell's own motion. A perpetual storm's rings are ellipses of its aspect, and each point's wind is taken over what the push adds there at gain 1.
- **Vortex gain.** On each ring, each layer's mean wind along the slot's sense is compared with its target `share_k · s · Wind · p_held(r_ring)`, where `share_k` runs from `StormCellTopShare` on top to 1 at the bottom and `s = saturate(I / StormCellMaturity)`:

  ```
  gain = clamp((target − measured) / unit, ±2)     then one-sided to the target's sign     (SimCellTowardTarget)
  ```

  `unit = Wind · p_held(r_ring)`, so a gain of 1 is the push's own wind there; target and unit both carry the eased sense's magnitude. The layer takes the ring with the larger gain. `SIM_CELL_GAIN_LIMIT` (2) bounds the push while the flow is far from target.
- **Inflow gain.** The eyewall ring's mean inward wind against `radial_k · T_r`, `radial_k` from −1 on top to +1 at the bottom (none on one layer), `T_r = StormCellInflow · s · Wind · p_held(r_ring)`, cut back so that it and the vortex target stay under `0.9 U` (`SIM_CELL_TARGET_CEILING`): `|T_r| ≤ √((0.9 U)² − (s · Wind · p_held)²)`. The unit is `Wind · p(r_ring)` on the stamp profile. The start log warns when a full-strength cell's inflow is cut.

The loop is one-sided: a layer below its target is pushed toward it and one at or past it is left alone, whatever its drag, so a young cell never spins down the stronger storm it spawned on. `StormCellForcing` sets only how fast.

### Vortex and inflow

The vector profile `p(r)`, `r` in radii (`SimCellProfileHeld`):

| Range | Strength |
|---|---|
| `r < eye` | `StormCellEyeStrength · r / eye` |
| `eye ≤ r < eyewall` | `StormCellEyeStrength` to 1, smoothstep |
| `eyewall ≤ r < hold` | 1 |
| `hold ≤ r < 1` | `(1 − t)^StormCellFalloff`, `t` from 0 at `hold` to 1 at the radius |

The wind uses `hold = lerp(eyewall, 1, StormCellWindBreadth)`, held so the fall-off spans at least two grid cells (`SimCellWindHoldFor`); the stamp profile (cloud, storm, draft, inflow) holds only at the eyewall.

Both pushes integrate in closed form (`SimCellIntegralHeld`), so `MainCellFieldCS` writes, per layer:

```
ψ_k = Σ share · Wind · R · sense · gain_k · (G_held(r) − G_held(1))       streamfunction at cell corners (SimCellStream)
χ_k = −Σ share · Wind · R · inflowgain_k · (G(r) − G(1))                    velocity potential at centres (SimCellPotential)
```

`G` the profile's integral from the centre and `share = 1 − exp(−StormCellForcing · Δt)` (each slot its own rate), so the relaxation is exact at any step. Predict adds the C-grid curl of `ψ`, exactly divergence-free (the vortex spins the flow without raising the surface), and the gradient of `χ`, `inflowgain · Wind · p(r)` toward the centre. Inflow below and outflow above turns the vortex's rings into trailing spiral bands at an inflow angle of `atan(StormCellInflow)`; its convergence across the core condenses, and latent heat lifts that mass across the interface, which keeps the storm tracer under the core alive. Everything is gated on `SimCellNear` (within a slot's reach plus two grid cells).

### Cloud and sustain

In `MainPredictCS`, from `CellColumn`:

- **Cloud feed** (`SimCellCloudFeed`): every layer's cloud relaxes toward full at `I · lift · p(r) · (1 − eye)`, `lift = StormCellCloudCover / (1 − StormCellCloudCover) / CloudLifetime`, so against decay alone a full-intensity eyewall settles at `StormCellCloudCover` whatever `CloudLifetime`. It is a lift on top of the weather, so a hurricane always holds more cloud than its surroundings and is the last feature coverage or erosion removes.
- **Sustain** (`SimCellStormFloor`): every layer's storm is held up toward `min(StormCellSustainRatio · genesis storm, 1) · s · p(r)` at the storm formation rate `a`, peaking at the eyewall, none at the centre. At 0 a cell lives as long as its parent storm; from about 1.25 it keeps its parent alive and ends when the window or humidity fails, or at `StormCellLifetime`.

### Eye tracer and the low core

The eye is an advected tracer in noise phase B's w. A balanced vortex turns about its pressure minimum, so the eye follows the low, not the vorticity.

- **The low** (`SimCellWriteControl`): the bottom layer's output pressure at the centre and on four six-point rings out to the eyewall ring (`SIM_CELL_LOW_RINGS`); stored are the eyewall ring's mean and the least sample.
- **Source** (`SimCellEyeSource`): a low deepens about as `r²`, so depth below the eyewall's isobar reads back as an equivalent radius, and the eye ramps in on a quintic over its outer `StormCellEyeSoftness`:

  ```
  x = √(1 − (wall − p)/(wall − least))          1 on the isobar, 0 at the minimum
  core = 1 − smootherstep(1 − StormCellEyeSoftness, 1, x)
  source = max over slots of s · near · core · eye share,   near fading from the measuring ring to twice it
  ```
- **Carry** (`MainPredictCS`): the tracer is advected and relaxes toward the source at `StormCellEyeRate` per turnover where the source is higher and `1 / StormCellEyeTrail` where lower. Settling on the source it takes its cross-section; the flow deforms it, drags it with the storm and winds it into streaks behind a moving source, and with memory it cannot blink out between steps.
- **In the sim** the eye's draft `StormCellEyeDraft · eye` joins the W the moisture sees: where a cell is near, `MainPredictCS` adds it to W, so cloud evaporates at `EvaporationRate · max(−(W + StormCellEyeDraft · eye), 0)` and the storm tracer decays with the same sinking, and while `W + StormCellEyeDraft · eye ≤ 0` rising-air condensation stops there (vapour past saturation still condenses). The cloud feed is scaled by `1 − eye`. The feed itself never clears cloud to zero.
- **On the atlas** the eye becomes a depth factor (below).

### Output stamp

`SimCellStamp`, in `MainResampleCS`, at each slot's display centre; each quantity from the strongest overlapping slot:

| Quantity | Value | Atlas |
|---|---|---|
| Draft | `I · StormCellDraft · p(r)` | added to W, with `StormCellEyeDraft · eye`; clamped to ±1 |
| Storm | `I · max(storm · whole, (StormCellStorm + StormCellBandExcess) · band)` | smooth max with the sim's storm, width `StormCellStormBlend` (`SimSmoothMax`) |
| Low | `pressure · I · whole` | `SoftSat(SoftSat⁻¹(p) − low)`, so a deep drop rounds off toward −1 |
| Eye | eye tracer × presence | noise B w `= 1 − StormCellEyeDepth · eye` |

`whole = 1 − smoothstep(eyewall, 1, r)`; `band` is 1 inside the eyewall and `smoothstep(StormCellBandFloor, 1, p)` outside; `storm` and `pressure` are the slot's (`StormCellStorm`, `StormCellPressure`, or a perpetual storm's `Storm`, `Lift`). Presence is `saturate(I / 0.05 − 1)` faded over the outer 0.2 of the radius (`SIM_EYE_PRESENCE_RIM`), so a freed cell's decaying eye leaves the atlas. Cover is left as the sim has it: the stamp intensifies the cloud the vortex winds rather than adding its own.

### Storm cell constants

| Constant | Value | Role |
|---|---|---|
| `SIM_CELL_SPAWN_DRAWS` | 8 | Seeds per spawn attempt |
| Seed refine | 3 ring searches from ½ radius | Centring on the storm's peak |
| `SIM_GENESIS_WINDOW_GATE` | 0.5 | Window a seed must exceed |
| `SIM_GENESIS_FADE_EQUATOR`, `_POLE` | 0.07, 0.14 rad | Genesis band edges |
| Spawn intensity | 0.1 | |
| `SIM_CELL_FREE` | 0.05 | Slot frees below |
| Merge reach | 1 radius (a perpetual storm's reach) | |
| `SIM_CELL_GAIN_LIMIT` | 2 | Gain clamp |
| `SIM_CELL_RING_CELLS` | 2 grid cells | Least ring radius and wind fall-off span |
| `SIM_CELL_TARGET_CEILING` | 0.9 of `U` | Vortex and inflow targets together |
| `SIM_CELL_HEMISPHERE_SHARPNESS` | 40 | Three-cell sense through the equator |
| `SIM_CELL_LOW_RINGS` | 4 | Rings the low is read on |
| `SIM_EYE_PRESENCE_RIM` | 0.2 | Presence fade at the rim |
| Core follow rings | 3, squared vorticity weights | |

## 8. Perpetual storms

Authored storms that never die, the Great Red Spot class: storm cells held in place. Each takes a slot and runs the hurricane path (vortex and inflow, cloud feed, sustain, stamp, low and optionally the eye) with its own size, wind and oval; the hurricane shape and look settings apply to it too. Settings: `PerpetualStorms` (up to `FlowSimShader::MaxPerpetualStorms`, 8; extras are ignored with a warning), `PerpetualStormForcing`, `PerpetualStormClearance`; per storm `Latitude`, `Longitude`, `Radius`, `Aspect`, `Wind`, `Spiral`, `Steering`, `Drift`, `Storm`, `Cover`, `Lift`, `Eye` (`FFlowPerpetualStorm`).

Resolved on the CPU each frame in `BuildParams`:

- **Settling** (`NearestFlowReversal`). A storm belongs between two jets of opposite direction. `Latitude` is a request: the storm settles at the nearest latitude within 45° (0.25° steps, `|lat| ≤ 85°`) where the layers' mean prescribed zonal wind `R̄(μ) cos φ` changes sign, interpolated. With no reversal there, it stays where asked. There the flow across it nets to about zero and the shear sets its spin.
- **Size.** Half-height `Radius · DeformationRadius`, held so its eyewall spans at least two grid columns and to at most 45°.
- **Background** (`PerpetualBackground`). 17 taps across its latitude span, weighted by a profile peaking at the eyewall, give the mean zonal speed and the relative vorticity `2μR̄ − (1 − μ²) dR̄/dμ`.
- **Spin.** The sign of that vorticity: positive counterclockwise seen from outside. An eastward jet poleward and a westward one equatorward make it anticyclonic. A spin against the shear is torn apart by the jets.
- **Steering and drift, in closed form.** `rate = (Steering · background speed + Drift · U) / cos φ`; longitude `= Longitude + rate · t`. `FillCommonParameters` evaluates it in double at each step's end time and wraps it. The flow it uses is the prescribed profile the nudge holds, not the live flow. A snapshot depends on nothing about the storms.
- **Look.** Signed target wind `sense · Wind · U` (`Wind` held to 0.9), `Storm`, `Lift` as the pressure drop, `Cover` as a lift rate (as `StormCellCloudCover`); `Spiral` is a signed inflow share (as `StormCellInflow`, negative reverses); `Eye` scales the eye source. Push rate `PerpetualStormForcing / T`.

On the GPU:

- **Placement** (`MainCellsCS`). Each step a storm's slot is set to its position at full intensity; it is never spawned, moved, merged or freed. Its move from the slot's previous centre is kept for the display centre when the slot held it, zero otherwise (as after a seed). Health is 1; its low and gains are measured like a cell's at sense share 1.
- **Shape** (`SimCellReach`). An ellipse in the storm's local frame: `r = (angle / half-height) · √(n² + e² / Aspect²)`, `e` and `n` the offset direction's east and north shares, so the wind falls to `Wind / Aspect` at the long axis's ends. The east axis comes from `SimCellEastOf`, x at the pole itself.
- **Clearance.** No hurricane spawns within `PerpetualStormClearance` of a storm's reach (`half-height · Aspect`); a hurricane inside its reach merges away.

The dump's `Derived.PerpetualLatitudes`, `PerpetualRates` and `PerpetualSenses` report where each settled, its angular rate and its spin.

## 9. Outputs

### The atlas

`MainResampleCS` writes the flow target the cloud field reads (`UFlowSimSubsystem::PrepareTargets`, `FlowField.ush`; the readers are in [`Design/FieldReference.md`](FieldReference.md)):

- An equi-angular cube, six faces packed 3 × 2, face edge `F` (`GridResolution`) plus a 4-texel gutter each side (`FLOW_ATLAS_GUTTER`): `3(F + 8) × 2(F + 8)`, RGBA16F.
- `4L` slices: `[0, L)` flow, `[L, 2L)` weather, `[2L, 3L)` noise A, `[3L, 4L)` noise B. Readers derive `L` from the slice count.
- Every texel, gutter included, is the output at its direction: cubic in longitude, Lagrange cubic on the rows' true latitudes folded over the poles (`SimMeridianTapsAt`; a cubic in `μ` leaves a dent at the pole), blended between the previous and latest states by `StateBlend`. Velocity comes from the Cartesian centre field, written in the face's tangent basis (`Flow_FaceBasis`).
- Readers sample a cubic B-spline through four bilinear taps and cross-fade faces over 2 texels at the edges (`FLOW_ATLAS_BLEND`, `Flow_FaceSet`).

| Slice | x | y | z | w |
|---|---|---|---|---|
| Flow | Velocity along `T1` | along `T2` (sim units) | Pressure: `M` less its row mean, `/ PressureScale`, soft-saturated; less the cells' low | Relative vorticity `/ VorticityScale`, positive counterclockwise |
| Weather | W, low-passed, normalised (−1, 1), plus the cells' drafts | Column cover from the top down to this layer, `1 − Π(1 − q)` | Formation altitude [0, 1], bottom of the stack to top | Column storm (max), smooth-maxed with the cells' |
| Noise A | Displacement xyz (radians) | | | 0 |
| Noise B | Displacement xyz | | | Depth factor `1 − StormCellEyeDepth · eye` |

Formation altitude places each layer's cloud in its own share of the stack, raised within it by the ascent it formed at: `Σ_{k≤layer} ((L − 1 − k) q_k + y_k) / L`, divided by `max(Σ q_k, 0.05)`. On one layer it is the formation ascent. The bottom layer's weather slice is the whole sky seen from above; the actor's `CloudLayer` chooses which layer the deck reads, and the terrestrial deck reads the depth factor through `Flow_SampleColumn`.

The lat-lon output on the sim grid (`LatLon`, `LatLonLatest`) differs: velocity is east/north, which the resample re-projects into the face basis; it carries no storm-cell stamp (no drafts, cell storm, low or clamping); and noise A's w holds the eye tracer and noise B's w the layer's relative humidity, which the cells read.

### Output-only settings

These act only in the resample (and the Storm debug view), never on the state, so they change the atlas without changing the weather: `StormCellDraft`, `StormCellStorm`, `StormCellBandExcess`, `StormCellBandFloor`, `StormCellStormBlend`, `StormCellPressure`, `StormCellEyeDepth`, and a perpetual storm's `Storm` and `Lift`. `StormCellEyeDraft` acts on both: on the moisture in Predict and on the atlas's W. Every other storm cell setting acts on the state.

## 10. Snapshots

`UFlowSnapshot` holds raw floats, never a texture, so nothing compresses or re-encodes a physical field.

**Layout** (`MainCaptureCS`, `MainRestoreCS`; `FFlowSimulation::StateFloatsPerCell` 15, `StateTrailingFloats` 256): one plane per float, each plane layer-major, then row, then column:

```
0 u   1 v   2 φ   3 cloud   4 cloud ascent   5 vapour   6 storm
7–9 noise A xyz   10–12 noise B xyz   13 low-passed ascent   14 eye tracer
then 32 slots × 2 float4 of cell state (256 floats)
```

The 13-plane layout without the last two planes still restores; they come back as zero. Static asserts tie `UFlowSnapshot::FloatsPerCell`, `LegacyFloatsPerCell`, `TrailingFloats`, `FlowSimShader::SnapshotPlanes` and the slot count together. The tracers are state too: a snapshot without them would restore the winds under an empty sky.

Also stored: `Grid`, `SimulatedTime` (float), `StepsCompleted` (int32), and `Provenance`: `BandCount`, the resolved `JetStrength`, `EquatorialBoost`, `Asymmetry`, `WidthBias`, `PlanetaryVorticity`, the resolved `ThermalShear`.

**Not stored**: the control block (gains, health, low), which a restore zeroes and the next cells pass rewrites; every derived field; the config. Perpetual slots' state is captured with the rest but overwritten by placement on the first step, so a snapshot depends on nothing about the perpetual storms.

**Matching** (`UFlowSimSubsystem::QueueInitialState`, `Enqueue_RenderThread`):

- **Layout refused.** A snapshot whose `Grid` differs or whose size fits neither layout (`IsValidFor`, `FloatsPerCellOf`) is refused with a warning, and the sim seeds and spins up.
- **Jet profile warns.** `MatchesShape` compares `BandCount`, `JetStrength`, `EquatorialBoost`, `Asymmetry`, `WidthBias` and `ThermalShear` (tolerance 1e-3); a mismatch warns and restores anyway; the warning says the nudge re-registers the zonal mean over a few hundred steps. `PlanetaryVorticity` is recorded but not compared, and nothing else (zonal profile kind, `JetLatitude`, the jet shape handles, `ThermalShape` and zone, `LayerProfiles`, the perpetual list) is recorded.
- A restore takes the snapshot's time and step count and skips spin-up. Slots keep their indices across a restore (`MainRestoreCS`). If the perpetual count has grown, hurricanes in newly perpetual slots are overwritten by placement; if it has shrunk, former perpetual storms in freed slots carry on as hurricanes at intensity 1.

`FlowSim.Save` captures (a GPU readback, blocking); with no argument it writes the running config's `InitialState`.

## 11. Start log, derived values and debug

**Start log** (`ReportCourant`, `ReportStack`, `ReportInertSettings` on start; `ReportCourant` and `ReportStack` again on a grid change):

- Steps per frame at 60 fps; advective Courant (peak angular rate · `Δt · N_lon / 2π`), gravity-wave Courant (implicit), Froude (the stack's peak prescribed wind over `c`); a warning when `SpeedRoot` exceeds 0.85, since a wind's Froude number is its fraction of the root times `SpeedRoot`, Coriolis rotation per step (`PlanetaryVorticity · Δt`, warning above 0.5 rad), wave speed.
- Grid damping: the implicit scheme's share per turnover and the divergence damping fraction per step; notes when the implicit scheme alone exceeds `GridDamping`, warns when the 0.45 cap binds.
- The storm cell inflow cut, when a full-strength cell's inflow target does not fit under `0.9 U`.
- Speed root and turnover; the budget against the knee (section 2) with its warning.
- The turnover in days (one rotation, `4π / PlanetaryVorticity`, since `PlanetaryVorticity` is `2Ω`), turnovers per real second, cloud, storm, storm cell and forcing lifetimes in days, the cell radius in degrees.
- Mode wave speeds, the storm criterion ratio, the thinning warning (section 5).
- Inert settings: `EddySpeed` without a `ForcingVolume`, coupling and shear on one layer, slot overflow with perpetual storms, `StormCellPersistence` 1, `StormCellSustainRatio` below 1, `SpinUpTurnovers` with an `InitialState`, `FilterLatitude` 0, `DragRate` 0, `SurfaceEvaporation` 0.

**Derived** (`CloudAtmosphere.DumpParams`, `DescribeSim` in `AtmosphereParamDump.cpp`), at the last frame's step: `Step`, `ImplicitWeight`, `ImplicitDampingRate`, `DivergenceDampingPerStep`, `PressureScale`, `VorticityScale`, `DivergenceScale`, `NoiseDriftRate`, `NoiseResetTime`, `AtlasFaceSize`, `Grid`, `LayerDepth`, `ModeWaveSpeed`, `PerpetualLatitudes` (degrees), `PerpetualRates` (radians per sim time), `PerpetualSenses`. The sim section also reports `SimulatedTime`, `DisplayTime`, `StepsCompleted`, `StepsLastFrame` and `Courant`.

**Debug view** (`MainDebugVisCS`): point-sampled on the sim grid, since a checkerboard in pressure or the residual is the signal. `DebugMode`, `DebugLayer` (the mode, for the residual) and `DebugScale` (0 derives one per mode in `BuildParams`), overridden by `r.FlowSim.DebugMode`, `DebugLayer`, `DebugScale` and `Paused`. Modes 0–19 are tabled in [`Design/UsageGuide.md`](UsageGuide.md) (Debug modes). For maintenance: Residual (featureless once the solve converges; coherent structure means the transform or column solve is broken), ZonalError (whether the nudge holds), Froude, Storm genesis (where a seed passes its gates), Storm cell health (which condition fails), Storm cell gains (full brightness is pinned).

## 12. Pitfalls and open questions

### Pitfalls

Numerics:

- **A collocated (A-grid) gravity step has a checkerboard mode**: centred gradient and divergence compose into a 2Δx Laplacian blind to red–black alternation, and pressure decouples into two grids.
- **The explicit gravity terms must travel with the parcel.** Evaluated at the arrival point, `|λ|² = (1 + β² ± 2β sin θ) / (1 + α² a²)` with `a = k c Δt`, `β = (1 − α) a`, `θ = k U Δt` exceeds 1 once `U / c > (2α − 1) / (2(1 − α))`, a Froude number of a quarter at `α = 0.6`. At the departure point the advection phase multiplies the whole numerator and the scheme is stable for `α ≥ 0.5`.
- **On a stack the explicit pressure terms move at each other's speeds**: a layer's pressure includes other layers' thickness, carried at their velocity. At large steps the mismatch amplifies grid noise a few rows from the poles; hence `StackWeight` above `StackLargeStep`.
- **An unconverged iterative solve is an explicit gravity step**, unstable once `c Δt` exceeds the grid spacing; the solve is direct for that reason.
- **Bilinear advection** diffuses at grid scale per step: eddies starve and the planet relaxes to plain bands. **UV-space cubic sampling** (`floor(G − 0.5) + 0.5`) samples half a texel off, a constant drift. **A chord backtrace** (`normalize(P − VΔt)`) falls short by `cos θ`, a slow meridional creep.
- **Face components read through the fold** reverse sign; face operators wrap longitude only.
- **A two-face centre average** smooths `u` only east–west and `v` only north–south, bleeding a compact vortex into a cross.
- **A damping fraction per step** is a rate of `−ln(1 − f)/Δt`: halving the step doubles it. Damping is authored as a rate and per row.
- **An explicit relaxation step longer than `1/decay`** overshoots and flips between clamps; every source term uses `SimRelax`.
- **An absolute float32 clock** stops resolving the step: resets stop firing and phases quantise. Clocks are wrapped in double on the CPU.
- **The weather depends on the step**: interpolation smooths once per step and the step sets how grid-scale gravity waves split between pressure and divergence; the config comments note that larger steps give sharper, thicker cloud.
- **Coriolis split.** Above about 0.5 rad per step (`PlanetaryVorticity · Δt`) the explicit rotation against implicit pressure weakens balanced jets and radiates gravity waves. Since `Δt = StepSize · T`, the polar rotation per step is `√2 · StepSize / SpeedRoot`.

Forcing and profile:

- **Strong nudging reads as ripples**: every nudge is an unbalanced push.
- **A missing forcing volume binds black**, which decodes to a uniform −1; `HasForcing` gates it.
- **A forcing difference narrower than a volume texel** returns the trilinear field's piecewise-constant gradient, drawing the lattice into the flow.
- **A fixed or slowly changing forcing pattern** pins every eddy it makes to one spot.
- **A corner in the jet rate is a vortex sheet** (`ζ = 2μR − (1 − μ²)R′`), and Rayleigh–Kuo reads `R″`: the saturation must be C2. A phase offset or `abs()` on the band cosines puts a kink at the equator. Near a whole `JetHarmonic` the pattern repeats.
- **A shear strong enough for storms can lift an interface to the top of the stack**, where the thickness floor clips it; a thicker top layer (`DepthScale`) gives room. The start log warns.

Moisture:

- **Unsmoothed latent heat feeds grid-scale convection**: checkerboard columns and fronts sweeping from strong updraughts. `LatentHeating` is a positive feedback.
- **Raw W in condensation** makes every passing gravity wave condense cloud along its crest, a travelling line in the cloud.
- **`UpperSaturation` near 0** makes condensation form cloud without spending vapour; the upper layers stay overcast.

Storm cells:

- **A cell judging itself**: read at its centre, its own vortex leaks into the sample and its layers' counter-spin reads as shear that closes its window; the same leak in the steering walks it off its storm. Conditions use the half-radius ring.
- **A seed judged on a ring** sees only the zonal shear; the local eddy winds that open the window average away, and under strong `ShearSpeed` no seed passes.
- **A parent storm sampled off the eyewall**: the cell's inflow converges at the eyewall, so its storm sits there; read only near the centre a healthy cell sees its parent fade, decays and respawns as brief spirals.
- **Equal-weight steering** follows the upper wind as much as the lower, and under shear the cell drifts off its vortex.
- **Without core follow** the cell slides off the flow's drifting vortex and its eye is swept round a centre it is not on.
- **A two-sided loop** brakes the stronger parent storm a young cell spawns on: the parent dies, the cell with it, and outflow holes the cloud.
- **A target wind far past the ceiling** flattens the vortex into a broad band at the cap with gains pinned, unregulated; `StormCellWindBreadth` widens the band under the knee.
- **Regulating the eyewall alone** lets the environment erode the outer band, and the spin feeding the storm tracer with it; hence the band ring.
- **Gains measured against the full target near the turnover of the sense** rise to cancel the easing until they pin.
- **A hard hemisphere sign** flips a cell drifting along the equator, each reversal sending a wave through the layer.
- **Convergence confined to the eyewall** leaves the rest of the core without ascent; the core's storm decays, and the cells with it.
- **A fixed cloud target** below the ambient cloud does nothing, and the hurricane culls with its surroundings.
- **Resolution**: nothing under about two grid cells survives the push; at `GridResolution` 128 the eyewall wants to sit at least 1.4° out, at 256 at least 0.7°. A perpetual storm's eyewall under two grid columns pins its gain.
- **Eye**: an eye taken from vorticity leans off centre (vorticity peaks in an often-open ring); an eye measured from calm air each step slides into a crescent and wanders; a ramp in pressure, or a cubic, draws a ridge at the rim in the deck's lighting; a saturating feed (`rate · source · (1 − tracer)`) settles on a flat floor behind a hard wall; an eye faded over the eyewall radius is clipped inside its isobar; a drawn eye, or cloud cleared to zero along any contour, reads as a cutout and as a hole punch in a deck whose cloud amount saturates.
- **Stamp**: raising cover stamps a solid disc ignoring the wound cloud; storm raised only in the band leaves the rest of the hurricane reading as fair-weather cloud; the pressure clamped after the drop pins the core at −1, a shelf; a polynomial smooth max adds `K/4` where both inputs are zero, a storm floor everywhere; an unweighted eye lingers in the atlas after its cell frees.
- **`StormCellPersistence` 1** fills every slot with cells that never decay, and spawning stops.

Perpetual storms:

- **A spin against the shear** is torn apart by the jets.
- **A drift far from the local flow** makes the eyewall fight the shear, and the gain pins (Storm cell gains view).
- **Far below the stirring**, `PerpetualStormForcing` pins the gains and the storm shows as cloud cover without rotation.
- **Near a pole** the ellipse's east axis falls back to x at the pole itself.
- **They share the 32 slots** with the hurricanes, which get what is left.

### Open questions

1. **Implicit Coriolis for a small gas giant `Ld`.** A realistic gas giant deformation radius is far smaller than the class default of 0.2 planet radii; reaching it at an affordable step count may need steps past the explicit Coriolis bound. Implicit Coriolis (a per-cell 2×2 inversion) couples `u` and `v` inside the Helmholtz operator with `f(μ)`, which keeps it separable in longitude, so the FFT still applies and each wavenumber's column becomes a complex block-tridiagonal system. Separate from the stack.
2. **Surface map (Phase E).** An ocean fraction and warmth by direction would scale `SurfaceEvaporation` and the cells' favour, so storms form over warm ocean and weaken over land: a texture read in Predict and MainCellsCS.
3. **Forcing normalisation.** `SimForcingSlope` does not divide out the forcing volume's own gradient, so `EddySpeed` is an equilibrium speed only for a unit-gradient bake; finer volume features stir harder.
4. **Seeding.** The sim has no seed: the forcing hashes (cycle, phase, layer), spawns hash `StepIndex · 32 + slot`, the start state is exactly zonal and Start and Reset zero the clock, so two planets on one config get the same weather and storm calendar (Todo API-06).
5. **Snapshot provenance.** The layout is known by size alone, `MatchesShape` misses most of the profile and the perpetual list, and `SimulatedTime` is a float (Todo API-10, SIM-01).
