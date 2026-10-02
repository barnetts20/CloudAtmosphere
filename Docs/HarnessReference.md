# Harness parameter reference

For the author of the Phase D procedural harness: which settings to draw per planet, over what range, which to hold, and what each change costs or disturbs. `Design/UsageGuide.md` says what each setting does; this doc says how to generate with it.

**The draw ranges are first estimates,** taken from the two current tunes, the slider limits and the rules below. They haven't been swept. Sweeping each planet class's draws in engine is the harness's first job. Tighten a range wherever its ends look wrong, and record the result here.

## Classes

| Class | Meaning | The harness |
|---|---|---|
| **Identity** | What makes one planet differ from another of its class. | Draws per planet, within the class's range. |
| **Style** | Look settings a planet class or game sets. | Sets per class, or draws narrowly. |
| **Fixed** | Tuned values that hold the model together. | Keeps the tune's value. Changing one is art direction, not variety. |
| **Quality** | Cost, per machine. | Never draws. Sets from the game's quality tier. |
| **Pipeline** | Assets and plumbing. | Sets once. |

Settings are relative by design (ParamPass D-25): winds are fractions of the speed root, rates and lifetimes are in turnovers, and sim lengths are in deformation radii. Identity draws can therefore be made independently, and the rules below cover the couplings that remain.

## Driving the core

- **The actor:** one per planet.
  1. `SetPlanetType` picks the model.
  2. `GetModelParams(Model)` returns the model's bundle (`FAtmosphereModelParams`). Set the drawn members, then `SetModelParams(Model, Params)`.
  3. Alternatively, `UAtmosphereTuneLibrary::ApplyPreset` applies an authored `UAtmospherePreset`, and `ApplyTune` applies a tune's JSON.
  4. Every actor setting takes effect on the next frame.
- **The sim config:**
  - Don't build one from the class defaults, which are an incoherent mix (API-09). Duplicate a template config (`TerrestrialSimDefault`, `GasGiantSimDefault`), set the drawn values, and assign it to the actor's `Simulation.TerrestrialConfig` or `GasGiantConfig`.
  - At runtime, `GetWritableSimConfig(Model)` returns the config to change: in a game world, a runtime copy.
  - The sim rereads its config every frame. How a change shows depends on the setting (the Acts column).
- **One sim per world.** The claiming planet nearest the camera drives it with its active model's config. The others draw the weather they kept when they last drove it. A planet that never drove the sim draws clear sky (RUN-01 changes that).
- **Start state:** the config's `InitialState` snapshot.
  - It must match `GridResolution` and `LayerCount`, or it's refused and the sim seeds and spins up for `SpinUpTurnovers`.
  - A snapshot captured under a different jet profile is accepted with a warning, and the winds re-register over a few hundred steps.
  - Every other resim setting relaxes the same way, so a snapshot library per model and grid tier, with a seeded pick, is the intended start (RUN-01).
- **Seeds:** the core has no seed setting yet (API-06, with the harness). Until then, planets with equal settings differ only by their start snapshot.
- **Variety beyond settings:** per-cell storm traits, drivers, perpetual storm variation, the genesis window and placement are Phase D hooks (PROC-01 to 03, 05, 07, 08). Shear zones for perpetual storm placement are PROC-08.

## Rules

Couplings a draw must respect. A draw that breaks one still runs, but looks wrong or loses a feature.

**Cloud field**
- `CloudBase` + `CloudThickness` ≤ 1 − `CeilingFalloff`, or the ceiling thins every tall column (`SolvedTopMax` shows it).
- `CoverageSoftness` ≤ 0.5, or no column reaches full coverage and `CloudCover` loses its hold.
- `CoverageDepthRamp` ≥ 0.1, or system edges become cliffs and shadow goes missing under them.
- `HeightScale` ≥ 0.05, because thinner shells lengthen grazing chords the bake doesn't shorten.
- `DetailLayer.FadeFar` ≤ `SurfaceShadow.CascadeRadii.Y`, so the near shadow cascade carries the grain wherever the clouds show it.
- `LightIntensity` up to about 1000 at `MieG` 0.95 and 40 at 0.99, before the haze's forward lobe clips in half float.

