#include "PlanetAtmosphereActor.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Engine/VolumeTexture.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureRenderTarget2DArray.h"
#include "Engine/TextureRenderTarget2DArray.h"
#include "AtmosphereTransmittance.h"
#include "GasGiantShadowMap.h"
#include "GasGiantSimSubsystem.h"
#include "GasGiantSimTypes.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialTypes.h"

// --------------------------------------------------------------------------
// Default material and texture assets
//
// Constructor defaults for the soft references. Overridable per instance, so a
// relocated asset is a data edit rather than a code one.
// --------------------------------------------------------------------------

static const TCHAR* MatPath_Terrestrial = TEXT("/CloudAtmosphere/Material/MT_UCA_Default_Inst.MT_UCA_Default_Inst");
static const TCHAR* MatPath_GasGiant = TEXT("/CloudAtmosphere/Material/MT_UGA_Default_Inst.MT_UGA_Default_Inst");
static const TCHAR* MatPath_Postprocess = TEXT("/CloudAtmosphere/Material/MT_UCA_Postprocess_Inst.MT_UCA_Postprocess_Inst");

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

    // Default radius = max(OceanRadius, PlanetRadius) at planet defaults:
    // PlanetRadius(100M) + SeaLevel(0.5) * NoiseAmplitude(15M) = 107,500,000 cm
    SetActorScale3D(FVector(107500000.0));

    // Structs shared between instances take their gas giant defaults here on
    // the CDO rather than as member initialisers, so reset-to-default in the
    // details panel gives each instance its own.
    GasGiantStructureLayer = FGasGiantNoiseLayerParams::MakeStructureDefaults();
    GasGiantDetailLayer = FGasGiantNoiseLayerParams::MakeDetailDefaults();

    // The geometry struct's own default is the terrestrial shell.
    GasGiantGeometry.HeightScale = 1.0f;

    TerrestrialMarchMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(MatPath_Terrestrial));
    GasGiantMarchMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(MatPath_GasGiant));
    PostprocessMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(MatPath_Postprocess));

    // Default source assets, so both models display something in the details
    // panel and a fresh actor renders. A deck with no volumes is not a subtle
    // failure -- the carves and the erosion both go to their neutral values and
    // the deck comes out as a smooth shell.
    static ConstructorHelpers::FObjectFinder<UVolumeTexture> DefaultCloudTexture(
        TEXT("/VoxelPlugin/VolumeTextures/Textures/VT_PerlinWorley_Balanced"));
    if (DefaultCloudTexture.Succeeded())
    {
        Terrestrial.CloudVolumeTexture = DefaultCloudTexture.Object;
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("PlanetAtmosphereActor: Failed to load default cloud volume texture"));
    }

    static ConstructorHelpers::FObjectFinder<UVolumeTexture> DefaultDetailVolume(
        TEXT("/CloudAtmosphere/Noise/VT_PerlinWorley_S8_128"));
    if (DefaultDetailVolume.Succeeded())
    {
        GasGiantDetailLayer.Volume = DefaultDetailVolume.Object;
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("PlanetAtmosphereActor: Failed to load default deck detail volume"));
    }

    static ConstructorHelpers::FObjectFinder<UVolumeTexture> DefaultStructureVolume(
        TEXT("/CloudAtmosphere/Noise/VT_PerlinWorley_S4_128"));
    if (DefaultStructureVolume.Succeeded())
    {
        GasGiantStructureLayer.Volume = DefaultStructureVolume.Object;
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("PlanetAtmosphereActor: Failed to load default deck structure volume"));
    }

    static ConstructorHelpers::FObjectFinder<UGasGiantSimConfig> DefaultSimConfig(
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

    if (PlanetType == EPlanetAtmosphereType::GasGiant &&
        Simulation.bStartOnBeginPlay)
    {
        StartGasGiantSimulation();
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
            if (UGasGiantSimSubsystem* Sim = World->GetSubsystem<UGasGiantSimSubsystem>())
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

    if (BuiltType == EPlanetAtmosphereType::GasGiant)
    {
        ApplyGasGiantParams(PlanetRadius, PlanetCenter, LightDir);
        RequestGasGiantShadowBake(PlanetRadius, PlanetCenter, LightDir);
        UpdateTransmittanceTable(PlanetRadius);
    }
    else
    {
        const FAtmosphereCommonView Common = GetCommonParams();

        ApplyCommonParams(Common, PlanetRadius, PlanetCenter, LightDir);
        ApplyTerrestrialParams(Common);
    }

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

void APlanetAtmosphereActor::ApplyCommonParams(const FAtmosphereCommonView& Common, float PlanetRadius,
    const FVector& PlanetCenter, const FVector& LightDir)
{
    SetVectorChecked(MID_Atmosphere, TEXT("Planet Center"),
        FLinearColor(PlanetCenter.X, PlanetCenter.Y, PlanetCenter.Z, 0.0f));
    SetScalarChecked(MID_Atmosphere, TEXT("Planet Radius"), PlanetRadius);
    SetScalarChecked(MID_Atmosphere, TEXT("Atmosphere Height Scale"), Common.Geometry.HeightScale);

    SetVectorChecked(MID_Atmosphere, TEXT("Light Direction"),
        FLinearColor(LightDir.X, LightDir.Y, LightDir.Z, 0.0f));
    SetVectorChecked(MID_Atmosphere, TEXT("Light Color"), LightColor);

    SetVectorChecked(MID_Atmosphere, TEXT("Rayleigh Beta"), Common.AirScattering.RayleighBeta);

    SetVectorChecked(MID_Atmosphere, TEXT("Mie Beta"), Common.AirScattering.MieBeta);

    SetScalarChecked(MID_Atmosphere, TEXT("Mie G"), Common.AirScattering.MieG);
    SetVectorChecked(MID_Atmosphere, TEXT("Atmosphere Absorption Beta"), Common.AirScattering.AbsorptionBeta);

    SetScalarChecked(MID_Atmosphere, TEXT("Atmosphere Absorption Falloff"), Common.AirScattering.AbsorptionFalloff);
    SetVectorChecked(MID_Atmosphere, TEXT("Atmosphere Ambient"), Common.AirScattering.Ambient);

    SetVectorChecked(MID_Atmosphere, TEXT("Cloud Ambient"), Common.CloudScattering.Ambient);
    SetVectorChecked(MID_Atmosphere, TEXT("Cloud Phase Params"), Common.CloudScattering.PhaseParams);

    SetScalarChecked(MID_Atmosphere, TEXT("Atmosphere Steps"), Common.Raymarch.AtmosphereSteps);
    SetScalarChecked(MID_Atmosphere, TEXT("Step Scale Factor"), Common.Raymarch.StepScaleFactor);
    SetScalarChecked(MID_Atmosphere, TEXT("Cloud Steps"), Common.Raymarch.CloudSteps);
    SetScalarChecked(MID_Atmosphere, TEXT("View Step Pixels"), Common.Raymarch.ViewStepPixels);
}

void APlanetAtmosphereActor::ApplyTerrestrialParams(const FAtmosphereCommonView& Common)
{
    SetScalarChecked(MID_Atmosphere, TEXT("Cloud Outer Height Scale"),
        Terrestrial.GetOuterHeightScale(Common.Geometry.HeightScale));
    SetScalarChecked(MID_Atmosphere, TEXT("Cloud Inner Height Scale"),
        Terrestrial.GetInnerHeightScale(Common.Geometry.HeightScale));

    if (Terrestrial.CloudVolumeTexture)
    {
        SetTextureChecked(MID_Atmosphere, TEXT("Cloud Volume Texture"), Terrestrial.CloudVolumeTexture);
    }

    SetVectorChecked(MID_Atmosphere, TEXT("Animation Weights"), Terrestrial.AnimationWeights);
    SetScalarChecked(MID_Atmosphere, TEXT("Cloud Coverage"), Terrestrial.CloudCoverage);
    SetScalarChecked(MID_Atmosphere, TEXT("Cloud Density Multiplier"), Terrestrial.CloudDensityMultiplier);
    SetScalarChecked(MID_Atmosphere, TEXT("Cloud Height Curve Min"), Terrestrial.CloudHeightCurveMin);
    SetScalarChecked(MID_Atmosphere, TEXT("Cloud Height Curve Max"), Terrestrial.CloudHeightCurveMax);
    SetScalarChecked(MID_Atmosphere, TEXT("Cloud Noise Frequency"), Terrestrial.CloudNoiseFrequency);
    SetVectorChecked(MID_Atmosphere, TEXT("Cloud Noise Weights"), Terrestrial.CloudNoiseWeights);
    SetVectorChecked(MID_Atmosphere, TEXT("Cloud Noise Invert"), Terrestrial.CloudNoiseInvert);
    SetScalarChecked(MID_Atmosphere, TEXT("Detail Noise Frequency"), Terrestrial.GetDetailNoiseFrequency());
    SetVectorChecked(MID_Atmosphere, TEXT("Detail Noise Weights"), Terrestrial.DetailNoiseWeights);
    SetVectorChecked(MID_Atmosphere, TEXT("Detail Noise Invert"), Terrestrial.DetailNoiseInvert);
    SetScalarChecked(MID_Atmosphere, TEXT("Detail Erode Strength"), Terrestrial.DetailErodeStrength);

    SetVectorChecked(MID_Atmosphere, TEXT("Cloud Beta"), Terrestrial.CloudBeta);
    SetVectorChecked(MID_Atmosphere, TEXT("Cloud Absorption Beta"), Terrestrial.CloudAbsorptionBeta);
}

// READOUT ONLY, NEVER PUSHED. Mirrors GG_TopBounds' upper bound before its
// clamp at the shell, with the detail carve centred as GG_DETAIL_RELIEF_CENTRED's
// default has it. A mismatch misreports the readout and changes nothing drawn.
static float SolveTopMaxReadout(const FGasGiantProfileParams& Profile, const FGasGiantFlowParams& Flow,
    const FGasGiantNoiseLayerParams& Structure, const FGasGiantNoiseLayerParams& Detail)
{
    const float UpReach = 0.5f * FMath::Abs(Flow.BandRelief) + FMath::Abs(Flow.PressureRelief)
        + 0.5f * FMath::Abs(Structure.Relief) + 0.5f * FMath::Abs(Detail.Relief)
        + FMath::Abs(Flow.StormTowerRelief);

    return Profile.DeckTop + FMath::Max(Profile.GradientThickness, 1e-4f) * UpReach;
}

void APlanetAtmosphereActor::ApplyGasGiantParams(float PlanetRadius, const FVector& PlanetCenter, const FVector& LightDir)
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
    // camera it centred its cascades on goes out in RequestGasGiantShadowBake.

    if (Simulation.Config && Simulation.Config->FlowTarget)
    {
        SetTextureChecked(MID_Atmosphere, TEXT("FlowTarget"), Simulation.Config->FlowTarget);
    }

    if (GasGiantShadowTarget)
    {
        SetTextureChecked(MID_Atmosphere, TEXT("ShadowTarget"), GasGiantShadowTarget);
    }

    // -- Gas Giant ----------------------------------------------------------------

    SetScalarChecked(MID_Atmosphere, TEXT("HeightScale"), GasGiantGeometry.HeightScale);

    // -- Deck ---------------------------------------------------------------------

    GasGiantProfile.SolvedTopMax = SolveTopMaxReadout(
        GasGiantProfile, GasGiantFlow, GasGiantStructureLayer, GasGiantDetailLayer);

    SetScalarChecked(MID_Atmosphere, TEXT("DeckTop"), GasGiantProfile.DeckTop);
    SetScalarChecked(MID_Atmosphere, TEXT("CeilingFalloff"), GasGiantProfile.CeilingFalloff);
    SetScalarChecked(MID_Atmosphere, TEXT("GradientThickness"), GasGiantProfile.GradientThickness);
    SetScalarChecked(MID_Atmosphere, TEXT("DeckBackstop"), GasGiantProfile.DeckBackstop);

    SetScalarChecked(MID_Atmosphere, TEXT("BandSharpness"), GasGiantFlow.BandSharpness);
    SetScalarChecked(MID_Atmosphere, TEXT("BandBias"), GasGiantFlow.BandBias);
    SetScalarChecked(MID_Atmosphere, TEXT("HemisphereBlend"), GasGiantFlow.HemisphereBlend);
    SetScalarChecked(MID_Atmosphere, TEXT("HemisphereVariance"), GasGiantFlow.HemisphereVariance);
    SetScalarChecked(MID_Atmosphere, TEXT("BandRelief"), GasGiantFlow.BandRelief);
    SetScalarChecked(MID_Atmosphere, TEXT("PressureRelief"), GasGiantFlow.PressureRelief);
    SetScalarChecked(MID_Atmosphere, TEXT("VortexThreshold"), GasGiantFlow.VortexThreshold);
    SetScalarChecked(MID_Atmosphere, TEXT("StormTowerRelief"), GasGiantFlow.StormTowerRelief);
    SetScalarChecked(MID_Atmosphere, TEXT("ReliefThinning"), GasGiantFlow.ReliefThinning);
    SetScalarChecked(MID_Atmosphere, TEXT("RotationWeight"), GasGiantFlow.RotationWeight);

    SetScalarChecked(MID_Atmosphere, TEXT("WarpTime"), GasGiantMotion.WarpTime);
    SetScalarChecked(MID_Atmosphere, TEXT("DeepShearRatio"), GasGiantMotion.DeepShearRatio);
    SetScalarChecked(MID_Atmosphere, TEXT("TurbulenceFloor"), GasGiantMotion.TurbulenceFloor);
    SetScalarChecked(MID_Atmosphere, TEXT("CrossfadePeriod"), GasGiantMotion.CrossfadePeriod);

    SetScalarChecked(MID_Atmosphere, TEXT("EdgeBias"), GasGiantSurface.EdgeBias);
    SetScalarChecked(MID_Atmosphere, TEXT("ErosionDepth"), GasGiantSurface.ErosionDepth);

    ApplyGasGiantLayer(TEXT("Structure"), GasGiantStructureLayer);
    ApplyGasGiantLayer(TEXT("Detail"), GasGiantDetailLayer);

    // -- Atmosphere Lighting ------------------------------------------------------

    const FGasGiantAtmosphereLightingParams& Air = GasGiantAtmosphereLighting;

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

    // -- Cloud Lighting -----------------------------------------------------------

    SetVectorChecked(MID_Atmosphere, TEXT("ScatterNegative"), GasGiantBands.ScatterNegative);
    SetVectorChecked(MID_Atmosphere, TEXT("ExtinctionNegative"), GasGiantBands.ExtinctionNegative);
    SetVectorChecked(MID_Atmosphere, TEXT("ScatterPositive"), GasGiantBands.ScatterPositive);
    SetVectorChecked(MID_Atmosphere, TEXT("ExtinctionPositive"), GasGiantBands.ExtinctionPositive);
    SetVectorChecked(MID_Atmosphere, TEXT("ScatterBase"), GasGiantBands.ScatterBase);
    SetVectorChecked(MID_Atmosphere, TEXT("ExtinctionBase"), GasGiantBands.ExtinctionBase);
    SetScalarChecked(MID_Atmosphere, TEXT("BandScale"), GasGiantBands.BandScale);

    SetScalarChecked(MID_Atmosphere, TEXT("DensityCurve"), GasGiantExtinction.DensityCurve);
    SetScalarChecked(MID_Atmosphere, TEXT("DeckOpticalDepth"), GasGiantExtinction.DeckOpticalDepth);
    SetScalarChecked(MID_Atmosphere, TEXT("LightExtinctionFraction"), GasGiantExtinction.LightExtinctionFraction);

    SetScalarChecked(MID_Atmosphere, TEXT("ForwardG"), GasGiantPhase.ForwardG);
    SetScalarChecked(MID_Atmosphere, TEXT("BackwardG"), GasGiantPhase.BackwardG);
    SetScalarChecked(MID_Atmosphere, TEXT("ForwardWeight"), GasGiantPhase.ForwardWeight);
    SetVectorChecked(MID_Atmosphere, TEXT("CloudAmbient"), GasGiantPhase.CloudAmbient);
    SetScalarChecked(MID_Atmosphere, TEXT("CloudAmbientFloor"), GasGiantPhase.CloudAmbientFloor);

    SetScalarChecked(MID_Atmosphere, TEXT("OctaveCount"), static_cast<float>(GasGiantMultipleScattering.OctaveCount));
    SetScalarChecked(MID_Atmosphere, TEXT("OctaveAttenuation"), GasGiantMultipleScattering.OctaveAttenuation);
    SetScalarChecked(MID_Atmosphere, TEXT("OctaveContribution"), GasGiantMultipleScattering.OctaveContribution);
    SetScalarChecked(MID_Atmosphere, TEXT("OctaveEccentricity"), GasGiantMultipleScattering.OctaveEccentricity);

    SetScalarChecked(MID_Atmosphere, TEXT("TerminatorSoftness"), GasGiantTerminator.TerminatorSoftness);
    SetScalarChecked(MID_Atmosphere, TEXT("AmbientTerminator"), GasGiantTerminator.AmbientTerminator);
    SetScalarChecked(MID_Atmosphere, TEXT("MieLobeDecay"), GasGiantTerminator.MieLobeDecay);
    SetScalarChecked(MID_Atmosphere, TEXT("LobeShadowPower"), GasGiantTerminator.LobeShadowPower);

    // -- Pipeline -----------------------------------------------------------------

    SetScalarChecked(MID_Atmosphere, TEXT("AtmosphereSteps"), GasGiantRaymarch.AtmosphereSteps);
    SetScalarChecked(MID_Atmosphere, TEXT("CloudSteps"), GasGiantRaymarch.CloudSteps);
    SetScalarChecked(MID_Atmosphere, TEXT("StepScaleFactor"), GasGiantRaymarch.StepScaleFactor);
    SetScalarChecked(MID_Atmosphere, TEXT("ViewStepPixels"), GasGiantRaymarch.ViewStepPixels);
    SetScalarChecked(MID_Atmosphere, TEXT("DeckSlope"), GasGiantRaymarch.DeckSlope);
}

void APlanetAtmosphereActor::ApplyGasGiantLayer(const TCHAR* Prefix, const FGasGiantNoiseLayerParams& Layer)
{
    auto Name = [Prefix](const TCHAR* Member) { return FName(FString(Prefix) + Member); };

    if (Layer.Volume)
    {
        SetTextureChecked(MID_Atmosphere, Name(TEXT("Volume")), Layer.Volume);
    }

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

bool APlanetAtmosphereActor::PrepareGasGiantShadowTarget()
{
    UTextureRenderTarget2DArray* Target = GasGiantShadowTarget;

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

    const int32 Edge = FMath::Clamp(GasGiantShadowResolution, 64, 4096);

    // bCanCreateUAV must be set BEFORE the resource is created, or the texture
    // comes back without UAV support and every dispatch that writes it silently
    // does nothing -- a black target with no warning anywhere.
    const bool bMismatch =
        Target->SizeX != Edge ||
        Target->SizeY != Edge ||
        Target->Slices != GasGiantShadow::CascadeCount ||
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
        Target->Init(Edge, Edge, GasGiantShadow::CascadeCount, PF_FloatRGBA);
        Target->UpdateResourceImmediate(true);

        UE_LOG(LogTemp, Log, TEXT("%s: Gas Giant Shadow Target set to %dx%d x %d RGBA16F."),
            *GetName(), Edge, Edge, GasGiantShadow::CascadeCount);
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

    // The values ApplyGasGiantParams pushes: the table is keyed to them, and
    // AtmoT_Profile converts them on both sides.
    const FGasGiantAtmosphereLightingParams& Air = GasGiantAtmosphereLighting;

    FAtmosphereTransmittanceParams Params;
    Params.PlanetRadius = PlanetRadius;
    Params.AtmosphereRadius = GasGiantGeometry.GetAtmosphereRadius(PlanetRadius);
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

void APlanetAtmosphereActor::RequestGasGiantShadowBake(
    float PlanetRadius, const FVector& PlanetCenter, const FVector& LightDir)
{
    UWorld* World = GetWorld();

    if (!World || !PrepareGasGiantShadowTarget())
    {
        return;
    }

    UGasGiantSimSubsystem* Sim = World->GetSubsystem<UGasGiantSimSubsystem>();

    if (!Sim || !Simulation.Config || !Simulation.Config->FlowTarget)
    {
        return;
    }

    FTextureRenderTargetResource* FlowRes =
        Simulation.Config->FlowTarget->GameThread_GetRenderTargetResource();

    FTextureRenderTargetResource* MapRes =
        GasGiantShadowTarget->GameThread_GetRenderTargetResource();

    if (!FlowRes || !MapRes)
    {
        return;
    }

    FGasGiantShadowParams Params;

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
    Params.MapSize = FIntPoint(GasGiantShadowTarget->SizeX, GasGiantShadowTarget->SizeY);

    // -- Deck ---------------------------------------------------------------
    //
    // GG_BuildField's arguments, under the names ApplyGasGiantParams pushes them
    // to the material. The bake derives from them with the same shader
    // functions, so any divergence here is a deck the light sees and the eye
    // does not.

    const FGasGiantNoiseLayerParams& Structure = GasGiantStructureLayer;
    const FGasGiantNoiseLayerParams& Detail = GasGiantDetailLayer;

    Params.PlanetRadius = PlanetRadius;
    Params.HeightScale = GasGiantGeometry.HeightScale;
    Params.Time = GetGasGiantTime();

    Params.DeckTop = GasGiantProfile.DeckTop;
    Params.CeilingFalloff = GasGiantProfile.CeilingFalloff;
    Params.GradientThickness = GasGiantProfile.GradientThickness;
    Params.DeckBackstop = GasGiantProfile.DeckBackstop;
    Params.DensityCurve = GasGiantExtinction.DensityCurve;

    Params.BandSharpness = GasGiantFlow.BandSharpness;
    Params.BandBias = GasGiantFlow.BandBias;
    Params.HemisphereBlend = GasGiantFlow.HemisphereBlend;
    Params.HemisphereVariance = GasGiantFlow.HemisphereVariance;
    Params.BandRelief = GasGiantFlow.BandRelief;
    Params.PressureRelief = GasGiantFlow.PressureRelief;
    Params.VortexThreshold = GasGiantFlow.VortexThreshold;
    Params.StormTowerRelief = GasGiantFlow.StormTowerRelief;
    Params.ReliefThinning = GasGiantFlow.ReliefThinning;
    Params.RotationWeight = GasGiantFlow.RotationWeight;

    Params.WarpTime = GasGiantMotion.WarpTime;
    Params.DeepShearRatio = GasGiantMotion.DeepShearRatio;
    Params.TurbulenceFloor = GasGiantMotion.TurbulenceFloor;
    Params.CrossfadePeriod = GasGiantMotion.CrossfadePeriod;

    Params.EdgeBias = GasGiantSurface.EdgeBias;
    Params.ErosionDepth = GasGiantSurface.ErosionDepth;

    const auto ToVector4 = [](const FLinearColor& C) { return FVector4f(C.R, C.G, C.B, C.A); };

    Params.StructureNoiseWeights = ToVector4(Structure.NoiseWeights);
    Params.StructureScale = Structure.Scale;
    Params.StructureAspect = Structure.Aspect;
    Params.StructureRelief = Structure.Relief;
    Params.StructureErosion = Structure.Erosion;
    Params.StructureFlowInherit = Structure.FlowInherit;
    Params.StructureShearInherit = Structure.ShearInherit;
    Params.StructureFadeNear = Structure.FadeNear;
    Params.StructureFadeSpan = Structure.FadeSpan;
    Params.StructureBandMix = Structure.BandMix;
    Params.StructureCrossfade = Structure.bCrossfade ? 1.0f : 0.0f;

    Params.DetailNoiseWeights = ToVector4(Detail.NoiseWeights);
    Params.DetailScale = Detail.Scale;
    Params.DetailAspect = Detail.Aspect;
    Params.DetailRelief = Detail.Relief;
    Params.DetailErosion = Detail.Erosion;
    Params.DetailFlowInherit = Detail.FlowInherit;
    Params.DetailShearInherit = Detail.ShearInherit;
    Params.DetailFadeNear = Detail.FadeNear;
    Params.DetailFadeSpan = Detail.FadeSpan;
    Params.DetailBandMix = Detail.BandMix;
    Params.DetailCrossfade = Detail.bCrossfade ? 1.0f : 0.0f;

    Params.DeckSlope = GasGiantRaymarch.DeckSlope;

    // -- Extinction ---------------------------------------------------------

    Params.ExtinctionNegative = ToVector4(GasGiantBands.ExtinctionNegative);
    Params.ExtinctionPositive = ToVector4(GasGiantBands.ExtinctionPositive);
    Params.ExtinctionBase = ToVector4(GasGiantBands.ExtinctionBase);
    Params.BandScale = GasGiantBands.BandScale;
    Params.DeckOpticalDepth = GasGiantExtinction.DeckOpticalDepth;
    Params.LightExtinctionFraction = GasGiantExtinction.LightExtinctionFraction;

    // -- Volumes ------------------------------------------------------------
    //
    // RHI handles taken from the properties ApplyGasGiantParams already pushes,
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

    Sim->RequestShadowBake(Params);
}

// --------------------------------------------------------------------------
// Gas giant simulation
// --------------------------------------------------------------------------

void APlanetAtmosphereActor::StartGasGiantSimulation()
{
    if (!Simulation.Config)
    {
        UE_LOG(LogTemp, Warning,
            TEXT("PlanetAtmosphereActor: gas giant with no SimConfig. The deck will render against "
                "an unbound flow field, which looks like flat horizontal stripes."));
        return;
    }

    UWorld* World = GetWorld();
    if (!World) return;

    UGasGiantSimSubsystem* Sim = World->GetSubsystem<UGasGiantSimSubsystem>();
    if (!Sim) return;

    Sim->StartSimulation(Simulation.Config);
    bStartedSimulation = true;
}

float APlanetAtmosphereActor::GetGasGiantTime() const
{
    if (const UWorld* World = GetWorld())
    {
        if (const UGasGiantSimSubsystem* Sim = World->GetSubsystem<UGasGiantSimSubsystem>())
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