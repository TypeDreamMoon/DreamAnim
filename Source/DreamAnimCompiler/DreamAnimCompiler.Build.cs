// Copyright DreamMoon. All Rights Reserved.

using UnrealBuildTool;

// The engine-facing half: semantic checks against reflection, the generator (model -> Animation
// Blueprint) and the extractor (Animation Blueprint -> model). UncookedOnly: it needs the editor's
// graph classes, and it must also load in the uncooked -game processes the runtime tests use.
public class DreamAnimCompiler : ModuleRules
{
	public DreamAnimCompiler(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DreamAnimLang",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AnimGraph",
			"AnimGraphRuntime",
			"AssetRegistry",
			"BlueprintGraph",
			"GameplayTags",
			"Json",
			"Kismet",
			"KismetCompiler",
			"Projects",
			"PropertyAccessEditor",
			"UnrealEd",
			"DreamAnim",
		});
	}
}
