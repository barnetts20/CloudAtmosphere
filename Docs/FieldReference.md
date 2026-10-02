# Cloud field reference

How the cloud field turns the sim's weather into 3D cloud, for both models, and why it is built the way it is. Per-setting guidance (ranges, defaults, what to reach for) is in `Design/UsageGuide.md`; this document covers the mechanics behind those settings. The shadow bake's loop, the march's lattice and the temporal resolve are in `Design/RenderReference.md`; the sim's own fields are in `Design/SimReference.md`.

## 1. Overview

The sim is the weather map and the noise is the cloud. Each column reads the sim once for its coverage, type, pressure, formation altitude and vertical motion. A slab between a condensation base and a type-dependent top bounds the column exactly; outside it density is zero. Inside, a structure noise blended by genus and shaped by a height profile survives by coverage, and a detail noise erodes the edges. Both noises are sampled through displacement fields the sim advects with the flow.

One field serves both cloud models:

| Model | `PlanetType` | March `ATMO_MODEL` | `TR_DEEP_DECK` | `ATMO_FIELD_CLOUDS` | Bake and coverage pass |
|---|---|---|---|---|---|
| Terrestrial slab | `Terrestrial` | 0 | 0 | 1 | bake's `FDeepDeck` permutation 0; coverage pass shared |
| Deep deck | `GasGiant` | 1 | 1 | 1 | bake's `FDeepDeck` permutation 1; coverage pass shared |
| Air only | `AirOnly` | 2 | 0 | 0 | no field, no sim claim, no bake |

The deep deck is the same slab over a deck that fills to full density below each column's base (section 4). The coverage pass (`FTerrestrialCoverageCS`) has no permutation, since `TR_Priority` is the same for both models. Air only compiles no cloud path: `Field_SegmentIsCloud` returns false and `TR_BuildAtmo` puts both cloud radii at the surface with zero extinction.

**Per frame** (`APlanetAtmosphereActor::UpdateAtmosphere`): the actor packs the field with `PackCloudField`, the one packer for the bake request and the march; the bake request's graph runs the coverage pass (`MainCoverageCS`) first, then the shadow bake; the march reads the coverage texel and the map.

**Units.**

| Quantity | Unit | Examples |
|---|---|---|
| Heights | Atmosphere fractions: `h = (r - R) / (R · HeightScale)`, 0 at the surface, 1 at the shell top | `CloudBase`, `CloudThickness`, `CeilingFalloff`, `DeepFill`, `SolvedTopMax` |
| Offsets and shares | Multiples or shares of `CloudThickness` | `BaseTropical`, `BasePressure`, `AltitudeLift`, `WarpShift`, `CeilingPressure`, `SurfaceSoftness`, `StratusDepth` |
| Deck depths | Fills, multiples of `DeepFill` | `FloorRelief`, `FloorSoftness`, `MaterialDepth` |
| Camera distances | Planet radii | `FadeNear`, `FadeFar`, `CascadeRadii` |
| Noise frequency | Noise units (volume tiles) per radian | `Scale` |

Space is planet-local with the spin axis on Z. The sim runs in the rotating frame, so every column read first spins its direction: `N = normalize(Atmo_RotateZ(dir, SpinAngle))`, `SpinAngle = -½ · PlanetaryVorticity · SpinRatio · t`, wrapped in double on the CPU.

**Files.**

| File | Holds |
|---|---|
| `TerrestrialDeck.ush` | `TRFieldParams`, `TR_BuildField`, the column, the profile, the noise layers, the deep fill and floor, the band bounds, `TR_SlabEntry` |
| `TerrestrialField.ush` | The `Field_*` interface, `TRScatterParams` and the materials, `TR_BuildAtmo` |
| `TerrestrialShadow.ush` | Deep material weight, tint range, least extinction, the shadow frame |
| `TerrestrialShadowMap.usf` | The bake's field hooks (`BakeField_*`) and `MainCoverageCS` |
| `FlowField.ush` | The sim atlas reader (`Flow_Sample*`) |
| `AtmosphereParams.h` | Every authored setting |
| `PlanetAtmosphereActor.cpp` | `PackCloudField`, `NoiseClock`, `SolveFieldBounds` |
| `TerrestrialShadowMap.h` | `FTerrestrialFieldParameters` |

## 2. The column

`TR_ColumnFromInputs` builds a `TRColumn` from one atlas read at a spun direction. Nothing outside it moves the slab, so its bounds are exact.

### 2.1 What a column reads

`Flow_SampleColumn` reads one layer's slices in a single pass over the cube faces, each slice a cubic B-spline through four bilinear taps, cross-faded across face edges (`FLOW_ATLAS_BLEND`). The layer is `TR_FlowLayer`: `CloudLayer`, negative for the bottom of the stack, clamped to the stack. The atlas holds `4L` slices for `L` layers.

| Slice | Channel | Field use |
|---|---|---|
| Flow `[0, L)` | z | Pressure, soft-saturated, highs positive. The velocity channels are in the face basis and unread. |
| Weather `[L, 2L)` | x | `W`, vertical motion, rising positive: genus subsidence, warp |
| | y | Column cloud from the top of the stack down to this layer: priority, storm index |
| | z | Formation altitude, 0 bottom of the stack to 1 top: base lift, genus, cirrus share |
| | w | Storm, the strongest in the column: priority, type, storm index |
| Noise A `[2L, 3L)` | xyz | Phase A displacement (density samples only) |
| Noise B `[3L, 4L)` | xyz | Phase B displacement |
| | w | Depth factor, `1 - StormCellEyeDepth · eye tracer`: thins the column and its density across an eye |

