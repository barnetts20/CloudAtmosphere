#include "SeededAtmosphereActor.h"

#include "AtmosphereGenerator.h"

void ASeededAtmosphereActor::Tick(float DeltaTime)
{
	// Before the base tick, which bids for the sim with the slot's config.
	if (bPendingGenerate)
	{
		bPendingGenerate = false;
		Generate();
	}

	Super::Tick(DeltaTime);
}

#if WITH_EDITOR
void ASeededAtmosphereActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName Name = PropertyChangedEvent.GetMemberPropertyName();
	const bool bSweep = Name == GET_MEMBER_NAME_CHECKED(ASeededAtmosphereActor, SweepPath)
		|| Name == GET_MEMBER_NAME_CHECKED(ASeededAtmosphereActor, SweepPosition);

	// A look sweep leaves the sim alone, so it follows the slider.
	double Unused = 0.0;

	if (bSweep && AtmosphereHarness::ReadValue(FAtmosphereModelParams::StaticStruct(), &Terrestrial, SweepPath, Unused))
	{
		GenerateParts(false);
		return;
	}

	// Anything else regenerates once, on release: each generation restarts the sim.
	if (PropertyChangedEvent.ChangeType == EPropertyChangeType::Interactive)
	{
		return;
	}

	if (bSweep
		|| Name == GET_MEMBER_NAME_CHECKED(ASeededAtmosphereActor, Seed)
		|| Name == GET_MEMBER_NAME_CHECKED(ASeededAtmosphereActor, Set)
		|| Name == GET_MEMBER_NAME_CHECKED(ASeededAtmosphereActor, Locks)
		|| Name == GET_MEMBER_NAME_CHECKED(APlanetAtmosphereActor, PlanetType))
	{
		Generate();
	}
}
#endif

void ASeededAtmosphereActor::Reroll()
{
#if WITH_EDITOR
	Modify();
#endif

	Seed = FMath::RandRange(0, MAX_int32 - 1);
	Generate();
}

void ASeededAtmosphereActor::Generate()
{
	GenerateParts(true);
}

void ASeededAtmosphereActor::GenerateParts(bool bSim)
{
	// A class default or template generates nothing: only placed actors own state.
	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		return;
	}

	const EPlanetAtmosphereType Model = PlanetType;

	FAtmosphereGenerateOptions Options;
	Options.Locks = Locks;
	Options.SweepPath = SweepPath;
	Options.SweepPosition = SweepPosition;
	Options.bSim = bSim;

	FAtmosphereGeneration Generation;

	if (!FAtmosphereGenerator::Generate(Seed, Model, Set, GetModelParams(Model), Options, this, Generation))
	{
		UE_LOG(LogAtmosphereHarness, Warning,
			TEXT("'%s': Set offers %s nothing: no enabled archetype of that model with a Template, and no look profile."),
			*GetName(), *UEnum::GetDisplayValueAsText(Model).ToString());
		return;
	}

	// A look-only pass keeps the sim's lines and roll from the last full one.
	if (!bSim)
	{
		Generation.Genome.RollColumns = Genome.RollColumns;
		Generation.Genome.RollDegrees = Genome.RollDegrees;
		Generation.Report.Append(Report.FilterByPredicate(
			[](const FAtmosphereDrawRecord& Line) { return Line.Path.StartsWith(TEXT("Sim.")); }));
	}

	Genome = Generation.Genome;
	Report = MoveTemp(Generation.Report);

	if (Generation.bHasLook)
	{
		SetModelParams(Model, Generation.Look);
	}

	// A config whose outer is this actor is already its writable copy, so
	// GetWritableSimConfig hands this one out rather than copying it again.
	if (Generation.Config)
	{
#if WITH_EDITOR
		Modify();
#endif

		TObjectPtr<UFlowSimConfig>& Slot = (Model == EPlanetAtmosphereType::GasGiant)
			? Simulation.GasGiantConfig
			: Simulation.TerrestrialConfig;

		Slot = Generation.Config;
	}

	if (bSim)
	{
		UE_LOG(LogAtmosphereHarness, Log, TEXT("'%s': seed %d, archetype %s, rolled %.1f degrees, %d settings."),
			*GetName(), Seed, Genome.Archetype ? *Genome.Archetype->GetName() : TEXT("none"),
			Genome.RollDegrees, Report.Num());
	}
}
