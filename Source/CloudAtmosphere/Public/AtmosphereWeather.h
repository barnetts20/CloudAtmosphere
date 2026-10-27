// Stage A5: the flow sim's weather at the surface, on the CPU: the lowest layer's wind and the
// column's storm on a latitude-longitude grid, read back every few seconds from the field the
// atmosphere draws (AtmosphereWeatherMap.usf), for whatever needs the weather off the GPU (the
// ocean's waves, which key it on the CPU).
//
// THE WIND IS IN SIM UNITS, in the planet's local axes (the field's frame, z the spin axis).
// The sim is dimensionless, so a reader scales it to its own units; MeanSpeed (area-weighted)
// is there to calibrate against.
//
// NOT DETERMINISTIC ACROSS MACHINES: the sim runs on the GPU. A game that needs the same weather
// on every client replicates the map, or the reader's results, from the server.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "RHIFwd.h"
#include "RHIGPUReadback.h"

class FTextureRenderTargetResource;
class FFlowSimulation;

/** One read-back of the weather. Immutable once published. */
struct CLOUDATMOSPHERE_API FAtmosphereWeatherMap
{
	/** The grid: cell (i, j) at longitude (i + 0.5) / Width * 2 pi - pi, latitude
	 *  ((j + 0.5) / Height - 0.5) * pi; row-major, south to north. */
	int32 Width = 0;
	int32 Height = 0;

	/** Per cell: the wind (planet-local axes, sim units) and the storm (w, 0..1). */
	TArray<FVector4f> Cells;

	/** The same, box-filtered over a few cells each way (BroadCells): the wind at the scale
	 *  of a weather system, for a swell's heading. */
	TArray<FVector4f> Broad;

	/** The wind's speed averaged over the sphere (sim units). */
	float MeanSpeed = 0.0f;

	/** Counts the maps published by its feed. */
	uint64 Serial = 0;

	/** Bilinear at a planet-local direction (any length). */
	FVector4f Sample(const FVector& PlanetDir) const;
	FVector4f SampleBroad(const FVector& PlanetDir) const;

	/** Fills Broad and MeanSpeed from Cells. */
	void Finish(int32 BroadCells);

private:
	static FVector4f SampleGrid(const TArray<FVector4f>& Grid, int32 Width, int32 Height, const FVector& PlanetDir);
};

/** The latest map, published by the render thread and read by any thread. */
class CLOUDATMOSPHERE_API FAtmosphereWeatherFeed
{
public:
	TSharedPtr<const FAtmosphereWeatherMap, ESPMode::ThreadSafe> Latest() const
	{
		FScopeLock Lock(&Mutex);
		return Map;
	}

	void Publish(const TSharedPtr<const FAtmosphereWeatherMap, ESPMode::ThreadSafe>& InMap)
	{
		FScopeLock Lock(&Mutex);
		Map = InMap;
	}

private:
	mutable FCriticalSection Mutex;
	TSharedPtr<const FAtmosphereWeatherMap, ESPMode::ThreadSafe> Map;
};

/** Stage A11: THE CLIMATE, the weather's long-run means read back from the sim (FlowSim.usf
 *  MainClimateCS, UFlowSimConfig::ClimateTurnovers): on the sim's own grid (Width x Height,
 *  longitude x sin(latitude) in the planet's frame with Z the spin axis and longitude 0 on +X,
 *  row-major from the south; cell (i, j) centred at longitude 2 pi (i + 0.5) / Width,
 *  sin(latitude) -1 + 2 (j + 0.5) / Height). Per cell:
 *    x precipitation (the column's net condensation, about 0 dry to 1 a saturated column in full
 *      ascent; its long-run mean, so 0.05 to 0.3 is a wet climate),
 *    y the low air's relative humidity (0 to 1),
 *    z cloud cover (0 to 1),
 *    w warmth: the column's thickness against its latitude's mean over the air's depth (about
 *      -0.1 to 0.1; positive warm). The latitude's own temperature profile and the ground's
 *      height are the reader's to add (the planet knows its terrain).
 *  Like the weather, not deterministic across machines. Immutable once published. */
struct CLOUDATMOSPHERE_API FAtmosphereClimateMap
{
	int32 Width = 0;
	int32 Height = 0;
	TArray<FVector4f> Cells;

	/** Counts the maps published by its feed. */
	uint64 Serial = 0;

	/** Stage T5a: how long the means had run past the spin-up when the map was copied, in the
	 *  sim's turnovers (UFlowSimSubsystem::GetClimateTurnovers; 0 while spinning up), and whether
	 *  the sim was still spinning up. A reader that wants a settled climate waits for a few
	 *  ClimateTurnovers. It drops back toward 0 when the sim resets. */
	float Turnovers = 0.0f;
	bool bSpinningUp = false;

