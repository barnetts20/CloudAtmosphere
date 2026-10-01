// THE TRANSFORM IS THE INTERFACE.
//
//   Actor Location  -> planet centre / atmosphere centre
//   Actor Scale max -> planet radius
//   Actor Rotation  -> light direction, and the directional light's rotation
//
// When owned by APlanetActor the scale is set externally to
// max(OceanRadius, PlanetRadius), the visible surface floor.
//
// TWO CLOUD MODELS SHARE ONE FIELD AND ONE MARCH, run as compute passes with a
// temporal resolve (FAtmosphereViewExtension), the sim as their weather map: a
// terrestrial slab over a surface, or a gas giant's deep deck over a saturated
// core. Each model has its own instance of every parameter group and its own
// sim config, with only the pipeline shared between them; see
// AtmosphereParams.h. The world's one sim is driven by the nearest claiming
// planet; the others draw the field they kept (UFlowSimSubsystem).

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
 *  planet-owned, location and scale are locked and rotation stays editable. */
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

    /** Tiling single-channel blue noise for the march's per-pixel draws: sRGB
     *  off, uncompressed grayscale, no mips, nearest filtering. Defaults to the
     *  engine's. Cleared, the atmosphere does not draw, and says so once in the
     *  log. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CloudAtmosphere|Pipeline")
    TObjectPtr<UTexture2D> BlueNoise;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline")
    FAtmosphereSimulationParams Simulation;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Pipeline|Raymarch", meta = (ShowOnlyInnerProperties))
    FAtmosphereRaymarchParams Raymarch;

    // Baked Lighting: the targets the per-frame and on-change bakes write and
    // the march reads. Set once for a performance tier, not tuned for looks. The
    // terrestrial cascade extents are look, and live with its surface shadows.

    /** The deck shadow bake, in the light's frame: a cascade of slices, all one
     *  resolution, each covering a smaller radius. Created on first use, one per
     *  atmosphere, and sized by ShadowResolution. Open it to watch the bake. */
    UPROPERTY(Transient, VisibleInstanceOnly, BlueprintReadOnly, Category = "CloudAtmosphere|Pipeline|Baked Lighting")
    TObjectPtr<UTextureRenderTarget2DArray> ShadowTarget;

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

    /** Edge of each cascade slice, in texels; the shadow target is sized to
     *  it. EVERY LEVEL SHARES
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

    /** Which cloud model renders. A change takes effect on the next tick, and
     *  rebakes the shadow map and restarts the view histories.
     *
     *  BOTH CASES ARE LIVE: one cloud field, the gas giant's a deep deck with no
     *  base (a shader permutation), each with its own parameter groups.
     *  Everything a model's look depends on is twinned, so a type change swaps
     *  the whole authored set rather than reinterpreting one. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CloudAtmosphere|Atmosphere")
    EPlanetAtmosphereType PlanetType = EPlanetAtmosphereType::GasGiant;

    // A GROUP OF ONE GETS NO WRAPPER: a substruct buys a fold-out, worth a click
    // only when there is more than one thing behind it.

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

    /** Parks or wakes the atmosphere: its passes, the light and the per-tick
     *  push, bake and transmittance update. THE ONLY RELIABLE OFF-SWITCH for the
     *  march: a view extension is not a primitive, so hiding the actor does not
     *  stop it, and a stopped tick leaves it drawing its last frame. Parked
     *  planets must call this with false, or every pooled atmosphere keeps
     *  tinting the screen and lighting the scene. Parking gives up the sim,
     *  keeping the field; waking rebakes every shadow level. */
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

    // ONE INSTANCE OF EVERY GROUP PER MODEL: shared STRUCTS, separate INSTANCES,
    // so a type change swaps the whole authored set. Both models run the one
    // cloud field; the gas giant adds its deep deck. A group's default lives
    // with its struct, and the constructor sets the terrestrial air, ambient,
    // multiple scattering and surface shadows on the CDO, which the gas giant
    // copies.

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

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Sampling", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereSamplingParams TerrestrialSampling;

    /** Bound on either cloud surface's slope, in cloud depths per radian: the
     *  cone angle for the shadow bake's entry search. Under-declaring it is the
     *  one way that search steps over cloud, and the symptom is shadow missing
     *  under steep cloud walls, such as a small storm's eyewall. Raise it
     *  first. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Terrestrial|Advanced", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::Terrestrial", EditConditionHides, ClampMin = "0.1"))
    float TerrestrialCloudSlope = 60.0f;

    // Gas giant: the terrestrial groups' twins, and the deep deck.

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialPlanetParams GasGiantPlanet;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Air", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereAirParams Air;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Ambient", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereAmbientParams Ambient;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Clouds|Shape", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialShapeParams GasGiantShape;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Clouds|Deep Deck", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FGasGiantDeepParams GasGiantDeep;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Clouds|Coverage", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialCoverageParams GasGiantCoverage;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Clouds|Type", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialTypeParams GasGiantType;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Clouds|Lift", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialLiftParams GasGiantLift;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Clouds|Warp", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialWarpParams GasGiantWarp;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Clouds|Structure Layer", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialStructureLayerParams GasGiantStructureLayer;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Clouds|Detail Layer", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialDetailLayerParams GasGiantDetailLayer;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Cloud Lighting|Material", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FTerrestrialCloudMaterialParams GasGiantCloudMaterial;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Cloud Lighting|Phase", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmospherePhaseParams Phase;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Cloud Lighting|Multiple Scattering", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereMultipleScatteringParams MultipleScattering;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Cloud Lighting|Surface Shadows", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereSurfaceShadowParams GasGiantSurfaceShadow;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Sampling", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ShowOnlyInnerProperties))
    FAtmosphereSamplingParams GasGiantSampling;

    /** TerrestrialCloudSlope's twin. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Gas Giant|Advanced", meta = (EditCondition = "PlanetType == EPlanetAtmosphereType::GasGiant", EditConditionHides, ClampMin = "0.1"))
    float GasGiantCloudSlope = 60.0f;

    // --- The active model's groups ---

    /** The sim config the active model bids with. */
    UFlowSimConfig* ActiveSimConfig() const;

    /** The clocks this frame's field is drawn at. */
    const FAtmosphereFieldClock& GetFieldClock() const
    {
        return FieldClock;
    }

    const FTerrestrialPlanetParams& ActivePlanet() const
    {
        return bTerrestrial() ? TerrestrialPlanet : GasGiantPlanet;
    }

    /** Above zero: the atmosphere's thickness divides every height. */
    float ActiveHeightScale() const
    {
        return FMath::Max(ActivePlanet().HeightScale, 1e-4f);
    }

    const FTerrestrialStructureLayerParams& ActiveStructureLayer() const
    {
        return bTerrestrial() ? TerrestrialStructureLayer : GasGiantStructureLayer;
    }

    const FTerrestrialDetailLayerParams& ActiveDetailLayer() const
    {
        return bTerrestrial() ? TerrestrialDetailLayer : GasGiantDetailLayer;
    }

    /** The noise volumes, which both models bind under the same names. */
    UVolumeTexture* ActiveStructureVolume() const
    {
        return ActiveStructureLayer().Volume.Get();
    }

    UVolumeTexture* ActiveDetailVolume() const
    {
        return ActiveDetailLayer().Volume.Get();
    }

    const FTerrestrialShapeParams& ActiveShape() const
    {
        return bTerrestrial() ? TerrestrialShape : GasGiantShape;
    }

    const FTerrestrialCoverageParams& ActiveCoverage() const
    {
        return bTerrestrial() ? TerrestrialCoverage : GasGiantCoverage;
    }

    const FTerrestrialTypeParams& ActiveType() const
    {
        return bTerrestrial() ? TerrestrialType : GasGiantType;
    }

    const FTerrestrialLiftParams& ActiveLift() const
    {
        return bTerrestrial() ? TerrestrialLift : GasGiantLift;
    }

    const FTerrestrialWarpParams& ActiveWarp() const
    {
        return bTerrestrial() ? TerrestrialWarp : GasGiantWarp;
    }

    const FTerrestrialCloudMaterialParams& ActiveCloudMaterial() const
    {
        return bTerrestrial() ? TerrestrialCloudMaterial : GasGiantCloudMaterial;
    }

    float ActiveCloudSlope() const
    {
        return bTerrestrial() ? TerrestrialCloudSlope : GasGiantCloudSlope;
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

    const FAtmosphereSamplingParams& ActiveSampling() const
    {
        return bTerrestrial() ? TerrestrialSampling : GasGiantSampling;
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
    void InitializeFromPlanet(USceneComponent* InAttachParent,
        FVector InScale = FVector::ZeroVector);

private:
    /** Root component; carries the planet radius as scale. */
    UPROPERTY()
    TObjectPtr<USceneComponent> AtmosphereRoot;

    /** Synced from the actor's rotation and LightColor; absolute rotation and
     *  scale, so the planet's frame and the root's radius never reach it. A
     *  COMPONENT, NOT A CHILD ACTOR: a duplicate or a deletion takes its own and
     *  never another's. */
    UPROPERTY(VisibleAnywhere, Category = "CloudAtmosphere|Pipeline")
    TObjectPtr<UDirectionalLightComponent> SunLightComponent;

    /** Legacy child actors a saved level can still hold, destroyed on
     *  Initialize. */
    UPROPERTY()
    TObjectPtr<APostProcessVolume> PostProcessVolume_DEPRECATED = nullptr;

    UPROPERTY()
    TObjectPtr<ADirectionalLight> SunLight_DEPRECATED = nullptr;

    /** Air transmittance table, created on first use. Visible for inspection,
     *  though it depends only on the radii and the air profile so there is
     *  nothing to watch it do. */
    UPROPERTY(Transient, VisibleInstanceOnly, Category = "CloudAtmosphere|Pipeline|Baked Lighting")
    TObjectPtr<UTextureRenderTarget2D> TransmittanceTable = nullptr;

    /** Inputs of the last enqueued bake; reset when the table is recreated. */
    FAtmosphereTransmittanceParams TransmittanceBaked;

    /** What every Active* accessor asks. */
    bool bTerrestrial() const { return PlanetType == EPlanetAtmosphereType::Terrestrial; }

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

    /** Cached scale set by the planet actor, used by the transform guard. */
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

    /** Queues this frame's deck shadow bake with the sim subsystem. The field
     *  comes from the packer the march shares, and the bake derives from it with
     *  the same shader functions, which keeps the deck the light sees identical
     *  to the deck the eye sees. */
    void RequestShadowBake(float PlanetRadius, const FVector& PlanetCenter, const FVector& LightDir);

    /** Fills the half of a bake request that is not the field: the map, the
     *  frame, the light, the camera, the history and the volumes. Returns false
     *  when the request could not be made usable. Records nothing;
     *  CommitShadowBake does once the subsystem takes the request. */
    bool FillShadowRequest(FTerrestrialShadowParams& Params, float PlanetRadius,
        const FVector& PlanetCenter, const FVector& LightDir);

    /** Records an accepted bake: the cameras the march reads the fine levels
     *  against, and the light and time the next bake reprojects from. */
    void CommitShadowBake(uint32 LevelMask, const FVector3f& LightDir, const FVector3f& CameraLocal);

    /** Creates the shadow target if absent and forces it to RGBA16F with UAV
     *  support at ShadowResolution, cleared to the no-deck sentinel,
     *  reinitialising only on a mismatch. */
    void PrepareShadowTarget();

    /** The next cascade in the bake rotation, and the camera each level was
     *  last baked around -- what the march reads that level against. */
    int32 ShadowLevelCursor = 0;
    FVector3f ShadowBakedCamera[AtmoShadowBake::CascadeCount];

    /** The light each level was last baked under, and when, in platform
     *  seconds: what the next bake of that level reprojects and weights its
     *  history by. */
    FVector3f ShadowBakedLight[AtmoShadowBake::CascadeCount];
    double ShadowBakeTime[AtmoShadowBake::CascadeCount] = {};

    /** False until every level has been baked into the current target. Cleared
     *  when the target is reinitialised, the model or the field changes or the
     *  atmosphere wakes, so the first request after any of them bakes all of
     *  them. */
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