`TR_Density` reads all four slices; `TR_ReadColumn` (coverage pass, slab entry) skips noise A; `TR_TypeCoarse` reads the weather slice alone.

### 2.2 Coverage

**Priority** (`TR_Priority`, shared by the march, the bake and the coverage pass):

```
Amount   = 1 - exp(-3 · max(cloud, 0) / CloudFull)
Priority = Amount · (1 + StormPriority · storm) / (1 + StormPriority)
```

The amount is 95% of full at `CloudFull` with no slope break. Plain cloud tops out at `1 / (1 + StormPriority)`; full storm reaches 1.

**Threshold** (`MainCoverageCS`, one 256-thread group): `TR_COVERAGE_SAMPLES` (4096) directions on a Fibonacci sphere, so each sample stands for equal area, each read with `TR_ReadColumn`. Priorities go into 256 bins over [0, 1]. Walking down from the top bin, the threshold `T` is where `CloudCover · 4096` samples lie above, interpolated linearly within the bin that reaches it. `CloudCover` 0 gives `T = 1 + S`. The result is clamped to `[S, 1 + S]`, `S = CoverageSoftness`, and written to the one-texel `CoverageTarget`, which `TR_BUILD_FIELD` loads as `CoverThreshold`. The target clears to 2, so nothing is covered before the first pass.

**Coverage** per column:

```
Coverage = smoothstep(T - S, T + S, Priority)
```

| Setting | Mechanism |
|---|---|
| `CloudCover` | The quantile: the share of the sphere ranked above `T`. Holds the covered share steady whatever the sim's cloud amount, up to the share that holds cloud. |
| `CoverageSoftness` | Half-width of the ramp in priority. Because `T ≥ S`, priority 0 always maps to coverage 0, so cloudless columns stay clear. Above 0.5, `T + S > 1` and no column reaches full coverage. |
| `CoverageFray` | Raises the shape threshold as coverage falls (section 3.4), so edges fray into cores instead of fading. |
| `CoverageDepthRamp` | Coverage over which column depth grows from 0 to full, `smoothstep(0, CoverageDepthRamp, Coverage)` (section 2.4). Clear sky has no slab. |

### 2.3 Type and genus

```
Tropical = 1 - |N.z|
Type     = saturate(TypeBias + TypeTropical · Tropical + TypeStorm · storm^TypeCurve)
```

Type sets the column's depth, its genus, the cirrus share (`Altitude · (1 - Type)`) and how far formation altitude lifts the base (`AltitudeLift · Altitude · (1 - Type)`, section 2.4). It never reaches the material (section 5).

The genus is a weight vector over the structure volume's four channels:

```
b       = saturate(-W · Subsidence)
Layered = lerp(lerp(Stratus, Stratocumulus, b), Cirrus, Altitude)
Genus   = lerp(Layered, Cumulus, Type)
Cirrus  = Altitude · (1 - Type)          // also turns the detail fibrous
```

Layered cloud is stratus, cellular where the air sinks and cirrus where it formed high; towering cloud is cumulus at any height.

### 2.4 Lift, lid and depth

```
Low      = -pressure / PressureScale
Pressure = Low / sqrt(1 + Low²)                       // lows positive, (-1, 1)
Altitude = saturate(AltitudeGain · weather.z)

Base  = CloudBase + CloudThickness · (BaseTropical · Tropical
                                      + BasePressure · Pressure
                                      + AltitudeLift · Altitude · (1 - Type))
Lid   = 1 + CeilingPressure · Pressure
Depth = CloudThickness · lerp(StratusDepth, 1, Type) · Lid
        · smoothstep(0, CoverageDepthRamp, Coverage) · DepthFactor
```

The base is the condensation level from tropicality and pressure, plus formation altitude for all but towers, so outflow becomes high layered cloud. No noise touches it. `DepthFactor` is noise B's w: an eye is a bowl sloping to its floor.

**Top.** The reach `Base + Depth` is capped at the shell on a smooth minimum over the ceiling band, never below the base:

```
Knee = max(CeilingFalloff - |Reach - 1|, 0) / CeilingFalloff
Top  = max(min(Reach, 1) - ¼ · CeilingFalloff · Knee², min(Base, 1))
```

**Eye factor.** `Thinning` (the depth factor) also scales density, `pow(Thinning, EyeOpenPower)`, after erosion, coverage and detail (section 3.6).

### 2.5 The marched band

`TR_FieldReach` is the one derivation of how far the base can move and how deep a column can get. Every term in `TR_ColumnFromInputs` has to appear in it:

```
BaseUp   = CloudThickness · (max(BaseTropical, 0) + |BasePressure| + max(AltitudeLift, 0))
BaseDown = CloudThickness · (-min(BaseTropical, 0) + |BasePressure| - min(AltitudeLift, 0))
Depth    = CloudThickness · (1 + |CeilingPressure|)

TopMax  = min(CloudBase + BaseUp + Depth, 1)       // TR_TopMax
BaseMin = CloudBase - BaseDown                     // TR_BaseMin
```

