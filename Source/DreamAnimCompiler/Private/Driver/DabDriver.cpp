// Copyright DreamMoon. All Rights Reserved.
//
// One source through the pipeline (parse, includes, templates, semantic, generate), formatting, the
// decompiler, and diagnostics in the forms tools read.

#include "Driver/DabDriver.h"
#include "Driver/DabDriverPrivate.h"

#include "Animation/AnimBlueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DreamAnimSet.h"
#include "Extract/DabExtract.h"
#include "Generate/DabGenerate.h"
#include "Lang/DabParser.h"
#include "Lang/DabPrinter.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace UE::DreamAnim::Compiler::DriverPrivate
{
	/** LiftDabAnimationAssets, then the set: created when missing, given the Blueprint's group, saved when asked. */
	bool MoveDabAnimationsToSet(const UAnimBlueprint& Blueprint, Model::FDabModel& Model, const FDabAnimSetMigration& Migration,
		Lang::FDabDiagnosticSink& Sink, TArray<FDabLiftedAsset>& OutLifted)
	{
		const FString TargetPath = ToDabObjectPath(Migration.AnimSetPath);
		const FString PreviousSet = Model.DefaultAnimSet;
		const TArray<Model::FDabModelAssetSlot> PreviousSlots = Model.AssetSlots;
		OutLifted = LiftDabAnimationAssets(Model, TargetPath);
		// Repeated migration is a no-op, including no creation of an unused target set.
		if (OutLifted.IsEmpty())
		{
			return true;
		}
		if (!FPackageName::IsValidLongPackageName(FPackageName::ObjectPathToPackageName(TargetPath))
			|| FPackageName::ObjectPathToObjectName(TargetPath).IsEmpty())
		{
			Sink.Error(TEXT("DAB8102"), Lang::FDabSpan(), FString::Printf(TEXT("%s is not a valid animation set path"), *Migration.AnimSetPath));
			return false;
		}
		if (UObject* Existing = LoadObject<UObject>(nullptr, *TargetPath, nullptr, LOAD_NoWarn | LOAD_Quiet); Existing && !Existing->IsA<UDreamAnimSet>())
		{
			Sink.Error(TEXT("DAB8102"), Lang::FDabSpan(), FString::Printf(TEXT("%s is not an animation set"), *TargetPath));
			return false;
		}
		TArray<FDabLiftedAsset> AssetsToFill = OutLifted;
		// A partially migrated Blueprint can still contain direct references. Preserve its existing selections
		// when moving those remaining references into another set.
		if (!PreviousSlots.IsEmpty() && !PreviousSet.IsEmpty() && !PreviousSet.Equals(TargetPath, ESearchCase::IgnoreCase))
		{
			const UDreamAnimSet* OldSet = LoadObject<UDreamAnimSet>(nullptr, *PreviousSet, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!OldSet)
			{
				Sink.Error(TEXT("DAB8102"), Lang::FDabSpan(), TEXT("The previous animation set cannot be read; its slot selections cannot be migrated safely."));
				return false;
			}
			for (const Model::FDabModelAssetSlot& Slot : PreviousSlots)
			{
				if (const UAnimationAsset* Asset = OldSet->Find(Blueprint.GetFName(), FName(*Slot.Name)))
				{
					AssetsToFill.Add({ Slot.Name, Slot.Type, Asset->GetPathName() });
				}
			}
		}
		if (Model.AssetSlots.IsEmpty())
		{
			return true;
		}
		UDreamAnimSet* Set = LoadOrCreateDabAnimSet(TargetPath, Sink);
		if (!Set)
		{
			return false;
		}
		TArray<FString> Kept;
		const bool bChanged = FillDabAnimSet(*Set, Blueprint.GetFName(), Model, AssetsToFill, &Kept);
		for (const FString& Slot : Kept)
		{
			Sink.Warning(TEXT("DAB8103"), Lang::FDabSpan(), FString::Printf(TEXT("%s already holds another animation in %s's slot %s; it is kept"),
				*Set->GetName(), *Blueprint.GetName(), *Slot));
		}
		if (!bChanged && !Set->GetPackage()->IsDirty())
		{
			return true;
		}
		Set->MarkPackageDirty();
		if (!Migration.bSave)
		{
			return true;
		}
		UPackage* Package = Set->GetPackage();
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		Args.bSlowTask = false;
		if (!UPackage::SavePackage(Package, Set, *Filename, Args))
		{
			Sink.Error(TEXT("DAB8102"), Lang::FDabSpan(), FString::Printf(TEXT("%s cannot be saved to %s"), *Package->GetName(), *Filename));
			return false;
		}
		return true;
	}

	/** Moves one pass's diagnostics onto the result, each naming its file absolutely, and counts the errors. */
	void AppendCompileDiagnostics(FDabCompileResult& Result, const Lang::FDabDiagnosticSink& Sink)
	{
		for (const Lang::FDabDiagnostic& Diagnostic : Sink.GetDiagnostics())
		{
			Lang::FDabDiagnostic& Added = Result.Diagnostics.Add_GetRef(Diagnostic);
			Added.FilePath = Added.FilePath.IsEmpty() ? Result.FilePath : NormalizeDabPath(Added.FilePath);
			if (Added.Severity == Lang::EDabSeverity::Error)
			{
				++Result.NumErrors;
			}
		}
	}

	/**
	 * Where in the text a place in the model was written (`<graph path>/<id>`, as the generator makes GUIDs
	 * from it): a node's call or named pose, a state, conduit or alias, a transition's row. Empty when the
	 * place is not one the text names.
	 */
	Lang::FDabSpan FindDabPlaceSpan(const FDabProvenance& Provenance, const FString& Place)
	{
		if (const FDabProvenance::FNodeOrigin* Node = Provenance.Nodes.Find(Place); Node && Node->Call)
		{
			return Node->Call->Span;
		}
		if (const Lang::FDabAssignStmt* const* Named = Provenance.NamedPoses.Find(Place))
		{
			return (*Named)->Span;
		}
		// Machine/<machine>/<State|Conduit|Alias>/<name>, Machine/<machine>/Transition/<from>-><to>#<n>
		TArray<FString> Parts;
		Place.ParseIntoArray(Parts, TEXT("/"));
		if (Parts.Num() == 4 && Parts[0] == TEXT("Machine"))
		{
			const FString Key = Parts[1] + TEXT("/") + Parts[3];
			if (Parts[2] == TEXT("Transition"))
			{
				if (const Lang::FDabTransitionStmt* const* Row = Provenance.Transitions.Find(Key))
				{
					return (*Row)->Span;
				}
			}
			else if (const Lang::FDabNamedBlockStmt* const* Block = Provenance.States.Find(Key))
			{
				return (*Block)->Span;
			}
		}
		return Lang::FDabSpan();
	}

	/**
	 * The Blueprint compiler's own messages as diagnostics, so a Blueprint that does not compile fails the
	 * build like any other error. Each is placed on the text of the innermost node it names that the text
	 * wrote; one that names none is placed at the top of the file.
	 */
	void AppendBlueprintCompilerMessages(FDabCompileResult& Result, const FDabGenerateResult& Generated, const FDabProvenance& Provenance)
	{
		const auto Add = [&Result, &Provenance](const Lang::EDabSeverity Severity, const TCHAR* Code, const FString& Message, const TArray<FString>* Origins)
		{
			Lang::FDabDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Code = Code;
			Diagnostic.Severity = Severity;
			Diagnostic.Message = FString::Printf(TEXT("The Blueprint compiler: %s"), *Message);
			Diagnostic.FilePath = Result.FilePath;
			for (const FString& Origin : Origins ? *Origins : TArray<FString>())
			{
				const Lang::FDabSpan Span = FindDabPlaceSpan(Provenance, Origin);
				if (!Span.IsEmpty())
				{
					Diagnostic.Span = Span;
					break;
				}
			}
		};
		for (int32 Index = 0; Index < Generated.CompilerErrors.Num(); ++Index)
		{
			Add(Lang::EDabSeverity::Error, TEXT("DAB7100"), Generated.CompilerErrors[Index],
				Generated.CompilerErrorOrigins.IsValidIndex(Index) ? &Generated.CompilerErrorOrigins[Index] : nullptr);
			++Result.NumErrors;
		}
		for (int32 Index = 0; Index < Generated.CompilerWarnings.Num(); ++Index)
		{
			Add(Lang::EDabSeverity::Warning, TEXT("DAB7101"), Generated.CompilerWarnings[Index],
				Generated.CompilerWarningOrigins.IsValidIndex(Index) ? &Generated.CompilerWarningOrigins[Index] : nullptr);
		}
	}

	/** A JSON string literal: quotes, backslashes and control characters escaped; everything else as it is (the file is UTF-8). */
	void AppendDabJsonString(FString& Out, const FString& Value)
	{
		Out.AppendChar(TEXT('"'));
		for (const TCHAR Character : Value)
		{
			switch (Character)
			{
			case TEXT('"'): Out += TEXT("\\\""); break;
			case TEXT('\\'): Out += TEXT("\\\\"); break;
			case TEXT('\n'): Out += TEXT("\\n"); break;
			case TEXT('\r'): Out += TEXT("\\r"); break;
			case TEXT('\t'): Out += TEXT("\\t"); break;
			case TEXT('\b'): Out += TEXT("\\b"); break;
			case TEXT('\f'): Out += TEXT("\\f"); break;
			default:
				if (Character < 0x20)
				{
					Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Character));
				}
				else
				{
					Out.AppendChar(Character);
				}
				break;
			}
		}
		Out.AppendChar(TEXT('"'));
	}
}

