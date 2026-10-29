#include "PlanetAtmosphereActor.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "CloudAtmosphere.h"
#include "CoreGlobals.h"
#include "Misc/Crc.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Engine/VolumeTexture.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureRenderTarget2DArray.h"
#include "AtmosphereTransmittance.h"
#include "AtmosphereViewExtension.h"
#include "Engine/Texture2D.h"
#include "Math/Float16Color.h"
#include "RenderingThread.h"
#include "SceneViewExtension.h"
#include "TerrestrialShadowMap.h"
#include "FlowSimShaders.h"
#include "FlowSimSubsystem.h"
#include "FlowSimTypes.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/CustomVersion.h"
#include "UObject/PropertyPortFlags.h"

// --------------------------------------------------------------------------
// Constructor
// --------------------------------------------------------------------------

APlanetAtmosphereActor::APlanetAtmosphereActor()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;

    AtmosphereRoot = CreateDefaultSubobject<USceneComponent>(TEXT("AtmosphereRoot"));
    SetRootComponent(AtmosphereRoot);

    SunLightComponent = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("SunLight"));
    SunLightComponent->SetupAttachment(AtmosphereRoot);
    SunLightComponent->SetMobility(EComponentMobility::Movable);
    SunLightComponent->SetUsingAbsoluteRotation(true);
    SunLightComponent->SetUsingAbsoluteScale(true);

    // Default radius = max(OceanRadius, PlanetRadius) at planet defaults:
    // PlanetRadius(100M) + SeaLevel(0.5) * NoiseAmplitude(15M) = 107,500,000 cm
    SetActorScale3D(FVector(107500000.0));

    // DEFAULTS ON THE CDO, for the groups whose struct defaults are not the
    // terrestrial tune; setting them here keeps reset-to-default per instance.
    // The gas giant starts from the terrestrial set, copied at the end.
    Terrestrial.MultipleScattering.SunlightPenetration = 0.0f;

    // SIZED TO BE LOOKED THROUGH FROM UNDERNEATH. Every beta is per atmosphere
    // thickness and every scale height a fraction of it, so a column's optical
    // depth is beta times that fraction and does not move with HeightScale:
    // these land the vertical column near a quarter of one optical depth, where
    // the struct defaults close it at about six.
    //
    // The absorber is the term to watch. It is a Lorentzian scaled by the
    // Rayleigh profile, so its column integral is roughly half a scale height,
    // and at a beta of 100 it alone closes the sky.
    Terrestrial.Air.RayleighDepth = FLinearColor(0.06236388f, 0.1903298f, 0.2996182f, 1.0f);
    Terrestrial.Air.RayleighScaleHeight = 0.15f;
    Terrestrial.Air.MieDepth = FLinearColor(0.09999546f, 0.08930685f, 0.07558977f, 1.0f);
    Terrestrial.Air.MieScaleHeight = 0.1f;
    Terrestrial.Air.MieG = 0.95f;
    Terrestrial.Air.AbsorptionDepth = FLinearColor(0.04203038f, 0.0339376f, 0.0368092f, 1.0f);

    // NOT NEAR-BLACK. Under a cloud base there is a lit surface bouncing light
    // back up, and this term is the whole of it: left near black, standing under
    // the deck is night.
    Terrestrial.Ambient.AirAmbient = FLinearColor(6.666667e-4f, 9.122807e-4f, 1.407407e-3f, 1.0f);
    Terrestrial.Ambient.AirAmbientFloor = 0.02f;
    Terrestrial.Ambient.CloudAmbient = FLinearColor(0.040f, 0.044f, 0.052f, 1.0f);
    Terrestrial.Ambient.CloudAmbientFloor = 0.04f;

    // A cloud shadow lands on lit terrain here rather than on more cloud, so it
    // carries more of the surface's brightness than the deck's does.
    Terrestrial.SurfaceShadow.DirectFraction = 0.85f;

    // Stage A2 (the planet integration's first look, 2026-10-25): an Earth-like
    // shell, a few hundredths of the radius deep, its cloud a third of it, and
    // the cloud's ambient at a thousandth of the star's light: at 0.04 the
    // cloud glowed.
    Terrestrial.Planet.HeightScale = 0.05f;
    Terrestrial.Shape.CloudThickness = 0.3f;
    Terrestrial.Ambient.CloudAmbient = FLinearColor(0.0010f, 0.0011f, 0.0013f, 1.0f);

    // NO SPIN ON A SURFACE THAT DOESN'T TURN. The sim runs in the planet's
    // rotating frame (its Coriolis is the planet's spin), and the field turns
    // with the planet's own frame (GetFieldFrame): a spin on top slides every
    // cloud across the ground at the planet's rotation speed, far faster than
    // the sim's winds churn them. A gas giant, with no surface, keeps one.
    Terrestrial.Planet.SpinRatio = 0.0f;

    // Default source assets, so the details panel shows something and a fresh
    // actor renders. A field with no volumes is not a subtle failure -- erosion
    // goes to its neutral value and the clouds come out as smooth slabs.
    // THE TYPED CLOUD NOISE, which the genus weights and the detail channels
    // read as noise types.
    static ConstructorHelpers::FObjectFinder<UVolumeTexture> DefaultCloudStructureVolume(
        TEXT("/CloudAtmosphere/Noise/CloudNoise_4_128"));
    if (DefaultCloudStructureVolume.Succeeded())
    {
        Terrestrial.StructureLayer.Volume = DefaultCloudStructureVolume.Object;
        GasGiant.StructureLayer.Volume = DefaultCloudStructureVolume.Object;
    }
    else
    {
        UE_LOG(LogCloudAtmosphere, Warning, TEXT("PlanetAtmosphereActor: Failed to load default cloud structure volume"));
    }

    static ConstructorHelpers::FObjectFinder<UVolumeTexture> DefaultCloudDetailVolume(
        TEXT("/CloudAtmosphere/Noise/CloudNoise_8_128"));
    if (DefaultCloudDetailVolume.Succeeded())
    {
        Terrestrial.DetailLayer.Volume = DefaultCloudDetailVolume.Object;
        GasGiant.DetailLayer.Volume = DefaultCloudDetailVolume.Object;
    }
    else
    {
        UE_LOG(LogCloudAtmosphere, Warning, TEXT("PlanetAtmosphereActor: Failed to load default cloud detail volume"));
    }

    // The engine's scalar blue noise: eight 128 x 128 slices stacked, which the
    // march wraps as one tile.
    static ConstructorHelpers::FObjectFinder<UTexture2D> DefaultBlueNoise(
        TEXT("/Engine/EngineMaterials/FastBlueNoise_scalar_128x128x8"));
    if (DefaultBlueNoise.Succeeded())
    {
        BlueNoise = DefaultBlueNoise.Object;
    }
    else
    {
        UE_LOG(LogCloudAtmosphere, Warning, TEXT("PlanetAtmosphereActor: Failed to load default blue noise"));
    }

    // Each model's own sim. (Until stage A2 both took GasGiantSimScratch, the
    // gas giant's scratch tune, which a terrestrial planet's weather doesn't
    // sit on.)
    static ConstructorHelpers::FObjectFinder<UFlowSimConfig> DefaultTerrestrialSimConfig(
        TEXT("/CloudAtmosphere/NoiseRecipes/TerrestrialSim_Main"));
    if (DefaultTerrestrialSimConfig.Succeeded())
    {
        Simulation.TerrestrialConfig = DefaultTerrestrialSimConfig.Object;
    }
    else
    {
        UE_LOG(LogCloudAtmosphere, Warning, TEXT("PlanetAtmosphereActor: Failed to load the default terrestrial sim config"));
    }

    static ConstructorHelpers::FObjectFinder<UFlowSimConfig> DefaultGasGiantSimConfig(
        TEXT("/CloudAtmosphere/NoiseRecipes/GasGiantSim_Main"));
    if (DefaultGasGiantSimConfig.Succeeded())
    {
        Simulation.GasGiantConfig = DefaultGasGiantSimConfig.Object;
    }
    else
    {
        UE_LOG(LogCloudAtmosphere, Warning, TEXT("PlanetAtmosphereActor: Failed to load the default gas giant sim config"));
    }

    // The gas giant's noise: broad structure stretched tall through the deep
    // deck, and fine detail.
    GasGiant.StructureLayer.Scale = 1.5f;
    GasGiant.StructureLayer.Aspect = 16.0f;
    GasGiant.DetailLayer.Scale = 24.0f;
    GasGiant.DetailLayer.Aspect = 8.0f;

    // The gas giant starts from the terrestrial tune.
    GasGiant.Air = Terrestrial.Air;
    GasGiant.Ambient = Terrestrial.Ambient;
    GasGiant.MultipleScattering = Terrestrial.MultipleScattering;
    GasGiant.SurfaceShadow = Terrestrial.SurfaceShadow;

    SyncModelFlags();

    // R2 (J4): the preset shown is the preset run: the struct defaults matched no row (a 1024^2
    // shadow map, 16 times every preset's texels, under a panel reading "High").
    ApplyGraphicsPresetValues();
}

// --------------------------------------------------------------------------
// Lifecycle
// --------------------------------------------------------------------------

namespace PlanetAtmosphereVersionDetail
{
    /** The actor's saved-data versions. */
    enum Type : int32
    {
        BeforeVersions = 0,
        /** R2 (J4): the constructor applies the graphics preset's values. */
        PresetDefaults = 1,
        Latest = PresetDefaults
    };
    const FGuid Guid(0x255DBAA8, 0x1ADB49D5, 0xBE1E8DCD, 0x6E93F60E);
    FCustomVersionRegistration Registration(Guid, Latest, TEXT("PlanetAtmosphereActor"));
}

void APlanetAtmosphereActor::Serialize(FArchive& Ar)
{
    Ar.UsingCustomVersion(PlanetAtmosphereVersionDetail::Guid);
    // Properties are saved as differences from the archetype: a value an older
    // level left at the old default wasn't written, and would now load the
    // preset's. Those values are put back first, so whatever was saved still
    // overrides them. Only against the native class's defaults: a Blueprint's
    // own defaults were loaded through this same path.
    if (Ar.IsLoading() && Ar.IsPersistent() && !Ar.HasAnyPortFlags(PPF_Duplicate)
        && Ar.CustomVer(PlanetAtmosphereVersionDetail::Guid) < PlanetAtmosphereVersionDetail::PresetDefaults)
    {
        const UObject* LoadedFrom = GetArchetype();
        if (LoadedFrom && LoadedFrom->GetClass()->HasAnyClassFlags(CLASS_Native))
        {
            const FAtmosphereSamplingParams OldSampling;
            const FAtmosphereRaymarchParams OldRaymarch;
            Sampling.CellSize = OldSampling.CellSize;
            Sampling.FreshWeight = OldSampling.FreshWeight;
            Sampling.LatticeGrowth = OldSampling.LatticeGrowth;
            Raymarch.AtmosphereSteps = OldRaymarch.AtmosphereSteps;
            Raymarch.CloudSteps = OldRaymarch.CloudSteps;
            Raymarch.ChordSpread = OldRaymarch.ChordSpread;
            ShadowResolution = 1024;   // the member's initialiser
        }
    }
    Super::Serialize(Ar);
}

void APlanetAtmosphereActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    ReleaseSimulation(false);
    ReleaseViewExtension();
    Super::EndPlay(EndPlayReason);
}

