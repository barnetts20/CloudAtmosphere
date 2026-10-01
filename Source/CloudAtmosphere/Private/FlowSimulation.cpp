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
#include "TextureResource.h"

DEFINE_LOG_CATEGORY(LogFlowSim);

void FFlowSimParams::ResolveTextures_RenderThread()
{
	check(IsInRenderingThread());

	ForcingTexture = ForcingResource ? ForcingResource->TextureRHI : FTextureRHIRef();
	FlowTexture = FlowResource ? FTextureRHIRef(FlowResource->GetRenderTargetTexture()) : FTextureRHIRef();
	DebugTexture = DebugResource ? FTextureRHIRef(DebugResource->GetRenderTargetTexture()) : FTextureRHIRef();
}

static_assert(UFlowSnapshot::FloatsPerCell == FFlowSimulation::StateFloatsPerCell
	&& UFlowSnapshot::LegacyFloatsPerCell == FFlowSimulation::LegacyStateFloatsPerCell,
	"Snapshot layout and solver state disagree about floats per cell.");

static_assert(UFlowSnapshot::TrailingFloats == FFlowSimulation::StateTrailingFloats
	&& FFlowSimulation::StateTrailingFloats == 8 * FlowSimShader::MaxStormCells,
	"Snapshot layout and solver state disagree about the storm cells.");

