#pragma once

#include "CoreMinimal.h"
#include "RHIFwd.h"
#include "RenderGraphResources.h"
#include "SceneViewExtension.h"
#include "TerrestrialShadowMap.h"
#include <atomic>

class FTextureResource;
class FTextureRenderTargetResource;
struct FPostProcessMaterialInputs;
struct FScreenPassTexture;

/** ATMO_MODEL, the march's permutation: the field it compiles. */
namespace AtmosphereMarchModel
{
	constexpr int32 Slab = 0;
	constexpr int32 DeepDeck = 1;
	constexpr int32 AirOnly = 2;
	constexpr int32 Count = 3;
}

/** Stage A3: the sky tables' sizes (GetSkyTables_RenderThread; AtmosphereMarchPass.usf's
 *  MainSkyLutCS for their mapping): the sky by the view's zenith, its azimuth from the sun
 *  and the sun's zenith, and its irradiance on the horizontal by the sun's zenith. */
namespace AtmosphereSkyTables
{
	constexpr int32 ViewZenith = 64;
	constexpr int32 Azimuth = 32;
	constexpr int32 SunZenith = 32;
	constexpr int32 Irradiance = 64;
}

/** The march's inputs, flattened for the render thread: the active model's
 *  field, from the packer the shadow bake shares, and its lighting, air,
 *  pipeline and sampling groups. The actor's FillMarchParams sets every member,
 *  so the defaults are zero rather than a second set of values. */
struct CLOUDATMOSPHERE_API FAtmosphereMarchParams
{
	/** AtmosphereMarchModel: the slab, the gas giant's deep deck, or air alone. */
	int32 Model = AtmosphereMarchModel::Slab;

	/** World position, double: the pass subtracts each view's own camera. */
	FVector PlanetCenter = FVector::ZeroVector;

	FVector4f PlanetRotation = FVector4f(0.0f, 0.0f, 0.0f, 1.0f);
	FVector3f LightDirection = FVector3f(0.0f, 0.0f, 1.0f);
	FVector3f LightColor = FVector3f::OneVector;

	float PlanetRadius = 0.0f;
	float HeightScale = 0.0f;

	// The field and its cloud material's albedo.
	FTerrestrialFieldParameters Field{};
	FVector3f CloudScatter = FVector3f::OneVector;
	FVector3f StormScatter = FVector3f::OneVector;
	FVector3f DeepScatter = FVector3f::OneVector;

	float LightExtinctionFraction = 0.0f;

	float ForwardG = 0.0f;
	float BackwardG = 0.0f;
	float ForwardWeight = 0.0f;
	FVector3f CloudAmbient = FVector3f::ZeroVector;
	float CloudAmbientFloor = 0.0f;

	float OctaveCount = 0.0f;
	float OctaveAttenuation = 0.0f;
	float OctaveEccentricity = 0.0f;

	float AmbientTerminator = 0.0f;
	float MieLobeDecay = 0.0f;

	FVector3f RayleighBeta = FVector3f::ZeroVector;
	float RayleighScaleHeight = 0.0f;
	FVector3f MieBeta = FVector3f::ZeroVector;
	float MieScaleHeight = 0.0f;
	float MieG = 0.0f;
	FVector3f AbsorptionBeta = FVector3f::ZeroVector;
	float AbsorptionAltitude = 0.0f;
	float AbsorptionFalloff = 0.0f;
	FVector3f AtmosphereAmbient = FVector3f::ZeroVector;
	float AtmosphereAmbientFloor = 0.0f;

	float AtmosphereSteps = 0.0f;
	float CloudSteps = 0.0f;
	float ChordSpread = 0.0f;
	FVector4f SurfaceShadow = FVector4f::Zero();

	/** The cameras shadow levels 1 and 2 were baked around, and the light each
	 *  level was baked under, planet-local. */
	FVector3f ShadowCamera1 = FVector3f::ZeroVector;
	FVector3f ShadowCamera2 = FVector3f::ZeroVector;
	FVector3f ShadowLight[AtmoShadowBake::CascadeCount] = { FVector3f::ZeroVector, FVector3f::ZeroVector, FVector3f::ZeroVector };

	// FAtmosphereSamplingParams.
	int32 CellSize = 0;
	float FreshWeight = 0.0f;
	float LatticeGrowth = 0.0f;

	// -- Resources: game-thread objects, resolved to RHI handles on the render
	// thread, as the shadow bake does.

