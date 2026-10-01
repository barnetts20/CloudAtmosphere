#include "AtmosphereViewExtension.h"

#include "AtmosphereTransmittance.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "ScreenPass.h"
#include "ShaderCompilerCore.h"
#include "ShaderParameterStruct.h"
#include "ShaderPermutation.h"
#include "TextureResource.h"

// ---------------------------------------------------------------------------
// Console variables
// ---------------------------------------------------------------------------

static TAutoConsoleVariable<int32> CVarAtmosphereTemporalDebug(
	TEXT("r.CloudAtmosphere.Temporal.Debug"),
	0,
	TEXT("Temporal resolve view: 0 off, 1 samples accumulated over their cap, 2 fresh blue,\n")
	TEXT("reprojected green, filled red, 3 this frame's samples alone, 4 motion red, clamp rejection\n")
	TEXT("green. Shown only; the history keeps the real result."),
	ECVF_RenderThreadSafe);

namespace
{
	constexpr int32 ThreadGroupSize = 8;

	/** Frames a history survives unrendered. */
	constexpr uint64 HistoryLifetime = 300;
}

// ---------------------------------------------------------------------------
// Shaders
//
// Parameter names must match the .usf declarations exactly. The march's are the
// field builders' argument names, which TR_BUILD_FIELD, TR_BUILD_SCATTER and
// TR_BUILD_ATMO expand to; the field's own come from FTerrestrialFieldParameters,
// which the bake includes too.
// ---------------------------------------------------------------------------

BEGIN_SHADER_PARAMETER_STRUCT(FAtmosphereMarchParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FTerrestrialFieldParameters, Field)
	SHADER_PARAMETER(FVector3f, CloudScatter)
	SHADER_PARAMETER(FVector3f, StormScatter)

	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, AtmoSceneDepth)
	SHADER_PARAMETER(FMatrix44f, RayViewToClip)
	SHADER_PARAMETER(float, LatticeGrowth)
	SHADER_PARAMETER(float, LatticeGrowthFar)

	SHADER_PARAMETER(FVector3f, PlanetOffset)
	SHADER_PARAMETER(FVector4f, PlanetRotation)
	SHADER_PARAMETER(FVector3f, LightDirection)
	SHADER_PARAMETER(FVector3f, LightColor)
	SHADER_PARAMETER(float, PlanetRadius)
	SHADER_PARAMETER(float, HeightScale)

	SHADER_PARAMETER(float, LightExtinctionFraction)

	SHADER_PARAMETER(float, ForwardG)
	SHADER_PARAMETER(float, BackwardG)
	SHADER_PARAMETER(float, ForwardWeight)
	SHADER_PARAMETER(FVector3f, CloudAmbient)
	SHADER_PARAMETER(float, CloudAmbientFloor)

	SHADER_PARAMETER(float, OctaveCount)
	SHADER_PARAMETER(float, OctaveAttenuation)
	SHADER_PARAMETER(float, OctaveEccentricity)

	SHADER_PARAMETER(float, AmbientTerminator)
	SHADER_PARAMETER(float, MieLobeDecay)

	SHADER_PARAMETER(FVector3f, RayleighBeta)
	SHADER_PARAMETER(float, RayleighScaleHeight)
	SHADER_PARAMETER(FVector3f, MieBeta)
	SHADER_PARAMETER(float, MieScaleHeight)
	SHADER_PARAMETER(float, MieG)
	SHADER_PARAMETER(FVector3f, AbsorptionBeta)
	SHADER_PARAMETER(float, AbsorptionAltitude)
	SHADER_PARAMETER(float, AbsorptionFalloff)
	SHADER_PARAMETER(FVector3f, AtmosphereAmbient)
	SHADER_PARAMETER(float, AtmosphereAmbientFloor)

	SHADER_PARAMETER(float, AtmosphereSteps)
	SHADER_PARAMETER(float, CloudSteps)
	SHADER_PARAMETER(float, ChordSpread)
	SHADER_PARAMETER(FVector4f, SurfaceShadow)

	SHADER_PARAMETER(FVector3f, ShadowCamera1)
	SHADER_PARAMETER(FVector3f, ShadowCamera2)

	SHADER_PARAMETER_TEXTURE(Texture2DArray, FlowTarget)
	SHADER_PARAMETER_SAMPLER(SamplerState, FlowTargetSampler)
	SHADER_PARAMETER_TEXTURE(Texture3D, StructureVolume)
	SHADER_PARAMETER_SAMPLER(SamplerState, StructureVolumeSampler)
	SHADER_PARAMETER_TEXTURE(Texture3D, DetailVolume)
	SHADER_PARAMETER_SAMPLER(SamplerState, DetailVolumeSampler)
	SHADER_PARAMETER_TEXTURE(Texture2D, BlueNoiseTexture)
	SHADER_PARAMETER_TEXTURE(Texture2DArray, ShadowTarget)
	SHADER_PARAMETER_SAMPLER(SamplerState, ShadowTargetSampler)
	SHADER_PARAMETER_TEXTURE(Texture2D, TransmittanceTable)
	SHADER_PARAMETER_SAMPLER(SamplerState, TransmittanceTableSampler)

	SHADER_PARAMETER(FUintVector2, MarchCells)
	SHADER_PARAMETER(uint32, MarchStride)
	SHADER_PARAMETER(FVector2f, MarchOffset)
	SHADER_PARAMETER(FVector2f, OutputSize)
	SHADER_PARAMETER(float, JitterShift)
	SHADER_PARAMETER(FUintVector2, NoiseOffset)

	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, MarchColor)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, MarchTint)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, MarchDepth)
