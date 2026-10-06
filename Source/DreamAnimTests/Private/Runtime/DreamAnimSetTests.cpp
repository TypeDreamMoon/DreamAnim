// Copyright DreamMoon. All Rights Reserved.
//
// Animation sets without a Blueprint: what the generator relies on when a Blueprint's declared slots
// change (SyncSlots never loses a chosen animation) and what generated code reads (Find, the library's
// fallback).

#include "Animation/AnimSequence.h"
#include "DreamAnimLibrary.h"
#include "DreamAnimSet.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Tests::AnimSetPrivate
{
	TArray<TPair<FName, FName>> Slots(std::initializer_list<TPair<FName, FName>> Pairs)
	{
		return TArray<TPair<FName, FName>>(Pairs);
	}

	const FDreamAnimSetSlot* FindSlot(const UDreamAnimSet& Set, FName Blueprint, FName Slot)
	{
		for (const FDreamAnimSetGroup& Group : Set.Groups)
		{
			if (Group.Blueprint == Blueprint)
			{
				return Group.Slots.FindByPredicate([Slot](const FDreamAnimSetSlot& Candidate) { return Candidate.Name == Slot; });
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamAnimSetSyncTest, "DreamAnim.Runtime.AnimSet.SyncSlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamAnimSetSyncTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::AnimSetPrivate;
	const FName Standing(TEXT("AB_Standing"));
	const FName Idle(TEXT("Idle"));
	const FName Walk(TEXT("Walk"));

	UDreamAnimSet* Set = NewObject<UDreamAnimSet>(GetTransientPackage());
	UAnimSequence* IdleAnim = NewObject<UAnimSequence>(GetTransientPackage());

	TestTrue(TEXT("first sync adds the group"), Set->SyncSlots(Standing, Slots({ { Idle, TEXT("AnimSequence") }, { Walk, TEXT("BlendSpace") } })));
	TestEqual(TEXT("one group"), Set->Groups.Num(), 1);
	TestEqual(TEXT("two slots"), Set->Groups[0].Slots.Num(), 2);
	TestFalse(TEXT("same slots again change nothing"), Set->SyncSlots(Standing, Slots({ { Idle, TEXT("AnimSequence") }, { Walk, TEXT("BlendSpace") } })));

	TestTrue(TEXT("SetAsset on a declared slot"), Set->SetAsset(Standing, Idle, IdleAnim));
	TestFalse(TEXT("SetAsset on an unknown slot"), Set->SetAsset(Standing, TEXT("Missing"), IdleAnim));
	TestTrue(TEXT("Find reads the asset"), Set->Find(Standing, Idle) == IdleAnim);
	TestNull(TEXT("an empty slot finds nothing"), Set->Find(Standing, Walk));
	TestNull(TEXT("another Blueprint finds nothing"), Set->Find(TEXT("AB_Other"), Idle));

	// The Blueprint stops declaring Idle: the slot stays, with its animation, marked orphan.
	TestTrue(TEXT("dropping a slot changes the set"), Set->SyncSlots(Standing, Slots({ { Walk, TEXT("BlendSpace") } })));
	const FDreamAnimSetSlot* IdleSlot = FindSlot(*Set, Standing, Idle);
	TestTrue(TEXT("the dropped slot is kept"), IdleSlot != nullptr);
	TestTrue(TEXT("the dropped slot is an orphan"), IdleSlot && IdleSlot->bOrphan);
	TestTrue(TEXT("the dropped slot keeps its animation"), IdleSlot && IdleSlot->Asset == IdleAnim);

	// Declared again, with another type: no longer an orphan, type updated, animation still there.
	TestTrue(TEXT("re-declaring changes the set"), Set->SyncSlots(Standing, Slots({ { Idle, TEXT("AnimationAsset") }, { Walk, TEXT("BlendSpace") } })));
	IdleSlot = FindSlot(*Set, Standing, Idle);
	TestTrue(TEXT("orphan mark cleared"), IdleSlot && !IdleSlot->bOrphan);
	TestTrue(TEXT("type follows the declaration"), IdleSlot && IdleSlot->Type == FName(TEXT("AnimationAsset")));
	TestTrue(TEXT("Find still reads the asset"), Set->Find(Standing, Idle) == IdleAnim);

	// A second Blueprint gets its own group; the first is untouched.
	TestTrue(TEXT("second Blueprint"), Set->SyncSlots(TEXT("AB_Crouching"), Slots({ { Idle, TEXT("AnimSequence") } })));
	TestEqual(TEXT("two groups"), Set->Groups.Num(), 2);
	TestNull(TEXT("same slot name, other Blueprint, empty"), Set->Find(TEXT("AB_Crouching"), Idle));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamAnimSetLibraryTest, "DreamAnim.Runtime.AnimSet.Library",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamAnimSetLibraryTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::AnimSetPrivate;
	UDreamAnimSet* Set = NewObject<UDreamAnimSet>(GetTransientPackage());
	UAnimSequence* Idle = NewObject<UAnimSequence>(GetTransientPackage());
	UAnimSequence* Fallback = NewObject<UAnimSequence>(GetTransientPackage());
	Set->SyncSlots(TEXT("AB"), Slots({ { TEXT("Idle"), TEXT("AnimSequence") }, { TEXT("Empty"), TEXT("AnimSequence") } }));
	Set->SetAsset(TEXT("AB"), TEXT("Idle"), Idle);

	TestTrue(TEXT("asset from the set"), UDreamAnimLibrary::GetAnimSetAsset(Set, TEXT("AB"), TEXT("Idle"), Fallback) == Idle);
	TestTrue(TEXT("empty slot falls back"), UDreamAnimLibrary::GetAnimSetAsset(Set, TEXT("AB"), TEXT("Empty"), Fallback) == Fallback);
	TestTrue(TEXT("no set falls back"), UDreamAnimLibrary::GetAnimSetAsset(nullptr, TEXT("AB"), TEXT("Idle"), Fallback) == Fallback);
	TestTrue(TEXT("typed read"), UDreamAnimLibrary::GetAnimSetSequence(Set, TEXT("AB"), TEXT("Idle")) == Idle);
	TestNull(TEXT("typed read of the wrong type"), UDreamAnimLibrary::GetAnimSetBlendSpace(Set, TEXT("AB"), TEXT("Idle")));
	TestNull(TEXT("no instance, no set"), UDreamAnimLibrary::GetAnimSet(nullptr));
	return true;
}

#endif
