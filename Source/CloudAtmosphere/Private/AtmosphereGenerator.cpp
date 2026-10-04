#include "AtmosphereGenerator.h"

#include "AtmosphereArchetype.h"
#include "AtmospherePreset.h"
#include "FlowSimTypes.h"
#include "FlowSnapshot.h"
#include "Hash/CityHash.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include <cmath>

DEFINE_LOG_CATEGORY(LogAtmosphereHarness);

using namespace AtmosphereHarness;

namespace
{
	/** SplitMix64's finaliser: every input bit reaches every output bit. */
	uint64 Mix(uint64 Z)
	{
		Z += 0x9E3779B97F4A7C15ull;
		Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
		Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
		return Z ^ (Z >> 31);
	}

	/** Hashed as UTF-8, so a key hashes alike whatever TCHAR is. */
	uint64 KeyOf(FStringView Key)
	{
		const FTCHARToUTF8 Utf8(Key.GetData(), Key.Len());
		return CityHash64(Utf8.Get(), (uint32)Utf8.Length());
	}

	/** The standard normal's distribution function. */
	double Phi(double X)
	{
		return 0.5 * std::erfc(-X / 1.4142135623730951);
	}

	/** Its inverse, by Acklam's rational approximation: relative error under
	 *  1.2e-9 over (0, 1). */
	double Probit(double P)
	{
		static constexpr double A[6] = { -3.969683028665376e+01, 2.209460984245205e+02, -2.759285104469687e+02,
			1.383577518672690e+02, -3.066479806614716e+01, 2.506628277459239e+00 };
		static constexpr double B[5] = { -5.447609879822406e+01, 1.615858368580409e+02, -1.556989798598866e+02,
			6.680131188771972e+01, -1.328068155288572e+01 };
		static constexpr double C[6] = { -7.784894002430293e-03, -3.223964580411365e-01, -2.400758277161838e+00,
			-2.549732539343734e+00, 4.374664141464968e+00, 2.938163982698783e+00 };
		static constexpr double D[4] = { 7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00,
			3.754408661907416e+00 };

		constexpr double Low = 0.02425;

		const auto Tail = [](double Q)
			{
				return (((((C[0] * Q + C[1]) * Q + C[2]) * Q + C[3]) * Q + C[4]) * Q + C[5])
					/ ((((D[0] * Q + D[1]) * Q + D[2]) * Q + D[3]) * Q + 1.0);
			};

		if (P < Low)
		{
			return Tail(std::sqrt(-2.0 * std::log(P)));
		}

		if (P > 1.0 - Low)
		{
			return -Tail(std::sqrt(-2.0 * std::log(1.0 - P)));
		}

		const double Q = P - 0.5;
		const double R = Q * Q;

		return (((((A[0] * R + A[1]) * R + A[2]) * R + A[3]) * R + A[4]) * R + A[5]) * Q
			/ (((((B[0] * R + B[1]) * R + B[2]) * R + B[3]) * R + B[4]) * R + 1.0);
	}

	/** A setting a path names: its property and where its value lives. */
	struct FResolved
	{
		FProperty* Property = nullptr;
		void* Address = nullptr;
	};

	/** Member names separated by dots, an array member indexed as Name[i]. */
	FResolved Resolve(const UStruct* Type, void* Container, FStringView Path)
	{
		TArray<FString> Tokens;
		FString(Path.Len(), Path.GetData()).ParseIntoArray(Tokens, TEXT("."));

		for (int32 i = 0; i < Tokens.Num() && Type; ++i)
		{
			FString Name = Tokens[i];
			int32 Index = INDEX_NONE;
			int32 Open = INDEX_NONE;

			if (Name.FindChar(TEXT('['), Open) && Name.EndsWith(TEXT("]")))
			{
				LexFromString(Index, *Name.Mid(Open + 1, Name.Len() - Open - 2));
				Name.LeftInline(Open);
			}

			FProperty* Property = FindFProperty<FProperty>(Type, FName(*Name));

			if (!Property)
			{
				return FResolved();
			}

			void* Address = Property->ContainerPtrToValuePtr<void>(Container);

			if (Open != INDEX_NONE)
			{
				FArrayProperty* Array = CastField<FArrayProperty>(Property);

				if (!Array)
				{
					return FResolved();
				}

				FScriptArrayHelper Helper(Array, Address);

				if (!Helper.IsValidIndex(Index))
				{
					return FResolved();
				}

				Address = Helper.GetRawPtr(Index);
				Property = Array->Inner;
			}

			if (i + 1 == Tokens.Num())
			{
				return FResolved{ Property, Address };
			}

			const FStructProperty* Struct = CastField<FStructProperty>(Property);
			Type = Struct ? Struct->Struct : nullptr;
			Container = Address;
		}

		return FResolved();
	}

