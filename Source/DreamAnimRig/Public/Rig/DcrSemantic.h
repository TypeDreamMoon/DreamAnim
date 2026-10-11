// Copyright DreamMoon. All Rights Reserved.
//
// The semantic pass for ControlRig documents: a parsed .dcr (includes resolved, templates expanded) -> the
// model, checking every name against the engine's reflection on the way. It is the inverse of lowering
// (DcrLower.h): text that lowering wrote reads back to the model it was written from, up to what
// normalization removes (Sequence nodes, reroutes, values on linked pins) and up to node names.
//
// What the text means, node by node:
// - Each call is one node; a `let` names a node (its name is the let's name) so every use shares it. Reading a
//   variable is a getter node of its own at every use. A function's input read by name links from the Entry
//   node.
// - Statements are chained by execute links in the order written. A mutable call nested in a statement runs
//   before it, inner to outer, left to right. `if` is a branch node, `for` a loop node; `Detached` chains
//   its statements without linking the first to anything.
// - A literal that holds expressions (`(Translation = (Z = Offset))`) is split: the constants become the
//   pin's value, the expressions links into the sub-pins.
// - A pin keeps a value only when it differs from what a fresh node holds; the values of resolved wildcard
//   pins are always kept. A linked pin keeps none.
// - `#Region` becomes a comment node that names the nodes its statements made.
// - A function marked `Inline` becomes a collapse node at its one call.
//
// Diagnostics are DAB9001-DAB9012 (Docs/Diagnostics.md).

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabDiagnostic.h"
#include "Lang/DabTemplates.h"
#include "Model/DcrModel.h"

namespace UE::DreamAnim::Rig
{
	struct FDcrSemanticOptions
	{
		/**
		 * The asset's object path. Unset: made from the document's header (`Name`, `Root`), the way the driver
		 * names the asset a document produces.
		 */
		FString AssetPath;
	};

	/**
	 * Builds the model of Unit.Main, which must hold a ControlRig document whose #includes are resolved and
	 * templates expanded. Keeps going after an error so one run reports as much as it can. Returns false when
	 * any error was reported; OutModel then holds whatever could be built.
	 *
	 * Resolving names loads assets: the preview mesh, shape libraries, the rigs `Uses` lists. Run it where
	 * assets can load (the editor).
	 */
	DREAMANIMRIG_API bool BuildDcrModel(const Lang::FDabCompilationUnit& Unit, Model::FDcrModel& OutModel,
		Lang::FDabDiagnosticSink& Sink, const FDcrSemanticOptions& Options = FDcrSemanticOptions());
}
