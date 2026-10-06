// Copyright DreamMoon. All Rights Reserved.
//
// The semantic pass: an expanded syntax tree -> a model, checking every name against the engine's
// reflection on the way. Docs/Mapping.md is the specification; DAB3001-DAB6099 are its diagnostics.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabDiagnostic.h"
#include "Lang/DabTemplates.h"
#include "Model/DabModel.h"

namespace UE::DreamAnim::Compiler
{
	struct FDabSemanticOptions
	{
		/**
		 * Finds the asset another DreamAnim file produces, by its asset name (`AB_Als_Standing`,
		 * `ALI_Overlay`), as an object path. Unset or returning empty: the name is looked up among loaded
		 * and registered assets instead.
		 */
		TFunction<FString(const FString& AssetName)> ResolveDreamAnimAsset;
	};

	/**
	 * Builds the model of Unit.Main (whose #includes must already be resolved and templates expanded).
	 * Keeps going after an error so one run reports as much as it can. Returns false when any error was
	 * reported; OutModel then holds whatever could be built.
	 */
	DREAMANIMCOMPILER_API bool BuildDabModel(const Lang::FDabCompilationUnit& Unit, Model::FDabModel& OutModel,
		Lang::FDabDiagnosticSink& Sink, const FDabSemanticOptions& Options = FDabSemanticOptions());
}
