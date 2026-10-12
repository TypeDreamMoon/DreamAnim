// Copyright DreamMoon. All Rights Reserved.

#include "Rig/DcrExtract.h"

#include "DcrExecPins.h"
#include "DcrNodeDefaults.h"
#include "DcrValues.h"
#include "Engine/SkeletalMesh.h"
#include "RigVMCore/RigVMTemplate.h"
#include "RigVMModel/Nodes/RigVMAggregateNode.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMCommentNode.h"
#include "RigVMModel/Nodes/RigVMDispatchNode.h"
#include "RigVMModel/Nodes/RigVMFunctionEntryNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReturnNode.h"
#include "RigVMModel/Nodes/RigVMInvokeEntryNode.h"
#include "RigVMModel/Nodes/RigVMRerouteNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMTypeUtils.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyElements.h"
#include "Rigs/RigModuleDefines.h"
#include "Settings/ControlRigSettings.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Rig
{
	using namespace Model;

	namespace
	{
		/** What reading values and the hierarchy needs: where to report what cannot be held, and the slots values are spelled through. */
		struct FValueContext
		{
			explicit FValueContext(TArray<FDcrModelUnsupported>& InUnsupported)
				: Unsupported(InUnsupported)
			{
			}

			TArray<FDcrModelUnsupported>& Unsupported;
			/** By type; null where the type does not resolve. */
			TMap<FString, TSharedPtr<FDcrValueSlot>> Slots;

			void AddUnsupported(const FString& Location, const FString& Description)
			{
				FDcrModelUnsupported& Entry = Unsupported.AddDefaulted_GetRef();
				Entry.Location = Location;
				Entry.Description = Description;
			}

			FDcrValueSlot* GetSlot(const FString& Key, TFunctionRef<bool(FDcrValueSlot&)> Init)
			{
				if (const TSharedPtr<FDcrValueSlot>* Found = Slots.Find(Key))
				{
					return Found->Get();
				}
				TSharedPtr<FDcrValueSlot> Slot = MakeShared<FDcrValueSlot>();
				if (!Init(*Slot))
				{
					Slot.Reset();
				}
				Slots.Add(Key, Slot);
				return Slot.Get();
			}
		};

		struct FExtractContext : FValueContext
		{
			FExtractContext(const FDcrRig& InRig, FDcrModel& InOut)
				: FValueContext(InOut.Unsupported)
				, Rig(InRig)
				, Out(InOut)
			{
			}

			const FDcrRig& Rig;
			FDcrModel& Out;
			URigVMFunctionLibrary* Library = nullptr;
		};

		void ExtractGraphInto(FExtractContext& Context, URigVMGraph* Graph, FDcrModelGraph& Out, const FString& Location);
		FDcrModelVariable MakeVariable(FExtractContext& Context, const FRigVMGraphVariableDescription& Description, const FString& Location);

		FDcrType MakeType(const FString& CPPType, const UObject* TypeObject, const FName& TypeObjectPath = NAME_None)
		{
			FDcrType Type;
			Type.CPPType = CPPType;
			if (TypeObject)
			{
				Type.ObjectPath = TypeObject->GetPathName();
			}
			else if (!TypeObjectPath.IsNone())
			{
				Type.ObjectPath = TypeObjectPath.ToString();
			}
			// A user enum's CPP type is made up by the engine from its name and path, and older assets kept an
			// earlier spelling (its bare name): the one the engine makes now.
			const UEnum* Enum = Cast<UEnum>(TypeObject);
			if (!Enum && !Type.ObjectPath.IsEmpty())
			{
				Enum = Cast<UEnum>(RigVMTypeUtils::FindObjectFromCPPTypeObjectPath(Type.ObjectPath));
			}
			if (Enum && Enum->CppType.IsEmpty())
			{
				const FString EnumType = RigVMTypeUtils::CPPTypeFromEnum(Enum);
				Type.CPPType = RigVMTypeUtils::IsArrayType(CPPType) ? RigVMTypeUtils::ArrayTypeFromBaseType(EnumType) : EnumType;
			}
			return Type;
		}

		// ---- One spelling per value ----------------------------------------------------------------------------
		//
		// Rig assets keep the text a pin was last given, so one struct value can sit in two assets with its
		// members in different orders. Every value that is kept is imported through its type and exported again.

		FDcrValueSlot* GetPropertySlot(FValueContext& Context, const FProperty& Property)
		{
			return Context.GetSlot(TEXT("P|") + Property.GetPathName(), [&Property](FDcrValueSlot& Slot) { return Slot.Init(Property); });
		}

		FDcrValueSlot* GetTypeSlot(FValueContext& Context, const FDcrType& Type)
		{
			return Context.GetSlot(TEXT("T|") + Type.CPPType + TEXT("|") + Type.ObjectPath, [&Type](FDcrValueSlot& Slot) { return Slot.Init(Type); });
		}

		FDcrValueSlot* GetStructSlot(FValueContext& Context, const UScriptStruct& Struct)
		{
			FDcrType Type;
			Type.CPPType = TEXT("F") + Struct.GetName();
			Type.ObjectPath = Struct.GetPathName();
			return GetTypeSlot(Context, Type);
		}

		/** Spells the text the slot's way. When it cannot be: the text stays as the asset has it, and Unsupported says so. */
		void Canonicalize(FValueContext& Context, FDcrValueSlot* Slot, const FString& Location, FString& InOutText)
		{
			if (InOutText.IsEmpty())
			{
				// No text is the type's default (an empty name is None): spelled as any default is.
				if (Slot && Slot->ImportText(FString()))
				{
					InOutText = Slot->ExportText();
				}
				return;
			}
			if (!Slot)
			{
				Context.AddUnsupported(Location, TEXT("the type of the value does not resolve; it is kept as the asset spells it"));
				return;
			}
			if (!Slot->ImportText(InOutText))
			{
				// RigVM keeps the default of an array variable or function pin as the default of one element: an
				// array with nothing in it.
				const FArrayProperty* Array = CastField<FArrayProperty>(Slot->GetProperty());
				FDcrValueSlot Element;
				FDcrValueSlot ElementDefault;
				if (Array && Element.Init(*Array->Inner) && ElementDefault.Init(*Array->Inner) && Element.ImportText(InOutText)
					&& Element.GetProperty()->Identical(Element.GetMemory(), ElementDefault.GetMemory()))
				{
					InOutText.Reset();
					return;
				}
				Context.AddUnsupported(Location, FString::Printf(TEXT("the value `%s` does not import as %s; it is kept as the asset spells it"),
					*InOutText.Left(200), Slot->GetProperty() ? *Slot->GetProperty()->GetCPPType() : TEXT("its type")));
				return;
			}
			InOutText = Slot->ExportText();
		}

		void CanonicalizeType(FValueContext& Context, const FDcrType& Type, const FString& Location, FString& InOutText)
		{
			if (!InOutText.IsEmpty())
			{
				Canonicalize(Context, GetTypeSlot(Context, Type), Location, InOutText);
			}
		}

		FString FloatText(double Value)
		{
			return FString::SanitizeFloat(Value);
		}

		/** Every number exact: an element's transforms are doubles the engine would write to six decimals. */
		template <typename T>
		FString ExportBaseStruct(const T& Value)
		{
			return ExportDcrStructText(*TBaseStructure<T>::Get(), &Value);
		}

		/** The identity to the bit: a transform holding -0 somewhere is not one, and says so. */
		bool IsIdentity(const FTransform& Transform)
		{
			return ExportBaseStruct(Transform) == ExportBaseStruct(FTransform::Identity);
		}

		void AddValue(TArray<FDcrValue>& Values, const FString& Name, const FString& Value)
		{
			FDcrValue& Entry = Values.AddDefaulted_GetRef();
			Entry.Name = Name;
			Entry.Value = Value;
		}

		/** Properties of a struct that differ from a default-constructed one, in export text. Transient and deprecated ones are not state. */
		void AddStructDifferences(FValueContext& Context, const FString& Location, const UScriptStruct* Struct, const void* Value, const void* Defaults, const TSet<FName>& Skip, TArray<FDcrValue>& Out)
		{
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				const FProperty* Property = *It;
				if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated) || Skip.Contains(Property->GetFName()))
				{
					continue;
				}
				if (AreDcrValuesSame(*Property, Property->ContainerPtrToValuePtr<void>(Value), Property->ContainerPtrToValuePtr<void>(Defaults)))
				{
					continue;
				}
				FString Text = ExportDcrValueText(*Property, Property->ContainerPtrToValuePtr<void>(Value));
				Canonicalize(Context, GetPropertySlot(Context, *Property), Location + TEXT(".") + Property->GetName(), Text);
				AddValue(Out, Property->GetName(), Text);
			}
		}

		/** A struct value (a transform, a colour) as export text, spelled the one way. */
		void AddStructValue(FValueContext& Context, const FString& Location, TArray<FDcrValue>& Values, const FString& Name, const UScriptStruct& Struct, FString Text)
		{
			Canonicalize(Context, GetStructSlot(Context, Struct), Location + TEXT(".") + Name, Text);
			AddValue(Values, Name, Text);
		}

		/** The path a soft object property of the object holds; false when the object has no such property. */
		bool ReadSoftObjectPath(UObject* Object, const TCHAR* PropertyName, FString& OutPath)
		{
			if (!Object)
			{
				return false;
			}
			const FSoftObjectProperty* Property = FindFProperty<FSoftObjectProperty>(Object->GetClass(), PropertyName);
			if (!Property)
			{
				return false;
			}
			const FSoftObjectPtr Value = Property->GetPropertyValue_InContainer(Object);
			OutPath = Value.IsNull() ? FString() : Value.ToSoftObjectPath().ToString();
			return true;
		}

		/** A property of the object in export text; false when the object has no such property. */
		bool ExportObjectProperty(FValueContext& Context, UObject* Object, const TCHAR* PropertyName, const FString& Location, FString& OutText)
		{
			if (!Object)
			{
				return false;
			}
			const FProperty* Property = Object->GetClass()->FindPropertyByName(PropertyName);
			if (!Property)
			{
				return false;
			}
			OutText.Reset();
			Property->ExportText_InContainer(0, OutText, Object, nullptr, Object, PPF_None);
			Canonicalize(Context, GetPropertySlot(Context, *Property), Location, OutText);
			return true;
		}

		// ---- Nodes ----------------------------------------------------------------------------------------------

		/** The wildcard root pins of a template node and what they were resolved to. The pins are read from Node, the arguments from Template. */
		void FillResolvedTypes(FExtractContext& Context, URigVMNode* Node, const FRigVMTemplate* Template, FDcrModelNode& Out, TSet<FName>& OutWildcardPins, const FString& Location)
		{
			if (!Node || !Template)
			{
				return;
			}
			for (URigVMPin* Pin : Node->GetPins())
			{
				if (!Pin || Pin->IsExecuteContext() || Pin->GetDirection() == ERigVMPinDirection::Hidden)
				{
					continue;
				}
				const FRigVMTemplateArgument* Argument = Template->FindArgument(Pin->GetFName());
				if (!Argument || Argument->IsSingleton())
				{
					continue;
				}
				if (Pin->IsWildCard())
				{
					Context.AddUnsupported(Location + TEXT(".") + Pin->GetName(), TEXT("wildcard pin that is not resolved to a type"));
					continue;
				}
				OutWildcardPins.Add(Pin->GetFName());
				Out.ResolvedTypes.Emplace(Pin->GetName(), MakeType(Pin->GetCPPType(), Pin->GetCPPTypeObject()));
			}
		}

		/**
		 * A unit's traits, in pin order, and each one's value where it differs from a new trait's of the same struct.
		 * The trait's `Name` member is left out of the value: the trait's name says it.
		 */
		void ExtractTraits(FExtractContext& Context, URigVMUnitNode* Unit, FDcrModelNode& Out, const FString& Location)
		{
			for (const FString& TraitName : Unit->GetTraitNames())
			{
				UScriptStruct* TraitStruct = Unit->GetTraitScriptStruct(FName(*TraitName));
				TSharedPtr<FStructOnScope> Instance = TraitStruct ? Unit->GetTraitInstance(FName(*TraitName)) : nullptr;
				if (!TraitStruct || !Instance.IsValid())
				{
					Context.AddUnsupported(Location + TEXT(".") + TraitName, TEXT("trait whose struct cannot be found"));
					continue;
				}
				FDcrModelTrait& Trait = Out.Traits.AddDefaulted_GetRef();
				Trait.Name = TraitName;
				Trait.StructPath = TraitStruct->GetPathName();

				FStructOnScope Fresh(TraitStruct);
				if (const FProperty* NameProperty = TraitStruct->FindPropertyByName(TEXT("Name")))
				{
					NameProperty->CopyCompleteValue_InContainer(Instance->GetStructMemory(), Fresh.GetStructMemory());
				}
				const FString Text = ExportDcrStructText(*TraitStruct, Instance->GetStructMemory());
				if (Text != ExportDcrStructText(*TraitStruct, Fresh.GetStructMemory()))
				{
					AddValue(Out.Values, TraitName, Text);
				}
			}
		}

		void ExtractUnit(FExtractContext& Context, URigVMUnitNode* Unit, FDcrModelNode& Out, const FString& Location)
		{
			UScriptStruct* Struct = Unit->GetScriptStruct();
			const FName Notation = Unit->GetNotation();
			if (!Struct && Notation.IsNone())
			{
				Out.Kind = EDcrNodeKind::Unknown;
				Context.AddUnsupported(Location, TEXT("unit node without a struct or a template (a struct the engine does not have)"));
				return;
			}
			// A template node no type was picked for has its notation and no struct.
			Out.Kind = EDcrNodeKind::Unit;
			Out.StructPath = Struct ? Struct->GetPathName() : FString();
			if (!Notation.IsNone())
			{
				Out.Notation = Notation.ToString();
			}
			FDcrNodeDefaults::CollectValues(Unit, TSet<FName>(), Out);
			const FRigVMTemplate* Template = Struct ? nullptr : Unit->GetTemplate();
			const FRigVMFunction* First = Template && Template->NumPermutations() > 0 ? Template->GetPermutation(0) : nullptr;
			if (First && First->Struct)
			{
				// No version picked: a value is kept where it differs from what the versions start with, as the text
				// writes the node (the pins they all have hold the same defaults).
				FStructOnScope Defaults(First->Struct);
				for (int32 Index = Out.Values.Num() - 1; Index >= 0; --Index)
				{
					const FProperty* Property = First->Struct->FindPropertyByName(*Out.Values[Index].Name);
					FDcrValueSlot Value;
					if (!Property || !Value.Init(*Property) || !Value.ImportText(Out.Values[Index].Value))
					{
						continue;
					}
					if (AreDcrValuesSame(*Property, Value.GetMemory(), Property->ContainerPtrToValuePtr<void>(Defaults.GetStructMemory())))
					{
						Out.Values.RemoveAt(Index);
					}
					else
					{
						Out.Values[Index].Value = Value.ExportText();
					}
				}
			}
			ExtractTraits(Context, Unit, Out, Location);
		}

		void ExtractDispatch(FExtractContext& Context, URigVMDispatchNode* Dispatch, FDcrModelNode& Out, const FString& Location)
		{
			Out.Kind = EDcrNodeKind::Dispatch;
			Out.Notation = Dispatch->GetNotation().ToString();
			TSet<FName> Wildcards;
			FillResolvedTypes(Context, Dispatch, Dispatch->GetTemplate(), Out, Wildcards, Location);
			FDcrNodeDefaults::CollectValues(Dispatch, Wildcards, Out);
		}

		/** The pins an aggregate node has past its first two aggregate pins, in order. */
		TArray<FString> GetAggregatePinNames(URigVMAggregateNode* Aggregate)
		{
			TArray<FString> Names;
			const URigVMPin* First = Aggregate->GetFirstAggregatePin();
			const URigVMPin* Second = Aggregate->GetSecondAggregatePin();
			if (!First || !Second)
			{
				return Names;
			}
			const FString FirstName = First->GetName();
			const FString SecondName = Second->GetName();
			const TArray<URigVMPin*> Pins = Aggregate->IsInputAggregate() ? Aggregate->GetAggregateInputs() : Aggregate->GetAggregateOutputs();
			for (const URigVMPin* Pin : Pins)
			{
				if (Pin && Pin->GetName() != FirstName && Pin->GetName() != SecondName)
				{
					Names.Add(Pin->GetName());
				}
			}
			return Names;
		}

		void ExtractAggregate(FExtractContext& Context, URigVMAggregateNode* Aggregate, FDcrModelNode& Out, const FString& Location)
		{
			URigVMNode* Inner = nullptr;
			for (URigVMNode* Contained : Aggregate->GetContainedNodes())
			{
				if (Contained && !Contained->IsA<URigVMFunctionEntryNode>() && !Contained->IsA<URigVMFunctionReturnNode>())
				{
					Inner = Contained;
					break;
				}
			}

			TSet<FName> Wildcards;
			bool bResolved = false;
			if (URigVMUnitNode* InnerUnit = Cast<URigVMUnitNode>(Inner))
			{
				UScriptStruct* Struct = InnerUnit->GetScriptStruct();
				if (Struct)
				{
					bResolved = true;
					Out.Kind = EDcrNodeKind::Unit;
					Out.StructPath = Struct->GetPathName();
					const FName Notation = InnerUnit->GetNotation();
					if (!Notation.IsNone())
					{
						Out.Notation = Notation.ToString();
					}
				}
			}
			else if (URigVMDispatchNode* InnerDispatch = Cast<URigVMDispatchNode>(Inner))
			{
				bResolved = true;
				Out.Kind = EDcrNodeKind::Dispatch;
				Out.Notation = InnerDispatch->GetNotation().ToString();
				FillResolvedTypes(Context, Aggregate, InnerDispatch->GetTemplate(), Out, Wildcards, Location);
			}

			if (!bResolved)
			{
				Out.Kind = EDcrNodeKind::Unknown;
				Context.AddUnsupported(Location, TEXT("aggregate node whose inner node is not a resolved unit or a dispatch"));
				return;
			}
			Out.AggregatePins = GetAggregatePinNames(Aggregate);
			FDcrNodeDefaults::CollectValues(Aggregate, Wildcards, Out);
		}

		void ExtractVariable(URigVMVariableNode* Variable, FDcrModelNode& Out)
		{
			// A function's input arguments are variables to the controller too: an ordinary getter named for the argument.
			Out.Kind = Variable->IsGetter() ? EDcrNodeKind::VariableGet : EDcrNodeKind::VariableSet;
			Out.Variable = Variable->GetVariableName().ToString();
			Out.VariableType = MakeType(Variable->GetCPPType(), Variable->GetCPPTypeObject());
			Out.bLocalVariable = Variable->IsLocalVariable();
			if (!Variable->IsGetter())
			{
				URigVMPin* ValuePin = Variable->GetValuePin();
				if (ValuePin && !ValuePin->IsLinked(false) && ValuePin->CanProvideDefaultValue())
				{
					const FString Value = ValuePin->GetDefaultValue();
					if (!FDcrNodeDefaults::IsTypeDefault(ValuePin->GetCPPType(), ValuePin->GetCPPTypeObject(), Value))
					{
						AddValue(Out.Values, ValuePin->GetName(), Value);
					}
				}
			}
		}

		void ExtractCall(FExtractContext& Context, URigVMFunctionReferenceNode* Reference, FDcrModelNode& Out, const FString& Location)
		{
			const FRigVMGraphFunctionHeader& Header = Reference->GetReferencedFunctionHeader();
			Out.Function = Header.Name.ToString();
			if (Out.Function.IsEmpty())
			{
				Out.Function = Header.LibraryPointer.GetFunctionName();
			}
			if (Reference->ReferencesFunctionInSamePackage())
			{
				Out.Kind = EDcrNodeKind::LocalCall;
				if (Context.Library && !Context.Library->FindFunction(*Out.Function))
				{
					Context.AddUnsupported(Location, FString::Printf(TEXT("call to the function '%s', which the rig's library does not have"), *Out.Function));
				}
			}
			else
			{
				Out.Kind = EDcrNodeKind::ExternalCall;
				// The asset, not its generated class: the Blueprint kind keeps its functions on RigName_C.
				FString Host = Header.LibraryPointer.HostObject.GetAssetPathString();
				if (Host.EndsWith(TEXT("_C")))
				{
					Host.LeftChopInline(2);
				}
				Out.FunctionHost = Host;
				if (Header.LibraryPointer.HostObject.IsNull())
				{
					Context.AddUnsupported(Location, TEXT("call to a function in an unknown asset"));
				}
			}
			FDcrNodeDefaults::CollectValues(Reference, TSet<FName>(), Out);
		}

		/**
		 * On a collapse node's graph the Entry and Return nodes say which pins the collapse node shows: Entry the
		 * ones going in, Return the ones coming out, in pin order, execute pins left out (those are the nodes'
		 * own ExecuteOut and ExecuteIn). The defaults of the inputs go to Entry's Values.
		 */
		void FillCollapseInterface(FExtractContext& Context, URigVMCollapseNode* Collapse, FDcrModelGraph& Contained, const FString& Location)
		{
			for (FDcrModelNode& Inner : Contained.Nodes)
			{
				const bool bEntry = Inner.Kind == EDcrNodeKind::Entry;
				if (!bEntry && Inner.Kind != EDcrNodeKind::Return)
				{
					continue;
				}
				for (URigVMPin* Pin : Collapse->GetPins())
				{
					if (!Pin || Pin->IsExecuteContext())
					{
						continue;
					}
					const ERigVMPinDirection Direction = Pin->GetDirection();
					const bool bIsInput = Direction == ERigVMPinDirection::Input || Direction == ERigVMPinDirection::IO;
					const bool bIsOutput = Direction == ERigVMPinDirection::Output || Direction == ERigVMPinDirection::IO;
					if (bEntry ? !bIsInput : !bIsOutput)
					{
						continue;
					}
					Inner.ResolvedTypes.Emplace(Pin->GetName(), MakeType(Pin->GetCPPType(), Pin->GetCPPTypeObject()));
					if (bEntry && Pin->CanProvideDefaultValue())
					{
						FString Value = Pin->GetDefaultValue();
						if (!FDcrNodeDefaults::IsTypeDefault(Pin->GetCPPType(), Pin->GetCPPTypeObject(), Value))
						{
							CanonicalizeType(Context, MakeType(Pin->GetCPPType(), Pin->GetCPPTypeObject()), Location + TEXT(".") + Pin->GetName(), Value);
							AddValue(Inner.Values, Pin->GetName(), Value);
						}
					}
				}
			}
		}

		void ExtractCollapse(FExtractContext& Context, URigVMCollapseNode* Collapse, FDcrModelNode& Out, const FString& Location)
		{
			Out.Kind = EDcrNodeKind::Collapse;
			if (URigVMGraph* Contained = Collapse->GetContainedGraph())
			{
				Out.Contained = MakeUnique<FDcrModelGraph>();
				Out.Contained->Name = Out.Name;
				for (const FRigVMGraphVariableDescription& Local : Contained->GetLocalVariables(false))
				{
					Out.Contained->Locals.Add(MakeVariable(Context, Local, Location + TEXT("/") + Local.Name.ToString()));
				}
				ExtractGraphInto(Context, Contained, *Out.Contained, Location);
				FillCollapseInterface(Context, Collapse, *Out.Contained, Location);
			}
			else
			{
				Context.AddUnsupported(Location, TEXT("collapse node without a graph"));
			}
			FDcrNodeDefaults::CollectValues(Collapse, TSet<FName>(), Out);
		}

		/**
		 * Spells the values the node keeps the one way. A unit's pins go through the struct's member, the others'
		 * through the type the pin has.
		 */
		void CanonicalizeNodeValues(FExtractContext& Context, URigVMNode* Node, FDcrModelNode& Out, const FString& Location)
		{
			if (Out.Values.IsEmpty())
			{
				return;
			}
			const URigVMUnitNode* Unit = Cast<URigVMUnitNode>(Node);
			const UScriptStruct* Struct = Unit ? Unit->GetScriptStruct() : nullptr;
			for (FDcrValue& Value : Out.Values)
			{
				FDcrValueSlot* Slot = nullptr;
				if (const FProperty* Member = Struct ? Struct->FindPropertyByName(*Value.Name) : nullptr)
				{
					Slot = GetPropertySlot(Context, *Member);
				}
				if (!Slot)
				{
					if (URigVMPin* Pin = Node->FindRootPinByName(*Value.Name))
					{
						Slot = GetTypeSlot(Context, MakeType(Pin->GetCPPType(), Pin->GetCPPTypeObject()));
					}
				}
				Canonicalize(Context, Slot, Location + TEXT(".") + Value.Name, Value.Value);
			}
		}

		FDcrModelNode ExtractNode(FExtractContext& Context, URigVMNode* Node, const FString& GraphLocation)
		{
			FDcrModelNode Out;
			Out.Name = Node->GetName();
			Out.Position = Node->GetPosition();
			const FString Location = GraphLocation + TEXT("/") + Out.Name;
			FillDcrExecutePins(Node, Out);

			if (const URigVMInjectionInfo* Injection = Node->GetInjectionInfo())
			{
				if (const URigVMPin* HostPin = Injection->GetPin())
				{
					Out.InjectedOn = HostPin->GetPinPath();
				}
				Out.bInjectedAsInput = Injection->bInjectedAsInput;
				if (Injection->InputPin)
				{
					Out.InjectedInputPin = Injection->InputPin->GetName();
				}
				if (Injection->OutputPin)
				{
					Out.InjectedOutputPin = Injection->OutputPin->GetName();
				}
			}

			if (Node->HasOrphanedPins())
			{
				Context.AddUnsupported(Location, TEXT("node with orphaned pins"));
			}

			if (!Node->GetTraitNames().IsEmpty() && !Node->IsA<URigVMUnitNode>())
			{
				Out.Kind = EDcrNodeKind::Unknown;
				Context.AddUnsupported(Location, FString::Printf(TEXT("node with trait pins (%s)"), *Node->GetClass()->GetName()));
				return Out;
			}

			if (URigVMCommentNode* Comment = Cast<URigVMCommentNode>(Node))
			{
				Out.Kind = EDcrNodeKind::Comment;
				Out.CommentText = Comment->GetCommentText();
				Out.CommentSize = Comment->GetSize();
			}
			else if (Node->IsA<URigVMRerouteNode>())
			{
				Out.Kind = EDcrNodeKind::Reroute;
				FDcrNodeDefaults::CollectValues(Node, TSet<FName>(), Out);
			}
			else if (URigVMVariableNode* Variable = Cast<URigVMVariableNode>(Node))
			{
				ExtractVariable(Variable, Out);
			}
			else if (Node->IsA<URigVMFunctionEntryNode>())
			{
				Out.Kind = EDcrNodeKind::Entry;
			}
			else if (Node->IsA<URigVMFunctionReturnNode>())
			{
				Out.Kind = EDcrNodeKind::Return;
				FDcrNodeDefaults::CollectValues(Node, TSet<FName>(), Out);
			}
			else if (URigVMAggregateNode* Aggregate = Cast<URigVMAggregateNode>(Node))
			{
				ExtractAggregate(Context, Aggregate, Out, Location);
			}
			else if (URigVMCollapseNode* Collapse = Cast<URigVMCollapseNode>(Node))
			{
				ExtractCollapse(Context, Collapse, Out, Location);
			}
			else if (URigVMFunctionReferenceNode* Reference = Cast<URigVMFunctionReferenceNode>(Node))
			{
				ExtractCall(Context, Reference, Out, Location);
			}
			else if (URigVMUnitNode* Unit = Cast<URigVMUnitNode>(Node))
			{
				ExtractUnit(Context, Unit, Out, Location);
			}
			else if (URigVMDispatchNode* Dispatch = Cast<URigVMDispatchNode>(Node))
			{
				ExtractDispatch(Context, Dispatch, Out, Location);
			}
			else if (const URigVMInvokeEntryNode* Invoke = Cast<URigVMInvokeEntryNode>(Node))
			{
				Out.Kind = EDcrNodeKind::InvokeEntry;
				Out.Function = Invoke->GetEntryName().ToString();
			}
			else
			{
				Out.Kind = EDcrNodeKind::Unknown;
				Context.AddUnsupported(Location, FString::Printf(TEXT("%s is not supported"), *Node->GetClass()->GetName()));
			}
			CanonicalizeNodeValues(Context, Node, Out, Location);
			return Out;
		}

		void ExtractGraphInto(FExtractContext& Context, URigVMGraph* Graph, FDcrModelGraph& Out, const FString& Location)
		{
			for (URigVMNode* Node : Graph->GetNodes())
			{
				if (Node)
				{
					Out.Nodes.Add(ExtractNode(Context, Node, Location));
				}
			}
			for (URigVMLink* Link : Graph->GetLinks())
			{
				if (!Link)
				{
					continue;
				}
				FDcrModelLink Entry;
				Entry.Source = Link->GetSourcePinPath();
				Entry.Target = Link->GetTargetPinPath();
				if (Entry.Source.IsEmpty() || Entry.Target.IsEmpty())
				{
					Context.AddUnsupported(Location, TEXT("link with a pin that cannot be resolved"));
					continue;
				}
				Out.Links.Add(MoveTemp(Entry));
			}
		}

		// ---- Functions and variables ---------------------------------------------------------------------------

		FDcrModelVariable MakeVariable(FExtractContext& Context, const FRigVMGraphVariableDescription& Description, const FString& Location)
		{
			FDcrModelVariable Variable;
			Variable.Name = Description.Name.ToString();
			const UObject* TypeObject = Description.CPPTypeObject.Get();
			Variable.Type = MakeType(Description.CPPType, TypeObject, Description.CPPTypeObjectPath);
			if (!FDcrNodeDefaults::IsTypeDefault(Description.CPPType, TypeObject, Description.DefaultValue))
			{
				Variable.Default = Description.DefaultValue;
				CanonicalizeType(Context, Variable.Type, Location, Variable.Default);
			}
			Variable.bPublic = Description.bPublic;
			const FString Category = Description.Category.ToString();
			if (Category != TEXT("Default"))
			{
				Variable.Category = Category;
			}
			Variable.Tooltip = Description.Tooltip.ToString();
			return Variable;
		}

		void ExtractFunctionPins(FExtractContext& Context, URigVMLibraryNode* Function, FDcrModelFunction& Out, const FString& Location)
		{
			Out.bMutable = false;
			bool bStandardExecuteSeen = false;
			for (URigVMPin* Pin : Function->GetPins())
			{
				if (!Pin)
				{
					continue;
				}
				const ERigVMPinDirection Direction = Pin->GetDirection();
				if (Direction != ERigVMPinDirection::Input && Direction != ERigVMPinDirection::Output && Direction != ERigVMPinDirection::IO)
				{
					continue;
				}
				if (Pin->IsExecuteContext())
				{
					Out.bMutable = true;
					// The one IO execute pin is what makes a function mutable; anything beyond it is listed and reported.
					if (Direction == ERigVMPinDirection::IO && !bStandardExecuteSeen)
					{
						bStandardExecuteSeen = true;
						continue;
					}
					Context.AddUnsupported(Location + TEXT(".") + Pin->GetName(), TEXT("function with more execute pins than the one that makes it mutable"));
				}

				FDcrModelFunctionPin FunctionPin;
				FunctionPin.Name = Pin->GetName();
				FunctionPin.Type = MakeType(Pin->GetCPPType(), Pin->GetCPPTypeObject());
				if (Direction != ERigVMPinDirection::Output && !Pin->IsExecuteContext())
				{
					FString Value = Pin->GetDefaultValue();
					if (!FDcrNodeDefaults::IsTypeDefault(Pin->GetCPPType(), Pin->GetCPPTypeObject(), Value))
					{
						CanonicalizeType(Context, FunctionPin.Type, Location + TEXT(".") + Pin->GetName(), Value);
						FunctionPin.Default = Value;
					}
				}
				if (Direction == ERigVMPinDirection::IO)
				{
					Context.AddUnsupported(Location + TEXT(".") + Pin->GetName(), TEXT("function pin that is both an input and an output"));
				}
				if (Direction != ERigVMPinDirection::Output)
				{
					Out.Inputs.Add(FunctionPin);
				}
				if (Direction != ERigVMPinDirection::Input)
				{
					Out.Outputs.Add(FunctionPin);
				}
			}
		}

		void ExtractFunction(FExtractContext& Context, URigVMLibraryNode* Function)
		{
			FDcrModelFunction& Out = Context.Out.Functions.AddDefaulted_GetRef();
			Out.Name = Function->GetName();
			Out.Graph.Name = Out.Name;
			const FString Location = TEXT("FunctionLibrary/") + Out.Name;
			Out.bPublic = Context.Library->IsFunctionPublic(Function->GetFName());
			Out.Category = Function->GetNodeCategory();
			Out.Keywords = Function->GetNodeKeywords();
			Out.Description = Function->GetNodeDescription();
			ExtractFunctionPins(Context, Function, Out, Location);

			URigVMGraph* Contained = Function->GetContainedGraph();
			if (!Contained)
			{
				Context.AddUnsupported(Location, TEXT("function without a graph"));
				return;
			}
			for (const FRigVMGraphVariableDescription& Local : Contained->GetLocalVariables(false))
			{
				Out.Locals.Add(MakeVariable(Context, Local, Location + TEXT("/") + Local.Name.ToString()));
			}
			ExtractGraphInto(Context, Contained, Out.Graph, Location);
		}

		void ExtractVariables(FExtractContext& Context)
		{
			for (const FRigVMGraphVariableDescription& Description : Context.Rig.RigVM->GetAssetVariables())
			{
				FDcrModelVariable Variable = MakeVariable(Context, Description, TEXT("Variables/") + Description.Name.ToString());
				// The description does not say; the property the asset generates for the variable does.
				if (const FProperty* Property = Context.Rig.RigVM->FindGeneratedPropertyByName(Description.Name))
				{
					Variable.bReadOnly = Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly);
				}
				Context.Out.Variables.Add(MoveTemp(Variable));
			}
		}

		// ---- Hierarchy -----------------------------------------------------------------------------------------

		EDcrElementKind KindOf(ERigElementType Type)
		{
			switch (Type)
			{
			case ERigElementType::Bone: return EDcrElementKind::Bone;
			case ERigElementType::Null: return EDcrElementKind::Null;
			case ERigElementType::Control: return EDcrElementKind::Control;
			case ERigElementType::Curve: return EDcrElementKind::Curve;
			case ERigElementType::Connector: return EDcrElementKind::Connector;
			case ERigElementType::Socket: return EDcrElementKind::Socket;
			default: break;
			}
			return EDcrElementKind::Unknown;
		}

		FString ElementLocation(const FRigElementKey& Key)
		{
			const EDcrElementKind Kind = KindOf(Key.Type);
			const FString TypeName = Kind != EDcrElementKind::Unknown ? FString(LexToString(Kind)) : StaticEnum<ERigElementType>()->GetNameStringByValue(static_cast<int64>(Key.Type));
			return FString::Printf(TEXT("Hierarchy/%s(%s)"), *TypeName, *Key.Name.ToString());
		}

		/** A control's value as the struct's export text: what ImportText on FRigControlValue reads back. */
		FString ControlValueText(const FRigControlValue& Value)
		{
			return ExportDcrStructText(*FRigControlValue::StaticStruct(), &Value);
		}

		void ExtractControlProperties(FValueContext& Context, const FString& Location, URigHierarchy* Hierarchy, FRigControlElement* Control, FDcrModelElement& Out)
		{
			// A control the text adds gets the limit array of its type (URigHierarchyController::AddControl): no limits.
			FRigControlSettings Defaults;
			FRigControlSettings Typed;
			Typed.ControlType = Control->Settings.ControlType;
			Typed.SetupLimitArrayForType();
			Defaults.LimitEnabled = Typed.LimitEnabled;
			AddStructDifferences(Context, Location, FRigControlSettings::StaticStruct(), &Control->Settings, &Defaults, TSet<FName>(), Out.Properties);

			const FTransform Offset = Hierarchy->GetControlOffsetTransform(Control, ERigTransformType::InitialLocal);
			if (!IsIdentity(Offset))
			{
				AddStructValue(Context, Location, Out.Properties, TEXT("Offset"), *TBaseStructure<FTransform>::Get(), ExportBaseStruct(Offset));
			}

			// A control that starts at its identity value says nothing. Compared as values of the control's type: the
			// store keeps a validity flag and unused floats the text never sees.
			const FRigControlValue InitialValue = Hierarchy->GetControlValue(Control, ERigControlValueType::Initial);
			const FString Initial = ControlValueText(InitialValue);
			if (!IsDcrControlValueIdentity(Control->Settings.ControlType, InitialValue))
			{
				AddStructValue(Context, Location, Out.Properties, TEXT("Initial"), *FRigControlValue::StaticStruct(), Initial);
			}

			const FTransform Shape = Hierarchy->GetControlShapeTransform(Control, ERigTransformType::InitialLocal);
			if (!IsIdentity(Shape))
			{
				AddStructValue(Context, Location, Out.Properties, TEXT("ShapeTransform"), *TBaseStructure<FTransform>::Get(), ExportBaseStruct(Shape));
			}
		}

		void ExtractElementProperties(FValueContext& Context, URigHierarchy* Hierarchy, const FRigElementKey& Key, FDcrModelElement& Out)
		{
			const FString Location = ElementLocation(Key);
			switch (Out.Kind)
			{
			case EDcrElementKind::Bone:
			{
				const FTransform Transform = Hierarchy->GetInitialLocalTransform(Key);
				if (!IsIdentity(Transform))
				{
					AddStructValue(Context, Location, Out.Properties, TEXT("Transform"), *TBaseStructure<FTransform>::Get(), ExportBaseStruct(Transform));
				}
				if (const FRigBoneElement* Bone = Hierarchy->Find<FRigBoneElement>(Key))
				{
					AddValue(Out.Properties, TEXT("BoneType"), StaticEnum<ERigBoneType>()->GetNameStringByValue(static_cast<int64>(Bone->BoneType)));
				}
				break;
			}
			case EDcrElementKind::Null:
			{
				const FTransform Transform = Hierarchy->GetInitialLocalTransform(Key);
				if (!IsIdentity(Transform))
				{
					AddStructValue(Context, Location, Out.Properties, TEXT("Transform"), *TBaseStructure<FTransform>::Get(), ExportBaseStruct(Transform));
				}
				break;
			}
			case EDcrElementKind::Control:
			{
				if (FRigControlElement* Control = Hierarchy->Find<FRigControlElement>(Key))
				{
					ExtractControlProperties(Context, Location, Hierarchy, Control, Out);
				}
				break;
			}
			case EDcrElementKind::Curve:
			{
				const float Value = Hierarchy->GetCurveValue(Key);
				if (Value != 0.f)
				{
					AddValue(Out.Properties, TEXT("Value"), FloatText(Value));
				}
				break;
			}
			case EDcrElementKind::Connector:
			{
				if (const FRigConnectorElement* Connector = Hierarchy->Find<FRigConnectorElement>(Key))
				{
					const FRigConnectorSettings Defaults;
					AddStructDifferences(Context, Location, FRigConnectorSettings::StaticStruct(), &Connector->Settings, &Defaults, TSet<FName>(), Out.Properties);
				}
				break;
			}
			case EDcrElementKind::Socket:
			{
				const FTransform Transform = Hierarchy->GetInitialLocalTransform(Key);
				if (!IsIdentity(Transform))
				{
					AddStructValue(Context, Location, Out.Properties, TEXT("Transform"), *TBaseStructure<FTransform>::Get(), ExportBaseStruct(Transform));
				}
				if (const FRigSocketElement* Socket = Hierarchy->Find<FRigSocketElement>(Key))
				{
					const FLinearColor Color = Socket->GetColor(Hierarchy);
					if (Color != FRigSocketElement::SocketDefaultColor)
					{
						AddStructValue(Context, Location, Out.Properties, TEXT("Color"), *TBaseStructure<FLinearColor>::Get(), ExportBaseStruct(Color));
					}
					const FString Description = Socket->GetDescription(Hierarchy);
					if (!Description.IsEmpty())
					{
						AddValue(Out.Properties, TEXT("Description"), Description);
					}
				}
				break;
			}
			default:
				Context.AddUnsupported(Location, TEXT("element kind the model does not hold"));
				break;
			}
		}

		void ExtractHierarchyInto(FValueContext& Context, URigHierarchy* Hierarchy, TArray<FDcrModelElement>& OutElements)
		{
			if (!Hierarchy)
			{
				Context.AddUnsupported(TEXT("Hierarchy"), TEXT("the asset has no hierarchy"));
				return;
			}

			struct FEntry
			{
				FRigElementKey Key;
				int32 Parent = INDEX_NONE;
			};
			TArray<FEntry> Entries;
			TMap<FRigElementKey, int32> EntryByKey;
			for (int32 Index = 0; Index < Hierarchy->Num(); ++Index)
			{
				const FRigElementKey Key = Hierarchy->GetKey(Index);
				if (!Key.IsValid())
				{
					continue;
				}
				// Spawned by the construction graph: the graph is the source of those.
				if (Hierarchy->IsProcedural(Key))
				{
					continue;
				}
				if (KindOf(Key.Type) == EDcrElementKind::Unknown)
				{
					Context.AddUnsupported(ElementLocation(Key), TEXT("element kind the model does not hold"));
					continue;
				}
				FEntry& Entry = Entries.AddDefaulted_GetRef();
				Entry.Key = Key;
				EntryByKey.Add(Key, Entries.Num() - 1);
			}

			for (FEntry& Entry : Entries)
			{
				const int32 NumParents = Hierarchy->GetNumberOfParents(Entry.Key);
				if (NumParents <= 0)
				{
					continue;
				}
				const FRigElementKey ParentKey = Hierarchy->GetFirstParent(Entry.Key);
				if (const int32* ParentIndex = EntryByKey.Find(ParentKey))
				{
					Entry.Parent = *ParentIndex;
				}
				else
				{
					Context.AddUnsupported(ElementLocation(Entry.Key), FString::Printf(TEXT("parent %s is not an element the model holds"), *ParentKey.Name.ToString()));
				}
				if (NumParents > 1)
				{
					Context.AddUnsupported(ElementLocation(Entry.Key), FString::Printf(TEXT("%d parents; only the first is kept"), NumParents));
				}
			}

			// Parents before their children, siblings in the order the hierarchy holds them.
			TArray<TArray<int32>> Children;
			Children.SetNum(Entries.Num());
			TArray<int32> Roots;
			for (int32 Index = 0; Index < Entries.Num(); ++Index)
			{
				if (Entries[Index].Parent == INDEX_NONE)
				{
					Roots.Add(Index);
				}
				else
				{
					Children[Entries[Index].Parent].Add(Index);
				}
			}
			TArray<int32> Order;
			TArray<bool> Visited;
			Visited.Init(false, Entries.Num());
			TArray<int32> Stack;
			for (int32 Index = Roots.Num() - 1; Index >= 0; --Index)
			{
				Stack.Add(Roots[Index]);
			}
			while (!Stack.IsEmpty())
			{
				const int32 Current = Stack.Pop();
				if (Visited[Current])
				{
					continue;
				}
				Visited[Current] = true;
				Order.Add(Current);
				for (int32 Child = Children[Current].Num() - 1; Child >= 0; --Child)
				{
					Stack.Add(Children[Current][Child]);
				}
			}
			// Only a cycle leaves an element behind.
			for (int32 Index = 0; Index < Entries.Num(); ++Index)
			{
				if (!Visited[Index])
				{
					Context.AddUnsupported(ElementLocation(Entries[Index].Key), TEXT("element in a parent cycle"));
					Order.Add(Index);
				}
			}

			for (const int32 Index : Order)
			{
				const FEntry& Entry = Entries[Index];
				FDcrModelElement& Element = OutElements.AddDefaulted_GetRef();
				Element.Kind = KindOf(Entry.Key.Type);
				Element.Name = Entry.Key.Name.ToString();
				if (Entry.Parent != INDEX_NONE)
				{
					Element.Parent = Entries[Entry.Parent].Key.Name.ToString();
					Element.ParentKind = KindOf(Entries[Entry.Parent].Key.Type);
				}
				ExtractElementProperties(Context, Hierarchy, Entry.Key, Element);
			}
		}

		// ---- Asset ---------------------------------------------------------------------------------------------

		void ExtractSettings(FExtractContext& Context)
		{
			const FDcrRig& Rig = Context.Rig;

			FString Preview;
			const bool bHasPreviewProperty = ReadSoftObjectPath(Rig.Editor, TEXT("PreviewSkeletalMesh"), Preview)
				|| ReadSoftObjectPath(Rig.Asset, TEXT("PreviewSkeletalMesh"), Preview);
			if (!bHasPreviewProperty)
			{
				if (const USkeletalMesh* Mesh = Rig.ControlRig->GetPreviewMesh())
				{
					Preview = Mesh->GetPathName();
				}
			}
			if (!Preview.IsEmpty())
			{
				AddValue(Context.Out.Settings, TEXT("PreviewSkeletalMesh"), Preview);
			}

			FString Libraries;
			const bool bHasLibrariesProperty = ExportObjectProperty(Context, Rig.Editor, TEXT("ShapeLibraries"), TEXT("Settings.ShapeLibraries"), Libraries)
				|| ExportObjectProperty(Context, Rig.Asset, TEXT("ShapeLibraries"), TEXT("Settings.ShapeLibraries"), Libraries);
			if (!bHasLibrariesProperty)
			{
				TArray<FString> Paths;
				for (const TSoftObjectPtr<UControlRigShapeLibrary>& Library : Rig.ControlRig->GetShapeLibraries())
				{
					if (!Library.IsNull())
					{
						// Quoted, as the engine exports the elements of an array of soft object pointers.
						Paths.Add(FString::Printf(TEXT("\"%s\""), *Library.ToSoftObjectPath().ToString()));
					}
				}
				if (!Paths.IsEmpty())
				{
					Libraries = TEXT("(") + FString::Join(Paths, TEXT(",")) + TEXT(")");
				}
			}
			// A new rig starts with the default shape library: that is no setting. None at all is one.
			const TArray<TSoftObjectPtr<UControlRigShapeLibrary>>& Current = Rig.ControlRig->GetShapeLibraries();
			const bool bDefault = Current.Num() == 1 && Current[0].ToSoftObjectPath() == UControlRigSettings::Get()->DefaultShapeLibrary.ToSoftObjectPath();
			if (!bDefault)
			{
				AddValue(Context.Out.Settings, TEXT("ShapeLibraries"), Libraries.IsEmpty() ? FString(TEXT("()")) : Libraries);
			}
		}

		void ExtractModule(FExtractContext& Context)
		{
			Context.Out.bModule = Context.Rig.ControlRig->IsControlRigModule();
			if (!Context.Out.bModule)
			{
				return;
			}
			const FRigModuleSettings& Settings = Context.Rig.ControlRig->GetRigModuleSettings();
			const FRigModuleSettings Defaults;
			// The exposed connectors are the hierarchy's connector elements again; the engine keeps them in step.
			TSet<FName> Skip;
			Skip.Add(TEXT("ExposedConnectors"));
			AddStructDifferences(Context, TEXT("ModuleSettings"), FRigModuleSettings::StaticStruct(), &Settings, &Defaults, Skip, Context.Out.ModuleSettings);
		}

		void ExtractGraphs(FExtractContext& Context)
		{
			TArray<URigVMGraph*> Graphs;
			for (URigVMGraph* Graph : Context.Rig.RigVM->GetAllModels())
			{
				if (Graph && Graph->IsTopLevelGraph() && !Cast<URigVMFunctionLibrary>(Graph) && !Graphs.Contains(Graph))
				{
					Graphs.Add(Graph);
				}
			}
			// The default graph comes first.
			if (URigVMGraph* Default = Context.Rig.RigVM->GetDefaultModel())
			{
				if (Graphs.Remove(Default) > 0)
				{
					Graphs.Insert(Default, 0);
				}
			}
			for (URigVMGraph* Graph : Graphs)
			{
				FDcrModelGraph& Out = Context.Out.Graphs.AddDefaulted_GetRef();
				Out.Name = Graph->GetGraphName();
				if (Out.Name.IsEmpty())
				{
					Out.Name = Graph->GetName();
				}
				ExtractGraphInto(Context, Graph, Out, Out.Name);
			}
		}
	}

	void ExtractDcrHierarchy(URigHierarchy& Hierarchy, TArray<FDcrModelElement>& OutElements, TArray<FDcrModelUnsupported>& OutUnsupported)
	{
		OutElements.Reset();
		FValueContext Context(OutUnsupported);
		ExtractHierarchyInto(Context, &Hierarchy, OutElements);
	}

	FDcrExtractResult ExtractDcrModel(const FDcrRig& Rig, FDcrModel& OutModel)
	{
		FDcrExtractResult Result;
		OutModel = FDcrModel();
		if (!Rig.IsValid())
		{
			Result.Error = TEXT("the asset is not a Control Rig");
			return Result;
		}
		if (!Rig.Editor)
		{
			Result.Error = TEXT("the Control Rig has no editor data");
			return Result;
		}

		OutModel.AssetPath = Rig.Asset->GetPathName();
		OutModel.AssetKind = Rig.GetKind() == EDcrAssetKind::Legacy ? EDcrModelAssetKind::Legacy : EDcrModelAssetKind::Independent;

		FExtractContext Context(Rig, OutModel);
		Context.Library = Rig.RigVM->GetLocalFunctionLibrary();

		if (Rig.ControlRig->IsModularRig())
		{
			Context.AddUnsupported(TEXT("ModularRig"), TEXT("the modular rig model of a modular rig asset is not extracted"));
		}

		ExtractSettings(Context);
		ExtractModule(Context);
		ExtractHierarchyInto(Context, Rig.ControlRig->GetHierarchy(), OutModel.Hierarchy);
		ExtractVariables(Context);
		if (Context.Library)
		{
			for (URigVMLibraryNode* Function : Context.Library->GetFunctions())
			{
				if (Function)
				{
					ExtractFunction(Context, Function);
				}
			}
		}
		ExtractGraphs(Context);

		Result.bSuccess = true;
		return Result;
	}
}
