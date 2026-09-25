#include "FlowSimulation.h"

#include "FlowSimShaders.h"
#include "FlowSnapshot.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderTargetPool.h"
#include "RHIGPUReadback.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"

// GBlackVolumeTexture, the fallback bound when no forcing volume is set.
#include "RenderUtils.h"

// TStaticSamplerState, for the wrapped trilinear forcing sampler.
#include "RHIStaticStates.h"

DEFINE_LOG_CATEGORY(LogFlowSim);

static_assert(UFlowSnapshot::FloatsPerCell == FFlowSimulation::StateFloatsPerCell,
	"Snapshot layout and solver state disagree about floats per cell.");

static_assert(UFlowSnapshot::TrailingFloats == FFlowSimulation::StateTrailingFloats
	&& FFlowSimulation::StateTrailingFloats == 8 * FlowSimShader::MaxStormCells,
	"Snapshot layout and solver state disagree about the storm cells.");

/** Everything registered into this frame's graph. Bundled so the pass helpers
 *  take one argument, and so the ping-pong swap is a single Swap(). */
struct FFlowSimResources
{
	FRDGTextureRef Face[2] = { nullptr, nullptr };
	FRDGTextureRef Centre = nullptr;
	FRDGTextureRef Explicit = nullptr;
	FRDGTextureRef Phi = nullptr;
	FRDGTextureRef PhiStar = nullptr;
	FRDGTextureRef Rhs = nullptr;
	FRDGTextureRef Spectrum[2] = { nullptr, nullptr };
	FRDGTextureRef Tracer[2] = { nullptr, nullptr };
	FRDGTextureRef Noise[2] = { nullptr, nullptr };
	FRDGTextureRef RowMean = nullptr;
	FRDGTextureRef MontgomeryEq = nullptr;
	FRDGTextureRef GlobalMean = nullptr;
	FRDGTextureRef LatLon = nullptr;
	FRDGTextureRef CentreLatest = nullptr;
	FRDGTextureRef LatLonLatest = nullptr;
	FRDGTextureRef Output = nullptr;
	FRDGTextureRef Debug = nullptr;
	FRDGBufferRef Cells = nullptr;

	int32 Current = 0;
	int32 TracerCurrent = 0;

	FRDGTextureRef Source() const { return Face[Current]; }
	FRDGTextureRef Dest() const { return Face[1 - Current]; }
	void Swap() { Current = 1 - Current; }

	FRDGTextureRef TracerSource() const { return Tracer[TracerCurrent]; }
	FRDGTextureRef TracerDest() const { return Tracer[1 - TracerCurrent]; }
	FRDGTextureRef NoiseSource() const { return Noise[TracerCurrent]; }
	FRDGTextureRef NoiseDest() const { return Noise[1 - TracerCurrent]; }
	void SwapTracer() { TracerCurrent = 1 - TracerCurrent; }
};

namespace
{
	using namespace FlowSimShader;

	FIntVector GroupCount2D(const FIntVector& GridSize)
	{
		return FIntVector(
			FMath::DivideAndRoundUp(GridSize.X, ThreadGroupSize2D),
			FMath::DivideAndRoundUp(GridSize.Y, ThreadGroupSize2D),
			GridSize.Z);
	}

	/** One group per row and layer: the row reduction folds each row in a
	 *  group. */
	FIntVector GroupCountRows(const FIntVector& GridSize)
	{
		return FIntVector(GridSize.Y, GridSize.Z, 1);
	}

	FIntVector GroupCountLayers(const FIntVector& GridSize)
	{
		return FIntVector(FMath::DivideAndRoundUp(GridSize.Z, ThreadGroupSizeLayers), 1, 1);
	}