**Winds and storms**
- Top-layer winds stay within 0.7 of the speed root: `JetSpeed` × `JetScale`, and `EddySpeed` × `EddyScale`. A hurricane's `StormCellWind` plus the background it rides on stays within 0.7 too. The ceiling eases faster faces, so authored and achieved winds part above 0.7. Both current tunes run `ShearSpeed` 1.05 deliberately.
- `DeformationRadius` ≥ 3π / `GridResolution`, about six grid cells, so eddies are resolved.
- Hurricane eyewall at least two grid cells: `StormCellRadius` × `DeformationRadius` × `StormCellEyewall` ≥ π / `GridResolution`.
- Hurricane radius ≤ 45°: `StormCellRadius` × `DeformationRadius` ≤ 0.785.
- `GenesisLatitudeMax` ≤ 45°, the range genesis is tested over. `GenesisLatitudeMin` a few degrees off the equator.
- `StormCellPersistence` < 1 while hurricanes are on, or they never decay and the slots fill.
- Perpetual storms at least 1.5 (R1 + R2) apart. They take the first of 32 cell slots, and hurricanes get up to `MaxStormCells` of the rest.
- `UpperSaturation` above 0, and `LatentHeating` modest: it is a positive feedback that runs away into grid-scale convection.

## The atmosphere actor

Every actor setting acts on the next frame; none needs a resim. Edits to the cloud field, `HeightScale` or the air's profile rebake the shadow map or the transmittance table once. The Cost column names a setting's effect on GPU time where the code has one; settings that only change values the march already computes cost nothing.

### Atmosphere

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `PlanetType` | Pipeline | by planet class | Terrestrial | GasGiant | Air Only skips clouds, sim and bake | Selects the shader variant. Air Only skips the sim, the bake and every cloud setting. |
| `LightColor` | Style | from the star | (1, 0.95, 0.9) | (1, 0.95, 0.9) | — | Hue only; the brightest channel counts as 1. |
| `LightIntensity` | Style | from the star and exposure | 4 | 4 | — | Up to about 1000 at `MieG` 0.95, 40 at 0.99, before the haze's forward lobe clips. |

### Planet

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `HeightScale` | Identity | 0.05–0.2 | 0.1 | 0.1 | — | At least 0.05: thinner shells lengthen grazing chords the bake doesn't shorten. Every height below scales with it. |
| `SpinRatio` | Identity | T: 0.2–1 · GG: 0.01–0.1 | 0.333 | 0.0167 | — | Turns the pattern only; the weather is unchanged. |

### Air

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `RayleighDepth` | Identity | hue from the planet class; magnitude 0.3–3× the terrestrial tune | (0.146, 0.465, 0.795) | (0.649, 2.7, 4) | — | The sky's colour and thickness. A gas giant's runs far higher (tune: up to 4 in blue). |
| `RayleighScaleHeight` | Style | 0.1–0.25 | 0.2 | 0.15 | — | Must leave air above the cloud tops, or the limb reads as a hard edge. |
| `MieDepth` | Identity | 0.02–0.3 per channel | (0.1, 0.0893, 0.0756) | (0.1, 0.0893, 0.0756) | — | Haze. |
| `MieScaleHeight` | Style | 0.05–0.2 | 0.1 | 0.1 | — | Cloud reaching high up the shell needs haze above it. |
| `MieG` | Style | 0.75–0.95 | 0.95 | 0.9 | — | Couples to `LightIntensity`'s clip limit. |
| `MieLobeDecay` | Fixed | keep | 2 | 2 | — |  |
| `AbsorptionDepth` | Style | 0–2× the tune, hue per class | (0.0523, 0.0422, 0.0458) | (0.0523, 0.0422, 0.0458) | — |  |
| `AbsorptionAltitude` | Fixed | keep | 0.15 | 0.15 | — |  |
| `AbsorptionFalloff` | Fixed | keep | 0.1 | 0.1 | — |  |

### Ambient

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `AirAmbient` | Style | per class; brighter over a bright surface | (0.004, 0.00547, 0.00844) | (0.004, 0.00547, 0.00844) | — | Stands in for ground bounce, which the model doesn't compute (API-08). |
| `AirAmbientFloor` | Fixed | keep | 0.05 | 0.05 | — |  |
| `CloudAmbient` | Style | per class | (0.04, 0.044, 0.052) | (0.04, 0.044, 0.052) | — |  |
| `CloudAmbientFloor` | Fixed | keep | 0.015 | 0.01 | — |  |
| `AmbientTerminator` | Fixed | keep | 0.15 | 0.15 | — |  |

### Shape

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `CloudBase` | Identity | T: 0–0.2 · GG: 0.1–0.25 | 0.01 | 0.175 | — | `CloudBase` + `CloudThickness` ≤ 1 − `CeilingFalloff` (check `SolvedTopMax`). |
| `CloudThickness` | Identity | 0.5–0.85 | 0.822 | 0.65 | widens the marched band; bake steps | See `CloudBase`. Widens the marched band. |
| `SurfaceSoftness` | Fixed | keep (≥ 0.25) | 0.33 | 0.33 | lower: more bake steps | Low values multiply bake steps. |
| `TopCurve` | Style | 1–2 | 2 | 1 | — |  |
| `BottomCurve` | Style | 1–2 | 2 | 1 | — |  |
| `CeilingFalloff` | Fixed | keep | 0.2 | 0.2 | — |  |
| `SlopePerTexel` | Fixed | keep; raise only for shadow holes | 1.5 | 1.5 | higher: more bake probes | Raise with crisp coverage edges or small eyewalls. |

