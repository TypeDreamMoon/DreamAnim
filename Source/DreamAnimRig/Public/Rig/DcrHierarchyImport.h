// Copyright DreamMoon. All Rights Reserved.
//
// The hierarchy's `Import = Mesh;`: the bones of the mesh's skeleton and the mesh's curves, imported the way
// the Control Rig editor imports them, and written as the extractor writes elements. Lowering writes it when a
// rig's hierarchy holds these; semantic expands it into them.
//
// A rig imported long ago may hold the same bones with initial transforms a few ulps away from what importing
// gives today. Such an element is still the import's, refined: the text writes it after `Import` with only the
// properties that differ, and semantic replaces the imported element's properties with them.

#pragma once

#include "CoreMinimal.h"
#include "Model/DcrModel.h"

namespace UE::DreamAnim::Rig
{
	/**
	 * The elements importing a skeletal mesh gives, in the order a rig holding only them would list them.
	 * False when MeshPath is not a skeletal mesh with a skeleton.
	 */
	DREAMANIMRIG_API bool ImportDcrHierarchy(const FString& MeshPath, TArray<Model::FDcrModelElement>& OutElements);

	struct FDcrHierarchyImportMatch
	{
		/** Elements of the rig importing gives exactly, by index. */
		TArray<int32> Exact;
		/** Elements importing gives with the same kind, name and parent but other properties, by index. */
		TArray<int32> Refined;
		/** For each refined element, the properties it holds that the imported one does not hold the same. */
		TArray<TArray<Model::FDcrValue>> RefinedProperties;

		bool IsEmpty() const { return Exact.IsEmpty() && Refined.IsEmpty(); }
	};

	/**
	 * How Hierarchy holds what importing MeshPath gives. Empty unless every imported element is there, of the same
	 * kind and under the same parent.
	 */
	DREAMANIMRIG_API FDcrHierarchyImportMatch MatchDcrHierarchyImport(const FString& MeshPath, TConstArrayView<Model::FDcrModelElement> Hierarchy);
}