	/** Fills every scalar parameter. Resources are attached per pass afterwards.
	 *  The one place a config value becomes a shader value. */
	void FillCommonParameters(FFlowSimParameters& P, const FFlowSimParams& Params)
	{
		P.SimGridSize = Params.GridSize;
		P.SimInvGridSize = FVector3f(
			1.0f / FMath::Max(Params.GridSize.X, 1),
			1.0f / FMath::Max(Params.GridSize.Y, 1),
			1.0f / FMath::Max(Params.GridSize.Z, 1));

		P.SimJetParams = Params.JetParams;
		P.SimWidthBias = Params.WidthBias;
		P.SimZonalProfile = Params.ZonalProfile;

		for (int32 i = 0; i < 8; ++i)
		{
			P.SimLayerProfile[i] = Params.LayerProfile[i];
			P.SimLayerState[i] = Params.LayerState[i];
		}

		for (int32 i = 0; i < 16; ++i)
		{
			P.SimMatMontgomery[i] = Params.Stack.Montgomery[i];
			P.SimMatMontgomeryInverse[i] = Params.Stack.MontgomeryInverse[i];
			P.SimMatModeToLayer[i] = Params.Stack.ModeToLayer[i];
			P.SimMatLayerToMode[i] = Params.Stack.LayerToMode[i];
			P.SimMatModeToMontgomery[i] = Params.Stack.ModeToMontgomery[i];
		}

		P.SimDeltaTime = Params.DeltaTime;
		P.SimTime = Params.Time;
		P.SimPlanetaryVorticity = Params.PlanetaryVorticity;
		P.SimWaveSpeedSq = Params.Stack.DesignSpeedSq;
		P.SimImplicitWeight = Params.ImplicitWeight;

		P.SimForcingChannel = Params.ForcingChannel;
		P.SimForcingBipolar = Params.bForcingBipolar ? 1 : 0;
		P.SimHasForcing = Params.ForcingTexture.IsValid() ? 1 : 0;

		P.SimNudgeRate = Params.NudgeRate;
		P.SimForcingAmplitude = Params.ForcingAmplitude;
		P.SimForcingScale = Params.ForcingScale;
		P.SimForcingLifetime = Params.ForcingLifetime;
		P.SimDragRate = Params.DragRate;
		P.SimLayerCoupling = Params.LayerCoupling;
		P.SimDivergenceDamping = Params.DivergenceDamping;
		P.SimSharpCentre = Params.bSharpCentreVelocity ? 1 : 0;
		P.SimThermalRelaxation = Params.ThermalRelaxation;
		P.SimThermalParams = Params.ThermalParams;

		P.SimCondensationRate = Params.CondensationRate;
		P.SimEvaporationRate = Params.EvaporationRate;
		P.SimCloudDecay = 1.0f / FMath::Max(Params.CloudLifetime, 1e-3f);
		P.SimMoistureParams = Params.MoistureParams;
		P.SimLatentHeating = Params.LatentHeating;
		P.SimStormParams = Params.StormParams;
		P.SimWindEvaporation = Params.WindEvaporation;

		P.SimCellShape = Params.CellShape;
		P.SimCellVortex = Params.CellVortex;
		P.SimCellDraft = Params.CellDraft;
		P.SimCellLife = Params.CellLife;
		P.SimCellMotion = Params.CellMotion;
		P.SimCellGenesis = Params.CellGenesis;
		P.SimCellCloud = Params.CellCloud;
		P.SimCellCount = FMath::Clamp(Params.CellCount, 0, FlowSimShader::MaxStormCells);
		P.SimStepIndex = Params.StepIndex;

		P.SimNoiseDriftRate = Params.NoiseDriftRate;
		P.SimNoiseResetTime = FMath::Max(Params.NoiseResetTime, 1e-3f);

		P.SimFilterLatitude = Params.FilterLatitude;
		P.SimFilterMaxHalfWidth = Params.FilterMaxHalfWidth;

		P.SimOutputScales = Params.OutputScales;
		P.SimAtlasFaceSize = Params.AtlasFaceSize;
		P.SimStateBlend = FMath::Clamp(Params.StateBlend, 0.0f, 1.0f);

		P.SimDebugMode = Params.DebugMode;
		P.SimDebugLayer = Params.DebugLayer;
		P.SimDebugScale = Params.DebugScale;
		P.SimDebugSize = Params.DebugSize;

		// A null forcing texture binds black and SimHasForcing gates it to zero.
		P.SimForcingNoise = Params.ForcingTexture.IsValid()
			? Params.ForcingTexture
			: GBlackVolumeTexture->TextureRHI;

		// Wrap on all three axes: the forcing volume is a tiling bake, and a
		// clamped read puts a band of constant value along each face.
		P.SimForcingNoiseSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
	}

	template <typename TShader>
	void AddSimPass(
		FRDGBuilder& GraphBuilder,
		const TCHAR* Name,
		FFlowSimParameters* Parameters,
		const FIntVector& Groups)
	{
		TShaderMapRef<TShader> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			FRDGEventName(TEXT("%s"), Name),
			Shader,
			Parameters,
			Groups);
	}

	FFlowSimParameters* NewParameters(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params)
	{
		FFlowSimParameters* P = GraphBuilder.AllocParameters<FFlowSimParameters>();
		FillCommonParameters(*P, Params);
		return P;
	}
}

