// Copyright DreamMoon. All Rights Reserved.
//
// Shared state of the semantic pass. DabSemantic.cpp walks the document, DabSemanticGraph.cpp builds
// pose graphs and state machines, DabSemanticValue.cpp turns written values into model values.
//
// Every value the pass puts into the model must print exactly as the extractor's value for the same
// Blueprint (Extract/DabExtractValues.cpp): the round-trip tests compare DumpDabModel text. So the
// memory -> expression conversion here (ValueToExpr) is the extractor's, and a written value is
// compared with an engine default by converting the default the same way and comparing the printed
// text.

#pragma once

#include "CoreMinimal.h"
#include "Semantic/DabSemantic.h"

class FProperty;
class FStructProperty;
class UClass;
class UEnum;
class UFunction;
class UObject;
class UScriptStruct;
class UStruct;

namespace UE::DreamAnim::Compiler::SemanticPrivate
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	// ------------------------------------------------------------------------------------------
	// Expression builders and inspection (DabSemanticValue.cpp)
	// ------------------------------------------------------------------------------------------

	FDabExprPtr MakeNameExpr(const FString& Name);
	FDabExprPtr MakeStringExpr(const FString& Value);
	FDabExprPtr MakeBoolExpr(bool bValue);
	/** Through FormatDabNumber; a negative number is a negation of its magnitude, as the parser and the extractor build it. */
	FDabExprPtr MakeNumberExpr(double Value);
	FDabExprPtr MakeParenExpr(FDabExprPtr Inner);
	/** Parenthesises an object that is an operator expression, as the extractor does. */
	FDabExprPtr MakeMemberExpr(FDabExprPtr Object, const FString& Member);
	FDabExprPtr MakeUnaryExpr(EDabUnaryOp Op, FDabExprPtr Operand);
	/** Adds the parentheses precedence needs around either side (left-associative), as the extractor does. */
	FDabExprPtr MakeBinaryExpr(EDabBinaryOp Op, FDabExprPtr Lhs, FDabExprPtr Rhs);
	FDabExprPtr MakeConditionalExpr(FDabExprPtr Condition, FDabExprPtr Then, FDabExprPtr Else);
	FDabArgumentPtr MakeArgumentNode(const FString& Name, bool bNameIsString, FDabExprPtr Value);
	FDabExprPtr MakeCallExpr(FDabExprPtr Callee, TArray<FDabArgumentPtr> Args);
	FDabExprPtr MakeListExpr(TArray<FDabExprPtr> Items);

	/** One line, however long: the form two values are compared in. */
	FString PrintValueText(const FDabExpr& Expr);
	/** The expression inside any parentheses the author wrote. */
	const FDabExpr& SkipParens(const FDabExpr& Expr);
	/** A number literal, possibly negated (`-0.5` is a negation in the tree). */
	bool ReadNumber(const FDabExpr& Expr, double& OutValue);
	/** The text of a bare name or a string literal. */
	bool ReadNameOrStringText(const FDabExpr& Expr, FString& OutText);
	/** A call to the bare name Callee: `Path(...)`, `bones(...)`, `Unsupported(...)`. */
	bool IsCallTo(const FDabExpr& Expr, const TCHAR* Callee);
	/** The extractor's stand-in for a value it could not express. It passes through untouched. */
	bool IsPlaceholderCall(const FDabExpr& Expr);
	/** The name a path starts at: `Self` for `Self.GetParent().X`; null when the expression is not a path. */
	const FDabNameExpr* GetPathRoot(const FDabExpr& Expr);
	/** `Props.X` / `Assets.X`: true with X in OutMember. */
	bool IsSectionRead(const FDabExpr& Expr, const TCHAR* Section, FString* OutMember = nullptr);
	bool IsEmptyTupleExpr(const FDabExpr& Expr);

	/** The shortest decimal that reads back as the same float (0.2f -> 0.2), as the extractor writes floats. */
	double FloatToShortestDoubleValue(float Value);

	// ------------------------------------------------------------------------------------------
	// Object references (DabSemanticValue.cpp)
	// ------------------------------------------------------------------------------------------

	/** `Path(Root, "Rel")` for an asset path, the string itself for `/Script/...`, `None` for nothing (the extractor's form). */
	FDabExprPtr MakeObjectPathRef(const FString& ObjectPath);
	/** The same for a loaded object; a Blueprint class is its Blueprint, a native class its `/Script/...` string. */
	FDabExprPtr MakeObjectRef(const UObject* Object);
	/** `Game` / `Engine` / `Plugin.X` (a name, a member or a string) -> the mount point (`Game`, `X`). */
	bool ReadMountRoot(const FDabExpr& Root, FString& OutMount);
	/** `Path(Plugin.ALS, "ALS/X")` -> `/ALS/ALS/X.X`; false with a reason when it is not a well-formed Path. */
	bool TryGetPathCallObjectPath(const FDabCallExpr& Call, FString& OutObjectPath, FString& OutError);
	/** `/A/B.B_C` -> `/A/B.B`: the asset a generated class belongs to. */
	FString NormalizeAssetObjectPath(const FString& ObjectPath);

	// ------------------------------------------------------------------------------------------
	// Value types (DabSemanticValue.cpp)
	// ------------------------------------------------------------------------------------------

	enum class ESemTypeKind : uint8
	{
		Unknown,
		Bool,
		Integer,
		Float,
		Double,
		Enum,
		Name,
		String,
		Text,
		GameplayTag,
		GameplayTagContainer,
		Struct,
		Object,
		Class,
		SoftObject,
		SoftClass,
		Array,
		Set,
		Map,
	};

	/** What a written value has to be: a property's type, or a variable's type as the text spells it. */
	struct FSemType
	{
		ESemTypeKind Kind = ESemTypeKind::Unknown;
		const UEnum* Enum = nullptr;
		const UScriptStruct* Struct = nullptr;
		/** Object and class kinds: the class the referenced object is (or the class a class value derives from); null when any. */
		const UClass* Class = nullptr;
		int64 MinInteger = MIN_int64;
		int64 MaxInteger = MAX_int64;
		/** Array, Set: the element. Map: the key. */
		TSharedPtr<FSemType> Inner;
		/** Map: the value. */
		TSharedPtr<FSemType> Value;

		static FSemType FromProperty(const FProperty& Property);
		/** `float`, `int`, `FName`, `Vector`, `AnimSequence`, `TArray<float>`, `TMap<FName, float>`, ... Unknown when not understood. */
		static FSemType FromSpelling(const FString& Spelling);
	};

	/** A property value in memory as the extractor writes it. Default, when given, is what a top-level struct's fields are compared with. */
	FDabExprPtr ValueToExpr(const FProperty& Property, const void* Value, const void* Default);
	/** The value of a freshly initialised Property: what a bound array element holds. */
	FDabExprPtr ZeroValueExpr(const FProperty& Property);
	/**
	 * Adds Name = Value unless it is the default the extractor drops: an empty tuple, or the same text as
	 * the default value at DefaultValue (when given). A later entry of the same name replaces an earlier one.
	 */
	void AddPropertyUnlessDefault(TArray<FDabModelProperty>& Out, const FString& Name, FDabExprPtr Value, const FProperty* Property, const void* DefaultValue);

	/** A temporary of a property's type, initialised and destroyed with it. */
	class FSemTempValue
	{
	public:
		explicit FSemTempValue(const FProperty& InProperty);
		~FSemTempValue();
		FSemTempValue(const FSemTempValue&) = delete;
		FSemTempValue& operator=(const FSemTempValue&) = delete;
		uint8* Get() const { return Memory; }

	private:
		const FProperty& Property;
		uint8* Memory = nullptr;
	};

	/** A default-initialised struct, destroyed with it. */
	class FSemTempStruct
	{
	public:
		explicit FSemTempStruct(const UScriptStruct& InStruct);
		~FSemTempStruct();
		FSemTempStruct(const FSemTempStruct&) = delete;
		FSemTempStruct& operator=(const FSemTempStruct&) = delete;
		uint8* Get() const { return Memory; }

	private:
		const UScriptStruct& Struct;
		uint8* Memory = nullptr;
	};

	// ------------------------------------------------------------------------------------------
	// Engine reflection the extractor and the pass must agree on (DabSemanticGraph.cpp)
	// ------------------------------------------------------------------------------------------

	bool IsStructNamedOrChildOf(const UStruct* Struct, FName Name);
	/** A pose link field, or an array of them. */
	bool IsPoseLinkProperty(const FProperty& Property);
	/** A runtime-node field the extractor records (not a pose link, not compiler bookkeeping). */
	bool IsRecordedNodeField(const FProperty& Field);
	/** An editor-node property the extractor records (Tag, BoundEnum, ...; not the editor's own view of the node). */
	bool IsRecordedEditorProperty(const FProperty& Property);
	/** An editor-node property a node argument or a machine's metadata may name: what the extractor reads off an editor node. */
	bool IsEditorNodeArgument(const FProperty& Property, const FStructProperty* NodeProperty);
	/** The editor node classes a node type can name: every one but those a newer version of the class replaced. */
	TArray<UClass*> GetSemanticNodeClasses();
	/**
	 * How the text names a node class: without the `AnimGraphNode_` its name ends with when no other class's name
	 * ends the same way (`SequencePlayer`, `GameplayTagsBlend` for `AlsAnimGraphNode_GameplayTagsBlend`), else in full.
	 */
	FString GetSemanticNodeTypeName(const UClass& Class, TConstArrayView<UClass*> Classes);

	// ------------------------------------------------------------------------------------------
	// The pass
	// ------------------------------------------------------------------------------------------

	/** A named pose of one graph: `X = Pose [ meta ];`. */
	struct FPoseDef
	{
		const FDabAssignStmt* Stmt = nullptr;
		bool bCached = false;
		int32 Uses = 0;
		bool bBuilding = false;
		bool bBuilt = false;
		/** The node the name stands for once built (the SaveCachedPose node for a cache). */
		FString RootId;
	};

	/** The graph being built and the named poses visible in it. */
	struct FGraphScope
	{
		FDabModelGraph* Graph = nullptr;
		/** `AnimGraph`, `Layer/X`, `Machine/M/State/S`: the generator's graph path, for provenance. */
		FString GraphPath;
		/** `Graph`, `Layer Overlay`, `StateMachine M, state S`: for messages. */
		FString Location;
		TMap<FString, FPoseDef> Poses;
		TArray<FString> PoseOrder;
		TSet<FString> UsedIds;
	};

	/** What a node class offers to arguments. */
	struct FNodeShape
	{
		UClass* Class = nullptr;
		/** The class default object, the default of every editor-node property. */
		const UObject* Defaults = nullptr;
		/** The editor node's runtime-node property (`Node`), its struct, and its value in the class default object. */
		const FStructProperty* NodeProperty = nullptr;
		const UScriptStruct* Struct = nullptr;
		const void* DefaultStruct = nullptr;
		/** Pose links that are single fields, in field order. */
		TArray<FString> PosePins;
		/** Pose links that are arrays (`Poses`, `BlendPose`). */
		TArray<FString> PoseArrays;
		/** LinkedAnimGraph / LinkedAnimLayer: pose inputs are named after the target's input poses. */
		bool bLinkedGraph = false;
		bool bLinkedLayer = false;
		/** The node exposes properties of another class as pins (linked graphs, Control Rig). */
		bool bCustomProperty = false;
	};

	/** One named value given to a node: a written argument, or what a builtin's positional spelling stands for. */
	struct FNodeValueArg
	{
		FString Name;
		const FDabExpr* Value = nullptr;
		FDabSpan Span;
	};

	/** A node about to be built. */
	struct FNodeRequest
	{
		UClass* Class = nullptr;
		FDabSpan Span;
		TArray<FNodeValueArg> Args;
		TOptional<FDabModelCall> Call;
		FString InputPoseName;
		/** Expressions a builtin made up (Call's `CallSite = On<Site>`); Args point into them. */
		TArray<FDabExprPtr> OwnedValues;
	};

	/** A state machine's shared rules, resolved on first use. */
	struct FRuleSet
	{
		TMap<FString, const FDabAssignStmt*> Statements;
		/** Null for a rule that failed to resolve. */
		TMap<FString, FDabExprPtr> Resolved;
		TSet<FString> Resolving;
	};

	enum class EAssetLookup : uint8
	{
		Found,
		NotFound,
		Ambiguous,
	};

	/** One registered asset, for finding assets by their bare name. */
	struct FSemAssetEntry
	{
		FString ObjectPath;
		/** The asset's class, `/Script/Engine.AnimSequence`. */
		FString ClassPath;
		/** For a Blueprint: the first native class its generated class derives from. */
		FString NativeParentClassPath;
	};

	/** Where a metadata key may land: a struct or class, the container its default is read from, and which of its properties count. */
	struct FMetaTarget
	{
		const UStruct* Owner = nullptr;
		const void* DefaultContainer = nullptr;
		/** A runtime node struct: `Tag` never resolves here (it is the editor node's). */
		bool bRuntimeStruct = false;
		TFunction<bool(const FProperty&)> Accepts;
	};

	class FSemantic
	{
	public:
		FSemantic(const FDabCompilationUnit& InUnit, FDabModel& InModel, FDabDiagnosticSink& InSink, const FDabSemanticOptions& InOptions);

		/** Builds the model; returns false when any error was reported. */
		bool Run();

	private:
		// -- document (DabSemantic.cpp) ---------------------------------------------------------
		void ReadHeader(const FDabDocument& Document);
		void ReadAliases(const FDabBlockExpr& Block);
		void ReadProperties(const FDabBlockExpr& Block);
		void ReadAssets(const FDabBlockExpr& Block);
		void ReadSettings(const FDabBlockExpr& Block);
		void ReadClassDefaults(const FDabBlockExpr& Block);
		void ReadParent(const FDabExpr& Value);
		void ReadNotifies(const FDabBlockExpr& Block);
		void ReadLayer(const FDabNamedBlockStmt& Stmt);
		/** `Layer X [ meta ]` / `Graph = {...} [ meta ]`: the result node's settings and the graph's BlendOptions. */
		void ReadResultMeta(const FDabMeta* Meta, FDabModelGraph& Graph, const FString& What);
		/** An asset path for a setting (`Skeleton`, `Implements`): `Path(...)`, a string, or an asset name. Empty after a diagnostic. */
		FString ReadAssetPathSetting(const FDabExpr& Value, const UClass* ExpectedClass, const FString& What);

		// -- graphs (DabSemanticGraph.cpp) -------------------------------------------------------
		// -- metadata and settings onto engine properties (DabSemanticGraph.cpp) -------------------
		bool FindMetaProperty(TConstArrayView<FMetaTarget> Targets, const FString& Key, const FProperty*& OutProperty, const void*& OutDefault) const;
		/** One constant (or a flag, Value null) onto Property, recorded unless it is the default. */
		void ReadMetaValue(const FProperty& Property, const FDabExpr* Value, const FDabSpan& Span, const void* Default,
			TArray<FDabModelProperty>& Out, const FString& What);
		void ReadMetaProperties(const FDabMeta* Meta, TConstArrayView<FMetaTarget> Targets, TArray<FDabModelProperty>& Out, const FString& What);

		/** Every `[ Cached ]` name and every named pose of every graph, before any graph is built. */
		void CollectGraphNames(const TArray<FDabStmtPtr>& Statements, const FString& Location, const TSet<FString>& Reserved);
		/** The `Input(X)` names each of the file's own layers declares, for linked layers that run them. */
		void CollectLayerInputs(const FDabNamedBlockStmt& Stmt);
		void BuildGraph(const TArray<FDabStmtPtr>& Statements, FDabModelGraph& Graph, const FString& Location, const TCHAR* ResultName,
			const TSet<FString>& Reserved, const FDabSpan& OwnerSpan);
		void BuildValueGraph(const FDabExpr& Value, FDabModelGraph& Graph, const FString& Location);
		/** A pose expression -> the id of its root node; empty for `None` or after an error. Feed, when set, makes the pose a stack step fed by *Feed. */
		FString BuildPose(const FDabExpr& Expr, FGraphScope& Scope, const FString& IdHint, const FString* Feed);
		FString BuildPoseName(const FDabNameExpr& Name, FGraphScope& Scope, const FString& IdHint);
		FString BuildNamedPose(const FString& Name, FPoseDef& Def, FGraphScope& Scope, const FDabSpan& UseSpan);
		void BuildCacheDefinition(const FString& Name, FPoseDef& Def, FGraphScope& Scope);
		FString AddSimpleNode(FGraphScope& Scope, const FString& IdHint, const TCHAR* ClassName, const FString& CacheName, const FString& MachineName);
		FString BuildCallNode(const FDabCallExpr& Call, FGraphScope& Scope, const FString& IdHint, const FString* Feed);
		FString BuildAnimNode(FNodeRequest& Request, FGraphScope& Scope, const FString& IdHint, const FString* Feed);
		void ApplyPoseMeta(const FDabMeta* Meta, const FString& RootId, FGraphScope& Scope, const FString& PoseName);
		void BuildStateMachine(const FDabNamedBlockStmt& Stmt);
		void BuildState(const FDabNamedBlockStmt& Stmt, const FString& MachineName, FDabModelState& OutState);
		void ReadTransitions(const FDabBlockExpr& Block, const FString& MachineName, const TMap<FString, int32>& Members, FRuleSet& Rules,
			FDabModelStateMachine& Machine);
		void ReadTransitionOptions(const FDabMeta* Meta, FDabModelTransition& Transition, const FString& What);
		const FDabExpr* ResolveRule(FRuleSet& Rules, const FString& Name, const FDabSpan& UseSpan);
		void CheckMachineUses();
		/** A cache used outside the graph that saves it and that graph's machines: DAB6017. */
		void CheckCacheUses();
		/** The AnimGraph or layer a graph path belongs to, through the machines that run its states; empty when unknown. */
		FString FindRootGraph(const FString& GraphPath) const;

		/** The node class a type name means; null after DAB3001/DAB3002. */
		UClass* ResolveNodeClass(const FString& TypeName, const FDabSpan& Span);
		UClass* FindNodeClassByName(const TCHAR* ClassName) const;
		/** A `Template` the file or one of its libraries defines. */
		bool IsTemplateName(const FString& Name) const;
		const FNodeShape* GetShape(UClass* Class);
		/** The runtime-node field or editor-node property an argument names, and its default; null when none. */
		const FProperty* FindNodeProperty(const FNodeShape& Shape, const FString& Name, const void*& OutDefault, bool& bOutEditor) const;
		/** The input pose names (made identifiers) of the graph a linked graph / layer node runs; false when the target is unknown. */
		bool GetLinkedInputPoses(const FDabModelNode& Node, TArray<FString>& OutNames);
		/** The class a reference value (`Path(...)`, a `/Script/...` string) names, loaded; null when it cannot be. */
		UClass* LoadReferencedClass(const FDabExpr& Reference);
		/** The enum a BlendListByEnum switches on: `BoundEnum`, or the type of the path bound to `ActiveEnumValue`. */
		const UEnum* FindBlendEnum(const FNodeRequest& Request);

		// -- values (DabSemanticValue.cpp) ------------------------------------------------------
		/**
		 * A written value that is read at run time: aliases expanded, numbers canonical, negated numbers folded,
		 * only the parentheses precedence needs. Rules, when given, replaces rule names (not inside the
		 * arguments of StateWeight / MachineWeight). Null after a diagnostic.
		 */
		FDabExprPtr Resolve(const FDabExpr& Expr, FRuleSet* Rules = nullptr, bool bInGetter = false);
		/** True when the value needs nothing at run time: literals, enum names, `Path(...)`, lists and tuples of those. */
		bool IsConstant(const FDabExpr& Expr) const;
		/** A constant in the model form of Type. StructDefault: what a top-level struct's fields compare with. Null after a diagnostic. */
		FDabExprPtr ConvertTyped(const FSemType& Type, const FDabExpr& Expr, const void* StructDefault, const FString& What);
		FDabExprPtr ConvertProperty(const FProperty& Property, const FDabExpr& Expr, const void* DefaultValue, const FString& What);
		FDabExprPtr ConvertStructValue(const UScriptStruct& Struct, const FDabExpr& Expr, const void* Default, const FString& What);
		FDabExprPtr ConvertObjectValue(const FSemType& Type, const FDabExpr& Expr, const FString& What);
		FDabExprPtr ConvertEnumValue(const UEnum& Enum, const FDabExpr& Expr, const FString& What);
		/** A constant whose type is not known: canonical numbers and paths, everything else as written. */
		FDabExprPtr ConvertUntyped(const FDabExpr& Expr);
		/** A binding value; checks `Props.X` and `Assets.X`. */
		FDabExprPtr ConvertBinding(const FDabExpr& Expr) { return Resolve(Expr); }
		/** `Target.Function(args)` (or `Target.Function` with no arguments) -> a model call; false after a diagnostic. */
		bool ConvertCall(const FDabExpr& Expr, FDabModelCall& OutCall, const FString& What);
		/** The callee of a call: its Self-rooted target, the function's name and, when the target's class has it, the function. */
		bool ResolveCallee(const FDabExpr& Callee, FDabExprPtr& OutTarget, FString& OutFunction, const UFunction*& OutUFunction, const FString& What);
		void ConvertCallArguments(const UFunction* Function, TConstArrayView<const FDabArgument*> Args, TArray<FDabModelArgument>& Out, const FString& What);
		/** The type a Self-rooted path reads: the struct or class it lands in and the last property. */
		bool TypePath(const FDabExpr& Expr, const UStruct*& OutScope, const FProperty*& OutProperty) const;
		/**
		 * An asset by its bare name: DreamAnim's own outputs first, then every registered asset of that name
		 * whose class derives from ExpectedClass or, for a class value (MetaClass set), every Blueprint whose
		 * generated class derives from MetaClass. Ambiguous when more than one matches.
		 */
		EAssetLookup FindAssetByName(const FString& Name, const UClass* ExpectedClass, const UClass* MetaClass, FString& OutObjectPath);
		/** A native class by name, with or without its prefix (`AlsAnimationInstance`, `UAlsAnimationInstance`), or by `/Script/...` path. */
		UClass* FindNativeClass(const FString& Name) const;
		/** The default a `Props.X` read stands for where only a constant fits; null after a diagnostic. */
		const FDabExpr* FindPropsDefault(const FDabExpr& Expr);
		void ReportTypeMismatch(const FSemType& Type, const FDabExpr& Expr, const FString& What, const FString& Detail = FString());

		FString MakeNodeId(FGraphScope& Scope, const FString& Hint) const;
		FDabModelNode* FindNodeMutable(FGraphScope& Scope, const FString& Id) const;

	public:
		const FDabCompilationUnit& Unit;
		FDabModel& Model;
		FDabDiagnosticSink& Sink;
		const FDabSemanticOptions& Options;

		EDabDocumentKind Kind = EDabDocumentKind::Unknown;
		/** Asset name of the Blueprint being built (`AB_Als_Standing`). */
		FString BlueprintName;
		/** The graph path the next BuildGraph / BuildValueGraph builds (see FGraphScope::GraphPath). */
		FString CurrentGraphPath;
		/** The parent class, for typing `Self` paths; null when unknown. */
		UClass* SelfClass = nullptr;

		/** `Main` -> `Self.GetParent()` (resolved). */
		TMap<FString, FDabExprPtr> Aliases;
		/** Variable name -> index into Model.Variables. */
		TMap<FString, int32> VariableIndex;
		TSet<FString> SlotNames;

		/** Cache name -> the graph location that defines it. */
		TMap<FString, FString> CacheOwners;
		/** Cache name -> the graph path that saves it, once built. */
		TMap<FString, FString> CacheGraphs;
		/** Every use of a cache from another graph than the one that saves it: checked once every graph is built. */
		struct FCacheUse
		{
			FString Name;
			FString GraphPath;
			FDabSpan Span;
		};
		TArray<FCacheUse> CacheUses;
		/** Machine name -> the graph path that runs it. */
		TMap<FString, FString> MachineGraphs;
		/** Every non-cached named pose of every graph -> its graph, to say why a name is not visible elsewhere. */
		TMap<FString, FString> PoseOwners;
		/** State machine statements by name, how often a pose names each, and the first use. */
		TMap<FString, const FDabNamedBlockStmt*> MachineStmts;
		TArray<FString> MachineOrder;
		TMap<FString, int32> MachineUses;
		/** Layer name -> the `Input(X)` names its graph declares. */
		TMap<FString, TArray<FString>> LayerInputs;

		TMap<FString, UClass*> NodeClassesByType;
		TArray<UClass*> NodeClasses;
		bool bNodeClassesCollected = false;
		TMap<const UClass*, TUniquePtr<FNodeShape>> Shapes;
		/** Reference text (`/ALS/X.X`) -> the class it names, loaded once per run. */
		TMap<FString, UClass*> LoadedClasses;
		/** Class path -> the class, when loaded. */
		TMap<FString, UClass*> ClassesByPath;

		/** Asset name -> every registered asset of that name, built once per run on the first bare-name lookup. */
		TMap<FString, TArray<FSemAssetEntry>> AssetIndex;
		bool bAssetIndexBuilt = false;
	};
}
