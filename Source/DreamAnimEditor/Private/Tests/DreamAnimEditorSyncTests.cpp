// Copyright DreamMoon. All Rights Reserved.
//
// The editor keeping a Blueprint and its source in step: rebuilding from a source under the project's
// DAnim folder, writing an edit back, ignoring the echo of its own write, and a text changed on disk while
// the Blueprint has its own change: merged when they touch different lines, the text's when they do not.

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimStateNode.h"
#include "DreamAnimEditorSettings.h"
#include "DreamAnimEditorSubsystem.h"
#include "Editor.h"
#include "Generate/DabGenerate.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace DreamAnimEditorSyncTests
{
	const TCHAR* const SourceText = TEXT(
		"AnimBlueprint(Name=\"DreamAnimTests/Sync/AB_Sync\", Root=\"Game\")\n"
		"{\n"
		"    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }\n"
		"\n"
		"    Properties = { float Speed = 0; }\n"
		"\n"
		"    Graph = { Output = Locomotion; }\n"
		"\n"
		"    StateMachine Locomotion\n"
		"    {\n"
		"        Entry = Idle;\n"
		"\n"
		"        State Idle\n"
		"        {\n"
		"            Pose = SequencePlayer(Sequence = A_Als_Idle, PlayRate = 1.2);\n"
		"        }\n"
		"    }\n"
		"}\n");

	/** Sets the Idle state's player's play rate, as the details panel would. */
	bool SetIdlePlayRate(FAutomationTestBase& Test, UAnimBlueprint& Blueprint, const TCHAR* Value)
	{
		TArray<UAnimStateNode*> States;
		FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, States);
		UAnimStateNode* const* Idle = States.FindByPredicate([](const UAnimStateNode* State) { return State->GetStateName() == TEXT("Idle"); });
		UAnimGraphNode_SequencePlayer* Player = nullptr;
		if (Idle && (*Idle)->BoundGraph)
		{
			for (UEdGraphNode* Node : (*Idle)->BoundGraph->Nodes)
			{
				Player = Player ? Player : Cast<UAnimGraphNode_SequencePlayer>(Node);
			}
		}
		const FStructProperty* NodeProperty = Player ? FindFProperty<FStructProperty>(Player->GetClass(), TEXT("Node")) : nullptr;
		const FProperty* PlayRate = NodeProperty ? FindFProperty<FProperty>(NodeProperty->Struct, TEXT("PlayRate")) : nullptr;
		if (!PlayRate)
		{
			Test.AddError(TEXT("Idle's sequence player and its PlayRate"));
			return false;
		}
		PlayRate->ImportText_Direct(Value, PlayRate->ContainerPtrToValuePtr<void>(NodeProperty->ContainerPtrToValuePtr<void>(Player)), Player, PPF_None);
		return true;
	}

	FString Load(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		return Text;
	}

	void Save(const FString& Path, const FString& Text)
	{
		FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamAnimEditorSyncTest, "DreamAnim.Editor.Sync",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDreamAnimEditorSyncTest::RunTest(const FString& Parameters)
{
	using namespace DreamAnimEditorSyncTests;
	using namespace UE::DreamAnim::Compiler;

	UDreamAnimEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UDreamAnimEditorSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("the subsystem"), Subsystem))
	{
		return false;
	}

	// A source under the project's DAnim folder, rebuilt without saving; all of it goes afterwards.
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("DAnim"));
	const bool bRootExisted = FPaths::DirectoryExists(Root);
	const FString Dir = Root / TEXT("DreamAnimTests");
	const FString Path = Dir / TEXT("AB_Sync.dab");
	UDreamAnimEditorSettings* Settings = GetMutableDefault<UDreamAnimEditorSettings>();
	const bool bSaved = Settings->bSaveRebuiltBlueprints;
	Settings->bSaveRebuiltBlueprints = false;
	ON_SCOPE_EXIT
	{
		Settings->bSaveRebuiltBlueprints = bSaved;
		IFileManager::Get().DeleteDirectory(*(bRootExisted ? Dir : Root), false, true);
		Subsystem->RefreshSources();
	};
	IFileManager::Get().MakeDirectory(*Dir, true);
	Save(Path, SourceText);
	Subsystem->RefreshSources();

	UAnimBlueprint* Blueprint = Subsystem->Rebuild(Path);
	if (!TestNotNull(TEXT("the Blueprint is built from the source"), Blueprint))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the Blueprint keeps the text it was built from"), GetDabGeneratedText(*Blueprint), FString(SourceText));
	TestFalse(TEXT("fresh from the text, it has no changes of its own"), Subsystem->HasEditorChanges(*Blueprint));

	// An edit is written back, and the Blueprint is in step with the new text.
	SetIdlePlayRate(*this, *Blueprint, TEXT("1.5"));
	TestTrue(TEXT("the edit is written"), Subsystem->WriteBack(*Blueprint));
	const FString Written = FString(SourceText).Replace(TEXT("PlayRate = 1.2"), TEXT("PlayRate = 1.5"));
	TestEqualSensitive(TEXT("the file has the edit"), Load(Path), Written);
	TestEqualSensitive(TEXT("the Blueprint keeps the written text"), GetDabGeneratedText(*Blueprint), Written);
	TestTrue(TEXT("the node positions are written next to the source"), FPaths::FileExists(GetDabLayoutPath(Path)));

	// The watcher's report of that write is not a change.
	Subsystem->HandleChangedSources({ Path });
	TestFalse(TEXT("its own write is no conflict"), Subsystem->IsInConflict(*Blueprint));
	TestTrue(TEXT("its own write rebuilds nothing"), FindObject<UAnimBlueprint>(nullptr, *Blueprint->GetPathName()) == Blueprint);

	// The text and the Blueprint both change, on different lines: a conflict, then a merge of both.
	SetIdlePlayRate(*this, *Blueprint, TEXT("2"));
	const FString Outside = Written.Replace(TEXT("float Speed = 0;"), TEXT("float Speed = 3;"));
	Save(Path, Outside);
	Subsystem->HandleChangedSources({ Path });
	TestTrue(TEXT("a text changed under the Blueprint's own change is a conflict"), Subsystem->IsInConflict(*Blueprint));
	TestEqualSensitive(TEXT("nothing is overwritten while it waits"), Load(Path), Outside);
	TestFalse(TEXT("no write-back while it waits"), Subsystem->WriteBack(*Blueprint));
	TestTrue(TEXT("different lines merge"), Subsystem->MergeEditorChanges(*Blueprint));
	const FString Merged = Outside.Replace(TEXT("PlayRate = 1.5"), TEXT("PlayRate = 2"));
	TestEqualSensitive(TEXT("the file has both changes"), Load(Path), Merged);
	TestFalse(TEXT("the conflict is over"), Subsystem->IsInConflict(*Blueprint));
	TestEqualSensitive(TEXT("rebuilt from the merge"), GetDabGeneratedText(*Blueprint), Merged);
	TestFalse(TEXT("the merge is what the Blueprint holds"), Subsystem->HasEditorChanges(*Blueprint));

	// Both change the same line: no merge; the text wins when asked.
	SetIdlePlayRate(*this, *Blueprint, TEXT("3"));
	const FString SameLine = Merged.Replace(TEXT("PlayRate = 2"), TEXT("PlayRate = 2.5"));
	Save(Path, SameLine);
	Subsystem->HandleChangedSources({ Path });
	TestTrue(TEXT("the same line is a conflict"), Subsystem->IsInConflict(*Blueprint));
	TestFalse(TEXT("the same line does not merge"), Subsystem->MergeEditorChanges(*Blueprint));
	TestEqualSensitive(TEXT("a failed merge writes nothing"), Load(Path), SameLine);
	TestTrue(TEXT("the text is used when asked"), Subsystem->UseText(*Blueprint) != nullptr);
	TestFalse(TEXT("then the conflict is over"), Subsystem->IsInConflict(*Blueprint));
	TestEqualSensitive(TEXT("and the Blueprint is the text's"), GetDabGeneratedText(*Blueprint), SameLine);
	TestFalse(TEXT("with none of its own change left"), Subsystem->HasEditorChanges(*Blueprint));

	// A text changed on disk with no change in the Blueprint just rebuilds it.
	const FString Plain = SameLine.Replace(TEXT("float Speed = 3;"), TEXT("float Speed = 4;"));
	Save(Path, Plain);
	Subsystem->HandleChangedSources({ Path });
	TestFalse(TEXT("no conflict without a change of its own"), Subsystem->IsInConflict(*Blueprint));
	TestEqualSensitive(TEXT("rebuilt from the new text"), GetDabGeneratedText(*Blueprint), Plain);
	return true;
}

