// Copyright DreamMoon. All Rights Reserved.

#include "DreamAnimSet.h"

#include "Animation/AnimationAsset.h"

UAnimationAsset* UDreamAnimSet::Find(FName Blueprint, FName Slot) const
{
	const TObjectPtr<UAnimationAsset>* Found = Lookup.Find(TPair<FName, FName>(Blueprint, Slot));
	return Found ? Found->Get() : nullptr;
}

bool UDreamAnimSet::SyncSlots(FName Blueprint, const TArray<TPair<FName, FName>>& DeclaredSlots)
{
	FDreamAnimSetGroup* Group = Groups.FindByPredicate([Blueprint](const FDreamAnimSetGroup& Candidate) { return Candidate.Blueprint == Blueprint; });
	bool bChanged = false;
	if (!Group && DeclaredSlots.IsEmpty())
	{
		// A Blueprint that never declared a slot has nothing to keep here.
		return false;
	}
	if (!Group)
	{
		Group = &Groups.AddDefaulted_GetRef();
		Group->Blueprint = Blueprint;
		bChanged = true;
	}

	for (FDreamAnimSetSlot& Slot : Group->Slots)
	{
		const bool bDeclared = DeclaredSlots.ContainsByPredicate([&Slot](const TPair<FName, FName>& Declared) { return Declared.Key == Slot.Name; });
		if (Slot.bOrphan == bDeclared)
		{
			Slot.bOrphan = !bDeclared;
			bChanged = true;
		}
	}

	for (const TPair<FName, FName>& Declared : DeclaredSlots)
	{
		FDreamAnimSetSlot* Slot = Group->Slots.FindByPredicate([&Declared](const FDreamAnimSetSlot& Candidate) { return Candidate.Name == Declared.Key; });
		if (!Slot)
		{
			Slot = &Group->Slots.AddDefaulted_GetRef();
			Slot->Name = Declared.Key;
			bChanged = true;
		}
		if (Slot->Type != Declared.Value)
		{
			Slot->Type = Declared.Value;
			bChanged = true;
		}
	}

	if (bChanged)
	{
		RebuildLookup();
	}
	return bChanged;
}

bool UDreamAnimSet::SetAsset(FName Blueprint, FName Slot, UAnimationAsset* Asset)
{
	check(IsInGameThread());
	FDreamAnimSetGroup* Group = Groups.FindByPredicate([Blueprint](const FDreamAnimSetGroup& Candidate) { return Candidate.Blueprint == Blueprint; });
	FDreamAnimSetSlot* Found = Group ? Group->Slots.FindByPredicate([Slot](const FDreamAnimSetSlot& Candidate) { return Candidate.Name == Slot; }) : nullptr;
	if (!Found)
	{
		return false;
	}
	Found->Asset = Asset;
	RebuildLookup();
	return true;
}

void UDreamAnimSet::PostLoad()
{
	Super::PostLoad();
	RebuildLookup();
}

#if WITH_EDITOR
void UDreamAnimSet::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	RebuildLookup();
}
#endif

void UDreamAnimSet::RebuildLookup()
{
	Lookup.Reset();
	for (const FDreamAnimSetGroup& Group : Groups)
	{
		for (const FDreamAnimSetSlot& Slot : Group.Slots)
		{
			if (Slot.Asset)
			{
				Lookup.Add(TPair<FName, FName>(Group.Blueprint, Slot.Name), Slot.Asset);
			}
		}
	}
}