void FFlowSimulation::RequestReset()
{
	bResetRequested = true;
}

void FFlowSimulation::QueueRestore_RenderThread(TArray<float>&& InData)
{
	check(IsInRenderingThread());

	PendingRestore = MoveTemp(InData);

	// Force the next Enqueue through initialisation so the upload happens.
	bResetRequested = true;
}

void FFlowSimulation::Release_RenderThread()
{
	PooledFace[0].SafeRelease();
	PooledFace[1].SafeRelease();
	PooledCentre.SafeRelease();
	PooledExplicit.SafeRelease();
	PooledPhi.SafeRelease();
	PooledPhiStar.SafeRelease();
	PooledRhs.SafeRelease();
	PooledSpectrum[0].SafeRelease();
	PooledSpectrum[1].SafeRelease();
	PooledTracer[0].SafeRelease();
	PooledTracer[1].SafeRelease();
	PooledNoise[0].SafeRelease();
	PooledNoise[1].SafeRelease();
	PooledLatLon.SafeRelease();
	PooledCentreLatest.SafeRelease();
	PooledLatLonLatest.SafeRelease();
	PooledRowMean.SafeRelease();
	PooledMontgomeryEq.SafeRelease();
	PooledGlobalMean.SafeRelease();
	PooledCells.SafeRelease();

	// PendingRestore is DELIBERATELY NOT cleared. A queued restore sets the reset
	// flag, and EnsureResources answers that flag by calling this function --
	// clearing the payload here would destroy it with the very reset that was
	// meant to apply it. The initialisation branch in Enqueue owns it.

	AllocatedGrid = FIntVector::ZeroValue;
	CurrentFace = 0;
	CurrentTracer = 0;
	bInitialised = false;
	bResetRequested = false;
}

