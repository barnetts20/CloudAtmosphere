#include "PlanetAtmosphereActor.h"
#include "CloudAtmosphere.h"
#include "CoreGlobals.h"
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
#include "GasGiantShadowMap.h"
#include "RenderingThread.h"
#include "SceneViewExtension.h"
#include "TerrestrialShadowMap.h"
#include "FlowSimSubsystem.h"
#include "FlowSimTypes.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialTypes.h"

// --------------------------------------------------------------------------
// Default material and texture assets
//
// Constructor defaults for the soft references. Overridable per instance, so a
// relocated asset is a data edit rather than a code one.
// --------------------------------------------------------------------------

static const TCHAR* MatPath_GasGiant = TEXT("/CloudAtmosphere/Material/MT_GasGiantAtmosphere_Default_Inst.MT_GasGiantAtmosphere_Default_Inst");
static const TCHAR* MatPath_Postprocess = TEXT("/CloudAtmosphere/Material/MT_Atmosphere_Postprocess_Inst.MT_Atmosphere_Postprocess_Inst");

// --------------------------------------------------------------------------
// Checked parameter pushes
//
// SetXParameterValue ON A NAME THE MATERIAL DOES NOT HAVE IS A SILENT NO-OP.
// No warning, no log, no return value -- the handle simply stops working, which
// reads as a shader bug and costs an afternoon. Every silent failure in this
// system so far has been a misspelled or renamed parameter.
//
// These wrappers check every push, editor-only, and are otherwise the same
// call. The point is that a rename is caught on the next rebuild rather than on
// the next screenshot.
//
// WARNED ONCE PER NAME, BECAUSE THE PUSH RUNS EVERY TICK. Unfiltered this would
// be a line per missing parameter per frame, which is not a diagnostic but a
// flood that buries the next one.
//
// RebuildMaterialInstances CLEARS THE FILTER, so the button in the details panel
// is the retrigger. Only the ACTIVE model's parameters are pushed, so covering
// both means pressing it, flipping PlanetType, and pressing it again.
// --------------------------------------------------------------------------

#if WITH_EDITOR
static TSet<FString> GWarnedMaterialParameters;

// THE PARAMETER SET, NOT A PARAMETER VALUE. GetXParameterValue answers "is this
// overridden anywhere in the chain", which is false for a parameter that exists
// in the base material and has never been touched in the instance -- a true
// answer to a question nobody asked, and a false alarm for the one that matters.
// GetAllXParameterInfo enumerates what the material HAS.
//
// Cached per MID because the enumeration allocates, and cleared alongside the
// warning filter so a rebuilt material is re-read rather than remembered.
static TMap<const UMaterialInstanceDynamic*, TSet<FName>> GKnownScalarNames;
static TMap<const UMaterialInstanceDynamic*, TSet<FName>> GKnownVectorNames;
static TMap<const UMaterialInstanceDynamic*, TSet<FName>> GKnownTextureNames;

enum class EAtmoParamKind : uint8 { Scalar, Vector, Texture };

static bool MaterialHasParameter(UMaterialInstanceDynamic* MID, EAtmoParamKind Kind, FName Name)
{
    TMap<const UMaterialInstanceDynamic*, TSet<FName>>& Cache =
        Kind == EAtmoParamKind::Scalar ? GKnownScalarNames
        : Kind == EAtmoParamKind::Vector ? GKnownVectorNames
        : GKnownTextureNames;

    TSet<FName>* Known = Cache.Find(MID);

    if (!Known)
    {
        TArray<FMaterialParameterInfo> Infos;
        TArray<FGuid> Guids;

        switch (Kind)
        {
        case EAtmoParamKind::Scalar:  MID->GetAllScalarParameterInfo(Infos, Guids); break;
        case EAtmoParamKind::Vector:  MID->GetAllVectorParameterInfo(Infos, Guids); break;
        default:                      MID->GetAllTextureParameterInfo(Infos, Guids); break;
        }

        Known = &Cache.Add(MID);

        for (const FMaterialParameterInfo& Info : Infos)
        {
            Known->Add(Info.Name);
        }
    }

    return Known->Contains(Name);
}

/** Drops a MID's cached parameter set. Called on every MID created, since a
 *  new instance can reuse a dead one's address or be rebuilt in place. */
static void ForgetMaterialParameters(const UMaterialInstanceDynamic* MID)
{
    GKnownScalarNames.Remove(MID);
    GKnownVectorNames.Remove(MID);
    GKnownTextureNames.Remove(MID);
}

// Keyed and reported by path, which names the actor: every actor's instances
// share their object names.
static void WarnMissingParameter(const UMaterialInstanceDynamic* MID, const TCHAR* Kind, FName Name)
{
    const FString Key = FString::Printf(TEXT("%s.%s"),
        MID ? *MID->GetPathName() : TEXT("null"), *Name.ToString());

    if (GWarnedMaterialParameters.Contains(Key))
    {
        return;
    }

    GWarnedMaterialParameters.Add(Key);

    UE_LOG(LogCloudAtmosphere, Warning,
        TEXT("PlanetAtmosphereActor: material '%s' has no %s parameter '%s' -- push ignored"),
        MID ? *MID->GetPathName() : TEXT("null"), Kind, *Name.ToString());
}
#endif

static void SetScalarChecked(UMaterialInstanceDynamic* MID, FName Name, float Value)
{
    if (!MID) return;

#if WITH_EDITOR
    if (!MaterialHasParameter(MID, EAtmoParamKind::Scalar, Name))
    {
        WarnMissingParameter(MID, TEXT("scalar"), Name);
    }
#endif

    MID->SetScalarParameterValue(Name, Value);
}

static void SetVectorChecked(UMaterialInstanceDynamic* MID, FName Name, const FLinearColor& Value)
{
    if (!MID) return;

#if WITH_EDITOR
    if (!MaterialHasParameter(MID, EAtmoParamKind::Vector, Name))
    {
        WarnMissingParameter(MID, TEXT("vector"), Name);
    }
#endif

    MID->SetVectorParameterValue(Name, Value);
}

static void SetTextureChecked(UMaterialInstanceDynamic* MID, FName Name, UTexture* Value)
{
    if (!MID) return;

#if WITH_EDITOR
    if (!MaterialHasParameter(MID, EAtmoParamKind::Texture, Name))
    {
        WarnMissingParameter(MID, TEXT("texture"), Name);
    }
#endif

    MID->SetTextureParameterValue(Name, Value);
}

// --------------------------------------------------------------------------
// Constructor
// --------------------------------------------------------------------------

