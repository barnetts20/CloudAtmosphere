// Manages a post-process volume with two blendable material instances and a
// directional light, rendering a volumetric atmosphere and cloud layer around a
// planet.
//
// THE TRANSFORM IS THE INTERFACE.
//
//   Actor Location  -> planet centre / atmosphere centre
//   Actor Scale max -> planet radius
//   Actor Rotation  -> light direction, and the directional light's rotation
//
// When owned by APlanetActor the scale is set externally to
// max(OceanRadius, PlanetRadius), the visible surface floor.
//
// TWO CLOUD MODELS SHARE ONE MARCH. PlanetType selects which material fills slot
// 0 -- a terrestrial cloud band, or a gas giant deck driven by the flow sim --
// and slot 1, the composite, is shared. Each model owns its own parameter
// groups, with only the composite and the sim shared between them; see
// AtmosphereParams.h.
//
// PITFALL: THE MARCH READS SCENE DEPTH ITSELF. There is no pass ahead of it
// producing depth or a target size; both were traps -- a depth routed through
// a user scene texture is quantised by distance, and a size taken from the
// pass reports the internal resolution, not the pixel's viewport fraction.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/VolumeTexture.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "AtmosphereParams.h"
#include "AtmosphereTransmittance.h"
#include "PlanetAtmosphereActor.generated.h"

class UTextureRenderTarget2D;
class UTextureRenderTarget2DArray;

/** Renders a volumetric atmosphere and cloud layer via post-process materials.
 *  Spawns two child actors (APostProcessVolume and ADirectionalLight) and
 *  creates two dynamic material instances assigned as blendables on the
 *  volume. Every parameter is pushed each tick by UpdateMaterialParameters, and
 *  the light's rotation and colour are synced from the actor's rotation and
 *  LightColor. When planet-owned, location and scale are locked and rotation
 *  stays editable.
 *
 *  ONE FUNCTION PICKS THE MATERIAL AND ONE PICKS THE PARAMETERS, BOTH FROM
 *  PlanetType. PITFALL: setting a parameter a material does not declare does
 *  nothing and logs nothing, so a march material and a parameter sweep that
 *  disagree render something plausible with none of the model-specific inputs
 *  bound -- which reads as a simulation or texture bug rather than a wiring one.
 */
UCLASS()
class CLOUDATMOSPHERE_API APlanetAtmosphereActor : public AActor
{
    GENERATED_BODY()

public:
    APlanetAtmosphereActor();

    // --- Pipeline ---
    //
    // The assets and passes the actor drives, rather than anything the march
    // reads. First in the panel because none of the parameters below mean
    // anything until these are right.
    //
    // Soft material references rather than hardcoded paths: a stale path logs a
    // warning and otherwise just looks like a broken material.

    /** Slot 0 for PlanetType::Terrestrial. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CloudAtmosphere|Pipeline|Materials")
    TSoftObjectPtr<UMaterialInterface> TerrestrialMarchMaterial;

    /** Slot 0 for PlanetType::GasGiant. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CloudAtmosphere|Pipeline|Materials")
    TSoftObjectPtr<UMaterialInterface> GasGiantMarchMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CloudAtmosphere|Pipeline|Materials")
    TSoftObjectPtr<UMaterialInterface> PostprocessMaterial;

    /** Recreates slot 0 against the current PlanetType and repopulates every
     *  slot. Call after changing PlanetType or either march material.
     *
     *  ALSO THE PARAMETER-CHECK RETRIGGER: every push is verified against the
     *  material and warns once per name, and this clears that filter. Only the
     *  ACTIVE model is pushed, so covering both means pressing it, flipping
     *  PlanetType, and pressing it again.
     *
     *  PITFALL: a CallInEditor function's category must be top-level. The
     *  details panel does not nest it -- a path with '|' becomes one flat
     *  category of that literal name, and every property sharing the exact path
     *  is pulled out of its nested group into it. */
    UFUNCTION(BlueprintCallable, CallInEditor, Category = "CloudAtmosphere")
    void RebuildMaterialInstances();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline")
    FAtmosphereCompositeParams Composite;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline")
    FAtmosphereSimulationParams Simulation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline|Raymarch", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantRaymarchParams GasGiantRaymarch;

    // Baked Lighting: the targets the per-frame and on-change bakes write and
    // the march reads. Set once for a performance tier, not tuned for looks.

