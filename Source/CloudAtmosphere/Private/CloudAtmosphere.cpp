#include "CloudAtmosphere.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

DEFINE_LOG_CATEGORY(LogCloudAtmosphere);

void FCloudAtmosphereModule::StartupModule()
{
	// Maps /Plugin/CloudAtmosphere to this plugin's Shaders folder, so the
	// global shaders resolve their virtual paths.
	//
	// WITHOUT THIS THE INCLUDES FAIL AT SHADER COMPILE, not at load, and the
	// error names the including file rather than the missing mapping -- so it
	// reads as a broken shader. FindPlugin matches the plugin's name, the
	// .uplugin's file name.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CloudAtmosphere"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogCloudAtmosphere, Error,
			TEXT("CloudAtmosphere plugin not found by IPluginManager; shader directory was not mapped. ")
			TEXT("The plugin must be named 'CloudAtmosphere', as its .uplugin file is."));
		return;
	}

	const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/CloudAtmosphere"), ShaderDir);

	UE_LOG(LogCloudAtmosphere, Log, TEXT("Mapped /Plugin/CloudAtmosphere -> %s"), *ShaderDir);
}

void FCloudAtmosphereModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FCloudAtmosphereModule, CloudAtmosphere)