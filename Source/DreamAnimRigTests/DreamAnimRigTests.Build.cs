// Copyright DreamMoon. All Rights Reserved.

using UnrealBuildTool;

// Tests of the Control Rig front end (DreamAnim.Rig.*): parsing and printing .dcr, extract and generate
// against the engine's own rigs, round trips, and running original and rebuilt rigs side by side.
// Editor, like DreamAnimRig: the rigs run inside the editor, no -game process is needed.
public class DreamAnimRigTests : ModuleRules
{
	public DreamAnimRigTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"AnimationCore",
			"AssetRegistry",
			"UnrealEd",
			"RigVM",
			"RigVMDeveloper",
			"ControlRig",
			"ControlRigDeveloper",
			"ControlRigEditor",
			"DreamAnimLang",
			"DreamAnimCompiler",
			"DreamAnimRig",
			"Json",
			"Projects",
		});
	}
}
