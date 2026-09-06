// Copyright Epic Games, Inc. All Rights Reserved.

#include "CloudAtmosphere.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "ShaderCore.h"

#define LOCTEXT_NAMESPACE "FCloudAtmosphereModule"

DEFINE_LOG_CATEGORY_STATIC(LogCloudAtmosphere, Log, All);

void FCloudAtmosphereModule::StartupModule()
{
	// Maps /Plugin/CloudAtmosphere to this plugin's Shaders folder, so the
	// material Custom nodes can #include the .ush files by virtual path.
	//
	// WITHOUT THIS THE INCLUDES FAIL AT SHADER COMPILE, not at load, and the
	// error names the including file rather than the missing mapping -- so it
	// reads as a broken material. The plugin folder name has to match the
	// .uplugin exactly for FindPlugin to resolve.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CloudAtmosphere"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogCloudAtmosphere, Error,
			TEXT("CloudAtmosphere plugin not found by IPluginManager; shader directory was not mapped. ")
			TEXT("The plugin folder name must be 'CloudAtmosphere' to match the .uplugin."));
		return;
	}

	const FString ShaderDir = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/CloudAtmosphere"), ShaderDir);

	UE_LOG(LogCloudAtmosphere, Log, TEXT("Mapped /Plugin/CloudAtmosphere -> %s"), *ShaderDir);
}

void FCloudAtmosphereModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FCloudAtmosphereModule, CloudAtmosphere)
