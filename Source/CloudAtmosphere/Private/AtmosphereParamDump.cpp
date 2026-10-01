#include "CoreMinimal.h"

#include "Algo/AllOf.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EngineUtils.h"
#include "FlowSimSettings.h"
#include "FlowSimSubsystem.h"
#include "HAL/IConsoleManager.h"
#include "JsonObjectConverter.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PlanetAtmosphereActor.h"
#include "Serialization/CustomVersion.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"

#include <cstdio>
#include <cstdlib>

// CloudAtmosphere.DumpParams [FileName]
//
// Writes the running sim config, the sim settings and every atmosphere actor
// in the world to Saved/CloudAtmosphere as JSON; each actor records its active
// model's sim config. Each object carries its full
// Values and its Overrides: every member that differs from the C++ defaults,
// with both values, so the tuned assets can be told apart from the class
// defaults and read back as the source of new defaults and presets.
//
// CloudAtmosphere.LoadParams FileName [Sim|Atmospheres] [Pipeline]
//
// Reads a file in the same layout back into the world's atmosphere actors and
// the active model's sim config of the one that ran the sim when dumped (else
// the first), or into the running config when no atmosphere is loaded.

DEFINE_LOG_CATEGORY_STATIC(LogAtmosphereDump, Log, All);

namespace AtmosphereDump
{
	using FFilter = TFunctionRef<bool(const FProperty*)>;

	/** The sim config's saved-data version; -1 if unregistered. */
	int32 ConfigVersion()
	{
		for (const FCustomVersion& Version : FCurrentCustomVersions::GetAll().GetAllVersions())
		{
			if (Version.GetFriendlyName() == FName(TEXT("FlowSimConfig")))
			{
				return Version.Version;
			}
		}

		return -1;
	}

	/** Every property but those kept only to load old data. */
	bool Current(const FProperty* Property)
	{
		return !Property->HasAnyPropertyFlags(CPF_Deprecated);
	}

