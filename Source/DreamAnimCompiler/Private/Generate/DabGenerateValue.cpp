// Copyright DreamMoon. All Rights Reserved.
//
// Values: model expressions -> property values and pin defaults. The model writes every constant the
// same way (numbers through FormatDabNumber, assets as Path(Root, "Rel"), enums as bare names, tags as
// strings, structs as named tuples of their non-default fields, arrays as lists), so one converter
// serves node properties, transition options, class settings, variables and pin defaults.

#include "Generate/DabGenerateInternal.h"

#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameplayTagContainer.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Lang/DabPrinter.h"
#include "Misc/OutputDeviceNull.h"
#include "Misc/SecureHash.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Compiler::GeneratePrivate
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	bool FDabGenPath::AllCalls() const
	{
		for (const bool bCall : SegmentIsCall)
		{
			if (!bCall)
			{
				return false;
			}
		}
		return !SegmentIsCall.IsEmpty();
	}

	const FDabExpr& DabGenUnwrap(const FDabExpr& Expr)
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

	FString DabGenPrint(const FDabExpr& Expr)
	{
		return PrintDabExpr(Expr);
	}

	FString DabGenPrintCall(const FDabModelCall& Call)
	{
		FString Text = Call.Target ? PrintDabExpr(*Call.Target) : FString(TEXT("Self"));
		Text += TEXT(".");
		Text += Call.Function;
		Text += TEXT("(");
		for (int32 Index = 0; Index < Call.Args.Num(); ++Index)
		{
			const FDabModelArgument& Argument = Call.Args[Index];
			if (Index > 0)
			{
				Text += TEXT(", ");
			}
			if (!Argument.Name.IsEmpty())
			{
				Text += Argument.Name;
				Text += TEXT(" = ");
			}
			Text += Argument.Value ? PrintDabExpr(*Argument.Value) : FString(TEXT("?"));
		}
		Text += TEXT(")");
		return Text;
	}

	FString DabGenShortHash(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		FMD5 Md5;
		Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		uint8 Digest[16];
		Md5.Final(Digest);
		return FString::Printf(TEXT("%02X%02X%02X%02X"), Digest[0], Digest[1], Digest[2], Digest[3]);
	}

	bool DabGenTryGetNumber(const FDabExpr& InExpr, double& OutNumber)
	{
		const FDabExpr& Expr = DabGenUnwrap(InExpr);
		if (const FDabLiteralExpr* Literal = Expr.As<FDabLiteralExpr>())
		{
			if (Literal->LiteralKind != EDabLiteralKind::Number)
			{
				return false;
			}
			// The lexeme is authoritative: it is what the printer writes back.
			OutNumber = Literal->Text.IsEmpty() ? Literal->Number : FCString::Atod(*Literal->Text);
			return true;
		}
		if (const FDabUnaryExpr* Unary = Expr.As<FDabUnaryExpr>())
		{
			double Inner = 0.0;
			if (Unary->Op == EDabUnaryOp::Negate && Unary->Operand && DabGenTryGetNumber(*Unary->Operand, Inner))
			{
				OutNumber = -Inner;
				return true;
			}
		}
		return false;
	}

	bool DabGenTryGetBool(const FDabExpr& InExpr, bool& OutValue)
	{
		const FDabExpr& Expr = DabGenUnwrap(InExpr);
		if (const FDabLiteralExpr* Literal = Expr.As<FDabLiteralExpr>())
		{
			if (Literal->LiteralKind == EDabLiteralKind::Bool)
			{
				OutValue = Literal->bBool || Literal->Text == TEXT("true");
				return true;
			}
			return false;
		}
		if (const FDabNameExpr* Name = Expr.As<FDabNameExpr>())
		{
			if (Name->Name == TEXT("true") || Name->Name == TEXT("false"))
			{
				OutValue = Name->Name == TEXT("true");
				return true;
			}
		}
		return false;
	}

	bool DabGenTryGetText(const FDabExpr& InExpr, FString& OutText)
	{
		const FDabExpr& Expr = DabGenUnwrap(InExpr);
		if (const FDabLiteralExpr* Literal = Expr.As<FDabLiteralExpr>())
		{
			if (Literal->LiteralKind == EDabLiteralKind::String)
			{
				OutText = Literal->Text;
				return true;
			}
			return false;
		}
		if (const FDabNameExpr* Name = Expr.As<FDabNameExpr>())
		{
			OutText = Name->Name;
			return true;
		}
		return false;
	}

	static const FDabExpr* DabGenFindUnsupportedIn(const FDabNode& Node)
	{
		if (const FDabCallExpr* Call = Node.As<FDabCallExpr>())
		{
			const FDabNameExpr* Callee = Call->Callee ? DabGenUnwrap(*Call->Callee).As<FDabNameExpr>() : nullptr;
			if (Callee && Callee->Name == TEXT("Unsupported"))
			{
				return Call;
			}
		}
		const FDabExpr* Found = nullptr;
		ForEachChild(Node, [&Found](const FDabNode& Child)
		{
			if (!Found)
			{
				Found = DabGenFindUnsupportedIn(Child);
			}
		});
		return Found;
	}

	const FDabExpr* DabGenFindUnsupported(const FDabExpr& Expr)
	{
		return DabGenFindUnsupportedIn(Expr);
	}

	bool DabGenIsSelf(const FDabExpr* Expr)
	{
		if (!Expr)
		{
			return true;
		}
		const FDabNameExpr* Name = DabGenUnwrap(*Expr).As<FDabNameExpr>();
		return Name && Name->Name == TEXT("Self");
	}

	bool DabGenIsConstant(const FDabExpr& InExpr)
	{
		const FDabExpr& Expr = DabGenUnwrap(InExpr);
		switch (Expr.Kind)
		{
		case EDabNodeKind::LiteralExpr:
			return true;
		case EDabNodeKind::NameExpr:
			{
				const FString& Name = Expr.As<FDabNameExpr>()->Name;
				return Name != TEXT("Self") && Name != TEXT("Props") && Name != TEXT("Assets");
			}
		case EDabNodeKind::UnaryExpr:
			{
				double Number = 0.0;
				return DabGenTryGetNumber(Expr, Number);
			}
		case EDabNodeKind::CallExpr:
			{
				FString Path;
				return DabGenTryGetAssetPath(Expr, Path);
			}
		case EDabNodeKind::TupleExpr:
			for (const FDabArgumentPtr& Item : Expr.As<FDabTupleExpr>()->Items)
			{
				if (!Item || !Item->Value || !DabGenIsConstant(*Item->Value))
				{
					return false;
				}
			}
			return true;
		case EDabNodeKind::ListExpr:
			for (const FDabExprPtr& Item : Expr.As<FDabListExpr>()->Items)
			{
				if (!Item || !DabGenIsConstant(*Item))
				{
					return false;
				}
			}
			return true;
		case EDabNodeKind::BlockExpr:
			for (const FDabStmtPtr& Statement : Expr.As<FDabBlockExpr>()->Statements)
			{
				const FDabAssignStmt* Assign = Statement ? Statement->As<FDabAssignStmt>() : nullptr;
				if (!Assign || !Assign->Value || !DabGenIsConstant(*Assign->Value))
				{
					return false;
				}
			}
			return true;
		default:
			return false;
		}
	}

	bool DabGenTryGetAssetPath(const FDabExpr& InExpr, FString& OutObjectPath)
	{
		const FDabCallExpr* Call = DabGenUnwrap(InExpr).As<FDabCallExpr>();
		if (!Call || !Call->Callee || Call->Args.Num() != 2 || !Call->Args[0] || !Call->Args[1] || !Call->Args[0]->Value || !Call->Args[1]->Value)
		{
			return false;
		}
		const FDabNameExpr* Callee = DabGenUnwrap(*Call->Callee).As<FDabNameExpr>();
		if (!Callee || Callee->Name != TEXT("Path"))
		{
			return false;
		}

		// Root: Game, Engine, Plugin.<Mount> (a name, a member chain, or the same written as a string).
		const FDabExpr& Root = DabGenUnwrap(*Call->Args[0]->Value);
		FString RootText;
		TArray<FString> RootSegments;
		if (const FDabLiteralExpr* RootLiteral = Root.As<FDabLiteralExpr>())
		{
			if (RootLiteral->LiteralKind == EDabLiteralKind::String)
			{
				RootText = RootLiteral->Text;
			}
		}
		else if (TryGetDabPath(Root, RootSegments))
		{
			RootText = FString::Join(RootSegments, TEXT("."));
		}

		FString Mount;
		if (RootText == TEXT("Game"))
		{
			Mount = TEXT("/Game");
		}
		else if (RootText == TEXT("Engine"))
		{
			Mount = TEXT("/Engine");
		}
		else if (RootText.StartsWith(TEXT("Plugin.")))
		{
			Mount = TEXT("/") + RootText.RightChop(7);
		}
		else
		{
			return false;
		}

		const FDabLiteralExpr* RelLiteral = DabGenUnwrap(*Call->Args[1]->Value).As<FDabLiteralExpr>();
		if (!RelLiteral || RelLiteral->LiteralKind != EDabLiteralKind::String)
		{
			return false;
		}
		FString Rel = RelLiteral->Text.TrimStartAndEnd();
		while (Rel.StartsWith(TEXT("/")))
		{
			Rel.RightChopInline(1);
		}

		// A subobject (a blend profile inside a skeleton) is `Package:Sub`; the asset's `.Leaf` goes before the `:`.
		FString SubObject;
		int32 ColonIndex = INDEX_NONE;
		if (Rel.FindChar(TEXT(':'), ColonIndex))
		{
			SubObject = Rel.Mid(ColonIndex);
			Rel.LeftInline(ColonIndex);
		}

		FString Path = Mount + TEXT("/") + Rel;
		int32 SlashIndex = INDEX_NONE;
		Path.FindLastChar(TEXT('/'), SlashIndex);
		const FString Leaf = Path.Mid(SlashIndex + 1);
		if (!Leaf.Contains(TEXT(".")))
		{
			Path += TEXT(".") + Leaf;
		}
		OutObjectPath = Path + SubObject;
		return true;
	}

	UObject* DabGenLoadObject(const FString& ObjectPath)
	{
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Found = StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath))
		{
			return Found;
		}
		if (ObjectPath.StartsWith(TEXT("/Script/")))
		{
			// Native objects are never loaded from disk.
			return nullptr;
		}
		return StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath, FStringView(), LOAD_NoWarn | LOAD_Quiet);
	}

	UClass* DabGenLoadClass(const FString& ObjectPath)
	{
		UObject* Object = DabGenLoadObject(ObjectPath);
		if (UClass* Class = Cast<UClass>(Object))
		{
			return Class;
		}
		if (const UBlueprint* Blueprint = Cast<UBlueprint>(Object))
		{
			return Blueprint->GeneratedClass.Get();
		}
		if (!Object && !ObjectPath.EndsWith(TEXT("_C")))
		{
			return Cast<UClass>(DabGenLoadObject(ObjectPath + TEXT("_C")));
		}
		return nullptr;
	}

	UObject* DabGenFindType(const FString& InName, UClass* TypeClass)
	{
		const FString Name = InName.TrimStartAndEnd();
		if (Name.IsEmpty())
		{
			return nullptr;
		}
		if (Name.Contains(TEXT("/")))
		{
			UObject* Object = DabGenLoadObject(Name);
			if (const UBlueprint* Blueprint = Cast<UBlueprint>(Object); Blueprint && TypeClass == UClass::StaticClass())
			{
				return Blueprint->GeneratedClass.Get();
			}
			return Object && Object->IsA(TypeClass) ? Object : nullptr;
		}
		if (UObject* Found = StaticFindFirstObject(TypeClass, Name, EFindFirstObjectOptions::NativeFirst))
		{
			return Found;
		}
		// Reflection drops the C++ prefix letter: FVector is Vector, UAnimSequence is AnimSequence.
		if (Name.Len() > 1 && FChar::IsUpper(Name[0]) && FChar::IsUpper(Name[1]))
		{
			const TCHAR Prefix = Name[0];
			const bool bStructPrefix = TypeClass == UScriptStruct::StaticClass() && Prefix == TEXT('F');
			const bool bClassPrefix = TypeClass == UClass::StaticClass() && (Prefix == TEXT('U') || Prefix == TEXT('A'));
			if (bStructPrefix || bClassPrefix)
			{
				return StaticFindFirstObject(TypeClass, Name.RightChop(1), EFindFirstObjectOptions::NativeFirst);
			}
		}
		return nullptr;
	}

	bool DabGenTryGetPath(const FDabExpr& InExpr, FDabGenPath& OutPath)
	{
		TArray<FString> Reversed;
		TArray<bool> ReversedCalls;
		const FDabExpr* Current = &DabGenUnwrap(InExpr);
		bool bPendingCall = false;
		while (true)
		{
			if (const FDabCallExpr* Call = Current->As<FDabCallExpr>())
			{
				if (!Call->Args.IsEmpty() || !Call->Callee || bPendingCall)
				{
					return false;
				}
				bPendingCall = true;
				Current = &DabGenUnwrap(*Call->Callee);
				continue;
			}
			if (const FDabMemberExpr* Member = Current->As<FDabMemberExpr>())
			{
				if (!Member->Object)
				{
					return false;
				}
				Reversed.Add(Member->Member);
				ReversedCalls.Add(bPendingCall);
				bPendingCall = false;
				Current = &DabGenUnwrap(*Member->Object);
				continue;
			}
			if (const FDabNameExpr* Name = Current->As<FDabNameExpr>())
			{
				if (bPendingCall)
				{
					// `Foo()`: a builtin or a mistake, not a path.
					return false;
				}
				if (Name->Name == TEXT("Self"))
				{
					OutPath.Root = EDabGenPathRoot::Self;
				}
				else if (Name->Name == TEXT("Props"))
				{
					OutPath.Root = EDabGenPathRoot::Props;
				}
				else if (Name->Name == TEXT("Assets"))
				{
					OutPath.Root = EDabGenPathRoot::Assets;
				}
				else
				{
					return false;
				}
				break;
			}
			return false;
		}

		OutPath.Segments.Reset();
		OutPath.SegmentIsCall.Reset();
		for (int32 Index = Reversed.Num() - 1; Index >= 0; --Index)
		{
			OutPath.Segments.Add(Reversed[Index]);
			OutPath.SegmentIsCall.Add(ReversedCalls[Index]);
		}
		return true;
	}

	FDabGenTempValue::FDabGenTempValue(const FProperty* InProperty)
		: Property(InProperty)
	{
		if (Property)
		{
			Memory = FMemory::Malloc(FMath::Max(Property->GetSize(), 1), Property->GetMinAlignment());
			Property->InitializeValue(Memory);
		}
	}

	FDabGenTempValue::~FDabGenTempValue()
	{
		if (Property && Memory)
		{
			Property->DestroyValue(Memory);
			FMemory::Free(Memory);
		}
	}

	static bool DabGenResolveEnumValue(const UEnum* Enum, const FDabExpr& Expr, int64& OutValue, FString& OutError)
	{
		double Number = 0.0;
		if (DabGenTryGetNumber(Expr, Number))
		{
			OutValue = static_cast<int64>(Number);
			return true;
		}
		FString Text;
		if (!DabGenTryGetText(Expr, Text))
		{
			OutError = FString::Printf(TEXT("expects a value of %s"), *Enum->GetName());
			return false;
		}
		int32 Index = Enum->GetIndexByNameString(Text);
		if (Index == INDEX_NONE)
		{
			// User-defined enums name their values NewEnumeratorN; accept the display name too.
			for (int32 Candidate = 0; Candidate < Enum->NumEnums(); ++Candidate)
			{
				if (Enum->GetDisplayNameTextByIndex(Candidate).ToString() == Text || Enum->GetNameStringByIndex(Candidate) == Text)
				{
					Index = Candidate;
					break;
				}
			}
		}
		if (Index == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("%s is not a value of %s"), *Text, *Enum->GetName());
			return false;
		}
		OutValue = Enum->GetValueByIndex(Index);
		return true;
	}

	static bool DabGenIsNoneName(const FDabExpr& Expr)
	{
		const FDabNameExpr* Name = DabGenUnwrap(Expr).As<FDabNameExpr>();
		return Name && (Name->Name == TEXT("None") || Name->Name == TEXT("nullptr") || Name->Name == TEXT("null"));
	}

	static bool DabGenImportObject(const FObjectPropertyBase* ObjectProperty, void* Value, const FDabExpr& Expr, FString& OutError)
	{
		if (DabGenIsNoneName(Expr))
		{
			ObjectProperty->SetObjectPropertyValue(Value, nullptr);
			return true;
		}

		const FClassProperty* ClassProperty = CastField<FClassProperty>(ObjectProperty);
		const FSoftClassProperty* SoftClassProperty = CastField<FSoftClassProperty>(ObjectProperty);
		const FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(ObjectProperty);
		const bool bWantsClass = ClassProperty || SoftClassProperty;
		UClass* MetaClass = ClassProperty ? ClassProperty->MetaClass.Get() : (SoftClassProperty ? SoftClassProperty->MetaClass.Get() : nullptr);

		FString ObjectPath;
		UObject* Object = nullptr;
		const FDabExpr& Unwrapped = DabGenUnwrap(Expr);
		if (!DabGenTryGetAssetPath(Unwrapped, ObjectPath))
		{
			if (const FDabLiteralExpr* Literal = Unwrapped.As<FDabLiteralExpr>(); Literal && Literal->LiteralKind == EDabLiteralKind::String)
			{
				ObjectPath = Literal->Text;
			}
			else if (const FDabNameExpr* Name = Unwrapped.As<FDabNameExpr>())
			{
				// A native type by name: `EAlsHipsDirection`, `AlsAnimationInstance`.
				Object = DabGenFindType(Name->Name, bWantsClass ? UClass::StaticClass() : ObjectProperty->PropertyClass.Get());
				if (!Object)
				{
					OutError = FString::Printf(TEXT("%s names no %s"), *Name->Name, bWantsClass ? TEXT("class") : *ObjectProperty->PropertyClass->GetName());
					return false;
				}
			}
			else
			{
				OutError = TEXT("expects Path(...) or None");
				return false;
			}
		}

		if (!ObjectPath.IsEmpty())
		{
			if (SoftObjectProperty && !SoftClassProperty)
			{
				// A soft reference keeps the path; nothing has to be loaded for it.
				*static_cast<FSoftObjectPtr*>(Value) = FSoftObjectPtr(FSoftObjectPath(ObjectPath));
				return true;
			}
			Object = bWantsClass ? static_cast<UObject*>(DabGenLoadClass(ObjectPath)) : DabGenLoadObject(ObjectPath);
			if (!Object)
			{
				OutError = FString::Printf(TEXT("%s cannot be loaded"), *ObjectPath);
				return false;
			}
		}

		if (bWantsClass)
		{
			UClass* Class = Cast<UClass>(Object);
			if (const UBlueprint* Blueprint = Cast<UBlueprint>(Object))
			{
				Class = Blueprint->GeneratedClass.Get();
			}
			if (!Class)
			{
				OutError = FString::Printf(TEXT("%s is not a class"), *Object->GetPathName());
				return false;
			}
			if (MetaClass && !Class->IsChildOf(MetaClass))
			{
				OutError = FString::Printf(TEXT("%s is not a %s"), *Class->GetName(), *MetaClass->GetName());
				return false;
			}
			ObjectProperty->SetObjectPropertyValue(Value, Class);
			return true;
		}

		if (!Object->IsA(ObjectProperty->PropertyClass))
		{
			OutError = FString::Printf(TEXT("%s is a %s, not a %s"), *Object->GetPathName(), *Object->GetClass()->GetName(), *ObjectProperty->PropertyClass->GetName());
			return false;
		}
		ObjectProperty->SetObjectPropertyValue(Value, Object);
		return true;
	}

	static bool DabGenImportTag(const FDabExpr& Expr, FGameplayTag& OutTag, FString& OutError)
	{
		FString Text;
		if (!DabGenTryGetText(Expr, Text))
		{
			OutError = TEXT("expects a gameplay tag as a string");
			return false;
		}
		if (Text.IsEmpty() || Text == TEXT("None"))
		{
			OutTag = FGameplayTag();
			return true;
		}
		OutTag = FGameplayTag::RequestGameplayTag(FName(*Text), false);
		if (!OutTag.IsValid())
		{
			OutError = FString::Printf(TEXT("%s is not a gameplay tag"), *Text);
			return false;
		}
		return true;
	}

	bool DabGenImportStruct(FDabGenContext& Ctx, const UScriptStruct* Struct, void* Data, const FDabExpr& InExpr, UObject* Owner, FString& OutError)
	{
		const FDabExpr& Expr = DabGenUnwrap(InExpr);

		if (Struct == FGameplayTag::StaticStruct())
		{
			return DabGenImportTag(Expr, *static_cast<FGameplayTag*>(Data), OutError);
		}
		if (Struct == FGameplayTagContainer::StaticStruct())
		{
			FGameplayTagContainer& Container = *static_cast<FGameplayTagContainer*>(Data);
			Container.Reset();
			TArray<const FDabExpr*> Items;
			if (const FDabListExpr* List = Expr.As<FDabListExpr>())
			{
				for (const FDabExprPtr& Item : List->Items)
				{
					if (Item)
					{
						Items.Add(Item.Get());
					}
				}
			}
			else
			{
				Items.Add(&Expr);
			}
			for (const FDabExpr* Item : Items)
			{
				FGameplayTag Tag;
				if (!DabGenImportTag(*Item, Tag, OutError))
				{
					return false;
				}
				if (Tag.IsValid())
				{
					Container.AddTag(Tag);
				}
			}
			return true;
		}

		if (const FDabTupleExpr* Tuple = Expr.As<FDabTupleExpr>())
		{
			// Only the fields the tuple names change; the rest keep the value they have.
			TArray<const FProperty*> Fields;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				Fields.Add(*It);
			}
			int32 Positional = 0;
			for (const FDabArgumentPtr& Item : Tuple->Items)
			{
				if (!Item || !Item->Value)
				{
					continue;
				}
				const FProperty* Field = Item->IsNamed()
					? FindFProperty<FProperty>(Struct, FName(*Item->Name))
					: (Fields.IsValidIndex(Positional) ? Fields[Positional++] : nullptr);
				if (!Field)
				{
					OutError = Item->IsNamed()
						? FString::Printf(TEXT("%s has no field %s"), *Struct->GetName(), *Item->Name)
						: FString::Printf(TEXT("%s has fewer fields than the tuple has items"), *Struct->GetName());
					return false;
				}
				// A tuple inside a tuple is the whole struct: what it leaves out is that struct's own default, as
				// the decompiler and the checker compare it. Only the outermost tuple keeps what the value had.
				void* FieldData = Field->ContainerPtrToValuePtr<void>(Data);
				if (CastField<FStructProperty>(Field) && DabGenUnwrap(*Item->Value).Is<FDabTupleExpr>())
				{
					Field->DestroyValue(FieldData);
					Field->InitializeValue(FieldData);
				}
				FString FieldError;
				if (!DabGenImportValue(Ctx, Field, FieldData, *Item->Value, Owner, FieldError))
				{
					OutError = FString::Printf(TEXT("%s: %s"), *Field->GetName(), *FieldError);
					return false;
				}
			}
			return true;
		}

		if (const FDabLiteralExpr* Literal = Expr.As<FDabLiteralExpr>(); Literal && Literal->LiteralKind == EDabLiteralKind::String)
		{
			// The engine's own text form, for structs a tuple cannot spell.
			FOutputDeviceNull Errors;
			if (Struct->ImportText(*Literal->Text, Data, Owner, PPF_None, &Errors, Struct->GetName()) != nullptr)
			{
				return true;
			}
			OutError = FString::Printf(TEXT("\"%s\" is not a %s"), *Literal->Text, *Struct->GetName());
			return false;
		}

		OutError = FString::Printf(TEXT("expects a tuple of %s fields"), *Struct->GetName());
		return false;
	}

	bool DabGenImportValue(FDabGenContext& Ctx, const FProperty* Property, void* Value, const FDabExpr& InExpr, UObject* Owner, FString& OutError,
		const UEnum* EnumHint)
	{
		const FDabExpr& Expr = DabGenUnwrap(InExpr);
		double Number = 0.0;
		const bool bNumber = DabGenTryGetNumber(Expr, Number);

		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			bool bValue = false;
			if (DabGenTryGetBool(Expr, bValue))
			{
				BoolProperty->SetPropertyValue(Value, bValue);
				return true;
			}
			if (bNumber)
			{
				BoolProperty->SetPropertyValue(Value, Number != 0.0);
				return true;
			}
			OutError = TEXT("expects true or false");
			return false;
		}

		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			int64 EnumValue = 0;
			if (!EnumProperty->GetEnum() || !DabGenResolveEnumValue(EnumProperty->GetEnum(), Expr, EnumValue, OutError))
			{
				if (OutError.IsEmpty())
				{
					OutError = TEXT("the property has no enum");
				}
				return false;
			}
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(Value, EnumValue);
			return true;
		}

		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			const UEnum* Enum = ByteProperty->Enum ? ByteProperty->Enum.Get() : EnumHint;
			if (Enum && !bNumber)
			{
				int64 EnumValue = 0;
				if (!DabGenResolveEnumValue(Enum, Expr, EnumValue, OutError))
				{
					return false;
				}
				ByteProperty->SetIntPropertyValue(Value, EnumValue);
				return true;
			}
		}

		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			if (!bNumber)
			{
				OutError = TEXT("expects a number");
				return false;
			}
			if (NumericProperty->IsFloatingPoint())
			{
				NumericProperty->SetFloatingPointPropertyValue(Value, Number);
			}
			else
			{
				int64 Integer = 0;
				if (!TryGetDabInteger(Expr, Integer) || !NumericProperty->CanHoldValue(Integer))
				{
					OutError = TEXT("expects an integer within the property's range");
					return false;
				}
				NumericProperty->SetIntPropertyValue(Value, Integer);
			}
			return true;
		}

		FString Text;
		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			if (!DabGenTryGetText(Expr, Text))
			{
				OutError = TEXT("expects a name");
				return false;
			}
			NameProperty->SetPropertyValue(Value, Text == TEXT("None") ? FName() : FName(*Text));
			return true;
		}
		if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			if (!DabGenTryGetText(Expr, Text))
			{
				OutError = TEXT("expects a string");
				return false;
			}
			StrProperty->SetPropertyValue(Value, Text);
			return true;
		}
		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			if (!DabGenTryGetText(Expr, Text))
			{
				OutError = TEXT("expects a string");
				return false;
			}
			TextProperty->SetPropertyValue(Value, FText::FromString(Text));
			return true;
		}

		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			return DabGenImportObject(ObjectProperty, Value, Expr, OutError);
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			return DabGenImportStruct(Ctx, StructProperty->Struct, Value, Expr, Owner, OutError);
		}

		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			const FDabListExpr* List = Expr.As<FDabListExpr>();
			if (!List)
			{
				OutError = TEXT("expects a list");
				return false;
			}
			// The list is the whole array; elements start from their defaults.
			FScriptArrayHelper Helper(ArrayProperty, Value);
			Helper.EmptyValues();
			Helper.AddValues(List->Items.Num());
			for (int32 Index = 0; Index < List->Items.Num(); ++Index)
			{
				FString ElementError;
				if (!List->Items[Index] || !DabGenImportValue(Ctx, ArrayProperty->Inner, Helper.GetRawPtr(Index), *List->Items[Index], Owner, ElementError, EnumHint))
				{
					OutError = FString::Printf(TEXT("[%d]: %s"), Index, *ElementError);
					return false;
				}
			}
			return true;
		}

		if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
		{
			// A map arrives as a tuple of named items (`("Als.Gait.Walking" = 1)`) or as a block of assignments.
			TArray<TPair<FString, const FDabExpr*>> Entries;
			if (const FDabTupleExpr* Tuple = Expr.As<FDabTupleExpr>())
			{
				for (const FDabArgumentPtr& Item : Tuple->Items)
				{
					if (Item && Item->Value && Item->IsNamed())
					{
						Entries.Emplace(Item->Name, Item->Value.Get());
					}
				}
			}
			else if (const FDabBlockExpr* Block = Expr.As<FDabBlockExpr>())
			{
				for (const FDabStmtPtr& Statement : Block->Statements)
				{
					const FDabAssignStmt* Assign = Statement ? Statement->As<FDabAssignStmt>() : nullptr;
					if (Assign && Assign->Value)
					{
						Entries.Emplace(Assign->Name, Assign->Value.Get());
					}
				}
			}
			else if (const FDabListExpr* EmptyList = Expr.As<FDabListExpr>(); !EmptyList || !EmptyList->Items.IsEmpty())
			{
				OutError = TEXT("expects a map (Key = Value, ...)");
				return false;
			}

			FScriptMapHelper Helper(MapProperty, Value);
			Helper.EmptyValues();
			for (const TPair<FString, const FDabExpr*>& Pair : Entries)
			{
				// A key is written as a name or a string; import it as the bare name it is.
				FDabNameExpr KeyExpr;
				KeyExpr.Name = Pair.Key;
				FDabGenTempValue Key(MapProperty->KeyProp);
				FDabGenTempValue Entry(MapProperty->ValueProp);
				FString EntryError;
				if (!DabGenImportValue(Ctx, MapProperty->KeyProp, Key.Get(), KeyExpr, Owner, EntryError)
					|| !DabGenImportValue(Ctx, MapProperty->ValueProp, Entry.Get(), *Pair.Value, Owner, EntryError))
				{
					OutError = FString::Printf(TEXT("%s: %s"), *Pair.Key, *EntryError);
					return false;
				}
				Helper.AddPair(Key.Get(), Entry.Get());
			}
			return true;
		}

		if (const FDabLiteralExpr* Literal = Expr.As<FDabLiteralExpr>(); Literal && Literal->LiteralKind == EDabLiteralKind::String)
		{
			FOutputDeviceNull Errors;
			if (Property->ImportText_Direct(*Literal->Text, Value, Owner, PPF_None, &Errors) != nullptr)
			{
				return true;
			}
		}

		OutError = FString::Printf(TEXT("a %s cannot take %s"), *Property->GetClass()->GetName(), *DabGenPrint(Expr));
		return false;
	}

	EDabGenApply DabGenApplyPath(FDabGenContext& Ctx, const UStruct* Struct, void* Container, const FString& Path, const FDabExpr& Expr,
		UObject* Owner, FString& OutError)
	{
		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.IsEmpty() || !Struct || !Container)
		{
			OutError = TEXT("an empty property name");
			return EDabGenApply::UnknownProperty;
		}

		const UStruct* CurrentStruct = Struct;
		void* CurrentContainer = Container;
		for (int32 SegmentIndex = 0; SegmentIndex < Segments.Num(); ++SegmentIndex)
		{
			FString Name = Segments[SegmentIndex];
			int32 ElementIndex = INDEX_NONE;
			int32 Bracket = INDEX_NONE;
			if (Name.FindChar(TEXT('['), Bracket))
			{
				if (!Name.EndsWith(TEXT("]")))
				{
					OutError = FString::Printf(TEXT("%s is not a property name"), *Segments[SegmentIndex]);
					return EDabGenApply::Failed;
				}
				ElementIndex = FCString::Atoi(*Name.Mid(Bracket + 1, Name.Len() - Bracket - 2));
				Name.LeftInline(Bracket);
			}

			const FProperty* Property = FindFProperty<FProperty>(CurrentStruct, FName(*Name));
			if (!Property)
			{
				OutError = FString::Printf(TEXT("%s has no property %s"), *CurrentStruct->GetName(), *Name);
				return SegmentIndex == 0 ? EDabGenApply::UnknownProperty : EDabGenApply::Failed;
			}

			void* ValuePtr = nullptr;
			const FProperty* ValueProperty = Property;
			if (ElementIndex != INDEX_NONE)
			{
				if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
				{
					FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(CurrentContainer));
					if (ElementIndex >= Helper.Num())
					{
						Helper.AddValues(ElementIndex + 1 - Helper.Num());
					}
					ValuePtr = Helper.GetRawPtr(ElementIndex);
					ValueProperty = ArrayProperty->Inner;
				}
				else if (ElementIndex >= 0 && ElementIndex < Property->ArrayDim)
				{
					ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer, ElementIndex);
				}
				else
				{
					OutError = FString::Printf(TEXT("%s has no element %d"), *Name, ElementIndex);
					return EDabGenApply::Failed;
				}
			}
			else
			{
				ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			}

			if (SegmentIndex == Segments.Num() - 1)
			{
				if (Property->ArrayDim > 1 && ElementIndex == INDEX_NONE)
				{
					// A C-style array takes a list, element by element.
					const FDabListExpr* List = DabGenUnwrap(Expr).As<FDabListExpr>();
					if (!List || List->Items.Num() > Property->ArrayDim)
					{
						OutError = FString::Printf(TEXT("%s takes a list of at most %d values"), *Name, Property->ArrayDim);
						return EDabGenApply::Failed;
					}
					for (int32 Index = 0; Index < List->Items.Num(); ++Index)
					{
						if (!List->Items[Index]
							|| !DabGenImportValue(Ctx, Property, Property->ContainerPtrToValuePtr<void>(CurrentContainer, Index), *List->Items[Index], Owner, OutError))
						{
							return EDabGenApply::Failed;
						}
					}
					return EDabGenApply::Applied;
				}
				return DabGenImportValue(Ctx, ValueProperty, ValuePtr, Expr, Owner, OutError) ? EDabGenApply::Applied : EDabGenApply::Failed;
			}

			const FStructProperty* StructProperty = CastField<FStructProperty>(ValueProperty);
			if (!StructProperty)
			{
				OutError = FString::Printf(TEXT("%s is not a struct"), *Name);
				return EDabGenApply::Failed;
			}
			CurrentStruct = StructProperty->Struct;
			CurrentContainer = ValuePtr;
		}
		return EDabGenApply::Failed;
	}

	bool DabGenMakeDefault(FDabGenContext& Ctx, const FEdGraphPinType& Type, const FDabExpr& InExpr, FString& OutValue, UObject*& OutObject,
		FString& OutError)
	{
		const FDabExpr& Expr = DabGenUnwrap(InExpr);
		OutValue.Reset();
		OutObject = nullptr;
		if (Type.ContainerType != EPinContainerType::None)
		{
			OutError = TEXT("a default value for a container is not supported");
			return false;
		}

		const FName Category = Type.PinCategory;
		double Number = 0.0;
		const bool bNumber = DabGenTryGetNumber(Expr, Number);

		if (Category == UEdGraphSchema_K2::PC_Boolean)
		{
			bool bValue = false;
			if (!DabGenTryGetBool(Expr, bValue))
			{
				if (!bNumber)
				{
					OutError = TEXT("expects true or false");
					return false;
				}
				bValue = Number != 0.0;
			}
			OutValue = bValue ? TEXT("true") : TEXT("false");
			return true;
		}
		if (Category == UEdGraphSchema_K2::PC_Byte || Category == UEdGraphSchema_K2::PC_Enum)
		{
			if (const UEnum* Enum = Cast<UEnum>(Type.PinSubCategoryObject.Get()))
			{
				// Enum pins hold the value's name.
				int64 EnumValue = 0;
				if (!DabGenResolveEnumValue(Enum, Expr, EnumValue, OutError))
				{
					return false;
				}
				const int32 Index = Enum->GetIndexByValue(EnumValue);
				if (Index == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("%lld is not a value of %s"), EnumValue, *Enum->GetName());
					return false;
				}
				OutValue = Enum->GetNameStringByIndex(Index);
				return true;
			}
		}
		if (Category == UEdGraphSchema_K2::PC_Int || Category == UEdGraphSchema_K2::PC_Int64 || Category == UEdGraphSchema_K2::PC_Byte)
		{
			int64 Integer = 0;
			if (!TryGetDabInteger(Expr, Integer)
				|| (Category == UEdGraphSchema_K2::PC_Int && (Integer < MIN_int32 || Integer > MAX_int32))
				|| (Category == UEdGraphSchema_K2::PC_Byte && (Integer < 0 || Integer > MAX_uint8)))
			{
				OutError = TEXT("expects an integer within the pin's range");
				return false;
			}
			OutValue = FString::Printf(TEXT("%lld"), Integer);
			return true;
		}
		if (Category == UEdGraphSchema_K2::PC_Real || Category == UEdGraphSchema_K2::PC_Float || Category == UEdGraphSchema_K2::PC_Double)
		{
			if (!bNumber)
			{
				OutError = TEXT("expects a number");
				return false;
			}
			OutValue = FString::SanitizeFloat(Number);
			return true;
		}
		if (Category == UEdGraphSchema_K2::PC_Name || Category == UEdGraphSchema_K2::PC_String || Category == UEdGraphSchema_K2::PC_Text)
		{
			if (!DabGenTryGetText(Expr, OutValue))
			{
				OutError = TEXT("expects a string");
				return false;
			}
			return true;
		}
		if (Category == UEdGraphSchema_K2::PC_Struct)
		{
			const UScriptStruct* Struct = Cast<UScriptStruct>(Type.PinSubCategoryObject.Get());
			if (!Struct)
			{
				OutError = TEXT("the pin's struct type is unknown");
				return false;
			}
			FStructOnScope Scope(Struct);
			if (!DabGenImportStruct(Ctx, Struct, Scope.GetStructMemory(), Expr, nullptr, OutError))
			{
				return false;
			}
			Struct->ExportText(OutValue, Scope.GetStructMemory(), nullptr, nullptr, PPF_None, nullptr);
			return true;
		}
		if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class || Category == UEdGraphSchema_K2::PC_SoftObject
			|| Category == UEdGraphSchema_K2::PC_SoftClass || Category == UEdGraphSchema_K2::PC_Interface)
		{
			if (DabGenIsNoneName(Expr))
			{
				return true;
			}
			const bool bWantsClass = Category == UEdGraphSchema_K2::PC_Class || Category == UEdGraphSchema_K2::PC_SoftClass;
			FString ObjectPath;
			if (!DabGenTryGetAssetPath(Expr, ObjectPath) && !DabGenTryGetText(Expr, ObjectPath))
			{
				OutError = TEXT("expects Path(...) or None");
				return false;
			}
			UObject* Object = bWantsClass ? static_cast<UObject*>(DabGenLoadClass(ObjectPath)) : DabGenLoadObject(ObjectPath);
			if (!Object && !ObjectPath.Contains(TEXT("/")))
			{
				Object = DabGenFindType(ObjectPath, bWantsClass ? UClass::StaticClass() : UObject::StaticClass());
			}
			if (!Object)
			{
				OutError = FString::Printf(TEXT("%s cannot be loaded"), *ObjectPath);
				return false;
			}
			OutObject = Object;
			OutValue = Object->GetPathName();
			return true;
		}

		OutError = FString::Printf(TEXT("a %s value cannot be written as %s"), *Category.ToString(), *DabGenPrint(Expr));
		return false;
	}

	bool DabGenSetPinDefault(UEdGraphPin* Pin, const FString& Value, UObject* Object, FString& OutError)
	{
		if (!Pin)
		{
			OutError = TEXT("there is no pin to set");
			return false;
		}
		const UEdGraphSchema_K2* Schema = Cast<const UEdGraphSchema_K2>(Pin->GetSchema());
		if (!Schema)
		{
			OutError = TEXT("the pin's graph has no Blueprint schema");
			return false;
		}

		FString UseValue;
		TObjectPtr<UObject> UseObject;
		FText UseText;
		if (Object)
		{
			UseObject = Object;
		}
		else
		{
			Schema->GetPinDefaultValuesFromString(Pin->PinType, Pin->GetOwningNode(), Value, UseValue, UseObject, UseText, false);
		}

		// TrySetDefaultValue drops a bad value silently; ask first so the problem is reported.
		const FString Problem = Schema->IsPinDefaultValid(Pin, UseValue, UseObject, UseText);
		if (!Problem.IsEmpty())
		{
			OutError = Problem;
			return false;
		}
		if (Object)
		{
			Schema->TrySetDefaultObject(*Pin, Object, false);
		}
		else
		{
			Schema->TrySetDefaultValue(*Pin, Value, false);
		}
		return true;
	}

	bool DabGenSetPinFromValue(UEdGraphPin* Pin, const FProperty* Property, const void* Value, FString& OutError)
	{
		if (!Pin)
		{
			OutError = TEXT("there is no pin to set");
			return false;
		}
		const FName Category = Pin->PinType.PinCategory;
		if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Class || Category == UEdGraphSchema_K2::PC_Interface)
		{
			const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property);
			UObject* Object = ObjectProperty && !Property->IsA<FSoftObjectProperty>() ? ObjectProperty->GetObjectPropertyValue(Value) : nullptr;
			return DabGenSetPinDefault(Pin, FString(), Object, OutError);
		}
		FString Text;
		FBlueprintEditorUtils::PropertyValueToString_Direct(Property, static_cast<const uint8*>(Value), Text, Pin->GetOwningNode());
		return DabGenSetPinDefault(Pin, Text, nullptr, OutError);
	}

	bool DabGenTypeToPinType(const FString& InType, FEdGraphPinType& OutType, FString& OutError)
	{
		OutType = FEdGraphPinType();
		const FString Type = InType.TrimStartAndEnd();

		const auto Inner = [&Type](const TCHAR* Prefix, FString& OutInner) -> bool
		{
			if (Type.StartsWith(Prefix) && Type.EndsWith(TEXT(">")))
			{
				const int32 PrefixLength = FCString::Strlen(Prefix);
				OutInner = Type.Mid(PrefixLength, Type.Len() - PrefixLength - 1).TrimStartAndEnd();
				return true;
			}
			return false;
		};

		FString InnerType;
		if (Inner(TEXT("TArray<"), InnerType))
		{
			if (!DabGenTypeToPinType(InnerType, OutType, OutError))
			{
				return false;
			}
			OutType.ContainerType = EPinContainerType::Array;
			return true;
		}
		const TCHAR* const ClassWrappers[] = { TEXT("TSubclassOf<"), TEXT("TSoftClassPtr<"), TEXT("TSoftObjectPtr<"), TEXT("TObjectPtr<") };
		const FName WrapperCategories[] = { UEdGraphSchema_K2::PC_Class, UEdGraphSchema_K2::PC_SoftClass, UEdGraphSchema_K2::PC_SoftObject, UEdGraphSchema_K2::PC_Object };
		for (int32 Index = 0; Index < static_cast<int32>(UE_ARRAY_COUNT(ClassWrappers)); ++Index)
		{
			if (Inner(ClassWrappers[Index], InnerType))
			{
				UClass* Class = Cast<UClass>(DabGenFindType(InnerType.TrimChar(TEXT('*')), UClass::StaticClass()));
				if (!Class)
				{
					OutError = FString::Printf(TEXT("%s names no class"), *InnerType);
					return false;
				}
				OutType.PinCategory = WrapperCategories[Index];
				OutType.PinSubCategoryObject = Class;
				return true;
			}
		}

		if (Type == TEXT("float") || Type == TEXT("double") || Type == TEXT("real"))
		{
			// Blueprint floats are double precision reals; the graph converts where a float is wanted.
			OutType.PinCategory = UEdGraphSchema_K2::PC_Real;
			OutType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			return true;
		}
		if (Type == TEXT("int") || Type == TEXT("int32"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Int;
			return true;
		}
		if (Type == TEXT("int64"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Int64;
			return true;
		}
		if (Type == TEXT("bool"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			return true;
		}
		if (Type == TEXT("byte") || Type == TEXT("uint8"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			return true;
		}
		if (Type == TEXT("FName") || Type == TEXT("Name"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Name;
			return true;
		}
		if (Type == TEXT("FString") || Type == TEXT("String") || Type == TEXT("string"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_String;
			return true;
		}
		if (Type == TEXT("FText") || Type == TEXT("Text"))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Text;
			return true;
		}

		const FString Bare = Type.TrimChar(TEXT('*'));
		if (UScriptStruct* Struct = Cast<UScriptStruct>(DabGenFindType(Bare, UScriptStruct::StaticClass())))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			OutType.PinSubCategoryObject = Struct;
			return true;
		}
		if (UEnum* Enum = Cast<UEnum>(DabGenFindType(Bare, UEnum::StaticClass())))
		{
			OutType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			OutType.PinSubCategoryObject = Enum;
			return true;
		}
		if (UClass* Class = Cast<UClass>(DabGenFindType(Bare, UClass::StaticClass())))
		{
			OutType.PinCategory = Class->HasAnyClassFlags(CLASS_Interface) ? UEdGraphSchema_K2::PC_Interface : UEdGraphSchema_K2::PC_Object;
			OutType.PinSubCategoryObject = Class;
			return true;
		}

		OutError = FString::Printf(TEXT("%s names no type"), *Type);
		return false;
	}

	void DabGenGetInputParameters(const UFunction* Function, TArray<FProperty*>& OutParameters)
	{
		OutParameters.Reset();
		if (!Function)
		{
			return;
		}
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			FProperty* Parameter = *It;
			if (Parameter->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				continue;
			}
			if (Parameter->HasAnyPropertyFlags(CPF_OutParm) && !Parameter->HasAnyPropertyFlags(CPF_ReferenceParm))
			{
				continue;
			}
			OutParameters.Add(Parameter);
		}
	}

	FString DabGenMetaString(const FDabExpr& InExpr)
	{
		const FDabExpr& Expr = DabGenUnwrap(InExpr);
		if (const FDabLiteralExpr* Literal = Expr.As<FDabLiteralExpr>())
		{
			if (Literal->LiteralKind == EDabLiteralKind::Bool)
			{
				return Literal->bBool ? TEXT("true") : TEXT("false");
			}
			return Literal->Text;
		}
		FString Text;
		if (DabGenTryGetText(Expr, Text))
		{
			return Text;
		}
		return DabGenPrint(Expr);
	}
}
