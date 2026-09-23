#include "PlanetAtmosphereActor.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Math/OrthoMatrix.h"
#include "Engine/VolumeTexture.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureRenderTarget2DArray.h"
#include "Engine/TextureRenderTarget2DArray.h"
#include "AtmosphereTransmittance.h"
#include "GasGiantShadowMap.h"
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

static const TCHAR* MatPath_Terrestrial = TEXT("/CloudAtmosphere/Material/MT_TerrestrialAtmosphere_Default_Inst.MT_TerrestrialAtmosphere_Default_Inst");
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

static void WarnMissingParameter(const UMaterialInstanceDynamic* MID, const TCHAR* Kind, FName Name)
{
    const FString Key = FString::Printf(TEXT("%s.%s"),
        MID ? *MID->GetName() : TEXT("null"), *Name.ToString());

    if (GWarnedMaterialParameters.Contains(Key))
    {
        return;
    }

    GWarnedMaterialParameters.Add(Key);

    UE_LOG(LogTemp, Warning,
        TEXT("PlanetAtmosphereActor: material '%s' has no %s parameter '%s' -- push ignored"),
        MID ? *MID->GetName() : TEXT("null"), Kind, *Name.ToString());
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

    // Past any authored interval, so every level captures on its first tick
    // rather than shadowing nothing until its cadence comes round.
    for (int32 Level = 0; Level < AtmoShadowBake::CascadeCount; ++Level)
    {
        FramesSinceCapture[Level] = MAX_int32;
    }

    // Default radius = max(OceanRadius, PlanetRadius) at planet defaults:
    // PlanetRadius(100M) + SeaLevel(0.5) * NoiseAmplitude(15M) = 107,500,000 cm
    SetActorScale3D(FVector(107500000.0));

    // A STRUCT SHARED BY BOTH MODELS TAKES EACH MODEL'S DEFAULTS HERE, on the
    // CDO, rather than as member initialisers: one struct cannot carry two sets,
    // and setting them here keeps reset-to-default per instance.
    StructureLayer = FAtmosphereNoiseLayerParams::MakeStructureDefaults();
    DetailLayer = FAtmosphereNoiseLayerParams::MakeDetailDefaults();
    Geometry.HeightScale = 1.0f;

    TerrestrialStructureLayer = FAtmosphereNoiseLayerParams::MakeStructureDefaults();
    TerrestrialDetailLayer = FAtmosphereNoiseLayerParams::MakeDetailDefaults();

    // THE STRUCTURE LAYER IS THE CLOUD SHAPE coverage erodes, so its scale sets
    // cloud size: a few features per system rather than per planet. Erosion is
    // how much the noise shapes the column; below 1 some of the column fills
    // regardless of the noise.
    TerrestrialStructureLayer.Scale = 6.0f;
    TerrestrialStructureLayer.Aspect = 2.0f;
    TerrestrialStructureLayer.Erosion = 0.85f;
    TerrestrialStructureLayer.FlowInherit = 0.6f;
    TerrestrialStructureLayer.ShearInherit = 0.4f;
    TerrestrialStructureLayer.FadeNear = 0.3f;
    TerrestrialStructureLayer.FadeSpan = 0.6f;
    TerrestrialStructureLayer.bCrossfade = true;

    // THE DETAIL LAYER ERODES EDGES: wispy bases, billowy tops.
    TerrestrialDetailLayer.Scale = 30.0f;
    TerrestrialDetailLayer.Aspect = 1.0f;
    TerrestrialDetailLayer.Erosion = 0.6f;
    TerrestrialDetailLayer.FlowInherit = 0.3f;
    TerrestrialDetailLayer.ShearInherit = 0.3f;
    TerrestrialDetailLayer.FadeSpan = 0.3f;

    TerrestrialGeometry.HeightScale = 0.2f;

    TerrestrialExtinction.LightExtinctionFraction = 1.0f;

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
    TerrestrialAtmosphereLighting.RayleighBeta = FLinearColor(0.416289f, 1.270482f, 2.0f, 1.0f);
    TerrestrialAtmosphereLighting.RayleighScaleHeight = 0.15f;
    TerrestrialAtmosphereLighting.MieBeta = FLinearColor(1.0f, 0.893109f, 0.755932f, 1.0f);
    TerrestrialAtmosphereLighting.MieScaleHeight = 0.1f;
    TerrestrialAtmosphereLighting.MieG = 0.95f;
    TerrestrialAtmosphereLighting.AbsorptionBeta = FLinearColor(0.5f, 0.403727f, 0.437888f, 1.0f);

    // NOT NEAR-BLACK, unlike the gas giant's. Under a cloud base there is a lit
    // surface bouncing light back up, and this term is the whole of it: left at
    // the gas giant value, standing under the deck is night.
    TerrestrialAtmosphereLighting.AtmosphereAmbient = FLinearColor(0.020f, 0.026f, 0.038f, 1.0f);
    TerrestrialAtmosphereLighting.AtmosphereAmbientFloor = 0.02f;

    TerrestrialPhase.CloudAmbient = FLinearColor(0.040f, 0.044f, 0.052f, 1.0f);
    TerrestrialPhase.CloudAmbientFloor = 0.04f;

    // A cloud shadow lands on lit terrain here rather than on more cloud, so it
    // carries more of the surface's brightness than the deck's does.
    TerrestrialSurfaceShadow.DirectFraction = 0.85f;



    TerrestrialMarchMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(MatPath_Terrestrial));
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
        UE_LOG(LogTemp, Warning, TEXT("PlanetAtmosphereActor: Failed to load default deck detail volume"));
    }

    static ConstructorHelpers::FObjectFinder<UVolumeTexture> DefaultStructureVolume(
        TEXT("/CloudAtmosphere/Noise/VT_PerlinWorley_S4_128"));
    if (DefaultStructureVolume.Succeeded())
    {
        StructureLayer.Volume = DefaultStructureVolume.Object;

        // BOTH TERRESTRIAL LAYERS ON THE COARSE VOLUME, at different scales. The
        // detail layer's own asset is tuned for a deck's fine finish, which on a
        // band reads as static rather than as cloud.
        TerrestrialStructureLayer.Volume = DefaultStructureVolume.Object;
        TerrestrialDetailLayer.Volume = DefaultStructureVolume.Object;
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("PlanetAtmosphereActor: Failed to load default deck structure volume"));
    }

    static ConstructorHelpers::FObjectFinder<UFlowSimConfig> DefaultSimConfig(
        TEXT("/CloudAtmosphere/NoiseRecipes/GasGiantSimScratch"));
    if (DefaultSimConfig.Succeeded())
    {
        Simulation.Config = DefaultSimConfig.Object;
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("PlanetAtmosphereActor: Failed to load default sim config"));
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

void APlanetAtmosphereActor::Destroyed()
{
    DestroyChildActors();
    Super::Destroyed();
}

void APlanetAtmosphereActor::BeginDestroy()
{
    DestroyChildActors();
    Super::BeginDestroy();
}

void APlanetAtmosphereActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
    if (!GetWorld() || GetWorld()->IsPreviewWorld()) return;

    if (!bInitialized)
    {
        bPendingInitialize = true;
    }
}

void APlanetAtmosphereActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    if (bPendingInitialize)
    {
        bPendingInitialize = false;
        Initialize();
    }

    if (bInitialized)
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

    // PlanetType selects the material, not just the parameter set, so it cannot
    // take effect through the per-tick sweep alone.
    const bool bTypeChanged =
        Changed == GET_MEMBER_NAME_CHECKED(APlanetAtmosphereActor, PlanetType) ||
        Changed == GET_MEMBER_NAME_CHECKED(APlanetAtmosphereActor, TerrestrialMarchMaterial) ||
        Changed == GET_MEMBER_NAME_CHECKED(APlanetAtmosphereActor, GasGiantMarchMaterial);

    if (bInitialized && bTypeChanged)
    {
        RebuildMaterialInstances();
        return;
    }

    if (bInitialized)
    {
        UpdateMaterialParameters();
        UpdateLightFromRotation();
    }
}

bool APlanetAtmosphereActor::CanEditChange(const FProperty* InProperty) const
{
    if (!Super::CanEditChange(InProperty))
        return false;

    if (bIsPlanetOwned && InProperty)
    {
        const FName PropName = InProperty->GetFName();
        // Lock location and scale -- driven by the planet.
        // Rotation remains editable (controls light direction).
        if (PropName == TEXT("RelativeLocation") || PropName == TEXT("RelativeScale3D"))
            return false;
    }

    return true;
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
    SpawnChildActors();
    CreateMaterialInstances();
    UpdateMaterialParameters();
    UpdateLightFromRotation();
    bInitialized = true;
}

void APlanetAtmosphereActor::RebuildMaterialInstances()
{
#if WITH_EDITOR
    // The parameter-check retrigger. Cleared before the push, so every missing
    // name reports again rather than staying silent from the first run.
    GWarnedMaterialParameters.Reset();
    GKnownScalarNames.Reset();
    GKnownVectorNames.Reset();
    GKnownTextureNames.Reset();
#endif

    CreateMaterialInstances();
    UpdateMaterialParameters();
}

// --------------------------------------------------------------------------
// Child actor management
// --------------------------------------------------------------------------

void APlanetAtmosphereActor::SpawnChildActors()
{
    UWorld* World = GetWorld();
    if (!World) return;

    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = this;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    // --- Post-Process Volume ---
    if (!PostProcessVolume)
    {
        PostProcessVolume = World->SpawnActor<APostProcessVolume>(
            APostProcessVolume::StaticClass(),
            GetActorTransform(),
            SpawnParams);

        if (PostProcessVolume)
        {
            PostProcessVolume->bUnbound = true;
            PostProcessVolume->BlendWeight = 1.0f;

            if (USceneComponent* PPRoot = PostProcessVolume->GetRootComponent())
            {
                PPRoot->AttachToComponent(AtmosphereRoot,
                    FAttachmentTransformRules::KeepWorldTransform);
            }
        }
    }

    // --- Directional Light ---
    if (!SunLight)
    {
        SunLight = World->SpawnActor<ADirectionalLight>(
            ADirectionalLight::StaticClass(),
            GetActorTransform(),
            SpawnParams);

        if (SunLight)
        {
            SunLight->GetComponent()->SetMobility(EComponentMobility::Movable);

            if (USceneComponent* LightRoot = SunLight->GetRootComponent())
            {
                LightRoot->AttachToComponent(AtmosphereRoot,
                    FAttachmentTransformRules::KeepWorldTransform);
            }
        }
    }
}

void APlanetAtmosphereActor::DestroyChildActors()
{
    // Released before the actor goes, or a pooled planet leaves the sim
    // stepping with nothing sampling it.
    if (bStartedSimulation)
    {
        if (UWorld* World = GetWorld())
        {
            if (UFlowSimSubsystem* Sim = World->GetSubsystem<UFlowSimSubsystem>())
            {
                Sim->StopSimulation();
            }
        }
        bStartedSimulation = false;
    }

    if (PostProcessVolume)
    {
        PostProcessVolume->Destroy();
        PostProcessVolume = nullptr;
    }
    if (SunLight)
    {
        SunLight->Destroy();
        SunLight = nullptr;
    }
    MID_Atmosphere = nullptr;
    MID_Postprocess = nullptr;

    DestroyGasGiantOccluderCaptures();
}

// --------------------------------------------------------------------------
// Material instances
// --------------------------------------------------------------------------

UMaterialInterface* APlanetAtmosphereActor::LoadMaterialAsset(const TSoftObjectPtr<UMaterialInterface>& Ref, const TCHAR* Label)
{
    if (Ref.IsNull())
    {
        UE_LOG(LogTemp, Warning, TEXT("PlanetAtmosphereActor: %s material is unset."), Label);
        return nullptr;
    }

    UMaterialInterface* Loaded = Ref.LoadSynchronous();
    if (!Loaded)
    {
        UE_LOG(LogTemp, Warning, TEXT("PlanetAtmosphereActor: %s material failed to load from '%s'."),
            Label, *Ref.ToSoftObjectPath().ToString());
    }
    return Loaded;
}

void APlanetAtmosphereActor::CreateMaterialInstances()
{
    if (!PostProcessVolume) return;

    // THE MODEL IS CHOSEN ONCE, HERE, and recorded in BuiltType. The parameter
    // sweep dispatches on BuiltType rather than PlanetType so a type change
    // that has not been rebuilt yet cannot push one model's parameters at the
    // other model's material, which would do nothing and log nothing.
    const bool bGasGiant = (PlanetType == EPlanetAtmosphereType::GasGiant);

    UMaterialInterface* BaseAtmo = bGasGiant
        ? LoadMaterialAsset(GasGiantMarchMaterial, TEXT("Gas giant march"))
        : LoadMaterialAsset(TerrestrialMarchMaterial, TEXT("Terrestrial march"));
    UMaterialInterface* BasePost = LoadMaterialAsset(PostprocessMaterial, TEXT("Postprocess"));

    if (!BaseAtmo || !BasePost)
    {
        return;
    }

    MID_Atmosphere = UMaterialInstanceDynamic::Create(BaseAtmo, this, TEXT("MID_Atmosphere"));
    MID_Postprocess = UMaterialInstanceDynamic::Create(BasePost, this, TEXT("MID_Postprocess"));

    BuiltType = PlanetType;

    // Order is the pipeline order: march, composite. Rebuilt rather
    // than assigned by index, so a stale instance cannot survive a swap and
    // write the same UserSceneTexture as its replacement.
    FPostProcessSettings& Settings = PostProcessVolume->Settings;
    Settings.WeightedBlendables.Array.Empty();
    Settings.WeightedBlendables.Array.Add(FWeightedBlendable(1.0f, MID_Atmosphere));
    Settings.WeightedBlendables.Array.Add(FWeightedBlendable(1.0f, MID_Postprocess));
}

// --------------------------------------------------------------------------
// Material parameter update
// --------------------------------------------------------------------------