// An editor deletion routes no EndPlay. The components go with the actor.
void APlanetAtmosphereActor::Destroyed()
{
    ReleaseSimulation(false);
    ReleaseViewExtension();
    Super::Destroyed();
}

// A level unloaded in the editor routes neither.
void APlanetAtmosphereActor::BeginDestroy()
{
    ReleaseViewExtension();
    Super::BeginDestroy();
}

void APlanetAtmosphereActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);

    SyncModelFlags();

    if (!bInitialized)
    {
        bPendingInitialize = true;
    }
}

void APlanetAtmosphereActor::Tick(float DeltaTime)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::Tick);
    Super::Tick(DeltaTime);

    const UWorld* World = GetWorld();

    if (bPendingInitialize && World && !World->IsPreviewWorld())
    {
        bPendingInitialize = false;
        Initialize();
    }

    if (bInitialized && bAtmosphereActive)
    {
        UpdateAtmosphere();
        UpdateLightFromRotation();
    }
}

#if WITH_EDITOR
void APlanetAtmosphereActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    if (PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(APlanetAtmosphereActor, GraphicsPreset))
    {
        ApplyGraphicsPreset();
    }

    SyncModelFlags();

    // Every parameter reaches the passes on the next Tick; the light syncs now.
    if (bInitialized)
    {
        UpdateLightFromRotation();
    }
}

void APlanetAtmosphereActor::EditorApplyTranslation(const FVector& DeltaTranslation, bool bAltDown, bool bShiftDown, bool bCtrlDown)
{
    if (bIsPlanetOwned) return;
    Super::EditorApplyTranslation(DeltaTranslation, bAltDown, bShiftDown, bCtrlDown);
}

void APlanetAtmosphereActor::EditorApplyScale(const FVector& DeltaScale, const FVector* PivotLocation, bool bAltDown, bool bShiftDown, bool bCtrlDown)
{
    if (bIsPlanetOwned) return;
    Super::EditorApplyScale(DeltaScale, PivotLocation, bAltDown, bShiftDown, bCtrlDown);
}

void APlanetAtmosphereActor::PostEditMove(bool bFinished)
{
    if (bIsPlanetOwned)
    {
        // Snap location back -- it follows the planet. Rotation is left alone
        // (light direction); the transform guard holds the planet-set relative scale.
        if (USceneComponent* Root = GetRootComponent())
        {
            Root->SetRelativeLocation(FVector::ZeroVector);
        }
    }
    Super::PostEditMove(bFinished);
}
#endif

// --------------------------------------------------------------------------
// Planet integration
// --------------------------------------------------------------------------

void APlanetAtmosphereActor::OnTransformUpdated(USceneComponent* Component, EUpdateTransformFlags Flags, ETeleportType Teleport)
{
    if (!bIsPlanetOwned) return;

    if (USceneComponent* Root = GetRootComponent())
    {
        // Lock location and scale. Rotation is left alone (light direction).
        // UpdateComponentToWorld broadcasts TransformUpdated again; that pass
        // finds location and scale clean and stops.
        bool bLocationDirty = !Root->GetRelativeLocation().IsNearlyZero(0.01);
        bool bScaleDirty = !Root->GetRelativeScale3D().Equals(PlanetDrivenScale, 0.01);

        if (bLocationDirty || bScaleDirty)
        {
            Root->SetRelativeLocation_Direct(FVector::ZeroVector);
            Root->SetRelativeScale3D_Direct(PlanetDrivenScale);
            Root->UpdateComponentToWorld();
        }
    }
}

void APlanetAtmosphereActor::InitializeFromPlanet(USceneComponent* InAttachParent,
    FVector InScale)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::InitializeFromPlanet);
    bIsPlanetOwned = true;

    // Apply scale BEFORE binding the transform guard -- prevents the old guard
    // from reverting a scale change that the planet actor intends.
    if (USceneComponent* Root = GetRootComponent())
        Root->TransformUpdated.RemoveAll(this);

    if (!InScale.IsZero())
        SetActorScale3D(InScale);

    PlanetDrivenScale = GetRootComponent() ? GetRootComponent()->GetRelativeScale3D() : GetActorScale3D();

    // Re-bind the guard now that PlanetDrivenScale reflects the new scale.
    if (USceneComponent* Root = GetRootComponent())
        Root->TransformUpdated.AddUObject(this, &APlanetAtmosphereActor::OnTransformUpdated);

    // Planet actor handles spawn + attach. Just run our init.
    bPendingInitialize = false;
    Initialize();
}

// --------------------------------------------------------------------------
// Initialize
// --------------------------------------------------------------------------

void APlanetAtmosphereActor::Initialize()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::Initialize);
    DestroyLegacyChildActors();
    PrepareForInitialize();

    // The components' switches are saved with them; parking is runtime state.
    SetAtmosphereActive(bAtmosphereActive);

    if (bAtmosphereActive)
    {
        UpdateAtmosphere();
        UpdateLightFromRotation();
    }

    bInitialized = true;
}

// --------------------------------------------------------------------------
// Teardown and legacy objects
// --------------------------------------------------------------------------

void APlanetAtmosphereActor::DestroyLegacyChildActors()
{
    // A saved actor can hold a post-process component named PostProcess.
    // Unbound and registered, it would enter every view's post-process blend.
    TInlineComponentArray<UPostProcessComponent*> PostProcess(this);

    for (UPostProcessComponent* Component : PostProcess)
    {
        if (Component->GetFName() == TEXT("PostProcess"))
        {
            Component->DestroyComponent();
        }
    }

    if (PostProcessVolume_DEPRECATED)
    {
        PostProcessVolume_DEPRECATED->Destroy();
        PostProcessVolume_DEPRECATED = nullptr;
    }

    if (SunLight_DEPRECATED)
    {
        SunLight_DEPRECATED->Destroy();
        SunLight_DEPRECATED = nullptr;
    }
}

void APlanetAtmosphereActor::ReleaseSimulation(bool bKeepField)
{
    // Released before the actor goes, or a pooled planet leaves the sim
    // stepping with nothing sampling it.
    if (!bClaimedSimulation)
    {
        return;
    }

    bClaimedSimulation = false;

    if (UWorld* World = GetWorld())
    {
        if (UFlowSimSubsystem* Sim = World->GetSubsystem<UFlowSimSubsystem>())
        {
            Sim->ReleaseClaim(this, bKeepField);
        }
    }
}

// --------------------------------------------------------------------------
// Per-frame update
// --------------------------------------------------------------------------

void APlanetAtmosphereActor::UpdateAtmosphere()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::UpdateAtmosphere);
    const FVector PlanetCenter = GetActorLocation();
    const float PlanetRadius = static_cast<float>(GetActorScale3D().GetMax());

    // Light direction from relative rotation -- treated as world-space direction
    // regardless of parent rotation. The user/gizmo sets relative rotation directly.
    const FVector LightDir = GetRootComponent()->GetRelativeRotation().Vector();

    if (HasClouds())
    {
        ClaimSimulation(PlanetCenter, PlanetRadius);
        RequestShadowBake(PlanetRadius, PlanetCenter, LightDir);
    }
    else
    {
        ReleaseCloudResources();
    }

    UpdateTransmittanceTable(PlanetRadius);
    UpdateComputeMarch(PlanetRadius, PlanetCenter, LightDir);
}

static TAutoConsoleVariable<int32> CVarCloudAtmosphereClimateView(
    TEXT("r.CloudAtmosphere.ClimateView"), 0,
    TEXT("1: each planet atmosphere keeps its latest climate map as a texture to look at (its ClimateView property; a transient texture per map). 0 (default): none."),
    ECVF_Default);

/** How far the lift can lower a column's base below CloudBase, in atmosphere
 *  thicknesses: TR_FieldReach's BaseDown. */
static float CloudBaseDown(const FCloudShapeParams& Shape, const FCloudLiftParams& Lift)
{
    const float D = FMath::Max(Shape.CloudThickness, 1e-4f);

    return D * (-FMath::Min(Lift.BaseTropical, 0.0f)
        + FMath::Abs(Lift.BasePressure) - FMath::Min(Lift.AltitudeLift, 0.0f));
}

/** The CloudBase the field is packed with: solved, while bSolveBase on the
 *  slab, so the lowest base lands at BaseMinTarget. */
static float ResolvedCloudBase(const FCloudShapeParams& Shape, const FCloudLiftParams& Lift, bool bDeep)
{
    return (Shape.bSolveBase && !bDeep) ? Shape.BaseMinTarget + CloudBaseDown(Shape, Lift) : Shape.CloudBase;
}

/** READOUT ONLY, NEVER PUSHED. Mirrors TR_FieldReach, TR_TopMax and
 *  TR_BaseMin, for the active model's shape. A mismatch misreports the
 *  readouts and changes nothing drawn. */
static void SolveFieldBounds(FCloudShapeParams& Shape, const FCloudLiftParams& Lift, bool bDeep)
{
    const float D = FMath::Max(Shape.CloudThickness, 1e-4f);

    const float BaseUp = D * (FMath::Max(Lift.BaseTropical, 0.0f)
        + FMath::Abs(Lift.BasePressure) + FMath::Max(Lift.AltitudeLift, 0.0f));

    const float BaseDown = CloudBaseDown(Shape, Lift);

    const float Depth = D * (1.0f + FMath::Abs(Lift.CeilingPressure));

    const float Base = ResolvedCloudBase(Shape, Lift, bDeep);

    Shape.SolvedTopMax = FMath::Min(Base + BaseUp + Depth, 1.0f);
    Shape.SolvedBaseMin = Base - BaseDown;
}

/** The sim's noise drift angle and phase A's position in its reset cycle, at
 *  sim time T. Reduced in double precision, so the shader's trig and its
 *  crossfade weight stay exact however long the sim has run. THE PHASE MUST BE
 *  THE SIM'S OWN: it resets each displacement where this puts its weight at
 *  zero. */
static void NoiseClock(const UFlowSimConfig* Config, double T, float& OutDriftAngle, float& OutPhase)
{
    OutDriftAngle = 0.0f;
    OutPhase = 0.0f;

    if (!Config)
    {
        return;
    }

    OutDriftAngle = (float)FMath::Fmod((double)Config->GetNoiseDriftRate() * T, 2.0 * UE_DOUBLE_PI);

    const double Cycles = T / (double)Config->GetNoiseResetTime();
    OutPhase = (float)(Cycles - FMath::FloorToDouble(Cycles));
}

/** The cloud material as the view and the bake read it: albedo and extinction
 *  non-negative, and fair-weather extinction's amount at 1, since
 *  CloudOpticalDepth carries it. */
static FCloudMaterialParams ResolveCloudMaterial(const FCloudMaterialParams& M)
{
    const auto NonNegative = [](const FLinearColor& C)
        {
            return FLinearColor(FMath::Max(C.R, 0.0f), FMath::Max(C.G, 0.0f), FMath::Max(C.B, 0.0f), FMath::Max(C.A, 0.0f));
        };

    FCloudMaterialParams Out = M;

    Out.CloudScatter = NonNegative(M.CloudScatter);
    Out.StormScatter = NonNegative(M.StormScatter);
    Out.CloudExtinction = NonNegative(M.CloudExtinction);
    Out.CloudExtinction.A = 1.0f;
    Out.StormExtinction = NonNegative(M.StormExtinction);

    return Out;
}

