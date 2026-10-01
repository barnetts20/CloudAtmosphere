#include "PlanetAtmosphereActor.h"
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
#include "RenderingThread.h"
#include "SceneViewExtension.h"
#include "TerrestrialShadowMap.h"
#include "FlowSimShaders.h"
#include "FlowSimSubsystem.h"
#include "FlowSimTypes.h"

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
    TerrestrialMultipleScattering.SunlightPenetration = 0.0f;

    // SIZED TO BE LOOKED THROUGH FROM UNDERNEATH. Every beta is per atmosphere
    // thickness and every scale height a fraction of it, so a column's optical
    // depth is beta times that fraction and does not move with HeightScale:
    // these land the vertical column near a quarter of one optical depth, where
    // the struct defaults close it at about six.
    //
    // The absorber is the term to watch. It is a Lorentzian scaled by the
    // Rayleigh profile, so its column integral is roughly half a scale height,
    // and at a beta of 100 it alone closes the sky.
    TerrestrialAir.RayleighDepth = FLinearColor(0.06236388f, 0.1903298f, 0.2996182f, 1.0f);
    TerrestrialAir.RayleighScaleHeight = 0.15f;
    TerrestrialAir.MieDepth = FLinearColor(0.09999546f, 0.08930685f, 0.07558977f, 1.0f);
    TerrestrialAir.MieScaleHeight = 0.1f;
    TerrestrialAir.MieG = 0.95f;
    TerrestrialAir.AbsorptionDepth = FLinearColor(0.04203038f, 0.0339376f, 0.0368092f, 1.0f);

    // NOT NEAR-BLACK. Under a cloud base there is a lit surface bouncing light
    // back up, and this term is the whole of it: left near black, standing under
    // the deck is night.
    TerrestrialAmbient.AirAmbient = FLinearColor(6.666667e-4f, 9.122807e-4f, 1.407407e-3f, 1.0f);
    TerrestrialAmbient.AirAmbientFloor = 0.02f;
    TerrestrialAmbient.CloudAmbient = FLinearColor(0.040f, 0.044f, 0.052f, 1.0f);
    TerrestrialAmbient.CloudAmbientFloor = 0.04f;

    // A cloud shadow lands on lit terrain here rather than on more cloud, so it
    // carries more of the surface's brightness than the deck's does.
    TerrestrialSurfaceShadow.DirectFraction = 0.85f;

    // Default source assets, so the details panel shows something and a fresh
    // actor renders. A field with no volumes is not a subtle failure -- erosion
    // goes to its neutral value and the clouds come out as smooth slabs.
    // THE TYPED CLOUD NOISE, which the genus weights and the detail channels
    // read as noise types.
    static ConstructorHelpers::FObjectFinder<UVolumeTexture> DefaultCloudStructureVolume(
        TEXT("/CloudAtmosphere/Noise/CloudNoise_4_128"));
    if (DefaultCloudStructureVolume.Succeeded())
    {
        TerrestrialStructureLayer.Volume = DefaultCloudStructureVolume.Object;
        GasGiantStructureLayer.Volume = DefaultCloudStructureVolume.Object;
    }
    else
    {
        UE_LOG(LogCloudAtmosphere, Warning, TEXT("PlanetAtmosphereActor: Failed to load default cloud structure volume"));
    }

    static ConstructorHelpers::FObjectFinder<UVolumeTexture> DefaultCloudDetailVolume(
        TEXT("/CloudAtmosphere/Noise/CloudNoise_8_128"));
    if (DefaultCloudDetailVolume.Succeeded())
    {
        TerrestrialDetailLayer.Volume = DefaultCloudDetailVolume.Object;
        GasGiantDetailLayer.Volume = DefaultCloudDetailVolume.Object;
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

    static ConstructorHelpers::FObjectFinder<UFlowSimConfig> DefaultSimConfig(
        TEXT("/CloudAtmosphere/NoiseRecipes/GasGiantSimScratch"));
    if (DefaultSimConfig.Succeeded())
    {
        Simulation.TerrestrialConfig = DefaultSimConfig.Object;
        Simulation.GasGiantConfig = DefaultSimConfig.Object;
    }
    else
    {
        UE_LOG(LogCloudAtmosphere, Warning, TEXT("PlanetAtmosphereActor: Failed to load default sim config"));
    }

    // The gas giant's noise: broad structure stretched tall through the deep
    // deck, and fine detail.
    GasGiantStructureLayer.Scale = 1.5f;
    GasGiantStructureLayer.Aspect = 16.0f;
    GasGiantDetailLayer.Scale = 24.0f;
    GasGiantDetailLayer.Aspect = 8.0f;

    // The gas giant starts from the terrestrial tune.
    Air = TerrestrialAir;
    Ambient = TerrestrialAmbient;
    MultipleScattering = TerrestrialMultipleScattering;
    GasGiantSurfaceShadow = TerrestrialSurfaceShadow;
}

