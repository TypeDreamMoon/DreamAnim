// Copyright DreamMoon. All Rights Reserved.
//
// Moving a Blueprint's animations into an animation set: the animation assets a model reads at run time
// become asset slots (`Assets.A_Als_Idle`), and a set is filled with what they held. What decompiling
// with an animation set does (Docs/Decompiling.md, "Moving the animations into a set").

#pragma once

#include "CoreMinimal.h"
#include "Model/DabModel.h"

class UDreamAnimSet;

namespace UE::DreamAnim::Compiler
{
	/** A slot made from an animation asset. */
	struct FDabLiftedAsset
	{
		FString Slot;
		/** The asset's class: `AnimSequence`, `BlendSpace`, `AnimMontage`. */
		FString Type;
		/** The asset's object path. */
		FString AssetPath;
	};

	/**
	 * Turns every animation asset the model reads at run time into a slot read: a node argument that can be
	 * bound (`Sequence`, `BlendSpace`) becomes a binding to `Assets.<Slot>`, and so does an argument of a call
	 * (state functions, notifies, node functions, Call nodes). One asset is one slot, named after the asset; a
	 * name another asset or an existing slot holds gets `_2`, `_3`. Values only a constant fits (settings of
	 * the editor node, transition options, class settings and defaults, tuning value defaults) keep their
	 * asset, and so does a pin or parameter that does not take what the slot's getter returns. The slots are
	 * added to Model.AssetSlots and, when new slots were lifted and AnimSetPath is given, Model.DefaultAnimSet becomes
	 * AnimSetPath. Returns the new slots, sorted by name.
	 */
	DREAMANIMCOMPILER_API TArray<FDabLiftedAsset> LiftDabAnimationAssets(Model::FDabModel& Model, const FString& AnimSetPath);

	/**
	 * Gives Set the group of the Blueprint named Blueprint (every slot of Model) and fills each lifted slot
	 * with its asset. A slot that already holds another asset keeps it; its name goes into OutKept. The set's
	 * skeleton is the model's when it has none. Returns true when the set changed.
	 */
	DREAMANIMCOMPILER_API bool FillDabAnimSet(UDreamAnimSet& Set, FName Blueprint, const Model::FDabModel& Model, const TArray<FDabLiftedAsset>& Lifted,
		TArray<FString>* OutKept = nullptr);
}