/** The active model's field: TR_BuildField's packed pins, in its layout, and
 *  its extinction. ONE PACKER FOR THE MARCH AND THE BAKE, so the two cannot
 *  pack differently. The pins are grouped by the shader's needs, not the
 *  panel's, so most draw on several groups. */
static FTerrestrialFieldParameters PackCloudField(const APlanetAtmosphereActor& A)
{
    const FAtmosphereFieldClock& Clock = A.GetFieldClock();

    const auto ToVector4 = [](const FLinearColor& C) { return FVector4f(C.R, C.G, C.B, C.A); };

    const FAtmosphereModelParams& Model = A.ActiveModel();
    const FCloudShapeParams& Shape = Model.Shape;
    const FCloudCoverageParams& Coverage = Model.Coverage;
    const FCloudTypeParams& Type = Model.Type;
    const FCloudLiftParams& Lift = Model.Lift;
    const FCloudWarpParams& Warp = Model.Warp;
    const FCloudStructureLayerParams& Structure = Model.StructureLayer;
    const FCloudDetailLayerParams& Detail = Model.DetailLayer;
    const FCloudMaterialParams& CloudMaterial = Model.Material;
    const FVector2D& Cascades = Model.SurfaceShadow.CascadeRadii;

    FTerrestrialFieldParameters Out{};

    float DriftAngle, NoisePhase;
    NoiseClock(Clock.Config.Get(), Clock.Time, DriftAngle, NoisePhase);

    // The planet's own spin, half the clock's PlanetaryVorticity times the
    // ratio, wrapped in double like the drift. Negated: turning the sample
    // point back carries the field forward.
    const UFlowSimConfig* SpinConfig = Clock.Config.Get();
    const double Omega = SpinConfig
        ? 0.5 * (double)SpinConfig->GetPlanetaryVorticity() * FMath::Max(Model.Planet.SpinRatio, 0.0f)
        : 0.0;
    const float SpinAngle = (float)FMath::Fmod(-Omega * Clock.SpinTime, 2.0 * UE_DOUBLE_PI);

    // (Stage A2: the base solved from BaseMinTarget on the slab, ResolvedCloudBase.)
    Out.CloudProfile = FVector4f(ResolvedCloudBase(Shape, Lift, A.PlanetType == EPlanetAtmosphereType::GasGiant),
        Shape.CloudThickness, Shape.SurfaceSoftness, Shape.CeilingFalloff);
    Out.CloudCurves = FVector4f(Shape.TopCurve, Shape.BottomCurve, Shape.SlopePerTexel, Warp.WarpStretch);
    // The deep deck's fill, floor and darkening ride in spare slots; the slab
    // reads none of them.
    const bool bDeep = (A.PlanetType == EPlanetAtmosphereType::GasGiant);
    const float DeepFill = bDeep ? Model.Deep.DeepFill : 0.0f;
    const float FloorRelief = bDeep ? Model.Deep.FloorRelief : 0.0f;
    const float Darkening = bDeep ? Model.Deep.Darkening : 0.0f;

    Out.CloudCoverage = FVector4f(Coverage.CloudCover, Coverage.StormPriority, Coverage.CoverageSoftness, DeepFill);
    Out.CloudType = FVector4f(Type.TypeBias, Coverage.CloudFull, Type.TypeTropical, Coverage.CoverageDepthRamp);
    Out.CloudLid = FVector4f(Lift.PressureScale, Coverage.EyeOpenPower, Lift.CeilingPressure, Type.StratusDepth);
    Out.CloudLift = FVector4f(Lift.BaseTropical, Lift.BasePressure, Lift.AltitudeGain, Lift.AltitudeLift);
    Out.CloudMotion = FVector4f(DriftAngle, NoisePhase, Warp.WarpShift, SpinAngle);

    // Each layer's volume flag: a layer without one is left out, since no
    // texture is not neutral noise.
    Out.NoiseLevels = FVector4f(
        Structure.MipBias, Structure.Volume ? 1.0f : 0.0f, Detail.MipBias, Detail.Volume ? 1.0f : 0.0f);

    // The structure layer's spare slots carry the storm's share of cloud type,
    // the genus blend's subsidence and the deep floor's relief.
    Out.StructureSampling = FVector4f(Structure.Scale, Structure.Aspect, Structure.Erosion, Type.TypeStorm);
    Out.StructureWarp = FVector4f(Structure.FlowInherit, Type.Subsidence, FloorRelief, Structure.Breakup);

    Out.DetailSampling = FVector4f(Detail.Scale, Detail.Aspect, Detail.Erosion, Detail.FadeMean);
    // The fade as a start and a length, in planet radii.
    Out.DetailWarp = FVector4f(
        Detail.FlowInherit, Detail.BillowHeight, Detail.FadeNear, FMath::Max(Detail.FadeFar - Detail.FadeNear, 0.0f));
    Out.NoiseStretch = FVector4f(Structure.StretchBias, Structure.StretchFlatten, Detail.StretchBias, Detail.StretchFlatten);

    Out.CloudGenusStratus = ToVector4(Type.Genus.Stratus);
    Out.CloudGenusStratocumulus = ToVector4(Type.Genus.Stratocumulus);
    Out.CloudGenusCumulus = ToVector4(Type.Genus.Cumulus);
    Out.CloudGenusCirrus = ToVector4(Type.Genus.Cirrus);

    // The cloud layer and coverage fray ride in the cascade pin's spare slots.
    Out.ShadowCascades = FVector4f(
        (float)Cascades.X, (float)Cascades.Y, (float)Coverage.CloudLayer, Coverage.CoverageFray);

    Out.CloudResponse = FVector4f(
        Darkening, Type.TypeCurve, CloudMaterial.StormBalance, CloudMaterial.StormBlend);

    const FCloudMaterialParams Material = ResolveCloudMaterial(CloudMaterial);

    Out.CloudExtinction = ToVector4(Material.CloudExtinction);
    Out.StormExtinction = ToVector4(Material.StormExtinction);
    Out.CloudOpticalDepth = FMath::Max(Material.CloudOpticalDepth, 0.0f);

    // The deep material and the floor's softness: the material's depth from
    // fills below the base to atmosphere fractions. The slab reads none of it.
    const FLinearColor DeepExtinction = Model.Deep.Extinction;

    Out.DeepExtinction = FVector4f(FMath::Max(DeepExtinction.R, 0.0f), FMath::Max(DeepExtinction.G, 0.0f),
        FMath::Max(DeepExtinction.B, 0.0f), FMath::Max(DeepExtinction.A, 0.0f));
    Out.DeepMaterial = FVector4f(FMath::Max(Model.Deep.MaterialDepth, 0.0f) * DeepFill,
        FMath::Max(Model.Deep.FloorSoftness, 0.001f), 0.0f, 0.0f);

    return Out;
}

/** A key for the field a shadow map was baked from: every pin but the clock's
 *  slots of CloudMotion (drift, noise phase, spin), which move every frame,
 *  with the radius and height scale the extents follow. PITFALL: a pin added
 *  to FTerrestrialFieldParameters and missing here keeps a stale history. */
static uint32 MakeShadowFieldKey(const FTerrestrialFieldParameters& F, float PlanetRadius, float HeightScale)
{
    const FVector4f Pins[] = {
        F.CloudProfile, F.CloudCurves, F.CloudCoverage, F.CloudType, F.CloudLid, F.CloudLift,
        F.NoiseLevels, F.StructureSampling, F.StructureWarp, F.DetailSampling, F.DetailWarp,
        F.CloudGenusStratus, F.CloudGenusStratocumulus, F.CloudGenusCumulus, F.CloudGenusCirrus,
        F.ShadowCascades, F.CloudResponse, F.CloudExtinction, F.StormExtinction, F.DeepExtinction, F.DeepMaterial,
        F.NoiseStretch,
        FVector4f(F.CloudMotion.Z, F.CloudOpticalDepth, PlanetRadius, HeightScale) };

    return FCrc::MemCrc32(Pins, sizeof(Pins));
}

/** ATMO_SHADOW_NO_DECK in AtmosphereShadowMap.ush: every crossing past any
 *  chord, so an unbaked texel reads as fully lit. */
static constexpr float ShadowNoDeck = 60000.0f;

// -- Stage P1b: the cloud shadow bake's schedule ------------------------------
//
// R1a rebaked one whole level every frame, in turn, whatever had changed: one
// 256-step, 4-subsample march per texel of a slice, 4.6-6.9 ms of GPU a frame at
// the Max preset (Design/Performance.md). P1b spreads each level's bake over
// FramesPerLevel frames, a share of its row blocks a frame, into a staging slice
// that replaces the level's when the last share is in (amortised shadow-map
// updates, as cloud and sky systems spread theirs), and skips a level whose
// field clock, light and camera have not moved.