The bounds are planet-wide, not this frame's, so the band does not narrow when the sky is quiet and the step lattice does not move with the weather.

| Consumer | Uses |
|---|---|
| March band (`TR_BuildAtmo`) | Outer radius `TR_ShadowCullRadius = R + R·HeightScale · TopMax`; inner radius at `BaseMin` (slab, floored at the surface) or `TR_DeckFull` (deep, floored at 0.1 R) |
| Shadow map | Cull radius and level 0 extent from the same `TR_ShadowCullRadius` |
| Bake entry | `TopMax` and `BaseMin` as the cone trace's geometric early-outs |
| Readouts | `SolveFieldBounds` mirrors the derivation into `SolvedTopMax` and `SolvedBaseMin` on the active model's Shape group. Display only; a mismatch misreports and changes nothing drawn. |

On the deep deck the band runs below `SolvedBaseMin` to `TR_DeckFull = BaseMin - max(DeepFill, SpanTop)`.

## 3. Density

`TR_Density` reads the column where the sample stands, returns zero outside the slab (`TR_SlabMissed`, tested on the step's near edges), and otherwise calls `TR_DensityInColumn`. In order:

1. Profile peak within the step, times the ceiling fade.
2. Warp: the noise's height coordinate.
3. Structure noise erodes the profile into a shape.
4. A threshold on the shape, then coverage as a scale.
5. Detail erosion.
6. The exact step mean of the result.
7. Eye thinning; on the deep deck, the floor.

It returns `float4(density, material, z, depth below base)`, where z is the column's pressure on the slab (no consumer reads it) and `TR_DeepBuried` on the deep deck.

### 3.1 Height profile

Each end ramps over `Span = clamp(SurfaceSoftness, 1e-4, 0.5) · CloudThickness` (`SpanTop`, `SpanBase`):

```
SlabDensity = smoothstep(0, 1, FromTop^TopCurve) · smoothstep(0, 1, FromBase^BottomCurve)
FromTop     = saturate((Top - h) / RampTop),  FromBase = saturate((h - Base) / RampBase)
```

For `TopCurve` and `BottomCurve` above 0.5, each ramp peaks at exactly 1 with zero slope at both ends. On the slab, a column shallower than both ramps scales them down together (`TR_SlabRamps`): `Fit = Ratio / (1 + Ratio⁴)^¼`, `Ratio = depth / (SpanTop + SpanBase)`, a smooth `min(Ratio, 1)`. A shallow column is therefore thin, not faint. Under `TR_DEEP_DECK`, `TR_SlabRamps` returns the spans unfitted: a deep column always has room for its top ramp, which keeps its full length (section 4).

The ceiling fade, `1 - smoothstep(1 - CeilingFalloff, 1, h)`, multiplies the profile, by Simpson's rule across the step.

`TR_CloudBeta` solves the extinction per atmosphere thickness so a full-depth column (`CloudThickness` deep, unfitted ramps) at density 1 accumulates `CloudOpticalDepth`:

```
Mean(c) = 3 / (2c + 1) - 2 / (3c + 1)
Beta    = CloudOpticalDepth / (Mean(TopCurve) · SpanTop + Mean(BottomCurve) · SpanBase
                               + CloudThickness - SpanTop - SpanBase)
```

Reshaping the profile leaves the opacity where it was put. On the slab the fit shortens a full column's ramps slightly, so such a column reads a few percent over `CloudOpticalDepth`. A low's lid (`Lid > 1`) makes a column deeper than `CloudThickness` and denser than `CloudOpticalDepth` in proportion.

### 3.2 Noise sampling

**Frequency.** `TR_NoiseFrequency`: `UVWScale = Scale · exp(Aspect · HeightScale · Rise)`. The sample coordinate is the unit direction times `UVWScale`, so the vertical-to-horizontal ratio is `Aspect` at every height and stays positive below the base. One noise unit is one tile of the volume; a great circle at the surface spans `2π · Scale` tiles.

**Warp.** Rising air stretches and lifts the noise; nothing in the bounds moves.

```
r        = (Top - Base) / (CloudThickness · max(StratusDepth, 0.1))
DepthFit = (1 + r⁴)^¼                                   // smooth max(r, 1)
Stretch  = max(1 + WarpStretch · W, 0.1) · DepthFit
Shift    = WarpShift · CloudThickness · W
Rise     = Base + (h - Base - Shift) / Stretch
```

The depth stretch makes a column of any depth span about as many noise features as a stratiform one, so erosion cuts a tower as it cuts a thin deck. The stretch is anchored at the column's base, so a change in depth leaves the base's phase alone.

**Flow-carried displacement.** The sim advects two displacement fields per layer, phase B reset half a period after phase A. Each step it carries the departure point's coordinate and turns it about Z by `NoiseDriftRate · dt` (`NoiseDriftSpeed` × the speed root); the displacement is that coordinate less the cell's direction. A phase resets to zero displacement when its clock crosses a whole period (`NoiseResetTurnovers` turnovers, at least two spin-up steps). `TR_SampleLayer` samples each layer through both phases and crossfades:

```
PA = Atmo_RotateZ(normalize(N + DispA · FlowInherit), -DriftAngle)
PB = Atmo_RotateZ(normalize(N + DispB · FlowInherit), -DriftAngle)
value = lerp(Vol(PB · UVWScale), Vol(PA · UVWScale), WeightA)
WeightA = 1 - |2 · frac(NoisePhase) - 1|                 // Flow_NoiseWeightA
```

Each phase's weight is zero at its own reset. `NoiseClock` computes `DriftAngle` and `NoisePhase` from the sim clock in double precision; the phase must be the sim's own so the weight is zero where the sim resets. Each layer has its own `FlowInherit`.

**Level of detail.** `TR_NoiseLod`: `Lod = max(log2(PixelFootprint · UVWScale · VolumeWidth) + MipBias, 0)`. The march sets `PixelFootprint` per sample (distance in planet radii × the pixel's angle); the bake sets it to half a map texel in radians, `0.5 · TexelWorld · max(|BasisU|, |BasisV|) / |Pos|`, about the spacing between its subsamples (`TRShadow_Sigma`). The mip prefilters octaves finer than the pixel instead of letting them shimmer; the channels are fBm, so the octaves removed first carry little variance.

### 3.3 Structure

```
Channels = (R, 1 - G, B, A)                             // TR_StructureChannels
Noise    = saturate(0.5 + dot(Channels - 0.5, Genus) / length(Genus))   // TR_BlendNoise
Shape    = Profile · saturate(1 + Erosion · (Noise - 1))
```

Dividing by the weights' length keeps the contrast of a blend of independent channels. Past `Erosion` 1 the factor goes negative wherever `Noise < 1 - 1/Erosion`, and those holes survive any coverage. The layer is active only with a volume bound and `Erosion > 0` (`StructureActive`); otherwise `Shape = Profile`.

### 3.4 Threshold and coverage

```
Cut       = Breakup · saturate(Erosion)
Threshold = Cut + (1 - Cut) · (1 - Coverage) · CoverageFray
D0        = saturate((Shape - Threshold) / (1 - Threshold)) · Coverage
```

Erosion sets the threshold everywhere, so a thick system breaks up as much as a thin one. At full coverage a gap opens where `1 - Erosion · (1 - Noise) ≤ Cut`; at the noise minimum that is `Erosion ≥ 1 / (1 + Breakup)`. `CoverageFray` raises the threshold as coverage falls, so partly covered edges keep only the noise's cores.

### 3.5 Detail

The detail layer erodes edges. Its channels are read as `TR_DetailChannels`: x = R (wispy), y = G (billow), z = 1 - A (fibrous); B is unread.

```
Billow   = saturate(HeightFraction / BillowHeight)       // HeightFraction = (h - Base) / (Top - Base)
Weights  = (1 - Billow, Billow, 0) · (1 - Cirrus) + (0, 0, Cirrus)
Strength = DetailFade · saturate(max(Levels - 1 - TR_DETAIL_MEAN_MIPS, 1) - Lod)
Modifier = lerp(FadeMean, TR_BlendNoise(Channels, Weights), Strength)
e        = Modifier · DetailErosion · TR_DETAIL_EROSION_SCALE      // 0.35
```

`DetailFade` is `1 - smoothstep(FadeNear, FadeFar, d)` in the march, `d` the jittered sample's distance from the camera in planet radii. The bake sets it per cascade: 1 at level `TR_SHADOW_DETAIL_LEVEL` (2) and finer, 0 coarser. The mip term fades the fetch out across the mip before `TR_DETAIL_MEAN_MIPS` (3) from the top of the chain, floored at one mip so a volume without mips still fades by footprint. Where `Strength` is 0 the layer reads `FadeMean` without a fetch: distant cloud and coarse cascades keep the same mean erosion without the grain.

Applied to the density at each profile level, the detail subtracts and renormalizes:

```
D = saturate((D0 - e) / (1 - e))
```

`TR_DensityInColumn` folds this into the step integral as a profile level `P0` and a gain (section 3.6).

### 3.6 Step integration and the eye

The noise is fetched once per step, at the sample's own height (`Rise` and the billow's `HeightFraction` both use it). Only the profile value is taken at its peak within the step (`PeakSlab`), which scales the shape and gates the fetches. With the noise fixed across the step, the density is a linear function of the profile above a level `P0` and zero below it. `TR_SlabExcessCumulative` integrates the profile's excess over `P0` across the step in closed form (base ramp, plateau, top ramp), and the step's density is `Gain · mean excess`. This is the mean of the density, not the density of the mean: thresholding a step-averaged profile clears any step longer than the slab is deep. The march passes `StepHalfHeight`, the step's radial half-extent in atmosphere fractions.

Finally `Density *= pow(saturate(Thinning), EyeOpenPower)`: the eye's floor fades as it thins, after erosion and coverage so the eyewall keeps its shape.

### 3.7 Noise volumes

Both layers use the same channel layout, each channel a noise type (`AtmosphereParams.h`):

| Channel | Noise | Structure reads | Detail reads |
|---|---|---|---|
| R | Perlin fBm | smooth, as R | wispy, as R |
| G | Worley F1 | billow, as 1 - G | billow, as G |
| B | Worley F2 - F1 | cellular, as B | unread |
| A | Ridged Perlin | fibrous, as A | fibrous, as 1 - A |

Requirements:

- **Tiling** on all three axes: both passes sample with trilinear wrap.
- **Full mip chain**, for `TR_NoiseLod` and the detail's mean cutoff.
- **Equalized** channels: `TR_BlendNoise` assumes channels distributed about 0.5, and `FadeMean` 0.5 is exact only for an equalized detail volume.
- **Isotropic**: the volume is sampled at planet-local positions, so a volume axis maps to different ground directions across the planet. Anisotropy belongs in the sampling (`Aspect`, warp).
- **Separate assets** for the two layers, so their features do not rhyme.

A layer without a volume is flagged unread in `NoiseLevels` (y, w) rather than read as black.

## 4. The deep deck

`TR_DEEP_DECK` 1. The column, the profile's top ramp and everything above the base are the slab's. Below the base:

**Fill.** `Fill = smoothstep(0, DeepFill, Base - h)` (`TR_DeepFill`). At the sample, coverage lerps to 1, erosion and detail erosion scale by `1 - Fill`, and thinning lerps to 1. The base ramp is dropped (`FromBase = 1`) and the plateau runs down from the top ramp; the step integral starts at the step's own bottom. Structure and detail fetches stop where `Fill = 1`, except in the floor's band.

**Floor.** Full-density mounds rising into the fill (`TR_DeepFloor`):

```
FloorTop = min(Base - DeepFill · (1 - FloorRelief · StructureNoise) + Offset, Base)
Offset   = DeepFill · DetailErosion · (FadeMean - FloorModifier)        // TR_DeepFloorDetail
Floor    = saturate((FloorTop - h) / (DeepFill · FloorSoftness)), box-averaged across the step
Density  = max(Density, Floor)
```

The floor reads the same structure noise as the cloud wherever a step reaches its band, `[Base - DeepFill · (1 + FloorSoftness + DetailErosion), Base - DeepFill · (1 - FloorRelief - DetailErosion)]`. Without an active structure layer the floor is flat at the fill's completion. The detail layer carves it with its billow channel alone, faded to `FadeMean` like the cloud's, so the offset settles to zero with distance. The floor only adds density, so the cloud above, whose coverage and erosion the fill drives, is unchanged. `FloorRelief` 0 is no floor.

**Full deck and band.** Below `TR_DeckFull = BaseMin - max(DeepFill, SpanTop)` every column is at density 1: the lowest base less the fill, or less the top ramp of a clear column whose top sits on its base. This relies on the deep deck's top ramp keeping its full `SpanTop` (no ramp fit under `TR_DEEP_DECK`). It may lie below the surface; the core is deck like the rest. The march's inner cloud radius sits there, and `Field_SegmentIsCloud` counts segments below it as cloud.

**Terminus.** Rays stop at `TR_DeepTerminusRadius`, `TR_DEEP_OPAQUE_DEPTH` (8) optical depths below `TR_DeckFull` at the least extinction any material in use has (`TR_MaterialLeastExtinction`), past the march's opacity cutoff (`-ln ATMO_OPAQUE_TRANSMITTANCE` ≈ 6.9). The view ray counts against the view coefficient; the bake against the light coefficient (`LightExtinctionFraction` in it). At or below radius 0 there is no terminus. `Field_TerminusIsField` reports it as the field's own surface.

**Deep material.** A third material takes over by depth below the column's base (sample w): `TR_DeepMaterialWeight = smoothstep(0, MaterialDepth · DeepFill, depth)`. The share depends only on the sample's depth below its own column's base, not on how much deck lies above it.

**Darkening.** The sky's ambient decays into the deck (`Field_AmbientShare`):

```
tau   = Beta · Extinction(sample) · Thickness · TR_DeepBuried
Share = exp(-sqrt(3 · max(1 - Albedo, Darkening) · (1 - TR_DEEP_DIFFUSE_G)) · tau)
```

`TR_DeepBuried` is the fill integrated from the sample up to its base (exact on the smoothstep: `DeepFill · (T³ - T⁴/2)` plus any depth past the fill), `TR_DEEP_DIFFUSE_G` is 0.5. The channels a material absorbs most go dark first; `Darkening` floors the absorption so a white deck still darkens. The share applies to the air's ambient as well as the cloud's. Cloud above the base takes the full ambient, as on the slab.

**Other deep terms.** The shadow read saturates at `TR_DEEP_SHADOW_SATURATION` (16) instead of the slab's `ATMO_SHADOW_SATURATION` (5). Air continues below the surface at the surface's density (`Field_AirDensity`), and a light path's buried part is integrated at that density before the transmittance table takes over at the surface (`Field_AirBuried`).

The slab packs 0 into the deep pins (`DeepFill`, `FloorRelief`, `Darkening`) and never reads them.

## 5. Materials

Two materials over the slab, a third under the deep deck (`TRScatterParams`, built by `TR_BuildScatter`). A material is `float2(storm coordinate, deep share)`.

**Storm coordinate** (`TR_MaterialFromWeather`):

```
Index = max(saturate(cloud), saturate(storm))
T     = lerp(1 + StormBlend, -StormBlend, StormBalance)
M     = smoothstep(T - StormBlend, T + StormBlend, Index)
```

The raw column cloud grades from a system's edge to its core; the storm puts storm cells on top. `StormBalance` 0 is no storm material, 1 all of it. Nothing that shapes density reads the coordinate, so the balance changes colour without touching the deck's shape.

**Per material.**

| Term | Slab | Deep deck |
|---|---|---|
| Albedo (`Field_Albedo`) | `lerp(CloudScatter, StormScatter, M)` | then `lerp(·, Deep.Scatter, deep share)` |
| View extinction multiplier (`Field_Extinction`) | `lerp(CloudExtinction.rgb · a, StormExtinction.rgb · a, M)` | then lerp to `Deep.Extinction.rgb · a` |
| Light tint (`Field_SampleLightTint`) | the rgb tints alone, lerped the same way | deep share included |

`CloudExtinction.a` is forced to 1 (`ResolveCloudMaterial`); `CloudOpticalDepth` carries the fair-weather amount through `TR_CloudBeta`. `StormExtinction.a` and `Deep.Extinction.a` are opacity multiples of fair-weather cloud's. Albedo is clamped to [0, 1], extinction to non-negative.

**Coefficients.** The view ray's coefficient is `Beta` (grey) times the extinction multiplier per sample. The light ray's is `Beta · LightExtinctionFraction`. The bake stores one scalar depth: it weights density by the sample's amount (`TRShadow_Amount`) and folds in the largest tint channel in use (`TR_MaterialTintRange`); the reader colours the depth by the receiver's own tint. Outside cloud the receiver's tint comes from `Field_LightTint`, one weather-slice read without the deep share.

## 6. The field interface

### 6.1 The march's hooks

The march sees only these functions, the names `FieldParams` (`TRFieldParams`) and `FieldMaterialParams` (`TRScatterParams`), and the texture macros `FIELD_TEXTURE_DECL` / `FIELD_TEXTURE_ARGS`. `TerrestrialField.ush` defines `ATMO_FIELD_INCLUDED`, without which the march fails to compile.

| Function | Returns |
|---|---|
| `Field_Sample(textures, LocalPos, StepHalfHeight, F)` | `TR_Density`: (density, storm coordinate, pressure or buried deck, depth below base). Opaque to the march beyond x. |
| `Field_Albedo(Sample, S)` | Single-scattering albedo of the sample's material |
| `Field_Extinction(Sample, S)` | View-ray extinction multiplier, tint × amount |
| `Field_AmbientShare(Sample, Albedo, Atmo, F, S)` | Share of the sky's ambient reaching the sample: 1 on the slab, the deck's decay on the deep deck |
| `Field_SegmentIsCloud(Origin, Dir, Segment, Planet)` | Whether a planned segment is marched as cloud: the band flag; on the deep deck also anything below the inner radius; false for air only |
| `Field_Terminus(...)` | Where the view ray stops and whether it is opaque: the planet sphere over sky (slab), the deep terminus (deep) |
| `Field_TerminusIsField()` | Whether that terminus is the field's own (deep) |
| `Field_AirDensity(h, Atmo)` | Air densities: zero below the surface on the slab, held at the surface's below it on the deep deck |
| `Field_AirBuried(...)` | The buried part of a light path's air (deep), zero on the slab |
| `Field_ShadowSaturation()` | Optical depth where the shadow read stops growing: 5 slab, 16 deep |
| `Field_LightTint(Flow, LocalPos, F, S)` | Receiver tint from one weather read, outside cloud |
| `Field_SampleLightTint(Sample, S)` | Receiver tint from a sample already taken |
| `Field_NeutralTint(S)` | `CloudExtinction.rgb`, where nothing shadows |
| `Field_ShadowFrame(F)` | The map's cull radius, thickness and cascade extents (`TR_ShadowFrame`) |
| `Field_FoldConstant(Atmo, S)` | (light coefficient × largest tint, smallest / largest tint) |

The march sets two per-sample members on its copy of `F`: `DetailFade` and `PixelFootprint`. `TR_BuildField` leaves them at 1 and 0.

`TR_BuildAtmo` hands the march the cloud radii (section 2.5) and `TR_CloudBeta` as the cloud coefficient: `cloudScatBeta` is the view ray's whole coefficient and `cloudAbsBeta` is `LightExtinctionFraction` of it, not a scattering/absorption split.

### 6.2 The bake's side

The bake loop is shared (`AtmosphereShadowBakeLoop.ush`); `TerrestrialShadowMap.usf` supplies the field's hooks over the same functions the march calls, so the light sees the field the eye sees.

| Hook | Field side |
|---|---|
| `BakeField_Params` | `TR_BUILD_FIELD`, with `DetailFade` set per cascade |
| `BakeField_CullRadius`, `_CascadeExtent`, `_TopBound` | `TR_ShadowCullRadius`, `TR_ShadowCascadeExtent` (from `CascadeRadii`), `TR_TopMax` |
| `BakeField_TerminusRadius` | Planet radius (slab); `TR_DeepTerminusRadius` on the light coefficient (deep) |
| `BakeField_Entry` | `TR_SlabEntry`, below |
| `BakeField_HeightStep` | `SpanTop · Thickness / ATMO_BAKE_STEPS_PER_GRADIENT`: `SurfaceSoftness` sets the bake's radial step |
| `BakeField_LateralArc` | `Flow_TexelArc`, one flow texel |
| `BakeField_FoldConstant` | `TR_CloudBeta · LightExtinctionFraction / Thickness ·` largest tint |
| `BakeField_Sigma` | `TRShadow_Sigma`: `TR_Density` at `ATMO_BAKE_SUBSAMPLES` (4) subsamples, each weighted by its amount, averaged as extinction |

**Slab entry** (`TR_SlabEntry`). A cone trace to where the light ray first reaches a slab, on the slope bound `SlopePerTexel` (`CloudSlope` in the shader): cloud depths per flow texel, since the surfaces are built on the flow. A probe's advance is its gap to the nearer surface over the closing rate (the ray's approach plus the slope over the ground covered), times `TR_CONE_RELAXATION` (0.9), for at most `TR_SLAB_ENTRY_PROBES` (24). A ray climbing above `TopMax` skips to the end; one below `BaseMin` and descending resumes at its perigee. On the deep deck a probe enters anywhere under `max(Top, Base)`. An under-declared slope steps over cloud and leaves shadow holes.

