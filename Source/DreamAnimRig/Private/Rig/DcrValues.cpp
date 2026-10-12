// Copyright DreamMoon. All Rights Reserved.

#include "Rig/DcrValues.h"

#include <cmath>

#include "EulerTransform.h"
#include "Misc/OutputDeviceNull.h"
#include "Model/DabModel.h"
#include "Rig/DcrNames.h"
#include "Rig/DcrSyntax.h"
#include "RigVMCore/RigVMTrait.h"
#include "RigVMTypeUtils.h"
#include "Rigs/RigHierarchyDefines.h"
#include "StructUtils/UserDefinedStruct.h"
#include "TransformNoScale.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Rig
{
	using namespace UE::DreamAnim::Lang;

	namespace ValuesPrivate
	{
		/** A temporary of a property's type, initialised and destroyed with it. */
		class FTempValue
		{
		public:
			explicit FTempValue(const FProperty& InProperty)
				: Property(InProperty)
			{
				Memory = static_cast<uint8*>(FMemory::Malloc(FMath::Max(1, Property.GetSize()), Property.GetMinAlignment()));
				Property.InitializeValue(Memory);
			}
			~FTempValue()
			{
				Property.DestroyValue(Memory);
				FMemory::Free(Memory);
			}
			FTempValue(const FTempValue&) = delete;
			FTempValue& operator=(const FTempValue&) = delete;

			uint8* Get() const { return Memory; }

		private:
			const FProperty& Property;
			uint8* Memory = nullptr;
		};

		/** The shortest decimal that reads back as the same float, as a double: 0.2f -> 0.2 rather than 0.200000003. */
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

		/** Structs written whole and by position when more than one member is set. */
		bool IsSmallMathStruct(const UScriptStruct& Struct)
		{
			static const FName Names[] = {
				TEXT("Vector"), TEXT("Vector3f"), TEXT("Vector3d"), TEXT("Vector2D"), TEXT("Vector2f"), TEXT("Vector4"), TEXT("Vector4f"),
				TEXT("Quat"), TEXT("Quat4f"), TEXT("LinearColor"), TEXT("Color"), TEXT("IntPoint"), TEXT("IntVector"), TEXT("IntVector4"),
			};
			for (const FName& Name : Names)
			{
				if (Struct.GetFName() == Name)
				{
					return true;
				}
			}
			return false;
		}

		const UEnum* GetElementTypeEnum()
		{
			return StaticEnum<ERigElementType>();
		}

		/** An element type by its name (`Bone`, `None`); INDEX_NONE for any other name. */
		int64 FindElementType(const FString& Name)
		{
			// `None` is the enum's first value, but a name the enum lookup reads as no name.
			return Name == TEXT("None") ? int64(ERigElementType::None) : GetElementTypeEnum()->GetValueByNameString(Name);
		}

		FDabExprPtr MakeEnumValue(const UEnum* Enum, int64 Value)
		{
			if (Enum && Enum->IsValidEnumValue(Value))
			{
				return MakeDcrName(Enum->GetNameStringByValue(Value));
			}
			return MakeDcrInteger(Value);
		}

		FDabExprPtr MakeObjectName(const FSoftObjectPath& Path)
		{
			return MakeDcrAssetExpr(Path);
		}

		const FDabExpr& StripParens(const FDabExpr& Expr)
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

		/** A number literal, possibly negated. */
		bool TryGetNumber(const FDabExpr& Expr, double& OutValue)
		{
			const FDabExpr& Inner = StripParens(Expr);
			if (const FDabLiteralExpr* Literal = Inner.As<FDabLiteralExpr>())
			{
				if (Literal->LiteralKind == EDabLiteralKind::Number)
				{
					OutValue = Literal->Number;
					return true;
				}
				return false;
			}
			if (const FDabUnaryExpr* Unary = Inner.As<FDabUnaryExpr>())
			{
				double Operand = 0.0;
				if (Unary->Op == EDabUnaryOp::Negate && Unary->Operand && TryGetNumber(*Unary->Operand, Operand))
				{
					OutValue = -Operand;
					return true;
				}
			}
			return false;
		}

		/** The text of a string literal or a bare name. */
		bool TryGetNameText(const FDabExpr& Expr, FString& OutText)
		{
			const FDabExpr& Inner = StripParens(Expr);
			if (const FDabLiteralExpr* Literal = Inner.As<FDabLiteralExpr>())
			{
				if (Literal->LiteralKind == EDabLiteralKind::String)
				{
					OutText = Literal->Text;
					return true;
				}
				return false;
			}
			if (const FDabNameExpr* Name = Inner.As<FDabNameExpr>())
			{
				OutText = Name->Name;
				return true;
			}
			return false;
		}

		FString JoinPath(const FString& SubPath, const FString& Segment)
		{
			return SubPath.IsEmpty() ? Segment : SubPath + TEXT(".") + Segment;
		}

		FDabExprPtr MakeStructExpr(const UScriptStruct& Struct, const void* Value, const void* Default);
	}

	// ------------------------------------------------------------------------------------------
	// Slots
	// ------------------------------------------------------------------------------------------

	bool FDcrValueSlot::Init(const Model::FDcrType& Type)
	{
		Property = nullptr;
		Bag.Reset();
		FString CPPType = Type.CPPType;
		EPropertyBagContainerType Container = EPropertyBagContainerType::None;
		if (RigVMTypeUtils::IsArrayType(CPPType))
		{
			Container = EPropertyBagContainerType::Array;
			CPPType = RigVMTypeUtils::BaseTypeFromArrayType(CPPType);
		}

		EPropertyBagPropertyType ValueType = EPropertyBagPropertyType::None;
		UObject* TypeObject = nullptr;
		if (!Type.ObjectPath.IsEmpty())
		{
			TypeObject = RigVMTypeUtils::FindObjectFromCPPTypeObjectPath(Type.ObjectPath);
			if (Cast<UScriptStruct>(TypeObject))
			{
				ValueType = EPropertyBagPropertyType::Struct;
			}
			else if (Cast<UEnum>(TypeObject))
			{
				ValueType = EPropertyBagPropertyType::Enum;
			}
			else if (Cast<UClass>(TypeObject))
			{
				ValueType = CPPType.StartsWith(TEXT("TSubclassOf")) ? EPropertyBagPropertyType::Class
					: CPPType.StartsWith(TEXT("TSoftObjectPtr")) ? EPropertyBagPropertyType::SoftObject
					: EPropertyBagPropertyType::Object;
			}
		}
		else
		{
			static const TPair<const TCHAR*, EPropertyBagPropertyType> Builtins[] = {
				{ TEXT("bool"), EPropertyBagPropertyType::Bool },
				{ TEXT("uint8"), EPropertyBagPropertyType::Byte },
				{ TEXT("int32"), EPropertyBagPropertyType::Int32 },
				{ TEXT("int64"), EPropertyBagPropertyType::Int64 },
				{ TEXT("float"), EPropertyBagPropertyType::Float },
				{ TEXT("double"), EPropertyBagPropertyType::Double },
				{ TEXT("FName"), EPropertyBagPropertyType::Name },
				{ TEXT("FString"), EPropertyBagPropertyType::String },
				{ TEXT("FText"), EPropertyBagPropertyType::Text },
			};
			for (const TPair<const TCHAR*, EPropertyBagPropertyType>& Builtin : Builtins)
			{
				if (CPPType.Equals(Builtin.Key, ESearchCase::CaseSensitive))
				{
					ValueType = Builtin.Value;
					break;
				}
			}
		}
		if (ValueType == EPropertyBagPropertyType::None)
		{
			return false;
		}

		static const FName SlotName(TEXT("Value"));
		Bag.AddProperties({ FPropertyBagPropertyDesc(SlotName, Container, ValueType, TypeObject) });
		const FPropertyBagPropertyDesc* Desc = Bag.FindPropertyDescByName(SlotName);
		Property = Desc ? Desc->CachedProperty : nullptr;
		return Property != nullptr;
	}

	bool FDcrValueSlot::Init(const FProperty& Like)
	{
		Property = nullptr;
		Bag.Reset();
		static const FName SlotName(TEXT("Value"));
		Bag.AddProperty(SlotName, &Like);
		const FPropertyBagPropertyDesc* Desc = Bag.FindPropertyDescByName(SlotName);
		Property = Desc ? Desc->CachedProperty : nullptr;
		return Property != nullptr;
	}

	void* FDcrValueSlot::GetMemory()
	{
		return Property ? Property->ContainerPtrToValuePtr<void>(Bag.GetMutableValue().GetMemory()) : nullptr;
	}

	bool FDcrValueSlot::ImportText(const FString& Text)
	{
		if (!Property)
		{
			return false;
		}
		// A new value, as Init made it: a user struct's member defaults included (ClearValue would zero them).
		void* Memory = GetMemory();
		Property->DestroyValue(Memory);
		Property->InitializeValue(Memory);
		if (Text.IsEmpty())
		{
			return true;
		}
		FOutputDeviceNull Errors;
		if (!Property->ImportText_Direct(*Text, Memory, nullptr, PPF_None, &Errors))
		{
			Property->DestroyValue(Memory);
			Property->InitializeValue(Memory);
			return false;
		}
		return true;
	}

	FString FDcrValueSlot::ExportText()
	{
		return Property ? ExportDcrValueText(*Property, GetMemory()) : FString();
	}

	namespace ValuesPrivate
	{
		FString ExactDouble(double Value)
		{
			return Value == 0.0 && std::signbit(Value) ? FString(TEXT("-0")) : Model::FormatDabNumber(Value);
		}

		FString ExactFloat(float Value)
		{
			return Value == 0.f && std::signbit(Value) ? FString(TEXT("-0")) : Model::FormatDabNumber(FloatToShortestDouble(Value));
		}

		/** One value inside a struct or an array: as the engine writes members there (strings and names quoted), numbers exact. */
		FString ExportExact(const FProperty& Property, const void* Value, int32 PortFlags)
		{
			if (const FFloatProperty* Float = CastField<FFloatProperty>(&Property))
			{
				return ExactFloat(Float->GetPropertyValue(Value));
			}
			if (const FDoubleProperty* Double = CastField<FDoubleProperty>(&Property))
			{
				return ExactDouble(Double->GetPropertyValue(Value));
			}
			if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
			{
				return ExportDcrStructText(*Struct->Struct, Value);
			}
			if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
			{
				FScriptArrayHelper Helper(Array, Value);
				FString Text = TEXT("(");
				for (int32 Index = 0; Index < Helper.Num(); ++Index)
				{
					Text += Index > 0 ? TEXT(",") : TEXT("");
					Text += ExportExact(*Array->Inner, Helper.GetRawPtr(Index), PPF_Delimited);
				}
				return Text + TEXT(")");
			}
			FString Text;
			Property.ExportTextItem_Direct(Text, Value, nullptr, nullptr, PortFlags);
			return Text;
		}
	}

	FString ExportDcrValueText(const FProperty& Property, const void* Value)
	{
		return ValuesPrivate::ExportExact(Property, Value, PPF_None);
	}

	FString ExportDcrStructText(const UScriptStruct& Struct, const void* Value)
	{
		// A struct that writes itself is left to do so (soft paths, gameplay tags): its text is not numbers.
		if (Struct.StructFlags & STRUCT_ExportTextItemNative)
		{
			FString Text;
			Struct.ExportText(Text, Value, nullptr, nullptr, PPF_None, nullptr);
			return Text;
		}
		FString Text = TEXT("(");
		bool bFirst = true;
		for (TFieldIterator<FProperty> It(&Struct); It; ++It)
		{
			if (!It->ShouldPort(PPF_None))
			{
				continue;
			}
			for (int32 Index = 0; Index < It->ArrayDim; ++Index)
			{
				Text += bFirst ? TEXT("") : TEXT(",");
				bFirst = false;
				Text += It->ArrayDim > 1 ? FString::Printf(TEXT("%s(%d)="), *It->GetName(), Index) : It->GetName() + TEXT("=");
				Text += ValuesPrivate::ExportExact(**It, It->ContainerPtrToValuePtr<void>(Value, Index), PPF_Delimited);
			}
		}
		return Text + TEXT(")");
	}

	bool AreDcrValuesSame(const FProperty& Property, const void* A, const void* B)
	{
		return ExportDcrValueText(Property, A) == ExportDcrValueText(Property, B);
	}

	bool ResetDcrValueParts(const FProperty& Property, void* Value, const void* Default, TConstArrayView<FString> SubPaths)
	{
		using namespace ValuesPrivate;
		bool bAll = true;
		// Elements past the end of the default are reset to a new element, as an array grown to take a link makes one.
		TArray<TUniquePtr<FTempValue>> NewElements;
		for (const FString& SubPath : SubPaths)
		{
			TArray<FString> Segments;
			SubPath.ParseIntoArray(Segments, TEXT("."));
			const FProperty* Current = &Property;
			void* Target = Value;
			const void* Source = Default;
			for (const FString& Segment : Segments)
			{
				if (const FArrayProperty* Array = CastField<FArrayProperty>(Current); Array && Segment.IsNumeric())
				{
					const int32 Index = FCString::Atoi(*Segment);
					FScriptArrayHelper Values(Array, Target);
					if (!Values.IsValidIndex(Index))
					{
						Target = nullptr;
						break;
					}
					Target = Values.GetRawPtr(Index);
					const void* NextSource = nullptr;
					if (Source)
					{
						FScriptArrayHelper Defaults(Array, const_cast<void*>(Source));
						NextSource = Defaults.IsValidIndex(Index) ? Defaults.GetRawPtr(Index) : nullptr;
					}
					if (!NextSource)
					{
						NextSource = NewElements.Add_GetRef(MakeUnique<FTempValue>(*Array->Inner))->Get();
					}
					Source = NextSource;
					Current = Array->Inner;
				}
				else if (const FStructProperty* Struct = CastField<FStructProperty>(Current))
				{
					const FProperty* Member = Struct->Struct->FindPropertyByName(*Segment);
					if (!Member)
					{
						Target = nullptr;
						break;
					}
					Target = Member->ContainerPtrToValuePtr<void>(Target);
					Source = Source ? Member->ContainerPtrToValuePtr<void>(Source) : nullptr;
					Current = Member;
				}
				else
				{
					Target = nullptr;
					break;
				}
			}
			if (!Target)
			{
				bAll = false;
				continue;
			}
			if (Source)
			{
				Current->CopyCompleteValue(Target, Source);
			}
			else
			{
				Current->ClearValue(Target);
			}
		}
		return bAll;
	}

	namespace ValuesPrivate
	{
		bool ClearNegativeZeros(const FProperty& Property, void* Value)
		{
			bool bChanged = false;
			if (const FFloatProperty* Float = CastField<FFloatProperty>(&Property))
			{
				float& Number = *static_cast<float*>(Value);
				bChanged = Number == 0.f && std::signbit(Number);
				Number = bChanged ? 0.f : Number;
			}
			else if (const FDoubleProperty* Double = CastField<FDoubleProperty>(&Property))
			{
				double& Number = *static_cast<double*>(Value);
				bChanged = Number == 0.0 && std::signbit(Number);
				Number = bChanged ? 0.0 : Number;
			}
			else if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
			{
				for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
				{
					for (int32 Index = 0; Index < It->ArrayDim; ++Index)
					{
						bChanged |= ClearNegativeZeros(**It, It->ContainerPtrToValuePtr<void>(Value, Index));
					}
				}
			}
			else if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
			{
				FScriptArrayHelper Values(Array, Value);
				for (int32 Index = 0; Index < Values.Num(); ++Index)
				{
					bChanged |= ClearNegativeZeros(*Array->Inner, Values.GetRawPtr(Index));
				}
			}
			return bChanged;
		}
	}

	FString StripDcrNegativeZeros(const Model::FDcrType& Type, const FString& Text)
	{
		FDcrValueSlot Slot;
		if (Text.IsEmpty() || !Text.Contains(TEXT("-0")) || !Slot.Init(Type) || !Slot.ImportText(Text))
		{
			return Text;
		}
		return ValuesPrivate::ClearNegativeZeros(*Slot.GetProperty(), Slot.GetMemory()) ? Slot.ExportText() : Text;
	}

	bool AreDcrValuesNear(const FProperty& Property, const void* A, const void* B, double Tolerance)
	{
		if (const FFloatProperty* Float = CastField<FFloatProperty>(&Property))
		{
			return FMath::Abs(double(*static_cast<const float*>(A)) - double(*static_cast<const float*>(B))) <= Tolerance;
		}
		if (const FDoubleProperty* Double = CastField<FDoubleProperty>(&Property))
		{
			return FMath::Abs(*static_cast<const double*>(A) - *static_cast<const double*>(B)) <= Tolerance;
		}
		if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
		{
			for (TFieldIterator<FProperty> It(Struct->Struct); It; ++It)
			{
				for (int32 Index = 0; Index < It->ArrayDim; ++Index)
				{
					if (!AreDcrValuesNear(**It, It->ContainerPtrToValuePtr<void>(A, Index), It->ContainerPtrToValuePtr<void>(B, Index), Tolerance))
					{
						return false;
					}
				}
			}
			return true;
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
		{
			FScriptArrayHelper ValuesA(Array, A);
			FScriptArrayHelper ValuesB(Array, B);
			if (ValuesA.Num() != ValuesB.Num())
			{
				return false;
			}
			for (int32 Index = 0; Index < ValuesA.Num(); ++Index)
			{
				if (!AreDcrValuesNear(*Array->Inner, ValuesA.GetRawPtr(Index), ValuesB.GetRawPtr(Index), Tolerance))
				{
					return false;
				}
			}
			return true;
		}
		return AreDcrValuesSame(Property, A, B);
	}

	bool AreDcrValueTextsNear(const Model::FDcrType& Type, const FString& A, const FString& B, double Tolerance)
	{
		FDcrValueSlot SlotA;
		FDcrValueSlot SlotB;
		return SlotA.Init(Type) && SlotB.Init(Type) && SlotA.ImportText(A) && SlotB.ImportText(B)
			&& AreDcrValuesNear(*SlotA.GetProperty(), SlotA.GetMemory(), SlotB.GetMemory(), Tolerance);
	}

	FString MakeDcrCanonicalDefault(const Model::FDcrType& Type, const FString& Text)
	{
		const FString Stripped = StripDcrNegativeZeros(Type, Text);
		FDcrValueSlot Slot;
		FDcrValueSlot New;
		if (Stripped.IsEmpty() || !Slot.Init(Type) || !New.Init(Type) || !Slot.ImportText(Stripped))
		{
			return Stripped;
		}
		return AreDcrValuesSame(*Slot.GetProperty(), Slot.GetMemory(), New.GetMemory()) ? FString() : Stripped;
	}

	FString MakeDcrEngineDefault(const Model::FDcrType& Type, const FString& Text)
	{
		if (Text.IsEmpty() && !Type.ObjectPath.IsEmpty() && Cast<UUserDefinedStruct>(RigVMTypeUtils::FindObjectFromCPPTypeObjectPath(Type.ObjectPath)))
		{
			FDcrValueSlot Slot;
			return Slot.Init(Type) ? Slot.ExportText() : Text;
		}
		return StripDcrNegativeZeros(Type, Text);
	}

	bool TrimDcrLinkedElements(const FProperty& Property, void* Value, const void* Default, TConstArrayView<FString> SubPaths)
	{
		using namespace ValuesPrivate;
		const FArrayProperty* Array = CastField<FArrayProperty>(&Property);
		if (!Array)
		{
			return false;
		}
		FScriptArrayHelper Values(Array, Value);
		const int32 DefaultNum = Default ? FScriptArrayHelper(Array, const_cast<void*>(Default)).Num() : 0;
		const FTempValue NewElement(*Array->Inner);
		bool bTrimmed = false;
		while (Values.Num() > DefaultNum)
		{
			const int32 Last = Values.Num() - 1;
			const FString Index = FString::FromInt(Last);
			const bool bLinked = SubPaths.ContainsByPredicate([&Index](const FString& Path) { return Path == Index || Path.StartsWith(Index + TEXT(".")); });
			if (!bLinked || !AreDcrValuesSame(*Array->Inner, Values.GetRawPtr(Last), NewElement.Get()))
			{
				break;
			}
			Values.RemoveValues(Last, 1);
			bTrimmed = true;
		}
		return bTrimmed;
	}

	Model::FDcrType GetDcrType(const FProperty& Property)
	{
		Model::FDcrType Type;
		Type.CPPType = RigVMTypeUtils::GetCPPTypeFromProperty(&Property);
		if (const UObject* Object = RigVMTypeUtils::GetCPPTypeObjectFromProperty(&Property))
		{
			Type.ObjectPath = Object->GetPathName();
		}
		return Type;
	}

	const FProperty* FindDcrStructMember(const UScriptStruct& Struct, const FString& Name)
	{
		for (TFieldIterator<FProperty> It(&Struct); It; ++It)
		{
			if (It->GetName().Equals(Name, ESearchCase::CaseSensitive) || GetDcrMemberName(**It).Equals(Name, ESearchCase::CaseSensitive))
			{
				return *It;
			}
		}
		return nullptr;
	}

	FString GetDcrMemberName(const FProperty& Member)
	{
		if (Cast<UUserDefinedStruct>(Member.GetOwnerStruct()))
		{
			return Member.GetAuthoredName();
		}
		return Member.GetName();
	}

	// ------------------------------------------------------------------------------------------
	// Value -> literal
	// ------------------------------------------------------------------------------------------

	FDabExprPtr MakeDcrValueExpr(const FProperty& Property, const void* Value, const void* Default)
	{
		using namespace ValuesPrivate;
		TUniquePtr<FTempValue> OwnDefault;
		if (!Default)
		{
			OwnDefault = MakeUnique<FTempValue>(Property);
			Default = OwnDefault->Get();
		}

		if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
		{
			return MakeDcrBool(Bool->GetPropertyValue(Value));
		}
		if (const FEnumProperty* Enum = CastField<FEnumProperty>(&Property))
		{
			return MakeEnumValue(Enum->GetEnum(), Enum->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value));
		}
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
		{
			if (const UEnum* NumericEnum = Numeric->GetIntPropertyEnum())
			{
				return MakeEnumValue(NumericEnum, Numeric->GetSignedIntPropertyValue(Value));
			}
			if (const FFloatProperty* Float = CastField<FFloatProperty>(&Property))
			{
				return MakeDcrNumber(FloatToShortestDouble(Float->GetPropertyValue(Value)));
			}
			if (Numeric->IsFloatingPoint())
			{
				return MakeDcrNumber(Numeric->GetFloatingPointPropertyValue(Value));
			}
			return MakeDcrInteger(Numeric->GetSignedIntPropertyValue(Value));
		}
		if (const FNameProperty* Name = CastField<FNameProperty>(&Property))
		{
			const FName NameValue = Name->GetPropertyValue(Value);
			return NameValue.IsNone() ? MakeDcrName(TEXT("None")) : MakeDcrString(NameValue.ToString());
		}
		if (const FStrProperty* String = CastField<FStrProperty>(&Property))
		{
			return MakeDcrString(String->GetPropertyValue(Value));
		}
		if (const FTextProperty* Text = CastField<FTextProperty>(&Property))
		{
			return MakeDcrString(Text->GetPropertyValue(Value).ToString());
		}
		if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
		{
			return MakeStructExpr(*Struct->Struct, Value, Default);
		}
		if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
		{
			FScriptArrayHelper Helper(Array, Value);
			FTempValue ElementDefault(*Array->Inner);
			TArray<FDabExprPtr> Items;
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				Items.Add(MakeDcrValueExpr(*Array->Inner, Helper.GetRawPtr(Index), ElementDefault.Get()));
			}
			return MakeDcrList(MoveTemp(Items));
		}
		if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(&Property))
		{
			return MakeObjectName(Soft->GetPropertyValue(Value).ToSoftObjectPath());
		}
		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
		{
			const UObject* Target = Object->GetObjectPropertyValue(Value);
			return MakeObjectName(Target ? FSoftObjectPath(Target) : FSoftObjectPath());
		}
		return MakeDcrUnsupported(FString::Printf(TEXT("a value of type %s"), *Property.GetCPPType()));
	}

	FDabExprPtr ValuesPrivate::MakeStructExpr(const UScriptStruct& Struct, const void* Value, const void* Default)
	{
		// A soft object path is an asset reference: `SKM_Manny`, `Path(Plugin.X, "A/B")`, `None`.
		if (Struct.IsChildOf(TBaseStructure<FSoftObjectPath>::Get()))
		{
			return MakeDcrAssetExpr(*static_cast<const FSoftObjectPath*>(Value));
		}

		// An element key with a kind: `Bone(pelvis)`. One without (`None`) is written as a struct: `()` at its default.
		if (&Struct == FRigElementKey::StaticStruct() && static_cast<const FRigElementKey*>(Value)->Type != ERigElementType::None)
		{
			const FRigElementKey& Key = *static_cast<const FRigElementKey*>(Value);
			TArray<FDabExprPtr> Args;
			Args.Add(MakeDcrName(Key.Name.ToString()));
			return MakeDcrCall(GetElementTypeEnum()->GetNameStringByValue(int64(Key.Type)), MoveTemp(Args));
		}

		TArray<const FProperty*> Members;
		TArray<const FProperty*> Changed;
		for (TFieldIterator<FProperty> It(&Struct); It; ++It)
		{
			Members.Add(*It);
			if (!AreDcrValuesSame(**It, It->ContainerPtrToValuePtr<void>(Value), It->ContainerPtrToValuePtr<void>(Default)))
			{
				Changed.Add(*It);
			}
		}

		TArray<TPair<FString, FDabExprPtr>> Items;
		if (IsSmallMathStruct(Struct) && Changed.Num() > 1)
		{
			for (const FProperty* Member : Members)
			{
				Items.Emplace(FString(), MakeDcrValueExpr(*Member, Member->ContainerPtrToValuePtr<void>(Value), Member->ContainerPtrToValuePtr<void>(Default)));
			}
			return MakeDcrTuple(MoveTemp(Items));
		}
		for (const FProperty* Member : Changed)
		{
			Items.Emplace(GetDcrMemberName(*Member),
				MakeDcrValueExpr(*Member, Member->ContainerPtrToValuePtr<void>(Value), Member->ContainerPtrToValuePtr<void>(Default)));
		}
		return MakeDcrTuple(MoveTemp(Items));
	}

	namespace ValuesPrivate
	{
		bool HasPartBelow(const TMap<FString, FDabExprPtr>& Parts, const FString& Path)
		{
			const FString Prefix = Path.IsEmpty() ? FString() : Path + TEXT(".");
			for (const TPair<FString, FDabExprPtr>& Part : Parts)
			{
				if (Part.Value && (Path.IsEmpty() || Part.Key == Path || Part.Key.StartsWith(Prefix)))
				{
					return true;
				}
			}
			return false;
		}

		FDabExprPtr MakeWithParts(const FProperty& Property, const void* Value, const void* Default, TMap<FString, FDabExprPtr>& Parts, const FString& Path)
		{
			if (FDabExprPtr* Exact = Parts.Find(Path); Exact && *Exact)
			{
				return MoveTemp(*Exact);
			}
			if (!HasPartBelow(Parts, Path))
			{
				return MakeDcrValueExpr(Property, Value, Default);
			}

			if (const FStructProperty* StructProperty = CastField<FStructProperty>(&Property))
			{
				const UScriptStruct& Struct = *StructProperty->Struct;
				if (&Struct == FRigElementKey::StaticStruct())
				{
					// `Bone(expr)` when only the name is linked and its expression cannot be read as an element name.
					const FString NamePath = JoinPath(Path, TEXT("Name"));
					FDabExprPtr* NamePart = Parts.Find(NamePath);
					const bool bOnlyName = NamePart && *NamePart && !Parts.Contains(JoinPath(Path, TEXT("Type")));
					if (bOnlyName && !(*NamePart)->Is<FDabNameExpr>() && !(*NamePart)->Is<FDabLiteralExpr>())
					{
						const FRigElementKey& Key = *static_cast<const FRigElementKey*>(Value);
						TArray<FDabExprPtr> Args;
						Args.Add(MoveTemp(*NamePart));
						return MakeDcrCall(GetElementTypeEnum()->GetNameStringByValue(int64(Key.Type)), MoveTemp(Args));
					}
				}
				TArray<TPair<FString, FDabExprPtr>> Items;
				for (TFieldIterator<FProperty> It(&Struct); It; ++It)
				{
					const FString MemberPath = JoinPath(Path, It->GetName());
					const void* MemberValue = It->ContainerPtrToValuePtr<void>(Value);
					const void* MemberDefault = It->ContainerPtrToValuePtr<void>(Default);
					if (HasPartBelow(Parts, MemberPath) || Parts.Contains(MemberPath))
					{
						Items.Emplace(GetDcrMemberName(**It), MakeWithParts(**It, MemberValue, MemberDefault, Parts, MemberPath));
					}
					else if (!AreDcrValuesSame(**It, MemberValue, MemberDefault))
					{
						Items.Emplace(GetDcrMemberName(**It), MakeDcrValueExpr(**It, MemberValue, MemberDefault));
					}
				}
				return MakeDcrTuple(MoveTemp(Items));
			}

			if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
			{
				// Elements linked past the end of the value are there on the pin, at their defaults: they exist to
				// take the links. The value is the caller's scratch copy, so it is grown in place.
				FScriptArrayHelper Helper(Array, const_cast<void*>(Value));
				const FString Prefix = Path.IsEmpty() ? FString() : Path + TEXT(".");
				int32 Needed = Helper.Num();
				for (const TPair<FString, FDabExprPtr>& Part : Parts)
				{
					// StartsWith is false for an empty prefix: the value's own elements are under no prefix at all.
					if (!Part.Value || (!Prefix.IsEmpty() && !Part.Key.StartsWith(Prefix)))
					{
						continue;
					}
					FString First = Part.Key.Mid(Prefix.Len());
					int32 Dot = INDEX_NONE;
					if (First.FindChar(TEXT('.'), Dot))
					{
						First.LeftInline(Dot);
					}
					if (First.IsNumeric())
					{
						Needed = FMath::Max(Needed, FCString::Atoi(*First) + 1);
					}
				}
				if (Needed > Helper.Num())
				{
					Helper.AddValues(Needed - Helper.Num());
				}
				FTempValue ElementDefault(*Array->Inner);
				TArray<FDabExprPtr> Items;
				for (int32 Index = 0; Index < Helper.Num(); ++Index)
				{
					Items.Add(MakeWithParts(*Array->Inner, Helper.GetRawPtr(Index), ElementDefault.Get(), Parts, JoinPath(Path, FString::FromInt(Index))));
				}
				return MakeDcrList(MoveTemp(Items));
			}

			return MakeDcrValueExpr(Property, Value, Default);
		}
	}

	FDabExprPtr MakeDcrValueExprWithParts(const FProperty& Property, const void* Value, const void* Default,
		TMap<FString, FDabExprPtr>& Parts, TArray<FString>& OutUnplaced)
	{
		using namespace ValuesPrivate;
		TUniquePtr<FTempValue> OwnDefault;
		if (!Default)
		{
			OwnDefault = MakeUnique<FTempValue>(Property);
			Default = OwnDefault->Get();
		}
		FDabExprPtr Result = MakeWithParts(Property, Value, Default, Parts, FString());
		for (TPair<FString, FDabExprPtr>& Part : Parts)
		{
			if (Part.Value)
			{
				OutUnplaced.Add(Part.Key);
			}
		}
		return Result;
	}

	FDabExprPtr MakeDcrValueExprFromText(const FProperty& Property, const FString& Text, const FString& DefaultText)
	{
		FDcrValueSlot Value;
		FDcrValueSlot Default;
		if (!Value.Init(Property) || !Default.Init(Property) || !Value.ImportText(Text) || !Default.ImportText(DefaultText))
		{
			return nullptr;
		}
		return MakeDcrValueExpr(*Value.GetProperty(), Value.GetMemory(), Default.GetMemory());
	}

	FDabExprPtr MakeDcrValueExprFromText(const Model::FDcrType& Type, const FString& Text, const FString& DefaultText)
	{
		FDcrValueSlot Value;
		FDcrValueSlot Default;
		if (!Value.Init(Type) || !Default.Init(Type) || !Value.ImportText(Text) || !Default.ImportText(DefaultText))
		{
			return nullptr;
		}
		return MakeDcrValueExpr(*Value.GetProperty(), Value.GetMemory(), Default.GetMemory());
	}

	// ------------------------------------------------------------------------------------------
	// Literal -> value
	// ------------------------------------------------------------------------------------------

	bool WriteDcrValueExpr(const FProperty& Property, void* Value, const FDabExpr& InExpr,
		TFunctionRef<bool(const FDabExpr&, const FProperty&)> IsConstant, FDcrAssetResolver ResolveAsset,
		TArray<FDcrValuePart>& OutParts, FString& OutError, const FString& SubPath)
	{
		using namespace ValuesPrivate;
		const FDabExpr& Expr = StripParens(InExpr);
		const FString Where = SubPath.IsEmpty() ? FString(TEXT("the value")) : FString::Printf(TEXT("`%s`"), *SubPath);

		// Structure is always walked; anything else is a leaf, and the caller says whether a leaf is known now.
		const FStructProperty* StructProperty = CastField<FStructProperty>(&Property);
		const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(&Property);
		const bool bTuple = Expr.Is<FDabTupleExpr>() && StructProperty;
		const bool bList = Expr.Is<FDabListExpr>() && ArrayProperty;
		// `Bone(name)`: a call named after an element type; any other call there is a node's value.
		const FDabCallExpr* KeyCall = StructProperty && StructProperty->Struct == FRigElementKey::StaticStruct() ? Expr.As<FDabCallExpr>() : nullptr;
		const FDabNameExpr* KeyType = KeyCall && KeyCall->Callee ? KeyCall->Callee->As<FDabNameExpr>() : nullptr;
		const bool bElementKey = KeyType && FindElementType(KeyType->Name) != INDEX_NONE;
		// `SetDeformerTransformVariable(Value = x)`: a trait's value, written as a call of the trait's struct.
		const FDabCallExpr* TraitCall = StructProperty && StructProperty->Struct->IsChildOf(FRigVMTrait::StaticStruct()) ? Expr.As<FDabCallExpr>() : nullptr;
		const FDabNameExpr* TraitType = TraitCall && TraitCall->Callee ? TraitCall->Callee->As<FDabNameExpr>() : nullptr;
		const bool bTrait = TraitType && FindDcrTraitStruct(TraitType->Name) == StructProperty->Struct;
		// An asset named where a soft object path goes: `SKM_Manny`, `Path(Plugin.X, "A/B")`, `None`.
		const bool bSoftPath = StructProperty && StructProperty->Struct->IsChildOf(TBaseStructure<FSoftObjectPath>::Get())
			&& ((Expr.Is<FDabNameExpr>() && IsConstant(Expr, Property)) || !GetDcrPathExprObjectPath(Expr).IsEmpty());
		if (bSoftPath)
		{
			FSoftObjectPath& Path = *static_cast<FSoftObjectPath*>(Value);
			const FDabNameExpr* Name = Expr.As<FDabNameExpr>();
			if (Name && Name->Name == TEXT("None"))
			{
				Path.Reset();
				return true;
			}
			FString ObjectPath = GetDcrPathExprObjectPath(Expr);
			if (ObjectPath.IsEmpty() && Name)
			{
				const TArray<FString> Found = FindDcrAssetsByName(Name->Name);
				if (Found.Num() != 1)
				{
					OutError = FString::Printf(TEXT("%s: %s"), *Where, Found.IsEmpty() ? *FString::Printf(TEXT("no asset is called %s"), *Name->Name)
						: *FString::Printf(TEXT("%d assets are called %s; write Path(...)"), Found.Num(), *Name->Name));
					return false;
				}
				ObjectPath = Found[0];
			}
			Path.SetPath(ObjectPath);
			return true;
		}
		if (!bTuple && !bList && !bElementKey && !bTrait && !IsConstant(Expr, Property))
		{
			OutParts.Add({ SubPath, &Expr });
			return true;
		}

		if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
		{
			const FDabLiteralExpr* Literal = Expr.As<FDabLiteralExpr>();
			if (!Literal || Literal->LiteralKind != EDabLiteralKind::Bool)
			{
				OutError = Where + TEXT(" takes true or false");
				return false;
			}
			Bool->SetPropertyValue(Value, Literal->bBool);
			return true;
		}

		const UEnum* Enum = nullptr;
		const FNumericProperty* Numeric = nullptr;
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property))
		{
			Enum = EnumProperty->GetEnum();
			Numeric = EnumProperty->GetUnderlyingProperty();
		}
		else if ((Numeric = CastField<FNumericProperty>(&Property)) != nullptr)
		{
			Enum = Numeric->GetIntPropertyEnum();
		}
		if (Numeric)
		{
			if (Enum)
			{
				FString ValueName;
				if (const FDabMemberExpr* Member = Expr.As<FDabMemberExpr>())
				{
					ValueName = Member->Member;
				}
				else if (const FDabNameExpr* Name = Expr.As<FDabNameExpr>())
				{
					ValueName = Name->Name;
				}
				if (!ValueName.IsEmpty())
				{
					const int64 EnumValue = Enum->GetValueByNameString(ValueName);
					if (EnumValue == INDEX_NONE)
					{
						OutError = FString::Printf(TEXT("%s: %s has no value %s"), *Where, *Enum->GetName(), *ValueName);
						return false;
					}
					Numeric->SetIntPropertyValue(Value, EnumValue);
					return true;
				}
			}
			double Number = 0.0;
			if (!TryGetNumber(Expr, Number))
			{
				OutError = Where + (Enum ? FString::Printf(TEXT(" takes a value of %s"), *Enum->GetName()) : FString(TEXT(" takes a number")));
				return false;
			}
			if (Numeric->IsFloatingPoint())
			{
				Numeric->SetFloatingPointPropertyValue(Value, Number);
				return true;
			}
			int64 Integer = 0;
			if (!TryGetDabInteger(Expr, Integer))
			{
				OutError = Where + TEXT(" takes a whole number");
				return false;
			}
			Numeric->SetIntPropertyValue(Value, Integer);
			return true;
		}

		if (CastField<FNameProperty>(&Property) || CastField<FStrProperty>(&Property) || CastField<FTextProperty>(&Property))
		{
			FString Text;
			if (!TryGetNameText(Expr, Text))
			{
				OutError = Where + TEXT(" takes a string");
				return false;
			}
			if (const FNameProperty* Name = CastField<FNameProperty>(&Property))
			{
				const bool bNone = Expr.Is<FDabNameExpr>() && Text == TEXT("None");
				Name->SetPropertyValue(Value, bNone ? NAME_None : FName(*Text));
			}
			else if (const FStrProperty* String = CastField<FStrProperty>(&Property))
			{
				String->SetPropertyValue(Value, Text);
			}
			else
			{
				CastField<FTextProperty>(&Property)->SetPropertyValue(Value, FText::FromString(Text));
			}
			return true;
		}

		if (StructProperty)
		{
			const UScriptStruct& Struct = *StructProperty->Struct;
			if (bElementKey)
			{
				const FDabCallExpr& Call = *Expr.As<FDabCallExpr>();
				const FDabNameExpr* TypeName = Call.Callee ? Call.Callee->As<FDabNameExpr>() : nullptr;
				const int64 Type = TypeName ? FindElementType(TypeName->Name) : INDEX_NONE;
				if (Type == INDEX_NONE || Call.Args.Num() != 1 || Call.Args[0]->IsNamed() || !Call.Args[0]->Value)
				{
					OutError = Where + TEXT(" takes an element: Bone(name), Control(name), Null(name), Curve(name), Connector(name), Socket(name)");
					return false;
				}
				FRigElementKey& Key = *static_cast<FRigElementKey*>(Value);
				Key.Type = ERigElementType(Type);
				const FDabExpr& NameArg = StripParens(*Call.Args[0]->Value);
				FString ElementName;
				// A name or a string is the element's name. Anything else (`Bone(Prefix + "_l")`) is the Name pin's link.
				if (!TryGetNameText(NameArg, ElementName))
				{
					OutParts.Add({ JoinPath(SubPath, TEXT("Name")), &NameArg });
					Key.Name = NAME_None;
					return true;
				}
				Key.Name = ElementName == TEXT("None") && NameArg.Is<FDabNameExpr>() ? FName(NAME_None) : FName(*ElementName);
				return true;
			}
			const FDabTupleExpr* Tuple = Expr.As<FDabTupleExpr>();
			if (bTrait && TraitCall->Args.Num() == 1 && !TraitCall->Args[0]->IsNamed() && TraitCall->Args[0]->Value)
			{
				// The whole trait from elsewhere.
				OutParts.Add({ SubPath, TraitCall->Args[0]->Value.Get() });
				return true;
			}
			if (!Tuple && !bTrait)
			{
				OutError = FString::Printf(TEXT("%s takes a %s: write a tuple, (A = 1, B = 2)"), *Where, *Struct.GetName());
				return false;
			}
			const TArray<FDabArgumentPtr>& Items = bTrait ? TraitCall->Args : Tuple->Items;
			if (Items.IsEmpty())
			{
				Struct.ClearScriptStruct(Value);
				return true;
			}
			const bool bPositional = !Items[0]->IsNamed();
			TArray<const FProperty*> Members;
			for (TFieldIterator<FProperty> It(&Struct); It; ++It)
			{
				Members.Add(*It);
			}
			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				const FDabArgument& Item = *Items[Index];
				if (Item.IsNamed() == bPositional)
				{
					OutError = Where + TEXT(": a tuple names every member or none");
					return false;
				}
				const FProperty* Member = bPositional ? (Members.IsValidIndex(Index) ? Members[Index] : nullptr) : FindDcrStructMember(Struct, Item.Name);
				if (!Member)
				{
					OutError = bPositional
						? FString::Printf(TEXT("%s: %s has %d members"), *Where, *Struct.GetName(), Members.Num())
						: FString::Printf(TEXT("%s: %s has no member %s"), *Where, *Struct.GetName(), *Item.Name);
					return false;
				}
				if (!WriteDcrValueExpr(*Member, Member->ContainerPtrToValuePtr<void>(Value), *Item.Value, IsConstant, ResolveAsset, OutParts, OutError,
					JoinPath(SubPath, Member->GetName())))
				{
					return false;
				}
			}
			return true;
		}

		if (ArrayProperty)
		{
			const FDabListExpr* List = Expr.As<FDabListExpr>();
			if (!List)
			{
				OutError = Where + TEXT(" takes a list, [a, b]");
				return false;
			}
			FScriptArrayHelper Helper(ArrayProperty, Value);
			Helper.EmptyAndAddValues(List->Items.Num());
			for (int32 Index = 0; Index < List->Items.Num(); ++Index)
			{
				if (!WriteDcrValueExpr(*ArrayProperty->Inner, Helper.GetRawPtr(Index), *List->Items[Index], IsConstant, ResolveAsset, OutParts, OutError,
					JoinPath(SubPath, FString::FromInt(Index))))
				{
					return false;
				}
			}
			return true;
		}

		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
		{
			if (const FDabNameExpr* Name = Expr.As<FDabNameExpr>(); Name && Name->Name == TEXT("None"))
			{
				Object->SetObjectPropertyValue(Value, nullptr);
				return true;
			}
			UObject* Asset = ResolveAsset(Expr, Object->PropertyClass);
			if (!Asset)
			{
				OutError = Where + TEXT(": no such asset");
				return false;
			}
			if (const FSoftObjectProperty* Soft = CastField<FSoftObjectProperty>(&Property))
			{
				Soft->SetPropertyValue(Value, FSoftObjectPtr(Asset));
			}
			else
			{
				Object->SetObjectPropertyValue(Value, Asset);
			}
			return true;
		}

		OutError = FString::Printf(TEXT("%s: values of type %s cannot be written"), *Where, *Property.GetCPPType());
		return false;
	}

	// ------------------------------------------------------------------------------------------
	// Control values
	// ------------------------------------------------------------------------------------------

	namespace ValuesPrivate
	{
		/** A slot of the type a control of Type holds its value as in text. */
		bool InitControlSlot(ERigControlType Type, FDcrValueSlot& Slot)
		{
			switch (Type)
			{
			case ERigControlType::Bool: return Slot.Init(MakeDcrBuiltinType(TEXT("bool")));
			case ERigControlType::Float:
			case ERigControlType::ScaleFloat: return Slot.Init(MakeDcrBuiltinType(TEXT("float")));
			case ERigControlType::Integer: return Slot.Init(MakeDcrBuiltinType(TEXT("int32")));
			case ERigControlType::Vector2D: return Slot.Init(MakeDcrType(*TBaseStructure<FVector2D>::Get()));
			case ERigControlType::Position:
			case ERigControlType::Scale: return Slot.Init(MakeDcrType(*TBaseStructure<FVector>::Get()));
			case ERigControlType::Rotator: return Slot.Init(MakeDcrType(*TBaseStructure<FRotator>::Get()));
			case ERigControlType::Transform: return Slot.Init(MakeDcrType(*TBaseStructure<FTransform>::Get()));
			case ERigControlType::TransformNoScale: return Slot.Init(MakeDcrType(*FTransformNoScale::StaticStruct()));
			case ERigControlType::EulerTransform: return Slot.Init(MakeDcrType(*FEulerTransform::StaticStruct()));
			}
			return false;
		}

		/** The value of a control of Type, out of the float store into Slot (made with InitControlSlot). */
		void ReadControlValue(ERigControlType Type, const FRigControlValue& Value, void* Memory)
		{
			switch (Type)
			{
			case ERigControlType::Bool: *static_cast<bool*>(Memory) = Value.Get<bool>(); break;
			case ERigControlType::Float:
			case ERigControlType::ScaleFloat: *static_cast<float*>(Memory) = Value.Get<float>(); break;
			case ERigControlType::Integer: *static_cast<int32*>(Memory) = Value.Get<int32>(); break;
			case ERigControlType::Vector2D:
			{
				const FVector3f Stored = Value.Get<FVector3f>();
				*static_cast<FVector2D*>(Memory) = FVector2D(Stored.X, Stored.Y);
				break;
			}
			case ERigControlType::Position:
			case ERigControlType::Scale: *static_cast<FVector*>(Memory) = FVector(Value.Get<FVector3f>()); break;
			case ERigControlType::Rotator:
			{
				// Kept as Euler angles: X roll, Y pitch, Z yaw.
				const FVector3f Euler = Value.Get<FVector3f>();
				*static_cast<FRotator*>(Memory) = FRotator(Euler.Y, Euler.Z, Euler.X);
				break;
			}
			case ERigControlType::Transform: *static_cast<FTransform*>(Memory) = Value.Get<FRigControlValue::FTransform_Float>().ToTransform(); break;
			case ERigControlType::TransformNoScale: *static_cast<FTransformNoScale*>(Memory) = Value.Get<FRigControlValue::FTransformNoScale_Float>().ToTransform(); break;
			case ERigControlType::EulerTransform: *static_cast<FEulerTransform*>(Memory) = Value.Get<FRigControlValue::FEulerTransform_Float>().ToTransform(); break;
			}
		}

		void WriteControlValue(ERigControlType Type, const void* Memory, FRigControlValue& Value)
		{
			switch (Type)
			{
			case ERigControlType::Bool: Value.Set<bool>(*static_cast<const bool*>(Memory)); break;
			case ERigControlType::Float:
			case ERigControlType::ScaleFloat: Value.Set<float>(*static_cast<const float*>(Memory)); break;
			case ERigControlType::Integer: Value.Set<int32>(*static_cast<const int32*>(Memory)); break;
			case ERigControlType::Vector2D:
			{
				const FVector2D& Vector = *static_cast<const FVector2D*>(Memory);
				Value.Set<FVector3f>(FVector3f(Vector.X, Vector.Y, 0.f));
				break;
			}
			case ERigControlType::Position:
			case ERigControlType::Scale: Value.Set<FVector3f>(FVector3f(*static_cast<const FVector*>(Memory))); break;
			case ERigControlType::Rotator:
			{
				const FRotator& Rotator = *static_cast<const FRotator*>(Memory);
				Value.Set<FVector3f>(FVector3f(Rotator.Roll, Rotator.Pitch, Rotator.Yaw));
				break;
			}
			case ERigControlType::Transform: Value.Set<FRigControlValue::FTransform_Float>(*static_cast<const FTransform*>(Memory)); break;
			case ERigControlType::TransformNoScale: Value.Set<FRigControlValue::FTransformNoScale_Float>(*static_cast<const FTransformNoScale*>(Memory)); break;
			case ERigControlType::EulerTransform: Value.Set<FRigControlValue::FEulerTransform_Float>(*static_cast<const FEulerTransform*>(Memory)); break;
			}
		}
	}

	FDabExprPtr MakeDcrControlValueExpr(ERigControlType Type, const FRigControlValue& Value)
	{
		using namespace ValuesPrivate;
		FDcrValueSlot Slot;
		if (!InitControlSlot(Type, Slot))
		{
			return nullptr;
		}
		ReadControlValue(Type, Value, Slot.GetMemory());
		return MakeDcrValueExpr(*Slot.GetProperty(), Slot.GetMemory(), nullptr);
	}

	bool WriteDcrControlValue(ERigControlType Type, const FDabExpr& Expr, FRigControlValue& OutValue, FString& OutError)
	{
		using namespace ValuesPrivate;
		FDcrValueSlot Slot;
		if (!InitControlSlot(Type, Slot))
		{
			OutError = TEXT("the control's type has no value the text can write");
			return false;
		}
		TArray<FDcrValuePart> Parts;
		const bool bWritten = WriteDcrValueExpr(*Slot.GetProperty(), Slot.GetMemory(), Expr,
			[](const FDabExpr& Part, const FProperty&) { return IsDcrConstantLiteral(Part) || Part.Is<FDabNameExpr>(); },
			[](const FDabExpr&, const UClass*) -> UObject* { return nullptr; }, Parts, OutError);
		if (!bWritten)
		{
			return false;
		}
		if (!Parts.IsEmpty())
		{
			OutError = TEXT("a control's initial value takes constants only");
			return false;
		}
		OutValue = FRigControlValue();
		WriteControlValue(Type, Slot.GetMemory(), OutValue);
		return true;
	}

	bool IsDcrControlValueIdentity(ERigControlType Type, const FRigControlValue& Value)
	{
		using namespace ValuesPrivate;
		FDcrValueSlot Slot;
		FDcrValueSlot Identity;
		if (!InitControlSlot(Type, Slot) || !InitControlSlot(Type, Identity))
		{
			return false;
		}
		ReadControlValue(Type, Value, Slot.GetMemory());
		switch (Type)
		{
		case ERigControlType::ScaleFloat: *static_cast<float*>(Identity.GetMemory()) = 1.f; break;
		case ERigControlType::Scale: *static_cast<FVector*>(Identity.GetMemory()) = FVector::OneVector; break;
		case ERigControlType::Transform: *static_cast<FTransform*>(Identity.GetMemory()) = FTransform::Identity; break;
		case ERigControlType::TransformNoScale: *static_cast<FTransformNoScale*>(Identity.GetMemory()) = FTransformNoScale::Identity; break;
		case ERigControlType::EulerTransform: *static_cast<FEulerTransform*>(Identity.GetMemory()) = FEulerTransform::Identity; break;
		default: break;
		}
		// The engine works a control's initial value out again from its transforms: rounding is no value.
		return AreDcrValuesNear(*Slot.GetProperty(), Slot.GetMemory(), Identity.GetMemory(), DcrRoundingTolerance);
	}
}