	/** Stage T5b: the climate window's snapshot (UFlowSimConfig::bClimateSnapshot), the one
	 *  read back as the window ended; and FPlatformTime::Seconds at the request (a reader that
	 *  restarted the sim takes only a snapshot requested after it). */
	bool bSnapshot = false;
	double RequestSeconds = 0.0;

	/** Stage T5c: THE SAND'S DRIFT per cell (the same grid), from the lowest layer's wind V
	 *  (east, north; sim units): x, y the resultant drift (the mean of V |V|^2), z the drift
	 *  potential (the mean of |V|^3), w the mean speed. |(x, y)| / z is Fryberger's directional
	 *  variability (about 1: one wind; low: many). */
	TArray<FVector4f> Drift;

	/** Bilinear at a planet-local direction (any length): wraps in longitude, clamps at the
	 *  poles' rows. */
	FVector4f Sample(const FVector& PlanetDir) const;
	FVector4f SampleDrift(const FVector& PlanetDir) const;

private:
	static FVector4f SampleGrid(const TArray<FVector4f>& Grid, int32 GridWidth, int32 GridHeight, const FVector& PlanetDir);
};

/** Stage A11: the latest climate map, published by the render thread and read by any thread. */
class CLOUDATMOSPHERE_API FAtmosphereClimateFeed
{
public:
	TSharedPtr<const FAtmosphereClimateMap, ESPMode::ThreadSafe> Latest() const
	{
		FScopeLock Lock(&Mutex);
		return Map;
	}

	void Publish(const TSharedPtr<const FAtmosphereClimateMap, ESPMode::ThreadSafe>& InMap)
	{
		FScopeLock Lock(&Mutex);
		Map = InMap;
	}

private:
	mutable FCriticalSection Mutex;
	TSharedPtr<const FAtmosphereClimateMap, ESPMode::ThreadSafe> Map;
};

/** Stage A11: the climate read-back's render-thread side: the copies in flight. */
class CLOUDATMOSPHERE_API FAtmosphereClimateReadback
{
public:
	explicit FAtmosphereClimateReadback(const TSharedRef<FAtmosphereClimateFeed, ESPMode::ThreadSafe>& InFeed);
	~FAtmosphereClimateReadback();

	FAtmosphereClimateReadback(const FAtmosphereClimateReadback&) = delete;
	FAtmosphereClimateReadback& operator=(const FAtmosphereClimateReadback&) = delete;

	/** Copies the sim's climate back (UFlowSimSubsystem::GetSimulation). Render thread. Stage
	 *  T5a: Turnovers and bSpinningUp, the sim's at the request (the game thread's), go on the map. */
	void Request_RenderThread(FRHICommandListImmediate& RHICmdList, FFlowSimulation* Simulation, float Turnovers = 0.0f, bool bSpinningUp = false,
		bool bSnapshot = false, double RequestSeconds = 0.0);

	/** Publishes the newest copy that has landed. Render thread. */
	void Poll_RenderThread();

private:
	struct FInFlight
	{
		TUniquePtr<FRHIGPUBufferReadback> Buffer;
		FIntVector Grid = FIntVector::ZeroValue;
		float Turnovers = 0.0f;
		bool bSpinningUp = false;
		bool bSnapshot = false;
		double RequestSeconds = 0.0;
	};

	TSharedRef<FAtmosphereClimateFeed, ESPMode::ThreadSafe> Feed;
	TArray<FInFlight> Pending;
	uint64 Serial = 0;
};

/** The read-back's render-thread side: the passes and the readbacks in flight. */
class CLOUDATMOSPHERE_API FAtmosphereWeatherReadback
{
public:
	/** The grid's size: 128 x 64 cells. */
	static constexpr int32 Width = 128;
	static constexpr int32 Height = 64;

	/** The box filter's half-width for Broad, in cells. */
	static constexpr int32 BroadCells = 6;

	explicit FAtmosphereWeatherReadback(const TSharedRef<FAtmosphereWeatherFeed, ESPMode::ThreadSafe>& InFeed);
	~FAtmosphereWeatherReadback();

	// Owns its read-backs: not copied (an exported class instantiates every member it has).
	FAtmosphereWeatherReadback(const FAtmosphereWeatherReadback&) = delete;
	FAtmosphereWeatherReadback& operator=(const FAtmosphereWeatherReadback&) = delete;

	/** Reads the field's weather back: the flow atlas the field draws, and its spin angle.
	 *  Render thread. */
	void Request_RenderThread(FRHICommandListImmediate& RHICmdList, FTextureRenderTargetResource* Flow, float SpinAngle);

	/** Publishes the readbacks that have landed. Render thread. */
	void Poll_RenderThread();

private:
	TSharedRef<FAtmosphereWeatherFeed, ESPMode::ThreadSafe> Feed;
	TArray<TUniquePtr<FRHIGPUBufferReadback>> Pending;
	uint64 Serial = 0;
};