### 6.3 The pin contract

The field travels as float4 pins, packed once in `PackCloudField` (one packer for the march, the bake and the coverage pass) and unpacked once in `TR_BuildField`. These must agree by name and change in one commit:

1. `TR_BuildField`'s signature and its packing comment (`TerrestrialDeck.ush`).
2. `TR_BUILD_FIELD()`, which expands to that signature from variables of the same names. A missing one fails as an undeclared identifier naming it.
3. The uniforms in `AtmosphereMarchPass.usf` and `TerrestrialShadowMap.usf`.
4. `FTerrestrialFieldParameters` (`TerrestrialShadowMap.h`), included in `FAtmosphereMarchParameters`, `FTerrestrialShadowParameters` and `FTerrestrialCoverageParameters`.
5. `PackCloudField`.
6. `MakeShadowFieldKey` (`PlanetAtmosphereActor.cpp`), which hashes every pin except the clock slots of `CloudMotion`. A pin missing from it keeps a stale shadow history.

`PlanetRadius`, `HeightScale` and the `CoverageThreshold` texture arrive beside the struct. `DeepMaterial` is shared with `TR_BUILD_SCATTER`.

The coverage pass solves the threshold, so it builds the field with `TR_BUILD_FIELD_AT(0.0f)` in place of the texture load and binds no `CoverageThreshold`; every other consumer uses `TR_BUILD_FIELD()`.

