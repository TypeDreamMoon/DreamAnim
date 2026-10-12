// Copyright DreamMoon. All Rights Reserved.
//
// Normalize: rewrite an extracted model into the form the text keeps, without changing what the rig does.
//
// - Sequence nodes (RigVMFunction_Sequence, the older RigUnit_SequenceExecution, and their aggregate forms)
//   become one chain: the end of each output's chain runs into the next output's first node. Done to 55
//   sequences of seven engine rigs, it left every bone, curve and variable bit for bit where it was.
//   "The end of a chain" is found by following each node's continuation: its `Completed` output when it has
//   one (branches, loops), else its IO execute pin, else an event's only output; a Sequence continues from
//   its last output. A Sequence whose earlier output's chain ends in a Return cannot be flattened; it is left
//   as it is and reported.
// - Reroutes become direct links.
// - Values left on pins that are linked (the engine keeps them; they do nothing) are dropped.
// - Nodes nothing executes and nothing reads are kept: the text writes them under `Detached`.

#pragma once

#include "CoreMinimal.h"
#include "Model/DcrModel.h"

namespace UE::DreamAnim::Rig
{
	struct FDcrNormalizeReport
	{
		int32 SequencesFlattened = 0;
		int32 SequencesKept = 0;
		int32 ReroutesFolded = 0;
		int32 LinkedValuesDropped = 0;
		int32 UnreadGettersDropped = 0;
		/** One line per sequence kept, and anything else worth reading. */
		TArray<FString> Notes;
	};

	DREAMANIMRIG_API FDcrNormalizeReport NormalizeDcrModel(Model::FDcrModel& Model);

	/**
	 * A normalized rig's model as its text's model holds the same rig: a getter that feeds several pins becomes
	 * one getter per pin (each use of a variable in the text is one), a function's input read through a getter
	 * becomes a link from the entry, and empty graphs go. For comparing a rig with the model of its text.
	 */
	DREAMANIMRIG_API void MakeDcrModelAsTextHoldsIt(Model::FDcrModel& Model);
}