bool FFlowSimulation::EnsureResources(const FFlowSimParams& Params)
{
	const bool bGridChanged = (AllocatedGrid != Params.GridSize);

	if (!bGridChanged && !bResetRequested && PooledPhi.IsValid())
	{
		return false;
	}

	Release_RenderThread();

	const FIntPoint Size(Params.GridSize.X, Params.GridSize.Y);
	const uint16 Slices = (uint16)FMath::Max(Params.GridSize.Z, 1);
	const ETextureCreateFlags Flags = TexCreate_ShaderResource | TexCreate_UAV;

	// 32-bit throughout. The Helmholtz operator is a small difference of larger
	// numbers and the thickness anomaly rides on the layer depth; half
	// precision loses both. The output texture the material reads is 16-bit,
	// which is fine once the differencing is done.
	const FRDGTextureDesc FaceDesc = FRDGTextureDesc::Create2DArray(
		Size, PF_G32R32F, FClearValueBinding::Black, Flags, Slices);

	const FRDGTextureDesc CentreDesc = FRDGTextureDesc::Create2DArray(
		Size, PF_A32B32G32R32F, FClearValueBinding::Black, Flags, Slices);

	const FRDGTextureDesc ScalarDesc = FRDGTextureDesc::Create2DArray(
		Size, PF_R32_FLOAT, FClearValueBinding::Black, Flags, Slices);

	PooledFace[0] = AllocatePooledTexture(FaceDesc, TEXT("FlowSim.FaceA"));
	PooledFace[1] = AllocatePooledTexture(FaceDesc, TEXT("FlowSim.FaceB"));
	PooledCentre = AllocatePooledTexture(CentreDesc, TEXT("FlowSim.Centre"));
	PooledExplicit = AllocatePooledTexture(CentreDesc, TEXT("FlowSim.Explicit"));
	PooledPhi = AllocatePooledTexture(ScalarDesc, TEXT("FlowSim.Phi"));
	PooledPhiStar = AllocatePooledTexture(ScalarDesc, TEXT("FlowSim.PhiStar"));
	PooledRhs = AllocatePooledTexture(ScalarDesc, TEXT("FlowSim.Rhs"));

	// Complex row spectra, (wavenumber, row, mode).
	const FRDGTextureDesc SpectrumDesc = FRDGTextureDesc::Create2DArray(
		Size, PF_G32R32F, FClearValueBinding::Black, Flags, Slices);

	PooledSpectrum[0] = AllocatePooledTexture(SpectrumDesc, TEXT("FlowSim.SpectrumA"));
	PooledSpectrum[1] = AllocatePooledTexture(SpectrumDesc, TEXT("FlowSim.SpectrumB"));

	// Cloud, cloud times formation ascent, vapour, storm.
	PooledTracer[0] = AllocatePooledTexture(CentreDesc, TEXT("FlowSim.TracerA"));
	PooledTracer[1] = AllocatePooledTexture(CentreDesc, TEXT("FlowSim.TracerB"));

	// Noise displacement, xyz; phase A in slices [0, L), phase B in [L, 2L).
	const FRDGTextureDesc NoiseDesc = FRDGTextureDesc::Create2DArray(
		Size, PF_A32B32G32R32F, FClearValueBinding::Black, Flags, (uint16)(2 * Slices));

	PooledNoise[0] = AllocatePooledTexture(NoiseDesc, TEXT("FlowSim.NoiseA"));
	PooledNoise[1] = AllocatePooledTexture(NoiseDesc, TEXT("FlowSim.NoiseB"));

	// The output on the grid: flow, weather and both noise phases per layer.
	// 32-bit, so the atlas is the one place the output is quantised.
	const FRDGTextureDesc LatLonDesc = FRDGTextureDesc::Create2DArray(
		Size, PF_A32B32G32R32F, FClearValueBinding::Black, Flags, (uint16)(4 * Slices));

	PooledLatLon = AllocatePooledTexture(LatLonDesc, TEXT("FlowSim.LatLon"));
	PooledLatLonLatest = AllocatePooledTexture(LatLonDesc, TEXT("FlowSim.LatLonLatest"));
	PooledCentreLatest = AllocatePooledTexture(CentreDesc, TEXT("FlowSim.CentreLatest"));

	// (row, layer).
	const FIntPoint RowSize(Params.GridSize.Y, Slices);

	PooledRowMean = AllocatePooledTexture(
		FRDGTextureDesc::Create2D(RowSize, PF_G32R32F, FClearValueBinding::Black, Flags),
		TEXT("FlowSim.RowMean"));

	PooledMontgomeryEq = AllocatePooledTexture(
		FRDGTextureDesc::Create2D(RowSize, PF_R32_FLOAT, FClearValueBinding::Black, Flags),
		TEXT("FlowSim.MontgomeryEq"));

	PooledGlobalMean = AllocatePooledTexture(
		FRDGTextureDesc::Create2D(FIntPoint(1, Slices), PF_R32_FLOAT, FClearValueBinding::Black, Flags),
		TEXT("FlowSim.GlobalMean"));

	PooledCells = AllocatePooledBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), 2 * FlowSimShader::MaxStormCells),
		TEXT("FlowSim.Cells"));

	AllocatedGrid = Params.GridSize;
	CurrentFace = 0;
	CurrentTracer = 0;
	bInitialised = false;
	bResetRequested = false;

	UE_LOG(LogFlowSim, Log, TEXT("Allocated sim state at %dx%d x %d layers."),
		Params.GridSize.X, Params.GridSize.Y, Params.GridSize.Z);

	return true;
}

void FFlowSimulation::AddBalancePass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R)
{
	FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
	P->SimMontgomeryEqUAV = GraphBuilder.CreateUAV(R.MontgomeryEq);

	AddSimPass<FFlowSimInitBalanceCS>(GraphBuilder, TEXT("FlowSim.Balance"), P, GroupCountLayers(Params.GridSize));
}

void FFlowSimulation::AddInitPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R)
{
	FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
	P->SimMontgomeryEqSRV = GraphBuilder.CreateSRV(R.MontgomeryEq);
	P->SimFaceUAV = GraphBuilder.CreateUAV(R.Source());
	P->SimPhiUAV = GraphBuilder.CreateUAV(R.Phi);
	P->SimTracerUAV = GraphBuilder.CreateUAV(R.TracerSource());
	P->SimNoiseUAV = GraphBuilder.CreateUAV(R.NoiseSource());
	P->SimCellUAV = GraphBuilder.CreateUAV(R.Cells);

	AddSimPass<FFlowSimInitStateCS>(GraphBuilder, TEXT("FlowSim.InitState"), P, GroupCount2D(Params.GridSize));
}

