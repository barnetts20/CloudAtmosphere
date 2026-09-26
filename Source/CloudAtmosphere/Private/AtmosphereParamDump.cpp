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
#include "Misc/Paths.h"
#include "PlanetAtmosphereActor.h"
#include "UObject/UnrealType.h"

#include <cstdio>
#include <cstdlib>

// CloudAtmosphere.DumpParams [FileName]
//
// Writes the running sim config, the sim settings and every atmosphere actor
// in the world to Saved/CloudAtmosphere as JSON. Each object carries its full
// Values and its Overrides: every member that differs from the C++ defaults,
// with both values, so the tuned assets can be told apart from the class
// defaults and read back as the source of new defaults and presets.

DEFINE_LOG_CATEGORY_STATIC(LogAtmosphereDump, Log, All);

namespace AtmosphereDump
{
	using FFilter = TFunctionRef<bool(const FProperty*)>;

	/** Every property but those kept only to load old data. */
	bool Current(const FProperty* Property)
	{
		return !Property->HasAnyPropertyFlags(CPF_Deprecated);
	}

	/** The actor's panel: edited members only, which leaves out the parked
	 *  occluder group and the transient readouts. */
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
		Derived->SetNumberField(TEXT("PressureScale"), Params.OutputScales.X);
		Derived->SetNumberField(TEXT("VorticityScale"), Params.OutputScales.Y);
		Derived->SetNumberField(TEXT("DivergenceScale"), Params.OutputScales.Z);
		Derived->SetNumberField(TEXT("NoiseDriftRate"), Params.NoiseDriftRate);
		Derived->SetNumberField(TEXT("NoiseResetTime"), Params.NoiseResetTime);
		Derived->SetNumberField(TEXT("AtlasFaceSize"), Params.AtlasFaceSize);
		Derived->SetStringField(TEXT("Grid"), FString::Printf(TEXT("%dx%dx%d"), Params.GridSize.X, Params.GridSize.Y, Layers));
		Derived->SetField(TEXT("LayerDepth"), Floats(MakeArrayView(Params.Stack.Depth, Layers)));
		Derived->SetField(TEXT("ModeWaveSpeed"), Floats(Speeds));
		Out->SetObjectField(TEXT("Derived"), Derived);

		Describe(UFlowSimConfig::StaticClass(), Config, GetDefault<UFlowSimConfig>(), Current, *Out);
		return Out;
	}

	TSharedRef<FJsonObject> DescribeActor(const APlanetAtmosphereActor& Actor, const UFlowSimConfig* Running)
	{
		// Against the native class defaults, the values the audit's class-default
		// calls were made at, whatever Blueprint subclass the actor is.
		const UClass* Native = APlanetAtmosphereActor::StaticClass();
		const UFlowSimConfig* Own = Actor.Simulation.Config.Get();

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("Actor"), Actor.GetActorNameOrLabel());
		Out->SetStringField(TEXT("Class"), Actor.GetClass()->GetPathName());
		Out->SetStringField(TEXT("SimConfig"), PathOf(Own));
		Out->SetBoolField(TEXT("SimConfigIsRunning"), Running && Own == Running);

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
			Actors.Add(MakeShared<FJsonValueObject>(DescribeActor(**It, Sub->GetConfig())));
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

static FAutoConsoleCommandWithWorldAndArgs GAtmosphereDumpParamsCmd(
	TEXT("CloudAtmosphere.DumpParams"),
	TEXT("Write the running sim config, the sim settings and every atmosphere actor's parameters, ")
	TEXT("each with its overrides of the C++ defaults, to Saved/CloudAtmosphere. Optional argument is the file name."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&AtmosphereDump::Dump));