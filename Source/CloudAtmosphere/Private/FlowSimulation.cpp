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
	FRDGTextureRef Cloud[2] = { nullptr, nullptr };
	FRDGTextureRef RowMean = nullptr;
	FRDGTextureRef PhiEq = nullptr;
	FRDGTextureRef GlobalMean = nullptr;
	FRDGTextureRef Output = nullptr;
	FRDGTextureRef Debug = nullptr;

	int32 Current = 0;
	int32 CloudCurrent = 0;

	FRDGTextureRef Source() const { return Face[Current]; }
	FRDGTextureRef Dest() const { return Face[1 - Current]; }
	void Swap() { Current = 1 - Current; }

	FRDGTextureRef CloudSource() const { return Cloud[CloudCurrent]; }
	FRDGTextureRef CloudDest() const { return Cloud[1 - CloudCurrent]; }
	void SwapCloud() { CloudCurrent = 1 - CloudCurrent; }
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

	FIntVector GroupCountRows(const FIntVector& GridSize)
	{
		return FIntVector(FMath::DivideAndRoundUp(GridSize.Y, ThreadGroupSize1D), GridSize.Z, 1);
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
		}

		P.SimDeltaTime = Params.DeltaTime;
		P.SimTime = Params.Time;
		P.SimPlanetaryVorticity = Params.PlanetaryVorticity;
		P.SimWaveSpeedSq = Params.WaveSpeedSq;
		P.SimImplicitWeight = Params.ImplicitWeight;
		P.SimHelmholtzScale = Params.HelmholtzScale;

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
		P.SimThermalRelaxation = Params.ThermalRelaxation;

		P.SimCondensationRate = Params.CondensationRate;
		P.SimEvaporationRate = Params.EvaporationRate;
		P.SimCloudDecay = 1.0f / FMath::Max(Params.CloudLifetime, 1e-3f);

		P.SimFilterLatitude = Params.FilterLatitude;
		P.SimFilterMaxHalfWidth = Params.FilterMaxHalfWidth;

		P.SimOutputScales = Params.OutputScales;

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
	PooledCloud[0].SafeRelease();
	PooledCloud[1].SafeRelease();
	PooledRowMean.SafeRelease();
	PooledPhiEq.SafeRelease();
	PooledGlobalMean.SafeRelease();

	// PendingRestore is DELIBERATELY NOT cleared. A queued restore sets the reset
	// flag, and EnsureResources answers that flag by calling this function --
	// clearing the payload here would destroy it with the very reset that was
	// meant to apply it. The initialisation branch in Enqueue owns it.

	AllocatedGrid = FIntVector::ZeroValue;
	CurrentFace = 0;
	CurrentCloud = 0;
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
	// numbers and the geopotential anomaly rides on c^2; half precision loses
	// both. The output texture the material reads is 16-bit, which is fine once
	// the differencing is done.
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

	// Complex row spectra, (wavenumber, row, layer).
	const FRDGTextureDesc SpectrumDesc = FRDGTextureDesc::Create2DArray(
		Size, PF_G32R32F, FClearValueBinding::Black, Flags, Slices);

	PooledSpectrum[0] = AllocatePooledTexture(SpectrumDesc, TEXT("FlowSim.SpectrumA"));
	PooledSpectrum[1] = AllocatePooledTexture(SpectrumDesc, TEXT("FlowSim.SpectrumB"));

	PooledCloud[0] = AllocatePooledTexture(ScalarDesc, TEXT("FlowSim.CloudA"));
	PooledCloud[1] = AllocatePooledTexture(ScalarDesc, TEXT("FlowSim.CloudB"));

	// (row, layer).
	const FIntPoint RowSize(Params.GridSize.Y, Slices);

	PooledRowMean = AllocatePooledTexture(
		FRDGTextureDesc::Create2D(RowSize, PF_G32R32F, FClearValueBinding::Black, Flags),
		TEXT("FlowSim.RowMean"));

	PooledPhiEq = AllocatePooledTexture(
		FRDGTextureDesc::Create2D(RowSize, PF_R32_FLOAT, FClearValueBinding::Black, Flags),
		TEXT("FlowSim.PhiEq"));

	PooledGlobalMean = AllocatePooledTexture(
		FRDGTextureDesc::Create2D(FIntPoint(1, Slices), PF_R32_FLOAT, FClearValueBinding::Black, Flags),
		TEXT("FlowSim.GlobalMean"));

	AllocatedGrid = Params.GridSize;
	CurrentFace = 0;
	CurrentCloud = 0;
	bInitialised = false;
	bResetRequested = false;

	UE_LOG(LogFlowSim, Log, TEXT("Allocated sim state at %dx%d x %d layers."),
		Params.GridSize.X, Params.GridSize.Y, Params.GridSize.Z);

	return true;
}

