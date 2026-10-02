using UnrealBuildTool;

public class CloudAtmosphere : ModuleRules
{
	public CloudAtmosphere(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// What the public headers include.
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"RenderCore",
				"RHI",
				"DeveloperSettings",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Renderer",
				"Projects",
				"Json",
				"JsonUtilities",
			}
			);
	}
}