    /** Destination for the deck shadow bake, in the light's frame: a cascade of
     *  slices, all one resolution, each covering a smaller radius. ASSIGNED, NOT
     *  CREATED, matching FlowTarget, so an asset can be opened beside the planet
     *  and watched while the light moves. Size, format and UAV support are forced
     *  on assignment; a target without bCanCreateUAV accepts every dispatch and
     *  stays black. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline|Baked Lighting", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides))
    TObjectPtr<UTextureRenderTarget2DArray> GasGiantShadowTarget;

    /** Edge of each cascade slice, in texels. The target is resized to match, so
     *  this rather than the asset's own size is the handle. EVERY LEVEL SHARES
     *  IT and the world scale falls out of the extents, so one number moves the
     *  coarse disc slice and the fine detail slice together. Square, because the
     *  map has one extent for both axes.
     *
     *  Bake time and memory scale with the square -- 2 MB per slice at 512 in
     *  RGBA16F. The bake band-limits at its source, so a lower value softens
     *  shadows rather than aliasing them. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline|Baked Lighting", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ClampMin = "128", ClampMax = "4096"))
    int32 GasGiantShadowResolution = 1024;

    // --- Atmosphere ---
    //
    // What the actor IS, before anything about how it looks.

    /** True when spawned and driven by APlanetActor. Location and scale become
     *  read-only; rotation remains editable (controls light direction). */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CloudAtmosphere|Atmosphere")
    bool bIsPlanetOwned = false;

    /** Which cloud model slot 0 renders. Changing this at runtime requires
     *  RebuildMaterialInstances -- the material is chosen once, at creation. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CloudAtmosphere|Atmosphere")
    EPlanetAtmosphereType PlanetType = EPlanetAtmosphereType::Terrestrial;

    // A GROUP OF ONE GETS NO WRAPPER: a substruct buys a fold-out, worth a click
    // only when there is more than one thing behind it.

    /** RGB direction is the hue, RGB magnitude the intensity. The march and the
     *  directional light both derive from this, so they cannot disagree about the
     *  star; light DIRECTION comes from the actor's rotation. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Atmosphere")
    FLinearColor LightColor = FLinearColor(30.0f, 28.5f, 27.0f, 10.0f);

    /** Enable or disable the atmosphere's unbound post-process volume. THE ONLY
     *  RELIABLE OFF-SWITCH for the ray march: the volume is not a primitive
     *  component, so hiding the actor or disabling its tick does not stop it.
     *  Parked planets must call this with false, or every pooled atmosphere keeps
     *  tinting the whole screen. */
    void SetAtmosphereActive(bool bActive);

    /** Aim the light and the march at the star: points the actor's forward at
     *  StarWorldPos and runs the rotation-to-light sync. Called each frame by the
     *  owning planet from IStarLit::SetStarWorldPosition. */
    void OrientToStar(const FVector& StarWorldPos);

    // --- Parameters ---
    //
    // ONE PROPERTY PER PANEL GROUP, INLINED. ShowOnlyInnerProperties puts the
    // members directly under the property's category, so the group name comes
    // from the category rather than from a struct row, and members carry no
    // category of their own so they display in declaration order.
    //
    // PITFALL: EditConditionHides does not survive the inlining. The condition
    // lives on the property row and there is no row left, so both models' groups
    // are visible at once, distinguished only by their parent category.

    // Terrestrial: the shared Common structs plus the cloud band.

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Geometry", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereGeometryParams TerrestrialGeometry;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Atmosphere Scattering", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereAirScatteringParams TerrestrialAtmosphereScattering;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud Scattering", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereCloudScatteringParams TerrestrialCloudScattering;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Raymarch", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereRaymarchParams TerrestrialRaymarch;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Terrestrial Cloud", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialCloudParams Terrestrial;

    // Gas giant: the authoritative layout. ApplyGasGiantParams pushes these
    // directly, under their members' own names; none of it goes through
    // GetCommonParams, which serves the terrestrial path alone.

