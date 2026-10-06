// Copyright DreamMoon. All Rights Reserved.
//
// Animation sets end to end: a Blueprint generated from text reads its asset slot from the active set,
// and swapping the set on a running instance changes the pose on the next update; swapping back gives
// the first pose again. Uses the ALS skeleton and animations, so it lives with the ALS corpus tests.

#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/SkeletalMeshActor.h"
#include "Components/SkeletalMeshComponent.h"
#include "DreamAnimLibrary.h"
#include "DreamAnimSet.h"
#include "Engine/SkeletalMesh.h"
#include "Generate/DabGenerate.h"
#include "Lang/DabParser.h"
#include "Lang/DabTemplates.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Semantic/DabSemantic.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Tests::AnimSetSwapPrivate
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;
	using namespace UE::DreamAnim::Compiler;

	const TCHAR* const SwapText = TEXT(R"DAB(
AnimBlueprint(Name="DreamAnimTests/AnimSetSwap", Root="Game")
{
    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }
    Assets = { AnimSequence Pose; }
    Graph = { Output = SequenceEvaluator(Sequence = Assets.Pose, ExplicitFrame = 0); }
}
)DAB");

	/** The text checked and generated; its class, or null after reporting why. */
	UClass* GenerateSwapBlueprint(FAutomationTestBase& Test)
	{
		FDabCompilationUnit Unit;
		FDabDiagnosticSink Sink(TEXT("AnimSetSwap.dab"));
		Unit.MainSource = FDabSourceText(TEXT("AnimSetSwap.dab"), SwapText);
		Unit.Main = ParseDab(Unit.MainSource, Sink);
		FDabFileSystemIncludeLoader Loader;
		ResolveDabIncludes(Unit, Loader, Sink);
		ExpandDabTemplates(Unit, Sink);
		FDabModel Model;
		BuildDabModel(Unit, Model, Sink);

		FDabGenerateOptions Options;
		Options.bSave = false;
		const FDabGenerateResult Result = Sink.HasErrors() ? FDabGenerateResult() : GenerateDabBlueprint(Model, Sink, Options);
		for (const FDabDiagnostic& Diagnostic : Sink.GetDiagnostics())
		{
			Test.AddError(FDabDiagnosticSink::ToWireString(Diagnostic));
		}
		for (const FString& Error : Result.CompilerErrors)
		{
			Test.AddError(FString::Printf(TEXT("Compiler: %s"), *Error));
		}
		return Result.Succeeded() && Result.Blueprint ? Result.Blueprint->GeneratedClass.Get() : nullptr;
	}

	UDreamAnimSet* MakeSet(USkeleton* Skeleton, UAnimSequence* Pose)
	{
		UDreamAnimSet* Set = NewObject<UDreamAnimSet>(GetTransientPackage());
		Set->Skeleton = Skeleton;
		Set->SyncSlots(TEXT("AnimSetSwap"), { TPair<FName, FName>(TEXT("Pose"), TEXT("AnimSequence")) });
		Set->SetAsset(TEXT("AnimSetSwap"), TEXT("Pose"), Pose);
		return Set;
	}

	/** The largest distance between the same bone in two component-space poses, in centimetres. */
	double LargestOffset(const TArray<FTransform>& A, const TArray<FTransform>& B)
	{
		double Largest = A.Num() == B.Num() ? 0.0 : TNumericLimits<double>::Max();
		for (int32 Bone = 0; Bone < FMath::Min(A.Num(), B.Num()); ++Bone)
		{
			Largest = FMath::Max(Largest, FVector::Dist(A[Bone].GetLocation(), B[Bone].GetLocation()));
		}
		return Largest;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabAlsAnimSetSwapTest, "DreamAnim.Corpus.Als.AnimSetSwap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDabAlsAnimSetSwapTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::AnimSetSwapPrivate;

	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/ALS/ALS/Character/SKM_Als.SKM_Als"));
	UAnimSequence* Stand = LoadObject<UAnimSequence>(nullptr, TEXT("/ALS/ALS/Animations/Base/A_Als_Stand_Pose.A_Als_Stand_Pose"));
	UAnimSequence* Crouch = LoadObject<UAnimSequence>(nullptr, TEXT("/ALS/ALS/Animations/Base/A_Als_Crouch_Pose.A_Als_Crouch_Pose"));
	if (!Mesh || !Stand || !Crouch)
	{
		AddError(TEXT("The ALS mesh or poses cannot be loaded"));
		return false;
	}
	UClass* AnimClass = GenerateSwapBlueprint(*this);
	if (!AnimClass)
	{
		return false;
	}
	UDreamAnimSet* StandSet = MakeSet(Mesh->GetSkeleton(), Stand);
	UDreamAnimSet* CrouchSet = MakeSet(Mesh->GetSkeleton(), Crouch);

	FTestWorldWrapper Wrapper;
	if (!Wrapper.CreateTestWorld(EWorldType::Game) || !Wrapper.BeginPlayInTestWorld())
	{
		AddError(TEXT("No test world"));
		return false;
	}
	ON_SCOPE_EXIT
	{
		Wrapper.DestroyTestWorld(false);
	};
	UWorld& World = *Wrapper.GetTestWorld();
	ASkeletalMeshActor* Actor = World.SpawnActor<ASkeletalMeshActor>();
	USkeletalMeshComponent& Component = *Actor->GetSkeletalMeshComponent();
	Component.SetSkeletalMesh(Mesh);
	Component.SetAnimationMode(EAnimationMode::AnimationBlueprint);
	Component.SetAnimInstanceClass(AnimClass);
	Component.VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	UAnimInstance* Instance = Component.GetAnimInstance();
	if (!Instance)
	{
		AddError(TEXT("The generated class makes no anim instance"));
		return false;
	}

	const auto PoseWith = [&](UDreamAnimSet* Set)
	{
		UDreamAnimLibrary::SetAnimSet(Instance, Set);
		for (int32 Frame = 0; Frame < 3; ++Frame)
		{
			FApp::SetDeltaTime(1.f / 30.f);
			Wrapper.TickTestWorld(1.f / 30.f);
		}
		return Component.GetComponentSpaceTransforms();
	};

	const TArray<FTransform> StandPose = PoseWith(StandSet);
	TestTrue(TEXT("the active set is the one set"), UDreamAnimLibrary::GetAnimSet(Instance) == StandSet);
	const TArray<FTransform> CrouchPose = PoseWith(CrouchSet);
	TestTrue(TEXT("the active set follows the swap"), UDreamAnimLibrary::GetAnimSet(Instance) == CrouchSet);
	const TArray<FTransform> StandAgain = PoseWith(StandSet);

	const double Swapped = LargestOffset(StandPose, CrouchPose);
	const double Restored = LargestOffset(StandPose, StandAgain);
	TestTrue(FString::Printf(TEXT("another set gives another pose (%.2f cm)"), Swapped), Swapped > 5.0);
	TestTrue(FString::Printf(TEXT("the first set gives the first pose again (%.4f cm)"), Restored), Restored < 0.01);
	return true;
}

#endif
