// THE TRANSFORM IS THE INTERFACE.
//
//   Actor Location  -> planet centre / atmosphere centre
//   Actor Scale max -> planet radius
//   Actor Rotation  -> light direction, and the directional light's rotation
//
// When owned by APlanetActor the scale is set externally to
// max(OceanRadius, PlanetRadius), the visible surface floor.
//
// THREE MODELS SHARE ONE FIELD AND ONE MARCH, run as compute passes with a
// temporal resolve (FAtmosphereViewExtension), the sim as their weather map: a
// terrestrial slab over a surface, a gas giant's deep deck over a saturated
// core, or the terrestrial air alone. The two cloud models each have a
// parameter bundle and sim config (AtmosphereParams.h); air only uses the
// terrestrial ones. The nearest claiming planet drives the world's one sim;
// the others draw the field they kept (UFlowSimSubsystem).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/VolumeTexture.h"
#include "AtmosphereParams.h"
#include "AtmosphereTransmittance.h"
#include "AtmosphereShadowBake.h"
#include "PlanetAtmosphereActor.generated.h"

class UTexture2D;
class UTextureRenderTarget2D;
class UTextureRenderTarget2DArray;
class FAtmosphereViewExtension;
class UFlowSimConfig;
struct FAtmosphereMarchParams;
struct FTerrestrialShadowParams;

/** The clocks a planet's field is drawn at: the sim's while the planet drives
 *  it, the kept field's otherwise. SIM TIME, NOT WORLD TIME: the field is
 *  coherent against the sim's own clock, which pauses, steps by hand and
 *  restores from snapshots. Double; the field's clocks reduce from it before
 *  narrowing. */
struct FAtmosphereFieldClock
{
    /** Sim time of the field shown, which the noise's drift and phases follow. */
    double Time = 0.0;

    /** Sim time the planet's spin turns the field by. Runs on while the field
     *  is kept, so a kept field still turns with its planet. */
    double SpinTime = 0.0;

    /** The config the field was simulated under, whose noise clocks it reads. */
    TWeakObjectPtr<const UFlowSimConfig> Config;
};

/** Renders a volumetric atmosphere and cloud layer. A scene view extension runs
 *  the march, its temporal resolve and the composite as compute passes, fed a
 *  params snapshot of the active model's groups every tick by UpdateAtmosphere,
 *  with the shadow bake and the transmittance table. Owns a directional light
 *  component, synced from the actor's rotation and LightColor. When
 *  planet-owned, location and scale are locked and rotation stays editable.
 *  Its panel shows CloudAtmosphere first after the transform (the module's
 *  details customization), its groups in declaration order: Atmosphere,
 *  Model, Pipeline. */
UCLASS()
class CLOUDATMOSPHERE_API APlanetAtmosphereActor : public AActor
{
    GENERATED_BODY()

public:
    APlanetAtmosphereActor();

    // --- Atmosphere ---

    /** True when spawned and driven by APlanetActor. Location and scale become
     *  read-only; rotation remains editable (controls light direction). */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CloudAtmosphere|Atmosphere")
    bool bIsPlanetOwned = false;

    /** Which model renders: the terrestrial slab, the gas giant's deep deck,
     *  or the terrestrial air alone. A change rebakes the shadow map and
     *  restarts the view histories on the next tick. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CloudAtmosphere|Atmosphere")
    EPlanetAtmosphereType PlanetType = EPlanetAtmosphereType::GasGiant;

    /** Switches the model; takes effect as PlanetType's edit does. */
    UFUNCTION(BlueprintCallable, Category = "CloudAtmosphere")
    void SetPlanetType(EPlanetAtmosphereType InType);

