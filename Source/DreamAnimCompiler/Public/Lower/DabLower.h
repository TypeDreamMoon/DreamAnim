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
}
