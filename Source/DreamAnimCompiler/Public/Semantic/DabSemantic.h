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
	/**
	 * Where each part of a model came from in the syntax tree it was built from (the expanded tree: a node
	 * a template body made points at the template call). Write-back uses it to find the text to change.
	 * The pointers live as long as the compilation unit.
	 */
	struct FDabProvenance
	{
		struct FNodeOrigin
		{
			/** The call that made the node. */
			const Lang::FDabCallExpr* Call = nullptr;
			/** The pose pin a stack fed, which the call does not write. */
			FString FedPin;
		};

		/** By `<graph path>/<node id>`, graph paths as MakeDabNodeGuid takes them (`AnimGraph`, `Layer/X`, `Machine/M/State/S`). */
		TMap<FString, FNodeOrigin> Nodes;
		/** A named pose's statement by `<graph path>/<pose name>`: its metadata holds Cached and node functions. */
		TMap<FString, const Lang::FDabAssignStmt*> NamedPoses;
		/** By machine name. */
		TMap<FString, const Lang::FDabNamedBlockStmt*> Machines;
		/** States, conduits and aliases by `<machine>/<name>`. */
		TMap<FString, const Lang::FDabNamedBlockStmt*> States;
		/** By `<machine>/<from>-><to>#<n>`, n counting that pair's rows from 0 as the generator does. */
		TMap<FString, const Lang::FDabTransitionStmt*> Transitions;
		/** By `<machine>/<rule>`. */
		TMap<FString, const Lang::FDabAssignStmt*> Rules;
		TMap<FString, const Lang::FDabDeclStmt*> Variables;
		TMap<FString, const Lang::FDabDeclStmt*> AssetSlots;
		TMap<FString, const Lang::FDabNamedBlockStmt*> Layers;
		/** Document sections by name: `Settings`, `Defaults`, `Aliases`, `Properties`, `Assets`, `Notifies`, `Graph`. */
		TMap<FString, const Lang::FDabAssignStmt*> Sections;
	};

	struct FDabSemanticOptions
	{
		/** Filled with where the model's parts came from, when set. */
		FDabProvenance* Provenance = nullptr;

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
