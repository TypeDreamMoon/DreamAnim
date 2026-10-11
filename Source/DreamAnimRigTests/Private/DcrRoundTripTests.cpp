// Copyright DreamMoon. All Rights Reserved.
//
// Every rig of the corpus, through the text and back:
//
//   rig --decompile--> text --parse, semantic, generate--> rebuilt rig --decompile--> text again
//
// The second text must be the first (a fixed point): the rebuilt rig is the rig the text says. Its code must
// be the same line for line; its comments, which the rebuilt rig holds as boxes around the nodes the layout put
// there, may land elsewhere (a warning). Rigs that run on their own are also run side by side, the rebuilt one
// against a recompiled copy of the original (an original and its recompiled copy already differ by about 1e-5,
// so the copy is the baseline), and must match to 1e-4; bit for bit is reported. A rig holding something the
// text cannot say does not come back whole and is only decompiled.

#include "DcrCorpus.h"
#include "DcrRunHarness.h"
#include "Lang/DabParser.h"
#include "Lang/DabPrinter.h"
#include "Lang/DabTemplates.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rig/DcrAsset.h"
#include "Rig/DcrExtract.h"
#include "Rig/DcrGenerate.h"
#include "Rig/DcrLower.h"
#include "Rig/DcrNormalize.h"
#include "Rig/DcrSemantic.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Rig::Tests
{
	namespace RoundTripPrivate
	{
		const TCHAR* GroupName(EDcrCorpusGroup Group)
		{
			switch (Group)
			{
			case EDcrCorpusGroup::Standalone: return TEXT("Standalone");
			case EDcrCorpusGroup::Module: return TEXT("Module");
			case EDcrCorpusGroup::Library: return TEXT("Library");
			case EDcrCorpusGroup::Independent: return TEXT("Independent");
			}
			return TEXT("Other");
		}

		const FDcrCorpusEntry* FindEntry(const FString& ObjectPath)
		{
			for (const FDcrCorpusEntry& Entry : GetDcrCorpus())
			{
				if (ObjectPath == Entry.ObjectPath)
				{
					return &Entry;
				}
			}
			return nullptr;
		}

		/** A rig as text; the model's asset path is set to WrittenAs first, so the header names the same asset either way. */
		bool Decompile(const FDcrRig& Rig, const FString& WrittenAs, FString& OutText, Model::FDcrModel& OutModel, FString& OutError, int32* OutUnsupported = nullptr)
		{
			const FDcrExtractResult Extracted = ExtractDcrModel(Rig, OutModel);
			if (!Extracted.bSuccess)
			{
				OutError = Extracted.Error;
				return false;
			}
			NormalizeDcrModel(OutModel);
			OutModel.AssetPath = WrittenAs;
			const FDcrLowerResult Lowered = LowerDcrModel(OutModel);
			OutText = Lang::PrintDab(*Lowered.File);
			if (OutUnsupported)
			{
				*OutUnsupported = Lowered.Unsupported.Num();
			}
			return true;
		}

		/** The code of a text: its comment and region lines left out, and runs of spaces (alignment) as one. */
		FString CodeOf(const FString& Text)
		{
			TArray<FString> Lines;
			Text.ParseIntoArray(Lines, TEXT("\n"), false);
			FString Code;
			bool bInBlockComment = false;
			for (const FString& Line : Lines)
			{
				const FString Trimmed = Line.TrimStartAndEnd();
				if (bInBlockComment || Trimmed.StartsWith(TEXT("/*")))
				{
					bInBlockComment = !Trimmed.EndsWith(TEXT("*/"));
					continue;
				}
				if (Trimmed.StartsWith(TEXT("//")) || Trimmed.StartsWith(TEXT("#Region")) || Trimmed.StartsWith(TEXT("#EndRegion")))
				{
					continue;
				}
				FString Collapsed;
				bool bSpace = false;
				for (const TCHAR Char : Trimmed)
				{
					const bool bIsSpace = Char == TEXT(' ');
					if (!bIsSpace || !bSpace)
					{
						Collapsed.AppendChar(Char);
					}
					bSpace = bIsSpace;
				}
				Code += Collapsed + TEXT("\n");
			}
			return Code;
		}

		int32 CountFloatMath(const Model::FDcrModelGraph& Graph)
		{
			int32 Count = 0;
			for (const Model::FDcrModelNode& Node : Graph.Nodes)
			{
				Count += Node.StructPath.Contains(TEXT("RigVMFunction_MathFloat")) ? 1 : 0;
				if (Node.Contained)
				{
					Count += CountFloatMath(*Node.Contained);
				}
			}
			return Count;
		}

		int32 CountFloatMath(const Model::FDcrModel& Model)
		{
			int32 Count = 0;
			for (const Model::FDcrModelGraph& Graph : Model.Graphs)
			{
				Count += CountFloatMath(Graph);
			}
			for (const Model::FDcrModelFunction& Function : Model.Functions)
			{
				Count += CountFloatMath(Function.Graph);
			}
			return Count;
		}

		FString FirstDifference(const FString& A, const FString& B)
		{
			TArray<FString> LinesA;
			TArray<FString> LinesB;
			A.ParseIntoArray(LinesA, TEXT("\n"), false);
			B.ParseIntoArray(LinesB, TEXT("\n"), false);
			for (int32 Index = 0; Index < FMath::Max(LinesA.Num(), LinesB.Num()); ++Index)
			{
				const FString LineA = LinesA.IsValidIndex(Index) ? LinesA[Index] : FString(TEXT("<end>"));
				const FString LineB = LinesB.IsValidIndex(Index) ? LinesB[Index] : FString(TEXT("<end>"));
				if (LineA != LineB)
				{
					return FString::Printf(TEXT("line %d:\n  first:  %s\n  second: %s"), Index + 1, *LineA.TrimStart(), *LineB.TrimStart());
				}
			}
			return FString();
		}
	}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDcrRoundTripTest, "DreamAnim.Rig.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FDcrRoundTripTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	using namespace UE::DreamAnim::Rig::Tests;
	for (const FDcrCorpusEntry& Entry : GetDcrCorpus())
	{
		OutBeautifiedNames.Add(FString::Printf(TEXT("%s.%s"), RoundTripPrivate::GroupName(Entry.Group), Entry.Name));
		OutTestCommands.Add(Entry.ObjectPath);
	}
}

