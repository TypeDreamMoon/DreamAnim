// Copyright DreamMoon. All Rights Reserved.
//
// The model: what an Animation Blueprint authored in DreamAnim holds, independent of how the text
// spells it and of the editor's object graph. Everything in the plugin meets here:
//
//   text --parse--> syntax tree --(semantic check)--> model --(generate)--> Animation Blueprint
//   Animation Blueprint --(extract)--> model --(lower)--> syntax tree --print--> text   (the decompiler)
//
// and the write-back compares the model extracted from an edited Blueprint with the model the text
// produces. The model is engine-shaped: node classes, property names and pin names are the engine's.
// The conveniences of the text (ModifyCurve(Curve = 1), enum-keyed blend maps, ExplicitFrame implying
// bUseExplicitFrame, aliases, templates) are applied on the way in and recovered on the way out, so the
// generator and the extractor stay literal.
//
// Values -- node properties, call arguments, conditions -- are syntax-tree expressions, so one printer
// serves logs, goldens and the decompiler. Two values are equal when they print the same; numbers
// therefore always go through FormatDabNumber, and asset references are always `Path(Root, "Rel")`.
// Paths that read C++ state are rooted at `Self` (`Self.GetParent().ViewState.HeadBlendAmount`); the
// text's aliases are expanded before a value reaches the model.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabAst.h"

namespace UE::DreamAnim::Model
{
	/** Shortest text that reads back as the same double: `0.2`, `1`, `-0.5`, `1e-06`. Used for every number in a model. */
	DREAMANIMLANG_API FString FormatDabNumber(double Value);

	/** A named value: a node property, a transition option, a class setting, a metadata entry. */
	struct FDabModelProperty
	{
		/**
		 * The engine's property name. A struct is one property holding a named tuple of its non-default fields;
		 * an array is one property holding a list (an element fed by a binding holds its type's zero value).
		 */
		FString Name;
		Lang::FDabExprPtr Value;
	};

	struct FDabModelArgument
	{
		/** Empty for a positional argument. */
		FString Name;
		Lang::FDabExprPtr Value;
	};

	/** A call into C++: `Self.GetParent().PlayTransitionAnimation(Path(...), PlayRate = 1.5)`. */
	struct FDabModelCall
	{
		/** The object the function is called on, rooted at Self: `Self`, `Self.GetParent()`. */
		Lang::FDabExprPtr Target;
		FString Function;
		/** Constant values only (literals, Path(...), asset slot reads `Assets.X`, tuning reads `Props.X`). */
		TArray<FDabModelArgument> Args;
	};

	/** A pin fed every update instead of holding a constant. */
	struct FDabModelBinding
	{
		/** The engine pin name: `Alpha`, `DesiredAlphas_2`, `RigInput` (custom pins by their property name). */
		FString PinName;
		/**
		 * The value: a Self-rooted path (`Self.GetParent().ViewState.HeadBlendAmount`), compiled to a Property
		 * Access binding; a slot read (`Assets.Look`) or a tuning read (`Props.StopBlend`); or any other
		 * expression, compiled to a generated thread-safe function the pin binds to.
		 */
		Lang::FDabExprPtr Value;
	};

	/** A pose pin fed by another node of the same graph. */
	struct FDabModelPoseLink
	{
		/** The engine pin name: `A`, `Base`, `Source`, `Poses_2`, `BlendPose_1`, or a linked graph's input pose name. */
		FString PinName;
		FString SourceNodeId;
	};

	/** A function hung on a node: InitialUpdateFunction, BecomeRelevantFunction or UpdateFunction. */
	struct FDabModelNodeFunction
	{
		/** `InitialUpdate`, `BecomeRelevant` or `Update`. */
		FString Site;
		FDabModelCall Call;
	};

