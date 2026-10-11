// Copyright DreamMoon. All Rights Reserved.
//
// The syntax tree. It records what was written and where, nothing more: whether `Frame(...)` is a
// template, an anim node type or a mistake is decided by later stages. Docs/Grammar.md is the grammar.
//
// Ownership: every child is a TUniquePtr held by its parent; nodes are not copyable (CloneExpr and
// CloneStmt make deep copies). Node addresses are stable for the life of the FDabFile, which is what
// the trivia and origin side tables key on.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabSource.h"

namespace UE::DreamAnim::Lang
{
	enum class EDabNodeKind : uint8
	{
		// expressions
		LiteralExpr,
		NameExpr,
		MemberExpr,
		CallExpr,
		ListExpr,
		TupleExpr,
		BlockExpr,
		StackExpr,
		UnaryExpr,
		BinaryExpr,
		ConditionalExpr,
		ParenExpr,

		// statements
		AssignStmt,
		DeclStmt,
		TransitionStmt,
		NamedBlockStmt,
		RegionStmt,

		// pieces
		Argument,
		MetaEntry,
		Meta,
		Param,

		// file level
		Include,
		Document,
	};

	DREAMANIMLANG_API const TCHAR* LexToString(EDabNodeKind Kind);

	struct FDabNode
	{
		EDabNodeKind Kind;
		/** The whole construct as written. */
		FDabSpan Span;

		explicit FDabNode(EDabNodeKind InKind) : Kind(InKind) {}
		virtual ~FDabNode() = default;

		FDabNode(const FDabNode&) = delete;
		FDabNode& operator=(const FDabNode&) = delete;

		template <typename T> T* As() { return Kind == T::StaticKind ? static_cast<T*>(this) : nullptr; }
		template <typename T> const T* As() const { return Kind == T::StaticKind ? static_cast<const T*>(this) : nullptr; }
		template <typename T> bool Is() const { return Kind == T::StaticKind; }
	};

	// ------------------------------------------------------------------------------------------
	// Expressions
	// ------------------------------------------------------------------------------------------

	struct FDabExpr : FDabNode
	{
		using FDabNode::FDabNode;
	};
	using FDabExprPtr = TUniquePtr<FDabExpr>;

	struct FDabStmt;
	using FDabStmtPtr = TUniquePtr<FDabStmt>;

	enum class EDabLiteralKind : uint8 { Number, String, Bool };

	struct FDabLiteralExpr final : FDabExpr
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::LiteralExpr;
		FDabLiteralExpr() : FDabExpr(StaticKind) {}