APlanetAtmosphereActor::APlanetAtmosphereActor()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;

    AtmosphereRoot = CreateDefaultSubobject<USceneComponent>(TEXT("AtmosphereRoot"));
    SetRootComponent(AtmosphereRoot);

    PostProcessComponent = CreateDefaultSubobject<UPostProcessComponent>(TEXT("PostProcess"));
    PostProcessComponent->SetupAttachment(AtmosphereRoot);
    PostProcessComponent->bUnbound = true;
    PostProcessComponent->BlendWeight = 1.0f;

    SunLightComponent = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("SunLight"));
    SunLightComponent->SetupAttachment(AtmosphereRoot);
    SunLightComponent->SetMobility(EComponentMobility::Movable);
    SunLightComponent->SetUsingAbsoluteRotation(true);
    SunLightComponent->SetUsingAbsoluteScale(true);

    // Default radius = max(OceanRadius, PlanetRadius) at planet defaults:
    // PlanetRadius(100M) + SeaLevel(0.5) * NoiseAmplitude(15M) = 107,500,000 cm
    SetActorScale3D(FVector(107500000.0));

    // A STRUCT SHARED BY BOTH MODELS TAKES EACH MODEL'S DEFAULTS HERE, on the
    // CDO, rather than as member initialisers: one struct cannot carry two sets,
    // and setting them here keeps reset-to-default per instance.
    StructureLayer = FAtmosphereNoiseLayerParams::MakeStructureDefaults();
    DetailLayer = FAtmosphereNoiseLayerParams::MakeDetailDefaults();
    Geometry.HeightScale = 1.0f;

    TerrestrialMultipleScattering.SunlightPenetration = 0.0f;

    // SIZED TO BE LOOKED THROUGH FROM UNDERNEATH, which is the whole difference
    // from the gas giant's air. Every beta is per atmosphere thickness and every
    // scale height a fraction of it, so a column's optical depth is beta times
    // that fraction and does not move with HeightScale: at the gas giant's values
    // the vertical column is about six optical depths, and a surface dweller sees
    // no sky and no cloud. These land it near a quarter of one.
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

    // NOT NEAR-BLACK, unlike the gas giant's. Under a cloud base there is a lit
    // surface bouncing light back up, and this term is the whole of it: left at
    // the gas giant value, standing under the deck is night.
    TerrestrialAmbient.AirAmbient = FLinearColor(6.666667e-4f, 9.122807e-4f, 1.407407e-3f, 1.0f);
    TerrestrialAmbient.AirAmbientFloor = 0.02f;
    TerrestrialAmbient.CloudAmbient = FLinearColor(0.040f, 0.044f, 0.052f, 1.0f);
    TerrestrialAmbient.CloudAmbientFloor = 0.04f;

    // A cloud shadow lands on lit terrain here rather than on more cloud, so it
    // carries more of the surface's brightness than the deck's does.
    TerrestrialSurfaceShadow.DirectFraction = 0.85f;

    GasGiantMarchMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(MatPath_GasGiant));
    PostprocessMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(MatPath_Postprocess));

    // Default source assets, so the details panel shows something and a fresh
    // actor renders. A deck with no volumes is not a subtle failure -- the
    // carves and the erosion both go to their neutral values and the deck comes
    // out as a smooth shell.
    static ConstructorHelpers::FObjectFinder<UVolumeTexture> DefaultDetailVolume(
        TEXT("/CloudAtmosphere/Noise/VT_PerlinWorley_S8_128"));
    if (DefaultDetailVolume.Succeeded())
    {
        DetailLayer.Volume = DefaultDetailVolume.Object;
    }
    else
    {
        UE_LOG(LogCloudAtmosphere, Warning, TEXT("PlanetAtmosphereActor: Failed to load default deck detail volume"));
    }

    static ConstructorHelpers::FObjectFinder<UVolumeTexture> DefaultStructureVolume(
        TEXT("/CloudAtmosphere/Noise/VT_PerlinWorley_S4_128"));
    if (DefaultStructureVolume.Succeeded())
    {
        StructureLayer.Volume = DefaultStructureVolume.Object;
    }
    else
    {
        UE_LOG(LogCloudAtmosphere, Warning, TEXT("PlanetAtmosphereActor: Failed to load default deck structure volume"));
    }

    // THE TYPED CLOUD NOISE, which the genus weights and the detail channels
    // read as noise types. The gas giant's octave ladders are another layout.
    static ConstructorHelpers::FObjectFinder<UVolumeTexture> DefaultCloudStructureVolume(
        TEXT("/CloudAtmosphere/Noise/CloudNoise_4_128"));
    if (DefaultCloudStructureVolume.Succeeded())
    {
        TerrestrialStructureLayer.Volume = DefaultCloudStructureVolume.Object;
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
        Simulation.Config = DefaultSimConfig.Object;
    }
    else
    {
        UE_LOG(LogCloudAtmosphere, Warning, TEXT("PlanetAtmosphereActor: Failed to load default sim config"));
    }
}

// --------------------------------------------------------------------------
// Lifecycle
// --------------------------------------------------------------------------

void APlanetAtmosphereActor::BeginPlay()
{
    Super::BeginPlay();

    // Both fields read the sim: the gas giant for its bands, the terrestrial
    // clouds as their weather map.
    if (Simulation.bStartOnBeginPlay)
    {
        StartFlowSimulation();
    }
}

void APlanetAtmosphereActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    StopFlowSimulation();
    ReleaseViewExtension();
    Super::EndPlay(EndPlayReason);
}

// An editor deletion routes no EndPlay. The components go with the actor.
void APlanetAtmosphereActor::Destroyed()
{
    StopFlowSimulation();
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
        UpdateMaterialParameters();
        UpdateLightFromRotation();
    }
}

#if WITH_EDITOR
void APlanetAtmosphereActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
    Super::PostEditChangeProperty(PropertyChangedEvent);

    const FName Changed = PropertyChangedEvent.GetPropertyName();

    // PlanetType selects the path, not just the parameter set, so it cannot
    // take effect through the per-tick sweep alone.
    const bool bTypeChanged =
        Changed == GET_MEMBER_NAME_CHECKED(APlanetAtmosphereActor, PlanetType) ||
        Changed == GET_MEMBER_NAME_CHECKED(APlanetAtmosphereActor, GasGiantMarchMaterial) ||
        Changed == GET_MEMBER_NAME_CHECKED(APlanetAtmosphereActor, PostprocessMaterial);

    if (bInitialized && bTypeChanged)
    {
        RebuildMaterialInstances();
        return;
    }

    // Everything else reaches the materials on the next Tick.
    if (bInitialized)
    {
        UpdateLightFromRotation();
    }
}

