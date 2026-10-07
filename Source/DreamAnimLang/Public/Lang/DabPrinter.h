// Copyright DreamMoon. All Rights Reserved.
//
// The printer: syntax tree -> canonical text (Docs/Grammar.md, "Canonical format"), and a tree dump
// for the corpus goldens.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabAst.h"

namespace UE::DreamAnim::Lang
{
	struct FDabPrintOptions
	{
		int32 IndentWidth = 4;
		/** A call, list, tuple, inline block or inline stack that would end past this column is broken over lines. */
		int32 MaxWidth = 110;
		/** Written between lines. The file ends with exactly one. */
		FString LineEnd = TEXT("\n");
		/**
		 * The text the printed nodes were parsed from, when a reprint should keep its author's layout: a
		 * block, stack, call, list or tuple that spans lines there stays broken even where it would fit on
		 * one. Nodes made afresh have no span in it and print canonically. Canonical printing leaves it null.
		 */
		const FString* KeepBrokenIn = nullptr;
	};

	/**
	 * The canonical text of a file: includes, then the document, with the file's trivia (comments,
	 * blank lines) where it was. Printing the parse of the result gives the same text again.
	 */
	DREAMANIMLANG_API FString PrintDab(const FDabFile& File, const FDabPrintOptions& Options = FDabPrintOptions());

	/**
	 * One expression in canonical form, as it would print starting at column StartColumn with the
	 * given indent level for any lines it breaks onto. Trivia, when File is given, is taken from it.
	 */
	DREAMANIMLANG_API FString PrintDabExpr(const FDabExpr& Expr, int32 IndentLevel = 0, int32 StartColumn = 0,
		const FDabFile* File = nullptr, const FDabPrintOptions& Options = FDabPrintOptions());

	/**
	 * One statement in canonical form at an indent level, without its own leading and trailing comments
	 * (they stay where they are) and without the alignment it would share with its neighbours: exact for
	 * named blocks, which never align; an assignment, declaration or transition in a run prints unpadded.
	 */
	DREAMANIMLANG_API FString PrintDabStmt(const FDabStmt& Stmt, int32 IndentLevel = 0,
		const FDabFile* File = nullptr, const FDabPrintOptions& Options = FDabPrintOptions());

	/**
	 * A one-node-per-line dump for corpus goldens: two spaces of indent per depth, the node kind, then
	 * what it holds. No spans and no trivia, so a golden survives reformatting. Examples:
	 *
	 *   Include "AlsCommon.dat"
	 *   Document AnimBlueprint
	 *     Arg Name
	 *       String "ALS/Character/AB_Als"
	 *     Block
	 *       Assign Settings
	 *         Block
	 *           Assign Parent
	 *             Name AlsAnimationInstance
	 *       NamedBlock Template Frame (Seq, N)
	 *         Param Seq
	 *         Param N
	 *         Call
	 *           Name SequenceEvaluator
	 *           Arg Sequence
	 *             Name Seq
	 *       Transition Idle -> Move
	 *         Name Moving
	 *         Meta
	 *           MetaEntry Blend
	 *             Number 0.3
	 *
	 * Leaf spellings: `Number <lexeme>`, `String "<escaped>"`, `Bool true`, `Name <id>`, `Member <id>`
	 * (child: the object), `Arg` / `Arg <name>` / `Arg "<string name>"`, `Unary !`, `Binary &&`,
	 * `Assign <name>` (`Assign "<name>"` for a string key), `Decl <type> <name>`, `Region "<label>"`,
	 * `EndRegion`, `MetaEntry <key>`, `Param <name>`. Containers: `List`, `Tuple`, `Block`, `Stack`,
	 * `Call`, `Conditional`, `Paren`, `Meta`. A NamedBlock line is `NamedBlock <Word> <Name>`, followed
	 * by ` (<p1>, <p2>)` when it has parameters.
	 */
	DREAMANIMLANG_API FString DumpDabTree(const FDabFile& File);
}
