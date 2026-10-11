// Copyright DreamMoon. All Rights Reserved.

#include "Extract/DabExtractPrivate.h"

#include "AnimStateAliasNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateNode.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameplayTagContainer.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Lang/DabPrinter.h"
#include "Misc/CString.h"
#include "Misc/OutputDeviceNull.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Compiler::ExtractPrivate
{
	using namespace UE::DreamAnim::Lang;

	namespace
	{
		bool IsIdentifier(const FString& Text)
		{
			if (Text.IsEmpty())
			{
				return false;
			}
			for (int32 Index = 0; Index < Text.Len(); ++Index)
			{
				const TCHAR Char = Text[Index];
				const bool bAllowed = Char == TEXT('_') || FChar::IsAlpha(Char) || (Index > 0 && FChar::IsDigit(Char));
				if (!bAllowed)
				{
					return false;
				}
			}
			return true;
		}

		FDabExprPtr MakeParen(FDabExprPtr Inner)
		{
			TUniquePtr<FDabParenExpr> Paren = MakeUnique<FDabParenExpr>();
			Paren->Inner = MoveTemp(Inner);
			return FDabExprPtr(MoveTemp(Paren));
		}

		/** Whether Operand needs parentheses as one side of a binary operator of the given precedence (left-associative). */
		bool NeedsParens(const FDabExpr& Operand, int32 Precedence, bool bRightSide)
		{
			if (Operand.Is<FDabConditionalExpr>())
			{
				return true;
			}
			if (const FDabBinaryExpr* Binary = Operand.As<FDabBinaryExpr>())
			{
				const int32 OperandPrecedence = GetDabBinaryOpPrecedence(Binary->Op);
				return bRightSide ? OperandPrecedence <= Precedence : OperandPrecedence < Precedence;
			}
			return false;
		}

		/** The enumerator's bare name (`Walking`, `TLT_StandardBlend`), or the number when it names none. */
		FDabExprPtr MakeEnumValue(const UEnum* Enum, int64 Value)
		{
			if (Enum && Enum->IsValidEnumValue(Value))
			{
				FString Name = Enum->GetNameStringByValue(Value);
				int32 Scope = INDEX_NONE;
				if (Name.FindLastChar(TEXT(':'), Scope))
				{
					Name.RightChopInline(Scope + 1);
				}
				if (!Name.IsEmpty())
				{
					return MakeName(Name);
				}
			}
			return MakeDabInteger(Value);
		}

		/** `Als.Gait.Walking` from `(TagName="Als.Gait.Walking")` or from the bare name. */
		FString ParseGameplayTagText(const FString& Text)
		{
			FString Trimmed = Text.TrimStartAndEnd();
			const int32 Key = Trimmed.Find(TEXT("TagName"), ESearchCase::IgnoreCase);
			if (Key != INDEX_NONE)
			{
				const int32 Open = Trimmed.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Key);
				const int32 Close = Open != INDEX_NONE ? Trimmed.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open + 1) : INDEX_NONE;
				if (Open != INDEX_NONE && Close != INDEX_NONE)
				{
					return Trimmed.Mid(Open + 1, Close - Open - 1);
				}
				int32 Equals = INDEX_NONE;
				if (Trimmed.FindChar(TEXT('='), Equals))
				{
					FString Value = Trimmed.Mid(Equals + 1);
					Value.RemoveFromEnd(TEXT(")"));
					return Value.TrimStartAndEnd();
				}
			}
			return Trimmed == TEXT("None") ? FString() : Trimmed;
		}

		/** Base struct's fields first, each struct's in declaration order: the order tuples print in. */
		TArray<const FProperty*> GetFieldsInDeclarationOrder(const UStruct& Struct)
		{
			TArray<const UStruct*> Chain;
			for (const UStruct* Current = &Struct; Current; Current = Current->GetSuperStruct())
			{
				Chain.Insert(Current, 0);
			}

			TArray<const FProperty*> Fields;
			for (const UStruct* Current : Chain)
			{
				for (TFieldIterator<FProperty> It(Current, EFieldIteratorFlags::ExcludeSuper); It; ++It)
				{
					Fields.Add(*It);
				}
			}
			return Fields;
		}

		FString PrintForSorting(const FDabExpr& Expr)
		{
			FDabPrintOptions Options;
			Options.MaxWidth = 1 << 24;
			return PrintDabExpr(Expr, 0, 0, nullptr, Options);
		}

		FDabExprPtr ConvertStruct(FDabExtractContext& Ctx, const UScriptStruct& Struct, const void* Value, const void* Default, const FString& Location)
		{
			if (&Struct == FGameplayTag::StaticStruct())
			{
				const FName TagName = static_cast<const FGameplayTag*>(Value)->GetTagName();
				return MakeString(TagName.IsNone() ? FString() : TagName.ToString());
			}
			if (&Struct == FGameplayTagContainer::StaticStruct())
			{
				TUniquePtr<FDabListExpr> List = MakeUnique<FDabListExpr>();
				for (const FGameplayTag& Tag : static_cast<const FGameplayTagContainer*>(Value)->GetGameplayTagArray())
				{
					List->Items.Add(MakeString(Tag.GetTagName().ToString()));
				}
				return FDabExprPtr(MoveTemp(List));
			}

			TUniquePtr<FDabTempStruct> Fallback;
			if (!Default)
			{
				Fallback = MakeUnique<FDabTempStruct>(Struct);
				Default = Fallback->Get();
			}

			TUniquePtr<FDabTupleExpr> Tuple = MakeUnique<FDabTupleExpr>();
			for (const FProperty* Field : GetFieldsInDeclarationOrder(Struct))
			{
				if (Field->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
				{
					continue;
				}
				// A GUID inside a struct names the struct (a notify event, a crossfade group); it does not
				// change what plays, and it differs on every node.
				const FStructProperty* FieldStruct = CastField<FStructProperty>(Field);
				if (FieldStruct && FieldStruct->Struct == TBaseStructure<FGuid>::Get())
				{
					continue;
				}

				const FString FieldLocation = Location + TEXT(".") + Field->GetName();
				for (int32 ArrayIndex = 0; ArrayIndex < Field->ArrayDim; ++ArrayIndex)
				{
					const void* FieldValue = Field->ContainerPtrToValuePtr<void>(Value, ArrayIndex);
					const void* FieldDefault = Field->ContainerPtrToValuePtr<void>(Default, ArrayIndex);
					if (Field->Identical(FieldValue, FieldDefault, 0))
					{
						continue;
					}
					if (Field->ArrayDim > 1)
					{
						Ctx.AddUnsupported(FieldLocation, TEXT("an element of a fixed-size array differs from its default; fixed-size arrays cannot be expressed"));
						continue;
					}

					// Nested structs compare with a default-initialised struct, not with the outer default's field.
					FDabExprPtr Item = ConvertPropertyValue(Ctx, *Field, FieldValue, nullptr, FieldLocation);
					if (!Item)
					{
						continue;
					}
					TUniquePtr<FDabArgument> Argument = MakeUnique<FDabArgument>();
					Argument->Name = Field->GetName();
					Argument->Value = MoveTemp(Item);
					Tuple->Items.Add(MoveTemp(Argument));
				}
			}
			return FDabExprPtr(MoveTemp(Tuple));
		}

		/** A map key as the string that names a tuple item. */
		bool GetMapKeyText(const FProperty& KeyProperty, const void* Key, FString& OutText)
		{
			if (const FNameProperty* Name = CastField<FNameProperty>(&KeyProperty))
			{
				OutText = Name->GetPropertyValue(Key).ToString();
				return true;
			}
			if (const FStrProperty* String = CastField<FStrProperty>(&KeyProperty))
			{
				OutText = String->GetPropertyValue(Key);
				return true;
			}
			if (const FEnumProperty* Enum = CastField<FEnumProperty>(&KeyProperty))
			{
				OutText = Enum->GetEnum()->GetNameStringByValue(Enum->GetUnderlyingProperty()->GetSignedIntPropertyValue(Key));
				return true;
			}
			if (const FByteProperty* Byte = CastField<FByteProperty>(&KeyProperty))
			{
				if (Byte->Enum)
				{
					OutText = Byte->Enum->GetNameStringByValue(Byte->GetSignedIntPropertyValue(Key));
					return true;
				}
			}
			if (const FStructProperty* Struct = CastField<FStructProperty>(&KeyProperty))
			{
				if (Struct->Struct == FGameplayTag::StaticStruct())
				{
					OutText = static_cast<const FGameplayTag*>(Key)->GetTagName().ToString();
					return true;
				}
			}
			return false;
		}

		FString GetTerminalSpelling(const FName Category, const FName SubCategory, const UObject* SubCategoryObject)
		{
			const auto ObjectName = [SubCategoryObject]() -> FString
			{
				if (!SubCategoryObject)
				{
					return TEXT("Object");
				}
				FString Name = SubCategoryObject->GetName();
				if (const UClass* Class = Cast<UClass>(SubCategoryObject))
				{
					if (!Class->HasAnyClassFlags(CLASS_Native))
					{
						Name.RemoveFromEnd(TEXT("_C"));
					}
				}
				return Name;
			};

			if (Category == UEdGraphSchema_K2::PC_Boolean)
			{
				return TEXT("bool");
			}
			if (Category == UEdGraphSchema_K2::PC_Int)
			{
				return TEXT("int");
			}
			if (Category == UEdGraphSchema_K2::PC_Int64)
			{
				return TEXT("int64");
			}
			if (Category == UEdGraphSchema_K2::PC_Real || Category == UEdGraphSchema_K2::PC_Float || Category == UEdGraphSchema_K2::PC_Double)
			{
				return TEXT("float");
			}
			if (Category == UEdGraphSchema_K2::PC_Byte || Category == UEdGraphSchema_K2::PC_Enum)
			{
				return SubCategoryObject ? SubCategoryObject->GetName() : FString(TEXT("uint8"));
			}
			if (Category == UEdGraphSchema_K2::PC_Name)
			{
				return TEXT("FName");
			}
			if (Category == UEdGraphSchema_K2::PC_String)
			{
				return TEXT("FString");
			}
			if (Category == UEdGraphSchema_K2::PC_Text)
			{
				return TEXT("FText");
			}
			if (Category == UEdGraphSchema_K2::PC_Struct || Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Interface)
			{
				return ObjectName();
			}
			if (Category == UEdGraphSchema_K2::PC_Class)
			{
				return FString::Printf(TEXT("TSubclassOf<%s>"), *ObjectName());
			}
			if (Category == UEdGraphSchema_K2::PC_SoftObject)
			{
				return FString::Printf(TEXT("TSoftObjectPtr<%s>"), *ObjectName());
			}
			if (Category == UEdGraphSchema_K2::PC_SoftClass)
			{
				return FString::Printf(TEXT("TSoftClassPtr<%s>"), *ObjectName());
			}
			return SubCategory.IsNone() ? Category.ToString() : FString::Printf(TEXT("%s<%s>"), *Category.ToString(), *SubCategory.ToString());
		}
	}

	// ------------------------------------------------------------------------------------------
	// Expressions
	// ------------------------------------------------------------------------------------------

	FDabExprPtr MakeName(const FString& Name)
	{
		TUniquePtr<FDabNameExpr> Expr = MakeUnique<FDabNameExpr>();
		Expr->Name = Name;
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeMember(FDabExprPtr Object, const FString& Member)
	{
		TUniquePtr<FDabMemberExpr> Expr = MakeUnique<FDabMemberExpr>();
		const bool bNeedsParens = Object.IsValid()
			&& (Object->Is<FDabBinaryExpr>() || Object->Is<FDabUnaryExpr>() || Object->Is<FDabConditionalExpr>());
		Expr->Object = bNeedsParens ? MakeParen(MoveTemp(Object)) : MoveTemp(Object);
		Expr->Member = Member;
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeString(const FString& Value)
	{
		TUniquePtr<FDabLiteralExpr> Expr = MakeUnique<FDabLiteralExpr>();
		Expr->LiteralKind = EDabLiteralKind::String;
		Expr->Text = Value;
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeBool(bool bValue)
	{
		TUniquePtr<FDabLiteralExpr> Expr = MakeUnique<FDabLiteralExpr>();
		Expr->LiteralKind = EDabLiteralKind::Bool;
		Expr->bBool = bValue;
		Expr->Text = bValue ? TEXT("true") : TEXT("false");
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeNumber(double Value)
	{
		// Number lexemes carry no sign: the parser reads `-0.5` as a negation, and so does the model.
		if (Value < 0.0)
		{
			return MakeUnary(EDabUnaryOp::Negate, MakeNumber(-Value));
		}
		TUniquePtr<FDabLiteralExpr> Expr = MakeUnique<FDabLiteralExpr>();
		Expr->LiteralKind = EDabLiteralKind::Number;
		Expr->Number = Value;
		Expr->Text = Model::FormatDabNumber(Value);
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeCall(FDabExprPtr Callee, TArray<FDabExprPtr> Args)
	{
		TUniquePtr<FDabCallExpr> Expr = MakeUnique<FDabCallExpr>();
		Expr->Callee = MoveTemp(Callee);
		for (FDabExprPtr& Arg : Args)
		{
			TUniquePtr<FDabArgument> Argument = MakeUnique<FDabArgument>();
			Argument->Value = MoveTemp(Arg);
			Expr->Args.Add(MoveTemp(Argument));
		}
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeUnary(EDabUnaryOp Op, FDabExprPtr Operand)
	{
		TUniquePtr<FDabUnaryExpr> Expr = MakeUnique<FDabUnaryExpr>();
		Expr->Op = Op;
		const bool bNeedsParens = Operand.IsValid() && (Operand->Is<FDabBinaryExpr>() || Operand->Is<FDabConditionalExpr>());
		Expr->Operand = bNeedsParens ? MakeParen(MoveTemp(Operand)) : MoveTemp(Operand);
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeBinary(EDabBinaryOp Op, FDabExprPtr Lhs, FDabExprPtr Rhs)
	{
		const int32 Precedence = GetDabBinaryOpPrecedence(Op);
		TUniquePtr<FDabBinaryExpr> Expr = MakeUnique<FDabBinaryExpr>();
		Expr->Op = Op;
		Expr->Lhs = (Lhs.IsValid() && NeedsParens(*Lhs, Precedence, false)) ? MakeParen(MoveTemp(Lhs)) : MoveTemp(Lhs);
		Expr->Rhs = (Rhs.IsValid() && NeedsParens(*Rhs, Precedence, true)) ? MakeParen(MoveTemp(Rhs)) : MoveTemp(Rhs);
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeConditional(FDabExprPtr Condition, FDabExprPtr Then, FDabExprPtr Else)
	{
		TUniquePtr<FDabConditionalExpr> Expr = MakeUnique<FDabConditionalExpr>();
		const bool bNeedsParens = Condition.IsValid() && Condition->Is<FDabConditionalExpr>();
		Expr->Condition = bNeedsParens ? MakeParen(MoveTemp(Condition)) : MoveTemp(Condition);
		Expr->Then = MoveTemp(Then);
		Expr->Else = MoveTemp(Else);
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakePlaceholder(const FString& What)
	{
		TArray<FDabExprPtr> Args;
		Args.Add(MakeString(What));
		return MakeCall(MakeName(TEXT("Unsupported")), MoveTemp(Args));
	}

	double FloatToShortestDouble(float Value)
	{
		for (int32 Precision = 1; Precision <= 9; ++Precision)
		{
			const FString Text = FString::Printf(TEXT("%.*g"), Precision, static_cast<double>(Value));
			const double Parsed = FCString::Atod(*Text);
			if (static_cast<float>(Parsed) == Value)
			{
				return Parsed;
			}
		}
		return static_cast<double>(Value);
	}

	// ------------------------------------------------------------------------------------------
	// Object references
	// ------------------------------------------------------------------------------------------

	FString GetClassAssetPath(const UClass* Class)
	{
		if (!Class)
		{
			return FString();
		}
		if (Class->HasAnyClassFlags(CLASS_Native))
		{
			return Class->GetPathName();
		}
		if (const UBlueprint* Blueprint = UBlueprint::GetBlueprintFromClass(Class))
		{
			return Blueprint->GetPathName();
		}
		// A generated class whose Blueprint is not loaded: `AB_X.AB_X_C` names the asset `AB_X.AB_X`.
		FString Path = Class->GetPathName();
		Path.RemoveFromEnd(TEXT("_C"));
		return Path;
	}

	FDabExprPtr MakeObjectReference(const UObject* Object)
	{
		if (!Object)
		{
			return MakeName(TEXT("None"));
		}
		if (const UClass* Class = Cast<UClass>(Object))
		{
			if (Class->HasAnyClassFlags(CLASS_Native))
			{
				return MakeString(Class->GetPathName());
			}
			return MakeObjectPathReference(GetClassAssetPath(Class));
		}
		return MakeObjectPathReference(Object->GetPathName());
	}

	FDabExprPtr MakeObjectPathReference(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty() || ObjectPath == TEXT("None"))
		{
			return MakeName(TEXT("None"));
		}
		// Native types (classes, enums, structs) have no asset to point at.
		if (ObjectPath.StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive) || !ObjectPath.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
		{
			return MakeString(ObjectPath);
		}

		FString Main = ObjectPath;
		FString SubObject;
		ObjectPath.Split(TEXT(":"), &Main, &SubObject, ESearchCase::CaseSensitive);

		FString Package = Main;
		FString Object;
		if (!Main.Split(TEXT("."), &Package, &Object, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			Package = Main;
			Object.Reset();
		}

		int32 LastSlash = INDEX_NONE;
		Package.FindLastChar(TEXT('/'), LastSlash);
		const FString ShortName = Package.Mid(LastSlash + 1);
		if (Object == ShortName + TEXT("_C"))
		{
			// A Blueprint class is referenced by its Blueprint.
			Object = ShortName;
		}

		const int32 MountEnd = Package.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 1);
		if (MountEnd == INDEX_NONE)
		{
			return MakeString(ObjectPath);
		}
		const FString Mount = Package.Mid(1, MountEnd - 1);
		FString Relative = Package.Mid(MountEnd + 1);
		if (!Object.IsEmpty() && Object != ShortName)
		{
			Relative += TEXT(".") + Object;
		}
		if (!SubObject.IsEmpty())
		{
			Relative += TEXT(":") + SubObject;
		}

		FDabExprPtr Root;
		if (Mount == TEXT("Game") || Mount == TEXT("Engine"))
		{
			Root = MakeName(Mount);
		}
		else if (IsIdentifier(Mount))
		{
			Root = MakeMember(MakeName(TEXT("Plugin")), Mount);
		}
		else
		{
			return MakeString(ObjectPath);
		}

		TArray<FDabExprPtr> Args;
		Args.Add(MoveTemp(Root));
		Args.Add(MakeString(Relative));
		return MakeCall(MakeName(TEXT("Path")), MoveTemp(Args));
	}

	// ------------------------------------------------------------------------------------------
	// Values
	// ------------------------------------------------------------------------------------------

	FDabExprPtr ConvertPropertyValue(FDabExtractContext& Ctx, const FProperty& Property, const void* Value, const void* Default, const FString& Location)
	{
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
		{
			return MakeBool(Bool->GetPropertyValue(Value));
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
		{
			return MakeEnumValue(EnumProperty->GetEnum(), EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value));
		}
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
		{
			if (const UEnum* Enum = Numeric->GetIntPropertyEnum())
			{
				return MakeEnumValue(Enum, Numeric->GetSignedIntPropertyValue(Value));
			}
			if (const FFloatProperty* Float = CastField<FFloatProperty>(&Property))
			{
				return MakeNumber(FloatToShortestDouble(Float->GetPropertyValue(Value)));
			}
			if (Numeric->IsFloatingPoint())
			{
				return MakeNumber(Numeric->GetFloatingPointPropertyValue(Value));
			}
			return MakeDabInteger(Numeric->GetSignedIntPropertyValue(Value));
		}
		if (const FNameProperty* Name = CastField<FNameProperty>(&Property))
		{
			const FName NameValue = Name->GetPropertyValue(Value);
			return MakeString(NameValue.IsNone() ? FString() : NameValue.ToString());
		}
		if (const FStrProperty* String = CastField<FStrProperty>(&Property))
		{
			return MakeString(String->GetPropertyValue(Value));
		}
		if (const FTextProperty* Text = CastField<FTextProperty>(&Property))
		{
			return MakeString(Text->GetPropertyValue(Value).ToString());
		}
		if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(&Property))
		{
			return MakeObjectPathReference(Soft->GetPropertyValue(Value).ToSoftObjectPath().ToString());
		}
		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
		{
			return MakeObjectReference(Object->GetObjectPropertyValue(Value));
		}
		if (const FInterfaceProperty* Interface = CastField<FInterfaceProperty>(&Property))
		{
			return MakeObjectReference(Interface->GetPropertyValue(Value).GetObject());
		}
		if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
		{
			return ConvertStruct(Ctx, *Struct->Struct, Value, Default, Location);
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
		{
			FScriptArrayHelper Helper(Array, Value);
			TUniquePtr<FDabListExpr> List = MakeUnique<FDabListExpr>();
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				FDabExprPtr Item = ConvertPropertyValue(Ctx, *Array->Inner, Helper.GetRawPtr(Index), nullptr, FString::Printf(TEXT("%s[%d]"), *Location, Index));
				if (!Item)
				{
					return nullptr;
				}
				List->Items.Add(MoveTemp(Item));
			}
			return FDabExprPtr(MoveTemp(List));
		}
		if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
		{
			// A set has no order of its own; sort by text so the value is canonical.
			FScriptSetHelper Helper(Set, Value);
			TArray<TPair<FString, FDabExprPtr>> Items;
			for (FScriptSetHelper::FIterator It(Helper); It; ++It)
			{
				FDabExprPtr Item = ConvertPropertyValue(Ctx, *Set->ElementProp, Helper.GetElementPtr(It.GetInternalIndex()), nullptr, Location);
				if (!Item)
				{
					return nullptr;
				}
				FString Key = PrintForSorting(*Item);
				Items.Emplace(MoveTemp(Key), MoveTemp(Item));
			}
			Items.StableSort([](const TPair<FString, FDabExprPtr>& A, const TPair<FString, FDabExprPtr>& B)
			{
				return A.Key.Compare(B.Key, ESearchCase::CaseSensitive) < 0;
			});
			TUniquePtr<FDabListExpr> List = MakeUnique<FDabListExpr>();
			for (TPair<FString, FDabExprPtr>& Item : Items)
			{
				List->Items.Add(MoveTemp(Item.Value));
			}
			return FDabExprPtr(MoveTemp(List));
		}
		if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
		{
			// A map keyed by names becomes a tuple whose items are named by string: `("A" = 1, "B" = 2)`.
			FScriptMapHelper Helper(Map, Value);
			if (Helper.Num() == 0)
			{
				return FDabExprPtr(MakeUnique<FDabListExpr>());
			}
			TArray<TPair<FString, FDabExprPtr>> Items;
			for (FScriptMapHelper::FIterator It(Helper); It; ++It)
			{
				FString Key;
				if (!GetMapKeyText(*Map->KeyProp, Helper.GetKeyPtr(It.GetInternalIndex()), Key))
				{
					Ctx.AddUnsupported(Location, FString::Printf(TEXT("a map keyed by %s cannot be expressed"), *Map->KeyProp->GetClass()->GetName()));
					return nullptr;
				}
				FDabExprPtr Item = ConvertPropertyValue(Ctx, *Map->ValueProp, Helper.GetValuePtr(It.GetInternalIndex()), nullptr, Location + TEXT("[") + Key + TEXT("]"));
				if (!Item)
				{
					return nullptr;
				}
				Items.Emplace(MoveTemp(Key), MoveTemp(Item));
			}
			Items.StableSort([](const TPair<FString, FDabExprPtr>& A, const TPair<FString, FDabExprPtr>& B)
			{
				return A.Key.Compare(B.Key, ESearchCase::CaseSensitive) < 0;
			});
			TUniquePtr<FDabTupleExpr> Tuple = MakeUnique<FDabTupleExpr>();
			for (TPair<FString, FDabExprPtr>& Item : Items)
			{
				TUniquePtr<FDabArgument> Argument = MakeUnique<FDabArgument>();
				Argument->Name = Item.Key;
				Argument->bNameIsString = true;
				Argument->Value = MoveTemp(Item.Value);
				Tuple->Items.Add(MoveTemp(Argument));
			}
			return FDabExprPtr(MoveTemp(Tuple));
		}

		Ctx.AddUnsupported(Location, FString::Printf(TEXT("a %s value cannot be expressed"), *Property.GetClass()->GetName()));
		return nullptr;
	}

	FDabExprPtr ConvertPinDefaultAs(FDabExtractContext& Ctx, const UEdGraphPin& Pin, const FProperty& Property, const FString& Location)
	{
		FDabTempValue Temp(Property);
		if (!FBlueprintEditorUtils::PropertyValueFromString_Direct(&Property, Pin.GetDefaultAsString(), Temp.Get()))
		{
			return nullptr;
		}
		return ConvertPropertyValue(Ctx, Property, Temp.Get(), nullptr, Location);
	}

	FDabExprPtr ConvertPinDefaultByType(FDabExtractContext& Ctx, const UEdGraphPin& Pin, const FString& Location)
	{
		const FEdGraphPinType& Type = Pin.PinType;
		const FName Category = Type.PinCategory;
		const UObject* SubCategoryObject = Type.PinSubCategoryObject.Get();

		if (Type.ContainerType != EPinContainerType::None)
		{
			Ctx.AddUnsupported(Location, TEXT("a container literal on a pin cannot be expressed"));
			return MakePlaceholder(TEXT("container literal"));
		}
		if (Category == UEdGraphSchema_K2::PC_Boolean)
		{
			return MakeBool(Pin.DefaultValue.ToBool());
		}
		if (Category == UEdGraphSchema_K2::PC_Int || Category == UEdGraphSchema_K2::PC_Int64)
		{
			return MakeDabInteger(FCString::Atoi64(*Pin.DefaultValue));
		}
		if (Category == UEdGraphSchema_K2::PC_Real || Category == UEdGraphSchema_K2::PC_Float || Category == UEdGraphSchema_K2::PC_Double)
		{
			const double Parsed = FCString::Atod(*Pin.DefaultValue);
			const bool bSingle = Category == UEdGraphSchema_K2::PC_Float || Type.PinSubCategory == UEdGraphSchema_K2::PC_Float;
			return MakeNumber(bSingle ? FloatToShortestDouble(static_cast<float>(Parsed)) : Parsed);
		}
		if (Category == UEdGraphSchema_K2::PC_Byte || Category == UEdGraphSchema_K2::PC_Enum)
		{
			if (const UEnum* Enum = Cast<UEnum>(SubCategoryObject))
			{
				if (Pin.DefaultValue.IsEmpty())
				{
					return MakeEnumValue(Enum, Enum->GetValueByIndex(0));
				}
				FString Name = Pin.DefaultValue;
				int32 Scope = INDEX_NONE;
				if (Name.FindLastChar(TEXT(':'), Scope))
				{
					Name.RightChopInline(Scope + 1);
				}
				return MakeName(Name);
			}
			return MakeDabInteger(FCString::Atoi64(*Pin.DefaultValue));
		}
		if (Category == UEdGraphSchema_K2::PC_Name)
		{
			return MakeString(Pin.DefaultValue == TEXT("None") ? FString() : Pin.DefaultValue);
		}
		if (Category == UEdGraphSchema_K2::PC_String)
		{
			return MakeString(Pin.DefaultValue);
		}
		if (Category == UEdGraphSchema_K2::PC_Text)
		{
			return MakeString(Pin.DefaultTextValue.ToString());
		}
		if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class || Category == UEdGraphSchema_K2::PC_Interface
			|| Category == UEdGraphSchema_K2::PC_SoftObject || Category == UEdGraphSchema_K2::PC_SoftClass)
		{
			return Pin.DefaultObject ? MakeObjectReference(Pin.DefaultObject) : MakeObjectPathReference(Pin.DefaultValue);
		}
		if (Category == UEdGraphSchema_K2::PC_Struct)
		{
			if (const UScriptStruct* Struct = Cast<UScriptStruct>(SubCategoryObject))
			{
				if (Struct == FGameplayTag::StaticStruct())
				{
					return MakeString(ParseGameplayTagText(Pin.DefaultValue));
				}
				FDabTempStruct Temp(*Struct);
				FOutputDeviceNull Errors;
				if (Pin.DefaultValue.IsEmpty() || Struct->ImportText(*Pin.DefaultValue, Temp.Get(), nullptr, PPF_None, &Errors, Struct->GetName()))
				{
					return ConvertStruct(Ctx, *Struct, Temp.Get(), nullptr, Location);
				}
			}
		}

		Ctx.AddUnsupported(Location, FString::Printf(TEXT("the default value '%s' of a %s pin cannot be read"), *Pin.GetDefaultAsString(), *Category.ToString()));
		return MakePlaceholder(Pin.GetDefaultAsString());
	}

	FString GetPinTypeSpelling(const FEdGraphPinType& Type)
	{
		const FString Terminal = GetTerminalSpelling(Type.PinCategory, Type.PinSubCategory, Type.PinSubCategoryObject.Get());
		switch (Type.ContainerType)
		{
		case EPinContainerType::Array:
			return FString::Printf(TEXT("TArray<%s>"), *Terminal);
		case EPinContainerType::Set:
			return FString::Printf(TEXT("TSet<%s>"), *Terminal);
		case EPinContainerType::Map:
			return FString::Printf(TEXT("TMap<%s, %s>"), *Terminal, *GetTerminalSpelling(Type.PinValueType.TerminalCategory,
				Type.PinValueType.TerminalSubCategory, Type.PinValueType.TerminalSubCategoryObject.Get()));
		default:
			return Terminal;
		}
	}

	FDabExprPtr MakeAccessPath(FDabExtractContext& Ctx, const TArray<FString>& Segments, const FString& Location)
	{
		if (Segments.IsEmpty())
		{
			Ctx.AddUnsupported(Location, TEXT("an empty property access path"));
			return MakePlaceholder(TEXT("empty path"));
		}

		// The generator reads a slot through its getter, in a forwarder's argument as in a pin's binding.
		const FString* Slot = Segments.Num() == 1 ? Ctx.AssetSlotFunctions.Find(FName(*Segments[0])) : nullptr;
		if (Slot)
		{
			Ctx.UsedFunctions.Add(FName(*Segments[0]));
			return MakeMember(MakeName(TEXT("Assets")), *Slot);
		}

		const UStruct* Scope = Ctx.InstanceClass;
		FDabExprPtr Expr;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			const FString& Segment = Segments[Index];
			if (Segment.Contains(TEXT("[")))
			{
				Ctx.AddUnsupported(Location, FString::Printf(TEXT("the path %s indexes an array; the language has no index operator"), *FString::Join(Segments, TEXT("."))));
				return MakePlaceholder(FString::Join(Segments, TEXT(".")));
			}

			const FName SegmentName(*Segment);
			const UFunction* Function = nullptr;
			const FProperty* Property = nullptr;
			if (const UClass* Class = Cast<UClass>(Scope))
			{
				Function = Class->FindFunctionByName(SegmentName);
			}
			if (!Function && Scope)
			{
				Property = FindFProperty<FProperty>(Scope, SegmentName);
			}

			if (Index == 0)
			{
				// The Blueprint's own variables are tuning values (`Props.X`); everything else is C++ state.
				const UClass* Owner = Property ? Property->GetOwnerClass() : nullptr;
				const bool bBlueprintVariable = Ctx.BlueprintVariables.Contains(SegmentName) || (Owner && !Owner->HasAnyClassFlags(CLASS_Native));
				Expr = MakeName(bBlueprintVariable && !Function ? TEXT("Props") : TEXT("Self"));
			}

			Expr = MakeMember(MoveTemp(Expr), Segment);

			const FProperty* Result = Property;
			if (Function)
			{
				Expr = MakeCall(MoveTemp(Expr), TArray<FDabExprPtr>());
				Result = Function->GetReturnProperty();
			}

			Scope = nullptr;
			if (const FObjectPropertyBase* ObjectResult = CastField<FObjectPropertyBase>(Result))
			{
				Scope = ObjectResult->PropertyClass;
			}
			else if (const FStructProperty* StructResult = CastField<FStructProperty>(Result))
			{
				Scope = StructResult->Struct;
			}
			else if (const FInterfaceProperty* InterfaceResult = CastField<FInterfaceProperty>(Result))
			{
				Scope = InterfaceResult->InterfaceClass;
			}
		}
		return Expr;
	}

	// ------------------------------------------------------------------------------------------
	// Temporaries and reflection
	// ------------------------------------------------------------------------------------------

	FDabTempValue::FDabTempValue(const FProperty& InProperty)
		: Property(InProperty)
	{
		Memory = static_cast<uint8*>(FMemory::Malloc(FMath::Max(1, Property.GetSize()), Property.GetMinAlignment()));
		Property.InitializeValue(Memory);
	}

	FDabTempValue::~FDabTempValue()
	{
		Property.DestroyValue(Memory);
		FMemory::Free(Memory);
	}

	FDabTempStruct::FDabTempStruct(const UScriptStruct& InStruct)
		: Struct(InStruct)
	{
		Memory = static_cast<uint8*>(FMemory::Malloc(FMath::Max(1, Struct.GetStructureSize()), Struct.GetMinAlignment()));
		Struct.InitializeStruct(Memory);
	}

	FDabTempStruct::~FDabTempStruct()
	{
		Struct.DestroyStruct(Memory);
		FMemory::Free(Memory);
	}

	bool IsInstanceOfClassPath(const UObject* Object, const TCHAR* PackageName, const TCHAR* ClassName)
	{
		const FTopLevelAssetPath Path{FName(PackageName), FName(ClassName)};
		for (const UClass* Class = Object ? Object->GetClass() : nullptr; Class; Class = Class->GetSuperClass())
		{
			if (Class->GetClassPathName() == Path)
			{
				return true;
			}
		}
		return false;
	}

	UObject* GetObjectPropertyByName(const UObject& Object, FName PropertyName)
	{
		const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Object.GetClass(), PropertyName);
		return Property ? Property->GetObjectPropertyValue_InContainer(&Object) : nullptr;
	}

	FString GetStateNodeName(const UEdGraphNode* Node)
	{
		if (!Node)
		{
			return FString();
		}
		if (const UAnimStateAliasNode* Alias = Cast<UAnimStateAliasNode>(Node))
		{
			return Alias->StateAliasName;
		}
		if (const UAnimStateNode* State = Cast<UAnimStateNode>(Node))
		{
			return State->BoundGraph ? State->BoundGraph->GetName() : State->GetName();
		}
		if (const UAnimStateConduitNode* Conduit = Cast<UAnimStateConduitNode>(Node))
		{
			return Conduit->BoundGraph ? Conduit->BoundGraph->GetName() : Conduit->GetName();
		}
		return Node->GetName();
	}
}