void FFlowSimulation::AddRestorePass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R)
{
	FRDGBufferRef Upload = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("FlowSim.RestoreUpload"),
		sizeof(float),
		StateFloats(Params.GridSize),
		PendingRestore.GetData(),
		PendingRestore.Num() * sizeof(float));

	FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
	P->SimRestoreBuffer = GraphBuilder.CreateSRV(Upload);
	P->SimFaceUAV = GraphBuilder.CreateUAV(R.Source());
	P->SimPhiUAV = GraphBuilder.CreateUAV(R.Phi);
	P->SimTracerUAV = GraphBuilder.CreateUAV(R.TracerSource());
	P->SimNoiseUAV = GraphBuilder.CreateUAV(R.NoiseSource());
	P->SimCellUAV = GraphBuilder.CreateUAV(R.Cells);

	AddSimPass<FFlowSimRestoreCS>(GraphBuilder, TEXT("FlowSim.Restore"), P, GroupCount2D(Params.GridSize));
}

void FFlowSimulation::AddCapturePass_RenderThread(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, FRHIGPUBufferReadback* Readback)
{
	check(IsInRenderingThread());

	if (!Readback || !PooledPhi.IsValid())
	{
		return;
	}

	const int32 Total = AllocatedGrid.X * AllocatedGrid.Y * AllocatedGrid.Z;

	if (Total <= 0)
	{
		return;
	}

	FRDGTextureRef Face = GraphBuilder.RegisterExternalTexture(PooledFace[CurrentFace]);
	FRDGTextureRef Phi = GraphBuilder.RegisterExternalTexture(PooledPhi);
	FRDGTextureRef Tracer = GraphBuilder.RegisterExternalTexture(PooledTracer[CurrentTracer]);
	FRDGTextureRef Noise = GraphBuilder.RegisterExternalTexture(PooledNoise[CurrentTracer]);
	FRDGBufferRef Cells = GraphBuilder.RegisterExternalBuffer(PooledCells);

	const int32 Floats = StateFloats(AllocatedGrid);

	FRDGBufferRef Capture = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Floats),
		TEXT("FlowSim.Capture"));

	FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
	P->SimFaceSRV = GraphBuilder.CreateSRV(Face);
	P->SimPhiSRV = GraphBuilder.CreateSRV(Phi);
	P->SimTracerSRV = GraphBuilder.CreateSRV(Tracer);
	P->SimNoiseSRV = GraphBuilder.CreateSRV(Noise);
	P->SimCellSRV = GraphBuilder.CreateSRV(Cells);
	P->SimCaptureBuffer = GraphBuilder.CreateUAV(Capture);

	AddSimPass<FFlowSimCaptureCS>(GraphBuilder, TEXT("FlowSim.Capture"), P, GroupCount2D(AllocatedGrid));

	AddEnqueueCopyPass(GraphBuilder, Readback, Capture, Floats * sizeof(float));
}

void FFlowSimulation::AddReducePasses(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R)
{
	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
		P->SimFaceSRV = GraphBuilder.CreateSRV(R.Source());
		P->SimPhiSRV = GraphBuilder.CreateSRV(R.Phi);
		P->SimRowMeanUAV = GraphBuilder.CreateUAV(R.RowMean);

		AddSimPass<FFlowSimReduceRowsCS>(GraphBuilder, TEXT("FlowSim.ReduceRows"), P, GroupCountRows(Params.GridSize));
	}

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
		P->SimRowMeanSRV = GraphBuilder.CreateSRV(R.RowMean);
		P->SimGlobalMeanUAV = GraphBuilder.CreateUAV(R.GlobalMean);

		// One group per layer.
		AddSimPass<FFlowSimReduceGlobalCS>(GraphBuilder, TEXT("FlowSim.ReduceGlobal"), P,
			FIntVector(Params.GridSize.Z, 1, 1));
	}
}

void FFlowSimulation::AddReconstructPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R, bool bLatest)
{
	FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
	P->SimFaceSRV = GraphBuilder.CreateSRV(R.Source());
	P->SimPhiSRV = GraphBuilder.CreateSRV(R.Phi);
	P->SimRowMeanSRV = GraphBuilder.CreateSRV(R.RowMean);
	P->SimTracerSRV = GraphBuilder.CreateSRV(R.TracerSource());
	P->SimNoiseSRV = GraphBuilder.CreateSRV(R.NoiseSource());
	P->SimCentreUAV = GraphBuilder.CreateUAV(bLatest ? R.CentreLatest : R.Centre);
	P->SimExplicitUAV = GraphBuilder.CreateUAV(R.Explicit);
	P->SimLatLonUAV = GraphBuilder.CreateUAV(bLatest ? R.LatLonLatest : R.LatLon);

	AddSimPass<FFlowSimReconstructCS>(GraphBuilder, TEXT("FlowSim.Reconstruct"), P, GroupCount2D(Params.GridSize));
}

