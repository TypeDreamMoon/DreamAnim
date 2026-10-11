// Copyright DreamMoon. All Rights Reserved.
//
// Extract: a Control Rig asset (either kind) into the model, literally. Every top-level graph, the function
// library (each function's signature, locals and graph), collapse nodes (as nested graphs), the hierarchy,
// member variables, asset and module settings. Nothing is simplified here: Sequence nodes, reroutes and
// values left on linked pins all come through, and DcrNormalize.h takes them out.
//
// Pin values are kept only where they differ from what a fresh node of the same kind holds; the pin knows
// that itself (URigVMPin::HasOriginalDefaultValue, see FDcrNodeDefaults). Every value that is kept is
// imported through its type and exported again, so one value has one spelling whichever asset it is read from.

#pragma once

#include "CoreMinimal.h"
#include "Model/DcrModel.h"
#include "Rig/DcrAsset.h"

class URigHierarchy;

namespace UE::DreamAnim::Rig
{
	struct FDcrExtractResult
	{
		bool bSuccess = false;
		/** Why it failed: the asset is not a Control Rig, or it has no editor data. Things the model cannot hold go to Model.Unsupported instead. */
		FString Error;
	};

	DREAMANIMRIG_API FDcrExtractResult ExtractDcrModel(const FDcrRig& Rig, Model::FDcrModel& OutModel);

	/**
	 * A hierarchy as the model's elements: what ExtractDcrModel puts in Model.Hierarchy, in the same order and
	 * with the same properties in the same export text, procedural elements left out. What the model cannot hold
	 * is added to OutUnsupported.
	 */
	DREAMANIMRIG_API void ExtractDcrHierarchy(URigHierarchy& Hierarchy, TArray<Model::FDcrModelElement>& OutElements, TArray<Model::FDcrModelUnsupported>& OutUnsupported);
}
