#include "AtmosphereArchetype.h"

#include "AtmosphereGenerator.h"
#include "AtmospherePreset.h"
#include "PlanetAtmosphereActor.h"

namespace
{
	/** Which models a starter draw is for. Air only takes the terrestrial range. */
	enum EStarterModels : uint8
	{
		ForTerrestrial = 1,
		ForGasGiant = 2,
		ForAirOnly = 4,
		ForClouds = ForTerrestrial | ForGasGiant,
		ForAll = ForClouds | ForAirOnly,
	};

	/** A first-estimate range from Design/HarnessReference.md. bOfBase reads
	 *  the range as multiples of Base's value, or the class default's. */
	struct FStarterDraw
	{
		const TCHAR* Path;
		EAtmosphereDrawDistribution Distribution;
		float Min, Max;
		float GasMin, GasMax;
		uint8 Models;
		const TCHAR* Link = nullptr;
		bool bOfBase = false;
	};

	// Unity builds merge anonymous namespaces across files: keep these names distinct.
	constexpr EAtmosphereDrawDistribution StarterEven = EAtmosphereDrawDistribution::Uniform;
	constexpr EAtmosphereDrawDistribution StarterLog = EAtmosphereDrawDistribution::LogUniform;

	const FStarterDraw StarterDraws[] =
	{
		{ TEXT("Planet.HeightScale"), StarterEven, 0.05f, 0.2f, 0.05f, 0.2f, ForAll },
		{ TEXT("Planet.SpinRatio"), StarterLog, 0.2f, 1.0f, 0.01f, 0.1f, ForClouds },

		{ TEXT("Air.RayleighDepth.R"), StarterLog, 0.3f, 3.0f, 0.3f, 3.0f, ForAll, TEXT("AirDepth"), true },
		{ TEXT("Air.RayleighDepth.G"), StarterLog, 0.3f, 3.0f, 0.3f, 3.0f, ForAll, TEXT("AirDepth"), true },
		{ TEXT("Air.RayleighDepth.B"), StarterLog, 0.3f, 3.0f, 0.3f, 3.0f, ForAll, TEXT("AirDepth"), true },
		{ TEXT("Air.RayleighScaleHeight"), StarterEven, 0.1f, 0.25f, 0.1f, 0.25f, ForAll },
		{ TEXT("Air.MieDepth.R"), StarterLog, 0.3f, 3.0f, 0.3f, 3.0f, ForAll, TEXT("Haze"), true },
		{ TEXT("Air.MieDepth.G"), StarterLog, 0.3f, 3.0f, 0.3f, 3.0f, ForAll, TEXT("Haze"), true },
		{ TEXT("Air.MieDepth.B"), StarterLog, 0.3f, 3.0f, 0.3f, 3.0f, ForAll, TEXT("Haze"), true },
		{ TEXT("Air.MieScaleHeight"), StarterEven, 0.05f, 0.2f, 0.05f, 0.2f, ForAll },
		{ TEXT("Air.MieG"), StarterEven, 0.75f, 0.95f, 0.75f, 0.95f, ForAll },
		{ TEXT("Air.AbsorptionDepth.R"), StarterEven, 0.0f, 2.0f, 0.0f, 2.0f, ForAll, TEXT("Absorption"), true },
		{ TEXT("Air.AbsorptionDepth.G"), StarterEven, 0.0f, 2.0f, 0.0f, 2.0f, ForAll, TEXT("Absorption"), true },
		{ TEXT("Air.AbsorptionDepth.B"), StarterEven, 0.0f, 2.0f, 0.0f, 2.0f, ForAll, TEXT("Absorption"), true },

		{ TEXT("Shape.CloudBase"), StarterEven, 0.0f, 0.05f, 0.0f, 0.05f, ForClouds },
		{ TEXT("Shape.CloudThickness"), StarterEven, 0.3f, 0.6f, 0.5f, 0.75f, ForClouds },
		{ TEXT("Shape.TopCurve"), StarterEven, 1.0f, 2.0f, 1.0f, 2.0f, ForClouds },
		{ TEXT("Shape.BottomCurve"), StarterEven, 1.0f, 2.0f, 1.0f, 2.0f, ForClouds },

		{ TEXT("Coverage.CloudCover"), StarterEven, 0.3f, 0.8f, 0.85f, 1.0f, ForClouds },
		{ TEXT("Coverage.StormPriority"), StarterEven, 0.5f, 1.5f, 0.5f, 1.5f, ForClouds },
		{ TEXT("Coverage.CoverageSoftness"), StarterEven, 0.15f, 0.5f, 0.15f, 0.5f, ForClouds },
		{ TEXT("Coverage.CoverageFray"), StarterEven, 0.4f, 1.0f, 0.4f, 1.0f, ForClouds },
		{ TEXT("Coverage.CoverageDepthRamp"), StarterEven, 0.2f, 0.6f, 0.2f, 0.6f, ForClouds },

		{ TEXT("Type.TypeBias"), StarterEven, 0.4f, 0.9f, 0.4f, 0.9f, ForClouds },
		{ TEXT("Type.TypeTropical"), StarterEven, 0.2f, 0.8f, 0.2f, 0.8f, ForClouds },
		{ TEXT("Type.TypeStorm"), StarterEven, 0.3f, 0.7f, 0.3f, 0.7f, ForClouds },
		{ TEXT("Type.TypeCurve"), StarterEven, 1.0f, 2.0f, 1.0f, 2.0f, ForClouds },
		{ TEXT("Type.StratusDepth"), StarterEven, 0.2f, 0.4f, 0.2f, 0.4f, ForClouds },

		{ TEXT("Lift.CeilingPressure"), StarterEven, 0.3f, 0.5f, 0.3f, 0.5f, ForClouds },
		{ TEXT("Warp.WarpStretch"), StarterEven, 0.2f, 0.5f, 0.2f, 0.5f, ForClouds },
		{ TEXT("Warp.WarpShift"), StarterEven, 0.15f, 0.3f, 0.15f, 0.3f, ForClouds },

		{ TEXT("StructureLayer.Scale"), StarterLog, 1.5f, 6.0f, 1.0f, 2.0f, ForClouds },
		{ TEXT("StructureLayer.Aspect"), StarterEven, 2.0f, 12.0f, 10.0f, 20.0f, ForClouds },
		{ TEXT("StructureLayer.Erosion"), StarterEven, 0.5f, 0.9f, 0.5f, 0.9f, ForClouds },
		{ TEXT("StructureLayer.Breakup"), StarterEven, 0.3f, 0.7f, 0.3f, 0.7f, ForClouds },
		{ TEXT("DetailLayer.Scale"), StarterEven, 16.0f, 32.0f, 16.0f, 32.0f, ForClouds },
		{ TEXT("DetailLayer.Erosion"), StarterEven, 0.3f, 0.6f, 0.3f, 0.6f, ForClouds },

		{ TEXT("Deep.DeepFill"), StarterEven, 0.1f, 0.4f, 0.1f, 0.4f, ForGasGiant },
		{ TEXT("Deep.FloorRelief"), StarterEven, 0.0f, 0.7f, 0.0f, 0.7f, ForGasGiant },
		{ TEXT("Deep.MaterialDepth"), StarterEven, 0.0f, 1.0f, 0.0f, 1.0f, ForGasGiant },

		{ TEXT("Material.CloudOpticalDepth"), StarterLog, 15.0f, 40.0f, 8.0f, 20.0f, ForClouds },
		{ TEXT("Material.StormBalance"), StarterEven, 0.3f, 0.7f, 0.3f, 0.7f, ForClouds },
		{ TEXT("Material.StormBlend"), StarterEven, 0.3f, 0.6f, 0.3f, 0.6f, ForClouds },
		{ TEXT("Material.CloudScatter.R"), StarterEven, 0.95f, 0.99f, 0.95f, 0.99f, ForTerrestrial, TEXT("CloudWhite") },
		{ TEXT("Material.CloudScatter.G"), StarterEven, 0.95f, 0.99f, 0.95f, 0.99f, ForTerrestrial, TEXT("CloudWhite") },
		{ TEXT("Material.CloudScatter.B"), StarterEven, 0.95f, 0.99f, 0.95f, 0.99f, ForTerrestrial, TEXT("CloudWhite") },
		{ TEXT("Material.StormExtinction.A"), StarterEven, 1.5f, 2.5f, 1.5f, 2.5f, ForClouds },

		{ TEXT("SurfaceShadow.DirectFraction"), StarterEven, 0.7f, 0.9f, 0.7f, 0.9f, ForAll },
	};
}

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

