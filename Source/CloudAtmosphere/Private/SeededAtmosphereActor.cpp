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

	// A drag regenerates once, on release: each generation restarts the sim.
	if (PropertyChangedEvent.ChangeType == EPropertyChangeType::Interactive)
	{
		return;
	}

	const FName Name = PropertyChangedEvent.GetMemberPropertyName();

	if (Name == GET_MEMBER_NAME_CHECKED(ASeededAtmosphereActor, Seed)
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
	// A class default or template generates nothing: only placed actors own state.
	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
	{
		return;
	}

	const EPlanetAtmosphereType Model = PlanetType;

	FAtmosphereGeneration Generation;

	if (!FAtmosphereGenerator::Generate(Seed, Model, Set, GetModelParams(Model), Locks, this, Generation))
	{
		UE_LOG(LogAtmosphereHarness, Warning, TEXT("'%s': the set offers model %d nothing to generate."), *GetName(), (int32)Model);
		return;
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

	UE_LOG(LogAtmosphereHarness, Log, TEXT("'%s': seed %d, archetype %s, rolled %.1f degrees, %d settings."),
		*GetName(), Seed, Genome.Archetype ? *Genome.Archetype->GetName() : TEXT("none"),
		Genome.RollDegrees, Report.Num());
}
