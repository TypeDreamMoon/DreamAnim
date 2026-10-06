// Copyright DreamMoon. All Rights Reserved.

#include "Corpus/AlsPoseRecorder.h"

#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameplayTagContainer.h"
#include "Misc/App.h"
#include "Misc/ScopeExit.h"
#include "Tests/AutomationCommon.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Tests::PoseRecorderPrivate
{
	const TCHAR* const CharacterClassPath = TEXT("/ALS/ALS/Character/B_Als_Character.B_Als_Character_C");

	/** Calls a UFunction whose only parameter is an FGameplayTag (the ALS setters). */
	void CallWithTag(UObject& Object, const TCHAR* FunctionName, const TCHAR* TagName)
	{
		UFunction* Function = Object.FindFunction(FName(FunctionName));
		if (!Function)
		{
			return;
		}
		uint8* Params = static_cast<uint8*>(FMemory_Alloca_Aligned(FMath::Max<int32>(Function->ParmsSize, 1), Function->GetMinAlignment()));
		FMemory::Memzero(Params, Function->ParmsSize);
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(Params);
			const FStructProperty* Struct = CastField<FStructProperty>(*It);
			if (Struct && Struct->Struct == FGameplayTag::StaticStruct())
			{
				*Struct->ContainerPtrToValuePtr<FGameplayTag>(Params) = FGameplayTag::RequestGameplayTag(FName(TagName), false);
			}
		}
		Object.ProcessEvent(Function, Params);
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Params);
		}
	}

	/** One step of the script: from Frame on, these inputs hold. */
	struct FStep
	{
		int32 Frame;
		FVector Input;
		const TCHAR* Gait = nullptr;
		const TCHAR* Stance = nullptr;
		const TCHAR* RotationMode = nullptr;
		const TCHAR* Overlay = nullptr;
	};

	/**
	 * Idle, walk and run in two directions, stop, crouch, aim, then every overlay in turn while moving: each
	 * of ALS's Animation Blueprints gets weight at some point.
	 */
	TArray<FStep> MakeSteps()
	{
		TArray<FStep> Steps;
		Steps.Add({ 0, FVector::ZeroVector, TEXT("Als.Gait.Walking"), TEXT("Als.Stance.Standing"), TEXT("Als.RotationMode.VelocityDirection"), TEXT("Als.OverlayMode.Default") });
		Steps.Add({ 20, FVector(1, 0, 0) });
		Steps.Add({ 60, FVector(1, 0, 0), TEXT("Als.Gait.Running") });
		Steps.Add({ 100, FVector(0, 1, 0) });
		Steps.Add({ 130, FVector::ZeroVector });
		Steps.Add({ 160, FVector(-1, 0, 0), nullptr, TEXT("Als.Stance.Crouching") });
		Steps.Add({ 190, FVector::ZeroVector, nullptr, TEXT("Als.Stance.Standing") });
		Steps.Add({ 210, FVector(0, -1, 0), nullptr, nullptr, TEXT("Als.RotationMode.Aiming") });
		Steps.Add({ 240, FVector(1, 0, 0), nullptr, nullptr, TEXT("Als.RotationMode.ViewDirection") });
		const TCHAR* const Overlays[] = {
			TEXT("Als.OverlayMode.Masculine"), TEXT("Als.OverlayMode.Feminine"), TEXT("Als.OverlayMode.Injured"),
			TEXT("Als.OverlayMode.HandsTied"), TEXT("Als.OverlayMode.Rifle"), TEXT("Als.OverlayMode.PistolOneHanded"),
			TEXT("Als.OverlayMode.PistolTwoHanded"), TEXT("Als.OverlayMode.Bow"), TEXT("Als.OverlayMode.Torch"),
			TEXT("Als.OverlayMode.Binoculars"), TEXT("Als.OverlayMode.Box"), TEXT("Als.OverlayMode.Barrel"),
		};
		int32 Frame = 260;
		for (const TCHAR* Overlay : Overlays)
		{
			Steps.Add({ Frame, (Frame / 13) % 2 ? FVector(0, 1, 0) : FVector::ZeroVector, nullptr, nullptr, nullptr, Overlay });
			Frame += 13;
		}
		return Steps;
	}

	void SpawnFloor(UWorld& World)
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AStaticMeshActor* Floor = World.SpawnActor<AStaticMeshActor>(FVector(0, 0, -50), FRotator::ZeroRotator, Params);
		// A static component refuses a new mesh once registered, and the character would fall through.
		Floor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		Floor->GetStaticMeshComponent()->SetStaticMesh(Cube);
		Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
		// The cube is 100 units wide: 200 x 200 metres, its top at z = 0.
		Floor->SetActorScale3D(FVector(200, 200, 1));
	}
}