END_SHADER_PARAMETER_STRUCT()

/** Both models' march: TR_DEEP_DECK selects the gas giant's deep deck. */
class FAtmosphereMarchCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAtmosphereMarchCS);

	using FParameters = FAtmosphereMarchParameters;
	SHADER_USE_PARAMETER_STRUCT(FAtmosphereMarchCS, FGlobalShader);

	class FDeepDeck : SHADER_PERMUTATION_BOOL("TR_DEEP_DECK");
	using FPermutationDomain = TShaderPermutationDomain<FDeepDeck>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("ATMO_MARCH_THREADS"), ThreadGroupSize);
		OutEnvironment.SetDefine(TEXT("ATMO_TRANSMITTANCE_WIDTH"), AtmosphereTransmittance::Width);
		OutEnvironment.SetDefine(TEXT("ATMO_TRANSMITTANCE_HEIGHT"), AtmosphereTransmittance::Height);
	}
};

IMPLEMENT_GLOBAL_SHADER(FAtmosphereMarchCS,
	"/Plugin/CloudAtmosphere/Private/AtmosphereMarchPass.usf", "MainMarchCS", SF_Compute);

BEGIN_SHADER_PARAMETER_STRUCT(FAtmosphereResolveParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, AtmoSceneDepth)
	SHADER_PARAMETER(FMatrix44f, RayViewToClip)
	SHADER_PARAMETER(FVector2f, OutputSize)

	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, FreshColor)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, FreshTint)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, FreshDepth)
	SHADER_PARAMETER(FUintVector2, FreshSize)
	SHADER_PARAMETER(uint32, CellSize)
	SHADER_PARAMETER(FUintVector2, CellOffset)

	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, HistoryColor)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, HistoryTint)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, HistoryAge)
	SHADER_PARAMETER_SAMPLER(SamplerState, HistorySampler)
	SHADER_PARAMETER(uint32, HistoryValid)
	SHADER_PARAMETER(FMatrix44f, PrevCameraToClip)
	SHADER_PARAMETER(FMatrix44f, PrevPlanetToClip)

	SHADER_PARAMETER(float, FreshWeight)
	SHADER_PARAMETER(uint32, TemporalDebug)

	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ResolvedColor)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, ResolvedTint)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, ResolvedAge)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, ResolvedDisplay)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FAtmosphereCompositeParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER(FVector2f, OutputSize)

	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, AtmosphereColor)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float2>, AtmosphereTint)
	SHADER_PARAMETER(uint32, TintActive)

	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SceneColorTexture)
	SHADER_PARAMETER(FUintVector2, SceneColorMin)

	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, CompositeOutput)
END_SHADER_PARAMETER_STRUCT()

/** The resolve and the composite share AtmosphereTemporal.usf. */
class FAtmosphereTemporalShader : public FGlobalShader
{
public:
	FAtmosphereTemporalShader() = default;
	FAtmosphereTemporalShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(
		const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("ATMO_TEMPORAL_THREADS"), ThreadGroupSize);
	}
};

