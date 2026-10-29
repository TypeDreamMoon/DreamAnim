// Copyright DreamMoon. All Rights Reserved.
//
// What the generator's files share: one context per generation, the value converter (expression ->
// property value / pin default), the anim graph builder, the state machine builder and the K2 builder
// (expression -> K2 nodes, generated functions). Everything lives in GeneratePrivate and every helper
// carries a DabGen prefix, because unity builds merge these files with the rest of the module.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"
#include "Generate/DabGenerate.h"
#include "Lang/DabAst.h"
#include "Lang/DabDiagnostic.h"
#include "Model/DabModel.h"

class FProperty;
class UAnimBlueprint;
class UAnimGraphNode_Base;
class UAnimGraphNode_SaveCachedPose;
class UAnimGraphNode_StateMachine;
class UAnimGraphNode_UseCachedPose;
class UAnimStateNodeBase;
class UClass;
class UEdGraph;
class UEdGraphNode;
class UEnum;
class UFunction;
class UObject;
class UScriptStruct;
class UStruct;

namespace UE::DreamAnim::Compiler::GeneratePrivate
{
	/** MD5 of the text as a GUID, never the invalid zero GUID. */
	FGuid DabGenHashGuid(const FString& Full);

	// Members below are named Model; inside those classes the name must not also mean the namespace.
	using FDabGenModelMachine = Model::FDabModelStateMachine;
	using FDabGenModelGraph = Model::FDabModelGraph;
	using FDabGenModelDocument = Model::FDabModel;

	/** A state machine as built: its editor node and its states, conduits and aliases by name. */
	struct FDabGenMachine
	{
		const FDabGenModelMachine* Model = nullptr;
		UAnimGraphNode_StateMachine* Node = nullptr;
		/** `Machine/<Name>`: the prefix of every GUID key inside the machine. */
		FString Path;
		TMap<FString, UAnimStateNodeBase*> StateNodes;
	};

	/** One pose graph being built: the AnimGraph, a layer, or a state's graph. */
	struct FDabGenGraph
	{
		const FDabGenModelGraph* Model = nullptr;
		UEdGraph* Graph = nullptr;
		/** `AnimGraph`, `Layer/<Name>`, `Machine/<M>/State/<S>`. */
		FString Path;
		UAnimGraphNode_Base* ResultNode = nullptr;
		UEdGraphPin* ResultPin = nullptr;
		/** The machine this graph is a state of; empty for the AnimGraph and layers. */
		FString MachineName;
		TMap<FString, UAnimGraphNode_Base*> Nodes;
	};

	/** Where an expression is built, for the getters that read it (`StateWeight(S)`, `TimeRemaining()`). */
	struct FDabGenScope
	{
		FString MachineName;
		/** The transition's source state; empty outside transitions. */
		FString FromState;
	};

	/** Which signature a generated forwarder function gets. */
	enum class EDabGenForwarder : uint8
	{
		/** (Context, Node) like Prototype_ThreadSafeAnimUpdateCall: state functions and node functions. */
		AnimUpdateCall,
		/** No parameters: the target of a Call Function node. */
		Plain,
	};

	/** A Self-rooted read: `Self.GetParent().ViewState.X` -> Root Self, Segments {GetParent, ViewState, X}. */
	enum class EDabGenPathRoot : uint8
	{
		None,
		Self,
		Props,
		Assets,
	};

	struct FDabGenPath
	{
		EDabGenPathRoot Root = EDabGenPathRoot::None;
		TArray<FString> Segments;
		/** Which segments were written as zero-argument calls. */
		TArray<bool> SegmentIsCall;

		bool LastIsCall() const { return !SegmentIsCall.IsEmpty() && SegmentIsCall.Last(); }
		bool AllCalls() const;
	};

	class FDabGenContext
	{
	public:
		FDabGenContext(const FDabGenModelDocument& InModel, Lang::FDabDiagnosticSink& InSink, UAnimBlueprint* InBlueprint);

		const FDabGenModelDocument& Model;
		Lang::FDabDiagnosticSink& Sink;
		UAnimBlueprint* Blueprint = nullptr;