		EDabLiteralKind LiteralKind = EDabLiteralKind::Number;
		/** Numbers: the lexeme as written (printed back verbatim). Strings: the value, escapes resolved. Bools: `true` / `false`. */
		FString Text;
		double Number = 0.0;
		bool bBool = false;
	};

	/** A bare identifier: `Moving`, `Self`, `SyncGroup`, `A_Als_Idle`. */
	struct FDabNameExpr final : FDabExpr
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::NameExpr;
		FDabNameExpr() : FDabExpr(StaticKind) {}

		FString Name;
	};

	/** `Object.Member`. `Main.HeadState.PitchAngle` is Member(Member(Name Main, HeadState), PitchAngle). */
	struct FDabMemberExpr final : FDabExpr
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::MemberExpr;
		FDabMemberExpr() : FDabExpr(StaticKind) {}

		FDabExprPtr Object;
		FString Member;
		FDabSpan MemberSpan;
	};

	/** One argument of a call or one item of a tuple: `Value` or `Name = Value`. */
	struct FDabArgument final : FDabNode
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::Argument;
		FDabArgument() : FDabNode(StaticKind) {}

		/** Empty for a positional argument. */
		FString Name;
		FDabSpan NameSpan;
		/** The name was written as a string (`"Als.Gait.Walking" = ...`). */
		bool bNameIsString = false;
		FDabExprPtr Value;

		bool IsNamed() const { return !Name.IsEmpty() || bNameIsString; }
	};
	using FDabArgumentPtr = TUniquePtr<FDabArgument>;

	/** `Callee(Args...)`. Callee is usually a NameExpr or a MemberExpr. */
	struct FDabCallExpr final : FDabExpr
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::CallExpr;
		FDabCallExpr() : FDabExpr(StaticKind) {}

		FDabExprPtr Callee;
		TArray<FDabArgumentPtr> Args;
		/** `(` to `)` inclusive. */
		FDabSpan ArgsSpan;
	};

	/** `[a, b, c]`. */
	struct FDabListExpr final : FDabExpr
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::ListExpr;
		FDabListExpr() : FDabExpr(StaticKind) {}

		TArray<FDabExprPtr> Items;
	};

	/** `(a, b)` or `(Min = 0, Max = 1)`. A single unnamed item in parentheses is a ParenExpr instead. */
	struct FDabTupleExpr final : FDabExpr
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::TupleExpr;
		FDabTupleExpr() : FDabExpr(StaticKind) {}

		TArray<FDabArgumentPtr> Items;
	};

	/**
	 * `{ statements }`: a section body (`Settings = { ... }`), a named block's body, or a map literal
	 * inside an argument list (`BlendPose = { Default = ...; LeftBackward = ...; }`). Which statement
	 * kinds are allowed where is checked by the later stages, not by the parser.
	 */
	struct FDabBlockExpr final : FDabExpr
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::BlockExpr;
		FDabBlockExpr() : FDabExpr(StaticKind) {}

		TArray<FDabStmtPtr> Statements;
	};

	/** `{ source; step; step; }`: the first step makes a pose, each later step takes the one above it. */
	struct FDabStackExpr final : FDabExpr
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::StackExpr;
		FDabStackExpr() : FDabExpr(StaticKind) {}

		TArray<FDabExprPtr> Steps;
	};

	enum class EDabUnaryOp : uint8 { Not, Negate };
	DREAMANIMLANG_API const TCHAR* GetDabUnaryOpSpelling(EDabUnaryOp Op);

	struct FDabUnaryExpr final : FDabExpr
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::UnaryExpr;
		FDabUnaryExpr() : FDabExpr(StaticKind) {}

		EDabUnaryOp Op = EDabUnaryOp::Not;
		FDabExprPtr Operand;
	};

	enum class EDabBinaryOp : uint8
	{
		Or, And,
		Equal, NotEqual,
		Less, LessEqual, Greater, GreaterEqual,
		Add, Subtract,
		Multiply, Divide,
	};
	DREAMANIMLANG_API const TCHAR* GetDabBinaryOpSpelling(EDabBinaryOp Op);
	/** Higher binds tighter: Or 1, And 2, equality 3, comparison 4, additive 5, multiplicative 6. */
	DREAMANIMLANG_API int32 GetDabBinaryOpPrecedence(EDabBinaryOp Op);

	struct FDabBinaryExpr final : FDabExpr
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::BinaryExpr;
		FDabBinaryExpr() : FDabExpr(StaticKind) {}

		EDabBinaryOp Op = EDabBinaryOp::And;
		FDabExprPtr Lhs;
		FDabExprPtr Rhs;
		FDabSpan OpSpan;
	};

	struct FDabConditionalExpr final : FDabExpr
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::ConditionalExpr;
		FDabConditionalExpr() : FDabExpr(StaticKind) {}

		FDabExprPtr Condition;
		FDabExprPtr Then;
		FDabExprPtr Else;
	};

	/** Parentheses the author wrote around an expression. Kept so the printer gives them back. */
	struct FDabParenExpr final : FDabExpr
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::ParenExpr;
		FDabParenExpr() : FDabExpr(StaticKind) {}

		FDabExprPtr Inner;
	};

	// ------------------------------------------------------------------------------------------
	// Metadata
	// ------------------------------------------------------------------------------------------

	/** `Key = Value` or a bare `Flag` inside `[ ... ]`. */
	struct FDabMetaEntry final : FDabNode
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::MetaEntry;
		FDabMetaEntry() : FDabNode(StaticKind) {}

		FString Key;
		FDabSpan KeySpan;
		bool bKeyIsString = false;
		/** Null for a flag. */
		FDabExprPtr Value;
	};

	/** `[ Blend = 0.3; Priority = 2 ]`, `[ Cached ]`. */
	struct FDabMeta final : FDabNode
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::Meta;
		FDabMeta() : FDabNode(StaticKind) {}

		TArray<TUniquePtr<FDabMetaEntry>> Entries;

		/** The entry with this key (case-sensitive), or null. */
		DREAMANIMLANG_API const FDabMetaEntry* Find(const TCHAR* Key) const;
	};

	// ------------------------------------------------------------------------------------------
	// Statements
	// ------------------------------------------------------------------------------------------

	struct FDabStmt : FDabNode
	{
		using FDabNode::FDabNode;
	};

	/** `Key = Value [meta];` -- a section (`Settings = { ... }`), a named pose, a map entry, `Entry = Idle;`. */
	struct FDabAssignStmt final : FDabStmt
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::AssignStmt;
		FDabAssignStmt() : FDabStmt(StaticKind) {}

		FString Name;
		FDabSpan NameSpan;
		bool bNameIsString = false;
		FDabExprPtr Value;
		/** Null when none was written. */
		TUniquePtr<FDabMeta> Meta;
		/** The statement ended with `;` (always true unless Value ends with `}`). */
		bool bHasSemicolon = true;
	};

	/** `Type Name [= Value] [meta];` -- `float StopBlend = 0.3 [ Group="Stop" ];`, `AnimSequence StandPose;`. */
	struct FDabDeclStmt final : FDabStmt
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::DeclStmt;
		FDabDeclStmt() : FDabStmt(StaticKind) {}

		FString TypeName;
		FDabSpan TypeSpan;
		FString Name;
		FDabSpan NameSpan;
		/** Null when no `= Value` was written. */
		FDabExprPtr Value;
		TUniquePtr<FDabMeta> Meta;
	};

	/** `From -> To : Condition [meta];` inside `Transitions = { ... }`. */
	struct FDabTransitionStmt final : FDabStmt
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::TransitionStmt;
		FDabTransitionStmt() : FDabStmt(StaticKind) {}

		FString From;
		FDabSpan FromSpan;
		FString To;
		FDabSpan ToSpan;
		FDabExprPtr Condition;
		TUniquePtr<FDabMeta> Meta;
	};

	/** A template parameter: `Seq`, or `BackTime = 0`. */
	struct FDabParam final : FDabNode
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::Param;
		FDabParam() : FDabNode(StaticKind) {}

		FString Name;
		FDabSpan NameSpan;
		/** Null when the parameter has no default. */
		FDabExprPtr Default;
	};

	/**
	 * `Word Name [(params)] [meta]` followed by a block body, `= Value;`, or just `;`.
	 * Word is one of StateMachine, State, Layer, Template, Conduit, Alias.
	 *
	 *   StateMachine StandingStates { ... }
	 *   State Stop { Pose = StopStates; }
	 *   Layer Overlay = LocalRefPose();
	 *   Layer Overlay [ LayerGroup = "Default Shared Group" ];
	 *   Template Frame(Seq, N) = SequenceEvaluator(Sequence = Seq, ExplicitFrame = N);
	 *   Conduit EntryConduit = Main.PoseState.GroundedAmount >= 1;
	 *   Alias StandingAlias = [ Crouching, StandingToCrouching ];
	 */
	struct FDabNamedBlockStmt final : FDabStmt
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::NamedBlockStmt;
		FDabNamedBlockStmt() : FDabStmt(StaticKind) {}

		FString Word;
		FDabSpan WordSpan;
		FString Name;
		FDabSpan NameSpan;
		/** Only templates have parameters; bHasParams tells `Template X()` from `Template X`. */
		bool bHasParams = false;
		TArray<TUniquePtr<FDabParam>> Params;
		TUniquePtr<FDabMeta> Meta;
		/** Set for the `{ ... }` form. */
		TUniquePtr<FDabBlockExpr> Body;
		/** Set for the `= Value;` form. Body and Value are never both set; neither is set for `Word Name [meta];`. */
		FDabExprPtr Value;
	};

	/** `#Region "Label"` or `#EndRegion`. */
	struct FDabRegionStmt final : FDabStmt
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::RegionStmt;
		FDabRegionStmt() : FDabStmt(StaticKind) {}

		bool bEnd = false;
		/** Empty for #EndRegion. */
		FString Label;
	};

	// ------------------------------------------------------------------------------------------
	// File
	// ------------------------------------------------------------------------------------------

	/** `#include "AlsCommon.dat"`. */
	struct FDabInclude final : FDabNode
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::Include;
		FDabInclude() : FDabNode(StaticKind) {}

		FString Path;
		FDabSpan PathSpan;
	};

	enum class EDabDocumentKind : uint8 { AnimBlueprint, AnimLayerInterface, AnimTemplates, Unknown };
	DREAMANIMLANG_API const TCHAR* LexToString(EDabDocumentKind Kind);

	/** `AnimBlueprint(Name="...", Root="...") { ... }`. */
	struct FDabDocument final : FDabNode
	{
		static constexpr EDabNodeKind StaticKind = EDabNodeKind::Document;
		FDabDocument() : FDabNode(StaticKind) {}

		EDabDocumentKind DocumentKind = EDabDocumentKind::Unknown;
		/** As written. */
		FString KindName;
		FDabSpan KindSpan;
		TArray<FDabArgumentPtr> HeaderArgs;
		TUniquePtr<FDabBlockExpr> Body;

		/** The value of a header argument written as a string literal (`Name`, `Root`); empty when absent. */
		DREAMANIMLANG_API FString GetHeaderString(const TCHAR* ArgName) const;
	};

	/** One `//` or block comment kept as trivia. */
	struct FDabComment
	{
		/** Verbatim, delimiters included. */
		FString Text;
		FDabSpan Span;
		bool bBlock = false;
	};

	/**
	 * Comments and blank lines around one node, kept when the parse asked for them. Recorded for
	 * statements, stack steps, list items, arguments, tuple items and includes.
	 */
	struct FDabTrivia
	{
		/** Own-line comments above the node, in source order. */
		TArray<FDabComment> Leading;
		/** Blank lines above the node's first leading comment (or the node itself), from the previous sibling or the opening bracket. */
		int32 BlankLinesBefore = 0;
		/** A comment that starts on the node's last line, after it (after its `;` or `,`). */
		TOptional<FDabComment> Trailing;
		/** For a block, stack, list, call or tuple: comments after its last child, before the closing bracket. */
		TArray<FDabComment> Inner;
	};

	/** Where an expression came from when template expansion produced it. */
	struct FDabExpansionOrigin
	{
		FString TemplateName;
		/** The template call that was replaced, in the file being compiled. */
		FDabSpan CallSpan;
		/** The template's definition, and the file it is in. */
		FDabSpan DefinitionSpan;
		FString DefinitionFile;
	};

	struct FDabFile
	{
		FString Path;
		TArray<TUniquePtr<FDabInclude>> Includes;
		/** Null when the file had no document (DAB2002). */
		TUniquePtr<FDabDocument> Document;

		/** Filled only when the parse kept trivia. */
		TMap<const FDabNode*, FDabTrivia> Trivia;
		/** Filled by template expansion: expression nodes a template call became, keyed by the root of each expansion. */
		TMap<const FDabNode*, FDabExpansionOrigin> Origins;

		const FDabTrivia* FindTrivia(const FDabNode* Node) const { return Trivia.Find(Node); }
	};

	// ------------------------------------------------------------------------------------------
	// Utilities (DabAst.cpp)
	// ------------------------------------------------------------------------------------------

	/** Deep copies. Spans are copied as they are; trivia is not (it lives in the file's side table). */
	DREAMANIMLANG_API FDabExprPtr CloneExpr(const FDabExpr& Expr);
	DREAMANIMLANG_API FDabStmtPtr CloneStmt(const FDabStmt& Stmt);
	DREAMANIMLANG_API TUniquePtr<FDabMeta> CloneMeta(const FDabMeta& Meta);
	DREAMANIMLANG_API FDabArgumentPtr CloneArgument(const FDabArgument& Argument);

	/**
	 * The whole number a constant spells (`-5`, `(3)`, `1e3`, `2.0`), read from the literal's digits so int64
	 * values above 2^53 come out exact; false for a fraction, a value outside int64 or anything else.
	 */
	DREAMANIMLANG_API bool TryGetDabInteger(const FDabExpr& Expr, int64& OutValue);
	/** An integer literal that keeps every digit of Value, under a unary minus when it is negative. */
	DREAMANIMLANG_API FDabExprPtr MakeDabInteger(int64 Value);

	/** Calls Visit on every direct child node, in source order. */
	DREAMANIMLANG_API void ForEachChild(const FDabNode& Node, TFunctionRef<void(const FDabNode&)> Visit);

	/**
	 * `Main.HeadState.PitchAngle` as {"Main","HeadState","PitchAngle"} when the expression is a chain of
	 * names and members only; false for anything else (a call, a literal).
	 */
	DREAMANIMLANG_API bool TryGetDabPath(const FDabExpr& Expr, TArray<FString>& OutSegments);
}