	struct FDabModelNode
	{
		/**
		 * Unique within its graph. From text: the author's name for a named pose, otherwise a path built
		 * from the consuming node and pin (`Output/Base/Additive`). From an extracted Blueprint: whatever the
		 * extractor chose. Only used to link nodes and to derive GUIDs; comparisons ignore it.
		 */
		FString Id;
		/** The editor node's NodeGuid, set by the extractor; write-back matches nodes by it. Comparisons ignore it. */
		FGuid Guid;
		/** The editor node class without its U prefix: `AnimGraphNode_TwoWayBlend`, `AlsAnimGraphNode_GameplayTagsBlend`. */
		FString ClassName;
		/** Non-default values: fields of the runtime node struct (FAnimNode_*) and properties of the editor node (`Tag`). */
		TArray<FDabModelProperty> Properties;
		TArray<FDabModelBinding> Bindings;
		TArray<FDabModelPoseLink> PoseInputs;
		TArray<FDabModelNodeFunction> NodeFunctions;

		/** AnimGraphNode_SaveCachedPose and AnimGraphNode_UseCachedPose: the cache's name. */
		FString CacheName;
		/** AnimGraphNode_StateMachine: the machine it runs (FDabModel::StateMachines by name). */
		FString StateMachineName;
		/** AnimGraphNode_LinkedInputPose: the input pose's name. */
		FString InputPoseName;
		/** AnimGraphNode_CallFunction: what it calls. Its call site is the `CallSite` property. */
		TOptional<FDabModelCall> Call;
	};

	/**
	 * A `#Region "Label"` of a graph or a state machine: the generator draws a comment box titled Label
	 * around what its statements made. From text only: the extractor does not read comment boxes back, and
	 * comparisons ignore regions.
	 */
	struct FDabModelRegion
	{
		FString Label;
		/** A graph's named poses (the result by its statement's name, `Output` or `Pose`); a machine's states, conduits and aliases. */
		TArray<FString> Members;
	};

	struct FDabModelGraph
	{
		/** `AnimGraph`, a layer's name, or a state's name. */
		FString Name;
		TArray<FDabModelNode> Nodes;
		/** The node feeding the graph's result (Root / StateResult / layer root); empty when nothing is linked. */
		FString ResultNodeId;
		/** Properties of the result node itself (`LayerGroup` on a layer's root). */
		TArray<FDabModelProperty> ResultProperties;
		/**
		 * The input poses of the AnimGraph or a layer, in the order the compiled function takes them: a linked
		 * graph node feeds its pose pins to them by position, so the order is behaviour.
		 */
		TArray<FString> InputPoses;
		TArray<FDabModelRegion> Regions;

		DREAMANIMLANG_API const FDabModelNode* FindNode(const FString& Id) const;
	};

	struct FDabModelState
	{
		FString Name;
		/** The editor node's NodeGuid, set by the extractor; write-back matches nodes by it. Comparisons ignore it. */
		FGuid Guid;
		/** Properties of the state node: `bAlwaysResetOnEntry`, notification events. */
		TArray<FDabModelProperty> Properties;
		TOptional<FDabModelCall> OnEnter;
		TOptional<FDabModelCall> OnExit;
		TOptional<FDabModelCall> OnBlended;
		FDabModelGraph Graph;
	};

	struct FDabModelConduit
	{
		FString Name;
		/** The editor node's NodeGuid, set by the extractor; write-back matches nodes by it. Comparisons ignore it. */
		FGuid Guid;
		/** Null means `true`. */
		Lang::FDabExprPtr Condition;
	};

	struct FDabModelAlias
	{
		FString Name;
		/** The editor node's NodeGuid, set by the extractor; write-back matches nodes by it. Comparisons ignore it. */
		FGuid Guid;
		bool bGlobal = false;
		TArray<FString> States;
	};

	enum class EDabConditionKind : uint8
	{
		/** Condition holds a boolean expression (a constant `true`/`false` is an expression too). */
		Expression,
		/** bAutomaticRuleBasedOnSequencePlayerInState. */
		Auto,
	};

