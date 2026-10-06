// The generator's contract: streams, distributions, paths, the archetype pick,
// reproducibility and the sim's seed. CPU only; run as CloudAtmosphere.Harness
// in the automation tests.

#include "AtmosphereArchetype.h"
#include "AtmosphereGenerator.h"
#include "AtmospherePreset.h"
#include "FlowSimTypes.h"
#include "FlowSnapshot.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtmosphereHarnessBaselineTest, "CloudAtmosphere.Harness.Baseline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtmosphereHarnessBaselineTest::RunTest(const FString& Parameters)
{
	const UScriptStruct* Look = FAtmosphereModelParams::StaticStruct();
	TArray<FString> Numbers, Colours, GasNumbers, GasColours, AirNumbers, AirColours;

	LookSettings(EPlanetAtmosphereType::Terrestrial, Numbers, Colours);
	LookSettings(EPlanetAtmosphereType::GasGiant, GasNumbers, GasColours);
	LookSettings(EPlanetAtmosphereType::AirOnly, AirNumbers, AirColours);

	TestTrue(TEXT("The air's profile is drawn"), Numbers.Contains(TEXT("Air.MieScaleHeight")));
	TestTrue(TEXT("A colour is one setting"), Colours.Contains(TEXT("Material.CloudScatter")));
	TestFalse(TEXT("Not its channels"), Numbers.Contains(TEXT("Material.CloudScatter.G")));
	TestFalse(TEXT("The quality tier is not drawn"), Numbers.Contains(TEXT("MultipleScattering.OctaveCount")));
	TestFalse(TEXT("Nor the cascade extents"), Numbers.Contains(TEXT("SurfaceShadow.CascadeRadii.X")));
	TestFalse(TEXT("The slab has no deep deck"), Numbers.Contains(TEXT("Deep.DeepFill")) || Colours.Contains(TEXT("Deep.Scatter")));
	TestTrue(TEXT("The gas giant has"), GasNumbers.Contains(TEXT("Deep.DeepFill")) && GasColours.Contains(TEXT("Deep.Scatter")));
	TestFalse(TEXT("Air only draws no cloud"), AirNumbers.Contains(TEXT("Shape.CloudBase")) || AirColours.Contains(TEXT("Material.CloudScatter")));

	// Pinned to a tune, every seed reproduces it.
	FAtmosphereModelParams Tune;
	Tune.Shape.CloudThickness = 0.37f;
	Tune.Material.CloudScatter = FLinearColor(0.2f, 0.4f, 0.6f, 1.0f);
	Tune.Deep.DeepFill = 0.29f;

	UAtmosphereLookProfile* Profile = NewObject<UAtmosphereLookProfile>();
	bool bAllRead = true;

	for (const FString& Path : GasNumbers)
	{
		double Value = 0.0;
		bAllRead &= ReadValue(Look, &Tune, Path, Value);

		FAtmosphereDraw& Entry = Profile->Draws.Draws.AddDefaulted_GetRef();
		Entry.Path = Path;
		Entry.Min = (float)Value;
		Entry.Max = (float)Value;
	}

	for (const FString& Path : GasColours)
	{
		FLinearColor Colour;
		bAllRead &= ReadColour(Look, &Tune, Path, Colour);

		FAtmosphereChoiceGroup& Palette = Profile->Draws.Choices.AddDefaulted_GetRef();
		Palette.Name = FName(*Path);
		FAtmospherePaletteColour& Entry = Palette.Colours.AddDefaulted_GetRef();
		Entry.Path = Path;
		Entry.Options.Add(Colour);
	}

	TestTrue(TEXT("Every look setting resolves"), bAllRead);

	FAtmosphereGenerationSet Set;
	Set.GasGiant.Look = Profile;

	bool bReproduced = true;

	for (int32 Seed : { 1, 2, 3 })
	{
		FAtmosphereGeneration Out;
		FAtmosphereGenerator::Generate(Seed, EPlanetAtmosphereType::GasGiant, Set, FAtmosphereModelParams(), FAtmosphereGenerateOptions(), nullptr, Out);

		bReproduced &= Out.Look.Material.CloudScatter == Tune.Material.CloudScatter;

		for (const FString& Path : GasNumbers)
		{
			double Want = 0.0, Got = 0.0;
			bReproduced &= ReadValue(Look, &Tune, Path, Want) && ReadValue(Look, &Out.Look, Path, Got) && Want == Got;
		}
	}

	TestTrue(TEXT("A pinned profile reproduces its tune on every seed"), bReproduced);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtmosphereHarnessPaletteTest, "CloudAtmosphere.Harness.Palette",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtmosphereHarnessPaletteTest::RunTest(const FString& Parameters)
{
	const FLinearColor Red(0.8f, 0.2f, 0.1f, 2.0f);
	const FLinearColor Blue(0.1f, 0.3f, 0.9f, 2.0f);

	UAtmosphereLookProfile* Profile = NewObject<UAtmosphereLookProfile>();
	FAtmosphereChoiceGroup& Palette = Profile->Draws.Choices.AddDefaulted_GetRef();
	Palette.Name = TEXT("Storm");
	FAtmospherePaletteColour& Storm = Palette.Colours.AddDefaulted_GetRef();
	Storm.Path = TEXT("Material.StormExtinction");
	Storm.Options = { Red, Blue };
	Palette.ScaleMin = 0.5f;
	Palette.ScaleMax = 2.0f;
	Palette.Mutation = 0.1f;

	FAtmosphereGenerationSet Set;
	Set.Terrestrial.Look = Profile;

	bool bBounded = true;
	bool bAlphaKept = true;
	int32 Reds = 0;

	for (int32 Seed = 0; Seed < 200; ++Seed)
	{
		FAtmosphereGeneration Out;
		FAtmosphereGenerator::Generate(Seed, EPlanetAtmosphereType::Terrestrial, Set, FAtmosphereModelParams(), FAtmosphereGenerateOptions(), nullptr, Out);

		const FLinearColor Got = Out.Look.Material.StormExtinction;
		const bool bRed = Got.R > Got.B;
		const FLinearColor& From = bRed ? Red : Blue;
		Reds += bRed ? 1 : 0;

		// Each channel within scale and mutation of the picked option.
		for (int32 Channel = 0; Channel < 3; ++Channel)
		{
			const float Share = Got.Component(Channel) / From.Component(Channel);
			bBounded &= Share >= 0.5f * 0.9f - 1e-4f && Share <= 2.0f * 1.1f + 1e-4f;
		}

		bAlphaKept &= Got.A == 2.0f;
	}

	TestTrue(TEXT("Channels stay within scale and mutation"), bBounded);
	TestTrue(TEXT("Alpha is the option's"), bAlphaKept);
	TestTrue(TEXT("Both options are picked"), Reds > 50 && Reds < 150);

	FAtmosphereGenerateOptions Sweeping;
	Sweeping.SweepPath = TEXT("Storm");
	Sweeping.SweepPosition = 1.0f;

	FAtmosphereGeneration Swept;
	FAtmosphereGenerator::Generate(7, EPlanetAtmosphereType::Terrestrial, Set, FAtmosphereModelParams(), Sweeping, nullptr, Swept);
	TestTrue(TEXT("A swept palette shows its option as authored"), Swept.Look.Material.StormExtinction == Blue);

	FAtmosphereDraw& Amount = Profile->Draws.Draws.AddDefaulted_GetRef();
	Amount.Path = TEXT("Material.StormExtinction.A");
	Amount.Min = 1.5f;
	Amount.Max = 1.5f;

	FAtmosphereGeneration Refined;
	FAtmosphereGenerator::Generate(7, EPlanetAtmosphereType::Terrestrial, Set, FAtmosphereModelParams(), FAtmosphereGenerateOptions(), nullptr, Refined);
	TestEqual(TEXT("An entry refines a palette's channel"), Refined.Look.Material.StormExtinction.A, 1.5f);
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAtmosphereHarnessSimSeedTest, "CloudAtmosphere.Harness.SimSeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAtmosphereHarnessSimSeedTest::RunTest(const FString& Parameters)
{
	UFlowSimConfig* Template = NewObject<UFlowSimConfig>();
	TestEqual(TEXT("Seed 0 is the unseeded sequence"), Template->GetSeedSalt(), 0u);

	TSet<uint32> Salts;

	for (int32 Seed = 1; Seed <= 2000; ++Seed)
	{
		Template->Seed = Seed;
		Salts.Add(Template->GetSeedSalt());
	}

	TestEqual(TEXT("Every seed salts differently"), Salts.Num(), 2000);
	TestFalse(TEXT("No seed salts as 0"), Salts.Contains(0u));

	const FFlowSimRange Swapped = FFlowSimRange(0.8f, 0.2f).Clamped(0.0f, 1.0f);
	TestEqual(TEXT("A Max below Min is held to Min"), Swapped.Max, 0.8f);
	TestEqual(TEXT("A range's ends are held"), FFlowSimRange(-1.0f, 2.0f).Clamped(0.0f, 1.0f).Max, 1.0f);
	TestTrue(TEXT("A draw reaches a range's end"),
		WriteValue(UFlowSimConfig::StaticClass(), Template, TEXT("StormCellRadius.Max"), 1.5));
	TestEqual(TEXT("Into the range"), Template->StormCellRadius.Max, 1.5f);
	TestTrue(TEXT("A draw on the whole range"), WriteValue(UFlowSimConfig::StaticClass(), Template, TEXT("StormCellWind"), 0.4));
	TestTrue(TEXT("Sets both ends"), Template->StormCellWind.Min == 0.4f && Template->StormCellWind.Max == 0.4f);

	// A snapshot of 8 x 4 x 1 cells: every layout restores, the seeds ride a roll as they are.
	constexpr int32 Cells = 32;
	UFlowSnapshot* Snapshot = NewObject<UFlowSnapshot>();
	Snapshot->Grid = FIntVector(8, 4, 1);

	Snapshot->State.SetNumZeroed(Cells * UFlowSnapshot::LegacyFloatsPerCell + UFlowSnapshot::LegacyTrailingFloats);
	TestTrue(TEXT("The oldest layout restores"), Snapshot->IsValidFor(Snapshot->Grid));
	Snapshot->State.SetNumZeroed(Cells * UFlowSnapshot::FloatsPerCell + UFlowSnapshot::LegacyTrailingFloats);
	TestTrue(TEXT("The layout without seeds restores"), Snapshot->IsValidFor(Snapshot->Grid));
	Snapshot->State.SetNumZeroed(Cells * UFlowSnapshot::LegacyFloatsPerCell + UFlowSnapshot::TrailingFloats);
	TestFalse(TEXT("Seeds without the last planes are no layout"), Snapshot->IsValidFor(Snapshot->Grid));
	Snapshot->State.SetNumZeroed(Cells * UFlowSnapshot::FloatsPerCell + UFlowSnapshot::TrailingFloats);
	TestTrue(TEXT("The current layout restores"), Snapshot->IsValidFor(Snapshot->Grid));

	const int32 Seeds = Cells * UFlowSnapshot::FloatsPerCell + UFlowSnapshot::LegacyTrailingFloats;
	Snapshot->State[Seeds] = 12345.0f;
	Snapshot->State[Seeds + UFlowSnapshot::SeedFloatsPerSlot] = -1.0f;

	const UFlowSnapshot* Rolled = Snapshot->MakeRolled(nullptr, 3);
	TestTrue(TEXT("A roll keeps the seeds"), Rolled && Rolled->State[Seeds] == 12345.0f
		&& Rolled->State[Seeds + UFlowSnapshot::SeedFloatsPerSlot] == -1.0f);

	// Each planet runs its own seed, one seed one sim seed, and a lock keeps the template's.
	Template->Seed = 77;
	Template->InitialState = Snapshot;

	UAtmosphereArchetype* Archetype = NewObject<UAtmosphereArchetype>();
	Archetype->Template = Template;

	FAtmosphereGenerationSet Set;
	Set.Terrestrial.Archetypes.AddDefaulted_GetRef().Archetype = Archetype;

	FAtmosphereGenerateOptions Locking;
	Locking.Locks.Add(TEXT("Seed"));

	FAtmosphereGeneration First, Again, Other, Locked;
	const FAtmosphereModelParams Current;
	FAtmosphereGenerator::Generate(1, EPlanetAtmosphereType::Terrestrial, Set, Current, FAtmosphereGenerateOptions(), nullptr, First);
	FAtmosphereGenerator::Generate(1, EPlanetAtmosphereType::Terrestrial, Set, Current, FAtmosphereGenerateOptions(), nullptr, Again);
	FAtmosphereGenerator::Generate(2, EPlanetAtmosphereType::Terrestrial, Set, Current, FAtmosphereGenerateOptions(), nullptr, Other);
	FAtmosphereGenerator::Generate(1, EPlanetAtmosphereType::Terrestrial, Set, Current, Locking, nullptr, Locked);

	if (!TestTrue(TEXT("Every generation has a config"), First.Config && Again.Config && Other.Config && Locked.Config))
	{
		return false;
	}

	TestTrue(TEXT("A planet's sim is seeded"), First.Config->Seed != 0 && First.Config->Seed != Template->Seed);
	TestEqual(TEXT("One seed, one sim seed"), Again.Config->Seed, First.Config->Seed);
	TestNotEqual(TEXT("Planets differ"), Other.Config->Seed, First.Config->Seed);
	TestEqual(TEXT("A lock keeps the template's"), Locked.Config->Seed, 77);
	return true;
}

#endif