		/** Every pose graph in build order; state graphs are appended while their machines are built. */
		TArray<TUniquePtr<FDabGenGraph>> Graphs;
		TMap<FString, TUniquePtr<FDabGenMachine>> Machines;

		TMap<FString, UAnimGraphNode_SaveCachedPose*> SaveCacheNodes;
		struct FCacheUse
		{
			UAnimGraphNode_UseCachedPose* Node = nullptr;
			FString CacheName;
			FString Location;
		};
		TArray<FCacheUse> CacheUses;

		/** Work that needs every graph's nodes: bindings, conditions, node and state functions. Run in order. */
		TArray<TFunction<void()>> Deferred;
		/** Nodes given Property Access bindings; reconstructed once the skeleton class is final. */
		TSet<UAnimGraphNode_Base*> BoundNodes;
		/** Nodes the Blueprint already had that the model claimed (interface layer input poses). */
		TSet<UEdGraphNode*> AdoptedNodes;

		/** Generated functions by what they compute, so identical uses share one. */
		TMap<FString, FName> ExprFunctions;
		TMap<FString, FName> AssetFunctions;
		TMap<FString, FName> Forwarders;
		TSet<FName> GeneratedFunctions;

		/** Gives Node the GUID of its place in the model; collisions get a salt so GUIDs stay unique in the Blueprint. */
		void AssignGuid(UEdGraphNode* Node, const FString& GraphPath, const FString& Id);
		/** The place each assigned GUID was made from, `<graph path>/<id>`: what a compiler message about the node points back to. */
		TMap<FGuid, FString> GuidPlaces;
		/** A deterministic GUID for any key (graphs, shared rules). */
		FGuid MakeGuid(const FString& Key) const;
		/** Every node no other step gave a GUID (default nodes, conversion nodes) gets one from its graph and order. */
		void AssignRemainingGuids();

		/** The class paths and functions resolve against (the skeleton class). */
		UClass* GetSkeletonClass() const;
		/** The asset name generated code keys the animation set with. */
		FName GetBlueprintAssetName() const;
		/** The machine's info, or null. */
		FDabGenMachine* FindMachine(const FString& Name) const;

	private:
		TSet<FGuid> UsedGuids;
		TSet<const UEdGraphNode*> GuidNodes;
	};

	// ------------------------------------------------------------------------------------------
	// Values (DabGenerateValue.cpp)
	// ------------------------------------------------------------------------------------------

	FString DabGenPrint(const Lang::FDabExpr& Expr);
	FString DabGenPrintCall(const Model::FDabModelCall& Call);
	/** Eight hex digits of a stable hash, for generated names. */
	FString DabGenShortHash(const FString& Text);

	/** A value fixed at compile time: literals, enum names, `Path(...)`, tuples and lists of these. */
	bool DabGenIsConstant(const Lang::FDabExpr& Expr);
	bool DabGenTryGetNumber(const Lang::FDabExpr& Expr, double& OutNumber);
	bool DabGenTryGetBool(const Lang::FDabExpr& Expr, bool& OutValue);
	/** A string literal's value or a bare name. */
	bool DabGenTryGetText(const Lang::FDabExpr& Expr, FString& OutText);
	bool DabGenIsSelf(const Lang::FDabExpr* Expr);
	/** The first `Unsupported("...")` the extractor left in Expr (something the language cannot say), or null. */
	const Lang::FDabExpr* DabGenFindUnsupported(const Lang::FDabExpr& Expr);

	/** `Path(Root, "Rel")` -> `/Mount/Rel.Leaf`. */
	bool DabGenTryGetAssetPath(const Lang::FDabExpr& Expr, FString& OutObjectPath);
	UObject* DabGenLoadObject(const FString& ObjectPath);
	/** A class from an object path: a native class, a generated class, or a Blueprint's generated class. */
	UClass* DabGenLoadClass(const FString& ObjectPath);
	/** A reflected type by name or path: `FVector`, `Vector`, `EAlsGait`, `/Script/ALS.EAlsGait`. */
	UObject* DabGenFindType(const FString& Name, UClass* TypeClass);