void FFlowSimulation::AddCellsPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R)
{
	FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
	P->SimCentreSRV = GraphBuilder.CreateSRV(R.Centre);
	P->SimLatLonSRV = GraphBuilder.CreateSRV(R.LatLon);
	P->SimCellUAV = GraphBuilder.CreateUAV(R.Cells);

	AddSimPass<FFlowSimCellsCS>(GraphBuilder, TEXT("FlowSim.Cells"), P, FIntVector(1, 1, 1));
}

void FFlowSimulation::AddSubstep(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, FFlowSimResources& R)
{
	const FIntVector Groups2D = GroupCount2D(Params.GridSize);

	// -- 1. Means and centre fields of the current state ------------------

	AddReducePasses(GraphBuilder, Params, R);
	AddReconstructPass(GraphBuilder, Params, R, false);
	AddCellsPass(GraphBuilder, Params, R);

	// -- 2. Predict: advection and every explicit term --------------------

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
		P->SimFaceSRV = GraphBuilder.CreateSRV(R.Source());
		P->SimCentreSRV = GraphBuilder.CreateSRV(R.Centre);
		P->SimExplicitSRV = GraphBuilder.CreateSRV(R.Explicit);
		P->SimPhiSRV = GraphBuilder.CreateSRV(R.Phi);
		P->SimRowMeanSRV = GraphBuilder.CreateSRV(R.RowMean);
		P->SimMontgomeryEqSRV = GraphBuilder.CreateSRV(R.MontgomeryEq);
		P->SimGlobalMeanSRV = GraphBuilder.CreateSRV(R.GlobalMean);
		P->SimLatLonSRV = GraphBuilder.CreateSRV(R.LatLon);
		P->SimTracerSRV = GraphBuilder.CreateSRV(R.TracerSource());
		P->SimNoiseSRV = GraphBuilder.CreateSRV(R.NoiseSource());
		P->SimCellSRV = GraphBuilder.CreateSRV(R.Cells);
		P->SimFaceUAV = GraphBuilder.CreateUAV(R.Dest());
		P->SimPhiStarUAV = GraphBuilder.CreateUAV(R.PhiStar);
		P->SimTracerUAV = GraphBuilder.CreateUAV(R.TracerDest());
		P->SimNoiseUAV = GraphBuilder.CreateUAV(R.NoiseDest());

		AddSimPass<FFlowSimPredictCS>(GraphBuilder, TEXT("FlowSim.Predict"), P, Groups2D);
	}
	R.Swap();
	R.SwapTracer();

	// -- 3. Polar filter; filtered phi* lands in Rhs -----------------------

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
		P->SimFaceSRV = GraphBuilder.CreateSRV(R.Source());
		P->SimPhiStarSRV = GraphBuilder.CreateSRV(R.PhiStar);
		P->SimFaceUAV = GraphBuilder.CreateUAV(R.Dest());
		P->SimRhsUAV = GraphBuilder.CreateUAV(R.Rhs);

		AddSimPass<FFlowSimFilterCS>(GraphBuilder, TEXT("FlowSim.Filter"), P, Groups2D);
	}
	R.Swap();

	// -- 4. Right-hand side, projected onto the vertical modes -------------

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
		P->SimFaceSRV = GraphBuilder.CreateSRV(R.Source());
		P->SimRhsUAV = GraphBuilder.CreateUAV(R.Rhs);

		// One thread per column: every mode reads every layer.
		AddSimPass<FFlowSimRhsCS>(GraphBuilder, TEXT("FlowSim.Rhs"), P,
			FIntVector(Groups2D.X, Groups2D.Y, 1));
	}

	// -- 5. Helmholtz per mode: row FFT, latitude solve, inverse FFT -------

	const FIntVector GroupsRows(Params.GridSize.Y, Params.GridSize.Z, 1);
	const FIntVector GroupsWavenumbers(Params.GridSize.X, Params.GridSize.Z, 1);

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
		P->SimRhsSRV = GraphBuilder.CreateSRV(R.Rhs);
		P->SimSpectrumUAV = GraphBuilder.CreateUAV(R.Spectrum[0]);

		AddSimPass<FFlowSimHelmholtzForwardCS>(GraphBuilder, TEXT("FlowSim.HelmholtzForward"), P, GroupsRows);
	}

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
		P->SimSpectrumSRV = GraphBuilder.CreateSRV(R.Spectrum[0]);
		P->SimSpectrumUAV = GraphBuilder.CreateUAV(R.Spectrum[1]);

		AddSimPass<FFlowSimHelmholtzColumnCS>(GraphBuilder, TEXT("FlowSim.HelmholtzColumn"), P, GroupsWavenumbers);
	}

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
		P->SimSpectrumSRV = GraphBuilder.CreateSRV(R.Spectrum[1]);
		P->SimPhiStarUAV = GraphBuilder.CreateUAV(R.PhiStar);

		AddSimPass<FFlowSimHelmholtzInverseCS>(GraphBuilder, TEXT("FlowSim.HelmholtzInverse"), P, GroupsRows);
	}

	// -- 6. Correct: the implicit pressure gradient, and the layers -----------

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
		P->SimFaceSRV = GraphBuilder.CreateSRV(R.Source());
		P->SimPhiStarSRV = GraphBuilder.CreateSRV(R.PhiStar);
		P->SimFaceUAV = GraphBuilder.CreateUAV(R.Dest());
		P->SimPhiUAV = GraphBuilder.CreateUAV(R.Phi);

		AddSimPass<FFlowSimCorrectCS>(GraphBuilder, TEXT("FlowSim.Correct"), P, Groups2D);
	}
	R.Swap();
}