class FAtmosphereResolveCS : public FAtmosphereTemporalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAtmosphereResolveCS);

	using FParameters = FAtmosphereResolveParameters;
	SHADER_USE_PARAMETER_STRUCT(FAtmosphereResolveCS, FAtmosphereTemporalShader);
};

class FAtmosphereCompositeCS : public FAtmosphereTemporalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAtmosphereCompositeCS);

	using FParameters = FAtmosphereCompositeParameters;
	SHADER_USE_PARAMETER_STRUCT(FAtmosphereCompositeCS, FAtmosphereTemporalShader);
};

IMPLEMENT_GLOBAL_SHADER(FAtmosphereResolveCS,
	"/Plugin/CloudAtmosphere/Private/AtmosphereTemporal.usf", "MainResolveCS", SF_Compute);

IMPLEMENT_GLOBAL_SHADER(FAtmosphereCompositeCS,
	"/Plugin/CloudAtmosphere/Private/AtmosphereTemporal.usf", "MainCompositeCS", SF_Compute);

// ---------------------------------------------------------------------------
// Params
// ---------------------------------------------------------------------------

void FAtmosphereMarchParams::ResolveTextures_RenderThread()
{
	check(IsInRenderingThread());

	const auto RenderTarget = [](FTextureRenderTargetResource* Resource)
		{
			return Resource ? FTextureRHIRef(Resource->GetRenderTargetTexture()) : FTextureRHIRef();
		};

	const auto Texture = [](FTextureResource* Resource)
		{
			return Resource ? Resource->TextureRHI : FTextureRHIRef();
		};

	FlowTexture = RenderTarget(FlowResource);
	ShadowTexture = RenderTarget(ShadowResource);
	TransmittanceTexture = RenderTarget(TransmittanceResource);
	StructureTexture = Texture(StructureResource);
	DetailTexture = Texture(DetailResource);
	BlueNoiseTexture = Texture(BlueNoiseResource);
}

// ---------------------------------------------------------------------------
// Extension
// ---------------------------------------------------------------------------

FAtmosphereViewExtension::FAtmosphereViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld)
	: FWorldSceneViewExtension(AutoRegister, InWorld)
{
}

void FAtmosphereViewExtension::SetFrame_GameThread(const FAtmosphereMarchParams& InMarch)
{
	check(IsInGameThread());

	TSharedRef<FAtmosphereViewExtension, ESPMode::ThreadSafe> Self =
		StaticCastSharedRef<FAtmosphereViewExtension>(AsShared());

	ENQUEUE_RENDER_COMMAND(AtmosphereSetFrame)(
		[Self, Params = InMarch](FRHICommandListImmediate&) mutable
		{
			Params.ResolveTextures_RenderThread();

			Self->March = MoveTemp(Params);
			Self->bHasFrame = true;
		});
}

void FAtmosphereViewExtension::SetEnabled(bool bInEnabled)
{
	check(IsInGameThread());

	if (bEnabled.exchange(bInEnabled) && !bInEnabled)
	{
		TSharedRef<FAtmosphereViewExtension, ESPMode::ThreadSafe> Self =
			StaticCastSharedRef<FAtmosphereViewExtension>(AsShared());

		ENQUEUE_RENDER_COMMAND(AtmosphereReleaseHistories)(
			[Self](FRHICommandListImmediate&)
			{
				Self->Histories.Empty();
			});
	}
}

void FAtmosphereViewExtension::ReleaseHistories_RenderThread()
{
	check(IsInRenderingThread());

	Histories.Empty();
	bHasFrame = false;
}

bool FAtmosphereViewExtension::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	return bEnabled.load() && FWorldSceneViewExtension::IsActiveThisFrame_Internal(Context);
}

void FAtmosphereViewExtension::SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& InView,
	FPostProcessingPassDelegateArray& InOutPassCallbacks, bool bIsPassEnabled)
{
	// Ahead of depth of field and the upscaler. PITFALL: TRANSLUCENCY IS NOT IN
	// SCENE COLOUR YET. It is merged at depth of field, so it draws over the
	// atmosphere unattenuated; the engine's own clouds composite before it.
	if (Pass == EPostProcessingPass::BeforeDOF)
	{
		InOutPassCallbacks.Add(FPostProcessingPassDelegate::CreateRaw(
			this, &FAtmosphereViewExtension::Render_RenderThread));
	}
}

