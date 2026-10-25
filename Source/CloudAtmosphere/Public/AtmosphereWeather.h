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

class FTextureRenderTargetResource;

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

	/** Reads the field's weather back: the flow atlas the field draws, and its spin angle.
	 *  Render thread. */
	void Request_RenderThread(FRHICommandListImmediate& RHICmdList, FTextureRenderTargetResource* Flow, float SpinAngle);

	/** Publishes the readbacks that have landed. Render thread. */
	void Poll_RenderThread();

private:
	TSharedRef<FAtmosphereWeatherFeed, ESPMode::ThreadSafe> Feed;
	TArray<TUniquePtr<class FRHIGPUBufferReadback>> Pending;
	uint64 Serial = 0;
};
