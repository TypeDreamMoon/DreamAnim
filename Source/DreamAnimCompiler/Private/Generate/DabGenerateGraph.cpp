// Copyright DreamMoon. All Rights Reserved.
//
// Pose graphs: model nodes -> anim graph nodes with their properties, pose links, pin defaults and
// Property Access bindings. The order inside one node matters and follows what the editor does when a
// user places and edits it: values that shape pins go in before the pins are made, the node is placed
// (Finalize), whatever PostPlacedNewNode changed is put back, and bindings come last, after the pose
// links, because linking a pin clears its bindings.

#include "Generate/DabGenerateInternal.h"

#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_CustomProperty.h"
#include "AnimGraphNode_LinkedAnimGraph.h"
#include "AnimGraphNode_LinkedAnimLayer.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimNodes/AnimNode_BlendListBase.h"
#include "AnimNodes/AnimNode_LayeredBoneBlend.h"
#include "AnimNodes/AnimNode_MultiWayBlend.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimNodeBase.h"
#include "AnimationGraphSchema.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Lower/DabLower.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Compiler::GeneratePrivate
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	static UClass* DabGenFindNodeClass(const FString& ClassName)
	{
		UClass* Class = FindObject<UClass>(nullptr, *(TEXT("/Script/AnimGraph.") + ClassName));
		if (!Class)
		{
			Class = FindFirstObject<UClass>(ClassName, EFindFirstObjectOptions::NativeFirst);
		}
		if (!Class && ClassName.StartsWith(TEXT("U")))
		{
			Class = FindFirstObject<UClass>(ClassName.RightChop(1), EFindFirstObjectOptions::NativeFirst);
		}
		if (!Class || !Class->IsChildOf(UAnimGraphNode_Base::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract))
		{
			return nullptr;
		}
		return Class;
	}

	static UClass* DabGenCallFunctionNodeClass()
	{
		// UAnimGraphNode_CallFunction's header is private to AnimGraph.
		return FindObject<UClass>(nullptr, TEXT("/Script/AnimGraph.AnimGraphNode_CallFunction"));
	}

	/** `Poses_2` -> (`Poses`, 2). */
	static bool DabGenSplitIndexedName(const FString& Name, FString& OutBase, int32& OutIndex)
	{
		int32 Underscore = INDEX_NONE;
		if (!Name.FindLastChar(TEXT('_'), Underscore) || Underscore == 0 || Underscore == Name.Len() - 1)
		{
			return false;
		}
		const FString Suffix = Name.Mid(Underscore + 1);
		for (const TCHAR Character : Suffix)
		{
			if (!FChar::IsDigit(Character))
			{
				return false;
			}
		}
		OutBase = Name.Left(Underscore);
		OutIndex = FCString::Atoi(*Suffix);
		return true;
	}

	static bool DabGenIsPoseLinkProperty(const FProperty* Property)
	{
		const FStructProperty* StructProperty = CastField<FStructProperty>(Property);
		return StructProperty && StructProperty->Struct->IsChildOf(FPoseLinkBase::StaticStruct());
	}

	static bool DabGenIsPosePin(const UEdGraphPin* Pin)
	{
		return Pin && UAnimationGraphSchema::IsPosePin(Pin->PinType);
	}

	/** How many entries each pose array needs: one past the highest pin the model lists (linked or not). */
	static TMap<FName, int32> DabGenCollectPoseArraySizes(UAnimGraphNode_Base* Node, const FDabModelNode& Model)
	{
		TMap<FName, int32> Sizes;
		const UScriptStruct* NodeStruct = Node->GetFNodeType();
		if (!NodeStruct)
		{
			return Sizes;
		}
		for (const FDabModelPoseLink& Link : Model.PoseInputs)
		{
			FString Base;
			int32 Index = INDEX_NONE;
			if (!DabGenSplitIndexedName(Link.PinName, Base, Index))
			{
				continue;
			}
			const FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(NodeStruct, FName(*Base));
			if (ArrayProperty && DabGenIsPoseLinkProperty(ArrayProperty->Inner))
			{
				int32& Size = Sizes.FindOrAdd(ArrayProperty->GetFName());
				Size = FMath::Max(Size, Index + 1);
			}
		}
		return Sizes;
	}

	/**
	 * Grows or shrinks a pose array and the arrays that run parallel to it (one entry per pose) by one entry.
	 * New parallel entries copy the last one, as the engine's own AddPose functions mostly do.
	 */
	static void DabGenResizeParallelArrays(const UScriptStruct* NodeStruct, void* NodeData, const FArrayProperty* PoseArray, int32 NewCount)
	{
		FScriptArrayHelper Poses(PoseArray, PoseArray->ContainerPtrToValuePtr<void>(NodeData));
		const int32 OldCount = Poses.Num();
		for (TFieldIterator<FProperty> It(NodeStruct); It; ++It)
		{
			const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(*It);
			if (!ArrayProperty || ArrayProperty == PoseArray)
			{
				continue;
			}
			FScriptArrayHelper Parallel(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(NodeData));
			if (Parallel.Num() != OldCount || OldCount == 0)
			{
				continue;
			}
			if (NewCount < OldCount)
			{
				Parallel.RemoveValues(NewCount, OldCount - NewCount);
			}
			else
			{
				for (int32 Index = OldCount; Index < NewCount; ++Index)
				{
					const int32 Added = Parallel.AddValue();
					ArrayProperty->Inner->CopySingleValue(Parallel.GetRawPtr(Added), Parallel.GetRawPtr(OldCount - 1));
				}
			}
		}
		if (NewCount < OldCount)
		{
			Poses.RemoveValues(NewCount, OldCount - NewCount);
		}
		else if (NewCount > OldCount)
		{
			Poses.AddValues(NewCount - OldCount);
		}
	}

	/** One AddPose / RemovePose step through the runtime node's own function where the engine has one. */
	static void DabGenStepPoseArray(const UScriptStruct* NodeStruct, FAnimNode_Base* NodeData, const FArrayProperty* PoseArray, bool bGrow)
	{
		FScriptArrayHelper Poses(PoseArray, PoseArray->ContainerPtrToValuePtr<void>(NodeData));
		const int32 Count = Poses.Num();
		const FName ArrayName = PoseArray->GetFName();

		if (NodeStruct->IsChildOf(FAnimNode_BlendListBase::StaticStruct()) && ArrayName == TEXT("BlendPose"))
		{
			// Virtual: a derived node (ALS's Blend Poses by Gameplay Tag) keeps its own arrays in step.
			FAnimNode_BlendListBase* BlendList = static_cast<FAnimNode_BlendListBase*>(NodeData);
			if (bGrow)
			{
				BlendList->AddPose();
			}
			else
			{
				BlendList->RemovePose(Count - 1);
			}
		}
		else if (NodeStruct->IsChildOf(FAnimNode_LayeredBoneBlend::StaticStruct()) && ArrayName == TEXT("BlendPoses"))
		{
			FAnimNode_LayeredBoneBlend* Layered = static_cast<FAnimNode_LayeredBoneBlend*>(NodeData);
			if (bGrow)
			{
				Layered->AddPose();
			}
			else
			{
				Layered->RemovePose(Count - 1);
			}
		}
		else if (NodeStruct->IsChildOf(FAnimNode_MultiWayBlend::StaticStruct()) && ArrayName == TEXT("Poses"))
		{
			FAnimNode_MultiWayBlend* MultiWay = static_cast<FAnimNode_MultiWayBlend*>(NodeData);
			if (bGrow)
			{
				MultiWay->AddPose();
			}
			else
			{
				MultiWay->RemovePose(Count - 1);
			}
		}

		if (Poses.Num() == Count)
		{
			DabGenResizeParallelArrays(NodeStruct, NodeData, PoseArray, bGrow ? Count + 1 : Count - 1);
		}
	}

	static void DabGenResizePoseArrays(UAnimGraphNode_Base* Node, const TMap<FName, int32>& Sizes)
	{
		const UScriptStruct* NodeStruct = Node->GetFNodeType();
		FAnimNode_Base* NodeData = Node->GetFNode();
		if (!NodeStruct || !NodeData)
		{
			return;
		}
		for (const TPair<FName, int32>& Size : Sizes)
		{
			const FArrayProperty* PoseArray = FindFProperty<FArrayProperty>(NodeStruct, Size.Key);
			if (!PoseArray)
			{
				continue;
			}
			FScriptArrayHelper Poses(PoseArray, PoseArray->ContainerPtrToValuePtr<void>(NodeData));
			for (int32 Guard = 0; Poses.Num() != Size.Value && Guard < 1024; ++Guard)
			{
				DabGenStepPoseArray(NodeStruct, NodeData, PoseArray, Poses.Num() < Size.Value);
			}
		}
	}

	/** Pins of array elements that no longer exist would survive reconstruction as orphans; let them go. */
	static void DabGenDropSurplusPins(UAnimGraphNode_Base* Node)
	{
		const UScriptStruct* NodeStruct = Node->GetFNodeType();
		const FAnimNode_Base* NodeData = Node->GetFNode();
		if (!NodeStruct || !NodeData)
		{
			return;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			FString Base;
			int32 Index = INDEX_NONE;
			if (!Pin || !DabGenSplitIndexedName(Pin->PinName.ToString(), Base, Index))
			{
				continue;
			}
			const FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(NodeStruct, FName(*Base));
			if (!ArrayProperty)
			{
				continue;
			}
			FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(NodeData));
			if (Index >= Helper.Num())
			{
				Pin->SetSavePinIfOrphaned(false);
			}
		}
	}

	static UObject* DabGenGetBindingObject(UAnimGraphNode_Base* Node)
	{
		// The Binding member is private and its class lives in AnimGraph's Internal headers.
		const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(UAnimGraphNode_Base::StaticClass(), TEXT("Binding"));
		return Property ? Property->GetObjectPropertyValue_InContainer(Node) : nullptr;
	}

	static bool DabGenShowNodePin(UAnimGraphNode_Base* Node, FName PropertyName)
	{
		const int32 Index = Node->ShowPinForProperties.IndexOfByPredicate([PropertyName](const FOptionalPinFromProperty& Entry)
		{
			return Entry.PropertyName == PropertyName;
		});
		if (Index == INDEX_NONE)
		{
			return false;
		}
		if (!Node->ShowPinForProperties[Index].bShowPin)
		{
			Node->SetPinVisibility(true, Index);
		}
		return true;
	}

	static bool DabGenShowCustomPin(UAnimGraphNode_Base* Node, FName PropertyName)
	{
		// CustomPinProperties and SetCustomPinVisibility are protected; the array is reflected.
		const FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(Node->GetClass(), TEXT("CustomPinProperties"));
		const FStructProperty* Inner = ArrayProperty ? CastField<FStructProperty>(ArrayProperty->Inner) : nullptr;
		if (!Inner || Inner->Struct != FOptionalPinFromProperty::StaticStruct())
		{
			return false;
		}
		TArray<FOptionalPinFromProperty>& Entries = *ArrayProperty->ContainerPtrToValuePtr<TArray<FOptionalPinFromProperty>>(Node);
		FOptionalPinFromProperty* Entry = Entries.FindByPredicate([PropertyName](const FOptionalPinFromProperty& Candidate)
		{
			return Candidate.PropertyName == PropertyName;
		});
		if (!Entry)
		{
			return false;
		}
		if (!Entry->bShowPin)
		{
			Entry->bShowPin = true;
			Node->ReconstructNode();
		}
		return true;
	}

	/** A linked graph's or layer's own variable exposed as a pin (`RigInput` on Control Rig), or null. */
	static const FProperty* DabGenFindCustomPinProperty(UAnimGraphNode_Base* Node, FName Name)
	{
		const UAnimGraphNode_CustomProperty* Custom = Cast<UAnimGraphNode_CustomProperty>(Node);
		const UStruct* Target = Custom ? Custom->GetTargetSkeletonStruct() : nullptr;
		return Target ? FindFProperty<FProperty>(Target, Name) : nullptr;
	}

	void DabGenApplyNodeProperties(FDabGenContext& Ctx, UAnimGraphNode_Base* Node, const TArray<FDabModelProperty>& Properties,
		const FString& Location, bool bReport, TArray<int32>* OutUnresolved)
	{
		const UScriptStruct* NodeStruct = Node->GetFNodeType();
		void* NodeData = Node->GetFNode();
		for (int32 Index = 0; Index < Properties.Num(); ++Index)
		{
			const FDabModelProperty& Property = Properties[Index];
			if (!Property.Value || Property.Name == TEXT("Tag"))
			{
				// The tag goes through SetTag once the node is placed (DabGenApplyNodeTag).
				continue;
			}
			if (const FDabExpr* Unsupported = DabGenFindUnsupported(*Property.Value))
			{
				if (bReport)
				{
					Ctx.Sink.Error(TEXT("DAB7002"), FDabSpan(), FString::Printf(TEXT("%s: %s cannot be generated: %s"), *Location, *Property.Name, *DabGenPrint(*Unsupported)));
				}
				continue;
			}

			FString Error;
			EDabGenApply Result = EDabGenApply::UnknownProperty;
			if (NodeStruct && NodeData)
			{
				Result = DabGenApplyPath(Ctx, NodeStruct, NodeData, Property.Name, *Property.Value, Node, Error);
			}
			if (Result == EDabGenApply::UnknownProperty)
			{
				Error.Reset();
				Result = DabGenApplyPath(Ctx, Node->GetClass(), Node, Property.Name, *Property.Value, Node, Error);
			}

			if (Result == EDabGenApply::UnknownProperty)
			{
				if (OutUnresolved)
				{
					OutUnresolved->AddUnique(Index);
				}
				else if (bReport)
				{
					Ctx.Sink.Error(TEXT("DAB7001"), FDabSpan(), FString::Printf(TEXT("%s: %s has no property %s"), *Location,
						NodeStruct ? *NodeStruct->GetName() : *Node->GetClass()->GetName(), *Property.Name));
				}
			}
			else if (Result == EDabGenApply::Failed && bReport)
			{
				Ctx.Sink.Error(TEXT("DAB7003"), FDabSpan(), FString::Printf(TEXT("%s: %s = %s: %s"), *Location, *Property.Name, *DabGenPrint(*Property.Value), *Error));
			}
		}
	}

	static void DabGenApplyNodeTag(FDabGenContext& Ctx, UAnimGraphNode_Base* Node, const TArray<FDabModelProperty>& Properties, const FString& Location)
	{
		for (const FDabModelProperty& Property : Properties)
		{
			if (Property.Name != TEXT("Tag") || !Property.Value)
			{
				continue;
			}
			FString Tag;
			if (!DabGenTryGetText(*Property.Value, Tag))
			{
				Ctx.Sink.Error(TEXT("DAB7003"), FDabSpan(), FString::Printf(TEXT("%s: Tag = %s: expects a name"), *Location, *DabGenPrint(*Property.Value)));
				continue;
			}
			// SetTag also refreshes the Blueprint's tag extension, which compiling the tag needs.
			Node->SetTag(FName(*Tag));
		}
	}

	void DabGenApplyObjectProperties(FDabGenContext& Ctx, UObject* Object, const TArray<FDabModelProperty>& Properties, const FString& Location)
	{
		for (const FDabModelProperty& Property : Properties)
		{
			if (!Property.Value)
			{
				continue;
			}
			if (const FDabExpr* Unsupported = DabGenFindUnsupported(*Property.Value))
			{
				Ctx.Sink.Error(TEXT("DAB7002"), FDabSpan(), FString::Printf(TEXT("%s: %s cannot be generated: %s"), *Location, *Property.Name, *DabGenPrint(*Unsupported)));
				continue;
			}
			FString Error;
			const EDabGenApply Result = DabGenApplyPath(Ctx, Object->GetClass(), Object, Property.Name, *Property.Value, Object, Error);
			if (Result == EDabGenApply::UnknownProperty)
			{
				Ctx.Sink.Error(TEXT("DAB7001"), FDabSpan(), FString::Printf(TEXT("%s: %s has no property %s"), *Location, *Object->GetClass()->GetName(), *Property.Name));
			}
			else if (Result == EDabGenApply::Failed)
			{
				Ctx.Sink.Error(TEXT("DAB7003"), FDabSpan(), FString::Printf(TEXT("%s: %s = %s: %s"), *Location, *Property.Name, *DabGenPrint(*Property.Value), *Error));
			}
		}
	}

	void DabGenSyncPinDefaults(FDabGenContext& Ctx, UAnimGraphNode_Base* Node, const TArray<FDabModelProperty>& Properties, const FString& Location,
		const TArray<int32>& Unresolved)
	{
		const UScriptStruct* NodeStruct = Node->GetFNodeType();
		const void* NodeData = Node->GetFNode();
		for (int32 Index = 0; Index < Properties.Num(); ++Index)
		{
			const FDabModelProperty& Property = Properties[Index];
			if (!Property.Value || Property.Name == TEXT("Tag") || DabGenFindUnsupported(*Property.Value))
			{
				continue;
			}

			FString Error;
			if (Unresolved.Contains(Index))
			{
				// Not a field of the node: a variable of the linked graph's class, exposed as a pin and given a constant.
				const FProperty* TargetProperty = DabGenFindCustomPinProperty(Node, FName(*Property.Name));
				if (!TargetProperty || !DabGenShowCustomPin(Node, TargetProperty->GetFName()))
				{
					Ctx.Sink.Error(TEXT("DAB7001"), FDabSpan(), FString::Printf(TEXT("%s: %s has no property %s"), *Location,
						NodeStruct ? *NodeStruct->GetName() : *Node->GetClass()->GetName(), *Property.Name));
					continue;
				}
				UEdGraphPin* Pin = Node->FindPin(TargetProperty->GetFName(), EGPD_Input);
				FDabGenTempValue Value(TargetProperty);
				if (!Pin || !DabGenImportValue(Ctx, TargetProperty, Value.Get(), *Property.Value, Node, Error) || !DabGenSetPinFromValue(Pin, TargetProperty, Value.Get(), Error))
				{
					Ctx.Sink.Error(TEXT("DAB7003"), FDabSpan(), FString::Printf(TEXT("%s: %s = %s: %s"), *Location, *Property.Name, *DabGenPrint(*Property.Value),
						Error.IsEmpty() ? TEXT("the pin cannot be shown") : *Error));
				}
				continue;
			}

			if (!NodeStruct || !NodeData)
			{
				continue;
			}
			FString Root = Property.Name;
			int32 Cut = INDEX_NONE;
			if (Root.FindChar(TEXT('.'), Cut))
			{
				Root.LeftInline(Cut);
			}
			if (Root.FindChar(TEXT('['), Cut))
			{
				Root.LeftInline(Cut);
			}
			const FProperty* NodeProperty = FindFProperty<FProperty>(NodeStruct, FName(*Root));
			if (!NodeProperty)
			{
				// A property of the editor node: it has no pin.
				continue;
			}

			// A visible pin's default is what compiles, not the field; copy every element that has a pin.
			const void* Value = NodeProperty->ContainerPtrToValuePtr<void>(NodeData);
			if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(NodeProperty))
			{
				FScriptArrayHelper Helper(ArrayProperty, Value);
				for (int32 Element = 0; Element < Helper.Num(); ++Element)
				{
					UEdGraphPin* Pin = Node->FindPin(FString::Printf(TEXT("%s_%d"), *Root, Element), EGPD_Input);
					if (Pin && !Pin->bHidden && Pin->LinkedTo.IsEmpty() && !DabGenIsPosePin(Pin)
						&& !DabGenSetPinFromValue(Pin, ArrayProperty->Inner, Helper.GetRawPtr(Element), Error))
					{
						Ctx.Sink.Error(TEXT("DAB7003"), FDabSpan(), FString::Printf(TEXT("%s: %s[%d]: %s"), *Location, *Root, Element, *Error));
					}
				}
			}
			else if (UEdGraphPin* Pin = Node->FindPin(Root, EGPD_Input))
			{
				if (!Pin->bHidden && Pin->LinkedTo.IsEmpty() && !DabGenIsPosePin(Pin) && !DabGenSetPinFromValue(Pin, NodeProperty, Value, Error))
				{
					Ctx.Sink.Error(TEXT("DAB7003"), FDabSpan(), FString::Printf(TEXT("%s: %s: %s"), *Location, *Root, *Error));
				}
			}
		}
	}

	UEdGraphPin* DabGenFindInputPin(UEdGraphNode* Node, const FString& PinName)
	{
		if (UEdGraphPin* Pin = Node->FindPin(PinName, EGPD_Input))
		{
			return Pin;
		}
		// Decompiled models make input pose names identifiers (`Locomotion Input` -> `LocomotionInput`); the linked
		// class may still spell them with spaces.
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && DabGenIsPosePin(Pin)
				&& UE::DreamAnim::Compiler::MakeDabIdentifier(Pin->PinName.ToString()) == PinName)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	bool DabGenBindPin(FDabGenContext& Ctx, UAnimGraphNode_Base* Node, const FString& PinName, const TArray<FString>& Path, bool bFunction,
		const FString& Location)
	{
		const FName PinFName(*PinName);
		FName BindingName = PinFName;
		int32 ArrayIndex = INDEX_NONE;
		bool bShown = false;

		if (const FProperty* TargetProperty = DabGenFindCustomPinProperty(Node, PinFName))
		{
			// A linked graph's variable: the binding is keyed by the class variable the compiler generates for the pin.
			bShown = DabGenShowCustomPin(Node, TargetProperty->GetFName());
			BindingName = FName(*CastChecked<UAnimGraphNode_CustomProperty>(Node)->GetPinTargetVariableName(PinFName));
		}
		else
		{
			const FProperty* PinProperty = Node->GetPinProperty(PinFName);
			if (!PinProperty)
			{
				Ctx.Sink.Error(TEXT("DAB7001"), FDabSpan(), FString::Printf(TEXT("%s: %s has no pin %s to bind"), *Location, *Node->GetClass()->GetName(), *PinName));
				return false;
			}
			bShown = DabGenShowNodePin(Node, PinProperty->GetFName());
			if (PinProperty->IsA<FArrayProperty>() && PinFName.GetNumber() > 0)
			{
				// `DesiredAlphas_2` is element 2: FName keeps the suffix as number 3.
				ArrayIndex = PinFName.GetNumber() - 1;
			}
		}
		if (!bShown)
		{
			Ctx.Sink.Error(TEXT("DAB7001"), FDabSpan(), FString::Printf(TEXT("%s: the pin %s cannot be bound"), *Location, *PinName));
			return false;
		}

		UObject* BindingObject = DabGenGetBindingObject(Node);
		const FMapProperty* Map = BindingObject ? FindFProperty<FMapProperty>(BindingObject->GetClass(), TEXT("PropertyBindings")) : nullptr;
		const FStructProperty* EntryProperty = Map ? CastField<FStructProperty>(Map->ValueProp) : nullptr;
		if (!Map || !Map->KeyProp->IsA<FNameProperty>() || !EntryProperty || EntryProperty->Struct != FAnimGraphNodePropertyBinding::StaticStruct())
		{
			Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: the node keeps its bindings in %s, which DreamAnim cannot write"), *Location,
				BindingObject ? *BindingObject->GetClass()->GetName() : TEXT("nothing")));
			return false;
		}

		// A link wins over a binding in the compiler.
		if (UEdGraphPin* Pin = Node->FindPin(PinFName, EGPD_Input))
		{
			Pin->BreakAllPinLinks();
		}

		FAnimGraphNodePropertyBinding Entry;
		Entry.PropertyName = BindingName;
		Entry.ArrayIndex = ArrayIndex;
		Entry.PropertyPath = Path;
		Entry.PathAsText = FText::FromString(FString::Join(Path, TEXT(".")));
		Entry.Type = bFunction ? EAnimGraphNodePropertyBindingType::Function : EAnimGraphNodePropertyBindingType::Property;
		Entry.bIsBound = true;

		BindingObject->Modify();
		FScriptMapHelper Helper(Map, Map->ContainerPtrToValuePtr<void>(BindingObject));
		Helper.AddPair(&BindingName, &Entry);
		Ctx.BoundNodes.Add(Node);
		return true;
	}

	/** The type a generated function bound to a pin must return: the pin's property (an element's for array pins). */
	static bool DabGenGetBindingType(UAnimGraphNode_Base* Node, const FString& PinName, FEdGraphPinType& OutType)
	{
		const FName PinFName(*PinName);
		const FProperty* Property = DabGenFindCustomPinProperty(Node, PinFName);
		if (!Property)
		{
			Property = Node->GetPinProperty(PinFName);
			if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property); ArrayProperty && PinFName.GetNumber() > 0)
			{
				Property = ArrayProperty->Inner;
			}
		}
		return Property && GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property, OutType);
	}

	void DabGenApplyBinding(FDabGenContext& Ctx, UAnimGraphNode_Base* Node, const FDabModelBinding& Binding, const FString& Location,
		const FDabGenScope& Scope)
	{
		if (!Binding.Value)
		{
			return;
		}
		const FString PinLocation = Location + TEXT(".") + Binding.PinName;
		if (const FDabExpr* Unsupported = DabGenFindUnsupported(*Binding.Value))
		{
			Ctx.Sink.Error(TEXT("DAB7002"), FDabSpan(), FString::Printf(TEXT("%s: the binding cannot be generated: %s"), *PinLocation, *DabGenPrint(*Unsupported)));
			return;
		}

		FDabGenPath Path;
		if (DabGenTryGetPath(*Binding.Value, Path))
		{
			if ((Path.Root == EDabGenPathRoot::Self || Path.Root == EDabGenPathRoot::Props) && !Path.Segments.IsEmpty())
			{
				DabGenBindPin(Ctx, Node, Binding.PinName, Path.Segments, Path.LastIsCall(), PinLocation);
				return;
			}
			if (Path.Root == EDabGenPathRoot::Assets && Path.Segments.Num() == 1)
			{
				const FName Function = DabGenEnsureAssetFunction(Ctx, Path.Segments[0], PinLocation);
				if (!Function.IsNone())
				{
					DabGenBindPin(Ctx, Node, Binding.PinName, { Function.ToString() }, true, PinLocation);
				}
				return;
			}
		}

		// Any other expression becomes a thread-safe function the pin reads every update.
		FEdGraphPinType ReturnType;
		if (!DabGenGetBindingType(Node, Binding.PinName, ReturnType))
		{
			Ctx.Sink.Error(TEXT("DAB7001"), FDabSpan(), FString::Printf(TEXT("%s: %s has no pin %s to bind"), *Location, *Node->GetClass()->GetName(), *Binding.PinName));
			return;
		}
		const FName Function = DabGenEnsureExprFunction(Ctx, *Binding.Value, ReturnType, Scope, PinLocation);
		if (!Function.IsNone())
		{
			DabGenBindPin(Ctx, Node, Binding.PinName, { Function.ToString() }, true, PinLocation);
		}
	}

	static void DabGenApplyNodeFunction(FDabGenContext& Ctx, UAnimGraphNode_Base* Node, const FDabModelNodeFunction& Function, const FString& Location)
	{
		FMemberReference* Reference = nullptr;
		FName MemberName;
		if (Function.Site == TEXT("InitialUpdate"))
		{
			Reference = &Node->InitialUpdateFunction;
			MemberName = GET_MEMBER_NAME_CHECKED(UAnimGraphNode_Base, InitialUpdateFunction);
		}
		else if (Function.Site == TEXT("BecomeRelevant"))
		{
			Reference = &Node->BecomeRelevantFunction;
			MemberName = GET_MEMBER_NAME_CHECKED(UAnimGraphNode_Base, BecomeRelevantFunction);
		}
		else if (Function.Site == TEXT("Update"))
		{
			Reference = &Node->UpdateFunction;
			MemberName = GET_MEMBER_NAME_CHECKED(UAnimGraphNode_Base, UpdateFunction);
		}
		else
		{
			Ctx.Sink.Error(TEXT("DAB7001"), FDabSpan(), FString::Printf(TEXT("%s: a node has no %s function (InitialUpdate, BecomeRelevant or Update)"), *Location, *Function.Site));
			return;
		}

		const FName FunctionName = DabGenResolveCallFunction(Ctx, Function.Call, EDabGenForwarder::AnimUpdateCall, Location + TEXT(".") + MemberName.ToString());
		if (FunctionName.IsNone())
		{
			return;
		}
		Reference->SetSelfMember(FunctionName);

		// What the details panel does after picking a function: resolve it into the runtime node, refresh the
		// Blueprint's extensions (Become Relevant needs one) and rebuild the node.
		FProperty* MemberProperty = FindFProperty<FProperty>(UAnimGraphNode_Base::StaticClass(), MemberName);
		FPropertyChangedEvent Event(MemberProperty);
		Node->PostEditChangeProperty(Event);
	}

	/** Mirrors UAnimGraphNode_CallFunction::SetupFromFunction, which is private: an inner graph holding one K2 call. */
	static bool DabGenPrepareCallFunctionNode(FDabGenContext& Ctx, UAnimGraphNode_Base* Node, const FDabModelNode& Model, FName FunctionName,
		const FString& GraphPath, const FString& Location, UFunction*& OutDirectFunction)
	{
		OutDirectFunction = nullptr;
		if (!Model.Call)
		{
			return false;
		}
		UClass* SkeletonClass = Ctx.GetSkeletonClass();
		UFunction* Function = (!FunctionName.IsNone() && SkeletonClass) ? SkeletonClass->FindFunctionByName(FunctionName) : nullptr;
		if (!Function)
		{
			if (!FunctionName.IsNone())
			{
				Ctx.Sink.Error(TEXT("DAB7004"), FDabSpan(), FString::Printf(TEXT("%s: the function %s is not in the skeleton class"), *Location, *FunctionName.ToString()));
			}
			return false;
		}

		UEdGraph* InnerGraph = FBlueprintEditorUtils::CreateNewGraph(Node, NAME_None, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		FGraphNodeCreator<UK2Node_CallFunction> Creator(*InnerGraph);
		UK2Node_CallFunction* Prototype = Creator.CreateNode(false);
		if (Ctx.GeneratedFunctions.Contains(FunctionName) || Function->GetOwnerClass() == SkeletonClass)
		{
			// A function of this Blueprint: refer to it by name, the generated class does not have it yet.
			Prototype->FunctionReference.SetSelfMember(FunctionName);
		}
		else
		{
			Prototype->FunctionReference.SetFromField<UFunction>(Function, false);
		}
		Creator.Finalize();
		Ctx.AssignGuid(Prototype, GraphPath, Model.Id + TEXT("/#Call"));

		const FObjectPropertyBase* GraphProperty = FindFProperty<FObjectPropertyBase>(Node->GetClass(), TEXT("InnerGraph"));
		const FObjectPropertyBase* PrototypeProperty = FindFProperty<FObjectPropertyBase>(Node->GetClass(), TEXT("CallFunctionPrototype"));
		if (!GraphProperty || !PrototypeProperty)
		{
			Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: %s has no InnerGraph / CallFunctionPrototype"), *Location, *Node->GetClass()->GetName()));
			return false;
		}
		GraphProperty->SetObjectPropertyValue_InContainer(Node, InnerGraph);
		PrototypeProperty->SetObjectPropertyValue_InContainer(Node, Prototype);

		if (FunctionName == FName(*Model.Call->Function) && DabGenIsSelf(Model.Call->Target.Get()))
		{
			OutDirectFunction = Function;
		}
		return true;
	}

	/** Constant arguments of a directly called function go into the Call Function node's parameter pins. */
	static void DabGenApplyCallArguments(FDabGenContext& Ctx, UAnimGraphNode_Base* Node, const FDabModelCall& Call, const UFunction* Function, const FString& Location)
	{
		TArray<FProperty*> Parameters;
		DabGenGetInputParameters(Function, Parameters);
		int32 Positional = 0;
		for (const FDabModelArgument& Argument : Call.Args)
		{
			const FProperty* Parameter = Argument.Name.IsEmpty()
				? (Parameters.IsValidIndex(Positional) ? Parameters[Positional++] : nullptr)
				: FindFProperty<FProperty>(Function, FName(*Argument.Name));
			UEdGraphPin* Pin = Parameter ? Node->FindPin(Parameter->GetFName(), EGPD_Input) : nullptr;
			if (!Pin || !Argument.Value)
			{
				Ctx.Sink.Error(TEXT("DAB7001"), FDabSpan(), FString::Printf(TEXT("%s: %s has no parameter %s"), *Location, *Function->GetName(),
					Argument.Name.IsEmpty() ? *FString::FromInt(Positional) : *Argument.Name));
				continue;
			}
			FDabGenTempValue Value(Parameter);
			FString Error;
			if (!DabGenImportValue(Ctx, Parameter, Value.Get(), *Argument.Value, Node, Error) || !DabGenSetPinFromValue(Pin, Parameter, Value.Get(), Error))
			{
				Ctx.Sink.Error(TEXT("DAB7003"), FDabSpan(), FString::Printf(TEXT("%s: %s = %s: %s"), *Location, *Parameter->GetName(), *DabGenPrint(*Argument.Value), *Error));
			}
		}
	}

	/** An implemented interface creates its layers' input pose nodes; the model's input pose takes that node over. */
	static UAnimGraphNode_Base* DabGenAdoptInputPose(FDabGenContext& Ctx, FDabGenGraph& Graph, const FDabModelNode& Model)
	{
		for (UEdGraphNode* Existing : Graph.Graph->Nodes)
		{
			UAnimGraphNode_LinkedInputPose* InputPose = Cast<UAnimGraphNode_LinkedInputPose>(Existing);
			if (!InputPose || Ctx.AdoptedNodes.Contains(InputPose))
			{
				continue;
			}
			const FString ExistingName = InputPose->Node.Name.ToString();
			if (ExistingName == Model.InputPoseName || UE::DreamAnim::Compiler::MakeDabIdentifier(ExistingName) == Model.InputPoseName)
			{
				Ctx.AdoptedNodes.Add(InputPose);
				Ctx.AssignGuid(InputPose, Graph.Path, Model.Id);
				DabGenApplyNodeProperties(Ctx, InputPose, Model.Properties, Graph.Path + TEXT("/") + Model.Id, true, nullptr);
				return InputPose;
			}
		}
		return nullptr;
	}

	/** What changing the instance class or layer in the details panel runs (OnLayerChanged and friends are not exported). */
	static void DabGenFinishLinkedGraphNode(FDabGenContext& Ctx, UAnimGraphNode_Base* Node)
	{
		const UScriptStruct* NodeStruct = Node->GetFNodeType();
		if (UAnimGraphNode_LinkedAnimLayer* LayerNode = Cast<UAnimGraphNode_LinkedAnimLayer>(Node))
		{
			FAnimNode_LinkedAnimLayer& Runtime = LayerNode->Node;
			UClass* Interface = Runtime.Interface.Get();
			FGuid InterfaceGuid;
			for (const FBPInterfaceDescription& Description : Ctx.Blueprint->ImplementedInterfaces)
			{
				for (const UEdGraph* InterfaceGraph : Description.Graphs)
				{
					if (InterfaceGraph && InterfaceGraph->GetFName() == Runtime.Layer && (!Interface || Description.Interface.Get() == Interface))
					{
						Interface = Description.Interface.Get();
						InterfaceGuid = InterfaceGraph->InterfaceGuid;
					}
				}
			}
			Runtime.Interface = Interface;
			LayerNode->InterfaceGuid = InterfaceGuid;
			if (!Interface)
			{
				// A self layer cannot be overridden by another class.
				Runtime.InstanceClass = nullptr;
			}
			FPropertyChangedEvent Event(FindFProperty<FProperty>(NodeStruct, TEXT("Layer")));
			Node->PostEditChangeProperty(Event);
		}
		else if (Node->IsA<UAnimGraphNode_LinkedAnimGraph>())
		{
			FPropertyChangedEvent Event(FindFProperty<FProperty>(NodeStruct, TEXT("InstanceClass")));
			Node->PostEditChangeProperty(Event);
		}
	}

	static UAnimGraphNode_Base* DabGenCreateAnimNode(FDabGenContext& Ctx, FDabGenGraph& Graph, const FDabModelNode& Model)
	{
		const FString Location = Graph.Path + TEXT("/") + Model.Id;
		UClass* NodeClass = DabGenFindNodeClass(Model.ClassName);
		if (!NodeClass)
		{
			Ctx.Sink.Error(TEXT("DAB7004"), FDabSpan(), FString::Printf(TEXT("%s: no anim graph node class %s"), *Location, *Model.ClassName));
			return nullptr;
		}

		if (NodeClass->IsChildOf(UAnimGraphNode_LinkedInputPose::StaticClass()))
		{
			if (UAnimGraphNode_Base* Adopted = DabGenAdoptInputPose(Ctx, Graph, Model))
			{
				return Adopted;
			}
		}

		const FDabModelStateMachine* Machine = nullptr;
		if (NodeClass->IsChildOf(UAnimGraphNode_StateMachine::StaticClass()))
		{
			Machine = Ctx.Model.FindStateMachine(Model.StateMachineName);
			if (!Machine)
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: no state machine %s in the model"), *Location, *Model.StateMachineName));
			}
		}

		// A Call Function node's target may be a generated forwarder; make it before the node exists, because
		// adding a function recompiles the skeleton class, which should not meet a half-made node.
		const UClass* CallFunctionClass = DabGenCallFunctionNodeClass();
		const bool bCallFunction = CallFunctionClass && NodeClass->IsChildOf(CallFunctionClass);
		FName CallFunctionName;
		if (bCallFunction)
		{
			if (Model.Call)
			{
				CallFunctionName = DabGenResolveCallFunction(Ctx, *Model.Call, EDabGenForwarder::Plain, Location);
			}
			else
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: a Call Function node without a call"), *Location));
			}
		}

		FGraphNodeCreator<UAnimGraphNode_Base> Creator(*Graph.Graph);
		UAnimGraphNode_Base* Node = Creator.CreateNode(false, NodeClass);
		const bool bCustom = Node->IsA<UAnimGraphNode_CustomProperty>();

		// Values that shape pins (pose counts, BoundEnum, VisibleEnumEntries, InstanceClass) go in before the pins exist.
		const TMap<FName, int32> PoseSizes = DabGenCollectPoseArraySizes(Node, Model);
		DabGenResizePoseArrays(Node, PoseSizes);
		TArray<int32> Unresolved;
		DabGenApplyNodeProperties(Ctx, Node, Model.Properties, Location, true, bCustom ? &Unresolved : nullptr);
		if (Machine)
		{
			DabGenApplyNodeProperties(Ctx, Node, Machine->Properties, Location, true, nullptr);
		}

		UFunction* DirectFunction = nullptr;
		if (bCallFunction)
		{
			DabGenPrepareCallFunctionNode(Ctx, Node, Model, CallFunctionName, Graph.Path, Location, DirectFunction);
		}
		if (UAnimGraphNode_SaveCachedPose* SaveNode = Cast<UAnimGraphNode_SaveCachedPose>(Node))
		{
			SaveNode->CacheName = Model.CacheName;
		}

		Creator.Finalize();
		Ctx.AssignGuid(Node, Graph.Path, Model.Id);

		// PostPlacedNewNode of some nodes adds poses (Blend Poses by Int, Multi-Way Blend); bring the node back to the model.
		DabGenResizePoseArrays(Node, PoseSizes);
		DabGenDropSurplusPins(Node);
		DabGenApplyNodeProperties(Ctx, Node, Model.Properties, Location, false, nullptr);
		if (Machine)
		{
			DabGenApplyNodeProperties(Ctx, Node, Machine->Properties, Location, false, nullptr);
		}
		Node->ReconstructNode();
		DabGenApplyNodeTag(Ctx, Node, Model.Properties, Location);
		if (Machine)
		{
			// A machine's node settings, its tag included, live with the machine in the model.
			DabGenApplyNodeTag(Ctx, Node, Machine->Properties, Location);
		}

		if (UAnimGraphNode_SaveCachedPose* SaveNode = Cast<UAnimGraphNode_SaveCachedPose>(Node))
		{
			if (Ctx.SaveCacheNodes.Contains(Model.CacheName))
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: a second cached pose named %s"), *Location, *Model.CacheName));
			}
			else
			{
				Ctx.SaveCacheNodes.Add(Model.CacheName, SaveNode);
			}
		}
		else if (UAnimGraphNode_UseCachedPose* UseNode = Cast<UAnimGraphNode_UseCachedPose>(Node))
		{
			Ctx.CacheUses.Add({ UseNode, Model.CacheName, Location });
		}
		else if (UAnimGraphNode_LinkedInputPose* InputPose = Cast<UAnimGraphNode_LinkedInputPose>(Node))
		{
			// PostPlacedNewNode picked a unique InPose name; the model's goes in afterwards, then what renaming in
			// the details panel runs, so layer nodes calling this graph pick up the new input.
			Ctx.AdoptedNodes.Add(InputPose);
			InputPose->Node.Name = FName(*Model.InputPoseName);
			InputPose->ReconstructNode();
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);
			InputPose->ReconstructNode();
			DabGenReconstructSelfLayerNodes(Ctx);
		}
		else if (Node->IsA<UAnimGraphNode_LinkedAnimGraph>() || Node->IsA<UAnimGraphNode_LinkedAnimLayer>())
		{
			DabGenFinishLinkedGraphNode(Ctx, Node);
		}
		else if (UAnimGraphNode_StateMachine* MachineNode = Cast<UAnimGraphNode_StateMachine>(Node))
		{
			if (Machine)
			{
				DabGenBuildMachine(Ctx, MachineNode, *Machine, Location);
			}
		}

		DabGenSyncPinDefaults(Ctx, Node, Model.Properties, Location, Unresolved);
		if (Machine)
		{
			DabGenSyncPinDefaults(Ctx, Node, Machine->Properties, Location, TArray<int32>());
		}
		if (DirectFunction && Model.Call)
		{
			DabGenApplyCallArguments(Ctx, Node, *Model.Call, DirectFunction, Location);
		}

		if (!Model.Bindings.IsEmpty() || !Model.NodeFunctions.IsEmpty())
		{
			const FDabModelNode* ModelNode = &Model;
			FDabGenScope Scope;
			Scope.MachineName = Graph.MachineName;
			Ctx.Deferred.Add([&Ctx, Node, ModelNode, Location, Scope]()
			{
				for (const FDabModelBinding& Binding : ModelNode->Bindings)
				{
					DabGenApplyBinding(Ctx, Node, Binding, Location, Scope);
				}
				for (const FDabModelNodeFunction& Function : ModelNode->NodeFunctions)
				{
					DabGenApplyNodeFunction(Ctx, Node, Function, Location);
				}
			});
		}
		return Node;
	}

	void DabGenReconstructSelfLayerNodes(FDabGenContext& Ctx)
	{
		// The body of UAnimGraphNode_LinkedInputPose::ReconstructLayerNodes, which is private: nodes calling a layer
		// of this Blueprint rebuild their pins from its input poses. Interface layers rebuild when compiled.
		TArray<UAnimGraphNode_LinkedAnimLayer*> LayerNodes;
		FBlueprintEditorUtils::GetAllNodesOfClass<UAnimGraphNode_LinkedAnimLayer>(Ctx.Blueprint, LayerNodes);
		for (UAnimGraphNode_LinkedAnimLayer* LayerNode : LayerNodes)
		{
			if (LayerNode->Node.Interface.Get() == nullptr)
			{
				LayerNode->ReconstructNode();
			}
		}
	}

	void DabGenCreateGraphNodes(FDabGenContext& Ctx, FDabGenGraph& Graph)
	{
		if (!Graph.Model || !Graph.Graph)
		{
			return;
		}
		if (Graph.ResultNode)
		{
			Ctx.AssignGuid(Graph.ResultNode, Graph.Path, TEXT("#Result"));
		}
		for (const FDabModelNode& Model : Graph.Model->Nodes)
		{
			if (Graph.Nodes.Contains(Model.Id))
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: two nodes with the id %s"), *Graph.Path, *Model.Id));
				continue;
			}
			if (UAnimGraphNode_Base* Node = DabGenCreateAnimNode(Ctx, Graph, Model))
			{
				Graph.Nodes.Add(Model.Id, Node);
			}
		}
		if (!Graph.Model->ResultProperties.IsEmpty())
		{
			const FString Location = Graph.Path + TEXT("/#Result");
			const TArray<FDabModelProperty>& Properties = Graph.Model->ResultProperties;
			TArray<int32> Unresolved;
			if (Graph.ResultNode)
			{
				DabGenApplyNodeProperties(Ctx, Graph.ResultNode, Properties, Location, true, &Unresolved);
				DabGenApplyNodeTag(Ctx, Graph.ResultNode, Properties, Location);
			}
			else
			{
				for (int32 Index = 0; Index < Properties.Num(); ++Index)
				{
					Unresolved.Add(Index);
				}
			}

			// What the result node does not have belongs to the graph itself (`BlendOptions` on UAnimationGraph).
			for (const int32 Index : Unresolved)
			{
				const FDabModelProperty& Property = Properties[Index];
				FString Error;
				const EDabGenApply Result = Property.Value
					? DabGenApplyPath(Ctx, Graph.Graph->GetClass(), Graph.Graph, Property.Name, *Property.Value, Graph.Graph, Error)
					: EDabGenApply::Applied;
				if (Result == EDabGenApply::UnknownProperty)
				{
					Ctx.Sink.Error(TEXT("DAB7001"), FDabSpan(), FString::Printf(TEXT("%s: neither the result node nor %s has a property %s"), *Location,
						*Graph.Graph->GetClass()->GetName(), *Property.Name));
				}
				else if (Result == EDabGenApply::Failed)
				{
					Ctx.Sink.Error(TEXT("DAB7003"), FDabSpan(), FString::Printf(TEXT("%s: %s = %s: %s"), *Location, *Property.Name, *DabGenPrint(*Property.Value), *Error));
				}
			}
			if (Graph.ResultNode)
			{
				DabGenSyncPinDefaults(Ctx, Graph.ResultNode, Properties, Location, TArray<int32>());
			}
		}
	}

	void DabGenLinkGraph(FDabGenContext& Ctx, FDabGenGraph& Graph)
	{
		if (!Graph.Model || !Graph.Graph)
		{
			return;
		}
		const UEdGraphSchema* Schema = Graph.Graph->GetSchema();

		const auto Connect = [&Ctx, &Graph, Schema](const FString& SourceId, UEdGraphPin* Input, const FString& Location)
		{
			UAnimGraphNode_Base* const* Source = Graph.Nodes.Find(SourceId);
			if (!Source)
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: no node %s in %s"), *Location, *SourceId, *Graph.Path));
				return;
			}
			// Usually `Pose`; a space converter's output is `ComponentPose`. The pose output is the one pose-typed output.
			UEdGraphPin* Output = (*Source)->FindPin(TEXT("Pose"), EGPD_Output);
			if (!Output)
			{
				for (UEdGraphPin* Pin : (*Source)->Pins)
				{
					const UScriptStruct* PinStruct = Pin && Pin->Direction == EGPD_Output ? Cast<UScriptStruct>(Pin->PinType.PinSubCategoryObject.Get()) : nullptr;
					if (PinStruct && PinStruct->IsChildOf(FPoseLinkBase::StaticStruct()))
					{
						Output = Pin;
						break;
					}
				}
			}
			if (!Output || !Input || !Schema->TryCreateConnection(Output, Input))
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: the pose of %s cannot be linked here"), *Location, *SourceId));
			}
		};

		for (const FDabModelNode& Model : Graph.Model->Nodes)
		{
			UAnimGraphNode_Base* const* Node = Graph.Nodes.Find(Model.Id);
			if (!Node)
			{
				continue;
			}
			for (const FDabModelPoseLink& Link : Model.PoseInputs)
			{
				if (Link.SourceNodeId.IsEmpty())
				{
					continue;
				}
				const FString Location = Graph.Path + TEXT("/") + Model.Id + TEXT(".") + Link.PinName;
				UEdGraphPin* Input = DabGenFindInputPin(*Node, Link.PinName);
				if (!Input)
				{
					Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: %s has no pose pin %s"), *Location, *(*Node)->GetClass()->GetName(), *Link.PinName));
					continue;
				}
				Connect(Link.SourceNodeId, Input, Location);
			}
		}

		if (!Graph.Model->ResultNodeId.IsEmpty())
		{
			const FString Location = Graph.Path + TEXT("/#Result");
			if (!Graph.ResultPin)
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: the graph has no result to link"), *Location));
			}
			else
			{
				Connect(Graph.Model->ResultNodeId, Graph.ResultPin, Location);
			}
		}
	}

	void DabGenResolveCacheUses(FDabGenContext& Ctx)
	{
		for (const FDabGenContext::FCacheUse& Use : Ctx.CacheUses)
		{
			UAnimGraphNode_SaveCachedPose* const* Save = Ctx.SaveCacheNodes.Find(Use.CacheName);
			if (!Save)
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: no cached pose named %s"), *Use.Location, *Use.CacheName));
				continue;
			}
			Use.Node->SaveCachedPoseNode = *Save;
			// NameOfCache is private; it lets the node find its Save node again if the pointer is lost.
			if (const FStrProperty* NameProperty = FindFProperty<FStrProperty>(Use.Node->GetClass(), TEXT("NameOfCache")))
			{
				NameProperty->SetPropertyValue_InContainer(Use.Node, Use.CacheName);
			}
		}
	}

	void DabGenLayoutGraph(FDabGenGraph& Graph)
	{
		if (!Graph.Graph)
		{
			return;
		}
		TMap<const UEdGraphNode*, int32> Depths;
		TArray<const UEdGraphNode*> Queue;
		if (Graph.ResultNode)
		{
			Depths.Add(Graph.ResultNode, 0);
			Queue.Add(Graph.ResultNode);
		}
		for (int32 Index = 0; Index < Queue.Num(); ++Index)
		{
			const UEdGraphNode* Node = Queue[Index];
			const int32 Depth = Depths.FindChecked(Node);
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input)
				{
					continue;
				}
				for (const UEdGraphPin* Linked : Pin->LinkedTo)
				{
					const UEdGraphNode* Source = Linked ? Linked->GetOwningNodeUnchecked() : nullptr;
					if (Source && !Depths.Contains(Source))
					{
						Depths.Add(Source, Depth + 1);
						Queue.Add(Source);
					}
				}
			}
		}

		int32 MaxDepth = 0;
		for (const TPair<const UEdGraphNode*, int32>& Entry : Depths)
		{
			MaxDepth = FMath::Max(MaxDepth, Entry.Value);
		}
		TMap<int32, int32> Rows;
		for (UEdGraphNode* Node : Graph.Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			const int32* Found = Depths.Find(Node);
			const int32 Column = Found ? *Found : MaxDepth + 1;
			int32& Row = Rows.FindOrAdd(Column);
			Node->NodePosX = -Column * 420;
			Node->NodePosY = Row * 240;
			++Row;
		}
	}
}