namespace
{
	/** The pixel of an N x N cell at rank Index. The cell's pixels are ranked by
	 *  the R2 lattice's value, an ordered dither for any N, so pixels marched on
	 *  consecutive frames sit far apart. Built once per size. */
	FIntPoint CellPixel(uint32 Index, uint32 N)
	{
		static TMap<uint32, TArray<FIntPoint>> Orders;

		TArray<FIntPoint>* Order = Orders.Find(N);

		if (!Order)
		{
			Order = &Orders.Add(N);

			for (uint32 y = 0; y < N; ++y)
			{
				for (uint32 x = 0; x < N; ++x)
				{
					Order->Add(FIntPoint(x, y));
				}
			}

			const auto Rank = [](const FIntPoint& Pixel)
				{
					return FMath::Frac(0.7548776662 * Pixel.X + 0.5698402910 * Pixel.Y);
				};

			Order->StableSort([&Rank](const FIntPoint& A, const FIntPoint& B) { return Rank(A) < Rank(B); });
		}

		return (*Order)[Index % (N * N)];
	}

	FRDGTextureRef CreateTarget(FRDGBuilder& GraphBuilder, FIntPoint Size, EPixelFormat Format, const TCHAR* Name)
	{
		return GraphBuilder.CreateTexture(
			FRDGTextureDesc::Create2D(Size, Format, FClearValueBinding::Black,
				TexCreate_ShaderResource | TexCreate_UAV),
			Name);
	}
}