	/** The actor's panel: edited members only, which leaves out internal state
	 *  and the transient readouts. */
	bool Authored(const FProperty* Property)
	{
		return Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_Transient);
	}

	TSharedPtr<FJsonValue> ValueOf(FProperty* Property, const void* Value)
	{
		return FJsonObjectConverter::UPropertyToJsonValue(
			Property, Value, 0, 0, nullptr, nullptr, EJsonObjectConversionFlags::SkipStandardizeCase);
	}

	/** Structs declared in this module are parameter groups and are diffed per
	 *  member; engine structs (colours, vectors) are single values. */
	bool IsGroup(const FProperty* Property)
	{
		const FStructProperty* Struct = CastField<FStructProperty>(Property);
		return Struct && Struct->Struct->GetPackage() == APlanetAtmosphereActor::StaticClass()->GetPackage();
	}

	TSharedRef<FJsonObject> Values(const UStruct* Type, const void* Container, FFilter Filter)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();

		for (TFieldIterator<FProperty> It(Type, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			if (Filter(*It))
			{
				Out->SetField(It->GetName(), ValueOf(*It, It->ContainerPtrToValuePtr<void>(Container)));
			}
		}

		return Out;
	}

	/** Every member differing from Default, flattened to "Group.Member". */
	void Overrides(const UStruct* Type, const void* Container, const void* Default,
		const FString& Prefix, FFilter Filter, FJsonObject& Out)
	{
		for (TFieldIterator<FProperty> It(Type, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			const void* Value = Property->ContainerPtrToValuePtr<void>(Container);
			const void* Base = Property->ContainerPtrToValuePtr<void>(Default);

			if (!Filter(Property) || Property->Identical(Value, Base))
			{
				continue;
			}

			const FString Name = Prefix + Property->GetName();

			if (IsGroup(Property))
			{
				Overrides(CastFieldChecked<FStructProperty>(Property)->Struct, Value, Base, Name + TEXT("."), Current, Out);
				continue;
			}

			TSharedRef<FJsonObject> Pair = MakeShared<FJsonObject>();
			Pair->SetField(TEXT("Value"), ValueOf(Property, Value));
			Pair->SetField(TEXT("Default"), ValueOf(Property, Base));
			Out.SetObjectField(Name, Pair);
		}
	}

	/** Adds Overrides, then the full Values. */
	void Describe(const UStruct* Type, const void* Container, const void* Default, FFilter Filter, FJsonObject& Out)
	{
		TSharedRef<FJsonObject> Overridden = MakeShared<FJsonObject>();
		Overrides(Type, Container, Default, FString(), Filter, *Overridden);

		Out.SetObjectField(TEXT("Overrides"), Overridden);
		Out.SetObjectField(TEXT("Values"), Values(Type, Container, Filter));
	}

	FString PathOf(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString(TEXT("None"));
	}

	TSharedRef<FJsonValue> Floats(TConstArrayView<float> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;

		for (const float V : Values)
		{
			Out.Add(MakeShared<FJsonValueNumber>(V));
		}

		return MakeShared<FJsonValueArray>(Out);
	}

	TSharedRef<FJsonObject> DescribeSim(const UFlowSimSubsystem& Sub)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		const UFlowSimConfig* Config = Sub.GetConfig();

		Out->SetBoolField(TEXT("Running"), Sub.IsRunning());
		Out->SetStringField(TEXT("Config"), PathOf(Config));
		Out->SetNumberField(TEXT("ConfigVersion"), ConfigVersion());
		Out->SetNumberField(TEXT("SimulatedTime"), Sub.GetSimulatedTime());
		Out->SetNumberField(TEXT("DisplayTime"), Sub.GetDisplayTime());
		Out->SetNumberField(TEXT("StepsCompleted"), Sub.GetStepsCompleted());
		Out->SetNumberField(TEXT("StepsLastFrame"), Sub.GetStepsLastFrame());
		Out->SetNumberField(TEXT("Courant"), Sub.GetCourant());

		FFlowSimParams Params;

		if (!Config || !Sub.GetRunningParams(Params))
		{
			return Out;
		}

		// What the config resolves to at the last frame's step: the output
		// normalisation the deck is calibrated against, and the stack.
		const int32 Layers = Params.GridSize.Z;
		TArray<float> Speeds;

		for (int32 m = 0; m < Layers; ++m)
		{
			Speeds.Add(FMath::Sqrt(FMath::Max(Params.Stack.ModeSpeedSq[m], 0.0f)));
		}

		TSharedRef<FJsonObject> Derived = MakeShared<FJsonObject>();
		Derived->SetNumberField(TEXT("Step"), Params.DeltaTime);
		Derived->SetNumberField(TEXT("ImplicitWeight"), Params.ImplicitWeight);
		Derived->SetNumberField(TEXT("ImplicitDampingRate"), Config->GetImplicitDampingRate(Params.DeltaTime));
		Derived->SetNumberField(TEXT("DivergenceDampingPerStep"), Params.DivergenceDamping);
		Derived->SetNumberField(TEXT("PressureScale"), Params.OutputScales.X);
		Derived->SetNumberField(TEXT("VorticityScale"), Params.OutputScales.Y);
		Derived->SetNumberField(TEXT("DivergenceScale"), Params.OutputScales.Z);
		Derived->SetNumberField(TEXT("NoiseDriftRate"), Params.NoiseDriftRate);
		Derived->SetNumberField(TEXT("NoiseResetTime"), Params.NoiseResetTime);
		Derived->SetNumberField(TEXT("AtlasFaceSize"), Params.AtlasFaceSize);
		Derived->SetStringField(TEXT("Grid"), FString::Printf(TEXT("%dx%dx%d"), Params.GridSize.X, Params.GridSize.Y, Layers));
		Derived->SetField(TEXT("LayerDepth"), Floats(MakeArrayView(Params.Stack.Depth, Layers)));
		Derived->SetField(TEXT("ModeWaveSpeed"), Floats(Speeds));

		// Each perpetual storm's settled latitude in degrees, its longitude rate
		// in radians per unit sim time, and its spin from the shear, 1
		// counterclockwise seen from outside.
		TArray<float> PerpetualLatitudes;
		TArray<float> PerpetualRates;
		TArray<float> PerpetualSenses;

		for (int32 i = 0; i < Params.PerpetualCount; ++i)
		{
			PerpetualLatitudes.Add(FMath::RadiansToDegrees(Params.PerpetualShape[i].X));
			PerpetualRates.Add((float)Params.PerpetualRate[i]);
			PerpetualSenses.Add(Params.PerpetualLook[i].X >= 0.0f ? 1.0f : -1.0f);
		}

		Derived->SetField(TEXT("PerpetualLatitudes"), Floats(PerpetualLatitudes));
		Derived->SetField(TEXT("PerpetualRates"), Floats(PerpetualRates));
		Derived->SetField(TEXT("PerpetualSenses"), Floats(PerpetualSenses));
		Out->SetObjectField(TEXT("Derived"), Derived);

		Describe(UFlowSimConfig::StaticClass(), Config, GetDefault<UFlowSimConfig>(), Current, *Out);
		return Out;
	}

	TSharedRef<FJsonObject> DescribeActor(const APlanetAtmosphereActor& Actor, const UFlowSimSubsystem& Sub)
	{
		const UFlowSimConfig* Running = Sub.GetConfig();

		// Against the native class defaults, the values the audit's class-default
		// calls were made at, whatever Blueprint subclass the actor is.
		const UClass* Native = APlanetAtmosphereActor::StaticClass();
		const UFlowSimConfig* Own = Actor.ActiveSimConfig();

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("Actor"), Actor.GetActorNameOrLabel());
		Out->SetStringField(TEXT("Class"), Actor.GetClass()->GetPathName());
		Out->SetStringField(TEXT("SimConfig"), PathOf(Own));
		Out->SetBoolField(TEXT("SimConfigIsRunning"), Running && Own == Running);
		Out->SetBoolField(TEXT("DrivesSim"), Sub.IsOwner(&Actor));

		Describe(Native, &Actor, Native->GetDefaultObject(), Authored, *Out);
		return Out;
	}

	// -- Writing --------------------------------------------------------------
	//
	// A small writer rather than TJsonWriter: its numbers are printed at 17
	// digits, which turns every authored float into 0.10000000149011612.

	/** Shortest decimal that reads back as the same float, or as the same double
	 *  when the value is not exactly a float. */
	FString Number(double Value)
	{
		if (!FMath::IsFinite(Value))
		{
			return TEXT("null");
		}

		const bool bFloat = (double)(float)Value == Value;
		char Buffer[40];

		for (int32 Digits = 6; Digits <= 17; ++Digits)
		{
			std::snprintf(Buffer, sizeof(Buffer), "%.*g", Digits, Value);
			const double Back = std::strtod(Buffer, nullptr);

			if (bFloat ? (float)Back == (float)Value : Back == Value)
			{
				break;
			}
		}

		return FString(ANSI_TO_TCHAR(Buffer));
	}

	FString Quote(const FString& In)
	{
		FString Out = TEXT("\"");

		for (const TCHAR C : In)
		{
			switch (C)
			{
			case TEXT('"'):  Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			default:
				if (C < 0x20)
				{
					Out += FString::Printf(TEXT("\\u%04x"), (uint32)C);
				}
				else
				{
					Out.AppendChar(C);
				}
			}
		}

		return Out + TEXT("\"");
	}

	bool IsScalar(const TSharedPtr<FJsonValue>& V)
	{
		return !V.IsValid() || (V->Type != EJson::Object && V->Type != EJson::Array);
	}

	void Write(const TSharedPtr<FJsonValue>& V, int32 Depth, FString& Out);

	/** One entry per line, or all on one line when bInline: containers of up to
	 *  four scalars, so colours and vectors read as one value. */
	template<typename TEntries, typename TWriteEntry>
	void WriteContainer(const TEntries& Entries, bool bInline, const TCHAR* Open, const TCHAR* Close,
		int32 Depth, FString& Out, TWriteEntry WriteEntry)
	{
		const FString Inner = FString::ChrN(Depth + 1, TEXT('\t'));
		bool bFirst = true;

		Out += Open;

		for (const auto& Entry : Entries)
		{
			if (!bFirst)
			{
				Out += TEXT(",");
			}

			if (!bInline)
			{
				Out += TEXT("\n") + Inner;
			}
			else if (!bFirst)
			{
				Out += TEXT(" ");
			}

			WriteEntry(Entry);
			bFirst = false;
		}

		if (!bInline && !bFirst)
		{
			Out += TEXT("\n") + FString::ChrN(Depth, TEXT('\t'));
		}

		Out += Close;
	}

	void Write(const TSharedPtr<FJsonValue>& V, int32 Depth, FString& Out)
	{
		if (!V.IsValid())
		{
			Out += TEXT("null");
			return;
		}

		switch (V->Type)
		{
		case EJson::Boolean: Out += V->AsBool() ? TEXT("true") : TEXT("false"); break;
		case EJson::Number:  Out += Number(V->AsNumber()); break;
		case EJson::String:  Out += Quote(V->AsString()); break;

		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& Items = V->AsArray();
			const bool bInline = Items.Num() <= 4 && Algo::AllOf(Items, IsScalar);

			WriteContainer(Items, bInline, TEXT("["), TEXT("]"), Depth, Out,
				[Depth, &Out](const TSharedPtr<FJsonValue>& Item) { Write(Item, Depth + 1, Out); });
			break;
		}

		case EJson::Object:
		{
			const TMap<FString, TSharedPtr<FJsonValue>>& Fields = V->AsObject()->Values;
			bool bInline = Fields.Num() <= 4;

			for (const auto& Field : Fields)
			{
				bInline &= IsScalar(Field.Value);
			}

			WriteContainer(Fields, bInline, TEXT("{"), TEXT("}"), Depth, Out,
				[Depth, &Out](const auto& Field)
				{
					Out += Quote(Field.Key) + TEXT(": ");
					Write(Field.Value, Depth + 1, Out);
				});
			break;
		}

		default: Out += TEXT("null"); break;
		}
	}

	void Dump(const TArray<FString>& Args, UWorld* World)
	{
		const UFlowSimSubsystem* Sub = World ? World->GetSubsystem<UFlowSimSubsystem>() : nullptr;

		if (!Sub)
		{
			UE_LOG(LogAtmosphereDump, Error, TEXT("No flow sim subsystem in this world."));
			return;
		}

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("World"), World->GetName());
		Root->SetStringField(TEXT("Written"), FDateTime::Now().ToIso8601());
		Root->SetObjectField(TEXT("Sim"), DescribeSim(*Sub));

		const UFlowSimSettings* Settings = GetDefault<UFlowSimSettings>();
		Root->SetObjectField(TEXT("Settings"), Values(UFlowSimSettings::StaticClass(), Settings, Current));

		TArray<TSharedPtr<FJsonValue>> Actors;

		for (TActorIterator<APlanetAtmosphereActor> It(World); It; ++It)
		{
			Actors.Add(MakeShared<FJsonValueObject>(DescribeActor(**It, *Sub)));
		}

		Root->SetArrayField(TEXT("Atmospheres"), Actors);

		FString Name = Args.Num() > 0
			? Args[0]
			: FString::Printf(TEXT("AtmosphereParams_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));

		if (!Name.EndsWith(TEXT(".json")))
		{
			Name += TEXT(".json");
		}

		const FString Path = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CloudAtmosphere"), Name));

		FString Text;
		Write(MakeShared<FJsonValueObject>(Root), 0, Text);
		Text += TEXT("\n");

		if (FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogAtmosphereDump, Display, TEXT("Wrote %d atmosphere(s) and the sim config to %s"), Actors.Num(), *Path);
		}
		else
		{
			UE_LOG(LogAtmosphereDump, Error, TEXT("Could not write %s"), *Path);
		}
	}
}