namespace UE::DreamAnim::Tests
{
	using namespace PoseRecorderPrivate;

	bool RecordAlsPoses(const FAlsPoseScript& Script, FAlsPoseRecording& Out, FString& Error)
	{
		Out = FAlsPoseRecording();
		UClass* CharacterClass = LoadClass<ACharacter>(nullptr, CharacterClassPath);
		if (!CharacterClass)
		{
			Error = FString::Printf(TEXT("Cannot load %s"), CharacterClassPath);
			return false;
		}

		// The engine's test world: a game instance and game mode so actors begin play, and a frame counter
		// that advances with every tick (meshes and movement skip work they think they already did this frame).
		FTestWorldWrapper Wrapper;
		if (!Wrapper.CreateTestWorld(EWorldType::Game) || !Wrapper.BeginPlayInTestWorld())
		{
			TArray<FString> Messages;
			Wrapper.AppendErrorMessages(Messages);
			Error = FString::Join(Messages, TEXT("; "));
			return false;
		}
		ON_SCOPE_EXIT
		{
			Wrapper.DestroyTestWorld(false);
		};
		UWorld& World = *Wrapper.GetTestWorld();
		SpawnFloor(World);

		const FTransform SpawnTransform(FRotator::ZeroRotator, FVector(0, 0, 120));
		ACharacter* Character = World.SpawnActorDeferred<ACharacter>(CharacterClass, SpawnTransform, nullptr, nullptr,
			ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
		if (!Character || !Character->GetMesh())
		{
			Error = TEXT("Cannot spawn the ALS character");
			return false;
		}
		if (Script.BeforeSpawn)
		{
			Script.BeforeSpawn(*Character);
		}
		Character->FinishSpawning(SpawnTransform);
		USkeletalMeshComponent& Mesh = *Character->GetMesh();
		if (Script.Setup)
		{
			Script.Setup(*Character);
		}

		const int32 NumBones = Mesh.GetNumBones();
		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			Out.Bones.Add(Mesh.GetBoneName(Bone));
		}

		const TArray<FStep> Steps = MakeSteps();
		FVector Input = FVector::ZeroVector;
		int32 NextStep = 0;
		for (int32 Frame = 0; Frame < Script.Frames; ++Frame)
		{
			while (Steps.IsValidIndex(NextStep) && Steps[NextStep].Frame == Frame)
			{
				const FStep& Step = Steps[NextStep++];
				Input = Step.Input;
				if (Step.Gait)
				{
					CallWithTag(*Character, TEXT("SetDesiredGait"), Step.Gait);
				}
				if (Step.Stance)
				{
					CallWithTag(*Character, TEXT("SetDesiredStance"), Step.Stance);
				}
				if (Step.RotationMode)
				{
					CallWithTag(*Character, TEXT("SetDesiredRotationMode"), Step.RotationMode);
				}
				if (Step.Overlay)
				{
					CallWithTag(*Character, TEXT("SetOverlayMode"), Step.Overlay);
					if (Script.AfterOverlayChange)
					{
						Script.AfterOverlayChange(*Character);
					}
				}
			}

			// Nothing renders in a test process. ALS resets the tick option every frame to "only montages when
			// not rendered", so the mesh is made to look just rendered instead.
			Mesh.SetLastRenderTime(World.GetTimeSeconds());
			Mesh.VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
			if (!Input.IsZero())
			{
				Character->AddMovementInput(Input, 1.f, true);
			}
			FApp::SetDeltaTime(Script.DeltaSeconds);
			Wrapper.TickTestWorld(Script.DeltaSeconds);

			Out.Frames.Add(Mesh.GetComponentSpaceTransforms());
			Out.Locations.Add(Character->GetActorLocation());
		}
		const UAnimInstance* AnimInstance = Mesh.GetAnimInstance();
		Out.Notes = FString::Printf(TEXT("anim instance %s, world time %.2f, begun play %d/%d, actor tick %d, mesh tick %d, from %s to %s"),
			AnimInstance ? *AnimInstance->GetClass()->GetName() : TEXT("none"), World.GetTimeSeconds(),
			World.HasBegunPlay() ? 1 : 0, Character->HasActorBegunPlay() ? 1 : 0,
			Character->IsActorTickEnabled() ? 1 : 0, Mesh.IsComponentTickEnabled() ? 1 : 0,
			*Out.Locations[0].ToString(), *Out.Locations.Last().ToString());
		return true;
	}

