// Copyright DreamMoon. All Rights Reserved.
//
// The model of a Control Rig made from a .dcr source. Like the Animation Blueprint model (DabModel.h) it is
// where the front end meets, and it is engine-shaped -- shaped like RigVM: graphs of named nodes joined by
// links between pin paths, the hierarchy as a list of elements, member variables, a function library.
//
//   text --parse--> syntax tree --(semantic)--> model --(generate)--> Control Rig
//   Control Rig --(extract)--> model --(normalize)--> model --(lower)--> syntax tree --print--> text
//
// Values are the engine's export text (`(Type=Bone,Name="pelvis")`, `1.500000`, `GlobalSpace`): the
// generator hands them to the controller as they are, the extractor reads them back the same way, and
// lower and semantic convert between them and the text's literals through the engine's reflection. Types
// are the engine's CPP type with the type object's path (`FVector` + `/Script/CoreUObject.Vector`).
//
// The model is literal. What the text's conveniences mean (operators, struct literals with expressions in
// them, `let`, statement order) is already spelled out as nodes, pins and links. Normalization
// (DcrNormalize.h) removes what the text does not keep: Sequence nodes become direct chains, reroutes
// become direct links, values left on linked pins are dropped. None of that changes what the rig does.

#pragma once

#include "CoreMinimal.h"

namespace UE::DreamAnim::Model
{
	/** A RigVM type: `double`; `FVector` with `/Script/CoreUObject.Vector`; `TArray<FRigElementKey>` with `/Script/ControlRig.RigElementKey`. */
	struct FDcrType
	{
		FString CPPType;
		/** The struct, enum or class by object path (of the element type for an array); empty for built-in types. */
		FString ObjectPath;

		bool IsSet() const { return !CPPType.IsEmpty(); }
		bool operator==(const FDcrType& Other) const { return CPPType == Other.CPPType && ObjectPath == Other.ObjectPath; }
		bool operator!=(const FDcrType& Other) const { return !(*this == Other); }
	};

	/** A named engine value in export text: a pin's value, a setting, an element property. */
	struct FDcrValue
	{
		FString Name;
		FString Value;
	};

	enum class EDcrNodeKind : uint8
	{
		/** A unit (RigUnit_*, RigVMFunction_*) by struct, including a template node resolved to that permutation. Events are units. */
		Unit,
		/** A dispatch (RigVMDispatch_*, RigDispatch_*): a template with no struct, by notation, its wildcards in ResolvedTypes. */
		Dispatch,
		VariableGet,
		VariableSet,
		/** A call to a function of this rig's own library. */
		LocalCall,
		/** A call to a public function of another rig. */
		ExternalCall,
		/** A collapse node: an inline subgraph, in Contained. Aggregate nodes are not collapse nodes here (see AggregatePins). */
		Collapse,
		/** The entry and the return of a function's or a collapse node's graph. */
		Entry,
		Return,
		Comment,
		/** A free reroute. Normalization folds these into direct links. */
		Reroute,
		/** Something the extractor could not place; also listed in FDcrModel::Unsupported. */
		Unknown,
	};
	DREAMANIMLANG_API const TCHAR* LexToString(EDcrNodeKind Kind);

	struct FDcrModelGraph;

	struct FDcrModelNode
	{
		FDcrModelNode();
		~FDcrModelNode();
		FDcrModelNode(FDcrModelNode&&);
		FDcrModelNode& operator=(FDcrModelNode&&);

