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
#include "GasGiantShadowMap.h"
#include "PlanetAtmosphereActor.generated.h"

class USceneCaptureComponent2D;
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

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline|Raymarch", meta = (ShowOnlyInnerProperties))
    FAtmosphereRaymarchParams Raymarch;

    // Baked Lighting: the targets the per-frame and on-change bakes write and
    // the march reads. Set once for a performance tier, not tuned for looks.

    /** Destination for the deck shadow bake, in the light's frame: a cascade of
     *  slices, all one resolution, each covering a smaller radius. ASSIGNED, NOT
     *  CREATED, matching FlowTarget, so an asset can be opened beside the planet
     *  and watched while the light moves. Size, format and UAV support are forced
     *  on assignment; a target without bCanCreateUAV accepts every dispatch and
     *  stays black. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline|Baked Lighting")
    TObjectPtr<UTextureRenderTarget2DArray> ShadowTarget;

    /** Edge of each cascade slice, in texels. The target is resized to match, so
     *  this rather than the asset's own size is the handle. EVERY LEVEL SHARES
     *  IT and the world scale falls out of the extents, so one number moves the
     *  coarse disc slice and the fine detail slice together. Square, because the
     *  map has one extent for both axes.
     *
     *  Bake time and memory scale with the square -- 2 MB per slice at 512 in
     *  RGBA16F. The bake band-limits at its source, so a lower value softens
     *  shadows rather than aliasing them. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline|Baked Lighting", meta = (ClampMin = "128", ClampMax = "4096"))
    int32 ShadowResolution = 1024;

    /** Cascades rebaked per frame, taken in turn. 1 rebakes each level every
     *  third frame at a third of the cost; the clouds and the light move slowly
     *  enough that the lag does not show. Each level is read against the camera
     *  it was baked with, so a moving camera costs nothing in placement. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline|Baked Lighting", meta = (ClampMin = "1", ClampMax = "3"))
    int32 ShadowLevelsPerFrame = 1;

    /** Opaque geometry casting into the deck shadow map. PARKED, so it carries
     *  no edit specifier and reaches neither the details panel nor Blueprint;
     *  FGasGiantOccluderShadowParams::IsEnabled answers false whatever a saved
     *  instance holds. Kept as a plain UPROPERTY so existing levels deserialize
     *  their authored values rather than losing them. */
    UPROPERTY()
    FGasGiantOccluderShadowParams GasGiantOccluderShadows;

    // --- Atmosphere ---
    //
    // What the actor IS, before anything about how it looks.

    /** True when spawned and driven by APlanetActor. Location and scale become
     *  read-only; rotation remains editable (controls light direction). */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CloudAtmosphere|Atmosphere")
    bool bIsPlanetOwned = false;

    /** Which cloud model slot 0 renders. Changing this at runtime requires
     *  RebuildMaterialInstances -- the material is chosen once, at creation.
     *
     *  BOTH CASES ARE LIVE, through separate shaders, bakes, materials and
     *  parameter groups. Everything a model's look depends on is twinned, so a
     *  type change swaps the whole authored set rather than reinterpreting one. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CloudAtmosphere|Atmosphere")
    EPlanetAtmosphereType PlanetType = EPlanetAtmosphereType::GasGiant;

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

    // Terrestrial. The lighting groups are twinned with the gas giant's --
    // shared STRUCTS, separate INSTANCES, since the terrestrial shell is a tenth
    // the gas giant's and every scale authored against it means something else.
    // The cloud groups are the terrestrial field's own: it reads the sim as a
    // weather map rather than as bands.
    //
    // A group's default lives with its struct when the struct is one model's
    // own, and on the CDO in the constructor when the struct is shared.

    // THE MASTER SCALE, first under Terrestrial as under Gas Giant: every cloud
    // height is a fraction of the shell it sets.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereGeometryParams TerrestrialGeometry;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Clouds|Profile", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialProfileParams TerrestrialProfile;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Clouds|Motion", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialMotionParams TerrestrialMotion;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Clouds|Genus", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialGenusParams TerrestrialGenus;

    /** The cloud shape coverage erodes. Relief and BandMix are unused here. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Clouds|Structure Layer", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereNoiseLayerParams TerrestrialStructureLayer;

    /** Edge erosion. Relief and BandMix are unused here. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Clouds|Detail Layer", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereNoiseLayerParams TerrestrialDetailLayer;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Atmosphere Lighting", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereLightingParams TerrestrialAtmosphereLighting;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Cloud Lighting|Material", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialCloudMaterialParams TerrestrialCloudMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Cloud Lighting|Extinction", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereExtinctionParams TerrestrialExtinction;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Cloud Lighting|Phase", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmospherePhaseParams TerrestrialPhase;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Cloud Lighting|Multiple Scattering", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereMultipleScatteringParams TerrestrialMultipleScattering;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Cloud Lighting|Terminator", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereTerminatorParams TerrestrialTerminator;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Cloud Lighting|Surface Shadows", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereSurfaceShadowParams TerrestrialSurfaceShadow;

    // Gas giant: the same groups in the same order, over its own field.
    // ApplyMarchParams pushes whichever set BuiltType selects, under their
    // members' own names.

    // THE MASTER SCALE, first under Gas Giant: every deck height is a fraction
    // of the shell it sets. The shared geometry struct, whose one member is the
    // height scale, inlined so the member sits directly under the category.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereGeometryParams Geometry;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Deck|Profile", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantProfileParams GasGiantProfile;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Deck|Flow", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereFlowParams Flow;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Deck|Bands", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantBandShapeParams GasGiantBandShape;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Deck|Motion", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereMotionParams Motion;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Deck|Surface", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereCarveParams Carve;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Deck|Structure Layer", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereNoiseLayerParams StructureLayer;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Deck|Detail Layer", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereNoiseLayerParams DetailLayer;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Atmosphere Lighting", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereLightingParams AtmosphereLighting;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Cloud Lighting|Bands", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantBandParams GasGiantBands;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Cloud Lighting|Extinction", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereExtinctionParams Extinction;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Cloud Lighting|Phase", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmospherePhaseParams Phase;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Cloud Lighting|Multiple Scattering", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereMultipleScatteringParams MultipleScattering;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Cloud Lighting|Terminator", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereTerminatorParams Terminator;

    /** WHERE THE MAP LANDS, not what it costs. The target and its resolution are
     *  one planet's pipeline and a type change reuses them; how hard a cloud
     *  shadow reads on the ground is look, and a band over terrain wants a
     *  different answer from a deck with nothing under it. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Cloud Lighting|Surface Shadows", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereSurfaceShadowParams GasGiantSurfaceShadow;

    // --- The built model's groups ---
    //
    // ON BuiltType, NOT PlanetType: the material is chosen once and the sweep has
    // to feed the one that exists. A type change needs RebuildMaterialInstances
    // either way, and reading the panel's value here would push a terrestrial
    // shell into a gas giant march for one frame.

    const FAtmosphereGeometryParams& ActiveGeometry() const
    {
        return bTerrestrial() ? TerrestrialGeometry : Geometry;
    }

    const FAtmosphereNoiseLayerParams& ActiveStructureLayer() const
    {
        return bTerrestrial() ? TerrestrialStructureLayer : StructureLayer;
    }

    const FAtmosphereNoiseLayerParams& ActiveDetailLayer() const
    {
        return bTerrestrial() ? TerrestrialDetailLayer : DetailLayer;
    }

    const FAtmosphereLightingParams& ActiveAtmosphereLighting() const
    {
        return bTerrestrial() ? TerrestrialAtmosphereLighting : AtmosphereLighting;
    }

    const FAtmosphereExtinctionParams& ActiveExtinction() const
    {
        return bTerrestrial() ? TerrestrialExtinction : Extinction;
    }

    const FAtmospherePhaseParams& ActivePhase() const
    {
        return bTerrestrial() ? TerrestrialPhase : Phase;
    }

    const FAtmosphereMultipleScatteringParams& ActiveMultipleScattering() const
    {
        return bTerrestrial() ? TerrestrialMultipleScattering : MultipleScattering;
    }

    const FAtmosphereTerminatorParams& ActiveTerminator() const
    {
        return bTerrestrial() ? TerrestrialTerminator : Terminator;
    }

    const FAtmosphereSurfaceShadowParams& ActiveSurfaceShadow() const
    {
        return bTerrestrial() ? TerrestrialSurfaceShadow : GasGiantSurfaceShadow;
    }

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

    /** What every Active* accessor asks. */
    bool bTerrestrial() const { return BuiltType == EPlanetAtmosphereType::Terrestrial; }

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

    /** Every group under its members' own names, plus the planet, the light, the
     *  clock and the local frame. AUTHORED VALUES ONLY: everything derived is
     *  computed in the shader, once, from these, and a value derived here would
     *  be a second source that can disagree with the bake's.
     *
     *  Pushes the shared groups, then hands off to the built model's own. */
    void ApplyMarchParams(float PlanetRadius, const FVector& PlanetCenter, const FVector& LightDir);

    /** The groups whose members are the model's own. The gas giant pushes each
     *  member under its own name; the terrestrial field packs its groups into
     *  the float4 pins TR_BuildField unpacks. */
    void ApplyGasGiantModelParams();
    void ApplyTerrestrialModelParams();

    /** One gas giant noise layer's members, each under Prefix + member name. */
    void ApplyNoiseLayer(const TCHAR* Prefix, const FAtmosphereNoiseLayerParams& Layer);

    /** Queues this frame's deck shadow bake with the sim subsystem. SEPARATE FROM
     *  ApplyMarchParams because its destination is a compute pass rather than
     *  a MID: it sends the same authored values under the same names, and the
     *  bake derives from them with the same shader functions, which keeps the
     *  deck the light sees identical to the deck the eye sees. */
    void RequestShadowBake(float PlanetRadius, const FVector& PlanetCenter, const FVector& LightDir);

    /** Fills the half of a bake request that does not depend on which field is
     *  baked: the map, the frame, the light, the camera and the shared groups.
     *  Returns false when the request could not be made usable.
     *
     *  A TEMPLATE BECAUSE THE TWO PARAMS STRUCTS ARE SEPARATE TYPES, deliberately
     *  -- they diverge as the fields do. Both instantiations live in the one
     *  translation unit that uses them. */
    template<typename TShadowParams>
    bool FillSharedShadowParams(TShadowParams& Params, float PlanetRadius,
        const FVector& PlanetCenter, const FVector& LightDir,
        class FTextureRenderTargetResource*& OutFlowRes);

    /** Forces the assigned shadow target to RGBA16F with UAV support, resizing
     *  only when the format is wrong. Returns false when nothing is usable,
     *  having logged the reason at most once per state. */
    bool PrepareShadowTarget();

    /** Suppresses the per-tick repeat of the shadow target complaint. Cleared
     *  when a usable target appears, so a fixed asset logs its recovery. */
    bool bWarnedShadowTarget = false;

    /** The next cascade in the bake rotation, and the camera each level was
     *  last baked around -- what the material reads that level against. */
    int32 ShadowLevelCursor = 0;
    FVector3f ShadowBakedCamera[AtmoShadowBake::CascadeCount];

    /** False until every level has been baked into the current target. Cleared
     *  when the target is reinitialised or the model changes, so the first
     *  request after either bakes all of them. */
    bool bShadowPrimed = false;


    // --- Occluder captures ---
    //
    // One orthographic depth capture per cascade, created on demand and torn
    // down when the feature is off or the planet is not a gas giant. Transient
    // and unassignable: unlike the shadow target there is nothing to watch in
    // them that the shadow target does not already show.

    // TArray rather than a fixed array on the reflected members: the header
    // tool wants a literal bound, and a second spelling of CascadeCount is a
    // number that can drift from the one the dispatch uses. Sized to
    // AtmoShadowBake::CascadeCount wherever they are touched.

    UPROPERTY(Transient)
    TArray<TObjectPtr<USceneCaptureComponent2D>> OccluderCaptures;

    /** Visible so a capture can be opened and looked at. There is nothing to
     *  read in a depth target by eye -- its values run to 1e8 and display flat
     *  white -- but with bDebugColorCapture on it shows the scene from the
     *  light, which is what tells a missing shadow from a missing capture. */
    UPROPERTY(Transient, VisibleInstanceOnly, Category = "CloudAtmosphere|Pipeline|Baked Lighting")
    TArray<TObjectPtr<UTextureRenderTarget2D>> OccluderDepthTargets;

    /** The frame each capture ACTUALLY RENDERED WITH, not the one computed this
     *  tick. A level on a slow cadence is then placed correctly and only late,
     *  where reusing this tick's frame would drag its last image across the
     *  deck as the light moves. */
    FAtmoOccluderFrame OccluderFrames[AtmoShadowBake::CascadeCount];

    int32 FramesSinceCapture[AtmoShadowBake::CascadeCount];

    /** Creates or destroys the capture components and their R32F targets to
     *  match the current settings and resolution. Returns whether any level is
     *  live. */
    bool PrepareGasGiantOccluderCaptures();

    /** Places each capture in the light's frame, captures the levels due this
     *  frame, and fills Params.Occluders from what each level last rendered.
     *
     *  A TEMPLATE for the same reason FillSharedShadowParams is: the occluder
     *  band belongs to the map rather than to the field, so both params structs
     *  carry it and neither type is the right one to name here. */
    template<typename TShadowParams>
    void UpdateOccluderCaptures(
        float PlanetRadius, const FVector& PlanetCenter,
        const FVector3f& LightLocal, const FVector3f& CameraLocal,
        TShadowParams& Params);

    /** Frees the capture components and targets. */
    void DestroyGasGiantOccluderCaptures();

    /** Creates the transmittance table if needed, rebakes it when its inputs or
     *  its resource change, and pushes it to the march material. */
    void UpdateTransmittanceTable(float PlanetRadius);

    /** Creates the table if absent and forces its fixed size, float format,
     *  clamp addressing and UAV support. */
    void PrepareTransmittanceTable();

    /** Starts the sim subsystem against the deck's config. */
    void StartFlowSimulation();

    /** Sim time of the state the field shows, or 0 when the sim is not running. NOT
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