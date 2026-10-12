// Copyright DreamMoon. All Rights Reserved.
//
// Values and links: what an argument becomes on the pin it goes to. A constant is written into the pin's
// value through reflection; an expression is evaluated and linked; a literal that holds both is split, the
// constants into the value and the expressions into links to its sub-pins.

#include "DcrSemanticInternal.h"

#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Rig::Sem
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	namespace
	{
		/** The property and memory at a path of engine member names below a value; false when the path leaves the value. */
		bool FindSubMemory(const FProperty* Root, void* Memory, const FString& Sub, const FProperty*& OutProperty, void*& OutMemory)
		{
			OutProperty = Root;
			OutMemory = Memory;
			if (Sub.IsEmpty())
			{
				return true;
			}
			TArray<FString> Segments;
			Sub.ParseIntoArray(Segments, TEXT("."));
			for (const FString& Segment : Segments)
			{
				if (const FArrayProperty* Array = CastField<FArrayProperty>(OutProperty); Array && Segment.IsNumeric())
				{
					FScriptArrayHelper Helper(Array, OutMemory);
					const int32 Index = FCString::Atoi(*Segment);
					if (!Helper.IsValidIndex(Index))
					{
						return false;
					}
					OutMemory = Helper.GetRawPtr(Index);
					OutProperty = Array->Inner;
				}
				else if (const FStructProperty* Struct = CastField<FStructProperty>(OutProperty))
				{
					const FProperty* Member = Struct->Struct->FindPropertyByName(FName(*Segment));
					if (!Member)
					{
						return false;
					}
					OutMemory = Member->ContainerPtrToValuePtr<void>(OutMemory);
					OutProperty = Member;
				}
				else
				{
					return false;
				}
			}
			return true;
		}

		FString DescribeType(const FDcrType& Type)
		{
			if (!Type.IsSet())
			{
				return TEXT("an unknown type");
			}
			const FString Name = GetDcrTypeName(Type);
			return Name.IsEmpty() ? Type.CPPType : Name;
		}
	}

	bool FBody::IsConstantLeaf(const FDabExpr& Expr, const FProperty& Property) const
	{
		if (IsDcrConstantLiteral(Expr))
		{
			return true;
		}
		const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property);
		const bool bEnum = CastField<FEnumProperty>(&Property) != nullptr || (Numeric && Numeric->GetIntPropertyEnum() != nullptr);
		const bool bObject = CastField<FObjectPropertyBase>(&Property) != nullptr;
		const FStructProperty* StructProperty = CastField<FStructProperty>(&Property);
		const bool bSoftPath = StructProperty && StructProperty->Struct->IsChildOf(TBaseStructure<FSoftObjectPath>::Get());
		if (const FDabNameExpr* Name = Expr.As<FDabNameExpr>())
		{
			const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property);
			const UEnum* Enum = EnumProperty ? EnumProperty->GetEnum() : (Numeric ? Numeric->GetIntPropertyEnum() : nullptr);
			if (Enum && IsEnumValueHere(Name->Name, *Enum))
			{
				return true;
			}
			if (HasName(Name->Name))
			{
				return false;
			}
			return bEnum || bObject || bSoftPath || (Name->Name == TEXT("None") && CastField<FNameProperty>(&Property) != nullptr);
		}
		if (const FDabMemberExpr* Member = Expr.As<FDabMemberExpr>())
		{
			const FDabNameExpr* Object = Member->Object ? Member->Object->As<FDabNameExpr>() : nullptr;
			return bEnum && Object && !HasName(Object->Name);
		}
		if (const FDabCallExpr* Call = Expr.As<FDabCallExpr>())
		{
			const FDabNameExpr* Callee = Call->Callee ? Call->Callee->As<FDabNameExpr>() : nullptr;
			return bObject && Callee && Callee->Name == TEXT("Path");
		}
		return false;
	}

	UObject* FBody::ResolveAssetLeaf(const FDabExpr& Expr, const UClass* Expected)
	{
		bLastAssetAmbiguous = false;
		LastAssetProblem.Reset();
		const FString ObjectPath = G.ResolveAssetPath(Expr, Expected, LastAssetProblem, bLastAssetAmbiguous);
		return ObjectPath.IsEmpty() ? nullptr : FSoftObjectPath(ObjectPath).TryLoad();
	}

	bool FBody::FitsType(const FDabExpr& Expr, const FDcrType& Type)
	{
		FDcrValueSlot Slot;
		if (!Type.IsSet() || !Slot.Init(Type))
		{
			return false;
		}
		TArray<FDcrValuePart> Parts;
		FString Error;
		return WriteDcrValueExpr(*Slot.GetProperty(), Slot.GetMemory(), Expr,
			[this](const FDabExpr& Leaf, const FProperty& Property) { return IsConstantLeaf(Leaf, Property); },
			[](const FDabExpr&, const UClass*) -> UObject* { return nullptr; },
			Parts, Error);
	}

	void FBody::SetPinValue(FNodeRec& Rec, const FString& PinName, const FString& Text, const FDabSpan& Span)
	{
		if (FDcrValue* Existing = Rec.Node.Values.FindByPredicate([&PinName](const FDcrValue& Value) { return Value.Name == PinName; }))
		{
			Existing->Value = Text;
			Existing->Span = Span;
			return;
		}
		FDcrValue& Value = Rec.Node.Values.AddDefaulted_GetRef();
		Value.Name = PinName;
		Value.Value = Text;
		Value.Span = Span;
	}

	void FBody::LinkValue(const FVal& Value, const FString& TargetPath, const FDcrType& TargetType, const FDabSpan& Span)
	{
		FVal Source = Value;
		if (!SettlePending(Source, TargetType.IsSet() ? &TargetType : nullptr))
		{
			return;
		}
		if (Source.Pin.IsEmpty())
		{
			Sink.Error(TEXT("DAB9004"), Span, FString::Printf(TEXT("`%s` has several outputs; pick one, as `%s.Name`."), Source.Rec ? *Source.Rec->Node.Name : TEXT("the call"), Source.Rec ? *Source.Rec->Node.Name : TEXT("Call")));
			return;
		}
		if (!AreTypesCompatible(Source.Type, TargetType))
		{
			Sink.Error(TEXT("DAB9004"), Span, FString::Printf(TEXT("The value is %s, and %s takes %s."), *DescribeType(Source.Type), *TargetPath, *DescribeType(TargetType)));
			return;
		}
		AddLink(Source.GetPath(), TargetPath);
	}

	void FBody::ApplyArg(FNodeRec& Rec, const FPin& Pin, const FDabExpr& Expr)
	{
		const FVal Value = EvalOperand(Expr, Pin.Type.IsSet() ? &Pin.Type : nullptr, FString());
		ApplyValue(Rec, Pin, Value, Expr.Span);
	}

	void FBody::ApplyValue(FNodeRec& Rec, const FPin& Pin, FVal Value, const FDabSpan& Span)
	{
		switch (Value.Kind)
		{
		case FVal::EKind::Invalid:
			return;
		case FVal::EKind::Void:
			Sink.Error(TEXT("DAB9004"), Span, FString::Printf(TEXT("This gives no value, and %s takes one."), *Pin.Name));
			return;
		case FVal::EKind::Pin:
			LinkValue(Value, Rec.Node.Name + TEXT(".") + Pin.Name, Pin.Type, Span);
			return;
		case FVal::EKind::Literal:
			if (Value.Expr)
			{
				// `Pin = Injected(Node(...))`: a node on the pin, which is known now.
				const FDabCallExpr* Call = Value.Expr->As<FDabCallExpr>();
				const FDabNameExpr* Callee = Call && Call->Callee ? Call->Callee->As<FDabNameExpr>() : nullptr;
				if (Callee && Callee->Name == TEXT("Injected") && Call->Args.Num() == 1 && !G.FindFunction(Callee->Name))
				{
					BuildInjected(*Call, Rec, Pin);
					return;
				}
				ApplyLiteral(Rec, Pin, *Value.Expr);
			}
			return;
		}
	}

	void FBody::ApplyLiteral(FNodeRec& Rec, const FPin& Pin, const FDabExpr& Expr)
	{
		FDcrValueSlot Slot;
		FDcrValueSlot DefaultSlot;
		const bool bSlots = Pin.Property ? (Slot.Init(*Pin.Property) && DefaultSlot.Init(*Pin.Property))
			: (Pin.Type.IsSet() && Slot.Init(Pin.Type) && DefaultSlot.Init(Pin.Type));
		if (!bSlots)
		{
			Sink.Error(TEXT("DAB9004"), Expr.Span, FString::Printf(TEXT("The pin %s has a type the text cannot write a value of (%s)."), *Pin.Name, *DescribeType(Pin.Type)));
			return;
		}
		Slot.ImportText(Pin.Default);
		DefaultSlot.ImportText(Pin.Default);

		TArray<FDcrValuePart> Parts;
		FString Error;
		const bool bWritten = WriteDcrValueExpr(*Slot.GetProperty(), Slot.GetMemory(), Expr,
			[this](const FDabExpr& Leaf, const FProperty& Property) { return IsConstantLeaf(Leaf, Property); },
			[this](const FDabExpr& Leaf, const UClass* Expected) { return ResolveAssetLeaf(Leaf, Expected); },
			Parts, Error);
		if (!bWritten)
		{
			const bool bAssetProblem = !LastAssetProblem.IsEmpty() && Error.Contains(TEXT("no such asset"));
			Sink.Error(bAssetProblem && bLastAssetAmbiguous ? TEXT("DAB9002") : ClassifyValueError(Error), Expr.Span,
				bAssetProblem ? Error + TEXT(" ") + LastAssetProblem : Error);
			return;
		}

		bool bWholeLinked = false;
		TArray<FString> LinkedPaths;
		for (const FDcrValuePart& Part : Parts)
		{
			if (!Part.Expr)
			{
				continue;
			}
			FDcrType PartType;
			FString EnginePath;
			const FDcrType RootType = Pin.Type.IsSet() ? Pin.Type : GetDcrType(*Slot.GetProperty());
			GetTypeAtSubPath(RootType, Part.SubPath, PartType, EnginePath);

			const FVal Value = EvalOperand(*Part.Expr, PartType.IsSet() ? &PartType : nullptr, FString());
			const FString TargetPath = Rec.Node.Name + TEXT(".") + Pin.Name + (Part.SubPath.IsEmpty() ? FString() : TEXT(".") + Part.SubPath);
			switch (Value.Kind)
			{
			case FVal::EKind::Invalid:
				break;
			case FVal::EKind::Void:
				Sink.Error(TEXT("DAB9004"), Part.Expr->Span, TEXT("This gives no value."));
				break;
			case FVal::EKind::Pin:
				bWholeLinked |= Part.SubPath.IsEmpty();
				LinkedPaths.Add(Part.SubPath);
				LinkValue(Value, TargetPath, PartType, Part.Expr->Span);
				break;
			case FVal::EKind::Literal:
			{
				// A name nothing here defines, or a let that stands for a constant.
				const FDabExpr& Constant = StripParens(*Value.Expr);
				if (const FDabCallExpr* ConstantCall = Constant.As<FDabCallExpr>())
				{
					const FDabNameExpr* ConstantCallee = ConstantCall->Callee ? ConstantCall->Callee->As<FDabNameExpr>() : nullptr;
					if (ConstantCallee && ConstantCallee->Name == TEXT("Injected"))
					{
						Sink.Error(TEXT("DAB9004"), Part.Expr->Span, TEXT("`Injected(...)` goes on a whole input pin, not on a part of its value."));
						break;
					}
				}
				if (Constant.Is<FDabNameExpr>() || Constant.Is<FDabMemberExpr>())
				{
					const FDabNameExpr* Name = Constant.As<FDabNameExpr>();
					if (!Name || !HasName(Name->Name))
					{
						Sink.Error(TEXT("DAB9001"), Part.Expr->Span, FString::Printf(TEXT("`%s` is not a variable, a let, a parameter or a value of the type it is written for."),
							Name ? *Name->Name : TEXT("This name")));
						break;
					}
				}
				const FProperty* SubProperty = nullptr;
				void* SubMemory = nullptr;
				TArray<FDcrValuePart> SubParts;
				FString SubError;
				if (!FindSubMemory(Slot.GetProperty(), Slot.GetMemory(), Part.SubPath, SubProperty, SubMemory)
					|| !WriteDcrValueExpr(*SubProperty, SubMemory, Constant,
						[this](const FDabExpr& Leaf, const FProperty& Property) { return IsConstantLeaf(Leaf, Property); },
						[this](const FDabExpr& Leaf, const UClass* Expected) { return ResolveAssetLeaf(Leaf, Expected); },
						SubParts, SubError))
				{
					Sink.Error(ClassifyValueError(SubError), Part.Expr->Span, SubError.IsEmpty() ? FString(TEXT("The value does not fit here.")) : SubError);
				}
				break;
			}
			}
		}

		if (bWholeLinked)
		{
			return;
		}
		// Elements made only to take links are not part of the value: the links make them.
		TrimDcrLinkedElements(*Slot.GetProperty(), Slot.GetMemory(), DefaultSlot.GetMemory(), LinkedPaths);
		const FString Text = Slot.ExportText();
		if (Pin.bWildcard || Text != DefaultSlot.ExportText())
		{
			// A literal that says all of the value is where write-back changes it; one with links in it is not.
			SetPinValue(Rec, Pin.Name, Text, LinkedPaths.IsEmpty() ? Expr.Span : FDabSpan());
		}
	}

	void FBody::ApplyDefault(FNodeRec& Rec, const FPin& Pin)
	{
		FDcrValueSlot Slot;
		const bool bSlot = Pin.Property ? Slot.Init(*Pin.Property) : (Pin.Type.IsSet() && Slot.Init(Pin.Type));
		if (!bSlot)
		{
			return;
		}
		Slot.ImportText(Pin.Default);
		SetPinValue(Rec, Pin.Name, Slot.ExportText());
	}

	bool FBody::IsTypeDefaultText(const FDcrType& Type, const FString& Text)
	{
		FDcrValueSlot Value;
		FDcrValueSlot Default;
		if (!Value.Init(Type) || !Default.Init(Type) || !Value.ImportText(Text))
		{
			return false;
		}
		return Value.ExportText() == Default.ExportText();
	}
}
