// Copyright DreamMoon. All Rights Reserved.
//
// Write-back: a Blueprint is generated from a small source, changed the way the editor changes it, and
// the change is written back into the text. Each case starts from a fresh copy; the text must come out
// with just that change in it and check to what the Blueprint holds.

#include "Animation/AnimBlueprint.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_Slot.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "Driver/DabDriver.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "WriteBack/DabWriteBack.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Tests::WriteBackTests
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Compiler;

	const TCHAR* const BaseText = TEXT(
		"AnimBlueprint(Name=\"DreamAnimTests/WriteBack/AB_WriteBack\", Root=\"Game\")\n"
		"{\n"
		"    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }\n"
		"\n"
		"    Properties = {\n"
		"        float Speed = 0;\n"
		"        bool bCrouching = false;\n"
		"    }\n"
		"\n"
		"    Template Still(Seq) = SequencePlayer(Sequence = Seq, PlayRate = 0.8);\n"
		"\n"
		"    Graph = {\n"
		"        // Everything comes out of the machine.\n"
		"        Output = Locomotion;\n"
		"    }\n"
		"\n"
		"    StateMachine Locomotion\n"
		"    {\n"
		"        Entry = Idle;\n"
		"\n"
		"        // Standing still.\n"
		"        State Idle\n"
		"        {\n"
		"            Pose = SequencePlayer(Sequence = A_Als_Idle, PlayRate = 1.2);\n"
		"        }\n"
		"\n"
		"        State Crouch\n"
		"        {\n"
		"            Pose = Still(A_Als_Crouch_Pose);\n"
		"        }\n"
		"\n"
		"        Transitions = {\n"
		"            Idle   -> Crouch : Props.bCrouching  [ Blend = 0.2 ];\n"
		"            Crouch -> Idle   : !Props.bCrouching;\n"
		"        }\n"
		"    }\n"
		"}\n");

	FString GetSourcePath()
	{
		FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamAnimTests"), TEXT("WriteBack"), TEXT("AB_WriteBack.dab")));
		FPaths::NormalizeFilename(Path);
		return Path;
	}

	/** A fresh Blueprint from BaseText, in a package of the case's own. */
	UAnimBlueprint* Generate(FAutomationTestBase& Test, const TCHAR* Case)
	{
		FDabCompileOptions Options;
		Options.bGenerate = true;
		Options.bSave = false;
		Options.OverridePackageName = FString::Printf(TEXT("/Game/DreamAnimTests/WriteBack/AB_WriteBack_%s"), Case);
		const FDabCompileResult Result = CompileDabText(GetSourcePath(), BaseText, Options);
		for (const FDabDiagnostic& Diagnostic : Result.Diagnostics)
		{
			if (Diagnostic.Severity == EDabSeverity::Error)
			{
				Test.AddError(FormatDabDiagnosticLine(Diagnostic));
			}
		}
		if (!Result.Blueprint)
		{
			Test.AddError(FString::Printf(TEXT("%s: nothing was generated"), Case));
		}
		return Result.Blueprint;
	}

	UAnimStateNode* FindState(UAnimBlueprint& Blueprint, const FString& Name)
	{
		TArray<UAnimStateNode*> States;
		FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, States);
		UAnimStateNode* const* Found = States.FindByPredicate([&Name](const UAnimStateNode* State) { return State->GetStateName() == Name; });
		return Found ? *Found : nullptr;
	}

	UAnimStateTransitionNode* FindTransition(UAnimBlueprint& Blueprint, const FString& From, const FString& To)
	{
		TArray<UAnimStateTransitionNode*> Transitions;
		FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, Transitions);
		UAnimStateTransitionNode* const* Found = Transitions.FindByPredicate([&From, &To](const UAnimStateTransitionNode* Transition)
		{
			return Transition->GetPreviousState() && Transition->GetNextState()
				&& Transition->GetPreviousState()->GetStateName() == From && Transition->GetNextState()->GetStateName() == To;
		});
		return Found ? *Found : nullptr;
	}

	template <typename NodeType>
	NodeType* FindInGraph(const UEdGraph* Graph)
	{
		if (Graph)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (NodeType* Typed = Cast<NodeType>(Node))
				{
					return Typed;
				}
			}
		}
		return nullptr;
	}

	UEdGraphPin* FindPosePin(UEdGraphNode& Node, EEdGraphPinDirection Direction)
	{
		for (UEdGraphPin* Pin : Node.Pins)
		{
			if (Pin && Pin->Direction == Direction && UAnimationGraphSchema::IsPosePin(Pin->PinType))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	/** Sets a runtime node field the way the details panel does: its pin when it shows one, else the field. */
	bool SetNodeField(FAutomationTestBase& Test, UAnimGraphNode_Base& Node, const TCHAR* Field, const FString& ValueText, UObject* Object = nullptr)
	{
		Node.Modify();
		if (UEdGraphPin* Pin = Node.FindPin(Field, EGPD_Input))
		{
			if (Object)
			{
				Pin->DefaultObject = Object;
			}
			else
			{
				GetDefault<UEdGraphSchema_K2>()->TrySetDefaultValue(*Pin, ValueText);
			}
			return true;
		}
		const FStructProperty* NodeProperty = FindFProperty<FStructProperty>(Node.GetClass(), TEXT("Node"));
		const FProperty* Property = NodeProperty ? FindFProperty<FProperty>(NodeProperty->Struct, Field) : nullptr;
		if (!Property)
		{
			Test.AddError(FString::Printf(TEXT("%s has no field %s"), *Node.GetClass()->GetName(), Field));
			return false;
		}
		void* Value = Property->ContainerPtrToValuePtr<void>(NodeProperty->ContainerPtrToValuePtr<void>(&Node));
		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property); ObjectProperty && Object)
		{
			ObjectProperty->SetObjectPropertyValue(Value, Object);
			return true;
		}
		return Property->ImportText_Direct(*ValueText, Value, &Node, PPF_None) != nullptr;
	}

	FDabWriteBackResult WriteBack(UAnimBlueprint& Blueprint)
	{
		return WriteBackDabBlueprint(Blueprint, GetSourcePath(), BaseText);
	}

	FString DescribeResult(const FDabWriteBackResult& Result)
	{
		TArray<FString> Lines;
		for (const FDabWriteBackChange& Change : Result.Changes)
		{
			Lines.Add(FString::Printf(TEXT("  change %s: %s%s%s"), *Change.Location, *Change.What, Change.bWritten ? TEXT("") : TEXT(" (not written)"),
				Change.bRewroteBlock ? TEXT(" (block rewritten)") : TEXT("")));
		}
		for (const FDabDiagnostic& Diagnostic : Result.Diagnostics)
		{
			Lines.Add(TEXT("  ") + FormatDabDiagnosticLine(Diagnostic));
		}
		Lines.Add(TEXT("  text:\n") + Result.Text);
		return FString::Join(Lines, TEXT("\n"));
	}

	bool HasCode(const FDabWriteBackResult& Result, const TCHAR* Code)
	{
		return Result.Diagnostics.ContainsByPredicate([Code](const FDabDiagnostic& Diagnostic) { return Diagnostic.Code == Code; });
	}

	/** Written, checks to the Blueprint, and nothing refused. */
	bool ExpectWritten(FAutomationTestBase& Test, const TCHAR* Case, const FDabWriteBackResult& Result)
	{
		const bool bClean = Result.bCompared && Result.bFixedPoint && Result.HasEdits() && Result.Diagnostics.IsEmpty();
		if (!bClean)
		{
			Test.AddError(FString::Printf(TEXT("%s: compared %d, fixed point %d, edits %d\n%s"), Case, Result.bCompared, Result.bFixedPoint, Result.HasEdits(),
				*DescribeResult(Result)));
		}
		return bClean;
	}

	/** The text's lines that BaseText does not have (trimmed), then `-` and the ones it lost. */
	TArray<FString> ChangedLines(const FString& Text)
	{
		TArray<FString> Before;
		TArray<FString> After;
		FString(BaseText).ParseIntoArray(Before, TEXT("\n"), false);
		Text.ParseIntoArray(After, TEXT("\n"), false);
		TArray<FString> Lost = Before;
		TArray<FString> Changed;
		for (const FString& Line : After)
		{
			if (Lost.RemoveSingle(Line) == 0)
			{
				Changed.Add(Line.TrimStartAndEnd());
			}
		}
		for (const FString& Line : Lost)
		{
			Changed.Add(TEXT("-") + Line.TrimStartAndEnd());
		}
		return Changed;
	}

	UAnimSequenceBase* LoadSequence(const TCHAR* Name)
	{
		return LoadObject<UAnimSequenceBase>(nullptr, *FString::Printf(TEXT("/ALS/ALS/Animations/Base/%s.%s"), Name, Name));
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabWriteBackValuesTest, "DreamAnim.WriteBack.Values",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDabWriteBackValuesTest::RunTest(const FString& Parameters)
	{
		// Untouched: the text already says everything.
		if (UAnimBlueprint* Blueprint = Generate(*this, TEXT("Same")))
		{
			const FDabWriteBackResult Result = WriteBack(*Blueprint);
			if (!(Result.bCompared && Result.bFixedPoint && !Result.HasEdits() && Result.Diagnostics.IsEmpty()))
			{
				AddError(TEXT("an untouched Blueprint already agrees with its text\n") + DescribeResult(Result));
			}
			TestEqualSensitive(TEXT("an untouched Blueprint leaves the text alone"), Result.Text, FString(BaseText));
		}

		// A number on a node: just that number changes.
		if (UAnimBlueprint* Blueprint = Generate(*this, TEXT("PlayRate")))
		{
			UAnimGraphNode_SequencePlayer* Player = FindInGraph<UAnimGraphNode_SequencePlayer>(FindState(*Blueprint, TEXT("Idle"))->BoundGraph);
			if (TestNotNull(TEXT("Idle's player"), Player) && SetNodeField(*this, *Player, TEXT("PlayRate"), TEXT("1.5")))
			{
				const FDabWriteBackResult Result = WriteBack(*Blueprint);
				ExpectWritten(*this, TEXT("PlayRate"), Result);
				TestEqualSensitive(TEXT("only the number is replaced"), Result.Text, FString(BaseText).Replace(TEXT("PlayRate = 1.2"), TEXT("PlayRate = 1.5")));
			}
		}

		// An option the text leaves at its default: the call is reprinted with it.
		if (UAnimBlueprint* Blueprint = Generate(*this, TEXT("Loop")))
		{
			UAnimGraphNode_SequencePlayer* Player = FindInGraph<UAnimGraphNode_SequencePlayer>(FindState(*Blueprint, TEXT("Idle"))->BoundGraph);
			if (TestNotNull(TEXT("Idle's player"), Player) && SetNodeField(*this, *Player, TEXT("bLoopAnimation"), TEXT("false")))
			{
				const FDabWriteBackResult Result = WriteBack(*Blueprint);
				ExpectWritten(*this, TEXT("Loop"), Result);
				const TArray<FString> Changed = ChangedLines(Result.Text);
				TestTrue(TEXT("one line, with the new option"), Changed.Num() == 2 && Changed[0].Contains(TEXT("bLoopAnimation = false"))
					&& Changed[0].Contains(TEXT("A_Als_Idle")) && Changed[0].Contains(TEXT("PlayRate = 1.2")));
			}
		}

		// A template's argument: written where the template is called.
		if (UAnimBlueprint* Blueprint = Generate(*this, TEXT("TemplateArgument")))
		{
			UAnimGraphNode_SequencePlayer* Player = FindInGraph<UAnimGraphNode_SequencePlayer>(FindState(*Blueprint, TEXT("Crouch"))->BoundGraph);
			UAnimSequenceBase* Stand = LoadSequence(TEXT("A_Als_Stand_Pose"));
			if (TestNotNull(TEXT("Crouch's player"), Player) && TestNotNull(TEXT("the stand pose"), Stand)
				&& SetNodeField(*this, *Player, TEXT("Sequence"), FString(), Stand))
			{
				const FDabWriteBackResult Result = WriteBack(*Blueprint);
				ExpectWritten(*this, TEXT("TemplateArgument"), Result);
				TestEqualSensitive(TEXT("the argument is replaced"), Result.Text,
					FString(BaseText).Replace(TEXT("Still(A_Als_Crouch_Pose)"), TEXT("Still(A_Als_Stand_Pose)")));
			}
		}

		// A template's own constant: every use would change, so it is refused.
		if (UAnimBlueprint* Blueprint = Generate(*this, TEXT("TemplateBody")))
		{
			UAnimGraphNode_SequencePlayer* Player = FindInGraph<UAnimGraphNode_SequencePlayer>(FindState(*Blueprint, TEXT("Crouch"))->BoundGraph);
			if (TestNotNull(TEXT("Crouch's player"), Player) && SetNodeField(*this, *Player, TEXT("PlayRate"), TEXT("0.9")))
			{
				const FDabWriteBackResult Result = WriteBack(*Blueprint);
				if (!HasCode(Result, TEXT("DAB8001")) || Result.bFixedPoint)
				{
					AddError(TEXT("a template body's value is refused\n") + DescribeResult(Result));
				}
				TestEqualSensitive(TEXT("a refused change leaves the text alone"), Result.Text, FString(BaseText));
			}
		}

		// A transition's blend time: the row says it.
		if (UAnimBlueprint* Blueprint = Generate(*this, TEXT("Crossfade")))
		{
			UAnimStateTransitionNode* Transition = FindTransition(*Blueprint, TEXT("Idle"), TEXT("Crouch"));
			if (TestNotNull(TEXT("Idle -> Crouch"), Transition))
			{
				Transition->Modify();
				Transition->CrossfadeDuration = 0.35f;
				const FDabWriteBackResult Result = WriteBack(*Blueprint);
				ExpectWritten(*this, TEXT("Crossfade"), Result);
				const TArray<FString> Changed = ChangedLines(Result.Text);
				TestTrue(TEXT("the row has the new blend time"), Changed.Num() >= 2 && Changed[0].Contains(TEXT("Blend = 0.35")));
			}
		}

		// A state's setting: in the state's header.
		if (UAnimBlueprint* Blueprint = Generate(*this, TEXT("StateSetting")))
		{
			UAnimStateNode* Crouch = FindState(*Blueprint, TEXT("Crouch"));
			if (TestNotNull(TEXT("Crouch"), Crouch))
			{
				Crouch->Modify();
				Crouch->bAlwaysResetOnEntry = true;
				const FDabWriteBackResult Result = WriteBack(*Blueprint);
				ExpectWritten(*this, TEXT("StateSetting"), Result);
				const TArray<FString> Changed = ChangedLines(Result.Text);
				TestEqualSensitive(TEXT("the state's header has the setting"), FString::Join(Changed, TEXT(" | ")), FString(TEXT("State Crouch [ bAlwaysResetOnEntry = true ] | -State Crouch")));
			}
		}

		// A variable's default: in Properties.
		if (UAnimBlueprint* Blueprint = Generate(*this, TEXT("Variable")))
		{
			UObject* Defaults = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
			const FProperty* Speed = Blueprint->GeneratedClass ? FindFProperty<FProperty>(Blueprint->GeneratedClass, TEXT("Speed")) : nullptr;
			if (TestNotNull(TEXT("Speed"), Speed) && Defaults)
			{
				Speed->ImportText_InContainer(TEXT("2.5"), Defaults, Defaults, PPF_None);
				const FDabWriteBackResult Result = WriteBack(*Blueprint);
				ExpectWritten(*this, TEXT("Variable"), Result);
				const TArray<FString> Changed = ChangedLines(Result.Text);
				TestEqualSensitive(TEXT("the declaration has the new default"), FString::Join(Changed, TEXT(" | ")), FString(TEXT("float Speed = 2.5; | -float Speed = 0;")));
			}
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabWriteBackStructureTest, "DreamAnim.WriteBack.Structure",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDabWriteBackStructureTest::RunTest(const FString& Parameters)
	{
		// A state and a transition into it, placed by hand.
		if (UAnimBlueprint* Blueprint = Generate(*this, TEXT("AddState")))
		{
			UAnimStateNode* Idle = FindState(*Blueprint, TEXT("Idle"));
			UAnimationStateMachineGraph* Machine = Idle ? Cast<UAnimationStateMachineGraph>(Idle->GetGraph()) : nullptr;
			UAnimSequenceBase* Stand = LoadSequence(TEXT("A_Als_Stand_Pose"));
			if (TestNotNull(TEXT("the machine"), Machine) && TestNotNull(TEXT("the stand pose"), Stand))
			{
				FGraphNodeCreator<UAnimStateNode> StateCreator(*Machine);
				UAnimStateNode* State = StateCreator.CreateNode(false);
				StateCreator.Finalize();
				State->OnRenameNode(TEXT("Stand"));

				UAnimationStateGraph* StateGraph = Cast<UAnimationStateGraph>(State->BoundGraph);
				FGraphNodeCreator<UAnimGraphNode_SequencePlayer> PlayerCreator(*StateGraph);
				UAnimGraphNode_SequencePlayer* Player = PlayerCreator.CreateNode(false);
				PlayerCreator.Finalize();
				SetNodeField(*this, *Player, TEXT("Sequence"), FString(), Stand);
				StateGraph->GetSchema()->TryCreateConnection(FindPosePin(*Player, EGPD_Output), FindPosePin(*StateGraph->GetResultNode(), EGPD_Input));

				FGraphNodeCreator<UAnimStateTransitionNode> TransitionCreator(*Machine);
				UAnimStateTransitionNode* Transition = TransitionCreator.CreateNode(false);
				TransitionCreator.Finalize();
				Transition->CreateConnections(Idle, State);

				const FDabWriteBackResult Result = WriteBack(*Blueprint);
				ExpectWritten(*this, TEXT("AddState"), Result);
				TestTrue(TEXT("the new state"), Result.Text.Contains(TEXT("State Stand")) && Result.Text.Contains(TEXT("A_Als_Stand_Pose")));
				TestTrue(TEXT("the new transition"), Result.Text.Contains(TEXT("-> Stand")));
				TestTrue(TEXT("the comment on a kept state stays"), Result.Text.Contains(TEXT("// Standing still.")));
				TestTrue(TEXT("the template call stays"), Result.Text.Contains(TEXT("Still(A_Als_Crouch_Pose)")));
			}
		}

		// A transition removed.
		if (UAnimBlueprint* Blueprint = Generate(*this, TEXT("RemoveTransition")))
		{
			UAnimStateTransitionNode* Transition = FindTransition(*Blueprint, TEXT("Crouch"), TEXT("Idle"));
			if (TestNotNull(TEXT("Crouch -> Idle"), Transition))
			{
				Transition->DestroyNode();
				const FDabWriteBackResult Result = WriteBack(*Blueprint);
				ExpectWritten(*this, TEXT("RemoveTransition"), Result);
				TestFalse(TEXT("the row is gone"), Result.Text.Contains(TEXT("!Props.bCrouching")));
				TestTrue(TEXT("the other row stays"), Result.Text.Contains(TEXT("Props.bCrouching")));
			}
		}

		// A node put between the machine and the output.
		if (UAnimBlueprint* Blueprint = Generate(*this, TEXT("InsertNode")))
		{
			UEdGraph* AnimGraph = nullptr;
			for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				if (Graph && Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph)
				{
					AnimGraph = Graph;
				}
			}
			UAnimGraphNode_Root* Root = FindInGraph<UAnimGraphNode_Root>(AnimGraph);
			UAnimGraphNode_StateMachineBase* MachineNode = FindInGraph<UAnimGraphNode_StateMachineBase>(AnimGraph);
			if (TestNotNull(TEXT("the output"), Root) && TestNotNull(TEXT("the machine node"), MachineNode))
			{
				FGraphNodeCreator<UAnimGraphNode_Slot> SlotCreator(*AnimGraph);
				UAnimGraphNode_Slot* Slot = SlotCreator.CreateNode(false);
				SlotCreator.Finalize();
				UEdGraphPin* RootInput = FindPosePin(*Root, EGPD_Input);
				RootInput->BreakAllPinLinks();
				const UEdGraphSchema* Schema = AnimGraph->GetSchema();
				Schema->TryCreateConnection(FindPosePin(*MachineNode, EGPD_Output), FindPosePin(*Slot, EGPD_Input));
				Schema->TryCreateConnection(FindPosePin(*Slot, EGPD_Output), RootInput);

				const FDabWriteBackResult Result = WriteBack(*Blueprint);
				ExpectWritten(*this, TEXT("InsertNode"), Result);
				TestTrue(TEXT("the slot is in the graph"), Result.Text.Contains(TEXT("Slot(")));
				TestTrue(TEXT("the graph's comment stays"), Result.Text.Contains(TEXT("// Everything comes out of the machine.")));
				TestTrue(TEXT("the machine is untouched"), Result.Text.Contains(TEXT("Pose = SequencePlayer(Sequence = A_Als_Idle, PlayRate = 1.2);")));
			}
		}
		return true;
	}
}

#endif
