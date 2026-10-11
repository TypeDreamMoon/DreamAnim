// Copyright DreamMoon. All Rights Reserved.
//
// The generator's entry: find or create the asset, empty it (rebuilding in place keeps the asset object
// and its generated class, so references to them survive), then fill it in an order where everything a
// step refers to already exists:
//
//   settings, variables, interfaces, layer graphs
//   -> every pose graph's nodes (machines append their states' graphs as they are built)
//   -> pose links and cached poses
//   -> bindings, conditions, node and state functions (generated functions are made on first use)
//   -> the EventGraph (notifies, animation set glue)
//   -> GUIDs for whatever no step named, compile, save.

#include "Generate/DabGenerate.h"

#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNodeBase.h"
#include "AnimationStateMachineGraph.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimLayerInterface.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"
#include "Animation/Skeleton.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DreamAnimLibrary.h"
#include "DreamAnimSet.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Factories/AnimBlueprintFactory.h"
#include "GameFramework/Actor.h"
#include "Generate/DabGenerateInternal.h"
#include "K2Node_AnimGetter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphToken.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/UObjectToken.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Compiler::GeneratePrivate
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	// ------------------------------------------------------------------------------------------
	// Context
	// ------------------------------------------------------------------------------------------

	FDabGenContext::FDabGenContext(const FDabModel& InModel, FDabDiagnosticSink& InSink, UAnimBlueprint* InBlueprint)
		: Model(InModel)
		, Sink(InSink)
		, Blueprint(InBlueprint)
	{
	}

	FGuid FDabGenContext::MakeGuid(const FString& Key) const
	{
		return DabGenHashGuid(Model.AssetPath + TEXT("/") + Key);
	}

	FGuid DabGenHashGuid(const FString& Full)
	{
		const FTCHARToUTF8 Utf8(*Full);
		FMD5 Md5;
		Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		uint8 Digest[16];
		Md5.Final(Digest);
		uint32 Parts[4];
		FMemory::Memcpy(Parts, Digest, sizeof(Parts));
		FGuid Guid(Parts[0], Parts[1], Parts[2], Parts[3]);
		if (!Guid.IsValid())
		{
			Guid.D = 1;
		}
		return Guid;
	}

	void FDabGenContext::AssignGuid(UEdGraphNode* Node, const FString& GraphPath, const FString& Id)
	{
		if (!Node)
		{
			return;
		}
		if (GuidNodes.Contains(Node))
		{
			UsedGuids.Remove(Node->NodeGuid);
		}
		// The engine replaces duplicate GUIDs on load, so a collision gets a salt instead.
		const FString Place = GraphPath + TEXT("/") + Id;
		FString Key = Place;
		FGuid Guid = MakeGuid(Key);
		while (UsedGuids.Contains(Guid))
		{
			Key += TEXT("#");
			Guid = MakeGuid(Key);
		}
		Node->NodeGuid = Guid;
		UsedGuids.Add(Guid);
		GuidNodes.Add(Node);
		GuidPlaces.Add(Guid, Place);
	}

	void FDabGenContext::AssignRemainingGuids()
	{
		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		for (const UEdGraph* Graph : AllGraphs)
		{
			if (!Graph)
			{
				continue;
			}
			// A graph inside a node is known by that node, whose GUID came first (parents precede their subgraphs).
			FString GraphKey = Graph->GetName();
			if (const UEdGraphNode* Owner = Cast<UEdGraphNode>(Graph->GetOuter()))
			{
				GraphKey = Owner->NodeGuid.ToString() + TEXT("/") + GraphKey;
			}
			int32 Ordinal = 0;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && !GuidNodes.Contains(Node))
				{
					AssignGuid(Node, TEXT("#Graph/") + GraphKey, FString::Printf(TEXT("#auto%d"), Ordinal++));
				}
			}
		}
	}

	UClass* FDabGenContext::GetSkeletonClass() const
	{
		if (!Blueprint)
		{
			return nullptr;
		}
		return Blueprint->SkeletonGeneratedClass.Get() ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->GeneratedClass.Get();
	}

	FName FDabGenContext::GetBlueprintAssetName() const
	{
		return Blueprint ? Blueprint->GetFName() : NAME_None;
	}

	FDabGenMachine* FDabGenContext::FindMachine(const FString& Name) const
	{
		const TUniquePtr<FDabGenMachine>* Found = Machines.Find(Name);
		return Found ? Found->Get() : nullptr;
	}

	// ------------------------------------------------------------------------------------------
	// Asset
	// ------------------------------------------------------------------------------------------

	static UEdGraph* DabGenFindFunctionGraph(const UBlueprint* Blueprint, FName Name)
	{
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() == Name)
			{
				return Graph;
			}
		}
		return nullptr;
	}

	static UAnimGraphNode_Root* DabGenFindRoot(const UEdGraph* Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UAnimGraphNode_Root* Root = Cast<UAnimGraphNode_Root>(Node))
			{
				return Root;
			}
		}
		return nullptr;
	}

	/** The parent class and skeleton the model names; false (reported) when they cannot be found. */
	static bool DabGenResolveParentAndSkeleton(const FDabModel& Model, FDabDiagnosticSink& Sink, UClass*& OutParent, USkeleton*& OutSkeleton)
	{
		OutParent = nullptr;
		OutSkeleton = nullptr;
		if (Model.Kind == EDabDocumentKind::AnimBlueprint)
		{
			OutParent = Model.ParentClass.IsEmpty() ? UAnimInstance::StaticClass() : DabGenLoadClass(Model.ParentClass);
			// The factory would open a message box for a bad parent; check first.
			if (!OutParent || !OutParent->IsChildOf(UAnimInstance::StaticClass()) || !FKismetEditorUtilities::CanCreateBlueprintOfClass(OutParent))
			{
				Sink.Error(TEXT("DAB7004"), FDabSpan(), FString::Printf(TEXT("Settings: %s is not an animation instance class a Blueprint can derive from"), *Model.ParentClass));
				return false;
			}
		}
		if (!Model.Skeleton.IsEmpty())
		{
			OutSkeleton = Cast<USkeleton>(DabGenLoadObject(Model.Skeleton));
			if (!OutSkeleton)
			{
				Sink.Error(TEXT("DAB7004"), FDabSpan(), FString::Printf(TEXT("Settings: %s is not a skeleton"), *Model.Skeleton));
				return false;
			}
		}
		return true;
	}

	static UAnimBlueprint* DabGenFindOrCreateAsset(const FDabModel& Model, FDabDiagnosticSink& Sink, const FDabGenerateOptions& Options, UClass* ParentClass,
		USkeleton* Skeleton, bool bPreview)
	{
		FString PackageName;
		FString AssetName;
		if (!Options.OverridePackageName.IsEmpty())
		{
			PackageName = Options.OverridePackageName;
			AssetName = FPackageName::GetShortName(PackageName);
		}
		else if (!Model.AssetPath.Split(TEXT("."), &PackageName, &AssetName))
		{
			PackageName = Model.AssetPath;
			AssetName = FPackageName::GetShortName(PackageName);
		}
		if (!FPackageName::IsValidLongPackageName(PackageName, bPreview) || AssetName.IsEmpty())
		{
			Sink.Error(TEXT("DAB7006"), FDabSpan(), FString::Printf(TEXT("%s is not a valid asset path"), *PackageName));
			return nullptr;
		}

		const bool bInterface = Model.Kind == EDabDocumentKind::AnimLayerInterface;
		const FString ObjectPath = PackageName + TEXT(".") + AssetName;
		UObject* Existing = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath);
		if (!Existing && FPackageName::DoesPackageExist(PackageName))
		{
			LoadPackage(nullptr, *PackageName, LOAD_NoWarn | LOAD_Quiet);
			Existing = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath);
		}
		if (Existing)
		{
			UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(Existing);
			if (!Blueprint || (Blueprint->BlueprintType == BPTYPE_Interface) != bInterface)
			{
				Sink.Error(TEXT("DAB7006"), FDabSpan(), FString::Printf(TEXT("%s already exists as a %s, not an %s"), *ObjectPath,
					*Existing->GetClass()->GetName(), bInterface ? TEXT("Animation Layer Interface") : TEXT("Animation Blueprint")));
				return nullptr;
			}
			// Rebuilt in place: the parent and skeleton may have changed.
			if (!bInterface && ParentClass && Blueprint->ParentClass.Get() != ParentClass)
			{
				Blueprint->ParentClass = ParentClass;
			}
			Blueprint->TargetSkeleton = Skeleton;
			return Blueprint;
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			Sink.Error(TEXT("DAB7006"), FDabSpan(), FString::Printf(TEXT("%s: the package cannot be created"), *PackageName));
			return nullptr;
		}
		// Never ConfigureProperties: it opens a dialog.
		UAnimBlueprintFactory* Factory = bInterface ? NewObject<UAnimLayerInterfaceFactory>() : NewObject<UAnimBlueprintFactory>();
		Factory->BlueprintType = bInterface ? BPTYPE_Interface : BPTYPE_Normal;
		Factory->ParentClass = ParentClass;
		Factory->TargetSkeleton = Skeleton;
		Factory->bTemplate = !bInterface && Skeleton == nullptr;
		UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), Package, FName(*AssetName),
			bPreview ? RF_Transient : RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
		if (!Blueprint)
		{
			Sink.Error(TEXT("DAB7006"), FDabSpan(), FString::Printf(TEXT("%s: the asset cannot be created"), *ObjectPath));
			return nullptr;
		}
		if (!bPreview)
		{
			FAssetRegistryModule::AssetCreated(Blueprint);
			Package->MarkPackageDirty();
		}
		return Blueprint;
	}

	/** Empties a Blueprint for regeneration, keeping the asset, its generated class and the AnimGraph's root. */
	static void DabGenClearBlueprint(UAnimBlueprint* Blueprint)
	{
		// Anim getters listen to the Blueprint's graph renames until they are destroyed, and removed nodes are only
		// destroyed by the next garbage collection: the old ones would answer this build's renames. They can only
		// stop listening while they still belong to the Blueprint, so they go before their graphs do.
		TArray<UK2Node_AnimGetter*> OldGetters;
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, OldGetters);
		for (UK2Node_AnimGetter* Getter : OldGetters)
		{
			if (Getter && !Getter->HasAnyFlags(RF_BeginDestroyed))
			{
				Getter->ConditionalBeginDestroy();
			}
		}

		// Function graphs (self layers, generated functions) except the AnimGraph.
		TArray<UEdGraph*> Graphs;
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() != UEdGraphSchema_K2::GN_AnimGraph)
			{
				Graphs.Add(Graph);
			}
		}
		for (UEdGraph* Graph : Graphs)
		{
			FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::MarkTransient);
		}

		// Implemented interfaces and their layer graphs.
		for (FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
		{
			TArray<UEdGraph*> InterfaceGraphs;
			for (UEdGraph* Graph : Description.Graphs)
			{
				if (Graph)
				{
					InterfaceGraphs.Add(Graph);
				}
			}
			for (UEdGraph* Graph : InterfaceGraphs)
			{
				FBlueprintEditorUtils::RemoveGraph(Blueprint, Graph, EGraphRemoveFlags::MarkTransient);
			}
		}
		Blueprint->ImplementedInterfaces.Empty();

		// The AnimGraph keeps only its root; the EventGraph keeps nothing.
		TArray<UEdGraph*> NodeGraphs;
		if (UEdGraph* AnimGraph = DabGenFindFunctionGraph(Blueprint, UEdGraphSchema_K2::GN_AnimGraph))
		{
			NodeGraphs.Add(AnimGraph);
		}
		for (UEdGraph* Page : Blueprint->UbergraphPages)
		{
			if (Page)
			{
				NodeGraphs.Add(Page);
			}
		}
		for (UEdGraph* Graph : NodeGraphs)
		{
			const TArray<UEdGraphNode*> Nodes(Graph->Nodes);
			for (UEdGraphNode* Node : Nodes)
			{
				if (Node && !Node->IsA<UAnimGraphNode_Root>())
				{
					FBlueprintEditorUtils::RemoveNode(Blueprint, Node, true);
				}
			}
		}

		TArray<FName> Variables;
		for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
		{
			Variables.Add(Variable.VarName);
		}
		if (!Variables.IsEmpty())
		{
			FBlueprintEditorUtils::BulkRemoveMemberVariables(Blueprint, Variables);
		}
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	}

	/** Add against the new descriptions and parent, not the previous build's still-live skeleton. */
	static bool DabGenAddMemberVariable(UAnimBlueprint* Blueprint, FName Name, const FEdGraphPinType& Type, const FString& Default)
	{
		if (Name.IsNone() || Name == NAME_Self || FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name) != INDEX_NONE
			|| (Blueprint->ParentClass && FindFProperty<FProperty>(Blueprint->ParentClass, Name)))
		{
			return false;
		}
		FBPVariableDescription Variable;
		Variable.VarName = Name;
		Variable.VarGuid = FGuid::NewGuid();
		Variable.FriendlyName = FName::NameToDisplayString(Name.ToString(), Type.PinCategory == UEdGraphSchema_K2::PC_Boolean);
		Variable.VarType = Type;
		Variable.PropertyFlags = CPF_Edit | CPF_BlueprintVisible | CPF_DisableEditOnInstance;
		if (Type.PinCategory == UEdGraphSchema_K2::PC_Object || Type.PinCategory == UEdGraphSchema_K2::PC_Interface)
		{
			const UClass* Class = Cast<UClass>(Type.PinSubCategoryObject.Get());
			if (Class && Class->IsChildOf(AActor::StaticClass()))
			{
				Variable.PropertyFlags |= CPF_DisableEditOnTemplate;
			}
		}
		Variable.ReplicationCondition = COND_None;
		Variable.Category = UEdGraphSchema_K2::VR_DefaultCategory;
		Variable.DefaultValue = Default;
		Variable.VarType.bIsConst = false;
		Variable.VarType.bIsWeakPointer = false;
		Variable.VarType.bIsReference = false;
		if (Type.PinCategory == UEdGraphSchema_K2::PC_String || Type.PinCategory == UEdGraphSchema_K2::PC_Text)
		{
			Variable.SetMetaData(TEXT("MultiLine"), TEXT("true"));
		}
		Blueprint->NewVariables.Add(MoveTemp(Variable));
		FBlueprintEditorUtils::ValidateBlueprintChildVariables(Blueprint, Name);
		return true;
	}

	static void DabGenAddVariables(FDabGenContext& Ctx)
	{
		UAnimBlueprint* Blueprint = Ctx.Blueprint;
		for (const FDabModelVariable& Variable : Ctx.Model.Variables)
		{
			const FString Location = TEXT("Properties/") + Variable.Name;
			FEdGraphPinType Type;
			FString Error;
			if (!DabGenTypeToPinType(Variable.Type, Type, Error))
			{
				Ctx.Sink.Error(TEXT("DAB7004"), FDabSpan(), FString::Printf(TEXT("%s: %s"), *Location, *Error));
				continue;
			}
			FString Default;
			UObject* DefaultObject = nullptr;
			if (Variable.Default)
			{
				if (const FDabExpr* Unsupported = DabGenFindUnsupported(*Variable.Default))
				{
					Ctx.Sink.Error(TEXT("DAB7002"), FDabSpan(), FString::Printf(TEXT("%s: the default cannot be generated: %s"), *Location, *DabGenPrint(*Unsupported)));
				}
				else if (!DabGenMakeDefault(Ctx, Type, *Variable.Default, Default, DefaultObject, Error))
				{
					Ctx.Sink.Error(TEXT("DAB7003"), FDabSpan(), FString::Printf(TEXT("%s = %s: %s"), *Location, *DabGenPrint(*Variable.Default), *Error));
				}
			}

			const FName Name(*Variable.Name);
			if (!DabGenAddMemberVariable(Blueprint, Name, Type, Default))
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: the variable cannot be added (the name is taken)"), *Location));
				continue;
			}
			// A tuning value: set per instance in the details panel, read by the graph, never written.
			FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, Name, false);
			FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Blueprint, Name, true);
			for (const FDabModelProperty& Meta : Variable.Meta)
			{
				if (!Meta.Value)
				{
					continue;
				}
				const FString Value = DabGenMetaString(*Meta.Value);
				if (Meta.Name == TEXT("Group") || Meta.Name == TEXT("Category"))
				{
					// The skeleton is regenerated after all variables are present, so set the description directly.
					const int32 Index = FBlueprintEditorUtils::FindNewVariableIndex(Blueprint, Name);
					Blueprint->NewVariables[Index].Category = Value.IsEmpty() ? UEdGraphSchema_K2::VR_DefaultCategory : FText::FromString(Value);
				}
				else
				{
					FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, Name, nullptr, FName(*Meta.Name), Value);
				}
			}
		}

		if (Ctx.Model.AssetSlots.IsEmpty())
		{
			return;
		}

		// The animation set the slot getters read (Active) and the one SetAnimSet writes (Pending).
		UDreamAnimSet* DefaultSet = nullptr;
		if (!Ctx.Model.DefaultAnimSet.IsEmpty())
		{
			DefaultSet = Cast<UDreamAnimSet>(DabGenLoadObject(Ctx.Model.DefaultAnimSet));
			if (!DefaultSet)
			{
				Ctx.Sink.Error(TEXT("DAB7004"), FDabSpan(), FString::Printf(TEXT("Settings: DefaultAnimSet %s is not a DreamAnim animation set"), *Ctx.Model.DefaultAnimSet));
			}
		}
		FEdGraphPinType SetType;
		SetType.PinCategory = UEdGraphSchema_K2::PC_Object;
		SetType.PinSubCategoryObject = UDreamAnimSet::StaticClass();
		const FName SetVariables[] = { UDreamAnimLibrary::ActiveSetVariable, UDreamAnimLibrary::PendingSetVariable };
		const UClass* Parent = Blueprint->ParentClass.Get();
		for (const FName SetVariable : SetVariables)
		{
			if (Parent && FindFProperty<FProperty>(Parent, SetVariable))
			{
				// A parent DreamAnim Blueprint already declares them.
				continue;
			}
			const FString Default = (SetVariable == UDreamAnimLibrary::ActiveSetVariable && DefaultSet) ? DefaultSet->GetPathName() : FString();
			if (!DabGenAddMemberVariable(Blueprint, SetVariable, SetType, Default))
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("Assets: the variable %s cannot be added (the name is taken)"), *SetVariable.ToString()));
				continue;
			}
			Blueprint->NewVariables.Last().Category = FText::FromString(TEXT("DreamAnim"));
		}
	}

	static void DabGenImplementInterfaces(FDabGenContext& Ctx)
	{
		for (const FString& InterfacePath : Ctx.Model.Interfaces)
		{
			UClass* InterfaceClass = DabGenLoadClass(InterfacePath);
			if (!InterfaceClass || !InterfaceClass->IsChildOf(UAnimLayerInterface::StaticClass()))
			{
				Ctx.Sink.Error(TEXT("DAB7004"), FDabSpan(), FString::Printf(TEXT("Settings: %s is not an Animation Layer Interface (generate and compile it first)"), *InterfacePath));
				continue;
			}
			// Creates the interface's layer graphs, each with its root and input poses.
			if (!FBlueprintEditorUtils::ImplementNewInterface(Ctx.Blueprint, InterfaceClass->GetClassPathName()))
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("Settings: %s cannot be implemented"), *InterfacePath));
			}
		}
	}

	/** One graph record per layer: an implemented interface's graph, or a new self layer. */
	static void DabGenPrepareLayers(FDabGenContext& Ctx)
	{
		UAnimBlueprint* Blueprint = Ctx.Blueprint;
		for (const FDabModelLayer& Layer : Ctx.Model.Layers)
		{
			const FName LayerName(*Layer.Name);
			UEdGraph* LayerGraph = nullptr;
			for (const FBPInterfaceDescription& Description : Blueprint->ImplementedInterfaces)
			{
				for (UEdGraph* Graph : Description.Graphs)
				{
					if (Graph && Graph->GetFName() == LayerName)
					{
						LayerGraph = Graph;
					}
				}
			}
			if (!LayerGraph)
			{
				LayerGraph = DabGenFindFunctionGraph(Blueprint, LayerName);
			}
			if (!LayerGraph)
			{
				// What adding an animation layer in the editor does.
				LayerGraph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, LayerName, UAnimationGraph::StaticClass(), UAnimationGraphSchema::StaticClass());
				FBlueprintEditorUtils::AddDomainSpecificGraph(Blueprint, LayerGraph);
			}

			TUniquePtr<FDabGenGraph> Record = MakeUnique<FDabGenGraph>();
			Record->Model = &Layer.Graph;
			Record->Graph = LayerGraph;
			Record->Path = TEXT("Layer/") + Layer.Name;
			UAnimGraphNode_Root* Root = DabGenFindRoot(LayerGraph);
			Record->ResultNode = Root;
			Record->ResultPin = Root ? Root->FindPin(TEXT("Result"), EGPD_Input) : nullptr;
			Ctx.Graphs.Add(MoveTemp(Record));
		}
	}

	static void DabGenPrepareAnimGraph(FDabGenContext& Ctx)
	{
		const FDabModelGraph& Model = Ctx.Model.AnimGraph;
		UEdGraph* AnimGraph = DabGenFindFunctionGraph(Ctx.Blueprint, UEdGraphSchema_K2::GN_AnimGraph);
		if (!AnimGraph)
		{
			if (!Model.Nodes.IsEmpty() || !Model.ResultNodeId.IsEmpty())
			{
				// A child of another Animation Blueprint has no AnimGraph of its own.
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), TEXT("AnimGraph: the Blueprint has no AnimGraph (its parent is an Animation Blueprint)"));
			}
			return;
		}
		TUniquePtr<FDabGenGraph> Record = MakeUnique<FDabGenGraph>();
		Record->Model = &Model;
		Record->Graph = AnimGraph;
		Record->Path = TEXT("AnimGraph");
		UAnimGraphNode_Root* Root = DabGenFindRoot(AnimGraph);
		Record->ResultNode = Root;
		Record->ResultPin = Root ? Root->FindPin(TEXT("Result"), EGPD_Input) : nullptr;
		Ctx.Graphs.Add(MoveTemp(Record));
	}

	/** How far a node reaches from its position, for a comment box around it: a node has no size until it is drawn. */
	static FIntPoint DabGenNodeExtent(const UEdGraphNode& Node)
	{
		return Node.IsA<UAnimStateNodeBase>() ? FIntPoint(200, 80) : FIntPoint(320, 160);
	}

	/** A comment box titled Label around Nodes, where they finally are: what `#Region "Label"` writes. */
	static void DabGenAddRegionComment(FDabGenContext& Ctx, UEdGraph& Graph, const FString& GraphPath, const FString& Label, const TArray<UEdGraphNode*>& Nodes)
	{
		if (Nodes.IsEmpty())
		{
			return;
		}
		FIntPoint Min(TNumericLimits<int32>::Max(), TNumericLimits<int32>::Max());
		FIntPoint Max(TNumericLimits<int32>::Lowest(), TNumericLimits<int32>::Lowest());
		for (const UEdGraphNode* Node : Nodes)
		{
			const FIntPoint Position(Node->NodePosX, Node->NodePosY);
			Min = Min.ComponentMin(Position);
			Max = Max.ComponentMax(Position + DabGenNodeExtent(*Node));
		}
		FGraphNodeCreator<UEdGraphNode_Comment> Creator(Graph);
		UEdGraphNode_Comment* Comment = Creator.CreateNode(false);
		// After Finalize: placing a new comment gives it the default text.
		Creator.Finalize();
		Comment->NodeComment = Label;
		Comment->NodePosX = Min.X - 40;
		Comment->NodePosY = Min.Y - 80;
		Comment->NodeWidth = Max.X - Min.X + 80;
		Comment->NodeHeight = Max.Y - Min.Y + 120;
		Ctx.AssignGuid(Comment, GraphPath, TEXT("Region/") + Label);
	}

	/** Every region's comment box: around the nodes a graph's named poses made, around a machine's states. */
	static void DabGenAddRegionComments(FDabGenContext& Ctx)
	{
		for (const TUniquePtr<FDabGenGraph>& Record : Ctx.Graphs)
		{
			if (!Record->Model || !Record->Graph)
			{
				continue;
			}
			for (const FDabModelRegion& Region : Record->Model->Regions)
			{
				// A pose's nodes are the one named after it and those under it (`Pose/A/Base`).
				TArray<UEdGraphNode*> Nodes;
				for (const TPair<FString, UAnimGraphNode_Base*>& Entry : Record->Nodes)
				{
					const FString& Id = Entry.Key;
					if (Entry.Value && Region.Members.ContainsByPredicate([&Id](const FString& Member) { return Id == Member || Id.StartsWith(Member + TEXT("/")); }))
					{
						Nodes.Add(Entry.Value);
					}
				}
				DabGenAddRegionComment(Ctx, *Record->Graph, Record->Path, Region.Label, Nodes);
			}
		}
		for (const TPair<FString, TUniquePtr<FDabGenMachine>>& Entry : Ctx.Machines)
		{
			const FDabGenMachine& Machine = *Entry.Value;
			UEdGraph* Graph = Machine.Node ? Machine.Node->EditorStateMachineGraph : nullptr;
			if (!Machine.Model || !Graph)
			{
				continue;
			}
			for (const FDabModelRegion& Region : Machine.Model->Regions)
			{
				TArray<UEdGraphNode*> Nodes;
				for (const FString& Member : Region.Members)
				{
					if (UAnimStateNodeBase* const* State = Machine.StateNodes.Find(Member); State && *State)
					{
						Nodes.Add(*State);
					}
				}
				DabGenAddRegionComment(Ctx, *Graph, Machine.Path, Region.Label, Nodes);
			}
		}
	}

	/** The default animation set, when its slots had to change to match the model's. */
	static UDreamAnimSet* DabGenSyncAnimSet(FDabGenContext& Ctx)
	{
		if (Ctx.Model.DefaultAnimSet.IsEmpty() || Ctx.Model.AssetSlots.IsEmpty())
		{
			return nullptr;
		}
		UDreamAnimSet* Set = Cast<UDreamAnimSet>(DabGenLoadObject(Ctx.Model.DefaultAnimSet));
		if (!Set)
		{
			return nullptr;
		}
		TArray<TPair<FName, FName>> Slots;
		for (const FDabModelAssetSlot& Slot : Ctx.Model.AssetSlots)
		{
			Slots.Emplace(FName(*Slot.Name), FName(*Slot.Type));
		}
		if (!Set->SyncSlots(Ctx.GetBlueprintAssetName(), Slots))
		{
			return nullptr;
		}
		Set->MarkPackageDirty();
		return Set;
	}

	/**
	 * The places in the model of the generated nodes a compiler message names: each node the message
	 * mentions, then the nodes whose graphs hold it (a rule's transition, a state), innermost first.
	 */
	static TArray<FString> DabGenMessageOrigins(const FTokenizedMessage& Message, const TMap<FGuid, FString>& GuidPlaces)
	{
		TArray<FString> Origins;
		for (const TSharedRef<IMessageToken>& Token : Message.GetMessageTokens())
		{
			const UObject* Object = nullptr;
			if (Token->GetType() == EMessageToken::EdGraph)
			{
				const FEdGraphToken& GraphToken = static_cast<const FEdGraphToken&>(*Token);
				Object = GraphToken.GetGraphObject();
				if (!Object && GraphToken.GetPin())
				{
					Object = GraphToken.GetPin()->GetOwningNodeUnchecked();
				}
			}
			else if (Token->GetType() == EMessageToken::Object)
			{
				Object = static_cast<const FUObjectToken&>(*Token).GetObject().Get();
			}
			for (const UObject* Current = Object; Current && !Current->IsA<UBlueprint>(); Current = Current->GetOuter())
			{
				const UEdGraphNode* Node = Cast<UEdGraphNode>(Current);
				if (const FString* Place = Node ? GuidPlaces.Find(Node->NodeGuid) : nullptr)
				{
					Origins.AddUnique(*Place);
				}
			}
		}
		return Origins;
	}

	static void DabGenCompile(UAnimBlueprint* Blueprint, const TMap<FGuid, FString>& GuidPlaces, FDabGenerateResult& Result)
	{
		// Silent: an error in the log would fail the automation test that generates on purpose broken models.
		FCompilerResultsLog Log;
		Log.SetSilentMode(true);
		// Saving is the generator's call (Options.bSave), not the editor's save-on-compile setting.
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection | EBlueprintCompileOptions::SkipSave, &Log);
		for (const TSharedRef<FTokenizedMessage>& Message : Log.Messages)
		{
			const EMessageSeverity::Type Severity = Message->GetSeverity();
			if (Severity == EMessageSeverity::Error)
			{
				Result.CompilerErrors.Add(Message->ToText().ToString());
				Result.CompilerErrorOrigins.Add(DabGenMessageOrigins(*Message, GuidPlaces));
			}
			else if (Severity == EMessageSeverity::Warning || Severity == EMessageSeverity::PerformanceWarning)
			{
				Result.CompilerWarnings.Add(Message->ToText().ToString());
				Result.CompilerWarningOrigins.Add(DabGenMessageOrigins(*Message, GuidPlaces));
			}
		}
		if (Blueprint->Status == BS_Error && Result.CompilerErrors.IsEmpty())
		{
			Result.CompilerErrors.Add(FString::Printf(TEXT("%s failed to compile"), *Blueprint->GetName()));
			Result.CompilerErrorOrigins.AddDefaulted();
		}
	}

	static bool DabGenSave(UObject* Asset, FDabDiagnosticSink& Sink)
	{
		UPackage* Package = Asset->GetPackage();
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		Args.Error = GWarn;
		Args.bSlowTask = false;
		if (!UPackage::SavePackage(Package, Asset, *Filename, Args))
		{
			Sink.Error(TEXT("DAB7006"), FDabSpan(), FString::Printf(TEXT("%s cannot be saved to %s"), *Package->GetName(), *Filename));
			return false;
		}
		return true;
	}

	static FDabGenerateResult DabGenGenerate(const FDabModel& Model, FDabDiagnosticSink& Sink, const FDabGenerateOptions& Options, bool bPreview = false)
	{
		FDabGenerateResult Result;
		const int32 ErrorsBefore = Sink.NumErrors();
		if (Model.Kind != EDabDocumentKind::AnimBlueprint && Model.Kind != EDabDocumentKind::AnimLayerInterface)
		{
			Sink.Error(TEXT("DAB7006"), FDabSpan(), FString::Printf(TEXT("%s: only Animation Blueprints and Animation Layer Interfaces are generated"), *Model.AssetPath));
			return Result;
		}
		const bool bBlueprint = Model.Kind == EDabDocumentKind::AnimBlueprint;

		UClass* ParentClass = nullptr;
		USkeleton* Skeleton = nullptr;
		if (!DabGenResolveParentAndSkeleton(Model, Sink, ParentClass, Skeleton))
		{
			return Result;
		}
		UAnimBlueprint* Blueprint = DabGenFindOrCreateAsset(Model, Sink, Options, ParentClass, Skeleton, bPreview);
		if (!Blueprint)
		{
			return Result;
		}
		Blueprint->Modify();
		// Do not reinstance child classes against a temporarily empty parent layout.
		const bool bWasSuppressing = Blueprint->bSuppressStructurallyModified;
		Blueprint->bSuppressStructurallyModified = true;
		DabGenClearBlueprint(Blueprint);

		FDabGenContext Ctx(Model, Sink, Blueprint);
		// Rebuilding means the settings currently in the source, including settings removed since the last build.
		for (TFieldIterator<FProperty> It(UAnimBlueprint::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			const FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Edit) && Property->GetFName() != TEXT("TargetSkeleton")
				&& !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				Property->CopyCompleteValue_InContainer(Blueprint, GetDefault<UAnimBlueprint>());
			}
		}
		Blueprint->bIsTemplate = bBlueprint && Skeleton == nullptr;
		DabGenApplyObjectProperties(Ctx, Blueprint, Model.ClassSettings, TEXT("Settings"));
		if (bBlueprint)
		{
			DabGenAddVariables(Ctx);
			DabGenImplementInterfaces(Ctx);
		}
		DabGenPrepareLayers(Ctx);
		if (bBlueprint)
		{
			DabGenPrepareAnimGraph(Ctx);
		}
		Blueprint->bSuppressStructurallyModified = bWasSuppressing;
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		// Every graph's nodes; state machines append their states' graphs while this runs.
		for (int32 Index = 0; Index < Ctx.Graphs.Num(); ++Index)
		{
			DabGenCreateGraphNodes(Ctx, *Ctx.Graphs[Index]);
		}
		// Layer nodes calling this Blueprint's own layers take their input pose pins from the skeleton class.
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		DabGenReconstructSelfLayerNodes(Ctx);

		for (int32 Index = 0; Index < Ctx.Graphs.Num(); ++Index)
		{
			DabGenLinkGraph(Ctx, *Ctx.Graphs[Index]);
		}
		DabGenResolveCacheUses(Ctx);

		// Bindings after links: linking a pin clears its bindings.
		for (int32 Index = 0; Index < Ctx.Deferred.Num(); ++Index)
		{
			const TFunction<void()> Work = MoveTemp(Ctx.Deferred[Index]);
			if (Work)
			{
				Work();
			}
		}
		Ctx.Deferred.Reset();

		if (bBlueprint)
		{
			// A slot no pin reads yet still belongs to the Blueprint: its getter is what records it.
			for (const FDabModelAssetSlot& Slot : Model.AssetSlots)
			{
				DabGenEnsureAssetFunction(Ctx, Slot.Name, TEXT("Assets.") + Slot.Name);
			}
			DabGenBuildEventGraph(Ctx);
		}
		for (const TUniquePtr<FDabGenGraph>& Graph : Ctx.Graphs)
		{
			DabGenLayoutGraph(*Graph);
		}

		// Bindings resolve their types against the final skeleton class.
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		for (UAnimGraphNode_Base* Node : Ctx.BoundNodes)
		{
			Node->ReconstructNode();
		}
		Ctx.AssignRemainingGuids();
		if (!Options.NodePositions.IsEmpty())
		{
			// Where the editor last had them; nodes the layout does not know keep the automatic place.
			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			for (const UEdGraph* Graph : AllGraphs)
			{
				if (!Graph)
				{
					continue;
				}
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (const FIntPoint* Position = Node ? Options.NodePositions.Find(Node->NodeGuid) : nullptr)
					{
						Node->NodePosX = Position->X;
						Node->NodePosY = Position->Y;
					}
				}
			}
		}
		DabGenAddRegionComments(Ctx);
		Blueprint->RequestRefreshExtensions();

		Result.Blueprint = Blueprint;
		// Class defaults go on the current class's default object before the compile: the compile carries them
		// to the new one and, only then, lists them among the values new instances copy from it.
		const auto ApplyDefaults = [&Ctx, Blueprint, ParentClass, &Model]()
		{
			UObject* Defaults = Blueprint->GeneratedClass->GetDefaultObject();
			const UObject* ParentDefaults = ParentClass ? ParentClass->GetDefaultObject() : nullptr;
			if (ParentDefaults)
			{
				for (TFieldIterator<FProperty> It(ParentClass); It; ++It)
				{
					const FProperty* Property = *It;
					// An instanced subobject is the parent's own: copying the pointer would share it, so it stays.
					if (Property->HasAnyPropertyFlags(CPF_Edit)
						&& !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DisableEditOnTemplate
							| CPF_InstancedReference | CPF_ContainsInstancedReference)
						&& Property->GetOwnerClass() != UObject::StaticClass())
					{
						Property->CopyCompleteValue_InContainer(Defaults, ParentDefaults);
					}
				}
			}
			// A variable whose default the text no longer gives gets its type's zero value; the compile would
			// otherwise carry the previous class's value over by name.
			for (const FBPVariableDescription& Variable : Blueprint->NewVariables)
			{
				const FProperty* Property = Variable.DefaultValue.IsEmpty() ? FindFProperty<FProperty>(Defaults->GetClass(), Variable.VarName) : nullptr;
				if (Property)
				{
					Property->ClearValue_InContainer(Defaults);
				}
			}
			DabGenApplyObjectProperties(Ctx, Defaults, Model.ClassDefaults, TEXT("Defaults"));
		};
		const bool bDefaultsFirst = Blueprint->GeneratedClass && (!ParentClass || Blueprint->GeneratedClass->IsChildOf(ParentClass));
		if (bDefaultsFirst)
		{
			ApplyDefaults();
		}
		DabGenCompile(Blueprint, Ctx.GuidPlaces, Result);
		if (!bDefaultsFirst && Blueprint->GeneratedClass)
		{
			ApplyDefaults();
			Result.CompilerErrors.Reset();
			Result.CompilerWarnings.Reset();
			Result.CompilerErrorOrigins.Reset();
			Result.CompilerWarningOrigins.Reset();
			DabGenCompile(Blueprint, Ctx.GuidPlaces, Result);
		}

		const bool bModelClean = Sink.NumErrors() == ErrorsBefore;
		UDreamAnimSet* SyncedSet = nullptr;
		if (!bPreview && bModelClean && Result.CompilerErrors.IsEmpty())
		{
			SyncedSet = DabGenSyncAnimSet(Ctx);
			if (!Options.SourceText.IsEmpty())
			{
				SetDabGeneratedText(*Blueprint, Options.SourceText);
			}
		}
		if (Options.bSave && bModelClean && Result.CompilerErrors.IsEmpty())
		{
			DabGenSave(Blueprint, Sink);
			// The set's new slots are part of the build: a command line run that only marked it would lose them.
			if (SyncedSet)
			{
				DabGenSave(SyncedSet, Sink);
			}
		}
		else
		{
			Blueprint->MarkPackageDirty();
		}
		return Result;
	}
}