    // THE MASTER SCALE, first under Gas Giant: every deck height is a fraction
    // of the shell it sets. The shared geometry struct, whose one member is the
    // height scale, inlined so the member sits directly under the category.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereGeometryParams GasGiantGeometry;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Deck|Profile", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantProfileParams GasGiantProfile;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Deck|Flow", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantFlowParams GasGiantFlow;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Deck|Motion", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantMotionParams GasGiantMotion;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Deck|Surface", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantSurfaceParams GasGiantSurface;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Deck|Structure Layer", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantNoiseLayerParams GasGiantStructureLayer;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Deck|Detail Layer", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantNoiseLayerParams GasGiantDetailLayer;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Atmosphere Lighting", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantAtmosphereLightingParams GasGiantAtmosphereLighting;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Cloud Lighting|Bands", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantBandParams GasGiantBands;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Cloud Lighting|Extinction", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantExtinctionParams GasGiantExtinction;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Cloud Lighting|Phase", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantPhaseParams GasGiantPhase;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Cloud Lighting|Multiple Scattering", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantMultipleScatteringParams GasGiantMultipleScattering;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Cloud Lighting|Terminator", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantTerminatorParams GasGiantTerminator;

    // --- Lifecycle ---

    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void BeginPlay() override;
    virtual void Destroyed() override;
    virtual void BeginDestroy() override;
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void Tick(float DeltaTime) override;

    // --- Editor Property & Transform Locking ---

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
    virtual bool CanEditChange(const FProperty* InProperty) const override;
    virtual void PostEditMove(bool bFinished) override;
    virtual void EditorApplyTranslation(const FVector& DeltaTranslation, bool bAltDown, bool bShiftDown, bool bCtrlDown) override;
    virtual void EditorApplyScale(const FVector& DeltaScale, const FVector* PivotLocation, bool bAltDown, bool bShiftDown, bool bCtrlDown) override;
#endif

    /** Called by PlanetActor after spawn and attach. Sets bIsPlanetOwned, binds
     *  the transform guard and runs Initialize, with InScale applied first.
     *  Skips the deferred OnConstruction path. */
    void InitializeFromPlanet(USceneComponent* InAttachParent,
        FVector InScale = FVector::ZeroVector);

private:
    /** Root component -- child actors (PPV, light) attach here. */
    UPROPERTY()
    TObjectPtr<USceneComponent> AtmosphereRoot;

    /** Unbound post-process volume carrying the 3 blendable material instances. */
    UPROPERTY()
    TObjectPtr<APostProcessVolume> PostProcessVolume = nullptr;

    /** Directional light whose rotation and color are synced from actor rotation
     *  and the LightColor property. */
    UPROPERTY()
    TObjectPtr<ADirectionalLight> SunLight = nullptr;

    // --- Dynamic Material Instances (created from plugin base materials) ---

    /** Pass 0: atmosphere + cloud ray marching. Parent depends on PlanetType. */
    UPROPERTY()
    TObjectPtr<UMaterialInstanceDynamic> MID_Atmosphere = nullptr;

    /** Pass 1: distance-based blur compositing. */
    UPROPERTY()
    TObjectPtr<UMaterialInstanceDynamic> MID_Postprocess = nullptr;

    /** Air transmittance table, created on first use. Visible for inspection,
     *  though it depends only on the radii and the air profile so there is
     *  nothing to watch it do. */
    UPROPERTY(Transient, VisibleInstanceOnly, Category = "CloudAtmosphere|Pipeline|Baked Lighting")
    TObjectPtr<UTextureRenderTarget2D> TransmittanceTable = nullptr;

    /** Inputs and destination of the last enqueued bake. */
    FAtmosphereTransmittanceParams TransmittanceBaked;

    /** Which model MID_Atmosphere was created for. Guards a PlanetType change
     *  reaching the parameter sweep before the material is rebuilt, which would
     *  push a whole model's parameters at a material declaring none of them and
     *  silently render the other model. */
    EPlanetAtmosphereType BuiltType = EPlanetAtmosphereType::Terrestrial;

    bool bInitialized = false;

    /** When true, Initialize runs on the next Tick. Set by OnConstruction to
     *  defer initialization until the world is ready. */
    bool bPendingInitialize = true;

    /** True once this actor has asked the subsystem to start. Cleared on
     *  teardown so a pooled planet does not leave the sim running. */
    bool bStartedSimulation = false;

    /** Cached scale set by the planet actor, used by the transform guard. */
    FVector PlanetDrivenScale = FVector::OneVector;

