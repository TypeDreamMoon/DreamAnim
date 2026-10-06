// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "DreamAnimSet.generated.h"

class UAnimationAsset;
class USkeleton;

/** One animation slot an Animation Blueprint declares (`Assets = { AnimSequence Idle; }`). */
USTRUCT(BlueprintType)
struct DREAMANIM_API FDreamAnimSetSlot
{
	GENERATED_BODY()

	/** The slot's name in the text. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Slot")
	FName Name;

	/** The asset type the slot takes: `AnimSequence`, `BlendSpace`, `AnimMontage`... The editor filters the picker by it. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Slot")
	FName Type;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Slot")
	TObjectPtr<UAnimationAsset> Asset;

	/** The Blueprint no longer declares this slot. Kept so a choice is never lost; remove it by hand. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Slot")
	bool bOrphan = false;
};

/** The slots one Animation Blueprint declares. */
USTRUCT(BlueprintType)
struct DREAMANIM_API FDreamAnimSetGroup
{
	GENERATED_BODY()

	/** The Animation Blueprint's asset name (`AB_Als_Standing`); the key its generated code reads with. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Group")
	FName Blueprint;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Group", meta = (TitleProperty = "Name"))
	TArray<FDreamAnimSetSlot> Slots;
};

/**
 * One character's animations for a family of DreamAnim Blueprints. The Blueprints declare slots; a set
 * fills them; swapping the set at run time (UDreamAnimLibrary::SetAnimSet) gives the same Blueprints
 * another character's animations.
 *
 * The slot list is maintained by the DreamAnim generator: a slot a Blueprint starts declaring is added,
 * one it stops declaring is marked orphan, and an animation already chosen is never removed.
 *
 * Read from the animation worker threads: Find is const and touches only the lookup built on the game
 * thread (PostLoad / edits), so the asset must not be changed while animation runs.
 */
UCLASS(BlueprintType)
class DREAMANIM_API UDreamAnimSet : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** The skeleton every animation in the set must use. The editor filters pickers by it and warns on a mismatch. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Anim Set")
	TObjectPtr<USkeleton> Skeleton;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Anim Set", meta = (TitleProperty = "Blueprint"))
	TArray<FDreamAnimSetGroup> Groups;

	/** The asset in a slot, or null when the group or slot does not exist or is empty. Thread-safe. */
	UAnimationAsset* Find(FName Blueprint, FName Slot) const;

	/**
	 * Adds the group and slots a Blueprint declares, marks the group's slots it no longer declares as
	 * orphans, and clears the orphan mark of slots declared again. Returns true when anything changed.
	 */
	bool SyncSlots(FName Blueprint, const TArray<TPair<FName, FName>>& DeclaredSlots);

	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	void RebuildLookup();

	/** (Blueprint, Slot) -> asset. Rebuilt on the game thread; only read elsewhere. */
	TMap<TPair<FName, FName>, TObjectPtr<UAnimationAsset>> Lookup;
};
