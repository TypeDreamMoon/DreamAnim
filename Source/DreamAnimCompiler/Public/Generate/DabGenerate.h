// Copyright DreamMoon. All Rights Reserved.
//
// The generator: a model -> an Animation Blueprint (or Animation Layer Interface) asset, created or
// rebuilt in place, then compiled. It is literal: every node, pin, binding and transition comes from the
// model as it is; the conveniences of the text were applied by the semantic pass.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabDiagnostic.h"
#include "Model/DabModel.h"

class UAnimBlueprint;

namespace UE::DreamAnim::Compiler
{
	struct FDabGenerateOptions
	{
		/** Save the package after a successful compile. Tests that only inspect the result leave it off. */
		bool bSave = true;
		/**
		 * Where to create the asset instead of Model.AssetPath (a long package name like
		 * `/Game/DreamAnimTests/AB_Copy`). The round-trip tests regenerate next to the original this way.
		 */
		FString OverridePackageName;
	};

	struct FDabGenerateResult
	{
		UAnimBlueprint* Blueprint = nullptr;
		/** Problems the Blueprint compiler reported, as text. */
		TArray<FString> CompilerErrors;
		TArray<FString> CompilerWarnings;

		bool Succeeded() const { return Blueprint != nullptr && CompilerErrors.IsEmpty(); }
	};

	/**
	 * The NodeGuid the generator gives a node: an MD5 of `<AssetPath>/<GraphPath>/<Id>`. Graph paths are
	 * `AnimGraph`, `Layer/<Name>` and `Machine/<Machine>/State/<State>`; a state node is `State/<Name>` in
	 * `Machine/<Machine>`, a transition `Transition/<From>-><To>#<n>`. Write-back finds the text a node came
	 * from with it.
	 */
	DREAMANIMCOMPILER_API FGuid MakeDabNodeGuid(const FString& AssetPath, const FString& GraphPath, const FString& Id);

	/**
	 * Creates the asset, or rebuilds it in place when it exists (its graphs, functions, variables and
	 * interface layers are replaced; the asset object and its generated class stay, so references to them
	 * survive). Every node gets a GUID derived from the asset path and the node's place in the model, so
	 * generating the same model twice gives the same GUIDs. Assets the model references (linked graph
	 * targets, implemented interfaces) must already be generated and compiled. Problems of the model are
	 * reported to Sink as DAB7xxx; the compiler's own messages go into the result.
	 */
	DREAMANIMCOMPILER_API FDabGenerateResult GenerateDabBlueprint(const Model::FDabModel& Model, Lang::FDabDiagnosticSink& Sink,
		const FDabGenerateOptions& Options = FDabGenerateOptions());
}