	/** Reads a chain of names, members and zero-argument calls rooted at Self, Props or Assets. */
	bool DabGenTryGetPath(const Lang::FDabExpr& Expr, FDabGenPath& OutPath);

	/**
	 * Writes the value of Expr into Value, a property of type Property. Structs take named tuples applied over
	 * the current value, arrays take lists, objects take `Path(...)`, enums and names take bare names, tags take
	 * strings. EnumHint resolves enum names for byte properties that carry no enum themselves.
	 */
	bool DabGenImportValue(FDabGenContext& Ctx, const FProperty* Property, void* Value, const Lang::FDabExpr& Expr, UObject* Owner,
		FString& OutError, const UEnum* EnumHint = nullptr);
	bool DabGenImportStruct(FDabGenContext& Ctx, const UScriptStruct* Struct, void* Data, const Lang::FDabExpr& Expr, UObject* Owner, FString& OutError);

	enum class EDabGenApply : uint8 { Applied, UnknownProperty, Failed };
	/** Applies Expr to the property at Path (`A.B`, `C[1]`) inside Container, an instance of Struct. */
	EDabGenApply DabGenApplyPath(FDabGenContext& Ctx, const UStruct* Struct, void* Container, const FString& Path, const Lang::FDabExpr& Expr,
		UObject* Owner, FString& OutError);

	/** The default-value text (or object) of a pin or variable of Type holding Expr. */
	bool DabGenMakeDefault(FDabGenContext& Ctx, const FEdGraphPinType& Type, const Lang::FDabExpr& Expr, FString& OutValue, UObject*& OutObject,
		FString& OutError);
	/** Sets a pin's default after the schema has checked it. */
	bool DabGenSetPinDefault(UEdGraphPin* Pin, const FString& Value, UObject* Object, FString& OutError);
	/** Sets a pin's default to a property value. */
	bool DabGenSetPinFromValue(UEdGraphPin* Pin, const FProperty* Property, const void* Value, FString& OutError);

	/** `float`, `int`, `bool`, `FName`, a struct, enum or object type name, `TSubclassOf<X>`, `TArray<X>`. */
	bool DabGenTypeToPinType(const FString& Type, FEdGraphPinType& OutType, FString& OutError);
	/** Metadata values are strings in the engine. */
	FString DabGenMetaString(const Lang::FDabExpr& Expr);
	/** The parameters a caller passes, in declaration order (no return value, no pure outputs). */
	void DabGenGetInputParameters(const UFunction* Function, TArray<FProperty*>& OutParameters);

	/** A property value outside any object, for conversions. */
	class FDabGenTempValue
	{
	public:
		explicit FDabGenTempValue(const FProperty* InProperty);
		~FDabGenTempValue();
		FDabGenTempValue(const FDabGenTempValue&) = delete;
		FDabGenTempValue& operator=(const FDabGenTempValue&) = delete;
		void* Get() const { return Memory; }

	private:
		const FProperty* Property = nullptr;
		void* Memory = nullptr;
	};

	// ------------------------------------------------------------------------------------------
	// Anim graphs (DabGenerateGraph.cpp)
	// ------------------------------------------------------------------------------------------

	/** Creates the nodes of a pose graph with their properties; machines inside are built too. */
	void DabGenCreateGraphNodes(FDabGenContext& Ctx, FDabGenGraph& Graph);
	/** Links pose pins and the result. Run once every graph's nodes exist. */
	void DabGenLinkGraph(FDabGenContext& Ctx, FDabGenGraph& Graph);
	/** Points every Use Cached Pose node at its Save node. */
	void DabGenResolveCacheUses(FDabGenContext& Ctx);
	/** Rebuilds the pins of nodes that call this Blueprint's own layers, after their input poses changed. */
	void DabGenReconstructSelfLayerNodes(FDabGenContext& Ctx);
	/** Places a graph's nodes in columns from the result, so the editor shows something readable. */
	void DabGenLayoutGraph(FDabGenGraph& Graph);