    /** The star's colour. Only its hue is read: the brightest channel counts as
     *  1. The march and the directional light both derive from LightProduct, so
     *  they cannot disagree about the star; light DIRECTION comes from the
     *  actor's rotation. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Atmosphere", meta = (HideAlphaChannel))
    FLinearColor LightColor = FLinearColor(1.0f, 0.95f, 0.9f, 1.0f);

    /** The star's brightness, on its brightest channel. Every ambient is a ratio
     *  of the light, so they follow it. Light is stored unexposed in half float,
     *  so the haze's forward lobe clips past about 1000 at MieG 0.95 and about
     *  40 at its 0.99 limit. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Atmosphere", meta = (ClampMin = "0.0", UIMax = "20.0"))
    float LightIntensity = 4.0f;

    /** LightColor's hue at LightIntensity: the light the march and the
     *  directional light take. */
    FLinearColor LightProduct() const
    {
        const float Peak = FMath::Max3(LightColor.R, LightColor.G, LightColor.B);
        const float Scale = (Peak > 0.0f) ? FMath::Max(LightIntensity, 0.0f) / Peak : 0.0f;

        return FLinearColor(LightColor.R * Scale, LightColor.G * Scale, LightColor.B * Scale, 1.0f);
    }

    /** Parks or wakes the passes, the light and the per-tick updates. THE ONLY
     *  OFF-SWITCH for the march: hiding the actor or stopping its tick leaves
     *  it drawing. Parking gives up the sim, keeping the field; waking rebakes
     *  every shadow level. */
    UFUNCTION(BlueprintCallable, Category = "CloudAtmosphere")
    void SetAtmosphereActive(bool bActive);

    UFUNCTION(BlueprintPure, Category = "CloudAtmosphere")
    bool IsAtmosphereActive() const { return bAtmosphereActive; }

    /** Aim the light and the march at the star: sets the actor's relative
     *  rotation, which is the light direction, toward StarWorldPos and runs the
     *  rotation-to-light sync. The cloud field keeps the planet's frame. Called
     *  each frame by the owning planet from IStarLit::SetStarWorldPosition. */
    UFUNCTION(BlueprintCallable, Category = "CloudAtmosphere")
    void OrientToStar(const FVector& StarWorldPos);

    // --- Parameters ---
    //
    // ONE BUNDLE PER MODEL, each shown only while its model is active. A
    // group's default lives with its struct; the constructor sets the
    // terrestrial air, ambient, multiple scattering and surface shadows on the
    // CDO, which the gas giant copies.

    /** The terrestrial model's, which air only draws without its clouds. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Model", meta = (EditCondition = "PlanetType != EPlanetAtmosphereType::GasGiant", EditConditionHides))
    FAtmosphereModelParams Terrestrial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Model", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides))
    FAtmosphereModelParams GasGiant;

    /** A model's bundle, as a copy. */
    UFUNCTION(BlueprintPure, Category = "CloudAtmosphere")
    FAtmosphereModelParams GetModelParams(EPlanetAtmosphereType InModel) const;

    /** Replaces a model's bundle; the next tick pushes it and rebakes the
     *  shadows if the field changed. Which groups the model shows is kept. */
    UFUNCTION(BlueprintCallable, Category = "CloudAtmosphere")
    void SetModelParams(EPlanetAtmosphereType InModel, const FAtmosphereModelParams& InParams);

    /** A model's sim config to change at runtime. In a game world the first
     *  call replaces the slot's shared asset with a transient copy owned by
     *  this actor, which a sim running the active model's slot carries on
     *  under, so no other planet
     *  and no asset sees the change. In an editor world it is the asset,
     *  which is how tuning saves. Null when the slot is empty. */
    UFUNCTION(BlueprintCallable, Category = "CloudAtmosphere")
    UFlowSimConfig* GetWritableSimConfig(EPlanetAtmosphereType InModel);

    // --- Pipeline: the assets and passes the actor drives. ---

    /** Tiling single-channel blue noise for the march: sRGB off, uncompressed
     *  grayscale, no mips, nearest filtering. Cleared, nothing draws. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CloudAtmosphere|Pipeline")
    TObjectPtr<UTexture2D> BlueNoise;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline")
    FAtmosphereSimulationParams Simulation;

    // Quality: Raymarch, Sampling and the shadow settings below are a
    // performance tier, shared by both models; tunes apply them only on request.

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline|Raymarch", meta = (ShowOnlyInnerProperties))
    FAtmosphereRaymarchParams Raymarch;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline|Sampling", meta = (ShowOnlyInnerProperties))
    FAtmosphereSamplingParams Sampling;

    // Baked Lighting: set once for a performance tier. The cascade extents are
    // look, in each model's SurfaceShadow.

    /** The deck shadow bake in the light's frame: a cascade of slices, each
     *  covering a smaller radius, sized by ShadowResolution. */
    UPROPERTY(Transient, VisibleInstanceOnly, BlueprintReadOnly, Category = "CloudAtmosphere|Pipeline|Baked Lighting")
    TObjectPtr<UTextureRenderTarget2DArray> ShadowTarget;

