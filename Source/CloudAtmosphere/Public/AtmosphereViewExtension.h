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
	float LatticeGrowthFar = 0.0f;

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

	// -- ISceneViewExtension

	virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
	virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
	virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

	virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView,
		FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled) override;

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
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
};