static_assert(sizeof(FFlowSimParams::PerpetualShape) == FlowSimShader::MaxPerpetualStorms * sizeof(FVector4f)
	&& sizeof(FFlowSimParams::PerpetualLook) == FlowSimShader::MaxPerpetualStorms * sizeof(FVector4f)
	&& sizeof(FFlowSimParams::PerpetualForm) == FlowSimShader::MaxPerpetualStorms * sizeof(FVector4f)
	&& sizeof(FFlowSimParams::PerpetualRate) == FlowSimShader::MaxPerpetualStorms * sizeof(double),
	"FFlowSimParams holds one perpetual storm per slot.");

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
	FRDGTextureRef CellFlow = nullptr;

	/** The scalars the passes being added read: this substep's, or the
	 *  frame's outside the substeps. */
	TRDGUniformBufferRef<FFlowSimUniformParameters> Uniforms;

	FRDGTextureRef CellColumn = nullptr;

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

	/** Fills every scalar parameter. The one place a config value becomes a
	 *  shader value. */
	void FillCommonParameters(FFlowSimUniformParameters& P, const FFlowSimParams& Params)
	{
		P.GridSize = Params.GridSize;
		P.InvGridSize = FVector2f(
			1.0f / FMath::Max(Params.GridSize.X, 1),
			1.0f / FMath::Max(Params.GridSize.Y, 1));

		P.JetParams = Params.JetParams;
		P.WidthBias = Params.WidthBias;
		P.ZonalProfile = Params.ZonalProfile;
		P.JetLatitudeScale = Params.JetLatitudeScale;

		for (int32 i = 0; i < 8; ++i)
		{
			P.LayerProfile[i] = Params.LayerProfile[i];
			P.LayerState[i] = Params.LayerState[i];
		}

		for (int32 i = 0; i < 16; ++i)
		{
			P.MatMontgomery[i] = Params.Stack.Montgomery[i];
			P.MatMontgomeryInverse[i] = Params.Stack.MontgomeryInverse[i];
			P.MatModeToLayer[i] = Params.Stack.ModeToLayer[i];
			P.MatLayerToMode[i] = Params.Stack.LayerToMode[i];
			P.MatModeToMontgomery[i] = Params.Stack.ModeToMontgomery[i];
		}

		P.DeltaTime = Params.DeltaTime;

		// Wrapped in double: the forcing's pattern seeds repeat after 4096
		// lifetimes, and the reset test needs only the phase.
		// The forcing clock as a whole cycle and a fraction, so the fraction keeps
		// full precision however many cycles have run.
		const double ForcingCycles = Params.Time / FMath::Max((double)Params.ForcingLifetime, 1e-3);
		const double ForcingWhole = FMath::FloorToDouble(ForcingCycles);

		P.ForcingCycle = (uint32)FMath::Fmod(ForcingWhole, 4294967296.0);
		P.ForcingFraction = (float)(ForcingCycles - ForcingWhole);
		P.NoiseClock = (float)FMath::Fmod(Params.Time / (double)Params.NoiseResetTime, 2.0);
		P.PlanetaryVorticity = Params.PlanetaryVorticity;
		P.WaveSpeedSq = Params.Stack.DesignSpeedSq;
		P.ImplicitWeight = Params.ImplicitWeight;

		P.ForcingChannel = Params.ForcingChannel;
		P.HasForcing = Params.ForcingTexture.IsValid() ? 1 : 0;

		P.NudgeRate = Params.NudgeRate;
		P.ForcingAmplitude = Params.ForcingAmplitude;
		P.ForcingFrequency = Params.ForcingFrequency;
		P.ForcingLifetime = Params.ForcingLifetime;
		P.DragRate = Params.DragRate;
		P.LayerCoupling = Params.LayerCoupling;
		P.DivergenceDamping = Params.DivergenceDamping;
		P.FroudeCeiling = Params.FroudeCeiling;
		P.ShockDamping = Params.ShockDamping;
		P.SharpCentre = Params.bSharpCentreVelocity ? 1 : 0;
		P.ThermalRelaxation = Params.ThermalRelaxation;
		P.ThermalParams = Params.ThermalParams;

		P.CondensationRate = Params.CondensationRate;
		P.EvaporationRate = Params.EvaporationRate;
		P.CloudDecay = 1.0f / FMath::Max(Params.CloudLifetime, 1e-3f);
		P.MoistureParams = Params.MoistureParams;
		P.LatentHeating = Params.LatentHeating;
		P.AscentSmoothing = Params.AscentSmoothing;
		P.StormParams = Params.StormParams;
		P.WindEvaporation = Params.WindEvaporation;

		P.CellShape = Params.CellShape;
		P.CellVortex = Params.CellVortex;
		P.CellDraft = Params.CellDraft;
		P.CellLife = Params.CellLife;
		P.CellMotion = Params.CellMotion;
		P.CellGenesis = Params.CellGenesis;
		P.CellCloud = Params.CellCloud;
		P.CellWindBreadth = Params.CellWindBreadth;
		P.CellSustain = Params.CellSustain;
		P.CellEyeDepth = Params.CellEyeDepth;
		P.CellCoreFollow = Params.CellCoreFollow;
		P.CellEyeSoftness = Params.CellEyeSoftness;
		P.CellCount = FMath::Clamp(Params.CellCount, 0, FlowSimShader::MaxStormCells);
		P.StepIndex = Params.StepIndex;

		// Each perpetual storm's longitude at PerpetualTime, in double and
		// wrapped, so it holds its precision however long the sim has run.
		P.PerpetualCount = FMath::Clamp(Params.PerpetualCount, 0, FlowSimShader::MaxPerpetualStorms);
		P.PerpetualForcing = Params.PerpetualForcing;

		for (int32 i = 0; i < FlowSimShader::MaxPerpetualStorms; ++i)
		{
			FVector4f Shape = Params.PerpetualShape[i];
			Shape.Y = (float)FMath::Fmod((double)Shape.Y + Params.PerpetualRate[i] * Params.PerpetualTime, 2.0 * UE_DOUBLE_PI);

			P.PerpetualShape[i] = Shape;
			P.PerpetualLook[i] = Params.PerpetualLook[i];
			P.PerpetualForm[i] = Params.PerpetualForm[i];
		}

		P.NoiseDriftRate = Params.NoiseDriftRate;
		P.NoiseResetTime = Params.NoiseResetTime;

		P.FilterLatitude = Params.FilterLatitude;

		P.OutputScales = Params.OutputScales;
		P.AtlasFaceSize = Params.AtlasFaceSize;
		P.StateBlend = FMath::Clamp(Params.StateBlend, 0.0f, 1.0f);

		P.DebugMode = Params.DebugMode;
		P.DebugLayer = Params.DebugLayer;
		P.DebugScale = Params.DebugScale;
		P.DebugSize = Params.DebugSize;
	}

	TRDGUniformBufferRef<FFlowSimUniformParameters> CreateUniforms(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params)
	{
		FFlowSimUniformParameters* U = GraphBuilder.AllocParameters<FFlowSimUniformParameters>();
		FillCommonParameters(*U, Params);
		return GraphBuilder.CreateUniformBuffer(U);
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

	/** A pass's parameters over shared scalars. Resources are attached per pass
	 *  afterwards. */
	FFlowSimParameters* NewParameters(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params,
		TRDGUniformBufferRef<FFlowSimUniformParameters> Uniforms)
	{
		FFlowSimParameters* P = GraphBuilder.AllocParameters<FFlowSimParameters>();
		P->FlowSimUB = Uniforms;

		// A null forcing texture binds black and SimHasForcing gates it to zero.
		P->SimForcingNoise = Params.ForcingTexture.IsValid()
			? Params.ForcingTexture
			: GBlackVolumeTexture->TextureRHI;

		// Wrap on all three axes: the forcing volume is a tiling bake, and a
		// clamped read puts a band of constant value along each face.
		P->SimForcingNoiseSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();

		return P;
	}

	/** What MainInitBalanceCS reads: the grid, the jets, the thermal shear and
	 *  the rotation. */
	TArray<float> BalanceKeyOf(const FFlowSimParams& Params)
	{
		TArray<float> Key;
		Key.Reserve(48);

		Key.Append({ (float)Params.GridSize.X, (float)Params.GridSize.Y, (float)Params.GridSize.Z });
		Key.Append({ Params.JetParams.X, Params.JetParams.Y, Params.JetParams.Z, Params.JetParams.W });
		Key.Append({ Params.WidthBias, (float)Params.ZonalProfile, Params.JetLatitudeScale, Params.PlanetaryVorticity });
		Key.Append({ Params.ThermalParams.X, Params.ThermalParams.Y, Params.ThermalParams.Z, Params.ThermalParams.W });

		for (int32 i = 0; i < 8; ++i)
		{
			Key.Append({ Params.LayerProfile[i].X, Params.LayerProfile[i].Y, Params.LayerProfile[i].Z, Params.LayerProfile[i].W });
		}

		return Key;
	}

	/** Every scalar parameter a pass sees as bytes, less the output blend and
	 *  the clocks, which Reduce and Reconstruct do not read: what they read,
	 *  and more. */
	TArray<uint8> LatestKeyOf(const FFlowSimParams& Params)
	{
		FFlowSimUniformParameters P;
		FMemory::Memzero(&P, sizeof(P));
		FillCommonParameters(P, Params);
		P.StateBlend = 0.0f;
		P.ForcingCycle = 0;
		P.ForcingFraction = 0.0f;
		P.NoiseClock = 0.0f;
		P.StepIndex = 0;

		TArray<uint8> Key;
		Key.Append(reinterpret_cast<const uint8*>(&P), sizeof(P));
		return Key;
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
	PooledCellFlow.SafeRelease();
	PooledCellColumn.SafeRelease();

	BalanceKey.Reset();
	LatestKey.Reset();

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

	// Two float4 of state per slot (captured in snapshots), then two of vortex
	// gains, two of inflow gains, one of health and one of the low per
	// slot (rewritten each step). Perpetual storms take the first slots. Must
	// match SIM_CELL_BUFFER_SIZE.
	PooledCells = AllocatePooledBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), 8 * FlowSimShader::MaxStormCells),
		TEXT("FlowSim.Cells"));

	// Streamfunction at the corners and potential at the centres, one row past
	// the last centre row for the north polar faces.
	PooledCellFlow = AllocatePooledTexture(
		FRDGTextureDesc::Create2DArray(FIntPoint(Size.X, Size.Y + 1), PF_G32R32F, FClearValueBinding::Black, Flags, Slices),
		TEXT("FlowSim.CellFlow"));

	PooledCellColumn = AllocatePooledTexture(
		FRDGTextureDesc::Create2D(Size, PF_A32B32G32R32F, FClearValueBinding::Black, Flags),
		TEXT("FlowSim.CellColumn"));

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
	FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
	P->SimMontgomeryEqUAV = GraphBuilder.CreateUAV(R.MontgomeryEq);

	AddSimPass<FFlowSimInitBalanceCS>(GraphBuilder, TEXT("FlowSim.Balance"), P, GroupCountLayers(Params.GridSize));
}