FScreenPassTexture FAtmosphereViewExtension::Render_RenderThread(
	FRDGBuilder& GraphBuilder, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
{
	const FScreenPassTextureSlice SceneColorSlice = Inputs.GetInput(EPostProcessMaterialInput::SceneColor);

	const FSceneTextureUniformParameters* SceneTextures = Inputs.SceneTextures.SceneTextures
		? Inputs.SceneTextures.SceneTextures->GetContents()
		: nullptr;

	if (!bHasFrame || !March.IsUsable() || !SceneColorSlice.IsValid()
		|| !SceneTextures || !SceneTextures->SceneDepthTexture
		|| !March.FlowTexture.IsValid() || !March.ShadowTexture.IsValid()
		|| !March.TransmittanceTexture.IsValid() || !March.BlueNoiseTexture.IsValid()
		|| View.bIsReflectionCapture || View.bIsPlanarReflection
		|| !View.IsPerspectiveProjection()
		|| View.GetFeatureLevel() < ERHIFeatureLevel::SM5)
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	RDG_EVENT_SCOPE(GraphBuilder, "CloudAtmosphere");

	const FScreenPassTexture SceneColor = FScreenPassTexture::CopyFromSlice(GraphBuilder, SceneColorSlice);
	const FIntRect Rect = SceneColor.ViewRect;
	const FIntPoint OutputSize = Rect.Size();

	if (OutputSize.X <= 0 || OutputSize.Y <= 0)
	{
		return Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
	}

	FRDGTextureRef SceneDepth = SceneTextures->SceneDepthTexture;
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(View.GetFeatureLevel());

	FRHISamplerState* BilinearClamp = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	// -- The view's history ---------------------------------------------------
	//
	// Only a view with a state keeps one; a view without, such as a scene
	// capture, resolves every frame from that frame's samples alone. A cut, a
	// gap in rendering, a model change or a new cell size starts it over. PITFALL: NOT
	// A RESIZE. The view rect is the internal resolution, which the editor and
	// dynamic resolution change from frame to frame; the resolve reads the
	// history by UV at its own size.

	const uint32 ViewKey = View.State ? View.State->GetViewKey() : 0u;
	const uint32 CellSize = (uint32)FMath::Clamp(March.CellSize, 1, 16);

	const FVector ViewOrigin = View.ViewMatrices.GetViewOrigin();

	// The march's rays take the jittered projection, so they land on the pixels
	// the jittered depth holds. The history is unjittered: each pixel is its
	// centre's ray, and the jittered samples accumulate into it as TAA's do.
	// PITFALL: A HISTORY REPROJECTED THROUGH THE JITTERED PROJECTION MOVES
	// UNDER A STILL CAMERA. Each frame's lookup lands at the jitter's difference
	// from the last, up to a pixel, and the bilinear fetch re-interpolates every
	// pixel-scale detail at a new offset: static in thin cloud and at edges that
	// no weight or clamp reaches.
	const FMatrix UnjitteredProjection = View.ViewMatrices.GetProjectionNoAAMatrix();
	const FMatrix44f RayViewToClip = FMatrix44f(View.ViewMatrices.GetProjectionMatrix());
	const FMatrix44f HistoryViewToClip = FMatrix44f(UnjitteredProjection);

	// Camera-relative world to the history's clip space: the view rotation, no
	// translation, then the unjittered projection.
	const FMatrix44f CameraToClip = FMatrix44f(
		View.ViewMatrices.GetViewMatrix().RemoveTranslation() * UnjitteredProjection);

	// HELD BY POINTER: the extractions below write into it when the graph
	// executes, after other views of the family may have grown the map.
	FViewHistory* History = nullptr;

	if (ViewKey != 0u)
	{
		TUniquePtr<FViewHistory>& Entry = Histories.FindOrAdd(ViewKey);

		if (!Entry)
		{
			Entry = MakeUnique<FViewHistory>();
		}

		History = Entry.Get();
	}

	const FQuat PlanetRotation = FQuat(
		March.PlanetRotation.X, March.PlanetRotation.Y, March.PlanetRotation.Z, March.PlanetRotation.W).GetNormalized();

	const bool bHistoryValid = History
		&& History->Color.IsValid() && History->Tint.IsValid() && History->Age.IsValid()
		&& History->LastRendered + 2 >= GFrameCounterRenderThread
		&& History->bGasGiant == March.bGasGiant
		&& History->CellSize == CellSize
		&& !View.bCameraCut;

	// A point fixed to the planet, from this frame's camera-relative position to
	// the previous frame's: turned by the planet's rotation since then, then
	// offset by where the camera stood against the planet. Formed in double.
	// PITFALL: REPROJECTED IN WORLD SPACE, A CAMERA RIDING A MOVING PLANET SEES
	// EVERY PIXEL MOVE by the planet's travel, and the clamp and fill run at
	// their motion settings over the whole view.
	FMatrix44f PlanetDelta = FMatrix44f::Identity;

	if (bHistoryValid)
	{
		const FQuat Turn = History->PlanetRotation * PlanetRotation.Inverse();
		const FVector Move = Turn.RotateVector(ViewOrigin - March.PlanetCenter)
			- (History->ViewOrigin - History->PlanetCenter);

		PlanetDelta = FMatrix44f(FQuatRotationTranslationMatrix(Turn, Move));
	}

	// -- March ----------------------------------------------------------------

	// A frame without history marches at most every other pixel, so the view
	// starts from more than a fill of sparse samples. A view without state never
	// has history, so it marches every pixel: at any larger N it would march one
	// fixed rank each frame, a dither upsampled into every scene capture.
	const uint32 MarchN = !History ? 1u : (bHistoryValid ? CellSize : FMath::Min(CellSize, 2u));
	const uint32 CellPixels = MarchN * MarchN;

	// The rank marched this frame, rotated each cycle so a pixel's revisits are
	// an odd number of frames apart: by one for an even N^2, by two for an odd
	// one. PITFALL: a pixel revisited a multiple of the engine's jitter period
	// apart (8) lands on the same sub-pixel jitter each visit, as N^2 does, and
	// N^2 - 1 does for every odd N.
	const uint32 Frame = bHistoryValid ? History->Frame : 0u;
	const uint32 Rotation = (CellPixels & 1u) ? 2u : 1u;
	const uint32 Rank = (Frame + Rotation * (Frame / CellPixels)) % CellPixels;
	const FIntPoint Offset = CellPixel(Rank, MarchN);

	const FIntPoint MarchCells(
		FMath::DivideAndRoundUp(OutputSize.X, (int32)MarchN),
		FMath::DivideAndRoundUp(OutputSize.Y, (int32)MarchN));

	FRDGTextureRef MarchColor = CreateTarget(GraphBuilder, MarchCells, PF_FloatRGBA, TEXT("CloudAtmosphere.March"));
	FRDGTextureRef MarchTint = CreateTarget(GraphBuilder, MarchCells, PF_G16R16F, TEXT("CloudAtmosphere.MarchTint"));
	FRDGTextureRef MarchDepth = CreateTarget(GraphBuilder, MarchCells, PF_G32R32F, TEXT("CloudAtmosphere.MarchDepth"));

	{
		// This frame's pixel of each cell, its draws moved by the golden ratio per
		// sample the pixel has taken.
		const uint32 Samples = Frame / CellPixels;

		FAtmosphereMarchParameters* P = GraphBuilder.AllocParameters<FAtmosphereMarchParameters>();

		P->Field = March.Field;
		P->CloudScatter = March.CloudScatter;
		P->StormScatter = March.StormScatter;

		P->View = View.ViewUniformBuffer;
		P->AtmoSceneDepth = SceneDepth;
		P->RayViewToClip = RayViewToClip;
		P->LatticeGrowth = FMath::Max(March.LatticeGrowth, 1e-3f);
		P->LatticeGrowthFar = FMath::Max(March.LatticeGrowthFar, 1e-3f);

		// Formed here in double from this view's own camera.
		P->PlanetOffset = FVector3f(March.PlanetCenter - ViewOrigin);
		P->PlanetRotation = March.PlanetRotation;
		P->LightDirection = March.LightDirection;
		P->LightColor = March.LightColor;
		P->PlanetRadius = March.PlanetRadius;
		P->HeightScale = March.HeightScale;

		P->LightExtinctionFraction = March.LightExtinctionFraction;

		P->ForwardG = March.ForwardG;
		P->BackwardG = March.BackwardG;
		P->ForwardWeight = March.ForwardWeight;
		P->CloudAmbient = March.CloudAmbient;
		P->CloudAmbientFloor = March.CloudAmbientFloor;

		P->OctaveCount = March.OctaveCount;
		P->OctaveAttenuation = March.OctaveAttenuation;
		P->OctaveEccentricity = March.OctaveEccentricity;

		P->AmbientTerminator = March.AmbientTerminator;
		P->MieLobeDecay = March.MieLobeDecay;

		P->RayleighBeta = March.RayleighBeta;
		P->RayleighScaleHeight = March.RayleighScaleHeight;
		P->MieBeta = March.MieBeta;
		P->MieScaleHeight = March.MieScaleHeight;
		P->MieG = March.MieG;
		P->AbsorptionBeta = March.AbsorptionBeta;
		P->AbsorptionAltitude = March.AbsorptionAltitude;
		P->AbsorptionFalloff = March.AbsorptionFalloff;
		P->AtmosphereAmbient = March.AtmosphereAmbient;
		P->AtmosphereAmbientFloor = March.AtmosphereAmbientFloor;

		P->AtmosphereSteps = March.AtmosphereSteps;
		P->CloudSteps = March.CloudSteps;
		P->ChordSpread = March.ChordSpread;
		P->SurfaceShadow = March.SurfaceShadow;

		P->ShadowCamera1 = March.ShadowCamera1;
		P->ShadowCamera2 = March.ShadowCamera2;

		// Samplers as the bake binds them: the flow atlas carries its own
		// gutters, the volumes tile, and the shadow map and the table clamp.
		P->FlowTarget = March.FlowTexture;
		P->FlowTargetSampler = TStaticSamplerState<SF_Bilinear, AM_Wrap, AM_Clamp, AM_Clamp>::GetRHI();
		P->StructureVolume = March.StructureTexture.IsValid() ? March.StructureTexture : GBlackVolumeTexture->TextureRHI;
		P->StructureVolumeSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
		P->DetailVolume = March.DetailTexture.IsValid() ? March.DetailTexture : GBlackVolumeTexture->TextureRHI;
		P->DetailVolumeSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
		P->BlueNoiseTexture = March.BlueNoiseTexture;
		P->ShadowTarget = March.ShadowTexture;
		P->ShadowTargetSampler = BilinearClamp;
		P->TransmittanceTable = March.TransmittanceTexture;
		P->TransmittanceTableSampler = BilinearClamp;

		P->MarchCells = FUintVector2((uint32)MarchCells.X, (uint32)MarchCells.Y);
		P->MarchStride = MarchN;
		P->MarchOffset = FVector2f(Offset.X + 0.5f, Offset.Y + 0.5f);
		P->OutputSize = FVector2f(OutputSize.X, OutputSize.Y);
		P->JitterShift = (float)FMath::Frac(Samples * 0.6180339887498949);
		P->NoiseOffset = FUintVector2(
			(uint32)(FMath::Frac(0.5 + Rank * 0.7548776662) * 4096.0),
			(uint32)(FMath::Frac(0.5 + Rank * 0.5698402910) * 4096.0));

		P->MarchColor = GraphBuilder.CreateUAV(MarchColor);
		P->MarchTint = GraphBuilder.CreateUAV(MarchTint);
		P->MarchDepth = GraphBuilder.CreateUAV(MarchDepth);

		FAtmosphereMarchCS::FPermutationDomain Permutation;
		Permutation.Set<FAtmosphereMarchCS::FDeepDeck>(March.bGasGiant);

		TShaderMapRef<FAtmosphereMarchCS> Shader(ShaderMap, Permutation);

		FComputeShaderUtils::AddPass(GraphBuilder,
			RDG_EVENT_NAME("AtmosphereMarch %dx%d", MarchCells.X, MarchCells.Y),
			Shader, P, FComputeShaderUtils::GetGroupCount(MarchCells, ThreadGroupSize));
	}

	// -- Resolve --------------------------------------------------------------

	// The debug views draw into their own target so the history keeps the real
	// result; unwritten, a texel stands in with them off.
	const uint32 TemporalDebug = (uint32)FMath::Clamp(CVarAtmosphereTemporalDebug.GetValueOnRenderThread(), 0, 4);

	FRDGTextureRef Atmosphere = nullptr;
	FRDGTextureRef ResolvedColor = CreateTarget(GraphBuilder, OutputSize, PF_FloatRGBA, TEXT("CloudAtmosphere.History"));
	FRDGTextureRef ResolvedTint = CreateTarget(GraphBuilder, OutputSize, PF_G16R16F, TEXT("CloudAtmosphere.HistoryTint"));
	FRDGTextureRef ResolvedAge = CreateTarget(GraphBuilder, OutputSize, PF_G16R16F, TEXT("CloudAtmosphere.HistoryAge"));

	{
		FRDGTextureRef ResolvedDisplay = CreateTarget(GraphBuilder,
			TemporalDebug != 0u ? OutputSize : FIntPoint(1, 1), PF_FloatRGBA, TEXT("CloudAtmosphere.Debug"));

		Atmosphere = (TemporalDebug != 0u) ? ResolvedDisplay : ResolvedColor;

		FRDGTextureRef HistoryColor;
		FRDGTextureRef HistoryTint;
		FRDGTextureRef HistoryAge;

		if (bHistoryValid)
		{
			HistoryColor = GraphBuilder.RegisterExternalTexture(History->Color);
			HistoryTint = GraphBuilder.RegisterExternalTexture(History->Tint);
			HistoryAge = GraphBuilder.RegisterExternalTexture(History->Age);
		}
		else
		{
			// Bound but never read: HistoryValid is 0.
			HistoryColor = CreateTarget(GraphBuilder, FIntPoint(1, 1), PF_FloatRGBA, TEXT("CloudAtmosphere.NoHistory"));
			HistoryTint = CreateTarget(GraphBuilder, FIntPoint(1, 1), PF_G16R16F, TEXT("CloudAtmosphere.NoHistoryTint"));
			HistoryAge = CreateTarget(GraphBuilder, FIntPoint(1, 1), PF_G16R16F, TEXT("CloudAtmosphere.NoHistoryAge"));

			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(HistoryColor), 0.0f);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(HistoryTint), 0.0f);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(HistoryAge), 0.0f);
		}

		FAtmosphereResolveParameters* P = GraphBuilder.AllocParameters<FAtmosphereResolveParameters>();

		P->View = View.ViewUniformBuffer;
		P->AtmoSceneDepth = SceneDepth;
		P->RayViewToClip = HistoryViewToClip;
		P->OutputSize = FVector2f(OutputSize.X, OutputSize.Y);

		P->FreshColor = MarchColor;
		P->FreshTint = MarchTint;
		P->FreshDepth = MarchDepth;
		P->FreshSize = FUintVector2((uint32)MarchCells.X, (uint32)MarchCells.Y);
		P->CellSize = MarchN;
		P->CellOffset = FUintVector2((uint32)Offset.X, (uint32)Offset.Y);

		P->HistoryColor = HistoryColor;
		P->HistoryTint = HistoryTint;
		P->HistoryAge = HistoryAge;
		P->HistorySampler = BilinearClamp;
		P->HistoryValid = bHistoryValid ? 1u : 0u;
		P->PrevCameraToClip = bHistoryValid ? History->CameraToClip : CameraToClip;
		P->PrevPlanetToClip = bHistoryValid ? PlanetDelta * History->CameraToClip : CameraToClip;

		P->FreshWeight = FMath::Clamp(March.FreshWeight, 0.01f, 1.0f);
		P->TemporalDebug = TemporalDebug;

		P->ResolvedColor = GraphBuilder.CreateUAV(ResolvedColor);
		P->ResolvedTint = GraphBuilder.CreateUAV(ResolvedTint);
		P->ResolvedAge = GraphBuilder.CreateUAV(ResolvedAge);
		P->ResolvedDisplay = GraphBuilder.CreateUAV(ResolvedDisplay);

		TShaderMapRef<FAtmosphereResolveCS> Shader(ShaderMap);

		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("TemporalResolve %d", (int32)MarchN),
			Shader, P, FComputeShaderUtils::GetGroupCount(OutputSize, ThreadGroupSize));

		if (History)
		{
			GraphBuilder.QueueTextureExtraction(ResolvedColor, &History->Color);
			GraphBuilder.QueueTextureExtraction(ResolvedTint, &History->Tint);
			GraphBuilder.QueueTextureExtraction(ResolvedAge, &History->Age);

			History->bGasGiant = March.bGasGiant;
			History->CellSize = CellSize;
			History->CameraToClip = CameraToClip;
			History->ViewOrigin = ViewOrigin;
			History->PlanetCenter = March.PlanetCenter;
			History->PlanetRotation = PlanetRotation;
			History->Frame = Frame + 1;
			History->LastRendered = GFrameCounterRenderThread;
		}
	}

	// -- Composite ------------------------------------------------------------
	//
	// Into a new texture of the scene colour's size, written inside the view
	// rect only: the passes after this one read within it.

	FRDGTextureDesc OutputDesc = SceneColor.Texture->Desc;
	OutputDesc.Flags |= TexCreate_ShaderResource | TexCreate_UAV;

	FRDGTextureRef Output = GraphBuilder.CreateTexture(OutputDesc, TEXT("CloudAtmosphere.SceneColor"));

	{
		FAtmosphereCompositeParameters* P = GraphBuilder.AllocParameters<FAtmosphereCompositeParameters>();

		P->View = View.ViewUniformBuffer;
		P->OutputSize = FVector2f(OutputSize.X, OutputSize.Y);

		P->AtmosphereColor = Atmosphere;
		P->AtmosphereTint = ResolvedTint;

		// A debug view is opaque: its alpha alone, no tint.
		P->TintActive = (TemporalDebug != 0u) ? 0u : 1u;

		P->SceneColorTexture = SceneColor.Texture;
		P->SceneColorMin = FUintVector2((uint32)Rect.Min.X, (uint32)Rect.Min.Y);

		P->CompositeOutput = GraphBuilder.CreateUAV(Output);

		TShaderMapRef<FAtmosphereCompositeCS> Shader(ShaderMap);

		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Composite"),
			Shader, P, FComputeShaderUtils::GetGroupCount(OutputSize, ThreadGroupSize));
	}

	// Drop histories no view has rendered for a while.
	for (auto It = Histories.CreateIterator(); It; ++It)
	{
		if (It->Value->LastRendered + HistoryLifetime < GFrameCounterRenderThread)
		{
			It.RemoveCurrent();
		}
	}

	// BeforeDOF is never last in the chain, so OverrideOutput is not expected;
	// honoured when its format matches.
	if (Inputs.OverrideOutput.IsValid() && Inputs.OverrideOutput.Texture->Desc.Format == OutputDesc.Format)
	{
		FRHICopyTextureInfo Copy;
		Copy.SourcePosition = FIntVector(Rect.Min.X, Rect.Min.Y, 0);
		Copy.DestPosition = FIntVector(Inputs.OverrideOutput.ViewRect.Min.X, Inputs.OverrideOutput.ViewRect.Min.Y, 0);
		Copy.Size = FIntVector(OutputSize.X, OutputSize.Y, 1);

		AddCopyTexturePass(GraphBuilder, Output, Inputs.OverrideOutput.Texture, Copy);

		return Inputs.OverrideOutput;
	}

	return FScreenPassTexture(Output, Rect);
}
