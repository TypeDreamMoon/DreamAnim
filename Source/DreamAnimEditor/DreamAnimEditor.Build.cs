// Copyright DreamMoon. All Rights Reserved.

using UnrealBuildTool;

// Editor-only pieces: how animation sets look and are created in the editor.
public class DreamAnimEditor : ModuleRules
{
	public DreamAnimEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UnrealEd",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AssetTools",
			"PropertyEditor",
			"Slate",
			"SlateCore",
			"DreamAnim",
		});
	}
}
