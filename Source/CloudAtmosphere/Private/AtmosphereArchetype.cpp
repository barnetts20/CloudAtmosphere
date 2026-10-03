#include "AtmosphereArchetype.h"

void UAtmosphereArchetype::PostInitProperties()
{
	Super::PostInitProperties();

	// A loaded asset's saved id overwrites this one.
	if (!HasAnyFlags(RF_ClassDefaultObject) && !ArchetypeId.IsValid())
	{
		ArchetypeId = FGuid::NewGuid();
	}
}

void UAtmosphereArchetype::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);

	// PITFALL: two archetypes with one id tie on every seed.
	if (!bDuplicateForPIE)
	{
		ArchetypeId = FGuid::NewGuid();
	}
}
