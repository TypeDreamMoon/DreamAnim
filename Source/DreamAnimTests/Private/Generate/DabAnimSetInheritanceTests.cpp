// Copyright DreamMoon. All Rights Reserved.
//
// Animation sets across a parent and a child Blueprint: a child's own DefaultAnimSet overrides the
// inherited one, a set it only inherits stays its parent's business, slots keep their names although the
// child's getters need other function names, and slots a Blueprint stops declaring become orphans.

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Driver/DabDriver.h"
#include "DreamAnimLibrary.h"
#include "DreamAnimSet.h"
#include "Extract/DabExtract.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "WriteBack/DabWriteBack.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Tests::AnimSetInheritancePrivate
{
	using namespace UE::DreamAnim;
	using namespace UE::DreamAnim::Compiler;

	const TCHAR* const ParentSettings = TEXT("Parent = AnimInstance; DefaultAnimSet = Path(Game, \"DreamAnimTests/Inheritance/DAS_Parent\");");
	const TCHAR* const ChildParent = TEXT("Parent = Path(Game, \"DreamAnimTests/Inheritance/AB_Parent\");");

	FString MakeSource(const TCHAR* Name, const FString& Settings, const TCHAR* Body)
	{
		return FString::Printf(TEXT("AnimBlueprint(Name=\"DreamAnimTests/Inheritance/%s\", Root=\"Game\")\n{\n    Settings = { Skeleton = SK_Als; %s }\n    %s\n}\n"),
			Name, *Settings, Body);
	}

	UAnimBlueprint* Build(FAutomationTestBase& Test, const FString& Text)
	{
		FDabCompileOptions Options;
		Options.bSave = false;
		const FDabCompileResult Result = CompileDabText(TEXT("Inheritance.dab"), Text, Options);
		for (const Lang::FDabDiagnostic& Diagnostic : Result.Diagnostics)
		{
			if (Diagnostic.Severity == Lang::EDabSeverity::Error)
			{
				Test.AddError(FormatDabDiagnosticLine(Diagnostic));
			}
		}
		return Result.Succeeded() ? Result.Blueprint : nullptr;
	}

	UDreamAnimSet* MakeSet(const TCHAR* Name)
	{
		return NewObject<UDreamAnimSet>(CreatePackage(*(FString(TEXT("/Game/DreamAnimTests/Inheritance/")) + Name)), Name, RF_Public | RF_Standalone);
	}

	const UDreamAnimSet* GetDefaultSet(const UAnimBlueprint& Blueprint)
	{
		return UDreamAnimLibrary::GetAnimSet(CastChecked<UAnimInstance>(Blueprint.GeneratedClass->GetDefaultObject()));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabAnimSetChildOverrideTest, "DreamAnim.Generate.InheritedAndRemovedSlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/**
 * A child with its own DefaultAnimSet and a slot of the same name as its parent's: its class defaults hold
 * its set, the slot keeps its name through extraction and a round trip, and the slots each Blueprint stops
 * declaring are marked orphans in its set. The parent is rebuilt after the child with no garbage collection
 * in between, as an editor session does it, while the child's discarded build candidate still exists.
 */
bool FDabAnimSetChildOverrideTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::AnimSetInheritancePrivate;

	UDreamAnimSet* ParentSet = MakeSet(TEXT("DAS_Parent"));
	UDreamAnimSet* ChildSet = MakeSet(TEXT("DAS_Child"));
	UAnimBlueprint* Parent = Build(*this, MakeSource(TEXT("AB_Parent"), ParentSettings, TEXT("Assets = { AnimSequence Pose; } Graph = { Output = LocalRefPose(); }")));
	if (!TestNotNull(TEXT("the parent"), Parent))
	{
		return false;
	}
	const FString ChildSettings = FString(ChildParent) + TEXT(" DefaultAnimSet = Path(Game, \"DreamAnimTests/Inheritance/DAS_Child\");");
	UAnimBlueprint* Child = Build(*this, MakeSource(TEXT("AB_Child"), ChildSettings, TEXT("Assets = { AnimSequence Pose; }")));
	if (!TestNotNull(TEXT("the child"), Child))
	{
		return false;
	}
	TestTrue(TEXT("the parent keeps its set"), GetDefaultSet(*Parent) == ParentSet);
	TestTrue(TEXT("the child's set overrides the inherited one"), GetDefaultSet(*Child) == ChildSet);

	Model::FDabModel Model;
	Lang::FDabDiagnosticSink Sink(TEXT("Inheritance"));
	TestTrue(TEXT("the child extracts"), ExtractDabModel(*Child, Model, Sink));
	TestEqualSensitive(TEXT("its own set is its DefaultAnimSet"), Model.DefaultAnimSet, ChildSet->GetPathName());
	if (TestEqual(TEXT("one slot"), Model.AssetSlots.Num(), 1))
	{
		TestEqualSensitive(TEXT("the slot keeps its name, not its getter's"), Model.AssetSlots[0].Name, FString(TEXT("Pose")));
	}
	Child = Build(*this, DecompileDabBlueprint(*Child).Text);
	if (!TestNotNull(TEXT("the child round-trips"), Child))
	{
		return false;
	}
	TestTrue(TEXT("and keeps its set"), GetDefaultSet(*Child) == ChildSet);

	if (!Build(*this, MakeSource(TEXT("AB_Child"), ChildSettings, TEXT(""))))
	{
		return false;
	}
	if (TestEqual(TEXT("the child has one group"), ChildSet->Groups.Num(), 1) && TestEqual(TEXT("the old slot stays"), ChildSet->Groups[0].Slots.Num(), 1))
	{
		TestTrue(TEXT("the child's last removed slot is an orphan"), ChildSet->Groups[0].Slots[0].bOrphan);
	}
	Parent = Build(*this, MakeSource(TEXT("AB_Parent"), ParentSettings, TEXT("Graph = { Output = LocalRefPose(); }")));
	if (!TestNotNull(TEXT("the parent without slots"), Parent))
	{
		return false;
	}
	TestTrue(TEXT("the parent's last removed slot is an orphan"), ParentSet->Groups[0].Slots[0].bOrphan);
	TestTrue(TEXT("a DefaultAnimSet without slots stays in the text"), DecompileDabBlueprint(*Parent).Text.Contains(TEXT("DAS_Parent")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabAnimSetChildInheritsTest, "DreamAnim.Generate.InheritedSetStaysWithParent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/**
 * A child that names no set of its own runs with its parent's. Its text says nothing about it, so reading
 * the child back gives no DefaultAnimSet and no class default for it, and writing back an unchanged child
 * is a fixed point with nothing refused.
 */
bool FDabAnimSetChildInheritsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::AnimSetInheritancePrivate;

	UDreamAnimSet* ParentSet = MakeSet(TEXT("DAS_Elder"));
	const TCHAR* const ElderSettings = TEXT("Parent = AnimInstance; DefaultAnimSet = Path(Game, \"DreamAnimTests/Inheritance/DAS_Elder\");");
	if (!Build(*this, MakeSource(TEXT("AB_Elder"), ElderSettings, TEXT("Assets = { AnimSequence Pose; } Graph = { Output = LocalRefPose(); }"))))
	{
		return false;
	}
	const FString ChildText = MakeSource(TEXT("AB_Heir"), TEXT("Parent = Path(Game, \"DreamAnimTests/Inheritance/AB_Elder\");"), TEXT(""));
	UAnimBlueprint* Child = Build(*this, ChildText);
	if (!TestNotNull(TEXT("the child"), Child))
	{
		return false;
	}
	TestTrue(TEXT("the child runs with its parent's set"), GetDefaultSet(*Child) == ParentSet);

	Model::FDabModel Model;
	Lang::FDabDiagnosticSink Sink(TEXT("Inheritance"));
	TestTrue(TEXT("the child extracts"), ExtractDabModel(*Child, Model, Sink));
	TestTrue(TEXT("an inherited set is not the child's DefaultAnimSet"), Model.DefaultAnimSet.IsEmpty());
	TestFalse(TEXT("nor one of its class defaults"), Model.ClassDefaults.ContainsByPredicate([](const Model::FDabModelProperty& Default)
	{
		return Default.Name == UDreamAnimLibrary::ActiveSetVariable.ToString();
	}));

	const FDabWriteBackResult Written = WriteBackDabBlueprint(*Child, TEXT("Inheritance.dab"), ChildText);
	for (const Lang::FDabDiagnostic& Diagnostic : Written.Diagnostics)
	{
		AddError(FormatDabDiagnosticLine(Diagnostic));
	}
	TestTrue(TEXT("an unchanged child writes back completely"), Written.IsComplete());
	TestFalse(TEXT("with no edits"), Written.HasEdits());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabAnimSetNoEmptyGroupTest, "DreamAnim.Generate.SetWithoutSlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/** A Blueprint that names a DefaultAnimSet but declares no slot adds no empty group to the set. */
bool FDabAnimSetNoEmptyGroupTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::AnimSetInheritancePrivate;

	UDreamAnimSet* Set = MakeSet(TEXT("DAS_Empty"));
	UAnimBlueprint* Blueprint = Build(*this, MakeSource(TEXT("AB_NoSlots"),
		TEXT("Parent = AnimInstance; DefaultAnimSet = Path(Game, \"DreamAnimTests/Inheritance/DAS_Empty\");"), TEXT("Graph = { Output = LocalRefPose(); }")));
	if (!TestNotNull(TEXT("the build"), Blueprint))
	{
		return false;
	}
	TestTrue(TEXT("the set is the default"), GetDefaultSet(*Blueprint) == Set);
	TestEqual(TEXT("no group is added"), Set->Groups.Num(), 0);
	return true;
}

#endif