namespace AtmosphereLoad
{
	using AtmosphereDump::IsGroup;

	struct FReport
	{
		int32 Applied = 0;
		int32 Unchanged = 0;
		TArray<FString> Skipped;
		TArray<FString> Clamped;

		/** Top-level members left alone unless the Pipeline argument is given. */
		const TSet<FName>* Excluded = nullptr;
	};

	/** Assets, targets, debug views and start state: what a machine or a
	 *  session owns rather than what a tune is, so a preset does not repoint
	 *  them. */
	const TSet<FName>& SimPipeline()
	{
		static const TSet<FName> Names = {
			TEXT("InitialState"), TEXT("bDebugView"),
			TEXT("DebugMode"), TEXT("DebugLayer"), TEXT("DebugScale"), TEXT("bPaused") };
		return Names;
	}

	const TSet<FName>& ActorPipeline()
	{
		static const TSet<FName> Names = {
			TEXT("BlueNoise"), TEXT("Simulation") };
		return Names;
	}

	// -- Renamed members ------------------------------------------------------
	//
	// A file written before a rename is read under the current names. Rows are
	// dotted paths ("Group.Member", "Group" or "Member") and apply in order, so
	// a later row renaming an earlier row's New chains. An empty New retires a
	// member: its value is dropped and the load says so. Nothing is converted: a
	// member whose meaning changed is retired, and a preset carries its value.

	struct FRename
	{
		const TCHAR* Old;
		const TCHAR* New;

		/** Nonzero applies the row only to a file below this config version:
		 *  a member that kept its name under a new meaning. */
		int32 Before = 0;
	};

	TConstArrayView<FRename> SimRenames()
	{
		static const FRename Rows[] = {
			{ TEXT("ForcingScale"), TEXT("ForcingFrequency") },
			{ TEXT("SaturationPole"), TEXT("SaturationPoleRatio") },
			{ TEXT("FroudeCeiling"), TEXT("SpeedRoot") },

			// Retired: the forcing always decodes unipolar (a file saved with true
			// runs at twice its eddy forcing), the equator's saturation is the
			// vapour unit (the pole's ratio above is exact where it was 1), and
			// the polar filter's width follows from the grid.
			{ TEXT("bForcingBipolar"), TEXT("") },
			{ TEXT("SaturationEquator"), TEXT("") },
			{ TEXT("FilterMaxHalfWidth"), TEXT("") },

			// The render targets are the subsystem's, created at run time.
			{ TEXT("FlowTarget"), TEXT("") },
			{ TEXT("DebugTarget"), TEXT("") },
			{ TEXT("bAutoResizeTargets"), TEXT("") },

			// New meanings, retired for the defaults: the eye comes from the low
			// alone and its ramp is a ratio of the eyewall; genesis humidity is a
			// margin over the onset, the sustain a multiple of GenesisStorm and
			// the genesis shear a multiple of ShearSpeed; the storm is an amount
			// over its lifetime, the cell cloud an equilibrium cover, the band an
			// excess over the whole storm and the decay a persistence.
			{ TEXT("StormCellEye"), TEXT("") },
			{ TEXT("StormCellEyeLow"), TEXT("") },
			{ TEXT("GenesisHumidity"), TEXT("") },
			{ TEXT("StormCellSustain"), TEXT("") },
			{ TEXT("GenesisShearSpeed"), TEXT("") },
			{ TEXT("StormRate"), TEXT("") },
			{ TEXT("StormCellCloud"), TEXT("") },
			{ TEXT("StormCellCloudRate"), TEXT("") },
			{ TEXT("StormCellBandStorm"), TEXT("") },
			{ TEXT("StormCellDecay"), TEXT("") },

			// Version 7 authors every rate and lifetime in turnovers, and the
			// storm cells' radius and the forcing frequency against
			// DeformationRadius, under the same names.
			{ TEXT("NudgeRate"), TEXT(""), 7 },
			{ TEXT("DragRate"), TEXT(""), 7 },
			{ TEXT("ThermalRelaxation"), TEXT(""), 7 },
			{ TEXT("LayerCoupling"), TEXT(""), 7 },
			{ TEXT("GridDamping"), TEXT(""), 7 },
			{ TEXT("SurfaceEvaporation"), TEXT(""), 7 },
			{ TEXT("CondensationRate"), TEXT(""), 7 },
			{ TEXT("EvaporationRate"), TEXT(""), 7 },
			{ TEXT("CloudLifetime"), TEXT(""), 7 },
			{ TEXT("AscentSmoothing"), TEXT(""), 7 },
			{ TEXT("StormLifetime"), TEXT(""), 7 },
			{ TEXT("StormCellSpawnRate"), TEXT(""), 7 },
			{ TEXT("StormCellLifetime"), TEXT(""), 7 },
			{ TEXT("StormCellGrowth"), TEXT(""), 7 },
			{ TEXT("StormCellFollow"), TEXT(""), 7 },
			{ TEXT("StormCellCoreFollow"), TEXT(""), 7 },
			{ TEXT("StormCellForcing"), TEXT(""), 7 },
			{ TEXT("ForcingLifetime"), TEXT(""), 7 },
			{ TEXT("StormCellRadius"), TEXT(""), 7 },
			{ TEXT("ForcingFrequency"), TEXT(""), 7 },
		};
		return Rows;
	}

