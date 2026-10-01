#pragma once

#include "Modules/ModuleManager.h"

/** The plugin's own log: module startup and the atmosphere actor. */
CLOUDATMOSPHERE_API DECLARE_LOG_CATEGORY_EXTERN(LogCloudAtmosphere, Log, All);

class FCloudAtmosphereModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
