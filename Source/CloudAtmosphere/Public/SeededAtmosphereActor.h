// An atmosphere generated from a seed.

#pragma once

#include "CoreMinimal.h"
#include "PlanetAtmosphereActor.h"
#include "AtmosphereArchetype.h"
#include "AtmosphereHarnessTypes.h"
#include "SeededAtmosphereActor.generated.h"

/** A planet atmosphere that owns its generated state: it draws the active
 *  model's look and sim from Seed and Set and writes them into its own
 *  settings, in the editor and in play. Editing Seed, Set, Locks or
 *  PlanetType regenerates. The written settings can be inspected and tweaked;
 *  the next generation overwrites every drawn path, and with a profile Base,
 *  every other look setting too.
 *
 *  THE GENERATED CONFIG IS TRANSIENT, so the level saves its slot empty and
 *  the actor regenerates on its first tick after load, before it bids for the
 *  sim, and the look with it: a tweak to a drawn path lasts until the next
 *  generation, load included. Lock a path to keep the actor's value. */
UCLASS(meta = (DisplayName = "Seeded Cloud Atmosphere"))
class CLOUDATMOSPHERE_API ASeededAtmosphereActor : public APlanetAtmosphereActor
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Generation")
	int32 Seed = 0;

	/** The archetypes and looks this planet draws from, per model. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Generation")
	FAtmosphereGenerationSet Set;

	/** Paths the seed leaves alone: a bundle path keeps this actor's value, a
	 *  sim config path its archetype's. Paths as the draws name them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudAtmosphere|Generation")
	TArray<FString> Locks;

	/** A draw entry's path to hold at SweepPosition in its range while every
	 *  other entry keeps the seed's draw: for finding a range's ends. A look
	 *  path updates as the slider moves, without restarting the sim. Not saved. */
	UPROPERTY(EditAnywhere, Transient, Category = "CloudAtmosphere|Generation|Sweep")
	FString SweepPath;

	/** 0 the entry's low end, 1 its high; for Normal, the share of the cut bell. */
	UPROPERTY(EditAnywhere, Transient, Category = "CloudAtmosphere|Generation|Sweep", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SweepPosition = 0.5f;

	UPROPERTY(VisibleInstanceOnly, Transient, BlueprintReadOnly, Category = "CloudAtmosphere|Generation|Result")
	FAtmosphereGenome Genome;

	/** Every value the last generation wrote, and every path it could not. */
	UPROPERTY(VisibleInstanceOnly, Transient, BlueprintReadOnly, Category = "CloudAtmosphere|Generation|Result", meta = (TitleProperty = "Path"))
	TArray<FAtmosphereDrawRecord> Report;

	/** Generates the active model from Seed and applies it. A new sim config
	 *  for the model driving the sim restarts it from the rolled start. Also
	 *  after editing an archetype or look profile, which nothing here watches. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CloudAtmosphere|Generation")
	void Generate();

	/** A new random Seed, then Generate. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CloudAtmosphere|Generation")
	void Reroll();

	virtual void Tick(float DeltaTime) override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	/** bSim false regenerates the look alone, keeping the sim's config. */
	void GenerateParts(bool bSim);

	/** Set on construction and load, so the first tick generates. */
	bool bPendingGenerate = true;
};
