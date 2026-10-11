// Copyright DreamMoon. All Rights Reserved.

#include "Rig/DcrSyntax.h"

#include <cmath>

#include "Model/DabModel.h"

namespace UE::DreamAnim::Rig
{
	using namespace UE::DreamAnim::Lang;

	namespace SyntaxPrivate
	{
		FDabExprPtr MakeParen(FDabExprPtr Inner)
		{
			TUniquePtr<FDabParenExpr> Paren = MakeUnique<FDabParenExpr>();
			Paren->Inner = MoveTemp(Inner);
			return FDabExprPtr(MoveTemp(Paren));
		}

		/** Whether Operand needs parentheses as one side of a binary operator of the given precedence (left-associative). */
		bool NeedsParens(const FDabExpr& Operand, int32 Precedence, bool bRightSide)
		{
			if (Operand.Is<FDabConditionalExpr>())
			{
				return true;
			}
			if (const FDabBinaryExpr* Binary = Operand.As<FDabBinaryExpr>())
			{
				const int32 OperandPrecedence = GetDabBinaryOpPrecedence(Binary->Op);
				return bRightSide ? OperandPrecedence <= Precedence : OperandPrecedence < Precedence;
			}
			return false;
		}

		bool IsOperand(const FDabExpr& Expr)
		{
			return Expr.Is<FDabBinaryExpr>() || Expr.Is<FDabUnaryExpr>() || Expr.Is<FDabConditionalExpr>();
		}

		FDabArgumentPtr MakeArgument(const FString& Name, FDabExprPtr Value)
		{
			FDabArgumentPtr Argument = MakeUnique<FDabArgument>();
			Argument->Name = Name;
			Argument->Value = MoveTemp(Value);
			return Argument;
		}
	}

