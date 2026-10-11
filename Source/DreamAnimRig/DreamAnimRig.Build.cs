// Copyright DreamMoon. All Rights Reserved.

using UnrealBuildTool;

// The Control Rig front end (.dcr): reading a rig into the model (extract), turning the model into text
// (lower) and back (semantic), and building rigs from it (generate). Editor: it creates assets through
// the Control Rig editor's factories, and nothing here runs in a game.
public class DreamAnimRig : ModuleRules
{
	public DreamAnimRig(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DreamAnimLang",
			"RigVM",
			"RigVMDeveloper",
			"ControlRig",
			"ControlRigDeveloper",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"AnimationCore",
			"AssetRegistry",
			"AssetTools",
			"ControlRigEditor",
			"DreamAnimCompiler",
			"Projects",
			"UnrealEd",
		});
	}
}