void FFlowSimulation::AddInitPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R)
{
	FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
	P->SimMontgomeryEqSRV = GraphBuilder.CreateSRV(R.MontgomeryEq);
	P->SimFaceUAV = GraphBuilder.CreateUAV(R.Source());
	P->SimPhiUAV = GraphBuilder.CreateUAV(R.Phi);
	P->SimTracerUAV = GraphBuilder.CreateUAV(R.TracerSource());
	P->SimNoiseUAV = GraphBuilder.CreateUAV(R.NoiseSource());
	P->SimCellUAV = GraphBuilder.CreateUAV(R.Cells);

	AddSimPass<FFlowSimInitStateCS>(GraphBuilder, TEXT("FlowSim.InitState"), P, GroupCount2D(Params.GridSize));
}

void FFlowSimulation::AddRestorePass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R, int32 FloatsPerCell)
{
	FRDGBufferRef Upload = CreateStructuredBuffer(
		GraphBuilder,
		TEXT("FlowSim.RestoreUpload"),
		sizeof(float),
		PendingRestore.Num(),
		PendingRestore.GetData(),
		PendingRestore.Num() * sizeof(float));

	FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
	P->SimRestoreBuffer = GraphBuilder.CreateSRV(Upload);
	P->SimRestoreFloatsPerCell = (uint32)FloatsPerCell;
	P->SimFaceUAV = GraphBuilder.CreateUAV(R.Source());
	P->SimPhiUAV = GraphBuilder.CreateUAV(R.Phi);
	P->SimTracerUAV = GraphBuilder.CreateUAV(R.TracerSource());
	P->SimNoiseUAV = GraphBuilder.CreateUAV(R.NoiseSource());
	P->SimCellUAV = GraphBuilder.CreateUAV(R.Cells);

	AddSimPass<FFlowSimRestoreCS>(GraphBuilder, TEXT("FlowSim.Restore"), P, GroupCount2D(Params.GridSize));
}

