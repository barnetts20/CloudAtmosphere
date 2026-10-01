// A model's look as an asset, and the calls that apply looks and tunes at
// runtime, which the console commands wrap (AtmosphereParamDump.cpp).

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AtmosphereParams.h"
#include "AtmospherePreset.generated.h"

class APlanetAtmosphereActor;
class UFlowSimConfig;

/** One model's bundle and the sim it bids with: what a cooked build applies in
 *  place of a tune file. To make one in the editor, copy a model's row on an
 *  actor and paste it onto Model. */
UCLASS(BlueprintType)
class CLOUDATMOSPHERE_API UAtmospherePreset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Preset")
	FAtmosphereModelParams Model;

	/** The sim config the model bids with; none leaves the actor's. Shared:
	 *  runtime changes go through the actor's GetWritableSimConfig. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Preset")
	TObjectPtr<UFlowSimConfig> SimConfig = nullptr;
};

/** What ApplyTune applies of a tune. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereTuneScope
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tune")
	bool bAtmosphere = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tune")
	bool bSim = true;

	/** Assets, the sim configs' slots, debug views and start state. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tune")
	bool bPipeline = false;

	/** Raymarch, Sampling and the shadow settings: the performance tier. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tune")
	bool bQuality = false;
};

UCLASS()
class CLOUDATMOSPHERE_API UAtmosphereTuneLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** The preset's bundle onto one of the actor's models, and its sim config
	 *  into that model's slot. A new config for the model driving the sim
	 *  restarts the sim from its InitialState. */
	UFUNCTION(BlueprintCallable, Category = "CloudAtmosphere")
	static void ApplyPreset(APlanetAtmosphereActor* Actor, const UAtmospherePreset* Preset, EPlanetAtmosphereType InModel);

	/** A tune in DumpParams' layout onto one actor: the file's first
	 *  atmosphere, then its Sim section onto the actor's writable config for
	 *  the model the tune leaves active. False when the text is not a tune. */
	UFUNCTION(BlueprintCallable, Category = "CloudAtmosphere")
	static bool ApplyTune(APlanetAtmosphereActor* Actor, const FString& TuneJson, FAtmosphereTuneScope Scope);
};
