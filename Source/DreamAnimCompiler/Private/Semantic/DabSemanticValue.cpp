// Copyright DreamMoon. All Rights Reserved.
//
// Values: written expressions -> the model's canonical forms (Docs/Mapping.md, "Values in the model"),
// and property memory -> the same forms (ValueToExpr, the extractor's conversion), so a written value is
// compared with an engine default exactly the way the extractor compares.

#include "Semantic/DabSemanticInternal.h"

#include "Animation/AnimBlueprint.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/BlueprintSupport.h"
#include "Engine/Blueprint.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "Lang/DabPrinter.h"
#include "Misc/CString.h"
#include "Misc/PackageName.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

namespace UE::DreamAnim::Compiler::SemanticPrivate
{
	// ------------------------------------------------------------------------------------------
	// Expression builders
	// ------------------------------------------------------------------------------------------

	FDabExprPtr MakeNameExpr(const FString& Name)
	{
		TUniquePtr<FDabNameExpr> Expr = MakeUnique<FDabNameExpr>();
		Expr->Name = Name;
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeStringExpr(const FString& Value)
	{
		TUniquePtr<FDabLiteralExpr> Expr = MakeUnique<FDabLiteralExpr>();
		Expr->LiteralKind = EDabLiteralKind::String;
		Expr->Text = Value;
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeBoolExpr(bool bValue)
	{
		TUniquePtr<FDabLiteralExpr> Expr = MakeUnique<FDabLiteralExpr>();
		Expr->LiteralKind = EDabLiteralKind::Bool;
		Expr->bBool = bValue;
		Expr->Text = bValue ? TEXT("true") : TEXT("false");
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeNumberExpr(double Value)
	{
		// Number lexemes carry no sign: `-0.5` is a negation, in the parser's trees and the extractor's.
		if (Value < 0.0)
		{
			return MakeUnaryExpr(EDabUnaryOp::Negate, MakeNumberExpr(-Value));
		}
		TUniquePtr<FDabLiteralExpr> Expr = MakeUnique<FDabLiteralExpr>();
		Expr->LiteralKind = EDabLiteralKind::Number;
		Expr->Number = Value;
		Expr->Text = FormatDabNumber(Value);
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeParenExpr(FDabExprPtr Inner)
	{
		TUniquePtr<FDabParenExpr> Paren = MakeUnique<FDabParenExpr>();
		Paren->Inner = MoveTemp(Inner);
		return FDabExprPtr(MoveTemp(Paren));
	}

	FDabExprPtr MakeMemberExpr(FDabExprPtr Object, const FString& Member)
	{
		TUniquePtr<FDabMemberExpr> Expr = MakeUnique<FDabMemberExpr>();
		const bool bNeedsParens = Object.IsValid()
			&& (Object->Is<FDabBinaryExpr>() || Object->Is<FDabUnaryExpr>() || Object->Is<FDabConditionalExpr>());
		Expr->Object = bNeedsParens ? MakeParenExpr(MoveTemp(Object)) : MoveTemp(Object);
		Expr->Member = Member;
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeUnaryExpr(EDabUnaryOp Op, FDabExprPtr Operand)
	{
		TUniquePtr<FDabUnaryExpr> Expr = MakeUnique<FDabUnaryExpr>();
		Expr->Op = Op;
		const bool bNeedsParens = Operand.IsValid() && (Operand->Is<FDabBinaryExpr>() || Operand->Is<FDabConditionalExpr>());
		Expr->Operand = bNeedsParens ? MakeParenExpr(MoveTemp(Operand)) : MoveTemp(Operand);
		return FDabExprPtr(MoveTemp(Expr));
	}

	/** Whether Operand needs parentheses as one side of a binary operator of the given precedence. */
	bool SemanticOperandNeedsParens(const FDabExpr& Operand, int32 Precedence, bool bRightSide)
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

	FDabExprPtr MakeBinaryExpr(EDabBinaryOp Op, FDabExprPtr Lhs, FDabExprPtr Rhs)
	{
		const int32 Precedence = GetDabBinaryOpPrecedence(Op);
		TUniquePtr<FDabBinaryExpr> Expr = MakeUnique<FDabBinaryExpr>();
		Expr->Op = Op;
		Expr->Lhs = (Lhs.IsValid() && SemanticOperandNeedsParens(*Lhs, Precedence, false)) ? MakeParenExpr(MoveTemp(Lhs)) : MoveTemp(Lhs);
		Expr->Rhs = (Rhs.IsValid() && SemanticOperandNeedsParens(*Rhs, Precedence, true)) ? MakeParenExpr(MoveTemp(Rhs)) : MoveTemp(Rhs);
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeConditionalExpr(FDabExprPtr Condition, FDabExprPtr Then, FDabExprPtr Else)
	{
		TUniquePtr<FDabConditionalExpr> Expr = MakeUnique<FDabConditionalExpr>();
		const bool bNeedsParens = Condition.IsValid() && Condition->Is<FDabConditionalExpr>();
		Expr->Condition = bNeedsParens ? MakeParenExpr(MoveTemp(Condition)) : MoveTemp(Condition);
		Expr->Then = MoveTemp(Then);
		Expr->Else = MoveTemp(Else);
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabArgumentPtr MakeArgumentNode(const FString& Name, bool bNameIsString, FDabExprPtr Value)
	{
		FDabArgumentPtr Argument = MakeUnique<FDabArgument>();
		Argument->Name = Name;
		Argument->bNameIsString = bNameIsString;
		Argument->Value = MoveTemp(Value);
		return Argument;
	}

	FDabExprPtr MakeCallExpr(FDabExprPtr Callee, TArray<FDabArgumentPtr> Args)
	{
		TUniquePtr<FDabCallExpr> Expr = MakeUnique<FDabCallExpr>();
		Expr->Callee = MoveTemp(Callee);
		Expr->Args = MoveTemp(Args);
		return FDabExprPtr(MoveTemp(Expr));
	}

	FDabExprPtr MakeListExpr(TArray<FDabExprPtr> Items)
	{
		TUniquePtr<FDabListExpr> List = MakeUnique<FDabListExpr>();
		List->Items = MoveTemp(Items);
		return FDabExprPtr(MoveTemp(List));
	}

	// ------------------------------------------------------------------------------------------
	// Inspection
	// ------------------------------------------------------------------------------------------

	FString PrintValueText(const FDabExpr& Expr)
	{
		FDabPrintOptions PrintOptions;
		PrintOptions.MaxWidth = 1 << 24;
		return PrintDabExpr(Expr, 0, 0, nullptr, PrintOptions);
	}

	const FDabExpr& SkipParens(const FDabExpr& Expr)
	{
		const FDabExpr* Current = &Expr;
		while (const FDabParenExpr* Paren = Current->As<FDabParenExpr>())
		{
			if (!Paren->Inner)
			{
				break;
			}
			Current = Paren->Inner.Get();
		}
		return *Current;
	}

	bool ReadNumber(const FDabExpr& InExpr, double& OutValue)
	{
		const FDabExpr& Expr = SkipParens(InExpr);
		if (const FDabLiteralExpr* Literal = Expr.As<FDabLiteralExpr>())
		{
			if (Literal->LiteralKind != EDabLiteralKind::Number)
			{
				return false;
			}
			OutValue = Literal->Text.IsEmpty() ? Literal->Number : FCString::Atod(*Literal->Text);
			return true;
		}
		if (const FDabUnaryExpr* Unary = Expr.As<FDabUnaryExpr>())
		{
			double Inner = 0.0;
			if (Unary->Op == EDabUnaryOp::Negate && Unary->Operand && ReadNumber(*Unary->Operand, Inner))
			{
				OutValue = -Inner;
				return true;
			}
		}
		return false;
	}

	bool IsCallTo(const FDabExpr& Expr, const TCHAR* Callee)
	{
		const FDabCallExpr* Call = SkipParens(Expr).As<FDabCallExpr>();
		const FDabNameExpr* Name = Call && Call->Callee ? SkipParens(*Call->Callee).As<FDabNameExpr>() : nullptr;
		return Name && Name->Name == Callee;
	}

	bool IsPlaceholderCall(const FDabExpr& Expr)
	{
		return IsCallTo(Expr, TEXT("Unsupported"));
	}

	const FDabNameExpr* GetPathRoot(const FDabExpr& InExpr)
	{
		const FDabExpr* Current = &SkipParens(InExpr);
		for (int32 Guard = 0; Current && Guard < 256; ++Guard)
		{
			if (const FDabNameExpr* Name = Current->As<FDabNameExpr>())
			{
				return Name;
			}
			if (const FDabMemberExpr* Member = Current->As<FDabMemberExpr>())
			{
				Current = Member->Object ? &SkipParens(*Member->Object) : nullptr;
			}
			else if (const FDabCallExpr* Call = Current->As<FDabCallExpr>())
			{
				Current = Call->Callee ? &SkipParens(*Call->Callee) : nullptr;
			}
			else
			{
				return nullptr;
			}
		}
		return nullptr;
	}

	bool IsSectionRead(const FDabExpr& Expr, const TCHAR* Section, FString* OutMember)
	{
		const FDabMemberExpr* Member = SkipParens(Expr).As<FDabMemberExpr>();
		const FDabNameExpr* Object = Member && Member->Object ? SkipParens(*Member->Object).As<FDabNameExpr>() : nullptr;
		if (!Object || Object->Name != Section)
		{
			return false;
		}
		if (OutMember)
		{
			*OutMember = Member->Member;
		}
		return true;
	}

	bool IsEmptyTupleExpr(const FDabExpr& Expr)
	{
		const FDabTupleExpr* Tuple = Expr.As<FDabTupleExpr>();
		return Tuple && Tuple->Items.IsEmpty();
	}

	double FloatToShortestDoubleValue(float Value)
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

	/** The identifier rule the extractor applies to a plugin mount before writing `Plugin.<Mount>`. */
	bool IsSemanticIdentifier(const FString& Text)
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

	FDabExprPtr MakeObjectPathRef(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty() || ObjectPath == TEXT("None"))
		{
			return MakeNameExpr(TEXT("None"));
		}
		// Native types (classes, enums, structs) have no asset to point at.
		if (ObjectPath.StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive) || !ObjectPath.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
		{
			return MakeStringExpr(ObjectPath);
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
			return MakeStringExpr(ObjectPath);
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
			Root = MakeNameExpr(Mount);
		}
		else if (IsSemanticIdentifier(Mount))
		{
			Root = MakeMemberExpr(MakeNameExpr(TEXT("Plugin")), Mount);
		}
		else
		{
			return MakeStringExpr(ObjectPath);
		}

		TArray<FDabArgumentPtr> Args;
		Args.Add(MakeArgumentNode(FString(), false, MoveTemp(Root)));
		Args.Add(MakeArgumentNode(FString(), false, MakeStringExpr(Relative)));
		return MakeCallExpr(MakeNameExpr(TEXT("Path")), MoveTemp(Args));
	}

	FString GetSemanticClassAssetPath(const UClass* Class)
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
		FString Path = Class->GetPathName();
		Path.RemoveFromEnd(TEXT("_C"));
		return Path;
	}

	FDabExprPtr MakeObjectRef(const UObject* Object)
	{
		if (!Object)
		{
			return MakeNameExpr(TEXT("None"));
		}
		if (const UClass* Class = Cast<UClass>(Object))
		{
			if (Class->HasAnyClassFlags(CLASS_Native))
			{
				return MakeStringExpr(Class->GetPathName());
			}
			return MakeObjectPathRef(GetSemanticClassAssetPath(Class));
		}
		return MakeObjectPathRef(Object->GetPathName());
	}

	/** `Game` / `Engine` / `Plugin.X` (as a name, a member or a string) -> the mount point. */
	bool ReadMountRoot(const FDabExpr& InRoot, FString& OutMount)
	{
		const FDabExpr& Root = SkipParens(InRoot);
		FString Text;
		if (const FDabNameExpr* Name = Root.As<FDabNameExpr>())
		{
			Text = Name->Name;
		}
		else if (const FDabMemberExpr* Member = Root.As<FDabMemberExpr>())
		{
			const FDabNameExpr* Object = Member->Object ? SkipParens(*Member->Object).As<FDabNameExpr>() : nullptr;
			if (!Object || Object->Name != TEXT("Plugin"))
			{
				return false;
			}
			OutMount = Member->Member;
			return !OutMount.IsEmpty();
		}
		else if (const FDabLiteralExpr* Literal = Root.As<FDabLiteralExpr>(); Literal && Literal->LiteralKind == EDabLiteralKind::String)
		{
			Text = Literal->Text;
		}
		else
		{
			return false;
		}

		if (Text == TEXT("Game") || Text == TEXT("Engine"))
		{
			OutMount = Text;
			return true;
		}
		if (Text.StartsWith(TEXT("Plugin."), ESearchCase::CaseSensitive))
		{
			OutMount = Text.RightChop(7);
			return !OutMount.IsEmpty();
		}
		return false;
	}

	bool TryGetPathCallObjectPath(const FDabCallExpr& Call, FString& OutObjectPath, FString& OutError)
	{
		if (Call.Args.Num() != 2 || !Call.Args[0] || !Call.Args[1] || Call.Args[0]->IsNamed() || Call.Args[1]->IsNamed()
			|| !Call.Args[0]->Value || !Call.Args[1]->Value)
		{
			OutError = TEXT("Path takes a root and a relative path: Path(Plugin.ALS, \"ALS/Character/SK_Als\").");
			return false;
		}
		FString Mount;
		if (!ReadMountRoot(*Call.Args[0]->Value, Mount))
		{
			OutError = FString::Printf(TEXT("`%s` is not a root; a root is Game, Engine or Plugin.<Name>."), *PrintValueText(*Call.Args[0]->Value));
			return false;
		}
		const FDabLiteralExpr* Relative = SkipParens(*Call.Args[1]->Value).As<FDabLiteralExpr>();
		if (!Relative || Relative->LiteralKind != EDabLiteralKind::String || Relative->Text.IsEmpty())
		{
			OutError = TEXT("The second argument of Path is the asset's path under the root, as a string.");
			return false;
		}

		FString Rel = Relative->Text;
		Rel.RemoveFromStart(TEXT("/"));
		FString Main = Rel;
		FString SubObject;
		Rel.Split(TEXT(":"), &Main, &SubObject, ESearchCase::CaseSensitive);

		int32 LastSlash = INDEX_NONE;
		Main.FindLastChar(TEXT('/'), LastSlash);
		FString PackageRel = Main;
		FString Object;
		const int32 Dot = Main.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (Dot != INDEX_NONE && Dot > LastSlash)
		{
			PackageRel = Main.Left(Dot);
			Object = Main.Mid(Dot + 1);
		}
		int32 PackageSlash = INDEX_NONE;
		PackageRel.FindLastChar(TEXT('/'), PackageSlash);
		if (Object.IsEmpty())
		{
			Object = PackageRel.Mid(PackageSlash + 1);
		}

		OutObjectPath = FString::Printf(TEXT("/%s/%s.%s"), *Mount, *PackageRel, *Object);
		if (!SubObject.IsEmpty())
		{
			OutObjectPath += TEXT(":") + SubObject;
		}
		return true;
	}

	FString NormalizeAssetObjectPath(const FString& ObjectPath)
	{
		// `/A/B.B:Sub` names a subobject of the asset (a blend profile in a skeleton): the leaf goes before the `:`.
		FString Main = ObjectPath;
		FString SubObject;
		ObjectPath.Split(TEXT(":"), &Main, &SubObject, ESearchCase::CaseSensitive);

		FString Package = Main;
		FString Object;
		const int32 LastSlash = Main.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		const int32 Dot = Main.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (Dot != INDEX_NONE && Dot > LastSlash)
		{
			Package = Main.Left(Dot);
			Object = Main.Mid(Dot + 1);
		}
		const FString ShortName = Package.Mid(LastSlash + 1);
		if (Object.IsEmpty() || Object == ShortName + TEXT("_C"))
		{
			Object = ShortName;
		}
		FString Result = Package + TEXT(".") + Object;
		if (!SubObject.IsEmpty())
		{
			Result += TEXT(":") + SubObject;
		}
		return Result;
	}

	// ------------------------------------------------------------------------------------------
	// Temporaries
	// ------------------------------------------------------------------------------------------

	FSemTempValue::FSemTempValue(const FProperty& InProperty)
		: Property(InProperty)
	{
		Memory = static_cast<uint8*>(FMemory::Malloc(FMath::Max(1, Property.GetSize()), Property.GetMinAlignment()));
		Property.InitializeValue(Memory);
	}

	FSemTempValue::~FSemTempValue()
	{
		Property.DestroyValue(Memory);
		FMemory::Free(Memory);
	}

	FSemTempStruct::FSemTempStruct(const UScriptStruct& InStruct)
		: Struct(InStruct)
	{
		Memory = static_cast<uint8*>(FMemory::Malloc(FMath::Max(1, Struct.GetStructureSize()), Struct.GetMinAlignment()));
		Struct.InitializeStruct(Memory);
	}

	FSemTempStruct::~FSemTempStruct()
	{
		Struct.DestroyStruct(Memory);
		FMemory::Free(Memory);
	}

	// ------------------------------------------------------------------------------------------
	// Memory -> model form (the extractor's ConvertPropertyValue)
	// ------------------------------------------------------------------------------------------

	/** The enumerator's bare name (`Walking`, `TLT_StandardBlend`), or the number when it names none. */
	FDabExprPtr MakeEnumNameExpr(const UEnum& Enum, int64 Value)
	{
		if (Enum.IsValidEnumValue(Value))
		{
			FString Name = Enum.GetNameStringByValue(Value);
			int32 Scope = INDEX_NONE;
			if (Name.FindLastChar(TEXT(':'), Scope))
			{
				Name.RightChopInline(Scope + 1);
			}
			if (!Name.IsEmpty())
			{
				return MakeNameExpr(Name);
			}
		}
		return MakeNumberExpr(static_cast<double>(Value));
	}

	/** Base struct's fields first, each struct's in declaration order: the order tuples print in. */
	TArray<const FProperty*> GetSemanticStructFields(const UStruct& Struct)
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

	/** A struct field a tuple never holds: not saved, or a GUID (identity, not behaviour). */
	bool IsSkippedStructField(const FProperty& Field)
	{
		if (Field.HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
		{
			return true;
		}
		const FStructProperty* FieldStruct = CastField<FStructProperty>(&Field);
		return FieldStruct && FieldStruct->Struct == TBaseStructure<FGuid>::Get();
	}

	FDabExprPtr StructMemoryToExpr(const UScriptStruct& Struct, const void* Value, const void* Default)
	{
		if (&Struct == FGameplayTag::StaticStruct())
		{
			const FName TagName = static_cast<const FGameplayTag*>(Value)->GetTagName();
			return MakeStringExpr(TagName.IsNone() ? FString() : TagName.ToString());
		}
		if (&Struct == FGameplayTagContainer::StaticStruct())
		{
			TArray<FDabExprPtr> Items;
			for (const FGameplayTag& Tag : static_cast<const FGameplayTagContainer*>(Value)->GetGameplayTagArray())
			{
				Items.Add(MakeStringExpr(Tag.GetTagName().ToString()));
			}
			return MakeListExpr(MoveTemp(Items));
		}

		TUniquePtr<FSemTempStruct> Fallback;
		if (!Default)
		{
			Fallback = MakeUnique<FSemTempStruct>(Struct);
			Default = Fallback->Get();
		}

		TUniquePtr<FDabTupleExpr> Tuple = MakeUnique<FDabTupleExpr>();
		for (const FProperty* Field : GetSemanticStructFields(Struct))
		{
			if (IsSkippedStructField(*Field) || Field->ArrayDim > 1)
			{
				continue;
			}
			const void* FieldValue = Field->ContainerPtrToValuePtr<void>(Value);
			const void* FieldDefault = Field->ContainerPtrToValuePtr<void>(Default);
			if (Field->Identical(FieldValue, FieldDefault, 0))
			{
				continue;
			}
			// Nested structs compare with a default-initialised struct, not with the outer default's field.
			FDabExprPtr Item = ValueToExpr(*Field, FieldValue, nullptr);
			if (Item)
			{
				Tuple->Items.Add(MakeArgumentNode(Field->GetName(), false, MoveTemp(Item)));
			}
		}
		return FDabExprPtr(MoveTemp(Tuple));
	}

	/** A map key as the string that names a tuple item. */
	bool GetSemanticMapKeyText(const FProperty& KeyProperty, const void* Key, FString& OutText)
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

	/** Items sorted by their key text, case-sensitively: the canonical order of sets and maps. */
	void SortSemanticItems(TArray<TPair<FString, FDabExprPtr>>& Items)
	{
		Items.StableSort([](const TPair<FString, FDabExprPtr>& A, const TPair<FString, FDabExprPtr>& B)
		{
			return A.Key.Compare(B.Key, ESearchCase::CaseSensitive) < 0;
		});
	}

	FDabExprPtr ValueToExpr(const FProperty& Property, const void* Value, const void* Default)
	{
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
		{
			return MakeBoolExpr(Bool->GetPropertyValue(Value));
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
		{
			const UEnum* Enum = EnumProperty->GetEnum();
			const int64 EnumValue = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value);
			return Enum ? MakeEnumNameExpr(*Enum, EnumValue) : MakeNumberExpr(static_cast<double>(EnumValue));
		}
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
		{
			if (const UEnum* Enum = Numeric->GetIntPropertyEnum())
			{
				return MakeEnumNameExpr(*Enum, Numeric->GetSignedIntPropertyValue(Value));
			}
			if (const FFloatProperty* Float = CastField<FFloatProperty>(&Property))
			{
				return MakeNumberExpr(FloatToShortestDoubleValue(Float->GetPropertyValue(Value)));
			}
			if (Numeric->IsFloatingPoint())
			{
				return MakeNumberExpr(Numeric->GetFloatingPointPropertyValue(Value));
			}
			return MakeNumberExpr(static_cast<double>(Numeric->GetSignedIntPropertyValue(Value)));
		}
		if (const FNameProperty* Name = CastField<FNameProperty>(&Property))
		{
			const FName NameValue = Name->GetPropertyValue(Value);
			return MakeStringExpr(NameValue.IsNone() ? FString() : NameValue.ToString());
		}
		if (const FStrProperty* String = CastField<FStrProperty>(&Property))
		{
			return MakeStringExpr(String->GetPropertyValue(Value));
		}
		if (const FTextProperty* Text = CastField<FTextProperty>(&Property))
		{
			return MakeStringExpr(Text->GetPropertyValue(Value).ToString());
		}
		if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(&Property))
		{
			return MakeObjectPathRef(Soft->GetPropertyValue(Value).ToSoftObjectPath().ToString());
		}
		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
		{
			return MakeObjectRef(Object->GetObjectPropertyValue(Value));
		}
		if (const FInterfaceProperty* Interface = CastField<FInterfaceProperty>(&Property))
		{
			return MakeObjectRef(Interface->GetPropertyValue(Value).GetObject());
		}
		if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
		{
			return StructMemoryToExpr(*Struct->Struct, Value, Default);
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
		{
			FScriptArrayHelper Helper(Array, Value);
			TArray<FDabExprPtr> Items;
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				FDabExprPtr Item = ValueToExpr(*Array->Inner, Helper.GetRawPtr(Index), nullptr);
				if (!Item)
				{
					return nullptr;
				}
				Items.Add(MoveTemp(Item));
			}
			return MakeListExpr(MoveTemp(Items));
		}
		if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
		{
			FScriptSetHelper Helper(Set, Value);
			TArray<TPair<FString, FDabExprPtr>> Items;
			for (FScriptSetHelper::FIterator It(Helper); It; ++It)
			{
				FDabExprPtr Item = ValueToExpr(*Set->ElementProp, Helper.GetElementPtr(It), nullptr);
				if (!Item)
				{
					return nullptr;
				}
				FString Key = PrintValueText(*Item);
				Items.Emplace(MoveTemp(Key), MoveTemp(Item));
			}
			SortSemanticItems(Items);
			TArray<FDabExprPtr> Sorted;
			for (TPair<FString, FDabExprPtr>& Item : Items)
			{
				Sorted.Add(MoveTemp(Item.Value));
			}
			return MakeListExpr(MoveTemp(Sorted));
		}
		if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
		{
			FScriptMapHelper Helper(Map, Value);
			if (Helper.Num() == 0)
			{
				return MakeListExpr(TArray<FDabExprPtr>());
			}
			TArray<TPair<FString, FDabExprPtr>> Items;
			for (FScriptMapHelper::FIterator It(Helper); It; ++It)
			{
				FString Key;
				if (!GetSemanticMapKeyText(*Map->KeyProp, Helper.GetKeyPtr(It), Key))
				{
					return nullptr;
				}
				FDabExprPtr Item = ValueToExpr(*Map->ValueProp, Helper.GetValuePtr(It), nullptr);
				if (!Item)
				{
					return nullptr;
				}
				Items.Emplace(MoveTemp(Key), MoveTemp(Item));
			}
			SortSemanticItems(Items);
			TUniquePtr<FDabTupleExpr> Tuple = MakeUnique<FDabTupleExpr>();
			for (TPair<FString, FDabExprPtr>& Item : Items)
			{
				Tuple->Items.Add(MakeArgumentNode(Item.Key, true, MoveTemp(Item.Value)));
			}
			return FDabExprPtr(MoveTemp(Tuple));
		}
		return nullptr;
	}

	FDabExprPtr ZeroValueExpr(const FProperty& Property)
	{
		FSemTempValue Temp(Property);
		FDabExprPtr Value = ValueToExpr(Property, Temp.Get(), nullptr);
		return Value ? MoveTemp(Value) : MakeNumberExpr(0.0);
	}

	void AddPropertyUnlessDefault(TArray<FDabModelProperty>& Out, const FString& Name, FDabExprPtr Value, const FProperty* Property, const void* DefaultValue)
	{
		if (!Value || IsEmptyTupleExpr(*Value))
		{
			return;
		}
		if (Property && DefaultValue)
		{
			const FDabExprPtr DefaultExpr = ValueToExpr(*Property, DefaultValue, DefaultValue);
			if (DefaultExpr && PrintValueText(*DefaultExpr) == PrintValueText(*Value))
			{
				Out.RemoveAll([&Name](const FDabModelProperty& Existing) { return Existing.Name == Name; });
				return;
			}
		}
		Out.RemoveAll([&Name](const FDabModelProperty& Existing) { return Existing.Name == Name; });
		FDabModelProperty Entry;
		Entry.Name = Name;
		Entry.Value = MoveTemp(Value);
		Out.Add(MoveTemp(Entry));
	}

	// ------------------------------------------------------------------------------------------
	// Types
	// ------------------------------------------------------------------------------------------

	void SetIntegerRange(FSemType& Type, int64 Min, int64 Max)
	{
		Type.Kind = ESemTypeKind::Integer;
		Type.MinInteger = Min;
		Type.MaxInteger = Max;
	}

	FSemType FSemType::FromProperty(const FProperty& Property)
	{
		FSemType Type;
		if (CastField<FBoolProperty>(&Property))
		{
			Type.Kind = ESemTypeKind::Bool;
		}
		else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
		{
			Type.Kind = EnumProperty->GetEnum() ? ESemTypeKind::Enum : ESemTypeKind::Integer;
			Type.Enum = EnumProperty->GetEnum();
		}
		else if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
		{
			if (const UEnum* IntEnum = Numeric->GetIntPropertyEnum())
			{
				Type.Kind = ESemTypeKind::Enum;
				Type.Enum = IntEnum;
			}
			else if (CastField<FFloatProperty>(&Property))
			{
				Type.Kind = ESemTypeKind::Float;
			}
			else if (Numeric->IsFloatingPoint())
			{
				Type.Kind = ESemTypeKind::Double;
			}
			else if (CastField<FByteProperty>(&Property))
			{
				SetIntegerRange(Type, 0, MAX_uint8);
			}
			else if (CastField<FInt8Property>(&Property))
			{
				SetIntegerRange(Type, MIN_int8, MAX_int8);
			}
			else if (CastField<FInt16Property>(&Property))
			{
				SetIntegerRange(Type, MIN_int16, MAX_int16);
			}
			else if (CastField<FUInt16Property>(&Property))
			{
				SetIntegerRange(Type, 0, MAX_uint16);
			}
			else if (CastField<FIntProperty>(&Property))
			{
				SetIntegerRange(Type, MIN_int32, MAX_int32);
			}
			else if (CastField<FUInt32Property>(&Property))
			{
				SetIntegerRange(Type, 0, MAX_uint32);
			}
			else if (CastField<FUInt64Property>(&Property))
			{
				SetIntegerRange(Type, 0, MAX_int64);
			}
			else
			{
				SetIntegerRange(Type, MIN_int64, MAX_int64);
			}
		}
		else if (CastField<FNameProperty>(&Property))
		{
			Type.Kind = ESemTypeKind::Name;
		}
		else if (CastField<FStrProperty>(&Property))
		{
			Type.Kind = ESemTypeKind::String;
		}
		else if (CastField<FTextProperty>(&Property))
		{
			Type.Kind = ESemTypeKind::Text;
		}
		else if (const FSoftClassProperty* SoftClass = CastField<FSoftClassProperty>(&Property))
		{
			Type.Kind = ESemTypeKind::SoftClass;
			Type.Class = SoftClass->MetaClass;
		}
		else if (const FSoftObjectProperty* SoftObject = CastField<FSoftObjectProperty>(&Property))
		{
			Type.Kind = ESemTypeKind::SoftObject;
			Type.Class = SoftObject->PropertyClass;
		}
		else if (const FClassProperty* ClassProperty = CastField<FClassProperty>(&Property))
		{
			Type.Kind = ESemTypeKind::Class;
			Type.Class = ClassProperty->MetaClass;
		}
		else if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
		{
			Type.Kind = ESemTypeKind::Object;
			Type.Class = Object->PropertyClass;
		}
		else if (CastField<FInterfaceProperty>(&Property))
		{
			Type.Kind = ESemTypeKind::Object;
		}
		else if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
		{
			if (StructProperty->Struct == FGameplayTag::StaticStruct())
			{
				Type.Kind = ESemTypeKind::GameplayTag;
			}
			else if (StructProperty->Struct == FGameplayTagContainer::StaticStruct())
			{
				Type.Kind = ESemTypeKind::GameplayTagContainer;
			}
			else
			{
				Type.Kind = ESemTypeKind::Struct;
				Type.Struct = StructProperty->Struct;
			}
		}
		else if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
		{
			Type.Kind = ESemTypeKind::Array;
			Type.Inner = MakeShared<FSemType>(FromProperty(*Array->Inner));
		}
		else if (const FSetProperty* Set = CastField<FSetProperty>(&Property))
		{
			Type.Kind = ESemTypeKind::Set;
			Type.Inner = MakeShared<FSemType>(FromProperty(*Set->ElementProp));
		}
		else if (const FMapProperty* Map = CastField<FMapProperty>(&Property))
		{
			Type.Kind = ESemTypeKind::Map;
			Type.Inner = MakeShared<FSemType>(FromProperty(*Map->KeyProp));
			Type.Value = MakeShared<FSemType>(FromProperty(*Map->ValueProp));
		}
		return Type;
	}

	/** `TArray<float>` -> ("TArray", ["float"]); arguments split at top-level commas. False for a plain name. */
	bool SplitTemplateSpelling(const FString& Spelling, FString& OutName, TArray<FString>& OutArgs)
	{
		int32 Open = INDEX_NONE;
		if (!Spelling.FindChar(TEXT('<'), Open) || !Spelling.EndsWith(TEXT(">")))
		{
			return false;
		}
		OutName = Spelling.Left(Open).TrimStartAndEnd();
		const FString Inside = Spelling.Mid(Open + 1, Spelling.Len() - Open - 2);
		int32 Depth = 0;
		FString Current;
		for (const TCHAR Char : Inside)
		{
			if (Char == TEXT('<'))
			{
				++Depth;
			}
			else if (Char == TEXT('>'))
			{
				--Depth;
			}
			if (Char == TEXT(',') && Depth == 0)
			{
				OutArgs.Add(Current.TrimStartAndEnd());
				Current.Reset();
				continue;
			}
			Current.AppendChar(Char);
		}
		OutArgs.Add(Current.TrimStartAndEnd());
		return true;
	}

	UClass* FindSemanticClassBySpelling(const FString& Name)
	{
		if (Name.IsEmpty())
		{
			return nullptr;
		}
		if (UClass* Class = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst))
		{
			return Class;
		}
		// The extractor spells a Blueprint class by its Blueprint's name, without `_C`.
		if (UClass* GeneratedClass = FindFirstObject<UClass>(*(Name + TEXT("_C")), EFindFirstObjectOptions::NativeFirst))
		{
			return GeneratedClass;
		}
		if (Name.Len() > 1 && (Name[0] == TEXT('U') || Name[0] == TEXT('A')))
		{
			return FindFirstObject<UClass>(*Name.RightChop(1), EFindFirstObjectOptions::NativeFirst);
		}
		return nullptr;
	}

	FSemType FSemType::FromSpelling(const FString& InSpelling)
	{
		const FString Spelling = InSpelling.TrimStartAndEnd();
		FSemType Type;

		FString TemplateName;
		TArray<FString> Args;
		if (SplitTemplateSpelling(Spelling, TemplateName, Args))
		{
			if ((TemplateName == TEXT("TArray") || TemplateName == TEXT("TSet")) && Args.Num() == 1)
			{
				Type.Kind = TemplateName == TEXT("TArray") ? ESemTypeKind::Array : ESemTypeKind::Set;
				Type.Inner = MakeShared<FSemType>(FromSpelling(Args[0]));
			}
			else if (TemplateName == TEXT("TMap") && Args.Num() == 2)
			{
				Type.Kind = ESemTypeKind::Map;
				Type.Inner = MakeShared<FSemType>(FromSpelling(Args[0]));
				Type.Value = MakeShared<FSemType>(FromSpelling(Args[1]));
			}
			else if (Args.Num() == 1 && (TemplateName == TEXT("TSubclassOf") || TemplateName == TEXT("TSoftClassPtr")
				|| TemplateName == TEXT("TSoftObjectPtr") || TemplateName == TEXT("TObjectPtr")))
			{
				Type.Kind = TemplateName == TEXT("TSubclassOf") ? ESemTypeKind::Class
					: TemplateName == TEXT("TSoftClassPtr") ? ESemTypeKind::SoftClass
					: TemplateName == TEXT("TSoftObjectPtr") ? ESemTypeKind::SoftObject
					: ESemTypeKind::Object;
				Type.Class = FindSemanticClassBySpelling(Args[0]);
			}
			return Type;
		}

		if (Spelling == TEXT("bool"))
		{
			Type.Kind = ESemTypeKind::Bool;
		}
		else if (Spelling == TEXT("int") || Spelling == TEXT("int32"))
		{
			SetIntegerRange(Type, MIN_int32, MAX_int32);
		}
		else if (Spelling == TEXT("int64"))
		{
			SetIntegerRange(Type, MIN_int64, MAX_int64);
		}
		else if (Spelling == TEXT("uint8") || Spelling == TEXT("byte"))
		{
			SetIntegerRange(Type, 0, MAX_uint8);
		}
		else if (Spelling == TEXT("float") || Spelling == TEXT("double") || Spelling == TEXT("real"))
		{
			// A Blueprint `float` is a double since the engine's move to large worlds.
			Type.Kind = ESemTypeKind::Double;
		}
		else if (Spelling == TEXT("FName") || Spelling == TEXT("Name"))
		{
			Type.Kind = ESemTypeKind::Name;
		}
		else if (Spelling == TEXT("FString") || Spelling == TEXT("String"))
		{
			Type.Kind = ESemTypeKind::String;
		}
		else if (Spelling == TEXT("FText") || Spelling == TEXT("Text"))
		{
			Type.Kind = ESemTypeKind::Text;
		}
		else if (Spelling == TEXT("GameplayTag") || Spelling == TEXT("FGameplayTag"))
		{
			Type.Kind = ESemTypeKind::GameplayTag;
		}
		else if (Spelling == TEXT("GameplayTagContainer") || Spelling == TEXT("FGameplayTagContainer"))
		{
			Type.Kind = ESemTypeKind::GameplayTagContainer;
		}
		else if (const UEnum* FoundEnum = FindFirstObject<UEnum>(*Spelling, EFindFirstObjectOptions::NativeFirst))
		{
			Type.Kind = ESemTypeKind::Enum;
			Type.Enum = FoundEnum;
		}
		else if (const UScriptStruct* FoundStruct = FindFirstObject<UScriptStruct>(*Spelling, EFindFirstObjectOptions::NativeFirst))
		{
			Type.Kind = ESemTypeKind::Struct;
			Type.Struct = FoundStruct;
		}
		else if (const UScriptStruct* PrefixedStruct = Spelling.StartsWith(TEXT("F"), ESearchCase::CaseSensitive)
			? FindFirstObject<UScriptStruct>(*Spelling.RightChop(1), EFindFirstObjectOptions::NativeFirst) : nullptr)
		{
			Type.Kind = ESemTypeKind::Struct;
			Type.Struct = PrefixedStruct;
		}
		else if (const UClass* FoundClass = FindSemanticClassBySpelling(Spelling))
		{
			Type.Kind = ESemTypeKind::Object;
			Type.Class = FoundClass;
		}
		return Type;
	}

	FString DescribeSemanticType(const FSemType& Type)
	{
		switch (Type.Kind)
		{
		case ESemTypeKind::Bool:                 return TEXT("true or false");
		case ESemTypeKind::Integer:              return TEXT("a whole number");
		case ESemTypeKind::Float:
		case ESemTypeKind::Double:               return TEXT("a number");
		case ESemTypeKind::Enum:                 return FString::Printf(TEXT("a value of %s"), Type.Enum ? *Type.Enum->GetName() : TEXT("an enum"));
		case ESemTypeKind::Name:
		case ESemTypeKind::String:
		case ESemTypeKind::Text:                 return TEXT("a string");
		case ESemTypeKind::GameplayTag:          return TEXT("a gameplay tag string (\"A.B.C\")");
		case ESemTypeKind::GameplayTagContainer: return TEXT("a list of gameplay tag strings");
		case ESemTypeKind::Struct:               return FString::Printf(TEXT("a tuple of %s fields"), Type.Struct ? *Type.Struct->GetName() : TEXT("struct"));
		case ESemTypeKind::Object:
		case ESemTypeKind::SoftObject:           return TEXT("an asset: Path(...), its name, or None");
		case ESemTypeKind::Class:
		case ESemTypeKind::SoftClass:            return TEXT("a class: a C++ class name, a Blueprint's Path(...) or name, or None");
		case ESemTypeKind::Array:
		case ESemTypeKind::Set:                  return TEXT("a list [ ... ]");
		case ESemTypeKind::Map:                  return TEXT("a map (\"Key\" = Value, ...)");
		default:                                 return TEXT("a constant");
		}
	}

	// ------------------------------------------------------------------------------------------
	// Written values
	// ------------------------------------------------------------------------------------------

	bool FSemantic::IsConstant(const FDabExpr& InExpr) const
	{
		const FDabExpr& Expr = SkipParens(InExpr);
		switch (Expr.Kind)
		{
		case EDabNodeKind::LiteralExpr:
			return true;
		case EDabNodeKind::NameExpr:
		{
			const FString& Name = static_cast<const FDabNameExpr&>(Expr).Name;
			return Name != TEXT("Self") && Name != TEXT("Props") && Name != TEXT("Assets") && !Aliases.Contains(Name);
		}
		case EDabNodeKind::UnaryExpr:
		{
			double Value = 0.0;
			return ReadNumber(Expr, Value);
		}
		case EDabNodeKind::CallExpr:
			return IsCallTo(Expr, TEXT("Path")) || IsCallTo(Expr, TEXT("bones"));
		case EDabNodeKind::ListExpr:
			for (const FDabExprPtr& Item : static_cast<const FDabListExpr&>(Expr).Items)
			{
				if (Item && !IsConstant(*Item))
				{
					return false;
				}
			}
			return true;
		case EDabNodeKind::TupleExpr:
			for (const FDabArgumentPtr& Item : static_cast<const FDabTupleExpr&>(Expr).Items)
			{
				if (Item && Item->Value && !IsConstant(*Item->Value))
				{
					return false;
				}
			}
			return true;
		case EDabNodeKind::BlockExpr:
			for (const FDabStmtPtr& Statement : static_cast<const FDabBlockExpr&>(Expr).Statements)
			{
				const FDabAssignStmt* Assign = Statement ? Statement->As<FDabAssignStmt>() : nullptr;
				if (Assign && Assign->Value && !IsConstant(*Assign->Value))
				{
					return false;
				}
			}
			return true;
		default:
			return false;
		}
	}

	FDabExprPtr FSemantic::Resolve(const FDabExpr& InExpr, FRuleSet* Rules, bool bInGetter)
	{
		const FDabExpr& Expr = SkipParens(InExpr);
		switch (Expr.Kind)
		{
		case EDabNodeKind::LiteralExpr:
		{
			const FDabLiteralExpr& Literal = static_cast<const FDabLiteralExpr&>(Expr);
			if (Literal.LiteralKind == EDabLiteralKind::Number)
			{
				double Value = 0.0;
				ReadNumber(Literal, Value);
				return MakeNumberExpr(Value);
			}
			return CloneExpr(Literal);
		}
		case EDabNodeKind::NameExpr:
		{
			const FString& Name = static_cast<const FDabNameExpr&>(Expr).Name;
			if (const FDabExprPtr* Alias = Aliases.Find(Name))
			{
				return CloneExpr(**Alias);
			}
			if (Rules && !bInGetter && Rules->Statements.Contains(Name))
			{
				const FDabExpr* Rule = ResolveRule(*Rules, Name, Expr.Span);
				return Rule ? CloneExpr(*Rule) : nullptr;
			}
			return MakeNameExpr(Name);
		}
		case EDabNodeKind::MemberExpr:
		{
			const FDabMemberExpr& Member = static_cast<const FDabMemberExpr&>(Expr);
			FString Read;
			if (IsSectionRead(Member, TEXT("Props"), &Read) && !VariableIndex.Contains(Read))
			{
				Sink.Error(TEXT("DAB5003"), Member.MemberSpan.IsEmpty() ? Member.Span : Member.MemberSpan,
					FString::Printf(TEXT("There is no tuning value `%s`."), *Read),
					TEXT("Declare it in Properties = { float Name = 0; }."));
				return nullptr;
			}
			if (IsSectionRead(Member, TEXT("Assets"), &Read) && !SlotNames.Contains(Read))
			{
				Sink.Error(TEXT("DAB5003"), Member.MemberSpan.IsEmpty() ? Member.Span : Member.MemberSpan,
					FString::Printf(TEXT("There is no asset slot `%s`."), *Read),
					TEXT("Declare it in Assets = { AnimSequence Name; }."));
				return nullptr;
			}
			FDabExprPtr Object = Member.Object ? Resolve(*Member.Object, Rules, bInGetter) : nullptr;
			if (!Object)
			{
				return nullptr;
			}
			return MakeMemberExpr(MoveTemp(Object), Member.Member);
		}
		case EDabNodeKind::CallExpr:
		{
			const FDabCallExpr& Call = static_cast<const FDabCallExpr&>(Expr);
			if (IsPlaceholderCall(Call))
			{
				return CloneExpr(Call);
			}
			if (!Call.Callee)
			{
				return nullptr;
			}
			const FDabNameExpr* CalleeName = SkipParens(*Call.Callee).As<FDabNameExpr>();
			const bool bGetter = CalleeName && (CalleeName->Name == TEXT("StateWeight") || CalleeName->Name == TEXT("MachineWeight"));
			// A function's own name is never a rule or an alias.
			FDabExprPtr Callee = CalleeName && !Aliases.Contains(CalleeName->Name) ? MakeNameExpr(CalleeName->Name) : Resolve(*Call.Callee, Rules, bInGetter);
			if (!Callee)
			{
				return nullptr;
			}
			TArray<FDabArgumentPtr> Args;
			for (const FDabArgumentPtr& Arg : Call.Args)
			{
				if (!Arg || !Arg->Value)
				{
					continue;
				}
				FDabExprPtr Value = Resolve(*Arg->Value, Rules, bInGetter || bGetter);
				if (!Value)
				{
					return nullptr;
				}
				Args.Add(MakeArgumentNode(Arg->Name, Arg->bNameIsString, MoveTemp(Value)));
			}
			return MakeCallExpr(MoveTemp(Callee), MoveTemp(Args));
		}
		case EDabNodeKind::UnaryExpr:
		{
			const FDabUnaryExpr& Unary = static_cast<const FDabUnaryExpr&>(Expr);
			FDabExprPtr Operand = Unary.Operand ? Resolve(*Unary.Operand, Rules, bInGetter) : nullptr;
			if (!Operand)
			{
				return nullptr;
			}
			double Value = 0.0;
			if (Unary.Op == EDabUnaryOp::Negate && ReadNumber(*Operand, Value))
			{
				return MakeNumberExpr(-Value);
			}
			return MakeUnaryExpr(Unary.Op, MoveTemp(Operand));
		}
		case EDabNodeKind::BinaryExpr:
		{
			const FDabBinaryExpr& Binary = static_cast<const FDabBinaryExpr&>(Expr);
			FDabExprPtr Lhs = Binary.Lhs ? Resolve(*Binary.Lhs, Rules, bInGetter) : nullptr;
			FDabExprPtr Rhs = Binary.Rhs ? Resolve(*Binary.Rhs, Rules, bInGetter) : nullptr;
			if (!Lhs || !Rhs)
			{
				return nullptr;
			}
			return MakeBinaryExpr(Binary.Op, MoveTemp(Lhs), MoveTemp(Rhs));
		}
		case EDabNodeKind::ConditionalExpr:
		{
			const FDabConditionalExpr& Conditional = static_cast<const FDabConditionalExpr&>(Expr);
			FDabExprPtr Condition = Conditional.Condition ? Resolve(*Conditional.Condition, Rules, bInGetter) : nullptr;
			FDabExprPtr Then = Conditional.Then ? Resolve(*Conditional.Then, Rules, bInGetter) : nullptr;
			FDabExprPtr Else = Conditional.Else ? Resolve(*Conditional.Else, Rules, bInGetter) : nullptr;
			if (!Condition || !Then || !Else)
			{
				return nullptr;
			}
			return MakeConditionalExpr(MoveTemp(Condition), MoveTemp(Then), MoveTemp(Else));
		}
		case EDabNodeKind::ListExpr:
		{
			TArray<FDabExprPtr> Items;
			for (const FDabExprPtr& Item : static_cast<const FDabListExpr&>(Expr).Items)
			{
				FDabExprPtr Value = Item ? Resolve(*Item, Rules, bInGetter) : nullptr;
				if (!Value)
				{
					return nullptr;
				}
				Items.Add(MoveTemp(Value));
			}
			return MakeListExpr(MoveTemp(Items));
		}
		case EDabNodeKind::TupleExpr:
		{
			TUniquePtr<FDabTupleExpr> Tuple = MakeUnique<FDabTupleExpr>();
			for (const FDabArgumentPtr& Item : static_cast<const FDabTupleExpr&>(Expr).Items)
			{
				FDabExprPtr Value = Item && Item->Value ? Resolve(*Item->Value, Rules, bInGetter) : nullptr;
				if (!Value)
				{
					return nullptr;
				}
				Tuple->Items.Add(MakeArgumentNode(Item->Name, Item->bNameIsString, MoveTemp(Value)));
			}
			return FDabExprPtr(MoveTemp(Tuple));
		}
		default:
			Sink.Error(TEXT("DAB5005"), Expr.Span, TEXT("A block or a stack is not a value here."),
				TEXT("Values are constants, paths such as Main.Speed, Props.X, Assets.X, or expressions of them."));
			return nullptr;
		}
	}

	const FDabExpr* FSemantic::FindPropsDefault(const FDabExpr& Expr)
	{
		FString Name;
		if (!IsSectionRead(Expr, TEXT("Props"), &Name))
		{
			return nullptr;
		}
		const int32* Index = VariableIndex.Find(Name);
		if (!Index || !Model.Variables.IsValidIndex(*Index))
		{
			Sink.Error(TEXT("DAB5003"), Expr.Span, FString::Printf(TEXT("There is no tuning value `%s`."), *Name),
				TEXT("Declare it in Properties = { float Name = 0; }."));
			return nullptr;
		}
		const FDabExprPtr& Default = Model.Variables[*Index].Default;
		if (!Default)
		{
			Sink.Error(TEXT("DAB5006"), Expr.Span,
				FString::Printf(TEXT("`Props.%s` stands for its default here, where nothing can be read at run time, but it has none."), *Name),
				FString::Printf(TEXT("Give it a default: `%s %s = ...;`."), *Model.Variables[*Index].Type, *Name));
			return nullptr;
		}
		return Default.Get();
	}

	void FSemantic::ReportTypeMismatch(const FSemType& Type, const FDabExpr& Expr, const FString& What, const FString& Detail)
	{
		Sink.Error(TEXT("DAB4006"), Expr.Span,
			FString::Printf(TEXT("%s takes %s, not `%s`%s"), *What, *DescribeSemanticType(Type), *PrintValueText(Expr),
				Detail.IsEmpty() ? TEXT(".") : *(TEXT(": ") + Detail + TEXT("."))));
	}

	/** The text of a name or string literal. */
	bool ReadNameOrStringText(const FDabExpr& InExpr, FString& OutText)
	{
		const FDabExpr& Expr = SkipParens(InExpr);
		if (const FDabNameExpr* Name = Expr.As<FDabNameExpr>())
		{
			OutText = Name->Name;
			return true;
		}
		if (const FDabLiteralExpr* Literal = Expr.As<FDabLiteralExpr>(); Literal && Literal->LiteralKind == EDabLiteralKind::String)
		{
			OutText = Literal->Text;
			return true;
		}
		return false;
	}

	FDabExprPtr FSemantic::ConvertProperty(const FProperty& Property, const FDabExpr& Expr, const void* DefaultValue, const FString& What)
	{
		return ConvertTyped(FSemType::FromProperty(Property), Expr, CastField<FStructProperty>(&Property) ? DefaultValue : nullptr, What);
	}

	FDabExprPtr FSemantic::ConvertTyped(const FSemType& Type, const FDabExpr& InExpr, const void* StructDefault, const FString& What)
	{
		const FDabExpr& Expr = SkipParens(InExpr);
		if (IsPlaceholderCall(Expr))
		{
			// The extractor's stand-in for something inexpressible: the model already lists it as unsupported.
			return CloneExpr(Expr);
		}
		if (IsSectionRead(Expr, TEXT("Props")))
		{
			// Where nothing can be read at run time a tuning value stands for its default.
			const FDabExpr* Default = FindPropsDefault(Expr);
			return Default ? ConvertTyped(Type, *Default, StructDefault, What) : nullptr;
		}
		if (IsSectionRead(Expr, TEXT("Assets")))
		{
			Sink.Error(TEXT("DAB4010"), Expr.Span,
				FString::Printf(TEXT("%s is fixed when the Blueprint compiles; `%s` is read from the animation set at run time."), *What, *PrintValueText(Expr)),
				TEXT("Write Path(...) to the asset, or move the value to a node argument that can be bound."));
			return nullptr;
		}

		switch (Type.Kind)
		{
		case ESemTypeKind::Bool:
		{
			const FDabLiteralExpr* Literal = Expr.As<FDabLiteralExpr>();
			if (Literal && Literal->LiteralKind == EDabLiteralKind::Bool)
			{
				return MakeBoolExpr(Literal->bBool);
			}
			ReportTypeMismatch(Type, Expr, What);
			return nullptr;
		}
		case ESemTypeKind::Integer:
		case ESemTypeKind::Float:
		case ESemTypeKind::Double:
		{
			double Value = 0.0;
			if (!ReadNumber(Expr, Value))
			{
				ReportTypeMismatch(Type, Expr, What);
				return nullptr;
			}
			if (Type.Kind == ESemTypeKind::Float)
			{
				return MakeNumberExpr(FloatToShortestDoubleValue(static_cast<float>(Value)));
			}
			if (Type.Kind == ESemTypeKind::Double)
			{
				return MakeNumberExpr(Value);
			}
			if (FMath::IsNaN(Value) || FMath::Abs(Value) >= 9.2e18 || static_cast<double>(static_cast<int64>(Value)) != Value)
			{
				ReportTypeMismatch(Type, Expr, What);
				return nullptr;
			}
			if (static_cast<int64>(Value) < Type.MinInteger || static_cast<int64>(Value) > Type.MaxInteger)
			{
				ReportTypeMismatch(Type, Expr, What, FString::Printf(TEXT("the value must lie in %lld..%lld"), Type.MinInteger, Type.MaxInteger));
				return nullptr;
			}
			return MakeNumberExpr(Value);
		}
		case ESemTypeKind::Enum:
			return Type.Enum ? ConvertEnumValue(*Type.Enum, Expr, What) : ConvertUntyped(Expr);
		case ESemTypeKind::Name:
		{
			FString Text;
			if (!ReadNameOrStringText(Expr, Text))
			{
				ReportTypeMismatch(Type, Expr, What);
				return nullptr;
			}
			return MakeStringExpr(FName(*Text).IsNone() ? FString() : Text);
		}
		case ESemTypeKind::String:
		case ESemTypeKind::Text:
		{
			FString Text;
			if (!ReadNameOrStringText(Expr, Text))
			{
				ReportTypeMismatch(Type, Expr, What);
				return nullptr;
			}
			return MakeStringExpr(Text);
		}
		case ESemTypeKind::GameplayTag:
		{
			FString Text;
			if (!ReadNameOrStringText(Expr, Text))
			{
				ReportTypeMismatch(Type, Expr, What);
				return nullptr;
			}
			if (Text == TEXT("None"))
			{
				Text.Reset();
			}
			if (!Text.IsEmpty() && !UGameplayTagsManager::Get().RequestGameplayTag(FName(*Text), false).IsValid())
			{
				Sink.Warning(TEXT("DAB4003"), Expr.Span, FString::Printf(TEXT("The gameplay tag \"%s\" is not registered."), *Text),
					TEXT("Add it to the project's gameplay tag table (Project Settings > GameplayTags) or fix its spelling."));
			}
			return MakeStringExpr(Text);
		}
		case ESemTypeKind::GameplayTagContainer:
		{
			const FDabListExpr* List = Expr.As<FDabListExpr>();
			if (!List)
			{
				ReportTypeMismatch(Type, Expr, What);
				return nullptr;
			}
			FSemType TagType;
			TagType.Kind = ESemTypeKind::GameplayTag;
			TArray<FDabExprPtr> Items;
			for (const FDabExprPtr& Item : List->Items)
			{
				FDabExprPtr Tag = Item ? ConvertTyped(TagType, *Item, nullptr, What) : nullptr;
				if (!Tag)
				{
					return nullptr;
				}
				Items.Add(MoveTemp(Tag));
			}
			return MakeListExpr(MoveTemp(Items));
		}
		case ESemTypeKind::Struct:
			return Type.Struct ? ConvertStructValue(*Type.Struct, Expr, StructDefault, What) : ConvertUntyped(Expr);
		case ESemTypeKind::Object:
		case ESemTypeKind::Class:
		case ESemTypeKind::SoftObject:
		case ESemTypeKind::SoftClass:
			return ConvertObjectValue(Type, Expr, What);
		case ESemTypeKind::Array:
		case ESemTypeKind::Set:
		{
			const FDabListExpr* List = Expr.As<FDabListExpr>();
			if (!List || !Type.Inner)
			{
				ReportTypeMismatch(Type, Expr, What);
				return nullptr;
			}
			TArray<TPair<FString, FDabExprPtr>> Items;
			for (int32 Index = 0; Index < List->Items.Num(); ++Index)
			{
				const FDabExprPtr& Item = List->Items[Index];
				FDabExprPtr Value = Item ? ConvertTyped(*Type.Inner, *Item, nullptr, FString::Printf(TEXT("%s[%d]"), *What, Index)) : nullptr;
				if (!Value)
				{
					return nullptr;
				}
				FString Key = PrintValueText(*Value);
				Items.Emplace(MoveTemp(Key), MoveTemp(Value));
			}
			if (Type.Kind == ESemTypeKind::Set)
			{
				// A set has no order of its own; the extractor sorts it by text.
				SortSemanticItems(Items);
			}
			TArray<FDabExprPtr> Values;
			for (TPair<FString, FDabExprPtr>& Item : Items)
			{
				Values.Add(MoveTemp(Item.Value));
			}
			return MakeListExpr(MoveTemp(Values));
		}
		case ESemTypeKind::Map:
		{
			TArray<TPair<FString, const FDabExpr*>> Written;
			if (const FDabTupleExpr* Tuple = Expr.As<FDabTupleExpr>())
			{
				for (const FDabArgumentPtr& Item : Tuple->Items)
				{
					if (Item && Item->Value)
					{
						if (!Item->IsNamed())
						{
							ReportTypeMismatch(Type, *Item->Value, What, TEXT("every entry of a map is `\"Key\" = Value`"));
							return nullptr;
						}
						Written.Emplace(Item->Name, Item->Value.Get());
					}
				}
			}
			else if (const FDabBlockExpr* Block = Expr.As<FDabBlockExpr>())
			{
				for (const FDabStmtPtr& Statement : Block->Statements)
				{
					if (const FDabAssignStmt* Assign = Statement ? Statement->As<FDabAssignStmt>() : nullptr; Assign && Assign->Value)
					{
						Written.Emplace(Assign->Name, Assign->Value.Get());
					}
				}
			}
			else if (const FDabListExpr* List = Expr.As<FDabListExpr>(); !List || !List->Items.IsEmpty())
			{
				ReportTypeMismatch(Type, Expr, What);
				return nullptr;
			}
			if (Written.IsEmpty())
			{
				return MakeListExpr(TArray<FDabExprPtr>());
			}

			TArray<TPair<FString, FDabExprPtr>> Items;
			for (const TPair<FString, const FDabExpr*>& Entry : Written)
			{
				FString Key = Entry.Key;
				if (Type.Inner && Type.Inner->Kind == ESemTypeKind::Enum && Type.Inner->Enum)
				{
					const int32 Index = Type.Inner->Enum->GetIndexByNameString(Key);
					if (Index == INDEX_NONE)
					{
						Sink.Error(TEXT("DAB4002"), Entry.Value->Span, FString::Printf(TEXT("%s has no value `%s`."), *Type.Inner->Enum->GetName(), *Key));
						return nullptr;
					}
					Key = Type.Inner->Enum->GetNameStringByValue(Type.Inner->Enum->GetValueByIndex(Index));
				}
				FDabExprPtr Value = Type.Value ? ConvertTyped(*Type.Value, *Entry.Value, nullptr, FString::Printf(TEXT("%s[\"%s\"]"), *What, *Key))
					: ConvertUntyped(*Entry.Value);
				if (!Value)
				{
					return nullptr;
				}
				Items.Emplace(MoveTemp(Key), MoveTemp(Value));
			}
			SortSemanticItems(Items);
			TUniquePtr<FDabTupleExpr> Result = MakeUnique<FDabTupleExpr>();
			for (TPair<FString, FDabExprPtr>& Item : Items)
			{
				Result->Items.Add(MakeArgumentNode(Item.Key, true, MoveTemp(Item.Value)));
			}
			return FDabExprPtr(MoveTemp(Result));
		}
		default:
			return ConvertUntyped(Expr);
		}
	}

	FDabExprPtr FSemantic::ConvertUntyped(const FDabExpr& InExpr)
	{
		const FDabExpr& Expr = SkipParens(InExpr);
		double Number = 0.0;
		if (ReadNumber(Expr, Number))
		{
			return MakeNumberExpr(Number);
		}
		if (IsCallTo(Expr, TEXT("Path")))
		{
			FString ObjectPath;
			FString Error;
			if (TryGetPathCallObjectPath(static_cast<const FDabCallExpr&>(Expr), ObjectPath, Error))
			{
				return MakeObjectPathRef(ObjectPath);
			}
			Sink.Error(TEXT("DAB3006"), Expr.Span, Error);
			return nullptr;
		}
		if (const FDabListExpr* List = Expr.As<FDabListExpr>())
		{
			TArray<FDabExprPtr> Items;
			for (const FDabExprPtr& Item : List->Items)
			{
				FDabExprPtr Value = Item ? ConvertUntyped(*Item) : nullptr;
				if (!Value)
				{
					return nullptr;
				}
				Items.Add(MoveTemp(Value));
			}
			return MakeListExpr(MoveTemp(Items));
		}
		if (const FDabTupleExpr* Tuple = Expr.As<FDabTupleExpr>())
		{
			TUniquePtr<FDabTupleExpr> Result = MakeUnique<FDabTupleExpr>();
			for (const FDabArgumentPtr& Item : Tuple->Items)
			{
				FDabExprPtr Value = Item && Item->Value ? ConvertUntyped(*Item->Value) : nullptr;
				if (!Value)
				{
					return nullptr;
				}
				Result->Items.Add(MakeArgumentNode(Item->Name, Item->bNameIsString, MoveTemp(Value)));
			}
			return FDabExprPtr(MoveTemp(Result));
		}
		if (const FDabBlockExpr* Block = Expr.As<FDabBlockExpr>(); Block && Block->Statements.IsEmpty())
		{
			return FDabExprPtr(MakeUnique<FDabTupleExpr>());
		}
		return Resolve(Expr);
	}

	FDabExprPtr FSemantic::ConvertEnumValue(const UEnum& Enum, const FDabExpr& Expr, const FString& What)
	{
		double Number = 0.0;
		if (ReadNumber(Expr, Number))
		{
			return MakeEnumNameExpr(Enum, static_cast<int64>(Number));
		}
		FString Text;
		if (!ReadNameOrStringText(Expr, Text))
		{
			FSemType Type;
			Type.Kind = ESemTypeKind::Enum;
			Type.Enum = &Enum;
			ReportTypeMismatch(Type, Expr, What);
			return nullptr;
		}
		const int32 Index = Enum.GetIndexByNameString(Text);
		if (Index == INDEX_NONE)
		{
			TArray<FString> Names;
			for (int32 Entry = 0; Entry < Enum.NumEnums() - 1 && Names.Num() < 12; ++Entry)
			{
				Names.Add(Enum.GetNameStringByIndex(Entry));
			}
			Sink.Error(TEXT("DAB4002"), Expr.Span, FString::Printf(TEXT("%s: %s has no value `%s`."), *What, *Enum.GetName(), *Text),
				FString::Printf(TEXT("Its values: %s."), *FString::Join(Names, TEXT(", "))));
			return nullptr;
		}
		return MakeEnumNameExpr(Enum, Enum.GetValueByIndex(Index));
	}

	FDabExprPtr FSemantic::ConvertStructValue(const UScriptStruct& Struct, const FDabExpr& Expr, const void* Default, const FString& What)
	{
		struct FWrittenField
		{
			FString Name;
			const FDabExpr* Value = nullptr;
		};
		TArray<FWrittenField> Written;
		if (const FDabTupleExpr* Tuple = Expr.As<FDabTupleExpr>())
		{
			for (const FDabArgumentPtr& Item : Tuple->Items)
			{
				if (Item && Item->Value)
				{
					Written.Add({Item->IsNamed() ? Item->Name : FString(), Item->Value.Get()});
				}
			}
		}
		else if (const FDabBlockExpr* Block = Expr.As<FDabBlockExpr>())
		{
			// `{}` stands for `()`, which does not parse; `{ X = 1; }` reads like a tuple.
			for (const FDabStmtPtr& Statement : Block->Statements)
			{
				const FDabAssignStmt* Assign = Statement ? Statement->As<FDabAssignStmt>() : nullptr;
				if (Assign && Assign->Value)
				{
					Written.Add({Assign->Name, Assign->Value.Get()});
				}
				else if (Statement && !Statement->Is<FDabRegionStmt>())
				{
					Sink.Error(TEXT("DAB4006"), Statement->Span, FString::Printf(TEXT("%s: a struct's fields are written `Field = Value`."), *What));
				}
			}
		}
		else
		{
			FSemType Type;
			Type.Kind = ESemTypeKind::Struct;
			Type.Struct = &Struct;
			ReportTypeMismatch(Type, Expr, What);
			return nullptr;
		}

		const TArray<const FProperty*> AllFields = GetSemanticStructFields(Struct);
		TArray<const FProperty*> Fields;
		for (const FProperty* Field : AllFields)
		{
			if (!IsSkippedStructField(*Field))
			{
				Fields.Add(Field);
			}
		}

		TUniquePtr<FSemTempStruct> Fallback;
		if (!Default)
		{
			Fallback = MakeUnique<FSemTempStruct>(Struct);
			Default = Fallback->Get();
		}

		TMap<const FProperty*, const FDabExpr*> Values;
		int32 Position = 0;
		for (const FWrittenField& Item : Written)
		{
			const FProperty* Field = nullptr;
			if (Item.Name.IsEmpty())
			{
				if (!Fields.IsValidIndex(Position))
				{
					Sink.Error(TEXT("DAB4007"), Item.Value->Span, FString::Printf(TEXT("%s: %s has only %d fields."), *What, *Struct.GetName(), Fields.Num()));
					continue;
				}
				Field = Fields[Position++];
			}
			else
			{
				const FName FieldName(*Item.Name);
				for (const FProperty* Candidate : AllFields)
				{
					if (Candidate->GetFName() == FieldName)
					{
						Field = Candidate;
						break;
					}
				}
				if (!Field)
				{
					Sink.Error(TEXT("DAB4007"), Item.Value->Span, FString::Printf(TEXT("%s: %s has no field `%s`."), *What, *Struct.GetName(), *Item.Name));
					continue;
				}
				if (IsSkippedStructField(*Field))
				{
					// GUIDs name a struct rather than configure it, and unsaved fields do not survive; the model keeps neither.
					continue;
				}
			}
			if (Values.Contains(Field))
			{
				Sink.Error(TEXT("DAB4011"), Item.Value->Span, FString::Printf(TEXT("%s: the field `%s` is given twice."), *What, *Field->GetName()));
				continue;
			}
			Values.Add(Field, Item.Value);
		}

		TUniquePtr<FDabTupleExpr> Result = MakeUnique<FDabTupleExpr>();
		for (const FProperty* Field : Fields)
		{
			const FDabExpr* const* Value = Values.Find(Field);
			if (!Value)
			{
				continue;
			}
			if (Field->ArrayDim > 1)
			{
				Sink.Error(TEXT("DAB4006"), (*Value)->Span, FString::Printf(TEXT("%s: `%s` is a fixed-size array, which the language cannot write."), *What, *Field->GetName()));
				continue;
			}
			// Nested structs compare with a default-initialised struct, as the extractor's do.
			FDabExprPtr Converted = ConvertProperty(*Field, **Value, nullptr, What + TEXT(".") + Field->GetName());
			if (!Converted)
			{
				continue;
			}
			const FDabExprPtr DefaultExpr = ValueToExpr(*Field, Field->ContainerPtrToValuePtr<void>(Default), nullptr);
			if (DefaultExpr && PrintValueText(*DefaultExpr) == PrintValueText(*Converted))
			{
				continue;
			}
			Result->Items.Add(MakeArgumentNode(Field->GetName(), false, MoveTemp(Converted)));
		}
		return FDabExprPtr(MoveTemp(Result));
	}

	FDabExprPtr FSemantic::ConvertObjectValue(const FSemType& Type, const FDabExpr& Expr, const FString& What)
	{
		const bool bClassValue = Type.Kind == ESemTypeKind::Class || Type.Kind == ESemTypeKind::SoftClass;

		if (IsCallTo(Expr, TEXT("Path")))
		{
			FString ObjectPath;
			FString Error;
			if (!TryGetPathCallObjectPath(static_cast<const FDabCallExpr&>(Expr), ObjectPath, Error))
			{
				Sink.Error(TEXT("DAB3006"), Expr.Span, Error);
				return nullptr;
			}
			return MakeObjectPathRef(ObjectPath);
		}
		if (const FDabLiteralExpr* Literal = Expr.As<FDabLiteralExpr>(); Literal && Literal->LiteralKind == EDabLiteralKind::String)
		{
			return MakeObjectPathRef(Literal->Text);
		}
		const FDabNameExpr* Name = Expr.As<FDabNameExpr>();
		if (!Name)
		{
			ReportTypeMismatch(Type, Expr, What);
			return nullptr;
		}
		if (Name->Name == TEXT("None"))
		{
			return MakeNameExpr(TEXT("None"));
		}
		if (bClassValue)
		{
			if (UClass* Native = FindNativeClass(Name->Name))
			{
				return MakeStringExpr(Native->GetPathName());
			}
		}
		if (Type.Class && Type.Class->IsChildOf(UEnum::StaticClass()))
		{
			if (const UEnum* Enum = FindFirstObject<UEnum>(*Name->Name, EFindFirstObjectOptions::NativeFirst))
			{
				return MakeObjectRef(Enum);
			}
		}

		FString ObjectPath;
		const EAssetLookup Lookup = bClassValue
			? FindAssetByName(Name->Name, nullptr, Type.Class ? Type.Class : UObject::StaticClass(), ObjectPath)
			: FindAssetByName(Name->Name, Type.Class, nullptr, ObjectPath);
		if (Lookup == EAssetLookup::Found)
		{
			return MakeObjectPathRef(ObjectPath);
		}
		if (Lookup == EAssetLookup::Ambiguous)
		{
			Sink.Error(TEXT("DAB3007"), Expr.Span, FString::Printf(TEXT("%s: more than one asset is named `%s`."), *What, *Name->Name),
				TEXT("Write Path(Root, \"Folder/Name\") to say which one."));
			return nullptr;
		}
		Sink.Error(TEXT("DAB3004"), Expr.Span,
			FString::Printf(TEXT("%s: there is no %s named `%s`."), *What, bClassValue ? TEXT("class or Blueprint") : TEXT("asset"), *Name->Name),
			TEXT("Check the spelling, or write Path(Root, \"Folder/Name\")."));
		return nullptr;
	}

	// ------------------------------------------------------------------------------------------
	// Names of assets and classes
	// ------------------------------------------------------------------------------------------

	UClass* FSemantic::FindNativeClass(const FString& Name) const
	{
		if (Name.StartsWith(TEXT("/Script/"), ESearchCase::CaseSensitive))
		{
			UClass* Class = FindObject<UClass>(nullptr, *Name);
			return Class && Class->HasAnyClassFlags(CLASS_Native) ? Class : nullptr;
		}
		UClass* Class = FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst);
		if ((!Class || !Class->HasAnyClassFlags(CLASS_Native)) && Name.Len() > 1 && (Name[0] == TEXT('U') || Name[0] == TEXT('A')))
		{
			Class = FindFirstObject<UClass>(*Name.RightChop(1), EFindFirstObjectOptions::NativeFirst);
		}
		return Class && Class->HasAnyClassFlags(CLASS_Native) ? Class : nullptr;
	}

	/** A class known by path, loaded lazily for the duration of one run. */
	UClass* FindSemanticClassByPath(const FString& ClassPath, TMap<FString, UClass*>& Cache)
	{
		if (UClass* const* Known = Cache.Find(ClassPath))
		{
			return *Known;
		}
		UClass* Class = ClassPath.IsEmpty() ? nullptr : FindObject<UClass>(nullptr, *ClassPath);
		Cache.Add(ClassPath, Class);
		return Class;
	}

	EAssetLookup FSemantic::FindAssetByName(const FString& Name, const UClass* ExpectedClass, const UClass* MetaClass, FString& OutObjectPath)
	{
		if (Options.ResolveDreamAnimAsset)
		{
			const FString Resolved = Options.ResolveDreamAnimAsset(Name);
			if (!Resolved.IsEmpty())
			{
				OutObjectPath = NormalizeAssetObjectPath(Resolved);
				return EAssetLookup::Found;
			}
		}
		if (Name == BlueprintName && !Model.AssetPath.IsEmpty())
		{
			OutObjectPath = Model.AssetPath;
			return EAssetLookup::Found;
		}

		if (!bAssetIndexBuilt)
		{
			bAssetIndexBuilt = true;
			if (IAssetRegistry* Registry = IAssetRegistry::Get())
			{
				if (Registry->IsLoadingAssets())
				{
					Registry->WaitForCompletion();
				}
				const FTopLevelAssetPath RedirectorPath = UObjectRedirector::StaticClass()->GetClassPathName();
				Registry->EnumerateAllAssets([this, &RedirectorPath](const FAssetData& Asset)
				{
					if (Asset.AssetClassPath == RedirectorPath)
					{
						return true;
					}
					FSemAssetEntry Entry;
					Entry.ObjectPath = Asset.GetObjectPathString();
					Entry.ClassPath = Asset.AssetClassPath.ToString();
					FString NativeParent;
					if (Asset.GetTagValue(FBlueprintTags::NativeParentClassPath, NativeParent))
					{
						Entry.NativeParentClassPath = FPackageName::ExportTextPathToObjectPath(NativeParent);
					}
					AssetIndex.FindOrAdd(Asset.AssetName.ToString()).Add(MoveTemp(Entry));
					return true;
				});
			}
		}

		const TArray<FSemAssetEntry>* Entries = AssetIndex.Find(Name);
		if (!Entries)
		{
			return EAssetLookup::NotFound;
		}
		TArray<FString> Matches;
		for (const FSemAssetEntry& Entry : *Entries)
		{
			const UClass* AssetClass = FindSemanticClassByPath(Entry.ClassPath, ClassesByPath);
			if (MetaClass)
			{
				// A class value: a Blueprint whose generated class derives from MetaClass.
				if (!AssetClass || !AssetClass->IsChildOf(UBlueprint::StaticClass()))
				{
					continue;
				}
				const UClass* NativeParent = FindSemanticClassByPath(Entry.NativeParentClassPath, ClassesByPath);
				if (NativeParent && !NativeParent->IsChildOf(MetaClass) && !MetaClass->IsChildOf(NativeParent))
				{
					continue;
				}
			}
			else if (ExpectedClass && (!AssetClass || !AssetClass->IsChildOf(ExpectedClass)))
			{
				continue;
			}
			Matches.AddUnique(Entry.ObjectPath);
		}
		if (Matches.Num() == 1)
		{
			OutObjectPath = Matches[0];
			return EAssetLookup::Found;
		}
		return Matches.IsEmpty() ? EAssetLookup::NotFound : EAssetLookup::Ambiguous;
	}

	// ------------------------------------------------------------------------------------------
	// Paths and calls
	// ------------------------------------------------------------------------------------------

	/** The struct or class a property's value lands in, for the next step of a path. */
	const UStruct* GetSemanticPropertyScope(const FProperty* Property)
	{
		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property))
		{
			return Object->PropertyClass;
		}
		if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
		{
			return Struct->Struct;
		}
		if (const FInterfaceProperty* Interface = CastField<FInterfaceProperty>(Property))
		{
			return Interface->InterfaceClass;
		}
		return nullptr;
	}

	bool FSemantic::TypePath(const FDabExpr& InExpr, const UStruct*& OutScope, const FProperty*& OutProperty) const
	{
		const FDabExpr& Expr = SkipParens(InExpr);
		OutScope = nullptr;
		OutProperty = nullptr;
		if (const FDabNameExpr* Name = Expr.As<FDabNameExpr>())
		{
			if (Name->Name == TEXT("Self"))
			{
				OutScope = SelfClass;
				return SelfClass != nullptr;
			}
			if (const FDabExprPtr* Alias = Aliases.Find(Name->Name))
			{
				return TypePath(**Alias, OutScope, OutProperty);
			}
			return false;
		}
		if (const FDabMemberExpr* Member = Expr.As<FDabMemberExpr>())
		{
			const UStruct* Scope = nullptr;
			const FProperty* Ignored = nullptr;
			if (!Member->Object || !TypePath(*Member->Object, Scope, Ignored) || !Scope)
			{
				return false;
			}
			OutProperty = FindFProperty<FProperty>(Scope, FName(*Member->Member));
			OutScope = GetSemanticPropertyScope(OutProperty);
			return OutProperty != nullptr;
		}
		if (const FDabCallExpr* Call = Expr.As<FDabCallExpr>())
		{
			const FDabMemberExpr* Callee = Call->Callee ? SkipParens(*Call->Callee).As<FDabMemberExpr>() : nullptr;
			const UStruct* Scope = nullptr;
			const FProperty* Ignored = nullptr;
			if (!Callee || !Call->Args.IsEmpty() || !Callee->Object || !TypePath(*Callee->Object, Scope, Ignored))
			{
				return false;
			}
			const UClass* Class = Cast<UClass>(Scope);
			const UFunction* Function = Class ? Class->FindFunctionByName(FName(*Callee->Member)) : nullptr;
			OutProperty = Function ? Function->GetReturnProperty() : nullptr;
			OutScope = GetSemanticPropertyScope(OutProperty);
			return OutProperty != nullptr;
		}
		return false;
	}

	bool FSemantic::ResolveCallee(const FDabExpr& Callee, FDabExprPtr& OutTarget, FString& OutFunction, const UFunction*& OutUFunction, const FString& What)
	{
		OutUFunction = nullptr;
		const FDabMemberExpr* Member = SkipParens(Callee).As<FDabMemberExpr>();
		if (!Member || !Member->Object)
		{
			Sink.Error(TEXT("DAB5001"), Callee.Span,
				FString::Printf(TEXT("%s calls a function on the instance: `Target.Function(...)`; `%s` is not one."), *What, *PrintValueText(Callee)),
				TEXT("Write `Self.Refresh()` or, through an alias, `Main.PlayAnimation(...)`."));
			return false;
		}
		OutTarget = Resolve(*Member->Object);
		if (!OutTarget)
		{
			return false;
		}
		const FDabNameExpr* Root = GetPathRoot(*OutTarget);
		if (!Root || Root->Name != TEXT("Self"))
		{
			Sink.Error(TEXT("DAB5001"), Member->Object->Span,
				FString::Printf(TEXT("%s: a call's target is Self or a path from it; `%s` is not."), *What, *PrintValueText(*Member->Object)),
				TEXT("Declare the alias in Aliases = { Main = Self.GetParent(); } or start the path at Self."));
			return false;
		}
		OutFunction = Member->Member;

		const UStruct* Scope = nullptr;
		const FProperty* Ignored = nullptr;
		TypePath(*OutTarget, Scope, Ignored);
		if (const UClass* TargetClass = Cast<UClass>(Scope))
		{
			OutUFunction = TargetClass->FindFunctionByName(FName(*OutFunction));
			if (!OutUFunction)
			{
				Sink.Warning(TEXT("DAB5004"), Member->MemberSpan.IsEmpty() ? Member->Span : Member->MemberSpan,
					FString::Printf(TEXT("%s: %s has no function `%s`."), *What, *TargetClass->GetName(), *OutFunction));
			}
		}
		return true;
	}

	void FSemantic::ConvertCallArguments(const UFunction* Function, TConstArrayView<const FDabArgument*> Args, TArray<FDabModelArgument>& Out, const FString& What)
	{
		TArray<const FProperty*> Parameters;
		if (Function)
		{
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				const bool bOutput = It->HasAnyPropertyFlags(CPF_ReturnParm) || (It->HasAnyPropertyFlags(CPF_OutParm) && !It->HasAnyPropertyFlags(CPF_ReferenceParm));
				if (!bOutput)
				{
					Parameters.Add(*It);
				}
			}
		}

		int32 Position = 0;
		for (const FDabArgument* Arg : Args)
		{
			if (!Arg || !Arg->Value)
			{
				continue;
			}
			FString Name = Arg->IsNamed() ? Arg->Name : FString();
			const FProperty* Parameter = nullptr;
			if (Name.IsEmpty())
			{
				// The extractor names every argument after its pin, which is the parameter's name.
				if (Parameters.IsValidIndex(Position))
				{
					Parameter = Parameters[Position];
					Name = Parameter->GetName();
				}
				++Position;
			}
			else if (Function)
			{
				const FName ParameterName(*Name);
				for (const FProperty* Candidate : Parameters)
				{
					if (Candidate->GetFName() == ParameterName)
					{
						Parameter = Candidate;
						break;
					}
				}
				if (!Parameter)
				{
					Sink.Warning(TEXT("DAB5004"), Arg->NameSpan, FString::Printf(TEXT("%s: %s has no parameter `%s`."), *What, *Function->GetName(), *Name));
				}
			}

			const FDabExpr& Value = SkipParens(*Arg->Value);
			const FString ArgumentWhat = FString::Printf(TEXT("%s, argument %s"), *What, Name.IsEmpty() ? TEXT("?") : *Name);
			FDabExprPtr Converted;
			if (IsPlaceholderCall(Value))
			{
				Converted = CloneExpr(Value);
			}
			else if (IsSectionRead(Value, TEXT("Props")) || IsSectionRead(Value, TEXT("Assets")))
			{
				Converted = Resolve(Value);
			}
			else if (IsConstant(Value))
			{
				Converted = Parameter ? ConvertProperty(*Parameter, Value, nullptr, ArgumentWhat) : ConvertUntyped(Value);
			}
			else
			{
				Sink.Error(TEXT("DAB5002"), Value.Span,
					FString::Printf(TEXT("%s: an argument is a constant, Props.X or Assets.X; `%s` is computed."), *ArgumentWhat, *PrintValueText(Value)),
					TEXT("Give the C++ function no such parameter and let it read the value itself."));
				continue;
			}
			if (Converted)
			{
				FDabModelArgument Argument;
				Argument.Name = Name;
				Argument.Value = MoveTemp(Converted);
				Out.Add(MoveTemp(Argument));
			}
		}
	}

	bool FSemantic::ConvertCall(const FDabExpr& InExpr, FDabModelCall& OutCall, const FString& What)
	{
		const FDabExpr& Expr = SkipParens(InExpr);
		const FDabExpr* Callee = &Expr;
		TArray<const FDabArgument*> Args;
		if (const FDabCallExpr* Call = Expr.As<FDabCallExpr>())
		{
			Callee = Call->Callee.Get();
			for (const FDabArgumentPtr& Arg : Call->Args)
			{
				Args.Add(Arg.Get());
			}
		}
		if (!Callee)
		{
			return false;
		}
		const UFunction* Function = nullptr;
		FDabExprPtr Target;
		FString FunctionName;
		if (!ResolveCallee(*Callee, Target, FunctionName, Function, What))
		{
			return false;
		}
		OutCall.Target = MoveTemp(Target);
		OutCall.Function = FunctionName;
		OutCall.Args.Reset();
		ConvertCallArguments(Function, Args, OutCall.Args, What);
		return true;
	}
}