// ---------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamAnimEditorLiveTest, "DreamAnim.Editor.Live",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * The same over frames, as it happens while someone works: the folder watcher reports a saved file and
 * the Blueprint is rebuilt; an edit made in a transaction is written back once edits pause.
 */
bool FDreamAnimEditorLiveTest::RunTest(const FString& Parameters)
{
	using namespace DreamAnimEditorSyncTests;
	using namespace UE::DreamAnim::Compiler;

	UDreamAnimEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UDreamAnimEditorSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("the subsystem"), Subsystem))
	{
		return false;
	}
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("DAnim"));
	const bool bRootExisted = FPaths::DirectoryExists(Root);
	const FString Dir = Root / TEXT("DreamAnimTestsLive");
	const FString Path = Dir / TEXT("AB_Live.dab");
	const FString Text = FString(SourceText).Replace(TEXT("Sync/AB_Sync"), TEXT("Live/AB_Live"));

	UDreamAnimEditorSettings* Settings = GetMutableDefault<UDreamAnimEditorSettings>();
	const bool bSaved = Settings->bSaveRebuiltBlueprints;
	const float Delay = Settings->WriteBackDelay;
	Settings->bSaveRebuiltBlueprints = false;
	Settings->WriteBackDelay = 0.2f;
	const auto Restore = [Settings, bSaved, Delay, bRootExisted, Root, Dir, Subsystem]()
	{
		Settings->bSaveRebuiltBlueprints = bSaved;
		Settings->WriteBackDelay = Delay;
		IFileManager::Get().DeleteDirectory(*(bRootExisted ? Dir : Root), false, true);
		Subsystem->RefreshSources();
	};

	IFileManager::Get().MakeDirectory(*Dir, true);
	Save(Path, Text);
	Subsystem->RefreshSources();
	UAnimBlueprint* Blueprint = Subsystem->Rebuild(Path);
	if (!TestNotNull(TEXT("the Blueprint is built from the source"), Blueprint))
	{
		Restore();
		return false;
	}
	const TWeakObjectPtr<UAnimBlueprint> WeakBlueprint(Blueprint);

	// Saved by someone else: the watcher reports it and the Blueprint is rebuilt.
	const FString Saved = Text.Replace(TEXT("float Speed = 0;"), TEXT("float Speed = 7;"));
	Save(Path, Saved);
	const double Start = FPlatformTime::Seconds();
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, WeakBlueprint, Saved, Start]()
	{
		if (WeakBlueprint.IsValid() && GetDabGeneratedText(*WeakBlueprint).Equals(Saved, ESearchCase::CaseSensitive))
		{
			return true;
		}
		if (FPlatformTime::Seconds() - Start > 15.0)
		{
			AddError(TEXT("a saved source rebuilds its Blueprint"));
			return true;
		}
		return false;
	}));

	// Edited in the editor: written back once edits pause.
	const FString Edited = Saved.Replace(TEXT("PlayRate = 1.2"), TEXT("PlayRate = 0.5"));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, WeakBlueprint]()
	{
		if (WeakBlueprint.IsValid())
		{
			GEditor->BeginTransaction(NSLOCTEXT("DreamAnimTests", "EditPlayRate", "Edit Play Rate"));
			TArray<UAnimGraphNode_SequencePlayer*> Players;
			FBlueprintEditorUtils::GetAllNodesOfClass(WeakBlueprint.Get(), Players);
			for (UAnimGraphNode_SequencePlayer* Player : Players)
			{
				Player->Modify();
			}
			SetIdlePlayRate(*this, *WeakBlueprint, TEXT("0.5"));
			GEditor->EndTransaction();
		}
		return true;
	}));
	const TSharedRef<double> EditStart = MakeShared<double>(0.0);
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Path, Edited, EditStart]()
	{
		if (*EditStart == 0.0)
		{
			*EditStart = FPlatformTime::Seconds();
		}
		if (Load(Path).Equals(Edited, ESearchCase::CaseSensitive))
		{
			return true;
		}
		if (FPlatformTime::Seconds() - *EditStart > 30.0)
		{
			AddError(FString::Printf(TEXT("an edit in a transaction is written back; the file holds:\n%s"), *Load(Path)));
			return true;
		}
		return false;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, WeakBlueprint, Path, Edited, Subsystem]()
	{
		TestFalse(TEXT("its own write is not taken for a change"), WeakBlueprint.IsValid() && Subsystem->IsInConflict(*WeakBlueprint));
		TestTrue(TEXT("and the node positions are next to it"), FPaths::FileExists(GetDabLayoutPath(Path)));
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Restore]()
	{
		Restore();
		return true;
	}));
	return true;
}

#endif
