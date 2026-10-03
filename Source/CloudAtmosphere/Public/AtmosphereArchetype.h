// The assets a planet is generated from: archetypes (weather) and look
// profiles (the bundle), and the per-model sets an actor draws from.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "AtmosphereHarnessTypes.h"
#include "AtmosphereArchetype.generated.h"

class UFlowSimConfig;
class UAtmospherePreset;

/** A weather regime: a coherent sim config paired with the snapshot captured
 *  under it, and the sim settings a seed may move without breaking that
 *  pairing. The template is duplicated per planet and never edited.
 *
 *  THE SKELETON IS PINNED. The jets, the thermal shear, the layers and the
 *  perpetual storms' count and latitudes are what the snapshot's weather sits
 *  on; a different set of them is a different archetype, not a draw. */
UCLASS(BlueprintType)
class CLOUDATMOSPHERE_API UAtmosphereArchetype : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Terrestrial or GasGiant. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Archetype")
	EPlanetAtmosphereType Model = EPlanetAtmosphereType::Terrestrial;

	/** The config, its InitialState the snapshot captured under it. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Archetype")
	TObjectPtr<UFlowSimConfig> Template = nullptr;

	/** Draws onto the planet's copy of Template; paths are its members. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Archetype")
	FAtmosphereDrawSet SimDraws;

	/** How far a seed may turn the start state east, in degrees. 0 starts every
	 *  planet of the archetype from the same weather in the same place. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Archetype", meta = (ClampMin = "0.0", ClampMax = "360.0"))
	float MaxRoll = 360.0f;

	/** Look draws that replace the look profile's entry on the same path, or
	 *  its choice group of the same name, for planets of this archetype. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Archetype")
	FAtmosphereDrawSet LookDraws;

	/** Keys the archetype's share of seeds, so renaming or moving the asset
	 *  keeps them. A duplicate gets its own. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Archetype")
	FGuid ArchetypeId;

	virtual void PostInitProperties() override;
	virtual void PostDuplicate(bool bDuplicateForPIE) override;
};

/** A model's look: a base bundle and the look settings a seed draws over it. */
UCLASS(BlueprintType)
class CLOUDATMOSPHERE_API UAtmosphereLookProfile : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Look")
	EPlanetAtmosphereType Model = EPlanetAtmosphereType::Terrestrial;

	/** The bundle draws start from, its Model; its SimConfig is unread. None
	 *  starts from the actor's bundle, so undrawn settings stay the actor's
	 *  own; set one to make every setting a function of the seed. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Look")
	TObjectPtr<UAtmospherePreset> Base = nullptr;

	/** Paths are bundle members. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Look")
	FAtmosphereDrawSet Draws;

	/** Adds a draw for every Identity and Style look setting of Model at its
	 *  first-estimate range (Design/HarnessReference.md), and a palette for
	 *  the air's and the clouds' colours from Base's, or the actor class
	 *  default's: scaled for the air's thickness, mutated for hue. Paths and
	 *  palettes already present are left alone. */
	UFUNCTION(CallInEditor, Category = "Look")
	void AddStarterDraws();
};

/** An archetype a set offers, switchable per actor. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereArchetypeEntry
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Archetype")
	TObjectPtr<UAtmosphereArchetype> Archetype = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Archetype")
	bool bEnabled = true;

	/** Relative share of seeds among the enabled archetypes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Archetype", meta = (ClampMin = "0.0"))
	float Weight = 1.0f;
};

/** What a cloud model draws from. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereModelSet
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Set", meta = (TitleProperty = "Archetype"))
	TArray<FAtmosphereArchetypeEntry> Archetypes;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Set")
	TObjectPtr<UAtmosphereLookProfile> Look = nullptr;
};

/** Everything an actor draws from, per model. A planet class is one of these. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereGenerationSet
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Set")
	FAtmosphereModelSet Terrestrial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Set")
	FAtmosphereModelSet GasGiant;

	/** Air only has no sim, so a look alone. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Set")
	TObjectPtr<UAtmosphereLookProfile> AirOnly = nullptr;
};
