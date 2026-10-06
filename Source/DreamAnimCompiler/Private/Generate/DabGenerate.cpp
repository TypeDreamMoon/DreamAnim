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
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimLayerInterface.h"
#include "Animation/Skeleton.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DreamAnimLibrary.h"
#include "DreamAnimSet.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Generate/DabGenerateInternal.h"
#include "K2Node_AnimGetter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
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
		const FString Full = Model.AssetPath + TEXT("/") + Key;
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
		FString Key = GraphPath + TEXT("/") + Id;
		FGuid Guid = MakeGuid(Key);
		while (UsedGuids.Contains(Guid))
		{
			Key += TEXT("#");
			Guid = MakeGuid(Key);
		}
		Node->NodeGuid = Guid;
		UsedGuids.Add(Guid);
		GuidNodes.Add(Node);
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
		USkeleton* Skeleton)
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
		if (!FPackageName::IsValidLongPackageName(PackageName) || AssetName.IsEmpty())
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
			RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
		if (!Blueprint)
		{
			Sink.Error(TEXT("DAB7006"), FDabSpan(), FString::Printf(TEXT("%s: the asset cannot be created"), *ObjectPath));
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Blueprint);
		Package->MarkPackageDirty();
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
			if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, Name, Type, Default))
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
					FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, Name, nullptr, FText::FromString(Value), true);
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
			if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, SetVariable, SetType, Default))
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("Assets: the variable %s cannot be added (the name is taken)"), *SetVariable.ToString()));
				continue;
			}
			FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, SetVariable, nullptr, FText::FromString(TEXT("DreamAnim")), true);
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

	static void DabGenSyncAnimSet(FDabGenContext& Ctx)
	{
		if (Ctx.Model.DefaultAnimSet.IsEmpty() || Ctx.Model.AssetSlots.IsEmpty())
		{
			return;
		}
		UDreamAnimSet* Set = Cast<UDreamAnimSet>(DabGenLoadObject(Ctx.Model.DefaultAnimSet));
		if (!Set)
		{
			return;
		}
		TArray<TPair<FName, FName>> Slots;
		for (const FDabModelAssetSlot& Slot : Ctx.Model.AssetSlots)
		{
			Slots.Emplace(FName(*Slot.Name), FName(*Slot.Type));
		}
		if (Set->SyncSlots(Ctx.GetBlueprintAssetName(), Slots))
		{
			Set->MarkPackageDirty();
		}
	}

	static void DabGenCompile(UAnimBlueprint* Blueprint, FDabGenerateResult& Result)
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
			}
			else if (Severity == EMessageSeverity::Warning || Severity == EMessageSeverity::PerformanceWarning)
			{
				Result.CompilerWarnings.Add(Message->ToText().ToString());
			}
		}
		if (Blueprint->Status == BS_Error && Result.CompilerErrors.IsEmpty())
		{
			Result.CompilerErrors.Add(FString::Printf(TEXT("%s failed to compile"), *Blueprint->GetName()));
		}
	}

	static bool DabGenSave(UAnimBlueprint* Blueprint, FDabDiagnosticSink& Sink)
	{
		UPackage* Package = Blueprint->GetPackage();
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		Args.Error = GWarn;
		Args.bSlowTask = false;
		if (!UPackage::SavePackage(Package, Blueprint, *Filename, Args))
		{
			Sink.Error(TEXT("DAB7006"), FDabSpan(), FString::Printf(TEXT("%s cannot be saved to %s"), *Package->GetName(), *Filename));
			return false;
		}
		return true;
	}

	static FDabGenerateResult DabGenGenerate(const FDabModel& Model, FDabDiagnosticSink& Sink, const FDabGenerateOptions& Options)
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
		UAnimBlueprint* Blueprint = DabGenFindOrCreateAsset(Model, Sink, Options, ParentClass, Skeleton);
		if (!Blueprint)
		{
			return Result;
		}
		Blueprint->Modify();
		DabGenClearBlueprint(Blueprint);

		FDabGenContext Ctx(Model, Sink, Blueprint);
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
		DabGenSyncAnimSet(Ctx);

		// Bindings resolve their types against the final skeleton class.
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		for (UAnimGraphNode_Base* Node : Ctx.BoundNodes)
		{
			Node->ReconstructNode();
		}
		Ctx.AssignRemainingGuids();
		Blueprint->RequestRefreshExtensions();

		Result.Blueprint = Blueprint;
		DabGenCompile(Blueprint, Result);

		const bool bModelClean = Sink.NumErrors() == ErrorsBefore;
		if (Options.bSave && bModelClean && Result.CompilerErrors.IsEmpty())
		{
			DabGenSave(Blueprint, Sink);
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
	FDabGenerateResult GenerateDabBlueprint(const Model::FDabModel& Model, Lang::FDabDiagnosticSink& Sink, const FDabGenerateOptions& Options)
	{
		return GeneratePrivate::DabGenGenerate(Model, Sink, Options);
	}
}