    /** One texel: the coverage priority threshold the bake's coverage pass
     *  solves each frame, read by the bake and the march. */
    UPROPERTY(Transient, VisibleInstanceOnly, BlueprintReadOnly, Category = "CloudAtmosphere|Pipeline|Baked Lighting")
    TObjectPtr<UTextureRenderTarget2D> CoverageTarget;

    /** The flow atlas this planet draws: the sim's while it drives the sim,
     *  KeptFlow while another planet does. */
    UPROPERTY(Transient, VisibleInstanceOnly, BlueprintReadOnly, Category = "CloudAtmosphere|Pipeline")
    TObjectPtr<UTextureRenderTarget2DArray> FlowTarget;

    /** The field as this planet last drove it, copied when another planet took
     *  the sim; clear until it first drives it. */
    UPROPERTY(Transient, VisibleInstanceOnly, BlueprintReadOnly, Category = "CloudAtmosphere|Pipeline")
    TObjectPtr<UTextureRenderTarget2DArray> KeptFlow;

    /** The sim's debug view while this planet drives the sim and the config's
     *  bDebugView is on. */
    UPROPERTY(Transient, VisibleInstanceOnly, BlueprintReadOnly, Category = "CloudAtmosphere|Pipeline")
    TObjectPtr<UTextureRenderTarget2D> SimDebugView;

    /** Edge of every cascade slice, in texels. Bake time and memory scale with
     *  its square, 2 MB per slice at 512; lower softens rather than aliases. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline|Baked Lighting", meta = (ClampMin = "128", ClampMax = "4096"))
    int32 ShadowResolution = 1024;

    /** Cascades rebaked per frame, in turn; each is read against the camera it
     *  was baked with. 1 rebakes a level every third frame. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline|Baked Lighting", meta = (ClampMin = "1", ClampMax = "3"))
    int32 ShadowLevelsPerFrame = 1;

    /** Seconds a rebaked cascade fades in from its reprojected previous bake:
     *  hides rebake steps, and the shadow trails the clouds by about this. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline|Baked Lighting", meta = (ClampMin = "0.0", UIMax = "1.0"))
    float ShadowTemporalSmoothing = 0.1f;

    // --- The active model ---

    /** The sim config the active model bids with. */
    UFlowSimConfig* ActiveSimConfig() const;

    /** The clocks this frame's field is drawn at. */
    const FAtmosphereFieldClock& GetFieldClock() const
    {
        return FieldClock;
    }

    const FAtmosphereModelParams& ActiveModel() const
    {
        return ModelOf(PlanetType);
    }

    /** The model draws clouds, and so runs the sim and the shadow bake. */
    bool HasClouds() const { return PlanetType != EPlanetAtmosphereType::AirOnly; }

    /** The clouds are the gas giant's deep deck rather than the slab. */
    bool IsDeepDeck() const { return PlanetType == EPlanetAtmosphereType::GasGiant; }

    /** Above zero: the atmosphere's thickness divides every height. */
    float ActiveHeightScale() const
    {
        return FMath::Max(ActiveModel().Planet.HeightScale, 1e-4f);
    }

    /** The noise volumes, which both models bind under the same names. */
    UVolumeTexture* ActiveStructureVolume() const
    {
        return ActiveModel().StructureLayer.Volume.Get();
    }

    UVolumeTexture* ActiveDetailVolume() const
    {
        return ActiveModel().DetailLayer.Volume.Get();
    }

    // --- Lifecycle ---

    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Destroyed() override;
    virtual void BeginDestroy() override;
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void Tick(float DeltaTime) override;