	struct FDabModelTransition
	{
		FString From;
		FString To;
		/** The editor node's NodeGuid, set by the extractor; write-back matches nodes by it. Comparisons ignore it. */
		FGuid Guid;
		EDabConditionKind ConditionKind = EDabConditionKind::Expression;
		Lang::FDabExprPtr Condition;
		/** The shared rule this transition uses, when its rule graph is shared (the text's `Rules` entry). */
		FString SharedRuleName;
		/** Non-default properties of the transition node: CrossfadeDuration, BlendMode, LogicType, PriorityOrder, BlendProfile, ... */
		TArray<FDabModelProperty> Options;
	};

	struct FDabModelStateMachine
	{
		/** Unique within the Blueprint. */
		FString Name;
		/** Non-default fields of FAnimNode_StateMachine and of the editor node. */
		TArray<FDabModelProperty> Properties;
		FString Entry;
		TArray<FDabModelState> States;
		TArray<FDabModelConduit> Conduits;
		TArray<FDabModelAlias> Aliases;
		TArray<FDabModelTransition> Transitions;
		TArray<FDabModelRegion> Regions;
	};

	/** A tuning value (`Properties = { float StopBlend = 0.3; }`): a Blueprint variable the graph only reads. */
	struct FDabModelVariable
	{
		FString Name;
		/** As the text spells it: `float`, `int`, `bool`, `FName`, a struct or object type name. */
		FString Type;
		Lang::FDabExprPtr Default;
		TArray<FDabModelProperty> Meta;
	};

	/** An animation-set slot (`Assets = { AnimSequence Idle; }`). */
	struct FDabModelAssetSlot
	{
		FString Name;
		FString Type;
	};

	struct FDabModelNotify
	{
		/** Without the `AnimNotify_` prefix. */
		FString Name;
		TArray<FDabModelCall> Calls;
	};

	struct FDabModelLayer
	{
		FString Name;
		/** True for a layer only declared (in an interface): no graph. */
		bool bDeclarationOnly = false;
		FDabModelGraph Graph;
	};

	/** Something the extractor found in a Blueprint that the language cannot say. */
	struct FDabModelUnsupported
	{
		/** Where, in the Blueprint's terms: `EventGraph/K2Node_CallFunction_3`, `AnimGraph/AnimGraphNode_X_2.PinName`. */
		FString Location;
		FString Description;
	};

	struct FDabModel
	{
		Lang::EDabDocumentKind Kind = Lang::EDabDocumentKind::AnimBlueprint;
		/** The asset's object path: `/ALS/ALS/Character/AB_Als.AB_Als`. */
		FString AssetPath;
		/** The parent class's path (`/Script/ALS.AlsAnimationInstance`). */
		FString ParentClass;
		/** The target skeleton's object path. */
		FString Skeleton;
		/** Implemented layer interfaces, as asset object paths. */
		TArray<FString> Interfaces;
		/** Non-default UAnimBlueprint settings. */
		TArray<FDabModelProperty> ClassSettings;
		/** Editable properties of the parent class this Blueprint's class defaults change (`Settings` on an ALS instance). */
		TArray<FDabModelProperty> ClassDefaults;
		/** `DefaultAnimSet`, as an asset object path; empty when none. */
		FString DefaultAnimSet;

		TArray<FDabModelVariable> Variables;
		TArray<FDabModelAssetSlot> AssetSlots;
		TArray<FDabModelNotify> Notifies;

		FDabModelGraph AnimGraph;
		TArray<FDabModelLayer> Layers;
		/** Every machine of the Blueprint, flat; machines nested in a state are referenced by name from its StateMachine node. */
		TArray<FDabModelStateMachine> StateMachines;

		TArray<FDabModelUnsupported> Unsupported;

		DREAMANIMLANG_API const FDabModelStateMachine* FindStateMachine(const FString& Name) const;
	};

	/**
	 * A canonical text of a model, for goldens and for comparing two models. Graphs are walked from their
	 * result node through pose links (so node ids and node order do not matter); properties, bindings,
	 * options and settings are sorted by name; values print through the language printer. Two models that
	 * dump the same are the same Blueprint as far as DreamAnim is concerned.
	 */
	DREAMANIMLANG_API FString DumpDabModel(const FDabModel& Model);
}
