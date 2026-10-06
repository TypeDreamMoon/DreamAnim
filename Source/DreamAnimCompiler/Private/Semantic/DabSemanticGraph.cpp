// Copyright DreamMoon. All Rights Reserved.
//
// Pose graphs and state machines: Docs/Mapping.md, "Poses" and "State machines". A pose expression
// becomes nodes of the graph it is written in; the conveniences of the text (keyed blend maps, curves,
// bones(...), ExplicitFrame, a single value for a per-pose array) are applied here, in the inverse of
// Lower/DabLower.cpp.

#include "Semantic/DabSemanticInternal.h"

#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_CustomProperty.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "Animation/AnimClassInterface.h"
#include "Animation/AnimNode_Root.h"
#include "Animation/BlendProfile.h"
#include "AnimationGraph.h"
#include "Components/ActorComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "Lower/DabLower.h"
#include "UObject/EnumProperty.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Compiler::SemanticPrivate
{
	/**
	 * Whether BlendListByEnum records its enum-to-pose table. The engine bakes the table into the
	 * compiler's working copy of the node (BakeDataDuringCompilation runs on the copy), so the node a
	 * saved Blueprint holds -- what the extractor reads -- normally has none.
	 */
	constexpr bool GSemRecordEnumToPoseIndex = false;

	// ------------------------------------------------------------------------------------------
	// What the extractor records
	// ------------------------------------------------------------------------------------------

	bool IsStructNamedOrChildOf(const UStruct* Struct, FName Name)
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

	bool IsPoseLinkProperty(const FProperty& Property)
	{
		const FProperty* Element = &Property;
		if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
		{
			Element = Array->Inner;
		}
		const FStructProperty* Struct = CastField<FStructProperty>(Element);
		return Struct && IsStructNamedOrChildOf(Struct->Struct, FName(TEXT("PoseLinkBase")));
	}

	struct FSemSkippedNodeField
	{
		const TCHAR* Struct;
		const TCHAR* Field;
	};

	/** Runtime-node fields that are compiler bookkeeping or that the model holds elsewhere; the extractor's list. */
	const FSemSkippedNodeField GSemSkippedNodeFields[] =
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
			if (IsStructNamedOrChildOf(Struct->Struct, FName(TEXT("AnimNodeFunctionRef"))))
			{
				return false;
			}
		}
		const UStruct* Owner = Field.GetOwnerStruct();
		for (const FSemSkippedNodeField& Skipped : GSemSkippedNodeFields)
		{
			if (Owner && Owner->GetFName() == FName(Skipped.Struct) && Field.GetFName() == FName(Skipped.Field))
			{
				return false;
			}
		}
		return true;
	}

	/** Editor-node properties that hold the editor's view of the node or what the model keeps elsewhere; the extractor's list. */
	const TCHAR* const GSemSkippedEditorProperties[] =
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
		for (const TCHAR* Skipped : GSemSkippedEditorProperties)
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
			if (IsStructNamedOrChildOf(Struct->Struct, FName(TEXT("OptionalPinFromProperty")))
				|| IsStructNamedOrChildOf(Struct->Struct, FName(TEXT("MemberReference")))
				|| IsStructNamedOrChildOf(Struct->Struct, FName(TEXT("Guid"))))
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

	/** An editor-node property a node argument or a machine's metadata may name: what the extractor reads off an editor node. */
	bool IsEditorNodeArgument(const FProperty& Property, const FStructProperty* NodeProperty)
	{
		if (&Property == NodeProperty)
		{
			return false;
		}
		const UClass* Owner = Property.GetOwnerClass();
		if (!Owner || !Owner->IsChildOf(UAnimGraphNode_Base::StaticClass()))
		{
			return false;
		}
		const FName Name = Property.GetFName();
		if (Owner == UAnimGraphNode_Base::StaticClass() && Name != FName(TEXT("Tag")) && Name != FName(TEXT("AlwaysDynamicProperties")))
		{
			return false;
		}
		return IsRecordedEditorProperty(Property);
	}

	const FDabExpr* FindSemanticModelProperty(const FDabModelNode& Node, const TCHAR* Name)
	{
		for (const FDabModelProperty& Property : Node.Properties)
		{
			if (Property.Name == Name)
			{
				return Property.Value.Get();
			}
		}
		return nullptr;
	}

	void AddSemanticBinding(FDabModelNode& Node, const FString& PinName, FDabExprPtr Value)
	{
		if (!Value)
		{
			return;
		}
		Node.Bindings.RemoveAll([&PinName](const FDabModelBinding& Existing) { return Existing.PinName == PinName; });
		FDabModelBinding Binding;
		Binding.PinName = PinName;
		Binding.Value = MoveTemp(Value);
		Node.Bindings.Add(MoveTemp(Binding));
	}

	/** `bones(a, "b c", Depth = n)` as the FInputBlendPose tuple it stands for: `(BranchFilters = [(BoneName = "a", BlendDepth = n), ...])`. */
	FDabExprPtr MakeSemanticBonesTuple(const FDabCallExpr& Call, FDabDiagnosticSink& Sink)
	{
		const FDabExpr* Depth = nullptr;
		TArray<FString> Bones;
		for (const FDabArgumentPtr& Arg : Call.Args)
		{
			if (!Arg || !Arg->Value)
			{
				continue;
			}
			if (Arg->IsNamed())
			{
				if (Arg->Name == TEXT("Depth"))
				{
					Depth = Arg->Value.Get();
				}
				else
				{
					Sink.Error(TEXT("DAB4009"), Arg->NameSpan, FString::Printf(TEXT("bones(...) takes bone names and Depth = n, not `%s`."), *Arg->Name));
				}
				continue;
			}
			FString Bone;
			if (!ReadNameOrStringText(*Arg->Value, Bone))
			{
				Sink.Error(TEXT("DAB4009"), Arg->Value->Span, FString::Printf(TEXT("`%s` is not a bone name."), *PrintValueText(*Arg->Value)),
					TEXT("Write a bone as a name (thigh_l) or a string (\"VB foot_l\")."));
				continue;
			}
			Bones.Add(Bone);
		}

		TArray<FDabExprPtr> Filters;
		for (const FString& Bone : Bones)
		{
			TUniquePtr<FDabTupleExpr> Filter = MakeUnique<FDabTupleExpr>();
			Filter->Items.Add(MakeArgumentNode(TEXT("BoneName"), false, MakeStringExpr(Bone)));
			if (Depth)
			{
				Filter->Items.Add(MakeArgumentNode(TEXT("BlendDepth"), false, CloneExpr(*Depth)));
			}
			Filters.Add(FDabExprPtr(MoveTemp(Filter)));
		}
		TUniquePtr<FDabTupleExpr> Setup = MakeUnique<FDabTupleExpr>();
		Setup->Items.Add(MakeArgumentNode(TEXT("BranchFilters"), false, MakeListExpr(MoveTemp(Filters))));
		Setup->Span = Call.Span;
		return FDabExprPtr(MoveTemp(Setup));
	}

	// ------------------------------------------------------------------------------------------
	// Node classes
	// ------------------------------------------------------------------------------------------

	UClass* FSemantic::FindNodeClassByName(const TCHAR* ClassName) const
	{
		for (UClass* Class : NodeClasses)
		{
			if (Class->GetName() == ClassName)
			{
				return Class;
			}
		}
		return nullptr;
	}

	UClass* FSemantic::ResolveNodeClass(const FString& TypeName, const FDabSpan& Span)
	{
		if (UClass* const* Known = NodeClassesByType.Find(TypeName))
		{
			return *Known;
		}

		// The full class name first (`AlsAnimGraphNode_GameplayTagsBlend`), then the short form.
		UClass* Found = nullptr;
		for (UClass* Class : NodeClasses)
		{
			if (Class->GetName().Equals(TypeName, ESearchCase::CaseSensitive))
			{
				Found = Class;
				break;
			}
		}
		if (!Found)
		{
			const FString Suffix = TEXT("AnimGraphNode_") + TypeName;
			TArray<UClass*> Matches;
			for (UClass* Class : NodeClasses)
			{
				if (Class->GetName().EndsWith(Suffix, ESearchCase::CaseSensitive))
				{
					Matches.Add(Class);
				}
			}
			if (Matches.Num() > 1)
			{
				TArray<FString> Names;
				for (const UClass* Match : Matches)
				{
					Names.Add(Match->GetName());
				}
				Sink.Error(TEXT("DAB3002"), Span, FString::Printf(TEXT("`%s` could mean any of %s."), *TypeName, *FString::Join(Names, TEXT(", "))),
					FString::Printf(TEXT("Write the full class name, such as %s(...)."), *Names[0]));
				NodeClassesByType.Add(TypeName, nullptr);
				return nullptr;
			}
			Found = Matches.IsEmpty() ? nullptr : Matches[0];
		}
		if (!Found || Found->HasAnyClassFlags(CLASS_Abstract))
		{
			Sink.Error(TEXT("DAB3001"), Span, FString::Printf(TEXT("There is no anim node type `%s`."), *TypeName),
				TEXT("A node type is an editor node class without its `AnimGraphNode_` prefix (TwoWayBlend, SequencePlayer); the module that defines it must be loaded."));
			Found = nullptr;
		}
		NodeClassesByType.Add(TypeName, Found);
		return Found;
	}

	const FNodeShape* FSemantic::GetShape(UClass* Class)
	{
		if (!Class)
		{
			return nullptr;
		}
		if (const TUniquePtr<FNodeShape>* Known = Shapes.Find(Class))
		{
			return Known->Get();
		}

		TUniquePtr<FNodeShape> Shape = MakeUnique<FNodeShape>();
		Shape->Class = Class;
		const UAnimGraphNode_Base* NodeDefaults = Cast<UAnimGraphNode_Base>(Class->GetDefaultObject());
		Shape->Defaults = NodeDefaults;
		Shape->NodeProperty = NodeDefaults ? NodeDefaults->GetFNodeProperty() : nullptr;
		if (Shape->NodeProperty)
		{
			Shape->Struct = Shape->NodeProperty->Struct;
			Shape->DefaultStruct = Shape->NodeProperty->ContainerPtrToValuePtr<void>(NodeDefaults);
			// A linked graph's pose inputs are named after the target's input poses, not its InputPoses array.
			Shape->bLinkedGraph = IsStructNamedOrChildOf(Shape->Struct, FName(TEXT("AnimNode_LinkedAnimGraph")));
			Shape->bLinkedLayer = IsStructNamedOrChildOf(Shape->Struct, FName(TEXT("AnimNode_LinkedAnimLayer")));
			for (TFieldIterator<FProperty> It(Shape->Struct); It; ++It)
			{
				if (!IsPoseLinkProperty(**It))
				{
					continue;
				}
				if (CastField<FArrayProperty>(*It))
				{
					if (!Shape->bLinkedGraph)
					{
						Shape->PoseArrays.Add(It->GetName());
					}
				}
				else
				{
					Shape->PosePins.Add(It->GetName());
				}
			}
		}
		Shape->bCustomProperty = Class->IsChildOf(UAnimGraphNode_CustomProperty::StaticClass());

		const FNodeShape* Result = Shape.Get();
		Shapes.Add(Class, MoveTemp(Shape));
		return Result;
	}

	const FProperty* FSemantic::FindNodeProperty(const FNodeShape& Shape, const FString& Name, const void*& OutDefault, bool& bOutEditor) const
	{
		OutDefault = nullptr;
		bOutEditor = false;
		const FName PropertyName(*Name);
		// Tag is always the editor node's: the compiler copies it into a runtime field of that name.
		if (Shape.Struct && Name != TEXT("Tag"))
		{
			if (const FProperty* Field = FindFProperty<FProperty>(Shape.Struct, PropertyName))
			{
				OutDefault = Field->ContainerPtrToValuePtr<void>(Shape.DefaultStruct);
				return Field;
			}
		}
		if (const FProperty* Property = FindFProperty<FProperty>(Shape.Class, PropertyName); Property && IsEditorNodeArgument(*Property, Shape.NodeProperty))
		{
			bOutEditor = true;
			OutDefault = Property->ContainerPtrToValuePtr<void>(Shape.Defaults);
			return Property;
		}
		return nullptr;
	}

	UClass* FSemantic::LoadReferencedClass(const FDabExpr& Reference)
	{
		const FDabExpr& Expr = SkipParens(Reference);
		FString Path;
		if (IsCallTo(Expr, TEXT("Path")))
		{
			FString Error;
			if (!TryGetPathCallObjectPath(static_cast<const FDabCallExpr&>(Expr), Path, Error))
			{
				return nullptr;
			}
		}
		else if (const FDabLiteralExpr* Literal = Expr.As<FDabLiteralExpr>(); Literal && Literal->LiteralKind == EDabLiteralKind::String)
		{
			Path = Literal->Text;
		}
		else
		{
			return nullptr;
		}

		if (UClass* const* Known = LoadedClasses.Find(Path))
		{
			return *Known;
		}
		UClass* Class = nullptr;
		if (Path.StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive))
		{
			Class = FindObject<UClass>(nullptr, *Path);
		}
		else
		{
			const FString AssetPath = NormalizeAssetObjectPath(Path);
			UObject* Object = LoadObject<UObject>(nullptr, AssetPath, FStringView(), LOAD_NoWarn | LOAD_Quiet);
			if (const UBlueprint* Blueprint = Cast<UBlueprint>(Object))
			{
				Class = Blueprint->GeneratedClass;
			}
			else
			{
				Class = Cast<UClass>(Object);
			}
			if (!Class)
			{
				Class = LoadObject<UClass>(nullptr, AssetPath + TEXT("_C"), FStringView(), LOAD_NoWarn | LOAD_Quiet);
			}
		}
		LoadedClasses.Add(Path, Class);
		return Class;
	}

	bool FSemantic::GetLinkedInputPoses(const FDabModelNode& Node, TArray<FString>& OutNames)
	{
		OutNames.Reset();
		const bool bLayer = Node.ClassName.EndsWith(TEXT("AnimGraphNode_LinkedAnimLayer"), ESearchCase::CaseSensitive);
		FString FunctionName = TEXT("AnimGraph");
		if (bLayer)
		{
			const FDabExpr* LayerValue = FindSemanticModelProperty(Node, TEXT("Layer"));
			if (!LayerValue || !ReadNameOrStringText(*LayerValue, FunctionName) || FunctionName.IsEmpty())
			{
				return false;
			}
		}

		UClass* Target = nullptr;
		if (bLayer)
		{
			if (const FDabExpr* Interface = FindSemanticModelProperty(Node, TEXT("Interface")))
			{
				Target = LoadReferencedClass(*Interface);
			}
		}
		if (!Target)
		{
			if (const FDabExpr* Instance = FindSemanticModelProperty(Node, TEXT("InstanceClass")))
			{
				Target = LoadReferencedClass(*Instance);
			}
		}
		if (!Target)
		{
			// A layer of this Blueprint: its inputs are the Input(...) poses its graph declares.
			if (const TArray<FString>* Own = bLayer ? LayerInputs.Find(FunctionName) : nullptr)
			{
				OutNames = *Own;
				return true;
			}
			return false;
		}

		const IAnimClassInterface* AnimClass = IAnimClassInterface::GetFromClass(Target);
		if (!AnimClass)
		{
			return false;
		}
		// The text may hold the layer's name made an identifier (`Upper Body` -> `UpperBody`).
		const FName Wanted(*FunctionName);
		for (const FAnimBlueprintFunction& Function : AnimClass->GetAnimBlueprintFunctions())
		{
			if (Function.Name == Wanted || UE::DreamAnim::Compiler::MakeDabIdentifier(Function.Name.ToString()) == FunctionName)
			{
				// Input poses keep their engine names (written as strings when they are not identifiers).
				for (const FName& Input : Function.InputPoseNames)
				{
					OutNames.Add(Input.ToString());
				}
				return true;
			}
		}
		return false;
	}

	const UEnum* FSemantic::FindBlendEnum(const FNodeRequest& Request)
	{
		for (const FNodeValueArg& Arg : Request.Args)
		{
			if (Arg.Name != TEXT("BoundEnum") || !Arg.Value)
			{
				continue;
			}
			const FDabExpr& Value = SkipParens(*Arg.Value);
			const UEnum* Enum = nullptr;
			FString Text;
			if (IsCallTo(Value, TEXT("Path")))
			{
				FString Error;
				if (TryGetPathCallObjectPath(static_cast<const FDabCallExpr&>(Value), Text, Error))
				{
					Enum = LoadObject<UEnum>(nullptr, Text, FStringView(), LOAD_NoWarn | LOAD_Quiet);
				}
			}
			else if (ReadNameOrStringText(Value, Text))
			{
				Enum = Text.StartsWith(TEXT("/"), ESearchCase::CaseSensitive)
					? LoadObject<UEnum>(nullptr, Text, FStringView(), LOAD_NoWarn | LOAD_Quiet)
					: FindFirstObject<UEnum>(*Text, EFindFirstObjectOptions::NativeFirst);
			}
			if (!Enum)
			{
				Sink.Error(TEXT("DAB4002"), Value.Span, FString::Printf(TEXT("There is no enum `%s`."), *PrintValueText(Value)),
					TEXT("Name a C++ enum (EAlsHipsDirection) or a user enum by Path(...)."));
			}
			return Enum;
		}

		// No BoundEnum: the enum of the path ActiveEnumValue is bound to.
		for (const FNodeValueArg& Arg : Request.Args)
		{
			if (Arg.Name != TEXT("ActiveEnumValue") || !Arg.Value || IsConstant(*Arg.Value))
			{
				continue;
			}
			const UStruct* Scope = nullptr;
			const FProperty* Property = nullptr;
			if (TypePath(*Arg.Value, Scope, Property))
			{
				if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
				{
					return EnumProperty->GetEnum();
				}
				if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
				{
					return ByteProperty->Enum;
				}
			}
		}
		return nullptr;
	}

	// ------------------------------------------------------------------------------------------
	// Graphs
	// ------------------------------------------------------------------------------------------

	FString FSemantic::MakeNodeId(FGraphScope& Scope, const FString& Hint) const
	{
		const FString Base = Hint.IsEmpty() ? FString(TEXT("Node")) : Hint;
		FString Candidate = Base;
		for (int32 Suffix = 2; Scope.UsedIds.Contains(Candidate); ++Suffix)
		{
			Candidate = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
		}
		Scope.UsedIds.Add(Candidate);
		return Candidate;
	}

	FDabModelNode* FSemantic::FindNodeMutable(FGraphScope& Scope, const FString& Id) const
	{
		for (FDabModelNode& Node : Scope.Graph->Nodes)
		{
			if (Node.Id.Equals(Id, ESearchCase::CaseSensitive))
			{
				return &Node;
			}
		}
		return nullptr;
	}

	/** Whether a named pose statement is marked `[ Cached ]`. */
	bool IsSemanticCachedPose(const FDabAssignStmt& Statement)
	{
		return Statement.Meta && Statement.Meta->Find(TEXT("Cached")) != nullptr;
	}

	void FSemantic::CollectGraphNames(const TArray<FDabStmtPtr>& Statements, const FString& Location, const TSet<FString>& Reserved)
	{
		for (const FDabStmtPtr& Statement : Statements)
		{
			const FDabAssignStmt* Assign = Statement ? Statement->As<FDabAssignStmt>() : nullptr;
			if (!Assign || Reserved.Contains(Assign->Name))
			{
				continue;
			}
			if (!IsSemanticCachedPose(*Assign))
			{
				PoseOwners.FindOrAdd(Assign->Name, Location);
				continue;
			}
			if (const FString* Owner = CacheOwners.Find(Assign->Name))
			{
				Sink.Error(TEXT("DAB6008"), Assign->NameSpan,
					FString::Printf(TEXT("The cached pose `%s` is already defined in %s; cache names are shared by the whole Blueprint."), *Assign->Name, **Owner));
				continue;
			}
			if (MachineStmts.Contains(Assign->Name))
			{
				Sink.Error(TEXT("DAB6008"), Assign->NameSpan, FString::Printf(TEXT("`%s` names both a cached pose and a state machine."), *Assign->Name));
				continue;
			}
			CacheOwners.Add(Assign->Name, Location);
		}
	}

	/** Every `Input(Name)` under Node. */
	void CollectSemanticInputNames(const FDabNode& Node, TArray<FString>& OutNames)
	{
		if (const FDabCallExpr* Call = Node.As<FDabCallExpr>())
		{
			const FDabNameExpr* Callee = Call->Callee ? SkipParens(*Call->Callee).As<FDabNameExpr>() : nullptr;
			FString Name;
			if (Callee && Callee->Name == TEXT("Input") && Call->Args.Num() > 0 && Call->Args[0] && Call->Args[0]->Value
				&& ReadNameOrStringText(*Call->Args[0]->Value, Name))
			{
				OutNames.AddUnique(Name);
			}
		}
		ForEachChild(Node, [&OutNames](const FDabNode& Child) { CollectSemanticInputNames(Child, OutNames); });
	}

	void FSemantic::CollectLayerInputs(const FDabNamedBlockStmt& Stmt)
	{
		TArray<FString> Names;
		if (Stmt.Body)
		{
			CollectSemanticInputNames(*Stmt.Body, Names);
		}
		if (Stmt.Value)
		{
			CollectSemanticInputNames(*Stmt.Value, Names);
		}
		LayerInputs.Add(Stmt.Name, MoveTemp(Names));
	}

	void FSemantic::BuildGraph(const TArray<FDabStmtPtr>& Statements, FDabModelGraph& Graph, const FString& Location, const TCHAR* ResultName,
		const TSet<FString>& Reserved, const FDabSpan& OwnerSpan)
	{
		FGraphScope Scope;
		Scope.Graph = &Graph;
		Scope.Location = Location;

		const FDabAssignStmt* Result = nullptr;
		for (const FDabStmtPtr& Statement : Statements)
		{
			if (!Statement || Statement->Is<FDabRegionStmt>())
			{
				continue;
			}
			if (const FDabNamedBlockStmt* Named = Statement->As<FDabNamedBlockStmt>())
			{
				if (Named->Word != TEXT("Template"))
				{
					Sink.Error(TEXT("DAB6011"), Named->WordSpan, FString::Printf(TEXT("`%s %s` cannot be written inside %s."), *Named->Word, *Named->Name, *Location),
						Named->Word == TEXT("StateMachine") ? TEXT("Write state machines at the document level and name them where they run.") : FString());
				}
				continue;
			}
			const FDabAssignStmt* Assign = Statement->As<FDabAssignStmt>();
			if (!Assign)
			{
				Sink.Error(TEXT("DAB6011"), Statement->Span, FString::Printf(TEXT("%s holds named poses and `%s = ...;`; this statement is neither."), *Location, ResultName));
				continue;
			}
			if (Assign->Name == TEXT("Inputs") && FCString::Strcmp(ResultName, TEXT("Output")) == 0)
			{
				// The graph's input poses in the order a linked graph node feeds them.
				const FDabListExpr* List = Assign->Value ? SkipParens(*Assign->Value).As<FDabListExpr>() : nullptr;
				if (!List)
				{
					Sink.Error(TEXT("DAB6011"), Assign->NameSpan, TEXT("`Inputs` lists the graph's input poses in order: Inputs = [ \"First Input\", Second ];"));
					continue;
				}
				for (const FDabExprPtr& Item : List->Items)
				{
					FString Name;
					if (Item && ReadNameOrStringText(*Item, Name))
					{
						Graph.InputPoses.AddUnique(Name);
					}
				}
				continue;
			}
			if (Assign->Name == ResultName)
			{
				if (Result)
				{
					Sink.Error(TEXT("DAB6008"), Assign->NameSpan, FString::Printf(TEXT("%s has two `%s` statements."), *Location, ResultName));
					continue;
				}
				Result = Assign;
				continue;
			}
			if (Reserved.Contains(Assign->Name))
			{
				continue;
			}
			if (Scope.Poses.Contains(Assign->Name))
			{
				Sink.Error(TEXT("DAB6008"), Assign->NameSpan, FString::Printf(TEXT("%s names the pose `%s` twice."), *Location, *Assign->Name));
				continue;
			}
			FPoseDef Def;
			Def.Stmt = Assign;
			Def.bCached = IsSemanticCachedPose(*Assign);
			Scope.Poses.Add(Assign->Name, Def);
			Scope.PoseOrder.Add(Assign->Name);
		}

		// A cache is built where it is defined; every use is a separate UseCachedPose node.
		for (const FString& Name : Scope.PoseOrder)
		{
			FPoseDef& Def = Scope.Poses.FindChecked(Name);
			if (Def.bCached && !Def.bBuilt)
			{
				BuildCacheDefinition(Name, Def, Scope);
			}
		}

		if (Result && Result->Value)
		{
			if (Result->Meta && Result->Meta->Find(TEXT("Cached")))
			{
				Sink.Error(TEXT("DAB6016"), Result->NameSpan, FString::Printf(TEXT("`%s` is the graph's result; it cannot be cached."), ResultName),
					TEXT("Cache a named pose and use its name here."));
			}
			Graph.ResultNodeId = BuildPose(*Result->Value, Scope, ResultName, nullptr);
			ApplyPoseMeta(Result->Meta.Get(), Graph.ResultNodeId, Scope, ResultName);
		}
		else if (!Result)
		{
			Sink.Error(TEXT("DAB6012"), OwnerSpan, FString::Printf(TEXT("%s has no `%s = ...;`."), *Location, ResultName),
				FString::Printf(TEXT("Write `%s = None;` for a graph that outputs nothing."), ResultName));
		}

		// Input poses not listed in `Inputs` follow in the order they are first written.
		if (FCString::Strcmp(ResultName, TEXT("Output")) == 0)
		{
			for (const FDabStmtPtr& Statement : Statements)
			{
				TArray<FString> Written;
				if (Statement)
				{
					CollectSemanticInputNames(*Statement, Written);
				}
				for (const FString& Name : Written)
				{
					Graph.InputPoses.AddUnique(Name);
				}
			}
		}

		// A named pose nothing uses still makes its nodes, so nothing written is lost.
		for (const FString& Name : Scope.PoseOrder)
		{
			FPoseDef& Def = Scope.Poses.FindChecked(Name);
			if (!Def.bCached && !Def.bBuilt && !Def.bBuilding)
			{
				Sink.Warning(TEXT("DAB6004"), Def.Stmt->NameSpan, FString::Printf(TEXT("The pose `%s` is never used."), *Name),
					FString::Printf(TEXT("Use it in %s or delete it; its nodes are made anyway, unlinked."), *Location));
				BuildNamedPose(Name, Def, Scope, Def.Stmt->NameSpan);
			}
		}
	}

	void FSemantic::BuildValueGraph(const FDabExpr& Value, FDabModelGraph& Graph, const FString& Location)
	{
		FGraphScope Scope;
		Scope.Graph = &Graph;
		Scope.Location = Location;
		Graph.ResultNodeId = BuildPose(Value, Scope, TEXT("Output"), nullptr);
		// `Layer X = Pose;` has no room for `Inputs`: its inputs are in the order they are written.
		TArray<FString> Written;
		CollectSemanticInputNames(Value, Written);
		for (const FString& Name : Written)
		{
			Graph.InputPoses.AddUnique(Name);
		}
	}

	FString FSemantic::BuildPose(const FDabExpr& InExpr, FGraphScope& Scope, const FString& IdHint, const FString* Feed)
	{
		const FDabExpr& Expr = SkipParens(InExpr);
		if (const FDabNameExpr* Name = Expr.As<FDabNameExpr>())
		{
			if (Feed)
			{
				Sink.Error(TEXT("DAB6001"), Expr.Span,
					FString::Printf(TEXT("`%s` is a pose of its own; every step of a stack after the first is a node with one free pose input."), *Name->Name),
					TEXT("Put the name first in the stack, or pass it as a pose argument."));
				return FString();
			}
			return BuildPoseName(*Name, Scope, IdHint);
		}
		if (const FDabCallExpr* Call = Expr.As<FDabCallExpr>())
		{
			return BuildCallNode(*Call, Scope, IdHint, Feed);
		}
		if (const FDabStackExpr* Stack = Expr.As<FDabStackExpr>())
		{
			if (Stack->Steps.IsEmpty())
			{
				Sink.Error(TEXT("DAB6010"), Expr.Span, TEXT("An empty stack is not a pose."));
				return FString();
			}
			FString Previous;
			for (int32 Index = 0; Index < Stack->Steps.Num(); ++Index)
			{
				const FDabExprPtr& Step = Stack->Steps[Index];
				if (!Step)
				{
					continue;
				}
				// The stack's last step is its root and takes the stack's id; the others are numbered under it.
				const FString StepHint = Index == Stack->Steps.Num() - 1 ? IdHint : FString::Printf(TEXT("%s/%d"), *IdHint, Index);
				if (Index == 0)
				{
					Previous = BuildPose(*Step, Scope, StepHint, Feed);
				}
				else
				{
					const FString Input = Previous;
					Previous = BuildPose(*Step, Scope, StepHint, &Input);
				}
			}
			return Previous;
		}
		Sink.Error(TEXT("DAB6010"), Expr.Span, FString::Printf(TEXT("`%s` is not a pose."), *PrintValueText(Expr)),
			TEXT("A pose is a node such as SequencePlayer(...), a stack { ... }, a named pose, a cached pose, a state machine's name, or None."));
		return FString();
	}

	FString FSemantic::AddSimpleNode(FGraphScope& Scope, const FString& IdHint, const TCHAR* ClassName, const FString& CacheName, const FString& MachineName)
	{
		FDabModelNode Node;
		Node.Id = MakeNodeId(Scope, IdHint);
		Node.ClassName = ClassName;
		Node.CacheName = CacheName;
		Node.StateMachineName = MachineName;
		const FString Id = Node.Id;
		Scope.Graph->Nodes.Add(MoveTemp(Node));
		return Id;
	}

	FString FSemantic::BuildPoseName(const FDabNameExpr& Name, FGraphScope& Scope, const FString& IdHint)
	{
		if (Name.Name == TEXT("None"))
		{
			return FString();
		}
		if (FPoseDef* Def = Scope.Poses.Find(Name.Name))
		{
			++Def->Uses;
			if (!Def->bCached)
			{
				return BuildNamedPose(Name.Name, *Def, Scope, Name.Span);
			}
			if (Def->bBuilding)
			{
				Sink.Error(TEXT("DAB6009"), Name.Span, FString::Printf(TEXT("The cached pose `%s` uses itself."), *Name.Name));
				return FString();
			}
			return AddSimpleNode(Scope, IdHint, TEXT("AnimGraphNode_UseCachedPose"), Name.Name, FString());
		}
		if (CacheOwners.Contains(Name.Name))
		{
			return AddSimpleNode(Scope, IdHint, TEXT("AnimGraphNode_UseCachedPose"), Name.Name, FString());
		}
		if (MachineStmts.Contains(Name.Name))
		{
			int32& Uses = MachineUses.FindOrAdd(Name.Name);
			if (++Uses > 1)
			{
				Sink.Error(TEXT("DAB6002"), Name.Span, FString::Printf(TEXT("The state machine `%s` runs in one place only; it is already used."), *Name.Name),
					TEXT("Cache the pose where the machine runs ([ Cached ]) and use the cache here."));
				return FString();
			}
			return AddSimpleNode(Scope, IdHint, TEXT("AnimGraphNode_StateMachine"), FString(), Name.Name);
		}

		const FString* Owner = PoseOwners.Find(Name.Name);
		Sink.Error(TEXT("DAB6003"), Name.Span, FString::Printf(TEXT("There is no pose `%s` in %s."), *Name.Name, *Scope.Location),
			Owner ? FString::Printf(TEXT("`%s` is a pose of %s; mark it [ Cached ] there to use it here."), *Name.Name, **Owner)
				: TEXT("A name in a pose is a named pose of this graph, a cached pose or a state machine."));
		return FString();
	}

	FString FSemantic::BuildNamedPose(const FString& Name, FPoseDef& Def, FGraphScope& Scope, const FDabSpan& UseSpan)
	{
		if (Def.bBuilding)
		{
			Sink.Error(TEXT("DAB6009"), UseSpan, FString::Printf(TEXT("The pose `%s` uses itself."), *Name));
			return FString();
		}
		if (Def.bBuilt)
		{
			Sink.Error(TEXT("DAB6005"), UseSpan, FString::Printf(TEXT("The pose `%s` is used more than once."), *Name),
				FString::Printf(TEXT("Mark it `%s = ... [ Cached ];` to use it in several places."), *Name));
			return FString();
		}
		Def.bBuilding = true;
		Def.RootId = Def.Stmt->Value ? BuildPose(*Def.Stmt->Value, Scope, Name, nullptr) : FString();
		Def.bBuilding = false;
		Def.bBuilt = true;
		ApplyPoseMeta(Def.Stmt->Meta.Get(), Def.RootId, Scope, Name);
		return Def.RootId;
	}

	void FSemantic::BuildCacheDefinition(const FString& Name, FPoseDef& Def, FGraphScope& Scope)
	{
		Def.bBuilding = true;
		const FString SaveId = MakeNodeId(Scope, Name);
		const FString PoseId = Def.Stmt->Value ? BuildPose(*Def.Stmt->Value, Scope, Name + TEXT("/Pose"), nullptr) : FString();
		Def.bBuilding = false;
		Def.bBuilt = true;
		Def.RootId = SaveId;

		FDabModelNode Save;
		Save.Id = SaveId;
		Save.ClassName = TEXT("AnimGraphNode_SaveCachedPose");
		Save.CacheName = Name;
		if (!PoseId.IsEmpty())
		{
			FDabModelPoseLink Link;
			Link.PinName = TEXT("Pose");
			Link.SourceNodeId = PoseId;
			Save.PoseInputs.Add(MoveTemp(Link));
		}
		Scope.Graph->Nodes.Add(MoveTemp(Save));

		// Functions hung on the statement belong to the pose's own root, not to the cache node.
		ApplyPoseMeta(Def.Stmt->Meta.Get(), PoseId, Scope, Name);
	}

	void FSemantic::ApplyPoseMeta(const FDabMeta* Meta, const FString& RootId, FGraphScope& Scope, const FString& PoseName)
	{
		if (!Meta)
		{
			return;
		}
		for (const TUniquePtr<FDabMetaEntry>& Entry : Meta->Entries)
		{
			if (!Entry || Entry->Key == TEXT("Cached"))
			{
				if (Entry && Entry->Value)
				{
					Sink.Error(TEXT("DAB6016"), Entry->KeySpan, TEXT("`Cached` is a flag; it takes no value."));
				}
				continue;
			}
			FString Site;
			if (Entry->Key == TEXT("OnInitialUpdate"))
			{
				Site = TEXT("InitialUpdate");
			}
			else if (Entry->Key == TEXT("OnBecomeRelevant"))
			{
				Site = TEXT("BecomeRelevant");
			}
			else if (Entry->Key == TEXT("OnUpdate"))
			{
				Site = TEXT("Update");
			}
			else
			{
				Sink.Error(TEXT("DAB6016"), Entry->KeySpan, FString::Printf(TEXT("`%s` is not metadata of a pose."), *Entry->Key),
					TEXT("A named pose takes Cached, OnInitialUpdate, OnBecomeRelevant and OnUpdate."));
				continue;
			}
			if (!Entry->Value)
			{
				Sink.Error(TEXT("DAB6016"), Entry->KeySpan, FString::Printf(TEXT("`%s` names the function to call: `%s = Self.Function()`."), *Entry->Key, *Entry->Key));
				continue;
			}
			FDabModelNodeFunction Function;
			Function.Site = Site;
			if (!ConvertCall(*Entry->Value, Function.Call, FString::Printf(TEXT("%s of `%s`"), *Entry->Key, *PoseName)))
			{
				continue;
			}
			FDabModelNode* Node = RootId.IsEmpty() ? nullptr : FindNodeMutable(Scope, RootId);
			if (!Node)
			{
				Sink.Error(TEXT("DAB6016"), Entry->KeySpan, FString::Printf(TEXT("`%s` has no node to call `%s` on."), *PoseName, *Entry->Key));
				continue;
			}
			if (Node->NodeFunctions.ContainsByPredicate([&Site](const FDabModelNodeFunction& Existing) { return Existing.Site == Site; }))
			{
				Sink.Error(TEXT("DAB4011"), Entry->KeySpan, FString::Printf(TEXT("The node of `%s` already has an %s function."), *PoseName, *Entry->Key));
				continue;
			}
			Node->NodeFunctions.Add(MoveTemp(Function));
		}
	}

	// ------------------------------------------------------------------------------------------
	// Nodes
	// ------------------------------------------------------------------------------------------

	FString FSemantic::BuildCallNode(const FDabCallExpr& Call, FGraphScope& Scope, const FString& IdHint, const FString* Feed)
	{
		const FDabNameExpr* Callee = Call.Callee ? SkipParens(*Call.Callee).As<FDabNameExpr>() : nullptr;
		if (!Callee || Callee->Name == TEXT("Path") || Callee->Name == TEXT("Unsupported") || Aliases.Contains(Callee->Name))
		{
			Sink.Error(TEXT("DAB6010"), Call.Span, FString::Printf(TEXT("`%s` is not a pose."), *PrintValueText(Call)),
				TEXT("A pose is a node such as SequencePlayer(...), a stack { ... }, a named pose, a cached pose, a state machine's name, or None."));
			return FString();
		}
		const FString& TypeName = Callee->Name;

		FNodeRequest Request;
		Request.Span = Call.Span;
		TArray<const FDabArgument*> Positional;
		TArray<const FDabArgument*> Named;
		for (const FDabArgumentPtr& Arg : Call.Args)
		{
			if (Arg && Arg->Value)
			{
				(Arg->IsNamed() ? Named : Positional).Add(Arg.Get());
			}
		}
		const auto AddNamedArgs = [&Request](TConstArrayView<const FDabArgument*> Args)
		{
			for (const FDabArgument* Arg : Args)
			{
				Request.Args.Add({Arg->Name, Arg->Value.Get(), Arg->NameSpan});
			}
		};

		if (TypeName == TEXT("Input"))
		{
			Request.Class = FindNodeClassByName(TEXT("AnimGraphNode_LinkedInputPose"));
			FString InputName;
			if (Positional.Num() != 1 || !ReadNameOrStringText(*Positional[0]->Value, InputName))
			{
				Sink.Error(TEXT("DAB4009"), Call.Span, TEXT("Input(Name) names one input pose of this graph."));
				return FString();
			}
			Request.InputPoseName = InputName;
			AddNamedArgs(Named);
		}
		else if (TypeName == TEXT("Slot"))
		{
			Request.Class = FindNodeClassByName(TEXT("AnimGraphNode_Slot"));
			if (Positional.Num() > 1)
			{
				Sink.Error(TEXT("DAB4008"), Positional[1]->Value->Span, TEXT("Slot takes the slot's name, then named arguments: Slot(\"Name\", bAlwaysUpdateSourcePose = true)."));
			}
			if (!Positional.IsEmpty())
			{
				Request.Args.Add({TEXT("SlotName"), Positional[0]->Value.Get(), Positional[0]->Value->Span});
			}
			AddNamedArgs(Named);
		}
		else if (TypeName == TEXT("Call"))
		{
			Request.Class = FindNodeClassByName(TEXT("AnimGraphNode_CallFunction"));
			if (Positional.IsEmpty())
			{
				Sink.Error(TEXT("DAB4009"), Call.Span, TEXT("Call(Target.Function) names the function it calls: Call(Self.Refresh)."));
				return FString();
			}
			const FDabExpr& First = SkipParens(*Positional[0]->Value);
			const FDabExpr* FunctionExpr = &First;
			TArray<const FDabArgument*> CallArgs;
			if (const FDabCallExpr* Inner = First.As<FDabCallExpr>())
			{
				FunctionExpr = Inner->Callee.Get();
				for (const FDabArgumentPtr& Arg : Inner->Args)
				{
					CallArgs.Add(Arg.Get());
				}
			}
			FDabModelCall ModelCall;
			const UFunction* Function = nullptr;
			if (!FunctionExpr || !ResolveCallee(*FunctionExpr, ModelCall.Target, ModelCall.Function, Function, TEXT("Call")))
			{
				return FString();
			}
			for (int32 Index = 1; Index < Positional.Num(); ++Index)
			{
				CallArgs.Add(Positional[Index]);
			}

			// Named arguments: `On` is the call site; the function's parameters are its arguments; the node's own settings stay on the node.
			const FNodeShape* CallShape = GetShape(Request.Class);
			for (const FDabArgument* Arg : Named)
			{
				if (Arg->Name == TEXT("On"))
				{
					FString Site;
					if (!ReadNameOrStringText(*Arg->Value, Site))
					{
						Sink.Error(TEXT("DAB4009"), Arg->Value->Span, TEXT("`On` names a call site: Update, BecomeRelevant, Initialize, ..."));
						continue;
					}
					if (!Site.StartsWith(TEXT("On"), ESearchCase::CaseSensitive))
					{
						Site = TEXT("On") + Site;
					}
					Request.OwnedValues.Add(MakeNameExpr(Site));
					Request.Args.Add({TEXT("CallSite"), Request.OwnedValues.Last().Get(), Arg->Value->Span});
					continue;
				}
				const bool bParameter = Function && FindFProperty<FProperty>(Function, FName(*Arg->Name)) != nullptr;
				const void* IgnoredDefault = nullptr;
				bool bIgnoredEditor = false;
				if (!bParameter && CallShape && FindNodeProperty(*CallShape, Arg->Name, IgnoredDefault, bIgnoredEditor))
				{
					Request.Args.Add({Arg->Name, Arg->Value.Get(), Arg->NameSpan});
					continue;
				}
				CallArgs.Add(Arg);
			}
			ConvertCallArguments(Function, CallArgs, ModelCall.Args, FString::Printf(TEXT("Call(%s.%s)"), *PrintValueText(*ModelCall.Target), *ModelCall.Function));
			Request.Call = MoveTemp(ModelCall);
		}
		else if (TypeName == TEXT("bones"))
		{
			Sink.Error(TEXT("DAB4009"), Call.Span, TEXT("bones(...) is a layer of LayeredBoneBlend's LayerSetup, not a pose."));
			return FString();
		}
		else
		{
			Request.Class = ResolveNodeClass(TypeName, Callee->Span);
			if (!Request.Class)
			{
				return FString();
			}
			for (const FDabArgument* Arg : Positional)
			{
				Sink.Error(TEXT("DAB4008"), Arg->Value->Span, FString::Printf(TEXT("The arguments of %s are named: `Name = value`."), *TypeName));
			}
			AddNamedArgs(Named);
		}

		if (!Request.Class)
		{
			Sink.Error(TEXT("DAB3001"), Callee->Span, FString::Printf(TEXT("The node class behind `%s` is not loaded."), *TypeName));
			return FString();
		}
		return BuildAnimNode(Request, Scope, IdHint, Feed);
	}

	FString FSemantic::BuildAnimNode(FNodeRequest& Request, FGraphScope& Scope, const FString& IdHint, const FString* Feed)
	{
		const FNodeShape* Shape = GetShape(Request.Class);
		if (!Shape || !Shape->NodeProperty)
		{
			Sink.Error(TEXT("DAB3001"), Request.Span, FString::Printf(TEXT("%s has no runtime node; it cannot be a pose."), *Request.Class->GetName()));
			return FString();
		}

		FDabModelNode Node;
		Node.Id = MakeNodeId(Scope, IdHint);
		Node.ClassName = Request.Class->GetName();
		Node.InputPoseName = Request.InputPoseName;
		if (Request.Call.IsSet())
		{
			Node.Call = MoveTemp(Request.Call);
		}

		FString TypeLabel = Node.ClassName;
		{
			const int32 At = TypeLabel.Find(TEXT("AnimGraphNode_"), ESearchCase::CaseSensitive);
			if (At != INDEX_NONE)
			{
				TypeLabel = TypeLabel.Mid(At + 14);
			}
		}
		const bool bByEnum = Node.ClassName == TEXT("AnimGraphNode_BlendListByEnum");
		const bool bByTag = Node.ClassName.EndsWith(TEXT("AnimGraphNode_GameplayTagsBlend"), ESearchCase::CaseSensitive);
		const bool bCurves = Node.ClassName == TEXT("AnimGraphNode_ModifyCurve");
		const bool bBoneLayers = Node.ClassName == TEXT("AnimGraphNode_LayeredBoneBlend");
		const bool bEvaluator = Node.ClassName == TEXT("AnimGraphNode_SequenceEvaluator");
		const UEnum* BlendEnum = bByEnum ? FindBlendEnum(Request) : nullptr;

		// A pose map keyed by enumerator or tag: Default is pin 0, each key in written order takes the next pin.
		bool bKeyed = false;
		int32 KeyedCount = 0;
		TArray<FString> KeyedEntries;
		TArray<int32> KeyedEnumIndices;
		const auto LookupKey = [&KeyedEntries, &KeyedEnumIndices, BlendEnum, bByEnum](const FString& KeyText) -> int32
		{
			if (KeyText == TEXT("Default"))
			{
				return 0;
			}
			if (bByEnum)
			{
				const int32 EnumIndex = BlendEnum ? BlendEnum->GetIndexByNameString(KeyText) : INDEX_NONE;
				const int32 Position = EnumIndex == INDEX_NONE ? INDEX_NONE : KeyedEnumIndices.Find(EnumIndex);
				return Position == INDEX_NONE ? INDEX_NONE : Position + 1;
			}
			const int32 Position = KeyedEntries.Find(KeyText);
			return Position == INDEX_NONE ? INDEX_NONE : Position + 1;
		};

		struct FPendingValue
		{
			const FNodeValueArg* Arg = nullptr;
			const FProperty* Property = nullptr;
			const void* Default = nullptr;
			bool bEditor = false;
		};
		TArray<FPendingValue> Values;
		const FNodeValueArg* InputsArg = nullptr;
		TSet<FString> WrittenPoses;
		TMap<FString, int32> PoseCounts;

		// Poses first: they make the nodes this one links to.
		for (const FNodeValueArg& Arg : Request.Args)
		{
			if (!Arg.Value)
			{
				continue;
			}
			if (Shape->bLinkedGraph && Arg.Name == TEXT("Inputs"))
			{
				InputsArg = &Arg;
				continue;
			}
			FPendingValue Pending;
			Pending.Arg = &Arg;
			Pending.Property = FindNodeProperty(*Shape, Arg.Name, Pending.Default, Pending.bEditor);
			if (!Pending.Property || Pending.bEditor || !IsPoseLinkProperty(*Pending.Property))
			{
				Values.Add(Pending);
				continue;
			}

			const FString PinName = Pending.Property->GetName();
			WrittenPoses.Add(PinName);
			const FDabExpr& Value = SkipParens(*Arg.Value);
			if (!CastField<FArrayProperty>(Pending.Property))
			{
				const FString Id = BuildPose(Value, Scope, Node.Id + TEXT("/") + PinName, nullptr);
				if (!Id.IsEmpty())
				{
					Node.PoseInputs.Add({PinName, Id});
				}
				continue;
			}
			if (Shape->bLinkedGraph)
			{
				Sink.Error(TEXT("DAB4004"), Arg.Span, FString::Printf(TEXT("%s takes its input poses as Inputs = { Name = Pose; }."), *TypeLabel));
				continue;
			}
			if (const FDabListExpr* List = Value.As<FDabListExpr>())
			{
				for (int32 Index = 0; Index < List->Items.Num(); ++Index)
				{
					// Every entry is listed, linked or not, so the array keeps its size.
					const FString ElementPin = FString::Printf(TEXT("%s_%d"), *PinName, Index);
					const FString Id = List->Items[Index] ? BuildPose(*List->Items[Index], Scope, Node.Id + TEXT("/") + ElementPin, nullptr) : FString();
					Node.PoseInputs.Add({ElementPin, Id});
				}
				PoseCounts.Add(PinName, List->Items.Num());
				continue;
			}
			const FDabBlockExpr* Block = Value.As<FDabBlockExpr>();
			if (!Block || !(bByEnum || bByTag) || PinName != TEXT("BlendPose"))
			{
				Sink.Error(TEXT("DAB4006"), Value.Span, FString::Printf(TEXT("%s of %s is a list of poses [ A, B, ... ]."), *PinName, *TypeLabel));
				continue;
			}
			if (bByEnum && !BlendEnum)
			{
				Sink.Error(TEXT("DAB4012"), Value.Span, TEXT("BlendListByEnum needs its enum to read the keys of BlendPose."),
					TEXT("Write `BoundEnum = EnumName`, or bind ActiveEnumValue to a path of that enum's type."));
				continue;
			}

			bKeyed = true;
			const FDabExpr* DefaultPose = nullptr;
			TArray<const FDabExpr*> KeyedPoses;
			for (const FDabStmtPtr& Statement : Block->Statements)
			{
				if (!Statement || Statement->Is<FDabRegionStmt>())
				{
					continue;
				}
				const FDabAssignStmt* Entry = Statement->As<FDabAssignStmt>();
				if (!Entry || !Entry->Value)
				{
					Sink.Error(TEXT("DAB6011"), Statement->Span, TEXT("Each entry of BlendPose is `Key = Pose;`."));
					continue;
				}
				if (Entry->Name == TEXT("Default"))
				{
					if (DefaultPose)
					{
						Sink.Error(TEXT("DAB4011"), Entry->NameSpan, TEXT("BlendPose gives Default twice."));
					}
					DefaultPose = Entry->Value.Get();
					continue;
				}
				if (bByEnum)
				{
					const int32 EnumIndex = BlendEnum->GetIndexByNameString(Entry->Name);
					if (EnumIndex == INDEX_NONE)
					{
						Sink.Error(TEXT("DAB4002"), Entry->NameSpan, FString::Printf(TEXT("%s has no value `%s`."), *BlendEnum->GetName(), *Entry->Name));
						continue;
					}
					if (KeyedEnumIndices.Contains(EnumIndex))
					{
						Sink.Error(TEXT("DAB4011"), Entry->NameSpan, FString::Printf(TEXT("BlendPose gives `%s` twice."), *Entry->Name));
						continue;
					}
					KeyedEnumIndices.Add(EnumIndex);
					KeyedEntries.Add(BlendEnum->GetNameByIndex(EnumIndex).ToString());
				}
				else
				{
					if (KeyedEntries.Contains(Entry->Name))
					{
						Sink.Error(TEXT("DAB4011"), Entry->NameSpan, FString::Printf(TEXT("BlendPose gives \"%s\" twice."), *Entry->Name));
						continue;
					}
					if (!UGameplayTagsManager::Get().RequestGameplayTag(FName(*Entry->Name), false).IsValid())
					{
						Sink.Warning(TEXT("DAB4003"), Entry->NameSpan, FString::Printf(TEXT("The gameplay tag \"%s\" is not registered."), *Entry->Name),
							TEXT("Add it to the project's gameplay tag table (Project Settings > GameplayTags) or fix its spelling."));
					}
					KeyedEntries.Add(Entry->Name);
				}
				KeyedPoses.Add(Entry->Value.Get());
			}

			KeyedCount = 1 + KeyedPoses.Num();
			for (int32 Index = 0; Index < KeyedCount; ++Index)
			{
				const FDabExpr* Pose = Index == 0 ? DefaultPose : KeyedPoses[Index - 1];
				const FString ElementPin = FString::Printf(TEXT("%s_%d"), *PinName, Index);
				const FString Id = Pose ? BuildPose(*Pose, Scope, Node.Id + TEXT("/") + ElementPin, nullptr) : FString();
				Node.PoseInputs.Add({ElementPin, Id});
			}
			PoseCounts.Add(PinName, KeyedCount);
		}

		// A linked graph's or layer's input poses, by the target's input pose names.
		if (InputsArg)
		{
			const FDabBlockExpr* Block = SkipParens(*InputsArg->Value).As<FDabBlockExpr>();
			if (!Block)
			{
				Sink.Error(TEXT("DAB4006"), InputsArg->Value->Span, TEXT("Inputs is a block of input poses: Inputs = { Name = Pose; }."));
			}
			else
			{
				for (const FDabStmtPtr& Statement : Block->Statements)
				{
					if (!Statement || Statement->Is<FDabRegionStmt>())
					{
						continue;
					}
					const FDabAssignStmt* Entry = Statement->As<FDabAssignStmt>();
					if (!Entry || !Entry->Value)
					{
						Sink.Error(TEXT("DAB6011"), Statement->Span, TEXT("Each entry of Inputs is `InputPose = Pose;`."));
						continue;
					}
					if (WrittenPoses.Contains(Entry->Name))
					{
						Sink.Error(TEXT("DAB4011"), Entry->NameSpan, FString::Printf(TEXT("Inputs gives `%s` twice."), *Entry->Name));
						continue;
					}
					WrittenPoses.Add(Entry->Name);
					const FString Id = BuildPose(*Entry->Value, Scope, Node.Id + TEXT("/") + Entry->Name, nullptr);
					if (!Id.IsEmpty())
					{
						Node.PoseInputs.Add({Entry->Name, Id});
					}
				}
			}
		}

		// A single value for a per-pose array means that value for every pose of the node's pose array.
		int32 PerPoseCount = INDEX_NONE;
		if (Shape->PoseArrays.Num() == 1)
		{
			if (const int32* Count = PoseCounts.Find(Shape->PoseArrays[0]))
			{
				PerPoseCount = *Count;
			}
		}

		TSet<FString> WrittenNames;
		TArray<const FNodeValueArg*> Curves;
		TArray<const FNodeValueArg*> CustomConstants;
		for (const FPendingValue& Pending : Values)
		{
			const FNodeValueArg& Arg = *Pending.Arg;
			const FString What = FString::Printf(TEXT("%s of %s"), *Arg.Name, *TypeLabel);
			const FDabExpr& Value = SkipParens(*Arg.Value);

			if (!Pending.Property)
			{
				if (bCurves)
				{
					Curves.Add(&Arg);
				}
				else if (Shape->bCustomProperty && !IsConstant(Value) && !IsPlaceholderCall(Value))
				{
					// A pin the node exposes from another class (an instance's variable, a rig's input).
					AddSemanticBinding(Node, Arg.Name, ConvertBinding(Value));
				}
				else if (Shape->bCustomProperty)
				{
					CustomConstants.Add(&Arg);
				}
				else
				{
					Sink.Error(TEXT("DAB4004"), Arg.Span, FString::Printf(TEXT("%s has no argument `%s`."), *TypeLabel, *Arg.Name),
						FString::Printf(TEXT("Arguments are the fields of %s and the editor node's settings (Tag, ...)."), *Shape->Struct->GetName()));
				}
				continue;
			}

			const FProperty& Property = *Pending.Property;
			const FString Name = Property.GetName();
			if (bByEnum && Name == TEXT("BoundEnum"))
			{
				// Read with the keys; recorded below.
				continue;
			}
			if (Pending.bEditor ? !IsRecordedEditorProperty(Property) : !IsRecordedNodeField(Property))
			{
				Sink.Error(TEXT("DAB4004"), Arg.Span, FString::Printf(TEXT("`%s` of %s is filled in by the editor or the compiler; it is not written."), *Name, *TypeLabel));
				continue;
			}
			if (bKeyed && ((bByEnum && (Name == TEXT("VisibleEnumEntries") || Name == TEXT("EnumToPoseIndex"))) || (bByTag && Name == TEXT("Tags"))))
			{
				Sink.Error(TEXT("DAB4011"), Arg.Span, FString::Printf(TEXT("`%s` follows from the keys of BlendPose; it is not written as well."), *Name));
				continue;
			}
			if (WrittenNames.Contains(Name))
			{
				Sink.Error(TEXT("DAB4011"), Arg.Span, FString::Printf(TEXT("%s gives `%s` twice."), *TypeLabel, *Name));
				continue;
			}
			WrittenNames.Add(Name);

			if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
			{
				TArray<const FDabExpr*> Items;
				TArray<FDabExprPtr> OwnedItems;
				if (const FDabListExpr* List = Value.As<FDabListExpr>())
				{
					for (const FDabExprPtr& Item : List->Items)
					{
						Items.Add(Item.Get());
					}
				}
				else if (const FDabBlockExpr* Block = Value.As<FDabBlockExpr>(); Block && bKeyed)
				{
					Items.Init(nullptr, KeyedCount);
					for (const FDabStmtPtr& Statement : Block->Statements)
					{
						const FDabAssignStmt* Entry = Statement ? Statement->As<FDabAssignStmt>() : nullptr;
						if (!Entry || !Entry->Value)
						{
							continue;
						}
						const int32 Index = LookupKey(Entry->Name);
						if (Index == INDEX_NONE)
						{
							Sink.Error(TEXT("DAB4002"), Entry->NameSpan, FString::Printf(TEXT("`%s` is not a key of BlendPose."), *Entry->Name),
								TEXT("Key a per-pose value the way BlendPose is keyed: Default, then BlendPose's own keys."));
							continue;
						}
						if (Items[Index])
						{
							Sink.Error(TEXT("DAB4011"), Entry->NameSpan, FString::Printf(TEXT("%s gives `%s` twice."), *Name, *Entry->Name));
							continue;
						}
						Items[Index] = Entry->Value.Get();
					}
				}
				else if (PerPoseCount != INDEX_NONE && !Value.Is<FDabBlockExpr>())
				{
					Items.Init(&Value, PerPoseCount);
				}
				else
				{
					Sink.Error(TEXT("DAB4006"), Value.Span, FString::Printf(TEXT("%s is a list [ ... ]%s."), *What,
						Shape->PoseArrays.Num() == 1 ? TEXT(", or one value for every pose once the poses are given") : TEXT("")));
					continue;
				}

				TArray<FDabExprPtr> Converted;
				for (int32 Index = 0; Index < Items.Num(); ++Index)
				{
					const FDabExpr* Item = Items[Index] ? &SkipParens(*Items[Index]) : nullptr;
					if (Item && bBoneLayers && Name == TEXT("LayerSetup") && IsCallTo(*Item, TEXT("bones")))
					{
						OwnedItems.Add(MakeSemanticBonesTuple(static_cast<const FDabCallExpr&>(*Item), Sink));
						Item = OwnedItems.Last().Get();
					}
					FDabExprPtr Element;
					if (!Item)
					{
						Element = ZeroValueExpr(*Array->Inner);
					}
					else if (IsConstant(*Item) || IsPlaceholderCall(*Item))
					{
						Element = ConvertProperty(*Array->Inner, *Item, nullptr, FString::Printf(TEXT("%s[%d]"), *What, Index));
					}
					else if (Pending.bEditor)
					{
						Sink.Error(TEXT("DAB4010"), Item->Span, FString::Printf(TEXT("%s is a setting of the editor node; `%s` cannot be bound to it."), *What, *PrintValueText(*Item)));
					}
					else
					{
						// A bound element holds the zero value, so the array keeps its shape.
						AddSemanticBinding(Node, FString::Printf(TEXT("%s_%d"), *Name, Index), ConvertBinding(*Item));
					}
					Converted.Add(Element ? MoveTemp(Element) : ZeroValueExpr(*Array->Inner));
				}
				AddPropertyUnlessDefault(Node.Properties, Name, MakeListExpr(MoveTemp(Converted)), &Property, Pending.Default);
				continue;
			}

			if (IsConstant(Value) || IsPlaceholderCall(Value) || (Pending.bEditor && IsSectionRead(Value, TEXT("Props"))))
			{
				AddPropertyUnlessDefault(Node.Properties, Name, ConvertProperty(Property, Value, Pending.Default, What), &Property, Pending.Default);
			}
			else if (Pending.bEditor)
			{
				Sink.Error(TEXT("DAB4010"), Value.Span, FString::Printf(TEXT("%s is a setting of the editor node; `%s` cannot be bound to it."), *What, *PrintValueText(Value)));
			}
			else
			{
				AddSemanticBinding(Node, Name, ConvertBinding(Value));
			}
		}

		// ModifyCurve: an argument that is not a field is a curve.
		if (!Curves.IsEmpty())
		{
			const FArrayProperty* NamesProperty = CastField<FArrayProperty>(FindFProperty<FProperty>(Shape->Struct, TEXT("CurveNames")));
			const FArrayProperty* ValuesProperty = CastField<FArrayProperty>(FindFProperty<FProperty>(Shape->Struct, TEXT("CurveValues")));
			if (WrittenNames.Contains(TEXT("CurveNames")) || WrittenNames.Contains(TEXT("CurveValues")))
			{
				Sink.Error(TEXT("DAB4011"), Curves[0]->Span, TEXT("ModifyCurve takes curves either as arguments or as CurveNames / CurveValues, not both."));
			}
			else if (NamesProperty && ValuesProperty)
			{
				TArray<FDabExprPtr> CurveNames;
				TArray<FDabExprPtr> CurveValues;
				for (int32 Index = 0; Index < Curves.Num(); ++Index)
				{
					const FNodeValueArg& Curve = *Curves[Index];
					CurveNames.Add(MakeStringExpr(FName(*Curve.Name).IsNone() ? FString() : Curve.Name));
					const FDabExpr& Value = SkipParens(*Curve.Value);
					FDabExprPtr Element;
					if (IsConstant(Value) || IsPlaceholderCall(Value))
					{
						Element = ConvertProperty(*ValuesProperty->Inner, Value, nullptr, FString::Printf(TEXT("the curve %s of ModifyCurve"), *Curve.Name));
					}
					else
					{
						AddSemanticBinding(Node, FString::Printf(TEXT("CurveValues_%d"), Index), ConvertBinding(Value));
					}
					CurveValues.Add(Element ? MoveTemp(Element) : ZeroValueExpr(*ValuesProperty->Inner));
				}
				AddPropertyUnlessDefault(Node.Properties, TEXT("CurveNames"), MakeListExpr(MoveTemp(CurveNames)), NamesProperty,
					NamesProperty->ContainerPtrToValuePtr<void>(Shape->DefaultStruct));
				AddPropertyUnlessDefault(Node.Properties, TEXT("CurveValues"), MakeListExpr(MoveTemp(CurveValues)), ValuesProperty,
					ValuesProperty->ContainerPtrToValuePtr<void>(Shape->DefaultStruct));
			}
		}

		// SequenceEvaluator: an explicit frame only plays with bUseExplicitFrame on.
		if (bEvaluator && WrittenNames.Contains(TEXT("ExplicitFrame")) && !WrittenNames.Contains(TEXT("bUseExplicitFrame")))
		{
			if (const FProperty* UseFrame = FindFProperty<FProperty>(Shape->Struct, TEXT("bUseExplicitFrame")))
			{
				AddPropertyUnlessDefault(Node.Properties, UseFrame->GetName(), MakeBoolExpr(true), UseFrame, UseFrame->ContainerPtrToValuePtr<void>(Shape->DefaultStruct));
			}
		}

		// The keys of a keyed pose map.
		if (bKeyed)
		{
			const auto AddList = [this, &Node, Shape](const TCHAR* PropertyName, bool bEditorProperty, TArray<FDabExprPtr> Items)
			{
				const UStruct* Owner = bEditorProperty ? static_cast<const UStruct*>(Shape->Class) : static_cast<const UStruct*>(Shape->Struct);
				const void* Container = bEditorProperty ? static_cast<const void*>(Shape->Defaults) : Shape->DefaultStruct;
				if (const FProperty* Property = FindFProperty<FProperty>(Owner, PropertyName))
				{
					AddPropertyUnlessDefault(Node.Properties, Property->GetName(), MakeListExpr(MoveTemp(Items)), Property, Property->ContainerPtrToValuePtr<void>(Container));
				}
				else
				{
					Sink.Error(TEXT("DAB4004"), FDabSpan(), FString::Printf(TEXT("%s has no `%s` in this engine version."), *Node.ClassName, PropertyName));
				}
			};
			TArray<FDabExprPtr> Entries;
			for (const FString& Entry : KeyedEntries)
			{
				Entries.Add(MakeStringExpr(Entry));
			}
			if (bByEnum)
			{
				AddList(TEXT("VisibleEnumEntries"), true, MoveTemp(Entries));
				if (GSemRecordEnumToPoseIndex && BlendEnum)
				{
					// UAnimGraphNode_BlendListByEnum::BakeDataDuringCompilation: one entry per enumerator, the pin it selects.
					TArray<int32> PoseIndex;
					PoseIndex.AddZeroed(BlendEnum->NumEnums());
					for (int32 Position = 0; Position < KeyedEnumIndices.Num(); ++Position)
					{
						PoseIndex[KeyedEnumIndices[Position]] = Position + 1;
					}
					TArray<FDabExprPtr> Indices;
					for (const int32 Pin : PoseIndex)
					{
						Indices.Add(MakeNumberExpr(Pin));
					}
					AddList(TEXT("EnumToPoseIndex"), false, MoveTemp(Indices));
				}
			}
			else
			{
				AddList(TEXT("Tags"), false, MoveTemp(Entries));
			}
		}
		if (bByEnum)
		{
			const FProperty* BoundEnumProperty = FindFProperty<FProperty>(Shape->Class, TEXT("BoundEnum"));
			if (BlendEnum && BoundEnumProperty)
			{
				// The same form the extractor writes for the property: a native enum's `/Script/...` string, a user enum's Path.
				AddPropertyUnlessDefault(Node.Properties, BoundEnumProperty->GetName(), MakeObjectRef(BlendEnum), BoundEnumProperty,
					BoundEnumProperty->ContainerPtrToValuePtr<void>(Shape->Defaults));
			}
			else if (!BlendEnum && !bKeyed)
			{
				Sink.Error(TEXT("DAB4012"), Request.Span, TEXT("BlendListByEnum needs its enum."),
					TEXT("Write `BoundEnum = EnumName`, or bind ActiveEnumValue to a path of that enum's type."));
			}
		}

		// Constants for pins the node exposes from another class: typed by that class when it can be loaded.
		if (!CustomConstants.IsEmpty())
		{
			const FDabExpr* InstanceValue = Shape->bLinkedGraph ? FindSemanticModelProperty(Node, TEXT("InstanceClass")) : nullptr;
			const UClass* Target = InstanceValue ? LoadReferencedClass(*InstanceValue) : nullptr;
			for (const FNodeValueArg* Arg : CustomConstants)
			{
				const FProperty* TargetProperty = Target ? FindFProperty<FProperty>(Target, FName(*Arg->Name)) : nullptr;
				const FString What = FString::Printf(TEXT("%s of %s"), *Arg->Name, *TypeLabel);
				FDabExprPtr Converted = TargetProperty ? ConvertProperty(*TargetProperty, *Arg->Value, nullptr, What) : ConvertUntyped(*Arg->Value);
				AddPropertyUnlessDefault(Node.Properties, Arg->Name, MoveTemp(Converted), nullptr, nullptr);
			}
		}

		// A linked graph's Inputs must be poses its target has.
		if (InputsArg && Shape->bLinkedGraph)
		{
			TArray<FString> TargetInputs;
			if (GetLinkedInputPoses(Node, TargetInputs))
			{
				for (const FDabModelPoseLink& Link : Node.PoseInputs)
				{
					if (!TargetInputs.Contains(Link.PinName))
					{
						Sink.Warning(TEXT("DAB4013"), InputsArg->Span, FString::Printf(TEXT("The graph %s runs has no input pose `%s`."), *TypeLabel, *Link.PinName),
							TargetInputs.IsEmpty() ? FString(TEXT("It has no input poses.")) : FString::Printf(TEXT("Its input poses: %s."), *FString::Join(TargetInputs, TEXT(", "))));
					}
				}
			}
		}

		// A stack step: the node's only free pose input takes the step above it.
		if (Feed)
		{
			TArray<FString> Free;
			bool bKnown = true;
			if (Shape->bLinkedGraph)
			{
				TArray<FString> TargetInputs;
				bKnown = GetLinkedInputPoses(Node, TargetInputs);
				for (const FString& Input : TargetInputs)
				{
					if (!WrittenPoses.Contains(Input))
					{
						Free.Add(Input);
					}
				}
			}
			else
			{
				for (const FString& Pin : Shape->PosePins)
				{
					if (!WrittenPoses.Contains(Pin))
					{
						Free.Add(Pin);
					}
				}
			}
			if (!bKnown)
			{
				Sink.Error(TEXT("DAB6001"), Request.Span, FString::Printf(TEXT("Cannot tell which input pose of %s the stack feeds: its target cannot be loaded."), *TypeLabel),
					TEXT("Give its inputs in Inputs = { Name = Pose; } and use the node outside a stack."));
			}
			else if (Free.Num() != 1)
			{
				Sink.Error(TEXT("DAB6001"), Request.Span,
					FString::Printf(TEXT("%s has %d free pose inputs%s; a step of a stack needs exactly one."), *TypeLabel, Free.Num(),
						Free.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *FString::Join(Free, TEXT(", ")))),
					TEXT("Give the other inputs as arguments, or write the node outside the stack."));
			}
			else if (!Feed->IsEmpty())
			{
				Node.PoseInputs.Add({Free[0], *Feed});
			}
		}

		const FString Id = Node.Id;
		Scope.Graph->Nodes.Add(MoveTemp(Node));
		return Id;
	}

	// ------------------------------------------------------------------------------------------
	// Metadata onto engine properties
	// ------------------------------------------------------------------------------------------

	bool FSemantic::FindMetaProperty(TConstArrayView<FMetaTarget> Targets, const FString& Key, const FProperty*& OutProperty, const void*& OutDefault) const
	{
		const FName PropertyName(*Key);
		for (const FMetaTarget& Target : Targets)
		{
			if (!Target.Owner || (Target.bRuntimeStruct && Key == TEXT("Tag")))
			{
				continue;
			}
			const FProperty* Property = FindFProperty<FProperty>(Target.Owner, PropertyName);
			if (Property && (!Target.Accepts || Target.Accepts(*Property)))
			{
				OutProperty = Property;
				OutDefault = Target.DefaultContainer ? Property->ContainerPtrToValuePtr<void>(Target.DefaultContainer) : nullptr;
				return true;
			}
		}
		return false;
	}

	void FSemantic::ReadMetaValue(const FProperty& Property, const FDabExpr* Value, const FDabSpan& Span, const void* Default,
		TArray<FDabModelProperty>& Out, const FString& What)
	{
		if (!Value)
		{
			// A flag sets a bool.
			if (CastField<FBoolProperty>(&Property))
			{
				AddPropertyUnlessDefault(Out, Property.GetName(), MakeBoolExpr(true), &Property, Default);
			}
			else
			{
				Sink.Error(TEXT("DAB4006"), Span, FString::Printf(TEXT("%s needs a value: `%s = ...`."), *What, *Property.GetName()));
			}
			return;
		}
		const FDabExpr& Written = SkipParens(*Value);
		if (!IsConstant(Written) && !IsPlaceholderCall(Written) && !IsSectionRead(Written, TEXT("Props")))
		{
			Sink.Error(TEXT("DAB4010"), Written.Span,
				FString::Printf(TEXT("%s is fixed when the Blueprint compiles; `%s` is read at run time."), *What, *PrintValueText(Written)),
				TEXT("Write a constant, or Props.X for a tuning value's default."));
			return;
		}
		AddPropertyUnlessDefault(Out, Property.GetName(), ConvertProperty(Property, Written, Default, What), &Property, Default);
	}

	void FSemantic::ReadMetaProperties(const FDabMeta* Meta, TConstArrayView<FMetaTarget> Targets, TArray<FDabModelProperty>& Out, const FString& What)
	{
		if (!Meta)
		{
			return;
		}
		TSet<FString> Seen;
		for (const TUniquePtr<FDabMetaEntry>& Entry : Meta->Entries)
		{
			if (!Entry)
			{
				continue;
			}
			const FProperty* Property = nullptr;
			const void* Default = nullptr;
			if (!FindMetaProperty(Targets, Entry->Key, Property, Default))
			{
				Sink.Error(TEXT("DAB4005"), Entry->KeySpan, FString::Printf(TEXT("%s has no setting `%s`."), *What, *Entry->Key));
				continue;
			}
			if (Seen.Contains(Property->GetName()))
			{
				Sink.Error(TEXT("DAB4011"), Entry->KeySpan, FString::Printf(TEXT("%s gives `%s` twice."), *What, *Property->GetName()));
				continue;
			}
			Seen.Add(Property->GetName());
			ReadMetaValue(*Property, Entry->Value.Get(), Entry->KeySpan, Default, Out, FString::Printf(TEXT("%s of %s"), *Entry->Key, *What));
		}
	}

	/** The runtime struct and the editor node of a node class, as metadata targets: what the extractor reads off such a node. */
	void AddSemanticNodeTargets(const FNodeShape& Shape, TArray<FMetaTarget>& Out)
	{
		FMetaTarget Runtime;
		Runtime.Owner = Shape.Struct;
		Runtime.DefaultContainer = Shape.DefaultStruct;
		Runtime.bRuntimeStruct = true;
		Runtime.Accepts = [](const FProperty& Property) { return IsRecordedNodeField(Property); };
		Out.Add(MoveTemp(Runtime));

		FMetaTarget Editor;
		Editor.Owner = Shape.Class;
		Editor.DefaultContainer = Shape.Defaults;
		const FStructProperty* NodeProperty = Shape.NodeProperty;
		Editor.Accepts = [NodeProperty](const FProperty& Property) { return IsEditorNodeArgument(Property, NodeProperty); };
		Out.Add(MoveTemp(Editor));
	}

	void FSemantic::ReadResultMeta(const FDabMeta* Meta, FDabModelGraph& Graph, const FString& What)
	{
		if (!Meta)
		{
			return;
		}
		TArray<FMetaTarget> Targets;
		if (const FNodeShape* Shape = GetShape(UAnimGraphNode_Root::StaticClass()))
		{
			AddSemanticNodeTargets(*Shape, Targets);
		}
		// A linked graph's blend options live on the graph; the extractor keeps them with the result.
		FMetaTarget GraphTarget;
		GraphTarget.Owner = UAnimationGraph::StaticClass();
		GraphTarget.DefaultContainer = GetDefault<UAnimationGraph>();
		GraphTarget.Accepts = [](const FProperty& Property) { return Property.GetFName() == FName(TEXT("BlendOptions")); };
		Targets.Add(MoveTemp(GraphTarget));
		ReadMetaProperties(Meta, Targets, Graph.ResultProperties, What);

		// The shared group is the default whether it is spelled as the engine's DefaultSharedGroup (dropped
		// above with the other defaults) or as None, which assets saved before layer groups existed hold.
		// The extractor records neither; any other group, "Default Shared Group" included, is a real group.
		const FString SharedGroup = FAnimNode_Root::DefaultSharedGroup.ToString();
		Graph.ResultProperties.RemoveAll([&SharedGroup](const FDabModelProperty& Property)
		{
			if (Property.Name != TEXT("LayerGroup") || !Property.Value)
			{
				return false;
			}
			const FDabLiteralExpr* Literal = Property.Value->As<FDabLiteralExpr>();
			return Literal && Literal->LiteralKind == EDabLiteralKind::String && (Literal->Text.IsEmpty() || Literal->Text == SharedGroup);
		});
	}

	// ------------------------------------------------------------------------------------------
	// State machines
	// ------------------------------------------------------------------------------------------

	const FDabExpr* FSemantic::ResolveRule(FRuleSet& Rules, const FString& Name, const FDabSpan& UseSpan)
	{
		if (const FDabExprPtr* Done = Rules.Resolved.Find(Name))
		{
			return Done->Get();
		}
		const FDabAssignStmt* const* Statement = Rules.Statements.Find(Name);
		if (!Statement || !(*Statement)->Value)
		{
			return nullptr;
		}
		if (Rules.Resolving.Contains(Name))
		{
			Sink.Error(TEXT("DAB6013"), UseSpan, FString::Printf(TEXT("The rule `%s` uses itself."), *Name));
			return nullptr;
		}
		Rules.Resolving.Add(Name);
		FDabExprPtr Value = Resolve(*(*Statement)->Value, &Rules);
		Rules.Resolving.Remove(Name);
		if (!Value)
		{
			return nullptr;
		}
		const FDabExpr* Result = Value.Get();
		Rules.Resolved.Add(Name, MoveTemp(Value));
		return Result;
	}

	void FSemantic::BuildStateMachine(const FDabNamedBlockStmt& Stmt)
	{
		FDabModelStateMachine Machine;
		Machine.Name = Stmt.Name;
		const FString Location = FString::Printf(TEXT("StateMachine %s"), *Stmt.Name);

		// The machine node's settings: its runtime struct first, then the editor node (Tag).
		if (const FNodeShape* Shape = GetShape(FindNodeClassByName(TEXT("AnimGraphNode_StateMachine"))))
		{
			TArray<FMetaTarget> Targets;
			AddSemanticNodeTargets(*Shape, Targets);
			ReadMetaProperties(Stmt.Meta.Get(), Targets, Machine.Properties, Location);
		}

		if (!Stmt.Body)
		{
			Sink.Error(TEXT("DAB6011"), Stmt.NameSpan, FString::Printf(TEXT("%s is written as a block: StateMachine %s { Entry = ...; State ... }."), *Location, *Stmt.Name));
			Model.StateMachines.Add(MoveTemp(Machine));
			return;
		}

		enum EMember : int32 { MemberState, MemberConduit, MemberAlias };
		TMap<FString, int32> Members;
		TArray<const FDabNamedBlockStmt*> StateStmts;
		TArray<const FDabNamedBlockStmt*> ConduitStmts;
		TArray<const FDabNamedBlockStmt*> AliasStmts;
		const FDabAssignStmt* EntryStmt = nullptr;
		const FDabAssignStmt* RulesStmt = nullptr;
		const FDabAssignStmt* TransitionsStmt = nullptr;

		for (const FDabStmtPtr& Statement : Stmt.Body->Statements)
		{
			if (!Statement || Statement->Is<FDabRegionStmt>())
			{
				continue;
			}
			if (const FDabNamedBlockStmt* Named = Statement->As<FDabNamedBlockStmt>())
			{
				int32 MemberKind = INDEX_NONE;
				if (Named->Word == TEXT("State"))
				{
					MemberKind = MemberState;
					StateStmts.Add(Named);
				}
				else if (Named->Word == TEXT("Conduit"))
				{
					MemberKind = MemberConduit;
					ConduitStmts.Add(Named);
				}
				else if (Named->Word == TEXT("Alias"))
				{
					MemberKind = MemberAlias;
					AliasStmts.Add(Named);
				}
				else if (Named->Word != TEXT("Template"))
				{
					Sink.Error(TEXT("DAB6011"), Named->WordSpan, FString::Printf(TEXT("`%s` cannot be written in a state machine."), *Named->Word),
						TEXT("A state machine holds Entry, State, Conduit, Alias, Rules and Transitions."));
				}
				if (MemberKind != INDEX_NONE)
				{
					if (Members.Contains(Named->Name))
					{
						Sink.Error(TEXT("DAB6008"), Named->NameSpan, FString::Printf(TEXT("%s has two members named `%s`."), *Location, *Named->Name));
					}
					else
					{
						Members.Add(Named->Name, MemberKind);
					}
				}
				continue;
			}
			const FDabAssignStmt* Assign = Statement->As<FDabAssignStmt>();
			const FDabAssignStmt** Slot = !Assign ? nullptr
				: Assign->Name == TEXT("Entry") ? &EntryStmt
				: Assign->Name == TEXT("Rules") ? &RulesStmt
				: Assign->Name == TEXT("Transitions") ? &TransitionsStmt
				: nullptr;
			if (!Slot)
			{
				Sink.Error(TEXT("DAB6011"), Statement->Span, FString::Printf(TEXT("%s holds Entry, State, Conduit, Alias, Rules and Transitions; this statement is none of them."), *Location));
				continue;
			}
			if (*Slot)
			{
				Sink.Error(TEXT("DAB6008"), Assign->NameSpan, FString::Printf(TEXT("%s gives `%s` twice."), *Location, *Assign->Name));
				continue;
			}
			*Slot = Assign;
		}

		// Rules first: conduits and transitions name them.
		FRuleSet Rules;
		if (RulesStmt)
		{
			const FDabBlockExpr* Block = RulesStmt->Value ? SkipParens(*RulesStmt->Value).As<FDabBlockExpr>() : nullptr;
			if (!Block)
			{
				Sink.Error(TEXT("DAB6011"), RulesStmt->NameSpan, TEXT("Rules is a block: Rules = { Name = Condition; }."));
			}
			else
			{
				for (const FDabStmtPtr& Statement : Block->Statements)
				{
					if (!Statement || Statement->Is<FDabRegionStmt>())
					{
						continue;
					}
					const FDabAssignStmt* Rule = Statement->As<FDabAssignStmt>();
					if (!Rule)
					{
						Sink.Error(TEXT("DAB6011"), Statement->Span, TEXT("Each rule is `Name = Condition;`."));
						continue;
					}
					if (Rules.Statements.Contains(Rule->Name))
					{
						Sink.Error(TEXT("DAB6008"), Rule->NameSpan, FString::Printf(TEXT("%s has two rules named `%s`."), *Location, *Rule->Name));
						continue;
					}
					Rules.Statements.Add(Rule->Name, Rule);
				}
				for (const TPair<FString, const FDabAssignStmt*>& Rule : Rules.Statements)
				{
					ResolveRule(Rules, Rule.Key, Rule.Value->NameSpan);
				}
			}
		}

		if (!EntryStmt)
		{
			Sink.Error(TEXT("DAB6007"), Stmt.NameSpan, FString::Printf(TEXT("%s has no `Entry = State;`."), *Location));
		}
		else
		{
			const FDabNameExpr* Entry = EntryStmt->Value ? SkipParens(*EntryStmt->Value).As<FDabNameExpr>() : nullptr;
			const int32* EntryKind = Entry ? Members.Find(Entry->Name) : nullptr;
			if (!Entry || !EntryKind || *EntryKind == MemberAlias)
			{
				Sink.Error(TEXT("DAB6006"), EntryStmt->Value ? EntryStmt->Value->Span : EntryStmt->NameSpan,
					FString::Printf(TEXT("The entry of %s is a state or conduit of the machine; `%s` is not one."), *Location,
						EntryStmt->Value ? *PrintValueText(*EntryStmt->Value) : TEXT("")));
			}
			else
			{
				Machine.Entry = Entry->Name;
			}
		}

		for (const FDabNamedBlockStmt* StateStmt : StateStmts)
		{
			FDabModelState State;
			BuildState(*StateStmt, Machine.Name, State);
			Machine.States.Add(MoveTemp(State));
		}

		for (const FDabNamedBlockStmt* ConduitStmt : ConduitStmts)
		{
			FDabModelConduit Conduit;
			Conduit.Name = ConduitStmt->Name;
			if (ConduitStmt->Body)
			{
				Sink.Error(TEXT("DAB6011"), ConduitStmt->NameSpan, TEXT("A conduit is `Conduit Name = Condition;` or `Conduit Name;`."));
			}
			else if (ConduitStmt->Value)
			{
				FDabExprPtr Condition = Resolve(*ConduitStmt->Value, &Rules);
				const FDabLiteralExpr* Literal = Condition ? Condition->As<FDabLiteralExpr>() : nullptr;
				// `true` is what no condition means.
				if (!(Literal && Literal->LiteralKind == EDabLiteralKind::Bool && Literal->bBool))
				{
					Conduit.Condition = MoveTemp(Condition);
				}
			}
			Machine.Conduits.Add(MoveTemp(Conduit));
		}

		for (const FDabNamedBlockStmt* AliasStmt : AliasStmts)
		{
			FDabModelAlias Alias;
			Alias.Name = AliasStmt->Name;
			const FDabExpr* Value = AliasStmt->Value ? &SkipParens(*AliasStmt->Value) : nullptr;
			if (const FDabNameExpr* All = Value ? Value->As<FDabNameExpr>() : nullptr; All && All->Name == TEXT("All"))
			{
				Alias.bGlobal = true;
			}
			else if (const FDabListExpr* List = Value ? Value->As<FDabListExpr>() : nullptr)
			{
				for (const FDabExprPtr& Item : List->Items)
				{
					const FDabNameExpr* Aliased = Item ? SkipParens(*Item).As<FDabNameExpr>() : nullptr;
					const int32* AliasedKind = Aliased ? Members.Find(Aliased->Name) : nullptr;
					if (!Aliased || !AliasedKind || *AliasedKind == MemberAlias)
					{
						Sink.Error(TEXT("DAB6006"), Item ? Item->Span : AliasStmt->NameSpan,
							FString::Printf(TEXT("An alias lists states of %s; `%s` is not one."), *Location, Item ? *PrintValueText(*Item) : TEXT("")));
						continue;
					}
					Alias.States.Add(Aliased->Name);
				}
			}
			else
			{
				Sink.Error(TEXT("DAB6011"), AliasStmt->NameSpan, TEXT("An alias is `Alias Name = [ State, ... ];` or `Alias Name = All;`."));
			}
			Machine.Aliases.Add(MoveTemp(Alias));
		}

		if (TransitionsStmt)
		{
			const FDabBlockExpr* Block = TransitionsStmt->Value ? SkipParens(*TransitionsStmt->Value).As<FDabBlockExpr>() : nullptr;
			if (!Block)
			{
				Sink.Error(TEXT("DAB6011"), TransitionsStmt->NameSpan, TEXT("Transitions is a block: Transitions = { From -> To : Condition; }."));
			}
			else
			{
				ReadTransitions(*Block, Machine.Name, Members, Rules, Machine);
			}
		}

		Model.StateMachines.Add(MoveTemp(Machine));
	}

	void FSemantic::BuildState(const FDabNamedBlockStmt& Stmt, const FString& MachineName, FDabModelState& OutState)
	{
		OutState.Name = Stmt.Name;
		OutState.Graph.Name = Stmt.Name;
		const FString Location = FString::Printf(TEXT("state %s of StateMachine %s"), *Stmt.Name, *MachineName);

		FMetaTarget StateTarget;
		StateTarget.Owner = UAnimStateNode::StaticClass();
		StateTarget.DefaultContainer = GetDefault<UAnimStateNode>();
		StateTarget.Accepts = [](const FProperty& Property)
		{
			const UClass* Owner = Property.GetOwnerClass();
			const FName Name = Property.GetFName();
			return Owner && Owner->IsChildOf(UAnimStateNode::StaticClass()) && Property.HasAnyPropertyFlags(CPF_Edit)
				&& !Property.HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)
				&& Name != FName(TEXT("BoundGraph")) && Name != FName(TEXT("StateType"));
		};
		ReadMetaProperties(Stmt.Meta.Get(), MakeArrayView(&StateTarget, 1), OutState.Properties, Location);

		static const TSet<FString> StateReserved = {TEXT("OnEnter"), TEXT("OnExit"), TEXT("OnBlended")};
		if (Stmt.Body)
		{
			for (const FDabStmtPtr& Statement : Stmt.Body->Statements)
			{
				const FDabAssignStmt* Assign = Statement ? Statement->As<FDabAssignStmt>() : nullptr;
				if (!Assign || !StateReserved.Contains(Assign->Name) || !Assign->Value)
				{
					continue;
				}
				TOptional<FDabModelCall>& Slot = Assign->Name == TEXT("OnEnter") ? OutState.OnEnter
					: Assign->Name == TEXT("OnExit") ? OutState.OnExit : OutState.OnBlended;
				if (Slot.IsSet())
				{
					Sink.Error(TEXT("DAB6008"), Assign->NameSpan, FString::Printf(TEXT("%s gives `%s` twice."), *Location, *Assign->Name));
					continue;
				}
				FDabModelCall Call;
				if (ConvertCall(*Assign->Value, Call, FString::Printf(TEXT("%s of %s"), *Assign->Name, *Location)))
				{
					Slot = MoveTemp(Call);
				}
			}
			BuildGraph(Stmt.Body->Statements, OutState.Graph, Location, TEXT("Pose"), StateReserved, Stmt.NameSpan);
		}
		else if (Stmt.Value)
		{
			BuildValueGraph(*Stmt.Value, OutState.Graph, Location);
		}
		else
		{
			Sink.Error(TEXT("DAB6012"), Stmt.NameSpan, FString::Printf(TEXT("%s has no `Pose = ...;`."), *Location),
				TEXT("Write `State Name { Pose = ...; }`."));
		}
	}

	void FSemantic::ReadTransitions(const FDabBlockExpr& Block, const FString& MachineName, const TMap<FString, int32>& Members, FRuleSet& Rules,
		FDabModelStateMachine& Machine)
	{
		for (const FDabStmtPtr& Statement : Block.Statements)
		{
			if (!Statement || Statement->Is<FDabRegionStmt>())
			{
				continue;
			}
			const FDabTransitionStmt* Row = Statement->As<FDabTransitionStmt>();
			if (!Row)
			{
				Sink.Error(TEXT("DAB6011"), Statement->Span, TEXT("Each transition is `From -> To : Condition [ options ];`."));
				continue;
			}

			FDabModelTransition Transition;
			Transition.From = Row->From;
			Transition.To = Row->To;
			const FString What = FString::Printf(TEXT("the transition %s -> %s of StateMachine %s"), *Row->From, *Row->To, *MachineName);
			if (!Members.Contains(Row->From))
			{
				Sink.Error(TEXT("DAB6006"), Row->FromSpan, FString::Printf(TEXT("StateMachine %s has no state, conduit or alias `%s`."), *MachineName, *Row->From));
			}
			const int32* ToKind = Members.Find(Row->To);
			if (!ToKind)
			{
				Sink.Error(TEXT("DAB6006"), Row->ToSpan, FString::Printf(TEXT("StateMachine %s has no state or conduit `%s`."), *MachineName, *Row->To));
			}
			else if (*ToKind == 2)
			{
				Sink.Error(TEXT("DAB6006"), Row->ToSpan, FString::Printf(TEXT("`%s` is an alias; a transition leads to a state or a conduit."), *Row->To));
			}

			if (Row->Condition)
			{
				const FDabExpr& Condition = SkipParens(*Row->Condition);
				const FDabNameExpr* Name = Condition.As<FDabNameExpr>();
				if (Name && Name->Name == TEXT("Auto"))
				{
					Transition.ConditionKind = EDabConditionKind::Auto;
				}
				else if (Name && Rules.Statements.Contains(Name->Name))
				{
					// A rule alone is the engine's shared rule.
					Transition.SharedRuleName = Name->Name;
					if (const FDabExpr* Rule = ResolveRule(Rules, Name->Name, Name->Span))
					{
						Transition.Condition = CloneExpr(*Rule);
					}
				}
				else
				{
					Transition.Condition = Resolve(Condition, &Rules);
				}
			}
			ReadTransitionOptions(Row->Meta.Get(), Transition, What);
			Machine.Transitions.Add(MoveTemp(Transition));
		}
	}

	void FSemantic::ReadTransitionOptions(const FDabMeta* Meta, FDabModelTransition& Transition, const FString& What)
	{
		if (!Meta)
		{
			return;
		}
		const UClass* NodeClass = UAnimStateTransitionNode::StaticClass();
		const UObject* NodeDefaults = GetDefault<UAnimStateTransitionNode>();
		TSet<FString> Seen;
		const auto Apply = [this, NodeClass, NodeDefaults, &Seen, &Transition, &What](const TCHAR* PropertyName, const FDabExpr* Value, const FDabSpan& Span)
		{
			const FProperty* Property = FindFProperty<FProperty>(NodeClass, PropertyName);
			if (!Property)
			{
				Sink.Error(TEXT("DAB4005"), Span, FString::Printf(TEXT("The transition node has no `%s` in this engine version."), PropertyName));
				return;
			}
			if (Seen.Contains(Property->GetName()))
			{
				Sink.Error(TEXT("DAB4011"), Span, FString::Printf(TEXT("%s sets `%s` twice."), *What, *Property->GetName()));
				return;
			}
			Seen.Add(Property->GetName());
			ReadMetaValue(*Property, Value, Span, Property->ContainerPtrToValuePtr<void>(NodeDefaults), Transition.Options,
				FString::Printf(TEXT("%s of %s"), PropertyName, *What));
		};

		for (const TUniquePtr<FDabMetaEntry>& Entry : Meta->Entries)
		{
			if (!Entry)
			{
				continue;
			}
			const FString& Key = Entry->Key;
			if (Key == TEXT("Blend") || Key == TEXT("Curve") || Key == TEXT("Priority"))
			{
				if (!Entry->Value)
				{
					Sink.Error(TEXT("DAB4006"), Entry->KeySpan, FString::Printf(TEXT("`%s` needs a value."), *Key));
					continue;
				}
				Apply(Key == TEXT("Blend") ? TEXT("CrossfadeDuration") : Key == TEXT("Curve") ? TEXT("BlendMode") : TEXT("PriorityOrder"), Entry->Value.Get(), Entry->KeySpan);
			}
			else if (Key == TEXT("Inertialize"))
			{
				const FDabExprPtr Logic = MakeNameExpr(TEXT("TLT_Inertialization"));
				Apply(TEXT("LogicType"), Logic.Get(), Entry->KeySpan);
				if (Entry->Value)
				{
					Apply(TEXT("CrossfadeDuration"), Entry->Value.Get(), Entry->KeySpan);
				}
			}
			else if (Key == TEXT("BlendProfile"))
			{
				// The profile sits in a wrapper with private fields; the option is the profile itself, as the extractor records it.
				if (!Entry->Value || Seen.Contains(Key))
				{
					Sink.Error(TEXT("DAB4011"), Entry->KeySpan, FString::Printf(TEXT("%s: BlendProfile needs one value."), *What));
					continue;
				}
				Seen.Add(Key);
				FSemType ProfileType;
				ProfileType.Kind = ESemTypeKind::Object;
				ProfileType.Class = UBlendProfile::StaticClass();
				FDabExprPtr Profile = ConvertTyped(ProfileType, *Entry->Value, nullptr, FString::Printf(TEXT("BlendProfile of %s"), *What));
				const FDabNameExpr* None = Profile ? Profile->As<FDabNameExpr>() : nullptr;
				if (Profile && !(None && None->Name == TEXT("None")))
				{
					AddPropertyUnlessDefault(Transition.Options, Key, MoveTemp(Profile), nullptr, nullptr);
				}
			}
			else
			{
				FMetaTarget Target;
				Target.Owner = NodeClass;
				Target.DefaultContainer = NodeDefaults;
				Target.Accepts = [](const FProperty& Property)
				{
					const UClass* Owner = Property.GetOwnerClass();
					return Owner && Owner->IsChildOf(UAnimStateTransitionNode::StaticClass()) && Property.HasAnyPropertyFlags(CPF_Edit)
						&& !Property.HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)
						&& Property.GetFName() != FName(TEXT("bAutomaticRuleBasedOnSequencePlayerInState"));
				};
				const FProperty* Property = nullptr;
				const void* Default = nullptr;
				if (!FindMetaProperty(MakeArrayView(&Target, 1), Key, Property, Default))
				{
					Sink.Error(TEXT("DAB4005"), Entry->KeySpan, FString::Printf(TEXT("A transition has no option `%s`."), *Key),
						TEXT("Options are Blend, Curve, Inertialize, Priority, BlendProfile and the transition node's settings (Bidirectional, ...)."));
					continue;
				}
				if (Seen.Contains(Property->GetName()))
				{
					Sink.Error(TEXT("DAB4011"), Entry->KeySpan, FString::Printf(TEXT("%s sets `%s` twice."), *What, *Property->GetName()));
					continue;
				}
				Seen.Add(Property->GetName());
				ReadMetaValue(*Property, Entry->Value.Get(), Entry->KeySpan, Default, Transition.Options, FString::Printf(TEXT("%s of %s"), *Key, *What));
			}
		}
	}

	void FSemantic::CheckMachineUses()
	{
		for (const FString& Name : MachineOrder)
		{
			if (MachineUses.FindRef(Name) == 0)
			{
				const FDabNamedBlockStmt* const* Stmt = MachineStmts.Find(Name);
				Sink.Error(TEXT("DAB6002"), Stmt ? (*Stmt)->NameSpan : FDabSpan(), FString::Printf(TEXT("The state machine `%s` is never used."), *Name),
					TEXT("Name it as a pose where it runs: `Output = Name;` or in a state's `Pose = Name;`."));
			}
		}
	}
}
