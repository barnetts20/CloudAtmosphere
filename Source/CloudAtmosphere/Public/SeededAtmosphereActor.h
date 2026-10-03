// An atmosphere generated from a seed.

#pragma once

#include "CoreMinimal.h"
#include "PlanetAtmosphereActor.h"
#include "AtmosphereArchetype.h"
#include "AtmosphereHarnessTypes.h"
#include "SeededAtmosphereActor.generated.h"

/** A planet atmosphere that owns its generated state: it draws the active
 *  model's look and sim from Seed and Set and applies them to itself, in the
 *  editor and in play. Editing Seed, Set, Locks or PlanetType regenerates.
 *
 *  THE GENERATED CONFIG IS TRANSIENT, so the level saves its slot empty and
 *  the actor regenerates on its first tick after load, before it bids for the
 *  sim. The look bundle is saved as generated; a locked path keeps it. */
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
	/** Set on construction and load, so the first tick generates. */
	bool bPendingGenerate = true;
};
