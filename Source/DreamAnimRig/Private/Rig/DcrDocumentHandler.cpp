// Copyright DreamMoon. All Rights Reserved.

#include "Rig/DcrDocumentHandler.h"

#include "ControlRigBlueprintLegacy.h"
#include "ControlRigRuntimeAsset.h"
#include "Lang/DabDiagnostic.h"
#include "Lang/DabPrinter.h"
#include "Misc/Paths.h"
#include "Rig/DcrAsset.h"
#include "Rig/DcrExtract.h"
#include "Rig/DcrGenerate.h"
#include "Rig/DcrLower.h"
#include "Rig/DcrNormalize.h"
#include "Rig/DcrSemantic.h"

namespace UE::DreamAnim::Rig
{
	using namespace UE::DreamAnim::Compiler;
	using namespace UE::DreamAnim::Lang;

	void FDcrDocumentHandler::Compile(FDabCompileResult& Result, const FString& Text, const FDabCompileOptions& Options)
	{
		if (!Result.Unit || !Result.Unit->Main || !Result.Unit->Main->Document)
		{
			return;
		}
		const FDabSpan DocumentSpan = Result.Unit->Main->Document->KindSpan;

		FDcrSemanticOptions SemanticOptions;
		if (!Options.OverridePackageName.IsEmpty())
		{
			SemanticOptions.AssetPath = Options.OverridePackageName + TEXT(".") + FPackageName::GetShortName(Options.OverridePackageName);
		}
		Model::FDcrModel Model;
		{
			FDabDiagnosticSink Sink(Result.FilePath);
			BuildDcrModel(*Result.Unit, Model, Sink, SemanticOptions);
			AddDabCompileDiagnostics(Result, Sink.GetDiagnostics());
		}
		if (Result.NumErrors > 0 || !Options.bGenerate)
		{
			return;
		}

		FDcrGenerateOptions GenerateOptions;
		GenerateOptions.bSave = Options.bSave;
		const FDcrGenerateResult Generated = GenerateDcrRig(Model, GenerateOptions);
		FDabDiagnosticSink Sink(Result.FilePath);
		for (const FString& Error : Generated.Errors)
		{
			Sink.Error(TEXT("DAB9101"), DocumentSpan, Error);
		}
		for (const FString& Warning : Generated.Warnings)
		{
			Sink.Warning(TEXT("DAB9102"), DocumentSpan, Warning);
		}
		for (const FString& Caller : Generated.BrokenCallers)
		{
			Sink.Warning(TEXT("DAB9103"), DocumentSpan,
				FString::Printf(TEXT("%s called a function this rig no longer has; the engine removed those calls."), *Caller),
				TEXT("Open that rig and replace the calls, or put the function back."));
		}
		AddDabCompileDiagnostics(Result, Sink.GetDiagnostics());
		Result.Asset = Generated.Rig.Asset;
	}

	TArray<FTopLevelAssetPath> FDcrDocumentHandler::GetDecompileClasses() const
	{
		return {
			UControlRigBlueprint::StaticClass()->GetClassPathName(),
			UControlRigRuntimeAsset::StaticClass()->GetClassPathName(),
		};
	}

	bool FDcrDocumentHandler::CanDecompile(const UObject& Asset) const
	{
		return WrapDcrRig(const_cast<UObject*>(&Asset)).IsValid();
	}

	FDabDecompileResult FDcrDocumentHandler::Decompile(UObject& Asset)
	{
		FDabDecompileResult Result;
		const FDcrRig Rig = WrapDcrRig(&Asset);
		FDabDiagnosticSink Sink(Asset.GetPathName());
		Model::FDcrModel Model;
		const FDcrExtractResult Extracted = ExtractDcrModel(Rig, Model);
		if (!Extracted.bSuccess)
		{
			Sink.Error(TEXT("DAB9104"), FDabSpan(), FString::Printf(TEXT("Cannot read %s: %s"), *Asset.GetPathName(), *Extracted.Error));
			Result.Diagnostics = Sink.GetDiagnostics();
			return Result;
		}
		NormalizeDcrModel(Model);
		const FDcrLowerResult Lowered = LowerDcrModel(Model);
		Result.Text = PrintDab(*Lowered.File);
		Result.NumUnsupported = Lowered.Unsupported.Num();
		Result.Diagnostics = Sink.GetDiagnostics();
		Result.bSucceeded = true;
		return Result;
	}

	FString FDcrDocumentHandler::GetDefaultDecompilePath(const UObject& Asset) const
	{
		// After `/Game/`, the path; under any other mount, the mount too (as Animation Blueprints go).
		FString Relative = Asset.GetPackage()->GetName();
		if (!Relative.RemoveFromStart(TEXT("/Game/")))
		{
			Relative.RemoveFromStart(TEXT("/"));
		}
		FString Path = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), TEXT("DAnim"), Relative + GetSourceExtension());
		FPaths::NormalizeFilename(Path);
		FPaths::CollapseRelativeDirectories(Path);
		return Path;
	}

	FString FDcrDocumentHandler::GetSourceExtension() const
	{
		return TEXT(".dcr");
	}
}
