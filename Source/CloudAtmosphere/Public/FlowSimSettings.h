#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "FlowSimTypes.h"
#include "FlowSimSettings.generated.h"

/** Project Settings -> Plugins -> Flow Sim: starts a sim in worlds no
 *  atmosphere claims it in, so the debug view runs without one. An atmosphere
 *  that claims the sim replaces this config with its own. */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Flow Sim"))
class CLOUDATMOSPHERE_API UFlowSimSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("Flow Sim"); }

	/** Config started automatically, and the one FlowSim.Start starts when
	 *  given no argument. Soft, so cooks that never touch the sim do not load
	 *  it and its snapshot. */
	UPROPERTY(config, EditAnywhere, Category = "Flow Sim", meta = (AllowedClasses = "/Script/CloudAtmosphere.FlowSimConfig"))
	TSoftObjectPtr<UFlowSimConfig> DefaultConfig;

	/** Start DefaultConfig automatically in editor worlds. */
	UPROPERTY(config, EditAnywhere, Category = "Flow Sim")
	bool bAutoStartInEditor = true;

	/** Start DefaultConfig automatically in PIE and game worlds. Off, so a
	 *  shipping world's sim comes from its atmospheres rather than from a
	 *  bring-up setting. */
	UPROPERTY(config, EditAnywhere, Category = "Flow Sim")
	bool bAutoStartInGame = false;
};