	FNumericProperty* NumericOf(FProperty* Property)
	{
		if (FNumericProperty* Numeric = CastField<FNumericProperty>(Property))
		{
			return Numeric;
		}

		const FEnumProperty* Enum = CastField<FEnumProperty>(Property);
		return Enum ? Enum->GetUnderlyingProperty() : nullptr;
	}

	bool Read(const FResolved& Setting, double& Out)
	{
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(Setting.Property))
		{
			Out = Bool->GetPropertyValue(Setting.Address) ? 1.0 : 0.0;
			return true;
		}

		const FNumericProperty* Numeric = NumericOf(Setting.Property);

		if (!Numeric)
		{
			return false;
		}

		Out = Numeric->IsFloatingPoint()
			? Numeric->GetFloatingPointPropertyValue(Setting.Address)
			: (double)Numeric->GetSignedIntPropertyValue(Setting.Address);
		return true;
	}

	bool Write(const FResolved& Setting, double Value)
	{
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(Setting.Property))
		{
			Bool->SetPropertyValue(Setting.Address, Value >= 0.5);
			return true;
		}

		const FNumericProperty* Numeric = NumericOf(Setting.Property);

		if (!Numeric)
		{
			return false;
		}

		if (Numeric->IsFloatingPoint())
		{
			Numeric->SetFloatingPointPropertyValue(Setting.Address, Value);
		}
		else
		{
			// A byte wraps rather than saturating.
			const double Whole = FMath::RoundToDouble(Value);
			Numeric->SetIntPropertyValue(Setting.Address,
				(int64)(CastField<FByteProperty>(Numeric) ? FMath::Clamp(Whole, 0.0, 255.0) : Whole));
		}

		return true;
	}

	/** A choice value onto a setting: a struct's numeric members take its
	 *  channels in declaration order, a scalar takes R. */
	bool WriteChannels(const FResolved& Setting, const FLinearColor& Value)
	{
		const FStructProperty* Struct = CastField<FStructProperty>(Setting.Property);

		if (!Struct)
		{
			return Write(Setting, Value.R);
		}

		const float Channels[4] = { Value.R, Value.G, Value.B, Value.A };
		int32 Channel = 0;

		for (TFieldIterator<FProperty> It(Struct->Struct); It && Channel < 4; ++It)
		{
			if (FNumericProperty* Numeric = CastField<FNumericProperty>(*It))
			{
				Write(FResolved{ Numeric, Numeric->ContainerPtrToValuePtr<void>(Setting.Address) }, Channels[Channel++]);
			}
		}

		return Channel > 0;
	}

	FString Describe(const FResolved& Setting)
	{
		FString Text;
		Setting.Property->ExportText_Direct(Text, Setting.Address, Setting.Address, nullptr, PPF_None);
		return Text;
	}

	/** One root the draws write: the bundle or the sim config. */
	struct FDrawTarget
	{
		int32 Seed = 0;
		EStream Stream = EStream::Look;
		const UStruct* Type = nullptr;
		void* Container = nullptr;
		const TCHAR* Prefix = TEXT("");
	};

	void Record(TArray<FAtmosphereDrawRecord>& Report, const FDrawTarget& Target, const FString& Path,
		const FString& Value, EAtmosphereDrawSource Source)
	{
		FAtmosphereDrawRecord& Line = Report.AddDefaulted_GetRef();
		Line.Path = Target.Prefix + Path;
		Line.Value = Value;
		Line.Source = Source;
	}

	/** The palettes: an option per group, its R, G and B scaled by one factor
	 *  and mutated per path and channel. A swept group shows its options as
	 *  authored. */
	void ApplyChoices(const FDrawTarget& Target, const FAtmosphereDrawSet& Set, const FAtmosphereGenerateOptions& Options,
		TArray<FAtmosphereDrawRecord>& Report)
	{
		for (const FAtmosphereChoiceGroup& Group : Set.Choices)
		{
			int32 Count = Group.Colours.Num() > 0 ? MAX_int32 : 0;

			for (const FAtmospherePaletteColour& Colour : Group.Colours)
			{
				Count = FMath::Min(Count, Colour.Options.Num());
			}

			if (Count == 0)
			{
				continue;
			}

			const FString Key = Group.Name.ToString();
			const bool bSwept = !Options.SweepPath.IsEmpty() && Options.SweepPath == Key;

			const double Pick = bSwept ? (double)Options.SweepPosition : Unit(Target.Seed, Target.Stream, TEXT("Choice/") + Key);
			const int32 Index = FMath::Clamp((int32)(Pick * Count), 0, Count - 1);

			FAtmosphereDraw Brightness;
			Brightness.Distribution = EAtmosphereDrawDistribution::LogUniform;
			Brightness.Min = Group.ScaleMin;
			Brightness.Max = Group.ScaleMax;

			const double Scale = bSwept ? 1.0 : AtmosphereHarness::Sample(Brightness, Unit(Target.Seed, Target.Stream, TEXT("Scale/") + Key), 1.0);

			for (const FAtmospherePaletteColour& Colour : Group.Colours)
			{
				const FString& Path = Colour.Path;

				if (Options.Locks.Contains(Path))
				{
					continue;
				}

				FLinearColor Value = Colour.Options[Index];
				float* Channels[3] = { &Value.R, &Value.G, &Value.B };

				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					const double Move = (bSwept || Group.Mutation <= 0.0f) ? 0.0
						: (2.0 * Unit(Target.Seed, Target.Stream, TEXT("Mutate/") + Path, Channel) - 1.0) * Group.Mutation;

					*Channels[Channel] = (float)FMath::Max((double)*Channels[Channel] * Scale * (1.0 + Move), 0.0);
				}

				const FResolved Setting = Resolve(Target.Type, Target.Container, Path);

				if (!Setting.Property || !WriteChannels(Setting, Value))
				{
					Record(Report, Target, Path, FString(), EAtmosphereDrawSource::Missing);
					continue;
				}

				Record(Report, Target, Path, Describe(Setting), EAtmosphereDrawSource::Choice);
			}
		}
	}

	void ApplyDraws(const FDrawTarget& Target, const FAtmosphereDrawSet& Set, const FAtmosphereGenerateOptions& Options,
		TArray<FAtmosphereDrawRecord>& Report)
	{
		ApplyChoices(Target, Set, Options, Report);

		const TArray<FString>& Locks = Options.Locks;

		for (const FAtmosphereDraw& Entry : Set.Draws)
		{
			if (Entry.Distribution == EAtmosphereDrawDistribution::Hold || Locks.Contains(Entry.Path))
			{
				continue;
			}

			const FResolved Setting = Resolve(Target.Type, Target.Container, Entry.Path);
			double Base = 0.0;

			if (!Setting.Property || !Read(Setting, Base))
			{
				Record(Report, Target, Entry.Path, FString(), EAtmosphereDrawSource::Missing);
				continue;
			}

			const double Drawn = Entry.Link.IsNone()
				? Unit(Target.Seed, Target.Stream, Entry.Path)
				: Unit(Target.Seed, EStream::Link, Entry.Link.ToString());

			const double Position = (!Options.SweepPath.IsEmpty() && Entry.Path == Options.SweepPath)
				? (double)Options.SweepPosition
				: (Entry.bInvert ? 1.0 - Drawn : Drawn);

			Write(Setting, Sample(Entry, Position, Base));
			Record(Report, Target, Entry.Path, Describe(Setting), EAtmosphereDrawSource::Draw);
		}
	}

	/** Base with Override's entries replacing those on the same path, and its
	 *  groups those of the same name. */
	FAtmosphereDrawSet Merge(const FAtmosphereDrawSet& Base, const FAtmosphereDrawSet& Override)
	{
		FAtmosphereDrawSet Out = Base;

		for (const FAtmosphereDraw& Entry : Override.Draws)
		{
			if (FAtmosphereDraw* Same = Out.Draws.FindByPredicate([&Entry](const FAtmosphereDraw& Other) { return Other.Path == Entry.Path; }))
			{
				*Same = Entry;
			}
			else
			{
				Out.Draws.Add(Entry);
			}
		}

		for (const FAtmosphereChoiceGroup& Group : Override.Choices)
		{
			if (FAtmosphereChoiceGroup* Same = Out.Choices.FindByPredicate([&Group](const FAtmosphereChoiceGroup& Other) { return Other.Name == Group.Name; }))
			{
				*Same = Group;
			}
			else
			{
				Out.Choices.Add(Group);
			}
		}

		return Out;
	}
}

