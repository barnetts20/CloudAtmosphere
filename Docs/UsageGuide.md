# CloudAtmosphere usage guide

Every setting you can change on the atmosphere actor, the sim config and the project settings, with what it does to the picture. The settings use their code names, which the details panel spaces out (`CloudCover` shows as "Cloud Cover"). They are also the names tune files use.

Each entry gives:

- **Range:** the hard limits, and the slider's range where it is narrower. The slider is a suggestion, and you can type past it up to the hard limits.
- **Defaults and your values:** the class default, then your current values (from `Design/Tunes` and the `*SimDefault` assets). A value in **bold** is one you've changed from the default.

## How it fits together

1. **The sim** (`UFlowSimConfig`) runs a coarse weather model over the whole planet. It produces winds, cloud, storm intensity, pressure, rising and sinking air, and hurricane eyes. It knows nothing about how clouds look.
2. **The cloud field** (the actor's Model groups Shape through Deep) turns that weather into 3D cloud:
   - Coverage picks how much of the weather becomes cloud.
   - Type and Lift give each column its height and kind.
   - The noise layers carve the shapes, and the sim's winds carry them.
3. **Lighting** (Material, Phase, Multiple Scattering, Ambient, Air) colours the cloud and the sky.
4. **Pipeline and Quality** settings decide the cost.

On the actor, the look settings and the quality settings are separate, so a tune can change the look and leave each machine's cost alone. The sim config's own Quality group (`GridResolution`, `StepSize`, `LayerCount`) is part of the tune, because the weather depends on it.

## Units

- **Heights in the atmosphere** are fractions of its thickness: 0 is the ground and 1 the top. The thickness is `Planet.HeightScale` × the planet radius. Lift and warp offsets are multiples of `Shape.CloudThickness`.
- **Distances from the camera** (`DetailLayer.FadeNear`/`FadeFar`, `SurfaceShadow.CascadeRadii`) are in planet radii.
- **Noise scales** are repeats per radian of the planet's surface: higher is smaller.
- **Colours** are linear, per channel.
  - **Albedo** (Scatter) runs 0 to 1: how much light survives each scattering.
  - **Optical depth** says how much light gets through: 1 lets 37% through, 2 lets 14% and 3 lets 5%.
- **Sim winds** are fractions of the **speed root**, the speed of the fastest flow (`SpeedRoot` × the wave speed set by `DeformationRadius` and `PlanetaryVorticity`).
- **Sim rates and lifetimes** are per **turnover**: the time a weather system takes to turn over once, 1.41 / (`SpeedRoot` × `PlanetaryVorticity`) in sim time. Real seconds per turnover are that divided by `SimSpeed`.
  - Your terrestrial config: one turnover is 0.393 sim time, about 79 s of real time.
  - Your gas giant config: one turnover is 0.147 sim time, about 29 s.
- **Sim lengths** (storm sizes, the stirring) are in **deformation radii**, the size weather systems settle at (`DeformationRadius`, in planet radii).

Because the other settings are in these relative units, they keep their balance when the three base settings change. `PlanetaryVorticity` mostly changes the tempo. `DeformationRadius` changes the size of systems against the planet and the grid. `SpeedRoot` changes how close the winds run to the speed cap.

## Common changes

| To change | Start with | Where |
|---|---|---|
| How cloudy the planet is | `CloudCover` | Actor · Coverage |
| The size of individual clouds | `StructureLayer.Scale` | Actor |
| The size of weather systems | `DeformationRadius` | Sim · Winds |
| How dense and opaque cloud is | `CloudOpticalDepth` | Actor · Material |
| How broken up cloud is | `StructureLayer.Erosion`, `Breakup`, `CoverageFray` | Actor |
| Towering vs flat layered cloud | `TypeBias`, `StratusDepth`, `CloudThickness` | Actor · Type, Shape |
| How much cloud is storm cloud, and its colour | `StormBalance`, `StormScatter` | Actor · Material |
| Softer, brighter cloud interiors | `ScatteringGlow`, `SunlightPenetration`, `CloudAmbient` | Actor |
| Sky colour and thickness | `RayleighDepth` | Actor · Air |
| Haze | `MieDepth` | Actor · Air |
| How fast the weather plays | `SimSpeed` | Sim · Time |
| How lively the weather is | `EddySpeed`, `ShearSpeed` | Sim |
| How often hurricanes form | `StormCellSpawnRate`, `GenesisMoisture`, `MaxStormCells` | Sim · Hurricanes |
| Hurricane size and strength | `StormCellRadius`, `StormCellWind` | Sim · Hurricanes |
| Hurricane eyes | `StormCellEyeDepth`, `StormCellEyeDraft`, `EyeOpenPower` | Sim, Actor · Coverage |
| A gas giant's bands | `BandCount`, `JetIrregularity`, `JetSpeed` | Sim · Winds |
| A gas giant's depths | `Deep.Scatter`, `MaterialDepth`, `DeepFill` | Actor · Deep |
| Cloud shadows on the ground | `DirectFraction` | Actor · Surface Shadow |
| Shadow missing under steep cloud walls | `SlopePerTexel` | Actor · Shape |
| Frame cost | `CellSize`, `CloudSteps`, `ShadowResolution`, `GridResolution` | Actor · Pipeline, Sim · Quality |

## The atmosphere actor

`APlanetAtmosphereActor`: its settings are under **CloudAtmosphere** in the details panel, right after the transform.

### Atmosphere

The star and which model the actor draws. The light's direction is the actor's rotation: rotate the actor to move the sun.

**`PlanetType`** · default GasGiant · yours GasGiant

Which model draws:

- **Terrestrial:** a cloud layer over a surface.
- **Gas Giant:** the same cloud layer over a deep deck that fills down to the core.
- **Air Only:** the terrestrial air with no clouds, for moons and thin-air worlds. Air-only planets never take the sim.

Switching rebakes the shadow map and restarts the temporal history on the next frame.

**`LightColor`** · default (1, 0.95, 0.9) · yours (1, 0.95, 0.9)

The star's colour. Only the hue is used: the brightest channel counts as 1, and `LightIntensity` sets the brightness. The actor's directional light is driven by the same colour × intensity, so the clouds and the scene always agree.

**`LightIntensity`** · ≥ 0 · slider to 20 · default 4 · yours 4

The star's brightness, on its brightest channel. All ambient light is a ratio of this, so it follows. Above about 1000 (at `MieG` 0.95), the haze's glow around the sun starts to clip.

**`bIsPlanetOwned`** · default off · yours off

**Read-only.** True when a planet actor spawned and drives this atmosphere. Location and scale are then locked to the planet. Rotation still sets the sun's direction.

### Model

Each actor holds two sets of look settings: `Terrestrial`, used by the Terrestrial and Air Only models, and `GasGiant`. The panel shows only the set the current `PlanetType` uses. Air Only hides every cloud group and keeps Planet, Air, Ambient and Surface Shadow. Deep is shown only on the gas giant. These groups set the look. The few that also change cost say so in their entries.

Each setting lists its range, then its class default and your current value on each model. A value in bold differs from the default. Where the two models' class defaults differ, both are given (terrestrial / gas giant).

#### Planet

The atmosphere's height and how the weather pattern turns with the planet.

**`HeightScale`** · > 0 · slider from 0.005 · default 0.2 · terrestrial **0.1** · gas giant **0.1**

How tall the atmosphere is, as a fraction of the planet radius above the surface. This is the master scale: every height in the cloud groups is a fraction of it. Below about 0.005 the shadow map loses precision near the day–night line.

**`SpinRatio`** · slider from 0 · default 1 · terrestrial **0.3333** · gas giant **0.01667**

How fast the whole weather pattern turns around the planet, as a share of the rotation rate the sim assumes for its Coriolis effect (`PlanetaryVorticity`/2). At 1, the clouds turn as fast as that planet would spin. At 0, the pattern holds still and only moves with the sim's own winds. It turns the pattern only and doesn't change the weather itself.

#### Air

The clear air: sky colour, haze and an ozone-like absorbing layer. Each *Depth* colour is the optical depth of a vertical column from the ground to the top of the atmosphere, per channel. Its hue is the colour the air scatters, and its size is how thick the air looks. The scale heights only spread that column with altitude: small values pack the air near the ground.

**`RayleighDepth`** · default (0.06236, 0.1903, 0.2996) · terrestrial **(0.1465, 0.465, 0.7946)** · gas giant **(0.6491, 2.704, 4)**

The molecular scattering of clear air: the sky's colour (its hue) and how thick the sky looks (its size). Earth-like air scatters blue most. Raise every channel together for a thicker, brighter sky with a redder sunset.

**`RayleighScaleHeight`** · > 0 · default 0.15 · terrestrial **0.2** · gas giant 0.15

How quickly the molecular air thins with altitude, as a fraction of the atmosphere's thickness. Too small leaves no blue air above the cloud tops, and the limb reads as a hard edge.

**`MieDepth`** · default (0.1, 0.08931, 0.07559) · terrestrial (0.1, 0.08931, 0.07559) · gas giant (0.1, 0.08931, 0.07559)

Haze (aerosols, dust): column optical depth per channel. It brightens and whitens the sky around the sun and softens the horizon.

**`MieScaleHeight`** · > 0 · default 0.1 · terrestrial 0.1 · gas giant 0.1

How quickly haze thins with altitude, as a fraction of the atmosphere's thickness. Clouds that reach most of the way up the atmosphere need haze above them, or they look pasted on.

**`MieG`** · -0.99 to 0.99 · default 0.95 · terrestrial 0.95 · gas giant **0.9**

How strongly haze scatters forward: 0 is even in all directions, and near 1 is a tight bright glow around the sun. Negative values scatter backward.

**`MieLobeDecay`** · ≥ 0 · default 2 · terrestrial 2 · gas giant 2

How quickly the haze's glow around the sun fades behind cloud, per unit of cloud optical depth. Light that has crossed thick cloud has lost its direction. Too low a value draws the sun's glow on top of an overcast planet.

**`AbsorptionDepth`** · default (0.04203, 0.03394, 0.03681) · terrestrial **(0.05232, 0.04225, 0.04582)** · gas giant **(0.05232, 0.04225, 0.04582)**

An absorbing layer at altitude, like Earth's ozone: column optical depth per channel. It absorbs rather than scatters, so it tints transmitted light. Ozone-like values absorb red and green, which keeps twilight skies blue.

**`AbsorptionAltitude`** · default 0.15 · terrestrial 0.15 · gas giant 0.15

The height the absorbing layer centres on, as a fraction of the atmosphere's thickness.

**`AbsorptionFalloff`** · > 0 · default 0.1 · terrestrial 0.1 · gas giant 0.1

The absorbing layer's half-width, as a fraction of the atmosphere's thickness.

#### Ambient

Light that has bounced around the sky, for the air and the clouds separately. Both are ratios of the star's light, so they follow `LightIntensity`. Both fade out across the day–night line (the terminator). Each has a floor, the share that stays on the night side.

**`AirAmbient`** · default (0.0006667, 0.0009123, 0.001407) · terrestrial **(0.004, 0.005474, 0.008444)** · gas giant **(0.004, 0.005474, 0.008444)**

The air's ambient light, as a ratio of the star's light per channel. Raise it over a bright surface. Keep it near black over an opaque cloud deck, which bounces little back up.

**`AirAmbientFloor`** · ≥ 0 · default 0.02 · terrestrial **0.05** · gas giant **0.05**

The share of the air's ambient that stays on the night side. At 0 the ambient is swamped by direct light by day and gone by night. The floor gives the night side some sky glow.

**`CloudAmbient`** · default (0.04, 0.044, 0.052) · terrestrial (0.04, 0.044, 0.052) · gas giant (0.04, 0.044, 0.052)

The clouds' ambient light, as a ratio of the star's light per channel. It lights cloud undersides and shadowed sides. Raise it for softer, brighter shade.

**`CloudAmbientFloor`** · ≥ 0 · default 0.04 · terrestrial **0.015** · gas giant **0.01**

The share of the clouds' ambient that stays on the night side. Above 0, the night side's clouds stay faintly visible.

**`AmbientTerminator`** · > 0 · slider to 1 · default 0.15 · terrestrial 0.15 · gas giant 0.15

How wide the band is over which ambient fades across the day–night line, in cosine of sun elevation (0.15 is about 9° either side). Too wide lets ambient wash the night side, which looks like the star shining through the planet.

#### Shape

Where the cloud layer sits and the vertical profile of a column of cloud. Heights are fractions of the atmosphere's thickness (`HeightScale` × planet radius), 0 at the ground and 1 at the top.

**`CloudBase`** · slider 0 to 1 · default 0.15 · terrestrial **0.01** · gas giant **0.175**

The cloud base of an unlifted column, as a fraction of the atmosphere's thickness. Lift moves individual columns up or down from here.

**`CloudThickness`** · > 0 · slider to 1 · default 0.5 · terrestrial **0.8219** · gas giant **0.65**

The depth of a fully towering column, as a fraction of the atmosphere's thickness. It is also the unit of every lift and warp offset, and it bounds the finely sampled band. Keep `CloudBase` + `CloudThickness` below 1 − `CeilingFalloff`, or the ceiling thins every tall column (`SolvedTopMax` shows it).

**`SurfaceSoftness`** · > 0, ≤ 0.5 · slider from 0.05 · default 0.35 · terrestrial **0.33** · gas giant **0.33**

The share of the cloud depth that each end of a column ramps in over. Low values give hard tops and bases. 0.5 leaves no solid core. Very low values also make the shadow bake work much harder.

**`TopCurve`** · > 0 · default 1 · terrestrial **2** · gas giant 1

The shape of the top ramp: higher gives a lower, flatter, harder-edged top, as `BottomCurve` does for bases. Below 0.5 the top gains a visible crease.

**`BottomCurve`** · > 0 · default 2 · terrestrial 2 · gas giant **1**

The shape of the bottom ramp: above 1 flattens cloud bases, as real cloud bases are flat.

**`CeilingFalloff`** · > 0 · slider to 0.5 · default 0.2 · terrestrial 0.2 · gas giant 0.2

The band under the top of the atmosphere over which cloud fades out, so the tallest towers cap softly instead of being sliced off.

**`SlopePerTexel`** · > 0 · slider 0.5 to 8 · default 1.5 · terrestrial 1.5 · gas giant 1.5

How steep a cloud wall the shadow bake allows for. If shadow goes missing under steep cloud walls (a small hurricane's eyewall, or a crisp `CoverageSoftness`), raise this first. Higher values cost bake time.

**`SolvedTopMax`**

**Read-only.** The highest a column's top can reach with the current settings. Above 1 − `CeilingFalloff`, the ceiling is capping the tallest columns.

**`SolvedBaseMin`**

**Read-only.** The lowest a column's base can fall. On the gas giant the finely sampled band runs further down, by the deep fill.

#### Coverage

How much of the planet is cloudy, and in what order the sim's weather fills it in. Every column of the sim's grid gets a rank: hurricanes highest, then storms, then plain cloud from the thick cores of systems outward. Each frame a threshold is solved so that `CloudCover` of the planet lies above it. Columns with no sim cloud are never covered, so the share can't exceed what the weather holds. The noise layers then break up whatever is covered.

**`CloudLayer`** · -1 to 7 · default -1 · terrestrial -1 · gas giant -1

Which sim layer the clouds are drawn from. -1 is the bottom layer, which includes the cloud of every layer above it, so it shows the whole sky. 0 is the top layer alone. Vertical motion, pressure and the noise's motion come from the same layer, so the clouds and their breakup move together. The sim's ColumnCloud debug view at the same layer shows exactly what is read.

**`CloudCover`** · 0 to 1 · default 0.5 · terrestrial 0.5 · gas giant **1**

**The main cloud amount:** the share of the planet that is cloudy, from 0 (clear) to 1 (every column the sim has cloud in). Columns fill in by rank, so the most important weather is always kept. The same value gives the same share whatever the sim is doing, up to the share the sim has cloud in. It only works fully with `CoverageSoftness` at 0.5 or below.

**`CloudFull`** · > 0 · slider 0.001 to 1 · default 0.05 · terrestrial **0.5** · gas giant **0.5**

How much sim cloud counts as a full column. Small values make cloud all-or-nothing, so storms alone set the order of fill. Larger values grade cloud from the cores of systems to their edges, so falling coverage thins systems from the outside in.

**`StormPriority`** · ≥ 0 · default 1 · terrestrial **0.75** · gas giant **1.5**

How far storms jump ahead of plain cloud in the fill order. At 0, the order is by cloud alone. Higher values keep storms when coverage is low. Storms raise only cloud that already exists.

**`CoverageSoftness`** · > 0 · slider to 1 · default 0.2 · terrestrial **0.75** · gas giant **0.25**

How gradually a system's edge goes from clear to covered. Lower is crisper. The threshold never drops below this value, so **above 0.5 no column ever reaches full coverage and `CloudCover` loses its hold**: at 0.75, no column passes about 74%.

**`CoverageFray`** · 0 to 1 · default 0.5 · terrestrial **0.75** · gas giant **1**

How system edges thin as coverage falls. At 1, edges fray into scattered puffs that the noise picks out. At 0, they fade evenly. Inside a fully covered system, the structure layer's erosion alone decides the gaps.

**`CoverageDepthRamp`** · > 0 · slider 0.1 to 0.6 · default 0.25 · terrestrial **0.6** · gas giant 0.25

How quickly a column grows to full height as its coverage rises. Wide values thin system edges into low wisps. Narrow values stand scattered cloud up at full height. Below about 0.1, edges become cliffs that shadows miss.

**`EyeOpenPower`** · ≥ 0 · slider to 4 · default 1 · terrestrial 1 · gas giant 1

How the sim's hurricane eye carves the cloud. Above 1, more eyewall is kept and the eye's floor clears sooner. Below 1, the eye is hazier.

#### Type

Cloud type, from 0 (flat layered cloud) to 1 (towering cloud). A column's type is `TypeBias` + `TypeTropical` × how tropical it is + `TypeStorm` × storm^`TypeCurve`. Type sets how deep the column is and which noise (genus) draws it. It does not set colour: the material follows the storm index (Material).

**`TypeBias`** · default 0.8 · terrestrial 0.8 · gas giant **0.5**

The baseline type of every column before the weather changes it: 0 flat layered cloud, 1 towering cloud.

**`TypeTropical`** · default 0.3 · terrestrial 0.3 · gas giant **0.8**

How much the tropics push cloud toward towering.

**`TypeStorm`** · ≥ 0 · default 0.5 · terrestrial 0.5 · gas giant 0.5

How much storms push cloud toward towering. Storms build gradually into the cloud around them rather than turning every stormy column into a full tower. This sets shape only. Storm colour is `StormBalance`.

**`TypeCurve`** · ≥ 0.1 · slider to 8 · default 2 · terrestrial 2 · gas giant **1**

How sharply the storm push builds: 1 is linear, and higher values reserve full towers for the stormiest columns.

**`StratusDepth`** · 0 to 1 · default 0.25 · terrestrial 0.25 · gas giant 0.25

The depth of flat layered cloud, as a fraction of a full tower's depth.

##### Genus

The noise each cloud genus draws with, as weights on the structure noise volume's four channels:

- **R:** smooth Perlin noise, which draws sheets.
- **G:** billow noise, which draws puffy cauliflower shapes.
- **B:** cellular noise, which draws broken cells.
- **A:** fibrous ridged noise, which draws streaks.

Each column blends the four genera by its type and altitude. Layered cloud turns cellular where the air sinks. The weights don't need to sum to 1, because the blend keeps the noise's contrast whatever their total.

**`Stratus`** · default (1, 0, 0, 0) · terrestrial (1, 0, 0, 0) · gas giant (1, 0, 0, 0)

Noise weights for low layered cloud in still or rising air: sheets.

**`Stratocumulus`** · default (0.3, 0, 0.7, 0) · terrestrial (0.3, 0, 0.7, 0) · gas giant (0.3, 0, 0.7, 0)

Noise weights for low layered cloud in sinking air: broken cells.

**`Cumulus`** · default (0.2, 0.8, 0, 0) · terrestrial (0.2, 0.8, 0, 0) · gas giant (0.2, 0.8, 0, 0)

Noise weights for towering cloud at any altitude: billows.

**`Cirrus`** · default (0.2, 0, 0, 0.8) · terrestrial (0.2, 0, 0, 0.8) · gas giant (0.2, 0, 0, 0.8)

Noise weights for high layered cloud: streaks.

**`Subsidence`** · ≥ 0 · default 3 · terrestrial 3 · gas giant 3

How quickly sinking air breaks flat stratus into cellular stratocumulus.

#### Lift

How the sim's pressure, the tropics and the altitude the cloud formed at move a column's top and base. Offsets are multiples of `CloudThickness`. Lows (negative pressure) give taller cloud with lower bases, and highs flatten it.

**`PressureScale`** · > 0 · default 0.5 · terrestrial **1** · gas giant 0.5

The sim pressure that counts as a full low or high. Raise it until cloud height varies smoothly across a system rather than jumping at its edge.

**`CeilingPressure`** · -0.99 to 0.99 · default 0.5 · terrestrial 0.5 · gas giant **0.3**

How much pressure changes cloud depth, as a share of `CloudThickness`. Positive values give lows taller cloud and flatten highs.

**`BaseTropical`** · default 0.1 · terrestrial **0.15** · gas giant 0.1

How far the tropics raise the cloud base, in multiples of `CloudThickness`.

**`BasePressure`** · default -0.1 · terrestrial **-0.35** · gas giant **-0.35**

How far pressure moves the cloud base, in multiples of `CloudThickness`. Negative values lower the base under lows.

**`AltitudeGain`** · ≥ 0 · default 3 · terrestrial 3 · gas giant 3

Scales the sim's formation altitude (which of its layers the cloud condensed in, and how fast it was rising) to a height in the cloud layer. Higher values lift more cloud to full altitude.

**`AltitudeLift`** · default 0.6 · terrestrial 0.6 · gas giant 0.6

How far formation altitude lifts flat cloud's base, in multiples of `CloudThickness`. Towers stay rooted, and layered cloud floats up to where it formed. It widens the finely sampled band by the same amount.

#### Warp

How rising air bends the cloud noise, drawing towers taller and pressing sinking air into sheets. It moves the noise only, not the cloud layer's bounds.

**`WarpStretch`** · slider from -0.9 · default 0.5 · terrestrial **0.25** · gas giant 0.5

How much rising air stretches the noise vertically: taller towers, and flatter sheets in sinking air.

**`WarpShift`** · default 0.2 · terrestrial **0.25** · gas giant 0.2

How far rising air lifts the noise, in multiples of `CloudThickness`.

#### Structure Layer

The large-scale cloud shapes that coverage cuts out of the weather. The sim carries the noise along with the wind, so shapes drift and shear with the weather. Scales are in noise repeats per radian at the surface: higher means smaller clouds. Features shrink with height (by `Aspect`), so high cloud is finer than low cloud.

**`Volume`** · default `CloudNoise_4_128` · terrestrial **`CloudNoise_4_128_eq`** · gas giant `CloudNoise_4_128`

The 3D cloud noise texture, with four noise types in its channels (see Genus). It must wrap on all three axes and have mips. With none set, the layer is skipped.

**`Scale`** · slider from 0.01 · default 6 / 1.5 · terrestrial **2** · gas giant 1.5

Horizontal frequency at the surface: **higher means smaller clouds**.

**`Aspect`** · ≥ 0 · slider from 0.01 · default 2 / 16 · terrestrial **12** · gas giant 16

Vertical frequency relative to horizontal: higher gives flatter, more layered features. It also sets how fast features shrink with height.

**`Erosion`** · ≥ 0 · slider to 2 · default 0.85 · terrestrial **0.6** · gas giant 0.85

How hard the noise breaks the cloud up. At 0 the cloud is smooth sheets. Together with `Breakup`, it cuts gaps even in fully covered systems once it passes 1/(1 + `Breakup`), about 0.67 at `Breakup` 0.5. Above 1, holes open whatever `Breakup` is.

**`Breakup`** · 0 to 1 · slider 0.2 to 0.8 · default 0.5 · terrestrial 0.5 · gas giant 0.5

The share of a thick system that full erosion cuts into gaps. Low values keep thick decks solid. High values break them into separate masses with clear sky between.

**`FlowInherit`** · slider 0 to 1 · default 0.9 · terrestrial **1** · gas giant 0.9

How much the noise rides the sim's winds: 1 moves fully with the weather, and 0 ignores the winds, keeping only the sim's `NoiseDriftSpeed` drift. Near 1, fast-turning regions shear the noise into streaks.

**`MipBias`** · slider -2 to 4 · default 1 · terrestrial 1 · gas giant 1

Texture detail level: lower is sharper but shimmers more in motion.

#### Detail Layer

Fine erosion of cloud edges: wispy toward the base, billowy toward the top, fibrous in high cirrus. The grain fades with distance to its average, so distant cloud keeps the same overall erosion without shimmering. Use a different volume from the structure layer so the two sets of features don't line up.

**`Volume`** · default `CloudNoise_8_128` · terrestrial **`CloudNoise_8_128_eq`** · gas giant `CloudNoise_8_128`

The 3D detail noise texture. Use a different asset from the structure layer so their features don't line up. With none set, the layer is skipped.

**`Scale`** · slider from 0.01 · default 30 / 24 · terrestrial **24** · gas giant 24

Horizontal frequency at the surface: higher is finer grain.

**`Aspect`** · ≥ 0 · slider from 0.01 · default 1 / 8 · terrestrial **8** · gas giant 8

Vertical frequency relative to horizontal, as for the structure layer.

**`Erosion`** · 0 to 1 · default 0.6 · terrestrial **0.3** · gas giant 0.6

How hard cloud edges are eaten away.

**`BillowHeight`** · > 0 · slider 0.05 to 0.6 · default 0.25 · terrestrial 0.25 · gas giant 0.25

The share of a column's height over which erosion turns from wispy (base) to billowy (top). Higher values give tall ragged bases. Lower values put cauliflower tops over almost the whole cloud.

**`FlowInherit`** · slider 0 to 1 · default 0.4 · terrestrial **0.33** · gas giant 0.4

How much the detail rides the sim's winds. Keep it lower than the structure layer's, so sheared regions turn strandy but keep some rounded detail.

**`MipBias`** · slider -2 to 4 · default 0 · terrestrial 0 · gas giant 0

Texture detail level for the detail noise, as for the structure layer.

**`FadeNear`** · ≥ 0 · default 0 · terrestrial 0 · gas giant 0

Distance from the camera, in planet radii, where the grain starts fading to its average.

**`FadeFar`** · ≥ 0 · default 0.3 · terrestrial 0.3 · gas giant 0.3

Distance, in planet radii, where the grain has fully faded. The detail texture isn't read beyond it. Only the near shadow cascade carries the grain, so keep `SurfaceShadow.CascadeRadii.Y` at or beyond this.

**`FadeMean`** · 0 to 1 · default 0.5 · terrestrial 0.5 · gas giant 0.5

The noise's average, which faded cloud settles to. Use 0.5 for an equalized volume. If cloud visibly thickens or thins across the fade distance, this doesn't match the volume.

#### Deep

**Gas giant only.** The deep cloud deck beneath the visible cloud layer: below each column's base the cloud fills in to full density and runs down to the core, so the planet never shows a floor. The deep deck has its own material, which takes over from the cloud and storm material a set depth below the base, and an optional lumpy floor of full-density mounds.

**`DeepFill`** · > 0 · slider 0.001 to 1 · default 0.1 · gas giant **0.3**

How far below a column's base the deck reaches full density, as a fraction of the atmosphere's thickness. Over this depth, coverage fills in and erosion, detail and the hurricane eyes fade out. Deeper fill means softer, hazier depths and more of the atmosphere finely sampled.

**`FloorRelief`** · 0 to 1 · default 0.5 · gas giant 0.5

How high a floor of full-density mounds rises into the fill, in fills. The mounds are shaped by the structure noise, and the detail layer erodes their edges. At 0 there's no floor, and at 1 the mounds reach the cloud base. The floor only adds cloud, so the deck above is unchanged.

**`FloorSoftness`** · > 0 · slider 0.01 to 1 · default 0.25 · gas giant 0.25

How soft the mounds' edges are, in fills: lower is harder edges.

**`Darkening`** · 0 to 1 · slider to 0.2 · default 0.01 · gas giant 0.01

The least absorption the sky's light meets as it fades into the deck below each column's base, whatever the albedo. Higher darkens the depths sooner. It changes neither the albedo nor sunlight.

**`MaterialDepth`** · ≥ 0 · slider to 4 · default 0.25 · gas giant 0.25

How deep below a column's base, in fills, the deck blends from the cloud and storm material to the deep material. At 0 it switches right at the base. Larger values let the upper material run deeper before the deep colour shows.

**`Scatter`** · default (0.98, 0.98, 0.98) · gas giant **(0.06756, 0.0325, 0.1615)**

The deep material's albedo per channel, from 0 to 1: the colour of the depths.

**`Extinction`** · default (1, 1, 1, 1) · gas giant (1, 1, 1, 1)

The deep material's opacity tint per channel. Alpha is its opacity as a multiple of fair-weather cloud's.

#### Material

The clouds' colour and opacity: fair-weather cloud at one end, storm cloud at the other. They blend by a storm index (`StormBalance`, `StormBlend`), not by cloud type. The index is the larger of a column's sim cloud and its storm, so the thick cores of systems take the storm material along with storms.

- **Scatter colours** are single-scattering albedo: how much light survives each scattering event. 1 is white, and lower values absorb in that channel.
- **Extinction colours** tint the opacity per channel, with 1 neutral.
- **Alpha channel:** on `StormExtinction` it is the storm cloud's opacity as a multiple of fair-weather cloud's.

**`CloudOpticalDepth`** · ≥ 0 · slider from 0.1 · default 40 · terrestrial **20** · gas giant **10**

**The main opacity:** optical depth through a full column of fair-weather cloud. Coverage and the noise take most columns well under it. Higher values give denser, darker-bottomed cloud with crisper silhouettes.

**`StormBalance`** · 0 to 1 · default 0.5 · terrestrial **0.6** · gas giant **0.6**

**How much of the cloud uses the storm material,** from 0 (none) to 1 (all). Columns rank by stormy weather, hurricanes first, and the storm material fills in from the top of that ranking. It changes colour only, never the cloud's shape.

**`StormBlend`** · > 0 · slider to 1 · default 0.15 · terrestrial **0.6** · gas giant **0.5**

How gradually fair-weather colour turns into storm colour from a system's edge toward its core. Small values give a crisp split, and large values a long gradient.

**`CloudScatter`** · default (0.98, 0.98, 0.98) · terrestrial (0.98, 0.98, 0.98) · gas giant **(0.1306, 0.588, 0.7795)**

Fair-weather cloud's albedo per channel, from 0 to 1. Near 1 is white cloud, and lower values absorb that colour.

**`CloudExtinction`** · default (1, 1, 1) · terrestrial (1, 1, 1) · gas giant (1, 1, 1)

Fair-weather cloud's opacity tint per channel, with 1 neutral. `CloudOpticalDepth` sets the amount.

**`StormScatter`** · default (0.9, 0.92, 0.95) · terrestrial **(0.1882, 0.1901, 0.2587)** · gas giant **(0.3941, 0.1733, 0.3516)**

Storm cloud's albedo per channel: lower values give darker, coloured storm cloud.

**`StormExtinction`** · default (1, 1, 1, 2) · terrestrial (1, 1, 1, 2) · gas giant (1, 1, 1, 2)

Storm cloud's opacity tint per channel. Alpha is its opacity as a multiple of fair-weather cloud's: 2 makes storms twice as dense.

#### Phase

Which directions the clouds scatter light toward. They use two lobes: a strong forward lobe that makes cloud rims glow around the sun, and a weak backward lobe that lights cloud seen with the sun behind you.

**`ForwardG`** · -0.99 to 0.99 · slider from 0 · default 0.9 · terrestrial 0.9 · gas giant 0.9

How tightly the forward lobe points: higher gives a brighter, narrower silver lining around the sun.

**`BackwardG`** · -0.99 to 0.99 · slider from 0 · default 0.1 · terrestrial 0.1 · gas giant 0.1

How tightly the backward lobe points, as a magnitude.

**`ForwardWeight`** · 0 to 1 · default 0.5 · terrestrial 0.5 · gas giant 0.5

The forward lobe's share of the blend, from 0 to 1.

#### Multiple Scattering

An approximation of light bouncing many times inside cloud. Each extra octave sees the cloud toward the sun as thinner, carries less light and scatters more evenly. Together they give the soft glow inside thick cloud and lighten its shadowed side. Each octave costs one exponential per cloud sample.

**`SunlightPenetration`** · 0 to 1 · default 0 · terrestrial 0 · gas giant 0

How far sunlight gets into cloud. At 0, light is dimmed by the cloud's full opacity. Higher values let more light reach the interior and shadowed side, standing in for light scattered forward. The shadow map uses this too.

**`ScatteringGlow`** · 0 to 1 · default 0.6 · terrestrial 0.6 · gas giant 0.6

How much light the extra octaves carry. Higher values make thick cloud glow brighter inside, and thin cloud brightens overall.

**`ScatteringSpread`** · 0 to 1 · default 0.4 · terrestrial 0.4 · gas giant 0.4

How much more evenly each extra octave scatters. Higher values soften the bright rim around the sun and light the shadowed side more. At 0, every octave stays as forward-pointing as the first.

**`OctaveCount`** · 1 to 4 · default 3 · terrestrial 3 · gas giant 3

The number of octaves, including the first: 1 is single scattering only. Each octave adds cost per cloud sample.

#### Surface Shadow

Cloud shadows on opaque geometry inside the atmosphere, such as terrain, meshes and other actors. It reads the cloud shadow map the clouds are already lit with, so it adds no extra pass. It also applies the air's own dimming and reddening of the sun, scaled by `DirectFraction`, which is what it does on Air Only. It multiplies the lit scene, so specular and bounce light darken along with direct sun. Translucent objects don't receive it.

**`bEnabled`** · default on · terrestrial on · gas giant on

Turns surface shadowing on or off: the clouds' shadows and the air's dimming of the sun.

**`DirectFraction`** · 0 to 1 · default 0.85 · terrestrial 0.85 · gas giant 0.85

How much of a surface's brightness comes from direct sun rather than sky and bounce light. The shadow darkens only that share, so full shade bottoms out at 1 − this rather than black. **This is the setting that stops cloud shadows looking like holes in the world.**

**`Strength`** · ≥ 0 · default 1 · terrestrial 1 · gas giant 1

An artistic multiplier on shadow darkness.

**`CascadeRadii`** · ≥ 0 · default (0.6, 0.3) · terrestrial (0.6, 0.3) · gas giant **(6, 0.3)**

Half-widths of the two inner shadow cascades around the camera, in planet radii: X the middle field and Y the near field. Narrower is sharper near the camera but hands over to the coarse level sooner. These cascades light the clouds as well as surfaces. Only the near field carries the detail layer's grain, so match Y to `DetailLayer.FadeFar`. 0 collapses a level.

### Pipeline

Assets, the sim connection and the performance settings. Tune loads leave the **Pipeline** settings alone unless asked. They also leave the **Quality** settings (Raymarch, Sampling and the three shadow settings) alone unless asked, so a tune carries the look and each machine keeps its own cost.

#### Simulation

The weather sim the clouds read. Only one sim runs per world. The claiming planet nearest the camera drives it with its active model's config, restored from that config's `InitialState`. Other planets draw the weather they kept when they last drove it.

**`TerrestrialConfig`** · default `GasGiantSimScratch` · yours **`TerrestrialSimDefault`**

**Pipeline.** The sim config the Terrestrial model runs, and the snapshot (`InitialState`) restored when the actor switches to it.

**`GasGiantConfig`** · default `GasGiantSimScratch` · yours **`GasGiantSimDefault`**

**Pipeline.** The sim config the Gas Giant model runs.

**`bClaimSimulation`** · default on · yours on

**Pipeline.** Whether this planet competes to drive the sim. When off, it never drives the sim and shows the weather it last kept, or none.

**`BlueNoise`** · default `FastBlueNoise_scalar_128x128x8` · yours **`FastBlueNoise_scalar_128x128x64`**

**Pipeline.** A tiling blue noise texture used to jitter the march: single channel, sRGB off, uncompressed, no mips, nearest filtering. With none set, nothing draws.

#### Raymarch

**Quality.** How many samples a view ray spends on air and on cloud.

**`AtmosphereSteps`** · 2 to 256 · default 64 · yours **32**

Steps a ray spends crossing clear air. Raise it if sky colour bands or the limb looks stepped.

**`CloudSteps`** · 2 to 256 · default 128 · yours **256**

How finely cloud is stepped near the camera: the cloud band's depth divided by this is the base step. Further away, steps grow by `LatticeGrowth`, so the count a ray takes varies. **The main cloud cost.** Raise it if dense cloud looks blocky or noisy up close.

**`ChordSpread`** · ≥ 1 · slider to 64 · default 8 · yours 8

For air, how much longer the last step is than the first: 1 is uniform, and higher values bunch samples near the camera without changing the count. For cloud, the most one step may rise through the cloud band, in base steps, so a distant camera still crosses the deck in several steps. Higher is cheaper and coarser far away.

#### Sampling

**Quality.** How the march spreads its samples across pixels, frames and distance.

**`CellSize`** · 1 to 16 · default 3 · yours **4**

One pixel in each CellSize × CellSize square is marched per frame, and the rest come from history. **The main overall cost:** 3 costs a ninth of full resolution, and 4 a sixteenth. Larger values are cheaper but slower to settle and softer in motion.

**`FreshWeight`** · 0.01 to 1 · default 0.15 · yours 0.15

The least share a new sample takes of a pixel's history. Lower values settle smoother but blur fast motion. Higher values follow change sooner but show more noise.

**`LatticeGrowth`** · > 0 · slider to 1 · default 0.2 · yours 0.2

How much longer each cloud step is than the last, with the camera in or near the cloud layer. Lower is finer and costlier.

**`LatticeGrowthFar`** · > 0 · slider to 1 · default 0.02 · yours 0.02

The same growth from four cloud-layer depths above the clouds and beyond, as when viewing the planet from space. The two are blended by altitude.

#### Baked Lighting

**Quality.** The cloud shadow map, which lights the clouds and shades surfaces. It has three cascades: the whole planet, a middle field and a near field, the last two centred on the camera (`SurfaceShadow.CascadeRadii`).

**`ShadowResolution`** · 128 to 4096 · default 1024 · yours **256**

The size of each shadow cascade, in texels. Bake time and memory grow with its square (2 MB per cascade at 512). Lower values soften shadows rather than aliasing them.

**`ShadowLevelsPerFrame`** · 1 to 3 · default 1 · yours 1

How many cascades are rebaked each frame, in turn: 1 rebakes each cascade every third frame. Raise it if shadows lag fast-moving clouds.

**`ShadowTemporalSmoothing`** · ≥ 0 · slider to 1 · default 0.1 · yours **0.05**

Seconds a rebaked cascade fades in from the previous one. It hides rebake steps, and shadows trail the clouds by about this much.

#### Readouts

Read-only state the actor creates at runtime: render targets, the sun light component and the transmittance table. They are useful to inspect, and there is nothing to set.

- **`ShadowTarget`**: The baked cloud shadow map: one slice per cascade.
- **`CoverageTarget`**: One texel: the coverage threshold solved each frame from `CloudCover`.
- **`FlowTarget`**: The weather map this planet draws: the sim's own while it drives the sim, otherwise `KeptFlow`.
- **`KeptFlow`**: This planet's weather as it last drove the sim, kept when another planet takes over.
- **`SimDebugView`**: The sim's debug view, while this planet drives the sim and the config's `bDebugView` is on.
- **`SunLightComponent`**: The directional light the actor owns, synced to its rotation, `LightColor` and `LightIntensity`.
- **`TransmittanceTable`**: The air's transmittance lookup table, built on first use.

## The sim config

`UFlowSimConfig` assets, one per model, assigned on the actor under Pipeline → Simulation. The sim rereads the asset every frame, so you can tune while it runs. Changes to the grid (`GridResolution`, `LayerCount`) reset it. Your values are from `TerrestrialSimDefault` and `GasGiantSimDefault`.

### Winds

The prevailing winds the sim holds the flow to, which also set where storms grow. Wind speeds are fractions of the speed root (see Units).

**`ZonalProfile`** · default Banded · terrestrial **ThreeCell** · gas giant Banded

The prevailing wind pattern:

- **Banded:** alternating east and west jets, `BandCount` repeats of them, as on a gas giant.
- **ThreeCell:** Earth's three cells. They are easterly trade winds to about 25°, a westerly jet peaking at `JetLatitude`, and polar easterlies beyond about 65°.

It also sets which way hurricanes spin: with their band's shear when banded, or by hemisphere when three-cell.

**`JetLatitude`** · 15 to 75 · default 45 · terrestrial 45 · gas giant 45

**ThreeCell.** The latitude of the westerly jet, in degrees. The whole pattern stretches with it, so the trades and polar easterlies move in proportion. Past about 53°, the polar easterlies leave the pole.

**`TradeWindStrength`** · slider 0 to 1 · default 0.35 · terrestrial 0.35 · gas giant 0.35

**ThreeCell.** The trade winds' strength relative to the westerly jet. Stronger trades carry tropical weather west faster and shear it against the jet.

**`PolarEasterlyStrength`** · slider 0 to 1 · default 0.5 · terrestrial 0.5 · gas giant 0.5

**ThreeCell.** The polar easterlies' strength relative to the westerly jet. Stronger values give a sharper polar front.

**`DeformationRadius`** · ≥ 0.01 · default 0.2 · terrestrial 0.2 · gas giant **0.25**

**The size of weather systems:** the radius eddies settle at, in planet radii (at 45°). Smaller values give more, smaller systems and need a finer grid to resolve. It is also the unit of storm cell sizes and the stirring's scale.

**`SpeedRoot`** · 0.1 to 1 · default 0.6 · terrestrial 0.6 · gas giant **0.8**

**The master speed:** how fast the fastest flow runs relative to the sim's wave speed. Every authored wind is a fraction of it. It also sets the turnover (see Units), so lower values slow the whole system. About 0.6 is a comfortable top. Higher values run faster but push the fastest winds toward the speeds where flow steepens into bores.

**`JetSpeed`** · default 0.35 · terrestrial **0.5** · gas giant **0.4**

The jets' peak wind, as a fraction of the speed root, on a layer with `JetScale` 1. Negative values reverse the jets.

**`ShearSpeed`** · default 0.2 · terrestrial **1.05** · gas giant **1.05**

**Two layers or more.** How much faster the top layer blows than the bottom, as a fraction of the speed root. This shear is the temperature contrast storms grow from: past a threshold (reported in the start log), eddies grow into weather systems.

**`BandCount`** · ≥ 1 · default 3 · terrestrial 3 · gas giant **5**

**Banded.** How many times the band pattern repeats from pole to pole. Each repeat is one east and one west jet. Fractions are allowed.

**`BaroclinicLatitude`** · 0 to 90 · default 45 · terrestrial **20** · gas giant **30**

**Midlatitude thermal shape.** The centre of the zone where the shear sits and storms grow, in degrees.

**`BaroclinicWidth`** · 1 to 90 · default 24 · terrestrial **30** · gas giant **45**

**Midlatitude thermal shape.** The half-width of that zone, in degrees. It must span a few deformation radii for storms to grow.

**`EquatorialBoost`** · default 0.5 · terrestrial **0** · gas giant 0.5

**Banded.** Extra eastward wind in the equatorial jet (super-rotation), as a share of the jets' strength. Each layer scales it by its `BoostScale`.

**`Asymmetry`** · default 0.5 · terrestrial 0.5 · gas giant 0.5

**Banded.** How differently the bands sit in each hemisphere: 0 mirrors them about the equator.

**`WidthBias`** · default 0 · terrestrial 0 · gas giant 0

**Banded.** Positive values widen the eastward bands, and negative values widen the westward ones.

**`JetIrregularity`** · ≥ 0 · slider to 0.8 · default 0.45 · terrestrial 0.45 · gas giant 0.45

**Banded.** How uneven the bands are. At 0, jets are evenly spaced at equal strength. Higher values vary widths and strengths, and some bands merge.

**`JetHarmonic`** · slider 1.3 to 1.9 · default 1.7 · terrestrial 1.7 · gas giant 1.7

**Banded.** Which bands come out wide or strong. Near a whole-number ratio (1, 2), the pattern visibly repeats.

**`JetFlatness`** · 0.05 to 0.95 · slider 0.2 to 0.7 · default 0.5 · terrestrial 0.5 · gas giant 0.5

**Banded.** How far the jets' peaks flatten into plateaus of even wind.

**`EquatorialJetWidth`** · 0.5 to 89 · slider 5 to 30 · default 13.91 · terrestrial 13.91 · gas giant 13.91

**Banded.** The latitude, in degrees, at which the equatorial boost falls to half: a narrow equatorial jet or a broad super-rotating belt.

**`ThermalShape`** · default Midlatitude · terrestrial Midlatitude · gas giant Midlatitude

Where the shear (`ShearSpeed`) sits in latitude:

- **Midlatitude:** one zone per hemisphere around `BaroclinicLatitude`, like a terrestrial planet's equator-to-pole temperature contrast.
- **FollowJets:** the jet profile itself, like a banded gas giant whose jets weaken with height.

**`PlanetaryVorticity`** · ≥ 0.1 · default 24 · terrestrial **6** · gas giant **12**

Twice the planet's rotation rate, in the sim's units: the Coriolis effect. Together with `DeformationRadius` it sets the wave speed the speed root scales. Changing it mostly changes the tempo, like `SimSpeed`. Very high values with a large `StepSize` radiate ripples.

### Flow Rates

How firmly the flow is held to the prevailing winds, and how quickly eddies lose energy. Rates are per turnover.

**`NudgeRate`** · default 0.1 · terrestrial **0.1964** · gas giant **0.1964**

How quickly each layer's average east–west wind is pulled back to the prevailing pattern, per turnover. Too strong shows as ripples, because every nudge is an unbalanced push.

**`DragRate`** · default 0.15 · terrestrial **0.1964** · gas giant **0.1964**

Friction on the eddies, per turnover, scaled per layer. It is the energy sink that stops eddies growing forever, and it makes flow spiral into lows and out of highs. The stirring scales with it, so it sets how long eddies stay organised, not how fast they spin. At 0, the stirring stops too.

**`ThermalRelaxation`** · ≥ 0 · default 0.05 · terrestrial **0.0491** · gas giant **0.0491**

How quickly the layers' heights relax back toward balance with the winds, per turnover. This restores the temperature contrast storms draw on.

### Stirring

Random stirring that keeps eddies (weather systems) forming. It is driven by a tiling noise volume read as a swirling pattern that changes over time.

**`EddySpeed`** · ≥ 0 · default 0.15 · terrestrial **0.6** · gas giant **0.25**

**The amount of weather:** the eddy speed the stirring sustains, as a fraction of the speed root, scaled per layer by `EddyScale`. Higher values give more, livelier systems. With `GenesisMoisture` 0, it also sets how often hurricanes form.

**`ForcingLifetime`** · ≥ 0.01 · slider from 0.05 · default 5 · terrestrial **2.5** · gas giant **3**

How long one stirring pattern lasts, in turnovers.

**`ForcingFrequency`** · ≥ 0 · slider from 0.0001 · default 0.05 · terrestrial **0.03** · gas giant **0.03**

Stirring noise tiles per deformation radius, so the stirring scales with the eddies.

**`ForcingVolume`** · default none · terrestrial **`VT_PerlinWorley_S4_128`** · gas giant **`CloudNoise_4_128`**

The tiling noise texture the stirring reads. It is part of the tune: the texture's feature size sets the eddies' size. With none set, there is no stirring.

**`ForcingChannel`** · 0 to 3 · default 1 · terrestrial **0** · gas giant **0**

Which channel of `ForcingVolume` is read. Each channel is a different noise octave, so this changes the eddies' size.

### Moisture

Water vapour and cloud. The surface moistens the bottom layer, rising air condenses vapour into cloud, sinking air evaporates it, and cloud rains out over `CloudLifetime`. Rates and lifetimes are per turnover.

**`CondensationOnset`** · 0 to 0.99 · default 0.7 · terrestrial **0.5** · gas giant **0.5**

The relative humidity at which rising air starts to condense. It condenses fully at saturation. Lower values give more cloud, sooner.

**`SurfaceEvaporation`** · ≥ 0 · default 0.2 · terrestrial **0.3928** · gas giant **0.4**

How quickly the surface moistens the bottom layer toward saturation, per turnover. **The moisture supply:** higher values give wetter, cloudier weather.

**`SaturationPoleRatio`** · ≥ 0 · default 0.25 · terrestrial **1** · gas giant **1**

How much vapour the poles hold relative to the equator. 1 is uniform. Lower values dry the poles, as on a cold-poled planet.

**`LatentHeating`** · ≥ 0 · default 0.1 · terrestrial 0.1 · gas giant 0.1

Heat released by condensation, which deepens lows under condensing air. It is a feedback: too strong runs away into grid-scale convection.

**`CloudLifetime`** · ≥ 0.01 · default 30 · terrestrial **10** · gas giant **20**

How long cloud lasts in still air before raining out, in turnovers. Longer values give more persistent, widespread cloud.

**`UpperSaturation`** · > 0, ≤ 1 · default 0.3 · terrestrial **0.5** · gas giant **0.5**

**Two layers or more.** How much vapour the top layer holds relative to the bottom: cold air aloft holds little. Don't set it to 0, which makes cloud for free and leaves the upper layers overcast.

**`WindEvaporationGain`** · ≥ 0 · default 1 · terrestrial **2** · gas giant **2**

How much wind speeds up surface evaporation: the extra moisture a storm's own winds draw from the surface.

**`CondensationRate`** · ≥ 0 · default 0.5 · terrestrial **0.7857** · gas giant **0.7857**

How quickly rising saturated air fills with cloud, per turnover.

**`EvaporationRate`** · ≥ 0 · default 0.3 · terrestrial **0.1964** · gas giant **0.1964**

How quickly sinking air evaporates cloud, per turnover. It also fades storm intensity in sinking air, and with a negative `StormCellEyeDraft` it clears hurricane eyes.

### Storms

The storm tracer: where condensing, rising, spinning air builds storm intensity. Storm deepens and darkens cloud (Coverage, Type, Material on the actor) and is what hurricanes spawn from.

**`StormThreshold`** · 0 to 0.99 · default 0.1 · terrestrial 0.1 · gas giant 0.1

How strongly air must rise near saturation before storm builds. Higher values confine storm to the most vigorous convection.

**`StormAmount`** · ≥ 0 · default 4 · terrestrial **16** · gas giant **64**

How intense storm gets under full drive: a column held at full drive settles at `StormAmount` / (1 + `StormAmount`). It also sets how fast storm builds and how firmly hurricanes hold their storm.

**`StormLifetime`** · ≥ 0.01 · default 10 · terrestrial 10 · gas giant **40.73**

How long storm lasts once its drive is gone, in turnovers.

**`StormSpin`** · ≥ 0 · default 2 · terrestrial 2 · gas giant 2

How much cyclonic spin boosts storm growth.

### Hurricanes

Storm cells: tracked hurricanes with an eyewall, spiral bands and an eye. They spawn on strong storms inside the genesis band and hold their strength while conditions last. Sizes are in deformation radii, and winds are fractions of the speed root.

**`MaxStormCells`** · 0 to 32 · default 12 · terrestrial **3** · gas giant **0**

The most hurricanes alive at once. There are 32 cell slots, shared with the perpetual storms, which take theirs first. 0 turns hurricanes off.

**`StormCellSpawnRate`** · ≥ 0 · default 0.4 · terrestrial **1** · gas giant **6**

Spawn attempts per turnover, planet-wide, scaled by the share of hurricane slots still free. Each attempt tests eight points in the genesis band for a storm strong enough to seed on. **The main hurricane frequency control,** together with `GenesisMoisture`.

**`GenesisLatitudeMin`** · 0 to 90 · default 8 · terrestrial 8 · gas giant 8

The equatorward edge of the band where hurricanes form, in degrees. Keep it a few degrees off the equator, where their spin direction flips.

**`GenesisLatitudeMax`** · 0 to 90 · default 22 · terrestrial **28** · gas giant **35**

The poleward edge of the band where hurricanes form, in degrees.

**`StormCellLifetime`** · ≥ 0.01 · slider from 0.05 · default 30 · terrestrial **20** · gas giant **256**

The turnovers after which a hurricane starts to decay whatever the conditions. It then fades over a further ln(20) / (`StormCellGrowth` × (1 − `StormCellPersistence`)) turnovers.

**`StormCellSustainRatio`** · ≥ 0 · default 3 · terrestrial **1.2** · gas giant **3.333**

How much storm a mature hurricane keeps feeding its eyewall, as a multiple of what genesis needed. At 0, a hurricane lives only as long as the storm it spawned on. From about 1.25 up, it keeps its parent storm alive, ending only when conditions fail or `StormCellLifetime` passes.

**`StormCellRadius`** · ≥ 0.01 · default 0.7 · terrestrial **3** · gas giant **4**

**Hurricane size:** the outer radius where every effect reaches zero, in deformation radii. It is held to between 0.5° and 45°, and the start log reports it in degrees.

**`StormCellEyewall`** · 0.01 to 0.95 · default 0.18 · terrestrial **0.1** · gas giant **0.15**

Where the winds peak, as a fraction of the radius.

**`StormCellWind`** · ≥ 0 · default 0.6 · terrestrial **0.7** · gas giant **0.8**

**Hurricane strength:** the eyewall wind a mature hurricane holds, as a fraction of the speed root. The push regulates itself to reach this. Keep it under about 0.7 less the wind the storm rides on. Far above that, the vortex flattens against the speed cap.

**`StormCellCloudCover`** · 0 to 0.99 · default 0.75 · terrestrial 0.75 · gas giant **0.8**

How strongly a hurricane lifts cloud toward full cover at its eyewall, fading with distance. A hurricane is then the last thing coverage or erosion removes.

**`StormCellSpacing`** · ≥ 0 · slider 1 to 4 · default 2 · terrestrial 2 · gas giant 2

The least distance between hurricanes at spawn, in radii: 1 packs them edge to edge, and 3 keeps them well apart.

### Hurricane Dynamics

How storm cells form, grow, decay and move, and the wind pattern they push into the flow.

**`GenesisShearRatio`** · ≥ 0.01 · default 2.5 · terrestrial **1.35** · gas giant **1.338**

**Two layers or more.** The layer-to-layer wind difference at which shear tears storms apart and no hurricane can form, as a multiple of `ShearSpeed`. Lower values close the genesis window sooner.

**`GenesisHumidityMargin`** · -1 to 1 · default 0.15 · terrestrial **0.25** · gas giant **0**

The humidity above `CondensationOnset` a hurricane needs to form. A live hurricane weakens below saturation and dies `StormCellDryTolerance` below this.

**`StormCellDryTolerance`** · slider 0 to 0.5 · default 0.15 · terrestrial 0.15 · gas giant 0.15

How far below the genesis humidity a hurricane survives: landfall and dry air intrusions.

**`GenesisStormRatio`** · ≥ 0 · slider 0.05 to 1 · default 0.25 · terrestrial 0.25 · gas giant 0.25

How strong the storm under a seed must be, as a share of full-drive storm. A hurricane weakens once the storm around it falls below this.

**`GenesisMoisture`** · 0 to 1 · default 0 · terrestrial 0 · gas giant 0

How far humidity can stand in for a parent storm. At 0, hurricanes need the sim's storm under them, so `EddySpeed` sets how often they form. At 1, a saturated column is enough, and hurricanes form at `StormCellSpawnRate` wherever the window and humidity allow.

**`GenesisSpin`** · ≥ 0 · default 0.05 · terrestrial 0.05 · gas giant 0.05

The cyclonic spin at which a seed counts fully. Weaker spin weights it down, and 0 ignores spin.

**`StormCellGrowth`** · ≥ 0 · default 0.2 · terrestrial **0.7857** · gas giant **0.7857**

How fast a hurricane's intensity grows while conditions hold, per turnover.

**`StormCellMaturity`** · > 0 · slider 0.05 to 1 · default 0.25 · terrestrial 0.25 · gas giant 0.25

The intensity at which a hurricane pushes at full strength. Lower values snap new storms to full wind, and higher values wind them up over their growth.

**`StormCellPersistence`** · 0 to 1 · default 0.5 · terrestrial **1** · gas giant **1**

How much of a hurricane outlasts its conditions: its decay rate is `StormCellGrowth` × (1 − this). At 1, hurricanes never decay, and once every slot is full no new one can spawn.

**`StormCellDriftSpeed`** · ≥ 0 · default 0.05 · terrestrial **0.02** · gas giant **0.02**

Poleward and westward drift on top of the steering winds, as a fraction of the speed root.

**`StormCellFollow`** · ≥ 0 · default 0.2 · terrestrial **0.3928** · gas giant **0.3928**

How strongly a hurricane is pulled toward the centre of the storm under it, per turnover. It keeps the hurricane on its parent storm.

**`StormCellCoreFollow`** · ≥ 0 · default 0.8 · terrestrial **1.571** · gas giant **1.571**

How strongly a hurricane is pulled onto its own centre of rotation, per turnover, keeping the eye centred as the vortex drifts. At 0, the hurricane is left to the steering winds.

**`StormCellFalloff`** · ≥ 0.1 · default 1.5 · terrestrial **2** · gas giant **2**

How fast winds fall from the eyewall to the edge: 1 is linear, and higher values tighten the storm onto its core.

**`StormCellWindBreadth`** · 0 to 0.95 · default 0 · terrestrial **0.25** · gas giant **0.25**

The share of the span from eyewall to edge over which wind holds its peak before falling off: a broad band of strongest wind. It affects the wind only. Cloud, storm and draft still peak at the eyewall.

**`StormCellEyeStrength`** · 0 to 1 · default 0.2 · terrestrial **0** · gas giant **0**

Wind strength at the eye's edge (`StormCellEyeRatio`), as a fraction of the eyewall's.

**`StormCellForcing`** · ≥ 0 · default 0.15 · terrestrial **3** · gas giant **3**

How firmly the flow is pushed toward the hurricane's wind pattern, per turnover. Higher values spin storms up faster and hold them tighter against the surrounding flow.

**`StormCellTopShare`** · -1 to 1 · default 0 · terrestrial **-0.33** · gas giant **-0.33**

**Two layers or more.** The top layer's share of the hurricane's wind, with the bottom's at 1. Negative values spin the top the other way, as a storm's outflow does.

**`StormCellInflow`** · 0 to 1 · default 0.2 · terrestrial **0.5** · gas giant **0.5**

**Two layers or more.** Inflow at the bottom and outflow at the top at the eyewall, as a fraction of the wind: the spiral's inflow angle (0.2 is about 11°, and 0.4 about 22°). It turns rings into trailing spiral bands. 0 is off.

**`StormCellEyeDraft`** · -1 to 1 · default -0.5 · terrestrial **-0.33** · gas giant **-0.33**

Vertical motion in the eye. Negative values sink the air, so cloud carried into the eye evaporates and the eye clears. Positive values condense cloud in the eye.

**`StormCellEyeRatio`** · 0 to 0.9 · default 0.44 · terrestrial **0.05** · gas giant **0.05**

The eye's radius as a share of the eyewall's. Inside it, winds fall linearly to calm at the centre.

### Hurricane Look

How storm cells mark the weather the clouds read: storm intensity, pressure, the eye and the eyewall band.

**`StormCellStorm`** · 0 to 1 · default 1 · terrestrial **0.75** · gas giant **0.75**

The storm intensity a hurricane raises across its whole area, full through the eyewall and easing to none at the edge. Storm deepens and darkens existing cloud without adding any.

**`StormCellStormBlend`** · ≥ 0 · slider 0.05 to 0.5 · default 0.25 · terrestrial 0.25 · gas giant 0.25

How smoothly a hurricane's storm merges into the storm around it. Higher values melt it into its surroundings, and lower values leave a visible crease.

**`StormCellPressure`** · ≥ 0 · slider to 2 · default 0.5 · terrestrial **2** · gas giant **2**

The pressure drop through the hurricane, easing to none at the edge. The cloud layer raises its tops and lowers its bases under lows (Lift on the actor).

**`StormCellEyeDepth`** · 0 to 1 · default 0.8 · terrestrial **1** · gas giant **1**

How much of the cloud's depth a full-strength eye removes at its centre. 1 clears it completely, and lower values leave a floor of cloud.

**`StormCellEyeSoftness`** · 0.05 to 1 · default 0.75 · terrestrial **1** · gas giant **1**

How gradually the eye's wall slopes in from the eyewall. Higher values give a gentle descent and a small clear floor. Lower values give a steep wall around a broad clear floor.

**`StormCellEyeRate`** · ≥ 0 · default 3.14 · terrestrial 3.14 · gas giant 3.14

How quickly the eye clears under a live hurricane, per turnover. Higher values form a crisp eye at once.

**`StormCellEyeTrail`** · ≥ 0.01 · slider 0.2 to 5 · default 1.274 · terrestrial 1.274 · gas giant 1.274

How long the eye lingers where the hurricane has moved on, in turnovers. The winds wind it up, so longer values draw spiral streaks of clear air behind moving storms.

**`StormCellBandExcess`** · 0 to 1 · default 0 · terrestrial **0.25** · gas giant **0.25**

Extra storm intensity in the eyewall band, on top of `StormCellStorm`.

**`StormCellBandFloor`** · 0 to 0.99 · slider 0.1 to 0.9 · default 0.4 · terrestrial 0.4 · gas giant 0.4

Where the eyewall band begins, as a share of peak wind strength. Lower values spread it out into the spiral arms, and higher values tighten it to a ring.

**`StormCellDraft`** · -1 to 1 · default 0.5 · terrestrial **0.15** · gas giant **0.15**

Rising air added with the hurricane's wind strength, which builds towering cloud and fills the column in.

### Perpetual Storms

Storms that never die, like Jupiter's Great Red Spot: a storm cell with its own size, wind and oval, held in the shear between two opposite jets, which sets its spin. They follow a closed-form path, so snapshots don't carry them. The hurricane shape and look settings apply to them too. They take the first cell slots, and hurricanes get up to `MaxStormCells` of the rest.

**`PerpetualStorms`**

The list of perpetual storms, up to the shader's limit. Each entry has the settings below.

**`Latitude`** · -85 to 85

The latitude asked for, in degrees. The storm settles at the nearest latitude within 45° where the winds reverse, between two opposite jets. `CloudAtmosphere.DumpParams` reports where each one settled.

**`Longitude`** · slider -180 to 180

Its longitude at sim time zero, in degrees.

**`Radius`** · ≥ 0.05

Half-height north to south, in deformation radii. It is kept large enough for its eyewall to span two grid columns.

**`Aspect`** · ≥ 1 · slider to 4

East–west length over north–south height: 1 is round, and 2 is an oval twice as long. The wind falls to 1/`Aspect` at the oval's ends.

**`Wind`** · 0 to 0.9

Eyewall wind, as a fraction of the speed root.

**`Spiral`** · -1 to 1

Inflow below and outflow above, as a share of `Wind`. It winds spiral arms, and negative values reverse them. It needs two layers or more.

**`Steering`** · 0 to 1

How much the winds across it carry it. At 0, it holds its place. At 1, an imbalance between its two jets moves it toward the stronger jet's direction.

**`Drift`** · -1 to 1

Eastward drift on top of steering, as a fraction of the speed root. Negative values drift west.

**`Storm`** · 0 to 1

Storm intensity across the whole storm, as `StormCellStorm`.

**`Cover`** · 0 to 0.99

The cloud cover its eyewall settles at, as `StormCellCloudCover`.

**`Lift`** · ≥ 0 · slider to 2

The pressure drop across it, as `StormCellPressure`. The cloud tops rise over it.

**`Eye`** · 0 to 1

How much of a hurricane's eye it opens: 0 is a calm core with no hole, and 1 an eye as deep as a hurricane's.

Your gas giant's perpetual storms (the terrestrial config has none):

| Latitude | Longitude | Radius | Aspect | Wind | Spiral | Steering | Drift | Storm | Cover | Lift | Eye |
|---|---|---|---|---|---|---|---|---|---|---|---|
| -22 | 0 | 1 | 1.5 | 0.6 | 0.5 | 0 | 0 | 1 | 0.8 | 0.3 | 0.5 |
| 22 | 51.84 | 0.5 | 1.5 | 0.6 | 0.5 | 0 | 0 | 1 | 0.8 | 0.5 | 0.5 |

**`PerpetualStormForcing`** · ≥ 0 · default 3 · terrestrial 3 · gas giant 3

How firmly the flow is pushed toward the perpetual storms' wind patterns, per turnover (their `StormCellForcing`). Too low against the stirring, and they show as cloud cover without rotation.

**`PerpetualStormClearance`** · ≥ 0 · slider 0.5 to 3 · default 1.5 · terrestrial 1.5 · gas giant 1.5

How far from a perpetual storm no hurricane may spawn, in multiples of its reach.

### Noise Motion

How the cloud noise moves with the weather. The sim carries a displacement field that the actor's noise layers follow (their `FlowInherit`). It resets periodically so it can't wind up forever.

**`NoiseDriftSpeed`** · default 0.3 · terrestrial **0.35** · gas giant **0.35**

An even drift the noise carries on top of the winds, as a fraction of the speed root.

**`NoiseResetTurnovers`** · ≥ 0.1 · default 3 · terrestrial **8** · gas giant **12**

How long the noise displacement builds up before it resets, in turnovers. Longer values wind the noise further into swirls and streaks.

### Time

How fast the sim runs.

**`SimSpeed`** · ≥ 0 · default 0.0025 · terrestrial **0.005** · gas giant **0.005**

**The speed control:** sim time per second of real time. The step size stays the same, so speed changes the steps per frame (and the cost), never the look. 0 freezes the sim.

**`SpinUpTurnovers`** · ≥ 0 · slider to 360 · default 13 · terrestrial **0** · gas giant **0**

Turnovers to run at a fast spin-up step before the sim is considered ready. It is skipped when an `InitialState` is set.

### Quality

Grid resolution, step size and layer count: cost and look.

**`GridResolution`** · 16 to 512 · default 64 · terrestrial 64 · gas giant 64

The size of one face of the cube map the weather is stored in, rounded down to a power of two. The solver's grid is 4× this in longitude and 2× in latitude, so 64 runs at 256 × 128. Higher values resolve smaller weather at a steep cost. Changing it resets the sim.

**`StepSize`** · > 0, ≤ 0.044 · default 0.0001 · terrestrial **0.0005091** · gas giant **0.001358**

Turnovers per step: a control for both look and cost, independent of speed. **The weather depends on it:** larger steps give sharper, thicker cloud. Smaller steps cost more per unit of sim time.

**`LayerCount`** · 1 to 8 · default 2 · terrestrial 2 · gas giant 2

Layers in the stack, top to bottom. Two is the fewest that grows storms from shear. One is a single shallow layer. Changing it resets the sim.

### Layers

The vertical stack of fluid layers. Two or more layers allow the shear that storms grow from.

**`LayerProfiles`**

Per-layer multipliers, listed top layer first. Layers between entries interpolate. An empty list uses the profile's defaults.

**`JetScale`**

Scales `JetSpeed` on this layer.

**`BoostScale`**

Scales `EquatorialBoost` on this layer. Banded only.

**`EddyScale`** · ≥ 0

This layer's eddy speed, as a multiple of `EddySpeed`.

**`DragScale`**

Scales the drag (and the stirring with it) on this layer. The bottom layer carries surface friction, and upper layers want little.

**`DepthScale`** · ≥ 0.1

The layer's relative thickness. Only ratios matter. A thicker top layer gives the interface room to rise toward the poles.

Your layer profiles (top layer first):

| Config | Layer | JetScale | BoostScale | EddyScale | DragScale | DepthScale |
|---|---|---|---|---|---|---|
| Terrestrial | 0 | 1 | 1 | 1.5 | 0.5 | 1.5 |
| Terrestrial | 1 | 1 | 1 | 0.05 | 1.5 | 1 |
| Gas giant | 0 | 1.5 | 1 | 1.5 | 0.5 | 1.5 |
| Gas giant | 1 | 0.5 | 1 | 0.05 | 1.5 | 1 |
| Class default | 0 | 1 | 1 | 1.5 | 0.5 | 1.5 |
| Class default | 1 | 1 | 1 | 0.05 | 1.5 | 1 |

**`LayerCoupling`** · default 0.01 · terrestrial **0.01964** · gas giant **0.01964**

**Two layers or more.** Friction between layers, per turnover. Strong coupling erodes the shear storms grow from.

**`Stratification`** · 0.01 to 1 · default 0.1 · terrestrial **0.05** · gas giant **0.05**

**Two layers or more.** The density step between layers. Small values keep the surface flat, so pressure systems ride on the interfaces between layers.

### Numerics

Smoothing that keeps the solver stable.

**`GridDamping`** · ≥ 0 · default 2 · terrestrial **1.277** · gas giant **1.277**

How quickly grid-scale ripples and bores die out, per turnover. Higher values give smoother vertical motion and cloud. Larger waves are barely affected. The start log reports when a large step already damps more than this.

**`FilterLatitude`** · 0 to 1 · default 0.9 · terrestrial 0.9 · gas giant 0.9

Smoothing along latitude rows near the poles, where grid cells crowd together. It first acts where cos(latitude) falls below half of this (63° at 0.9). Higher values filter wider and further from the poles.

### Pipeline

**Pipeline.** Start state and spin-up cost. Tune loads leave these alone unless asked.

**`InitialState`** · default none · terrestrial **`TerrestrialStateDefault`** · gas giant **`GasGiantStateDefault`**

A saved sim state (snapshot) to start from, restored whenever this config starts or an atmosphere switches to it. Leave it empty to seed and spin up. A snapshot that doesn't match the grid or layers is refused. `FlowSim.Save` with no argument captures into it.

**`MaxSpinUpStepsPerFrame`** · ≥ 1 · slider to 64 · default 8 · terrestrial 8 · gas giant 8

Spin-up steps per frame: higher reaches a ready state sooner, at a frame-time cost while spinning up.

### Debug

**Pipeline.** The sim's debug view, drawn one texel per grid cell and shown on the driving actor as `SimDebugView`. The `r.FlowSim.DebugMode`, `r.FlowSim.DebugLayer`, `r.FlowSim.DebugScale` and `r.FlowSim.Paused` console variables override these.

**`bDebugView`** · default off · terrestrial off · gas giant **on**

Draws the debug view.

**`DebugMode`** · default Vorticity · terrestrial Vorticity · gas giant **CellGains**

Which field the debug view shows. See the debug modes table below.

**`DebugLayer`** · 0 to 7 · default 0 · terrestrial **1** · gas giant **1**

Which layer to view.

**`DebugScale`** · ≥ 0 · default 0 · terrestrial **300** · gas giant **300**

The value that maps to full colour. 0 picks a sensible scale for each mode.

**`bPaused`** · default off · terrestrial off · gas giant off

Pauses stepping without losing the state. The debug view keeps updating.

**Debug modes**

| Mode | Shows |
|---|---|
| Vorticity | Spin: red counterclockwise, blue clockwise, seen from outside. |
| Pressure | Montgomery potential less its zonal mean. Positive in highs in both
hemispheres. |
| Speed | Wind speed. |
| East | East–west wind: red eastward, blue westward. |
| North | North–south wind: red northward, blue southward. |
| Residual | Rhs - (I - sL) psi for the vertical mode DebugLayer selects,
featureless once converged. |
| ZonalError | Zonal-mean eastward velocity minus the profile: whether the nudge holds. |
| Vertical | The layer's vertical motion: red rising, blue sinking. |
| Froude | Speed over the wave speed DeformationRadius sets. |
| Cloud | The layer's cloud tracer, 0 to 1. |
| CloudAscent | The vertical motion the cloud formed at, 0 to 1. |
| NoiseDisplacement | Noise displacement magnitude, phase A, in radians. |
| Humidity | Relative humidity less CondensationOnset: red where rising air would
condense. |
| Storm | Storm intensity, 0 to 1, in red; storm cells' vector strength in blue
where it is larger. |
| LayerHeight | Height of the layer's top: the free surface on layer 0, an interface
below it. |
| ColumnCloud | Cloud cover of the layer and every layer above it, 0 to 1: what the
terrestrial deck reads with its CloudLayer set to this layer. |
| Eye | The eye tracer, 0 to 1: clear air fed at storm cells' cores and carried
by the flow, which is how far their eyes thin the deck. |
| CellHealth | Each storm cell's conditions: red the genesis window, green humidity, blue
the parent storm, so a missing colour names the failure; eyewall grey is favour. |
| Genesis | A storm seed's gates at each point: red the genesis window, green humidity,
blue the storm. Bright where all pass, dim where any fails, dimmer near a cell. |
| CellGains | Each storm cell's disc: red the debug layer's vortex gain, green its
inflow gain, as a share of the gain limit. Full brightness is pinned. |

## Project settings

**Project Settings → Plugins → Flow Sim.** Starts a sim in worlds where no atmosphere claims one, so the debug view runs on its own. An atmosphere that claims the sim replaces this config with its own.

**`DefaultConfig`** · yours **`GasGiantSimScratch`**

The config started automatically, and the one the `FlowSim.*` console commands use when given no argument.

**`bAutoStartInEditor`** · yours **on**

Starts `DefaultConfig` automatically in editor worlds.

**`bAutoStartInGame`** · yours **off**

Starts `DefaultConfig` automatically in Play-in-Editor and game worlds. It is off by default, so a shipping world's sim comes from its atmospheres.

## Presets, tunes and runtime calls

- **`UAtmospherePreset`** is a data asset holding one model's look and its sim config, for cooked builds. Set its `PlanetType` first, so the panel shows that model's groups. Then copy a model's row on an actor and paste it onto `Model`.
- **`UAtmosphereTuneLibrary::ApplyPreset`** applies a preset to one of an actor's models.
- **`ApplyTune`** applies a tune file's text to an actor at runtime. Its scope flags (`bAtmosphere`, `bSim`, `bPipeline`, `bQuality`) choose what it touches.
- **On the actor:**
  - `SetPlanetType` and `SetModelParams` change the model and its look from code.
  - `GetWritableSimConfig` gives the config to change at runtime: in a game world, a copy, so the asset stays untouched.
- **Nothing restores itself from a dump or tune.** The actor's saved settings and the sim config assets are what load. Tunes apply only when you load them.

## Console commands

| Command | Does |
|---|---|
| `CloudAtmosphere.DumpParams [Name]` | Writes every actor's settings and the running sim config, with what differs from the defaults, to `Saved/CloudAtmosphere`. |
| `CloudAtmosphere.LoadParams File [Sim\|Atmospheres] [Pipeline] [Quality]` | Loads a dump or tune back into the world. Pipeline settings, and the actor's quality settings, load only when named. The sim config's Quality group and `SimSpeed` always load, and a changed grid or layer count resets the sim. In Play-in-Editor, sim values go to a runtime copy, so tune in the editor world to save them. |
| `CloudAtmosphere.DumpSchema [Name]` | Writes every setting with its metadata, defaults and current values: the source of this guide. |
| `FlowSim.Start [Config]` | Starts the sim with a config, or `DefaultConfig`. While an atmosphere drives the sim, its own config replaces this on the next frame. |
| `FlowSim.Stop` | Stops stepping and keeps the state. |
| `FlowSim.Reset` | Reseeds the sim, or restores its `InitialState`. |
| `FlowSim.Step [N]` | Advances N steps while paused. |
| `FlowSim.Save [Asset]` | Captures the sim into a snapshot asset, by default the running config's `InitialState`. |
| `FlowSim.Status` | Reports steps, sim time and spin-up progress. |
| `r.FlowSim.DebugMode`, `DebugLayer`, `DebugScale`, `Paused` | Override the config's debug settings. -1 (or negative for the scale) uses the config. |
| `r.CloudAtmosphere.Temporal.Debug N` | Temporal resolve views: 1 accumulated samples as a share of their cap (grey); 2 fresh (blue), reprojected (green) and filled (red); 3 this frame's samples alone; 4 motion (red) and clamp rejection (green). |

---

This guide is generated by `Tools/UsageGuide/gen.py` from a `CloudAtmosphere.DumpSchema` file and the descriptions in that folder. After a parameter change, update the description there, run the dump, and regenerate:

    python3 gen.py AtmosphereSchema.json UsageGuide.md
