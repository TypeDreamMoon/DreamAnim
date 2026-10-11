// Copyright DreamMoon. All Rights Reserved.
//
// What the extractor's files share: one context per extraction, the expression builders, the value
// converter (property -> expression), the K2 lowering and the anim graph reader.

#pragma once

#include "CoreMinimal.h"
#include "Extract/DabExtract.h"

class FProperty;
class UAnimBlueprint;
class UClass;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
class UFunction;
class UK2Node_CallFunction;
class UObject;
class UScriptStruct;
struct FEdGraphPinType;
struct FMemberReference;

namespace UE::DreamAnim::Compiler::ExtractPrivate
{
	using Lang::FDabExprPtr;

	/** State shared by every part of one extraction. */
	struct FDabExtractContext
	{
		FDabExtractContext(const UAnimBlueprint& InBlueprint, Model::FDabModel& InModel, const FDabExtractOptions& InOptions)
			: Blueprint(InBlueprint)
			, OutModel(InModel)
			, Options(InOptions)
		{
		}

		const UAnimBlueprint& Blueprint;
		Model::FDabModel& OutModel;
		const FDabExtractOptions& Options;

		/**
		 * The class paths resolve against: the skeleton class when there is one (it carries the Blueprint's own
		 * variables and functions even when the last compile failed), else the generated class, else the parent.
		 */
		UClass* InstanceClass = nullptr;
		/** Names of the Blueprint's own variables: reads of these are `Props.X`. */
		TSet<FName> BlueprintVariables;
		/** The Blueprint's K2 function graphs by name, for forwarder recognition. */
		TMap<FName, const UEdGraph*> FunctionGraphs;
		/** Validated generated getter -> logical slot, independent of function-name collision suffixes. */
		TMap<FName, FString> AssetSlotFunctions;
		/** Function graphs some state or node function referred to. */
		TSet<FName> UsedFunctions;
		/** Every state machine graph -> its name in the model, unique across the Blueprint. */
		TMap<const UEdGraph*, FString> MachineNames;

		void AddUnsupported(const FString& Location, const FString& Description)
		{
			OutModel.Unsupported.Add({Location, Description});
		}
	};

	// ------------------------------------------------------------------------------------------
	// Expressions and values (DabExtractValues.cpp)
	// ------------------------------------------------------------------------------------------

	FDabExprPtr MakeName(const FString& Name);
	FDabExprPtr MakeMember(FDabExprPtr Object, const FString& Member);
	FDabExprPtr MakeString(const FString& Value);
	FDabExprPtr MakeBool(bool bValue);
	/** Through FormatDabNumber; a negative number is a negation of a literal, as the parser builds it. */
	FDabExprPtr MakeNumber(double Value);
	/** `Callee(Args...)`, positional arguments. */
	FDabExprPtr MakeCall(FDabExprPtr Callee, TArray<FDabExprPtr> Args);
	FDabExprPtr MakeUnary(Lang::EDabUnaryOp Op, FDabExprPtr Operand);
	/** Adds the parentheses precedence needs around either side. */
	FDabExprPtr MakeBinary(Lang::EDabBinaryOp Op, FDabExprPtr Lhs, FDabExprPtr Rhs);
	FDabExprPtr MakeConditional(FDabExprPtr Condition, FDabExprPtr Then, FDabExprPtr Else);
	/** Stands in for something that could not be expressed, so extraction goes on: `Unsupported("What")`. */
	FDabExprPtr MakePlaceholder(const FString& What);

	/** The shortest decimal that reads back as the same float, as a double: 0.2f -> 0.2 rather than 0.200000003. */
	double FloatToShortestDouble(float Value);

	/** `Path(Root, "Rel")` for an asset, the Blueprint's path for a Blueprint class, `"/Script/M.X"` for a native type, `None` for null. */
	FDabExprPtr MakeObjectReference(const UObject* Object);
	/** The same from an object path string (a soft reference). */
	FDabExprPtr MakeObjectPathReference(const FString& ObjectPath);
	/** The asset path the model uses for a class: the Blueprint's object path for a generated class, the class path otherwise. */
	FString GetClassAssetPath(const UClass* Class);

	/**
	 * A property value as an expression. A struct becomes a named tuple of the fields that differ from
	 * Default, or from a default-initialised struct when Default is null (nested structs and array
	 * elements always use the latter). Returns null, after reporting at Location, when the value cannot be
	 * expressed.
	 */
	FDabExprPtr ConvertPropertyValue(FDabExtractContext& Ctx, const FProperty& Property, const void* Value, const void* Default, const FString& Location);

	/** Pin.GetDefaultAsString() imported as Property, then converted. Null when the import fails. */
	FDabExprPtr ConvertPinDefaultAs(FDabExtractContext& Ctx, const UEdGraphPin& Pin, const FProperty& Property, const FString& Location);