	FTextureRenderTargetResource* FlowResource = nullptr;
	FTextureRenderTargetResource* ShadowResource = nullptr;
	FTextureRenderTargetResource* TransmittanceResource = nullptr;
	FTextureRenderTargetResource* CoverageResource = nullptr;
	FTextureResource* StructureResource = nullptr;
	FTextureResource* DetailResource = nullptr;
	FTextureResource* BlueNoiseResource = nullptr;

	FTextureRHIRef FlowTexture;
	FTextureRHIRef ShadowTexture;
	FTextureRHIRef TransmittanceTexture;
	FTextureRHIRef CoverageTexture;
	FTextureRHIRef StructureTexture;
	FTextureRHIRef DetailTexture;
	FTextureRHIRef BlueNoiseTexture;

	void ResolveTextures_RenderThread();

	/** Whether the resources the march cannot run without are set. A missing
	 *  noise volume binds black and is left out by its amount, as in the bake;
	 *  air alone reads none of the cloud's, which bind black. */
	bool IsUsable() const
	{
		const bool bClouds = Model != AtmosphereMarchModel::AirOnly;

		return TransmittanceResource && BlueNoiseResource && PlanetRadius > 0.0f
			&& (!bClouds || (FlowResource && ShadowResource && CoverageResource));
	}
};

/** Runs the march, its temporal resolve and the composite for every view of one
 *  world, ahead of depth of field and the upscaler: TSR resolves what the march
 *  leaves, and bloom and eye adaptation see the atmosphere.
 *
 *  A VIEW EXTENSION BECAUSE THE RESOLVE KEEPS HISTORY: one per view state,
 *  keyed by its view key, dropped when unused for HistoryLifetime frames. A view without
 *  a state, such as a scene capture, resolves each frame without history.
 *
 *  ONE PER ATMOSPHERE. The actor creates it, feeds it a params snapshot every
 *  tick and releases it on the render thread. */
