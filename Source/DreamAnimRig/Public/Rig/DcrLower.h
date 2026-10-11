// Copyright DreamMoon. All Rights Reserved.
//
// Lowering: a normalized Control Rig model -> the syntax tree of its .dcr text (Docs/ControlRig.md).
//
// Each event, function and detached chain becomes a code block: the execute chain gives the statements in
// order, data nodes become expressions. A data node used once is written where it is used; one used more
// than once becomes a `let`; a statement node whose outputs are used later becomes a `let` too, unless it
// can be written inside the one statement that uses it right after it runs. Branches become `if`, loops
// `for`, setters assignments, the common math nodes operators. Nothing here changes what the rig does:
// generating the text again gives the same graph, up to node names, Sequence nodes, reroutes and the
// variable getters a shared getter is split into.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabAst.h"
#include "Model/DcrModel.h"

namespace UE::DreamAnim::Rig
{
	struct FDcrLowerOptions
	{
		/**
		 * Write `Import = Mesh;` for the bones and curves importing the preview mesh gives, when the rig's
		 * hierarchy starts with exactly those; every element is written out otherwise.
		 */
		bool bDetectImport = true;
	};

	struct FDcrLowerResult
	{
		TUniquePtr<Lang::FDabFile> File;
		/** What the text cannot say, by place; also listed in a comment at the top of the file. */
		TArray<Model::FDcrModelUnsupported> Unsupported;
	};

	/** The syntax tree of a model; print it with PrintDab. The model should be normalized first (NormalizeDcrModel). */
	DREAMANIMRIG_API FDcrLowerResult LowerDcrModel(const Model::FDcrModel& Model, const FDcrLowerOptions& Options = FDcrLowerOptions());
}
