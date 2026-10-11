// Copyright DreamMoon. All Rights Reserved.
//
// Tests of the front end for Control Rig documents (.dcr): names in backticks and `%` in the lexer, the extra
// block words, typed functions, generic types and code blocks in the parser, the canonical format of all of
// them, template expansion inside code, and the diagnostics of the new syntax. The .dab suites cover the rest.
//
//   Corpus   every file under Tests/Corpus/Dcr is parsed without diagnostics and printed. The files there are
//            canonical, so the print must equal the file; every file under Tests/Corpus/Dcr/Raw (the examples
//            of the syntax proposal as written there) must print as the file of the same name one folder up.
//            Printing the print must change nothing, and a deep copy must dump the same tree. A golden that
//            differs is written next to it as `<golden>.actual` and the test fails; review it and promote it.

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Lang/DabParser.h"
#include "Lang/DabPrinter.h"
#include "Lang/DabTemplates.h"
#include "Lang/DabToken.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::DreamAnim::Tests
{
	using namespace UE::DreamAnim::Lang;

	// Named apart from the helpers of the other Lang test files: the module may be built as one unity blob.
	namespace DcrTestPrivate
	{
		FString NormalizeNewlines(const FString& Text)
		{
			return Text.Replace(TEXT("\r\n"), TEXT("\n")).Replace(TEXT("\r"), TEXT("\n"));
		}

		/** The first differing line of two texts, for a readable failure. Empty when equal. */
		FString DescribeDifference(const FString& Expected, const FString& Actual)
		{
			TArray<FString> ExpectedLines;
			TArray<FString> ActualLines;
			NormalizeNewlines(Expected).ParseIntoArray(ExpectedLines, TEXT("\n"), false);
			NormalizeNewlines(Actual).ParseIntoArray(ActualLines, TEXT("\n"), false);
			const int32 Count = FMath::Max(ExpectedLines.Num(), ActualLines.Num());
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FString E = ExpectedLines.IsValidIndex(Index) ? ExpectedLines[Index] : FString(TEXT("<end>"));
				const FString A = ActualLines.IsValidIndex(Index) ? ActualLines[Index] : FString(TEXT("<end>"));
				if (!E.Equals(A, ESearchCase::CaseSensitive))
				{
					return FString::Printf(TEXT("line %d: expected `%s`, got `%s`"), Index + 1, *E, *A);
				}
			}
			return FString();
		}

		FString GetCorpusDir()
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamAnim"));
			return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Tests"), TEXT("Corpus"), TEXT("Dcr")) : FString();
		}

		FString DescribeDiagnostics(const FDabDiagnosticSink& Sink)
		{
			TArray<FString> Lines;
			for (const FDabDiagnostic& Diagnostic : Sink.GetDiagnostics())
			{
				Lines.Add(FDabDiagnosticSink::ToWireString(Diagnostic));
			}
			return FString::Join(Lines, TEXT("\n"));
		}

		/** Parses Text; null, after reporting the diagnostics as an error, when there are any. */
		TUniquePtr<FDabFile> ParseClean(FAutomationTestBase& Test, const TCHAR* What, const FString& Text)
		{
			FDabDiagnosticSink Sink(TEXT("t.dcr"));
			const FDabSourceText Source(TEXT("t.dcr"), Text);
			TUniquePtr<FDabFile> File = ParseDab(Source, Sink);
			if (Sink.GetDiagnostics().Num() > 0)
			{
				Test.AddError(FString::Printf(TEXT("%s: unexpected diagnostics:\n%s"), What, *DescribeDiagnostics(Sink)));
				return nullptr;
			}
			return File;
		}

		/** The text must parse clean and dump as ExpectedDump. */
		void CheckDump(FAutomationTestBase& Test, const TCHAR* What, const FString& Text, const FString& ExpectedDump)
		{
			const TUniquePtr<FDabFile> File = ParseClean(Test, What, Text);
			if (!File)
			{
				return;
			}
			const FString Difference = DescribeDifference(ExpectedDump, DumpDabTree(*File));
			if (!Difference.IsEmpty())
			{
				Test.AddError(FString::Printf(TEXT("%s: the tree differs (%s)."), What, *Difference));
			}
		}

		/** The text must parse clean and print as Expected; that must print as itself again, with the same tree. */
		void CheckFormat(FAutomationTestBase& Test, const TCHAR* What, const FString& Text, const FString& Expected)
		{
			const TUniquePtr<FDabFile> File = ParseClean(Test, What, Text);
			if (!File)
			{
				return;
			}
			const FString Printed = PrintDab(*File);
			const FString Difference = DescribeDifference(Expected, Printed);
			if (!Difference.IsEmpty())
			{
				Test.AddError(FString::Printf(TEXT("%s: the canonical text differs (%s)."), What, *Difference));
				return;
			}

			const TUniquePtr<FDabFile> Again = ParseClean(Test, What, Printed);
			if (!Again)
			{
				return;
			}
			const FString TreeDifference = DescribeDifference(DumpDabTree(*File), DumpDabTree(*Again));
			if (!TreeDifference.IsEmpty())
			{
				Test.AddError(FString::Printf(TEXT("%s: printing changed the tree (%s)."), What, *TreeDifference));
			}
			const FString FormatDifference = DescribeDifference(Printed, PrintDab(*Again));
			if (!FormatDifference.IsEmpty())
			{
				Test.AddError(FString::Printf(TEXT("%s: formatting is not idempotent (%s)."), What, *FormatDifference));
			}
		}

		struct FExpectedDiagnostic
		{
			const TCHAR* Code;
			int32 Line;
			int32 Column;
		};

		/** The text must raise exactly these diagnostics, in this order. Returns the file for further checks. */
		TUniquePtr<FDabFile> ParseExpecting(FAutomationTestBase& Test, const TCHAR* What, const FString& Text,
			const TArray<FExpectedDiagnostic>& Expected)
		{
			FDabDiagnosticSink Sink(TEXT("t.dcr"));
			const FDabSourceText Source(TEXT("t.dcr"), Text);
			TUniquePtr<FDabFile> File = ParseDab(Source, Sink);

			const TArray<FDabDiagnostic>& Actual = Sink.GetDiagnostics();
			bool bSame = Actual.Num() == Expected.Num();
			for (int32 Index = 0; bSame && Index < Actual.Num(); ++Index)
			{
				bSame = Actual[Index].Code.Equals(Expected[Index].Code, ESearchCase::CaseSensitive)
					&& Actual[Index].Span.Line == Expected[Index].Line
					&& Actual[Index].Span.Column == Expected[Index].Column;
			}
			if (!bSame)
			{
				TArray<FString> Wanted;
				for (const FExpectedDiagnostic& Diagnostic : Expected)
				{
					Wanted.Add(FString::Printf(TEXT("%s (%d,%d)"), Diagnostic.Code, Diagnostic.Line, Diagnostic.Column));
				}
				Test.AddError(FString::Printf(TEXT("%s: expected %s; got:\n%s"), What,
					*FString::Join(Wanted, TEXT(", ")), *DescribeDiagnostics(Sink)));
			}
			return File;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Corpus
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDcrCorpusTest, "DreamAnim.Lang.Dcr.Corpus",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	void FDcrCorpusTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
	{
		const FString Dir = DcrTestPrivate::GetCorpusDir();
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			const FString Folder = Pass == 0 ? Dir : FPaths::Combine(Dir, TEXT("Raw"));
			TArray<FString> Files;
			IFileManager::Get().FindFiles(Files, *FPaths::Combine(Folder, TEXT("*.dcr")), true, false);
			Files.Sort();
			for (const FString& File : Files)
			{
				OutBeautifiedNames.Add(Pass == 0 ? FPaths::GetBaseFilename(File) : FString(TEXT("Raw_")) + FPaths::GetBaseFilename(File));
				OutTestCommands.Add(FPaths::Combine(Folder, File));
			}
		}
	}

	bool FDcrCorpusTest::RunTest(const FString& Path)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			AddError(FString::Printf(TEXT("Cannot read %s"), *Path));
			return false;
		}

		FDabDiagnosticSink Sink(Path);
		const FDabSourceText Source(Path, Text);
		const TUniquePtr<FDabFile> File = ParseDab(Source, Sink);
		if (Sink.GetDiagnostics().Num() > 0)
		{
			AddError(FString::Printf(TEXT("A corpus file parses without diagnostics:\n%s"), *DcrTestPrivate::DescribeDiagnostics(Sink)));
			return false;
		}

		const FString Printed = PrintDab(*File);

		// The golden is the file itself, or for a file under Raw the file of the same name one folder up.
		FString GoldenPath = Path;
		if (FPaths::GetCleanFilename(FPaths::GetPath(Path)).Equals(TEXT("Raw"), ESearchCase::CaseSensitive))
		{
			GoldenPath = FPaths::Combine(FPaths::GetPath(FPaths::GetPath(Path)), FPaths::GetCleanFilename(Path));
		}
		FString Golden;
		if (!FFileHelper::LoadFileToString(Golden, *GoldenPath))
		{
			AddError(FString::Printf(TEXT("Cannot read the golden %s"), *GoldenPath));
			return false;
		}
		const FString Difference = DcrTestPrivate::DescribeDifference(Golden, Printed);
		if (!Difference.IsEmpty())
		{
			FFileHelper::SaveStringToFile(Printed, *(GoldenPath + TEXT(".actual")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			AddError(FString::Printf(TEXT("%s differs from the printed text (%s); wrote %s.actual."),
				*FPaths::GetCleanFilename(GoldenPath), *Difference, *FPaths::GetCleanFilename(GoldenPath)));
		}
		else
		{
			IFileManager::Get().Delete(*(GoldenPath + TEXT(".actual")), false, false, true);
		}

		// The print parses to the same tree and formats to itself.
		FDabDiagnosticSink PrintedSink(Path);
		const FDabSourceText PrintedSource(Path, Printed);
		const TUniquePtr<FDabFile> Reparsed = ParseDab(PrintedSource, PrintedSink);
		if (PrintedSink.GetDiagnostics().Num() > 0)
		{
			AddError(FString::Printf(TEXT("The printed text does not parse clean:\n%s"), *DcrTestPrivate::DescribeDiagnostics(PrintedSink)));
			return false;
		}
		const FString TreeDifference = DcrTestPrivate::DescribeDifference(DumpDabTree(*File), DumpDabTree(*Reparsed));
		if (!TreeDifference.IsEmpty())
		{
			AddError(FString::Printf(TEXT("Printing changed the tree (%s)."), *TreeDifference));
		}
		const FString FormatDifference = DcrTestPrivate::DescribeDifference(Printed, PrintDab(*Reparsed));
		if (!FormatDifference.IsEmpty())
		{
			AddError(FString::Printf(TEXT("Formatting is not idempotent (%s)."), *FormatDifference));
		}

		// A deep copy of every statement dumps the same tree.
		if (File->Document && File->Document->Body)
		{
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
			const FString CloneDifference = DcrTestPrivate::DescribeDifference(DumpDabTree(*File), DumpDabTree(Copy));
			if (!CloneDifference.IsEmpty())
			{
				AddError(FString::Printf(TEXT("A deep copy dumps another tree (%s)."), *CloneDifference));
			}
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Lexer
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrLexerTest, "DreamAnim.Lang.Dcr.Lexer",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDcrLexerTest::RunTest(const FString& Parameters)
	{
		{
			// A name in backticks is one Identifier, spaces and case kept; a backtick in a comment is just a comment.
			const FDabSourceText Source(TEXT("t.dcr"), FString(TEXT("`Module Settings`.ControlSize % 2 `x` // `not a name`\n`Old Arm CONSTR`")));
			FDabDiagnosticSink Sink(Source.GetPath());
			const TArray<FDabToken> Tokens = LexDab(Source, Sink);

			TestFalse(TEXT("names in backticks raise nothing"), Sink.HasErrors());
			TestEqual(TEXT("token count"), Tokens.Num(), 8);
			if (Tokens.Num() != 8)
			{
				return false;
			}
			TestEqual(TEXT("first token"), Tokens[0].Kind, EDabTokenKind::Identifier);
			TestEqual(TEXT("its text"), Tokens[0].Text, FString(TEXT("Module Settings")));
			TestTrue(TEXT("is quoted"), Tokens[0].bQuoted);
			TestEqual(TEXT("its span covers the backticks"), Tokens[0].Span.Length, 17);
			TestEqual(TEXT("member access follows"), Tokens[1].Kind, EDabTokenKind::Dot);
			TestEqual(TEXT("a plain identifier"), Tokens[2].Text, FString(TEXT("ControlSize")));
			TestFalse(TEXT("is not quoted"), Tokens[2].bQuoted);
			TestEqual(TEXT("percent"), Tokens[3].Kind, EDabTokenKind::Percent);
			TestEqual(TEXT("number"), Tokens[4].Kind, EDabTokenKind::Number);
			TestEqual(TEXT("one letter in backticks"), Tokens[5].Text, FString(TEXT("x")));
			TestTrue(TEXT("is quoted too"), Tokens[5].bQuoted);
			TestEqual(TEXT("a name keeps its case"), Tokens[6].Text, FString(TEXT("Old Arm CONSTR")));
			TestTrue(TEXT("and starts its line"), Tokens[6].bAtLineStart);
			TestEqual(TEXT("ends with EndOfFile"), Tokens[7].Kind, EDabTokenKind::EndOfFile);
			TestEqual(TEXT("percent spelling"), FString(GetDabTokenSpelling(EDabTokenKind::Percent)), FString(TEXT("%")));
		}
		{
			// Unclosed and empty names: DAB1007 at the opening backtick, and a token each so the parser stays aligned.
			const FDabSourceText Source(TEXT("t.dcr"), FString(TEXT("a `abc\n``\nb")));
			FDabDiagnosticSink Sink(Source.GetPath());
			const TArray<FDabToken> Tokens = LexDab(Source, Sink);

			TestEqual(TEXT("two diagnostics"), Sink.Num(), 2);
			if (Sink.Num() == 2)
			{
				TestEqual(TEXT("unclosed code"), Sink.GetDiagnostics()[0].Code, FString(TEXT("DAB1007")));
				TestEqual(TEXT("unclosed line"), Sink.GetDiagnostics()[0].Span.Line, 1);
				TestEqual(TEXT("unclosed column"), Sink.GetDiagnostics()[0].Span.Column, 3);
				TestEqual(TEXT("empty code"), Sink.GetDiagnostics()[1].Code, FString(TEXT("DAB1007")));
				TestEqual(TEXT("empty line"), Sink.GetDiagnostics()[1].Span.Line, 2);
				TestEqual(TEXT("empty column"), Sink.GetDiagnostics()[1].Span.Column, 1);
			}
			TestEqual(TEXT("token count"), Tokens.Num(), 5);
			if (Tokens.Num() == 5)
			{
				TestEqual(TEXT("the unclosed name runs to the end of its line"), Tokens[1].Text, FString(TEXT("abc")));
				TestTrue(TEXT("and is quoted"), Tokens[1].bQuoted);
				TestTrue(TEXT("the empty name is empty"), Tokens[2].Text.IsEmpty() && Tokens[2].bQuoted);
				TestEqual(TEXT("lexing goes on"), Tokens[3].Text, FString(TEXT("b")));
			}
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Parser: the shape of the tree
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSyntaxTest, "DreamAnim.Lang.Dcr.Syntax",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDcrSyntaxTest::RunTest(const FString& Parameters)
	{
		// let, set and expression statements.
		DcrTestPrivate::CheckDump(*this, TEXT("let, set, expression"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Event Forward\n"
				"    {\n"
				"        let A = F(1);\n"
				"        A = A + 1;\n"
				"        G(A);\n"
				"    }\n"
				"}\n"),
			TEXT(
				"Document ControlRig\n"
				"  Arg Name\n"
				"    String \"X\"\n"
				"  Block\n"
				"    NamedBlock Event Forward\n"
				"      Code\n"
				"        Let A\n"
				"          Call\n"
				"            Name F\n"
				"            Arg\n"
				"              Number 1\n"
				"        Set\n"
				"          Name A\n"
				"          Binary +\n"
				"            Name A\n"
				"            Number 1\n"
				"        ExprStmt\n"
				"          Call\n"
				"            Name G\n"
				"            Arg\n"
				"              Name A\n"));

		// A function with typed parameters, a default and an output; if, else if, else, for, index and return.
		DcrTestPrivate::CheckDump(*this, TEXT("function with control flow"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Function F(Array<double> Values, int32 N = 2) -> (double Out)\n"
				"    {\n"
				"        if (N > 0)\n"
				"        {\n"
				"            Values[0] = 1;\n"
				"        }\n"
				"        else if (N < 0)\n"
				"        {\n"
				"        }\n"
				"        else\n"
				"        {\n"
				"        }\n"
				"        for (A, B in Values)\n"
				"        {\n"
				"        }\n"
				"        return (Out = Values[N]);\n"
				"    }\n"
				"}\n"),
			TEXT(
				"Document ControlRig\n"
				"  Arg Name\n"
				"    String \"X\"\n"
				"  Block\n"
				"    NamedBlock Function F (Array<double> Values, int32 N) -> (double Out)\n"
				"      Param Array<double> Values\n"
				"      Param int32 N\n"
				"        Number 2\n"
				"      Param double Out\n"
				"      Code\n"
				"        If\n"
				"          Binary >\n"
				"            Name N\n"
				"            Number 0\n"
				"          Code\n"
				"            Set\n"
				"              Index\n"
				"                Name Values\n"
				"                Number 0\n"
				"              Number 1\n"
				"          If\n"
				"            Binary <\n"
				"              Name N\n"
				"              Number 0\n"
				"            Code\n"
				"            Code\n"
				"        For A, B\n"
				"          Name Values\n"
				"          Code\n"
				"        Return\n"
				"          Tuple\n"
				"            Arg Out\n"
				"              Index\n"
				"                Name Values\n"
				"                Name N\n"));

		// A pure function, a document-level Detached block with metadata, a nested Detached statement, `%`.
		DcrTestPrivate::CheckDump(*this, TEXT("pure function and Detached"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Function F() -> (double R) [ Public ] = 1 % 2;\n"
				"    Detached [ Graph = \"G\" ]\n"
				"    {\n"
				"        Detached\n"
				"        {\n"
				"            `in` = 1;\n"
				"        }\n"
				"    }\n"
				"}\n"),
			TEXT(
				"Document ControlRig\n"
				"  Arg Name\n"
				"    String \"X\"\n"
				"  Block\n"
				"    NamedBlock Function F () -> (double R)\n"
				"      Param double R\n"
				"      Meta\n"
				"        MetaEntry Public\n"
				"      Binary %\n"
				"        Number 1\n"
				"        Number 2\n"
				"    NamedBlock Detached\n"
				"      Meta\n"
				"        MetaEntry Graph\n"
				"          String \"G\"\n"
				"      Code\n"
				"        Detached\n"
				"          Code\n"
				"            Set\n"
				"              Name in\n"
				"              Number 1\n"));

		// Generic types, names in backticks as types, nested hierarchy elements and a body-less Curve.
		DcrTestPrivate::CheckDump(*this, TEXT("variables and hierarchy"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Variables = {\n"
				"        Array<RigElementKey> Chain [ Public ];\n"
				"        Map<Name, Array<double>> M;\n"
				"        `Module Settings` S;\n"
				"    }\n"
				"    Hierarchy = {\n"
				"        Control a { ShapeName = \"X\"; Control b { ControlType = Bool; } }\n"
				"        Curve c;\n"
				"    }\n"
				"}\n"),
			TEXT(
				"Document ControlRig\n"
				"  Arg Name\n"
				"    String \"X\"\n"
				"  Block\n"
				"    Assign Variables\n"
				"      Block\n"
				"        Decl Array<RigElementKey> Chain\n"
				"          Meta\n"
				"            MetaEntry Public\n"
				"        Decl Map<Name,Array<double>> M\n"
				"        Decl `Module Settings` S\n"
				"    Assign Hierarchy\n"
				"      Block\n"
				"        NamedBlock Control a\n"
				"          Block\n"
				"            Assign ShapeName\n"
				"              String \"X\"\n"
				"            NamedBlock Control b\n"
				"              Block\n"
				"                Assign ControlType\n"
				"                  Name Bool\n"
				"        NamedBlock Curve c\n"));

		// A `[` after an expression in code is an index, a lone flag included.
		DcrTestPrivate::CheckDump(*this, TEXT("index in code"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Event E\n"
				"    {\n"
				"        Chain[2] = Items[ Public ];\n"
				"    }\n"
				"}\n"),
			TEXT(
				"Document ControlRig\n"
				"  Arg Name\n"
				"    String \"X\"\n"
				"  Block\n"
				"    NamedBlock Event E\n"
				"      Code\n"
				"        Set\n"
				"          Index\n"
				"            Name Chain\n"
				"            Number 2\n"
				"          Index\n"
				"            Name Items\n"
				"            Name Public\n"));

		// Event is a block word in a ControlRig document and a type name anywhere else.
		DcrTestPrivate::CheckDump(*this, TEXT("Event in a ControlRig"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Event Forward;\n"
				"}\n"),
			TEXT(
				"Document ControlRig\n"
				"  Arg Name\n"
				"    String \"X\"\n"
				"  Block\n"
				"    NamedBlock Event Forward\n"));
		DcrTestPrivate::CheckDump(*this, TEXT("Event in an AnimBlueprint"),
			TEXT(
				"AnimBlueprint(Name=\"X\")\n"
				"{\n"
				"    Event Forward;\n"
				"}\n"),
			TEXT(
				"Document AnimBlueprint\n"
				"  Arg Name\n"
				"    String \"X\"\n"
				"  Block\n"
				"    Decl Event Forward\n"));

		// `%` and `true` in backticks in a .dab expression: the lexer-level additions work everywhere.
		{
			const FDabSourceText Source(TEXT("e"), FString(TEXT("a % b * c")));
			FDabDiagnosticSink Sink(Source.GetPath());
			const FDabExprPtr Expr = ParseDabExpression(Source, Sink);
			TestFalse(TEXT("no errors"), Sink.HasErrors());
			const FDabBinaryExpr* Multiply = Expr ? Expr->As<FDabBinaryExpr>() : nullptr;
			TestTrue(TEXT("% groups with * from the left"), Multiply && Multiply->Op == EDabBinaryOp::Multiply);
			const FDabBinaryExpr* Modulo = Multiply ? Multiply->Lhs->As<FDabBinaryExpr>() : nullptr;
			TestTrue(TEXT("a % b is the left operand"), Modulo && Modulo->Op == EDabBinaryOp::Modulo);
			TestEqual(TEXT("% binds like *"), GetDabBinaryOpPrecedence(EDabBinaryOp::Modulo), GetDabBinaryOpPrecedence(EDabBinaryOp::Multiply));
			TestEqual(TEXT("spelling"), FString(GetDabBinaryOpSpelling(EDabBinaryOp::Modulo)), FString(TEXT("%")));
			TestEqual(TEXT("document kind name"), FString(LexToString(EDabDocumentKind::ControlRig)), FString(TEXT("ControlRig")));
		}
		{
			const FDabSourceText Source(TEXT("e"), FString(TEXT("`true` && true")));
			FDabDiagnosticSink Sink(Source.GetPath());
			const FDabExprPtr Expr = ParseDabExpression(Source, Sink);
			const FDabBinaryExpr* And = Expr ? Expr->As<FDabBinaryExpr>() : nullptr;
			TestTrue(TEXT("a name in backticks is never a bool"), And && And->Lhs->Is<FDabNameExpr>() && And->Rhs->Is<FDabLiteralExpr>());
		}
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Printer
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrFormatTest, "DreamAnim.Lang.Dcr.Format",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDcrFormatTest::RunTest(const FString& Parameters)
	{
		// Code words and names that are not identifiers go in backticks; calls, `if`/`else` and `for` take their layout.
		DcrTestPrivate::CheckFormat(*this, TEXT("names and statements"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Event   Forward{\n"
				"        `in`=1;let   `My Var`=  `return`( `a b` = 2 , c=3 );\n"
				"        if(x){y=1;}else{ `else`=2; }\n"
				"        for(  `for`  in  Items ){  }\n"
				"    }\n"
				"}\n"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Event Forward\n"
				"    {\n"
				"        `in` = 1;\n"
				"        let `My Var` = `return`(`a b` = 2, c = 3);\n"
				"        if (x)\n"
				"        {\n"
				"            y = 1;\n"
				"        }\n"
				"        else\n"
				"        {\n"
				"            `else` = 2;\n"
				"        }\n"
				"        for (`for` in Items)\n"
				"        {\n"
				"        }\n"
				"    }\n"
				"}\n"));

		// `=` lines up over a run of lets and over a run of assignments; a blank line or another kind ends a run.
		DcrTestPrivate::CheckFormat(*this, TEXT("alignment"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"Event E\n"
				"{\n"
				"let A=1;\n"
				"let LongName=2;\n"
				"B=3;\n"
				"Cc=4;\n"
				"\n"
				"D=5;\n"
				"EeEe=6;\n"
				"}\n"
				"}\n"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Event E\n"
				"    {\n"
				"        let A        = 1;\n"
				"        let LongName = 2;\n"
				"        B  = 3;\n"
				"        Cc = 4;\n"
				"\n"
				"        D    = 5;\n"
				"        EeEe = 6;\n"
				"    }\n"
				"}\n"));

		// Blank lines collapse to one, comments keep their place, the last one stays inside the block.
		DcrTestPrivate::CheckFormat(*this, TEXT("blank lines and comments"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Event E\n"
				"    {\n"
				"\n"
				"\n"
				"        // above\n"
				"        A = 1; // after\n"
				"\n"
				"\n"
				"        B = 2;\n"
				"        /* tail */\n"
				"    }\n"
				"}\n"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Event E\n"
				"    {\n"
				"        // above\n"
				"        A = 1; // after\n"
				"\n"
				"        B = 2;\n"
				"        /* tail */\n"
				"    }\n"
				"}\n"));

		// A call that does not fit breaks one argument per line with the `=` lined up.
		DcrTestPrivate::CheckFormat(*this, TEXT("wide call"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Event E { ModifyTransforms(ItemToModify = [ (Item = Bone(IK_foot_L), Transform = (Translation = (Z = ZOffset_L))) ], Mode = AdditiveGlobal); }\n"
				"}\n"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Event E\n"
				"    {\n"
				"        ModifyTransforms(\n"
				"            ItemToModify = [ (Item = Bone(IK_foot_L), Transform = (Translation = (Z = ZOffset_L))) ],\n"
				"            Mode         = AdditiveGlobal\n"
				"        );\n"
				"    }\n"
				"}\n"));

		// A function head: typed parameters with defaults, outputs, metadata; the type is stored without spaces.
		DcrTestPrivate::CheckFormat(*this, TEXT("function head"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"Function  F( Array<double>Values , double  `Local Scale`=0 )->( double  Out )[ Public ]\n"
				"{\n"
				"return(Out=1);\n"
				"}\n"
				"}\n"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Function F(Array<double> Values, double `Local Scale` = 0) -> (double Out) [ Public ]\n"
				"    {\n"
				"        return (Out = 1);\n"
				"    }\n"
				"}\n"));

		// Generic types align by their printed width.
		DcrTestPrivate::CheckFormat(*this, TEXT("generic declarations"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Variables = { Array < Array < double > > T ; TArray< RigElementKey , double> U; }\n"
				"}\n"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Variables = {\n"
				"        Array<Array<double>>         T;\n"
				"        TArray<RigElementKey,double> U;\n"
				"    }\n"
				"}\n"));

		// `%`, an index, parentheses and a bool-looking name.
		DcrTestPrivate::CheckFormat(*this, TEXT("operators"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Event E { A = (B + 1) % 3 + C[D + 1] * `true`; }\n"
				"}\n"),
			TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Event E\n"
				"    {\n"
				"        A = (B + 1) % 3 + C[D + 1] * `true`;\n"
				"    }\n"
				"}\n"));

		// A name in backticks is accepted in a .dab and printed back.
		DcrTestPrivate::CheckFormat(*this, TEXT("backticks in a .dab"),
			TEXT(
				"AnimBlueprint(Name=\"X\")\n"
				"{\n"
				"    Graph = { `My Pose`=Foo(); }\n"
				"}\n"),
			TEXT(
				"AnimBlueprint(Name=\"X\")\n"
				"{\n"
				"    Graph = { `My Pose` = Foo(); }\n"
				"}\n"));

		// QuoteDabName and its rule.
		TestEqual(TEXT("plain"), QuoteDabName(TEXT("Forward")), FString(TEXT("Forward")));
		TestEqual(TEXT("space"), QuoteDabName(TEXT("Module Settings")), FString(TEXT("`Module Settings`")));
		TestEqual(TEXT("digit first"), QuoteDabName(TEXT("1a")), FString(TEXT("`1a`")));
		TestEqual(TEXT("dash"), QuoteDabName(TEXT("a-b")), FString(TEXT("`a-b`")));
		TestEqual(TEXT("a code word outside code"), QuoteDabName(TEXT("in"), false), FString(TEXT("in")));
		TestEqual(TEXT("a code word inside code"), QuoteDabName(TEXT("in"), true), FString(TEXT("`in`")));
		TestEqual(TEXT("case matters"), QuoteDabName(TEXT("Return"), true), FString(TEXT("Return")));
		TestEqual(TEXT("empty stays empty"), QuoteDabName(FString(), true), FString());
		TestTrue(TEXT("a non-ASCII letter is a letter"), IsDabPlainIdentifier(FString::Chr(static_cast<TCHAR>(0x7A7A))));
		TestTrue(TEXT("Detached is a code word"), IsDabCodeWord(TEXT("Detached")));
		TestFalse(TEXT("Event is not"), IsDabCodeWord(TEXT("Event")));
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Diagnostics
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrErrorsTest, "DreamAnim.Lang.Dcr.Errors",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDcrErrorsTest::RunTest(const FString& Parameters)
	{
		using DcrTestPrivate::ParseExpecting;

		// A name in backticks that never closes: DAB1007 at the backtick, and nothing more for the name that follows it.
		ParseExpecting(*this, TEXT("unclosed backtick"),
			TEXT(
				"ControlRig(Name=\"E\", Root=\"Game\")\n"
				"{\n"
				"    Event `Forward\n"
				"    {\n"
				"    }\n"
				"}\n"),
			{ { TEXT("DAB1007"), 3, 11 } });

		ParseExpecting(*this, TEXT("empty backtick"),
			TEXT(
				"ControlRig(Name=\"E\", Root=\"Game\")\n"
				"{\n"
				"    Event `` \n"
				"    {\n"
				"    }\n"
				"}\n"),
			{ { TEXT("DAB1007"), 3, 11 } });

		// Metadata after a code statement: DAB2015 at the `[`, once.
		ParseExpecting(*this, TEXT("metadata after a code statement"),
			TEXT(
				"ControlRig(Name=\"E\", Root=\"Game\")\n"
				"{\n"
				"    Event Forward\n"
				"    {\n"
				"        Speed = 1 [ Category = \"A\" ];\n"
				"    }\n"
				"}\n"),
			{ { TEXT("DAB2015"), 5, 19 } });

		// An else with no if: DAB2016 at the word; the block after it is parsed quietly.
		ParseExpecting(*this, TEXT("else without if"),
			TEXT(
				"ControlRig(Name=\"E\", Root=\"Game\")\n"
				"{\n"
				"    Event Forward\n"
				"    {\n"
				"        else\n"
				"        {\n"
				"        }\n"
				"    }\n"
				"}\n"),
			{ { TEXT("DAB2016"), 5, 9 } });

		// Event is not a block word in an AnimBlueprint: it reads as a declaration `Event Forward`, whose `{` is unexpected.
		ParseExpecting(*this, TEXT("Event in an AnimBlueprint"),
			TEXT(
				"AnimBlueprint(Name=\"E\", Root=\"Game\")\n"
				"{\n"
				"    Event Forward\n"
				"    {\n"
				"    }\n"
				"}\n"),
			{ { TEXT("DAB2001"), 4, 5 }, { TEXT("DAB2010"), 4, 5 } });

		// A for loop without `in`, with too many names.
		ParseExpecting(*this, TEXT("for without in"),
			TEXT(
				"ControlRig(Name=\"E\", Root=\"Game\")\n"
				"{\n"
				"    Event Forward\n"
				"    {\n"
				"        for (Control Controls)\n"
				"        {\n"
				"        }\n"
				"    }\n"
				"}\n"),
			{ { TEXT("DAB2017"), 5, 22 } });

		ParseExpecting(*this, TEXT("for with five names"),
			TEXT(
				"ControlRig(Name=\"E\", Root=\"Game\")\n"
				"{\n"
				"    Event Forward\n"
				"    {\n"
				"        for (A, B, C, D, E in Items)\n"
				"        {\n"
				"        }\n"
				"    }\n"
				"}\n"),
			{ { TEXT("DAB2017"), 5, 26 } });

		// A return gives its outputs by name.
		ParseExpecting(*this, TEXT("return without a name"),
			TEXT(
				"ControlRig(Name=\"E\", Root=\"Game\")\n"
				"{\n"
				"    Function F() -> (double R)\n"
				"    {\n"
				"        return (1);\n"
				"    }\n"
				"}\n"),
			{ { TEXT("DAB2018"), 5, 16 } });

		// A typed let needs its `=`.
		ParseExpecting(*this, TEXT("typed let without a value"),
			TEXT(
				"ControlRig(Name=\"E\", Root=\"Game\")\n"
				"{\n"
				"    Event Forward\n"
				"    {\n"
				"        float Scale;\n"
				"    }\n"
				"}\n"),
			{ { TEXT("DAB2001"), 5, 20 } });

		// Generic types are a Control Rig thing: elsewhere `Array<double> A;` is an expression, and not a statement.
		ParseExpecting(*this, TEXT("generic type in an AnimBlueprint"),
			TEXT(
				"AnimBlueprint(Name=\"E\", Root=\"Game\")\n"
				"{\n"
				"    Array<double> A;\n"
				"}\n"),
			{ { TEXT("DAB2010"), 3, 5 } });
		return true;
	}

	// ------------------------------------------------------------------------------------------
	// Templates
	// ------------------------------------------------------------------------------------------

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrTemplatesTest, "DreamAnim.Lang.Dcr.Templates",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDcrTemplatesTest::RunTest(const FString& Parameters)
	{
		const FString Text = TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Template Twice(X) = X * 2;\n"
				"\n"
				"    Function F(double A = Twice(3)) -> (double R)\n"
				"    {\n"
				"        let B = Twice(A);\n"
				"        if (Twice(B) > 1)\n"
				"        {\n"
				"            B = Twice(Twice(B));\n"
				"        }\n"
				"        for (I in Twice(Items))\n"
				"        {\n"
				"        }\n"
				"        return (R = Twice(B));\n"
				"    }\n"
				"}\n");

		FDabCompilationUnit Unit;
		FDabDiagnosticSink Sink(TEXT("t.dcr"));
		Unit.MainSource = FDabSourceText(TEXT("t.dcr"), Text);
		Unit.Main = ParseDab(Unit.MainSource, Sink);
		TestFalse(TEXT("the source parses"), Sink.HasErrors());
		TestTrue(TEXT("the templates expand"), ExpandDabTemplates(Unit, Sink));
		TestFalse(TEXT("without errors"), Sink.HasErrors());
		if (!Unit.Main || !Unit.Main->Document || !Unit.Main->Document->Body)
		{
			return false;
		}

		// Calls in a parameter default, a let, a condition, an iterable, an assignment and a return all expand.
		const FString Difference = DcrTestPrivate::DescribeDifference(TEXT(
				"ControlRig(Name=\"X\")\n"
				"{\n"
				"    Template Twice(X) = X * 2;\n"
				"\n"
				"    Function F(double A = 3 * 2) -> (double R)\n"
				"    {\n"
				"        let B = A * 2;\n"
				"        if (B * 2 > 1)\n"
				"        {\n"
				"            B = B * 2 * 2;\n"
				"        }\n"
				"        for (I in Items * 2)\n"
				"        {\n"
				"        }\n"
				"        return (R = B * 2);\n"
				"    }\n"
				"}\n"), PrintDab(*Unit.Main));
		if (!Difference.IsEmpty())
		{
			AddError(FString::Printf(TEXT("The expanded text differs (%s)."), *Difference));
		}

		// The expansion in the let is recorded with the template it came from.
		const TArray<FDabStmtPtr>& Statements = Unit.Main->Document->Body->Statements;
		const FDabNamedBlockStmt* Function = Statements.Num() > 1 ? Statements[1]->As<FDabNamedBlockStmt>() : nullptr;
		const FDabLetStmt* Let = (Function && Function->Code && Function->Code->Statements.Num() > 0)
			? Function->Code->Statements[0]->As<FDabLetStmt>() : nullptr;
		TestTrue(TEXT("the function has a let"), Let && Let->Value);
		if (Let && Let->Value)
		{
			const FDabExpansionOrigin* Origin = Unit.Main->Origins.Find(Let->Value.Get());
			TestTrue(TEXT("the let's value has an origin"), Origin != nullptr);
			if (Origin)
			{
				TestEqual(TEXT("which names the template"), Origin->TemplateName, FString(TEXT("Twice")));
			}
		}
		return true;
	}
}
