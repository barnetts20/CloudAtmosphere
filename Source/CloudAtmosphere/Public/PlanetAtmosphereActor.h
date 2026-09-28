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
// PITFALL: THE MARCH READS SCENE DEPTH ITSELF, with no pass ahead of it. A
// depth routed through a user scene texture is quantised by distance, and a
// target size taken from a pass reports the internal resolution rather than
// the pixel's viewport fraction.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/DirectionalLight.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PostProcessComponent.h"
#include "Engine/VolumeTexture.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "AtmosphereParams.h"
#include "AtmosphereTransmittance.h"
#include "AtmosphereShadowBake.h"
#include "PlanetAtmosphereActor.generated.h"

class UTextureRenderTarget2D;
class UTextureRenderTarget2DArray;

/** Renders a volumetric atmosphere and cloud layer via post-process materials.
 *  Owns an unbound post-process component and a directional light component,
 *  and creates two transient dynamic material instances assigned as
 *  blendables on the post-process component. Every parameter is pushed each
 *  tick by UpdateMaterialParameters, and the light's rotation and colour are
 *  synced from the actor's rotation and LightColor. When planet-owned,
 *  location and scale are locked and rotation stays editable.
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
    // the march reads. Set once for a performance tier, not tuned for looks. The
    // terrestrial cascade extents are look, and live with its surface shadows.

    /** Destination for the deck shadow bake, in the light's frame: a cascade of
     *  slices, all one resolution, each covering a smaller radius. ASSIGNED, NOT
     *  CREATED, matching FlowTarget, so an asset can be opened beside the planet
     *  and watched while the light moves. ONE PER ATMOSPHERE: a target another
     *  atmosphere already bakes into is refused with a warning. Size, format,
     *  clear colour and UAV support are forced on assignment. */
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

    /** Seconds over which a rebaked cascade fades in from its previous bake,
     *  reprojected to the current light and camera. Hides the step each rebake
     *  takes as the clouds advect under the texel lattice, at the cost of the
     *  shadow trailing the clouds by about this long. 0 shows each bake as it
     *  lands. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline|Baked Lighting", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float ShadowTemporalSmoothing = 0.1f;

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

    /** Parks or wakes the atmosphere: both passes, the light and the per-tick
     *  push, bake and transmittance update. THE ONLY RELIABLE OFF-SWITCH for the
     *  march: an unbound post-process component is not a primitive, so hiding
     *  the actor or stopping its tick does not stop it. Parked planets must call
     *  this with false, or every pooled atmosphere keeps tinting the screen and
     *  lighting the scene. Waking rebakes every shadow level. */
    void SetAtmosphereActive(bool bActive);

    /** Aim the light and the march at the star: sets the actor's relative
     *  rotation, which is the light direction, toward StarWorldPos and runs the
     *  rotation-to-light sync. The cloud field keeps the planet's frame. Called
     *  each frame by the owning planet from IStarLit::SetStarWorldPosition. */
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

    // Terrestrial. The air, ambient and cloud lighting groups are twinned with
    // the gas giant's -- shared STRUCTS, separate INSTANCES, since the terrestrial shell is a fifth
    // the gas giant's and every scale authored against it means something else.
    // The cloud groups are the terrestrial field's own: it reads the sim as a
    // weather map rather than as bands.
    //
    // A group's default lives with its struct when the struct is one model's
    // own, and on the CDO in the constructor when the struct is shared.

    // THE MASTER SCALE, first under Terrestrial: every cloud height is a
    // fraction of the shell it sets.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialPlanetParams TerrestrialPlanet;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Air", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereAirParams TerrestrialAir;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Ambient", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereAmbientParams TerrestrialAmbient;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Clouds|Shape", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialShapeParams TerrestrialShape;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Clouds|Coverage", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialCoverageParams TerrestrialCoverage;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Clouds|Type", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialTypeParams TerrestrialType;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Clouds|Lift", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialLiftParams TerrestrialLift;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Clouds|Warp", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialWarpParams TerrestrialWarp;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Clouds|Structure Layer", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialStructureLayerParams TerrestrialStructureLayer;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Clouds|Detail Layer", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialDetailLayerParams TerrestrialDetailLayer;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Cloud Lighting|Material", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialCloudMaterialParams TerrestrialCloudMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Cloud Lighting|Phase", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmospherePhaseParams TerrestrialPhase;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Cloud Lighting|Multiple Scattering", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereMultipleScatteringParams TerrestrialMultipleScattering;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Cloud Lighting|Surface Shadows", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereSurfaceShadowParams TerrestrialSurfaceShadow;

    /** Bound on either cloud surface's slope, in cloud depths per radian: the
     *  cone angle for the entry search. Under-declaring it is the one way that
     *  search steps over cloud, and the symptom is cloud missing on grazing
     *  rays. Raise it first. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Advanced", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ClampMin = "0.1"))
    float TerrestrialCloudSlope = 60.0f;

    // Gas giant: its own deck groups, with its own instances of the shared air,
    // ambient and cloud lighting groups. ApplyMarchParams pushes whichever set
    // BuiltType selects, under their members' own names.

    // THE MASTER SCALE, first under Gas Giant: every deck height is a fraction
    // of the shell it sets. The geometry struct, whose one member is the height
    // scale, inlined so the member sits directly under the category.
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

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Air", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereAirParams Air;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Ambient", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereAmbientParams Ambient;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Cloud Lighting|Bands", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantBandParams GasGiantBands;

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

    float ActiveHeightScale() const
    {
        return bTerrestrial() ? TerrestrialPlanet.HeightScale : Geometry.HeightScale;
    }

    /** The noise volumes, which both models bind under the same names. */
    UVolumeTexture* ActiveStructureVolume() const
    {
        return bTerrestrial() ? TerrestrialStructureLayer.Volume.Get() : StructureLayer.Volume.Get();
    }

    UVolumeTexture* ActiveDetailVolume() const
    {
        return bTerrestrial() ? TerrestrialDetailLayer.Volume.Get() : DetailLayer.Volume.Get();
    }

    const FAtmosphereAirParams& ActiveAir() const
    {
        return bTerrestrial() ? TerrestrialAir : Air;
    }

    const FAtmosphereAmbientParams& ActiveAmbient() const
    {
        return bTerrestrial() ? TerrestrialAmbient : Ambient;
    }

    const FAtmospherePhaseParams& ActivePhase() const
    {
        return bTerrestrial() ? TerrestrialPhase : Phase;
    }

    const FAtmosphereMultipleScatteringParams& ActiveMultipleScattering() const
    {
        return bTerrestrial() ? TerrestrialMultipleScattering : MultipleScattering;
    }

    const FAtmosphereSurfaceShadowParams& ActiveSurfaceShadow() const
    {
        return bTerrestrial() ? TerrestrialSurfaceShadow : GasGiantSurfaceShadow;
    }

    // --- Lifecycle ---

    virtual void OnConstruction(const FTransform& Transform) override;
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Destroyed() override;
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void Tick(float DeltaTime) override;

    // --- Editor Property & Transform Locking ---

