// Copyright DreamMoon. All Rights Reserved.
//
// Values between the engine's export text and the .dcr literals, through reflection. The model keeps pin
// values, settings and element properties as export text (`(Type=Bone,Name="pelvis")`); the text writes
// literals (`Bone(pelvis)`, `(X = 1, Y = 2, Z = 3)`, `GlobalSpace`). Lowering goes one way, semantic the
// other.
//
// Literal forms, by type:
// - bool `true` / `false`; numbers as the shortest decimal that reads back the same; enums by value name;
//   FName and FString as strings (`None` for an empty name); objects by asset name (`None` for none).
// - FRigElementKey as `Bone(pelvis)`, `Control(foot_l_ik)`: the element type's name, then the element's.
// - A struct as a tuple of the members that differ from its default, by name. A small math struct
//   (vector, rotator-free quaternion, colour, int point) with two or more members off the default is written
//   whole, by position: `(0, 0, 30)`. `()` is a struct at its default.
// - An array as a list.
// User-defined struct members go by their display name (`ControlSize`), not the GUID-suffixed one.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabAst.h"
#include "Model/DcrModel.h"
#include "StructUtils/PropertyBag.h"

class FProperty;
enum class ERigControlType : uint8;
struct FRigControlValue;

namespace UE::DreamAnim::Rig
{
	/**
	 * One value of a RigVM type in memory: a property bag holding one property, so any type works -- the
	 * pins of dispatches and the variables included, which have no property of their own to borrow.
	 */
	class FDcrValueSlot
	{
	public:
		/** False when the type does not resolve (a struct, enum or class path that does not load, an unknown CPP type). */
		bool Init(const Model::FDcrType& Type);
		bool Init(const FProperty& Like);
		bool IsValid() const { return Property != nullptr; }
		const FProperty* GetProperty() const { return Property; }
		void* GetMemory();
		/** False when Text does not import; the value is then the type's default. An empty Text is the default. */
		bool ImportText(const FString& Text);
		FString ExportText();

	private:
		FInstancedPropertyBag Bag;
		const FProperty* Property = nullptr;
	};

	/**
	 * A value in the engine's export text, every number exact: floats and doubles in the shortest spelling that
	 * reads back to the same bits (`0.2`, `-0`, `1.5e-09`) where the engine writes six decimals. Structs that
	 * export themselves, sets and maps go the engine's way. ImportText reads it back.
	 */
	FString ExportDcrValueText(const FProperty& Property, const void* Value);
	FString ExportDcrStructText(const UScriptStruct& Struct, const void* Value);
	/** Whether two values are the same to the bit (`-0` is not `0`), as ExportDcrValueText spells them. */
	bool AreDcrValuesSame(const FProperty& Property, const void* A, const void* B);

	/**
	 * Puts the parts of Value at SubPaths (`Shape.Color`, `0.Transform`) back to what Default holds there (an
	 * element Default lacks: to the element type's default). For the members of a pin that links feed: what
	 * they hold under the link does nothing. False when a path is not in Value.
	 */
	bool ResetDcrValueParts(const FProperty& Property, void* Value, const void* Default, TConstArrayView<FString> SubPaths);
	/**
	 * Drops the trailing elements of an array Value that are there only for links (SubPaths reach them, and they
	 * hold what a new element holds), down to Default's length: an array grows to take links into elements, so
	 * the links say how long it is. True when something was dropped.
	 */
	bool TrimDcrLinkedElements(const FProperty& Property, void* Value, const void* Default, TConstArrayView<FString> SubPaths);
	/**
	 * The text of a default as the engine can hold it: -0 written as 0. A variable's or a function pin's default
	 * goes through the engine's own parsing, which takes `-0` in a struct for no number at all.
	 */
	FString StripDcrNegativeZeros(const Model::FDcrType& Type, const FString& Text);
	/**
	 * Whether two values are the same but for rounding: every number within Tolerance of the other's, everything
	 * else the same. For what the engine works out again (a transform from its parent's, an initial control value)
	 * and so differs from what it saved by a few units in the last place.
	 */
	bool AreDcrValuesNear(const FProperty& Property, const void* A, const void* B, double Tolerance);
	/** What rounding in the engine's own arithmetic leaves behind: far below anything a rig is authored to. */
	inline constexpr double DcrRoundingTolerance = 1e-6;
	/** The same for two values as text (none: a new value) of Type. */
	bool AreDcrValueTextsNear(const Model::FDcrType& Type, const FString& A, const FString& B, double Tolerance);
	/** A variable's or a function pin's default as the model holds it: no -0, and nothing when it is a new value of its type. */
	FString MakeDcrCanonicalDefault(const Model::FDcrType& Type, const FString& Text);
	/**
	 * A default as the engine is given it: no -0, and a user struct's member defaults spelled out where the model
	 * says nothing (the engine would make its members zero).
	 */
	FString MakeDcrEngineDefault(const Model::FDcrType& Type, const FString& Text);

