// Copyright DreamMoon. All Rights Reserved.
//
// One source through the pipeline (parse, includes, templates, semantic, generate), formatting, the
// decompiler, and diagnostics in the forms tools read.

#include "Driver/DabDriver.h"
#include "Driver/DabDriverPrivate.h"

#include "Animation/AnimBlueprint.h"
#include "Extract/DabExtract.h"
#include "Generate/DabGenerate.h"
#include "Lang/DabParser.h"
#include "Lang/DabPrinter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

namespace UE::DreamAnim::Compiler::DriverPrivate
{
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
	 * The Blueprint compiler's own messages as diagnostics, so a Blueprint that does not compile fails the
	 * build like any other error. They have no position in the text.
	 */
	void AppendBlueprintCompilerMessages(FDabCompileResult& Result, const FDabGenerateResult& Generated)
	{
		const auto Add = [&Result](const Lang::EDabSeverity Severity, const TCHAR* Code, const FString& Message)
		{
			Lang::FDabDiagnostic& Diagnostic = Result.Diagnostics.AddDefaulted_GetRef();
			Diagnostic.Code = Code;
			Diagnostic.Severity = Severity;
			Diagnostic.Message = FString::Printf(TEXT("The Blueprint compiler: %s"), *Message);
			Diagnostic.FilePath = Result.FilePath;
		};
		for (const FString& Message : Generated.CompilerErrors)
		{
			Add(Lang::EDabSeverity::Error, TEXT("DAB7100"), Message);
			++Result.NumErrors;
		}
		for (const FString& Message : Generated.CompilerWarnings)
		{
			Add(Lang::EDabSeverity::Warning, TEXT("DAB7101"), Message);
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
		{
			FDabSemanticOptions SemanticOptions;
			SemanticOptions.Provenance = Options.Provenance;
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
			DriverPrivate::AppendBlueprintCompilerMessages(Result, Generated);
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

	FDabDecompileResult DecompileDabBlueprint(const UAnimBlueprint& Blueprint, const FDabLowerOptions& Options)
	{
		FDabDecompileResult Result;
		const FString ObjectPath = Blueprint.GetPathName();
		Lang::FDabDiagnosticSink Sink(ObjectPath);
		Model::FDabModel Model;
		const bool bExtracted = ExtractDabModel(Blueprint, Model, Sink);
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
		return DriverPrivate::NormalizeDabPath(FPaths::Combine(FPaths::ProjectDir(), TEXT("DAnim"),
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