#if WITH_EDITOR
    virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
    virtual void PostEditUndo() override;
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
    /** Root component; carries the planet radius as scale. */
    UPROPERTY()
    TObjectPtr<USceneComponent> AtmosphereRoot;

    /** Unbound, carrying both blendables. COMPONENTS, NOT CHILD ACTORS: a
     *  duplicate or a deletion takes its own and never another's. */
    UPROPERTY(VisibleAnywhere, Category = "CloudAtmosphere|Pipeline")
    TObjectPtr<UPostProcessComponent> PostProcessComponent;

    /** Synced from the actor's rotation and LightColor; absolute rotation and
     *  scale, so the planet's frame and the root's radius never reach it. */
    UPROPERTY(VisibleAnywhere, Category = "CloudAtmosphere|Pipeline")
    TObjectPtr<UDirectionalLightComponent> SunLightComponent;

    /** Legacy child actors a saved level can still hold, destroyed on
     *  Initialize. */
    UPROPERTY()
    TObjectPtr<APostProcessVolume> PostProcessVolume_DEPRECATED = nullptr;

    UPROPERTY()
    TObjectPtr<ADirectionalLight> SunLight_DEPRECATED = nullptr;

    // --- Dynamic Material Instances (created from plugin base materials) ---
    //
    // TRANSIENT, object and pointer: the post-process component's saved
    // blendables would otherwise write them into the level.

    /** Pass 0: atmosphere + cloud ray marching. Parent depends on PlanetType. */
    UPROPERTY(Transient, DuplicateTransient)
    TObjectPtr<UMaterialInstanceDynamic> MID_Atmosphere = nullptr;

    /** Pass 1: distance-based blur compositing. */
    UPROPERTY(Transient, DuplicateTransient)
    TObjectPtr<UMaterialInstanceDynamic> MID_Postprocess = nullptr;

    /** Air transmittance table, created on first use. Visible for inspection,
     *  though it depends only on the radii and the air profile so there is
     *  nothing to watch it do. */
    UPROPERTY(Transient, VisibleInstanceOnly, Category = "CloudAtmosphere|Pipeline|Baked Lighting")
    TObjectPtr<UTextureRenderTarget2D> TransmittanceTable = nullptr;

    /** Inputs of the last enqueued bake; reset when the table is recreated. */
    FAtmosphereTransmittanceParams TransmittanceBaked;

    /** Which model MID_Atmosphere was created for. Guards a PlanetType change
     *  reaching the parameter sweep before the material is rebuilt, which would
     *  push a whole model's parameters at a material declaring none of them and
     *  silently render the other model. */
    EPlanetAtmosphereType BuiltType = EPlanetAtmosphereType::Terrestrial;

    /** What every Active* accessor asks. */
    bool bTerrestrial() const { return BuiltType == EPlanetAtmosphereType::Terrestrial; }

    bool bInitialized = false;

    /** Initialize runs on the next Tick outside a preview world. True from
     *  construction, so a loaded actor initialises without OnConstruction. */
    bool bPendingInitialize = true;

    /** False while parked by SetAtmosphereActive. */
    bool bAtmosphereActive = true;

    /** True once this actor has asked the subsystem to start. Cleared on
     *  teardown so a pooled planet does not leave the sim running. */
    bool bStartedSimulation = false;

    /** The frame the last shadow bake was requested on: one request per frame
     *  however many paths push parameters. */
    uint64 ShadowBakeFrame = MAX_uint64;

    /** Cached scale set by the planet actor, used by the transform guard. */
    FVector PlanetDrivenScale = FVector::OneVector;

    /** Bound to RootComponent->TransformUpdated when planet-owned. Snaps location
     *  and scale back to planet-driven values, leaving rotation alone. */
    void OnTransformUpdated(USceneComponent* Component, EUpdateTransformFlags Flags, ETeleportType Teleport);

    /** Destroys legacy child actors, creates the material instances, pushes
     *  every parameter and syncs the light. */
    void Initialize();

    /** Destroys the legacy child actors a saved level can still hold. */
    void DestroyLegacyChildActors();

    /** Stops the sim if this actor started it. */
    void StopFlowSimulation();

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
     *  Returns false when the request could not be made usable. Records
     *  nothing; CommitShadowBake does once the subsystem takes the request.
     *
     *  A TEMPLATE BECAUSE THE TWO PARAMS STRUCTS ARE SEPARATE TYPES, deliberately
     *  -- they diverge as the fields do. Both instantiations live in the one
     *  translation unit that uses them. */
    template<typename TShadowParams>
    bool FillSharedShadowParams(TShadowParams& Params, float PlanetRadius,
        const FVector& PlanetCenter, const FVector& LightDir);

    /** Records an accepted bake and pushes the cameras the march reads the
     *  fine levels against. */
    void CommitShadowBake(uint32 LevelMask, const FVector3f& LightDir, const FVector3f& CameraLocal);

    /** Forces the assigned shadow target to RGBA16F with UAV support, cleared
     *  to the no-deck sentinel, reinitialising only on a mismatch. Returns false
     *  when nothing is usable or another atmosphere is baking into the target,
     *  having logged the reason at most once per state. */
    bool PrepareShadowTarget();

    /** Suppresses the per-tick repeat of the shadow target complaint. Cleared
     *  when a usable target appears, so a fixed asset logs its recovery. */
    bool bWarnedShadowTarget = false;

    /** The same, for a target another atmosphere is baking into. */
    bool bWarnedSharedShadowTarget = false;

    /** The next cascade in the bake rotation, and the camera each level was
     *  last baked around -- what the material reads that level against. */
    int32 ShadowLevelCursor = 0;
    FVector3f ShadowBakedCamera[AtmoShadowBake::CascadeCount];

    /** The light each level was last baked under, and when, in platform
     *  seconds: what the next bake of that level reprojects and weights its
     *  history by. */
    FVector3f ShadowBakedLight[AtmoShadowBake::CascadeCount];
    double ShadowBakeTime[AtmoShadowBake::CascadeCount] = {};

    /** False until every level has been baked into the current target. Cleared
     *  when the target is reinitialised or the model changes, so the first
     *  request after either bakes all of them. */
    bool bShadowPrimed = false;

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
     *  moved. Double; the terrestrial clocks reduce from it before narrowing,
     *  while the gas giant's Time pin is still a raw float (GG-04). */
    double GetFieldTime() const;

    /** Syncs the directional light's rotation, colour and intensity from the
     *  actor's rotation and the LightColor property. */
    void UpdateLightFromRotation();

    /** The frame the cloud field is defined in: the planet's, which the
     *  atmosphere is attached to, or the world's when it stands alone. The
     *  actor's own rotation is the light direction and never turns the field. */
    FQuat GetFieldFrame() const;

    /** Resolves a soft material reference, logging which one failed. */
    static UMaterialInterface* LoadMaterialAsset(const TSoftObjectPtr<UMaterialInterface>& Ref, const TCHAR* Label);
};