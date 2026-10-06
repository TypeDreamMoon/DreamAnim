// Copyright DreamMoon. All Rights Reserved.

#include "Animation/AnimBlueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"

// ALS-Refactored is the test host's corpus of real Animation Blueprints. The tests that decompile
// and regenerate Blueprints assume the originals load and compile cleanly first, so a failure here
// means the corpus is broken, not the code under test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDreamAnimAlsCorpusCompilesTest, "DreamAnim.Corpus.Als.Compiles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDreamAnimAlsCorpusCompilesTest::RunTest(const FString& Parameters)
{
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.SearchAllAssets(true);

	FARFilter Filter;
	Filter.PackagePaths.Add(TEXT("/ALS"));
	Filter.bRecursivePaths = true;
	Filter.ClassPaths.Add(UAnimBlueprint::StaticClass()->GetClassPathName());
	TArray<FAssetData> Assets;
	Registry.GetAssets(Filter, Assets);

	if (Assets.IsEmpty())
	{
		AddError(TEXT("No Animation Blueprints under /ALS. Is ALS-Refactored in the test host's Plugins folder?"));
		return false;
	}

	int32 TotalWarnings = 0;
	for (const FAssetData& Asset : Assets)
	{
		UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(Asset.GetAsset());
		if (!Blueprint)
		{
			AddError(FString::Printf(TEXT("%s did not load"), *Asset.GetObjectPathString()));
			continue;
		}

		FCompilerResultsLog Results;
		Results.bSilentMode = true;
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);

		for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
		{
			if (Message->GetSeverity() == EMessageSeverity::Error)
			{
				AddError(FString::Printf(TEXT("%s: %s"), *Blueprint->GetName(), *Message->ToText().ToString()));
			}
		}
		TotalWarnings += Results.NumWarnings;
		AddInfo(FString::Printf(TEXT("%s: %d error(s), %d warning(s)"), *Blueprint->GetName(), Results.NumErrors, Results.NumWarnings));
	}

	AddInfo(FString::Printf(TEXT("%d Animation Blueprints compiled, %d warning(s) in total"), Assets.Num(), TotalWarnings));
	return true;
}
