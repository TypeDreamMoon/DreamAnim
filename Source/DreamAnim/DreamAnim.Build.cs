// Copyright DreamMoon. All Rights Reserved.

using UnrealBuildTool;

public class DreamAnim : ModuleRules
{
	public DreamAnim(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
	}
}
