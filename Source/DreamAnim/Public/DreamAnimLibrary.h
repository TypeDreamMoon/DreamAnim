// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DreamAnimLibrary.generated.h"

class UAnimationAsset;
class UAnimInstance;
class UAnimMontage;
class UAnimSequenceBase;
class UBlendSpace;
class UDreamAnimSet;

/**
 * Run-time support for DreamAnim Blueprints with animation slots.
 *
 * Every generated Blueprint that declares slots gets two variables, DreamAnimSetActive and
 * DreamAnimSetPending, and a line in its Update Animation event calling ApplyPendingAnimSet. Swapping a
 * set only writes Pending; Apply moves it to Active on the game thread before the worker threads run the
 * graph, and the graph only ever reads Active -- so one update never mixes two sets.
 */
UCLASS()
class DREAMANIM_API UDreamAnimLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** The Blueprint variables the generator adds. */
	static const FName ActiveSetVariable;
	static const FName PendingSetVariable;

	/**
	 * Gives a character another animation set: AnimInstance and every linked instance on its mesh take it
	 * at their next update. Game thread only.
	 */
	UFUNCTION(BlueprintCallable, Category = "DreamAnim")
	static void SetAnimSet(UAnimInstance* AnimInstance, UDreamAnimSet* Set);

	/** The set an instance currently plays from. */
	UFUNCTION(BlueprintPure, Category = "DreamAnim")
	static UDreamAnimSet* GetAnimSet(const UAnimInstance* AnimInstance);

	/**
	 * Called by generated code at the start of each update, on the game thread: Pending becomes Active.
	 * A linked instance with nothing pending follows its main instance's Active set, so linked instances
	 * created after a swap pick it up too.
	 */
	UFUNCTION(BlueprintCallable, Category = "DreamAnim", meta = (BlueprintInternalUseOnly = "true"))
	static void ApplyPendingAnimSet(UAnimInstance* AnimInstance);

	/** The asset in a slot of Set, or Fallback when Set is null or the slot is empty. Thread-safe. */
	UFUNCTION(BlueprintPure, Category = "DreamAnim", meta = (BlueprintThreadSafe))
	static UAnimationAsset* GetAnimSetAsset(const UDreamAnimSet* Set, FName Blueprint, FName Slot, UAnimationAsset* Fallback = nullptr);

	UFUNCTION(BlueprintPure, Category = "DreamAnim", meta = (BlueprintThreadSafe))
	static UAnimSequenceBase* GetAnimSetSequence(const UDreamAnimSet* Set, FName Blueprint, FName Slot);

	UFUNCTION(BlueprintPure, Category = "DreamAnim", meta = (BlueprintThreadSafe))
	static UBlendSpace* GetAnimSetBlendSpace(const UDreamAnimSet* Set, FName Blueprint, FName Slot);

	UFUNCTION(BlueprintPure, Category = "DreamAnim", meta = (BlueprintThreadSafe))
	static UAnimMontage* GetAnimSetMontage(const UDreamAnimSet* Set, FName Blueprint, FName Slot);
};
