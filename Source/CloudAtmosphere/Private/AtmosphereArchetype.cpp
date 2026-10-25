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

	/** A first-estimate range from Design/HarnessReference.md. */
	struct FStarterDraw
	{
		const TCHAR* Path;
		EAtmosphereDrawDistribution Distribution;
		float Min, Max;
		float GasMin, GasMax;
		uint8 Models;
	};

	/** A colour's palette: Base's colour, or the class default's, as its one
	 *  option, scaled and mutated. */
	struct FStarterPalette
	{
		const TCHAR* Path;
		float ScaleMin, ScaleMax;
		float Mutation;
		uint8 Models;
		/** Stage A2: the colour from the class default for the model, not
		 *  Base's: an absolute range, whatever Base was tuned to. */
		bool bFromClassDefault = false;
	};

	// Unity builds merge anonymous namespaces across files: keep these names distinct.
	constexpr EAtmosphereDrawDistribution StarterEven = EAtmosphereDrawDistribution::Uniform;
	constexpr EAtmosphereDrawDistribution StarterLog = EAtmosphereDrawDistribution::LogUniform;

	const FStarterDraw StarterDraws[] =
	{
		// (Stage A2: an Earth-like shell, a few hundredths of the radius deep.
		// No spin on a terrestrial planet: its sim runs in the planet's
		// rotating frame, so a spin slides the clouds across the ground.)
		{ TEXT("Planet.HeightScale"), StarterEven, 0.03f, 0.07f, 0.05f, 0.2f, ForAll },
		{ TEXT("Planet.SpinRatio"), StarterLog, 0.01f, 0.1f, 0.01f, 0.1f, ForGasGiant },

		{ TEXT("Air.RayleighScaleHeight"), StarterEven, 0.1f, 0.25f, 0.1f, 0.25f, ForAll },
		{ TEXT("Air.MieScaleHeight"), StarterEven, 0.05f, 0.2f, 0.05f, 0.2f, ForAll },
		{ TEXT("Air.MieG"), StarterEven, 0.75f, 0.95f, 0.75f, 0.95f, ForAll },

		// (Stage A2: the slab's base solved, its lowest just under the ground:
		// Shape.bSolveBase. The thickness a third or so of the shell.)
		{ TEXT("Shape.CloudBase"), StarterEven, 0.0f, 0.05f, 0.0f, 0.05f, ForGasGiant },
		{ TEXT("Shape.BaseMinTarget"), StarterEven, -0.01f, 0.0f, -0.01f, 0.0f, ForTerrestrial },
		{ TEXT("Shape.CloudThickness"), StarterEven, 0.2f, 0.4f, 0.5f, 0.75f, ForClouds },
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
		{ TEXT("Material.StormExtinction.A"), StarterEven, 1.5f, 2.5f, 1.5f, 2.5f, ForClouds },

		{ TEXT("SurfaceShadow.DirectFraction"), StarterEven, 0.7f, 0.9f, 0.7f, 0.9f, ForAll },
	};

	const FStarterPalette StarterPalettes[] =
	{
		{ TEXT("Air.RayleighDepth"), 0.3f, 3.0f, 0.1f, ForAll },
		{ TEXT("Air.MieDepth"), 0.3f, 3.0f, 0.1f, ForAll },
		{ TEXT("Air.AbsorptionDepth"), 0.0f, 2.0f, 0.1f, ForAll },

		{ TEXT("Material.CloudScatter"), 0.97f, 1.0f, 0.0f, ForTerrestrial },
		{ TEXT("Material.CloudScatter"), 1.0f, 1.0f, 0.1f, ForGasGiant },
		{ TEXT("Material.StormScatter"), 1.0f, 1.0f, 0.1f, ForClouds },
		{ TEXT("Deep.Scatter"), 1.0f, 1.0f, 0.1f, ForGasGiant },

		// (Stage A2: the cloud's ambient about a thousandth of the star's
		// light, 0.0005 to 0.0015.)
		{ TEXT("Ambient.CloudAmbient"), 0.5f, 1.5f, 0.0f, ForClouds, true },
	};

	/** Starter paths a model no longer draws (stage A2): a refresh removes
	 *  them. */
	struct FRetiredDraw
	{
		const TCHAR* Path;
		uint8 Models;
	};

	const FRetiredDraw RetiredDraws[] =
	{
		{ TEXT("Planet.SpinRatio"), ForTerrestrial },
		{ TEXT("Shape.CloudBase"), ForTerrestrial },
	};

	uint8 StarterMaskOf(EPlanetAtmosphereType Model)
	{
		return (Model == EPlanetAtmosphereType::GasGiant) ? ForGasGiant
			: (Model == EPlanetAtmosphereType::AirOnly) ? ForAirOnly : ForTerrestrial;
	}
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

