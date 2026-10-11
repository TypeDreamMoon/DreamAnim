// Copyright DreamMoon. All Rights Reserved.
//
// Building syntax-tree expressions for the .dcr text: what lowering and the value codec produce, in the
// shapes the parser would have built from the same text.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabAst.h"

namespace UE::DreamAnim::Rig
{
	Lang::FDabExprPtr MakeDcrName(const FString& Name);
	Lang::FDabExprPtr MakeDcrString(const FString& Value);
	Lang::FDabExprPtr MakeDcrBool(bool bValue);
	/** Through FormatDabNumber; a negative number is a negation of a literal, as the parser builds it. */
	Lang::FDabExprPtr MakeDcrNumber(double Value);
	/** An integer kept exactly. */
	Lang::FDabExprPtr MakeDcrInteger(int64 Value);
	Lang::FDabExprPtr MakeDcrMember(Lang::FDabExprPtr Object, const FString& Member);
	/** Positional arguments first, then named ones. */
	Lang::FDabExprPtr MakeDcrCall(Lang::FDabExprPtr Callee, TArray<Lang::FDabExprPtr> Positional, TArray<TPair<FString, Lang::FDabExprPtr>> Named = {});
	Lang::FDabExprPtr MakeDcrCall(const FString& Callee, TArray<Lang::FDabExprPtr> Positional, TArray<TPair<FString, Lang::FDabExprPtr>> Named = {});
	/** `(A = x, B = y)`; with every name empty, `(x, y)`. Callers never make a tuple of one positional item: `(x)` reads back as parentheses. */
	Lang::FDabExprPtr MakeDcrTuple(TArray<TPair<FString, Lang::FDabExprPtr>> Items);
	Lang::FDabExprPtr MakeDcrList(TArray<Lang::FDabExprPtr> Items);
	Lang::FDabExprPtr MakeDcrUnary(Lang::EDabUnaryOp Op, Lang::FDabExprPtr Operand);
	/** Adds the parentheses precedence needs around either side. */
	Lang::FDabExprPtr MakeDcrBinary(Lang::EDabBinaryOp Op, Lang::FDabExprPtr Lhs, Lang::FDabExprPtr Rhs);
	Lang::FDabExprPtr MakeDcrConditional(Lang::FDabExprPtr Condition, Lang::FDabExprPtr Then, Lang::FDabExprPtr Else);
	Lang::FDabExprPtr MakeDcrIndex(Lang::FDabExprPtr Object, Lang::FDabExprPtr Index);
	/** `Unsupported("What")`: stands in for something the text cannot say, so lowering goes on. */
	Lang::FDabExprPtr MakeDcrUnsupported(const FString& What);

	/** True for a literal, a negated number, or a tuple, list or element key made only of those. */
	bool IsDcrConstantLiteral(const Lang::FDabExpr& Expr);
}
