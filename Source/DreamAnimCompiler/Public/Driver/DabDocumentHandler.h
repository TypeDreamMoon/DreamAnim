// Copyright DreamMoon. All Rights Reserved.
//
// Front ends for document kinds the Animation Blueprint pipeline does not build itself. The driver parses,
// resolves includes and expands templates for every source; for a document whose kind has a handler, the
// handler takes it from there (checks and generation), and the command line's decompile asks the handlers
// whether they take an asset. DreamAnimRig registers the ControlRig handler when it starts, so this module
// does not depend on Control Rig.

#pragma once

#include "CoreMinimal.h"
#include "Driver/DabDriver.h"
#include "Driver/DabVocabularyExport.h"
#include "UObject/TopLevelAssetPath.h"
#include "WriteBack/DabWriteBack.h"

namespace UE::DreamAnim::Compiler
{
	class IDabDocumentHandler
	{
	public:
		virtual ~IDabDocumentHandler() = default;

		/**
		 * Checks Result.Unit (parsed, includes resolved, templates expanded) and, when Options.bGenerate and it
		 * checks clean, builds it. Adds every diagnostic with AddDabCompileDiagnostics and sets Result.Asset.
		 */
		virtual void Compile(FDabCompileResult& Result, const FString& Text, const FDabCompileOptions& Options) = 0;

		/** The asset classes Decompile takes, so the command line can find them under a content path. */
		virtual TArray<FTopLevelAssetPath> GetDecompileClasses() const = 0;
		virtual bool CanDecompile(const UObject& Asset) const = 0;
		virtual FDabDecompileResult Decompile(UObject& Asset) = 0;
		/** Where Asset's text goes by default: under the project's `DAnim/`, like GetDefaultDecompilePath. */
		virtual FString GetDefaultDecompilePath(const UObject& Asset) const = 0;
		/** The extension of this kind's sources, with the dot (`.dcr`). */
		virtual FString GetSourceExtension() const = 0;

		/** Whether changes made to this kind's assets in the editor are written into their sources (WriteBack). */
		virtual bool SupportsWriteBack() const { return false; }
		/**
		 * What Asset holds that Text (its source at FilePath) does not, written into the text. Nothing is read from
		 * or written to disk.
		 */
		virtual FDabWriteBackResult WriteBack(UObject& Asset, const FString& FilePath, const FString& Text, const FDabWriteBackOptions& Options)
		{
			return FDabWriteBackResult();
		}
		/** The text Asset was last built from or written to, kept with it; empty when there is none. */
		virtual FString GetGeneratedText(const UObject& Asset) const { return FString(); }
		virtual void SetGeneratedText(UObject& Asset, const FString& Text) const {}

		/** Adds what this kind of document knows from the engine to the vocabulary `dab.ps1 vocab` writes (Out is its root). */
		virtual void AddVocabulary(FJsonObject& Out, const FDabVocabularyOptions& Options) const {}
	};

	DREAMANIMCOMPILER_API void RegisterDabDocumentHandler(Lang::EDabDocumentKind Kind, TSharedRef<IDabDocumentHandler> Handler);
	DREAMANIMCOMPILER_API void UnregisterDabDocumentHandler(Lang::EDabDocumentKind Kind);
	DREAMANIMCOMPILER_API IDabDocumentHandler* FindDabDocumentHandler(Lang::EDabDocumentKind Kind);
	/** The handler whose Decompile takes Asset, or null. */
	DREAMANIMCOMPILER_API IDabDocumentHandler* FindDabDecompileHandler(const UObject& Asset);
	DREAMANIMCOMPILER_API TArray<IDabDocumentHandler*> GetDabDocumentHandlers();

	/** Appends diagnostics to a compile result, filling in its file path where they have none and counting errors. */
	DREAMANIMCOMPILER_API void AddDabCompileDiagnostics(FDabCompileResult& Result, TConstArrayView<Lang::FDabDiagnostic> Diagnostics);
}
