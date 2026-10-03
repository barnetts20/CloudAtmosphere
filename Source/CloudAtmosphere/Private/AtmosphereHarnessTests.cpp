// The generator's contract: streams, distributions, paths, the archetype pick
// and reproducibility. CPU only; run as CloudAtmosphere.Harness in the
// automation tests.

#include "AtmosphereArchetype.h"
#include "AtmosphereGenerator.h"
#include "AtmospherePreset.h"
#include "FlowSimTypes.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

using namespace AtmosphereHarness;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtmosphereHarnessStreamsTest, "CloudAtmosphere.Harness.Streams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtmosphereHarnessStreamsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("A key draws the same twice"), Unit(7, EStream::Look, TEXT("Coverage.CloudCover")),
		Unit(7, EStream::Look, TEXT("Coverage.CloudCover")));
	TestNotEqual(TEXT("Seeds differ"), Unit(7, EStream::Look, TEXT("Coverage.CloudCover")),
		Unit(8, EStream::Look, TEXT("Coverage.CloudCover")));
	TestNotEqual(TEXT("Families differ"), Unit(7, EStream::Look, TEXT("A")), Unit(7, EStream::Sim, TEXT("A")));

	FAtmosphereDraw Normal;
	Normal.Distribution = EAtmosphereDrawDistribution::Normal;
	Normal.Min = 0.2f;
	Normal.Max = 0.8f;
	Normal.Mean = 0.4f;
	Normal.Sigma = 0.1f;

	FAtmosphereDraw Amount;
	Amount.Distribution = EAtmosphereDrawDistribution::LogUniform;
	Amount.Min = 8.0f;
	Amount.Max = 64.0f;

	double Previous = -1.0;
	bool bInRange = true;
	bool bMonotonic = true;

	for (int32 i = 0; i < 1000; ++i)
	{
		const double U = Unit(i, EStream::Look, TEXT("Test"));
		bInRange &= U >= 0.0 && U < 1.0;

		const double Ordered = Sample(Normal, i / 1000.0, 0.0);
		bMonotonic &= Ordered >= Previous;
		Previous = Ordered;

		const double Drawn = Sample(Normal, U, 0.0);
		const double Scaled = Sample(Amount, U, 0.0);
		bInRange &= Drawn >= 0.2 && Drawn <= 0.8 && Scaled >= 8.0 && Scaled <= 64.0;
	}

	TestTrue(TEXT("Draws stay in range"), bInRange);
	TestTrue(TEXT("A truncated normal is monotonic in its draw"), bMonotonic);
	TestTrue(TEXT("Log-uniform's midpoint is the geometric mean"), FMath::IsNearlyEqual(Sample(Amount, 0.5, 0.0), 8.0 * 2.8284271247, 1e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtmosphereHarnessPathsTest, "CloudAtmosphere.Harness.Paths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtmosphereHarnessPathsTest::RunTest(const FString& Parameters)
{
	const UScriptStruct* Look = FAtmosphereModelParams::StaticStruct();
	FAtmosphereModelParams Params;
	double Value = 0.0;

	TestTrue(TEXT("Writes a colour channel"), WriteValue(Look, &Params, TEXT("Material.CloudScatter.G"), 0.5));
	TestEqual(TEXT("Reads it back"), (double)Params.Material.CloudScatter.G, 0.5);
	TestTrue(TEXT("Writes an int"), WriteValue(Look, &Params, TEXT("Coverage.CloudLayer"), 1.4));
	TestEqual(TEXT("Rounded"), Params.Coverage.CloudLayer, 1);
	TestFalse(TEXT("A missing member fails"), ReadValue(Look, &Params, TEXT("Coverage.NotASetting"), Value));
	TestFalse(TEXT("A group is not a number"), ReadValue(Look, &Params, TEXT("Coverage"), Value));

	UFlowSimConfig* Config = NewObject<UFlowSimConfig>();
	Config->PerpetualStorms.AddDefaulted();

	TestTrue(TEXT("Writes an array element's member"), WriteValue(UFlowSimConfig::StaticClass(), Config, TEXT("PerpetualStorms[0].Radius"), 1.25));
	TestEqual(TEXT("Into the element"), (double)Config->PerpetualStorms[0].Radius, 1.25);
	TestFalse(TEXT("An index past the end fails"), WriteValue(UFlowSimConfig::StaticClass(), Config, TEXT("PerpetualStorms[3].Radius"), 1.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtmosphereHarnessPickTest, "CloudAtmosphere.Harness.Pick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtmosphereHarnessPickTest::RunTest(const FString& Parameters)
{
	FAtmosphereModelSet Set;
	UFlowSimConfig* Template = NewObject<UFlowSimConfig>();

	for (int32 i = 0; i < 4; ++i)
	{
		UAtmosphereArchetype* Archetype = NewObject<UAtmosphereArchetype>();
		Archetype->Template = Template;
		Set.Archetypes.AddDefaulted_GetRef().Archetype = Archetype;
	}

	constexpr int32 Seeds = 2000;
	TArray<UAtmosphereArchetype*> Before;
	TMap<UAtmosphereArchetype*, int32> Shares;

	for (int32 Seed = 0; Seed < Seeds; ++Seed)
	{
		Before.Add(FAtmosphereGenerator::PickArchetype(Seed, EPlanetAtmosphereType::Terrestrial, Set));
		++Shares.FindOrAdd(Before.Last());
	}

	for (const TPair<UAtmosphereArchetype*, int32>& Share : Shares)
	{
		TestTrue(TEXT("Even weights share seeds about evenly"), FMath::Abs(Share.Value - Seeds / 4) < Seeds / 10);
	}

	UAtmosphereArchetype* Off = Set.Archetypes[2].Archetype;
	Set.Archetypes[2].bEnabled = false;

	bool bOnlyItsSeedsMoved = true;

	for (int32 Seed = 0; Seed < Seeds; ++Seed)
	{
		UAtmosphereArchetype* After = FAtmosphereGenerator::PickArchetype(Seed, EPlanetAtmosphereType::Terrestrial, Set);
		bOnlyItsSeedsMoved &= (Before[Seed] == Off) ? (After != Off) : (After == Before[Seed]);
	}

	TestTrue(TEXT("Switching an archetype off moves only its seeds"), bOnlyItsSeedsMoved);
	TestNull(TEXT("Another model's archetypes are not picked"),
		FAtmosphereGenerator::PickArchetype(0, EPlanetAtmosphereType::GasGiant, Set));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtmosphereHarnessReproduceTest, "CloudAtmosphere.Harness.Reproduce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtmosphereHarnessReproduceTest::RunTest(const FString& Parameters)
{
	UAtmosphereLookProfile* Profile = NewObject<UAtmosphereLookProfile>();
	Profile->Base = NewObject<UAtmospherePreset>();

	const auto Add = [Profile](const TCHAR* Path, float Min, float Max)
		{
			FAtmosphereDraw& Entry = Profile->Draws.Draws.AddDefaulted_GetRef();
			Entry.Path = Path;
			Entry.Min = Min;
			Entry.Max = Max;
		};

	Add(TEXT("Coverage.CloudCover"), 0.3f, 0.8f);
	Add(TEXT("Shape.CloudBase"), 0.0f, 0.2f);

	FAtmosphereGenerationSet Set;
	Set.Terrestrial.Look = Profile;

	const FAtmosphereModelParams Current;
	const FAtmosphereGenerateOptions Plain;
	FAtmosphereGeneration First, Second, Grown, Locked;

	FAtmosphereGenerator::Generate(42, EPlanetAtmosphereType::Terrestrial, Set, Current, Plain, nullptr, First);
	FAtmosphereGenerator::Generate(42, EPlanetAtmosphereType::Terrestrial, Set, Current, Plain, nullptr, Second);

	TestTrue(TEXT("One seed, one planet"),
		FAtmosphereModelParams::StaticStruct()->CompareScriptStruct(&First.Look, &Second.Look, PPF_None));

	FAtmosphereGeneration After;
	FAtmosphereModelParams Previous = First.Look;
	Previous.Shape.CloudThickness = 0.123f;
	FAtmosphereGenerator::Generate(42, EPlanetAtmosphereType::Terrestrial, Set, Previous, Plain, nullptr, After);

	TestTrue(TEXT("A planet does not depend on the one before it"),
		FAtmosphereModelParams::StaticStruct()->CompareScriptStruct(&First.Look, &After.Look, PPF_None));

	Add(TEXT("Shape.CloudThickness"), 0.5f, 0.85f);
	FAtmosphereGenerator::Generate(42, EPlanetAtmosphereType::Terrestrial, Set, Current, Plain, nullptr, Grown);

	TestEqual(TEXT("A new entry leaves the others' values"), Grown.Look.Coverage.CloudCover, First.Look.Coverage.CloudCover);
	TestEqual(TEXT("A new entry leaves the others' values"), Grown.Look.Shape.CloudBase, First.Look.Shape.CloudBase);

	FAtmosphereGenerateOptions Locking;
	Locking.Locks.Add(TEXT("Coverage.CloudCover"));
	FAtmosphereGenerator::Generate(42, EPlanetAtmosphereType::Terrestrial, Set, Current, Locking, nullptr, Locked);
	TestEqual(TEXT("A lock keeps the actor's value"), Locked.Look.Coverage.CloudCover, Current.Coverage.CloudCover);

	FAtmosphereGenerateOptions Sweeping;
	Sweeping.SweepPath = TEXT("Coverage.CloudCover");
	FAtmosphereGeneration Low, High;

	Sweeping.SweepPosition = 0.0f;
	FAtmosphereGenerator::Generate(42, EPlanetAtmosphereType::Terrestrial, Set, Current, Sweeping, nullptr, Low);
	Sweeping.SweepPosition = 1.0f;
	FAtmosphereGenerator::Generate(42, EPlanetAtmosphereType::Terrestrial, Set, Current, Sweeping, nullptr, High);

	TestEqual(TEXT("A sweep at 0 is the range's low end"), Low.Look.Coverage.CloudCover, 0.3f);
	TestEqual(TEXT("A sweep at 1 is its high end"), High.Look.Coverage.CloudCover, 0.8f);
	TestEqual(TEXT("A sweep leaves the other draws"), High.Look.Shape.CloudBase, First.Look.Shape.CloudBase);

	// Without a Base the look starts from the actor's bundle.
	Profile->Base = nullptr;

	FAtmosphereModelParams Tweaked;
	Tweaked.Type.TypeBias = 0.123f;
	Tweaked.Coverage.CloudCover = 0.05f;

	FAtmosphereGeneration Own;
	FAtmosphereGenerator::Generate(42, EPlanetAtmosphereType::Terrestrial, Set, Tweaked, Plain, nullptr, Own);

	TestTrue(TEXT("Without a Base the look is still drawn"), Own.bHasLook);
	TestEqual(TEXT("An undrawn setting keeps the actor's value"), Own.Look.Type.TypeBias, 0.123f);
	TestEqual(TEXT("A drawn setting is overwritten by the seed's value"), Own.Look.Coverage.CloudCover, Grown.Look.Coverage.CloudCover);
	return true;
}

#endif
