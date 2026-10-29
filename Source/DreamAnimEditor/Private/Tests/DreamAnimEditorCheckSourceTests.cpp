// Copyright DreamMoon. All Rights Reserved.
//
// UDreamAnimEditorSubsystem::CheckSource, which tools call through the editor's Python: the diagnostics come
// back as the JSON `dab.ps1 check -Json` writes, for a buffer that was never saved, for both kinds of source,
// with a relative path resolved against the project.

#include "DreamAnimEditorSubsystem.h"
#include "Editor.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamAnimEditorCheckSourceTest, "DreamAnim.Editor.CheckSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamAnimEditorCheckSourceTest::RunTest(const FString& Parameters)
{
	UDreamAnimEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UDreamAnimEditorSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("the editor subsystem"), Subsystem))
	{
		return false;
	}

	// A clean buffer: no diagnostics.
	const FString Clean = Subsystem->CheckSource(TEXT("DAnim/CheckSourceProbe/AB_Clean.dab"),
		TEXT("AnimBlueprint(Name=\"CheckSourceProbe/AB_Clean\", Root=\"Game\")\n{\n    Graph = { Output = LocalRefPose(); }\n}\n"));
	TestEqual(TEXT("a clean source gives []"), Clean, FString(TEXT("[]")));

	// A mistake: the diagnostic in the -Json form, at the file the relative path names.
	const FString Broken = Subsystem->CheckSource(TEXT("DAnim/CheckSourceProbe/AB_Broken.dab"),
		TEXT("AnimBlueprint(Name=\"CheckSourceProbe/AB_Broken\", Root=\"Game\")\n{\n    Graph = { Output = NoSuchNode(); }\n}\n"));
	AddInfo(Broken);
	TestTrue(TEXT("the unknown node type is DAB3001"), Broken.Contains(TEXT("\"code\":\"DAB3001\"")));
	TestTrue(TEXT("at line 3"), Broken.Contains(TEXT("\"line\":3")));
	FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	FPaths::NormalizeFilename(ProjectDir);
	TestTrue(TEXT("the relative path is the project's"), Broken.Contains(ProjectDir + TEXT("DAnim/CheckSourceProbe/AB_Broken.dab")));

	// A Control Rig source goes through its own checker.
	const FString Rig = Subsystem->CheckSource(TEXT("DAnim/CheckSourceProbe/CR_Probe.dcr"),
		TEXT("ControlRig(Name=\"CheckSourceProbe/CR_Probe\", Root=\"Game\")\n{\n    Variables = { double Speed; }\n\n    Event Forward\n    {\n        Speed = Missing;\n    }\n}\n"));
	TestTrue(TEXT("a .dcr's unknown name is DAB9001"), Rig.Contains(TEXT("\"code\":\"DAB9001\"")));

	// Nothing given and nothing on disk: the file cannot be read, and that is a diagnostic too.
	const FString Missing = Subsystem->CheckSource(TEXT("DAnim/CheckSourceProbe/AB_NotThere.dab"), FString());
	TestTrue(TEXT("a missing file is reported"), Missing.Contains(TEXT("\"severity\":\"error\"")));
	return true;
}

#endif
