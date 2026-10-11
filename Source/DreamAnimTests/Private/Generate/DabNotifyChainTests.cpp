// Copyright DreamMoon. All Rights Reserved.
//
// Disabled nodes in a notify's call chain, read as the Blueprint compiler runs them: a disabled call does
// nothing and disappears from the text; a disabled branch ends the chain, so what follows it is reported
// rather than written as a call that would run.

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Driver/DabDriver.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Tests::NotifyChainPrivate
{
	using namespace UE::DreamAnim;
	using namespace UE::DreamAnim::Compiler;

	/** A Blueprint whose FootPlant notify switches root motion to RootMotionFromEverything. */
	const TCHAR* const NotifyText = TEXT(
		"AnimBlueprint(Name=\"DreamAnimTests/NotifyChain/AB_NOTIFY\", Root=\"Game\")\n"
		"{\n"
		"    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }\n"
		"    Notifies = { FootPlant = Self.SetRootMotionMode(RootMotionFromEverything); }\n"
		"    Graph = { Output = LocalRefPose(); }\n"
		"}\n");

	UAnimBlueprint* Build(FAutomationTestBase& Test, const FString& Text)
	{
		FDabCompileOptions Options;
		Options.bSave = false;
		const FDabCompileResult Result = CompileDabText(TEXT("NotifyChain.dab"), Text, Options);
		for (const Lang::FDabDiagnostic& Diagnostic : Result.Diagnostics)
		{
			if (Diagnostic.Severity == Lang::EDabSeverity::Error)
			{
				Test.AddError(FormatDabDiagnosticLine(Diagnostic));
			}
		}
		return Result.Succeeded() ? Result.Blueprint : nullptr;
	}

	UK2Node_CallFunction* FindRootMotionCall(UAnimBlueprint& Blueprint)
	{
		TArray<UK2Node_CallFunction*> Calls;
		FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, Calls);
		UK2Node_CallFunction* const* Found = Calls.FindByPredicate([](const UK2Node_CallFunction* Call)
		{
			return Call->FunctionReference.GetMemberName() == TEXT("SetRootMotionMode");
		});
		return Found ? *Found : nullptr;
	}

	/** Fires the FootPlant notify on the class defaults and reads the root motion mode it leaves; 255 when it cannot. */
	uint8 FireNotify(FAutomationTestBase& Test, UAnimBlueprint& Blueprint)
	{
		UAnimInstance* Instance = CastChecked<UAnimInstance>(Blueprint.GeneratedClass->GetDefaultObject());
		Instance->SetRootMotionMode(ERootMotionMode::NoRootMotionExtraction);
		UFunction* Notify = Instance->FindFunction(TEXT("AnimNotify_FootPlant"));
		const FByteProperty* Mode = FindFProperty<FByteProperty>(Instance->GetClass(), TEXT("RootMotionMode"));
		if (!Test.TestNotNull(TEXT("the notify event exists"), Notify) || !Test.TestNotNull(TEXT("RootMotionMode exists"), Mode))
		{
			return 255;
		}
		Instance->ProcessEvent(Notify, nullptr);
		return Mode->GetPropertyValue_InContainer(Instance);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabDisabledNotifyCallTest, "DreamAnim.Generate.DisabledNotifyCall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/** A call disabled in the editor does nothing; after decompiling and building again it still does nothing. */
bool FDabDisabledNotifyCallTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::NotifyChainPrivate;

	UAnimBlueprint* Blueprint = Build(*this, FString(NotifyText).Replace(TEXT("AB_NOTIFY"), TEXT("AB_DisabledCall")));
	UK2Node_CallFunction* Call = Blueprint ? FindRootMotionCall(*Blueprint) : nullptr;
	if (!TestNotNull(TEXT("the notify's call"), Call))
	{
		return false;
	}
	Call->SetEnabledState(ENodeEnabledState::Disabled);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestTrue(TEXT("the Blueprint compiles"), Blueprint->Status != BS_Error);
	const uint8 Unchanged = uint8(ERootMotionMode::NoRootMotionExtraction);
	TestEqual(TEXT("the disabled call does nothing"), FireNotify(*this, *Blueprint), Unchanged);

	const FDabDecompileResult Decompiled = DecompileDabBlueprint(*Blueprint);
	TestEqual(TEXT("nothing is unsupported"), Decompiled.NumUnsupported, 0);
	TestFalse(TEXT("the disabled call is not written"), Decompiled.Text.Contains(TEXT("SetRootMotionMode(")));
	UAnimBlueprint* Rebuilt = Build(*this, Decompiled.Text);
	if (!TestNotNull(TEXT("the text builds again"), Rebuilt))
	{
		return false;
	}
	TestEqual(TEXT("and the notify still does nothing"), FireNotify(*this, *Rebuilt), Unchanged);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabDisabledBranchTest, "DreamAnim.Generate.DisabledBranchInNotify",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/**
 * A disabled branch between a notify and its call: the compiler does not pass execution through a node with
 * two outputs, so the call never runs. Decompiling must not write it as a call that would run; it reports the
 * branch instead.
 */
bool FDabDisabledBranchTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::NotifyChainPrivate;

	UAnimBlueprint* Blueprint = Build(*this, FString(NotifyText).Replace(TEXT("AB_NOTIFY"), TEXT("AB_DisabledBranch")));
	UK2Node_CallFunction* Call = Blueprint ? FindRootMotionCall(*Blueprint) : nullptr;
	UEdGraphPin* CallExec = Call ? Call->GetExecPin() : nullptr;
	UEdGraphPin* EventThen = CallExec && CallExec->LinkedTo.Num() == 1 ? CallExec->LinkedTo[0] : nullptr;
	if (!TestNotNull(TEXT("the notify's call and its event"), EventThen))
	{
		return false;
	}

	UEdGraph* Graph = Call->GetGraph();
	FGraphNodeCreator<UK2Node_IfThenElse> Creator(*Graph);
	UK2Node_IfThenElse* Branch = Creator.CreateNode(false);
	Creator.Finalize();
	const UEdGraphSchema* Schema = Graph->GetSchema();
	EventThen->BreakAllPinLinks();
	TestTrue(TEXT("the event leads to the branch"), Schema->TryCreateConnection(EventThen, Branch->GetExecPin()));
	TestTrue(TEXT("the branch's true side leads to the call"), Schema->TryCreateConnection(Branch->GetThenPin(), CallExec));
	Branch->SetEnabledState(ENodeEnabledState::Disabled);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);
	TestTrue(TEXT("the Blueprint compiles"), Blueprint->Status != BS_Error);
	TestEqual(TEXT("the call behind the disabled branch never runs"), FireNotify(*this, *Blueprint), uint8(ERootMotionMode::NoRootMotionExtraction));

	const FDabDecompileResult Decompiled = DecompileDabBlueprint(*Blueprint);
	TestTrue(TEXT("the branch is reported"), Decompiled.NumUnsupported > 0);
	TestFalse(TEXT("the call is not written as one that runs"), Decompiled.Text.Contains(TEXT("SetRootMotionMode(RootMotionFromEverything)")));
	return true;
}

#endif
