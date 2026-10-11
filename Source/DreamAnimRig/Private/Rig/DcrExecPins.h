// Copyright DreamMoon. All Rights Reserved.
//
// Execute pins, on the engine's nodes (to fill a model node's ExecuteIn / ExecuteOut) and on the model's
// (to follow execution through a graph without the engine).

#pragma once

#include "CoreMinimal.h"
#include "Model/DcrModel.h"

class URigVMNode;

namespace UE::DreamAnim::Rig
{
	/** Fills ExecuteIn / ExecuteOut from the node's root execute pins by direction. An IO pin goes to both. */
	void FillDcrExecutePins(URigVMNode* Node, Model::FDcrModelNode& Out);

	/** Splits `Node.Pin.Sub` at the first dot: the node's name and the rest (`Pin.Sub`). Without a dot the rest is empty. */
	void SplitDcrPinPath(const FString& Path, FString& OutNode, FString& OutRest);

	/** A Sequence unit, RigVMFunction_Sequence or the older RigUnit_SequenceExecution, aggregated or not. */
	bool IsDcrSequence(const Model::FDcrModelNode& Node);

	/** A sequence's outputs in order: its execute outputs without the pins it also takes execution in by. */
	TArray<FString> GetDcrSequenceOutputs(const Model::FDcrModelNode& Node);

	/**
	 * The pin execution goes on from once the node and everything it starts are done: a sequence's last
	 * output; else `Completed` (branches, loops); else the IO execute pin; else the only output (events).
	 * Empty when the node has none (a Return).
	 */
	FString GetDcrContinuation(const Model::FDcrModelNode& Node);
}