namespace
{
	void CollectLookSettings(const UStruct* Type, const FString& Prefix, TArray<FString>& Numbers, TArray<FString>& Colours)
	{
		for (TFieldIterator<FProperty> It(Type); It; ++It)
		{
			const FProperty* Property = *It;

			if (!Property->HasAnyPropertyFlags(CPF_Edit)
				|| Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_EditConst))
			{
				continue;
			}

			const FString Path = Prefix + Property->GetName();

			if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
			{
				if (Struct->Struct == TBaseStructure<FLinearColor>::Get())
				{
					Colours.Add(Path);
				}
				else
				{
					CollectLookSettings(Struct->Struct, Path + TEXT("."), Numbers, Colours);
				}
			}
			else if (const FNumericProperty* Numeric = CastField<FNumericProperty>(Property); Numeric && !Numeric->IsEnum())
			{
				Numbers.Add(Path);
			}
		}
	}
}

bool AtmosphereHarness::ReadColour(const UStruct* Type, const void* Container, FStringView Path, FLinearColor& Out)
{
	const FResolved Setting = Resolve(Type, const_cast<void*>(Container), Path);
	const FStructProperty* Struct = CastField<FStructProperty>(Setting.Property);

	if (!Struct || Struct->Struct != TBaseStructure<FLinearColor>::Get())
	{
		return false;
	}

	Out = *static_cast<const FLinearColor*>(Setting.Address);
	return true;
}