bool FDcrRoundTripTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim;
	using namespace UE::DreamAnim::Rig;
	using namespace UE::DreamAnim::Rig::Tests;
	using namespace UE::DreamAnim::Rig::Tests::RoundTripPrivate;

	const FDcrCorpusEntry* Entry = FindEntry(Parameters);
	const FDcrRig Original = LoadDcrRig(Parameters);
	if (!Entry || !Original.IsValid())
	{
		AddError(FString::Printf(TEXT("Cannot load %s."), *Parameters));
		return false;
	}

	// The text of the original.
	FString FirstText;
	Model::FDcrModel FirstModel;
	FString Error;
	int32 Unsupported = 0;
	if (!Decompile(Original, Parameters, FirstText, FirstModel, Error, &Unsupported))
	{
		AddError(FString::Printf(TEXT("Decompiling the original failed: %s"), *Error));
		return false;
	}
	if (Unsupported > 0)
	{
		AddWarning(FString::Printf(TEXT("The rig holds %d things the text cannot say; it is not rebuilt."), Unsupported));
		return true;
	}

	// Back to a model, into a rig of the same kind under the test folder.
	const FString Folder = TEXT("/Game/DreamAnimRigTests/RoundTrip/Text");
	const FString RebuiltPath = FString::Printf(TEXT("%s/%s.%s"), *Folder, Entry->Name, Entry->Name);
	Lang::FDabCompilationUnit Unit;
	Unit.MainSource = Lang::FDabSourceText(FString(Entry->Name) + TEXT(".dcr"), FirstText);
	Lang::FDabDiagnosticSink Sink(Unit.MainSource.GetPath());
	Unit.Main = Lang::ParseDab(Unit.MainSource, Sink);
	Lang::ExpandDabTemplates(Unit, Sink);
	Model::FDcrModel Model;
	FDcrSemanticOptions SemanticOptions;
	SemanticOptions.AssetPath = RebuiltPath;
	BuildDcrModel(Unit, Model, Sink, SemanticOptions);
	int32 Errors = 0;
	for (const Lang::FDabDiagnostic& Diagnostic : Sink.GetDiagnostics())
	{
		if (Diagnostic.Severity == Lang::EDabSeverity::Error && Errors++ < 20)
		{
			AddError(FString::Printf(TEXT("(%d,%d) %s: %s"), Diagnostic.Span.Line, Diagnostic.Span.Column, *Diagnostic.Code, *Diagnostic.Message));
		}
	}
	if (Errors > 0)
	{
		AddError(FString::Printf(TEXT("%d error(s) reading the text back."), Errors));
		return false;
	}
	Model.AssetKind = FirstModel.AssetKind;
	const FDcrGenerateResult Generated = GenerateDcrRig(Model);
	for (const FString& GenerateError : Generated.Errors)
	{
		AddError(TEXT("generate: ") + GenerateError);
	}
	if (!Generated.Rig.IsValid())
	{
		return false;
	}

	// The text again: the same.
	FString SecondText;
	Model::FDcrModel SecondModel;
	if (!Decompile(Generated.Rig, Parameters, SecondText, SecondModel, Error))
	{
		AddError(FString::Printf(TEXT("Decompiling the rebuilt rig failed: %s"), *Error));
		return false;
	}
	if (SecondText != FirstText)
	{
		const FString OutDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamAnimTests"), TEXT("RoundTrip"));
		FFileHelper::SaveStringToFile(FirstText, *FPaths::Combine(OutDir, FString(Entry->Name) + TEXT(".first.dcr")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FFileHelper::SaveStringToFile(SecondText, *FPaths::Combine(OutDir, FString(Entry->Name) + TEXT(".second.dcr")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		const FString FirstCode = CodeOf(FirstText);
		const FString SecondCode = CodeOf(SecondText);
		if (FirstCode == SecondCode)
		{
			AddWarning(FString::Printf(TEXT("The rebuilt rig's code is the same; its comments differ at %s"), *FirstDifference(FirstText, SecondText)));
		}
		else
		{
			AddError(FString::Printf(TEXT("The rebuilt rig's code differs at %s"), *FirstDifference(FirstCode, SecondCode)));
		}
	}
	else
	{
		AddInfo(TEXT("The rebuilt rig's text is the same, comments included."));
	}

	// Run side by side: rigs that run on their own.
	if (Entry->Group == EDcrCorpusGroup::Standalone)
	{
		const FDcrRig Baseline = DuplicateDcrRig(Original, TEXT("/Game/DreamAnimRigTests/RoundTrip/Baseline"), Entry->Name);
		if (!Baseline.IsValid())
		{
			AddError(TEXT("The baseline copy could not be made."));
			return false;
		}
		CompileDcrRig(Baseline);
		const FDcrRun Expected = RunDcrRig(Baseline, 60, &DriveDcrDefault);
		const FDcrRun Actual = RunDcrRig(Generated.Rig, 60, &DriveDcrDefault);
		if (!Expected.Error.IsEmpty() || !Actual.Error.IsEmpty())
		{
			AddError(FString::Printf(TEXT("Running failed: %s %s"), *Expected.Error, *Actual.Error));
			return false;
		}
		const FDcrRunDiff Diff = CompareDcrRuns(Expected, Actual);
		const bool bRewritten = CountFloatMath(Model) < CountFloatMath(FirstModel);
		if (Diff.IsExact())
		{
			AddInfo(TEXT("Runs match bit for bit over 60 frames."));
		}
		else if (Diff.IsWithin(1e-4))
		{
			AddInfo(FString::Printf(TEXT("Runs match within 1e-4 over 60 frames%s: %s"), bRewritten ? TEXT(" (float math rebuilt as double)") : TEXT(""), *Diff.Describe()));
		}
		else
		{
			AddError(FString::Printf(TEXT("Runs differ past 1e-4: %s"), *Diff.Describe()));
		}
	}
	return true;
}

#endif
