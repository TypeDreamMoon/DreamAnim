// Copyright DreamMoon. All Rights Reserved.
//
// The whole pipeline for one source file, and what the command line, the editor and the tests share:
// where sources live, which document makes which asset, compile, format, decompile, and diagnostics in
// the two forms tools read (a clickable line and JSON).

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabDiagnostic.h"
#include "Lang/DabTemplates.h"
#include "Lower/DabLower.h"
#include "Model/DabModel.h"
#include "Semantic/DabSemantic.h"

class UAnimBlueprint;

namespace UE::DreamAnim::Compiler
{
	/**
	 * Where sources live: the project's `DAnim/` and the `DAnim/` of every enabled plugin that has one, as
	 * absolute paths. #include resolves against the including file's folder first, then these in order.
	 */
	DREAMANIMCOMPILER_API TArray<FString> GetDabSourceRoots();

	/** `.dab`, `.dai` and `.dat`. */
	DREAMANIMCOMPILER_API bool IsDabSourceFile(const FString& FilePath);

	/** One source as the index knows it: read only as far as its includes and document header. */
	struct FDabSourceEntry
	{
		/** Absolute, normalised (forward slashes). */
		FString FilePath;
		Lang::EDabDocumentKind Kind = Lang::EDabDocumentKind::Unknown;
		/** The asset it makes (`/ALS/ALS/Character/AB_Als.AB_Als`); empty for a template library. */
		FString AssetPath;
		/** Resolved paths of the files it #includes directly. */
		TArray<FString> Includes;
	};

	/** Every source under some roots, by file and by the asset it makes. */
	class DREAMANIMCOMPILER_API FDabSourceIndex
	{
	public:
		void Scan(const TArray<FString>& Roots);
		/** One file changed, appeared or went away. */
		void Rescan(const FString& FilePath);

		const FDabSourceEntry* FindByFile(const FString& FilePath) const;
		/** By object path (`/Game/A/B.B`) or package path (`/Game/A/B`). */
		const FDabSourceEntry* FindByAsset(const FString& AssetPath) const;
		/** The documents that include FilePath, directly or through other libraries: what a .dat change rebuilds. */
		TArray<FString> FindDependents(const FString& FilePath) const;

		const TArray<FDabSourceEntry>& GetEntries() const { return Entries; }
		const TArray<FString>& GetRoots() const { return Roots; }

	private:
		TArray<FString> Roots;
		TArray<FDabSourceEntry> Entries;
	};

	struct FDabCompileOptions
	{
		/** Make the Animation Blueprint; false checks only (parse, includes, templates, semantic). */
		bool bGenerate = true;
		/** Save the generated package; false leaves it dirty in memory. */
		bool bSave = true;
		/** Extra #include roots after the file's folder; empty uses GetDabSourceRoots(). */
		TArray<FString> IncludeRoots;
		/** Generate into this package instead of the document's own asset path (tests). */
		FString OverridePackageName;
		/** Filled with where the model's parts came from (passed to BuildDabModel); write-back needs it. */
		FDabProvenance* Provenance = nullptr;
	};

	/** Everything one compile produced. Move-only: it holds the syntax trees. */
	struct DREAMANIMCOMPILER_API FDabCompileResult
	{
		FString FilePath;
		/** Every diagnostic of every stage, each with its FilePath set (a library's own path for its own). */
		TArray<Lang::FDabDiagnostic> Diagnostics;
		int32 NumErrors = 0;
		/** The parsed, expanded file and its libraries; null when the file could not be read. */
		TUniquePtr<Lang::FDabCompilationUnit> Unit;
		Model::FDabModel Model;
		/** Set when generated. */
		UAnimBlueprint* Blueprint = nullptr;

		bool Succeeded() const { return NumErrors == 0; }
	};

	/** Reads one source and runs it through the pipeline. */
	DREAMANIMCOMPILER_API FDabCompileResult CompileDabFile(const FString& FilePath, const FDabCompileOptions& Options = FDabCompileOptions());
	/** The same from text held in memory (an editor buffer, a test); FilePath names it in diagnostics and resolves includes. */
	DREAMANIMCOMPILER_API FDabCompileResult CompileDabText(const FString& FilePath, const FString& Text, const FDabCompileOptions& Options = FDabCompileOptions());

	/** The canonical text of a source. False, with diagnostics, when it does not parse. */
	DREAMANIMCOMPILER_API bool FormatDabText(const FString& FilePath, const FString& Text, FString& OutText, TArray<Lang::FDabDiagnostic>& OutDiagnostics);

	struct FDabDecompileResult
	{
		FString Text;
		TArray<Lang::FDabDiagnostic> Diagnostics;
		/** What the language cannot hold; listed at the top of Text too. */
		int32 NumUnsupported = 0;
		bool bSucceeded = false;
	};

	/** An Animation Blueprint as text: extract, make names identifiers, lower, print. */
	DREAMANIMCOMPILER_API FDabDecompileResult DecompileDabBlueprint(const UAnimBlueprint& Blueprint, const FDabLowerOptions& Options = FDabLowerOptions());

	/**
	 * Where a decompiled Blueprint's text goes by default: under the project's `DAnim/`, the package path
	 * after the mount (`/Game/Characters/AB_Hero` -> `DAnim/Characters/AB_Hero.dab`; a plugin mount adds its
	 * name: `DAnim/ALS/ALS/Character/AB_Als.dab`), `.dai` for an interface.
	 */
	DREAMANIMCOMPILER_API FString GetDefaultDecompilePath(const UAnimBlueprint& Blueprint);

	/** `<file>(<line>,<col>): error DABnnnn: message` -- what Visual Studio, VS Code problem matchers and the log read. */
	DREAMANIMCOMPILER_API FString FormatDabDiagnosticLine(const Lang::FDabDiagnostic& Diagnostic);

	/**
	 * `[{"file":..., "line":..., "column":..., "length":..., "severity":"error", "code":"DAB2001",
	 *    "message":..., "hint":...}, ...]` -- what the VS Code extension reads. Line and column are 1-based;
	 * length counts characters from there (it may run past the end of the line).
	 */
	DREAMANIMCOMPILER_API FString DabDiagnosticsToJson(const TArray<Lang::FDabDiagnostic>& Diagnostics);
}
