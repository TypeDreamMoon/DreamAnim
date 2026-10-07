// Copyright DreamMoon. All Rights Reserved.
//
// Lowering: a model -> a syntax tree the printer turns into a `.dab` file. With the extractor it makes
// the decompiler. Docs/Mapping.md ("Lowering") lists the choices it makes.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabAst.h"
#include "Model/DabModel.h"

namespace UE::DreamAnim::Compiler
{
	struct FDabLowerOptions
	{
		/** Introduce `Main = Self.GetParent()` and write paths through it. */
		bool bUseAliases = true;
		/** Pull a node out into a named pose when printing it inline would nest calls deeper than this. */
		int32 MaxInlineDepth = 3;
		/** Write an asset by its bare name (`A_Als_Idle`) instead of `Path(...)` when no other registered asset has that name. */
		bool bShortAssetNames = true;
		/** The alias `Self.GetParent()` is written as when bUseAliases is set. */
		FString ParentAlias = TEXT("Main");
		/** What NormalizeDabModelNames changed (old -> new), listed in a comment at the top of the file. */
		TMap<FString, FString> Renames;
	};

	/** The syntax tree of a model. Trivia (comments) is filled where the lowering has something to say, such as renamed states. */
	DREAMANIMCOMPILER_API TUniquePtr<Lang::FDabFile> LowerDabModel(const Model::FDabModel& Model, const FDabLowerOptions& Options = FDabLowerOptions());

	/**
	 * Makes every machine, state, conduit, alias, shared rule and cache name an identifier (Docs/Mapping.md,
	 * Lowering rule 7) and rewrites every reference to it. OutRenames, when given, receives old -> new for
	 * each name that changed.
	 */
	DREAMANIMCOMPILER_API void NormalizeDabModelNames(Model::FDabModel& Model, TMap<FString, FString>* OutRenames = nullptr);

	/** Lowering rule 7 for one name: `Rotate Left` -> `RotateLeft`, `(N) RB Movement` -> `NRBMovement`. No clash handling. */
	DREAMANIMCOMPILER_API FString MakeDabIdentifier(const FString& Name);

	// Pieces of a model lowered one at a time, as LowerDabModel writes them: what write-back puts back into a
	// file someone wrote. Blank lines the lowering wants go into TriviaFile's trivia, keyed by the returned nodes.

	/** The name LowerDabNodeCall writes in place of each pose argument. */
	DREAMANIMCOMPILER_API extern const TCHAR* const DabPoseMarker;

	/**
	 * One node of a graph as a call, every pose argument (a pin, a pose list, a keyed blend map, `Inputs`)
	 * replaced by the name DabPoseMarker and SkipPin (the pin a stack feeds) left out. A use of a cache or a
	 * state machine is its name; an input pose is `Input(...)`.
	 */
	DREAMANIMCOMPILER_API Lang::FDabExprPtr LowerDabNodeCall(const Model::FDabModel& Model, const Model::FDabModelGraph& Graph,
		const Model::FDabModelNode& Node, const FString& SkipPin, const FDabLowerOptions& Options = FDabLowerOptions());

	/** A model value as text writes it: the parent alias, assets by name. */
	DREAMANIMCOMPILER_API Lang::FDabExprPtr LowerDabValue(const Model::FDabModel& Model, const Lang::FDabExpr& Value, const FDabLowerOptions& Options = FDabLowerOptions());

	/** `Target.Function(args)`. */
	DREAMANIMCOMPILER_API Lang::FDabExprPtr LowerDabCall(const Model::FDabModel& Model, const Model::FDabModelCall& Call, const FDabLowerOptions& Options = FDabLowerOptions());

	/** `[ Name = Value; ... ]`, or null for no properties. */
	DREAMANIMCOMPILER_API TUniquePtr<Lang::FDabMeta> LowerDabMeta(const Model::FDabModel& Model, const TArray<Model::FDabModelProperty>& Properties,
		const FDabLowerOptions& Options = FDabLowerOptions());

	/** One row of a Transitions table: `From -> To : Condition [ options ];`. */
	DREAMANIMCOMPILER_API TUniquePtr<Lang::FDabTransitionStmt> LowerDabTransition(const Model::FDabModel& Model, const Model::FDabModelTransition& Transition,
		const FDabLowerOptions& Options = FDabLowerOptions());

	/** A graph's statements (input order, caches, named poses, the result under ResultName). */
	DREAMANIMCOMPILER_API TArray<Lang::FDabStmtPtr> LowerDabGraph(const Model::FDabModel& Model, const Model::FDabModelGraph& Graph, const FString& ResultName,
		Lang::FDabFile& TriviaFile, const FDabLowerOptions& Options = FDabLowerOptions());

	/** A whole `StateMachine Name [meta] { ... }` statement. */
	DREAMANIMCOMPILER_API TUniquePtr<Lang::FDabNamedBlockStmt> LowerDabStateMachine(const Model::FDabModel& Model, const Model::FDabModelStateMachine& Machine,
		Lang::FDabFile& TriviaFile, const FDabLowerOptions& Options = FDabLowerOptions());
}