void AtmosphereHarness::LookSettings(EPlanetAtmosphereType Model, TArray<FString>& OutNumbers, TArray<FString>& OutColours)
{
	OutNumbers.Reset();
	OutColours.Reset();
	CollectLookSettings(FAtmosphereModelParams::StaticStruct(), FString(), OutNumbers, OutColours);

	const auto Excluded = [Model](const FString& Path)
		{
			const bool bQuality = Path == TEXT("MultipleScattering.OctaveCount") || Path.StartsWith(TEXT("SurfaceShadow.CascadeRadii."));
			const bool bAir = Path.StartsWith(TEXT("Planet.")) || Path.StartsWith(TEXT("Air.")) || Path.StartsWith(TEXT("Ambient."));

			return bQuality
				|| (Path.StartsWith(TEXT("Deep.")) && Model != EPlanetAtmosphereType::GasGiant)
				|| (!bAir && Model == EPlanetAtmosphereType::AirOnly);
		};

	OutNumbers.RemoveAll(Excluded);
	OutColours.RemoveAll(Excluded);
}

double AtmosphereHarness::Unit(int32 Seed, EStream Stream, FStringView Key, uint32 Sub)
{
	uint64 Hash = Mix((uint64)(uint32)Seed);
	Hash = Mix(Hash ^ (uint64)Stream);
	Hash = Mix(Hash ^ KeyOf(Key));
	Hash = Mix(Hash ^ (uint64)Sub);

	// The top 53 bits, exactly representable.
	return (double)(Hash >> 11) * (1.0 / 9007199254740992.0);
}

double AtmosphereHarness::Sample(const FAtmosphereDraw& Entry, double U, double Base)
{
	U = FMath::Clamp(U, 0.0, 1.0);

	const double Min = Entry.Min;
	const double Max = Entry.Max;

	switch (Entry.Distribution)
	{
	case EAtmosphereDrawDistribution::Uniform:
		return FMath::Lerp(Min, Max, U);

	case EAtmosphereDrawDistribution::Normal:
	{
		// Through the distribution function, so the cut keeps the bell's shape
		// and one draw maps monotonically.
		const double Sigma = Entry.Sigma;
		const double Lowest = FMath::Min(Min, Max);
		const double Highest = FMath::Max(Min, Max);

		if (Sigma <= 0.0)
		{
			return FMath::Clamp((double)Entry.Mean, Lowest, Highest);
		}

		const double P = FMath::Lerp(Phi((Min - Entry.Mean) / Sigma), Phi((Max - Entry.Mean) / Sigma), U);
		return FMath::Clamp(Entry.Mean + Sigma * Probit(FMath::Clamp(P, 1e-12, 1.0 - 1e-12)), Lowest, Highest);
	}

	case EAtmosphereDrawDistribution::LogUniform:
		return (Min > 0.0 && Max > 0.0) ? std::exp(FMath::Lerp(std::log(Min), std::log(Max), U)) : FMath::Lerp(Min, Max, U);

	case EAtmosphereDrawDistribution::Jitter:
	{
		const double Move = (2.0 * U - 1.0) * Entry.Amount;
		return Entry.bRelative ? Base * (1.0 + Move) : Base + Move;
	}

	case EAtmosphereDrawDistribution::Choice:
	{
		const int32 Count = Entry.Values.Num();
		return Count ? (double)Entry.Values[FMath::Min((int32)(U * Count), Count - 1)] : Base;
	}

	default:
		return Base;
	}
}

