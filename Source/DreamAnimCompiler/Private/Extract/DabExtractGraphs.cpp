// Copyright DreamMoon. All Rights Reserved.

#include "Extract/DabExtractPrivate.h"

#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SaveCachedPose.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimGraphNode_UseCachedPose.h"
#include "AnimStateAliasNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimationStateMachineGraph.h"
#include "Components/ActorComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Knot.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Compiler::ExtractPrivate
{
	using namespace UE::DreamAnim::Lang;
	using Model::FDabModelBinding;
	using Model::FDabModelGraph;
	using Model::FDabModelNode;
	using Model::FDabModelProperty;

	namespace
	{
		void AddProperty(TArray<FDabModelProperty>& Out, const FString& Name, FDabExprPtr Value)
		{
			FDabModelProperty Property;
			Property.Name = Name;
			Property.Value = MoveTemp(Value);
			Out.Add(MoveTemp(Property));
		}

		void AddBinding(TArray<FDabModelBinding>& Out, const FString& PinName, FDabExprPtr Value)
		{
			FDabModelBinding Binding;
			Binding.PinName = PinName;
			Binding.Value = MoveTemp(Value);
			Out.Add(MoveTemp(Binding));
		}

		/** A struct value with no field to record: the difference was in fields the model does not keep. */
		bool IsEmptyTuple(const FDabExpr& Expr)
		{
			const FDabTupleExpr* Tuple = Expr.As<FDabTupleExpr>();
			return Tuple && Tuple->Items.IsEmpty();
		}

		bool IsChildOfStructNamed(const UStruct* Struct, FName Name)
		{
			for (const UStruct* Current = Struct; Current; Current = Current->GetSuperStruct())
			{
				if (Current->GetFName() == Name)
				{
					return true;
				}
			}
			return false;
		}

		// Engine structs compared by name: their StaticStruct() is not exported from AnimGraph / Engine.
		const FName PoseLinkBaseName(TEXT("PoseLinkBase"));
		const FName AnimNodeFunctionRefName(TEXT("AnimNodeFunctionRef"));
		const FName AnimGraphNodePropertyBindingName(TEXT("AnimGraphNodePropertyBinding"));
		const FName OptionalPinFromPropertyName(TEXT("OptionalPinFromProperty"));
		const FName MemberReferenceName(TEXT("MemberReference"));
		const FName GuidName(TEXT("Guid"));

		bool IsPoseLinkProperty(const FProperty& Property)
		{
			const FProperty* Element = &Property;
			if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
			{
				Element = Array->Inner;
			}
			const FStructProperty* Struct = CastField<FStructProperty>(Element);
			return Struct && IsChildOfStructNamed(Struct->Struct, PoseLinkBaseName);
		}

		struct FSkippedField
		{
			const TCHAR* Struct;
			const TCHAR* Field;
		};

		/**
		 * Runtime-node fields that are compiler bookkeeping or that the model holds elsewhere (a layer's or input
		 * pose's name, a cache's name), never authored values.
		 */
		const FSkippedField SkippedNodeFields[] =
		{
			{TEXT("AnimNode_Root"), TEXT("Name")},
			{TEXT("AnimNode_StateResult"), TEXT("StateIndex")},
			{TEXT("AnimNode_LinkedInputPose"), TEXT("Name")},
			{TEXT("AnimNode_LinkedInputPose"), TEXT("Graph")},
			{TEXT("AnimNode_LinkedInputPose"), TEXT("bIsOutputLinked")},
			{TEXT("AnimNode_SaveCachedPose"), TEXT("CachePoseName")},
			{TEXT("AnimNode_UseCachedPose"), TEXT("CachePoseName")},
			{TEXT("AnimNode_StateMachine"), TEXT("StateMachineIndexInClass")},
			{TEXT("AnimNode_CustomProperty"), TEXT("SourcePropertyNames")},
			{TEXT("AnimNode_CustomProperty"), TEXT("DestPropertyNames")},
			{TEXT("AnimNode_LinkedAnimGraph"), TEXT("InputPoseNames")},
			{TEXT("AnimNode_ControlRig"), TEXT("DefaultControlRigAssetReference")},
		};

		bool IsRecordedNodeField(const FProperty& Field)
		{
			if (Field.HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) || IsPoseLinkProperty(Field))
			{
				return false;
			}
			if (const FStructProperty* Struct = CastField<FStructProperty>(&Field))
			{
				// Compiled copies of the editor node's function references.
				if (IsChildOfStructNamed(Struct->Struct, AnimNodeFunctionRefName))
				{
					return false;
				}
			}
			const UStruct* Owner = Field.GetOwnerStruct();
			for (const FSkippedField& Skipped : SkippedNodeFields)
			{
				if (Owner && Owner->GetFName() == FName(Skipped.Struct) && Field.GetFName() == FName(Skipped.Field))
				{
					return false;
				}
			}
			return true;
		}

		/** Editor-node properties that hold the editor's view of the node, its pins, or what the model keeps elsewhere. */
		const TCHAR* const SkippedEditorProperties[] =
		{
			TEXT("ShowPinForProperties"), TEXT("CustomPinProperties"), TEXT("PropertyBindings_DEPRECATED"), TEXT("BlueprintUsage"), TEXT("Binding"),
			TEXT("InitialUpdateFunction"), TEXT("BecomeRelevantFunction"), TEXT("UpdateFunction"),
			TEXT("StateEntryFunction"), TEXT("StateFullyBlendedInFunction"), TEXT("StateExitFunction"), TEXT("StateFullyBlendedOutFunction"),
			TEXT("EditorStateMachineGraph"), TEXT("CacheName"), TEXT("NameOfCache"), TEXT("SaveCachedPoseNode"), TEXT("InputPoseIndex"),
			TEXT("FunctionReference"), TEXT("InterfaceGuid"), TEXT("InnerGraph"), TEXT("CallFunctionPrototype"),
			TEXT("BlendSpaceGraph"), TEXT("Graphs"), TEXT("LastPreviewComponent"),
			TEXT("bPreviewLive"), TEXT("bShowLinearLimits"), TEXT("bShowAngularLimits"), TEXT("bShowPlanarLimit"), TEXT("bShowSphericalLimit"),
			TEXT("bShowCollisionSpheres"), TEXT("bEnableDebugDraw"), TEXT("bDrawDebugCones"), TEXT("AxisLength"), TEXT("ConeSubdivision"),
		};

		bool IsRecordedEditorProperty(const FProperty& Property)
		{
			if (Property.HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated))
			{
				return false;
			}
			const FName Name = Property.GetFName();
			for (const TCHAR* Skipped : SkippedEditorProperties)
			{
				if (Name == FName(Skipped))
				{
					return false;
				}
			}

			const FProperty* Element = &Property;
			if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
			{
				Element = Array->Inner;
			}
			if (const FStructProperty* Struct = CastField<FStructProperty>(Element))
			{
				if (IsChildOfStructNamed(Struct->Struct, OptionalPinFromPropertyName)
					|| IsChildOfStructNamed(Struct->Struct, MemberReferenceName)
					|| IsChildOfStructNamed(Struct->Struct, GuidName))
				{
					return false;
				}
			}
			if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Element))
			{
				const UClass* Class = Object->PropertyClass;
				if (Class && (Class->IsChildOf(UEdGraph::StaticClass()) || Class->IsChildOf(UEdGraphNode::StaticClass()) || Class->IsChildOf(UActorComponent::StaticClass())))
				{
					return false;
				}
			}
			return true;
		}

		/** Every property of Object declared on Floor or below that differs from the class default. */
		void ReadObjectProperties(FDabExtractContext& Ctx, const UObject& Object, const UClass& Floor, bool bEditableOnly,
			TConstArrayView<FName> Skip, const FString& Location, TArray<FDabModelProperty>& Out)
		{
			const UObject* Defaults = Object.GetClass()->GetDefaultObject();
			for (TFieldIterator<FProperty> It(Object.GetClass()); It; ++It)
			{
				const FProperty* Property = *It;
				const UClass* Owner = Property->GetOwnerClass();
				if (!Owner || !Owner->IsChildOf(&Floor)
					|| Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)
					|| (bEditableOnly && !Property->HasAnyPropertyFlags(CPF_Edit))
					|| Skip.Contains(Property->GetFName()))
				{
					continue;
				}
				const void* Value = Property->ContainerPtrToValuePtr<void>(&Object);
				const void* Default = Property->ContainerPtrToValuePtr<void>(Defaults);
				if (Property->Identical(Value, Default, 0))
				{
					continue;
				}
				const FString Name = Property->GetName();
				FDabExprPtr Converted = ConvertPropertyValue(Ctx, *Property, Value, Default, Location + TEXT(".") + Name);
				if (Converted.IsValid() && !IsEmptyTuple(*Converted))
				{
					AddProperty(Out, Name, MoveTemp(Converted));
				}
			}
		}

		/** The node feeding a pin, through reroutes; null when nothing is linked. */
		const UEdGraphNode* FollowLink(const UEdGraphPin& Pin)
		{
			const UEdGraphPin* Current = &Pin;
			for (int32 Guard = 0; Guard < 1024; ++Guard)
			{
				const UEdGraphPin* Source = Current->LinkedTo.Num() > 0 ? Current->LinkedTo[0] : nullptr;
				const UEdGraphNode* Node = Source ? Source->GetOwningNodeUnchecked() : nullptr;
				const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node);
				if (!Knot)
				{
					return Node;
				}
				Current = Knot->GetInputPin();
				if (!Current)
				{
					return nullptr;
				}
			}
			return nullptr;
		}

		const UEdGraphPin* FindFirstPoseInput(const UEdGraphNode& Node)
		{
			for (const UEdGraphPin* Pin : Node.Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && UAnimationGraphSchema::IsPosePin(Pin->PinType))
				{
					return Pin;
				}
			}
			return nullptr;
		}

		/** The editor's binding key for a pin: the pin name, or `__CustomProperty_<Pin>_<NodeGuid>` for a custom property pin. */
		FString GetBindingPinName(const UAnimGraphNode_Base& Node, FName Key)
		{
			FString Name = Key.ToString();
			static const FString CustomPrefix(TEXT("__CustomProperty_"));
			if (Name.StartsWith(CustomPrefix, ESearchCase::CaseSensitive))
			{
				Name.RightChopInline(CustomPrefix.Len());
				if (!Name.RemoveFromEnd(TEXT("_") + Node.NodeGuid.ToString(), ESearchCase::IgnoreCase))
				{
					int32 Underscore = INDEX_NONE;
					if (Name.FindLastChar(TEXT('_'), Underscore))
					{
						Name.LeftInline(Underscore);
					}
				}
			}
			return Name;
		}

		FDabExprPtr ReadOwnFunctionBinding(FDabExtractContext& Ctx, FName FunctionName, const FString& Location);

		/**
		 * The node's Property Access bindings (pin name -> Self-rooted path). The binding object's class is private
		 * to AnimGraph, so its PropertyBindings map is read by reflection.
		 */
		TMap<FString, FDabExprPtr> ReadPropertyBindings(FDabExtractContext& Ctx, const UAnimGraphNode_Base& Node, const FString& NodeLocation)
		{
			TMap<FString, FDabExprPtr> Bound;
			const UObject* BindingObject = GetObjectPropertyByName(Node, TEXT("Binding"));
			if (!BindingObject)
			{
				return Bound;
			}
			const FMapProperty* Map = FindFProperty<FMapProperty>(BindingObject->GetClass(), TEXT("PropertyBindings"));
			const FStructProperty* ValueStruct = Map ? CastField<FStructProperty>(Map->ValueProp) : nullptr;
			if (!Map || !Map->KeyProp->IsA<FNameProperty>() || !ValueStruct || ValueStruct->Struct->GetFName() != AnimGraphNodePropertyBindingName)
			{
				Ctx.AddUnsupported(NodeLocation, FString::Printf(TEXT("its bindings are held by %s, which the extractor cannot read"), *BindingObject->GetClass()->GetName()));
				return Bound;
			}

			FScriptMapHelper Helper(Map, Map->ContainerPtrToValuePtr<void>(BindingObject));
			for (FScriptMapHelper::FIterator It(Helper); It; ++It)
			{
				const FName Key = *reinterpret_cast<const FName*>(Helper.GetKeyPtr(It.GetInternalIndex()));
				const FAnimGraphNodePropertyBinding& Entry = *reinterpret_cast<const FAnimGraphNodePropertyBinding*>(Helper.GetValuePtr(It.GetInternalIndex()));
				if (!Entry.bIsBound)
				{
					continue;
				}
				const FString PinName = GetBindingPinName(Node, Key);
				const FString Location = NodeLocation + TEXT(".") + PinName;
				FDabExprPtr Inlined = Entry.PropertyPath.Num() == 1 ? ReadOwnFunctionBinding(Ctx, FName(*Entry.PropertyPath[0]), Location) : nullptr;
				Bound.Add(PinName, Inlined ? MoveTemp(Inlined) : MakeAccessPath(Ctx, Entry.PropertyPath, Location));
			}
			return Bound;
		}

		/**
		 * A binding to one of the Blueprint's own functions: a slot getter is `Assets.X`; any other function
		 * that only computes its return value is that value's expression. Null when it is neither.
		 */
		FDabExprPtr ReadOwnFunctionBinding(FDabExtractContext& Ctx, FName FunctionName, const FString& Location)
		{
			const UEdGraph* const* Graph = Ctx.FunctionGraphs.Find(FunctionName);
			if (!Graph || !*Graph)
			{
				return nullptr;
			}
			if (const FString* Slot = Ctx.AssetSlotFunctions.Find(FunctionName))
			{
				Ctx.UsedFunctions.Add(FunctionName);
				return MakeMember(MakeName(TEXT("Assets")), *Slot);
			}

			const UK2Node_FunctionResult* Result = nullptr;
			for (const UEdGraphNode* Node : (*Graph)->Nodes)
			{
				if (const UK2Node_FunctionResult* Candidate = Cast<UK2Node_FunctionResult>(Node))
				{
					Result = Candidate;
				}
			}
			const UEdGraphPin* ReturnPin = Result ? Result->FindPin(UEdGraphSchema_K2::PN_ReturnValue, EGPD_Input) : nullptr;
			if (!ReturnPin)
			{
				return nullptr;
			}
			FDabLowering Lowering(Ctx, Location);
			FDabExprPtr Expr = LowerInputPin(Lowering, *ReturnPin, nullptr);
			if (Lowering.Failures > 0 || !Expr)
			{
				return nullptr;
			}
			Ctx.UsedFunctions.Add(FunctionName);
			return Expr;
		}

		/**
		 * A pin fed every update: linked to K2 nodes (lowered to an expression) or bound by Property Access.
		 * Records the binding and returns true; returns false for a pin that holds a constant.
		 */
		bool ReadPinFeed(FDabExtractContext& Ctx, const UEdGraphPin& Pin, const FString& GraphLocation,
			TMap<FString, FDabExprPtr>& Bound, TArray<FDabModelBinding>& OutBindings)
		{
			const FString PinName = Pin.PinName.ToString();
			if (Pin.LinkedTo.Num() > 0 && Pin.LinkedTo[0])
			{
				// A link wins over a binding, as in the compiler (linking clears the pin's bindings).
				FDabLowering Lowering(Ctx, GraphLocation);
				AddBinding(OutBindings, PinName, LowerInputPin(Lowering, Pin, nullptr));
				Bound.Remove(PinName);
				return true;
			}
			if (FDabExprPtr* Path = Bound.Find(PinName))
			{
				AddBinding(OutBindings, PinName, MoveTemp(*Path));
				Bound.Remove(PinName);
				return true;
			}
			return false;
		}

		struct FNodeValues
		{
			TArray<FDabModelProperty> Properties;
			TArray<FDabModelBinding> Bindings;
			/** Pins already accounted for (property pins, call arguments). */
			TSet<const UEdGraphPin*> HandledPins;
		};

		/**
		 * The node's values: runtime-struct fields that differ from the class default (pin defaults applied as the
		 * compiler applies them), editor-node properties that do, bindings, and custom property pins.
		 */
		void ReadNodeValues(FDabExtractContext& Ctx, const UAnimGraphNode_Base& Node, const FString& GraphLocation, FNodeValues& Out)
		{
			const FString NodeLocation = GraphLocation + TEXT("/") + Node.GetName();
			const UObject* Defaults = Node.GetClass()->GetDefaultObject();
			TMap<FString, FDabExprPtr> Bound = ReadPropertyBindings(Ctx, Node, NodeLocation);

			// Runtime struct.
			TSet<FName> StructFieldNames;
			const FStructProperty* NodeProperty = Node.GetFNodeProperty();
			if (NodeProperty)
			{
				const UScriptStruct& Struct = *NodeProperty->Struct;
				const void* Stored = NodeProperty->ContainerPtrToValuePtr<void>(&Node);
				const void* DefaultNode = NodeProperty->ContainerPtrToValuePtr<void>(Defaults);

				// The effective node: the stored struct with every shown, constant pin's default pushed in.
				FDabTempStruct Effective(Struct);
				Struct.CopyScriptStruct(Effective.Get(), Stored);

				for (TFieldIterator<FProperty> It(&Struct); It; ++It)
				{
					const FProperty* Field = *It;
					const FString FieldName = Field->GetName();
					StructFieldNames.Add(Field->GetFName());
					if (!IsRecordedNodeField(*Field))
					{
						// A pin of a field the model does not keep is still not a custom property pin.
						if (const UEdGraphPin* SkippedPin = Node.FindPin(FieldName, EGPD_Input))
						{
							Out.HandledPins.Add(SkippedPin);
						}
						continue;
					}

					uint8* Value = Field->ContainerPtrToValuePtr<uint8>(Effective.Get());
					const void* Default = Field->ContainerPtrToValuePtr<void>(DefaultNode);

					if (const FArrayProperty* Array = CastField<FArrayProperty>(Field))
					{
						FScriptArrayHelper Helper(Array, Value);
						for (int32 Index = 0; Index < Helper.Num(); ++Index)
						{
							const UEdGraphPin* Pin = Node.FindPin(FString::Printf(TEXT("%s_%d"), *FieldName, Index), EGPD_Input);
							if (!Pin)
							{
								continue;
							}
							Out.HandledPins.Add(Pin);
							uint8* Element = Helper.GetRawPtr(Index);
							if (ReadPinFeed(Ctx, *Pin, GraphLocation, Bound, Out.Bindings))
							{
								// A fed element holds no constant: the zero value keeps the array's shape.
								Array->Inner->ClearValue(Element);
							}
							else
							{
								FBlueprintEditorUtils::PropertyValueFromString_Direct(Array->Inner, Pin->GetDefaultAsString(), Element);
							}
						}
					}
					else if (const UEdGraphPin* Pin = Node.FindPin(FieldName, EGPD_Input))
					{
						Out.HandledPins.Add(Pin);
						if (ReadPinFeed(Ctx, *Pin, GraphLocation, Bound, Out.Bindings))
						{
							continue;
						}
						FBlueprintEditorUtils::PropertyValueFromString_Direct(Field, Pin->GetDefaultAsString(), Value);
					}

					if (Field->Identical(Value, Default, 0))
					{
						continue;
					}
					FDabExprPtr Converted = ConvertPropertyValue(Ctx, *Field, Value, Default, NodeLocation + TEXT(".") + FieldName);
					if (Converted.IsValid() && !IsEmptyTuple(*Converted))
					{
						AddProperty(Out.Properties, FieldName, MoveTemp(Converted));
					}
				}
			}

			// Editor node: Tag, AlwaysDynamicProperties, and what node classes add (BoundEnum, VisibleEnumEntries, ...).
			static const FName TagName(TEXT("Tag"));
			static const FName AlwaysDynamicPropertiesName(TEXT("AlwaysDynamicProperties"));
			for (TFieldIterator<FProperty> It(Node.GetClass()); It; ++It)
			{
				const FProperty* Property = *It;
				if (Property == NodeProperty)
				{
					continue;
				}
				const UClass* Owner = Property->GetOwnerClass();
				if (!Owner || !Owner->IsChildOf(UAnimGraphNode_Base::StaticClass()))
				{
					continue;
				}
				if (Owner == UAnimGraphNode_Base::StaticClass() && Property->GetFName() != TagName && Property->GetFName() != AlwaysDynamicPropertiesName)
				{
					continue;
				}
				if (!IsRecordedEditorProperty(*Property))
				{
					continue;
				}

				const void* Value = Property->ContainerPtrToValuePtr<void>(&Node);
				const void* Default = Property->ContainerPtrToValuePtr<void>(Defaults);
				if (Property->Identical(Value, Default, 0))
				{
					continue;
				}
				const FString Name = Property->GetName();
				if (Property->GetFName() == TagName)
				{
					// The editor node's Tag is the one the compiler uses (linked graphs are found by it at run
					// time); a runtime field of the same name only receives it at compile time.
					Out.Properties.RemoveAll([](const Model::FDabModelProperty& Existing) { return Existing.Name == TEXT("Tag"); });
				}
				else if (StructFieldNames.Contains(Property->GetFName()))
				{
					Ctx.AddUnsupported(NodeLocation + TEXT(".") + Name,
						TEXT("the editor node's property has the name of a runtime node field; only the field is recorded"));
					continue;
				}
				FDabExprPtr Converted = ConvertPropertyValue(Ctx, *Property, Value, Default, NodeLocation + TEXT(".") + Name);
				if (Converted.IsValid() && !IsEmptyTuple(*Converted))
				{
					AddProperty(Out.Properties, Name, MoveTemp(Converted));
				}
			}

			// Pins no runtime field explains: custom property pins (linked graphs and layers, Control Rig).
			for (const UEdGraphPin* Pin : Node.Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input || Pin->bOrphanedPin || Pin->ParentPin || Out.HandledPins.Contains(Pin)
					|| UAnimationGraphSchema::IsPosePin(Pin->PinType) || UEdGraphSchema_K2::IsExecPin(*Pin))
				{
					continue;
				}
				Out.HandledPins.Add(Pin);
				if (ReadPinFeed(Ctx, *Pin, GraphLocation, Bound, Out.Bindings))
				{
					continue;
				}
				if (GetDefault<UEdGraphSchema_K2>()->DoesDefaultValueMatchAutogenerated(*Pin))
				{
					continue;
				}
				const FString PinName = Pin->PinName.ToString();
				const FString PinLocation = NodeLocation + TEXT(".") + PinName;
				const FProperty* PinProperty = Node.GetPinProperty(Pin->PinName);
				FDabExprPtr Converted = PinProperty ? ConvertPinDefaultAs(Ctx, *Pin, *PinProperty, PinLocation) : FDabExprPtr();
				if (!Converted.IsValid())
				{
					Converted = ConvertPinDefaultByType(Ctx, *Pin, PinLocation);
				}
				if (Converted.IsValid())
				{
					AddProperty(Out.Properties, PinName, MoveTemp(Converted));
				}
			}

			// Bindings whose pin is hidden still compile (the compiler binds by property name).
			for (TPair<FString, FDabExprPtr>& Entry : Bound)
			{
				AddBinding(Out.Bindings, Entry.Key, MoveTemp(Entry.Value));
			}
		}

		void ReadPoseInputs(FDabExtractContext& Ctx, const UAnimGraphNode_Base& Node, const FString& NodeLocation, FDabModelNode& OutNode)
		{
			for (const UEdGraphPin* Pin : Node.Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input || Pin->bOrphanedPin || !UAnimationGraphSchema::IsPosePin(Pin->PinType))
				{
					continue;
				}
				Model::FDabModelPoseLink Link;
				Link.PinName = Pin->PinName.ToString();

				const UEdGraphNode* Source = FollowLink(*Pin);
				if (const UAnimGraphNode_Base* AnimSource = Cast<UAnimGraphNode_Base>(Source))
				{
					Link.SourceNodeId = AnimSource->GetName();
					OutNode.PoseInputs.Add(MoveTemp(Link));
				}
				else if (Source)
				{
					Ctx.AddUnsupported(NodeLocation + TEXT(".") + Link.PinName, FString::Printf(TEXT("the pose comes from a %s node"), *Source->GetClass()->GetName()));
				}
				else if (CastField<FArrayProperty>(Node.GetPinProperty(Pin->PinName)))
				{
					// An unlinked element of a pose array still counts: it keeps the array's size.
					OutNode.PoseInputs.Add(MoveTemp(Link));
				}
			}
		}

		void ReadNodeFunctions(FDabExtractContext& Ctx, const UAnimGraphNode_Base& Node, const FString& NodeLocation, FDabModelNode& OutNode)
		{
			const auto Read = [&Ctx, &NodeLocation, &OutNode](const FMemberReference& Reference, const TCHAR* Site, const TCHAR* PropertyName)
			{
				if (TOptional<Model::FDabModelCall> Call = ExtractFunctionReference(Ctx, Reference, NodeLocation + TEXT(".") + PropertyName))
				{
					Model::FDabModelNodeFunction Function;
					Function.Site = Site;
					Function.Call = MoveTemp(Call.GetValue());
					OutNode.NodeFunctions.Add(MoveTemp(Function));
				}
			};
			Read(Node.InitialUpdateFunction, TEXT("InitialUpdate"), TEXT("InitialUpdateFunction"));
			Read(Node.BecomeRelevantFunction, TEXT("BecomeRelevant"), TEXT("BecomeRelevantFunction"));
			Read(Node.UpdateFunction, TEXT("Update"), TEXT("UpdateFunction"));
		}

		/** A CallFunction node's call: the function its inner prototype node names, arguments from the outer node's pins. */
		TOptional<Model::FDabModelCall> ReadCallFunctionNode(FDabExtractContext& Ctx, const UAnimGraphNode_Base& Node, const FString& GraphLocation, FNodeValues& Values)
		{
			const FString NodeLocation = GraphLocation + TEXT("/") + Node.GetName();
			const UK2Node_CallFunction* Prototype = Cast<UK2Node_CallFunction>(GetObjectPropertyByName(Node, TEXT("CallFunctionPrototype")));
			if (!Prototype)
			{
				Ctx.AddUnsupported(NodeLocation, TEXT("a CallFunction node without a function"));
				return TOptional<Model::FDabModelCall>();
			}

			const UFunction* Function = Prototype->GetTargetFunction();
			if (Function && Function->HasAnyFunctionFlags(FUNC_Static))
			{
				Ctx.AddUnsupported(NodeLocation, FString::Printf(TEXT("calls the library function %s::%s; only functions on the instance can be called"),
					*Function->GetOwnerClass()->GetName(), *Function->GetName()));
				return TOptional<Model::FDabModelCall>();
			}

			Model::FDabModelCall Call;
			Call.Target = MakeName(TEXT("Self"));
			Call.Function = Prototype->FunctionReference.GetMemberName().ToString();

			FDabLowering Lowering(Ctx, GraphLocation);
			for (const UEdGraphPin* PrototypePin : Prototype->Pins)
			{
				if (!PrototypePin || PrototypePin->Direction != EGPD_Input || UEdGraphSchema_K2::IsExecPin(*PrototypePin)
					|| PrototypePin->PinName == UEdGraphSchema_K2::PN_Self)
				{
					continue;
				}
				const UEdGraphPin* Pin = Node.FindPin(PrototypePin->PinName, EGPD_Input);
				if (!Pin)
				{
					continue;
				}
				Values.HandledPins.Add(Pin);
				const bool bLinked = Pin->LinkedTo.Num() > 0;
				if (!bLinked && GetDefault<UEdGraphSchema_K2>()->DoesDefaultValueMatchAutogenerated(*Pin))
				{
					continue;
				}
				Model::FDabModelArgument Argument;
				Argument.Name = Pin->PinName.ToString();
				Argument.Value = LowerInputPin(Lowering, *Pin, Function ? FindFProperty<FProperty>(Function, Pin->PinName) : nullptr);
				Call.Args.Add(MoveTemp(Argument));
			}
			return TOptional<Model::FDabModelCall>(MoveTemp(Call));
		}

		void ExtractStateMachine(FDabExtractContext& Ctx, const UAnimGraphNode_StateMachineBase& MachineNode, const FString& MachineName);

		/** One anim node into OutGraph; a state machine node also extracts its machine. */
		void ExtractNode(FDabExtractContext& Ctx, const UAnimGraphNode_Base& Node, const FString& GraphLocation, FDabModelGraph& OutGraph)
		{
			const FString NodeLocation = GraphLocation + TEXT("/") + Node.GetName();

			FDabModelNode ModelNode;
			ModelNode.Id = Node.GetName();
			ModelNode.Guid = Node.NodeGuid;
			ModelNode.ClassName = Node.GetClass()->GetName();

			if (const UAnimGraphNode_UseCachedPose* Use = Cast<UAnimGraphNode_UseCachedPose>(&Node))
			{
				const UAnimGraphNode_SaveCachedPose* Save = Use->SaveCachedPoseNode.Get();
				if (Save)
				{
					ModelNode.CacheName = Save->CacheName;
				}
				else if (const FStrProperty* NameOfCache = FindFProperty<FStrProperty>(Use->GetClass(), TEXT("NameOfCache")))
				{
					ModelNode.CacheName = NameOfCache->GetPropertyValue_InContainer(Use);
				}
				if (ModelNode.CacheName.IsEmpty())
				{
					Ctx.AddUnsupported(NodeLocation, TEXT("uses a cached pose that no longer exists"));
				}
				OutGraph.Nodes.Add(MoveTemp(ModelNode));
				return;
			}

			FNodeValues Values;
			const bool bCallFunction = IsInstanceOfClassPath(&Node, TEXT("/Script/AnimGraph"), TEXT("AnimGraphNode_CallFunction"));
			if (bCallFunction)
			{
				ModelNode.Call = ReadCallFunctionNode(Ctx, Node, GraphLocation, Values);
			}
			ReadNodeValues(Ctx, Node, GraphLocation, Values);

			if (const UAnimGraphNode_StateMachineBase* MachineNode = Cast<UAnimGraphNode_StateMachineBase>(&Node))
			{
				// The machine node's settings belong to the machine (the text writes them as its metadata).
				const FString* MachineName = Ctx.MachineNames.Find(MachineNode->EditorStateMachineGraph.Get());
				if (MachineName)
				{
					ModelNode.StateMachineName = *MachineName;
					ExtractStateMachine(Ctx, *MachineNode, *MachineName);
					if (Model::FDabModelStateMachine* Machine = Ctx.OutModel.StateMachines.FindByPredicate(
						[MachineName](const Model::FDabModelStateMachine& Candidate) { return Candidate.Name == *MachineName; }))
					{
						Machine->Properties = MoveTemp(Values.Properties);
					}
				}
				else
				{
					Ctx.AddUnsupported(NodeLocation, TEXT("a state machine node without a state machine graph"));
				}
				for (const FDabModelBinding& Binding : Values.Bindings)
				{
					Ctx.AddUnsupported(NodeLocation + TEXT(".") + Binding.PinName, TEXT("a binding on a state machine node cannot be expressed"));
				}
				Values.Bindings.Reset();
			}
			else
			{
				ModelNode.Properties = MoveTemp(Values.Properties);
			}
			ModelNode.Bindings = MoveTemp(Values.Bindings);

			if (const UAnimGraphNode_SaveCachedPose* Save = Cast<UAnimGraphNode_SaveCachedPose>(&Node))
			{
				ModelNode.CacheName = Save->CacheName;
			}
			if (const UAnimGraphNode_LinkedInputPose* Input = Cast<UAnimGraphNode_LinkedInputPose>(&Node))
			{
				ModelNode.InputPoseName = Input->Node.Name.ToString();
			}

			if (!bCallFunction && !Node.IsA<UAnimGraphNode_StateMachineBase>())
			{
				for (const UEdGraph* SubGraph : Node.GetSubGraphs())
				{
					if (SubGraph)
					{
						Ctx.AddUnsupported(NodeLocation, FString::Printf(TEXT("a %s node holds a nested graph (%s), which the language cannot say"),
							*ModelNode.ClassName, *SubGraph->GetName()));
						break;
					}
				}
			}

			ReadPoseInputs(Ctx, Node, NodeLocation, ModelNode);
			ReadNodeFunctions(Ctx, Node, NodeLocation, ModelNode);
			OutGraph.Nodes.Add(MoveTemp(ModelNode));
		}

		/** Root, StateResult or layer root: the node a graph's pose ends in. */
		const UAnimGraphNode_Base* FindResultNode(const UEdGraph& Graph)
		{
			for (const UEdGraphNode* Node : Graph.Nodes)
			{
				if (Node && (Node->IsA<UAnimGraphNode_Root>() || Node->IsA<UAnimGraphNode_StateResult>()))
				{
					return Cast<UAnimGraphNode_Base>(Node);
				}
			}
			return nullptr;
		}

		void ReadResultProperties(FDabExtractContext& Ctx, const UEdGraph& Graph, const UAnimGraphNode_Base* ResultNode, const FString& Location, FDabModelGraph& OutGraph)
		{
			if (ResultNode)
			{
				FNodeValues Values;
				ReadNodeValues(Ctx, *ResultNode, Location, Values);
				OutGraph.ResultProperties = MoveTemp(Values.Properties);
				// The AnimGraph's root carries no group and a layer's group defaults to the shared one; the CDO
				// holds neither reliably, so both count as default here.
				OutGraph.ResultProperties.RemoveAll([](const Model::FDabModelProperty& Property)
				{
					const Lang::FDabLiteralExpr* Literal = Property.Value ? Property.Value->As<Lang::FDabLiteralExpr>() : nullptr;
					return Property.Name == TEXT("LayerGroup") && Literal
						&& (Literal->Text.IsEmpty() || Literal->Text == FAnimNode_Root::DefaultSharedGroup.ToString());
				});
				for (const FDabModelBinding& Binding : Values.Bindings)
				{
					Ctx.AddUnsupported(Location + TEXT("/") + ResultNode->GetName() + TEXT(".") + Binding.PinName, TEXT("a binding on a graph's result node cannot be expressed"));
				}
			}

			// A linked graph's blend options live on the graph itself; the model keeps them with the result.
			if (Graph.IsA<UAnimationGraph>())
			{
				static const FName BlendOptionsName(TEXT("BlendOptions"));
				TArray<FName> Skip;
				for (TFieldIterator<FProperty> It(Graph.GetClass()); It; ++It)
				{
					if (It->GetFName() != BlendOptionsName)
					{
						Skip.Add(It->GetFName());
					}
				}
				ReadObjectProperties(Ctx, Graph, *UAnimationGraph::StaticClass(), false, Skip, Location, OutGraph.ResultProperties);
			}
		}

		/** A transition or conduit rule graph's condition. */
		FDabExprPtr ExtractRule(FDabExtractContext& Ctx, const UEdGraph* Graph, const FString& Location)
		{
			const UAnimGraphNode_TransitionResult* Result = nullptr;
			if (Graph)
			{
				for (const UEdGraphNode* Node : Graph->Nodes)
				{
					if (const UAnimGraphNode_TransitionResult* Candidate = Cast<UAnimGraphNode_TransitionResult>(Node))
					{
						Result = Candidate;
						break;
					}
				}
			}
			if (!Result)
			{
				Ctx.AddUnsupported(Location, TEXT("a rule graph without a result node"));
				return MakePlaceholder(TEXT("no rule"));
			}

			static const FName CanEnterName(TEXT("bCanEnterTransition"));
			const FString NodeLocation = Location + TEXT("/") + Result->GetName();
			const UEdGraphPin* Pin = Result->FindPin(CanEnterName, EGPD_Input);
			if (Pin && Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0])
			{
				FDabLowering Lowering(Ctx, Location);
				return LowerInputPin(Lowering, *Pin, nullptr);
			}

			TMap<FString, FDabExprPtr> Bound = ReadPropertyBindings(Ctx, *Result, NodeLocation);
			if (FDabExprPtr* Path = Bound.Find(CanEnterName.ToString()))
			{
				return MoveTemp(*Path);
			}
			if (Pin)
			{
				return MakeBool(Pin->GetDefaultAsString().ToBool());
			}
			return MakeBool(Result->Node.bCanEnterTransition);
		}

		bool IsLiteralTrue(const FDabExpr& Expr)
		{
			const FDabLiteralExpr* Literal = Expr.As<FDabLiteralExpr>();
			return Literal && Literal->LiteralKind == EDabLiteralKind::Bool && Literal->bBool;
		}

		Model::FDabModelState ExtractState(FDabExtractContext& Ctx, const UAnimStateNode& StateNode, const FString& MachineName)
		{
			Model::FDabModelState State;
			State.Name = GetStateNodeName(&StateNode);
			State.Guid = StateNode.NodeGuid;
			const FString Location = MachineName + TEXT("/") + State.Name;

			static const FName StateSkip[] = {FName(TEXT("BoundGraph")), FName(TEXT("StateType"))};
			ReadObjectProperties(Ctx, StateNode, *UAnimStateNode::StaticClass(), true, StateSkip, Location, State.Properties);

			State.Graph.Name = State.Name;
			const UEdGraph* StateGraph = StateNode.BoundGraph.Get();
			if (!StateGraph)
			{
				Ctx.AddUnsupported(Location, TEXT("a state without a graph"));
				return State;
			}

			// The functions are member references on the editor's result node; the copies in its runtime struct can be stale.
			if (const UAnimGraphNode_StateResult* Result = Cast<UAnimGraphNode_StateResult>(FindResultNode(*StateGraph)))
			{
				const FString ResultLocation = Location + TEXT("/") + Result->GetName();
				State.OnEnter = ExtractFunctionReference(Ctx, Result->StateEntryFunction, ResultLocation + TEXT(".StateEntryFunction"));
				State.OnExit = ExtractFunctionReference(Ctx, Result->StateExitFunction, ResultLocation + TEXT(".StateExitFunction"));
				State.OnBlended = ExtractFunctionReference(Ctx, Result->StateFullyBlendedInFunction, ResultLocation + TEXT(".StateFullyBlendedInFunction"));
				if (!Result->StateFullyBlendedOutFunction.GetMemberName().IsNone())
				{
					Ctx.AddUnsupported(ResultLocation + TEXT(".StateFullyBlendedOutFunction"),
						FString::Printf(TEXT("the state calls %s when fully blended out; the language has no such hook"),
							*Result->StateFullyBlendedOutFunction.GetMemberName().ToString()));
				}
			}

			ExtractAnimGraph(Ctx, *StateGraph, Location, State.Graph);
			return State;
		}

		Model::FDabModelTransition ExtractTransition(FDabExtractContext& Ctx, const UAnimStateTransitionNode& TransitionNode, const FString& MachineName)
		{
			Model::FDabModelTransition Transition;
			Transition.Guid = TransitionNode.NodeGuid;
			Transition.From = GetStateNodeName(TransitionNode.GetPreviousState());
			Transition.To = GetStateNodeName(TransitionNode.GetNextState());
			const FString Location = FString::Printf(TEXT("%s/%s -> %s"), *MachineName, *Transition.From, *Transition.To);

			if (TransitionNode.bAutomaticRuleBasedOnSequencePlayerInState)
			{
				Transition.ConditionKind = Model::EDabConditionKind::Auto;
			}
			else
			{
				Transition.Condition = ExtractRule(Ctx, TransitionNode.BoundGraph.Get(), Location);
			}
			if (TransitionNode.bSharedRules)
			{
				Transition.SharedRuleName = TransitionNode.SharedRulesName;
			}

			// The blend profile sits in a wrapper whose fields are private; the option is the profile itself.
			static const FName BlendProfileWrapperName(TEXT("BlendProfileWrapper"));
			static const FName TransitionSkip[] = {FName(TEXT("bAutomaticRuleBasedOnSequencePlayerInState")), BlendProfileWrapperName};
			ReadObjectProperties(Ctx, TransitionNode, *UAnimStateTransitionNode::StaticClass(), true, TransitionSkip, Location, Transition.Options);

			const UAnimStateTransitionNode* Defaults = GetDefault<UAnimStateTransitionNode>();
			if (const FStructProperty* WrapperProperty = FindFProperty<FStructProperty>(UAnimStateTransitionNode::StaticClass(), BlendProfileWrapperName))
			{
				const void* Value = WrapperProperty->ContainerPtrToValuePtr<void>(&TransitionNode);
				const void* Default = WrapperProperty->ContainerPtrToValuePtr<void>(Defaults);
				if (!WrapperProperty->Identical(Value, Default, 0))
				{
					if (TransitionNode.BlendProfileWrapper.UsesSkeletonBlendProfile())
					{
						AddProperty(Transition.Options, TEXT("BlendProfile"), MakeObjectReference(TransitionNode.BlendProfileWrapper.GetBlendProfile().Get()));
					}
					else if (FDabExprPtr Converted = ConvertPropertyValue(Ctx, *WrapperProperty, Value, Default, Location + TEXT(".BlendProfileWrapper")))
					{
						AddProperty(Transition.Options, WrapperProperty->GetName(), MoveTemp(Converted));
					}
				}
			}

			if (TransitionNode.LogicType == ETransitionLogicType::TLT_Custom)
			{
				Ctx.AddUnsupported(Location, TEXT("the transition blends through a custom blend graph"));
			}
			return Transition;
		}

		void ExtractStateMachine(FDabExtractContext& Ctx, const UAnimGraphNode_StateMachineBase& MachineNode, const FString& MachineName)
		{
			Model::FDabModelStateMachine Machine;
			Machine.Name = MachineName;

			const UAnimationStateMachineGraph* Graph = MachineNode.EditorStateMachineGraph.Get();
			if (Graph && Graph->EntryNode)
			{
				Machine.Entry = GetStateNodeName(Graph->EntryNode->GetOutputNode());
			}
			if (Machine.Entry.IsEmpty())
			{
				Ctx.AddUnsupported(MachineName, TEXT("the state machine has no entry state"));
			}

			if (Graph)
			{
				for (const UEdGraphNode* Node : Graph->Nodes)
				{
					if (!Node || Node->IsA<UEdGraphNode_Comment>() || Node->IsA<UAnimStateEntryNode>())
					{
						continue;
					}
					if (const UAnimStateNode* StateNode = Cast<UAnimStateNode>(Node))
					{
						Machine.States.Add(ExtractState(Ctx, *StateNode, MachineName));
					}
					else if (const UAnimStateConduitNode* ConduitNode = Cast<UAnimStateConduitNode>(Node))
					{
						Model::FDabModelConduit Conduit;
						Conduit.Name = GetStateNodeName(ConduitNode);
						Conduit.Guid = ConduitNode->NodeGuid;
						FDabExprPtr Condition = ExtractRule(Ctx, ConduitNode->BoundGraph.Get(), MachineName + TEXT("/") + Conduit.Name);
						if (Condition.IsValid() && !IsLiteralTrue(*Condition))
						{
							Conduit.Condition = MoveTemp(Condition);
						}
						Machine.Conduits.Add(MoveTemp(Conduit));
					}
					else if (const UAnimStateAliasNode* AliasNode = Cast<UAnimStateAliasNode>(Node))
					{
						Model::FDabModelAlias Alias;
						Alias.Name = AliasNode->StateAliasName;
						Alias.Guid = AliasNode->NodeGuid;
						Alias.bGlobal = AliasNode->bGlobalAlias;
						if (!Alias.bGlobal)
						{
							for (const TWeakObjectPtr<UAnimStateNodeBase>& Aliased : AliasNode->GetAliasedStates())
							{
								if (const UAnimStateNodeBase* AliasedState = Aliased.Get())
								{
									Alias.States.Add(GetStateNodeName(AliasedState));
								}
							}
							Alias.States.StableSort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
						}
						Machine.Aliases.Add(MoveTemp(Alias));
					}
					else if (const UAnimStateTransitionNode* TransitionNode = Cast<UAnimStateTransitionNode>(Node))
					{
						Machine.Transitions.Add(ExtractTransition(Ctx, *TransitionNode, MachineName));
					}
					else
					{
						Ctx.AddUnsupported(MachineName + TEXT("/") + Node->GetName(), FString::Printf(TEXT("a %s node in a state machine"), *Node->GetClass()->GetName()));
					}
				}
			}

			Ctx.OutModel.StateMachines.Add(MoveTemp(Machine));
		}
	}

	void CollectStateMachineNames(FDabExtractContext& Ctx, const UEdGraph& Graph)
	{
		for (const UEdGraphNode* Node : Graph.Nodes)
		{
			const UAnimGraphNode_StateMachineBase* MachineNode = Cast<UAnimGraphNode_StateMachineBase>(Node);
			const UAnimationStateMachineGraph* MachineGraph = MachineNode ? MachineNode->EditorStateMachineGraph.Get() : nullptr;
			if (!MachineGraph || Ctx.MachineNames.Contains(MachineGraph))
			{
				continue;
			}

			// Two machines may share a graph name (their graphs live under different nodes); the model's names may not.
			const FString BaseName = MachineGraph->GetName();
			FString Name = BaseName;
			for (int32 Suffix = 2; ; ++Suffix)
			{
				bool bTaken = false;
				for (const TPair<const UEdGraph*, FString>& Entry : Ctx.MachineNames)
				{
					if (Entry.Value.Equals(Name, ESearchCase::CaseSensitive))
					{
						bTaken = true;
						break;
					}
				}
				if (!bTaken)
				{
					break;
				}
				Name = FString::Printf(TEXT("%s_%d"), *BaseName, Suffix);
			}
			Ctx.MachineNames.Add(MachineGraph, Name);

			for (const UEdGraphNode* StateNode : MachineGraph->Nodes)
			{
				const UAnimStateNode* State = Cast<UAnimStateNode>(StateNode);
				if (State && State->BoundGraph)
				{
					CollectStateMachineNames(Ctx, *State->BoundGraph);
				}
			}
		}
	}

	void ExtractAnimGraph(FDabExtractContext& Ctx, const UEdGraph& Graph, const FString& Location, FDabModelGraph& OutGraph)
	{
		const UAnimGraphNode_Base* ResultNode = FindResultNode(Graph);
		if (ResultNode)
		{
			if (const UEdGraphPin* ResultPin = FindFirstPoseInput(*ResultNode))
			{
				const UEdGraphNode* Source = FollowLink(*ResultPin);
				if (const UAnimGraphNode_Base* AnimSource = Cast<UAnimGraphNode_Base>(Source))
				{
					OutGraph.ResultNodeId = AnimSource->GetName();
				}
				else if (Source)
				{
					Ctx.AddUnsupported(Location + TEXT("/") + ResultNode->GetName(), FString::Printf(TEXT("the result pose comes from a %s node"), *Source->GetClass()->GetName()));
				}
			}
		}
		else
		{
			Ctx.AddUnsupported(Location, TEXT("an anim graph without a result node"));
		}
		ReadResultProperties(Ctx, Graph, ResultNode, Location, OutGraph);

		for (const UEdGraphNode* Node : Graph.Nodes)
		{
			if (!Node || Node == ResultNode || Node->IsA<UEdGraphNode_Comment>() || Node->IsA<UK2Node_Knot>() || !Node->IsNodeEnabled())
			{
				continue;
			}
			if (const UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
			{
				ExtractNode(Ctx, *AnimNode, Location, OutGraph);
			}
			else if (!Node->IsA<UK2Node>())
			{
				// K2 nodes are read where they feed a pin; anything else has no place in an anim graph.
				Ctx.AddUnsupported(Location + TEXT("/") + Node->GetName(), FString::Printf(TEXT("a %s node in an anim graph"), *Node->GetClass()->GetName()));
			}
		}
	}

	void ExtractResultProperties(FDabExtractContext& Ctx, const UEdGraph& Graph, const FString& Location, FDabModelGraph& OutGraph)
	{
		ReadResultProperties(Ctx, Graph, FindResultNode(Graph), Location, OutGraph);
	}
}