void UAtmosphereLookProfile::RefreshStarterDraws()
{
	const uint8 Mask = StarterMaskOf(Model);

	Modify();

	// Every starter path and palette, and every retired one, out; then the
	// current starters in.
	const auto IsStarterPath = [Mask](const FString& Path)
		{
			for (const FStarterDraw& Starter : StarterDraws)
			{
				if (Path == Starter.Path)
				{
					return true;
				}
			}

			for (const FRetiredDraw& Retired : RetiredDraws)
			{
				if ((Retired.Models & Mask) && Path == Retired.Path)
				{
					return true;
				}
			}

			return false;
		};

	const int32 Draws0 = Draws.Draws.Num();
	Draws.Draws.RemoveAll([&IsStarterPath](const FAtmosphereDraw& Entry) { return IsStarterPath(Entry.Path); });

	const int32 Choices0 = Draws.Choices.Num();
	Draws.Choices.RemoveAll([](const FAtmosphereChoiceGroup& Group)
		{
			for (const FStarterPalette& Starter : StarterPalettes)
			{
				if (Group.Name == FName(Starter.Path))
				{
					return true;
				}
			}

			return false;
		});

	UE_LOG(LogAtmosphereHarness, Log, TEXT("'%s': removed %d starter draws and %d palettes to refresh them."),
		*GetName(), Draws0 - Draws.Draws.Num(), Choices0 - Draws.Choices.Num());

	AddStarterDraws();
}

void UAtmosphereLookProfile::AddStarterDraws()
{
	const uint8 Mask = StarterMaskOf(Model);

	Modify();

	// Palettes start from Base's colours, or the class default's for Model.
	const FAtmosphereModelParams ClassSource = GetDefault<APlanetAtmosphereActor>()->GetModelParams(Model);
	const FAtmosphereModelParams ColourSource = Base ? Base->Model : ClassSource;

	int32 Added = 0;

	for (const FStarterDraw& Starter : StarterDraws)
	{
		const bool bAlready = Draws.Draws.ContainsByPredicate(
			[&Starter](const FAtmosphereDraw& Entry) { return Entry.Path == Starter.Path; });

		if (!(Starter.Models & Mask) || bAlready)
		{
			continue;
		}

		FAtmosphereDraw& Entry = Draws.Draws.AddDefaulted_GetRef();
		Entry.Path = Starter.Path;
		Entry.Distribution = Starter.Distribution;
		Entry.Min = (Mask == ForGasGiant) ? Starter.GasMin : Starter.Min;
		Entry.Max = (Mask == ForGasGiant) ? Starter.GasMax : Starter.Max;
		++Added;
	}

	for (const FStarterPalette& Starter : StarterPalettes)
	{
		const FName Name(Starter.Path);
		const bool bAlready = Draws.Choices.ContainsByPredicate(
			[&Name](const FAtmosphereChoiceGroup& Group) { return Group.Name == Name; });

		FLinearColor Colour;

		const FAtmosphereModelParams& Source = Starter.bFromClassDefault ? ClassSource : ColourSource;

		if (!(Starter.Models & Mask) || bAlready
			|| !AtmosphereHarness::ReadColour(FAtmosphereModelParams::StaticStruct(), &Source, Starter.Path, Colour))
		{
			continue;
		}

		FAtmosphereChoiceGroup& Palette = Draws.Choices.AddDefaulted_GetRef();
		Palette.Name = Name;
		FAtmospherePaletteColour& Entry = Palette.Colours.AddDefaulted_GetRef();
		Entry.Path = Starter.Path;
		Entry.Options.Add(Colour);
		Palette.ScaleMin = Starter.ScaleMin;
		Palette.ScaleMax = Starter.ScaleMax;
		Palette.Mutation = Starter.Mutation;
		++Added;
	}

	UE_LOG(LogAtmosphereHarness, Log, TEXT("'%s': added %d starter draws and palettes."), *GetName(), Added);
}
