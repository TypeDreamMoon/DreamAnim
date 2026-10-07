// Copyright DreamMoon. All Rights Reserved.

using UnrealBuildTool;

// Editor-only pieces: how animation sets look and are created in the editor, and keeping Animation
// Blueprints and their sources in step (rebuild on save, write editor changes back, asset menus).
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
			"DeveloperSettings",
			"EditorSubsystem",
			"DreamAnimLang",
			"DreamAnimCompiler",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AnimGraph",
			"AssetTools",
			"BlueprintGraph",
			"ContentBrowser",
			"DirectoryWatcher",
			"InputCore",
			"MessageLog",
			"PropertyEditor",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"DreamAnim",
		});
	}
}