// --------------------------------------------------------------------------
// Lifecycle
// --------------------------------------------------------------------------

void APlanetAtmosphereActor::BeginPlay()
{
    Super::BeginPlay();
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

    if (!bInitialized)
    {
        bPendingInitialize = true;
    }
}

void APlanetAtmosphereActor::Tick(float DeltaTime)
{
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
        // Snap location back -- it follows the planet. Rotation is intentionally left alone
        // (controls light direction). Scale is absolute and planet-driven.
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
        // Lock location and scale. Rotation is intentionally left alone (light direction).
        // Using _Direct setters avoids firing TransformUpdated recursively.
        bool bLocationDirty = !Root->GetRelativeLocation().IsNearlyZero(0.01);
        bool bScaleDirty = !Root->GetComponentScale().Equals(PlanetDrivenScale, 0.01);

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
    bIsPlanetOwned = true;

    // Apply scale BEFORE binding the transform guard -- prevents the old guard
    // from reverting a scale change that the planet actor intends.
    if (USceneComponent* Root = GetRootComponent())
        Root->TransformUpdated.RemoveAll(this);

    if (!InScale.IsZero())
        SetActorScale3D(InScale);

    PlanetDrivenScale = GetActorScale3D();

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
    DestroyLegacyChildActors();

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
    // A saved actor can still hold the post-process component the atmosphere
    // once drew through, by its old subobject name. Unbound and registered, it
    // would enter every view's post-process blend.
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
    const FVector PlanetCenter = GetActorLocation();
    const float PlanetRadius = static_cast<float>(GetActorScale3D().GetMax());

    // Light direction from relative rotation -- treated as world-space direction
    // regardless of parent rotation. The user/gizmo sets relative rotation directly.
    const FVector LightDir = GetRootComponent()->GetRelativeRotation().Vector();

    ClaimSimulation(PlanetCenter, PlanetRadius);
    RequestShadowBake(PlanetRadius, PlanetCenter, LightDir);
    UpdateTransmittanceTable(PlanetRadius);
    UpdateComputeMarch(PlanetRadius, PlanetCenter, LightDir);
}

/** READOUT ONLY, NEVER PUSHED. Mirrors TR_FieldReach, TR_TopMax and
 *  TR_BaseMin, for the active model's shape. A mismatch misreports the
 *  readouts and changes nothing drawn. */
