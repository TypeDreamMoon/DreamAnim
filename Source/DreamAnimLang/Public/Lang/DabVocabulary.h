// Copyright DreamMoon. All Rights Reserved.
//
// The words each kind of document gives a meaning to, and the grammar version. The parser reads its block
// words from here; `dab.ps1 vocab` exports the table for editors and tools; a test holds it to the names the
// language reference lists.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabAst.h"

namespace UE::DreamAnim::Lang
{
	/**
	 * One more each time the syntax or the meaning of the text changes: what a file may hold, what it builds,
	 * what write-back writes or how node GUIDs are made. Docs/Grammar.md states it and Docs/Changelog.md says
	 * what each version changed.
	 */
	inline constexpr int32 DabGrammarVersion = 5;

	enum class EDabWordRole : uint8
	{
		/** The word before a document's header arguments: `AnimBlueprint(...)`. */
		Document,
		/** A named argument of the header: `Name`, `Root`. */
		HeaderArgument,
		/** `Name = { ... }` at the document level. */
		Section,
		/** A word that starts a named block: `StateMachine Name { ... }`, `Event Forward { ... }`. */
		Block,
		/** A key with a fixed meaning inside a block: a state's `Pose`, a machine's `Entry`. */
		Key,
		/** A word that starts a statement of code: `let`, `if`. */
		Statement,
		/** A name with a fixed meaning where a value is written: `true`, `Self`, `None`. */
		Value,
		/** A function of expressions: `Clamp(x, lo, hi)`. */
		Function,
		/** A call that makes a node of its own kind: `Slot(...)`, `Input(...)`, `RunEvent(...)`. */
		Builtin,
		/** A key of the Settings section. */
		Setting,
		/** A metadata key with a fixed meaning: a pose's `Cached`, a transition's `Blend`, a function's `Public`. */
		Metadata,
		/** `#include`, `#Region`, `#EndRegion`. */
		Directive,
	};

	DREAMANIMLANG_API const TCHAR* LexToString(EDabWordRole Role);

	struct FDabWord
	{
		const TCHAR* Text;
		EDabWordRole Role;
		/** Where the word has its meaning: `document`, `StateMachine`, `State`, `code`, `expression`, ... */
		const TCHAR* Context;
		/** One line on what it is. */
		const TCHAR* Summary;
	};

	struct FDabDocumentWords
	{
		EDabDocumentKind Kind;
		/** `.dab`, ... */
		const TCHAR* Extension;
		TConstArrayView<FDabWord> Words;
	};

	/** Every kind of document with its words, in the order of EDabDocumentKind. */
	DREAMANIMLANG_API TConstArrayView<FDabDocumentWords> GetDabDocumentWords();

	/** The words of one kind of document; empty for Unknown. */
	DREAMANIMLANG_API TConstArrayView<FDabWord> GetDabWords(EDabDocumentKind Kind);

	/** Whether Text (not in backticks) starts a named block in a document of that kind. */
	DREAMANIMLANG_API bool IsDabBlockWord(EDabDocumentKind Kind, FStringView Text);
}