bool FFlowSimulation::AddCapturePass_RenderThread(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, FRHIGPUBufferReadback* Readback, FIntVector& OutGrid)
{
	check(IsInRenderingThread());

	OutGrid = AllocatedGrid;

	if (!Readback || !PooledPhi.IsValid())
	{
		return false;
	}

	const int32 Total = AllocatedGrid.X * AllocatedGrid.Y * AllocatedGrid.Z;

	if (Total <= 0)
	{
		return false;
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

	// THE ALLOCATED GRID, NOT THE CONFIG'S: a grid edit is not reallocated
	// until the next step, and the capture indexes the state as it is.
	FFlowSimParams Allocated = Params;
	Allocated.GridSize = AllocatedGrid;

	FFlowSimParameters* P = NewParameters(GraphBuilder, Allocated, CreateUniforms(GraphBuilder, Allocated));

	P->SimFaceSRV = GraphBuilder.CreateSRV(Face);
	P->SimPhiSRV = GraphBuilder.CreateSRV(Phi);
	P->SimTracerSRV = GraphBuilder.CreateSRV(Tracer);
	P->SimNoiseSRV = GraphBuilder.CreateSRV(Noise);
	P->SimCellSRV = GraphBuilder.CreateSRV(Cells);
	P->SimCaptureBuffer = GraphBuilder.CreateUAV(Capture);

	AddSimPass<FFlowSimCaptureCS>(GraphBuilder, TEXT("FlowSim.Capture"), P, GroupCount2D(AllocatedGrid));

	AddEnqueueCopyPass(GraphBuilder, Readback, Capture, Floats * sizeof(float));

	return true;
}

void FFlowSimulation::AddReducePasses(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R)
{
	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
		P->SimFaceSRV = GraphBuilder.CreateSRV(R.Source());
		P->SimPhiSRV = GraphBuilder.CreateSRV(R.Phi);
		P->SimRowMeanUAV = GraphBuilder.CreateUAV(R.RowMean);

		AddSimPass<FFlowSimReduceRowsCS>(GraphBuilder, TEXT("FlowSim.ReduceRows"), P, GroupCountRows(Params.GridSize));
	}

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
		P->SimRowMeanSRV = GraphBuilder.CreateSRV(R.RowMean);
		P->SimGlobalMeanUAV = GraphBuilder.CreateUAV(R.GlobalMean);

		// One group per layer.
		AddSimPass<FFlowSimReduceGlobalCS>(GraphBuilder, TEXT("FlowSim.ReduceGlobal"), P,
			FIntVector(Params.GridSize.Z, 1, 1));
	}
}

