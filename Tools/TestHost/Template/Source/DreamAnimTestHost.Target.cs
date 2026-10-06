// Copyright DreamMoon. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

// The game target. Nothing builds it -- the tests use the editor binary, with -game where they need
// a standalone game -- but a code project needs one. Building it against a source engine compiles the
// whole engine again, so don't.
public class DreamAnimTestHostTarget : TargetRules
{
	public DreamAnimTestHostTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V7;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_8;
		ExtraModuleNames.Add("DreamAnimTestHost");
	}
}