	FString CompareAlsPoses(const FAlsPoseRecording& Expected, const FAlsPoseRecording& Actual, double LocationTolerance, double AngleTolerance)
	{
		if (Expected.Bones != Actual.Bones)
		{
			return TEXT("the skeletons differ");
		}
		if (Expected.Frames.Num() != Actual.Frames.Num())
		{
			return FString::Printf(TEXT("%d frames recorded, expected %d"), Actual.Frames.Num(), Expected.Frames.Num());
		}
		const double AngleRadians = FMath::DegreesToRadians(AngleTolerance);
		FString First;
		int32 LastFrame = INDEX_NONE;
		int32 DifferingFrames = 0;
		for (int32 Frame = 0; Frame < Expected.Frames.Num(); ++Frame)
		{
			const TArray<FTransform>& E = Expected.Frames[Frame];
			const TArray<FTransform>& A = Actual.Frames[Frame];
			if (E.Num() != A.Num())
			{
				return FString::Printf(TEXT("frame %d: %d bones, expected %d"), Frame, A.Num(), E.Num());
			}
			for (int32 Bone = 0; Bone < E.Num(); ++Bone)
			{
				const double Distance = FVector::Dist(E[Bone].GetLocation(), A[Bone].GetLocation());
				const double Angle = E[Bone].GetRotation().AngularDistance(A[Bone].GetRotation());
				if (Distance > LocationTolerance || Angle > AngleRadians)
				{
					if (First.IsEmpty())
					{
						First = FString::Printf(TEXT("frame %d, bone %s: off by %.4f cm and %.4f degrees"),
							Frame, Expected.Bones.IsValidIndex(Bone) ? *Expected.Bones[Bone].ToString() : TEXT("?"),
							Distance, FMath::RadiansToDegrees(Angle));
					}
					LastFrame = Frame;
					++DifferingFrames;
					break;
				}
			}
			if (LastFrame != Frame && FVector::Dist(Expected.Locations[Frame], Actual.Locations[Frame]) > LocationTolerance)
			{
				if (First.IsEmpty())
				{
					First = FString::Printf(TEXT("frame %d: the character is at %s, expected %s"), Frame,
						*Actual.Locations[Frame].ToString(), *Expected.Locations[Frame].ToString());
				}
				LastFrame = Frame;
				++DifferingFrames;
			}
		}
		return First.IsEmpty() ? FString()
			: FString::Printf(TEXT("%s (%d of %d frames differ, the last is frame %d)"), *First, DifferingFrames, Expected.Frames.Num(), LastFrame);
	}
}
