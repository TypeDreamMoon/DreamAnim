// Copyright DreamMoon. All Rights Reserved.
//
// The parser. Recursive descent over the tokens of LexDab, following Docs/Grammar.md. It keeps going
// after an error: a statement that fails to parse is skipped up to the next `;` or the `}` closing its
// block, and the rest of the file is still parsed, so one run reports as many problems as it can.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabAst.h"
#include "Lang/DabDiagnostic.h"

namespace UE::DreamAnim::Lang
{
	struct FDabParseOptions
	{
		/** Record comments and blank lines in FDabFile::Trivia (the printer and the write-back need them). */
		bool bKeepTrivia = true;
	};

	/**
	 * Parses one file. Never returns null: on errors the file holds whatever parsed. The file's Path is
	 * the source's path, and the sink should be constructed with the same path.
	 */
	DREAMANIMLANG_API TUniquePtr<FDabFile> ParseDab(const FDabSourceText& Source, FDabDiagnosticSink& Sink, const FDabParseOptions& Options = FDabParseOptions());

	/**
	 * Parses a single expression (the whole text must be one expression, surrounding whitespace and
	 * comments aside). Null with an error reported when it is not. Used to check a value the editor is
	 * about to write back before it goes into the file.
	 */
	DREAMANIMLANG_API FDabExprPtr ParseDabExpression(const FDabSourceText& Source, FDabDiagnosticSink& Sink);
}