    // --- Editor Property & Transform Locking ---

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
    virtual void PostEditMove(bool bFinished) override;
    virtual void EditorApplyTranslation(const FVector& DeltaTranslation, bool bAltDown, bool bShiftDown, bool bCtrlDown) override;
    virtual void EditorApplyScale(const FVector& DeltaScale, const FVector* PivotLocation, bool bAltDown, bool bShiftDown, bool bCtrlDown) override;
#endif

    /** Called by PlanetActor after spawn and attach. Sets bIsPlanetOwned, binds
     *  the transform guard and runs Initialize, with InScale applied first.
     *  Skips the deferred OnConstruction path. */
    UFUNCTION(BlueprintCallable, Category = "CloudAtmosphere")
    void InitializeFromPlanet(USceneComponent* InAttachParent,
        FVector InScale = FVector::ZeroVector);

private:
    /** Root component; carries the planet radius as scale. */
    UPROPERTY()
    TObjectPtr<USceneComponent> AtmosphereRoot;

    /** Synced from the actor's rotation and LightColor, in absolute rotation
     *  and scale. A component, so a duplicate or deletion takes its own. */
    UPROPERTY(VisibleAnywhere, Category = "CloudAtmosphere|Pipeline")
    TObjectPtr<UDirectionalLightComponent> SunLightComponent;

    /** Legacy child actors a saved level can still hold, destroyed on
     *  Initialize. */
    UPROPERTY()
    TObjectPtr<APostProcessVolume> PostProcessVolume_DEPRECATED = nullptr;

    UPROPERTY()
    TObjectPtr<ADirectionalLight> SunLight_DEPRECATED = nullptr;

    /** Air transmittance table, created on first use. */
    UPROPERTY(Transient, VisibleInstanceOnly, Category = "CloudAtmosphere|Pipeline|Baked Lighting")
    TObjectPtr<UTextureRenderTarget2D> TransmittanceTable = nullptr;

    /** Inputs of the last enqueued bake; reset when the table is recreated. */
    FAtmosphereTransmittanceParams TransmittanceBaked;

    /** A model's bundle and sim config slot: air only shares the
     *  terrestrial model's. */
    const FAtmosphereModelParams& ModelOf(EPlanetAtmosphereType InModel) const;
    FAtmosphereModelParams& ModelOf(EPlanetAtmosphereType InModel);
    TObjectPtr<UFlowSimConfig>& SimConfigSlot(EPlanetAtmosphereType InModel);

    /** Sets each bundle's transient flags from the model: which groups the
     *  panel shows. */
    void SyncModelFlags();

    /** Frees what only the clouds use: the shadow and coverage targets, and
     *  the sim claim. */
    void ReleaseCloudResources();

    /** The model the shadow map was last baked for: a change rebakes every
     *  level. */
    EPlanetAtmosphereType ShadowType = EPlanetAtmosphereType::Terrestrial;

    bool bInitialized = false;

    /** Initialize runs on the next Tick outside a preview world. True from
     *  construction, so a loaded actor initialises without OnConstruction. */
    bool bPendingInitialize = true;

    /** False while parked by SetAtmosphereActive. */
    bool bAtmosphereActive = true;

    /** True while this actor bids for the sim, so it releases the sim when it
     *  stops or goes. */
    bool bClaimedSimulation = false;

    FAtmosphereFieldClock FieldClock;

    /** The clock KeptFlow was copied at, and the world time then: what a kept
     *  field is drawn at. */
    FAtmosphereFieldClock KeptClock;
    double KeptAt = 0.0;

    /** The frame the last shadow bake was requested on: one request per frame
     *  however many paths push parameters. */
    uint64 ShadowBakeFrame = MAX_uint64;

    /** The root's scale relative to the planet, as the planet set it: what the
     *  transform guard holds. */
    FVector PlanetDrivenScale = FVector::OneVector;

    /** Bound to RootComponent->TransformUpdated when planet-owned. Snaps location
     *  and scale back to planet-driven values, leaving rotation alone. */
    void OnTransformUpdated(USceneComponent* Component, EUpdateTransformFlags Flags, ETeleportType Teleport);

    /** Destroys legacy objects, pushes every parameter and syncs the light. */
    void Initialize();

    /** Destroys the legacy child actors and components a saved level can still
     *  hold. */
    void DestroyLegacyChildActors();

