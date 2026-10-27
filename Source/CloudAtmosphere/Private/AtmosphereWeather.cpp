#include "AtmosphereWeather.h"
#include "FlowSimulation.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"
#include "RHI.h"
#include "RHIGPUReadback.h"
#include "RHIStaticStates.h"
#include "ShaderParameterStruct.h"
#include "TextureResource.h"

// ---------------------------------------------------------------------------
// The map
// ---------------------------------------------------------------------------

FVector4f FAtmosphereWeatherMap::SampleGrid(const TArray<FVector4f>& Grid, int32 W, int32 H, const FVector& PlanetDir)
{
	if (W <= 0 || H <= 0 || Grid.Num() != W * H)
	{
		return FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
	}

	const FVector D = PlanetDir.GetSafeNormal();
	const double Lon = FMath::Atan2(D.Y, D.X);
	const double Lat = FMath::Asin(FMath::Clamp(D.Z, -1.0, 1.0));

	// Cell coordinates, centres at integers: wrap in longitude, clamp in latitude.
	const double X = (Lon + UE_DOUBLE_PI) / (2.0 * UE_DOUBLE_PI) * W - 0.5;
	const double Y = FMath::Clamp((Lat / UE_DOUBLE_PI + 0.5) * H - 0.5, 0.0, (double)(H - 1));

	const int32 X0 = FMath::FloorToInt32(X);
	const int32 Y0 = FMath::Min(FMath::FloorToInt32(Y), H - 1);
	const float FX = (float)(X - X0);
	const float FY = (float)(Y - Y0);

	const int32 XA = ((X0 % W) + W) % W;
	const int32 XB = (XA + 1) % W;
	const int32 YB = FMath::Min(Y0 + 1, H - 1);

	const FVector4f A = FMath::Lerp(Grid[Y0 * W + XA], Grid[Y0 * W + XB], FX);
	const FVector4f B = FMath::Lerp(Grid[YB * W + XA], Grid[YB * W + XB], FX);

	return FMath::Lerp(A, B, FY);
}

FVector4f FAtmosphereWeatherMap::Sample(const FVector& PlanetDir) const
{
	return SampleGrid(Cells, Width, Height, PlanetDir);
}

FVector4f FAtmosphereWeatherMap::SampleBroad(const FVector& PlanetDir) const
{
	return SampleGrid(Broad.Num() == Cells.Num() ? Broad : Cells, Width, Height, PlanetDir);
}

void FAtmosphereWeatherMap::Finish(int32 BroadCells)
{
	Broad.Reset();
	MeanSpeed = 0.0f;

	if (Width <= 0 || Height <= 0 || Cells.Num() != Width * Height)
	{
		return;
	}

	// The mean speed, area-weighted (cos latitude per row).
	double Sum = 0.0;
	double Weight = 0.0;

	for (int32 J = 0; J < Height; ++J)
	{
		const double Lat = ((J + 0.5) / Height - 0.5) * UE_DOUBLE_PI;
		const double C = FMath::Cos(Lat);

		for (int32 I = 0; I < Width; ++I)
		{
			const FVector4f& V = Cells[J * Width + I];
			Sum += C * FVector3f(V.X, V.Y, V.Z).Size();
			Weight += C;
		}
	}

	MeanSpeed = Weight > 0.0 ? (float)(Sum / Weight) : 0.0f;

	// Separable box filter, wrapping in longitude (wider toward the poles, so a
	// system's span stays about the same on the ground), clamped in latitude.
	TArray<FVector4f> Rows;
	Rows.SetNumZeroed(Cells.Num());

	const int32 R = FMath::Max(BroadCells, 0);

	for (int32 J = 0; J < Height; ++J)
	{
		const double Lat = ((J + 0.5) / Height - 0.5) * UE_DOUBLE_PI;
		const int32 RX = FMath::Min(FMath::CeilToInt32(R / FMath::Max(FMath::Cos(Lat), 0.1)), Width / 2);

		for (int32 I = 0; I < Width; ++I)
		{
			FVector4f Acc(0.0f, 0.0f, 0.0f, 0.0f);

			for (int32 K = -RX; K <= RX; ++K)
			{
				Acc += Cells[J * Width + ((I + K) % Width + Width) % Width];
			}

			Rows[J * Width + I] = Acc / (float)(2 * RX + 1);
		}
	}

	Broad.SetNumZeroed(Cells.Num());

	for (int32 J = 0; J < Height; ++J)
	{
		for (int32 I = 0; I < Width; ++I)
		{
			FVector4f Acc(0.0f, 0.0f, 0.0f, 0.0f);
			int32 Count = 0;

			for (int32 K = -R; K <= R; ++K)
			{
				const int32 JK = J + K;

				if (JK >= 0 && JK < Height)
				{
					Acc += Rows[JK * Width + I];
					++Count;
				}
			}

			Broad[J * Width + I] = Count > 0 ? Acc / (float)Count : Rows[J * Width + I];
		}
	}
}

