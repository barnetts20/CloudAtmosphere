#pragma once

#include "CoreMinimal.h"
#include "RHIFwd.h"
#include "RenderGraphResources.h"
#include "SceneViewExtension.h"
#include <atomic>

class FTextureResource;
class FTextureRenderTargetResource;
struct FPostProcessMaterialInputs;
struct FScreenPassTexture;

/** The terrestrial march's inputs, flattened for the render thread: the field's
 *  packed pins under the names TR_BUILD_FIELD, TR_BUILD_SCATTER and
 *  TR_BUILD_ATMO expand to, filled from the packers the shadow bake shares, and
 *  the sampling and composite settings. */
struct CLOUDATMOSPHERE_API FTerrestrialMarchParams
{
	/** World position, double: the pass subtracts each view's own camera. */
	FVector PlanetCenter = FVector::ZeroVector;

	FVector4f PlanetRotation = FVector4f(0.0f, 0.0f, 0.0f, 1.0f);
	FVector3f LightDirection = FVector3f(0.0f, 0.0f, 1.0f);
	FVector3f LightColor = FVector3f::OneVector;

	float PlanetRadius = 0.0f;
	float HeightScale = 0.0f;
	float Time = 0.0f;

	// TR_BuildField's packed pins, as PackTerrestrialField writes them.
	FVector4f CloudProfile = FVector4f::Zero();
	FVector4f CloudCurves = FVector4f::Zero();
	FVector4f CloudCoverage = FVector4f::Zero();
	FVector4f CloudType = FVector4f::Zero();
	FVector4f CloudLid = FVector4f::Zero();
	FVector4f CloudLift = FVector4f::Zero();
	FVector4f CloudMotion = FVector4f::Zero();
	FVector4f CloudResponse = FVector4f::Zero();
	FVector4f NoiseLevels = FVector4f::Zero();
	FVector4f StructureSampling = FVector4f::Zero();
	FVector4f StructureWarp = FVector4f::Zero();
	FVector4f DetailSampling = FVector4f::Zero();
	FVector4f DetailWarp = FVector4f::Zero();
	FVector4f CloudGenusStratus = FVector4f::Zero();
	FVector4f CloudGenusStratocumulus = FVector4f::Zero();
	FVector4f CloudGenusCumulus = FVector4f::Zero();
	FVector4f CloudGenusCirrus = FVector4f::Zero();
	FVector4f ShadowCascades = FVector4f::Zero();

	FVector3f CloudScatter = FVector3f::OneVector;
	FVector3f StormScatter = FVector3f::OneVector;
	FVector4f CloudExtinction = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
	FVector4f StormExtinction = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
	float CloudOpticalDepth = 0.0f;
	float LightExtinctionFraction = 0.0f;

	float ForwardG = 0.0f;
	float BackwardG = 0.0f;
	float ForwardWeight = 0.0f;
	FVector3f CloudAmbient = FVector3f::ZeroVector;
	float CloudAmbientFloor = 0.0f;

	float OctaveCount = 1.0f;
	float OctaveAttenuation = 0.0f;
	float OctaveContribution = 0.0f;
	float OctaveEccentricity = 0.0f;

	float TerminatorSoftness = 0.0f;
	float AmbientTerminator = 0.0f;
	float MieLobeDecay = 0.0f;
	float LobeShadowPower = 0.0f;

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
	float ChordSpread = 1.0f;
	FVector4f SurfaceShadow = FVector4f::Zero();

	/** The cameras shadow levels 1 and 2 were baked around, planet-local. */
	FVector3f ShadowCamera1 = FVector3f::ZeroVector;
	FVector3f ShadowCamera2 = FVector3f::ZeroVector;

	// FTerrestrialSamplingParams.
	int32 CellSize = 4;
	float FreshWeight = 0.15f;
	float LatticeGrowth = 0.2f;
	float LatticeGrowthFar = 0.02f;

	// -- Resources: game-thread objects, resolved to RHI handles on the render
	// thread, as the shadow bake does.

	FTextureRenderTargetResource* FlowResource = nullptr;
	FTextureRenderTargetResource* ShadowResource = nullptr;
	FTextureRenderTargetResource* TransmittanceResource = nullptr;
	FTextureResource* StructureResource = nullptr;
	FTextureResource* DetailResource = nullptr;
	FTextureResource* BlueNoiseResource = nullptr;

	FTextureRHIRef FlowTexture;
	FTextureRHIRef ShadowTexture;
	FTextureRHIRef TransmittanceTexture;
	FTextureRHIRef StructureTexture;
	FTextureRHIRef DetailTexture;
	FTextureRHIRef BlueNoiseTexture;

	void ResolveTextures_RenderThread();

	/** Whether the resources the march cannot run without are set. A missing
	 *  noise volume binds black and is left out by its amount, as in the bake. */
	bool IsUsable() const
	{
		return FlowResource && ShadowResource && TransmittanceResource && BlueNoiseResource
			&& PlanetRadius > 0.0f;
	}
};

/** Runs the terrestrial march, its temporal resolve and the composite for every
 *  view of one world, ahead of depth of field and the upscaler: TSR resolves
 *  what the march leaves, and bloom and eye adaptation see the atmosphere.
 *
 *  A VIEW EXTENSION BECAUSE THE RESOLVE KEEPS HISTORY: one per view state,
 *  keyed by its view key, dropped when unused for a few seconds. A view without
 *  a state, such as a scene capture, resolves each frame without history.
 *
 *  ONE PER ATMOSPHERE. The actor creates it, feeds it a params snapshot every
 *  tick and releases it on the render thread. */
class CLOUDATMOSPHERE_API FAtmosphereViewExtension : public FWorldSceneViewExtension
{
public:
	FAtmosphereViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld);

	/** Hands the render thread this frame's march. Game thread. */
	void SetFrame_GameThread(const FTerrestrialMarchParams& March);

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

	/** One view state's history: the resolved atmosphere and its sample counts,
	 *  and the camera, planet and cell size it was resolved with. */
	struct FViewHistory
	{
		TRefCountPtr<IPooledRenderTarget> Color;
		TRefCountPtr<IPooledRenderTarget> Age;
		uint32 CellSize = 0;
		FMatrix44f CameraToClip = FMatrix44f::Identity;
		FVector ViewOrigin = FVector::ZeroVector;
		FVector PlanetCenter = FVector::ZeroVector;
		FQuat PlanetRotation = FQuat::Identity;
		uint32 Frame = 0;
		uint64 LastRendered = 0;
	};

	std::atomic<bool> bEnabled { false };

	// Render thread only.
	FTerrestrialMarchParams March;
	bool bHasFrame = false;
	TMap<uint32, TUniquePtr<FViewHistory>> Histories;
};