static TAutoConsoleVariable<int32> CVarShadowBakeAmortize(
    TEXT("r.CloudAtmosphere.ShadowBake.Amortize"), 1,
    TEXT("Stage P1b. 1 (default): each cloud shadow level is rebaked only when its clock, light or camera moved, spread over FramesPerLevel frames. ")
    TEXT("0: the R1a schedule, one whole level every frame in turn, for A/B."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarShadowBakeFramesPerLevel(
    TEXT("r.CloudAtmosphere.ShadowBake.FramesPerLevel"), 4,
    TEXT("Stage P1b. Frames one cloud shadow level's rebake is spread over (1 to 16): the bake's GPU time a frame divides by about this, ")
    TEXT("and each level is refreshed every 3 x this frames while the clouds move. Default 4."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarShadowBakeMinInterval(
    TEXT("r.CloudAtmosphere.ShadowBake.MinInterval"), 0.0f,
    TEXT("Stage P1b. Least world seconds between two rebakes of one cloud shadow level; frames with no level due bake nothing. 0 (default): no floor."),
    ECVF_Default);

static TAutoConsoleVariable<float> CVarShadowBakeCameraTexels(
    TEXT("r.CloudAtmosphere.ShadowBake.CameraTexels"), 1.0f,
    TEXT("Stage P1b. Camera travel, in the level's texels, that makes an inner cloud shadow level due on a still field. Default 1."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarShadowBakeQuad(
    TEXT("r.CloudAtmosphere.ShadowBake.QuadSubsamples"), 1,
    TEXT("Stage P1b2. 1 (default): each cloud shadow texel's four subsamples run on a quad of lanes and are summed in order, ")
    TEXT("four times the threads for the same result, where the platform has wave operations. 0: one thread a texel, for A/B."),
    ECVF_Default);

static TAutoConsoleVariable<int32> CVarShadowBakeLog(
    TEXT("r.CloudAtmosphere.ShadowBake.Log"), 0,
    TEXT("Stage P1b. 1: log each cloud shadow level as its rebake completes, with the seconds since its last one."),
    ECVF_Default);

/** Least light turn that makes a level due on a still field, as 1 - cos: well
 *  under a texel at any cascade (2 degrees drops the history altogether). R2
 *  (J1): above float noise; the cosine 0.99999995 rounded to 1 - 1 ulp, and the
 *  self-dot of an unchanged unit vector fell below it for ~1 direction in 5, so
 *  a still field rebaked every frame. */
static constexpr float ShadowDueLightTurn = 1.0e-6f;   // ~0.08 degrees

void APlanetAtmosphereActor::PrepareShadowTarget()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::PrepareShadowTarget);
    if (!ShadowTarget)
    {
        ShadowTarget = NewObject<UTextureRenderTarget2DArray>(this, TEXT("ShadowTarget"), RF_Transient);
    }

    UTextureRenderTarget2DArray* Target = ShadowTarget;

    // The property's own range, which a Blueprint or a loaded file can bypass.
    const int32 Edge = FMath::Clamp(ShadowResolution, 128, 4096);

    // bCanCreateUAV must be set BEFORE the resource is created, or the texture
    // comes back without UAV support and every dispatch that writes it silently
    // does nothing -- a black target with no warning anywhere.
    const int32 DesiredSlices = AtmoShadowBake::CascadeCount;

    // CLEARED TO THE NO-DECK SENTINEL, so a texel no bake has reached reads lit
    // rather than shadowed.
    const FLinearColor Clear(ShadowNoDeck, ShadowNoDeck, ShadowNoDeck, ShadowNoDeck);

    const bool bMismatch =
        Target->SizeX != Edge ||
        Target->SizeY != Edge ||
        Target->Slices != DesiredSlices ||
        Target->OverrideFormat != PF_FloatRGBA ||
        Target->ClearColor != Clear ||
        !Target->bCanCreateUAV;

    if (bMismatch)
    {
        Target->bCanCreateUAV = true;
        Target->OverrideFormat = PF_FloatRGBA;
        Target->ClearColor = Clear;

        // The map stores depths in atmosphere thicknesses, which run well past 1
        // and reach the no-deck sentinel at 60000. A float format has no sRGB
        // variant, so nothing clamps them.
        Target->Init(Edge, Edge, DesiredSlices, PF_FloatRGBA);
        Target->UpdateResourceImmediate(true);

        bShadowPrimed = false;

        UE_LOG(LogCloudAtmosphere, Log, TEXT("%s: Shadow Target set to %dx%d x %d RGBA16F."),
            *GetName(), Edge, Edge, DesiredSlices);
    }
}

void APlanetAtmosphereActor::PrepareCoverageTarget()
{
    if (!CoverageTarget)
    {
        CoverageTarget = NewObject<UTextureRenderTarget2D>(this, TEXT("CoverageTarget"), RF_Transient);
    }

    UTextureRenderTarget2D* Target = CoverageTarget;

    if (Target->SizeX != 1 || Target->OverrideFormat != PF_R32_FLOAT || !Target->bCanCreateUAV)
    {
        Target->bCanCreateUAV = true;

        // Above any priority: nothing is covered until the first pass runs.
        Target->ClearColor = FLinearColor(2.0f, 0.0f, 0.0f, 0.0f);

        Target->InitCustomFormat(1, 1, PF_R32_FLOAT, true);
        Target->UpdateResourceImmediate(true);
    }
}

void APlanetAtmosphereActor::PrepareTransmittanceTable()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::PrepareTransmittanceTable);
    if (!TransmittanceTable)
    {
        TransmittanceTable = NewObject<UTextureRenderTarget2D>(
            this, TEXT("TransmittanceTable"), RF_Transient);
    }

    UTextureRenderTarget2D* Table = TransmittanceTable;

    // CLAMP ON BOTH AXES. Wrapping the cosine axis blends the straight-up
    // column into the horizon one; wrapping altitude blends the ground row into
    // the top. The march binds a clamped sampler too.
    const bool bMismatch =
        Table->SizeX != AtmosphereTransmittance::Width ||
        Table->SizeY != AtmosphereTransmittance::Height ||
        Table->OverrideFormat != AtmosphereTransmittance::Format ||
        Table->AddressX != TA_Clamp ||
        Table->AddressY != TA_Clamp ||
        !Table->bCanCreateUAV;

    if (bMismatch)
    {
        // bCanCreateUAV before the resource exists, or every dispatch into it
        // silently does nothing.
        Table->bCanCreateUAV = true;
        Table->AddressX = TA_Clamp;
        Table->AddressY = TA_Clamp;
        Table->ClearColor = FLinearColor::Black;

        // Linear gamma: the table holds integrals well past 1.
        Table->InitCustomFormat(AtmosphereTransmittance::Width, AtmosphereTransmittance::Height,
            AtmosphereTransmittance::Format, true);
        Table->UpdateResourceImmediate(true);

        // A recreated table comes back cleared.
        TransmittanceBaked = FAtmosphereTransmittanceParams();
    }
}

void APlanetAtmosphereActor::UpdateTransmittanceTable(float PlanetRadius)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::UpdateTransmittanceTable);
    PrepareTransmittanceTable();

    FTextureRenderTargetResource* TableRes = TransmittanceTable->GameThread_GetRenderTargetResource();

    if (!TableRes)
    {
        return;
    }

    // The values the march binds: the table is keyed to them, and
    // AtmoT_Profile converts them on both sides.
    const FAtmosphereAirParams& AirP = ActiveModel().Air;

    FAtmosphereTransmittanceParams Params;
    Params.PlanetRadius = PlanetRadius;
    Params.AtmosphereRadius = PlanetRadius * (1.0f + ActiveHeightScale());
    Params.ProfilePins = FVector4f(
        AirP.RayleighScaleHeight, AirP.MieScaleHeight, AirP.AbsorptionAltitude, AirP.AbsorptionFalloff);
    Params.TableResource = TableRes;

    if (Params.IsUsable() && !Params.Matches(TransmittanceBaked))
    {
        AtmosphereTransmittance::RequestBake(Params);
        TransmittanceBaked = Params;
    }
}

bool APlanetAtmosphereActor::FillShadowRequest(
    FTerrestrialShadowParams& Params, float PlanetRadius, const FVector& PlanetCenter,
    const FVector& LightDir)
{
    UWorld* World = GetWorld();

    if (!World || !FlowTarget)
    {
        return false;
    }

    PrepareShadowTarget();
    PrepareCoverageTarget();

    Params.FlowResource = FlowTarget->GameThread_GetRenderTargetResource();
    Params.MapResource = ShadowTarget->GameThread_GetRenderTargetResource();
    Params.CoverageResource = CoverageTarget->GameThread_GetRenderTargetResource();

    if (!Params.FlowResource || !Params.MapResource || !Params.CoverageResource)
    {
        return false;
    }

    // -- Frame --------------------------------------------------------------
    //
    // The planet's axes are the rows of WorldToLocal, exactly as the march
    // receives them, so the light and the camera arrive in the frame the field
    // is defined in. Spin is not applied here: the field applies it.

    const FQuat FieldFrame = GetFieldFrame();
    const FVector AxisX = FieldFrame.GetForwardVector();
    const FVector AxisY = FieldFrame.GetRightVector();
    const FVector AxisZ = FieldFrame.GetUpVector();

    auto ToLocal = [&AxisX, &AxisY, &AxisZ](const FVector& V)
        {
            return FVector3f(
                static_cast<float>(FVector::DotProduct(AxisX, V)),
                static_cast<float>(FVector::DotProduct(AxisY, V)),
                static_cast<float>(FVector::DotProduct(AxisZ, V)));
        };

    // THE SAME VECTOR THE MARCH GETS, not a re-derivation of it. Whichever
    // convention Light Direction carries, the map and the march agree about it
    // because they are handed the same value.
    Params.LightDir = ToLocal(LightDir).GetSafeNormal();

    // THE VIEW RENDERED LAST FRAME, which is what makes this work in an editor
    // viewport with no player controller. One frame stale, and a frame of camera
    // motion moves a fade distance by nothing visible.
    FVector CameraWorld = PlanetCenter;

    if (World->ViewLocationsRenderedLastFrame.Num() > 0)
    {
        CameraWorld = World->ViewLocationsRenderedLastFrame[0];
    }

    Params.CameraLocal = ToLocal(CameraWorld - PlanetCenter);

    // NO EXTENT AND NO CENTRE PUSHED. Every cascade derives its half-width from
    // CascadeRadii and its centre from the camera, on both sides, so the level
    // index is the only thing that distinguishes them -- and that comes from the
    // dispatch rather than from here.
    Params.MapSize = FIntPoint(ShadowTarget->SizeX, ShadowTarget->SizeY);
    Params.MapSlices = ShadowTarget->Slices;

    // -- Schedule -----------------------------------------------------------
    //
    // Every level on the first request into a fresh target, which holds
    // nothing yet. After that, R1a's rotation took one whole level a frame, in
    // turn (r.CloudAtmosphere.ShadowBake.Amortize 0). P1b's (1) carries on a
    // level's spread bake if one is under way; otherwise starts the next due
    // level in turn; otherwise bakes nothing, and the request runs only the
    // coverage pass the march reads. NOTHING IS RECORDED HERE: CommitShadowBake
    // does that once the request is taken.
    const int32 LevelCount = AtmoShadowBake::CascadeCount;

    // A fresh target holds nothing to blend from.
    const bool bHasHistory = bShadowPrimed;

    const double Now = World->GetTimeSeconds();

    // -- History ------------------------------------------------------------
    //
    // Each baked level keeps exp(-age / smoothing) of its previous bake, where
    // age is the world time since that bake, the clock that pauses and dilates
    // with the game. Per level, so the fade is the same whatever the frame rate
    // or the rotation's cadence. A light that jumped drops it: the reprojection
    // holds for a light that turns, not for one that teleports.
    auto MakeHistory = [this, bHasHistory, Now](int32 Level, const FVector3f& Light)
        {
            FAtmoShadowHistory History;

            History.LightDir = ShadowBakedLight[Level];
            History.CameraLocal = ShadowBakedCamera[Level];
            History.Weight = 0.0f;

            const bool bLightHeld = FVector3f::DotProduct(Light, ShadowBakedLight[Level])
                >= AtmoShadowBake::HistoryLightCosine;

            if (bHasHistory && bLightHeld && ShadowTemporalSmoothing > 0.0f && ShadowBakeTime[Level] > 0.0)
            {
                const float Age = static_cast<float>(FMath::Max(Now - ShadowBakeTime[Level], 0.0));

                History.Weight = FMath::Min(
                    FMath::Exp(-Age / ShadowTemporalSmoothing), AtmoShadowBake::MaxHistoryWeight);
            }

            return History;
        };

    Params.RowStride = 1;
    Params.RowPhase = 0;
    Params.bStaged = false;
    Params.bResolveStaging = false;
    Params.StagingResource = nullptr;
    Params.bQuadSubsamples = CVarShadowBakeQuad.GetValueOnGameThread() > 0;

    // This frame's light and camera, before a spread bake replaces them with its own.
    const FVector3f LightNow = Params.LightDir;
    const FVector3f CameraNow = Params.CameraLocal;

    if (!bShadowPrimed)
    {
        Params.LevelMask = (1u << LevelCount) - 1u;
    }
    else if (CVarShadowBakeAmortize.GetValueOnGameThread() <= 0)
    {
        Params.LevelMask = 1u << (ShadowLevelCursor % LevelCount);
    }
    else
    {
        // A staging slice remade mid-bake lost the rows already baked: start over.
        const bool bStagingLost = ShadowSpread.Level != INDEX_NONE && PrepareShadowStaging();
        // (R2, J2: and a field that jumped: another flow, its clock gone back, or the sim
        // reset.)
        const bool bFieldJumped = ShadowSpread.Level != INDEX_NONE
            && (ShadowSpread.Flow.Get() != FlowTarget || GetFieldClock().Time < ShadowSpread.ClockTime
                || ShadowSpread.SimSerial != FieldSimSerial);
        if (bFieldJumped)
        {
            ShadowSpread = FShadowSpread();
        }

        if (ShadowSpread.Level != INDEX_NONE && !bStagingLost)
        {
            // Carried on with the light, camera and history it started with, so
            // its row blocks are one bake and the march reads it with those.
            Params.LevelMask = 1u << ShadowSpread.Level;
            Params.RowStride = ShadowSpread.Stride;
            Params.RowPhase = ShadowSpread.Phase;
            Params.LightDir = ShadowSpread.LightDir;
            Params.CameraLocal = ShadowSpread.CameraLocal;
            Params.History[ShadowSpread.Level] = ShadowSpread.History;
        }
        else
        {
            Params.LevelMask = 0u;

            for (int32 Step = 0; Step < LevelCount; ++Step)
            {
                const int32 Level = (ShadowLevelCursor + Step) % LevelCount;

                if (IsShadowLevelDue(Level, Params.LightDir, Params.CameraLocal, PlanetRadius, Now))
                {
                    Params.LevelMask = 1u << Level;
                    break;
                }
            }

            // No more requests than the level has row blocks.
            const int32 RowBlocks = FMath::DivideAndRoundUp(Params.MapSize.Y, AtmoShadowBake::ThreadGroupSize);

            Params.RowStride = FMath::Clamp(CVarShadowBakeFramesPerLevel.GetValueOnGameThread(), 1,
                FMath::Min(16, FMath::Max(RowBlocks, 1)));
        }

        if (Params.LevelMask != 0u && Params.RowStride > 1)
        {
            PrepareShadowStaging();

            Params.StagingResource = ShadowStaging ? ShadowStaging->GameThread_GetRenderTargetResource() : nullptr;
        }

        if (Params.StagingResource)
        {
            Params.bStaged = true;
            Params.bResolveStaging = Params.RowPhase == Params.RowStride - 1;
        }
        else
        {
            // Whole and in place: one level, or no staging slice to spread it into.
            Params.RowStride = 1;
            Params.RowPhase = 0;
            Params.LightDir = LightNow;
            Params.CameraLocal = CameraNow;
        }
    }

    for (int32 Level = 0; Level < LevelCount; ++Level)
    {
        // A spread bake's history was fixed when it started.
        const bool bCarried = Params.bStaged && Params.RowPhase > 0;

        if ((Params.LevelMask & (1u << Level)) != 0 && !bCarried)
        {
            Params.History[Level] = MakeHistory(Level, Params.LightDir);
        }
    }

    // -- Field --------------------------------------------------------------
    //
    // Only what both fields read. Each model's own members come from the
    // packer the march shares, in RequestShadowBake, and the bake derives from
    // them with the same shader functions.

    Params.PlanetRadius = PlanetRadius;
    Params.HeightScale = ActiveHeightScale();

    // -- Extinction ---------------------------------------------------------

    Params.LightExtinctionFraction = ActiveModel().MultipleScattering.LightExtinctionFraction();

    // -- Volumes ------------------------------------------------------------
    //
    // Resources of the properties the march binds, not a second reference to
    // the assets: a compute pass cannot reach a UObject.

    UVolumeTexture* Structure = ActiveStructureVolume();
    UVolumeTexture* Detail = ActiveDetailVolume();

    Params.DetailResource = Detail ? Detail->GetResource() : nullptr;
    Params.StructureResource = Structure ? Structure->GetResource() : nullptr;

    return true;
}

