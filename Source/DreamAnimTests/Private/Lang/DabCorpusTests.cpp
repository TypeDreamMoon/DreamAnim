// Copyright DreamMoon. All Rights Reserved.
//
// Corpus tests for the language front end. Every file under Tests/Corpus is a test case:
//
//   Parse/      each file is parsed; `// EXPECT DABnnnn (line,col)` lines list the diagnostics it must
//               raise, exactly. A file that parses clean is compared with its `.tree` golden.
//   Format/     each file is printed and compared with its `.formatted` golden.
//   Templates/  each .dab is parsed, its #includes resolved and its templates expanded; EXPECT lines as
//               above, and the expanded tree is compared with its `.expanded.tree` golden.
//
// Every clean file in Parse/ and Format/ is also printed twice, to check that the printer keeps the
// tree and that formatting is idempotent.
//
// A golden that is missing or different is written next to the case as `<golden>.actual` and the test
// fails; review the .actual and rename it to promote it.

#include "Interfaces/IPluginManager.h"
#include "Lang/DabParser.h"
#include "Lang/DabPrinter.h"
#include "Lang/DabTemplates.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::DreamAnim::Tests
{
	using namespace UE::DreamAnim::Lang;

	namespace
	{
		FString GetCorpusDir()
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamAnim"));
			return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Tests"), TEXT("Corpus")) : FString();
		}

		void FindCases(const FString& SubDir, const TArray<FString>& Extensions, TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands)
		{
			const FString Dir = FPaths::Combine(GetCorpusDir(), SubDir);
			TArray<FString> Files;
			for (const FString& Extension : Extensions)
			{
				TArray<FString> Found;
				IFileManager::Get().FindFiles(Found, *FPaths::Combine(Dir, TEXT("*") + Extension), true, false);
				Files.Append(Found);
			}
			Files.Sort();
			for (const FString& File : Files)
			{
				OutBeautifiedNames.Add(FPaths::GetBaseFilename(File));
				OutTestCommands.Add(FPaths::Combine(Dir, File));
			}
		}

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
				const FString E = ExpectedLines.IsValidIndex(Index) ? ExpectedLines[Index] : TEXT("<end>");
				const FString A = ActualLines.IsValidIndex(Index) ? ActualLines[Index] : TEXT("<end>");
				if (!E.Equals(A, ESearchCase::CaseSensitive))
				{
					return FString::Printf(TEXT("line %d: expected `%s`, got `%s`"), Index + 1, *E, *A);
				}
			}
			return FString();
		}

		bool LoadText(const FString& Path, FString& OutText)
		{
			return FFileHelper::LoadFileToString(OutText, *Path);
		}

		void WriteActual(const FString& GoldenPath, const FString& Text)
		{
			FFileHelper::SaveStringToFile(Text, *(GoldenPath + TEXT(".actual")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		/** Compares Actual with the golden at GoldenPath, writing `.actual` on a miss. True when equal. */
		bool CheckGolden(FAutomationTestBase& Test, const FString& GoldenPath, const FString& Actual)
		{
			FString Golden;
			if (!LoadText(GoldenPath, Golden))
			{
				WriteActual(GoldenPath, Actual);
				Test.AddError(FString::Printf(TEXT("No golden %s; wrote %s.actual for review."), *FPaths::GetCleanFilename(GoldenPath), *FPaths::GetCleanFilename(GoldenPath)));
				return false;
			}
			const FString Difference = DescribeDifference(Golden, Actual);
			if (!Difference.IsEmpty())
			{
				WriteActual(GoldenPath, Actual);
				Test.AddError(FString::Printf(TEXT("%s differs (%s); wrote %s.actual."), *FPaths::GetCleanFilename(GoldenPath), *Difference, *FPaths::GetCleanFilename(GoldenPath)));
				return false;
			}
			IFileManager::Get().Delete(*(GoldenPath + TEXT(".actual")), false, false, true);
			return true;
		}

		struct FExpectation
		{
			FString Code;
			int32 Line = 0;
			int32 Column = 0;
			bool bHasPosition = false;
		};

		/** `// EXPECT DABnnnn` or `// EXPECT DABnnnn (line,col)`, one per line, anywhere in the file. */
		TArray<FExpectation> ReadExpectations(const FString& Text)
		{
			TArray<FExpectation> Result;
			TArray<FString> Lines;
			NormalizeNewlines(Text).ParseIntoArray(Lines, TEXT("\n"), false);
			for (const FString& RawLine : Lines)
			{
				const int32 At = RawLine.Find(TEXT("// EXPECT "), ESearchCase::CaseSensitive);
				if (At == INDEX_NONE)
				{
					continue;
				}
				FString Rest = RawLine.Mid(At + 10).TrimStartAndEnd();
				FExpectation Expectation;
				int32 Space = INDEX_NONE;
				if (Rest.FindChar(TEXT(' '), Space))
				{
					Expectation.Code = Rest.Left(Space);
					FString Position = Rest.Mid(Space + 1).TrimStartAndEnd();
					FString LineText;
					FString ColumnText;
					if (Position.StartsWith(TEXT("(")) && Position.EndsWith(TEXT(")"))
						&& Position.Mid(1, Position.Len() - 2).Split(TEXT(","), &LineText, &ColumnText))
					{
						Expectation.Line = FCString::Atoi(*LineText.TrimStartAndEnd());
						Expectation.Column = FCString::Atoi(*ColumnText.TrimStartAndEnd());
						Expectation.bHasPosition = true;
					}
				}
				else
				{
					Expectation.Code = Rest;
				}
				Result.Add(Expectation);
			}
			return Result;
		}

		/**
		 * Every expectation must match one diagnostic and every diagnostic one expectation. Diagnostics in
		 * other files (an #included library) match on the code alone.
		 */
		void CheckDiagnostics(FAutomationTestBase& Test, const FString& MainPath, const TArray<FExpectation>& Expected, const FDabDiagnosticSink& Sink)
		{
			TArray<FDabDiagnostic> Unmatched = Sink.GetDiagnostics();
			for (const FExpectation& Expectation : Expected)
			{
				const int32 Index = Unmatched.IndexOfByPredicate([&Expectation, &MainPath](const FDabDiagnostic& Diagnostic)
				{
					if (!Diagnostic.Code.Equals(Expectation.Code, ESearchCase::CaseSensitive))
					{
						return false;
					}
					const bool bOtherFile = !Diagnostic.FilePath.IsEmpty() && !FPaths::IsSamePath(Diagnostic.FilePath, MainPath);
					return !Expectation.bHasPosition || bOtherFile
						|| (Diagnostic.Span.Line == Expectation.Line && Diagnostic.Span.Column == Expectation.Column);
				});
				if (Index == INDEX_NONE)
				{
					Test.AddError(Expectation.bHasPosition
						? FString::Printf(TEXT("Expected %s at (%d,%d), not raised."), *Expectation.Code, Expectation.Line, Expectation.Column)
						: FString::Printf(TEXT("Expected %s, not raised."), *Expectation.Code));
				}
				else
				{
					Unmatched.RemoveAt(Index);
				}
			}
			for (const FDabDiagnostic& Diagnostic : Unmatched)
			{
				Test.AddError(FString::Printf(TEXT("Unexpected %s"), *FDabDiagnosticSink::ToWireString(Diagnostic)));
			}
		}

		/** Prints the file, reparses, and checks the tree survived and a second print changes nothing. */
		void CheckPrintRoundTrip(FAutomationTestBase& Test, const FDabFile& File, FString& OutPrinted)
		{
			OutPrinted = PrintDab(File);
			const FString OriginalTree = DumpDabTree(File);

			FDabSourceText Source(File.Path, OutPrinted);
			FDabDiagnosticSink Sink(File.Path);
			const TUniquePtr<FDabFile> Reparsed = ParseDab(Source, Sink);
			if (Sink.HasErrors())
			{
				Test.AddError(FString::Printf(TEXT("The printed text does not parse: %s"), *FDabDiagnosticSink::ToWireString(*Sink.FirstError())));
				return;
			}
			const FString TreeDifference = DescribeDifference(OriginalTree, DumpDabTree(*Reparsed));
			if (!TreeDifference.IsEmpty())
			{
				Test.AddError(FString::Printf(TEXT("Printing changed the tree (%s)."), *TreeDifference));
			}
			const FString PrintedAgain = PrintDab(*Reparsed);
			const FString FormatDifference = DescribeDifference(OutPrinted, PrintedAgain);
			if (!FormatDifference.IsEmpty())
			{
				Test.AddError(FString::Printf(TEXT("Formatting is not idempotent (%s)."), *FormatDifference));
			}
		}

		bool ParseCase(FAutomationTestBase& Test, const FString& Path, FString& OutText, TUniquePtr<FDabFile>& OutFile, FDabDiagnosticSink& Sink)
		{
			if (!LoadText(Path, OutText))
			{
				Test.AddError(FString::Printf(TEXT("Cannot read %s"), *Path));
				return false;
			}
			const FDabSourceText Source(Path, OutText);
			OutFile = ParseDab(Source, Sink);
			return true;
		}
	}

	IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDabCorpusParseTest, "DreamAnim.Lang.Corpus.Parse",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	void FDabCorpusParseTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
	{
		FindCases(TEXT("Parse"), { TEXT(".dab"), TEXT(".dai"), TEXT(".dat") }, OutBeautifiedNames, OutTestCommands);
	}

	bool FDabCorpusParseTest::RunTest(const FString& Path)
	{
		FString Text;
		TUniquePtr<FDabFile> File;
		FDabDiagnosticSink Sink(Path);
		if (!ParseCase(*this, Path, Text, File, Sink))
		{
			return false;
		}

		const TArray<FExpectation> Expected = ReadExpectations(Text);
		CheckDiagnostics(*this, Path, Expected, Sink);

		if (Expected.IsEmpty() && !Sink.HasErrors())
		{
			CheckGolden(*this, Path + TEXT(".tree"), DumpDabTree(*File));
			FString Printed;
			CheckPrintRoundTrip(*this, *File, Printed);
		}
		return true;
	}

	IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDabCorpusFormatTest, "DreamAnim.Lang.Corpus.Format",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	void FDabCorpusFormatTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
	{
		FindCases(TEXT("Format"), { TEXT(".dab"), TEXT(".dai"), TEXT(".dat") }, OutBeautifiedNames, OutTestCommands);
	}

	bool FDabCorpusFormatTest::RunTest(const FString& Path)
	{
		FString Text;
		TUniquePtr<FDabFile> File;
		FDabDiagnosticSink Sink(Path);
		if (!ParseCase(*this, Path, Text, File, Sink))
		{
			return false;
		}
		if (Sink.HasErrors())
		{
			AddError(FString::Printf(TEXT("A format case must parse clean: %s"), *FDabDiagnosticSink::ToWireString(*Sink.FirstError())));
			return false;
		}

		FString Printed;
		CheckPrintRoundTrip(*this, *File, Printed);
		CheckGolden(*this, FPaths::ChangeExtension(Path, TEXT(".formatted")), Printed);
		return true;
	}

	IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDabCorpusTemplatesTest, "DreamAnim.Lang.Corpus.Templates",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	void FDabCorpusTemplatesTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
	{
		FindCases(TEXT("Templates"), { TEXT(".dab") }, OutBeautifiedNames, OutTestCommands);
	}

	bool FDabCorpusTemplatesTest::RunTest(const FString& Path)
	{
		FDabCompilationUnit Unit;
		FString Text;
		if (!LoadText(Path, Text))
		{
			AddError(FString::Printf(TEXT("Cannot read %s"), *Path));
			return false;
		}
		const TArray<FExpectation> Expected = ReadExpectations(Text);

		FDabDiagnosticSink Sink(Path);
		Unit.MainSource = FDabSourceText(Path, Text);
		Unit.Main = ParseDab(Unit.MainSource, Sink);

		FDabFileSystemIncludeLoader Loader;
		ResolveDabIncludes(Unit, Loader, Sink);
		ExpandDabTemplates(Unit, Sink);

		CheckDiagnostics(*this, Path, Expected, Sink);
		if (Expected.IsEmpty() && !Sink.HasErrors())
		{
			CheckGolden(*this, FPaths::ChangeExtension(Path, TEXT(".expanded.tree")), DumpDabTree(*Unit.Main));
		}
		return true;
	}
}