namespace UE::DreamAnim::Compiler
{
	static const TCHAR* const DabGeneratedTextKey = TEXT("DreamAnim.Source");

	FString GetDabGeneratedText(const UAnimBlueprint& Blueprint)
	{
		const FString* Text = Blueprint.GetPackage()->GetMetaData().FindValue(&Blueprint, DabGeneratedTextKey);
		return Text ? *Text : FString();
	}

	void SetDabGeneratedText(UAnimBlueprint& Blueprint, const FString& Text)
	{
		Blueprint.GetPackage()->GetMetaData().SetValue(&Blueprint, DabGeneratedTextKey, *Text);
	}

	FString GetDabLayoutPath(const FString& SourcePath)
	{
		return SourcePath + TEXT(".layout");
	}

	FString MakeDabLayout(const UAnimBlueprint& Blueprint)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint.GetAllGraphs(Graphs);
		TArray<TPair<FString, FString>> Lines;
		for (const UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node || !Node->NodeGuid.IsValid())
				{
					continue;
				}
				// The comment is for whoever reads a diff of the file; only the GUID and position are read back.
				FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" "));
				const FString Sort = Graph->GetName() + TEXT("\n") + Node->NodeGuid.ToString(EGuidFormats::Digits);
				Lines.Emplace(Sort, FString::Printf(TEXT("%s %d %d  # %s: %s"), *Node->NodeGuid.ToString(EGuidFormats::Digits), Node->NodePosX, Node->NodePosY,
					*Graph->GetName(), *Title.TrimStartAndEnd()));
			}
		}
		Lines.Sort([](const TPair<FString, FString>& A, const TPair<FString, FString>& B) { return A.Key < B.Key; });

		FString Text = FString::Printf(TEXT("# Where the editor put the nodes of %s, by node GUID. Written by the editor; safe to delete.\n"), *Blueprint.GetName());
		for (const TPair<FString, FString>& Line : Lines)
		{
			Text += Line.Value;
			Text += TEXT("\n");
		}
		return Text;
	}

	TMap<FGuid, FIntPoint> ParseDabLayout(const FString& Text)
	{
		TMap<FGuid, FIntPoint> Positions;
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			FString Data = Line;
			int32 Hash = INDEX_NONE;
			if (Data.FindChar(TEXT('#'), Hash))
			{
				Data.LeftInline(Hash);
			}
			TArray<FString> Fields;
			Data.ParseIntoArrayWS(Fields);
			FGuid Guid;
			if (Fields.Num() == 3 && FGuid::Parse(Fields[0], Guid) && Fields[1].IsNumeric() && Fields[2].IsNumeric())
			{
				Positions.Add(Guid, FIntPoint(FCString::Atoi(*Fields[1]), FCString::Atoi(*Fields[2])));
			}
		}
		return Positions;
	}

	UClass* GetDabAssetSlotReadClass(const UClass* SlotClass)
	{
		if (SlotClass && SlotClass->IsChildOf(UAnimMontage::StaticClass()))
		{
			return UAnimMontage::StaticClass();
		}
		if (SlotClass && SlotClass->IsChildOf(UBlendSpace::StaticClass()))
		{
			return UBlendSpace::StaticClass();
		}
		if (SlotClass && SlotClass->IsChildOf(UAnimSequenceBase::StaticClass()))
		{
			return UAnimSequenceBase::StaticClass();
		}
		return UAnimationAsset::StaticClass();
	}

	FGuid MakeDabNodeGuid(const FString& AssetPath, const FString& GraphPath, const FString& Id)
	{
		return GeneratePrivate::DabGenHashGuid(AssetPath + TEXT("/") + GraphPath + TEXT("/") + Id);
	}

	FDabGenerateResult GenerateDabBlueprint(const Model::FDabModel& Model, Lang::FDabDiagnosticSink& Sink, const FDabGenerateOptions& Options)
	{
		const FString TargetPackage = Options.OverridePackageName.IsEmpty()
			? FPackageName::ObjectPathToPackageName(Model.AssetPath) : Options.OverridePackageName;
		const FString TargetObject = TargetPackage + TEXT(".") + FPackageName::GetShortName(TargetPackage);
		if (!FindObject<UObject>(nullptr, *TargetObject) && !FPackageName::DoesPackageExist(TargetPackage))
		{
			// Nothing to protect yet: a new asset that fails to compile is never saved.
			return GeneratePrivate::DabGenGenerate(Model, Sink, Options);
		}

		// Compile an isolated candidate before touching a live asset or its animation set. Keep the short
		// asset name: generated function names and animation-set group keys depend on it.
		FDabGenerateOptions PreviewOptions = Options;
		PreviewOptions.bSave = false;
		PreviewOptions.OverridePackageName = TEXT("/Temp/DreamAnimPreview/") + FGuid::NewGuid().ToString(EGuidFormats::Digits)
			+ TEXT("/") + FPackageName::GetShortName(TargetPackage);
		// The candidate reports to a sink of its own: when it compiles, the real build reports the same again.
		Lang::FDabDiagnosticSink PreviewSink(Sink.GetFilePath());
		FDabGenerateResult Preview = GeneratePrivate::DabGenGenerate(Model, PreviewSink, PreviewOptions, true);
		const bool bValid = Preview.Succeeded() && !PreviewSink.HasErrors();
		if (Preview.Blueprint)
		{
			// Keep graph outers intact until collection, and invalidate deferred editor callbacks on the nodes.
			TArray<UK2Node_AnimGetter*> Getters;
			FBlueprintEditorUtils::GetAllNodesOfClass(Preview.Blueprint, Getters);
			for (UK2Node_AnimGetter* Getter : Getters)
			{
				Getter->ConditionalBeginDestroy();
			}
			UPackage* Package = Preview.Blueprint->GetPackage();
			TArray<UObject*> Objects;
			GetObjectsWithOuter(Package, Objects, EGetObjectsFlags::IncludeNestedObjects);
			for (UObject* Object : Objects)
			{
				Object->ClearFlags(RF_Public | RF_Standalone);
				Object->MarkAsGarbage();
			}
			Package->SetDirtyFlag(false);
		}
		if (!bValid)
		{
			Sink.Append(MoveTemp(PreviewSink));
			Preview.Blueprint = nullptr;
			return Preview;
		}
		return GeneratePrivate::DabGenGenerate(Model, Sink, Options);
	}
}
