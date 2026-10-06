// FlowSim.Capture: the running config and its live state saved as a new
// config, a snapshot and optionally an archetype, in one step. Editor only.
//
//   FlowSim.Capture Name [/Game/Folder] [Terrestrial|GasGiant]
//
// Writes Name_Sim (the config, its InitialState the snapshot) and Name_State
// into Folder (default /Game/FlowSimCaptures); with a model, also the archetype
// Name over them. Arguments go in any order: a path starts with '/', a model is
// a planet type, anything else is the name. Existing assets are never replaced.

#include "FlowSimSubsystem.h"

#if WITH_EDITOR

#include "AtmosphereArchetype.h"
#include "AtmosphereParams.h"
#include "FlowSimTypes.h"
#include "FlowSimulation.h"
#include "FlowSnapshot.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace FlowSimCapture
{
	static const TCHAR* const DefaultFolder = TEXT("/Game/FlowSimCaptures");

	/** A free package path for Name in Folder, or empty with the reason logged. */
	FString FreePackage(const FString& Folder, const FString& Name)
	{
		const FString PackageName = Folder / Name;
		FText Reason;

		if (!FPackageName::IsValidLongPackageName(PackageName, false, &Reason))
		{
			UE_LOG(LogFlowSim, Error, TEXT("FlowSim.Capture: '%s' is not a valid asset path: %s"), *PackageName, *Reason.ToString());
			return FString();
		}

		if (FindPackage(nullptr, *PackageName) || FPackageName::DoesPackageExist(PackageName))
		{
			UE_LOG(LogFlowSim, Error, TEXT("FlowSim.Capture: '%s' already exists; pick another name."), *PackageName);
			return FString();
		}

		return PackageName;
	}

	/** Registers a new asset and writes its package to disk. */
	bool Save(UObject* Asset)
	{
		UPackage* Package = Asset->GetPackage();
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();

		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;

		if (!UPackage::SavePackage(Package, Asset, *File, Args))
		{
			UE_LOG(LogFlowSim, Error, TEXT("FlowSim.Capture: could not save '%s'; it is left dirty in the editor."), *Package->GetName());
			return false;
		}

		return true;
	}

	/** Drops a half-made asset so its name is free again. */
	void Discard(UObject* Asset)
	{
		if (Asset)
		{
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->MarkAsGarbage();
		}
	}

	void Run(const TArray<FString>& Args, UWorld* World)
	{
		FString Name;
		FString Folder = DefaultFolder;
		int64 Model = INDEX_NONE;
		const UEnum* ModelEnum = StaticEnum<EPlanetAtmosphereType>();

		for (const FString& Arg : Args)
		{
			const int64 AsModel = ModelEnum->GetValueByNameString(Arg);

			if (Arg.StartsWith(TEXT("/")))
			{
				Folder = Arg;
			}
			else if (AsModel != INDEX_NONE)
			{
				Model = AsModel;
			}
			else
			{
				Name = Arg;
			}
		}

		UFlowSimSubsystem* Sub = World ? World->GetSubsystem<UFlowSimSubsystem>() : nullptr;
		const UFlowSimConfig* Running = Sub ? Sub->GetConfig() : nullptr;

		if (Name.IsEmpty() || !Running)
		{
			UE_LOG(LogFlowSim, Error, TEXT("Usage: FlowSim.Capture Name [/Game/Folder] [Terrestrial|GasGiant], with the sim running."));
			return;
		}

		// PITFALL: A SPIN-UP STATE IS NOT A START STATE. Captured mid spin-up,
		// the snapshot restores weather that is still forming.
		if (Sub->IsSpinningUp())
		{
			UE_LOG(LogFlowSim, Error, TEXT("FlowSim.Capture: still spinning up (FlowSim.Status); capture once it finishes."));
			return;
		}

		const FString ConfigPath = FreePackage(Folder, Name + TEXT("_Sim"));
		const FString StatePath = FreePackage(Folder, Name + TEXT("_State"));
		const FString ArchetypePath = (Model != INDEX_NONE) ? FreePackage(Folder, Name) : FString();

		if (ConfigPath.IsEmpty() || StatePath.IsEmpty() || (Model != INDEX_NONE && ArchetypePath.IsEmpty()))
		{
			return;
		}

		UFlowSnapshot* State = NewObject<UFlowSnapshot>(CreatePackage(*StatePath),
			FName(*FPackageName::GetShortName(StatePath)), RF_Public | RF_Standalone);

		if (!Sub->SaveSnapshot(State))
		{
			Discard(State);
			return;
		}

		// A copy of whatever runs, a harness's transient copy included, as a
		// standalone asset whose start state is the capture.
		UFlowSimConfig* Config = DuplicateObject<UFlowSimConfig>(Running, CreatePackage(*ConfigPath),
			FName(*FPackageName::GetShortName(ConfigPath)));
		Config->ClearFlags(RF_Transient);
		Config->SetFlags(RF_Public | RF_Standalone);
		Config->InitialState = State;

		bool bSaved = Save(State) && Save(Config);

		if (Model != INDEX_NONE)
		{
			UAtmosphereArchetype* Archetype = NewObject<UAtmosphereArchetype>(CreatePackage(*ArchetypePath),
				FName(*FPackageName::GetShortName(ArchetypePath)), RF_Public | RF_Standalone);
			Archetype->Model = (EPlanetAtmosphereType)Model;
			Archetype->Template = Config;
			bSaved = Save(Archetype) && bSaved;
		}

		UE_LOG(LogFlowSim, Display, TEXT("FlowSim.Capture: %s '%s' in %s (%dx%dx%d)."),
			bSaved ? TEXT("saved") : TEXT("made, not all saved,"), *Name, *Folder,
			State->Grid.X, State->Grid.Y, State->Grid.Z);
	}
}

static FAutoConsoleCommandWithWorldAndArgs GFlowSimCaptureCmd(
	TEXT("FlowSim.Capture"),
	TEXT("FlowSim.Capture Name [/Game/Folder] [Terrestrial|GasGiant]: save the running config and its live state ")
	TEXT("as Name_Sim and Name_State (its InitialState), and with a model an archetype Name over them. ")
	TEXT("Folder defaults to /Game/FlowSimCaptures. Blocks on the GPU; editor only."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&FlowSimCapture::Run));

#endif