	/** Applies properties to an object (a state node, the Blueprint) by reflection; DAB7001 / DAB7003 for what does not apply. */
	void DabGenApplyObjectProperties(FDabGenContext& Ctx, UObject* Object, const TArray<Model::FDabModelProperty>& Properties, const FString& Location);
	/**
	 * Applies properties to an anim graph node: the runtime struct first, then the editor node. `Tag` is left to
	 * SetTag once the node is placed. Names neither has go to OutUnresolved when given (a linked graph's own
	 * variables, known only once its class is set), else they are reported.
	 */
	void DabGenApplyNodeProperties(FDabGenContext& Ctx, UAnimGraphNode_Base* Node, const TArray<Model::FDabModelProperty>& Properties,
		const FString& Location, bool bReport, TArray<int32>* OutUnresolved);
	/** Copies the model's values of visible pins into their defaults. */
	void DabGenSyncPinDefaults(FDabGenContext& Ctx, UAnimGraphNode_Base* Node, const TArray<Model::FDabModelProperty>& Properties,
		const FString& Location, const TArray<int32>& Unresolved);

	/** Binds Pin of Node to a Property Access path (`{"GetParent", "ViewState", "X"}`). */
	bool DabGenBindPin(FDabGenContext& Ctx, UAnimGraphNode_Base* Node, const FString& PinName, const TArray<FString>& Path, bool bFunction,
		const FString& Location);
	/** A model binding: a path, a slot read, a tuning read or an expression compiled to a function. */
	void DabGenApplyBinding(FDabGenContext& Ctx, UAnimGraphNode_Base* Node, const Model::FDabModelBinding& Binding, const FString& Location,
		const FDabGenScope& Scope);

	/** A pose input pin by its engine name; for linked graphs also by its identifier form (`LocomotionInput`). */
	UEdGraphPin* DabGenFindInputPin(UEdGraphNode* Node, const FString& PinName);

	// ------------------------------------------------------------------------------------------
	// State machines (DabGenerateStateMachine.cpp)
	// ------------------------------------------------------------------------------------------

	/** Builds a machine into the node's graph: states (their graphs are appended to Ctx.Graphs), conduits, aliases, transitions. */
	void DabGenBuildMachine(FDabGenContext& Ctx, UAnimGraphNode_StateMachine* Node, const Model::FDabModelStateMachine& Machine, const FString& Location);

	// ------------------------------------------------------------------------------------------
	// K2 (DabGenerateExpr.cpp)
	// ------------------------------------------------------------------------------------------

	/** The slot getter `DreamAnimAsset_<Slot>`, created on first use. NAME_None after an error. */
	FName DabGenEnsureAssetFunction(FDabGenContext& Ctx, const FString& Slot, const FString& Location);
	/** A thread-safe pure function returning Expr as ReturnType, shared by identical uses. NAME_None after an error. */
	FName DabGenEnsureExprFunction(FDabGenContext& Ctx, const Lang::FDabExpr& Expr, const FEdGraphPinType& ReturnType, const FDabGenScope& Scope,
		const FString& Location);
	/**
	 * The function a state, node or Call Function node refers to for Call: the function itself when it can be
	 * referred to directly (Self, no arguments, a fitting signature), else a generated forwarder.
	 */
	FName DabGenResolveCallFunction(FDabGenContext& Ctx, const Model::FDabModelCall& Call, EDabGenForwarder Kind, const FString& Location);
	/**
	 * Feeds a bool result pin (`bCanEnterTransition`) from a condition: null and literals become the pin's default,
	 * plain paths a Property Access binding, anything else K2 nodes in the result's graph.
	 */
	void DabGenBuildCondition(FDabGenContext& Ctx, UAnimGraphNode_Base* ResultNode, const Lang::FDabExpr* Condition, const FString& GraphPath,
		const FDabGenScope& Scope, const FString& Location);
	/** The AnimNotify events and the animation-set glue of the EventGraph. */
	void DabGenBuildEventGraph(FDabGenContext& Ctx);
}