void FFlowSimulation::AddBalancePass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R)
{
	FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
	P->SimPhiEqUAV = GraphBuilder.CreateUAV(R.PhiEq);

	AddSimPass<FFlowSimInitBalanceCS>(GraphBuilder, TEXT("FlowSim.Balance"), P, GroupCountLayers(Params.GridSize));
}

void FFlowSimulation::AddInitPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R)
{
	FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
	P->SimPhiEqSRV = GraphBuilder.CreateSRV(R.PhiEq);
	P->SimFaceUAV = GraphBuilder.CreateUAV(R.Source());
	P->SimPhiUAV = GraphBuilder.CreateUAV(R.Phi);
	P->SimCloudUAV = GraphBuilder.CreateUAV(R.CloudSource());

	AddSimPass<FFlowSimInitStateCS>(GraphBuilder, TEXT("FlowSim.InitState"), P, GroupCount2D(Params.GridSize));
}

void FFlowSimulation::AddRestorePass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R)
{
	const int32 Total = Params.GridSize.X * Params.GridSize.Y * Params.GridSize.Z;

	FRDGBufferRef Upload = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("FlowSim.RestoreUpload"),
		sizeof(float),
		Total * StateFloatsPerCell,
		PendingRestore.GetData(),
		PendingRestore.Num() * sizeof(float));

	FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
	P->SimRestoreBuffer = GraphBuilder.CreateSRV(Upload);
	P->SimFaceUAV = GraphBuilder.CreateUAV(R.Source());
	P->SimPhiUAV = GraphBuilder.CreateUAV(R.Phi);
	P->SimCloudUAV = GraphBuilder.CreateUAV(R.CloudSource());

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

	FRDGBufferRef Capture = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Total * StateFloatsPerCell),
		TEXT("FlowSim.Capture"));

	FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
	P->SimFaceSRV = GraphBuilder.CreateSRV(Face);
	P->SimPhiSRV = GraphBuilder.CreateSRV(Phi);
	P->SimCaptureBuffer = GraphBuilder.CreateUAV(Capture);

	AddSimPass<FFlowSimCaptureCS>(GraphBuilder, TEXT("FlowSim.Capture"), P, GroupCount2D(AllocatedGrid));

	AddEnqueueCopyPass(GraphBuilder, Readback, Capture, Total * StateFloatsPerCell * sizeof(float));
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

		AddSimPass<FFlowSimReduceGlobalCS>(GraphBuilder, TEXT("FlowSim.ReduceGlobal"), P, GroupCountLayers(Params.GridSize));
	}
}

void FFlowSimulation::AddReconstructPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R)
{
	FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
	P->SimFaceSRV = GraphBuilder.CreateSRV(R.Source());
	P->SimPhiSRV = GraphBuilder.CreateSRV(R.Phi);
	P->SimRowMeanSRV = GraphBuilder.CreateSRV(R.RowMean);
	P->SimCloudSRV = GraphBuilder.CreateSRV(R.CloudSource());
	P->SimCentreUAV = GraphBuilder.CreateUAV(R.Centre);
	P->SimExplicitUAV = GraphBuilder.CreateUAV(R.Explicit);
	P->SimOutputUAV = GraphBuilder.CreateUAV(R.Output);

	AddSimPass<FFlowSimReconstructCS>(GraphBuilder, TEXT("FlowSim.Reconstruct"), P, GroupCount2D(Params.GridSize));
}