bool AtmosphereHarness::ReadValue(const UStruct* Type, const void* Container, FStringView Path, double& Out)
{
	const FResolved Setting = Resolve(Type, const_cast<void*>(Container), Path);
	return Setting.Property && Read(Setting, Out);
}

bool AtmosphereHarness::WriteValue(const UStruct* Type, void* Container, FStringView Path, double Value)
{
	const FResolved Setting = Resolve(Type, Container, Path);
	return Setting.Property && Write(Setting, Value);
}

UAtmosphereArchetype* FAtmosphereGenerator::PickArchetype(int32 Seed, EPlanetAtmosphereType Model, const FAtmosphereModelSet& Set)
{
	UAtmosphereArchetype* Best = nullptr;
	double BestScore = TNumericLimits<double>::Max();

	for (const FAtmosphereArchetypeEntry& Entry : Set.Archetypes)
	{
		UAtmosphereArchetype* Archetype = Entry.Archetype;

		if (!Entry.bEnabled || Entry.Weight <= 0.0f || !Archetype || Archetype->Model != Model || !Archetype->Template)
		{
			continue;
		}

		// An exponential draw over the weight: the lowest wins, with chance in
		// proportion to the weights, and each score depends on its own id alone.
		const double U = Unit(Seed, EStream::Pick, Archetype->ArchetypeId.ToString(EGuidFormats::Digits));
		const double Score = -std::log1p(-U) / Entry.Weight;

		if (Score < BestScore)
		{
			BestScore = Score;
			Best = Archetype;
		}
	}

	return Best;
}

bool FAtmosphereGenerator::Generate(int32 Seed, EPlanetAtmosphereType Model, const FAtmosphereGenerationSet& Set,
	const FAtmosphereModelParams& CurrentLook, const FAtmosphereGenerateOptions& Options, UObject* Outer,
	FAtmosphereGeneration& Out)
{
	Out = FAtmosphereGeneration();
	Out.Genome.Seed = Seed;
	Out.Genome.GeneratorVersion = GeneratorVersion;
	Out.Genome.Model = Model;

	const FAtmosphereModelSet* ModelSet = (Model == EPlanetAtmosphereType::GasGiant) ? &Set.GasGiant
		: (Model == EPlanetAtmosphereType::Terrestrial) ? &Set.Terrestrial : nullptr;

	const UAtmosphereLookProfile* Profile = ModelSet ? ModelSet->Look.Get() : Set.AirOnly.Get();
	UAtmosphereArchetype* Archetype = ModelSet ? PickArchetype(Seed, Model, *ModelSet) : nullptr;

	if (!Profile && !Archetype)
	{
		return false;
	}

	Out.Genome.Archetype = Archetype;

	if (ModelSet && !Archetype && Options.bSim)
	{
		UE_LOG(LogAtmosphereHarness, Warning,
			TEXT("Seed %d: the set offers %s no enabled archetype of that model with a Template, so no sim is generated or rolled; the actor's own config runs."),
			Seed, *UEnum::GetDisplayValueAsText(Model).ToString());
	}

	// -- The look -------------------------------------------------------------
	//
	// From the profile's Base, or else the actor's bundle: drawn paths are
	// overwritten either way, and undrawn ones keep the starting value.
	// PITFALL: from the actor's bundle, Jitter walks further on every
	// generation, and a path one archetype draws and another does not keeps
	// the last planet's value. A Base makes the whole look a function of the seed.

	if (Profile)
	{
		const UScriptStruct* LookType = FAtmosphereModelParams::StaticStruct();

		Out.Look = Profile->Base ? Profile->Base->Model : CurrentLook;
		Out.bHasLook = true;

		const FDrawTarget LookTarget{ Seed, EStream::Look, LookType, &Out.Look, TEXT("") };

		ApplyDraws(LookTarget, Merge(Profile->Draws, Archetype ? Archetype->LookDraws : FAtmosphereDrawSet()), Options, Out.Report);

		for (const FString& Path : Options.Locks)
		{
			const FResolved To = Resolve(LookType, &Out.Look, Path);
			const FResolved From = Resolve(LookType, const_cast<FAtmosphereModelParams*>(&CurrentLook), Path);

			if (To.Property && From.Property)
			{
				To.Property->CopyCompleteValue(To.Address, From.Address);
				Record(Out.Report, LookTarget, Path, Describe(To), EAtmosphereDrawSource::Lock);
			}
		}
	}

	// -- The sim --------------------------------------------------------------

	if (Archetype && Options.bSim)
	{
		GenerateSim(Seed, *Archetype, Options, Outer, Out);
	}

	const int32 Missing = Out.Report.FilterByPredicate(
		[](const FAtmosphereDrawRecord& Line) { return Line.Source == EAtmosphereDrawSource::Missing; }).Num();

	if (Missing > 0)
	{
		UE_LOG(LogAtmosphereHarness, Warning, TEXT("Seed %d: %d draw paths name no setting; the report lists them."), Seed, Missing);
	}

	return true;
}

