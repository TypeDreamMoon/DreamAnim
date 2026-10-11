// Copyright DreamMoon. All Rights Reserved.

#include "Rig/DcrHierarchyImport.h"

#include "DcrNames.h"
#include "DcrValues.h"
#include "Engine/SkeletalMesh.h"
#include "Rig/DcrExtract.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogDreamAnimRigImport, Log, All);

namespace UE::DreamAnim::Rig
{
	using namespace UE::DreamAnim::Model;

	bool ImportDcrHierarchy(const FString& MeshPath, TArray<FDcrModelElement>& OutElements)
	{
		OutElements.Reset();
		USkeletalMesh* Mesh = MeshPath.IsEmpty() ? nullptr : LoadObject<USkeletalMesh>(nullptr, *MeshPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!Mesh || !Mesh->GetSkeleton())
		{
			return false;
		}
		URigHierarchy* Hierarchy = NewObject<URigHierarchy>(GetTransientPackage(), NAME_None, RF_Transient);
		URigHierarchyController* Controller = Hierarchy->GetController(true);
		if (!Controller)
		{
			return false;
		}
		// As the Control Rig editor's import: the skeleton's bones, then the mesh's curves.
		Controller->ImportBones(Mesh->GetSkeleton(), NAME_None, true, true, false, false, false);
		Controller->ImportCurvesFromSkeletalMesh(Mesh, NAME_None, false, false, false);
		TArray<FDcrModelUnsupported> Unsupported;
		ExtractDcrHierarchy(*Hierarchy, OutElements, Unsupported);
		return true;
	}

	FDcrHierarchyImportMatch MatchDcrHierarchyImport(const FString& MeshPath, TConstArrayView<FDcrModelElement> Hierarchy)
	{
		FDcrHierarchyImportMatch Match;
		TArray<FDcrModelElement> Imported;
		if (!ImportDcrHierarchy(MeshPath, Imported) || Imported.IsEmpty())
		{
			return Match;
		}
		auto Key = [](const FDcrModelElement& Element) { return FString::Printf(TEXT("%s(%s)"), LexToString(Element.Kind), *Element.Name); };
		TMap<FString, int32> Index;
		for (int32 Position = 0; Position < Hierarchy.Num(); ++Position)
		{
			Index.Add(Key(Hierarchy[Position]), Position);
		}

		for (const FDcrModelElement& Element : Imported)
		{
			const int32* Position = Index.Find(Key(Element));
			if (!Position)
			{
				UE_LOG(LogDreamAnimRigImport, Log, TEXT("Importing %s gives %s, which the rig does not have; its hierarchy is written out."), *MeshPath, *Key(Element));
				return FDcrHierarchyImportMatch();
			}
			const FDcrModelElement& Own = Hierarchy[*Position];
			if (Own.Parent != Element.Parent || (!Own.Parent.IsEmpty() && Own.ParentKind != Element.ParentKind))
			{
				UE_LOG(LogDreamAnimRigImport, Log, TEXT("Importing %s gives %s under '%s'; the rig has it under '%s'. Its hierarchy is written out."),
					*MeshPath, *Key(Element), *Element.Parent, *Own.Parent);
				return FDcrHierarchyImportMatch();
			}

			// What the rig holds that the import does not hold the same. A property the import has and the rig
			// does not is the rig's default; written so it replaces the imported one.
			TArray<FDcrValue> Differing;
			for (const FDcrValue& Value : Own.Properties)
			{
				const FDcrValue* Theirs = Element.Properties.FindByPredicate([&Value](const FDcrValue& Candidate) { return Candidate.Name == Value.Name; });
				// A transform the import works out a few units in the last place off is the import's.
				const bool bNear = Theirs && Value.Name == TEXT("Transform")
					&& AreDcrValueTextsNear(MakeDcrType(*TBaseStructure<FTransform>::Get()), Theirs->Value, Value.Value, DcrRoundingTolerance);
				if (!Theirs || (Theirs->Value != Value.Value && !bNear))
				{
					Differing.Add(Value);
				}
			}
			for (const FDcrValue& Value : Element.Properties)
			{
				const bool bNearDefault = Value.Name == TEXT("Transform")
					&& AreDcrValueTextsNear(MakeDcrType(*TBaseStructure<FTransform>::Get()), Value.Value, FString(), DcrRoundingTolerance);
				if (!bNearDefault && !Own.Properties.ContainsByPredicate([&Value](const FDcrValue& Candidate) { return Candidate.Name == Value.Name; }))
				{
					Differing.Add({ Value.Name, FString() });
				}
			}
			if (Differing.IsEmpty())
			{
				Match.Exact.Add(*Position);
			}
			else
			{
				Match.Refined.Add(*Position);
				Match.RefinedProperties.Add(MoveTemp(Differing));
			}
		}
		return Match;
	}
}
