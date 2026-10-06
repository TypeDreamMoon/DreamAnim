// Copyright DreamMoon. All Rights Reserved.

using UnrealBuildTool;

// The language front end: lexer, parser, syntax tree, printer, template expansion. Plain C++ over
// Core, with no editor or UObject dependency, so it can be tested fast and reused by the compiler,
// the editor and the command line alike.
public class DreamAnimLang : ModuleRules
{
	public DreamAnimLang(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core" });
	}
}