	TConstArrayView<FRename> AtmosphereRenames()
	{
		static const FRename Rows[] = {
			// Retired or moved: erosion lives in the structure layer's own Erosion,
			// the extinction group's members in the material, scattering and gas
			// giant profile groups; the rest are noise-layer members neither model
			// reads.
			{ TEXT("TerrestrialProfile.ErosionGain"), TEXT("") },
			{ TEXT("TerrestrialProfile.ErosionAscent"), TEXT("") },
			{ TEXT("TerrestrialStructureLayer.NoiseWeights"), TEXT("") },
			{ TEXT("TerrestrialStructureLayer.Relief"), TEXT("") },
			{ TEXT("TerrestrialStructureLayer.ShearInherit"), TEXT("") },
			{ TEXT("TerrestrialStructureLayer.FadeNear"), TEXT("") },
			{ TEXT("TerrestrialStructureLayer.FadeSpan"), TEXT("") },
			{ TEXT("TerrestrialStructureLayer.FadeMean"), TEXT("") },
			{ TEXT("TerrestrialStructureLayer.BandMix"), TEXT("") },
			{ TEXT("TerrestrialStructureLayer.bCrossfade"), TEXT("") },
			{ TEXT("TerrestrialDetailLayer.NoiseWeights"), TEXT("") },
			{ TEXT("TerrestrialDetailLayer.Relief"), TEXT("") },
			{ TEXT("TerrestrialDetailLayer.ShearInherit"), TEXT("") },
			{ TEXT("TerrestrialDetailLayer.BandMix"), TEXT("") },
			{ TEXT("TerrestrialDetailLayer.bCrossfade"), TEXT("") },
			{ TEXT("TerrestrialProfile.CloudOpticalDepth"), TEXT("TerrestrialCloudMaterial.CloudOpticalDepth") },
			{ TEXT("TerrestrialExtinction.LightExtinctionFraction"), TEXT("TerrestrialMultipleScattering.LightExtinctionFraction") },
			{ TEXT("TerrestrialExtinction.DensityCurve"), TEXT("") },
			{ TEXT("Extinction.LightExtinctionFraction"), TEXT("MultipleScattering.LightExtinctionFraction") },
			{ TEXT("Extinction.DensityCurve"), TEXT("GasGiantProfile.DensityCurve") },
			{ TEXT("StructureLayer.FadeMean"), TEXT("") },
			{ TEXT("StructureLayer.MipBias"), TEXT("") },
			{ TEXT("DetailLayer.FadeMean"), TEXT("") },
			{ TEXT("DetailLayer.MipBias"), TEXT("") },

			// The terrestrial groups by what they act on: the profile splits into
			// shape, coverage, type, lift and warp, the genus joins type, the shell
			// height and spin form the planet group, and the lighting group splits
			// into air and ambient, taking the ambient terminator and the Mie lobe
			// decay with it. The terrestrial planet shadow is fixed, so the rest of
			// its terminator group retires.
			{ TEXT("TerrestrialGeometry"), TEXT("TerrestrialPlanet") },
			{ TEXT("TerrestrialMotion.RotationWeight"), TEXT("TerrestrialPlanet.SpinRate") },
			{ TEXT("TerrestrialProfile"), TEXT("TerrestrialShape") },
			{ TEXT("TerrestrialShape.CloudLayer"), TEXT("TerrestrialCoverage.CloudLayer") },
			{ TEXT("TerrestrialShape.CloudCover"), TEXT("TerrestrialCoverage.CloudCover") },
			{ TEXT("TerrestrialShape.StormPriority"), TEXT("TerrestrialCoverage.StormPriority") },
			{ TEXT("TerrestrialShape.CoverageSoftness"), TEXT("TerrestrialCoverage.CoverageSoftness") },
			{ TEXT("TerrestrialShape.CoverageFray"), TEXT("TerrestrialCoverage.CoverageFray") },
			{ TEXT("TerrestrialShape.TypeBias"), TEXT("TerrestrialType.TypeBias") },
			{ TEXT("TerrestrialShape.TypeTropical"), TEXT("TerrestrialType.TypeTropical") },
			{ TEXT("TerrestrialShape.TypeStorm"), TEXT("TerrestrialType.TypeStorm") },
			{ TEXT("TerrestrialShape.TypeCurve"), TEXT("TerrestrialType.TypeCurve") },
			{ TEXT("TerrestrialShape.StratusDepth"), TEXT("TerrestrialType.StratusDepth") },
			{ TEXT("TerrestrialGenus"), TEXT("TerrestrialType") },
			{ TEXT("TerrestrialShape.PressureScale"), TEXT("TerrestrialLift.PressureScale") },
			{ TEXT("TerrestrialShape.CeilingDepth"), TEXT("TerrestrialLift.CeilingDepth") },
			{ TEXT("TerrestrialShape.CeilingPressure"), TEXT("TerrestrialLift.CeilingPressure") },
			{ TEXT("TerrestrialShape.BaseTropical"), TEXT("TerrestrialLift.BaseTropical") },
			{ TEXT("TerrestrialShape.BasePressure"), TEXT("TerrestrialLift.BasePressure") },
			{ TEXT("TerrestrialShape.AltitudeGain"), TEXT("TerrestrialLift.AltitudeGain") },
			{ TEXT("TerrestrialShape.AltitudeLift"), TEXT("TerrestrialLift.AltitudeLift") },
			{ TEXT("TerrestrialShape.WarpStretch"), TEXT("TerrestrialWarp.WarpStretch") },
			{ TEXT("TerrestrialShape.WarpShift"), TEXT("TerrestrialWarp.WarpShift") },
			{ TEXT("TerrestrialShape.CloudSlope"), TEXT("TerrestrialCloudSlope") },
			{ TEXT("TerrestrialShape.StormBalance"), TEXT("TerrestrialCloudMaterial.StormBalance") },
			{ TEXT("TerrestrialShape.StormBlend"), TEXT("TerrestrialCloudMaterial.StormBlend") },
			{ TEXT("TerrestrialAtmosphereLighting"), TEXT("TerrestrialAir") },
			{ TEXT("TerrestrialAir.AtmosphereAmbient"), TEXT("TerrestrialAmbient.AtmosphereAmbient") },
			{ TEXT("TerrestrialAir.AtmosphereAmbientFloor"), TEXT("TerrestrialAmbient.AtmosphereAmbientFloor") },
			{ TEXT("TerrestrialPhase.CloudAmbient"), TEXT("TerrestrialAmbient.CloudAmbient") },
			{ TEXT("TerrestrialPhase.CloudAmbientFloor"), TEXT("TerrestrialAmbient.CloudAmbientFloor") },
			{ TEXT("TerrestrialTerminator.AmbientTerminator"), TEXT("TerrestrialAmbient.AmbientTerminator") },
			{ TEXT("TerrestrialTerminator.MieLobeDecay"), TEXT("TerrestrialAir.MieLobeDecay") },
			{ TEXT("TerrestrialTerminator.TerminatorSoftness"), TEXT("") },
			{ TEXT("TerrestrialTerminator.LobeShadowPower"), TEXT("") },

			// The gas giant's air and ambient split the same way; its terminator
			// keeps its softness and lobe power.
			{ TEXT("AtmosphereLighting"), TEXT("Air") },
			{ TEXT("Air.AtmosphereAmbient"), TEXT("Ambient.AtmosphereAmbient") },
			{ TEXT("Air.AtmosphereAmbientFloor"), TEXT("Ambient.AtmosphereAmbientFloor") },
			{ TEXT("Phase.CloudAmbient"), TEXT("Ambient.CloudAmbient") },
			{ TEXT("Phase.CloudAmbientFloor"), TEXT("Ambient.CloudAmbientFloor") },
			{ TEXT("Terminator.AmbientTerminator"), TEXT("Ambient.AmbientTerminator") },
			{ TEXT("Terminator.MieLobeDecay"), TEXT("Air.MieLobeDecay") },

			// Retired for the defaults: sunlight penetration is one less the
			// light-ray extinction fraction, and each model's default gives its
			// former default; the cascade radii moved to the surface shadows at
			// new defaults.
			{ TEXT("TerrestrialMultipleScattering.LightExtinctionFraction"), TEXT("") },
			{ TEXT("MultipleScattering.LightExtinctionFraction"), TEXT("") },
			{ TEXT("ShadowCascadeRadii"), TEXT("") },

			// Multiple scattering as glow and spread: attenuation carries over as
			// glow; the contribution folds into it and the eccentricity inverts,
			// so both retire for the defaults.
			{ TEXT("TerrestrialMultipleScattering.OctaveAttenuation"), TEXT("TerrestrialMultipleScattering.ScatteringGlow") },
			{ TEXT("TerrestrialMultipleScattering.OctaveContribution"), TEXT("") },
			{ TEXT("TerrestrialMultipleScattering.OctaveEccentricity"), TEXT("") },
			{ TEXT("MultipleScattering.OctaveAttenuation"), TEXT("MultipleScattering.ScatteringGlow") },
			{ TEXT("MultipleScattering.OctaveContribution"), TEXT("") },
			{ TEXT("MultipleScattering.OctaveEccentricity"), TEXT("") },

			// The air as column depths and its ambient as a ratio of the light, and
			// the detail fade in noise features: new meanings, retired for the
			// defaults. The ambient floor keeps its meaning.
			{ TEXT("TerrestrialAir.RayleighBeta"), TEXT("") },
			{ TEXT("TerrestrialAir.MieBeta"), TEXT("") },
			{ TEXT("TerrestrialAir.AbsorptionBeta"), TEXT("") },
			{ TEXT("Air.RayleighBeta"), TEXT("") },
			{ TEXT("Air.MieBeta"), TEXT("") },
			{ TEXT("Air.AbsorptionBeta"), TEXT("") },
			{ TEXT("TerrestrialAmbient.AtmosphereAmbient"), TEXT("") },
			{ TEXT("TerrestrialAmbient.AtmosphereAmbientFloor"), TEXT("TerrestrialAmbient.AirAmbientFloor") },
			{ TEXT("Ambient.AtmosphereAmbient"), TEXT("") },
			{ TEXT("Ambient.AtmosphereAmbientFloor"), TEXT("Ambient.AirAmbientFloor") },
			{ TEXT("TerrestrialDetailLayer.FadeNear"), TEXT("") },
			{ TEXT("TerrestrialDetailLayer.FadeSpan"), TEXT("") },

			// Tower depth is CloudThickness alone. Exact where CeilingDepth was 1;
			// otherwise CloudThickness takes the product, the lift terms,
			// SurfaceSoftness and WarpShift divide by CeilingDepth, and
			// CloudOpticalDepth re-solves.
			{ TEXT("TerrestrialLift.CeilingDepth"), TEXT("") },

			// Both models draw through the compute passes: the materials, the gas
			// giant's composite blur and the terrestrial young-pixel blur retire.
			{ TEXT("TerrestrialMarchMaterial"), TEXT("") },
			{ TEXT("GasGiantMarchMaterial"), TEXT("") },
			{ TEXT("PostprocessMaterial"), TEXT("") },
			{ TEXT("Composite"), TEXT("") },
			{ TEXT("TerrestrialSampling.YoungBlur"), TEXT("") },

			// The gas giant runs the cloud field: its shell height and spin carry
			// over, its own deck's groups retire, and it takes GasGiant twins of the
			// terrestrial groups at their defaults.
			{ TEXT("Geometry.HeightScale"), TEXT("GasGiantPlanet.HeightScale") },
			{ TEXT("Flow.RotationWeight"), TEXT("GasGiantPlanet.SpinRate") },
			{ TEXT("Geometry"), TEXT("") },
			{ TEXT("GasGiantProfile"), TEXT("") },
			{ TEXT("Flow"), TEXT("") },
			{ TEXT("GasGiantBandShape"), TEXT("") },
			{ TEXT("Motion"), TEXT("") },
			{ TEXT("Carve"), TEXT("") },
			{ TEXT("StructureLayer"), TEXT("") },
			{ TEXT("DetailLayer"), TEXT("") },
			{ TEXT("GasGiantBands"), TEXT("") },
			{ TEXT("Terminator"), TEXT("") },
			// The fill's relief became the floor's, a different meaning.
			{ TEXT("GasGiantDeep.FillRelief"), TEXT("") },

			// A sim config per model; the one config was the gas giant's. The
			// shadow target is created at run time.
			{ TEXT("Simulation.Config"), TEXT("Simulation.GasGiantConfig") },
			{ TEXT("Simulation.bStartOnBeginPlay"), TEXT("Simulation.bClaimSimulation") },
			{ TEXT("ShadowTarget"), TEXT("") },

			// The detail fade is in planet radii, as a near and a far distance,
			// where it was in noise features as a start and a length.
			{ TEXT("TerrestrialDetailLayer.FadeStart"), TEXT("") },
			{ TEXT("TerrestrialDetailLayer.FadeLength"), TEXT("") },
			{ TEXT("GasGiantDetailLayer.FadeStart"), TEXT("") },
			{ TEXT("GasGiantDetailLayer.FadeLength"), TEXT("") },
		};
		return Rows;
	}

