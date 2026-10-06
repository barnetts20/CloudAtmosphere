// A seed into a planet: the archetype pick, the look and sim draws, the locks
// and the start state's roll. Pure: it writes nothing but its output.

#pragma once

#include "CoreMinimal.h"
#include "AtmosphereParams.h"
#include "AtmosphereHarnessTypes.h"
#include "AtmosphereGenerator.generated.h"

class UFlowSimConfig;
struct FAtmosphereModelSet;
struct FAtmosphereGenerationSet;

namespace AtmosphereHarness
{
	/** Raised when the same seed and assets would generate a different planet:
	 *  distribution maths, stream keys or the order draws apply in. */
	constexpr int32 GeneratorVersion = 3;

	/** A draw's stream family, so one key in two families draws independently. */
	enum class EStream : uint64
	{
		Look = 1,
		Sim = 2,
		Pick = 3,
		Roll = 4,
		Link = 5,
	};

	/** A uniform number in [0, 1), counter based: a function of the seed, the
	 *  family, the key and the sub-draw alone, so adding or reordering draws
	 *  leaves every other key's value as it was. */
	CLOUDATMOSPHERE_API double Unit(int32 Seed, EStream Stream, FStringView Key, uint32 Sub = 0);

	/** Entry's value for U in [0, 1). Base is the template's value, which
	 *  Jitter moves and Hold keeps. Monotonic in U for every distribution but
	 *  Choice, so linked entries move together. */
	CLOUDATMOSPHERE_API double Sample(const FAtmosphereDraw& Entry, double U, double Base);

	/** A numeric, enum or bool setting at Path inside Container, an instance of
	 *  Type. False when Path names no such setting. */
	CLOUDATMOSPHERE_API bool ReadValue(const UStruct* Type, const void* Container, FStringView Path, double& Out);
	CLOUDATMOSPHERE_API bool WriteValue(const UStruct* Type, void* Container, FStringView Path, double Value);

	/** The colour at Path, a FLinearColor member. */
	CLOUDATMOSPHERE_API bool ReadColour(const UStruct* Type, const void* Container, FStringView Path, FLinearColor& Out);

	/** Every look setting Model reads that a draw can set: the bundle's
	 *  editable numbers and its colours, less the quality tier's and the
	 *  groups Model does not draw (Deep without a deep deck; for air only,
	 *  everything but Planet, Air and Ambient). */
	CLOUDATMOSPHERE_API void LookSettings(EPlanetAtmosphereType Model, TArray<FString>& OutNumbers, TArray<FString>& OutColours);
}

/** How a generation runs, beyond the seed and the set. */
struct FAtmosphereGenerateOptions
{
	/** Paths the seed leaves alone: a look path keeps CurrentLook's value, a
	 *  sim path the template's. */
	TArray<FString> Locks;

	/** A draw entry's path to place at SweepPosition in its range, 0 its low
	 *  end and 1 its high, in place of the seed's draw. Empty draws every
	 *  entry from the seed. */
	FString SweepPath;
	float SweepPosition = 0.5f;

	/** False generates the look alone: no config, no pick of the start. */
	bool bSim = true;
};

/** One generation's output. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereGeneration
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Generation")
	FAtmosphereGenome Genome;

	/** The model's bundle, when bHasLook. */
	UPROPERTY(BlueprintReadOnly, Category = "Generation")
	FAtmosphereModelParams Look;

	/** The set has a look profile for the model, so Look was generated. */
	UPROPERTY(BlueprintReadOnly, Category = "Generation")
	bool bHasLook = false;

	/** A transient copy of the archetype's template with its own Seed and
	 *  draws, its InitialState the rolled snapshot; null for air only or no
	 *  archetype. */
	UPROPERTY(BlueprintReadOnly, Category = "Generation")
	TObjectPtr<UFlowSimConfig> Config = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Generation")
	TArray<FAtmosphereDrawRecord> Report;
};

class CLOUDATMOSPHERE_API FAtmosphereGenerator
{
public:
	/** Model's planet for Seed from Set. The look starts from the look
	 *  profile's Base, or CurrentLook without one, and the draws overwrite
	 *  their paths. Outer owns the config and its snapshot. False when the
	 *  set offers the model nothing. */
	static bool Generate(int32 Seed, EPlanetAtmosphereType Model, const FAtmosphereGenerationSet& Set,
		const FAtmosphereModelParams& CurrentLook, const FAtmosphereGenerateOptions& Options, UObject* Outer,
		FAtmosphereGeneration& Out);

	/** The enabled archetype of Model that Seed picks, by weighted rendezvous
	 *  hashing on each archetype's id: switching one archetype off moves only
	 *  the seeds that picked it. Null when none is enabled. */
	static UAtmosphereArchetype* PickArchetype(int32 Seed, EPlanetAtmosphereType Model, const FAtmosphereModelSet& Set);

private:
	/** The config: Archetype's template duplicated into Outer, its Seed from
	 *  the planet's unless locked, its sim draws, and its start state rolled. */
	static void GenerateSim(int32 Seed, const UAtmosphereArchetype& Archetype, const FAtmosphereGenerateOptions& Options,
		UObject* Outer, FAtmosphereGeneration& Out);
};