    /** Withdraws this actor's bid, stopping the sim if it drives it;
     *  bKeepField keeps its field first. */
    void ReleaseSimulation(bool bKeepField);

    /** Bids for the sim with the active model's config, and picks what this
     *  frame draws: the sim's flow and clock while this planet drives it, the
     *  kept ones otherwise. */
    void ClaimSimulation(const FVector& PlanetCenter, float PlanetRadius);

    /** Runs the march, its temporal resolve and the composite as compute
     *  passes for this world's views. Created on first use, released on the
     *  render thread. */
    TSharedPtr<FAtmosphereViewExtension, ESPMode::ThreadSafe> ViewExtension;

    /** Suppresses the per-tick repeat of the missing blue noise warning. */
    bool bWarnedBlueNoise = false;

    /** Hands the view extension this frame's march while its resources are
     *  set, and enables it to match. */
    void UpdateComputeMarch(float PlanetRadius, const FVector& PlanetCenter, const FVector& LightDir);

    /** The active model's march inputs, from the packers the shadow bake
     *  shares. False when a resource the march needs is missing. */
    bool FillMarchParams(FAtmosphereMarchParams& Out, float PlanetRadius,
        const FVector& PlanetCenter, const FVector& LightDir);

    /** Disables the view extension and drops it on the render thread, with its
     *  histories. */
    void ReleaseViewExtension();

    /** The shadow bake, the transmittance table and the march, for the active
     *  model. Called every tick. */
    void UpdateAtmosphere();

    /** Queues this frame's shadow bake with the sim subsystem, on the field
     *  packer and shader functions the march shares. */
    void RequestShadowBake(float PlanetRadius, const FVector& PlanetCenter, const FVector& LightDir);

    /** A bake request's map, frame, light, camera, history and volumes; false
     *  when unusable. CommitShadowBake records it once taken. */
    bool FillShadowRequest(FTerrestrialShadowParams& Params, float PlanetRadius,
        const FVector& PlanetCenter, const FVector& LightDir);

    /** Records an accepted bake: the cameras the march reads the fine levels
     *  against, and the light and time the next bake reprojects from. */
    void CommitShadowBake(uint32 LevelMask, const FVector3f& LightDir, const FVector3f& CameraLocal);

    /** Creates the shadow target if absent and forces it to RGBA16F with UAV
     *  support at ShadowResolution, cleared to the no-deck sentinel,
     *  reinitialising only on a mismatch. */
    void PrepareShadowTarget();

    /** Creates the one-texel R32F coverage target if absent, cleared above any
     *  priority. */
    void PrepareCoverageTarget();

    /** The next cascade in the bake rotation, and the camera each level was
     *  last baked around -- what the march reads that level against. */
    int32 ShadowLevelCursor = 0;
    FVector3f ShadowBakedCamera[AtmoShadowBake::CascadeCount];

    /** The light each level was last baked under, and when, in world
     *  seconds: what the next bake of that level reprojects and weights its
     *  history by. */
    FVector3f ShadowBakedLight[AtmoShadowBake::CascadeCount];
    double ShadowBakeTime[AtmoShadowBake::CascadeCount] = {};

    /** False until every level is baked into the current target; cleared on a
     *  new target, model or field, or a wake, so the next request bakes all. */
    bool bShadowPrimed = false;

    /** The field the map was last requested for, as MakeShadowFieldKey. */
    uint32 ShadowFieldKey = 0;

    /** Creates the transmittance table if needed, and rebakes it when its
     *  inputs or its resource change. */
    void UpdateTransmittanceTable(float PlanetRadius);

    /** Creates the table if absent and forces its fixed size, float format,
     *  clamp addressing and UAV support. */
    void PrepareTransmittanceTable();

    /** Warns once when the active model has no sim config. */
    bool bWarnedSimConfig = false;

    /** Syncs the directional light's rotation, colour and intensity from the
     *  actor's rotation and the LightColor property. */
    void UpdateLightFromRotation();

    /** The frame the cloud field is defined in: the planet's, which the
     *  atmosphere is attached to, or the world's when it stands alone. The
     *  actor's own rotation is the light direction and never turns the field. */
    FQuat GetFieldFrame() const;
};