	/** The object holding Path's last segment, creating groups on the way when
	 *  bCreate; null where a segment is missing or not an object. */
	TSharedPtr<FJsonObject> Holder(const TSharedPtr<FJsonObject>& Root, const TArray<FString>& Path, bool bCreate)
	{
		TSharedPtr<FJsonObject> Scope = Root;

		for (int32 i = 0; i + 1 < Path.Num() && Scope.IsValid(); ++i)
		{
			const TSharedPtr<FJsonObject>* Next = nullptr;

			if (Scope->TryGetObjectField(Path[i], Next))
			{
				Scope = *Next;
			}
			else if (bCreate && !Scope->HasField(Path[i]))
			{
				const TSharedPtr<FJsonObject> Created = MakeShared<FJsonObject>();
				Scope->SetObjectField(Path[i], Created);
				Scope = Created;
			}
			else
			{
				Scope = nullptr;
			}
		}

		return Scope;
	}

	/** Value into Target under Key. A group merges member by member; anything
	 *  Target already holds is kept. */
	void Merge(FJsonObject& Target, const FString& Key, const TSharedPtr<FJsonValue>& Value)
	{
		const TSharedPtr<FJsonObject>* Existing = nullptr;

		if (Value->Type == EJson::Object && Target.TryGetObjectField(Key, Existing))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Value->AsObject()->Values)
			{
				Merge(**Existing, Field.Key, Field.Value);
			}
		}
		else if (!Target.HasField(Key))
		{
			Target.SetField(Key, Value);
		}
	}

	/** One row over a Values object, whose groups nest. A group the move
	 *  empties is removed. */
	void RenameValues(const TSharedPtr<FJsonObject>& Values, const FRename& Row, TArray<FString>& Retired)
	{
		TArray<FString> From;
		TArray<FString> To;
		FString(Row.Old).ParseIntoArray(From, TEXT("."));
		FString(Row.New).ParseIntoArray(To, TEXT("."));

		const TSharedPtr<FJsonObject> Source = From.IsEmpty() ? nullptr : Holder(Values, From, false);
		const TSharedPtr<FJsonValue> Value = Source.IsValid() ? Source->TryGetField(From.Last()) : nullptr;

		if (!Value.IsValid())
		{
			return;
		}

		Source->RemoveField(From.Last());

		if (To.IsEmpty())
		{
			Retired.Add(Row.Old);
		}
		else if (const TSharedPtr<FJsonObject> Target = Holder(Values, To, true))
		{
			Merge(*Target, To.Last(), Value);
		}

		for (int32 n = From.Num() - 1; n > 0; --n)
		{
			From.SetNum(n);
			const TSharedPtr<FJsonObject> Parent = Holder(Values, From, false);
			const TSharedPtr<FJsonObject>* Group = nullptr;

			if (!Parent.IsValid() || !Parent->TryGetObjectField(From.Last(), Group) || (*Group)->Values.Num() > 0)
			{
				break;
			}

			Parent->RemoveField(From.Last());
		}
	}

	/** One row over an Overrides object, whose keys are flat paths. */
	void RenameOverrides(FJsonObject& Overrides, const FRename& Row, TArray<FString>& Retired)
	{
		const FString Old = Row.Old;
		TArray<FString> Keys;
		Overrides.Values.GetKeys(Keys);

		for (const FString& Key : Keys)
		{
			if (Key != Old && !Key.StartsWith(Old + TEXT(".")))
			{
				continue;
			}

			const TSharedPtr<FJsonValue> Value = Overrides.TryGetField(Key);
			Overrides.RemoveField(Key);

			if (*Row.New == TEXT('\0'))
			{
				Retired.Add(Key);
			}
			else if (const FString Renamed = Row.New + Key.Mid(Old.Len()); !Overrides.HasField(Renamed))
			{
				Overrides.SetField(Renamed, Value);
			}
		}
	}

	/** A section's Values, or its Overrides when it has none, under the
	 *  current names, for a file at FileVersion. */
	void ApplyRenames(const FJsonObject& Section, TConstArrayView<FRename> Rows, const FString& Label, int32 FileVersion = MAX_int32)
	{
		const TSharedPtr<FJsonObject>* Values = nullptr;
		const TSharedPtr<FJsonObject>* Overrides = nullptr;

		if (!Section.TryGetObjectField(TEXT("Values"), Values))
		{
			Section.TryGetObjectField(TEXT("Overrides"), Overrides);
		}

		TArray<FString> Retired;

		for (const FRename& Row : Rows)
		{
			if (Row.Before > 0 && FileVersion >= Row.Before)
			{
				continue;
			}

			if (Values)
			{
				RenameValues(*Values, Row, Retired);
			}
			else if (Overrides)
			{
				RenameOverrides(**Overrides, Row, Retired);
			}
		}

		if (Retired.Num() > 0)
		{
			UE_LOG(LogAtmosphereDump, Display, TEXT("%s: %d retired member(s) not applied: %s"),
				*Label, Retired.Num(), *FString::Join(Retired, TEXT(", ")));
		}
	}