// An undo restores PlanetType or a material without a property event, so the
// instances are checked against what the properties now name.
void APlanetAtmosphereActor::PostEditUndo()
{
    Super::PostEditUndo();

    if (!bInitialized)
    {
        return;
    }

    const bool bGasGiant = PlanetType == EPlanetAtmosphereType::GasGiant;

    const bool bStale = BuiltType != PlanetType
        || (bGasGiant && (!MID_Atmosphere || MID_Atmosphere->Parent != GasGiantMarchMaterial.Get()
            || !MID_Postprocess || MID_Postprocess->Parent != PostprocessMaterial.Get()));

    if (bStale)
    {
        RebuildMaterialInstances();
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

    CreateMaterialInstances();

    if (bAtmosphereActive)
    {
        UpdateMaterialParameters();
        UpdateLightFromRotation();
    }

    bInitialized = true;
}

void APlanetAtmosphereActor::RebuildMaterialInstances()
{
#if WITH_EDITOR
    // The parameter-check retrigger. Cleared before the push, so every missing
    // name reports again rather than staying silent from the first run.
    GWarnedMaterialParameters.Reset();
#endif

    CreateMaterialInstances();
    UpdateMaterialParameters();
}

// --------------------------------------------------------------------------
// Teardown and legacy child actors
// --------------------------------------------------------------------------

void APlanetAtmosphereActor::DestroyLegacyChildActors()
{
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

void APlanetAtmosphereActor::StopFlowSimulation()
{
    // Released before the actor goes, or a pooled planet leaves the sim
    // stepping with nothing sampling it.
    if (!bStartedSimulation)
    {
        return;
    }

    bStartedSimulation = false;

    if (UWorld* World = GetWorld())
    {
        if (UFlowSimSubsystem* Sim = World->GetSubsystem<UFlowSimSubsystem>())
        {
            Sim->StopSimulation();
        }
    }
}

// --------------------------------------------------------------------------
// Material instances
// --------------------------------------------------------------------------

UMaterialInterface* APlanetAtmosphereActor::LoadMaterialAsset(const TSoftObjectPtr<UMaterialInterface>& Ref, const TCHAR* Label)
{
    if (Ref.IsNull())
    {
        UE_LOG(LogCloudAtmosphere, Warning, TEXT("PlanetAtmosphereActor: %s material is unset."), Label);
        return nullptr;
    }

    UMaterialInterface* Loaded = Ref.LoadSynchronous();
    if (!Loaded)
    {
        UE_LOG(LogCloudAtmosphere, Warning, TEXT("PlanetAtmosphereActor: %s material failed to load from '%s'."),
            Label, *Ref.ToSoftObjectPath().ToString());
    }
    return Loaded;
}

void APlanetAtmosphereActor::CreateMaterialInstances()
{
    if (!PostProcessComponent) return;

    // THE MODEL IS CHOSEN ONCE, HERE, and recorded in BuiltType. The parameter
    // sweep dispatches on BuiltType rather than PlanetType so a type change
    // that has not been rebuilt yet cannot push one model's parameters at the
    // other model's path, which would do nothing and log nothing.
    BuiltType = PlanetType;
    MID_Atmosphere = nullptr;
    MID_Postprocess = nullptr;

    // The map holds the previous model's deck until every level is rebaked.
    bShadowPrimed = false;

    // The terrestrial model draws through the view extension and takes no
    // material.
    if (PlanetType == EPlanetAtmosphereType::GasGiant)
    {
        UMaterialInterface* BaseAtmo = LoadMaterialAsset(GasGiantMarchMaterial, TEXT("Gas giant march"));
        UMaterialInterface* BasePost = LoadMaterialAsset(PostprocessMaterial, TEXT("Postprocess"));

        if (BaseAtmo && BasePost)
        {
            MID_Atmosphere = UMaterialInstanceDynamic::Create(BaseAtmo, this, TEXT("MID_Atmosphere"));
            MID_Postprocess = UMaterialInstanceDynamic::Create(BasePost, this, TEXT("MID_Postprocess"));

            MID_Atmosphere->SetFlags(RF_Transient);
            MID_Postprocess->SetFlags(RF_Transient);

#if WITH_EDITOR
            ForgetMaterialParameters(MID_Atmosphere);
            ForgetMaterialParameters(MID_Postprocess);
#endif
        }
    }

    ApplyBlendables();
}

void APlanetAtmosphereActor::ApplyBlendables()
{
    if (!PostProcessComponent)
    {
        return;
    }

    // Order is the pipeline order: march, composite. Rebuilt rather than
    // assigned by index, so a stale instance cannot survive a swap and write
    // the same UserSceneTexture as its replacement.
    FPostProcessSettings& Settings = PostProcessComponent->Settings;
    Settings.WeightedBlendables.Array.Empty();

    if (!MID_Atmosphere || !MID_Postprocess)
    {
        return;
    }

    Settings.WeightedBlendables.Array.Add(FWeightedBlendable(1.0f, MID_Atmosphere));
    Settings.WeightedBlendables.Array.Add(FWeightedBlendable(1.0f, MID_Postprocess));
}

// --------------------------------------------------------------------------
// Material parameter update
// --------------------------------------------------------------------------

void APlanetAtmosphereActor::UpdateMaterialParameters()
{
    const FVector PlanetCenter = GetActorLocation();
    const float PlanetRadius = static_cast<float>(GetActorScale3D().GetMax());

    // Light direction from relative rotation -- treated as world-space direction
    // regardless of parent rotation. The user/gizmo sets relative rotation directly.
    const FVector LightDir = GetRootComponent()->GetRelativeRotation().Vector();

    if (bTerrestrial())
    {
        RequestShadowBake(PlanetRadius, PlanetCenter, LightDir);
        UpdateTransmittanceTable(PlanetRadius);
        UpdateComputeMarch(PlanetRadius, PlanetCenter, LightDir);
        return;
    }

    UpdateComputeMarch(PlanetRadius, PlanetCenter, LightDir);

    if (!MID_Atmosphere || !MID_Postprocess) return;

    ApplyMarchParams(PlanetRadius, PlanetCenter, LightDir);
    RequestShadowBake(PlanetRadius, PlanetCenter, LightDir);
    UpdateTransmittanceTable(PlanetRadius);

    // --- Postprocess (slot 1) ---
    //
    // EVERY ARGUMENT Atmo_Composite TAKES IS PUSHED FROM HERE, and nothing else
    // is. The pass reads the atmosphere buffer and the depth buffer, and does not
    // need to know where the planet is.

    // The radius eases from Max near the planet to Min far from it. The view
    // rendered last frame, as the shadow bake takes it: one frame stale.
    float CameraRadii = Composite.BlurFadeStart;

    if (const UWorld* World = GetWorld())
    {
        if (World->ViewLocationsRenderedLastFrame.Num() > 0)
        {
            CameraRadii = static_cast<float>(
                FVector::Dist(World->ViewLocationsRenderedLastFrame[0], PlanetCenter))
                / FMath::Max(PlanetRadius, 1e-4f);
        }
    }

    const float BlurFade = FMath::SmoothStep(Composite.BlurFadeStart,
        Composite.BlurFadeStart + FMath::Max(Composite.BlurFadeSpan, 1e-3f), CameraRadii);

    const float BlurRadius = FMath::Lerp(Composite.MaxBlurRadius, Composite.MinBlurRadius, BlurFade);

    SetScalarChecked(MID_Postprocess, TEXT("Blur Radius"), BlurRadius);
    SetScalarChecked(MID_Postprocess, TEXT("Blur Falloff Factor"), Composite.BlurFalloffFactor);
    SetScalarChecked(MID_Postprocess, TEXT("Depth Sharpness"), Composite.DepthSharpness);
    SetScalarChecked(MID_Postprocess, TEXT("Depth Tap Scale"), Composite.DepthTapScale);
    SetScalarChecked(MID_Postprocess, TEXT("Blur Weight"), Composite.BlurWeight);
}

// READOUT ONLY, NEVER PUSHED. Mirrors GG_TopBounds' upper bound before its
// clamp at the shell, with the detail carve centred as GG_DETAIL_RELIEF_CENTRED's
// default has it. A mismatch misreports the readout and changes nothing drawn.
static float SolveTopMaxReadout(float DeckTop, float GradientThickness, float BandRelief,
    const FAtmosphereFlowParams& Flow,
    const FAtmosphereNoiseLayerParams& Structure, const FAtmosphereNoiseLayerParams& Detail)
{
    const float UpReach = 0.5f * FMath::Abs(BandRelief) + FMath::Abs(Flow.PressureRelief)
        + 0.5f * FMath::Abs(Structure.Relief) + 0.5f * FMath::Abs(Detail.Relief)
        + FMath::Abs(Flow.StormTowerRelief);

    return DeckTop + FMath::Max(GradientThickness, 1e-4f) * UpReach;
}

/** The terrestrial planet shadow is a step: the transmittance table carries a
 *  grazing sun ray's attenuation, and any softness visibly above the floor reads
 *  worse. At a step the lobe power has nothing left to shape. */
static constexpr float TerrestrialTerminatorSoftness = 1e-4f;
static constexpr float TerrestrialLobeShadowPower = 6.0f;

/** READOUT ONLY, NEVER PUSHED. Mirrors TR_FieldReach, TR_TopMax and
 *  TR_BaseMin. A mismatch misreports the readouts and changes nothing drawn. */
static void SolveTerrestrialBounds(FTerrestrialShapeParams& Shape, const FTerrestrialLiftParams& Lift)
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

/** The terrestrial field's packed pins, in TR_BuildField's layout. ONE PACKER
 *  FOR THE MARCH AND THE BAKE, so the two cannot pack differently. */
struct FTerrestrialFieldPins
{
    FLinearColor CloudProfile;
    FLinearColor CloudCurves;
    FLinearColor CloudCoverage;
    FLinearColor CloudType;
    FLinearColor CloudLid;
    FLinearColor CloudLift;
    FLinearColor CloudMotion;
    FLinearColor NoiseLevels;
    FLinearColor StructureSampling;
    FLinearColor StructureWarp;
    FLinearColor DetailSampling;
    FLinearColor DetailWarp;
    FLinearColor CloudGenusStratus;
    FLinearColor CloudGenusStratocumulus;
    FLinearColor CloudGenusCumulus;
    FLinearColor CloudGenusCirrus;
    FLinearColor ShadowCascades;
    FLinearColor CloudResponse;
};

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

/** A gas giant layer's weights with its amount zeroed when it has no volume. No
 *  texture is neutral for erosion and relief both, so a missing layer is left
 *  out rather than read as whatever the material or the bake binds instead. */
static FLinearColor LayerWeights(const FAtmosphereNoiseLayerParams& Layer)
{
    FLinearColor Weights = Layer.NoiseWeights;
    Weights.A = Layer.Volume ? Weights.A : 0.0f;
    return Weights;
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

/** Reads the actor's terrestrial groups; the pins are grouped by the shader's
 *  needs, not the panel's, so most pins draw on several groups. */
static FTerrestrialFieldPins PackTerrestrialField(const APlanetAtmosphereActor& A, double Time)
{
    const FTerrestrialShapeParams& Shape = A.TerrestrialShape;
    const FTerrestrialCoverageParams& Coverage = A.TerrestrialCoverage;
    const FTerrestrialTypeParams& Type = A.TerrestrialType;
    const FTerrestrialLiftParams& Lift = A.TerrestrialLift;
    const FTerrestrialWarpParams& Warp = A.TerrestrialWarp;
    const FTerrestrialStructureLayerParams& Structure = A.TerrestrialStructureLayer;
    const FTerrestrialDetailLayerParams& Detail = A.TerrestrialDetailLayer;
    const FVector2D& Cascades = A.TerrestrialSurfaceShadow.CascadeRadii;

    FTerrestrialFieldPins Out;

    float DriftAngle, NoisePhase;
    NoiseClock(A.Simulation.Config, Time, DriftAngle, NoisePhase);

    // The planet's own spin, wrapped in double like the drift. Negated: turning
    // the sample point back carries the field forward.
    const float SpinAngle = (float)FMath::Fmod(-(double)A.TerrestrialPlanet.SpinRate * Time, 2.0 * UE_DOUBLE_PI);

    // Free slots stay zero.
    Out.CloudProfile = FLinearColor(Shape.CloudBase, Shape.CloudThickness, Shape.SurfaceSoftness, Shape.CeilingFalloff);
    Out.CloudCurves = FLinearColor(Shape.TopCurve, Shape.BottomCurve, A.TerrestrialCloudSlope, Warp.WarpStretch);
    Out.CloudCoverage = FLinearColor(Coverage.CloudCover, Coverage.StormPriority, Coverage.CoverageSoftness, 0.0f);
    Out.CloudType = FLinearColor(Type.TypeBias, 0.0f, Type.TypeTropical, 0.0f);
    Out.CloudLid = FLinearColor(Lift.PressureScale, 0.0f, Lift.CeilingPressure, Type.StratusDepth);
    Out.CloudLift = FLinearColor(Lift.BaseTropical, Lift.BasePressure, Lift.AltitudeGain, Lift.AltitudeLift);
    Out.CloudMotion = FLinearColor(DriftAngle, NoisePhase, Warp.WarpShift, SpinAngle);

    // Each layer's volume flag: a layer without one is left out, since no
    // texture is not neutral noise.
    Out.NoiseLevels = FLinearColor(
        Structure.MipBias, Structure.Volume ? 1.0f : 0.0f, Detail.MipBias, Detail.Volume ? 1.0f : 0.0f);

    // The structure layer's spare slots carry the storm's share of cloud type
    // and the genus blend's subsidence.
    Out.StructureSampling = FLinearColor(Structure.Scale, Structure.Aspect, Structure.Erosion, Type.TypeStorm);
    Out.StructureWarp = FLinearColor(Structure.FlowInherit, Type.Subsidence, 0.0f, 0.0f);

    Out.DetailSampling = FLinearColor(Detail.Scale, Detail.Aspect, Detail.Erosion, Detail.FadeMean);
    // The fade in planet radii: its authored feature count over the layer's
    // features per radian.
    const float FeatureRadii = 1.0f / FMath::Max(Detail.Scale, 0.01f);

    Out.DetailWarp = FLinearColor(
        Detail.FlowInherit, 0.0f, Detail.FadeStart * FeatureRadii, Detail.FadeLength * FeatureRadii);

    Out.CloudGenusStratus = Type.Stratus;
    Out.CloudGenusStratocumulus = Type.Stratocumulus;
    Out.CloudGenusCumulus = Type.Cumulus;
    Out.CloudGenusCirrus = Type.Cirrus;

    // The cloud layer and coverage fray ride in the cascade pin's spare slots.
    Out.ShadowCascades = FLinearColor(
        (float)Cascades.X, (float)Cascades.Y, (float)Coverage.CloudLayer, Coverage.CoverageFray);

    Out.CloudResponse = FLinearColor(
        0.0f, Type.TypeCurve, A.TerrestrialCloudMaterial.StormBalance, A.TerrestrialCloudMaterial.StormBlend);

    return Out;
}

void APlanetAtmosphereActor::ApplyMarchParams(float PlanetRadius, const FVector& PlanetCenter, const FVector& LightDir)
{
    // EVERY NAME HERE IS THE MEMBER'S OWN, so the parameter, the Custom node pin
    // and the shader term all read the same. A name the material lacks is caught
    // by the checked setters; nothing else would catch it.
    //
    // AUTHORED VALUES ONLY. GG_BuildField and GGAtmo_BuildAtmo derive the rest,
    // and the bake derives with the same functions from the same values.

    // -- Planet, light, clock -------------------------------------------------

    SetVectorChecked(MID_Atmosphere, TEXT("PlanetCenter"),
        FLinearColor(PlanetCenter.X, PlanetCenter.Y, PlanetCenter.Z, 0.0f));
    SetScalarChecked(MID_Atmosphere, TEXT("PlanetRadius"), PlanetRadius);

    SetVectorChecked(MID_Atmosphere, TEXT("LightDirection"),
        FLinearColor(LightDir.X, LightDir.Y, LightDir.Z, 0.0f));
    SetVectorChecked(MID_Atmosphere, TEXT("LightColor"), LightProduct());

    // The sim's clock, not the world's. Requires the material's Time parameter
    // to feed the Custom node directly -- wired through a multiply against an
    // engine Time node, this value is ignored and the field advects against
    // world time, which diverges the moment the sim pauses or restores.
    SetScalarChecked(MID_Atmosphere, TEXT("Time"), static_cast<float>(GetFieldTime()));

    // The field's frame as a quaternion, which the material rebuilds the
    // rotation from. The field is defined with the spin axis on Z; the march
    // runs world-oriented.
    const FQuat Rotation = GetFieldFrame();

    SetVectorChecked(MID_Atmosphere, TEXT("PlanetRotation"),
        FLinearColor(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W));

    // -- Textures ---------------------------------------------------------------
    //
    // FlowTarget is created at runtime, so it arrives through the config rather
    // than as a migrated asset. Any sampler works: every face of the atlas
    // carries its own gutter.
    //
    // ShadowTarget's sampler must be CLAMP on both axes: the map is a disc
    // inside a square, and wrapping puts the far limb against the near one. The
    // camera it centred its cascades on goes out in RequestShadowBake.

    if (Simulation.Config && Simulation.Config->FlowTarget)
    {
        SetTextureChecked(MID_Atmosphere, TEXT("FlowTarget"), Simulation.Config->FlowTarget);
    }

    if (ShadowTarget)
    {
        SetTextureChecked(MID_Atmosphere, TEXT("ShadowTarget"), ShadowTarget);
    }

    // -- The shell ----------------------------------------------------------------
    //
    // FROM THE BUILT MODEL'S OWN GROUP, as are the field, lighting and surface
    // shadow groups below; only Raymarch is shared.

    SetScalarChecked(MID_Atmosphere, TEXT("HeightScale"), ActiveHeightScale());

    // -- The field's own ----------------------------------------------------------

    ApplyGasGiantModelParams();

    if (UVolumeTexture* Volume = ActiveStructureVolume())
    {
        SetTextureChecked(MID_Atmosphere, TEXT("StructureVolume"), Volume);
    }

    if (UVolumeTexture* Volume = ActiveDetailVolume())
    {
        SetTextureChecked(MID_Atmosphere, TEXT("DetailVolume"), Volume);
    }

    // -- Air and ambient ----------------------------------------------------------

    const FAtmosphereAirParams& AirP = ActiveAir();
    const FAtmosphereAmbientParams& AmbientP = ActiveAmbient();

    SetVectorChecked(MID_Atmosphere, TEXT("RayleighBeta"), AirP.RayleighBeta());
    SetScalarChecked(MID_Atmosphere, TEXT("RayleighScaleHeight"), AirP.RayleighScaleHeight);
    SetVectorChecked(MID_Atmosphere, TEXT("MieBeta"), AirP.MieBeta());
    SetScalarChecked(MID_Atmosphere, TEXT("MieScaleHeight"), AirP.MieScaleHeight);
    SetScalarChecked(MID_Atmosphere, TEXT("MieG"), AirP.MieG);
    SetScalarChecked(MID_Atmosphere, TEXT("MieLobeDecay"), AirP.MieLobeDecay);
    SetVectorChecked(MID_Atmosphere, TEXT("AbsorptionBeta"), AirP.AbsorptionBeta());
    SetScalarChecked(MID_Atmosphere, TEXT("AbsorptionAltitude"), AirP.AbsorptionAltitude);
    SetScalarChecked(MID_Atmosphere, TEXT("AbsorptionFalloff"), AirP.AbsorptionFalloff);

    // The air's ambient is a ratio of the light, as the cloud's is in the march.
    const FLinearColor Light = LightProduct();

    SetVectorChecked(MID_Atmosphere, TEXT("AtmosphereAmbient"), FLinearColor(
        AmbientP.AirAmbient.R * Light.R, AmbientP.AirAmbient.G * Light.G, AmbientP.AirAmbient.B * Light.B, AmbientP.AirAmbient.A));
    SetScalarChecked(MID_Atmosphere, TEXT("AtmosphereAmbientFloor"), AmbientP.AirAmbientFloor);
    SetVectorChecked(MID_Atmosphere, TEXT("CloudAmbient"), AmbientP.CloudAmbient);
    SetScalarChecked(MID_Atmosphere, TEXT("CloudAmbientFloor"), AmbientP.CloudAmbientFloor);
    SetScalarChecked(MID_Atmosphere, TEXT("AmbientTerminator"), AmbientP.AmbientTerminator);

    // -- Cloud Lighting -------------------------------------------------------------

    const FAtmospherePhaseParams& PhaseP = ActivePhase();
    const FAtmosphereMultipleScatteringParams& MS = ActiveMultipleScattering();

    SetScalarChecked(MID_Atmosphere, TEXT("ForwardG"), PhaseP.ForwardG);
    SetScalarChecked(MID_Atmosphere, TEXT("BackwardG"), PhaseP.BackwardG);
    SetScalarChecked(MID_Atmosphere, TEXT("ForwardWeight"), PhaseP.ForwardWeight);

    SetScalarChecked(MID_Atmosphere, TEXT("LightExtinctionFraction"), MS.LightExtinctionFraction());
    SetScalarChecked(MID_Atmosphere, TEXT("OctaveCount"), static_cast<float>(MS.OctaveCount));
    SetScalarChecked(MID_Atmosphere, TEXT("OctaveAttenuation"), MS.OctaveAttenuation());
    SetScalarChecked(MID_Atmosphere, TEXT("OctaveContribution"), MS.OctaveAttenuation());
    SetScalarChecked(MID_Atmosphere, TEXT("OctaveEccentricity"), MS.OctaveEccentricity());

    SetScalarChecked(MID_Atmosphere, TEXT("TerminatorSoftness"), Terminator.TerminatorSoftness);
    SetScalarChecked(MID_Atmosphere, TEXT("LobeShadowPower"), Terminator.LobeShadowPower);

    // -- Pipeline -----------------------------------------------------------------

    SetScalarChecked(MID_Atmosphere, TEXT("AtmosphereSteps"), static_cast<float>(Raymarch.AtmosphereSteps));
    SetScalarChecked(MID_Atmosphere, TEXT("CloudSteps"), static_cast<float>(Raymarch.CloudSteps));
    SetScalarChecked(MID_Atmosphere, TEXT("ChordSpread"), Raymarch.ChordSpread);

    // -- Surface Shadows ----------------------------------------------------------
    //
    // PACKED: four related scalars on one Custom node pin. Pack() owns the
    // layout and both models' atmosphere builders unpack it.
    SetVectorChecked(MID_Atmosphere, TEXT("SurfaceShadow"), ActiveSurfaceShadow().Pack());
}

// --------------------------------------------------------------------------
// The gas giant field's own parameters
//
// Each member under its own name. A pin pushed here that the material lacks is
// caught by the checked setters.
// --------------------------------------------------------------------------

void APlanetAtmosphereActor::ApplyGasGiantModelParams()
{
    GasGiantProfile.SolvedTopMax = SolveTopMaxReadout(
        GasGiantProfile.DeckTop, GasGiantProfile.GradientThickness,
        GasGiantBandShape.BandRelief, Flow, StructureLayer, DetailLayer);

    SetScalarChecked(MID_Atmosphere, TEXT("DeckTop"), GasGiantProfile.DeckTop);
    SetScalarChecked(MID_Atmosphere, TEXT("CeilingFalloff"), GasGiantProfile.CeilingFalloff);
    SetScalarChecked(MID_Atmosphere, TEXT("GradientThickness"), GasGiantProfile.GradientThickness);
    SetScalarChecked(MID_Atmosphere, TEXT("DeckBackstop"), GasGiantProfile.DeckBackstop);
    SetScalarChecked(MID_Atmosphere, TEXT("DeckSlope"), GasGiantProfile.DeckSlope);
    SetScalarChecked(MID_Atmosphere, TEXT("DeckOpticalDepth"), GasGiantProfile.DeckOpticalDepth);
    SetScalarChecked(MID_Atmosphere, TEXT("DensityCurve"), GasGiantProfile.DensityCurve);

    // The band shape and the deck's relief.
    SetScalarChecked(MID_Atmosphere, TEXT("BandSharpness"), GasGiantBandShape.BandSharpness);
    SetScalarChecked(MID_Atmosphere, TEXT("BandBias"), GasGiantBandShape.BandBias);
    SetScalarChecked(MID_Atmosphere, TEXT("BandRelief"), GasGiantBandShape.BandRelief);
    SetScalarChecked(MID_Atmosphere, TEXT("PressureRelief"), Flow.PressureRelief);
    SetScalarChecked(MID_Atmosphere, TEXT("VortexThreshold"), Flow.VortexThreshold);
    SetScalarChecked(MID_Atmosphere, TEXT("StormTowerRelief"), Flow.StormTowerRelief);

    SetVectorChecked(MID_Atmosphere, TEXT("ScatterNegative"), GasGiantBands.ScatterNegative);
    SetVectorChecked(MID_Atmosphere, TEXT("ExtinctionNegative"), GasGiantBands.ExtinctionNegative);
    SetVectorChecked(MID_Atmosphere, TEXT("ScatterPositive"), GasGiantBands.ScatterPositive);
    SetVectorChecked(MID_Atmosphere, TEXT("ExtinctionPositive"), GasGiantBands.ExtinctionPositive);
    SetVectorChecked(MID_Atmosphere, TEXT("ScatterBase"), GasGiantBands.ScatterBase);
    SetVectorChecked(MID_Atmosphere, TEXT("ExtinctionBase"), GasGiantBands.ExtinctionBase);
    SetScalarChecked(MID_Atmosphere, TEXT("BandScale"), GasGiantBands.BandScale);

    SetScalarChecked(MID_Atmosphere, TEXT("HemisphereBlend"), Flow.HemisphereBlend);
    SetScalarChecked(MID_Atmosphere, TEXT("HemisphereVariance"), Flow.HemisphereVariance);
    SetScalarChecked(MID_Atmosphere, TEXT("ReliefThinning"), Flow.ReliefThinning);
    SetScalarChecked(MID_Atmosphere, TEXT("RotationWeight"), Flow.RotationWeight);

    SetScalarChecked(MID_Atmosphere, TEXT("WarpTime"), Motion.WarpTime);
    SetScalarChecked(MID_Atmosphere, TEXT("DeepShearRatio"), Motion.DeepShearRatio);
    SetScalarChecked(MID_Atmosphere, TEXT("TurbulenceFloor"), Motion.TurbulenceFloor);
    SetScalarChecked(MID_Atmosphere, TEXT("CrossfadePeriod"), Motion.CrossfadePeriod);

    SetScalarChecked(MID_Atmosphere, TEXT("EdgeBias"), Carve.EdgeBias);
    SetScalarChecked(MID_Atmosphere, TEXT("ErosionDepth"), Carve.ErosionDepth);

    ApplyNoiseLayer(TEXT("Structure"), StructureLayer);
    ApplyNoiseLayer(TEXT("Detail"), DetailLayer);
}

void APlanetAtmosphereActor::ApplyNoiseLayer(const TCHAR* Prefix, const FAtmosphereNoiseLayerParams& Layer)
{
    auto Name = [Prefix](const TCHAR* Member) { return FName(FString(Prefix) + Member); };

    SetVectorChecked(MID_Atmosphere, Name(TEXT("NoiseWeights")), LayerWeights(Layer));
    SetScalarChecked(MID_Atmosphere, Name(TEXT("Scale")), Layer.Scale);
    SetScalarChecked(MID_Atmosphere, Name(TEXT("Aspect")), Layer.Aspect);
    SetScalarChecked(MID_Atmosphere, Name(TEXT("Relief")), Layer.Relief);
    SetScalarChecked(MID_Atmosphere, Name(TEXT("Erosion")), Layer.Erosion);
    SetScalarChecked(MID_Atmosphere, Name(TEXT("FlowInherit")), Layer.FlowInherit);
    SetScalarChecked(MID_Atmosphere, Name(TEXT("ShearInherit")), Layer.ShearInherit);
    SetScalarChecked(MID_Atmosphere, Name(TEXT("FadeNear")), Layer.FadeNear);
    SetScalarChecked(MID_Atmosphere, Name(TEXT("FadeSpan")), Layer.FadeSpan);
    SetScalarChecked(MID_Atmosphere, Name(TEXT("BandMix")), Layer.BandMix);
    SetScalarChecked(MID_Atmosphere, Name(TEXT("Crossfade")), Layer.bCrossfade ? 1.0f : 0.0f);
}

/** ATMO_SHADOW_NO_DECK in AtmosphereShadowMap.ush: every crossing past any
 *  chord, so an unbaked texel reads as fully lit. */
static constexpr float ShadowNoDeck = 1000.0f;

/** Which actor bakes into each shadow target, and on which frame it last did. */
struct FShadowTargetClaim
{
    TWeakObjectPtr<const APlanetAtmosphereActor> Actor;
    uint64 Frame = 0;
};

static TMap<TWeakObjectPtr<const UTextureRenderTarget2DArray>, FShadowTargetClaim> GShadowTargetClaims;

bool APlanetAtmosphereActor::PrepareShadowTarget()
{
    UTextureRenderTarget2DArray* Target = ShadowTarget;

    if (!Target)
    {
        if (!bWarnedShadowTarget)
        {
            bWarnedShadowTarget = true;

            UE_LOG(LogCloudAtmosphere, Warning,
                TEXT("%s: no Shadow Target set. Create a Texture Render Target 2D ")
                TEXT("Array asset and assign it; the deck shadow bake has nowhere to write."),
                *GetName());
        }

        return false;
    }

    // ONE ACTOR PER TARGET. Two baking into one asset re-init it against each
    // other's settings and overwrite each other's slices. The first to claim it
    // keeps it while it keeps baking; a play world's actor takes it from an
    // editor world's, so a PIE session shadows even while the editor ticks.
    FShadowTargetClaim& Claim = GShadowTargetClaims.FindOrAdd(Target);
    const APlanetAtmosphereActor* Holder = Claim.Actor.Get();

    const bool bHeld = Holder && Holder != this && Claim.Frame + 1 >= GFrameCounter;
    const bool bOutranks = GetWorld() && GetWorld()->IsGameWorld()
        && !(Holder && Holder->GetWorld() && Holder->GetWorld()->IsGameWorld());

    if (bHeld && !bOutranks)
    {
        if (!bWarnedSharedShadowTarget)
        {
            bWarnedSharedShadowTarget = true;

            UE_LOG(LogCloudAtmosphere, Warning,
                TEXT("%s: Shadow Target '%s' is already baked by '%s'. Assign each ")
                TEXT("atmosphere its own target; this one bakes no shadows until then."),
                *GetName(), *Target->GetName(), *Holder->GetName());
        }

        return false;
    }

    Claim.Actor = this;
    Claim.Frame = GFrameCounter;
    bWarnedSharedShadowTarget = false;

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
        // and reach the no-deck sentinel at 1000. A float format has no sRGB
        // variant so nothing clamps them here -- but the material's Texture
        // Object must still be set to Linear Color, which no flag can enforce.
        Target->Init(Edge, Edge, DesiredSlices, PF_FloatRGBA);
        Target->UpdateResourceImmediate(true);

        bShadowPrimed = false;

        UE_LOG(LogCloudAtmosphere, Log, TEXT("%s: Shadow Target set to %dx%d x %d RGBA16F."),
            *GetName(), Edge, Edge, DesiredSlices);
    }

    bWarnedShadowTarget = false;

    return true;
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
    // the top. The material samples with the texture's own address modes.
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

        // Linear gamma: the table holds integrals well past 1. The material's
        // Texture Object must still be Linear Color, which no flag enforces.
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

    // The values ApplyMarchParams pushes: the table is keyed to them, and
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

    SetTextureChecked(MID_Atmosphere, TEXT("TransmittanceTable"), TransmittanceTable);
}

template<typename TShadowParams>
bool APlanetAtmosphereActor::FillSharedShadowParams(
    TShadowParams& Params, float PlanetRadius, const FVector& PlanetCenter,
    const FVector& LightDir)
{
    UWorld* World = GetWorld();

    if (!World || !PrepareShadowTarget())
    {
        return false;
    }

    if (!Simulation.Config || !Simulation.Config->FlowTarget)
    {
        return false;
    }

    Params.FlowResource = Simulation.Config->FlowTarget->GameThread_GetRenderTargetResource();
    Params.MapResource = ShadowTarget->GameThread_GetRenderTargetResource();

    if (!Params.FlowResource || !Params.MapResource)
    {
        return false;
    }

    // -- Frame --------------------------------------------------------------
    //
    // The planet's axes are the rows of WorldToLocal, exactly as the material
    // receives them, so the light and the camera arrive in the frame the field
    // is defined in. Spin is not applied here: GG_FlowProbe applies it.

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

    // THE SAME VECTOR THE MATERIAL GETS, not a re-derivation of it. Whichever
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

    // -- Deck ---------------------------------------------------------------
    //
    // The field's arguments, under the names ApplyMarchParams pushes them
    // to the material. The bake derives from them with the same shader
    // functions, so any divergence here is a deck the light sees and the eye
    // does not.

    // Only what both fields read. Each model's own members are filled by its
    // branch in RequestShadowBake.
    Params.PlanetRadius = PlanetRadius;
    Params.HeightScale = ActiveHeightScale();
    Params.Time = static_cast<float>(GetFieldTime());

    // -- Extinction ---------------------------------------------------------

    Params.LightExtinctionFraction = ActiveMultipleScattering().LightExtinctionFraction();

    // -- Volumes ------------------------------------------------------------
    //
    // Resources of the properties ApplyMarchParams already pushes, not a second
    // reference to the assets: a compute pass cannot reach a UObject.

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

    // ONE REQUEST PER FRAME. Initialize, a rebuild and Tick can all push in one
    // frame, and the subsystem would run every request it is handed.
    if (!Sim || ShadowBakeFrame == GFrameCounter)
    {
        return;
    }

    ShadowBakeFrame = GFrameCounter;

    // ONE REQUEST PER FIELD TYPE, because the two parameter structs are separate
    // types bound to separate shaders. What they hold is the same today and is
    // expected not to stay that way.
    const auto ToVector4 = [](const FLinearColor& C) { return FVector4f(C.R, C.G, C.B, C.A); };

    if (BuiltType == EPlanetAtmosphereType::GasGiant)
    {
        FGasGiantShadowParams Params;

        if (!FillSharedShadowParams(Params, PlanetRadius, PlanetCenter, LightDir))
        {
            return;
        }

        Params.DeckTop = GasGiantProfile.DeckTop;
        Params.CeilingFalloff = GasGiantProfile.CeilingFalloff;
        Params.GradientThickness = GasGiantProfile.GradientThickness;
        Params.DeckBackstop = GasGiantProfile.DeckBackstop;
        Params.DeckSlope = GasGiantProfile.DeckSlope;
        Params.DeckOpticalDepth = GasGiantProfile.DeckOpticalDepth;
        Params.DensityCurve = GasGiantProfile.DensityCurve;

        Params.BandSharpness = GasGiantBandShape.BandSharpness;
        Params.BandBias = GasGiantBandShape.BandBias;
        Params.BandRelief = GasGiantBandShape.BandRelief;
        Params.PressureRelief = Flow.PressureRelief;
        Params.VortexThreshold = Flow.VortexThreshold;
        Params.StormTowerRelief = Flow.StormTowerRelief;

        Params.ExtinctionNegative = ToVector4(GasGiantBands.ExtinctionNegative);
        Params.ExtinctionPositive = ToVector4(GasGiantBands.ExtinctionPositive);
        Params.ExtinctionBase = ToVector4(GasGiantBands.ExtinctionBase);
        Params.BandScale = GasGiantBands.BandScale;

        Params.HemisphereBlend = Flow.HemisphereBlend;
        Params.HemisphereVariance = Flow.HemisphereVariance;
        Params.ReliefThinning = Flow.ReliefThinning;
        Params.RotationWeight = Flow.RotationWeight;

        Params.WarpTime = Motion.WarpTime;
        Params.DeepShearRatio = Motion.DeepShearRatio;
        Params.TurbulenceFloor = Motion.TurbulenceFloor;
        Params.CrossfadePeriod = Motion.CrossfadePeriod;

        Params.EdgeBias = Carve.EdgeBias;
        Params.ErosionDepth = Carve.ErosionDepth;

        Params.StructureNoiseWeights = ToVector4(LayerWeights(StructureLayer));
        Params.StructureScale = StructureLayer.Scale;
        Params.StructureAspect = StructureLayer.Aspect;
        Params.StructureRelief = StructureLayer.Relief;
        Params.StructureErosion = StructureLayer.Erosion;
        Params.StructureFlowInherit = StructureLayer.FlowInherit;
        Params.StructureShearInherit = StructureLayer.ShearInherit;
        Params.StructureFadeNear = StructureLayer.FadeNear;
        Params.StructureFadeSpan = StructureLayer.FadeSpan;
        Params.StructureBandMix = StructureLayer.BandMix;
        Params.StructureCrossfade = StructureLayer.bCrossfade ? 1.0f : 0.0f;

        Params.DetailNoiseWeights = ToVector4(LayerWeights(DetailLayer));
        Params.DetailScale = DetailLayer.Scale;
        Params.DetailAspect = DetailLayer.Aspect;
        Params.DetailRelief = DetailLayer.Relief;
        Params.DetailErosion = DetailLayer.Erosion;
        Params.DetailFlowInherit = DetailLayer.FlowInherit;
        Params.DetailShearInherit = DetailLayer.ShearInherit;
        Params.DetailFadeNear = DetailLayer.FadeNear;
        Params.DetailFadeSpan = DetailLayer.FadeSpan;
        Params.DetailBandMix = DetailLayer.BandMix;
        Params.DetailCrossfade = DetailLayer.bCrossfade ? 1.0f : 0.0f;

        if (Sim->RequestShadowBake(Params))
        {
            CommitShadowBake(Params.LevelMask, Params.LightDir, Params.CameraLocal);
        }
    }
    else
    {
        FTerrestrialShadowParams Params;

        if (!FillSharedShadowParams(Params, PlanetRadius, PlanetCenter, LightDir))
        {
            return;
        }

        const FTerrestrialFieldPins Pins = PackTerrestrialField(*this, GetFieldTime());

        Params.CloudProfile = ToVector4(Pins.CloudProfile);
        Params.CloudCurves = ToVector4(Pins.CloudCurves);
        Params.CloudCoverage = ToVector4(Pins.CloudCoverage);
        Params.CloudType = ToVector4(Pins.CloudType);
        Params.CloudLid = ToVector4(Pins.CloudLid);
        Params.CloudLift = ToVector4(Pins.CloudLift);
        Params.CloudMotion = ToVector4(Pins.CloudMotion);
        Params.NoiseLevels = ToVector4(Pins.NoiseLevels);
        Params.StructureSampling = ToVector4(Pins.StructureSampling);
        Params.StructureWarp = ToVector4(Pins.StructureWarp);
        Params.DetailSampling = ToVector4(Pins.DetailSampling);
        Params.DetailWarp = ToVector4(Pins.DetailWarp);
        Params.CloudGenusStratus = ToVector4(Pins.CloudGenusStratus);
        Params.CloudGenusStratocumulus = ToVector4(Pins.CloudGenusStratocumulus);
        Params.CloudGenusCumulus = ToVector4(Pins.CloudGenusCumulus);
        Params.CloudGenusCirrus = ToVector4(Pins.CloudGenusCirrus);
        Params.ShadowCascades = ToVector4(Pins.ShadowCascades);
        Params.CloudResponse = ToVector4(Pins.CloudResponse);

        const FTerrestrialCloudMaterialParams Material = ResolveCloudMaterial(TerrestrialCloudMaterial);

        Params.CloudOpticalDepth = Material.CloudOpticalDepth;
        Params.CloudExtinction = ToVector4(Material.CloudExtinction);
        Params.StormExtinction = ToVector4(Material.StormExtinction);

        if (Sim->RequestShadowBake(Params))
        {
            CommitShadowBake(Params.LevelMask, Params.LightDir, Params.CameraLocal);
        }
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

    // PUSHED, NOT RE-DERIVED, AND PER LEVEL. A fine cascade's centre is snapped
    // to its own texel grid from the camera it was baked around, so the reader
    // has to snap from that same camera. Any other -- the material's own, or a
    // level baked frames ago read against this frame's -- can land a whole texel
    // out, which is a jump in where the map sits, not a smooth disagreement.
    // Level 0 is planet-centred and needs none.
    const auto PushCamera = [this](const TCHAR* Name, const FVector3f& Camera)
        {
            SetVectorChecked(MID_Atmosphere, Name, FLinearColor(Camera.X, Camera.Y, Camera.Z, 0.0f));
        };

    PushCamera(TEXT("ShadowCamera1"), ShadowBakedCamera[1]);
    PushCamera(TEXT("ShadowCamera2"), ShadowBakedCamera[2]);
}

// --------------------------------------------------------------------------
// Terrestrial compute march
//
// The march's inputs, handed to the view extension. The field comes from the
// packer the shadow bake shares, so the bake lights the field the eye sees.
// --------------------------------------------------------------------------

bool APlanetAtmosphereActor::FillTerrestrialMarchParams(
    FTerrestrialMarchParams& Out, float PlanetRadius, const FVector& PlanetCenter, const FVector& LightDir)
{
    const auto ToVector3 = [](const FLinearColor& C) { return FVector3f(C.R, C.G, C.B); };
    const auto ToVector4 = [](const FLinearColor& C) { return FVector4f(C.R, C.G, C.B, C.A); };

    // -- Planet, light, clock -------------------------------------------------

    const FQuat Rotation = GetFieldFrame();
    const FLinearColor Light = LightProduct();

    Out.PlanetCenter = PlanetCenter;
    Out.PlanetRotation = FVector4f(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W);
    Out.LightDirection = FVector3f(LightDir);
    Out.LightColor = ToVector3(Light);
    Out.PlanetRadius = PlanetRadius;
    Out.HeightScale = ActiveHeightScale();
    Out.Time = static_cast<float>(GetFieldTime());

    // -- Field ----------------------------------------------------------------

    SolveTerrestrialBounds(TerrestrialShape, TerrestrialLift);

    const FTerrestrialFieldPins Pins = PackTerrestrialField(*this, GetFieldTime());

    Out.CloudProfile = ToVector4(Pins.CloudProfile);
    Out.CloudCurves = ToVector4(Pins.CloudCurves);
    Out.CloudCoverage = ToVector4(Pins.CloudCoverage);
    Out.CloudType = ToVector4(Pins.CloudType);
    Out.CloudLid = ToVector4(Pins.CloudLid);
    Out.CloudLift = ToVector4(Pins.CloudLift);
    Out.CloudMotion = ToVector4(Pins.CloudMotion);
    Out.CloudResponse = ToVector4(Pins.CloudResponse);
    Out.NoiseLevels = ToVector4(Pins.NoiseLevels);
    Out.StructureSampling = ToVector4(Pins.StructureSampling);
    Out.StructureWarp = ToVector4(Pins.StructureWarp);
    Out.DetailSampling = ToVector4(Pins.DetailSampling);
    Out.DetailWarp = ToVector4(Pins.DetailWarp);
    Out.CloudGenusStratus = ToVector4(Pins.CloudGenusStratus);
    Out.CloudGenusStratocumulus = ToVector4(Pins.CloudGenusStratocumulus);
    Out.CloudGenusCumulus = ToVector4(Pins.CloudGenusCumulus);
    Out.CloudGenusCirrus = ToVector4(Pins.CloudGenusCirrus);
    Out.ShadowCascades = ToVector4(Pins.ShadowCascades);

    const FTerrestrialCloudMaterialParams Material = ResolveCloudMaterial(TerrestrialCloudMaterial);

    Out.CloudOpticalDepth = Material.CloudOpticalDepth;
    Out.CloudScatter = ToVector3(Material.CloudScatter);
    Out.StormScatter = ToVector3(Material.StormScatter);
    Out.CloudExtinction = ToVector4(Material.CloudExtinction);
    Out.StormExtinction = ToVector4(Material.StormExtinction);

    // -- Air, ambient, lighting -----------------------------------------------

    const FAtmosphereAirParams& AirP = ActiveAir();
    const FAtmosphereAmbientParams& AmbientP = ActiveAmbient();
    const FAtmospherePhaseParams& PhaseP = ActivePhase();
    const FAtmosphereMultipleScatteringParams& MS = ActiveMultipleScattering();

    Out.RayleighBeta = ToVector3(AirP.RayleighBeta());
    Out.RayleighScaleHeight = AirP.RayleighScaleHeight;
    Out.MieBeta = ToVector3(AirP.MieBeta());
    Out.MieScaleHeight = AirP.MieScaleHeight;
    Out.MieG = AirP.MieG;
    Out.MieLobeDecay = AirP.MieLobeDecay;
    Out.AbsorptionBeta = ToVector3(AirP.AbsorptionBeta());
    Out.AbsorptionAltitude = AirP.AbsorptionAltitude;
    Out.AbsorptionFalloff = AirP.AbsorptionFalloff;

    Out.AtmosphereAmbient = FVector3f(
        AmbientP.AirAmbient.R * Light.R, AmbientP.AirAmbient.G * Light.G, AmbientP.AirAmbient.B * Light.B);
    Out.AtmosphereAmbientFloor = AmbientP.AirAmbientFloor;
    Out.CloudAmbient = ToVector3(AmbientP.CloudAmbient);
    Out.CloudAmbientFloor = AmbientP.CloudAmbientFloor;
    Out.AmbientTerminator = AmbientP.AmbientTerminator;

    Out.ForwardG = PhaseP.ForwardG;
    Out.BackwardG = PhaseP.BackwardG;
    Out.ForwardWeight = PhaseP.ForwardWeight;

    Out.LightExtinctionFraction = MS.LightExtinctionFraction();
    Out.OctaveCount = static_cast<float>(MS.OctaveCount);
    Out.OctaveAttenuation = MS.OctaveAttenuation();
    Out.OctaveContribution = MS.OctaveAttenuation();
    Out.OctaveEccentricity = MS.OctaveEccentricity();

    Out.TerminatorSoftness = TerrestrialTerminatorSoftness;
    Out.LobeShadowPower = TerrestrialLobeShadowPower;

    // -- Pipeline and shadows -------------------------------------------------

    Out.AtmosphereSteps = static_cast<float>(Raymarch.AtmosphereSteps);
    Out.CloudSteps = static_cast<float>(Raymarch.CloudSteps);
    Out.ChordSpread = Raymarch.ChordSpread;
    Out.SurfaceShadow = ToVector4(ActiveSurfaceShadow().Pack());

    Out.ShadowCamera1 = ShadowBakedCamera[1];
    Out.ShadowCamera2 = ShadowBakedCamera[2];

    // -- Sampling -------------------------------------------------------------

    Out.CellSize = TerrestrialSampling.CellSize;
    Out.FreshWeight = TerrestrialSampling.FreshWeight;
    Out.LatticeGrowth = TerrestrialSampling.LatticeGrowth;
    Out.LatticeGrowthFar = TerrestrialSampling.LatticeGrowthFar;

    // -- Resources ------------------------------------------------------------

    UTexture* Noise = BlueNoise;

    if (!Noise && !bWarnedBlueNoise)
    {
        bWarnedBlueNoise = true;

        UE_LOG(LogCloudAtmosphere, Warning,
            TEXT("%s: no Blue Noise texture set. The terrestrial atmosphere does not draw until one is."),
            *GetName());
    }

    bWarnedBlueNoise = bWarnedBlueNoise && !Noise;

    Out.FlowResource = (Simulation.Config && Simulation.Config->FlowTarget)
        ? Simulation.Config->FlowTarget->GameThread_GetRenderTargetResource() : nullptr;
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
    FTerrestrialMarchParams Params;

    bool bWant = bTerrestrial()
        && FillTerrestrialMarchParams(Params, PlanetRadius, PlanetCenter, LightDir);

    // CREATED ON FIRST USE, so an atmosphere that never draws terrestrial
    // registers nothing, and one released by a delete comes back on undo.
    if (bWant && !ViewExtension && GetWorld())
    {
        ViewExtension = FSceneViewExtensions::NewExtension<FAtmosphereViewExtension>(GetWorld());
    }

    bWant = bWant && ViewExtension.IsValid();
    bComputeMarch = bWant;

    if (!ViewExtension)
    {
        return;
    }

    // The frame first, so the first enabled render has one to draw.
    if (bWant)
    {
        ViewExtension->SetFrame_GameThread(Params);
    }

    // A rebuild can push while parked; the extension stays off until woken.
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
// Gas giant simulation
// --------------------------------------------------------------------------

void APlanetAtmosphereActor::StartFlowSimulation()
{
    if (!Simulation.Config)
    {
        UE_LOG(LogCloudAtmosphere, Warning,
            TEXT("PlanetAtmosphereActor: no SimConfig. The clouds will render against an "
                "unbound flow field."));
        return;
    }

    UWorld* World = GetWorld();
    if (!World) return;

    UFlowSimSubsystem* Sim = World->GetSubsystem<UFlowSimSubsystem>();
    if (!Sim) return;

    Sim->StartSimulation(Simulation.Config);
    bStartedSimulation = true;
}

double APlanetAtmosphereActor::GetFieldTime() const
{
    if (const UWorld* World = GetWorld())
    {
        if (const UFlowSimSubsystem* Sim = World->GetSubsystem<UFlowSimSubsystem>())
        {
            return Sim->GetDisplayTime();
        }
    }
    return 0.0;
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

    // bEnabled drops the gas giant's passes out of the post-process chain and
    // SetEnabled the terrestrial ones; Tick stops the parameter push, the bake
    // and the transmittance update.
    if (PostProcessComponent)
    {
        PostProcessComponent->bEnabled = bActive;
    }

    // Woken, the next tick pushes a fresh frame and enables the extension;
    // enabled here it would draw the frame captured before parking.
    if (ViewExtension && !bActive)
    {
        ViewExtension->SetEnabled(false);
    }

    if (SunLightComponent)
    {
        SunLightComponent->SetVisibility(bActive);
    }
}
