// Copyright DreamMoon. All Rights Reserved.
//
// The extractor: an Animation Blueprint as the editor holds it -> a model (Model/DabModel.h). It is the
// first half of the decompiler and the reference the write-back compares against.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabDiagnostic.h"
#include "Model/DabModel.h"

class UAnimBlueprint;

namespace UE::DreamAnim::Compiler
{
	struct FDabExtractOptions
	{
		/**
		 * Recognise the transition-animation forwarders that ALS and similar projects write by hand -- a
		 * function made of an entry, a getter and one call -- and record them as the call they forward
		 * (FDabModelCall). Off, any Blueprint function used as a state or node function is unsupported.
		 */
		bool bRecogniseForwarders = true;
	};

	/**
	 * Reads every authored graph of Blueprint (the ones it lists in UbergraphPages, FunctionGraphs and its
	 * implemented interfaces, never the compiler's intermediate copies) into OutModel. Whatever the
	 * language cannot express goes into OutModel.Unsupported with a location. Returns false only when the
	 * Blueprint cannot be read at all (reported to Sink).
	 */
	DREAMANIMCOMPILER_API bool ExtractDabModel(const UAnimBlueprint& Blueprint, Model::FDabModel& OutModel, Lang::FDabDiagnosticSink& Sink,
		const FDabExtractOptions& Options = FDabExtractOptions());
}