void UAtmosphereLookProfile::AddStarterDraws()
{
	const uint8 Mask = (Model == EPlanetAtmosphereType::GasGiant) ? ForGasGiant
		: (Model == EPlanetAtmosphereType::AirOnly) ? ForAirOnly : ForTerrestrial;

	Modify();

	// The air's colours scale Base's, or the class default's for Model.
	const FAtmosphereModelParams AirSource = Base ? Base->Model
		: GetDefault<APlanetAtmosphereActor>()->GetModelParams(Model);

	int32 Added = 0;

	for (const FStarterDraw& Starter : StarterDraws)
	{
		const bool bAlready = Draws.Draws.ContainsByPredicate(
			[&Starter](const FAtmosphereDraw& Entry) { return Entry.Path == Starter.Path; });

		if (!(Starter.Models & Mask) || bAlready)
		{
			continue;
		}

		FAtmosphereDraw Entry;
		Entry.Path = Starter.Path;
		Entry.Distribution = Starter.Distribution;
		Entry.Min = (Mask == ForGasGiant) ? Starter.GasMin : Starter.Min;
		Entry.Max = (Mask == ForGasGiant) ? Starter.GasMax : Starter.Max;
		Entry.Link = Starter.Link ? FName(Starter.Link) : NAME_None;

		if (Starter.bOfBase)
		{
			double BaseValue = 0.0;

			if (!AtmosphereHarness::ReadValue(FAtmosphereModelParams::StaticStruct(), &AirSource, Entry.Path, BaseValue))
			{
				continue;
			}

			Entry.Min *= (float)BaseValue;
			Entry.Max *= (float)BaseValue;
		}

		Draws.Draws.Add(Entry);
		++Added;
	}

	UE_LOG(LogAtmosphereHarness, Log, TEXT("'%s': added %d starter draws."), *GetName(), Added);
}