| Pin | x | y | z | w |
|---|---|---|---|---|
| `CloudProfile` | `CloudBase` | `CloudThickness` | `SurfaceSoftness` | `CeilingFalloff` |
| `CloudCurves` | `TopCurve` | `BottomCurve` | `SlopePerTexel` | `WarpStretch` |
| `CloudCoverage` | `CloudCover` | `StormPriority` | `CoverageSoftness` | `DeepFill` (0 slab) |
| `CloudType` | `TypeBias` | `CloudFull` | `TypeTropical` | `CoverageDepthRamp` |
| `CloudLid` | `PressureScale` | `EyeOpenPower` | `CeilingPressure` | `StratusDepth` |
| `CloudLift` | `BaseTropical` | `BasePressure` | `AltitudeGain` | `AltitudeLift` |
| `CloudMotion` | drift angle | noise phase | `WarpShift` | spin angle |
| `NoiseLevels` | structure `MipBias` | structure volume bound | detail `MipBias` | detail volume bound |
| `StructureSampling` | `Scale` | `Aspect` | `Erosion` | `TypeStorm` |
| `StructureWarp` | `FlowInherit` | `Subsidence` | `FloorRelief` (0 slab) | `Breakup` |
| `DetailSampling` | `Scale` | `Aspect` | `Erosion` | `FadeMean` |
| `DetailWarp` | `FlowInherit` | `BillowHeight` | `FadeNear` | `FadeFar - FadeNear` |
| `CloudGenus<Name>` | R weight | G weight | B weight | A weight |
| `ShadowCascades` | `CascadeRadii.X` | `CascadeRadii.Y` | `CloudLayer` | `CoverageFray` |
| `CloudResponse` | `Darkening` (0 slab) | `TypeCurve` | `StormBalance` | `StormBlend` |
| `DeepMaterial` | `MaterialDepth · DeepFill` | `FloorSoftness` | - | - |
| `CloudExtinction`, `StormExtinction`, `DeepExtinction` | tint r | g | b | amount |
| `CloudOpticalDepth` | scalar | | | |

