// Copyright DreamMoon. All Rights Reserved.
//
// What the generator's files share: the build context (the rig, the result, the controllers whose engine
// reports are switched off while it builds) and the helpers that turn the model's strings into engine calls.

#pragma once

#include "CoreMinimal.h"
#include "Model/DcrModel.h"
#include "Rig/DcrAsset.h"
#include "Rig/DcrGenerate.h"
#include "UObject/WeakObjectPtrTemplates.h"

class URigVMController;
class URigVMFunctionLibrary;
class URigVMGraph;
class URigVMNode;
class URigVMPin;
class UScriptStruct;

namespace UE::DreamAnim::Rig::Gen
{
	/** What a graph is, which decides how its interface is made: a function has its signature, a collapse node's is read from its Entry and Return. */
	enum class EGraphRole : uint8
	{
		TopLevel,
		Function,
		Collapse,
	};

	struct FContext
	{
		FContext(const FDcrRig& InRig, const FDcrGenerateOptions& InOptions, FDcrGenerateResult& InResult);
		~FContext();

		FContext(const FContext&) = delete;
		FContext& operator=(const FContext&) = delete;

		FDcrRig Rig;
		FDcrGenerateOptions Options;
		FDcrGenerateResult& Result;
		/** The names the member variables got, by the model's names: an asset may sanitize a name when it adds the variable. */
		TMap<FString, FName> MemberVariables;

		/** The controller of a graph. The engine's own error reports and toasts are off while the generator runs (DreamAnim.Rig.GenerateEngineReports turns them back on). */
		URigVMController* Controller(URigVMGraph* Graph);
		URigVMFunctionLibrary* Library() const;
		URigVMController* LibraryController();

		/** Puts back the reporting state of every controller handed out. */
		void RestoreReporting();

		void Error(const FString& Where, const FString& Message);
		void Warning(const FString& Where, const FString& Message);

	private:
		bool bEngineReports = false;
		TArray<TWeakObjectPtr<URigVMController>> Controllers;
		TArray<bool> ReportingBefore;
	};

	/** The type's object path as a name; none for a built-in type. */
	inline FName TypeObjectPath(const Model::FDcrType& Type)
	{
		return Type.ObjectPath.IsEmpty() ? NAME_None : FName(*Type.ObjectPath);
	}

	/** Whether a pin has exactly this type (the C++ type and the type object's path). */
	bool PinHasType(const URigVMPin* Pin, const Model::FDcrType& Type);

	/** Whether export text reads as the default of its type (empty, `()`, zeros, `False`, `None`). Cheap and approximate: a wrong yes costs nothing, a wrong no costs one extra edit. */
	bool LooksLikeTypeDefault(const FString& Text);

	/** Parses export text into a struct; false (and the reason) when the text does not read as the struct. */
	bool ImportStructText(const UScriptStruct* Struct, const FString& Text, void* Data, FString& OutError);

	/** Empties a graph of everything but its Entry and Return nodes, and breaks the links left between those. */
	void ClearGraph(FContext& Ctx, URigVMGraph* Graph, const FString& Where);

	/** Builds the nodes, values and links of a model graph into a graph that has been cleared. */
	void BuildGraph(FContext& Ctx, URigVMGraph* Graph, const Model::FDcrModelGraph& Source, const FString& Where, EGraphRole Role);

	/** Gives the Entry and Return nodes of a mutable function the execute pin names the model uses (the engine's own is `ExecuteContext`). */
	void ApplyExecutePinName(FContext& Ctx, URigVMController* FunctionController, URigVMGraph* FunctionGraph, const Model::FDcrModelGraph& Source, const FString& Where);

	/** Replaces the hierarchy with the model's. */
	void BuildHierarchy(FContext& Ctx, const Model::FDcrModel& Model);
}