#if WITH_EDITOR
	/** Holds a number to its ClampMin and ClampMax, which the panel enforces and
	 *  a file does not. True when it moved. */
	bool ClampToMeta(const FProperty* Property, void* Value)
	{
		const FNumericProperty* Numeric = CastField<FNumericProperty>(Property);

		if (!Numeric || Property->ArrayDim != 1 || Numeric->IsEnum())
		{
			return false;
		}

		const FString& MinText = Property->GetMetaData(TEXT("ClampMin"));
		const FString& MaxText = Property->GetMetaData(TEXT("ClampMax"));

		if (MinText.IsEmpty() && MaxText.IsEmpty())
		{
			return false;
		}

		const double Was = Numeric->IsFloatingPoint()
			? Numeric->GetFloatingPointPropertyValue(Value)
			: (double)Numeric->GetSignedIntPropertyValue(Value);

		double Held = Was;

		if (!MinText.IsEmpty())
		{
			Held = FMath::Max(Held, FCString::Atod(*MinText));
		}

		if (!MaxText.IsEmpty())
		{
			Held = FMath::Min(Held, FCString::Atod(*MaxText));
		}

		if (Held == Was)
		{
			return false;
		}

		if (Numeric->IsFloatingPoint())
		{
			Numeric->SetFloatingPointPropertyValue(Value, Held);
		}
		else
		{
			Numeric->SetIntPropertyValue(Value, (int64)Held);
		}

		return true;
	}
#endif

	/** Members the panel edits, less those kept only to load old data. */
	bool Loadable(const FProperty* Property)
	{
		return Property->HasAnyPropertyFlags(CPF_Edit)
			&& !Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient | CPF_EditConst);
	}

	/** An object reference from its dumped path. False, leaving Out alone, when
	 *  the asset does not exist here. */
	bool ResolveObject(const FObjectPropertyBase* Property, const FString& Text, UObject*& Out)
	{
		if (Text.IsEmpty() || Text == TEXT("None"))
		{
			Out = nullptr;
			return true;
		}

		const FString Path = FPackageName::ExportTextPathToObjectPath(Text);
		UObject* Object = LoadObject<UObject>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);

		if (!Object || !Object->IsA(Property->PropertyClass))
		{
			return false;
		}

		Out = Object;
		return true;
	}

	/** One member from its JSON value. Parsed into a scratch copy, so a value
	 *  that fails to parse or names a missing asset leaves the member as it
	 *  was. */
	void SetValue(FProperty* Property, void* Value, const TSharedPtr<FJsonValue>& Json, const FString& Name, FReport& Report)
	{
		void* Scratch = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
		Property->InitializeValue(Scratch);
		Property->CopyCompleteValue(Scratch, Value);

		bool bParsed;

		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property);
			Object && Property->ArrayDim == 1 && Json.IsValid() && Json->Type == EJson::String)
		{
			UObject* Resolved = nullptr;
			bParsed = ResolveObject(Object, Json->AsString(), Resolved);

			if (bParsed)
			{
				Object->SetObjectPropertyValue(Scratch, Resolved);
			}
			else
			{
				Report.Skipped.Add(FString::Printf(TEXT("%s (no asset %s here)"), *Name, *Json->AsString()));
			}
		}
		else
		{
			bParsed = FJsonObjectConverter::JsonValueToUProperty(Json, Property, Scratch);

			if (!bParsed)
			{
				Report.Skipped.Add(Name + TEXT(" (unreadable value)"));
			}
		}

