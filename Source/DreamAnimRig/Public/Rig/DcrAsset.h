// Copyright DreamMoon. All Rights Reserved.
//
// One handle on a Control Rig asset, whichever kind it is. 5.8 has two: the Blueprint-based
// UControlRigBlueprint, and UControlRigRuntimeAsset whose graphs live on its editor data
// (UControlRigEditorAsset). Both implement IRigVMEditorAssetInterface and IControlRigEditorAssetInterface,
// and everything here goes through those two. The few calls the interfaces keep protected (adding a
// graph, compiling) are dispatched on the asset class in DcrAsset.cpp, and nowhere else.

#pragma once

#include "CoreMinimal.h"
#include "ControlRigEditorAsset.h"
#include "Misc/OutputDevice.h"
#include "RigVMEditorAsset.h"

class UControlRig;
class URigVMController;
class URigVMGraph;
class URigVMNode;

namespace UE::DreamAnim::Rig
{
	enum class EDcrAssetKind : uint8
	{
		/** UControlRigRuntimeAsset with UControlRigEditorAsset editor data: what 5.8 creates by default. */
		Independent,
		/** UControlRigBlueprint. */
		Legacy,
	};

	DREAMANIMRIG_API const TCHAR* LexToString(EDcrAssetKind Kind);

	struct FDcrRig
	{
		/** UControlRigBlueprint, or UControlRigRuntimeAsset. */
		UObject* Asset = nullptr;
		/** The object holding the graphs: the Blueprint itself, or the runtime asset's editor data. */
		UObject* Editor = nullptr;
		FControlRigAssetInterfacePtr ControlRig;
		FRigVMEditorAssetInterfacePtr RigVM;

		bool IsValid() const { return Asset && ControlRig.GetInterface() && RigVM.GetInterface(); }
		DREAMANIMRIG_API EDcrAssetKind GetKind() const;
	};

	DREAMANIMRIG_API FDcrRig WrapDcrRig(UObject* Asset);
	/** A new asset at PackagePath (`/Game/Rigs/CR_Hero`). The factories pick a unique name when one is taken. */
	DREAMANIMRIG_API FDcrRig CreateDcrRig(const FString& PackagePath, EDcrAssetKind Kind);
	DREAMANIMRIG_API FDcrRig LoadDcrRig(const FString& ObjectPath);
	DREAMANIMRIG_API FDcrRig DuplicateDcrRig(const FDcrRig& Source, const FString& Folder, const FString& Name);
	DREAMANIMRIG_API bool SaveDcrRig(const FDcrRig& Rig);

	DREAMANIMRIG_API URigVMController* GetDcrController(const FDcrRig& Rig, URigVMGraph* Graph);
	/** Adds a top-level graph (a model). The interface keeps AddModel protected; both asset classes expose it. */
	DREAMANIMRIG_API URigVMGraph* AddDcrModelGraph(const FDcrRig& Rig, const FString& Name);
	/** A top-level graph by its graph name (`RigVMModel`, `ConstructionGraph`). */
	DREAMANIMRIG_API URigVMGraph* FindDcrModelGraph(const FDcrRig& Rig, const FString& Name);
	/** Every graph the rig has (top-level, function library, functions, collapse nodes), sorted by node path. */
	DREAMANIMRIG_API TArray<URigVMGraph*> GetAllDcrGraphs(const FDcrRig& Rig);
	DREAMANIMRIG_API URigVMNode* FindDcrEventNode(URigVMGraph* Graph, const FName& EventName);

	/**
	 * Compiles: a legacy asset through the Blueprint compiler (its generated class carries the variables),
	 * the other kind through RecompileVM. Returns the errors it logged or reported.
	 */
	DREAMANIMRIG_API TArray<FString> CompileDcrRig(const FDcrRig& Rig, int32* OutWarnings = nullptr);

	/** Collects the warnings and errors logged while it lives. */
	class DREAMANIMRIG_API FDcrLogCapture : public FOutputDevice
	{
	public:
		FDcrLogCapture();
		virtual ~FDcrLogCapture() override;
		virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override;
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		TArray<FString> GetErrors() const;
		TArray<FString> GetWarnings() const;

	private:
		mutable FCriticalSection Lock;
		TArray<TPair<ELogVerbosity::Type, FString>> Lines;
	};
}
