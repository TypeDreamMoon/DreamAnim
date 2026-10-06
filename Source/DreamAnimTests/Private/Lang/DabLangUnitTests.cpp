// Copyright DreamMoon. All Rights Reserved.
//
// Small checks of the front end's building blocks that the corpus cannot express well: positions,
// token details, single-expression parsing, paths and cloning.

#include "Lang/DabParser.h"
#include "Lang/DabPrinter.h"
#include "Lang/DabToken.h"
#include "Misc/AutomationTest.h"

namespace UE::DreamAnim::Tests
{
	using namespace UE::DreamAnim::Lang;

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabSourcePositionsTest, "DreamAnim.Lang.Unit.SourcePositions",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDabSourcePositionsTest::RunTest(const FString& Parameters)
	{
		// \r\n, a lone \r and \n each end one line; a byte-order mark is not part of the text.
		const FDabSourceText Source(TEXT("t.dab"), FString(TEXT("﻿a\r\nbb\rccc\nd")));
		TestEqual(TEXT("BOM stripped"), Source.GetText().Left(1), FString(TEXT("a")));
		TestEqual(TEXT("line count"), Source.GetLineCount(), 4);

		int32 Line = 0;
		int32 Column = 0;
		const int32 OffsetOfC = Source.GetText().Find(TEXT("ccc"));
		Source.GetLineAndColumn(OffsetOfC + 1, Line, Column);
		TestEqual(TEXT("line of the second c"), Line, 3);
		TestEqual(TEXT("column of the second c"), Column, 2);
		TestEqual(TEXT("line text"), Source.GetLineText(2), FString(TEXT("bb")));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabLexerDetailsTest, "DreamAnim.Lang.Unit.LexerDetails",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDabLexerDetailsTest::RunTest(const FString& Parameters)
	{
		const FDabSourceText Source(TEXT("t.dab"), FString(TEXT("#Region \"走路\"\n移动速度 = 1.50e2 -> A_B;\nx # y")));
		FDabDiagnosticSink Sink(Source.GetPath());
		const TArray<FDabToken> Tokens = LexDab(Source, Sink);

		TestTrue(TEXT("enough tokens"), Tokens.Num() >= 9);
		if (Tokens.Num() < 9)
		{
			return false;
		}
		TestEqual(TEXT("directive"), Tokens[0].Kind, EDabTokenKind::Directive);
		TestEqual(TEXT("directive word"), Tokens[0].Text, FString(TEXT("Region")));
		TestEqual(TEXT("string value"), Tokens[1].Text, FString(TEXT("走路")));
		TestEqual(TEXT("Chinese identifier"), Tokens[2].Kind, EDabTokenKind::Identifier);
		TestEqual(TEXT("Chinese identifier text"), Tokens[2].Text, FString(TEXT("移动速度")));
		TestTrue(TEXT("identifier starts its line"), Tokens[2].bAtLineStart);
		TestEqual(TEXT("one newline before it"), Tokens[2].NewlinesBefore, 1);
		TestEqual(TEXT("number lexeme kept"), Tokens[4].Text, FString(TEXT("1.50e2")));
		TestEqual(TEXT("number value"), Tokens[4].Number, 150.0);
		TestEqual(TEXT("arrow"), Tokens[5].Kind, EDabTokenKind::Arrow);
		TestEqual(TEXT("an identifier with an underscore"), Tokens[6].Text, FString(TEXT("A_B")));
		TestEqual(TEXT("ends with EndOfFile"), Tokens.Last().Kind, EDabTokenKind::EndOfFile);

		// `#` that does not start its line is not a directive.
		TestTrue(TEXT("mid-line # is reported"), Sink.GetDiagnostics().ContainsByPredicate([](const FDabDiagnostic& D) { return D.Code == TEXT("DAB1001"); }));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabParseExpressionTest, "DreamAnim.Lang.Unit.ParseExpression",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDabParseExpressionTest::RunTest(const FString& Parameters)
	{
		{
			const FDabSourceText Source(TEXT("e"), FString(TEXT("!Moving && StateWeight(Move) >= 1 || Main.Gait == \"Als.Gait.Walking\"")));
			FDabDiagnosticSink Sink(Source.GetPath());
			const FDabExprPtr Expr = ParseDabExpression(Source, Sink);
			TestFalse(TEXT("no errors"), Sink.HasErrors());
			const FDabBinaryExpr* Or = Expr ? Expr->As<FDabBinaryExpr>() : nullptr;
			TestTrue(TEXT("|| binds loosest"), Or && Or->Op == EDabBinaryOp::Or);
			const FDabBinaryExpr* And = Or ? Or->Lhs->As<FDabBinaryExpr>() : nullptr;
			TestTrue(TEXT("&& under ||"), And && And->Op == EDabBinaryOp::And);
			const FDabUnaryExpr* Not = And ? And->Lhs->As<FDabUnaryExpr>() : nullptr;
			TestTrue(TEXT("! applies to Moving alone"), Not && Not->Op == EDabUnaryOp::Not && Not->Operand->Is<FDabNameExpr>());
		}
		{
			const FDabSourceText Source(TEXT("e"), FString(TEXT("a b")));
			FDabDiagnosticSink Sink(Source.GetPath());
			const FDabExprPtr Expr = ParseDabExpression(Source, Sink);
			TestTrue(TEXT("two expressions are an error"), Sink.HasErrors());
			TestNull(TEXT("and give no expression"), Expr.Get());
		}
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabPathsAndCloneTest, "DreamAnim.Lang.Unit.PathsAndClone",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDabPathsAndCloneTest::RunTest(const FString& Parameters)
	{
		{
			const FDabSourceText Source(TEXT("e"), FString(TEXT("Main.HeadState.PitchAngle")));
			FDabDiagnosticSink Sink(Source.GetPath());
			const FDabExprPtr Expr = ParseDabExpression(Source, Sink);
			TArray<FString> Segments;
			TestTrue(TEXT("a member chain is a path"), Expr && TryGetDabPath(*Expr, Segments));
			TestEqual(TEXT("path"), FString::Join(Segments, TEXT("|")), FString(TEXT("Main|HeadState|PitchAngle")));
		}
		{
			const FDabSourceText Source(TEXT("e"), FString(TEXT("Self.GetParent()")));
			FDabDiagnosticSink Sink(Source.GetPath());
			const FDabExprPtr Expr = ParseDabExpression(Source, Sink);
			TArray<FString> Segments;
			TestFalse(TEXT("a call is not a path"), Expr && TryGetDabPath(*Expr, Segments));
		}
		{
			const FString Text = TEXT(
				"AnimBlueprint(Name=\"A/B\", Root=\"Game\")\n"
				"{\n"
				"    Graph = {\n"
				"        Look = { BlendSpaceEvaluator(BlendSpace = Assets.Look, X = Main.A.B); Call(Self.F, On = BecomeRelevant); } [ Cached ];\n"
				"        Output = TwoWayBlend(A = Look, B = (1, 2), Alpha = Main.X > 0 ? 1 : -0.5);\n"
				"    }\n"
				"    StateMachine M [ MaxTransitionsPerFrame = 1 ]\n"
				"    {\n"
				"        Transitions = { A -> B : !Moving && StateWeight(A) >= 1 [ Blend = 0.3; Inertialize ]; }\n"
				"    }\n"
				"}\n");
			const FDabSourceText Source(TEXT("c.dab"), Text);
			FDabDiagnosticSink Sink(Source.GetPath());
			const TUniquePtr<FDabFile> File = ParseDab(Source, Sink);
			TestFalse(TEXT("clone source parses"), Sink.HasErrors());
			if (!File->Document || !File->Document->Body)
			{
				return false;
			}

			// Clone every statement into a second file and compare the dumps.
			FDabFile Copy;
			Copy.Path = File->Path;
			Copy.Document = MakeUnique<FDabDocument>();
			Copy.Document->DocumentKind = File->Document->DocumentKind;
			Copy.Document->KindName = File->Document->KindName;
			for (const FDabArgumentPtr& Arg : File->Document->HeaderArgs)
			{
				Copy.Document->HeaderArgs.Add(CloneArgument(*Arg));
			}
			Copy.Document->Body = MakeUnique<FDabBlockExpr>();
			for (const FDabStmtPtr& Stmt : File->Document->Body->Statements)
			{
				Copy.Document->Body->Statements.Add(CloneStmt(*Stmt));
			}
			TestEqual(TEXT("a deep clone dumps the same"), DumpDabTree(Copy), DumpDabTree(*File));
		}
		return true;
	}
}
