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
class UClass;

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
		/** The source the model was compiled from, kept with the asset (GetDabGeneratedText). */
		FString SourceText;
		/** Where nodes go instead of the automatic layout, by the NodeGuid they get (a source's `.layout` file). */
		TMap<FGuid, FIntPoint> NodePositions;
	};

	/** `<source>.layout`: where the editor put a Blueprint's nodes, next to the source it is made from. */
	DREAMANIMCOMPILER_API FString GetDabLayoutPath(const FString& SourcePath);
	/**
	 * Every node of every graph of the Blueprint as a line `<NodeGuid> <X> <Y>  # <graph>: <node>`, sorted, with
	 * a comment at the top. Generated GUIDs come from the node's place in the text, so they hold across rebuilds.
	 */
	DREAMANIMCOMPILER_API FString MakeDabLayout(const UAnimBlueprint& Blueprint);
	/** The positions in a layout file's text; lines that do not read as one are skipped. */
	DREAMANIMCOMPILER_API TMap<FGuid, FIntPoint> ParseDabLayout(const FString& Text);

	/**
	 * The source text a Blueprint was last generated from or written back to, kept in its package's
	 * metadata; empty for a Blueprint not made from text. While the file still holds this text, the two
	 * agree; it is the common base when both changed.
	 */
	DREAMANIMCOMPILER_API FString GetDabGeneratedText(const UAnimBlueprint& Blueprint);
	DREAMANIMCOMPILER_API void SetDabGeneratedText(UAnimBlueprint& Blueprint, const FString& Text);

	struct FDabGenerateResult
	{
		UAnimBlueprint* Blueprint = nullptr;
		/** Problems the Blueprint compiler reported, as text. */
		TArray<FString> CompilerErrors;
		TArray<FString> CompilerWarnings;
		/**
		 * One entry per message of CompilerErrors and CompilerWarnings, in the same order: where in the model
		 * the generated nodes the message names come from, innermost first (a node in a transition's rule,
		 * then the transition), as the `<graph path>/<id>` keys their GUIDs are made from (MakeDabNodeGuid).
		 * Empty for a message that names no generated node.
		 */
		TArray<TArray<FString>> CompilerErrorOrigins;
		TArray<TArray<FString>> CompilerWarningOrigins;

		bool Succeeded() const { return Blueprint != nullptr && CompilerErrors.IsEmpty(); }
	};

	/**
	 * What the getter of a slot of SlotClass returns (`DreamAnimAsset_<Slot>`): AnimMontage for a montage,
	 * BlendSpace for a blend space, AnimSequenceBase for any other sequence, AnimationAsset otherwise. A pin
	 * or parameter a slot read feeds must take it.
	 */
	DREAMANIMCOMPILER_API UClass* GetDabAssetSlotReadClass(const UClass* SlotClass);

	/**
	 * The NodeGuid the generator gives a node: an MD5 of `<AssetPath>/<GraphPath>/<Id>`. Graph paths are
	 * `AnimGraph`, `Layer/<Name>` and `Machine/<Machine>/State/<State>`; a state node is `State/<Name>` in
	 * `Machine/<Machine>`, a transition `Transition/<From>-><To>#<n>`. Write-back finds the text a node came
	 * from with it.
	 */
	DREAMANIMCOMPILER_API FGuid MakeDabNodeGuid(const FString& AssetPath, const FString& GraphPath, const FString& Id);

	/**
	 * Creates the asset, or rebuilds it in place when it exists (its graphs, functions, variables and
	 * settings are validated by compiling an isolated candidate first). Failed validation leaves the
	 * existing asset and animation set unchanged. On success its graphs, functions, variables and
	 * interface layers are replaced; the asset object and its generated class stay, so references to them
	 * survive). Every node gets a GUID derived from the asset path and the node's place in the model, so
	 * generating the same model twice gives the same GUIDs. Assets the model references (linked graph
	 * targets, implemented interfaces) must already be generated and compiled. Problems of the model are
	 * reported to Sink as DAB7xxx; the compiler's own messages go into the result.
	 */
	DREAMANIMCOMPILER_API FDabGenerateResult GenerateDabBlueprint(const Model::FDabModel& Model, Lang::FDabDiagnosticSink& Sink,
		const FDabGenerateOptions& Options = FDabGenerateOptions());
}