// ---------------------------------------------------------------------------
// The pass
// ---------------------------------------------------------------------------

BEGIN_SHADER_PARAMETER_STRUCT(FAtmosphereWeatherMapParameters, )
	SHADER_PARAMETER_TEXTURE(Texture2DArray, WeatherFlow)
	SHADER_PARAMETER_SAMPLER(SamplerState, WeatherFlowSampler)
	SHADER_PARAMETER(FUintVector2, WeatherMapSize)
	SHADER_PARAMETER(float, WeatherSpin)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, WeatherMapOut)
END_SHADER_PARAMETER_STRUCT()

class FAtmosphereWeatherMapCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FAtmosphereWeatherMapCS);

	using FParameters = FAtmosphereWeatherMapParameters;
	SHADER_USE_PARAMETER_STRUCT(FAtmosphereWeatherMapCS, FGlobalShader);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FAtmosphereWeatherMapCS,
	"/Plugin/CloudAtmosphere/Private/AtmosphereWeatherMap.usf", "MainWeatherMapCS", SF_Compute);

FAtmosphereWeatherReadback::FAtmosphereWeatherReadback(const TSharedRef<FAtmosphereWeatherFeed, ESPMode::ThreadSafe>& InFeed)
	: Feed(InFeed)
{
}

FAtmosphereWeatherReadback::~FAtmosphereWeatherReadback() = default;

void FAtmosphereWeatherReadback::Request_RenderThread(FRHICommandListImmediate& RHICmdList,
	FTextureRenderTargetResource* Flow, float SpinAngle)
{
	check(IsInRenderingThread());

	FTextureRHIRef FlowTexture = Flow ? FTextureRHIRef(Flow->GetRenderTargetTexture()) : FTextureRHIRef();

	// At most a few in flight: a stalled GPU doesn't pile them up.
	if (!FlowTexture.IsValid() || Pending.Num() >= 3)
	{
		return;
	}

	TShaderMapRef<FAtmosphereWeatherMapCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	if (!Shader.IsValid())
	{
		return;
	}

	const uint32 Cells = (uint32)(Width * Height);

	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CloudAtmosphere.WeatherMap"));

	FRDGBufferRef Map = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), Cells), TEXT("CloudAtmosphere.WeatherMap"));

	FAtmosphereWeatherMapParameters* P = GraphBuilder.AllocParameters<FAtmosphereWeatherMapParameters>();
	P->WeatherFlow = FlowTexture;
	P->WeatherFlowSampler = TStaticSamplerState<SF_Bilinear, AM_Wrap, AM_Clamp, AM_Clamp>::GetRHI();
	P->WeatherMapSize = FUintVector2((uint32)Width, (uint32)Height);
	P->WeatherSpin = SpinAngle;
	P->WeatherMapOut = GraphBuilder.CreateUAV(Map);

	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("AtmosphereWeatherMap %dx%d", Width, Height),
		Shader, P, FComputeShaderUtils::GetGroupCount(FIntPoint(Width, Height), 8));

	TUniquePtr<FRHIGPUBufferReadback> Readback = MakeUnique<FRHIGPUBufferReadback>(TEXT("CloudAtmosphere.WeatherReadback"));
	AddEnqueueCopyPass(GraphBuilder, Readback.Get(), Map, Cells * sizeof(FVector4f));

	GraphBuilder.Execute();

	Pending.Add(MoveTemp(Readback));
}

void FAtmosphereWeatherReadback::Poll_RenderThread()
{
	check(IsInRenderingThread());

	// In order: the newest that has landed wins, and those before it go with it.
	int32 Landed = INDEX_NONE;

	for (int32 Index = 0; Index < Pending.Num(); ++Index)
	{
		if (Pending[Index]->IsReady())
		{
			Landed = Index;
		}
	}

	if (Landed == INDEX_NONE)
	{
		return;
	}

	const int32 Cells = Width * Height;

	TSharedPtr<FAtmosphereWeatherMap, ESPMode::ThreadSafe> Map = MakeShared<FAtmosphereWeatherMap, ESPMode::ThreadSafe>();
	Map->Width = Width;
	Map->Height = Height;
	Map->Cells.SetNumUninitialized(Cells);

	if (const void* Data = Pending[Landed]->Lock(Cells * sizeof(FVector4f)))
	{
		FMemory::Memcpy(Map->Cells.GetData(), Data, Cells * sizeof(FVector4f));
		Pending[Landed]->Unlock();

		Map->Finish(BroadCells);
		Map->Serial = ++Serial;
		Feed->Publish(Map);
	}

	Pending.RemoveAt(0, Landed + 1);
}

// ---------------------------------------------------------------------------
// The climate (stage A11)
// ---------------------------------------------------------------------------

FVector4f FAtmosphereClimateMap::Sample(const FVector& PlanetDir) const
{
	return SampleGrid(Cells, Width, Height, PlanetDir);
}

FVector4f FAtmosphereClimateMap::SampleDrift(const FVector& PlanetDir) const
{
	return SampleGrid(Drift, Width, Height, PlanetDir);
}

