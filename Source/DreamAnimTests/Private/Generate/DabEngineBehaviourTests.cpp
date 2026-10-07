// Copyright DreamMoon. All Rights Reserved.
//
// What the engine does with constructs the checker and the generator accept but the engine's own editor
// would not offer: each Blueprint is generated, compiled and run on the ALS mesh in a test world, and its
// pose compared with one whose result is known.
//
//   MachineGetters      StateWeight and MachineWeight in a node argument inside a state
//
// A cache used in another graph than the one that saves it was tried the same way; the engine's compiler
// refuses it, so the checker reports it (DAB6017, Tests/Corpus/Validate).

#include "Animation/AnimInstance.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "Driver/DabDriver.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Tests::EngineBehaviourPrivate
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Compiler;

	/** A document whose AnimGraph outputs one frame of an ALS pose: the known result. */
	FString MakePoseText(const TCHAR* Name, const TCHAR* Pose)
	{
		return FString::Printf(TEXT(
			"AnimBlueprint(Name=\"DreamAnimTests/Engine/%s\", Root=\"Game\")\n"
			"{\n"
			"    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }\n"
			"    Graph = { Output = SequenceEvaluator(Sequence = %s, ExplicitFrame = 0); }\n"
			"}\n"), Name, Pose);
	}

	/** The text compiled in memory; its class, or null after reporting why. */
	UClass* Generate(FAutomationTestBase& Test, const TCHAR* Name, const FString& Text)
	{
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamAnimTests") / TEXT("Engine") / Name) + TEXT(".dab");
		FDabCompileOptions Options;
		Options.bSave = false;
		const FDabCompileResult Result = CompileDabText(Path, Text, Options);
		for (const FDabDiagnostic& Diagnostic : Result.Diagnostics)
		{
			if (Diagnostic.Severity == EDabSeverity::Error)
			{
				Test.AddError(FormatDabDiagnosticLine(Diagnostic));
			}
			else
			{
				Test.AddInfo(FormatDabDiagnosticLine(Diagnostic));
			}
		}
		return Result.Succeeded() && Result.Blueprint ? Result.Blueprint->GeneratedClass.Get() : nullptr;
	}

	/** The component-space pose of the ALS mesh running AnimClass for a few frames; empty after reporting why. */
	TArray<FTransform> RunPose(FAutomationTestBase& Test, UClass* AnimClass)
	{
		USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/ALS/ALS/Character/SKM_Als.SKM_Als"));
		FTestWorldWrapper Wrapper;
		if (!Mesh || !AnimClass || !Wrapper.CreateTestWorld(EWorldType::Game) || !Wrapper.BeginPlayInTestWorld())
		{
			Test.AddError(TEXT("No ALS mesh, class or test world"));
			return {};
		}
		ON_SCOPE_EXIT
		{
			Wrapper.DestroyTestWorld(false);
		};
		ASkeletalMeshActor* Actor = Wrapper.GetTestWorld()->SpawnActor<ASkeletalMeshActor>();
		USkeletalMeshComponent& Component = *Actor->GetSkeletalMeshComponent();
		Component.SetSkeletalMesh(Mesh);
		Component.SetAnimationMode(EAnimationMode::AnimationBlueprint);
		Component.SetAnimInstanceClass(AnimClass);
		Component.VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		if (!Component.GetAnimInstance())
		{
			Test.AddError(FString::Printf(TEXT("%s makes no anim instance"), *AnimClass->GetName()));
			return {};
		}
		for (int32 Frame = 0; Frame < 5; ++Frame)
		{
			FApp::SetDeltaTime(1.f / 30.f);
			Wrapper.TickTestWorld(1.f / 30.f);
		}
		return Component.GetComponentSpaceTransforms();
	}

	/** The largest distance between the same bone in two poses, in centimetres. */
	double LargestOffset(const TArray<FTransform>& A, const TArray<FTransform>& B)
	{
		double Largest = A.Num() == B.Num() && A.Num() > 0 ? 0.0 : TNumericLimits<double>::Max();
		for (int32 Bone = 0; Bone < FMath::Min(A.Num(), B.Num()); ++Bone)
		{
			Largest = FMath::Max(Largest, FVector::Dist(A[Bone].GetLocation(), B[Bone].GetLocation()));
		}
		return Largest;
	}

	/** Runs Text and checks it gives the crouch pose, not the standing one. */
	void ExpectCrouchPose(FAutomationTestBase& Test, const TCHAR* Name, const FString& Text)
	{
		UClass* Class = Generate(Test, Name, Text);
		UClass* Crouch = Generate(Test, TEXT("AB_CrouchPose"), MakePoseText(TEXT("AB_CrouchPose"), TEXT("A_Als_Crouch_Pose")));
		UClass* Stand = Generate(Test, TEXT("AB_StandPose"), MakePoseText(TEXT("AB_StandPose"), TEXT("A_Als_Stand_Pose")));
		if (!Class || !Crouch || !Stand)
		{
			return;
		}
		const TArray<FTransform> Pose = RunPose(Test, Class);
		const double FromCrouch = LargestOffset(Pose, RunPose(Test, Crouch));
		const double FromStand = LargestOffset(Pose, RunPose(Test, Stand));
		Test.TestTrue(FString::Printf(TEXT("%s gives the crouch pose (%.4f cm off)"), Name, FromCrouch), FromCrouch < 0.01);
		Test.TestTrue(FString::Printf(TEXT("which is not the standing pose (%.2f cm off)"), FromStand), FromStand > 5.0);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabMachineGettersTest, "DreamAnim.Generate.MachineGetters",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/** StateWeight and MachineWeight read in node arguments inside a state: both are 1 there, so the blends pick crouch. */
bool FDabMachineGettersTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::EngineBehaviourPrivate;
	ExpectCrouchPose(*this, TEXT("AB_MachineGetters"), TEXT(
		"AnimBlueprint(Name=\"DreamAnimTests/Engine/AB_MachineGetters\", Root=\"Game\")\n"
		"{\n"
		"    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }\n"
		"\n"
		"    Graph = { Output = Locomotion; }\n"
		"\n"
		"    StateMachine Locomotion\n"
		"    {\n"
		"        Entry = Idle;\n"
		"\n"
		"        State Idle\n"
		"        {\n"
		"            Pose = TwoWayBlend(\n"
		"                Alpha = StateWeight(Idle),\n"
		"                A     = SequenceEvaluator(Sequence = A_Als_Stand_Pose, ExplicitFrame = 0),\n"
		"                B     = TwoWayBlend(\n"
		"                    Alpha = MachineWeight(),\n"
		"                    A     = SequenceEvaluator(Sequence = A_Als_Stand_Pose, ExplicitFrame = 0),\n"
		"                    B     = SequenceEvaluator(Sequence = A_Als_Crouch_Pose, ExplicitFrame = 0)\n"
		"                )\n"
		"            );\n"
		"        }\n"
		"    }\n"
		"}\n"));
	return true;
}

#endif