void APlanetAtmosphereActor::RequestShadowBake(
    float PlanetRadius, const FVector& PlanetCenter, const FVector& LightDir)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::RequestShadowBake);
    UWorld* World = GetWorld();

    UFlowSimSubsystem* Sim = World ? World->GetSubsystem<UFlowSimSubsystem>() : nullptr;

    // ONE REQUEST PER FRAME. Initialize and Tick can both push in one frame,
    // and the subsystem would run every request it is handed.
    if (!Sim || ShadowBakeFrame == GFrameCounter)
    {
        return;
    }

    ShadowBakeFrame = GFrameCounter;

    // The map holds the other model's field until every level is rebaked.
    if (ShadowType != PlanetType)
    {
        ShadowType = PlanetType;
        bShadowPrimed = false;
    }

    // A changed field rebakes every level: reprojected, the old field's depths
    // would land at the new field's extents.
    const FTerrestrialFieldParameters Field = PackCloudField(*this);
    const uint32 FieldKey = MakeShadowFieldKey(Field, PlanetRadius, ActiveHeightScale());

    if (FieldKey != ShadowFieldKey)
    {
        ShadowFieldKey = FieldKey;
        bShadowPrimed = false;
    }

    FTerrestrialShadowParams Params;

    if (!FillShadowRequest(Params, PlanetRadius, PlanetCenter, LightDir))
    {
        return;
    }

    Params.bDeepDeck = IsDeepDeck();
    Params.Field = Field;

    if (Sim->RequestShadowBake(Params))
    {
        CommitShadowBake(Params);
    }
}

void APlanetAtmosphereActor::CommitShadowBake(const FTerrestrialShadowParams& Params)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::CommitShadowBake);
    const int32 LevelCount = AtmoShadowBake::CascadeCount;
    const UWorld* World = GetWorld();
    const double Now = World ? World->GetTimeSeconds() : 0.0;

    const uint32 LevelMask = Params.LevelMask;

    // A spread bake's request before its last: only its progress. The level's
    // slice still holds its previous bake, which the march goes on reading with
    // the camera and light recorded for it.
    if (Params.bStaged && !Params.bResolveStaging)
    {
        if (Params.RowPhase == 0)
        {
            ShadowSpread.Level = static_cast<int32>(FMath::CountTrailingZeros(LevelMask));
            ShadowSpread.Stride = Params.RowStride;
            ShadowSpread.LightDir = Params.LightDir;
            ShadowSpread.CameraLocal = Params.CameraLocal;
            ShadowSpread.History = Params.History[ShadowSpread.Level];
            ShadowSpread.Flow = FlowTarget;
            ShadowSpread.ClockTime = GetFieldClock().Time;
            ShadowSpread.SimSerial = FieldSimSerial;
        }

        ShadowSpread.Phase = Params.RowPhase + 1;
        return;
    }

    const FAtmosphereFieldClock& Clock = GetFieldClock();

    for (int32 Level = 0; Level < LevelCount; ++Level)
    {
        if (LevelMask & (1u << Level))
        {
            if (CVarShadowBakeLog.GetValueOnGameThread() > 0 && bShadowPrimed)
            {
                UE_LOG(LogCloudAtmosphere, Log, TEXT("%s: cloud shadow level %d rebaked over %d frame(s), %.3f s after its last (history weight %.2f)."),
                    *GetName(), Level, Params.RowStride, Now - ShadowBakeTime[Level], Params.History[Level].Weight);
            }

            ShadowBakedCamera[Level] = Params.CameraLocal;
            ShadowBakedLight[Level] = Params.LightDir;
            ShadowBakeTime[Level] = Now;
            // (R2, J1: the spin time only where the field spins: a terrestrial
            // planet's ratio is 0, and a kept field's spin clock runs on.)
            ShadowBakedClock[Level] = FVector2D(Clock.Time, ActiveModel().Planet.SpinRatio > 0.0f ? Clock.SpinTime : 0.0);
        }
    }

    // A whole bake, or a spread one's last rows: nothing is under way. The
    // cursor moves past the level baked, so the levels take turns.
    ShadowSpread = FShadowSpread();

    if (!bShadowPrimed)
    {
        ShadowLevelCursor = 0;
    }
    else if (LevelMask != 0u)
    {
        ShadowLevelCursor = (static_cast<int32>(FMath::CountTrailingZeros(LevelMask))
            + static_cast<int32>(FMath::CountBits(LevelMask))) % LevelCount;
    }

    bShadowPrimed = true;
}

bool APlanetAtmosphereActor::IsShadowLevelDue(int32 Level, const FVector3f& LightDir, const FVector3f& CameraLocal,
    float PlanetRadius, double Now) const
{
    if (ShadowBakeTime[Level] <= 0.0)
    {
        return true;
    }

    if (Now - ShadowBakeTime[Level] < FMath::Max(CVarShadowBakeMinInterval.GetValueOnGameThread(), 0.0f))
    {
        return false;
    }

    // The field moves with its clocks: the sim's display time (the flow, the
    // noise's drift and phases) and the spin time (the field's turn).
    const FAtmosphereFieldClock& Clock = GetFieldClock();

    const double SpinKey = ActiveModel().Planet.SpinRatio > 0.0f ? Clock.SpinTime : 0.0;
    if (Clock.Time != ShadowBakedClock[Level].X || SpinKey != ShadowBakedClock[Level].Y)
    {
        return true;
    }

    if (1.0f - FVector3f::DotProduct(LightDir, ShadowBakedLight[Level]) > ShadowDueLightTurn)
    {
        return true;
    }

    // An inner level's window follows the camera, snapped to its texels. Its
    // texel is about the cascade's width over the map's; level 0 is planet-centred.
    if (Level > 0 && ShadowTarget && ShadowTarget->SizeX > 0)
    {
        const FVector2D& Radii = ActiveModel().SurfaceShadow.CascadeRadii;
        const double Radius = (Level == 1) ? Radii.X : FMath::Min(Radii.X, Radii.Y);
        const double Texel = 2.0 * Radius * PlanetRadius / ShadowTarget->SizeX;
        const double Travel = FVector3f::Distance(CameraLocal, ShadowBakedCamera[Level]);

        if (Texel > 0.0 && Travel > Texel * FMath::Max(CVarShadowBakeCameraTexels.GetValueOnGameThread(), 0.0f))
        {
            return true;
        }
    }

    return false;
}

bool APlanetAtmosphereActor::PrepareShadowStaging()
{
    if (!ShadowTarget)
    {
        return false;
    }

    if (!ShadowStaging)
    {
        ShadowStaging = NewObject<UTextureRenderTarget2DArray>(this, TEXT("ShadowStaging"), RF_Transient);
    }

    UTextureRenderTarget2DArray* Staging = ShadowStaging;

    const bool bMismatch =
        Staging->SizeX != ShadowTarget->SizeX ||
        Staging->SizeY != ShadowTarget->SizeY ||
        Staging->Slices != 1 ||
        Staging->OverrideFormat != ShadowTarget->OverrideFormat ||
        !Staging->bCanCreateUAV;

    if (!bMismatch)
    {
        return false;
    }

    // bCanCreateUAV before the resource exists, or every dispatch into it
    // silently does nothing. The format is the map's: the copy into a level's
    // slice is a raw copy.
    Staging->bCanCreateUAV = true;
    Staging->OverrideFormat = ShadowTarget->OverrideFormat;
    Staging->ClearColor = ShadowTarget->ClearColor;
    Staging->Init(ShadowTarget->SizeX, ShadowTarget->SizeY, 1, ShadowTarget->OverrideFormat);
    Staging->UpdateResourceImmediate(true);

    return true;
}

