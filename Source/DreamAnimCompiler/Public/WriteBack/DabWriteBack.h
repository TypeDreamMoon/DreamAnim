// Copyright DreamMoon. All Rights Reserved.
//
// Write-back: what someone changed in the editor on a Blueprint generated from text, written into that
// text. The Blueprint is extracted again and compared value by value with the model the text checks to;
// nodes are matched by the NodeGuid the generator derived from their place in the text. A changed value
// replaces just that value's text; a node whose arguments come or go is reprinted; a structural change
// (nodes, links, states, transitions added or removed) rewrites the block it is in from the Blueprint,
// keeping the comments of statements that survive. Docs/WriteBack.md lists what is written and what is
// refused; DAB8001-DAB8009 are its diagnostics.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabDiagnostic.h"

class UAnimBlueprint;

namespace UE::DreamAnim::Compiler
{
	/** One difference between a source and the Blueprint generated from it. */
	struct FDabWriteBackChange
	{
		/** Where, in the text's terms: `Graph: Movement (ApplyAdditive)`, `StateMachine StandingStates: Idle -> Move`. */
		FString Location;
		/** What, briefly: `Alpha`, `the transition's options`, `the graph's nodes`. */
		FString What;
		/** Written into the text; false when it was refused (the diagnostics say why). */
		bool bWritten = false;
		/** Written by rewriting a whole block from the Blueprint rather than editing values in place. */
		bool bRewroteBlock = false;
	};

	struct FDabWriteBackOptions
	{
		/** Write structural changes by rewriting the block they are in; false refuses them (DAB8003). */
		bool bStructure = true;
		/**
		 * A value changed inside a template's body: rewrite the graph block the node is in from the Blueprint,
		 * which writes that block's template calls out (DAB8004), rather than refusing the change (DAB8001).
		 */
		bool bExpandTemplates = false;
	};

	struct FDabWriteBackResult
	{
		/** The source with the Blueprint's changes written in; the input text when nothing was written. */
		FString Text;
		TArray<FDabWriteBackChange> Changes;
		/** Refusals and problems (DAB8xxx), and the text's own errors when it does not check. */
		TArray<Lang::FDabDiagnostic> Diagnostics;
		/** The text could be read, checked and compared with the Blueprint. */
		bool bCompared = false;
		/** The new text checks to the Blueprint's model: text and asset agree again. */
		bool bFixedPoint = false;

		bool HasEdits() const
		{
			return Changes.ContainsByPredicate([](const FDabWriteBackChange& Change) { return Change.bWritten; });
		}
	};

	/**
	 * What Blueprint holds that Text (the source at FilePath it was generated from) does not, written into
	 * the text. FilePath resolves #include and names the file in diagnostics; nothing is read from or
	 * written to disk here.
	 */
	DREAMANIMCOMPILER_API FDabWriteBackResult WriteBackDabBlueprint(const UAnimBlueprint& Blueprint, const FString& FilePath, const FString& Text,
		const FDabWriteBackOptions& Options = FDabWriteBackOptions());
}
