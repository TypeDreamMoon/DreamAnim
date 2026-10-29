// Copyright DreamMoon. All Rights Reserved.
//
// The names the .dcr text and the rig use for the same things, in one place, so lowering and semantic agree:
// event words, the short names of units and dispatches, operators and the nodes they stand for, type names,
// asset references. Each "write" function here has a "read" function, and lowering only writes what reads
// back to the same thing.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabAst.h"
#include "Model/DcrModel.h"

class UScriptStruct;
class UClass;

namespace UE::DreamAnim::Rig
{
	// ------------------------------------------------------------------------------------------
	// Events
	// ------------------------------------------------------------------------------------------

	/** An event the text names with a word (`Event Forward`). Any other word is a user-defined event of that name. */
	struct FDcrEventWord
	{
		const TCHAR* Word;
		/** The event unit's struct path. */
		const TCHAR* StructPath;
		/** The name the engine runs it by. */
		const TCHAR* EventName;
	};

	TConstArrayView<FDcrEventWord> GetDcrEventWords();
	const FDcrEventWord* FindDcrEventByWord(const FString& Word);
	const FDcrEventWord* FindDcrEventByStruct(const FString& StructPath);
	/** By the name the engine runs it by (`Backwards Solve`). */
	const FDcrEventWord* FindDcrEventByEventName(const FString& EventName);

	/** The unit of a user-defined event; its `EventName` pin holds the name. */
	extern const TCHAR* const DcrUserEventStructPath;
	extern const TCHAR* const DcrUserEventNamePin;

	/**
	 * `RunEvent(Backward);`: runs another event of the rig where it stands on the chain (an InvokeEntry node).
	 * The argument is the event as `Event` writes it: a word, or a user event's name.
	 */
	extern const TCHAR* const DcrRunEventCallee;

	// ------------------------------------------------------------------------------------------
	// Units and dispatches
	// ------------------------------------------------------------------------------------------

	/** A unit or dispatch struct the text can call. */
	struct FDcrNodeType
	{
		const UScriptStruct* Struct = nullptr;
		/** True for a dispatch factory (RigVMDispatch_*, RigDispatch_*). */
		bool bDispatch = false;
		bool bDeprecated = false;
	};

	/** `RigUnit_GetTransform` -> `GetTransform`, `RigVMDispatch_If` -> `If`: the struct name without its prefix. */
	FString GetDcrShortName(const FString& StructName);

	/** Every unit and dispatch the text can call, sorted by struct name (deprecated ones included and marked). */
	TArray<FDcrNodeType> GetDcrNodeTypes();

	/** Units by short name or full name (`GetTransform`, `RigUnit_GetTransform`). Non-deprecated ones hide deprecated ones of the same short name. */
	TArray<FDcrNodeType> FindDcrUnits(const FString& Name);
	/** Dispatches the same way (`If`, `RigVMDispatch_If`). */
	TArray<FDcrNodeType> FindDcrDispatches(const FString& Name);
	/** Template notations whose template name, spaces removed, is Name (`SetTransform` -> the `Set Transform` template). */
	TArray<FName> FindDcrTemplateNotations(const FString& Name);

	/**
	 * The name the text calls a unit or dispatch by: its short name when that finds this struct alone among units
	 * and dispatches, its full name (without the F) otherwise. Callers also check the rig's own function names.
	 */
	FString GetDcrNodeTypeName(const UScriptStruct& Struct);

	/** A dispatch's factory struct from a template notation; null when the notation is not a dispatch's. */
	const UScriptStruct* FindDcrDispatchStruct(const FString& Notation);
	/** A dispatch factory's template notation; empty when Struct is not a dispatch. */
	FString GetDcrDispatchNotation(const UScriptStruct& Struct);

	// ------------------------------------------------------------------------------------------
	// Operators
	// ------------------------------------------------------------------------------------------

	enum class EDcrOperatorKind : uint8 { Binary, Unary, Conditional, Index };

	/** An operator and the node pins its operands go to. */
	struct FDcrOperator
	{
		EDcrOperatorKind Kind = EDcrOperatorKind::Binary;
		Lang::EDabBinaryOp Binary = Lang::EDabBinaryOp::Add;
		Lang::EDabUnaryOp Unary = Lang::EDabUnaryOp::Negate;
		/** A unit's template name (`Add`, `Scale`), or a dispatch's struct name (`RigVMDispatch_If`). */
		const TCHAR* NodeName = nullptr;
		bool bDispatch = false;
		/** The operand pins in operand order (`A`, `B`; `Value`; `Condition`, `True`, `False`; `Array`, `Index`) and the result pin. */
		TArray<const TCHAR*, TInlineAllocator<3>> Operands;
		const TCHAR* Result = nullptr;
	};

	/** The nodes a binary operator can stand for, in the order semantic tries them (`*`: Multiply, then Scale). */
	TArray<const FDcrOperator*> GetDcrBinaryOperators(Lang::EDabBinaryOp Op);
	const FDcrOperator* GetDcrUnaryOperator(Lang::EDabUnaryOp Op);
	const FDcrOperator& GetDcrConditionalOperator();
	const FDcrOperator& GetDcrIndexOperator();

	/**
	 * The operator a unit or dispatch can be written as, by its template name or dispatch struct; null for any
	 * other. This is only the candidate: lowering still checks that semantic picks the same struct back for the
	 * node's operand types.
	 */
	const FDcrOperator* FindDcrOperatorForStruct(const UScriptStruct& Struct);