	/**
	 * The limit array a new control of the type gets (URigHierarchyController::AddControl sets it up when the
	 * settings bring none), as LimitEnabled's export text: no limits, one entry per channel of the type.
	 */
	FString GetDcrNewControlLimitsText(ERigControlType Type);

	/** The model's spelling of a property's type: RigVM's CPP type and the type object's path. */
	Model::FDcrType GetDcrType(const FProperty& Property);

	/** A value as a literal. Default (same type) is what members are compared against; null means the type's default. */
	Lang::FDabExprPtr MakeDcrValueExpr(const FProperty& Property, const void* Value, const void* Default);
	/** The same from export text, against DefaultText (empty: the type's default). Null when Text does not import. */
	Lang::FDabExprPtr MakeDcrValueExprFromText(const FProperty& Property, const FString& Text, const FString& DefaultText = FString());
	Lang::FDabExprPtr MakeDcrValueExprFromText(const Model::FDcrType& Type, const FString& Text, const FString& DefaultText = FString());

	/**
	 * A literal whose parts below the value are expressions (`(Translation = (Z = ZOffset_L))`): Parts maps a
	 * path below the value (`Transform.Translation.Z`, `0.Item`) to what goes there. Members around them are
	 * written by name when they differ from Default; array elements are all written. Parts on paths the type
	 * does not have are dropped and returned in OutUnplaced.
	 */
	Lang::FDabExprPtr MakeDcrValueExprWithParts(const FProperty& Property, const void* Value, const void* Default,
		TMap<FString, Lang::FDabExprPtr>& Parts, TArray<FString>& OutUnplaced);

	/** Part of a literal left to run time: where it goes below the value (`Shape.Name`, `0.Transform.Translation.Z`), and what. */
	struct FDcrValuePart
	{
		FString SubPath;
		const Lang::FDabExpr* Expr = nullptr;
	};

	/** Finds an asset a literal names by its bare name or `Path(...)`; null when there is none. */
	using FDcrAssetResolver = TFunctionRef<UObject*(const Lang::FDabExpr& Expr, const UClass* Expected)>;

	/**
	 * Writes a literal over Value. Parts IsConstant accepts are written; the others are returned in OutParts with
	 * where they go (the caller links them). A struct tuple names members, or lists every member by position; a
	 * list replaces the array; `()` resets to the default. False, with OutError, when the literal does not fit.
	 */
	bool WriteDcrValueExpr(const FProperty& Property, void* Value, const Lang::FDabExpr& Expr,
		TFunctionRef<bool(const Lang::FDabExpr&, const FProperty&)> IsConstant, FDcrAssetResolver ResolveAsset,
		TArray<FDcrValuePart>& OutParts, FString& OutError, const FString& SubPath = FString());

	/**
	 * A control's value (FRigControlValue keeps every type in one float store) as the text writes it for the
	 * control's type: `true`, `0.5`, `3`, `(X = 1, Y = 2)`, a vector, a rotator (`(Roll = 90)`), a transform, a
	 * transform without scale, an Euler transform (`(Location = (Z = 10), Rotation = (Yaw = 90))`). Members are
	 * written where they differ from the type's own default.
	 */
	Lang::FDabExprPtr MakeDcrControlValueExpr(ERigControlType Type, const FRigControlValue& Value);
	/** The inverse, for constant literals; false with OutError when the literal does not fit the type. */
	bool WriteDcrControlValue(ERigControlType Type, const Lang::FDabExpr& Expr, FRigControlValue& OutValue, FString& OutError);
	/** Whether a control of the type starts at the value: false, 0, zero vectors and rotators, scale 1, identity transforms. */
	bool IsDcrControlValueIdentity(ERigControlType Type, const FRigControlValue& Value);

	/** The member of Struct a literal names: by name, or by display name for a user-defined struct. */
	const FProperty* FindDcrStructMember(const UScriptStruct& Struct, const FString& Name);
	/** The name a literal uses for a member: the display name for a user-defined struct's members. */
	FString GetDcrMemberName(const FProperty& Member);
}