### Coverage

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `CloudLayer` | Fixed | keep (−1) | -1 | -1 | — |  |
| `CloudCover` | Identity | T: 0.3–0.8 · GG: 0.85–1 | 0.5 | 1 | more cloud, more noise fetches | Needs `CoverageSoftness` ≤ 0.5 to act fully; capped by the share the sim has cloud in. |
| `CloudFull` | Fixed | keep (0.3–0.5) | 0.5 | 0.5 | — |  |
| `StormPriority` | Style | 0.5–1.5 | 0.75 | 1.5 | — |  |
| `CoverageSoftness` | Style | 0.15–0.5 | 0.75 | 0.25 | — | Above 0.5 no column reaches full coverage and `CloudCover` loses its hold. |
| `CoverageFray` | Style | 0.4–1 | 0.75 | 1 | — |  |
| `CoverageDepthRamp` | Style | 0.2–0.6 | 0.6 | 0.25 | — | Below about 0.1 edges become cliffs the shadow bake misses. |
| `EyeOpenPower` | Fixed | keep | 1 | 1 | — |  |

### Type

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `TypeBias` | Identity | 0.4–0.9 | 0.8 | 0.5 | — | Towering vs layered character. |
| `TypeTropical` | Style | 0.2–0.8 | 0.3 | 0.8 | — |  |
| `TypeStorm` | Style | 0.3–0.7 | 0.5 | 0.5 | — |  |
| `TypeCurve` | Style | 1–2 | 2 | 1 | — |  |
| `StratusDepth` | Style | 0.2–0.4 | 0.25 | 0.25 | — |  |
| `Genus.Stratus` | Fixed | keep | (1, 0, 0) | (1, 0, 0) | — | Genus weights pair with the noise volume's channels. |
| `Genus.Stratocumulus` | Fixed | keep | (0.3, 0, 0.7) | (0.3, 0, 0.7) | — |  |
| `Genus.Cumulus` | Fixed | keep | (0.2, 0.8, 0) | (0.2, 0.8, 0) | — |  |
| `Genus.Cirrus` | Fixed | keep | (0.2, 0, 0) | (0.2, 0, 0) | — |  |
| `Subsidence` | Fixed | keep | 3 | 3 | — |  |

### Lift

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `PressureScale` | Fixed | keep | 1 | 0.5 | — |  |
| `CeilingPressure` | Style | 0.3–0.5 | 0.5 | 0.3 | widens the marched band | Widens the marched band. |
| `BaseTropical` | Fixed | keep | 0.15 | 0.1 | widens the marched band | Widens the marched band. |
| `BasePressure` | Fixed | keep | -0.35 | -0.35 | widens the marched band | Widens the marched band. |
| `AltitudeGain` | Fixed | keep | 3 | 3 | — |  |
| `AltitudeLift` | Fixed | keep | 0.6 | 0.6 | widens the marched band | Widens the marched band. |

### Warp

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `WarpStretch` | Style | 0.2–0.5 | 0.25 | 0.5 | — |  |
| `WarpShift` | Style | 0.15–0.3 | 0.25 | 0.2 | — |  |

### Structure Layer

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `Volume` | Pipeline | per class | CloudNoise_4_128_eq | CloudNoise_4_128 | none skips the layer's fetches | The noise pack; equalized volumes want `DetailLayer.FadeMean` 0.5. |
| `Scale` | Identity | T: 1.5–6 · GG: 1–2 | 2 | 1.5 | — | Cloud size. |
| `Aspect` | Style | T: 2–12 · GG: 10–20 | 12 | 16 | — |  |
| `Erosion` | Identity | 0.5–0.9 | 0.6 | 0.85 | 0 skips the layer's fetches | With `Breakup`: gaps open in full systems past 1/(1 + `Breakup`). |
| `Breakup` | Style | 0.3–0.7 | 0.5 | 0.5 | — |  |
| `FlowInherit` | Fixed | keep (0.9–1) | 1 | 0.9 | — |  |
| `MipBias` | Fixed | keep | 1 | 1 | — |  |