static void SolveFieldBounds(FTerrestrialShapeParams& Shape, const FTerrestrialLiftParams& Lift)
{
    const float D = FMath::Max(Shape.CloudThickness, 1e-4f);

    const float BaseUp = D * (FMath::Max(Lift.BaseTropical, 0.0f)
        + FMath::Abs(Lift.BasePressure) + FMath::Max(Lift.AltitudeLift, 0.0f));

    const float BaseDown = D * (-FMath::Min(Lift.BaseTropical, 0.0f)
        + FMath::Abs(Lift.BasePressure) - FMath::Min(Lift.AltitudeLift, 0.0f));

    const float Depth = D * (1.0f + FMath::Abs(Lift.CeilingPressure));

    Shape.SolvedTopMax = FMath::Min(Shape.CloudBase + BaseUp + Depth, 1.0f);
    Shape.SolvedBaseMin = Shape.CloudBase - BaseDown;
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

/** The cloud material as the view and the bake read it: albedo held to [0, 1],
 *  extinction non-negative, and fair-weather extinction's amount at 1, since
 *  CloudOpticalDepth carries it. */
static FTerrestrialCloudMaterialParams ResolveCloudMaterial(const FTerrestrialCloudMaterialParams& M)
{
    const auto NonNegative = [](const FLinearColor& C)
        {
            return FLinearColor(FMath::Max(C.R, 0.0f), FMath::Max(C.G, 0.0f), FMath::Max(C.B, 0.0f), FMath::Max(C.A, 0.0f));
        };

    FTerrestrialCloudMaterialParams Out = M;

    Out.CloudScatter = M.CloudScatter.GetClamped(0.0f, 1.0f);
    Out.StormScatter = M.StormScatter.GetClamped(0.0f, 1.0f);
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

    const FTerrestrialShapeParams& Shape = A.ActiveShape();
    const FTerrestrialCoverageParams& Coverage = A.ActiveCoverage();
    const FTerrestrialTypeParams& Type = A.ActiveType();
    const FTerrestrialLiftParams& Lift = A.ActiveLift();
    const FTerrestrialWarpParams& Warp = A.ActiveWarp();
    const FTerrestrialStructureLayerParams& Structure = A.ActiveStructureLayer();
    const FTerrestrialDetailLayerParams& Detail = A.ActiveDetailLayer();
    const FTerrestrialCloudMaterialParams& CloudMaterial = A.ActiveCloudMaterial();
    const FVector2D& Cascades = A.ActiveSurfaceShadow().CascadeRadii;

    FTerrestrialFieldParameters Out{};

    float DriftAngle, NoisePhase;
    NoiseClock(Clock.Config.Get(), Clock.Time, DriftAngle, NoisePhase);

    // The planet's own spin, half the clock's PlanetaryVorticity times the
    // ratio, wrapped in double like the drift. Negated: turning the sample
    // point back carries the field forward.
    const UFlowSimConfig* SpinConfig = Clock.Config.Get();
    const double Omega = SpinConfig
        ? 0.5 * (double)FMath::Max(SpinConfig->PlanetaryVorticity, 0.1f) * FMath::Max(A.ActivePlanet().SpinRatio, 0.0f)
        : 0.0;
    const float SpinAngle = (float)FMath::Fmod(-Omega * Clock.SpinTime, 2.0 * UE_DOUBLE_PI);

    // Free slots stay zero.
    Out.CloudProfile = FVector4f(Shape.CloudBase, Shape.CloudThickness, Shape.SurfaceSoftness, Shape.CeilingFalloff);
    Out.CloudCurves = FVector4f(Shape.TopCurve, Shape.BottomCurve, A.ActiveCloudSlope(), Warp.WarpStretch);
    // The deep deck's fill in coverage's spare slot and its floor's relief in
    // the structure layer's; the slab reads neither.
    const bool bDeep = (A.PlanetType == EPlanetAtmosphereType::GasGiant);
    const float DeepFill = bDeep ? A.GasGiantDeep.DeepFill : 0.0f;
    const float FloorRelief = bDeep ? A.GasGiantDeep.FloorRelief : 0.0f;

    Out.CloudCoverage = FVector4f(Coverage.CloudCover, Coverage.StormPriority, Coverage.CoverageSoftness, DeepFill);
    Out.CloudType = FVector4f(Type.TypeBias, 0.0f, Type.TypeTropical, 0.0f);
    Out.CloudLid = FVector4f(Lift.PressureScale, 0.0f, Lift.CeilingPressure, Type.StratusDepth);
    Out.CloudLift = FVector4f(Lift.BaseTropical, Lift.BasePressure, Lift.AltitudeGain, Lift.AltitudeLift);
    Out.CloudMotion = FVector4f(DriftAngle, NoisePhase, Warp.WarpShift, SpinAngle);

    // Each layer's volume flag: a layer without one is left out, since no
    // texture is not neutral noise.
    Out.NoiseLevels = FVector4f(
        Structure.MipBias, Structure.Volume ? 1.0f : 0.0f, Detail.MipBias, Detail.Volume ? 1.0f : 0.0f);

    // The structure layer's spare slots carry the storm's share of cloud type,
    // the genus blend's subsidence and the deep floor's relief.
    Out.StructureSampling = FVector4f(Structure.Scale, Structure.Aspect, Structure.Erosion, Type.TypeStorm);
    Out.StructureWarp = FVector4f(Structure.FlowInherit, Type.Subsidence, FloorRelief, 0.0f);

    Out.DetailSampling = FVector4f(Detail.Scale, Detail.Aspect, Detail.Erosion, Detail.FadeMean);
    // The fade as a start and a length, in planet radii.
    Out.DetailWarp = FVector4f(
        Detail.FlowInherit, 0.0f, Detail.FadeNear, FMath::Max(Detail.FadeFar - Detail.FadeNear, 0.0f));

    Out.CloudGenusStratus = ToVector4(Type.Stratus);
    Out.CloudGenusStratocumulus = ToVector4(Type.Stratocumulus);
    Out.CloudGenusCumulus = ToVector4(Type.Cumulus);
    Out.CloudGenusCirrus = ToVector4(Type.Cirrus);

    // The cloud layer and coverage fray ride in the cascade pin's spare slots.
    Out.ShadowCascades = FVector4f(
        (float)Cascades.X, (float)Cascades.Y, (float)Coverage.CloudLayer, Coverage.CoverageFray);

    Out.CloudResponse = FVector4f(
        0.0f, Type.TypeCurve, CloudMaterial.StormBalance, CloudMaterial.StormBlend);

    const FTerrestrialCloudMaterialParams Material = ResolveCloudMaterial(CloudMaterial);

    Out.CloudExtinction = ToVector4(Material.CloudExtinction);
    Out.StormExtinction = ToVector4(Material.StormExtinction);
    Out.CloudOpticalDepth = FMath::Max(Material.CloudOpticalDepth, 0.0f);

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
        F.ShadowCascades, F.CloudResponse, F.CloudExtinction, F.StormExtinction,
        FVector4f(F.CloudMotion.Z, F.CloudOpticalDepth, PlanetRadius, HeightScale) };

    return FCrc::MemCrc32(Pins, sizeof(Pins));
}