	/** A literal from a pin's default by the pin's type alone, for pins no property describes. */
	FDabExprPtr ConvertPinDefaultByType(FDabExtractContext& Ctx, const UEdGraphPin& Pin, const FString& Location);

	/** `float`, `int`, `bool`, `FName`, `Vector`, `AnimSequence`, `TArray<float>` ... */
	FString GetPinTypeSpelling(const FEdGraphPinType& Type);

	/**
	 * A Property Access path (`GetParent`, `ViewState`, `HeadBlendAmount`) as a Self-rooted expression:
	 * functions become calls and the walk continues on what they return. A first segment naming a
	 * Blueprint variable roots at `Props` instead.
	 */
	FDabExprPtr MakeAccessPath(FDabExtractContext& Ctx, const TArray<FString>& Segments, const FString& Location);

	/** A temporary of a property's type, initialised and destroyed with it. */
	class FDabTempValue
	{
	public:
		explicit FDabTempValue(const FProperty& InProperty);
		~FDabTempValue();
		FDabTempValue(const FDabTempValue&) = delete;
		FDabTempValue& operator=(const FDabTempValue&) = delete;

		uint8* Get() const { return Memory; }

	private:
		const FProperty& Property;
		uint8* Memory = nullptr;
	};

	/** A default-initialised struct, destroyed with it. */
	class FDabTempStruct
	{
	public:
		explicit FDabTempStruct(const UScriptStruct& InStruct);
		~FDabTempStruct();
		FDabTempStruct(const FDabTempStruct&) = delete;
		FDabTempStruct& operator=(const FDabTempStruct&) = delete;

		uint8* Get() const { return Memory; }

	private:
		const UScriptStruct& Struct;
		uint8* Memory = nullptr;
	};

	// ------------------------------------------------------------------------------------------
	// Reflection helpers (DabExtractValues.cpp)
	// ------------------------------------------------------------------------------------------

	/** Whether Object's class is, or derives from, the class at ClassPath (`/Script/Module.Class`): for private engine classes. */
	bool IsInstanceOfClassPath(const UObject* Object, const TCHAR* PackageName, const TCHAR* ClassName);

	/** An object-valued UPROPERTY read by name, for private members. */
	UObject* GetObjectPropertyByName(const UObject& Object, FName PropertyName);

	/** The name a state, conduit or alias node has in the model. */
	FString GetStateNodeName(const UEdGraphNode* Node);

	// ------------------------------------------------------------------------------------------
	// K2 lowering (DabExtractK2.cpp)
	// ------------------------------------------------------------------------------------------

	/** One lowering run: the nodes it read and whether anything could not be expressed. */
	struct FDabLowering
	{
		FDabLowering(FDabExtractContext& InCtx, FString InLocation)
			: Ctx(InCtx)
			, Location(MoveTemp(InLocation))
		{
		}

		FDabExtractContext& Ctx;
		/** The graph's location; a node adds `/NodeName`. */
		FString Location;
		TSet<const UEdGraphNode*> Visited;
		int32 Failures = 0;

		void Fail(const UEdGraphNode* Node, const FString& Description);
	};

	/**
	 * The value an input pin receives: the lowered expression of whatever feeds it, or its default. Property,
	 * when known, types the default.
	 */
	FDabExprPtr LowerInputPin(FDabLowering& Lowering, const UEdGraphPin& Pin, const FProperty* Property);

	/** An impure call node as a call: target from its self pin, arguments from its other inputs. */
	TOptional<Model::FDabModelCall> LowerCallNode(FDabLowering& Lowering, const UK2Node_CallFunction& Node);

	/**
	 * A function a state or node refers to: a C++ function on the instance becomes `Self.F()`; a Blueprint
	 * function is read as a forwarder when Options allow. Anything else is reported and unset.
	 */
	TOptional<Model::FDabModelCall> ExtractFunctionReference(FDabExtractContext& Ctx, const FMemberReference& Reference, const FString& Location);

	/** The EventGraph's AnimNotify events into Model.Notifies; everything else in it is reported. */
	void ExtractEventGraph(FDabExtractContext& Ctx, const UEdGraph& Graph);

	// ------------------------------------------------------------------------------------------
	// Anim graphs and state machines (DabExtractGraphs.cpp)
	// ------------------------------------------------------------------------------------------

	/** Names every state machine under Graph (recursively), before any condition refers to one. */
	void CollectStateMachineNames(FDabExtractContext& Ctx, const UEdGraph& Graph);

	/** An anim graph (AnimGraph, a layer, a state) into OutGraph; its state machines go to Ctx.Model.StateMachines. */
	void ExtractAnimGraph(FDabExtractContext& Ctx, const UEdGraph& Graph, const FString& Location, Model::FDabModelGraph& OutGraph);

	/** Only the result node's properties of an anim graph: a layer an interface declares. */
	void ExtractResultProperties(FDabExtractContext& Ctx, const UEdGraph& Graph, const FString& Location, Model::FDabModelGraph& OutGraph);
}