void FFlowSimulation::AddSubstep(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, FFlowSimResources& R)
{
	const FIntVector Groups2D = GroupCount2D(Params.GridSize);

	// -- 1. Means and centre fields of the current state ------------------

	AddReducePasses(GraphBuilder, Params, R);
	AddReconstructPass(GraphBuilder, Params, R);

	// -- 2. Predict: advection and every explicit term --------------------

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
		P->SimFaceSRV = GraphBuilder.CreateSRV(R.Source());
		P->SimCentreSRV = GraphBuilder.CreateSRV(R.Centre);
		P->SimExplicitSRV = GraphBuilder.CreateSRV(R.Explicit);
		P->SimPhiSRV = GraphBuilder.CreateSRV(R.Phi);
		P->SimRowMeanSRV = GraphBuilder.CreateSRV(R.RowMean);
		P->SimPhiEqSRV = GraphBuilder.CreateSRV(R.PhiEq);
		P->SimGlobalMeanSRV = GraphBuilder.CreateSRV(R.GlobalMean);
		P->SimCloudSRV = GraphBuilder.CreateSRV(R.CloudSource());
		P->SimFaceUAV = GraphBuilder.CreateUAV(R.Dest());
		P->SimPhiStarUAV = GraphBuilder.CreateUAV(R.PhiStar);
		P->SimCloudUAV = GraphBuilder.CreateUAV(R.CloudDest());

		AddSimPass<FFlowSimPredictCS>(GraphBuilder, TEXT("FlowSim.Predict"), P, Groups2D);
	}
	R.Swap();
	R.SwapCloud();

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

	// -- 4. Right-hand side -------------------------------------------------

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
		P->SimFaceSRV = GraphBuilder.CreateSRV(R.Source());
		P->SimRhsUAV = GraphBuilder.CreateUAV(R.Rhs);

		AddSimPass<FFlowSimRhsCS>(GraphBuilder, TEXT("FlowSim.Rhs"), P, Groups2D);
	}

	// -- 5. Helmholtz, direct: row FFT, latitude solve, inverse FFT --------

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
		P->SimPhiUAV = GraphBuilder.CreateUAV(R.Phi);

		AddSimPass<FFlowSimHelmholtzInverseCS>(GraphBuilder, TEXT("FlowSim.HelmholtzInverse"), P, GroupsRows);
	}

	// -- 6. Correct: the implicit pressure gradient -------------------------

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params);
		P->SimFaceSRV = GraphBuilder.CreateSRV(R.Source());
		P->SimPhiSRV = GraphBuilder.CreateSRV(R.Phi);
		P->SimFaceUAV = GraphBuilder.CreateUAV(R.Dest());

		AddSimPass<FFlowSimCorrectCS>(GraphBuilder, TEXT("FlowSim.Correct"), P, Groups2D);
	}
	R.Swap();
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
	P->SimRhsSRV = GraphBuilder.CreateSRV(R.Rhs);
	P->SimRowMeanSRV = GraphBuilder.CreateSRV(R.RowMean);
	P->SimCloudSRV = GraphBuilder.CreateSRV(R.CloudSource());
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
	R.Cloud[0] = GraphBuilder.RegisterExternalTexture(PooledCloud[0]);
	R.Cloud[1] = GraphBuilder.RegisterExternalTexture(PooledCloud[1]);
	R.RowMean = GraphBuilder.RegisterExternalTexture(PooledRowMean);
	R.PhiEq = GraphBuilder.RegisterExternalTexture(PooledPhiEq);
	R.GlobalMean = GraphBuilder.RegisterExternalTexture(PooledGlobalMean);
	R.Current = CurrentFace;
	R.CloudCurrent = CurrentCloud;

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
		const int32 Expected = Params.GridSize.X * Params.GridSize.Y * Params.GridSize.Z * StateFloatsPerCell;

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

			// Balanced by construction, so there is no cold solve: the
			// geopotential already matches the flow it starts under.
			AddInitPass(GraphBuilder, Params, R);
		}

		PendingRestore.Empty();
		bInitialised = true;
	}

	for (int32 Step = 0; Step < NumSubsteps; ++Step)
	{
		AddSubstep(GraphBuilder, Params, R);
	}

	// Final reduce and reconstruct, so the texture the material reads matches
	// the state the last substep produced rather than the one it started from.
	AddReducePasses(GraphBuilder, Params, R);
	AddReconstructPass(GraphBuilder, Params, R);

	AddDebugPass(GraphBuilder, Params, R);

	// Carry both ping-pong indices across the frame boundary. The faces swap
	// three times per substep and the cloud once, so both genuinely alternate.
	CurrentFace = R.Current;
	CurrentCloud = R.CloudCurrent;
}