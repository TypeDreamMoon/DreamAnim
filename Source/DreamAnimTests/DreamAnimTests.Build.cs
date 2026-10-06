// Copyright DreamMoon. All Rights Reserved.

using UnrealBuildTool;

// UncookedOnly rather than Editor: an Editor module does not load under -game, and the runtime
// tests drive animation in a standalone game process.
public class DreamAnimTests : ModuleRules
{
	public DreamAnimTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AssetRegistry",
			"UnrealEd",
			"DreamAnim",
			"DreamAnimLang",
			"DreamAnimCompiler",
			"GameplayTags",
			"Projects",
		});
	}
}
