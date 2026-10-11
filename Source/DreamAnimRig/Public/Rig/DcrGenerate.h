// Copyright DreamMoon. All Rights Reserved.
//
// Generate: build the model into a Control Rig asset.
//
// - A missing asset is created at Model.AssetPath in Model.AssetKind (new rigs default to the independent
//   kind, as 5.8 does). An existing asset is rebuilt in place and keeps its kind; references to it
//   from other assets stay valid.
// - Rebuilding replaces the hierarchy, the member variables and every graph. Library functions are updated
//   in place, never removed and added again: removing a function deletes the call nodes of every loaded
//   rig that calls it. A function the model no longer has is removed, and the result
//   names the loaded rigs that called it.
// - Nodes are named as the model names them, so names from `let` survive a rebuild; the result maps each
//   node path to its node so compiler messages can be placed back in the text.
// - Undo and graph notifications are off while it builds.

#pragma once

#include "CoreMinimal.h"
#include "Model/DcrModel.h"
#include "Rig/DcrAsset.h"

namespace UE::DreamAnim::Rig
{
	struct FDcrGenerateOptions
	{
		/** Save the package after a successful build. */
		bool bSave = false;
		/** Lay the nodes out; otherwise each node goes where the model's Position puts it. */
		bool bAutoLayout = true;
	};

	struct FDcrGenerateResult
	{
		FDcrRig Rig;
		/** Failures to build something the model asked for (a node that would not add, a link that would not connect), and compile errors. */
		TArray<FString> Errors;
		TArray<FString> Warnings;
		/** Loaded rigs whose calls into a removed library function were deleted by the engine. */
		TArray<FString> BrokenCallers;
		bool Succeeded() const { return Rig.IsValid() && Errors.IsEmpty(); }
	};

	DREAMANIMRIG_API FDcrGenerateResult GenerateDcrRig(const Model::FDcrModel& Model, const FDcrGenerateOptions& Options = FDcrGenerateOptions());
}