	FDabExprPtr MakeDcrName(const FString& Name)
	{
		TUniquePtr<FDabNameExpr> Expr = MakeUnique<FDabNameExpr>();
		Expr->Name = Name;
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeDcrString(const FString& Value)
	{
		TUniquePtr<FDabLiteralExpr> Expr = MakeUnique<FDabLiteralExpr>();
		Expr->LiteralKind = EDabLiteralKind::String;
		Expr->Text = Value;
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeDcrBool(bool bValue)
	{
		TUniquePtr<FDabLiteralExpr> Expr = MakeUnique<FDabLiteralExpr>();
		Expr->LiteralKind = EDabLiteralKind::Bool;
		Expr->bBool = bValue;
		Expr->Text = bValue ? TEXT("true") : TEXT("false");
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeDcrNumber(double Value)
	{
		// Number lexemes carry no sign: the parser reads `-0.5` as a negation. `-0` keeps its sign the same way.
		if (Value < 0.0 || (Value == 0.0 && std::signbit(Value)))
		{
			return MakeDcrUnary(EDabUnaryOp::Negate, MakeDcrNumber(-Value));
		}
		TUniquePtr<FDabLiteralExpr> Expr = MakeUnique<FDabLiteralExpr>();
		Expr->LiteralKind = EDabLiteralKind::Number;
		Expr->Number = Value;
		Expr->Text = Model::FormatDabNumber(Value);
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeDcrInteger(int64 Value)
	{
		return MakeDabInteger(Value);
	}

	FDabExprPtr MakeDcrMember(FDabExprPtr Object, const FString& Member)
	{
		TUniquePtr<FDabMemberExpr> Expr = MakeUnique<FDabMemberExpr>();
		Expr->Object = Object.IsValid() && SyntaxPrivate::IsOperand(*Object) ? SyntaxPrivate::MakeParen(MoveTemp(Object)) : MoveTemp(Object);
		Expr->Member = Member;
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeDcrCall(FDabExprPtr Callee, TArray<FDabExprPtr> Positional, TArray<TPair<FString, FDabExprPtr>> Named)
	{
		TUniquePtr<FDabCallExpr> Expr = MakeUnique<FDabCallExpr>();
		Expr->Callee = MoveTemp(Callee);
		for (FDabExprPtr& Arg : Positional)
		{
			Expr->Args.Add(SyntaxPrivate::MakeArgument(FString(), MoveTemp(Arg)));
		}
		for (TPair<FString, FDabExprPtr>& Arg : Named)
		{
			Expr->Args.Add(SyntaxPrivate::MakeArgument(Arg.Key, MoveTemp(Arg.Value)));
		}
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeDcrCall(const FString& Callee, TArray<FDabExprPtr> Positional, TArray<TPair<FString, FDabExprPtr>> Named)
	{
		return MakeDcrCall(MakeDcrName(Callee), MoveTemp(Positional), MoveTemp(Named));
	}

	FDabExprPtr MakeDcrTuple(TArray<TPair<FString, FDabExprPtr>> Items)
	{
		TUniquePtr<FDabTupleExpr> Expr = MakeUnique<FDabTupleExpr>();
		for (TPair<FString, FDabExprPtr>& Item : Items)
		{
			Expr->Items.Add(SyntaxPrivate::MakeArgument(Item.Key, MoveTemp(Item.Value)));
		}
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeDcrList(TArray<FDabExprPtr> Items)
	{
		TUniquePtr<FDabListExpr> Expr = MakeUnique<FDabListExpr>();
		Expr->Items = MoveTemp(Items);
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeDcrUnary(EDabUnaryOp Op, FDabExprPtr Operand)
	{
		TUniquePtr<FDabUnaryExpr> Expr = MakeUnique<FDabUnaryExpr>();
		Expr->Op = Op;
		const bool bNeedsParens = Operand.IsValid() && (Operand->Is<FDabBinaryExpr>() || Operand->Is<FDabConditionalExpr>());
		Expr->Operand = bNeedsParens ? SyntaxPrivate::MakeParen(MoveTemp(Operand)) : MoveTemp(Operand);
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeDcrBinary(EDabBinaryOp Op, FDabExprPtr Lhs, FDabExprPtr Rhs)
	{
		const int32 Precedence = GetDabBinaryOpPrecedence(Op);
		TUniquePtr<FDabBinaryExpr> Expr = MakeUnique<FDabBinaryExpr>();
		Expr->Op = Op;
		Expr->Lhs = (Lhs.IsValid() && SyntaxPrivate::NeedsParens(*Lhs, Precedence, false)) ? SyntaxPrivate::MakeParen(MoveTemp(Lhs)) : MoveTemp(Lhs);
		Expr->Rhs = (Rhs.IsValid() && SyntaxPrivate::NeedsParens(*Rhs, Precedence, true)) ? SyntaxPrivate::MakeParen(MoveTemp(Rhs)) : MoveTemp(Rhs);
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeDcrConditional(FDabExprPtr Condition, FDabExprPtr Then, FDabExprPtr Else)
	{
		TUniquePtr<FDabConditionalExpr> Expr = MakeUnique<FDabConditionalExpr>();
		const bool bNeedsParens = Condition.IsValid() && Condition->Is<FDabConditionalExpr>();
		Expr->Condition = bNeedsParens ? SyntaxPrivate::MakeParen(MoveTemp(Condition)) : MoveTemp(Condition);
		Expr->Then = MoveTemp(Then);
		Expr->Else = Else.IsValid() && Else->Is<FDabConditionalExpr>() ? SyntaxPrivate::MakeParen(MoveTemp(Else)) : MoveTemp(Else);
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeDcrIndex(FDabExprPtr Object, FDabExprPtr Index)
	{
		TUniquePtr<FDabIndexExpr> Expr = MakeUnique<FDabIndexExpr>();
		Expr->Object = Object.IsValid() && SyntaxPrivate::IsOperand(*Object) ? SyntaxPrivate::MakeParen(MoveTemp(Object)) : MoveTemp(Object);
		Expr->Index = MoveTemp(Index);
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeDcrUnsupported(const FString& What)
	{
		TArray<FDabExprPtr> Args;
		Args.Add(MakeDcrString(What));
		return MakeDcrCall(TEXT("Unsupported"), MoveTemp(Args));
	}

	bool IsDcrConstantLiteral(const FDabExpr& Expr)
	{
		if (Expr.Is<FDabLiteralExpr>())
		{
			return true;
		}
		if (const FDabUnaryExpr* Unary = Expr.As<FDabUnaryExpr>())
		{
			return Unary->Op == EDabUnaryOp::Negate && Unary->Operand && Unary->Operand->Is<FDabLiteralExpr>();
		}
		if (const FDabParenExpr* Paren = Expr.As<FDabParenExpr>())
		{
			return Paren->Inner && IsDcrConstantLiteral(*Paren->Inner);
		}
		if (const FDabTupleExpr* Tuple = Expr.As<FDabTupleExpr>())
		{
			for (const FDabArgumentPtr& Item : Tuple->Items)
			{
				if (!Item || !Item->Value || !IsDcrConstantLiteral(*Item->Value))
				{
					return false;
				}
			}
			return true;
		}
		if (const FDabListExpr* List = Expr.As<FDabListExpr>())
		{
			for (const FDabExprPtr& Item : List->Items)
			{
				if (!Item || !IsDcrConstantLiteral(*Item))
				{
					return false;
				}
			}
			return true;
		}
		return false;
	}
}