void APlanetAtmosphereActor::UpdateMaterialParameters()
{
    if (!MID_Atmosphere || !MID_Postprocess) return;

    const FVector PlanetCenter = GetActorLocation();
    const float PlanetRadius = static_cast<float>(GetActorScale3D().GetMax());

    // Light direction from relative rotation -- treated as world-space direction
    // regardless of parent rotation. The user/gizmo sets relative rotation directly.
    const FVector LightDir = GetRootComponent()->GetRelativeRotation().Vector();

    // BOTH MODELS TAKE THE SAME THREE STEPS. Which field they describe is
    // decided inside, on BuiltType, so nothing about the sequence is per model.
    ApplyMarchParams(PlanetRadius, PlanetCenter, LightDir);
    RequestShadowBake(PlanetRadius, PlanetCenter, LightDir);
    UpdateTransmittanceTable(PlanetRadius);

    // --- Postprocess (slot 1) ---
    //
    // One material for both models, so every blur parameter comes from
    // Environment and none of it is per-model.
    //
    // EVERY ARGUMENT Atmo_Composite TAKES IS PUSHED FROM HERE, and nothing else
    // is. The pass reads the atmosphere buffer and the depth buffer, and does not
    // need to know where the planet is.

    SetScalarChecked(MID_Postprocess, TEXT("Blur Radius"), static_cast<float>(Composite.BlurRadius));
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

/** READOUT ONLY, NEVER PUSHED. Mirrors TR_FieldReach, TR_TopMax and
 *  TR_BaseMin. A mismatch misreports the readouts and changes nothing drawn. */
static void SolveTerrestrialBounds(
    const FTerrestrialProfileParams& Profile, float& OutTopMax, float& OutBaseMin)
{
    const float D = FMath::Max(Profile.CloudThickness, 1e-4f);

    const float BaseUp = D * (FMath::Max(Profile.BaseTropical, 0.0f)
        + FMath::Abs(Profile.BasePressure) + FMath::Max(Profile.AltitudeLift, 0.0f));

    const float BaseDown = D * (-FMath::Min(Profile.BaseTropical, 0.0f)
        + FMath::Abs(Profile.BasePressure) - FMath::Min(Profile.AltitudeLift, 0.0f));

    const float Depth = D * FMath::Max(
        Profile.CeilingDepth * (1.0f + FMath::Abs(Profile.CeilingPressure)), 1e-3f);

    OutTopMax = FMath::Min(Profile.CloudBase + BaseUp + Depth, 1.0f);
    OutBaseMin = Profile.CloudBase - BaseDown;
}

/** The terrestrial field's packed pins, in TR_BuildField's layout. ONE PACKER
 *  FOR THE MATERIAL AND THE BAKE, so the two cannot pack differently. */
struct FTerrestrialFieldPins
{
    FLinearColor CloudProfile;
    FLinearColor CloudCurves;
    FLinearColor CloudCoverage;
    FLinearColor CloudType;
    FLinearColor CloudLid;
    FLinearColor CloudLift;
    FLinearColor CloudMotion;
    FLinearColor StructureNoiseWeights;
    FLinearColor StructureSampling;
    FLinearColor StructureWarp;
    FLinearColor DetailNoiseWeights;
    FLinearColor DetailSampling;
    FLinearColor DetailWarp;
    FLinearColor CloudDrift;
};

/** The westerly jet's angular rate: the three-cell profile's peak for the sim
 *  layer the deck reads. */
static float TerrestrialJetRate(const UFlowSimConfig* Config)
{
    if (!Config)
    {
        return 0.0f;
    }

    const float Scale = Config->LayerProfiles.Num() > 0 ? Config->LayerProfiles[0].JetScale : 1.0f;

    return Config->JetStrength * Scale;
}

/** A drift angle wrapped to one turn in double precision, so the shader's trig
 *  stays exact however long the sim has run. */
static float WrappedDriftAngle(float Drift, float JetRate, float Time)
{
    return (float)FMath::Fmod((double)Drift * (double)JetRate * (double)Time, 2.0 * UE_DOUBLE_PI);
}

static FTerrestrialFieldPins PackTerrestrialField(
    const FTerrestrialProfileParams& P, const FTerrestrialMotionParams& M,
    const FAtmosphereNoiseLayerParams& Structure, const FAtmosphereNoiseLayerParams& Detail,
    float JetRate, float Time)
{
    FTerrestrialFieldPins Out;

    Out.CloudProfile = FLinearColor(P.CloudBase, P.CloudThickness, P.SurfaceSoftness, P.CeilingFalloff);
    Out.CloudCurves = FLinearColor(P.TopCurve, P.BottomCurve, P.CloudSlope, P.WarpStretch);
    Out.CloudCoverage = FLinearColor(P.CloudCover, P.CoverageGain, P.StratusDepth, P.ErosionGain);
    Out.CloudType = FLinearColor(P.TypeBias, P.TypeAscent, P.TypeTropical, P.ErosionAscent);
    Out.CloudLid = FLinearColor(P.PressureScale, P.CeilingDepth, P.CeilingPressure, P.WarpShift);
    Out.CloudLift = FLinearColor(P.BaseTropical, P.BasePressure, P.AltitudeGain, P.AltitudeLift);
    Out.CloudMotion = FLinearColor(M.WarpTime, M.DeepShearRatio, M.CrossfadePeriod, M.RotationWeight);

    const auto Sampling = [](const FAtmosphereNoiseLayerParams& L)
        {
            return FLinearColor(L.Scale, L.Aspect, L.Erosion, L.bCrossfade ? 1.0f : 0.0f);
        };

    const auto Warp = [](const FAtmosphereNoiseLayerParams& L)
        {
            return FLinearColor(L.FlowInherit, L.ShearInherit, L.FadeNear, L.FadeSpan);
        };

    Out.StructureNoiseWeights = Structure.NoiseWeights;
    Out.StructureSampling = Sampling(Structure);
    Out.StructureWarp = Warp(Structure);

    Out.DetailNoiseWeights = Detail.NoiseWeights;
    Out.DetailSampling = Sampling(Detail);
    Out.DetailWarp = Warp(Detail);

    Out.CloudDrift = FLinearColor(
        WrappedDriftAngle(M.StructureDrift, JetRate, Time),
        WrappedDriftAngle(M.DetailDrift, JetRate, Time), 0.0f, 0.0f);

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
    SetVectorChecked(MID_Atmosphere, TEXT("LightColor"), LightColor);

    // The sim's clock, not the world's. Requires the material's Time parameter
    // to feed the Custom node directly -- wired through a multiply against an
    // engine Time node, this value is ignored and the field advects against
    // world time, which diverges the moment the sim pauses or restores.
    SetScalarChecked(MID_Atmosphere, TEXT("Time"), GetGasGiantTime());

    // The planet's orientation as a quaternion; GGAtmo_WorldToLocal rebuilds
    // the rotation from it. The field is defined with the spin axis on Z; the
    // march runs world-oriented.
    const FQuat Rotation = GetActorQuat();

    SetVectorChecked(MID_Atmosphere, TEXT("PlanetRotation"),
        FLinearColor(Rotation.X, Rotation.Y, Rotation.Z, Rotation.W));

    // -- Textures ---------------------------------------------------------------
    //
    // FlowTarget is created at runtime, so it arrives through the config rather
    // than as a migrated asset. Its sampler must be WRAP U, CLAMP V: the sim grid
    // is a cylinder, and wrapping V joins the north pole to the south.
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
    // FROM THE BUILT MODEL'S OWN GROUP, as everything below is. The pin names are
    // shared because both materials expand the same build macro; the values
    // behind them are not.

    SetScalarChecked(MID_Atmosphere, TEXT("HeightScale"), ActiveGeometry().HeightScale);

    // -- The field's own ----------------------------------------------------------

    if (BuiltType == EPlanetAtmosphereType::GasGiant)
    {
        ApplyGasGiantModelParams();
    }
    else
    {
        ApplyTerrestrialModelParams();
    }

    // The noise volumes, which both models bind under the same names.
    if (ActiveStructureLayer().Volume)
    {
        SetTextureChecked(MID_Atmosphere, TEXT("StructureVolume"), ActiveStructureLayer().Volume);
    }

    if (ActiveDetailLayer().Volume)
    {
        SetTextureChecked(MID_Atmosphere, TEXT("DetailVolume"), ActiveDetailLayer().Volume);
    }

    // -- Atmosphere Lighting ------------------------------------------------------

    const FAtmosphereLightingParams& Air = ActiveAtmosphereLighting();

    SetVectorChecked(MID_Atmosphere, TEXT("RayleighBeta"), Air.RayleighBeta);
    SetScalarChecked(MID_Atmosphere, TEXT("RayleighScaleHeight"), Air.RayleighScaleHeight);
    SetVectorChecked(MID_Atmosphere, TEXT("MieBeta"), Air.MieBeta);
    SetScalarChecked(MID_Atmosphere, TEXT("MieScaleHeight"), Air.MieScaleHeight);
    SetScalarChecked(MID_Atmosphere, TEXT("MieG"), Air.MieG);
    SetVectorChecked(MID_Atmosphere, TEXT("AbsorptionBeta"), Air.AbsorptionBeta);
    SetScalarChecked(MID_Atmosphere, TEXT("AbsorptionAltitude"), Air.AbsorptionAltitude);
    SetScalarChecked(MID_Atmosphere, TEXT("AbsorptionFalloff"), Air.AbsorptionFalloff);
    SetVectorChecked(MID_Atmosphere, TEXT("AtmosphereAmbient"), Air.AtmosphereAmbient);
    SetScalarChecked(MID_Atmosphere, TEXT("AtmosphereAmbientFloor"), Air.AtmosphereAmbientFloor);

    // -- Cloud Lighting -------------------------------------------------------------

    const FAtmosphereExtinctionParams& Ext = ActiveExtinction();
    const FAtmospherePhaseParams& PhaseP = ActivePhase();
    const FAtmosphereMultipleScatteringParams& MS = ActiveMultipleScattering();
    const FAtmosphereTerminatorParams& Term = ActiveTerminator();

    // DensityCurve is pushed by the gas giant alone: the terrestrial band shapes
    // each of its two ramps separately and its material carries no such pin.
    SetScalarChecked(MID_Atmosphere, TEXT("LightExtinctionFraction"), Ext.LightExtinctionFraction);

    SetScalarChecked(MID_Atmosphere, TEXT("ForwardG"), PhaseP.ForwardG);
    SetScalarChecked(MID_Atmosphere, TEXT("BackwardG"), PhaseP.BackwardG);
    SetScalarChecked(MID_Atmosphere, TEXT("ForwardWeight"), PhaseP.ForwardWeight);
    SetVectorChecked(MID_Atmosphere, TEXT("CloudAmbient"), PhaseP.CloudAmbient);
    SetScalarChecked(MID_Atmosphere, TEXT("CloudAmbientFloor"), PhaseP.CloudAmbientFloor);

    SetScalarChecked(MID_Atmosphere, TEXT("OctaveCount"), static_cast<float>(MS.OctaveCount));
    SetScalarChecked(MID_Atmosphere, TEXT("OctaveAttenuation"), MS.OctaveAttenuation);
    SetScalarChecked(MID_Atmosphere, TEXT("OctaveContribution"), MS.OctaveContribution);
    SetScalarChecked(MID_Atmosphere, TEXT("OctaveEccentricity"), MS.OctaveEccentricity);

    SetScalarChecked(MID_Atmosphere, TEXT("TerminatorSoftness"), Term.TerminatorSoftness);
    SetScalarChecked(MID_Atmosphere, TEXT("AmbientTerminator"), Term.AmbientTerminator);
    SetScalarChecked(MID_Atmosphere, TEXT("MieLobeDecay"), Term.MieLobeDecay);
    SetScalarChecked(MID_Atmosphere, TEXT("LobeShadowPower"), Term.LobeShadowPower);

    // -- Pipeline -----------------------------------------------------------------

    SetScalarChecked(MID_Atmosphere, TEXT("AtmosphereSteps"), Raymarch.AtmosphereSteps);
    SetScalarChecked(MID_Atmosphere, TEXT("CloudSteps"), Raymarch.CloudSteps);
    SetScalarChecked(MID_Atmosphere, TEXT("ChordSpread"), Raymarch.ChordSpread);

    // -- Surface Shadows ----------------------------------------------------------
    //
    // PACKED, unlike everything above: four related scalars on one Custom node
    // pin, the way the band tints and the cloud phase already travel. Pack()
    // owns the layout and GG_BuildAtmo unpacks it.
    SetVectorChecked(MID_Atmosphere, TEXT("SurfaceShadow"), ActiveSurfaceShadow().Pack());
}

// --------------------------------------------------------------------------
// The field's own parameters
//
// SAME NAMES ON BOTH PATHS. The two materials carry the same pins, so what
// differs is only which authored group a value is read from. When a field stops
// wanting one of these it stops pushing it and its material drops the pin; the
// checked setters catch the half-done case.
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
    SetScalarChecked(MID_Atmosphere, TEXT("DensityCurve"), Extinction.DensityCurve);

    // Read by the deck's relief and not by the terrestrial band, which routes the
    // same channels through its own weight vectors instead.
    SetScalarChecked(MID_Atmosphere, TEXT("BandSharpness"), GasGiantBandShape.BandSharpness);
    SetScalarChecked(MID_Atmosphere, TEXT("BandBias"), GasGiantBandShape.BandBias);
    // These four are the deck's relief. The terrestrial band routes the same
    // channels through its own weight vectors and carries no such pins.
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

void APlanetAtmosphereActor::ApplyTerrestrialModelParams()
{
    SolveTerrestrialBounds(
        TerrestrialProfile, TerrestrialProfile.SolvedTopMax, TerrestrialProfile.SolvedBaseMin);

    const FTerrestrialFieldPins Pins = PackTerrestrialField(
        TerrestrialProfile, TerrestrialMotion, TerrestrialStructureLayer, TerrestrialDetailLayer,
        TerrestrialJetRate(Simulation.Config), GetGasGiantTime());

    SetVectorChecked(MID_Atmosphere, TEXT("CloudProfile"), Pins.CloudProfile);
    SetVectorChecked(MID_Atmosphere, TEXT("CloudCurves"), Pins.CloudCurves);
    SetVectorChecked(MID_Atmosphere, TEXT("CloudCoverage"), Pins.CloudCoverage);
    SetVectorChecked(MID_Atmosphere, TEXT("CloudType"), Pins.CloudType);
    SetVectorChecked(MID_Atmosphere, TEXT("CloudLid"), Pins.CloudLid);
    SetVectorChecked(MID_Atmosphere, TEXT("CloudLift"), Pins.CloudLift);
    SetVectorChecked(MID_Atmosphere, TEXT("CloudMotion"), Pins.CloudMotion);
    SetVectorChecked(MID_Atmosphere, TEXT("StructureNoiseWeights"), Pins.StructureNoiseWeights);
    SetVectorChecked(MID_Atmosphere, TEXT("StructureSampling"), Pins.StructureSampling);
    SetVectorChecked(MID_Atmosphere, TEXT("StructureWarp"), Pins.StructureWarp);
    SetVectorChecked(MID_Atmosphere, TEXT("DetailNoiseWeights"), Pins.DetailNoiseWeights);
    SetVectorChecked(MID_Atmosphere, TEXT("DetailSampling"), Pins.DetailSampling);
    SetVectorChecked(MID_Atmosphere, TEXT("DetailWarp"), Pins.DetailWarp);
    SetVectorChecked(MID_Atmosphere, TEXT("CloudDrift"), Pins.CloudDrift);

    SetScalarChecked(MID_Atmosphere, TEXT("CloudOpticalDepth"), TerrestrialProfile.CloudOpticalDepth);

    SetVectorChecked(MID_Atmosphere, TEXT("CloudScatter"), TerrestrialCloudMaterial.CloudScatter);
    SetVectorChecked(MID_Atmosphere, TEXT("CloudExtinction"), TerrestrialCloudMaterial.CloudExtinction);
    SetVectorChecked(MID_Atmosphere, TEXT("StormScatter"), TerrestrialCloudMaterial.StormScatter);
    SetVectorChecked(MID_Atmosphere, TEXT("StormExtinction"), TerrestrialCloudMaterial.StormExtinction);
}

void APlanetAtmosphereActor::ApplyNoiseLayer(const TCHAR* Prefix, const FAtmosphereNoiseLayerParams& Layer)
{
    auto Name = [Prefix](const TCHAR* Member) { return FName(FString(Prefix) + Member); };

    SetVectorChecked(MID_Atmosphere, Name(TEXT("NoiseWeights")), Layer.NoiseWeights);
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

bool APlanetAtmosphereActor::PrepareShadowTarget()
{
    UTextureRenderTarget2DArray* Target = ShadowTarget;

    if (!Target)
    {
        if (!bWarnedShadowTarget)
        {
            bWarnedShadowTarget = true;

            UE_LOG(LogTemp, Warning,
                TEXT("%s: no Gas Giant Shadow Target set. Create a Texture Render Target 2D ")
                TEXT("Array asset and assign it; the deck shadow bake has nowhere to write."),
                *GetName());
        }

        return false;
    }

    const int32 Edge = FMath::Clamp(ShadowResolution, 64, 4096);

    // bCanCreateUAV must be set BEFORE the resource is created, or the texture
    // comes back without UAV support and every dispatch that writes it silently
    // does nothing -- a black target with no warning anywhere.
    // ONE BAND, OR TWO WITH GEOMETRY OCCLUSION. The second band carries the
    // occluder term the march adds to the deck's optical depth, and the bake and
    // the reader both gate on this count -- so turning the feature off here is
    // what stops them paying for it, and the allocation is the only place the
    // decision lives.
    const int32 DesiredSlices =
        AtmoShadowBake::SlicesFor(GasGiantOccluderShadows.IsEnabled());

    const bool bMismatch =
        Target->SizeX != Edge ||
        Target->SizeY != Edge ||
        Target->Slices != DesiredSlices ||
        Target->OverrideFormat != PF_FloatRGBA ||
        !Target->bCanCreateUAV;

    if (bMismatch)
    {
        Target->bCanCreateUAV = true;
        Target->OverrideFormat = PF_FloatRGBA;
        Target->ClearColor = FLinearColor::Black;

        // The map stores depths in atmosphere thicknesses, which run well past 1
        // and reach the no-deck sentinel at 1000. A float format has no sRGB
        // variant so nothing clamps them here -- but the material's Texture
        // Object must still be set to Linear Color, which no flag can enforce.
        Target->Init(Edge, Edge, DesiredSlices, PF_FloatRGBA);
        Target->UpdateResourceImmediate(true);

        UE_LOG(LogTemp, Log, TEXT("%s: Gas Giant Shadow Target set to %dx%d x %d RGBA16F."),
            *GetName(), Edge, Edge, DesiredSlices);
    }

    bWarnedShadowTarget = false;

    return true;
}

// --------------------------------------------------------------------------
// Occluder depth captures
//
// One orthographic depth capture per cascade, looking down the light at the
// planet. The bake turns each texel's captured depth into the ray parameter at
// which an opaque surface blocks the light, and clamps the deck's four crossing
// nodes to it.
//
// THE CAPTURE DESCRIBES ITSELF. Its axes are read back off the component's own
// transform and travel to the bake in FAtmoOccluderFrame, so nothing here
// has to agree with GasGiantShadow.ush about where a cascade is. Matching the
// cascade's extent, resolution and texel snapping is what makes the resample an
// identity and keeps the two lattices moving together; a mismatch costs
// resolution and lattice stability, never placement.
//
// ORTHOGRAPHIC IS NOT A CHOICE OF FRAMING. The engine's perspective depth
// conversion carries an epsilon that saturates at planetary range -- half the
// true distance at 1000 km -- while the orthographic conversion is exact. A
// capture left perspective inherits that silently.
// --------------------------------------------------------------------------

/** GG_ShadowBasis's rule, on the CPU, in planet-local space. Anchored to the
 *  spin axis so the grid does not rotate as the light moves. Used only to place
 *  and snap the captures; the frame the bake reads comes off the component. */
static void OccluderBasisLocal(const FVector3f& L, FVector3f& OutU, FVector3f& OutV)
{
    FVector3f V = FVector3f(0.0f, 0.0f, 1.0f) - L * L.Z;

    if (V.SizeSquared() < 1e-6f)
    {
        V = FVector3f(1.0f, 0.0f, 0.0f) - L * L.X;
    }

    OutV = V.GetSafeNormal();
    OutU = FVector3f::CrossProduct(OutV, L);
}

/** Set once per component. The capture wants opaque depth and nothing else:
 *  every lit, translucent or post-processed feature is both wasted work and a
 *  chance for something that writes no depth to matter. */
static void ConfigureOccluderCapture(
    USceneCaptureComponent2D* Capture, const FGasGiantOccluderShadowParams& Settings)
{
    Capture->bCaptureEveryFrame = false;
    Capture->bCaptureOnMovement = false;

    // CaptureScene EARLY-OUTS ON IsVisible. A capture that is not visible is
    // not a capture that renders nothing -- it never runs at all, and leaves a
    // cleared target that reads as an occluder on the capture plane.
    Capture->SetVisibility(true);

    // The root carries the planet radius as scale. Nothing in the capture path
    // reads it, but a component sitting at 1e8 scale is a trap for anything
    // that later does.
    Capture->SetWorldScale3D(FVector::OneVector);

    // Manual cadence, so the view state is reused rather than rebuilt per call.
    Capture->bAlwaysPersistRenderingState = true;

    // Linear world-unit depth in R. PITFALL: the equivalent perspective path
    // saturates at range; see the note above.
    Capture->CaptureSource = Settings.bDebugColorCapture
        ? SCS_FinalColorLDR
        : SCS_SceneDepth;

    Capture->ProjectionType = ECameraProjectionMode::Orthographic;
    Capture->bUseCustomProjectionMatrix = true;

    Capture->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_RenderScenePrimitives;

    Capture->PostProcessBlendWeight = 0.0f;
    Capture->bEnableClipPlane = false;

    FEngineShowFlags& Flags = Capture->ShowFlags;

    // Lighting stays ON in the debug view: an unlit capture of an unlit scene
    // is a silhouette against a silhouette, which cannot be read.
    const bool bDebug = Settings.bDebugColorCapture;

    Flags.SetLighting(bDebug);
    Flags.SetDynamicShadows(false);
    Flags.SetSkyLighting(bDebug);
    Flags.SetAtmosphere(false);
    Flags.SetFog(false);
    Flags.SetVolumetricFog(false);
    Flags.SetPostProcessing(bDebug);
    Flags.SetBloom(false);
    Flags.SetAntiAliasing(false);
    Flags.SetMotionBlur(false);

    // Anything that does not write opaque depth contributes nothing and costs a
    // pass.
    Flags.SetTranslucency(false);
    Flags.SetParticles(false);
    Flags.SetDecals(false);
}

bool APlanetAtmosphereActor::PrepareGasGiantOccluderCaptures()
{
    if (BuiltType != EPlanetAtmosphereType::GasGiant || !GasGiantOccluderShadows.IsEnabled())
    {
        DestroyGasGiantOccluderCaptures();

        return false;
    }

    OccluderCaptures.SetNum(AtmoShadowBake::CascadeCount);
    OccluderDepthTargets.SetNum(AtmoShadowBake::CascadeCount);

    // The cascade's resolution, so the capture and the slice share a texel grid
    // and the resample is an identity.
    const int32 Edge = FMath::Clamp(ShadowResolution, 64, 4096);

    // R32F, NOT A HALF FORMAT. Eleven mantissa bits is a part in four thousand
    // of the distance -- kilometres at planetary range, against a comparison
    // measured in metres. RGBA8 only ever holds the debug view.
    const ETextureRenderTargetFormat Format = GasGiantOccluderShadows.bDebugColorCapture
        ? RTF_RGBA8
        : RTF_R32f;

    bool bAnyLive = false;

    for (int32 Level = 0; Level < AtmoShadowBake::CascadeCount; ++Level)
    {
        if (!GasGiantOccluderShadows.IsLevelEnabled(Level))
        {
            if (OccluderCaptures[Level])
            {
                OccluderCaptures[Level]->DestroyComponent();
                OccluderCaptures[Level] = nullptr;
            }

            OccluderDepthTargets[Level] = nullptr;
            OccluderFrames[Level] = FAtmoOccluderFrame();
            FramesSinceCapture[Level] = MAX_int32;

            continue;
        }

        UTextureRenderTarget2D* Target = OccluderDepthTargets[Level];

        const bool bRebuild = Target
            && (Target->SizeX != Edge || Target->SizeY != Edge
                || Target->RenderTargetFormat != Format);

        if (!Target || bRebuild)
        {
            if (!Target)
            {
                Target = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient);

                Target->ClearColor = FLinearColor::Black;
                Target->bAutoGenerateMips = false;
                Target->AddressX = TA_Clamp;
                Target->AddressY = TA_Clamp;

                OccluderDepthTargets[Level] = Target;
            }

            Target->RenderTargetFormat = Format;

            Target->InitAutoFormat(Edge, Edge);
            Target->UpdateResourceImmediate(true);

            // A rebuilt target is cleared, and a cleared R32F reads as depth
            // zero -- an occluder on the capture plane. The level stays invalid
            // until it has rendered again.
            FramesSinceCapture[Level] = MAX_int32;
            OccluderFrames[Level].bCaptured = false;
        }

        USceneCaptureComponent2D* Capture = OccluderCaptures[Level];

        if (!Capture)
        {
            Capture = NewObject<USceneCaptureComponent2D>(this, NAME_None, RF_Transient);

            Capture->SetupAttachment(AtmosphereRoot);
            Capture->RegisterComponent();

            OccluderCaptures[Level] = Capture;
        }

        // Reapplied rather than set once: the capture source and the show flags
        // follow the debug toggle, which is editable while the actor runs.
        ConfigureOccluderCapture(Capture, GasGiantOccluderShadows);

        Capture->TextureTarget = Target;

        bAnyLive = true;
    }

    return bAnyLive;
}

template<typename TShadowParams>
void APlanetAtmosphereActor::UpdateOccluderCaptures(
    float PlanetRadius, const FVector& PlanetCenter,
    const FVector3f& LightLocal, const FVector3f& CameraLocal,
    TShadowParams& Params)
{
    if (!PrepareGasGiantOccluderCaptures())
    {
        return;
    }

    const FVector AxisX = GetActorForwardVector();
    const FVector AxisY = GetActorRightVector();
    const FVector AxisZ = GetActorUpVector();

    auto ToLocal = [&AxisX, &AxisY, &AxisZ](const FVector& V)
        {
            return FVector3f(
                static_cast<float>(FVector::DotProduct(AxisX, V)),
                static_cast<float>(FVector::DotProduct(AxisY, V)),
                static_cast<float>(FVector::DotProduct(AxisZ, V)));
        };

    auto ToWorld = [&AxisX, &AxisY, &AxisZ](const FVector3f& V)
        {
            return AxisX * V.X + AxisY * V.Y + AxisZ * V.Z;
        };

    FVector3f BasisU, BasisV;
    OccluderBasisLocal(LightLocal, BasisU, BasisV);

    const FVector LightWorld = ToWorld(LightLocal);
    const FVector BasisUWorld = ToWorld(BasisU);
    const FVector BasisVWorld = ToWorld(BasisV);

    // A SUPERSET OF THE SHADER'S DISC, not a reproduction of it. The shader's
    // extent comes from GG_TopBounds, which is not worth a second derivation
    // here; the unfaded shell bounds it from above, so a capture sized to the
    // shell always covers the slice and the extra width costs a little
    // resolution. Levels 1 and 2 ARE the shader's expression, because there it
    // is an authored fade times a radius rather than a derivation.
    const float Disc = PlanetRadius * (1.0f + ActiveGeometry().HeightScale)
        * AtmoShadowBake::CaptureExtentMargin;

    const FAtmosphereNoiseLayerParams& Structure = ActiveStructureLayer();
    const FAtmosphereNoiseLayerParams& Detail = ActiveDetailLayer();

    const float StructureFar = PlanetRadius *
        (Structure.FadeNear + FMath::Max(Structure.FadeSpan, 1e-4f));

    const float DetailFar = PlanetRadius *
        (Detail.FadeNear + FMath::Max(Detail.FadeSpan, 1e-4f));

    float Extents[AtmoShadowBake::CascadeCount];
    Extents[0] = Disc;
    Extents[1] = FMath::Min(StructureFar, Extents[0]);
    Extents[2] = FMath::Min(DetailFar, Extents[1]);

    // EVERY LEVEL SHARES ONE PLANE, well off the planet along the light.
    //
    // THE NEAR PLANE IS AT THE CAPTURE, which the engine's own orthographic
    // captures assume too, so this distance is the ceiling on what can cast:
    // anything above it is clipped. Pulling the plane back rather than pushing
    // the near plane negative keeps every captured depth positive, which is what
    // lets zero stay the sentinel for a texel the capture never wrote.
    const float PlaneDist = Disc * FMath::Max(GasGiantOccluderShadows.CaptureDistanceScale, 1.1f);

    // A shell diameter past the planet centre, so the far side of the deck is
    // comfortably inside and background is unambiguous.
    const float Far = PlaneDist + 2.0f * Disc;

    const int32 Edge = FMath::Clamp(ShadowResolution, 64, 4096);

    for (int32 Level = 0; Level < AtmoShadowBake::CascadeCount; ++Level)
    {
        USceneCaptureComponent2D* Capture = OccluderCaptures[Level];

        if (!Capture || !OccluderDepthTargets[Level] || Extents[Level] <= 0.0f)
        {
            continue;
        }

        const float Extent = Extents[Level];

        // GG_ShadowCascadeCentre's snapping, at the same extent and resolution,
        // so the capture and the cascade slice step together. Unsnapped, the
        // occluder shadow crawls against the deck shadow around it as the
        // camera moves.
        FVector2f Centre = FVector2f::ZeroVector;

        if (Level > 0)
        {
            const float TexelSize = 2.0f * Extent / static_cast<float>(Edge);

            const FVector2f Plane(
                FVector3f::DotProduct(CameraLocal, BasisU),
                FVector3f::DotProduct(CameraLocal, BasisV));

            Centre = FVector2f(
                FMath::FloorToFloat(Plane.X / TexelSize) * TexelSize,
                FMath::FloorToFloat(Plane.Y / TexelSize) * TexelSize);
        }

        const FVector Location = PlanetCenter
            + LightWorld * PlaneDist
            + BasisUWorld * Centre.X
            + BasisVWorld * Centre.Y;

        // Looking down the direction light travels, pole up. The image's right
        // axis is whatever this rotation produces; it is read back below rather
        // than assumed, which is what makes a mirrored capture unexpressible.
        Capture->SetWorldLocationAndRotation(
            Location, FRotationMatrix::MakeFromXZ(-LightWorld, BasisVWorld).Rotator());

        Capture->OrthoWidth = 2.0f * Extent;

        // EXPLICIT CLIP PLANES. The engine's default orthographic far plane is
        // WORLD_MAX/8, which leaves the bake no way to tell background from
        // geometry. Near at the capture plane, far a shell diameter past the
        // planet centre.
        Capture->CustomProjectionMatrix =
            FReversedZOrthoMatrix(Extent, Extent, 1.0f / Far, 0.0f);

        Capture->MaxViewDistanceOverride = GasGiantOccluderShadows.MaxViewDistanceScale > 0.0f
            ? Far * GasGiantOccluderShadows.MaxViewDistanceScale
            : -1.0f;

        Capture->HiddenActors.Reset();

        for (const TObjectPtr<AActor>& Hidden : GasGiantOccluderShadows.HiddenActors)
        {
            if (Hidden)
            {
                Capture->HiddenActors.Add(Hidden);
            }
        }

        // The atmosphere's own actors. None of them render opaque depth, so
        // this is insurance rather than a fix.
        Capture->HiddenActors.Add(this);

        if (PostProcessVolume)
        {
            Capture->HiddenActors.Add(PostProcessVolume);
        }

        if (SunLight)
        {
            Capture->HiddenActors.Add(SunLight);
        }

        FAtmoOccluderFrame& Frame = OccluderFrames[Level];

        const int32 Interval = GasGiantOccluderShadows.GetIntervalFrames(Level);

        if (FramesSinceCapture[Level] < Interval)
        {
            ++FramesSinceCapture[Level];
        }
        else
        {
            Capture->CaptureScene();

            FramesSinceCapture[Level] = 1;

            // RECORDED FROM THE COMPONENT, AFTER IT MOVED, and only on the tick
            // it actually rendered. A level on a slow cadence then keeps the
            // placement its image was taken with; reusing this tick's placement
            // would drag that image across the deck as the light turns.
            Frame.U = ToLocal(Capture->GetRightVector());
            Frame.V = ToLocal(Capture->GetUpVector());

            const FVector3f OffsetLocal = ToLocal(Location - PlanetCenter);

            Frame.Centre = FVector2f(
                FVector3f::DotProduct(OffsetLocal, Frame.U),
                FVector3f::DotProduct(OffsetLocal, Frame.V));

            Frame.Extent = Extent;
            Frame.PlaneDist = PlaneDist;
            Frame.Far = Far;

            // The debug view holds colour, not depth. Left valid it would read
            // as an occluder a few centimetres off the capture plane across the
            // whole level and black the deck out.
            Frame.bCaptured = !GasGiantOccluderShadows.bDebugColorCapture;
        }

        // The resource, not the frame: a render target can be recreated under
        // us between captures, and a stale handle is the one part of this that
        // does not describe where the image was taken.
        if (FTextureRenderTargetResource* DepthRes =
            OccluderDepthTargets[Level]->GameThread_GetRenderTargetResource())
        {
            Frame.DepthTexture = DepthRes->GetRenderTargetTexture();
        }

        Params.Occluders[Level] = Frame;
    }
}

void APlanetAtmosphereActor::DestroyGasGiantOccluderCaptures()
{
    for (TObjectPtr<USceneCaptureComponent2D>& Capture : OccluderCaptures)
    {
        if (Capture)
        {
            Capture->DestroyComponent();
            Capture = nullptr;
        }
    }

    OccluderDepthTargets.Reset();

    for (int32 Level = 0; Level < AtmoShadowBake::CascadeCount; ++Level)
    {
        OccluderFrames[Level] = FAtmoOccluderFrame();
        FramesSinceCapture[Level] = MAX_int32;
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
    const FAtmosphereLightingParams& Air = ActiveAtmosphereLighting();

    FAtmosphereTransmittanceParams Params;
    Params.PlanetRadius = PlanetRadius;
    Params.AtmosphereRadius = ActiveGeometry().GetAtmosphereRadius(PlanetRadius);
    Params.ProfilePins = FVector4f(
        Air.RayleighScaleHeight, Air.MieScaleHeight, Air.AbsorptionAltitude, Air.AbsorptionFalloff);
    Params.Table = TableRes->GetRenderTargetTexture();

    // A null texture means the resource is still initialising. Not recorded as
    // baked, so the next tick tries again.
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
    const FVector& LightDir, FTextureRenderTargetResource*& OutFlowRes)
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

    OutFlowRes = Simulation.Config->FlowTarget->GameThread_GetRenderTargetResource();

    FTextureRenderTargetResource* MapRes =
        ShadowTarget->GameThread_GetRenderTargetResource();

    if (!OutFlowRes || !MapRes)
    {
        return false;
    }

    FTextureRenderTargetResource* FlowRes = OutFlowRes;

    // -- Frame --------------------------------------------------------------
    //
    // The planet's axes are the rows of WorldToLocal, exactly as the material
    // receives them, so the light and the camera arrive in the frame the field
    // is defined in. Spin is not applied here: GG_FlowProbe applies it.

    const FVector AxisX = GetActorForwardVector();
    const FVector AxisY = GetActorRightVector();
    const FVector AxisZ = GetActorUpVector();

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

    // PUSHED, NOT RE-DERIVED. The near map's centre is snapped to its own texel
    // grid, so the reconstruction has to snap from the same camera the bake did.
    // Deriving it from the material's own camera instead would differ by a frame,
    // and a frame is enough to land a whole texel out -- which is a jump in where
    // the map sits, not a smooth disagreement.
    SetVectorChecked(MID_Atmosphere, TEXT("ShadowCameraLocal"),
        FLinearColor(Params.CameraLocal.X, Params.CameraLocal.Y, Params.CameraLocal.Z, 0.0f));

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
    const FAtmosphereNoiseLayerParams& Structure = ActiveStructureLayer();
    const FAtmosphereNoiseLayerParams& Detail = ActiveDetailLayer();

    Params.PlanetRadius = PlanetRadius;
    Params.HeightScale = ActiveGeometry().HeightScale;
    Params.Time = GetGasGiantTime();

    // -- Extinction ---------------------------------------------------------

    Params.LightExtinctionFraction = ActiveExtinction().LightExtinctionFraction;

    // -- Volumes ------------------------------------------------------------
    //
    // RHI handles taken from the properties ApplyMarchParams already pushes,
    // not a second reference to the assets. A compute pass runs on the render
    // thread and cannot reach a UObject, so this is the only crossing available.

    if (Detail.Volume && Detail.Volume->GetResource())
    {
        Params.DetailTexture = Detail.Volume->GetResource()->TextureRHI;
    }

    if (Structure.Volume && Structure.Volume->GetResource())
    {
        Params.StructureTexture = Structure.Volume->GetResource()->TextureRHI;
    }

    Params.FlowTexture = FlowRes->GetRenderTargetTexture();

    Params.MapTexture = MapRes->GetRenderTargetTexture();

    // -- Occluders ----------------------------------------------------------
    //
    // A level with no capture leaves its frame invalid and the bake skips it,
    // so this cannot refuse the deck bake -- the difference is a planet with no
    // geometry shadows rather than a planet with no shadows.
    //
    // CaptureScene enqueues from here, during the actor tick; the bake goes out
    // from the subsystem's tick afterwards, so a capture taken this frame is
    // already in flight when the bake reads it. Nothing depends on that: the
    // frame travels with the image, so a stale capture is placed correctly.
    Params.OccluderSoftness = GasGiantOccluderShadows.EdgeWidth;
    Params.OccluderInset = GasGiantOccluderShadows.EdgeInset;
    Params.OccluderStrength = GasGiantOccluderShadows.Strength;
    Params.OccluderFalloff = GasGiantOccluderShadows.FalloffDistance;

    UpdateOccluderCaptures(
        PlanetRadius, PlanetCenter, Params.LightDir, Params.CameraLocal, Params);

    return true;
}

void APlanetAtmosphereActor::RequestShadowBake(
    float PlanetRadius, const FVector& PlanetCenter, const FVector& LightDir)
{
    UWorld* World = GetWorld();

    UFlowSimSubsystem* Sim = World ? World->GetSubsystem<UFlowSimSubsystem>() : nullptr;

    if (!Sim)
    {
        return;
    }

    // ONE REQUEST PER FIELD TYPE, because the two parameter structs are separate
    // types bound to separate shaders. What they hold is the same today and is
    // expected not to stay that way.
    const auto ToVector4 = [](const FLinearColor& C) { return FVector4f(C.R, C.G, C.B, C.A); };

    FTextureRenderTargetResource* FlowRes = nullptr;

    if (BuiltType == EPlanetAtmosphereType::GasGiant)
    {
        FGasGiantShadowParams Params;

        if (!FillSharedShadowParams(Params, PlanetRadius, PlanetCenter, LightDir, FlowRes))
        {
            return;
        }

        Params.DeckTop = GasGiantProfile.DeckTop;
        Params.CeilingFalloff = GasGiantProfile.CeilingFalloff;
        Params.GradientThickness = GasGiantProfile.GradientThickness;
        Params.DeckBackstop = GasGiantProfile.DeckBackstop;
        Params.DeckSlope = GasGiantProfile.DeckSlope;
        Params.DeckOpticalDepth = GasGiantProfile.DeckOpticalDepth;
        Params.DensityCurve = Extinction.DensityCurve;

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

        Params.StructureNoiseWeights = ToVector4(StructureLayer.NoiseWeights);
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

        Params.DetailNoiseWeights = ToVector4(DetailLayer.NoiseWeights);
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

        Sim->RequestShadowBake(Params);
    }
    else
    {
        FTerrestrialShadowParams Params;

        if (!FillSharedShadowParams(Params, PlanetRadius, PlanetCenter, LightDir, FlowRes))
        {
            return;
        }

        const FTerrestrialFieldPins Pins = PackTerrestrialField(
            TerrestrialProfile, TerrestrialMotion, TerrestrialStructureLayer, TerrestrialDetailLayer,
            TerrestrialJetRate(Simulation.Config), Params.Time);

        Params.CloudProfile = ToVector4(Pins.CloudProfile);
        Params.CloudCurves = ToVector4(Pins.CloudCurves);
        Params.CloudCoverage = ToVector4(Pins.CloudCoverage);
        Params.CloudType = ToVector4(Pins.CloudType);
        Params.CloudLid = ToVector4(Pins.CloudLid);
        Params.CloudLift = ToVector4(Pins.CloudLift);
        Params.CloudMotion = ToVector4(Pins.CloudMotion);
        Params.StructureNoiseWeights = ToVector4(Pins.StructureNoiseWeights);
        Params.StructureSampling = ToVector4(Pins.StructureSampling);
        Params.StructureWarp = ToVector4(Pins.StructureWarp);
        Params.DetailNoiseWeights = ToVector4(Pins.DetailNoiseWeights);
        Params.DetailSampling = ToVector4(Pins.DetailSampling);
        Params.DetailWarp = ToVector4(Pins.DetailWarp);
        Params.CloudDrift = ToVector4(Pins.CloudDrift);

        Params.CloudOpticalDepth = TerrestrialProfile.CloudOpticalDepth;
        Params.CloudExtinction = ToVector4(TerrestrialCloudMaterial.CloudExtinction);
        Params.StormExtinction = ToVector4(TerrestrialCloudMaterial.StormExtinction);

        Sim->RequestShadowBake(Params);
    }
}

// --------------------------------------------------------------------------
// Gas giant simulation
// --------------------------------------------------------------------------

void APlanetAtmosphereActor::StartFlowSimulation()
{
    if (!Simulation.Config)
    {
        UE_LOG(LogTemp, Warning,
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

float APlanetAtmosphereActor::GetGasGiantTime() const
{
    if (const UWorld* World = GetWorld())
    {
        if (const UFlowSimSubsystem* Sim = World->GetSubsystem<UFlowSimSubsystem>())
        {
            return Sim->GetSimulatedTime();
        }
    }
    return 0.0f;
}

// --------------------------------------------------------------------------
// Light update
// --------------------------------------------------------------------------

void APlanetAtmosphereActor::OrientToStar(const FVector& StarWorldPos)
{
    // Point the atmosphere's forward at the star, then let the existing rotation->light
    // sync propagate it to the directional light + raymarch MIDs. If illumination ends
    // up inverted, negate ToStar: a directional light's forward is the *travel*
    // direction (away from the star), not the direction toward it.
    //
    // PITFALL for gas giants: this also rotates the planet's local frame, which
    // is what localAxisX/Y/Z carry. Aiming the actor at a moving star therefore
    // spins the deck's spin axis with it. A planet whose axis must stay fixed
    // needs the light on a separate transform from the field's frame.
    const FVector ToStar = StarWorldPos - GetActorLocation();
    if (ToStar.IsNearlyZero()) return;
    SetActorRotation(ToStar.Rotation());
    UpdateLightFromRotation();
}

void APlanetAtmosphereActor::UpdateLightFromRotation()
{
    if (!SunLight) return;

    UDirectionalLightComponent* LightComp = SunLight->GetComponent();
    if (!LightComp) return;

    // Directional light faces opposite the light direction vector
    const FVector LightDir = GetRootComponent()->GetRelativeRotation().Vector();
    const FRotator SunRotation = (-LightDir).Rotation();
    SunLight->SetActorRotation(SunRotation);

    // Extract color and intensity from LightColor.
    // RGB = normalized color, magnitude of RGB = intensity multiplier.
    const FVector ColorVec(LightColor.R, LightColor.G, LightColor.B);
    const float Magnitude = ColorVec.Size();

    if (Magnitude > KINDA_SMALL_NUMBER)
    {
        const FLinearColor NormalizedColor(
            LightColor.R / Magnitude,
            LightColor.G / Magnitude,
            LightColor.B / Magnitude, 1.0f);
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
    // bEnabled drops the volume out of the post-process chain entirely -- the ray march
    // stops. Unbound volumes affect the whole camera regardless of actor visibility, so
    // this is what makes a pooled/dormant planet cost zero atmosphere GPU.
    if (PostProcessVolume) PostProcessVolume->bEnabled = bActive;
}