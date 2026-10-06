// Copyright DreamMoon. All Rights Reserved.
//
// The ALS corpus run through the whole pipeline, one test case per Blueprint:
//
//   Decompile  extract the model, normalise its names, lower it to text and print it; the text must parse
//              clean and match the golden Tests/Corpus/Decompiled/<Name>.dab (written as .actual on a miss).
//   Semantic   check that text again: the model it gives must dump the same as the extracted one.
//   Generate   generate a new Blueprint from that model next to the host's content, compile it, extract it
//              again: it must compile without errors and dump the same as the original.
//
// The three stages are separate tests so a failure says which half of the pipeline broke.

#include "Animation/AnimBlueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Extract/DabExtract.h"
#include "Generate/DabGenerate.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Lang/DabParser.h"
#include "Lang/DabPrinter.h"
#include "Lang/DabTemplates.h"
#include "Lower/DabLower.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Semantic/DabSemantic.h"

namespace UE::DreamAnim::Tests
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;
	using namespace UE::DreamAnim::Compiler;

	namespace
	{
		void FindAlsBlueprints(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands)
		{
			IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			Registry.SearchAllAssets(true);
			FARFilter Filter;
			Filter.PackagePaths.Add(TEXT("/ALS"));
			Filter.bRecursivePaths = true;
			Filter.ClassPaths.Add(UAnimBlueprint::StaticClass()->GetClassPathName());
			TArray<FAssetData> Assets;
			Registry.GetAssets(Filter, Assets);
			Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
			for (const FAssetData& Asset : Assets)
			{
				// Two Blueprints share a name (the Bow overlay and the bow object), so the package path is the key.
				OutBeautifiedNames.Add(Asset.PackageName.ToString().Replace(TEXT("/ALS/ALS/"), TEXT("")).Replace(TEXT("/"), TEXT("_")));
				OutTestCommands.Add(Asset.GetObjectPathString());
			}
		}

		FString GetDecompiledDir()
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamAnim"));
			return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Tests"), TEXT("Corpus"), TEXT("Decompiled")) : FString();
		}

		FString NormalizeAlsLineEnds(const FString& Text)
		{
			return Text.Replace(TEXT("\r\n"), TEXT("\n")).Replace(TEXT("\r"), TEXT("\n"));
		}

		FString FirstAlsDifference(const FString& Expected, const FString& Actual)
		{
			TArray<FString> E;
			TArray<FString> A;
			NormalizeAlsLineEnds(Expected).ParseIntoArray(E, TEXT("\n"), false);
			NormalizeAlsLineEnds(Actual).ParseIntoArray(A, TEXT("\n"), false);
			for (int32 Index = 0; Index < FMath::Max(E.Num(), A.Num()); ++Index)
			{
				const FString L = E.IsValidIndex(Index) ? E[Index] : TEXT("<end>");
				const FString R = A.IsValidIndex(Index) ? A[Index] : TEXT("<end>");
				if (!L.Equals(R, ESearchCase::CaseSensitive))
				{
					return FString::Printf(TEXT("line %d: expected `%s`, got `%s`"), Index + 1, *L.TrimStart(), *R.TrimStart());
				}
			}
			return FString();
		}

		/** The extracted, name-normalised model of an asset, reporting problems to the test. */
		bool ExtractNormalised(FAutomationTestBase& Test, const FString& ObjectPath, FDabModel& OutModel, TMap<FString, FString>* OutRenames = nullptr)
		{
			UAnimBlueprint* Blueprint = LoadObject<UAnimBlueprint>(nullptr, *ObjectPath);
			if (!Blueprint)
			{
				Test.AddError(FString::Printf(TEXT("Cannot load %s"), *ObjectPath));
				return false;
			}
			FDabDiagnosticSink Sink(ObjectPath);
			if (!ExtractDabModel(*Blueprint, OutModel, Sink))
			{
				Test.AddError(FString::Printf(TEXT("Extraction failed: %s"), Sink.FirstError() ? *FDabDiagnosticSink::ToWireString(*Sink.FirstError()) : TEXT("?")));
				return false;
			}
			for (const FDabDiagnostic& Diagnostic : Sink.GetDiagnostics())
			{
				Test.AddWarning(FDabDiagnosticSink::ToWireString(Diagnostic));
			}
			NormalizeDabModelNames(OutModel, OutRenames);
			return true;
		}

		/** Text -> expanded unit -> model. */
		bool CheckText(FAutomationTestBase& Test, const FString& Path, const FString& Text, FDabModel& OutModel)
		{
			FDabCompilationUnit Unit;
			FDabDiagnosticSink Sink(Path);
			Unit.MainSource = FDabSourceText(Path, Text);
			Unit.Main = ParseDab(Unit.MainSource, Sink);
			FDabFileSystemIncludeLoader Loader;
			ResolveDabIncludes(Unit, Loader, Sink);
			ExpandDabTemplates(Unit, Sink);
			BuildDabModel(Unit, OutModel, Sink);
			for (const FDabDiagnostic& Diagnostic : Sink.GetDiagnostics())
			{
				if (Diagnostic.Severity == EDabSeverity::Error)
				{
					Test.AddError(FDabDiagnosticSink::ToWireString(Diagnostic));
				}
			}
			return !Sink.HasErrors();
		}

		/** Both sides of a failed comparison, for a diff tool: <host>/Saved/DreamAnimTests/Models/<Name>.<Side>.txt. */
		void SaveModelDumps(const FString& ObjectPath, const TCHAR* Stage, const FString& Expected, const FString& Actual)
		{
			const FString Name = FPackageName::GetShortName(FPackageName::ObjectPathToPackageName(ObjectPath));
			const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamAnimTests"), TEXT("Models"));
			FFileHelper::SaveStringToFile(Expected, *FPaths::Combine(Dir, FString::Printf(TEXT("%s.%s.expected.txt"), *Name, Stage)), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			FFileHelper::SaveStringToFile(Actual, *FPaths::Combine(Dir, FString::Printf(TEXT("%s.%s.actual.txt"), *Name, Stage)), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		FString DecompileToText(const FDabModel& Model, const TMap<FString, FString>& Renames = TMap<FString, FString>())
		{
			FDabLowerOptions Options;
			Options.Renames = Renames;
			const TUniquePtr<FDabFile> File = LowerDabModel(Model, Options);
			return PrintDab(*File);
		}
	}

	IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDabAlsDecompileTest, "DreamAnim.Corpus.Als.Decompile",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	void FDabAlsDecompileTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
	{
		FindAlsBlueprints(OutBeautifiedNames, OutTestCommands);
	}

	bool FDabAlsDecompileTest::RunTest(const FString& ObjectPath)
	{
		FDabModel Model;
		TMap<FString, FString> Renames;
		if (!ExtractNormalised(*this, ObjectPath, Model, &Renames))
		{
			return false;
		}
		for (const FDabModelUnsupported& Unsupported : Model.Unsupported)
		{
			AddInfo(FString::Printf(TEXT("Unsupported %s: %s"), *Unsupported.Location, *Unsupported.Description));
		}

		const FString Text = DecompileToText(Model, Renames);
		const FString Name = FPackageName::GetShortName(FPackageName::ObjectPathToPackageName(ObjectPath));
		const FString GoldenName = FPackageName::ObjectPathToPackageName(ObjectPath).Replace(TEXT("/ALS/ALS/"), TEXT("")).Replace(TEXT("/"), TEXT("_"));
		const FString GoldenPath = FPaths::Combine(GetDecompiledDir(), GoldenName + (Model.Kind == EDabDocumentKind::AnimLayerInterface ? TEXT(".dai") : TEXT(".dab")));

		// The text must parse clean before it is worth comparing.
		{
			FDabSourceText Source(GoldenPath, Text);
			FDabDiagnosticSink Sink(GoldenPath);
			ParseDab(Source, Sink);
			if (Sink.HasErrors())
			{
				AddError(FString::Printf(TEXT("%s: decompiled text does not parse: %s"), *Name, *FDabDiagnosticSink::ToWireString(*Sink.FirstError())));
			}
		}

		FString Golden;
		if (!FFileHelper::LoadFileToString(Golden, *GoldenPath))
		{
			FFileHelper::SaveStringToFile(Text, *(GoldenPath + TEXT(".actual")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			AddError(FString::Printf(TEXT("No golden %s; wrote .actual for review."), *FPaths::GetCleanFilename(GoldenPath)));
			return false;
		}
		const FString Difference = FirstAlsDifference(Golden, Text);
		if (!Difference.IsEmpty())
		{
			FFileHelper::SaveStringToFile(Text, *(GoldenPath + TEXT(".actual")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			AddError(FString::Printf(TEXT("%s differs (%s); wrote .actual."), *FPaths::GetCleanFilename(GoldenPath), *Difference));
		}
		return true;
	}

	IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDabAlsSemanticTest, "DreamAnim.Corpus.Als.Semantic",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	void FDabAlsSemanticTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
	{
		FindAlsBlueprints(OutBeautifiedNames, OutTestCommands);
	}

	bool FDabAlsSemanticTest::RunTest(const FString& ObjectPath)
	{
		FDabModel Extracted;
		if (!ExtractNormalised(*this, ObjectPath, Extracted))
		{
			return false;
		}
		const FString Text = DecompileToText(Extracted);
		// What the language cannot say is listed in the text's header comment, not in its model.
		Extracted.Unsupported.Reset();

		FDabModel Checked;
		CheckText(*this, ObjectPath + TEXT(".dab"), Text, Checked);
		const FString ExpectedDump = DumpDabModel(Extracted);
		const FString ActualDump = DumpDabModel(Checked);
		const FString Difference = FirstAlsDifference(ExpectedDump, ActualDump);
		if (!Difference.IsEmpty())
		{
			SaveModelDumps(ObjectPath, TEXT("semantic"), ExpectedDump, ActualDump);
			AddError(FString::Printf(TEXT("The decompiled text checks to a different model (%s)."), *Difference));
		}
		return true;
	}

	IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDabAlsGenerateTest, "DreamAnim.Corpus.Als.Generate",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	void FDabAlsGenerateTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
	{
		FindAlsBlueprints(OutBeautifiedNames, OutTestCommands);
	}

	bool FDabAlsGenerateTest::RunTest(const FString& ObjectPath)
	{
		FDabModel Extracted;
		if (!ExtractNormalised(*this, ObjectPath, Extracted))
		{
			return false;
		}
		FDabModel Checked;
		const FString Text = DecompileToText(Extracted);
		// Losses are known and listed in the text. Whatever the regenerated Blueprint reports as unsupported
		// stays in its model: that would be something the generator made that the extractor cannot read.
		Extracted.Unsupported.Reset();
		if (!CheckText(*this, ObjectPath + TEXT(".dab"), Text, Checked))
		{
			return false;
		}

		FDabGenerateOptions Options;
		Options.bSave = false;
		Options.OverridePackageName = FString::Printf(TEXT("/Game/DreamAnimTests/Als/%s"),
			*FPackageName::ObjectPathToPackageName(ObjectPath).Replace(TEXT("/ALS/ALS/"), TEXT("")).Replace(TEXT("/"), TEXT("_")));
		FDabDiagnosticSink Sink(ObjectPath);
		const FDabGenerateResult Result = GenerateDabBlueprint(Checked, Sink, Options);
		for (const FDabDiagnostic& Diagnostic : Sink.GetDiagnostics())
		{
			AddError(FDabDiagnosticSink::ToWireString(Diagnostic));
		}
		for (const FString& Error : Result.CompilerErrors)
		{
			AddError(FString::Printf(TEXT("Compiler: %s"), *Error));
		}
		if (!Result.Blueprint)
		{
			return false;
		}

		FDabModel Regenerated;
		FDabDiagnosticSink ExtractSink(Options.OverridePackageName);
		ExtractDabModel(*Result.Blueprint, Regenerated, ExtractSink);
		NormalizeDabModelNames(Regenerated);
		// The copy lives elsewhere; compare everything but the asset path.
		Regenerated.AssetPath = Extracted.AssetPath;
		const FString ExpectedDump = DumpDabModel(Extracted);
		const FString ActualDump = DumpDabModel(Regenerated);
		const FString Difference = FirstAlsDifference(ExpectedDump, ActualDump);
		if (!Difference.IsEmpty())
		{
			SaveModelDumps(ObjectPath, TEXT("generate"), ExpectedDump, ActualDump);
			AddError(FString::Printf(TEXT("The generated Blueprint extracts to a different model (%s)."), *Difference));
		}
		return true;
	}
}