// --------------------------------------------------------------------------
// Compute march
//
// The march's inputs, handed to the view extension. The field comes from the
// packer the shadow bake shares, so the bake lights the field the eye sees.
// --------------------------------------------------------------------------

bool APlanetAtmosphereActor::FillMarchParams(
    FAtmosphereMarchParams& Out, float PlanetRadius, const FVector& PlanetCenter, const FVector& LightDir)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::FillMarchParams);
    const auto ToVector3 = [](const FLinearColor& C) { return FVector3f(C.R, C.G, C.B); };
    const auto ToVector4 = [](const FLinearColor& C) { return FVector4f(C.R, C.G, C.B, C.A); };

    // -- Planet, light, clock -------------------------------------------------

    const FQuat Rotation = GetFieldFrame();
    const FLinearColor Light = LightProduct();

    Out.Model = IsDeepDeck() ? AtmosphereMarchModel::DeepDeck
        : HasClouds() ? AtmosphereMarchModel::Slab : AtmosphereMarchModel::AirOnly;
    Out.PlanetCenter = PlanetCenter;
    Out.PlanetRotation = FVector4f(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W);
    Out.LightDirection = FVector3f(LightDir);
    Out.LightColor = ToVector3(Light);
    Out.PlanetRadius = PlanetRadius;
    Out.HeightScale = ActiveHeightScale();

    // -- Field ----------------------------------------------------------------

    FAtmosphereModelParams& Model = ModelOf(PlanetType);

    if (HasClouds())
    {
        SolveFieldBounds(Model.Shape, Model.Lift, PlanetType == EPlanetAtmosphereType::GasGiant);
        Out.Field = PackCloudField(*this);
    }

    const FCloudMaterialParams Material = ResolveCloudMaterial(Model.Material);

    Out.CloudScatter = ToVector3(Material.CloudScatter);
    Out.StormScatter = ToVector3(Material.StormScatter);
    Out.DeepScatter = ToVector3(Model.Deep.Scatter.GetClamped(0.0f, MAX_flt));

    // -- Air, ambient, lighting -----------------------------------------------

    const FAtmosphereAirParams& AirP = Model.Air;
    const FAtmosphereAmbientParams& AmbientP = Model.Ambient;
    const FAtmospherePhaseParams& PhaseP = Model.Phase;
    const FAtmosphereMultipleScatteringParams& MS = Model.MultipleScattering;

    Out.RayleighBeta = ToVector3(AirP.RayleighBeta());
    Out.RayleighScaleHeight = AirP.RayleighScaleHeight;
    Out.MieBeta = ToVector3(AirP.MieBeta());
    Out.MieScaleHeight = AirP.MieScaleHeight;
    // THE GUARDS ARE SINGULARITIES ONLY: the phase function at |g| 1, a lobe
    // growing behind cloud, a terminator of zero width, mixing weights outside
    // [0, 1]. The authored ranges are the parameter reference's, not the push's.
    Out.MieG = FMath::Clamp(AirP.MieG, -0.99f, 0.99f);
    Out.MieLobeDecay = FMath::Max(AirP.MieLobeDecay, 0.0f);
    Out.AbsorptionBeta = ToVector3(AirP.AbsorptionBeta());
    Out.AbsorptionAltitude = AirP.AbsorptionAltitude;
    Out.AbsorptionFalloff = AirP.AbsorptionFalloff;

    // The air's ambient is a ratio of the light, as the cloud's is in the march.
    // NON-NEGATIVE, as LightProduct is: negative radiance makes the temporal
    // resolve's compression singular.
    const FLinearColor AirAmbient = AmbientP.AirAmbient.GetClamped(0.0f, MAX_flt);
    const FLinearColor CloudAmbient = AmbientP.CloudAmbient.GetClamped(0.0f, MAX_flt);

    Out.AtmosphereAmbient = FVector3f(AirAmbient.R * Light.R, AirAmbient.G * Light.G, AirAmbient.B * Light.B);
    Out.AtmosphereAmbientFloor = AmbientP.AirAmbientFloor;
    Out.CloudAmbient = ToVector3(CloudAmbient);
    Out.CloudAmbientFloor = AmbientP.CloudAmbientFloor;
    Out.AmbientTerminator = FMath::Max(AmbientP.AmbientTerminator, 1e-4f);

    Out.ForwardG = FMath::Clamp(PhaseP.ForwardG, -0.99f, 0.99f);
    Out.BackwardG = FMath::Clamp(PhaseP.BackwardG, -0.99f, 0.99f);
    Out.ForwardWeight = FMath::Clamp(PhaseP.ForwardWeight, 0.0f, 1.0f);

    Out.LightExtinctionFraction = MS.LightExtinctionFraction();
    Out.OctaveCount = static_cast<float>(MS.OctaveCount);
    Out.OctaveAttenuation = MS.OctaveAttenuation();
    Out.OctaveEccentricity = MS.OctaveEccentricity();

    // -- Pipeline and shadows -------------------------------------------------

    Out.AtmosphereSteps = static_cast<float>(Raymarch.AtmosphereSteps);
    Out.CloudSteps = static_cast<float>(Raymarch.CloudSteps);
    Out.ChordSpread = Raymarch.ChordSpread;
    Out.SurfaceShadow = ToVector4(Model.SurfaceShadow.Pack());

    // THE CAMERAS EACH LEVEL WAS BAKED AROUND, NOT RE-DERIVED. A fine cascade's
    // centre is snapped to its own texel grid from that camera, so the reader
    // has to snap from the same one. Any other -- this frame's, against a level
    // baked frames ago -- can land a whole texel out, which is a jump in where
    // the map sits, not a smooth disagreement. Level 0 is planet-centred.
    Out.ShadowCamera1 = ShadowBakedCamera[1];
    Out.ShadowCamera2 = ShadowBakedCamera[2];

    for (int32 Level = 0; Level < AtmoShadowBake::CascadeCount; ++Level)
    {
        Out.ShadowLight[Level] = ShadowBakedLight[Level];
    }

    // -- Sampling -------------------------------------------------------------

    Out.CellSize = Sampling.CellSize;
    Out.FreshWeight = Sampling.FreshWeight;
    Out.LatticeGrowth = Sampling.LatticeGrowth;

    // -- Resources ------------------------------------------------------------

    UTexture* Noise = BlueNoise;

    if (!Noise && !bWarnedBlueNoise)
    {
        bWarnedBlueNoise = true;

        UE_LOG(LogCloudAtmosphere, Warning,
            TEXT("%s: no Blue Noise texture set. The atmosphere does not draw until one is."),
            *GetName());
    }

    bWarnedBlueNoise = bWarnedBlueNoise && !Noise;

    const bool bNoStructure = HasClouds() && !ActiveStructureVolume();

    if (bNoStructure && !bWarnedStructureVolume)
    {
        bWarnedStructureVolume = true;

        UE_LOG(LogCloudAtmosphere, Warning,
            TEXT("%s: no structure noise volume for the %s model. Its clouds draw as smooth slabs until one is set."),
            *GetName(), IsDeepDeck() ? TEXT("gas giant") : TEXT("terrestrial"));
    }

    bWarnedStructureVolume = bWarnedStructureVolume && bNoStructure;

    Out.FlowResource = FlowTarget ? FlowTarget->GameThread_GetRenderTargetResource() : nullptr;
    Out.ShadowResource = ShadowTarget ? ShadowTarget->GameThread_GetRenderTargetResource() : nullptr;
    Out.TransmittanceResource = TransmittanceTable ? TransmittanceTable->GameThread_GetRenderTargetResource() : nullptr;
    Out.CoverageResource = CoverageTarget ? CoverageTarget->GameThread_GetRenderTargetResource() : nullptr;
    Out.StructureResource = ActiveStructureVolume() ? ActiveStructureVolume()->GetResource() : nullptr;
    Out.DetailResource = ActiveDetailVolume() ? ActiveDetailVolume()->GetResource() : nullptr;
    Out.BlueNoiseResource = Noise ? Noise->GetResource() : nullptr;

    return Out.IsUsable();
}

void APlanetAtmosphereActor::UpdateComputeMarch(
    float PlanetRadius, const FVector& PlanetCenter, const FVector& LightDir)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::UpdateComputeMarch);
    FAtmosphereMarchParams Params;

    bool bWant = FillMarchParams(Params, PlanetRadius, PlanetCenter, LightDir);

    // CREATED ON FIRST USE, so an atmosphere that never draws registers nothing,
    // and one released by a delete comes back on undo.
    if (bWant && !ViewExtension && GetWorld())
    {
        ViewExtension = FSceneViewExtensions::NewExtension<FAtmosphereViewExtension>(GetWorld());
    }

    bWant = bWant && ViewExtension.IsValid();

    // R2 (J3): the climate's read-back whether or not the march draws: it reads the sim, not the
    // march, and the planet's build waits on it while this atmosphere drives the sim (a march
    // that can't draw, e.g. its blue noise cleared, stalled the terrain's build for good).
    UpdateClimateReadback();

    if (!ViewExtension)
    {
        return;
    }

    // The frame first, so the first enabled render has one to draw.
    if (bWant)
    {
        ViewExtension->SetFrame_GameThread(Params);
        UpdateWeatherReadback(Params);
    }

    ViewExtension->SetEnabled(bWant);
}

void APlanetAtmosphereActor::UpdateWeatherReadback(const FAtmosphereMarchParams& March)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::UpdateWeatherReadback);
    // Only while someone reads it (the actor holds one reference), with a field to read.
    if (WeatherReadbackSeconds <= 0.0f || WeatherFeed.GetSharedReferenceCount() <= 1
        || March.Model == AtmosphereMarchModel::AirOnly || !March.FlowResource)
    {
        return;
    }

    if (!WeatherReadback.IsValid())
    {
        WeatherReadback = MakeShared<FAtmosphereWeatherReadback, ESPMode::ThreadSafe>(WeatherFeed);
    }

    const double Now = FPlatformTime::Seconds();
    const bool bDue = Now - WeatherReadbackTime >= (double)WeatherReadbackSeconds;

    if (bDue)
    {
        WeatherReadbackTime = Now;
    }

    // The field the march draws, at its spin.
    ENQUEUE_RENDER_COMMAND(CloudAtmosphereWeatherReadback)(
        [Readback = WeatherReadback, Flow = March.FlowResource, Spin = March.Field.CloudMotion.W, bDue](FRHICommandListImmediate& RHICmdList)
        {
            TRACE_CPUPROFILER_EVENT_SCOPE_STR("RT CloudAtmosphereWeatherReadback");
            Readback->Poll_RenderThread();

            if (bDue)
            {
                Readback->Request_RenderThread(RHICmdList, Flow, Spin);
            }
        });
}