void FFlowSimulation::AddResamplePass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R)
{
	const FIntPoint Atlas = AtlasSize(Params.AtlasFaceSize);

	const FIntVector Groups(
		FMath::DivideAndRoundUp(Atlas.X, ThreadGroupSize2D),
		FMath::DivideAndRoundUp(Atlas.Y, ThreadGroupSize2D),
		Params.GridSize.Z);

	FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
	P->SimCentreSRV = GraphBuilder.CreateSRV(R.Centre);
	P->SimLatLonSRV = GraphBuilder.CreateSRV(R.LatLon);
	P->SimCentreLatestSRV = GraphBuilder.CreateSRV(R.CentreLatest);
	P->SimLatLonLatestSRV = GraphBuilder.CreateSRV(R.LatLonLatest);
	P->SimCellSRV = GraphBuilder.CreateSRV(R.Cells);
	P->SimOutputUAV = GraphBuilder.CreateUAV(R.Output);

	AddSimPass<FFlowSimResampleCS>(GraphBuilder, TEXT("FlowSim.Resample"), P, Groups);
}

void FFlowSimulation::AddDebugPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R)
{
	if (!R.Debug)
	{
		return;
	}

	const FIntVector Groups(
		FMath::DivideAndRoundUp(Params.DebugSize.X, ThreadGroupSize2D),
		FMath::DivideAndRoundUp(Params.DebugSize.Y, ThreadGroupSize2D),
		1);

	FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
	P->SimFaceSRV = GraphBuilder.CreateSRV(R.Source());
	P->SimPhiSRV = GraphBuilder.CreateSRV(R.Phi);
	P->SimPhiStarSRV = GraphBuilder.CreateSRV(R.PhiStar);
	P->SimRhsSRV = GraphBuilder.CreateSRV(R.Rhs);
	P->SimRowMeanSRV = GraphBuilder.CreateSRV(R.RowMean);
	P->SimTracerSRV = GraphBuilder.CreateSRV(R.TracerSource());
	P->SimNoiseSRV = GraphBuilder.CreateSRV(R.NoiseSource());

	// The frame's last Reconstruct wrote the latest state's output.
	P->SimLatLonSRV = GraphBuilder.CreateSRV(R.LatLonLatest);
	P->SimCellSRV = GraphBuilder.CreateSRV(R.Cells);
	P->SimDebugUAV = GraphBuilder.CreateUAV(R.Debug);

	AddSimPass<FFlowSimDebugVisCS>(GraphBuilder, TEXT("FlowSim.DebugVis"), P, Groups);
}

