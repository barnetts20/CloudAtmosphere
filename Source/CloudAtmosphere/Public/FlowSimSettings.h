#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "GasGiantSimTypes.h"
#include "GasGiantSimSettings.generated.h"

/** Project Settings -> Plugins -> Gas Giant Sim.
 *
 *  WHY THE EDITOR ENTRY POINT IS A SETTING AND NOT A BLUEPRINT.
 *
 *  A Level Blueprint's BeginPlay does not fire outside PIE, so a
 *  blueprint-driven start means entering play every time you want to look at
 *  the field. That is the wrong loop for this stage of the work: the whole
 *  point of the debug view is a fast change-look-change cycle, and PIE round
 *  trips are the slowest part of it.
 *
 *  A setting plus console commands means the sim is running in the editor
 *  viewport from the moment the project opens, with nothing authored and
 *  nothing to remember to trigger.
 *
 *  Blueprint control still exists -- UGasGiantSimSubsystem's StartSimulation
 *  and friends are BlueprintCallable -- and is the right path for shipping,
 *  where a planet actor should own its own sim rather than the project having
 *  one global one. This is a bring-up affordance, and the auto-start defaults
 *  reflect that: on in the editor, off in game. */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Gas Giant Sim"))
class CLOUDATMOSPHERE_API UGasGiantSimSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("Gas Giant Sim"); }

	/** Config started automatically, and the one the console commands act on
	 *  when given no argument.
	 *
	 *  Soft, so setting it does not drag the config and both render targets
	 *  into memory for every cook that never touches the sim. */
	UPROPERTY(config, EditAnywhere, Category = "Gas Giant", meta = (AllowedClasses = "/Script/CloudAtmosphere.GasGiantSimConfig"))
	TSoftObjectPtr<UGasGiantSimConfig> DefaultConfig;

	/** Start automatically in editor worlds. On by default: this is the whole
	 *  reason the setting exists. */
	UPROPERTY(config, EditAnywhere, Category = "Gas Giant")
	bool bAutoStartInEditor = true;

	/** Start automatically in PIE and game worlds.
	 *
	 *  OFF by default, deliberately. A global auto-started sim is convenient
	 *  for bring-up and wrong for shipping, where each planet should drive its
	 *  own. Leaving this off means the shipping path has to be written
	 *  explicitly rather than inherited by accident from a debug setting. */
	UPROPERTY(config, EditAnywhere, Category = "Gas Giant")
	bool bAutoStartInGame = false;
};