namespace UE::DreamAnim::Compiler
{
	FDabCompileResult CompileDabFile(const FString& FilePath, const FDabCompileOptions& Options)
	{
		const FString Path = DriverPrivate::NormalizeDabPath(FilePath);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			FDabCompileResult Result;
			Result.FilePath = Path;
			Lang::FDabDiagnosticSink Sink(Path);
			Sink.Error(TEXT("DAB6014"), Lang::FDabSpan(), FString::Printf(TEXT("Cannot read %s."), *Path),
				TEXT("Check that the file exists and is not locked by another program."));
			DriverPrivate::AppendCompileDiagnostics(Result, Sink);
			return Result;
		}
		return CompileDabText(Path, Text, Options);
	}

	FDabCompileResult CompileDabText(const FString& FilePath, const FString& Text, const FDabCompileOptions& Options)
	{
		FDabCompileResult Result;
		Result.FilePath = DriverPrivate::NormalizeDabPath(FilePath);
		// On the heap from the start: the provenance points into these trees, and they must not move.
		Result.Unit = MakeUnique<Lang::FDabCompilationUnit>();
		Lang::FDabCompilationUnit& Unit = *Result.Unit;
		Unit.MainSource = Lang::FDabSourceText(Result.FilePath, Text);

		{
			Lang::FDabDiagnosticSink Sink(Result.FilePath);
			Unit.Main = Lang::ParseDab(Unit.MainSource, Sink);
			DriverPrivate::AppendCompileDiagnostics(Result, Sink);
		}
		{
			TArray<FString> Roots;
			for (const FString& Root : Options.IncludeRoots.IsEmpty() ? GetDabSourceRoots() : Options.IncludeRoots)
			{
				Roots.AddUnique(DriverPrivate::NormalizeDabPath(Root));
			}
			Lang::FDabFileSystemIncludeLoader Loader(MoveTemp(Roots));
			Lang::FDabDiagnosticSink Sink(Result.FilePath);
			Lang::ResolveDabIncludes(Unit, Loader, Sink);
			DriverPrivate::AppendCompileDiagnostics(Result, Sink);
		}
		{
			Lang::FDabDiagnosticSink Sink(Result.FilePath);
			Lang::ExpandDabTemplates(Unit, Sink);
			DriverPrivate::AppendCompileDiagnostics(Result, Sink);
		}
		// A template library is checked as far as it can be alone: its syntax, its includes and its templates.
		if (Unit.Main && Unit.Main->Document && Unit.Main->Document->DocumentKind == Lang::EDabDocumentKind::AnimTemplates)
		{
			return Result;
		}
		// Where the model's parts came from: write-back asks for it, and compiler messages are placed with it.
		FDabProvenance OwnProvenance;
		FDabProvenance& Provenance = Options.Provenance ? *Options.Provenance : OwnProvenance;
		{
			FDabSemanticOptions SemanticOptions;
			SemanticOptions.Provenance = &Provenance;
			SemanticOptions.ResolveDreamAnimAsset = Options.ResolveDreamAnimAsset;
			Lang::FDabDiagnosticSink Sink(Result.FilePath);
			BuildDabModel(Unit, Result.Model, Sink, SemanticOptions);
			DriverPrivate::AppendCompileDiagnostics(Result, Sink);
		}

		if (Options.bGenerate && Result.NumErrors == 0)
		{
			FDabGenerateOptions GenerateOptions;
			GenerateOptions.bSave = Options.bSave;
			GenerateOptions.OverridePackageName = Options.OverridePackageName;
			GenerateOptions.SourceText = Text;
			FString Layout;
			if (FFileHelper::LoadFileToString(Layout, *GetDabLayoutPath(Result.FilePath)))
			{
				GenerateOptions.NodePositions = ParseDabLayout(Layout);
			}
			Lang::FDabDiagnosticSink Sink(Result.FilePath);
			const FDabGenerateResult Generated = GenerateDabBlueprint(Result.Model, Sink, GenerateOptions);
			DriverPrivate::AppendCompileDiagnostics(Result, Sink);
			DriverPrivate::AppendBlueprintCompilerMessages(Result, Generated, Provenance);
			Result.Blueprint = Generated.Blueprint;
		}
		return Result;
	}

	bool FormatDabText(const FString& FilePath, const FString& Text, FString& OutText, TArray<Lang::FDabDiagnostic>& OutDiagnostics)
	{
		const FString Path = DriverPrivate::NormalizeDabPath(FilePath);
		const Lang::FDabSourceText Source(Path, Text);
		Lang::FDabDiagnosticSink Sink(Path);
		const TUniquePtr<Lang::FDabFile> File = Lang::ParseDab(Source, Sink);
		for (const Lang::FDabDiagnostic& Diagnostic : Sink.GetDiagnostics())
		{
			Lang::FDabDiagnostic& Added = OutDiagnostics.Add_GetRef(Diagnostic);
			Added.FilePath = Added.FilePath.IsEmpty() ? Path : DriverPrivate::NormalizeDabPath(Added.FilePath);
		}
		// A tree with errors in it prints without the statements that failed: the canonical text of a broken
		// file would silently drop what the author wrote.
		if (Sink.HasErrors() || !File)
		{
			OutText.Reset();
			return false;
		}
		OutText = Lang::PrintDab(*File);
		return true;
	}

	UDreamAnimSet* LoadOrCreateDabAnimSet(const FString& AnimSetPath, Lang::FDabDiagnosticSink& Sink)
	{
		const FString ObjectPath = DriverPrivate::ToDabObjectPath(AnimSetPath);
		const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
		const FString AssetName = FPackageName::ObjectPathToObjectName(ObjectPath);
		if (ObjectPath.IsEmpty() || AssetName.IsEmpty() || !FPackageName::IsValidLongPackageName(PackageName))
		{
			Sink.Error(TEXT("DAB8102"), Lang::FDabSpan(), FString::Printf(TEXT("%s is not an asset path such as /Game/Characters/DAS_Hero"), *AnimSetPath));
			return nullptr;
		}
		if (UObject* Existing = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			UDreamAnimSet* Set = Cast<UDreamAnimSet>(Existing);
			if (!Set)
			{
				Sink.Error(TEXT("DAB8102"), Lang::FDabSpan(), FString::Printf(TEXT("%s is a %s, not an animation set"), *ObjectPath, *Existing->GetClass()->GetName()));
			}
			return Set;
		}
		UPackage* Package = CreatePackage(*PackageName);
		if (FindObject<UObject>(Package, *AssetName))
		{
			Sink.Error(TEXT("DAB8102"), Lang::FDabSpan(), FString::Printf(TEXT("%s holds another object named %s"), *PackageName, *AssetName));
			return nullptr;
		}
		UDreamAnimSet* Set = NewObject<UDreamAnimSet>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(Set);
		Set->MarkPackageDirty();
		return Set;
	}

	FDabDecompileResult DecompileDabBlueprint(const UAnimBlueprint& Blueprint, const FDabLowerOptions& Options, const FDabAnimSetMigration* AnimSet)
	{
		FDabDecompileResult Result;
		const FString ObjectPath = Blueprint.GetPathName();
		Lang::FDabDiagnosticSink Sink(ObjectPath);
		Model::FDabModel Model;
		bool bExtracted = ExtractDabModel(Blueprint, Model, Sink);
		if (bExtracted && AnimSet)
		{
			bExtracted = DriverPrivate::MoveDabAnimationsToSet(Blueprint, Model, *AnimSet, Sink, Result.LiftedAssets);
		}
		if (bExtracted)
		{
			TMap<FString, FString> Renames;
			NormalizeDabModelNames(Model, &Renames);
			FDabLowerOptions LowerOptions = Options;
			LowerOptions.Renames.Append(Renames);
			if (const TUniquePtr<Lang::FDabFile> File = LowerDabModel(Model, LowerOptions))
			{
				Result.Text = Lang::PrintDab(*File);
			}
			Result.NumUnsupported = Model.Unsupported.Num();
		}
		Result.Diagnostics = Sink.GetDiagnostics();
		for (Lang::FDabDiagnostic& Diagnostic : Result.Diagnostics)
		{
			// Extraction problems are about the asset, not a file: they name it by its object path.
			if (Diagnostic.FilePath.IsEmpty())
			{
				Diagnostic.FilePath = ObjectPath;
			}
		}
		Result.bSucceeded = bExtracted && !Sink.HasErrors() && !Result.Text.IsEmpty();
		return Result;
	}

	FString GetDefaultDecompilePath(const UAnimBlueprint& Blueprint)
	{
		const FString PackageName = Blueprint.GetPackage()->GetName();
		return DriverPrivate::NormalizeDabPath(FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), TEXT("DAnim"),
			DriverPrivate::GetDabDecompileRelativePath(PackageName) + DriverPrivate::GetDabDecompileExtension(Blueprint)));
	}

	FString FormatDabDiagnosticLine(const Lang::FDabDiagnostic& Diagnostic)
	{
		return FString::Printf(TEXT("%s(%d,%d): %s %s: %s"), *Diagnostic.FilePath, Diagnostic.Span.Line, Diagnostic.Span.Column,
			Lang::LexToString(Diagnostic.Severity), *Diagnostic.Code, *Diagnostic.Message);
	}

	FString DabDiagnosticsToJson(const TArray<Lang::FDabDiagnostic>& Diagnostics)
	{
		FString Json = TEXT("[");
		for (int32 Index = 0; Index < Diagnostics.Num(); ++Index)
		{
			const Lang::FDabDiagnostic& Diagnostic = Diagnostics[Index];
			if (Index > 0)
			{
				Json += TEXT(", ");
			}
			Json += TEXT("{\"file\":");
			DriverPrivate::AppendDabJsonString(Json, Diagnostic.FilePath);
			Json += FString::Printf(TEXT(", \"line\":%d, \"column\":%d, \"length\":%d, \"severity\":"),
				Diagnostic.Span.Line, Diagnostic.Span.Column, FMath::Max(Diagnostic.Span.Length, 0));
			DriverPrivate::AppendDabJsonString(Json, Lang::LexToString(Diagnostic.Severity));
			Json += TEXT(", \"code\":");
			DriverPrivate::AppendDabJsonString(Json, Diagnostic.Code);
			Json += TEXT(", \"message\":");
			DriverPrivate::AppendDabJsonString(Json, Diagnostic.Message);
			Json += TEXT(", \"hint\":");
			DriverPrivate::AppendDabJsonString(Json, Diagnostic.Hint);
			Json += TEXT("}");
		}
		Json += TEXT("]");
		return Json;
	}
}