void FAtmosphereGenerator::GenerateSim(int32 Seed, const UAtmosphereArchetype& Archetype, const FAtmosphereGenerateOptions& Options,
	UObject* Outer, FAtmosphereGeneration& Out)
{
	UObject* Owner = Outer ? Outer : GetTransientPackage();

	UFlowSimConfig* Config = DuplicateObject<UFlowSimConfig>(Archetype.Template.Get(), Owner,
		MakeUniqueObjectName(Owner, UFlowSimConfig::StaticClass(), Archetype.Template->GetFName()));

	// PITFALL: a duplicate keeps the asset's RF_Standalone, which the garbage
	// collector never frees.
	Config->ClearFlags(RF_Standalone | RF_Public);
	Config->SetFlags(RF_Transient);
	Out.Config = Config;

	const FDrawTarget SimTarget{ Seed, EStream::Sim, UFlowSimConfig::StaticClass(), Config, TEXT("Sim.") };

	ApplyDraws(SimTarget, Archetype.SimDraws, Options, Out.Report);

	for (const FString& Path : Options.Locks)
	{
		const FResolved Kept = Resolve(SimTarget.Type, Config, Path);

		if (Kept.Property)
		{
			Record(Out.Report, SimTarget, Path, Describe(Kept), EAtmosphereDrawSource::Lock);
		}
	}

	// The start state turned east, the perpetual storms with it, since they are
	// placed from the config rather than the state.
	const UFlowSnapshot* Snapshot = Archetype.Template->InitialState;

	if (!Snapshot)
	{
		UE_LOG(LogAtmosphereHarness, Warning, TEXT("Archetype '%s': Template '%s' has no InitialState; the start is not rolled."),
			*Archetype.GetName(), *Archetype.Template->GetName());
	}
	else
	{
		const int32 Width = Snapshot->Grid.X;
		const double Span = FMath::Clamp((double)Archetype.MaxRoll, 0.0, 360.0) / 360.0;
		const int32 Columns = (Width > 0) ? FMath::Min((int32)(Unit(Seed, EStream::Roll, TEXT("Roll")) * Span * Width), Width - 1) : 0;

		if (Columns == 0)
		{
			return;
		}

		if (UFlowSnapshot* Rolled = Snapshot->MakeRolled(Config, Columns))
		{
			const float Degrees = 360.0f * (float)Columns / (float)Width;

			Config->InitialState = Rolled;

			for (FFlowPerpetualStorm& Storm : Config->PerpetualStorms)
			{
				Storm.Longitude = (float)FRotator::NormalizeAxis((double)Storm.Longitude + Degrees);
			}

			Out.Genome.RollColumns = Columns;
			Out.Genome.RollDegrees = Degrees;
		}
		else
		{
			UE_LOG(LogAtmosphereHarness, Warning, TEXT("Archetype '%s': InitialState '%s' has no layout this solver restores; the start is not rolled."),
				*Archetype.GetName(), *Snapshot->GetName());
		}
	}
}
