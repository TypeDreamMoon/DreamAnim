// Copyright DreamMoon. All Rights Reserved.
//
// The ALS corpus run through the whole pipeline, one test case per Blueprint:
//
//   Decompile  extract the model, normalise its names, lower it to text and print it; the text must parse
//              clean and match the golden Tests/Corpus/Decompiled/<Name>.dab (written as .actual on a miss).
//   Semantic   check that text again: the model it gives must dump the same as the extracted one.
//   Generate   generate a new Blueprint from that model next to the host's content, compile it, extract it
//              again: it must compile without errors and dump the same as the original.
//   PoseDiff   one test for the character: the ALS character driven through a fixed script once with the
//              original Blueprints and once with the ones generated from their text, every bone of every
//              frame compared.
//
// The stages are separate tests so a failure says which half of the pipeline broke.

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Corpus/AlsPoseRecorder.h"
#include "GameFramework/Character.h"
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

		/** The Blueprint at ObjectPath, decompiled, checked and generated again under /Game/DreamAnimTests/Als: its class. */
		/** Maps the printed value of a property (`Path(Plugin.ALS, "...")`) to the value to use instead. */
		using FAlsPathMap = TMap<FString, FString>;

		UClass* RegenerateAls(FAutomationTestBase& Test, const FString& ObjectPath, const FAlsPathMap& Retarget = FAlsPathMap(), const TCHAR* PackageSuffix = TEXT(""));

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

	namespace
	{
		/** `/ALS/ALS/X/Y.Y` as the checked model writes it: `Path(Plugin.ALS, "ALS/X/Y")`. */
		FDabExprPtr MakeAlsPathExpr(const FString& ObjectPath)
		{
			const FString Package = FPackageName::ObjectPathToPackageName(ObjectPath);
			FDabDiagnosticSink Sink;
			FDabSourceText Source(TEXT("Path"), FString::Printf(TEXT("Path(Plugin.ALS, \"%s\")"), *Package.RightChop(5)));
			return ParseDabExpression(Source, Sink);
		}

		/** The package a regenerated ALS Blueprint goes to. */
		FString RegeneratedPackage(const FString& ObjectPath, const TCHAR* Suffix = TEXT(""))
		{
			return FString::Printf(TEXT("/Game/DreamAnimTests/Als/%s%s"),
				*FPackageName::ObjectPathToPackageName(ObjectPath).Replace(TEXT("/ALS/ALS/"), TEXT("")).Replace(TEXT("/"), TEXT("_")), Suffix);
		}

		/** Node properties whose printed value is a key of Retarget take the mapped value instead. */
		void RetargetModel(FDabModel& Model, const FAlsPathMap& Retarget)
		{
			const auto Graph = [&Retarget](FDabModelGraph& Target)
			{
				for (FDabModelNode& Node : Target.Nodes)
				{
					for (FDabModelProperty& Property : Node.Properties)
					{
						const FString* Replacement = Property.Value ? Retarget.Find(PrintDabExpr(*Property.Value)) : nullptr;
						if (Replacement)
						{
							FDabDiagnosticSink Sink;
							FDabSourceText Source(TEXT("Retarget"), *Replacement);
							if (FDabExprPtr Parsed = ParseDabExpression(Source, Sink))
							{
								Property.Value = MoveTemp(Parsed);
							}
						}
					}
				}
			};
			Graph(Model.AnimGraph);
			for (FDabModelLayer& Layer : Model.Layers)
			{
				Graph(Layer.Graph);
			}
			for (FDabModelStateMachine& Machine : Model.StateMachines)
			{
				for (FDabModelState& State : Machine.States)
				{
					Graph(State.Graph);
				}
			}
		}

		UClass* RegenerateAls(FAutomationTestBase& Test, const FString& ObjectPath, const FAlsPathMap& Retarget, const TCHAR* PackageSuffix)
		{
			FDabModel Extracted;
			if (!ExtractNormalised(Test, ObjectPath, Extracted))
			{
				return nullptr;
			}
			FDabModel Checked;
			if (!CheckText(Test, ObjectPath + TEXT(".dab"), DecompileToText(Extracted), Checked))
			{
				return nullptr;
			}
			RetargetModel(Checked, Retarget);
			FDabGenerateOptions Options;
			Options.bSave = false;
			Options.OverridePackageName = RegeneratedPackage(ObjectPath, PackageSuffix);
			FDabDiagnosticSink Sink(ObjectPath);
			const FDabGenerateResult Result = GenerateDabBlueprint(Checked, Sink, Options);
			if (!Result.Succeeded() || !Result.Blueprint)
			{
				Test.AddError(FString::Printf(TEXT("%s does not generate"), *ObjectPath));
				return nullptr;
			}
			return Result.Blueprint->GeneratedClass;
		}

		/** The class of an ALS Blueprint by its path under /ALS/ALS/Character/AnimationInstances. */
		FString AlsInstancePath(const TCHAR* Relative)
		{
			const FString Name = FPaths::GetBaseFilename(Relative);
			return FString::Printf(TEXT("/ALS/ALS/Character/%s.%s"), Relative, *Name);
		}
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabAlsPoseDiffTest, "DreamAnim.Corpus.Als.PoseDiff",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDabAlsPoseDiffTest::RunTest(const FString& Parameters)
	{
		// The main instance, the graphs it links by tag, and the overlay layers ALS links as the overlay changes.
		const TCHAR* const MainPath = TEXT("AB_Als");
		const TPair<const TCHAR*, const TCHAR*> Tagged[] = {
			{ TEXT("Grounded"), TEXT("AnimationInstances/AB_Als_Grounded") },
			{ TEXT("Standing"), TEXT("AnimationInstances/Stances/AB_Als_Standing") },
			{ TEXT("Crouching"), TEXT("AnimationInstances/Stances/AB_Als_Crouching") },
			{ TEXT("Locomotion"), TEXT("AnimationInstances/AB_Als_Locomotion") },
			{ TEXT("Layering"), TEXT("AnimationInstances/AB_Als_Layering") },
			{ TEXT("View"), TEXT("AnimationInstances/AB_Als_Head") },
			{ TEXT("Ragdolling"), TEXT("AnimationInstances/AB_Als_Ragdolling") },
		};
		const TCHAR* const Overlays[] = {
			TEXT("Default"), TEXT("Masculine"), TEXT("Feminine"), TEXT("Injured"), TEXT("HandsTied"), TEXT("Rifle"),
			TEXT("PistolOneHanded"), TEXT("PistolTwoHanded"), TEXT("Bow"), TEXT("Torch"), TEXT("Binoculars"), TEXT("Box"), TEXT("Barrel"),
		};

		// The main instance's references to the others, as the checked model writes them, and the generated
		// copies they become in a project made entirely from text.
		FAlsPathMap Retarget;
		const auto AddRetarget = [&Retarget](const FString& ObjectPath)
		{
			const FString Original = PrintDabExpr(*MakeAlsPathExpr(ObjectPath));
			const FString Package = RegeneratedPackage(ObjectPath);
			Retarget.Add(Original, FString::Printf(TEXT("Path(Game, \"%s\")"), *Package.RightChop(6)));
		};

		UClass* GeneratedMain = RegenerateAls(*this, AlsInstancePath(MainPath));
		TMap<FName, UClass*> GeneratedByTag;
		for (const TPair<const TCHAR*, const TCHAR*>& Entry : Tagged)
		{
			if (UClass* Class = RegenerateAls(*this, AlsInstancePath(Entry.Value)))
			{
				GeneratedByTag.Add(FName(Entry.Key), Class);
				AddRetarget(AlsInstancePath(Entry.Value));
			}
		}
		TMap<FString, UClass*> GeneratedOverlays;
		for (const TCHAR* Overlay : Overlays)
		{
			const FString Path = AlsInstancePath(*FString::Printf(TEXT("AnimationInstances/Overlays/AB_Als_%s"), Overlay));
			if (UClass* Class = RegenerateAls(*this, Path))
			{
				GeneratedOverlays.Add(FPackageName::ObjectPathToObjectName(Path) + TEXT("_C"), Class);
				AddRetarget(Path);
			}
		}
		UClass* GeneratedMainLinked = RegenerateAls(*this, AlsInstancePath(MainPath), Retarget, TEXT("_Linked"));
		if (!GeneratedMain || !GeneratedMainLinked || GeneratedByTag.Num() != UE_ARRAY_COUNT(Tagged) || GeneratedOverlays.Num() != UE_ARRAY_COUNT(Overlays))
		{
			return false;
		}

		// The character links an overlay's layers from its OverlayAnimationInstanceClasses map whenever the overlay
		// changes; pointing the map at the generated classes lets it link those exactly as it links the originals.
		const auto UseGeneratedOverlays = [&GeneratedOverlays](ACharacter& Character)
		{
			const FMapProperty* Map = FindFProperty<FMapProperty>(Character.GetClass(), TEXT("OverlayAnimationInstanceClasses"));
			const FObjectPropertyBase* ValueProperty = Map ? CastField<FObjectPropertyBase>(Map->ValueProp) : nullptr;
			if (!ValueProperty)
			{
				return;
			}
			FScriptMapHelper Helper(Map, Map->ContainerPtrToValuePtr<void>(&Character));
			for (FScriptMapHelper::FIterator It(Helper); It; ++It)
			{
				void* Value = Helper.GetValuePtr(It.GetInternalIndex());
				const UObject* Original = ValueProperty->GetObjectPropertyValue(Value);
				if (UClass* const* Generated = Original ? GeneratedOverlays.Find(Original->GetName()) : nullptr)
				{
					ValueProperty->SetObjectPropertyValue(Value, *Generated);
				}
			}
		};

		FAlsPoseScript Original;
		FAlsPoseScript Regenerated;
		Regenerated.BeforeSpawn = [GeneratedMainLinked, &UseGeneratedOverlays](ACharacter& Character)
		{
			Character.GetMesh()->AnimClass = GeneratedMainLinked;
			UseGeneratedOverlays(Character);
		};

		FAlsPoseRecording Expected;
		FAlsPoseRecording Actual;
		FString Error;
		if (!RecordAlsPoses(Original, Expected, Error) || !RecordAlsPoses(Regenerated, Actual, Error))
		{
			AddError(Error);
			return false;
		}
		AddInfo(Actual.Notes);
		const FString Difference = CompareAlsPoses(Expected, Actual);
		if (Difference.IsEmpty())
		{
			return true;
		}
		AddError(FString::Printf(TEXT("The generated Blueprints move the character differently: %s"), *Difference));

		// Which one: swap them in one at a time.
		const auto Single = [&](const FString& Label, const FAlsPoseScript& Script)
		{
			FAlsPoseRecording One;
			FString OneError;
			if (!RecordAlsPoses(Script, One, OneError))
			{
				AddError(OneError);
				return;
			}
			const FString OneDifference = CompareAlsPoses(Expected, One);
			AddInfo(FString::Printf(TEXT("only %s generated: %s"), *Label, OneDifference.IsEmpty() ? TEXT("same") : *OneDifference));
		};
		{
			FAlsPoseScript MainOnly;
			MainOnly.BeforeSpawn = [GeneratedMain](ACharacter& Character)
			{
				Character.GetMesh()->AnimClass = GeneratedMain;
			};
			Single(TEXT("AB_Als"), MainOnly);
		}
		for (const TPair<FName, UClass*>& Entry : GeneratedByTag)
		{
			FAlsPoseScript TagOnly;
			const FName Tag = Entry.Key;
			UClass* Class = Entry.Value;
			TagOnly.Setup = [Tag, Class](ACharacter& Character)
			{
				Character.GetMesh()->GetAnimInstance()->LinkAnimGraphByTag(Tag, Class);
			};
			Single(Tag.ToString(), TagOnly);
		}
		{
			FAlsPoseScript OverlaysOnly;
			OverlaysOnly.BeforeSpawn = UseGeneratedOverlays;
			Single(TEXT("the overlays"), OverlaysOnly);
		}
		for (const TPair<FString, UClass*>& Overlay : GeneratedOverlays)
		{
			// One overlay at a time, through the same map.
			const FString OverlayName = Overlay.Key;
			UClass* OverlayClass = Overlay.Value;
			FAlsPoseScript OneOverlay;
			OneOverlay.BeforeSpawn = [OverlayName, OverlayClass](ACharacter& Character)
			{
				const FMapProperty* Map = FindFProperty<FMapProperty>(Character.GetClass(), TEXT("OverlayAnimationInstanceClasses"));
				const FObjectPropertyBase* ValueProperty = Map ? CastField<FObjectPropertyBase>(Map->ValueProp) : nullptr;
				if (!ValueProperty)
				{
					return;
				}
				FScriptMapHelper Helper(Map, Map->ContainerPtrToValuePtr<void>(&Character));
				for (FScriptMapHelper::FIterator It(Helper); It; ++It)
				{
					void* Value = Helper.GetValuePtr(It.GetInternalIndex());
					const UObject* Original = ValueProperty->GetObjectPropertyValue(Value);
					if (Original && Original->GetName() == OverlayName)
					{
						ValueProperty->SetObjectPropertyValue(Value, OverlayClass);
					}
				}
			};
			Single(OverlayName, OneOverlay);
		}
		return true;
	}
}