/** ATMO_SHADOW_NO_DECK in AtmosphereShadowMap.ush: every crossing past any
 *  chord, so an unbaked texel reads as fully lit. */
static constexpr float ShadowNoDeck = 60000.0f;

void APlanetAtmosphereActor::PrepareShadowTarget()
{
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

void APlanetAtmosphereActor::PrepareTransmittanceTable()
{
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
    PrepareTransmittanceTable();

    FTextureRenderTargetResource* TableRes = TransmittanceTable->GameThread_GetRenderTargetResource();

    if (!TableRes)
    {
        return;
    }

    // The values the march binds: the table is keyed to them, and
    // AtmoT_Profile converts them on both sides.
    const FAtmosphereAirParams& AirP = ActiveAir();

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

    Params.FlowResource = FlowTarget->GameThread_GetRenderTargetResource();
    Params.MapResource = ShadowTarget->GameThread_GetRenderTargetResource();

    if (!Params.FlowResource || !Params.MapResource)
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

    // -- Rotation -----------------------------------------------------------
    //
    // ShadowLevelsPerFrame levels this request, taken in turn; every level on
    // the first request into a fresh target, which holds nothing yet. NOTHING
    // IS RECORDED HERE: CommitShadowBake does that once the request is taken.
    const int32 LevelCount = AtmoShadowBake::CascadeCount;

    // A fresh target holds nothing to blend from.
    const bool bHasHistory = bShadowPrimed;

    if (!bShadowPrimed)
    {
        Params.LevelMask = (1u << LevelCount) - 1u;
    }
    else
    {
        Params.LevelMask = 0u;

        for (int32 i = 0; i < FMath::Clamp(ShadowLevelsPerFrame, 1, LevelCount); ++i)
        {
            Params.LevelMask |= 1u << ((ShadowLevelCursor + i) % LevelCount);
        }
    }

    // -- History ------------------------------------------------------------
    //
    // Each baked level keeps exp(-age / smoothing) of its previous bake, where
    // age is the time since that bake. Per level and in seconds, so the fade is
    // the same whatever the frame rate or the rotation's cadence. A light that
    // jumped drops it: the reprojection holds for a light that turns, not for
    // one that teleports.
    const double Now = FPlatformTime::Seconds();

    for (int32 Level = 0; Level < LevelCount; ++Level)
    {
        if ((Params.LevelMask & (1u << Level)) == 0)
        {
            continue;
        }

        FAtmoShadowHistory& History = Params.History[Level];

        History.LightDir = ShadowBakedLight[Level];
        History.CameraLocal = ShadowBakedCamera[Level];
        History.Weight = 0.0f;

        const bool bLightHeld = FVector3f::DotProduct(Params.LightDir, ShadowBakedLight[Level])
            >= AtmoShadowBake::HistoryLightCosine;

        if (bHasHistory && bLightHeld && ShadowTemporalSmoothing > 0.0f && ShadowBakeTime[Level] > 0.0)
        {
            const float Age = static_cast<float>(Now - ShadowBakeTime[Level]);

            History.Weight = FMath::Min(
                FMath::Exp(-Age / ShadowTemporalSmoothing), AtmoShadowBake::MaxHistoryWeight);
        }
    }

    // NO EXTENT AND NO CENTRE PUSHED. Every cascade derives its half-width from
    // the fade radii and its centre from the camera, on both sides, so the level
    // index is the only thing that distinguishes them -- and that comes from the
    // dispatch rather than from here.
    Params.MapSize = FIntPoint(ShadowTarget->SizeX, ShadowTarget->SizeY);

    // -- Field --------------------------------------------------------------
    //
    // Only what both fields read. Each model's own members come from the
    // packer the march shares, in RequestShadowBake, and the bake derives from
    // them with the same shader functions.

    Params.PlanetRadius = PlanetRadius;
    Params.HeightScale = ActiveHeightScale();

    // -- Extinction ---------------------------------------------------------

    Params.LightExtinctionFraction = ActiveMultipleScattering().LightExtinctionFraction();

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

    Params.bDeepDeck = !bTerrestrial();
    Params.Field = Field;

    if (Sim->RequestShadowBake(Params))
    {
        CommitShadowBake(Params.LevelMask, Params.LightDir, Params.CameraLocal);
    }
}

void APlanetAtmosphereActor::CommitShadowBake(
    uint32 LevelMask, const FVector3f& LightDir, const FVector3f& CameraLocal)
{
    const int32 LevelCount = AtmoShadowBake::CascadeCount;
    const double Now = FPlatformTime::Seconds();

    for (int32 Level = 0; Level < LevelCount; ++Level)
    {
        if (LevelMask & (1u << Level))
        {
            ShadowBakedCamera[Level] = CameraLocal;
            ShadowBakedLight[Level] = LightDir;
            ShadowBakeTime[Level] = Now;
        }
    }

    ShadowLevelCursor = bShadowPrimed
        ? (ShadowLevelCursor + FMath::CountBits(LevelMask)) % LevelCount
        : 0;

    bShadowPrimed = true;
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
    const auto ToVector3 = [](const FLinearColor& C) { return FVector3f(C.R, C.G, C.B); };
    const auto ToVector4 = [](const FLinearColor& C) { return FVector4f(C.R, C.G, C.B, C.A); };

    // -- Planet, light, clock -------------------------------------------------

    const FQuat Rotation = GetFieldFrame();
    const FLinearColor Light = LightProduct();

    Out.bGasGiant = !bTerrestrial();
    Out.PlanetCenter = PlanetCenter;
    Out.PlanetRotation = FVector4f(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W);
    Out.LightDirection = FVector3f(LightDir);
    Out.LightColor = ToVector3(Light);
    Out.PlanetRadius = PlanetRadius;
    Out.HeightScale = ActiveHeightScale();

    // -- Field ----------------------------------------------------------------

    SolveFieldBounds(
        bTerrestrial() ? TerrestrialShape : GasGiantShape,
        bTerrestrial() ? TerrestrialLift : GasGiantLift);

    Out.Field = PackCloudField(*this);

    const FTerrestrialCloudMaterialParams Material = ResolveCloudMaterial(ActiveCloudMaterial());

    Out.CloudScatter = ToVector3(Material.CloudScatter);
    Out.StormScatter = ToVector3(Material.StormScatter);

    // -- Air, ambient, lighting -----------------------------------------------

    const FAtmosphereAirParams& AirP = ActiveAir();
    const FAtmosphereAmbientParams& AmbientP = ActiveAmbient();
    const FAtmospherePhaseParams& PhaseP = ActivePhase();
    const FAtmosphereMultipleScatteringParams& MS = ActiveMultipleScattering();

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
    Out.AtmosphereAmbient = FVector3f(
        AmbientP.AirAmbient.R * Light.R, AmbientP.AirAmbient.G * Light.G, AmbientP.AirAmbient.B * Light.B);
    Out.AtmosphereAmbientFloor = AmbientP.AirAmbientFloor;
    Out.CloudAmbient = ToVector3(AmbientP.CloudAmbient);
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
    Out.SurfaceShadow = ToVector4(ActiveSurfaceShadow().Pack());

    // THE CAMERAS EACH LEVEL WAS BAKED AROUND, NOT RE-DERIVED. A fine cascade's
    // centre is snapped to its own texel grid from that camera, so the reader
    // has to snap from the same one. Any other -- this frame's, against a level
    // baked frames ago -- can land a whole texel out, which is a jump in where
    // the map sits, not a smooth disagreement. Level 0 is planet-centred.
    Out.ShadowCamera1 = ShadowBakedCamera[1];
    Out.ShadowCamera2 = ShadowBakedCamera[2];

    // -- Sampling -------------------------------------------------------------

    const FAtmosphereSamplingParams& Sampling = ActiveSampling();

    Out.CellSize = Sampling.CellSize;
    Out.FreshWeight = Sampling.FreshWeight;
    Out.LatticeGrowth = Sampling.LatticeGrowth;
    Out.LatticeGrowthFar = Sampling.LatticeGrowthFar;

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

    Out.FlowResource = FlowTarget ? FlowTarget->GameThread_GetRenderTargetResource() : nullptr;
    Out.ShadowResource = ShadowTarget ? ShadowTarget->GameThread_GetRenderTargetResource() : nullptr;
    Out.TransmittanceResource = TransmittanceTable ? TransmittanceTable->GameThread_GetRenderTargetResource() : nullptr;
    Out.StructureResource = ActiveStructureVolume() ? ActiveStructureVolume()->GetResource() : nullptr;
    Out.DetailResource = ActiveDetailVolume() ? ActiveDetailVolume()->GetResource() : nullptr;
    Out.BlueNoiseResource = Noise ? Noise->GetResource() : nullptr;

    return Out.IsUsable();
}

void APlanetAtmosphereActor::UpdateComputeMarch(
    float PlanetRadius, const FVector& PlanetCenter, const FVector& LightDir)
{
    FAtmosphereMarchParams Params;

    bool bWant = FillMarchParams(Params, PlanetRadius, PlanetCenter, LightDir);

    // CREATED ON FIRST USE, so an atmosphere that never draws registers nothing,
    // and one released by a delete comes back on undo.
    if (bWant && !ViewExtension && GetWorld())
    {
        ViewExtension = FSceneViewExtensions::NewExtension<FAtmosphereViewExtension>(GetWorld());
    }

    bWant = bWant && ViewExtension.IsValid();

    if (!ViewExtension)
    {
        return;
    }

    // The frame first, so the first enabled render has one to draw.
    if (bWant)
    {
        ViewExtension->SetFrame_GameThread(Params);
    }

    // Initialize can push while parked; the extension stays off until woken.
    ViewExtension->SetEnabled(bWant && bAtmosphereActive);
}

void APlanetAtmosphereActor::ReleaseViewExtension()
{
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

UFlowSimConfig* APlanetAtmosphereActor::ActiveSimConfig() const
{
    return bTerrestrial() ? Simulation.TerrestrialConfig.Get() : Simulation.GasGiantConfig.Get();
}

void APlanetAtmosphereActor::ClaimSimulation(const FVector& PlanetCenter, float PlanetRadius)
{
    UWorld* World = GetWorld();
    UFlowSimSubsystem* Sim = World ? World->GetSubsystem<UFlowSimSubsystem>() : nullptr;

    if (!Sim)
    {
        return;
    }

    UFlowSimConfig* Config = ActiveSimConfig();

    if (!Config && Simulation.bClaimSimulation && !bWarnedSimConfig)
    {
        bWarnedSimConfig = true;

        UE_LOG(LogCloudAtmosphere, Warning,
            TEXT("%s: no sim config for the %s model. The clouds draw the kept field, or none."),
            *GetName(), bTerrestrial() ? TEXT("terrestrial") : TEXT("gas giant"));
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
        KeptClock.SpinTime = KeptTime;
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