FVector4f FAtmosphereClimateMap::SampleGrid(const TArray<FVector4f>& Grid, int32 GridWidth, int32 GridHeight, const FVector& PlanetDir)
{
	if (GridWidth <= 0 || GridHeight <= 0 || Grid.Num() != GridWidth * GridHeight)
	{
		return FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
	}

	const FVector D = PlanetDir.GetSafeNormal();
	double Lon = FMath::Atan2(D.Y, D.X);
	Lon = Lon < 0.0 ? Lon + 2.0 * UE_DOUBLE_PI : Lon;

	// The sim's grid: centres at integers, longitude from 0, rows even in sin(latitude).
	const double X = Lon / (2.0 * UE_DOUBLE_PI) * GridWidth - 0.5;
	const double Y = FMath::Clamp((FMath::Clamp(D.Z, -1.0, 1.0) + 1.0) * 0.5 * GridHeight - 0.5, 0.0, (double)(GridHeight - 1));

	const int32 X0 = FMath::FloorToInt32(X);
	const int32 Y0 = FMath::Min(FMath::FloorToInt32(Y), GridHeight - 1);
	const float FX = (float)(X - X0);
	const float FY = (float)(Y - Y0);

	const int32 XA = ((X0 % GridWidth) + GridWidth) % GridWidth;
	const int32 XB = (XA + 1) % GridWidth;
	const int32 YB = FMath::Min(Y0 + 1, GridHeight - 1);

	const FVector4f A = FMath::Lerp(Grid[Y0 * GridWidth + XA], Grid[Y0 * GridWidth + XB], FX);
	const FVector4f B = FMath::Lerp(Grid[YB * GridWidth + XA], Grid[YB * GridWidth + XB], FX);

	return FMath::Lerp(A, B, FY);
}

FAtmosphereClimateReadback::FAtmosphereClimateReadback(const TSharedRef<FAtmosphereClimateFeed, ESPMode::ThreadSafe>& InFeed)
	: Feed(InFeed)
{
}

FAtmosphereClimateReadback::~FAtmosphereClimateReadback() = default;

void FAtmosphereClimateReadback::Request_RenderThread(FRHICommandListImmediate& RHICmdList, FFlowSimulation* Simulation, float Turnovers, bool bSpinningUp,
	bool bSnapshot, double RequestSeconds)
{
	check(IsInRenderingThread());

	if (!Simulation || Pending.Num() >= 3)
	{
		return;
	}

	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CloudAtmosphere.ClimateReadback"));

	FInFlight Copy;
	Copy.Buffer = MakeUnique<FRHIGPUBufferReadback>(TEXT("CloudAtmosphere.ClimateReadback"));
	Copy.Turnovers = Turnovers;
	Copy.bSpinningUp = bSpinningUp;
	Copy.bSnapshot = bSnapshot;
	Copy.RequestSeconds = RequestSeconds;

	const bool bCopied = Simulation->AddClimateCapturePass_RenderThread(GraphBuilder, Copy.Buffer.Get(), Copy.Grid);

	GraphBuilder.Execute();

	if (bCopied)
	{
		Pending.Add(MoveTemp(Copy));
	}
}

void FAtmosphereClimateReadback::Poll_RenderThread()
{
	check(IsInRenderingThread());

	int32 Landed = INDEX_NONE;

	for (int32 Index = 0; Index < Pending.Num(); ++Index)
	{
		if (Pending[Index].Buffer->IsReady())
		{
			Landed = Index;
		}
	}

	if (Landed == INDEX_NONE)
	{
		return;
	}

	const FIntVector Grid = Pending[Landed].Grid;
	const int32 Cells = Grid.X * Grid.Y;

	if (Cells > 0)
	{
		if (const void* Data = Pending[Landed].Buffer->Lock(2 * Cells * sizeof(FVector4f)))
		{
			TSharedPtr<FAtmosphereClimateMap, ESPMode::ThreadSafe> Map = MakeShared<FAtmosphereClimateMap, ESPMode::ThreadSafe>();
			Map->Width = Grid.X;
			Map->Height = Grid.Y;
			Map->Turnovers = Pending[Landed].Turnovers;
			Map->bSpinningUp = Pending[Landed].bSpinningUp;
			Map->bSnapshot = Pending[Landed].bSnapshot;
			Map->RequestSeconds = Pending[Landed].RequestSeconds;
			Map->Cells.SetNumUninitialized(Cells);
			FMemory::Memcpy(Map->Cells.GetData(), Data, Cells * sizeof(FVector4f));
			// Stage T5c: the sand's drift, after the cells.
			Map->Drift.SetNumUninitialized(Cells);
			FMemory::Memcpy(Map->Drift.GetData(), static_cast<const FVector4f*>(Data) + Cells, Cells * sizeof(FVector4f));
			Pending[Landed].Buffer->Unlock();

			Map->Serial = ++Serial;
			Feed->Publish(Map);
		}
	}

	Pending.RemoveAt(0, Landed + 1);
}