### Detail Layer

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `Volume` | Pipeline | per class | CloudNoise_8_128_eq | CloudNoise_8_128 | none skips the layer's fetches | A different asset from the structure layer. |
| `Scale` | Style | 16–32 | 24 | 24 | higher: detail fetch stops nearer |  |
| `Aspect` | Fixed | keep per class | 8 | 8 | higher: detail fetch stops nearer |  |
| `Erosion` | Style | 0.3–0.6 | 0.3 | 0.6 | 0 skips the layer's fetches |  |
| `BillowHeight` | Fixed | keep | 0.25 | 0.25 | — |  |
| `FlowInherit` | Fixed | keep (below the structure's) | 0.33 | 0.4 | — |  |
| `MipBias` | Fixed | keep | 0 | 0 | higher: detail fetch stops nearer |  |
| `FadeNear` | Fixed | keep | 0 | 0 | — |  |
| `FadeFar` | Fixed | keep | 0.3 | 0.3 | farther: detail fetched further out | ≤ `SurfaceShadow.CascadeRadii.Y`, the near cascade that carries the grain. |
| `FadeMean` | Fixed | match the volume | 0.5 | 0.5 | — | 0.5 for an equalized volume. |

### Deep

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `DeepFill` | Identity | 0.1–0.4 | — | 0.3 | lowers the marched band's floor | Gas giant. Lowers the marched band's floor. |
| `FloorRelief` | Style | 0–0.7 | — | 0.5 | above 0 adds floor fetches | Gas giant. Above 0 adds floor fetches. |
| `FloorSoftness` | Fixed | keep | — | 0.25 | widens the floor's fetch band |  |
| `Darkening` | Fixed | keep | — | 0.01 | — |  |
| `MaterialDepth` | Style | 0–1 | — | 0.25 | — |  |
| `Scatter` | Identity | palette per class | — | (0.0676, 0.0325, 0.161) | — | The depths' colour; with `CloudScatter` and `StormScatter` the gas giant's palette. |
| `Extinction` | Fixed | keep | — | (1, 1, 1) | moves where rays stop | Moves the deep terminus where rays stop. |

### Material

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `CloudOpticalDepth` | Identity | T: 15–40 · GG: 8–20 | 20 | 10 | denser: rays end sooner |  |
| `StormBalance` | Style | 0.3–0.7 | 0.6 | 0.6 | — |  |
| `StormBlend` | Style | 0.3–0.6 | 0.6 | 0.5 | — |  |
| `CloudScatter` | Identity | T: near white (0.95–0.99) · GG: palette | (0.98, 0.98, 0.98) | (0.131, 0.588, 0.78) | — |  |
| `CloudExtinction` | Fixed | keep | (1, 1, 1) | (1, 1, 1) | — |  |
| `StormScatter` | Identity | palette per class | (0.188, 0.19, 0.259) | (0.394, 0.173, 0.352) | — |  |
| `StormExtinction` | Style | RGB keep; A 1.5–2.5 | (1, 1, 1) | (1, 1, 1) | — |  |

### Phase

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `ForwardG` | Fixed | keep | 0.9 | 0.9 | — |  |
| `BackwardG` | Fixed | keep | 0.1 | 0.1 | — |  |
| `ForwardWeight` | Fixed | keep | 0.5 | 0.5 | — |  |

### Multiple Scattering

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `SunlightPenetration` | Fixed | keep | 0 | 0 | — |  |
| `ScatteringGlow` | Fixed | keep | 0.6 | 0.6 | 0 skips the extra octaves |  |
| `ScatteringSpread` | Fixed | keep | 0.4 | 0.4 | — |  |
| `OctaveCount` | Quality | — | 3 | 3 | one term per octave per cloud sample |  |

### Surface Shadow

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `bEnabled` | Pipeline | — | on | on | shadow lookups on geometry pixels |  |
| `DirectFraction` | Style | 0.7–0.9 | 0.85 | 0.85 | — |  |
| `Strength` | Fixed | keep | 1 | 1 | 0 skips the tint read |  |
| `CascadeRadii` | Quality | — | (0.6, 0.3) | (6, 0.3) | 0 drops a cascade's reads | Y ≥ `DetailLayer.FadeFar`. |

### Pipeline and quality

| Setting | Class | Draw | Terrestrial | Gas giant | Cost | Rules and notes |
|---|---|---|---|---|---|---|
| `BlueNoise` | Pipeline | — | FastBlueNoise_scalar_128x128x64 | FastBlueNoise_scalar_128x128x64 | — |  |
| `Simulation.TerrestrialConfig` | Pipeline | the harness's config | TerrestrialSimDefault | TerrestrialSimDefault | — | See Driving the core. |
| `Simulation.GasGiantConfig` | Pipeline | the harness's config | GasGiantSimDefault | GasGiantSimDefault | — |  |
| `Simulation.bClaimSimulation` | Pipeline | — | on | on | — |  |
| `Raymarch.AtmosphereSteps` | Quality | — | 32 | 32 | air steps per ray |  |
| `Raymarch.CloudSteps` | Quality | — | 256 | 256 | cloud steps near the camera |  |
| `Raymarch.ChordSpread` | Quality | — | 8 | 8 | higher: longer far cloud steps |  |
| `Sampling.CellSize` | Quality | — | 4 | 4 | marches 1/CellSize² of the pixels |  |
| `Sampling.FreshWeight` | Quality | — | 0.15 | 0.15 | — |  |
| `Sampling.LatticeGrowth` | Quality | — | 0.2 | 0.2 | lower: more cloud steps |  |
| `Sampling.LatticeGrowthFar` | Quality | — | 0.02 | 0.02 | lower: more cloud steps from space |  |
| `ShadowResolution` | Quality | — | 256 | 256 | bake time and memory ∝ its square |  |
| `ShadowLevelsPerFrame` | Quality | — | 1 | 1 | cascades baked per frame |  |
| `ShadowTemporalSmoothing` | Quality | — | 0.05 | 0.05 | 0 skips the history reads |  |

**Quality tiers.** In rough order of cost:

1. **`CellSize`:** the march covers 1/CellSize² of the pixels per frame.
2. **`CloudSteps` and `LatticeGrowth`:** cloud steps per ray.
3. **`ShadowResolution` and `ShadowLevelsPerFrame`:** bake texels per frame.
4. **`OctaveCount`:** per cloud sample.
5. **`AtmosphereSteps`:** air steps per ray.

On the sim side, cost is grid cells × steps per frame. That is `GridResolution`² × `LayerCount` × `SimSpeed` / (`StepSize` × turnover). `GridResolution` and `StepSize` are part of the tune, though, because the weather depends on them, and `GridResolution` must match the snapshot library's tier.

## The sim config

The Acts column says how a change reaches the picture:

- **resim:** the state passes read it, so the weather drifts to a new balance over some turnovers. A snapshot captured under another value starts off balance and relaxes the same way.
- **hurricanes:** the storm cell pass reads it. Live hurricanes respond within their lifetime, and new ones spawn under it.
- **instant:** only the output passes read it, so it shows on the next frame and snapshots stay valid.
- **tempo:** it changes how fast sim time passes or the step.
- **reset:** it changes the grid and restarts the sim. A snapshot must match it.

### Winds

| Setting | Class | Draw | Terrestrial | Gas giant | Acts | Rules and notes |
|---|---|---|---|---|---|---|
| `ZonalProfile` | Identity | T: ThreeCell · GG: Banded | ThreeCell | Banded | resim, hurricanes, instant | Also sets hurricane spin. |
| `JetLatitude` | Identity | 35–55 | 45 | 45 | resim, hurricanes, instant | ThreeCell. |
| `TradeWindStrength` | Identity | 0.2–0.5 | 0.35 | 0.35 | resim, hurricanes, instant | ThreeCell. |
| `PolarEasterlyStrength` | Identity | 0.3–0.7 | 0.5 | 0.5 | resim, hurricanes, instant | ThreeCell. |
| `DeformationRadius` | Identity | 0.15–0.3 at `GridResolution` 64 | 0.2 | 0.25 | resim, hurricanes, instant | At least about 6 grid cells: ≥ 3π / `GridResolution`. Rescales every speed and length. |
| `SpeedRoot` | Identity | 0.5–0.8 | 0.6 | 0.8 | tempo, resim, hurricanes, instant | Also sets the turnover, so the tempo. |
| `JetSpeed` | Identity | T: 0.3–0.6 · GG: 0.3–0.5 | 0.5 | 0.4 | resim, hurricanes, instant | Top layer: `JetSpeed` × its `JetScale` within 0.7. |
| `ShearSpeed` | Identity | 0.5–1.1 | 1.05 | 1.05 | resim, hurricanes, instant | Storms grow past the start log's criterion. Above 0.7 the ceiling eases the faces; both tunes run 1.05. |
| `BandCount` | Identity | 3–8 | 3 | 5 | resim, hurricanes, instant | Banded. |
| `BaroclinicLatitude` | Identity | 20–50 | 20 | 30 | resim, hurricanes, instant | Midlatitude thermal shape. |
| `BaroclinicWidth` | Style | 20–45 | 30 | 45 | resim, hurricanes, instant | Must span a few deformation radii. |
| `EquatorialBoost` | Identity | 0–1 | 0 | 0.5 | resim, hurricanes, instant | Banded. |
| `Asymmetry` | Style | 0–0.6 | 0.5 | 0.5 | resim, hurricanes, instant | Banded. |
| `WidthBias` | Style | −0.3–0.3 | 0 | 0 | resim, hurricanes, instant | Banded. |
| `JetIrregularity` | Identity | 0.2–0.6 | 0.45 | 0.45 | resim, hurricanes, instant | Banded. |
| `JetHarmonic` | Style | 1.3–1.9 | 1.7 | 1.7 | resim, hurricanes, instant | Banded. Avoid whole-number ratios. |
| `JetFlatness` | Style | 0.3–0.6 | 0.5 | 0.5 | resim, hurricanes, instant | Banded. |
| `EquatorialJetWidth` | Style | 8–25 | 13.9 | 13.9 | resim, hurricanes, instant | Banded. |
| `ThermalShape` | Identity | T: Midlatitude · GG: by tune | Midlatitude | Midlatitude | resim, hurricanes, instant |  |
| `PlanetaryVorticity` | Style | 4–16 | 6 | 12 | tempo, resim, hurricanes, instant | Tempo, with `SimSpeed`. The actor's `SpinRatio` is a share of it. |

### Flow Rates

| Setting | Class | Draw | Terrestrial | Gas giant | Acts | Rules and notes |
|---|---|---|---|---|---|---|
| `NudgeRate` | Fixed | keep | 0.196 | 0.196 | resim |  |
| `DragRate` | Fixed | keep | 0.196 | 0.196 | resim |  |
| `ThermalRelaxation` | Fixed | keep | 0.0491 | 0.0491 | resim |  |

### Stirring

| Setting | Class | Draw | Terrestrial | Gas giant | Acts | Rules and notes |
|---|---|---|---|---|---|---|
| `EddySpeed` | Identity | 0.2–0.6 | 0.6 | 0.25 | resim | Top layer: × its `EddyScale` within 0.7. With `GenesisMoisture` 0, sets hurricane frequency. |
| `ForcingLifetime` | Fixed | keep | 2.5 | 3 | resim |  |
| `ForcingFrequency` | Fixed | keep | 0.03 | 0.03 | resim |  |
| `ForcingVolume` | Pipeline | per tune | VT_PerlinWorley_S4_128 | CloudNoise_4_128 | resim | Its features set the eddy scale. |
| `ForcingChannel` | Fixed | keep | 0 | 0 | resim |  |

### Moisture

| Setting | Class | Draw | Terrestrial | Gas giant | Acts | Rules and notes |
|---|---|---|---|---|---|---|
| `CondensationOnset` | Identity | 0.4–0.7 | 0.5 | 0.5 | resim, hurricanes |  |
| `SurfaceEvaporation` | Identity | 0.2–0.6 | 0.393 | 0.4 | resim |  |
| `SaturationPoleRatio` | Identity | 0.25–1 | 1 | 1 | resim |  |
| `LatentHeating` | Style | 0–0.15 | 0.1 | 0.1 | resim | A feedback: high values run away into grid-scale convection. |
| `CloudLifetime` | Identity | 8–30 | 10 | 20 | resim, hurricanes |  |
| `UpperSaturation` | Fixed | keep (0.3–0.6, never 0) | 0.5 | 0.5 | resim |  |
| `WindEvaporationGain` | Fixed | keep | 2 | 2 | resim |  |
| `CondensationRate` | Fixed | keep | 0.786 | 0.786 | resim |  |
| `EvaporationRate` | Fixed | keep | 0.196 | 0.196 | resim |  |

### Storms

| Setting | Class | Draw | Terrestrial | Gas giant | Acts | Rules and notes |
|---|---|---|---|---|---|---|
| `StormThreshold` | Identity | 0.05–0.2 | 0.1 | 0.1 | resim |  |
| `StormAmount` | Identity | 8–64 | 16 | 64 | resim, hurricanes |  |
| `StormLifetime` | Style | 10–40 | 10 | 40.7 | resim |  |
| `StormSpin` | Fixed | keep | 2 | 2 | resim |  |

### Hurricanes

| Setting | Class | Draw | Terrestrial | Gas giant | Acts | Rules and notes |
|---|---|---|---|---|---|---|
| `MaxStormCells` | Identity | 0–12 | 3 | 0 | resim, hurricanes, instant | 0 for no hurricanes. Shares 32 slots with the perpetual storms. |
| `StormCellSpawnRate` | Identity | 0.5–6 | 1 | 6 | hurricanes |  |
| `GenesisLatitudeMin` | Style | 5–10 | 8 | 8 | hurricanes | A few degrees off the equator. |
| `GenesisLatitudeMax` | Style | 20–35 | 28 | 35 | hurricanes | ≤ 45, the range genesis is tested over. |
| `StormCellLifetime` | Style | 15–60 | 20 | 256 | hurricanes |  |
| `StormCellSustainRatio` | Style | 1–3.3 | 1.2 | 3.33 | hurricanes | From about 1.25 a hurricane keeps its parent storm alive. |
| `StormCellRadius` | Identity | 2–4 | 3 | 4 | hurricanes, instant | `StormCellRadius` × `DeformationRadius` ≤ 0.785 (45°). |
| `StormCellEyewall` | Style | 0.1–0.2 | 0.1 | 0.15 | hurricanes, instant | Eyewall at least two grid cells: `StormCellRadius` × `DeformationRadius` × `StormCellEyewall` ≥ π / `GridResolution`. |
| `StormCellWind` | Identity | 0.5–0.75 | 0.7 | 0.8 | hurricanes | Plus the background wind it rides on, within 0.7. |
| `StormCellCloudCover` | Style | 0.7–0.85 | 0.75 | 0.8 | hurricanes |  |
| `StormCellSpacing` | Style | 1.5–3 | 2 | 2 | hurricanes |  |

### Hurricane Dynamics

| Setting | Class | Draw | Terrestrial | Gas giant | Acts | Rules and notes |
|---|---|---|---|---|---|---|
| `GenesisShearRatio` | Fixed | keep | 1.35 | 1.34 | hurricanes |  |
| `GenesisHumidityMargin` | Style | 0–0.25 | 0.25 | 0 | hurricanes |  |
| `StormCellDryTolerance` | Fixed | keep | 0.15 | 0.15 | hurricanes |  |
| `GenesisStormRatio` | Fixed | keep | 0.25 | 0.25 | hurricanes |  |
| `GenesisMoisture` | Identity | 0–1 | 0 | 0 | hurricanes | Frees hurricane frequency from `EddySpeed`. |
| `GenesisSpin` | Fixed | keep | 0.05 | 0.05 | hurricanes |  |
| `StormCellGrowth` | Fixed | keep | 0.786 | 0.786 | hurricanes |  |
| `StormCellMaturity` | Fixed | keep | 0.25 | 0.25 | hurricanes |  |
| `StormCellPersistence` | Fixed | keep (< 1 with hurricanes) | 1 | 1 | hurricanes | At 1 hurricanes never decay and the slots fill. |
| `StormCellDriftSpeed` | Fixed | keep | 0.02 | 0.02 | hurricanes |  |
| `StormCellFollow` | Fixed | keep | 0.393 | 0.393 | hurricanes |  |
| `StormCellCoreFollow` | Fixed | keep | 1.57 | 1.57 | hurricanes |  |
| `StormCellFalloff` | Fixed | keep | 2 | 2 | hurricanes, instant |  |
| `StormCellWindBreadth` | Fixed | keep | 0.25 | 0.25 | hurricanes |  |
| `StormCellEyeStrength` | Fixed | keep | 0 | 0 | hurricanes, instant |  |
| `StormCellForcing` | Fixed | keep | 3 | 3 | hurricanes |  |
| `StormCellTopShare` | Fixed | keep | -0.33 | -0.33 | hurricanes |  |
| `StormCellInflow` | Style | 0.2–0.5 | 0.5 | 0.5 | hurricanes | Spiral arm angle. |
| `StormCellEyeDraft` | Fixed | keep (negative) | -0.33 | -0.33 | resim, instant |  |
| `StormCellEyeRatio` | Fixed | keep | 0.05 | 0.05 | hurricanes, instant |  |

### Hurricane Look

| Setting | Class | Draw | Terrestrial | Gas giant | Acts | Rules and notes |
|---|---|---|---|---|---|---|
| `StormCellStorm` | Style | 0.5–1 | 0.75 | 0.75 | instant |  |
| `StormCellStormBlend` | Fixed | keep | 0.25 | 0.25 | instant |  |
| `StormCellPressure` | Style | 1–2 | 2 | 2 | instant |  |
| `StormCellEyeDepth` | Style | 0.7–1 | 1 | 1 | instant |  |
| `StormCellEyeSoftness` | Style | 0.5–1 | 1 | 1 | hurricanes |  |
| `StormCellEyeRate` | Fixed | keep | 3.14 | 3.14 | resim |  |
| `StormCellEyeTrail` | Style | 0.5–2 | 1.27 | 1.27 | resim |  |
| `StormCellBandExcess` | Fixed | keep | 0.25 | 0.25 | instant |  |
| `StormCellBandFloor` | Fixed | keep | 0.4 | 0.4 | instant |  |
| `StormCellDraft` | Fixed | keep | 0.15 | 0.15 | instant |  |

### Perpetual Storms

| Setting | Class | Draw | Terrestrial | Gas giant | Acts | Rules and notes |
|---|---|---|---|---|---|---|
| `PerpetualStorms` | Identity | 0–3 storms (GG) | 0 entries | 2 entries | resim, hurricanes, instant | Each pair at least 1.5 (R1 + R2) apart. Variation over time is PROC-03. |
| `↳ Latitude` | Identity | a shear zone (PROC-08) | per entry | per entry | hurricanes, instant | Settles at the nearest flow reversal within 45°. |
| `↳ Longitude` | Identity | −180–180 | per entry | per entry | hurricanes, instant |  |
| `↳ Radius` | Identity | 0.5–1.5 | per entry | per entry | hurricanes, instant |  |
| `↳ Aspect` | Identity | 1–2.5 | per entry | per entry | hurricanes, instant |  |
| `↳ Wind` | Identity | 0.4–0.7 | per entry | per entry | hurricanes |  |
| `↳ Spiral` | Style | 0–0.6 | per entry | per entry | hurricanes |  |
| `↳ Steering` | Style | 0–0.5 | per entry | per entry | hurricanes, instant |  |
| `↳ Drift` | Style | −0.1–0.1 | per entry | per entry | hurricanes, instant |  |
| `↳ Storm` | Style | 0.6–1 | per entry | per entry | instant |  |
| `↳ Cover` | Style | 0.6–0.9 | per entry | per entry | hurricanes |  |
| `↳ Lift` | Style | 0.2–0.6 | per entry | per entry | instant |  |
| `↳ Eye` | Style | 0–0.6 | per entry | per entry | hurricanes |  |
| `PerpetualStormForcing` | Fixed | keep | 3 | 3 | hurricanes |  |
| `PerpetualStormClearance` | Fixed | keep | 1.5 | 1.5 | hurricanes |  |

### Noise Motion

| Setting | Class | Draw | Terrestrial | Gas giant | Acts | Rules and notes |
|---|---|---|---|---|---|---|
| `NoiseDriftSpeed` | Fixed | keep | 0.35 | 0.35 | resim |  |
| `NoiseResetTurnovers` | Fixed | keep | 8 | 12 | resim |  |

### Time

| Setting | Class | Draw | Terrestrial | Gas giant | Acts | Rules and notes |
|---|---|---|---|---|---|---|
| `SimSpeed` | Style | per game | 0.005 | 0.005 | tempo | Tempo. Cost scales with it. |
| `SpinUpTurnovers` | Pipeline | — | 0 | 0 | tempo | Unused with an `InitialState`. |

### Quality

| Setting | Class | Draw | Terrestrial | Gas giant | Acts | Rules and notes |
|---|---|---|---|---|---|---|
| `GridResolution` | Quality | — | 64 | 64 | reset | Must match the snapshot's. Couples to `DeformationRadius` and the eyewall rule. |
| `StepSize` | Fixed | keep | 0.000509 | 0.00136 | tempo, resim, hurricanes | Changes the look, so it is part of the tune. |
| `LayerCount` | Fixed | keep (2) | 2 | 2 | reset | Must match the snapshot's. |

### Layers

| Setting | Class | Draw | Terrestrial | Gas giant | Acts | Rules and notes |
|---|---|---|---|---|---|---|
| `LayerProfiles` | Fixed | keep | 2 entries | 2 entries | resim, hurricanes |  |
| `↳ JetScale` | Fixed | keep | per entry | per entry | resim, hurricanes, instant |  |
| `↳ BoostScale` | Fixed | keep | per entry | per entry | resim, hurricanes, instant |  |
| `↳ EddyScale` | Fixed | keep | per entry | per entry | resim |  |
| `↳ DragScale` | Fixed | keep | per entry | per entry | resim |  |
| `↳ DepthScale` | Fixed | keep | per entry | per entry | resim |  |
| `LayerCoupling` | Fixed | keep | 0.0196 | 0.0196 | resim |  |
| `Stratification` | Fixed | keep | 0.05 | 0.05 | resim |  |

### Numerics

| Setting | Class | Draw | Terrestrial | Gas giant | Acts | Rules and notes |
|---|---|---|---|---|---|---|
| `GridDamping` | Fixed | keep | 1.28 | 1.28 | resim |  |
| `FilterLatitude` | Fixed | keep | 0.9 | 0.9 | resim |  |

---

Generated by `Tools/UsageGuide/gen_harness.py` from a `CloudAtmosphere.DumpSchema` file, the stage map (`stages.json`) and the classes and draws in `harness_desc.py`. The Terrestrial and Gas giant columns are the current tunes. Regenerate after a parameter change or a sweep:

    python3 gen_harness.py AtmosphereSchema.json stages.json HarnessReference.md