void APlanetAtmosphereActor::UpdateClimateReadback()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::UpdateClimateReadback);
    // The view of the latest map, on the game thread: only while asked for (nothing reads it but
    // a person in the details panel; a transient texture per map otherwise: R1, ATM-5).
    if (const TSharedPtr<const FAtmosphereClimateMap, ESPMode::ThreadSafe> Map = ClimateFeed->Latest();
        CVarCloudAtmosphereClimateView.GetValueOnGameThread() != 0
        && Map.IsValid() && Map->Serial != ClimateViewSerial && Map->Width > 0 && Map->Height > 0)
    {
        ClimateViewSerial = Map->Serial;

        UTexture2D* Texture = UTexture2D::CreateTransient(Map->Width, Map->Height, PF_FloatRGBA,
            MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), TEXT("AtmosphereClimateView")));

        if (Texture && Texture->GetPlatformData() && Texture->GetPlatformData()->Mips.Num() > 0)
        {
            Texture->SRGB = false;
            Texture->Filter = TF_Bilinear;
            Texture->CompressionSettings = TC_HDR;

            // Flipped to north up, as a map reads.
            FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
            FFloat16Color* Texels = static_cast<FFloat16Color*>(Mip.BulkData.Lock(LOCK_READ_WRITE));

            for (int32 J = 0; J < Map->Height; J++)
            {
                for (int32 I = 0; I < Map->Width; I++)
                {
                    const FVector4f& C = Map->Cells[J * Map->Width + I];
                    Texels[(Map->Height - 1 - J) * Map->Width + I] = FFloat16Color(FLinearColor(C.X, C.Y, C.Z, C.W + 0.5f));
                }
            }

            Mip.BulkData.Unlock();
            Texture->UpdateResource();
            ClimateView = Texture;
        }
    }

    UWorld* World = GetWorld();
    UFlowSimSubsystem* Sim = World ? World->GetSubsystem<UFlowSimSubsystem>() : nullptr;

    // Only the planet driving the sim reads its climate (another's kept map stays).
    if (ClimateReadbackSeconds <= 0.0f || !Sim || !Sim->IsOwner(this) || !Sim->GetSimulation())
    {
        return;
    }

    if (!ClimateReadback.IsValid())
    {
        ClimateReadback = MakeShared<FAtmosphereClimateReadback, ESPMode::ThreadSafe>(ClimateFeed);
    }

    const double Now = FPlatformTime::Seconds();
    bool bDue = false;
    bool bSnapshot = false;

    // Stage T5b: with a climate window (the running state's: the config's flag applies at a
    // reset), one read-back as each window ends (the means are held after it, so later copies
    // would only repeat it): each reset's window once, by its serial; else every
    // ClimateReadbackSeconds.
    if (Sim->HasClimateWindow())
    {
        if (Sim->IsClimateSnapshotReady() && Sim->GetClimateWindowSerial() != ClimateSnapshotSerialRead)
        {
            ClimateSnapshotSerialRead = Sim->GetClimateWindowSerial();
            bDue = true;
            bSnapshot = true;
        }
    }
    else
    {
        bDue = Now - ClimateReadbackTime >= (double)ClimateReadbackSeconds;
    }

    if (bDue)
    {
        ClimateReadbackTime = Now;
    }

    // Stage T5a: the means' age goes with the copy (a reader waits for a settled climate).
    const float Turnovers = (float)Sim->GetClimateTurnovers();
    const bool bSpinningUp = Sim->IsSpinningUp();

    ENQUEUE_RENDER_COMMAND(CloudAtmosphereClimateReadback)(
        [Readback = ClimateReadback, Simulation = Sim->GetSimulation(), bDue, Turnovers, bSpinningUp, bSnapshot, Now](FRHICommandListImmediate& RHICmdList)
        {
            TRACE_CPUPROFILER_EVENT_SCOPE_STR("RT CloudAtmosphereClimateReadback");
            Readback->Poll_RenderThread();

            if (bDue)
            {
                Readback->Request_RenderThread(RHICmdList, Simulation, Turnovers, bSpinningUp, bSnapshot, Now);
            }
        });
}

bool APlanetAtmosphereActor::DrivesSimulation() const
{
    const UWorld* World = GetWorld();
    const UFlowSimSubsystem* Sim = World ? World->GetSubsystem<UFlowSimSubsystem>() : nullptr;

    return Sim && Sim->IsOwner(this);
}

void APlanetAtmosphereActor::RestartSimulation()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::RestartSimulation);
    UWorld* World = GetWorld();
    UFlowSimSubsystem* Sim = World ? World->GetSubsystem<UFlowSimSubsystem>() : nullptr;

    if (Sim && Sim->IsOwner(this))
    {
        UFlowSimConfig* Running = Sim->GetConfig();
        // A config change pending (the seed regenerated this tick): the claim's resolve restarts
        // the sim on the new config anyway; a reset of the outgoing one would be a second spin-up
        // start and InitialState upload for nothing (R1, ATM-2).
        if (Running && Running != ActiveSimConfig())
        {
            // (R2, K2: but the outgoing config's window is over for the planet now.)
            Sim->InvalidateClimateWindow();
            return;
        }
        // The surface the restart is for (a new SetSurfaceMap), on the running config now: the
        // claim that would put it there runs only at this actor's next tick, after the window's
        // first steps (stage A9's rule: an own config only).
        if (Running && Running->GetOuter() == this)
        {
            Running->SurfaceMap = (&ActiveModel() == &GasGiant) ? nullptr : SurfaceMap.Get();
        }

        Sim->ResetSimulation();
    }
}

bool APlanetAtmosphereActor::BidsForSimulation() const
{
    return bAtmosphereActive && HasClouds() && Simulation.bClaimSimulation && ActiveSimConfig() != nullptr;
}

bool APlanetAtmosphereActor::CanProvideClimateSnapshot() const
{
    const UWorld* World = GetWorld();
    const UFlowSimSubsystem* Sim = World ? World->GetSubsystem<UFlowSimSubsystem>() : nullptr;

    if (!Sim || !Sim->IsOwner(this) || ClimateReadbackSeconds <= 0.0f || !Sim->IsAdvancing())
    {
        return false;
    }

    // The running state's window, or the next reset's (the config's flag).
    const UFlowSimConfig* Running = Sim->GetConfig();
    return Sim->HasClimateWindow() || (Running && Running->bClimateSnapshot);
}

void APlanetAtmosphereActor::ReleaseViewExtension()
{
    // Stage A11: and the climate's.
    if (ClimateReadback.IsValid())
    {
        ENQUEUE_RENDER_COMMAND(CloudAtmosphereReleaseClimateReadback)(
            [Readback = MoveTemp(ClimateReadback)](FRHICommandListImmediate&) mutable
            {
                Readback.Reset();
            });
    }

    // Stage A5: the weather's read-backs in flight go on the render thread too.
    if (WeatherReadback.IsValid())
    {
        ENQUEUE_RENDER_COMMAND(CloudAtmosphereReleaseWeatherReadback)(
            [Readback = MoveTemp(WeatherReadback)](FRHICommandListImmediate&) mutable
            {
                Readback.Reset();
            });
    }

    if (!ViewExtension)
    {
        return;
    }

    ViewExtension->SetEnabled(false);

    // THE LAST REFERENCE GOES ON THE RENDER THREAD, with the pooled histories
    // it holds. A view family still rendering keeps its own until it is done.
    ENQUEUE_RENDER_COMMAND(CloudAtmosphereReleaseViewExtension)(
        [Extension = MoveTemp(ViewExtension)](FRHICommandListImmediate&) mutable
        {
            Extension->ReleaseHistories_RenderThread();
            Extension.Reset();
        });

    ViewExtension.Reset();
}

// --------------------------------------------------------------------------
// Flow simulation
// --------------------------------------------------------------------------

const FAtmosphereModelParams& APlanetAtmosphereActor::ModelOf(EPlanetAtmosphereType InModel) const
{
    switch (InModel)
    {
    case EPlanetAtmosphereType::Terrestrial:
    case EPlanetAtmosphereType::AirOnly:
        return Terrestrial;
    case EPlanetAtmosphereType::GasGiant:
        return GasGiant;
    }

    checkNoEntry();
    return Terrestrial;
}

FAtmosphereModelParams& APlanetAtmosphereActor::ModelOf(EPlanetAtmosphereType InModel)
{
    return const_cast<FAtmosphereModelParams&>(static_cast<const APlanetAtmosphereActor*>(this)->ModelOf(InModel));
}

TObjectPtr<UFlowSimConfig>& APlanetAtmosphereActor::SimConfigSlot(EPlanetAtmosphereType InModel)
{
    return (&ModelOf(InModel) == &GasGiant) ? Simulation.GasGiantConfig : Simulation.TerrestrialConfig;
}

void APlanetAtmosphereActor::SyncModelFlags()
{
    Terrestrial.bClouds = HasClouds();
    Terrestrial.bDeepDeck = false;
    GasGiant.bClouds = true;
    GasGiant.bDeepDeck = true;
}

void APlanetAtmosphereActor::ReleaseCloudResources()
{
    ReleaseSimulation(true);

    // THE TARGETS ARE KEPT, for the clouds' return. PITFALL: dropped and
    // recreated under the same name before GC, NewObject replaces the live
    // object in place and stalls the game thread on its render resource.
    bShadowPrimed = false;
}

void APlanetAtmosphereActor::SetPlanetType(EPlanetAtmosphereType InType)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::SetPlanetType);
#if WITH_EDITOR
    Modify();
#endif

    PlanetType = InType;
    SyncModelFlags();
}

/** One graphics preset: the Advanced Graphics values it writes. */
struct FAtmosphereGraphicsPresetRow
{
    int32 CellSize;
    float FreshWeight;
    int32 AtmosphereSteps;
    int32 CloudSteps;
    float ChordSpread;
    float LatticeGrowth;
    int32 ShadowResolution;
};

/** In EAtmosphereGraphicsPreset's order, Max to Min. */
static constexpr FAtmosphereGraphicsPresetRow GraphicsPresetRows[] = {
    { 2, 0.50f, 32, 256, 8.0f, 0.050f, 256 },
    { 3, 0.33f, 32, 256, 8.0f, 0.075f, 256 },
    { 4, 0.25f, 32, 256, 8.0f, 0.100f, 256 },
    { 5, 0.20f, 32, 256, 8.0f, 0.125f, 128 },
    { 6, 0.18f, 16, 128, 8.0f, 0.150f, 128 },
};

static_assert(static_cast<int32>(UE_ARRAY_COUNT(GraphicsPresetRows)) == static_cast<int32>(EAtmosphereGraphicsPreset::Min) + 1,
    "One row per graphics preset.");

void APlanetAtmosphereActor::ApplyGraphicsPreset()
{
#if WITH_EDITOR
    Modify();
#endif

    ApplyGraphicsPresetValues();
}

void APlanetAtmosphereActor::ApplyGraphicsPresetValues()
{
    const FAtmosphereGraphicsPresetRow& Row = GraphicsPresetRows[FMath::Clamp(
        static_cast<int32>(GraphicsPreset), 0, static_cast<int32>(UE_ARRAY_COUNT(GraphicsPresetRows)) - 1)];

    Sampling.CellSize = Row.CellSize;
    Sampling.FreshWeight = Row.FreshWeight;
    Sampling.LatticeGrowth = Row.LatticeGrowth;
    Raymarch.AtmosphereSteps = Row.AtmosphereSteps;
    Raymarch.CloudSteps = Row.CloudSteps;
    Raymarch.ChordSpread = Row.ChordSpread;
    ShadowResolution = Row.ShadowResolution;
}

void APlanetAtmosphereActor::SetGraphicsPreset(EAtmosphereGraphicsPreset InPreset)
{
#if WITH_EDITOR
    Modify();
#endif

    GraphicsPreset = InPreset;
    ApplyGraphicsPreset();
}