#if WITH_EDITOR
		if (bParsed && ClampToMeta(Property, Scratch))
		{
			Report.Clamped.Add(Name);
		}
#endif

		if (bParsed && !Property->Identical(Value, Scratch))
		{
			Property->CopyCompleteValue(Value, Scratch);
			++Report.Applied;
		}
		else if (bParsed)
		{
			++Report.Unchanged;
		}

		Property->DestroyValue(Scratch);
		FMemory::Free(Scratch);
	}

	/** A member of Type by name, or null with the reason recorded. */
	FProperty* Find(const UStruct* Type, const FString& Key, bool bTop, const FString& Name, FReport& Report)
	{
		FProperty* Property = FindFProperty<FProperty>(Type, *Key);

		if (!Property || (bTop ? !Loadable(Property) : Property->HasAnyPropertyFlags(CPF_Deprecated)))
		{
			Report.Skipped.Add(Name + (Property ? TEXT(" (not editable)") : TEXT(" (unknown)")));
			return nullptr;
		}

		if (bTop && Report.Excluded && Report.Excluded->Contains(Property->GetFName()))
		{
			Report.Skipped.Add(Name + TEXT(" (pipeline; pass Pipeline to apply)"));
			return nullptr;
		}

		return Property;
	}

	/** A Values object: parameter groups member by member, so a file may carry
	 *  any subset of a group; everything else as a whole. */
	void ApplyValues(const UStruct* Type, void* Container, const FJsonObject& Json, const FString& Prefix, bool bTop, FReport& Report)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Json.Values)
		{
			const FString Name = Prefix + Field.Key;
			FProperty* Property = Find(Type, Field.Key, bTop, Name, Report);

			if (!Property)
			{
				continue;
			}

			void* Value = Property->ContainerPtrToValuePtr<void>(Container);

			if (IsGroup(Property) && Field.Value.IsValid() && Field.Value->Type == EJson::Object)
			{
				ApplyValues(CastFieldChecked<FStructProperty>(Property)->Struct, Value, *Field.Value->AsObject(), Name + TEXT("."), false, Report);
			}
			else
			{
				SetValue(Property, Value, Field.Value, Name, Report);
			}
		}
	}

	/** An Overrides object: "Group.Member" keys, each holding its Value. */
	void ApplyOverrides(const UStruct* Type, void* Container, const FJsonObject& Json, FReport& Report)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Json.Values)
		{
			TArray<FString> Parts;
			Field.Key.ParseIntoArray(Parts, TEXT("."));

			const TSharedPtr<FJsonObject>* Pair = nullptr;

			if (Parts.Num() == 0 || !Field.Value.IsValid() || !Field.Value->TryGetObject(Pair) || !(*Pair)->HasField(TEXT("Value")))
			{
				Report.Skipped.Add(Field.Key + TEXT(" (no Value)"));
				continue;
			}

			const UStruct* Scope = Type;
			void* Owner = Container;
			FProperty* Property = nullptr;

			for (int32 i = 0; i < Parts.Num(); ++i)
			{
				Property = Find(Scope, Parts[i], i == 0, Field.Key, Report);

				if (!Property || i + 1 == Parts.Num())
				{
					break;
				}

				if (!IsGroup(Property))
				{
					Report.Skipped.Add(Field.Key + TEXT(" (not a group)"));
					Property = nullptr;
					break;
				}

				Owner = Property->ContainerPtrToValuePtr<void>(Owner);
				Scope = CastFieldChecked<FStructProperty>(Property)->Struct;
			}

			if (Property)
			{
				SetValue(Property, Property->ContainerPtrToValuePtr<void>(Owner), (*Pair)->TryGetField(TEXT("Value")), Field.Key, Report);
			}
		}
	}

	/** One section onto one object: its Values when the section has them, the
	 *  full state, otherwise its Overrides of the C++ defaults over what the
	 *  object already holds. */
	void ApplySection(UObject& Target, const UStruct* Type, const FJsonObject& Section, const FString& Label,
		const TSet<FName>* Excluded)
	{
		const TSharedPtr<FJsonObject>* Values = nullptr;
		const TSharedPtr<FJsonObject>* Overrides = nullptr;

		if (!Section.TryGetObjectField(TEXT("Values"), Values) && !Section.TryGetObjectField(TEXT("Overrides"), Overrides))
		{
			UE_LOG(LogAtmosphereDump, Warning, TEXT("%s: no Values or Overrides."), *Label);
			return;
		}

#if WITH_EDITOR
		Target.Modify();
#endif

		FReport Report;
		Report.Excluded = Excluded;

		if (Values)
		{
			ApplyValues(Type, &Target, **Values, FString(), true, Report);
		}
		else
		{
			ApplyOverrides(Type, &Target, **Overrides, Report);
		}

#if WITH_EDITOR
		Target.PostEditChange();
#endif
		Target.MarkPackageDirty();

		UE_LOG(LogAtmosphereDump, Display, TEXT("%s: applied %d value(s) from %s, %d already matched, %d skipped."),
			*Label, Report.Applied, Values ? TEXT("Values") : TEXT("Overrides"), Report.Unchanged, Report.Skipped.Num());

		for (const FString& Skipped : Report.Skipped)
		{
			UE_LOG(LogAtmosphereDump, Warning, TEXT("  skipped %s"), *Skipped);
		}

		for (const FString& Clamped : Report.Clamped)
		{
			UE_LOG(LogAtmosphereDump, Warning, TEXT("  clamped %s to its range"), *Clamped);
		}
	}

	/** The file as given, or under Saved/CloudAtmosphere, with or without its
	 *  extension. */
	FString Locate(FString Name)
	{
		if (!Name.EndsWith(TEXT(".json")))
		{
			Name += TEXT(".json");
		}

		if (FPaths::FileExists(Name))
		{
			return Name;
		}

		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CloudAtmosphere"), Name));
	}

	/** Each atmosphere in the file onto the world's actor of the same name, or
	 *  onto the only actor when both hold exactly one. Returns the actor whose
	 *  entry was dumped driving the sim (DrivesSim, else SimConfigIsRunning),
	 *  else the first written, or null: the Sim section is that one's config. */
	APlanetAtmosphereActor* ApplyAtmospheres(const TArray<TSharedPtr<FJsonValue>>& Entries, UWorld& World, const TSet<FName>* Excluded)
	{
		APlanetAtmosphereActor* First = nullptr;
		APlanetAtmosphereActor* SimActor = nullptr;
		APlanetAtmosphereActor* Driver = nullptr;

		TArray<APlanetAtmosphereActor*> Actors;

		for (TActorIterator<APlanetAtmosphereActor> It(&World); It; ++It)
		{
			Actors.Add(*It);
		}

		for (const TSharedPtr<FJsonValue>& Entry : Entries)
		{
			const TSharedPtr<FJsonObject>* Section = nullptr;

			if (!Entry.IsValid() || !Entry->TryGetObject(Section))
			{
				continue;
			}

			FString Name;
			(*Section)->TryGetStringField(TEXT("Actor"), Name);

			APlanetAtmosphereActor* const* Match = Actors.FindByPredicate(
				[&Name](const APlanetAtmosphereActor* Actor) { return Actor->GetActorNameOrLabel() == Name; });

			APlanetAtmosphereActor* Target = Match ? *Match : (Actors.Num() == 1 && Entries.Num() == 1 ? Actors[0] : nullptr);

			if (!Target)
			{
				UE_LOG(LogAtmosphereDump, Warning, TEXT("No atmosphere actor named '%s' in this world."), *Name);
				continue;
			}

			const FString Label = FString::Printf(TEXT("Atmosphere '%s'"), *Target->GetActorNameOrLabel());

			ApplyRenames(**Section, AtmosphereRenames(), Label);
			ApplySection(*Target, APlanetAtmosphereActor::StaticClass(), **Section, Label, Excluded);

			bool bDrove = false;
			bool bRanSim = false;
			(*Section)->TryGetBoolField(TEXT("DrivesSim"), bDrove);
			(*Section)->TryGetBoolField(TEXT("SimConfigIsRunning"), bRanSim);

			Driver = (bDrove && !Driver) ? Target : Driver;
			SimActor = (bRanSim && !SimActor) ? Target : SimActor;
			First = First ? First : Target;
		}

		return Driver ? Driver : SimActor ? SimActor : First;
	}

	void Load(const TArray<FString>& Args, UWorld* World)
	{
		if (Args.Num() == 0 || !World)
		{
			UE_LOG(LogAtmosphereDump, Error, TEXT("Usage: CloudAtmosphere.LoadParams FileName [Sim|Atmospheres] [Pipeline]"));
			return;
		}

		const FString Path = Locate(Args[0]);

		FString Scope;
		bool bPipeline = false;

		for (int32 i = 1; i < Args.Num(); ++i)
		{
			if (Args[i].Equals(TEXT("Pipeline"), ESearchCase::IgnoreCase))
			{
				bPipeline = true;
			}
			else
			{
				Scope = Args[i];
			}
		}

		const bool bSim = Scope.IsEmpty() || Scope.Equals(TEXT("Sim"), ESearchCase::IgnoreCase);
		const bool bAtmospheres = Scope.IsEmpty() || Scope.Equals(TEXT("Atmospheres"), ESearchCase::IgnoreCase);

		FString Text;
		TSharedPtr<FJsonObject> Root;

		if (!FFileHelper::LoadFileToString(Text, *Path)
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			UE_LOG(LogAtmosphereDump, Error, TEXT("Could not read %s"), *Path);
			return;
		}

		// THE ATMOSPHERES FIRST: the Sim section belongs to the config of the
		// model the file sets, which its PlanetType has to select before the
		// section is applied.
		const TArray<TSharedPtr<FJsonValue>>* Atmospheres = nullptr;
		APlanetAtmosphereActor* SimOwner = nullptr;

		if (bAtmospheres && Root->TryGetArrayField(TEXT("Atmospheres"), Atmospheres))
		{
			SimOwner = ApplyAtmospheres(*Atmospheres, *World, bPipeline ? nullptr : &ActorPipeline());
		}

		const TSharedPtr<FJsonObject>* Sim = nullptr;

		if (bSim && Root->TryGetObjectField(TEXT("Sim"), Sim))
		{
			const UFlowSimSubsystem* Sub = World->GetSubsystem<UFlowSimSubsystem>();
			UFlowSimConfig* Config = SimOwner ? SimOwner->ActiveSimConfig() : (Sub ? Sub->GetConfig() : nullptr);

			// NO CONVERSION ON LOAD. A file from an older config version applies its
			// values under today's meanings; renamed members are read under their
			// current names (SimRenames).
			double FileVersion = -1.0;
			const int32 Current = AtmosphereDump::ConfigVersion();

			if (!(*Sim)->TryGetNumberField(TEXT("ConfigVersion"), FileVersion))
			{
				UE_LOG(LogAtmosphereDump, Warning,
					TEXT("The Sim section records no ConfigVersion; values written before a conversion apply under the current meanings."));
			}
			else if ((int32)FileVersion < Current)
			{
				UE_LOG(LogAtmosphereDump, Warning,
					TEXT("The Sim section is config version %d, the config is %d; values written before a conversion apply under the current meanings."),
					(int32)FileVersion, Current);
			}

			ApplyRenames(**Sim, SimRenames(), TEXT("Sim section"), FileVersion < 0.0 ? Current : (int32)FileVersion);

			if (Config)
			{
				ApplySection(*Config, UFlowSimConfig::StaticClass(), **Sim, FString::Printf(TEXT("Sim config '%s'"), *Config->GetName()),
					bPipeline ? nullptr : &SimPipeline());
			}
			else
			{
				UE_LOG(LogAtmosphereDump, Warning, TEXT("No sim config for the file's atmosphere and none running, so the Sim section is not applied."));
			}
		}

		UE_LOG(LogAtmosphereDump, Display, TEXT("Loaded %s. Save the changed assets to keep the values."), *Path);
	}
}

static FAutoConsoleCommandWithWorldAndArgs GAtmosphereDumpParamsCmd(
	TEXT("CloudAtmosphere.DumpParams"),
	TEXT("Write the running sim config, the sim settings and every atmosphere actor's parameters, ")
	TEXT("each with its overrides of the C++ defaults, to Saved/CloudAtmosphere. Optional argument is the file name."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AtmosphereDump::Dump));

static FAutoConsoleCommandWithWorldAndArgs GAtmosphereLoadParamsCmd(
	TEXT("CloudAtmosphere.LoadParams"),
	TEXT("Apply a parameter file in DumpParams' layout to the running sim config and the world's atmosphere actors: ")
	TEXT("each section's Values, or its Overrides when it has no Values. Any subset of members may be given. ")
	TEXT("File from Saved/CloudAtmosphere or a full path; optional Sim or Atmospheres limits it. Assets, targets, ")
	TEXT("debug views and start state are left alone unless Pipeline is given; numbers are held to their ranges; ")
	TEXT("members written under a former name are read under the current one."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AtmosphereLoad::Load));