## 7. Cost

**Per field sample.** One `Flow_SampleColumn` (four slices of four bilinear taps on one face, up to three faces near an edge), then up to two structure fetches and two detail fetches (one per noise phase). The bake pays this per subsample, four per step.

**What widens the marched band** (`TR_FieldReach`). `CloudThickness` scales every term; `|BasePressure|` widens both ways; positive `BaseTropical` and `AltitudeLift` raise `TopMax`, negative ones lower `BaseMin`; `|CeilingPressure|` deepens the reach above the highest base. On the deep deck `DeepFill` and `SpanTop` (`SurfaceSoftness · CloudThickness`) lower the inner radius further. The march's base cloud step is the band's depth over `CloudSteps`, so a wider band is coarser at the same count or costlier at the same resolution, and it fine-steps more of every ray.

**What skips fetches.**

| Condition | Skips |
|---|---|
| Sample outside its column's slab (`TR_SlabMissed`) | All noise; the column read is still paid |
| No structure volume, or structure `Erosion` 0 | Structure fetches (`StructureActive`) |
| No detail volume, or detail `Erosion` 0 | Detail fetches (`DetailActive`) |
| Density zero after the threshold, at the step's peak | Detail fetches |
| Beyond `FadeFar`, or the mip past the mean cutoff | Detail fetches (reads `FadeMean`) |
| Bake cascades coarser than `TR_SHADOW_DETAIL_LEVEL` | Detail fetches |
| Deep deck below `Fill = 1`, outside the floor's band | Structure and detail fetches |
| `FloorRelief` 0 | The floor's fetches |

