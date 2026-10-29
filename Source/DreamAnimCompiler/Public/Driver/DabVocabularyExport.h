// Copyright DreamMoon. All Rights Reserved.
//
// `dab.ps1 vocab`: what the languages know, as JSON, for editors and tools that read or draw DreamAnim
// sources. The words of each kind of document come from Lang/DabVocabulary.h; the node types and their pins
// from the engine, read the way the checker reads them.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UE::DreamAnim::Compiler
{
	struct FDabVocabularyOptions
	{
		/** Add every RigVM unit and dispatch a `.dcr` can call, with its pins (a few thousand entries). */
		bool bRigUnits = false;
	};

	/**
	 * The vocabulary as a JSON object (pretty-printed):
	 *
	 *   format          1, the layout of this object
	 *   grammarVersion  Lang::DabGrammarVersion
	 *   pluginVersion   the plugin's VersionName
	 *   engineVersion   the engine's version string
	 *   documents       [ { kind, extension, words: [ { text, role, context, summary } ] } ]
	 *   animNodes       [ { name, class, struct, ... , pins: [ ... ] } ]  every anim node type, see Docs/Tools.md
	 *   rigUnits        [ ... ]  with bRigUnits; written by the Control Rig document handler
	 */
	DREAMANIMCOMPILER_API FString MakeDabVocabularyJson(const FDabVocabularyOptions& Options = FDabVocabularyOptions());

	/** The same as an object, for a document handler to add to (IDabDocumentHandler::AddVocabulary). */
	DREAMANIMCOMPILER_API TSharedRef<FJsonObject> MakeDabVocabulary(const FDabVocabularyOptions& Options = FDabVocabularyOptions());
}
