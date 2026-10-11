// Copyright DreamMoon. All Rights Reserved.
//
// The state a machine getter reads, through generation and decompiling: a time getter keeps the state it
// names, a getter in a shared rule reads each transition's own source state, and a state renamed to an
// identifier is found in its own machine, not in another that has a state of the same name.

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/SkeletalMeshActor.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationStateGraph.h"
#include "Components/SkeletalMeshComponent.h"
#include "Driver/DabDriver.h"
#include "Engine/SkeletalMesh.h"
#include "K2Node_AnimGetter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Tests::MachineGetterPrivate
{
	using namespace UE::DreamAnim;
	using namespace UE::DreamAnim::Compiler;

	FString MakeSource(const TCHAR* Name, const FString& Body)
	{
		return FString::Printf(TEXT("AnimBlueprint(Name=\"DreamAnimTests/MachineGetters/%s\", Root=\"Game\")\n{\n    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }\n%s\n}\n"),
			Name, *Body);
	}

	UAnimBlueprint* Build(FAutomationTestBase& Test, const FString& Text)
	{
		FDabCompileOptions Options;
		Options.bSave = false;
		const FDabCompileResult Result = CompileDabText(TEXT("MachineGetters.dab"), Text, Options);
		for (const Lang::FDabDiagnostic& Diagnostic : Result.Diagnostics)
		{
			if (Diagnostic.Severity == Lang::EDabSeverity::Error)
			{
				Test.AddError(FormatDabDiagnosticLine(Diagnostic));
			}
		}
		return Result.Succeeded() ? Result.Blueprint : nullptr;
	}

	/** The Blueprint's text, which must hold everything the Blueprint does. */
	FString Decompile(FAutomationTestBase& Test, const UAnimBlueprint& Blueprint)
	{
		const FDabDecompileResult Result = DecompileDabBlueprint(Blueprint);
		Test.TestTrue(TEXT("it decompiles"), Result.bSucceeded);
		Test.TestEqual(TEXT("with nothing unsupported"), Result.NumUnsupported, 0);
		return Result.Text;
	}

	/** Every time getter (TimeRemaining, TimeRemainingRatio) of the Blueprint reads Expected; there are Count of them. */
	void ExpectTimeTargets(FAutomationTestBase& Test, UAnimBlueprint& Blueprint, const TCHAR* Stage, const TCHAR* Expected, int32 Count)
	{
		TArray<UK2Node_AnimGetter*> Getters;
		FBlueprintEditorUtils::GetAllNodesOfClass(&Blueprint, Getters);
		int32 Found = 0;
		for (const UK2Node_AnimGetter* Getter : Getters)
		{
			const FName Function = Getter->FunctionReference.GetMemberName();
			if (Function != TEXT("GetRelevantAnimTimeRemaining") && Function != TEXT("GetRelevantAnimTimeRemainingFraction"))
			{
				continue;
			}
			++Found;
			const UAnimStateNode* State = Cast<UAnimStateNode>(Getter->SourceStateNode.Get());
			const FString Actual = State ? State->GetStateName() : FString(TEXT("<none>"));
			Test.AddInfo(FString::Printf(TEXT("%s: %s reads %s"), Stage, *Function.ToString(), *Actual));
			Test.TestEqual(FString::Printf(TEXT("%s: the getter reads the state it names"), Stage), Actual, FString(Expected));
		}
		Test.TestEqual(FString::Printf(TEXT("%s: every time getter is there"), Stage), Found, Count);
	}

	/** The state machine M's current state after ten frames on the ALS mesh. */
	FName RunMachine(FAutomationTestBase& Test, UAnimBlueprint& Blueprint)
	{
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/ALS/ALS/Character/SKM_Als.SKM_Als"));
		FTestWorldWrapper Wrapper;
		if (!Mesh || !Wrapper.CreateTestWorld(EWorldType::Game) || !Wrapper.BeginPlayInTestWorld())
		{
			Test.AddError(TEXT("No ALS mesh or test world"));
			return NAME_None;
		}
		ON_SCOPE_EXIT
		{
			Wrapper.DestroyTestWorld(false);
		};
		ASkeletalMeshActor* Actor = Wrapper.GetTestWorld()->SpawnActor<ASkeletalMeshActor>();
		USkeletalMeshComponent& Component = *Actor->GetSkeletalMeshComponent();
		Component.SetSkeletalMesh(Mesh);
		Component.SetAnimationMode(EAnimationMode::AnimationBlueprint);
		Component.SetAnimInstanceClass(Blueprint.GeneratedClass);
		Component.VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		UAnimInstance* Instance = Component.GetAnimInstance();
		if (!Test.TestNotNull(TEXT("the generated class makes an instance"), Instance))
		{
			return NAME_None;
		}
		for (int32 Frame = 0; Frame < 10; ++Frame)
		{
			FApp::SetDeltaTime(1.f / 30.f);
			Wrapper.TickTestWorld(1.f / 30.f);
		}
		return Instance->GetCurrentStateName(Instance->GetStateMachineIndex(TEXT("M")));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabTimeGetterRoundTripTest, "DreamAnim.Generate.TimeGetterRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/** A transition reading the time left in another state than its source: the state survives decompiling. */
bool FDabTimeGetterRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::MachineGetterPrivate;

	UAnimBlueprint* Blueprint = Build(*this, MakeSource(TEXT("AB_TimeTargets"),
		TEXT("    Graph = { Output = M; }\n"
			"    StateMachine M { Entry = A; State A { Pose = LocalRefPose(); } State B { Pose = LocalRefPose(); }\n"
			"        Transitions = { A -> B : TimeRemaining(B) < 0.1 && TimeRemainingRatio(B) < 0.1; } }")));
	if (!Blueprint)
	{
		return false;
	}
	ExpectTimeTargets(*this, *Blueprint, TEXT("Built"), TEXT("B"), 2);
	if (UAnimBlueprint* Rebuilt = Build(*this, Decompile(*this, *Blueprint)))
	{
		ExpectTimeTargets(*this, *Rebuilt, TEXT("Rebuilt"), TEXT("B"), 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabTimeGetterNodeBindingTest, "DreamAnim.Generate.TimeGetterNodeBinding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/** A node argument inside a state reading the time left in that state: the state survives decompiling. */
bool FDabTimeGetterNodeBindingTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::MachineGetterPrivate;

	UAnimBlueprint* Blueprint = Build(*this, MakeSource(TEXT("AB_TimeBinding"),
		TEXT("    Graph = { Output = M; }\n"
			"    StateMachine M { Entry = A; State A { Pose = TwoWayBlend(A = LocalRefPose(), B = LocalRefPose(), Alpha = TimeRemaining(A)); } }")));
	if (!Blueprint)
	{
		return false;
	}
	ExpectTimeTargets(*this, *Blueprint, TEXT("Built"), TEXT("A"), 1);
	if (UAnimBlueprint* Rebuilt = Build(*this, Decompile(*this, *Blueprint)))
	{
		ExpectTimeTargets(*this, *Rebuilt, TEXT("Rebuilt"), TEXT("A"), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabStateNameScopesTest, "DreamAnim.Generate.StateNameScopes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/**
 * Two machines each with a state named "Rotate Left" in the engine, one of them also with `RotateLeft`:
 * renaming them to identifiers happens per machine, so a StateWeight getter still reads its own machine
 * after decompiling and building again.
 */
bool FDabStateNameScopesTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::MachineGetterPrivate;

	UAnimBlueprint* Blueprint = Build(*this, MakeSource(TEXT("AB_StateNames"),
		TEXT("    Graph = { Output = TwoWayBlend(A = First, B = Second, Alpha = 0.5); }\n"
			"    StateMachine First { Entry = FirstSpaced; State FirstSpaced { Pose = TwoWayBlend(A = LocalRefPose(), B = LocalRefPose(), Alpha = StateWeight(FirstSpaced)); } }\n"
			"    StateMachine Second { Entry = RotateLeft; State RotateLeft { Pose = LocalRefPose(); } State SecondSpaced { Pose = LocalRefPose(); } }")));
	if (!Blueprint)
	{
		return false;
	}
	TArray<UAnimStateNode*> States;
	FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, States);
	int32 Renamed = 0;
	for (UAnimStateNode* State : States)
	{
		if (State->BoundGraph && (State->BoundGraph->GetName() == TEXT("FirstSpaced") || State->BoundGraph->GetName() == TEXT("SecondSpaced")))
		{
			TestTrue(TEXT("the state takes a name with a space"), State->BoundGraph->Rename(TEXT("Rotate Left"), nullptr, REN_DontCreateRedirectors));
			++Renamed;
		}
	}
	if (!TestEqual(TEXT("one such state in each machine"), Renamed, 2))
	{
		return false;
	}

	TArray<UK2Node_AnimGetter*> Before;
	FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Before);
	if (!TestEqual(TEXT("one StateWeight getter"), Before.Num(), 1) || !TestNotNull(TEXT("reading a state"), Before[0]->SourceStateNode.Get()))
	{
		return false;
	}
	const FString OriginalMachine = Before[0]->SourceStateNode->GetGraph()->GetName();

	UAnimBlueprint* Rebuilt = Build(*this, Decompile(*this, *Blueprint));
	if (!TestNotNull(TEXT("the text builds again"), Rebuilt))
	{
		return false;
	}
	TArray<UK2Node_AnimGetter*> After;
	FBlueprintEditorUtils::GetAllNodesOfClass(Rebuilt, After);
	if (!TestEqual(TEXT("one StateWeight getter after"), After.Num(), 1) || !TestNotNull(TEXT("reading a state after"), After[0]->SourceStateNode.Get()))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("The getter reads machine %s, then %s"), *OriginalMachine, *After[0]->SourceStateNode->GetGraph()->GetName()));
	TestEqualSensitive(TEXT("the getter still reads its own machine"), After[0]->SourceStateNode->GetGraph()->GetName(), OriginalMachine);
	return true;
}

#endif