	/** `RigVMFunction_MathFloatAdd` -> `RigVMFunction_MathDoubleAdd`: the double twin a float math unit is rebuilt as; null when there is none. */
	const UScriptStruct* GetDcrDoubleTwin(const UScriptStruct& Struct);

	/** What is known about an operand when an operator's unit is chosen: its type, or else the constant literal it is. */
	struct FDcrOperand
	{
		Model::FDcrType Type;
		const Lang::FDabExpr* Literal = nullptr;
		/** For a literal that holds expressions (`(X = a, Y = b, Z = c)`): whether it can be written as the type. Unset: CanDcrLiteralBe. */
		TFunction<bool(const Model::FDcrType&)> Fits;
	};

	struct FDcrOperatorChoice
	{
		const FDcrOperator* Operator = nullptr;
		const UScriptStruct* Unit = nullptr;
	};

	/**
	 * The unit an operator builds over these operands: among the permutations of the candidates' templates, one
	 * whose operand pins take the operands -- a typed operand by its exact type, or float for double and back; a
	 * literal when it can be written into the pin. Fewest float/double swaps wins, then the most double pins, then
	 * the candidate order. Nothing for a dispatch candidate (equality), which resolves by type as any dispatch does.
	 * With Result, a unit whose result cannot go there (not that type, nor float for double and back) is passed
	 * over while another fits: `(1, 1, 1) * Scale` written for a vector is not a quaternion's scale. A float unit
	 * comes back as its double twin.
	 */
	FDcrOperatorChoice ResolveDcrOperatorUnit(TConstArrayView<const FDcrOperator*> Candidates, TConstArrayView<FDcrOperand> Operands,
		const Model::FDcrType* Result = nullptr);

	/** Whether a constant literal can be written into a value of Type (enum names, `None`, tuples and lists included). */
	bool CanDcrLiteralBe(const Lang::FDabExpr& Literal, const Model::FDcrType& Type);

	// ------------------------------------------------------------------------------------------
	// Pins
	// ------------------------------------------------------------------------------------------

	/** A pin of a unit or a dispatch as the text sees it. Execute and hidden pins are not pins here. */
	struct FDcrPin
	{
		FString Name;
		/** Takes a value or a link: Input, IO and Visible pins. */
		bool bInput = false;
		/** Gives a value: Output and IO pins. */
		bool bOutput = false;
		/** A unit's member; null for a dispatch's argument. */
		const FProperty* Property = nullptr;
		/** The pin's type when it has one fixed type; unset for a dispatch's wildcard. */
		Model::FDcrType Type;
	};

	/** A unit's or a dispatch's pins in declaration order. */
	TArray<FDcrPin> GetDcrNodePins(const UScriptStruct& Struct);

	/** A trait struct's name as the text writes it: `RigVMTrait_SetDeformerTransformVariable` -> `SetDeformerTransformVariable`. */
	FString GetDcrTraitTypeName(const UScriptStruct& Struct);
	/** The trait struct (a child of FRigVMTrait) called Name, by its short or its full name; null when none is, or several are. */
	const UScriptStruct* FindDcrTraitStruct(const FString& Name);
	/** Whether a unit member is an execute pin (an execute context or execute pin struct). */
	bool IsDcrExecuteProperty(const FProperty& Property);

	// ------------------------------------------------------------------------------------------
	// Types
	// ------------------------------------------------------------------------------------------

	/**
	 * The text's spelling of a type: `double`, `FName`, `Vector`, `RigControlAxis`, a user struct's asset name,
	 * `SkeletalMesh` for an object, `Array<RigElementKey>`. Empty when the text cannot spell it.
	 */
	FString GetDcrTypeName(const Model::FDcrType& Type);
	/** The type a spelling names (the F and E prefixes may be written or not; `TArray<T>` reads as `Array<T>`). False, with OutError, when none. */
	bool FindDcrType(const FString& Spelling, Model::FDcrType& OutType, FString& OutError);
	/** The model type of a type object (struct, enum, class) as RigVM spells it, optionally as an array. */
	Model::FDcrType MakeDcrType(const UObject& TypeObject, bool bArray = false);
	/** A built-in type (`bool`, `int32`, `double`, `FName`, ...), optionally as an array. */
	Model::FDcrType MakeDcrBuiltinType(const TCHAR* CPPType, bool bArray = false);

	// ------------------------------------------------------------------------------------------
	// Assets
	// ------------------------------------------------------------------------------------------

	/**
	 * Registered assets named Name whose class is Class or derives from it (any class when null), by object path.
	 * Redirectors are left out. The index behind it follows the asset registry's added, removed and renamed events.
	 */
	TArray<FString> FindDcrAssetsByName(const FString& Name, const UClass* Class = nullptr);

	/** An asset as the text names it: its bare name when no other registered asset has that name, `Path(Root, "Rel")` otherwise; `None` for none. */
	Lang::FDabExprPtr MakeDcrAssetExpr(const FSoftObjectPath& Path);
	/** `Path(Plugin.X, "A/B")` -> `/X/A/B.B`; empty for anything else. */
	FString GetDcrPathExprObjectPath(const Lang::FDabExpr& Expr);
	/** `/Game/A/B.B` -> `Game` and `A/B`; `/ControlRig/A/B.B` -> `Plugin.ControlRig` and `A/B`: a document's Root and Name. */
	FString SplitDcrObjectPath(const FString& ObjectPath, FString& OutRel);
}