Higher detail `Scale`, `Aspect` or `MipBias` reaches the mean cutoff nearer the camera. A low `SurfaceSoftness` shortens the bake's radial step and raises its step count. A high `SlopePerTexel` shortens the entry search's advances.

**Fixed costs.** The coverage pass is one group of 4096 column reads per bake request. `Field_LightTint` adds a weather read only for shadowed samples outside cloud.

## 8. Pitfalls

| Symptom | Cause | Where |
|---|---|---|
| Shallow columns vanish while deep ones barely change | Ramps cut against the depth instead of scaled together: a shallow column peaks near 0.15 and coverage and erosion remove it | `TR_SlabRamps` |
| Lighting rings where depth sweeps through a value (eye walls) | A hard min or max at a slope break: the ramp fit, the depth stretch, the ceiling cap. Each is a smooth p-norm or knee. | `TR_SlabRamps`, `TR_DensityInColumn`, `TR_ColumnFromInputs` |
| Thin cloud flickers; grazing views see through cloud opaque from inside | Thresholds applied to a step-averaged or point-sampled profile | `TR_DensityInColumn` |
| Coverage flattens the sim's contrast, the noise decides the gaps | An additive cover instead of a quantile threshold | `TR_ColumnFromInputs`, `MainCoverageCS` |
| `CloudCover` loses its hold | `CoverageSoftness` above 0.5: the threshold never falls below it | `MainCoverageCS` |
| A visible edge where the cloud amount saturates | A linear amount ramp | `TR_Priority` |
| Bands of towers sliding through the flow; every stormy column a full tower | Type from local `W`; storm as a floor on type instead of a term | `TR_ColumnFromInputs` |
| Storm material saturating in dense deck, or painting every tropical cloud | A storm index from the cloud amount, or from type | `TR_MaterialFromWeather` |
| Erosion erases thin cloud and leaves thick cloud untouched | A threshold driven by coverage alone; thick cloud stays opaque at a fraction of its density | `TR_DensityInColumn` |
| Holes that no coverage fills | Structure `Erosion` above 1 extrapolates the shaping factor negative | `TR_DensityInColumn` |
| Noise swims as columns deepen, shears into rings across an eye wall | Warp anchored at the ground instead of the column base | `TR_DensityInColumn` |
| Towers read solid at any erosion | No depth stretch: a tower stacks several independent features | `TR_DensityInColumn` |
| Distant cloud thins | The mip averages before erosion and coverage cut the noise; lower `MipBias` | `TR_NoiseLod` |
| Cloud thickens or thins across the detail fade band | `FadeMean` off from the volume's real mean | `FCloudDetailLayerParams` |
| The eye's floor an opaque sheet, or cut away abruptly | Density not scaled by thinning, or scaled before erosion | `TR_DensityInColumn` |
| Deck cloud changes when the floor is tuned | A floor that moves the fill instead of adding density | `TR_DeepFloor` |
| Rings concentric with the disc, a pinwheel at the nadir | Columns keyframed along each ray and interpolated | `TR_Density` |
| Tops of features sliced off | A term in `TR_ColumnFromInputs` missing from `TR_FieldReach` | `TR_FieldReach`, `SolveFieldBounds` |
| Shadow missing under steep walls (eyewalls, crisp edges, `CoverageDepthRamp` below about 0.1) | `SlopePerTexel` under-declared | `TR_SlabEntry` |
| Shadows smooth, plausible and misplaced | A second derivation of any field quantity on one side; a map read against another radius than it was written at | `TerrestrialShadowMap.usf`, `TR_ShadowCullRadius` |
| Shadow and view colour differ mid-blend | The light path lerps amount and tint separately (one scalar depth); the view ray lerps their product | `TRShadow_Amount` |
| Deck blacked out at a fixed radius | Deep terminus too shallow | `TR_DEEP_OPAQUE_DEPTH` |
| In-scatter vanishes in a ring in the deep deck | A long step through saturated deck taking one shadowed light read instead of a terminus | `Field_Terminus` |
| A hard line at the surface sphere under the deep deck | Air cut off at the surface | `Field_AirDensity` |
| Noise pops at resets | A noise phase that is not the sim's own clock | `NoiseClock` |
| A stale shadow after a field change | A pin missing from the shadow field key | `MakeShadowFieldKey` |
| A compile error far from any texture or type | `FIELD_TEXTURE_DECL` and `FIELD_TEXTURE_ARGS` out of step; `FieldParams` or `FieldMaterialParams` shadowing a shared type | `TerrestrialField.ush` |
| Tall columns thinned everywhere | `CloudBase + CloudThickness` above `1 - CeilingFalloff` (`SolvedTopMax` shows it) | `TR_CeilingFade` |
| The bake hits its step cap | `SurfaceSoftness` near zero | `BakeField_HeightStep` |
