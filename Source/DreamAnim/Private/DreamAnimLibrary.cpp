// Copyright DreamMoon. All Rights Reserved.

#include "DreamAnimLibrary.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"
#include "Components/SkeletalMeshComponent.h"
#include "DreamAnimSet.h"

const FName UDreamAnimLibrary::ActiveSetVariable(TEXT("DreamAnimSetActive"));
const FName UDreamAnimLibrary::PendingSetVariable(TEXT("DreamAnimSetPending"));

int64 UDreamAnimLibrary::SelectInt64(int64 A, int64 B, bool bPickA)
{
	return bPickA ? A : B;
}

uint8 UDreamAnimLibrary::SelectByte(uint8 A, uint8 B, bool bPickA)
{
	return bPickA ? A : B;
}

namespace
{
	FObjectPropertyBase* FindSetProperty(const UAnimInstance& Instance, FName Name)
	{
		FObjectPropertyBase* Property = CastField<FObjectPropertyBase>(Instance.GetClass()->FindPropertyByName(Name));
		return Property && Property->PropertyClass && Property->PropertyClass->IsChildOf(UDreamAnimSet::StaticClass()) ? Property : nullptr;
	}

	UDreamAnimSet* ReadSet(const UAnimInstance& Instance, FName Name)
	{
		const FObjectPropertyBase* Property = FindSetProperty(Instance, Name);
		return Property ? Cast<UDreamAnimSet>(Property->GetObjectPropertyValue_InContainer(&Instance)) : nullptr;
	}

	void WriteSet(UAnimInstance& Instance, FName Name, UDreamAnimSet* Set)
	{
		if (FObjectPropertyBase* Property = FindSetProperty(Instance, Name))
		{
			Property->SetObjectPropertyValue_InContainer(&Instance, Set);
		}
	}
}

void UDreamAnimLibrary::SetAnimSet(UAnimInstance* AnimInstance, UDreamAnimSet* Set)
{
	if (!AnimInstance)
	{
		return;
	}
	check(IsInGameThread());

	WriteSet(*AnimInstance, PendingSetVariable, Set);
	if (const USkeletalMeshComponent* Component = AnimInstance->GetSkelMeshComponent())
	{
		if (Component->GetAnimInstance() == AnimInstance)
		{
			// The const overload: the mutable one is private.
			for (UAnimInstance* Linked : Component->GetLinkedAnimInstances())
			{
				if (Linked)
				{
					WriteSet(*Linked, PendingSetVariable, Set);
				}
			}
		}
	}
}

UDreamAnimSet* UDreamAnimLibrary::GetAnimSet(const UAnimInstance* AnimInstance)
{
	return AnimInstance ? ReadSet(*AnimInstance, ActiveSetVariable) : nullptr;
}

void UDreamAnimLibrary::ApplyPendingAnimSet(UAnimInstance* AnimInstance)
{
	if (!AnimInstance)
	{
		return;
	}

	if (UDreamAnimSet* Pending = ReadSet(*AnimInstance, PendingSetVariable))
	{
		WriteSet(*AnimInstance, ActiveSetVariable, Pending);
		WriteSet(*AnimInstance, PendingSetVariable, nullptr);
		return;
	}

	// A linked instance follows its main instance; this also covers one created after a swap.
	const USkeletalMeshComponent* Component = AnimInstance->GetSkelMeshComponent();
	const UAnimInstance* Main = Component ? Component->GetAnimInstance() : nullptr;
	if (Main && Main != AnimInstance && FindSetProperty(*Main, ActiveSetVariable))
	{
		UDreamAnimSet* MainSet = ReadSet(*Main, ActiveSetVariable);
		if (MainSet && MainSet != ReadSet(*AnimInstance, ActiveSetVariable))
		{
			WriteSet(*AnimInstance, ActiveSetVariable, MainSet);
		}
	}
}

UAnimationAsset* UDreamAnimLibrary::GetAnimSetAsset(const UDreamAnimSet* Set, FName Blueprint, FName Slot, UAnimationAsset* Fallback)
{
	UAnimationAsset* Found = Set ? Set->Find(Blueprint, Slot) : nullptr;
	return Found ? Found : Fallback;
}

UAnimSequenceBase* UDreamAnimLibrary::GetAnimSetSequence(const UDreamAnimSet* Set, FName Blueprint, FName Slot)
{
	return Cast<UAnimSequenceBase>(GetAnimSetAsset(Set, Blueprint, Slot));
}

UBlendSpace* UDreamAnimLibrary::GetAnimSetBlendSpace(const UDreamAnimSet* Set, FName Blueprint, FName Slot)
{
	return Cast<UBlendSpace>(GetAnimSetAsset(Set, Blueprint, Slot));
}

UAnimMontage* UDreamAnimLibrary::GetAnimSetMontage(const UDreamAnimSet* Set, FName Blueprint, FName Slot)
{
	return Cast<UAnimMontage>(GetAnimSetAsset(Set, Blueprint, Slot));
}