FAtmosphereModelParams APlanetAtmosphereActor::GetModelParams(EPlanetAtmosphereType InModel) const
{
    return ModelOf(InModel);
}

void APlanetAtmosphereActor::SetModelParams(EPlanetAtmosphereType InModel, const FAtmosphereModelParams& InParams)
{
#if WITH_EDITOR
    Modify();
#endif

    ModelOf(InModel) = InParams;
    SyncModelFlags();
}

UFlowSimConfig* APlanetAtmosphereActor::GetWritableSimConfig(EPlanetAtmosphereType InModel)
{
    TObjectPtr<UFlowSimConfig>& Slot = SimConfigSlot(InModel);

    UFlowSimConfig* Shared = Slot.Get();
    UWorld* World = GetWorld();

    if (!Shared || !World || !World->IsGameWorld() || Shared->GetOuter() == this)
    {
        return Shared;
    }

    // The other slot keeps the shared asset when both name it.
    UFlowSimConfig* Copy = DuplicateObject<UFlowSimConfig>(Shared, this,
        MakeUniqueObjectName(this, UFlowSimConfig::StaticClass(), Shared->GetFName()));
    Copy->SetFlags(RF_Transient);
    Slot = Copy;

    // The running sim carries on under the copy only if it is the active
    // model's: another slot's copy is not what this actor bids with.
    UFlowSimSubsystem* Sim = World->GetSubsystem<UFlowSimSubsystem>();

    if (Sim && &Slot == &SimConfigSlot(PlanetType))
    {
        Sim->AdoptConfig(this, Shared, Copy);
    }

    return Copy;
}

UFlowSimConfig* APlanetAtmosphereActor::ActiveSimConfig() const
{
    return (&ActiveModel() == &GasGiant) ? Simulation.GasGiantConfig.Get() : Simulation.TerrestrialConfig.Get();
}

void APlanetAtmosphereActor::ClaimSimulation(const FVector& PlanetCenter, float PlanetRadius)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::ClaimSimulation);
    UWorld* World = GetWorld();
    UFlowSimSubsystem* Sim = World ? World->GetSubsystem<UFlowSimSubsystem>() : nullptr;

    if (!Sim)
    {
        return;
    }

    UFlowSimConfig* Config = ActiveSimConfig();

    // Stage A9: THE SURFACE ON THE TERRESTRIAL SIM, on this actor's own config (a generated one,
    // or a game world's copy; a shared asset in the editor is left alone): set before the bid, so
    // a new config starts with it, and live on the running one.
    if (Config)
    {
        const bool bGasGiant = (&ActiveModel() == &GasGiant);
        UTexture2D* Want = bGasGiant ? nullptr : SurfaceMap.Get();

        if (Config->SurfaceMap != Want)
        {
            UFlowSimConfig* Own = (Config->GetOuter() == this) ? Config
                : GetWritableSimConfig(bGasGiant ? EPlanetAtmosphereType::GasGiant : EPlanetAtmosphereType::Terrestrial);

            if (Own && Own->GetOuter() == this)
            {
                Own->SurfaceMap = Want;
                Config = Own;
            }
        }
    }

    if (!Config && Simulation.bClaimSimulation && !bWarnedSimConfig)
    {
        bWarnedSimConfig = true;

        UE_LOG(LogCloudAtmosphere, Warning,
            TEXT("%s: no sim config for the %s model. The clouds draw the kept field, or none."),
            *GetName(), IsDeepDeck() ? TEXT("gas giant") : TEXT("terrestrial"));
    }

    bWarnedSimConfig = bWarnedSimConfig && !Config;

    if (!KeptFlow)
    {
        KeptFlow = NewObject<UTextureRenderTarget2DArray>(this, TEXT("KeptFlow"), RF_Transient);
    }

    if (Simulation.bClaimSimulation && Config)
    {
        // NEAREST BY THE CAMERA'S HEIGHT ABOVE THE PLANET, from the view rendered
        // last frame, which covers editor viewports too. No bid without a view:
        // every planet would tie, and the first frame would hand the sim to an
        // arbitrary one.
        if (World->ViewLocationsRenderedLastFrame.Num() > 0)
        {
            const FVector Camera = World->ViewLocationsRenderedLastFrame[0];
            const double Height = FMath::Max(FVector::Dist(Camera, PlanetCenter) - PlanetRadius, 0.0);

            Sim->ClaimSimulation(this, Config, Height, KeptFlow);
            bClaimedSimulation = true;
        }
    }
    else
    {
        ReleaseSimulation(true);
    }

    // -- What this frame draws ------------------------------------------------
    //
    // Ownership was settled as the frame started, before any actor ticked, so
    // this frame's march and bake read one field whichever it is.

    const double Now = World->GetTimeSeconds();

    if (Sim->IsOwner(this) && Sim->GetFlowTarget())
    {
        FlowTarget = Sim->GetFlowTarget();
        SimDebugView = Sim->GetDebugTarget();
        FieldSimSerial = Sim->GetClimateWindowSerial();

        FieldClock.Time = Sim->GetDisplayTime();
        FieldClock.SpinTime = FieldClock.Time;
        FieldClock.Config = Sim->GetConfig();
        return;
    }

    // A KEPT FIELD HOLDS STILL AND TURNS WITH ITS PLANET: its noise clocks stay
    // where it was copied, and its spin runs on at the sim's rate. The clock
    // comes from the subsystem, recorded with the copy.
    double KeptTime = 0.0;
    UFlowSimConfig* KeptConfig = nullptr;

    if (Sim->TakeKeptClock(this, KeptTime, KeptConfig))
    {
        KeptClock.Time = KeptTime;
        KeptClock.Config = KeptConfig;
        KeptAt = Now;
    }
    else if (KeptFlow->Slices < 4)
    {
        // Never driven: a cleared atlas at the active config's size, which
        // reads as no weather.
        const int32 Resolution = Config ? Config->GridResolution : 64;
        const int32 KeptLayers = Config ? FMath::Clamp(Config->LayerCount, 1, 8) : 2;
        const FIntPoint Atlas = FlowSimShader::AtlasSize(FlowSimShader::GridResolution(Resolution));

        KeptFlow->OverrideFormat = PF_FloatRGBA;
        KeptFlow->ClearColor = FLinearColor::Black;
        KeptFlow->Init(Atlas.X, Atlas.Y, 4 * KeptLayers, PF_FloatRGBA);
        KeptFlow->UpdateResourceImmediate(true);

        KeptClock = FAtmosphereFieldClock();
        KeptClock.Config = Config;
        KeptAt = Now;
    }

    const UFlowSimConfig* SpinConfig = KeptClock.Config.Get();
    const double SpinRate = SpinConfig ? (double)FMath::Max(SpinConfig->SimSpeed, 0.0f) : 0.0;

    FlowTarget = KeptFlow;
    SimDebugView = nullptr;

    FieldClock = KeptClock;
    FieldClock.SpinTime = KeptClock.Time + (Now - KeptAt) * SpinRate;
}

void APlanetAtmosphereActor::SetSurfaceMap(int32 Width, int32 Height, TConstArrayView<FVector2f> Cells)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(APlanetAtmosphereActor::SetSurfaceMap);
    check(IsInGameThread());

    if (Width <= 0 || Height <= 0 || Cells.Num() != Width * Height)
    {
        SurfaceMap = nullptr;
        return;
    }

    // Half floats, elevation in R and land in G; bilinear, wrapped in longitude and clamped at
    // the poles by the sim's own sampler.
    UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_FloatRGBA,
        MakeUniqueObjectName(GetTransientPackage(), UTexture2D::StaticClass(), TEXT("AtmosphereSurfaceMap")));

    if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() == 0)
    {
        SurfaceMap = nullptr;
        return;
    }

    Texture->SRGB = false;
    Texture->Filter = TF_Bilinear;
    Texture->AddressX = TA_Wrap;
    Texture->AddressY = TA_Clamp;
    Texture->CompressionSettings = TC_HDR;

    FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
    FFloat16Color* Texels = static_cast<FFloat16Color*>(Mip.BulkData.Lock(LOCK_READ_WRITE));

    for (int32 Index = 0; Index < Cells.Num(); Index++)
    {
        Texels[Index] = FFloat16Color(FLinearColor(
            FMath::Clamp(Cells[Index].X, 0.0f, 1.0f), FMath::Clamp(Cells[Index].Y, 0.0f, 1.0f), 0.0f, 1.0f));
    }

    Mip.BulkData.Unlock();
    Texture->UpdateResource();

    SurfaceMap = Texture;
}

// --------------------------------------------------------------------------
// Light update
// --------------------------------------------------------------------------

void APlanetAtmosphereActor::OrientToStar(const FVector& StarWorldPos)
{
    // RELATIVE, because the light reads the relative rotation as a world
    // direction: a world rotation under a rotated planet would aim it off by
    // the planet's own rotation.
    //
    // PITFALL: the field's frame is the planet's (GetFieldFrame), never this
    // rotation. Taken from the actor, aiming at a moving star turns the clouds
    // and their spin axis with the light.
    const FVector ToStar = StarWorldPos - GetActorLocation();
    if (ToStar.IsNearlyZero()) return;
    SetActorRelativeRotation(ToStar.Rotation());
    UpdateLightFromRotation();
}

FQuat APlanetAtmosphereActor::GetFieldFrame() const
{
    const USceneComponent* Root = GetRootComponent();
    const USceneComponent* Parent = Root ? Root->GetAttachParent() : nullptr;

    return Parent ? Parent->GetComponentQuat() : FQuat::Identity;
}

void APlanetAtmosphereActor::UpdateLightFromRotation()
{
    UDirectionalLightComponent* LightComp = SunLightComponent;
    if (!LightComp) return;

    // Directional light faces opposite the light direction vector
    const FVector LightDir = GetRootComponent()->GetRelativeRotation().Vector();
    LightComp->SetWorldRotation((-LightDir).Rotation());

    // The march's light as a unit colour and its length.
    const FLinearColor Light = LightProduct();
    const float Magnitude = FVector(Light.R, Light.G, Light.B).Size();

    if (Magnitude > KINDA_SMALL_NUMBER)
    {
        const FLinearColor NormalizedColor(
            Light.R / Magnitude,
            Light.G / Magnitude,
            Light.B / Magnitude, 1.0f);
        LightComp->SetLightColor(NormalizedColor);
        LightComp->SetIntensity(Magnitude);
    }
    else
    {
        LightComp->SetLightColor(FLinearColor::White);
        LightComp->SetIntensity(0.0f);
    }
}

void APlanetAtmosphereActor::SetAtmosphereActive(bool bActive)
{
    // The map stopped following the clouds while parked.
    if (bActive && !bAtmosphereActive)
    {
        bShadowPrimed = false;
    }

    bAtmosphereActive = bActive;

    // Tick stops the parameter push, the bake and the transmittance update.
    // Woken, the next tick pushes a fresh frame and enables the extension;
    // enabled here it would draw the frame captured before parking.
    if (ViewExtension && !bActive)
    {
        ViewExtension->SetEnabled(false);
    }

    // A parked planet gives up the sim, keeping its field; waking restarts the
    // sim from its config's snapshot.
    if (!bActive)
    {
        ReleaseSimulation(true);
    }

    if (SunLightComponent)
    {
        SunLightComponent->SetVisibility(bActive);
    }
}