    /** Bound to RootComponent->TransformUpdated when planet-owned. Snaps location
     *  and scale back to planet-driven values, leaving rotation alone. */
    void OnTransformUpdated(USceneComponent* Component, EUpdateTransformFlags Flags, ETeleportType Teleport);

    /** Spawns child actors, creates the material instances, pushes every
     *  parameter and syncs the light. */
    void Initialize();

    /** Spawns the APostProcessVolume and ADirectionalLight as child actors,
     *  attaching them to AtmosphereRoot. */
    void SpawnChildActors();

    /** Destroys the post-process volume and directional light, nulls the MID pointers. */
    void DestroyChildActors();

    /** Creates the two dynamic material instances and assigns them as
     *  blendables. Slot 0's parent is chosen from PlanetType here and recorded
     *  in BuiltType. */
    void CreateMaterialInstances();

    /** Pushes every parameter to the atmosphere and postprocess instances.
     *  Called every tick and on property changes. Dispatches the cloud half on
     *  BuiltType, not PlanetType. */
    void UpdateMaterialParameters();

    /** The terrestrial model's Common groups, as ApplyCommonParams takes them.
     *  The gas giant pushes its own groups directly. */
    FAtmosphereCommonView GetCommonParams() const
    {
        return FAtmosphereCommonView{
            TerrestrialGeometry,
            TerrestrialAtmosphereScattering,
            TerrestrialCloudScattering,
            TerrestrialRaymarch };
    }

    /** Geometry, light, air scattering, cloud lighting, raymarching, under the
     *  terrestrial material's parameter names. */
    void ApplyCommonParams(const FAtmosphereCommonView& Common, float PlanetRadius,
        const FVector& PlanetCenter, const FVector& LightDir);

    /** Cloud shell, noise and extinction. Terrestrial material only. */
    void ApplyTerrestrialParams(const FAtmosphereCommonView& Common);

    /** Every gas giant group under its members' own names, plus the planet, the
     *  light, the clock and the local frame. AUTHORED VALUES ONLY: everything
     *  derived is computed in the shader, once, from these, and a value derived
     *  here would be a second source that can disagree with the bake's. */
    void ApplyGasGiantParams(float PlanetRadius, const FVector& PlanetCenter, const FVector& LightDir);

    /** One noise layer's members, each under Prefix + member name. */
    void ApplyGasGiantLayer(const TCHAR* Prefix, const FGasGiantNoiseLayerParams& Layer);

    /** Queues this frame's deck shadow bake with the sim subsystem. SEPARATE FROM
     *  ApplyGasGiantParams because its destination is a compute pass rather than
     *  a MID: it sends the same authored values under the same names, and the
     *  bake derives from them with the same shader functions, which keeps the
     *  deck the light sees identical to the deck the eye sees. */
    void RequestGasGiantShadowBake(float PlanetRadius, const FVector& PlanetCenter, const FVector& LightDir);

    /** Forces the assigned shadow target to RGBA16F with UAV support, resizing
     *  only when the format is wrong. Returns false when nothing is usable,
     *  having logged the reason at most once per state. */
    bool PrepareGasGiantShadowTarget();

    /** Suppresses the per-tick repeat of the shadow target complaint. Cleared
     *  when a usable target appears, so a fixed asset logs its recovery. */
    bool bWarnedShadowTarget = false;

    /** Creates the transmittance table if needed, rebakes it when its inputs or
     *  its resource change, and pushes it to the march material. */
    void UpdateTransmittanceTable(float PlanetRadius);

    /** Creates the table if absent and forces its fixed size, float format,
     *  clamp addressing and UAV support. */
    void PrepareTransmittanceTable();

    /** Starts the sim subsystem against the deck's config. */
    void StartGasGiantSimulation();

    /** Simulated time from the sim subsystem, or 0 when it is not running. NOT
     *  WORLD TIME: the field is coherent against the sim's own clock, and the two
     *  diverge the moment the sim pauses, is stepped by hand or is restored from
     *  a snapshot -- after which the warp would advect a field that has not
     *  moved. */
    float GetGasGiantTime() const;

    /** Syncs the directional light's rotation, colour and intensity from the
     *  actor's rotation and the LightColor property. */
    void UpdateLightFromRotation();

    /** Resolves a soft material reference, logging which one failed. */
    static UMaterialInterface* LoadMaterialAsset(const TSoftObjectPtr<UMaterialInterface>& Ref, const TCHAR* Label);
};