		/** Unique in its graph; links refer to nodes by it. From an asset: the node's name. From text: the `let` name, or a generated one. */
		FString Name;
		EDcrNodeKind Kind = EDcrNodeKind::Unit;
		/** Unit: the struct's object path (`/Script/ControlRig.RigUnit_GetTransform`). For an aggregate node, its unit's. */
		FString StructPath;
		/** Unit whose struct is a permutation of a template, and Dispatch: the template notation (`Add::Execute(in A,in B,out Result)`). */
		FString Notation;
		/**
		 * Dispatch: each wildcard root pin and the type it was resolved to.
		 * Entry and Return of a collapse node's graph: every pin the collapse node shows (execute pins excluded),
		 * in pin order, with its type; their defaults are in Values. A function's Entry and Return leave this
		 * empty: the function's signature says it.
		 */
		TArray<TPair<FString, FDcrType>> ResolvedTypes;
		/**
		 * Root input (and IO) pins whose value differs from what a freshly added node of the same kind holds,
		 * by root pin name, in export text. The values of resolved wildcard pins always appear (they carry the
		 * type). Hidden pins never appear. A linked pin may still carry a value -- the engine keeps it --
		 * until normalization drops it.
		 */
		TArray<FDcrValue> Values;
		/** Pins added to an aggregate node past its own two (`C`, `D` on a four-input Add or Sequence), in order. */
		TArray<FString> AggregatePins;
		/** The node's execute pins by root pin name: those execution comes in by, and those it leaves by (an IO pin is in both). */
		TArray<FString> ExecuteIn;
		TArray<FString> ExecuteOut;
		/** VariableGet / VariableSet: the variable, its type, and whether it is a local variable of the enclosing function. */
		FString Variable;
		FDcrType VariableType;
		bool bLocalVariable = false;
		/** LocalCall / ExternalCall: the function's name. ExternalCall also: the asset holding it, by object path. */
		FString Function;
		FString FunctionHost;
		/** Collapse: the subgraph (with its Entry and Return). */
		TUniquePtr<FDcrModelGraph> Contained;
		/** Comment: the text and the box's size (the box's corner is Position). */
		FString CommentText;
		FVector2D CommentSize = FVector2D::ZeroVector;
		/**
		 * Comment from text (a `#Region`): the nodes the box goes around, by name. The generator fits the box
		 * around them after layout and ignores Position and CommentSize. Empty from an asset.
		 */
		TArray<FString> CommentMembers;
		/** A node injected on a pin: that pin's path (`SphereTrace.Start`), the side, and the node's pins the value passes through. Empty otherwise. */
		FString InjectedOn;
		bool bInjectedAsInput = true;
		FString InjectedInputPin;
		FString InjectedOutputPin;
		/** Layout only; comparisons ignore it. */
		FVector2D Position = FVector2D::ZeroVector;
	};

	/** A link from an output pin to an input pin by pin path (`GetTransform.Transform.Translation.X` -> `Add.A.X`). Execute links included. */
	struct FDcrModelLink
	{
		FString Source;
		FString Target;
	};

	struct FDcrModelGraph
	{
		/** A top-level graph's name (`RigVMModel`, `ConstructionGraph`); a function's name; a collapse node's name. */
		FString Name;
		TArray<FDcrModelNode> Nodes;
		TArray<FDcrModelLink> Links;

		DREAMANIMLANG_API const FDcrModelNode* FindNode(const FString& NodeName) const;
		DREAMANIMLANG_API FDcrModelNode* FindNode(const FString& NodeName);
	};

	struct FDcrModelVariable
	{
		FString Name;
		FDcrType Type;
		/** Export text; empty for the type's default. */
		FString Default;
		bool bPublic = false;
		bool bReadOnly = false;
		FString Category;
		FString Tooltip;
	};

	struct FDcrModelFunctionPin
	{
		FString Name;
		FDcrType Type;
		/** Export text; empty for the type's default. */
		FString Default;
	};

	struct FDcrModelFunction
	{
		FString Name;
		/** Has execute pins (a code block in text); false for a pure function (`= expression;`). */
		bool bMutable = true;
		bool bPublic = false;
		FString Category;
		FString Keywords;
		FString Description;
		TArray<FDcrModelFunctionPin> Inputs;
		TArray<FDcrModelFunctionPin> Outputs;
		TArray<FDcrModelVariable> Locals;
		/** Holds the Entry and Return nodes. */
		FDcrModelGraph Graph;
	};