void FFlowSimulation::Enqueue_RenderThread(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, int32 NumSubsteps)
{
	check(IsInRenderingThread());

	if (!Params.FlowTexture.IsValid())
	{
		return;
	}

	const bool bNeedsSeeding = EnsureResources(Params);

	RDG_EVENT_SCOPE(GraphBuilder, "FlowSim");

	FFlowSimResources R;
	R.Face[0] = GraphBuilder.RegisterExternalTexture(PooledFace[0]);
	R.Face[1] = GraphBuilder.RegisterExternalTexture(PooledFace[1]);
	R.Centre = GraphBuilder.RegisterExternalTexture(PooledCentre);
	R.Explicit = GraphBuilder.RegisterExternalTexture(PooledExplicit);
	R.Phi = GraphBuilder.RegisterExternalTexture(PooledPhi);
	R.PhiStar = GraphBuilder.RegisterExternalTexture(PooledPhiStar);
	R.Rhs = GraphBuilder.RegisterExternalTexture(PooledRhs);
	R.Spectrum[0] = GraphBuilder.RegisterExternalTexture(PooledSpectrum[0]);
	R.Spectrum[1] = GraphBuilder.RegisterExternalTexture(PooledSpectrum[1]);
	R.Tracer[0] = GraphBuilder.RegisterExternalTexture(PooledTracer[0]);
	R.Tracer[1] = GraphBuilder.RegisterExternalTexture(PooledTracer[1]);
	R.Noise[0] = GraphBuilder.RegisterExternalTexture(PooledNoise[0]);
	R.Noise[1] = GraphBuilder.RegisterExternalTexture(PooledNoise[1]);
	R.RowMean = GraphBuilder.RegisterExternalTexture(PooledRowMean);
	R.MontgomeryEq = GraphBuilder.RegisterExternalTexture(PooledMontgomeryEq);
	R.GlobalMean = GraphBuilder.RegisterExternalTexture(PooledGlobalMean);
	R.LatLon = GraphBuilder.RegisterExternalTexture(PooledLatLon);
	R.CentreLatest = GraphBuilder.RegisterExternalTexture(PooledCentreLatest);
	R.LatLonLatest = GraphBuilder.RegisterExternalTexture(PooledLatLonLatest);
	R.Cells = GraphBuilder.RegisterExternalBuffer(PooledCells);
	R.Current = CurrentFace;
	R.TracerCurrent = CurrentTracer;

	R.Output = GraphBuilder.RegisterExternalTexture(
		CreateRenderTarget(Params.FlowTexture, TEXT("FlowSim.Flow")));

	if (Params.DebugTexture.IsValid() && Params.DebugSize.X > 0 && Params.DebugSize.Y > 0)
	{
		R.Debug = GraphBuilder.RegisterExternalTexture(
			CreateRenderTarget(Params.DebugTexture, TEXT("FlowSim.Debug")));
	}

	// Every frame: the thermal relaxation target and the initial state both
	// read it, and a live profile edit should reach both.
	AddBalancePass(GraphBuilder, Params, R);

	if (bNeedsSeeding || !bInitialised)
	{
		const int32 Expected = StateFloats(Params.GridSize);

		if (PendingRestore.Num() == Expected)
		{
			AddRestorePass(GraphBuilder, Params, R);

			UE_LOG(LogFlowSim, Log, TEXT("Restored state from snapshot."));
		}
		else
		{
			if (PendingRestore.Num() > 0)
			{
				UE_LOG(LogFlowSim, Warning,
					TEXT("Snapshot has %d floats, grid needs %d. Seeding instead."),
					PendingRestore.Num(), Expected);
			}

			// Balanced by construction, so there is no cold solve.
			AddInitPass(GraphBuilder, Params, R);
		}

		PendingRestore.Empty();
		bInitialised = true;

		// The start state is also the previous state, until a step replaces it.
		AddReducePasses(GraphBuilder, Params, R);
		AddReconstructPass(GraphBuilder, Params, R, false);
	}

	// Each substep at its own time: the forcing's phases and the noise resets
	// are clocked by it.
	for (int32 Step = 0; Step < NumSubsteps; ++Step)
	{
		FFlowSimParams StepParams = Params;
		StepParams.Time = Params.Time + (float)Step * Params.DeltaTime;
		StepParams.StepIndex = Params.StepIndex + Step;

		AddSubstep(GraphBuilder, StepParams, R);
	}

	// THE OUTPUT BLENDS THE LAST TWO STATES. Each substep's reconstruct leaves
	// the state it started from in Centre and LatLon, so after the loop they
	// hold the previous state; this one writes the latest beside them. A frame
	// without substeps leaves the previous state as it was.
	AddReducePasses(GraphBuilder, Params, R);
	AddReconstructPass(GraphBuilder, Params, R, true);
	AddResamplePass(GraphBuilder, Params, R);

	AddDebugPass(GraphBuilder, Params, R);

	// Carry both ping-pong indices across the frame boundary. The faces swap
	// three times per substep and the tracers once, so both genuinely alternate.
	CurrentFace = R.Current;
	CurrentTracer = R.TracerCurrent;
}