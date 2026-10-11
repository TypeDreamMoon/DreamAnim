// Copyright DreamMoon. All Rights Reserved.
//
// The driver's handler for ControlRig documents: compile is semantic then generate, decompile is extract,
// normalize, lower and print. Registered when DreamAnimRig starts.

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
	};
}
