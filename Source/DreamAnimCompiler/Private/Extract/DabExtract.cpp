// Copyright DreamMoon. All Rights Reserved.

#include "Extract/DabExtract.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimClassInterface.h"
#include "Animation/Skeleton.h"
#include "AnimationGraph.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "DreamAnimLibrary.h"
#include "Extract/DabExtractPrivate.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Compiler::ExtractPrivate
{
	namespace
	{
		void ExtractClassSettings(FDabExtractContext& Ctx)
		{
			// Settings are what the Class Settings panel edits; bIsTemplate is not editable there but decides
			// whether the Blueprint needs a skeleton, so it counts too.
			static const FName TargetSkeletonName(TEXT("TargetSkeleton"));
			static const FName IsTemplateName(TEXT("bIsTemplate"));

			const UAnimBlueprint* Defaults = GetDefault<UAnimBlueprint>();
			for (TFieldIterator<FProperty> It(UAnimBlueprint::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
			{
				const FProperty* Property = *It;
				const FName Name = Property->GetFName();
				const bool bSetting = Property->HasAnyPropertyFlags(CPF_Edit) || Name == IsTemplateName;
				if (!bSetting || Name == TargetSkeletonName || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
				{
					continue;
				}
				const void* Value = Property->ContainerPtrToValuePtr<void>(&Ctx.Blueprint);
				const void* Default = Property->ContainerPtrToValuePtr<void>(Defaults);
				if (Property->Identical(Value, Default, 0))
				{
					continue;
				}
				if (Lang::FDabExprPtr Converted = ConvertPropertyValue(Ctx, *Property, Value, Default, FString::Printf(TEXT("ClassSettings.%s"), *Name.ToString())))
				{
					Model::FDabModelProperty Setting;
					Setting.Name = Name.ToString();
					Setting.Value = MoveTemp(Converted);
					Ctx.OutModel.ClassSettings.Add(MoveTemp(Setting));
				}
			}
		}

		/**
		 * The class defaults this Blueprint sets on properties its parent class declares, as the Class Defaults
		 * panel shows them: editable, not transient, compared with the parent's defaults.
		 */
		void ExtractClassDefaults(FDabExtractContext& Ctx)
		{
			const UClass* Generated = Ctx.Blueprint.GeneratedClass;
			const UClass* Parent = Ctx.Blueprint.ParentClass;
			const UObject* Defaults = Generated ? Generated->GetDefaultObject(false) : nullptr;
			const UObject* ParentDefaults = Parent ? Parent->GetDefaultObject() : nullptr;
			if (!Defaults || !ParentDefaults)
			{
				return;
			}
			for (TFieldIterator<FProperty> It(Parent); It; ++It)
			{
				const FProperty* Property = *It;
				if (!Property->HasAnyPropertyFlags(CPF_Edit) || Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DisableEditOnTemplate)
					|| Property->GetOwnerClass() == UObject::StaticClass())
				{
					continue;
				}
				// An inherited animation set is the child's DefaultAnimSet setting, read with the variables.
				if (Property->GetFName() == UDreamAnimLibrary::ActiveSetVariable || Property->GetFName() == UDreamAnimLibrary::PendingSetVariable)
				{
					continue;
				}
				const void* Value = Property->ContainerPtrToValuePtr<void>(Defaults);
				const void* Default = Property->ContainerPtrToValuePtr<void>(ParentDefaults);
				if (Property->Identical(Value, Default, 0))
				{
					continue;
				}
				if (Lang::FDabExprPtr Converted = ConvertPropertyValue(Ctx, *Property, Value, Default, FString::Printf(TEXT("Defaults.%s"), *Property->GetName())))
				{
					Model::FDabModelProperty Entry;
					Entry.Name = Property->GetName();
					Entry.Value = MoveTemp(Converted);
					Ctx.OutModel.ClassDefaults.Add(MoveTemp(Entry));
				}
			}
		}

		/**
		 * The animation set slots, from the getters the generator makes (`DreamAnimAsset_<Slot>`). The declared
		 * type is in the getter's keywords; failing that, its return type stands in.
		 */
		void ExtractAssetSlots(FDabExtractContext& Ctx)
		{
			static const FString GetterPrefix(TEXT("DreamAnimAsset_"));
			static const FString KeywordPrefix(TEXT("DreamAnimSlot "));
			for (const TPair<FName, const UEdGraph*>& Function : Ctx.FunctionGraphs)
			{
				const FString FunctionName = Function.Key.ToString();
				if (!FunctionName.StartsWith(GetterPrefix, ESearchCase::CaseSensitive) || !Function.Value)
				{
					continue;
				}
				Model::FDabModelAssetSlot Slot;
				Slot.Name = FunctionName.RightChop(GetterPrefix.Len());
				for (const UEdGraphNode* Node : Function.Value->Nodes)
				{
					if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
					{
						const FString Keywords = Entry->MetaData.Keywords.ToString();
						if (Keywords.StartsWith(KeywordPrefix, ESearchCase::CaseSensitive))
						{
							Slot.Type = Keywords.RightChop(KeywordPrefix.Len());
						}
					}
					else if (const UK2Node_FunctionResult* Result = Cast<UK2Node_FunctionResult>(Node); Result && Slot.Type.IsEmpty())
					{
						const UEdGraphPin* ReturnPin = Result->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Input);
						const UClass* ReturnClass = ReturnPin ? Cast<UClass>(ReturnPin->PinType.PinSubCategoryObject.Get()) : nullptr;
						Slot.Type = ReturnClass ? ReturnClass->GetName() : TEXT("AnimationAsset");
					}
				}
				Ctx.UsedFunctions.Add(Function.Key);
				Ctx.OutModel.AssetSlots.Add(MoveTemp(Slot));
			}
			Ctx.OutModel.AssetSlots.Sort([](const Model::FDabModelAssetSlot& A, const Model::FDabModelAssetSlot& B) { return A.Name < B.Name; });
		}

		/**
		 * A graph's input poses in the order its compiled function takes them; a Blueprint that never compiled
		 * falls back to the order of its input pose nodes, which is what the compiler would use.
		 */
		void ReadInputPoseOrder(FDabExtractContext& Ctx, FName FunctionName, Model::FDabModelGraph& Graph)
		{
			const UClass* Class = Ctx.Blueprint.GeneratedClass ? Ctx.Blueprint.GeneratedClass.Get() : Ctx.Blueprint.SkeletonGeneratedClass.Get();
			const IAnimClassInterface* AnimClass = Class ? IAnimClassInterface::GetFromClass(Class) : nullptr;
			if (AnimClass)
			{
				for (const FAnimBlueprintFunction& Function : AnimClass->GetAnimBlueprintFunctions())
				{
					if (Function.Name == FunctionName)
					{
						for (const FName& Input : Function.InputPoseNames)
						{
							Graph.InputPoses.Add(Input.ToString());
						}
						return;
					}
				}
			}
			for (const Model::FDabModelNode& Node : Graph.Nodes)
			{
				if (!Node.InputPoseName.IsEmpty())
				{
					Graph.InputPoses.AddUnique(Node.InputPoseName);
				}
			}
		}

		void ExtractVariables(FDabExtractContext& Ctx)
		{
			const UAnimBlueprint& Blueprint = Ctx.Blueprint;

			// The generated class's default object holds the compiled defaults; the skeleton class stands in when
			// the Blueprint has never compiled.
			const UClass* DefaultsClass = Blueprint.GeneratedClass ? Blueprint.GeneratedClass.Get() : Blueprint.SkeletonGeneratedClass.Get();
			const UObject* Defaults = DefaultsClass ? DefaultsClass->GetDefaultObject() : nullptr;
			const FString DefaultCategory = UEdGraphSchema_K2::VR_DefaultCategory.ToString();
			// A child can override the inherited active set without declaring a new variable. A set it only
			// inherits belongs to its parent's text, not to its own.
			const FObjectPropertyBase* SetProperty = DefaultsClass ? FindFProperty<FObjectPropertyBase>(DefaultsClass, UDreamAnimLibrary::ActiveSetVariable) : nullptr;
			const UObject* DefaultSet = SetProperty && Defaults ? SetProperty->GetObjectPropertyValue_InContainer(Defaults) : nullptr;
			const UClass* ParentClass = Blueprint.ParentClass;
			const bool bInherited = SetProperty && ParentClass && ParentClass->IsChildOf(SetProperty->GetOwnerClass());
			const UObject* ParentSet = bInherited ? SetProperty->GetObjectPropertyValue_InContainer(ParentClass->GetDefaultObject()) : nullptr;
			if (DefaultSet && (!bInherited || DefaultSet != ParentSet))
			{
				Ctx.OutModel.DefaultAnimSet = DefaultSet->GetPathName();
			}

			for (const FBPVariableDescription& Description : Blueprint.NewVariables)
			{
				if (Description.VarName == UDreamAnimLibrary::ActiveSetVariable || Description.VarName == UDreamAnimLibrary::PendingSetVariable)
				{
					continue;
				}
				Model::FDabModelVariable Variable;
				Variable.Name = Description.VarName.ToString();
				Variable.Type = GetPinTypeSpelling(Description.VarType);

				const FString Location = FString::Printf(TEXT("Variables.%s"), *Variable.Name);
				const FProperty* Property = DefaultsClass ? FindFProperty<FProperty>(DefaultsClass, Description.VarName) : nullptr;
				if (Property && Defaults)
				{
					Variable.Default = ConvertPropertyValue(Ctx, *Property, Property->ContainerPtrToValuePtr<void>(Defaults), nullptr, Location);
				}
				else
				{
					Ctx.AddUnsupported(Location, TEXT("the variable has no compiled default; compile the Blueprint first"));
				}

				const FString Category = Description.Category.ToString();
				if (!Category.IsEmpty() && Category != DefaultCategory)
				{
					Model::FDabModelProperty Group;
					Group.Name = TEXT("Group");
					Group.Value = MakeString(Category);
					Variable.Meta.Add(MoveTemp(Group));
				}

				Ctx.OutModel.Variables.Add(MoveTemp(Variable));
			}
		}

		bool IsAnimGraph(const UEdGraph* Graph)
		{
			return Graph && Graph->IsA<UAnimationGraph>();
		}
	}

	static bool ExtractBlueprint(const UAnimBlueprint& Blueprint, Model::FDabModel& OutModel, Lang::FDabDiagnosticSink& Sink, const FDabExtractOptions& Options)
	{
		OutModel = Model::FDabModel();
		FDabExtractContext Ctx(Blueprint, OutModel, Options);

		if (Blueprint.SkeletonGeneratedClass)
		{
			Ctx.InstanceClass = Blueprint.SkeletonGeneratedClass.Get();
		}
		else if (Blueprint.GeneratedClass)
		{
			Ctx.InstanceClass = Blueprint.GeneratedClass.Get();
		}
		else
		{
			Ctx.InstanceClass = Blueprint.ParentClass.Get();
		}
		if (!Ctx.InstanceClass)
		{
			Sink.Error(TEXT("DAB8101"), Lang::FDabSpan(),
				FString::Printf(TEXT("'%s' has neither a generated class nor a parent class; there is nothing to read paths against."), *Blueprint.GetPathName()),
				TEXT("Open the Blueprint in the editor and compile it, or fix its parent class."));
			return false;
		}

		const bool bInterface = Blueprint.BlueprintType == BPTYPE_Interface;
		OutModel.Kind = bInterface ? Lang::EDabDocumentKind::AnimLayerInterface : Lang::EDabDocumentKind::AnimBlueprint;
		OutModel.AssetPath = Blueprint.GetPathName();
		OutModel.ParentClass = Blueprint.ParentClass ? Blueprint.ParentClass->GetPathName() : FString();
		OutModel.Skeleton = Blueprint.TargetSkeleton ? Blueprint.TargetSkeleton->GetPathName() : FString();
		OutModel.AnimGraph.Name = UEdGraphSchema_K2::GN_AnimGraph.ToString();

		for (const FBPInterfaceDescription& Interface : Blueprint.ImplementedInterfaces)
		{
			if (Interface.Interface)
			{
				OutModel.Interfaces.Add(GetClassAssetPath(Interface.Interface.Get()));
			}
		}

		for (const FBPVariableDescription& Description : Blueprint.NewVariables)
		{
			Ctx.BlueprintVariables.Add(Description.VarName);
		}
		for (const TObjectPtr<UEdGraph>& Graph : Blueprint.FunctionGraphs)
		{
			if (Graph && !IsAnimGraph(Graph.Get()))
			{
				Ctx.FunctionGraphs.Add(Graph->GetFName(), Graph.Get());
			}
		}
		ExtractAssetSlots(Ctx);

		ExtractClassSettings(Ctx);
		ExtractClassDefaults(Ctx);
		ExtractVariables(Ctx);

		// Machines are named before any graph is read: a condition may name a machine extracted later.
		for (const TObjectPtr<UEdGraph>& Graph : Blueprint.FunctionGraphs)
		{
			if (IsAnimGraph(Graph.Get()))
			{
				CollectStateMachineNames(Ctx, *Graph);
			}
		}
		for (const FBPInterfaceDescription& Interface : Blueprint.ImplementedInterfaces)
		{
			for (const TObjectPtr<UEdGraph>& Graph : Interface.Graphs)
			{
				if (IsAnimGraph(Graph.Get()))
				{
					CollectStateMachineNames(Ctx, *Graph);
				}
			}
		}

		// The AnimGraph and the Blueprint's own layers.
		for (const TObjectPtr<UEdGraph>& Graph : Blueprint.FunctionGraphs)
		{
			if (!IsAnimGraph(Graph.Get()))
			{
				continue;
			}
			const FString Name = Graph->GetName();
			if (bInterface)
			{
				// An interface only declares its layers: the root's settings, no poses.
				Model::FDabModelLayer Layer;
				Layer.Name = Name;
				Layer.bDeclarationOnly = true;
				Layer.Graph.Name = Name;
				ExtractResultProperties(Ctx, *Graph, Name, Layer.Graph);
				OutModel.Layers.Add(MoveTemp(Layer));
			}
			else if (Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph)
			{
				ExtractAnimGraph(Ctx, *Graph, Name, OutModel.AnimGraph);
			}
			else
			{
				Model::FDabModelLayer Layer;
				Layer.Name = Name;
				Layer.Graph.Name = Name;
				ExtractAnimGraph(Ctx, *Graph, Name, Layer.Graph);
				OutModel.Layers.Add(MoveTemp(Layer));
			}
		}

		// Layers implemented for an interface.
		for (const FBPInterfaceDescription& Interface : Blueprint.ImplementedInterfaces)
		{
			for (const TObjectPtr<UEdGraph>& Graph : Interface.Graphs)
			{
				if (!Graph)
				{
					continue;
				}
				if (!IsAnimGraph(Graph.Get()))
				{
					Ctx.AddUnsupported(Graph->GetName(), TEXT("an interface function that is not an animation layer"));
					continue;
				}
				Model::FDabModelLayer Layer;
				Layer.Name = Graph->GetName();
				Layer.Graph.Name = Layer.Name;
				ExtractAnimGraph(Ctx, *Graph, Layer.Name, Layer.Graph);
				OutModel.Layers.Add(MoveTemp(Layer));
			}
		}

		ReadInputPoseOrder(Ctx, UEdGraphSchema_K2::GN_AnimGraph, OutModel.AnimGraph);
		for (Model::FDabModelLayer& Layer : OutModel.Layers)
		{
			if (!Layer.bDeclarationOnly)
			{
				ReadInputPoseOrder(Ctx, FName(*Layer.Name), Layer.Graph);
			}
		}

		for (const TObjectPtr<UEdGraph>& Graph : Blueprint.UbergraphPages)
		{
			if (Graph)
			{
				ExtractEventGraph(Ctx, *Graph);
			}
		}

		for (const TObjectPtr<UEdGraph>& Graph : Blueprint.MacroGraphs)
		{
			if (Graph)
			{
				Ctx.AddUnsupported(Graph->GetName(), TEXT("a macro graph is Blueprint logic; move it to C++"));
			}
		}
		for (const TObjectPtr<UEdGraph>& Graph : Blueprint.DelegateSignatureGraphs)
		{
			if (Graph)
			{
				Ctx.AddUnsupported(Graph->GetName(), TEXT("an event dispatcher is Blueprint logic; move it to C++"));
			}
		}

		// Functions that something forwarded through became calls; any other function is logic the text cannot hold.
		for (const TPair<FName, const UEdGraph*>& Function : Ctx.FunctionGraphs)
		{
			if (!Ctx.UsedFunctions.Contains(Function.Key))
			{
				Ctx.AddUnsupported(Function.Key.ToString(), TEXT("a Blueprint function no state or node function forwards through; move it to C++"));
			}
		}

		return true;
	}
}

namespace UE::DreamAnim::Compiler
{
	bool ExtractDabModel(const UAnimBlueprint& Blueprint, Model::FDabModel& OutModel, Lang::FDabDiagnosticSink& Sink, const FDabExtractOptions& Options)
	{
		return ExtractPrivate::ExtractBlueprint(Blueprint, OutModel, Sink, Options);
	}
}
