// Copyright DreamMoon. All Rights Reserved.
//
// The driver's handler for ControlRig documents: compile is semantic then generate, decompile is extract,
// normalize, lower and print, write-back is WriteBackDcrRig. Registered when DreamAnimRig starts.

#pragma once

#include "CoreMinimal.h"
#include "Driver/DabDocumentHandler.h"

namespace UE::DreamAnim::Rig
{
	class FDcrDocumentHandler final : public Compiler::IDabDocumentHandler
	{
	public:
		virtual void Compile(Compiler::FDabCompileResult& Result, const FString& Text, const Compiler::FDabCompileOptions& Options) override;
		virtual TArray<FTopLevelAssetPath> GetDecompileClasses() const override;
		virtual bool CanDecompile(const UObject& Asset) const override;
		virtual Compiler::FDabDecompileResult Decompile(UObject& Asset) override;
		virtual FString GetDefaultDecompilePath(const UObject& Asset) const override;
		virtual FString GetSourceExtension() const override;
		virtual bool SupportsWriteBack() const override { return true; }
		virtual Compiler::FDabWriteBackResult WriteBack(UObject& Asset, const FString& FilePath, const FString& Text,
			const Compiler::FDabWriteBackOptions& Options) override;
		virtual FString GetGeneratedText(const UObject& Asset) const override;
		virtual void SetGeneratedText(UObject& Asset, const FString& Text) const override;
		/** With RigUnits: every unit and dispatch a .dcr can call, with its pins, as `rigUnits`. */
		virtual void AddVocabulary(FJsonObject& Out, const Compiler::FDabVocabularyOptions& Options) const override;
	};
}