void FFlowSimulation::AddReconstructPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R, bool bLatest)
{
	FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
	P->SimFaceSRV = GraphBuilder.CreateSRV(R.Source());
	P->SimPhiSRV = GraphBuilder.CreateSRV(R.Phi);
	P->SimRowMeanSRV = GraphBuilder.CreateSRV(R.RowMean);
	P->SimTracerSRV = GraphBuilder.CreateSRV(R.TracerSource());
	P->SimNoiseSRV = GraphBuilder.CreateSRV(R.NoiseSource());
	P->SimCentreUAV = GraphBuilder.CreateUAV(bLatest ? R.CentreLatest : R.Centre);
	P->SimReconstructLatest = bLatest ? 1 : 0;

	// Bound either way, since the kernel declares it; unwritten when latest.
	P->SimExplicitUAV = GraphBuilder.CreateUAV(R.Explicit);
	P->SimLatLonUAV = GraphBuilder.CreateUAV(bLatest ? R.LatLonLatest : R.LatLon);

	AddSimPass<FFlowSimReconstructCS>(GraphBuilder, TEXT("FlowSim.Reconstruct"), P, GroupCount2D(Params.GridSize));
}

void FFlowSimulation::AddCellsPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R)
{
	FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
	P->SimCentreSRV = GraphBuilder.CreateSRV(R.Centre);
	P->SimLatLonSRV = GraphBuilder.CreateSRV(R.LatLon);
	P->SimCellUAV = GraphBuilder.CreateUAV(R.Cells);

	AddSimPass<FFlowSimCellsCS>(GraphBuilder, TEXT("FlowSim.Cells"), P, FIntVector(1, 1, 1));
}

void FFlowSimulation::AddCellFieldPass(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, const FFlowSimResources& R)
{
	FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
	P->SimLatLonSRV = GraphBuilder.CreateSRV(R.LatLon);
	P->SimCellSRV = GraphBuilder.CreateSRV(R.Cells);
	P->SimCellFlowUAV = GraphBuilder.CreateUAV(R.CellFlow);
	P->SimCellColumnUAV = GraphBuilder.CreateUAV(R.CellColumn);

	// One row past the centres for the polar faces, one slice past the layers
	// for the column terms.
	const FIntVector Groups = GroupCount2D(FIntVector(Params.GridSize.X, Params.GridSize.Y + 1, Params.GridSize.Z + 1));

	AddSimPass<FFlowSimCellFieldCS>(GraphBuilder, TEXT("FlowSim.CellField"), P, Groups);
}