	enum class EDcrElementKind : uint8 { Bone, Null, Control, Curve, Connector, Socket, Unknown };
	DREAMANIMLANG_API const TCHAR* LexToString(EDcrElementKind Kind);

	struct FDcrModelElement
	{
		EDcrElementKind Kind = EDcrElementKind::Bone;
		FString Name;
		/** The parent element; Parent empty for a top-level one. */
		EDcrElementKind ParentKind = EDcrElementKind::Bone;
		FString Parent;
		/**
		 * Non-default properties in export text, by name:
		 * - Bone: `Transform` (initial, local), `BoneType`;
		 * - Null: `Transform` (initial, local);
		 * - Control: FRigControlSettings properties by their names (`ControlType`, `AnimationType`, `ShapeName`,
		 *   `ShapeColor`, ...), plus `Offset` (the initial offset transform), `Initial` (the initial value) and
		 *   `ShapeTransform`;
		 * - Curve: `Value`;
		 * - Connector: FRigConnectorSettings properties by their names;
		 * - Socket: `Transform`, `Color`, `Description`.
		 */
		TArray<FDcrValue> Properties;
	};

	struct FDcrModelUnsupported
	{
		/** Where, in the rig's terms: `RigVMModel/Node`, `RigVMModel/Node.Pin`, `FunctionLibrary/Func/Node`, `Hierarchy/Bone(x)`. */
		FString Location;
		FString Description;
	};

	enum class EDcrModelAssetKind : uint8 { Independent, Legacy };

	struct FDcrModel
	{
		/** The asset's object path. */
		FString AssetPath;
		EDcrModelAssetKind AssetKind = EDcrModelAssetKind::Independent;
		/** Non-default asset settings in export text, by property name (`PreviewSkeletalMesh`, `ShapeLibraries`, ...). */
		TArray<FDcrValue> Settings;
		/** A rig module. ModuleSettings are its FRigModuleSettings properties in export text. */
		bool bModule = false;
		TArray<FDcrValue> ModuleSettings;
		/** In hierarchy order: parents before their children, siblings in their order. */
		TArray<FDcrModelElement> Hierarchy;
		TArray<FDcrModelVariable> Variables;
		/** The rig's own function library, in library order. */
		TArray<FDcrModelFunction> Functions;
		/** Top-level graphs in the asset's order; the first is the default graph (`RigVMModel`). */
		TArray<FDcrModelGraph> Graphs;
		TArray<FDcrModelUnsupported> Unsupported;
	};

	struct FDcrDumpOptions
	{
		/**
		 * Name nodes by how they are reached -- from the events and entries, along execute links, then data links
		 * into each node's inputs -- instead of by their own names, so two models that differ only in node names
		 * dump the same.
		 */
		bool bCanonicalNodeNames = true;
		/** Read float math units as their double twins (RigVMFunction_MathFloatAdd as RigVMFunction_MathDoubleAdd): the text rebuilds float math in double. */
		bool bFloatAsDouble = false;
		/** Leave out comment nodes. */
		bool bSkipComments = false;
	};

	// The node holds its subgraph by TUniquePtr, so its special members are defined here, where the graph is complete.
	inline FDcrModelNode::FDcrModelNode() = default;
	inline FDcrModelNode::~FDcrModelNode() = default;
	inline FDcrModelNode::FDcrModelNode(FDcrModelNode&&) = default;
	inline FDcrModelNode& FDcrModelNode::operator=(FDcrModelNode&&) = default;

	/**
	 * A canonical text of a model, for goldens and for comparing two models. Positions are left out; settings,
	 * properties, values and links are sorted; nodes come in canonical order. Two models that dump the same
	 * are the same rig as far as DreamAnim is concerned.
	 */
	DREAMANIMLANG_API FString DumpDcrModel(const FDcrModel& Model, const FDcrDumpOptions& Options = FDcrDumpOptions());
}
