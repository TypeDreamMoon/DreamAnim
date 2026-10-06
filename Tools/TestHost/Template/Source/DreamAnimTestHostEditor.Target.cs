// Copyright DreamMoon. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

// The editor target the test runner builds. Shared build environment on purpose: the engine modules
// already in Engine/Binaries are linked as they are, and only the host module and the plugins in
// Plugins/ are compiled. The other settings match DevTestEditor.Target.cs so neither project makes
// the other rebuild.
public class DreamAnimTestHostEditorTarget : TargetRules
{
	public DreamAnimTestHostEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		BuildEnvironment = TargetBuildEnvironment.Shared;
		ExtraModuleNames.Add("DreamAnimTestHost");
	}
}