class CLOUDATMOSPHERE_API FAtmosphereViewExtension : public FWorldSceneViewExtension
{
public:
	FAtmosphereViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld);

	/** Hands the render thread this frame's march. Game thread. */
	void SetFrame_GameThread(const FAtmosphereMarchParams& March);

	/** Off skips every view and frees the histories, which a gap in rendering
	 *  invalidates anyway. Game thread. */
	void SetEnabled(bool bInEnabled);

	/** Frees the histories. Render thread, ahead of the last release. */
	void ReleaseHistories_RenderThread();

	// -- Stage A2: a renderer that lights its own sun (the ocean) -----------
	//
	// The march dims what it composites over by the surface shadow, at the
	// scene depth: Transmittance *= 1 - DirectFraction * SunUp * (1 - Sun), the
	// sun at the surface through the deck's shadow and the air. A renderer that
	// lights its own sun takes Sun and SunUp at its own points from
	// AddSurfaceSunPass_RenderThread, the same expression (Atmo_SurfaceSun),
	// dims its sun by them and what it shows from behind itself by the
	// multiply, and marks its pixels with SetSelfLitMask_RenderThread, which
	// the march leaves its multiply off.

	/** Whether the march dims surfaces this frame: on, with a frame, usable,
	 *  and the surface shadow enabled. Render thread. */
	bool IsSurfaceShadowOn_RenderThread() const;

	/** The sun at surface points, as the march's surface shadow takes it.
	 *  Points: Buffer<float>, three floats each in their own frame; Count of
	 *  them; PointsToView: that frame to the view's camera-relative world, row
	 *  vectors (the rotation and scale, then the frame's origin from the view's
	 *  camera, formed in double). Out: RWBuffer<float4>, 2 * Count elements:
	 *  float4(Sun, SunUp) and float4(Air, DirectFraction). A NaN point keeps
	 *  (1, 1, 1, 1), (1, 1, 1, 0).
	 *  False: nothing added (off, no frame, the surface shadow disabled); light
	 *  the sun undimmed and mark no pixels. Render thread, in the frame's graph. */
	bool AddSurfaceSunPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
		FRDGBufferSRVRef Points, uint32 Count, const FMatrix44f& PointsToView, FRDGBufferUAVRef Out);

	/** This frame's self-lit pixels for View: a Texture2D<float> of the view
	 *  rect (texel 0 its corner), above 0.5 where the renderer dimmed its own
	 *  sun and what it shows from behind itself. In the graph the post-process
	 *  pass runs in (the scene renderer's), before it. Null clears the view's.
	 *  Render thread. */
	void SetSelfLitMask_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View, FRDGTextureRef Mask);

	/** Stage A3: the air's sky at the surface, per unit of the light above the
	 *  air, for a renderer that shades its own sky. OutSky: a Texture3D,
	 *  AtmosphereSkyTables' sizes, RGBA half: the radiance by (sqrt of the
	 *  view's zenith cosine, its azimuth from the sun over pi, 0.5 + 0.5
	 *  sign(mus) sqrt(|mus|) for the sun's zenith cosine), at texel centres.
	 *  OutIrradiance: a Texture2D, AtmosphereSkyTables::Irradiance x 1, the
	 *  irradiance on the horizontal by the sun's coordinate alone. Clear air
	 *  only: no cloud. Made when the air changes and every 30 frames; shared
	 *  by every view. False: none (off, no frame). Render thread. */
	bool GetSkyTables_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
		FRDGTextureRef& OutSky, FRDGTextureRef& OutIrradiance);

	/** Stage A6: the flow sim's weather at surface points (AddSurfaceSunPass's
	 *  inputs), as the field reads its columns: float4(cloud cover 0..1, storm
	 *  0..1, vertical motion -1..1 rising positive, 0), one per point. False:
	 *  none (off, no frame, air alone). Render thread. */
	bool AddSurfaceWeatherPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
		FRDGBufferSRVRef Points, uint32 Count, const FMatrix44f& PointsToView, FRDGBufferUAVRef Out);

	/** Stage A3b: the march itself along a renderer's own rays (the ocean's mirror
	 *  directions): the air and the cloud a reflection sees, lit as the view sees
	 *  them. Rays: Buffer<float>, six floats each, origin (camera-relative) then
	 *  direction; GridWidth: the rays' grid width, which seeds their blue noise by
	 *  cell; the air's and the cloud's step counts (the view's are far more). Out:
	 *  RWBuffer<float4>, Count elements: float4(the light gathered, absolute, as
	 *  the march's; its mean transmittance). Nothing but the planet and the shell
	 *  stops a ray. False: none (off, no frame). Render thread. */
	bool AddReflectionPass_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
		FRDGBufferSRVRef Rays, uint32 Count, uint32 GridWidth, int32 AtmosphereSteps, int32 CloudSteps, FRDGBufferUAVRef Out);

	// -- ISceneViewExtension

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView,
		FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	/** Whether this frame's march has what it cannot run without. */
	bool CanMarch_RenderThread() const;

	/** One view's self-lit mask, valid in its own graph and frame only. */
	struct FSelfLitMask
	{
		const FSceneView* View = nullptr;
		FMatrix ViewMatrix = FMatrix::Identity;
		const FRDGBuilder* Builder = nullptr;
		uint64 Frame = 0;
		FRDGTextureRef Mask = nullptr;
	};

	const FSelfLitMask* FindSelfLitMask(FRDGBuilder& GraphBuilder, const FSceneView& View) const;

	FScreenPassTexture Render_RenderThread(FRDGBuilder& GraphBuilder, const FSceneView& View,
		const FPostProcessMaterialInputs& Inputs);

	/** One view state's history: the resolved atmosphere, its red and blue
	 *  transmittance and its sample counts, and the model, camera, planet and
	 *  cell size it was resolved with. */
	struct FViewHistory
	{
		TRefCountPtr<IPooledRenderTarget> Color;
		TRefCountPtr<IPooledRenderTarget> Tint;
		TRefCountPtr<IPooledRenderTarget> Age;
		int32 Model = -1;
		uint32 CellSize = 0;
		FMatrix44f CameraToClip = FMatrix44f::Identity;
		FVector ViewOrigin = FVector::ZeroVector;
		FVector PlanetCenter = FVector::ZeroVector;
		FQuat PlanetRotation = FQuat::Identity;
		float SpinAngle = 0.0f;
		uint32 Frame = 0;
		uint64 LastRendered = 0;
	};

	std::atomic<bool> bEnabled { false };

	// Render thread only.
	FAtmosphereMarchParams March;
	bool bHasFrame = false;
	TMap<uint32, TUniquePtr<FViewHistory>> Histories;
	TArray<FSelfLitMask> SelfLitMasks;

	// Stage A3: the sky tables, the key they were made under and when.
	TRefCountPtr<IPooledRenderTarget> SkyLut;
	TRefCountPtr<IPooledRenderTarget> SkyIrradiance;
	uint32 SkyKey = 0;
	uint64 SkyFrame = 0;
};
