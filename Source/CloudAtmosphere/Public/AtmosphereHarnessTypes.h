// What a seed draws and what a generation reports: the procedural harness
// that turns a seed into a planet. See Design/HarnessPlan.md.

#pragma once

#include "CoreMinimal.h"
#include "AtmosphereParams.h"
#include "AtmosphereHarnessTypes.generated.h"

class UAtmosphereArchetype;

CLOUDATMOSPHERE_API DECLARE_LOG_CATEGORY_EXTERN(LogAtmosphereHarness, Log, All);

/** How a draw entry turns its random number into a value. */
UENUM(BlueprintType)
enum class EAtmosphereDrawDistribution : uint8
{
	/** Even between Min and Max. */
	Uniform,

	/** A bell about Mean with spread Sigma, cut to [Min, Max]. */
	Normal,

	/** Even in ratio between Min and Max, both above zero: as likely to double
	 *  as to halve. For rates, lifetimes and amounts. */
	LogUniform,

	/** The template's value moved by up to Amount either way. */
	Jitter,

	/** One of Values, each as likely. */
	Choice,

	/** The template's value: an entry that records it is deliberately not drawn. */
	Hold,
};

/** One setting a seed draws. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereDraw
{
	GENERATED_BODY()

	/** The setting, as a path of member names from the bundle or the sim
	 *  config: Coverage.CloudCover, Material.CloudScatter.R, StormAmount,
	 *  PerpetualStorms[0].Radius. The entry's random stream is keyed by it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Draw")
	FString Path;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Draw")
	EAtmosphereDrawDistribution Distribution = EAtmosphereDrawDistribution::Uniform;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Draw", meta = (EditCondition = "Distribution == EAtmosphereDrawDistribution::Uniform || Distribution == EAtmosphereDrawDistribution::Normal || Distribution == EAtmosphereDrawDistribution::LogUniform", EditConditionHides))
	float Min = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Draw", meta = (EditCondition = "Distribution == EAtmosphereDrawDistribution::Uniform || Distribution == EAtmosphereDrawDistribution::Normal || Distribution == EAtmosphereDrawDistribution::LogUniform", EditConditionHides))
	float Max = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Draw", meta = (EditCondition = "Distribution == EAtmosphereDrawDistribution::Normal", EditConditionHides))
	float Mean = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Draw", meta = (EditCondition = "Distribution == EAtmosphereDrawDistribution::Normal", EditConditionHides, ClampMin = "0.0"))
	float Sigma = 0.15f;

	/** Largest move from the template's value, either way. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Draw", meta = (EditCondition = "Distribution == EAtmosphereDrawDistribution::Jitter", EditConditionHides))
	float Amount = 0.1f;

	/** Amount as a share of the template's value rather than in its units. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Draw", meta = (EditCondition = "Distribution == EAtmosphereDrawDistribution::Jitter", EditConditionHides))
	bool bRelative = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Draw", meta = (EditCondition = "Distribution == EAtmosphereDrawDistribution::Choice", EditConditionHides))
	TArray<float> Values;

	/** Entries sharing a link read one draw instead of their own, each through
	 *  its own distribution, so they rise and fall together: a storm amount
	 *  across StormAmount, StormCellSpawnRate and MaxStormCells. Links are
	 *  shared across the look and the sim. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Draw")
	FName Link;

	/** Reads the draw as one less it, so the entry falls as its link rises. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Draw")
	bool bInvert = false;
};

/** One option of a choice group: a value per path, in the group's order. A
 *  colour or vector path takes the value's channels in its own member order
 *  (RGBA, XYZW); a scalar path takes R. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereChoiceOption
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Choice")
	TArray<FLinearColor> Values;
};

/** Several settings set together from one pick, so they always agree: a
 *  gas giant's cloud, storm and deep scatter, say. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereChoiceGroup
{
	GENERATED_BODY()

	/** Keys the group's stream, and what an archetype's group of the same name
	 *  replaces. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Choice")
	FName Name;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Choice")
	TArray<FString> Paths;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Choice")
	TArray<FAtmosphereChoiceOption> Options;
};

/** The draws onto one root, the bundle or the sim config. Entries apply in
 *  order, then the choice groups, so a group wins a path both set. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereDrawSet
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Draws", meta = (TitleProperty = "Path"))
	TArray<FAtmosphereDraw> Draws;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Draws", meta = (TitleProperty = "Name"))
	TArray<FAtmosphereChoiceGroup> Choices;
};

/** What a generated planet is: enough, with the assets, to generate it again. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereGenome
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Genome")
	int32 Seed = 0;

	/** AtmosphereHarness::GeneratorVersion when generated. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Genome")
	int32 GeneratorVersion = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Genome")
	EPlanetAtmosphereType Model = EPlanetAtmosphereType::Terrestrial;

	/** None for air only, or when no archetype was enabled. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Genome")
	TObjectPtr<UAtmosphereArchetype> Archetype = nullptr;

	/** The start state's turn east, in grid columns and degrees. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Genome")
	int32 RollColumns = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Genome")
	float RollDegrees = 0.0f;
};

UENUM(BlueprintType)
enum class EAtmosphereDrawSource : uint8
{
	Draw,
	Choice,

	/** A locked path: the actor's value for the look, the archetype's for the sim. */
	Lock,

	/** A path that resolves to no setting; nothing was written. */
	Missing,
};

/** One line of a generation's report. */
USTRUCT(BlueprintType)
struct CLOUDATMOSPHERE_API FAtmosphereDrawRecord
{
	GENERATED_BODY()

	/** The path, Sim. before a sim config path. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Report")
	FString Path;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Report")
	FString Value;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Report")
	EAtmosphereDrawSource Source = EAtmosphereDrawSource::Draw;
};
