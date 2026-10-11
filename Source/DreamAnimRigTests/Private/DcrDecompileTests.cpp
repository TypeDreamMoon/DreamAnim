// Copyright DreamMoon. All Rights Reserved.
//
// Every rig of the corpus as text: extract, normalize, lower, print. The text must read back without an
// error and print the same again. Each rig's text is written under Saved/DreamAnimTests/Decompile for
// reading, with what it could not say and how long it is reported.

#include "DcrCorpus.h"
#include "HAL/FileManager.h"
#include "Lang/DabParser.h"
#include "Lang/DabPrinter.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rig/DcrAsset.h"
#include "Rig/DcrExtract.h"
#include "Rig/DcrLower.h"
#include "Rig/DcrNormalize.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Rig::Tests
{
	namespace DecompilePrivate
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

		int32 CountNodes(const Model::FDcrModelGraph& Graph)
		{
			int32 Count = 0;
			for (const Model::FDcrModelNode& Node : Graph.Nodes)
			{
				++Count;
				if (Node.Contained)
				{
					Count += CountNodes(*Node.Contained);
				}
			}
			return Count;
		}

		/** The first line two texts differ on, for the failure message. */
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
					return FString::Printf(TEXT("line %d:\n  first:  %s\n  second: %s"), Index + 1, *LineA, *LineB);
				}
			}
			return FString();
		}
	}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDcrDecompileCorpusTest, "DreamAnim.Rig.Decompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FDcrDecompileCorpusTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	using namespace UE::DreamAnim::Rig::Tests;
	for (const FDcrCorpusEntry& Entry : GetDcrCorpus())
	{
		OutBeautifiedNames.Add(FString::Printf(TEXT("%s.%s"), DecompilePrivate::GroupName(Entry.Group), Entry.Name));
		OutTestCommands.Add(Entry.ObjectPath);
	}
}

bool FDcrDecompileCorpusTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim;
	using namespace UE::DreamAnim::Rig;
	using namespace UE::DreamAnim::Rig::Tests;

	const FDcrRig Rig = LoadDcrRig(Parameters);
	if (!Rig.IsValid())
	{
		AddError(FString::Printf(TEXT("Cannot load %s."), *Parameters));
		return false;
	}

	Model::FDcrModel Model;
	const FDcrExtractResult Extracted = ExtractDcrModel(Rig, Model);
	if (!Extracted.bSuccess)
	{
		AddError(FString::Printf(TEXT("Extract failed: %s"), *Extracted.Error));
		return false;
	}
	const FDcrNormalizeReport Normalized = NormalizeDcrModel(Model);

	const double Start = FPlatformTime::Seconds();
	const FDcrLowerResult Lowered = LowerDcrModel(Model);
	const FString Text = Lang::PrintDab(*Lowered.File);
	const double Seconds = FPlatformTime::Seconds() - Start;

	const FString Name = FPackageName::GetShortName(FSoftObjectPath(Parameters).GetLongPackageName());
	const FString OutPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamAnimTests"), TEXT("Decompile"), Name + TEXT(".dcr"));
	FFileHelper::SaveStringToFile(Text, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	// It reads back, and prints the same.
	Lang::FDabSourceText Source(OutPath, Text);
	Lang::FDabDiagnosticSink Sink(OutPath);
	TUniquePtr<Lang::FDabFile> Parsed = Lang::ParseDab(Source, Sink);
	for (const Lang::FDabDiagnostic& Diagnostic : Sink.GetDiagnostics())
	{
		if (Diagnostic.Severity == Lang::EDabSeverity::Error)
		{
			AddError(FString::Printf(TEXT("(%d,%d) %s: %s"), Diagnostic.Span.Line, Diagnostic.Span.Column, *Diagnostic.Code, *Diagnostic.Message));
		}
	}
	if (Parsed)
	{
		const FString Again = Lang::PrintDab(*Parsed);
		if (Again != Text)
		{
			AddError(FString::Printf(TEXT("Printing the text read back changes it at %s"), *DecompilePrivate::FirstDifference(Text, Again)));
		}
	}

	int32 Nodes = 0;
	for (const Model::FDcrModelGraph& Graph : Model.Graphs)
	{
		Nodes += DecompilePrivate::CountNodes(Graph);
	}
	for (const Model::FDcrModelFunction& Function : Model.Functions)
	{
		Nodes += DecompilePrivate::CountNodes(Function.Graph);
	}
	TArray<FString> Lines;
	Text.ParseIntoArray(Lines, TEXT("\n"), false);
	AddInfo(FString::Printf(TEXT("%d lines for %d nodes, %d elements, %d functions; %d unsupported; sequences flattened %d, kept %d; lowered in %.0f ms -> %s"),
		Lines.Num(), Nodes, Model.Hierarchy.Num(), Model.Functions.Num(), Lowered.Unsupported.Num(), Normalized.SequencesFlattened,
		Normalized.SequencesKept, Seconds * 1000.0, *OutPath));
	for (const Model::FDcrModelUnsupported& Unsupported : Lowered.Unsupported)
	{
		AddInfo(FString::Printf(TEXT("unsupported %s: %s"), *Unsupported.Location, *Unsupported.Description));
	}
	return true;
}

#endif