void FFlowSimulation::AddSubstep(FRDGBuilder& GraphBuilder, const FFlowSimParams& Params, FFlowSimResources& R)
{
	// This substep's clocks, for every pass in it.
	R.Uniforms = CreateUniforms(GraphBuilder, Params);

	const FIntVector Groups2D = GroupCount2D(Params.GridSize);

	// -- 1. Means and centre fields of the current state ------------------

	AddReducePasses(GraphBuilder, Params, R);
	AddReconstructPass(GraphBuilder, Params, R, false);
	AddCellsPass(GraphBuilder, Params, R);

	// Without cells or perpetual storms Predict reads none of the fields, so
	// they are left stale.
	if (Params.CellCount > 0 || Params.PerpetualCount > 0)
	{
		AddCellFieldPass(GraphBuilder, Params, R);
	}

	// -- 2. Predict: advection and every explicit term --------------------

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
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
		P->SimCellFlowSRV = GraphBuilder.CreateSRV(R.CellFlow);
		P->SimCellColumnSRV = GraphBuilder.CreateSRV(R.CellColumn);
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
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
		P->SimFaceSRV = GraphBuilder.CreateSRV(R.Source());
		P->SimPhiStarSRV = GraphBuilder.CreateSRV(R.PhiStar);
		P->SimFaceUAV = GraphBuilder.CreateUAV(R.Dest());
		P->SimRhsUAV = GraphBuilder.CreateUAV(R.Rhs);

		AddSimPass<FFlowSimFilterCS>(GraphBuilder, TEXT("FlowSim.Filter"), P, Groups2D);
	}
	R.Swap();

	// -- 4. Right-hand side, projected onto the vertical modes -------------

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
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
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
		P->SimRhsSRV = GraphBuilder.CreateSRV(R.Rhs);
		P->SimSpectrumUAV = GraphBuilder.CreateUAV(R.Spectrum[0]);

		AddSimPass<FFlowSimHelmholtzForwardCS>(GraphBuilder, TEXT("FlowSim.HelmholtzForward"), P, GroupsRows);
	}

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
		P->SimSpectrumSRV = GraphBuilder.CreateSRV(R.Spectrum[0]);
		P->SimSpectrumUAV = GraphBuilder.CreateUAV(R.Spectrum[1]);

		AddSimPass<FFlowSimHelmholtzColumnCS>(GraphBuilder, TEXT("FlowSim.HelmholtzColumn"), P, GroupsWavenumbers);
	}

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
		P->SimSpectrumSRV = GraphBuilder.CreateSRV(R.Spectrum[1]);
		P->SimPhiStarUAV = GraphBuilder.CreateUAV(R.PhiStar);

		AddSimPass<FFlowSimHelmholtzInverseCS>(GraphBuilder, TEXT("FlowSim.HelmholtzInverse"), P, GroupsRows);
	}

	// -- 6. Correct: the implicit pressure gradient, and the layers -----------

	{
		FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
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

	// Every layer in each thread, which shares the taps and the cells' stamp.
	const FIntVector Groups(
		FMath::DivideAndRoundUp(Atlas.X, ThreadGroupSize2D),
		FMath::DivideAndRoundUp(Atlas.Y, ThreadGroupSize2D),
		1);

	FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
	P->SimCentreSRV = GraphBuilder.CreateSRV(R.Centre);
	P->SimLatLonSRV = GraphBuilder.CreateSRV(R.LatLon);
	P->SimCentreLatestSRV = GraphBuilder.CreateSRV(R.CentreLatest);
	P->SimLatLonLatestSRV = GraphBuilder.CreateSRV(R.LatLonLatest);
	P->SimCellSRV = GraphBuilder.CreateSRV(R.Cells);

	// The latest noise state, whose phase B w is the eye tracer.
	P->SimNoiseSRV = GraphBuilder.CreateSRV(R.NoiseSource());
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

	FFlowSimParameters* P = NewParameters(GraphBuilder, Params, R.Uniforms);
	P->SimFaceSRV = GraphBuilder.CreateSRV(R.Source());
	P->SimPhiSRV = GraphBuilder.CreateSRV(R.Phi);
	P->SimPhiStarSRV = GraphBuilder.CreateSRV(R.PhiStar);
	P->SimRhsSRV = GraphBuilder.CreateSRV(R.Rhs);
	P->SimRowMeanSRV = GraphBuilder.CreateSRV(R.RowMean);
	P->SimTracerSRV = GraphBuilder.CreateSRV(R.TracerSource());
	P->SimNoiseSRV = GraphBuilder.CreateSRV(R.NoiseSource());

	// The frame's last Reconstruct wrote the latest state's output and centre
	// fields; the storm views read the bottom layer's velocity from the latter.
	P->SimLatLonSRV = GraphBuilder.CreateSRV(R.LatLonLatest);
	P->SimCentreSRV = GraphBuilder.CreateSRV(R.CentreLatest);
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
	R.CellFlow = GraphBuilder.RegisterExternalTexture(PooledCellFlow);
	R.CellColumn = GraphBuilder.RegisterExternalTexture(PooledCellColumn);
	// The frame's passes place the perpetual storms at the output's time,
	// where the resample stamps them.
	FFlowSimParams FrameParams = Params;
	FrameParams.PerpetualTime = Params.Time + ((double)NumSubsteps - 1.0 + (double)Params.StateBlend) * Params.DeltaTime;

	const TRDGUniformBufferRef<FFlowSimUniformParameters> FrameUniforms = CreateUniforms(GraphBuilder, FrameParams);

	R.Uniforms = FrameUniforms;
	R.Current = CurrentFace;
	R.TracerCurrent = CurrentTracer;

	R.Output = GraphBuilder.RegisterExternalTexture(
		CreateRenderTarget(Params.FlowTexture, TEXT("FlowSim.Flow")));

	if (Params.DebugTexture.IsValid() && Params.DebugSize.X > 0 && Params.DebugSize.Y > 0)
	{
		R.Debug = GraphBuilder.RegisterExternalTexture(
			CreateRenderTarget(Params.DebugTexture, TEXT("FlowSim.Debug")));
	}

	// Whenever its inputs change: the thermal relaxation target and the initial
	// state both read it, and a live profile edit should reach both.
	TArray<float> Balance = BalanceKeyOf(Params);

	if (Balance != BalanceKey)
	{
		AddBalancePass(GraphBuilder, Params, R);
		BalanceKey = MoveTemp(Balance);
	}

	if (bNeedsSeeding || !bInitialised)
	{
		const int32 Expected = StateFloats(Params.GridSize);
		const int32 FloatsPerCell = FloatsPerCellOf(Params.GridSize, PendingRestore.Num());

		if (FloatsPerCell > 0)
		{
			AddRestorePass(GraphBuilder, Params, R, FloatsPerCell);

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
		LatestKey.Reset();

		// The start state is also the previous state, until a step replaces it;
		// a first substep writes the same pair itself.
		if (NumSubsteps <= 0)
		{
			AddReducePasses(GraphBuilder, Params, R);
			AddReconstructPass(GraphBuilder, Params, R, false);
		}
	}

	// Each substep at its own time: the forcing's phases and the noise resets
	// are clocked by it.
	for (int32 Step = 0; Step < NumSubsteps; ++Step)
	{
		FFlowSimParams StepParams = Params;
		StepParams.Time = Params.Time + (double)Step * Params.DeltaTime;
		StepParams.StepIndex = Params.StepIndex + Step;
		StepParams.PerpetualTime = StepParams.Time + Params.DeltaTime;

		AddSubstep(GraphBuilder, StepParams, R);
	}

	// Back to the frame's scalars, which the passes below have always read.
	R.Uniforms = FrameUniforms;

	// THE OUTPUT BLENDS THE LAST TWO STATES. Each substep's reconstruct leaves
	// the state it started from in Centre and LatLon, so after the loop they
	// hold the previous state; this one writes the latest beside them. A frame
	// without substeps leaves the previous state as it was, and the latest too
	// unless a parameter changed: most frames at a low SimSpeed, and every
	// paused one.
	TArray<uint8> Latest = LatestKeyOf(Params);

	if (NumSubsteps > 0 || Latest != LatestKey)
	{
		AddReducePasses(GraphBuilder, Params, R);
		AddReconstructPass(GraphBuilder, Params, R, true);
		LatestKey = MoveTemp(Latest);
	}

	AddResamplePass(GraphBuilder, Params, R);

	AddDebugPass(GraphBuilder, Params, R);

	// Carry both ping-pong indices across the frame boundary. The faces swap
	// three times per substep and the tracers once, so both genuinely alternate.
	CurrentFace = R.Current;
	CurrentTracer = R.TracerCurrent;
}