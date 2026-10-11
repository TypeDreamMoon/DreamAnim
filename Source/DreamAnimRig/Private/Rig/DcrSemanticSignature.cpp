// Copyright DreamMoon. All Rights Reserved.
//
// What nodes look like to the semantic pass: the pins of a unit or a dispatch, their types and defaults, the
// execute pins, how a template resolves, and the small type helpers around them.

#include "DcrSemanticInternal.h"

#include "RigVMCore/RigVMDispatchFactory.h"
#include "RigVMCore/RigVMExecuteContext.h"
#include "RigVMCore/RigVMFunction.h"
#include "RigVMCore/RigVMRegistry.h"
#include "RigVMCore/RigVMStruct.h"
#include "RigVMCore/RigVMTemplate.h"
#include "RigVMTypeUtils.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Rig::Sem
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	// ------------------------------------------------------------------------------------------
	// Small members
	// ------------------------------------------------------------------------------------------

	const FPin* FSignature::Find(const FString& Name) const
	{
		return Pins.FindByPredicate([&Name](const FPin& Pin) { return Pin.Name == Name; });
	}

	FPin* FSignature::Find(const FString& Name)
	{
		return Pins.FindByPredicate([&Name](const FPin& Pin) { return Pin.Name == Name; });
	}

	int32 FSignature::NumInputs() const
	{
		int32 Count = 0;
		for (const FPin& Pin : Pins)
		{
			Count += Pin.bInput ? 1 : 0;
		}
		return Count;
	}

	int32 FSignature::NumOutputs() const
	{
		int32 Count = 0;
		for (const FPin& Pin : Pins)
		{
			Count += Pin.bOutput ? 1 : 0;
		}
		return Count;
	}

	const FPin* FSignature::GetInput(int32 Index) const
	{
		int32 Seen = 0;
		for (const FPin& Pin : Pins)
		{
			if (!Pin.bInput)
			{
				continue;
			}
			if (Seen == Index)
			{
				return &Pin;
			}
			++Seen;
		}
		return nullptr;
	}

	FString FVal::GetPath() const
	{
		FString Path = Rec ? Rec->Node.Name : FString();
		if (!Pin.IsEmpty())
		{
			Path += TEXT(".") + Pin;
		}
		if (!Sub.IsEmpty())
		{
			Path += TEXT(".") + Sub;
		}
		return Path;
	}

	// ------------------------------------------------------------------------------------------
	// Units
	// ------------------------------------------------------------------------------------------

	namespace
	{
		void AddExecutePin(FSignature& Sig, const FString& Name, ERigVMPinDirection Direction)
		{
			if (Direction == ERigVMPinDirection::Input || Direction == ERigVMPinDirection::IO)
			{
				Sig.ExecIn.AddUnique(Name);
			}
			if (Direction == ERigVMPinDirection::Output || Direction == ERigVMPinDirection::IO)
			{
				Sig.ExecOut.AddUnique(Name);
			}
		}
	}

	FSignature MakeUnitSignature(const UScriptStruct& Struct)
	{
		FSignature Sig;
		FStructOnScope Defaults(&Struct);
		for (const FDcrPin& Source : GetDcrNodePins(Struct))
		{
			FPin& Pin = Sig.Pins.AddDefaulted_GetRef();
			Pin.Name = Source.Name;
			Pin.bInput = Source.bInput;
			Pin.bOutput = Source.bOutput;
			Pin.Property = Source.Property;
			Pin.Type = Source.Type;
			if (Source.Property)
			{
				Pin.bAggregate = Source.Property->HasMetaData(FRigVMStruct::AggregateMetaName);
				if (Source.bInput)
				{
					Source.Property->ExportTextItem_Direct(Pin.Default, Source.Property->ContainerPtrToValuePtr<void>(Defaults.GetStructMemory()), nullptr, nullptr, PPF_None);
				}
			}
		}
		// Execute pins by their members, which the names of RigVM's own units and Control Rig's differ in.
		for (TFieldIterator<FProperty> It(&Struct); It; ++It)
		{
			FProperty* Property = *It;
			if (!IsDcrExecuteProperty(*Property))
			{
				continue;
			}
			const ERigVMPinDirection Direction = FRigVMStruct::GetPinDirectionFromProperty(Property);
			if (Direction != ERigVMPinDirection::Hidden)
			{
				AddExecutePin(Sig, Property->GetName(), Direction);
			}
		}
		return Sig;
	}

	FString GetUnitNotation(const UScriptStruct& Struct)
	{
		const FRigVMFunction* Function = FRigVMRegistry::Get().FindFunction(const_cast<UScriptStruct*>(&Struct), TEXT("Execute"));
		const FRigVMTemplate* Template = Function ? Function->GetTemplate() : nullptr;
		return Template ? Template->GetNotation().ToString() : FString();
	}

	const UScriptStruct* FindUnitByFullName(const TCHAR* FullName)
	{
		const TArray<FDcrNodeType> Found = FindDcrUnits(FullName);
		return Found.Num() == 1 ? Found[0].Struct : nullptr;
	}

	const UScriptStruct* FindDispatchByFullName(const TCHAR* FullName)
	{
		const TArray<FDcrNodeType> Found = FindDcrDispatches(FullName);
		return Found.Num() == 1 ? Found[0].Struct : nullptr;
	}

	FString GetUnitAggregateSecond(const UScriptStruct& Struct)
	{
		TArray<FString> Inputs;
		int32 Outputs = 0;
		for (TFieldIterator<FProperty> It(&Struct); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property->HasMetaData(FRigVMStruct::AggregateMetaName))
			{
				continue;
			}
			const ERigVMPinDirection Direction = FRigVMStruct::GetPinDirectionFromProperty(Property);
			if (Direction == ERigVMPinDirection::Input)
			{
				Inputs.Add(Property->GetName());
			}
			else if (Direction == ERigVMPinDirection::Output)
			{
				++Outputs;
			}
		}
		return Inputs.Num() == 2 && Outputs == 1 ? Inputs[1] : FString();
	}

	FString GetNextAggregateName(const FString& Last)
	{
		if (Last.IsEmpty())
		{
			return Last;
		}
		if (Last.IsNumeric())
		{
			return FString::FromInt(FCString::Atoi(*Last) + 1);
		}
		if (Last.Len() == 1)
		{
			const TCHAR Char = Last[0];
			if ((Char >= TEXT('a') && Char < TEXT('z')) || (Char >= TEXT('A') && Char < TEXT('Z')))
			{
				return FString::Chr(Char + 1);
			}
		}
		int32 Underscore = INDEX_NONE;
		if (Last.FindLastChar(TEXT('_'), Underscore))
		{
			return Last.Left(Underscore) + TEXT("_") + GetNextAggregateName(Last.Mid(Underscore + 1));
		}
		return Last + TEXT("_1");
	}

	// ------------------------------------------------------------------------------------------
	// Dispatches
	// ------------------------------------------------------------------------------------------

	bool GetDispatchInfo(const UScriptStruct& Struct, FDispatchInfo& Out)
	{
		if (!Struct.IsChildOf(FRigVMDispatchFactory::StaticStruct()))
		{
			return false;
		}
		const FRigVMDispatchFactory* Factory = FRigVMRegistry::Get().FindDispatchFactory(FRigVMDispatchFactory::GetFactoryName(&Struct));
		const FRigVMTemplate* Template = Factory ? Factory->GetTemplate() : nullptr;
		if (!Template)
		{
			return false;
		}
		Out.Struct = &Struct;
		Out.Factory = Factory;
		Out.Template = Template;
		return true;
	}

	FSignature MakeDispatchSignature(const UScriptStruct& Struct)
	{
		FSignature Sig;
		FDispatchInfo Info;
		if (!GetDispatchInfo(Struct, Info))
		{
			return Sig;
		}
		const TArray<FName> AggregateInputs = Info.Factory->GetAggregateInputArguments();
		for (const FDcrPin& Source : GetDcrNodePins(Struct))
		{
			FPin& Pin = Sig.Pins.AddDefaulted_GetRef();
			Pin.Name = Source.Name;
			Pin.bInput = Source.bInput;
			Pin.bOutput = Source.bOutput;
			Pin.Type = Source.Type;
			Pin.bWildcard = !Source.Type.IsSet();
			Pin.bAggregate = AggregateInputs.Contains(FName(*Source.Name));
			if (Source.bInput && Source.Type.IsSet())
			{
				const int32 TypeIndex = GetTypeIndexOf(Source.Type);
				if (TypeIndex != INDEX_NONE)
				{
					Pin.Default = Info.Factory->GetArgumentDefaultValue(FName(*Source.Name), TypeIndex);
				}
			}
		}
		// A dispatch's execute pins are not among its template's arguments: the template lists them apart.
		for (const FRigVMExecuteArgument& Argument : Info.Factory->GetExecuteArguments_NoLock(FRigVMDispatchContext(), FRigVMRegistry::Get().GetHandle_NoLock()))
		{
			if (Argument.Direction != ERigVMPinDirection::Hidden)
			{
				AddExecutePin(Sig, Argument.Name.ToString(), Argument.Direction);
			}
		}
		return Sig;
	}

	EResolveResult ResolveTemplateTypes(const FRigVMTemplate& Template, const TMap<FString, FDcrType>& Known, TMap<FString, FDcrType>& OutTypes)
	{
		OutTypes.Reset();
		FRigVMTemplateTypeMap Map;
		for (const TPair<FString, FDcrType>& Pair : Known)
		{
			const int32 TypeIndex = GetTypeIndexOf(Pair.Value);
			if (TypeIndex == INDEX_NONE)
			{
				return EResolveResult::Mismatch;
			}
			Map.Add(FName(*Pair.Key), TypeIndex);
		}
		// The exact types first: with float and double taken as one, a double array would also fit the float permutations.
		TArray<int32> Permutations;
		const FRigVMTemplateTypeMap Given = Map;
		bool bCast = false;
		if (!Template.Resolve(Map, Permutations, false) || Permutations.IsEmpty())
		{
			Map = Given;
			Permutations.Reset();
			if (!Template.Resolve(Map, Permutations, true) || Permutations.IsEmpty())
			{
				return EResolveResult::Mismatch;
			}
			// Float and double mixed: the permutations that take the most of the known arguments as they are, and of
			// those, the one the first known argument (in the node's order) fits as it is.
			bCast = true;
			TArray<int32> Best;
			int32 BestScore = -1;
			for (const int32 Permutation : Permutations)
			{
				int32 Score = 0;
				for (const TPair<FName, TRigVMTypeIndex>& Pair : Given)
				{
					const FRigVMTemplateArgument* Argument = Template.FindArgument(Pair.Key);
					Score += Argument && Argument->GetTypeIndex(Permutation) == Pair.Value ? 1 : 0;
				}
				if (Score > BestScore)
				{
					BestScore = Score;
					Best.Reset();
				}
				if (Score == BestScore)
				{
					Best.Add(Permutation);
				}
			}
			Permutations = MoveTemp(Best);
			for (int32 Index = 0; Index < Template.NumArguments() && Permutations.Num() > 1; ++Index)
			{
				const FRigVMTemplateArgument* Argument = Template.GetArgument(Index);
				const TRigVMTypeIndex* Type = Argument ? Given.Find(Argument->GetName()) : nullptr;
				if (!Type)
				{
					continue;
				}
				const TArray<int32> Fitting = Permutations.FilterByPredicate([Argument, Type](int32 Permutation) { return Argument->GetTypeIndex(Permutation) == *Type; });
				if (!Fitting.IsEmpty())
				{
					Permutations = Fitting;
				}
				break;
			}
		}
		// Every argument: what the known ones settled it to, else the one type all the permutations left agree on.
		const FRigVMRegistry& Registry = FRigVMRegistry::Get();
		bool bOpen = false;
		for (int32 Index = 0; Index < Template.NumArguments(); ++Index)
		{
			const FRigVMTemplateArgument* Argument = Template.GetArgument(Index);
			if (!Argument)
			{
				continue;
			}
			const TRigVMTypeIndex* Settled = bCast ? nullptr : Map.Find(Argument->GetName());
			if (Settled && !Registry.IsWildCardType(*Settled))
			{
				OutTypes.Add(Argument->GetName().ToString(), GetTypeFromIndex(*Settled));
				continue;
			}
			TRigVMTypeIndex Common = INDEX_NONE;
			bool bAgree = true;
			for (const int32 Permutation : Permutations)
			{
				const TRigVMTypeIndex Type = Argument->GetTypeIndex(Permutation);
				if (Common == INDEX_NONE)
				{
					Common = Type;
				}
				else if (Common != Type)
				{
					bAgree = false;
					break;
				}
			}
			if (bAgree && Common != INDEX_NONE && !Registry.IsWildCardType(Common))
			{
				OutTypes.Add(Argument->GetName().ToString(), GetTypeFromIndex(Common));
			}
			else
			{
				bOpen = true;
			}
		}
		return bOpen ? EResolveResult::Open : EResolveResult::Resolved;
	}

	// ------------------------------------------------------------------------------------------
	// Types
	// ------------------------------------------------------------------------------------------

	int32 GetTypeIndexOf(const FDcrType& Type)
	{
		if (!Type.IsSet())
		{
			return INDEX_NONE;
		}
		UObject* TypeObject = Type.ObjectPath.IsEmpty() ? nullptr : RigVMTypeUtils::FindObjectFromCPPTypeObjectPath(Type.ObjectPath);
		return FRigVMRegistry::Get().GetTypeIndex(FName(*Type.CPPType), TypeObject);
	}

	FDcrType GetTypeFromIndex(int32 TypeIndex)
	{
		FDcrType Type;
		const FRigVMTemplateArgumentType& ArgumentType = FRigVMRegistry::Get().GetType(TypeIndex);
		Type.CPPType = ArgumentType.CPPType.ToString();
		if (ArgumentType.CPPTypeObject)
		{
			Type.ObjectPath = ArgumentType.CPPTypeObject->GetPathName();
		}
		return Type;
	}

	bool IsArrayType(const FDcrType& Type)
	{
		return RigVMTypeUtils::IsArrayType(Type.CPPType);
	}

	bool AreTypesCompatible(const FDcrType& From, const FDcrType& To)
	{
		if (!From.IsSet() || !To.IsSet() || From == To)
		{
			return true;
		}
		const bool bFromArray = RigVMTypeUtils::IsArrayType(From.CPPType);
		if (bFromArray == RigVMTypeUtils::IsArrayType(To.CPPType))
		{
			const FString FromBase = bFromArray ? RigVMTypeUtils::BaseTypeFromArrayType(From.CPPType) : From.CPPType;
			const FString ToBase = bFromArray ? RigVMTypeUtils::BaseTypeFromArrayType(To.CPPType) : To.CPPType;
			if ((FromBase == TEXT("float") && ToBase == TEXT("double")) || (FromBase == TEXT("double") && ToBase == TEXT("float")))
			{
				return true;
			}
		}
		const int32 FromIndex = GetTypeIndexOf(From);
		const int32 ToIndex = GetTypeIndexOf(To);
		return FromIndex != INDEX_NONE && ToIndex != INDEX_NONE && FRigVMRegistry::Get().CanMatchTypes(FromIndex, ToIndex, true);
	}

	bool GetTypeAtSubPath(const FDcrType& Type, const FString& Path, FDcrType& OutType, FString& OutEnginePath)
	{
		OutType = Type;
		OutEnginePath.Reset();
		if (Path.IsEmpty())
		{
			return true;
		}
		FDcrValueSlot Slot;
		if (!Slot.Init(Type))
		{
			return false;
		}
		const FProperty* Property = Slot.GetProperty();
		TArray<FString> Segments;
		Path.ParseIntoArray(Segments, TEXT("."));
		for (const FString& Segment : Segments)
		{
			if (!Property)
			{
				return false;
			}
			if (const FArrayProperty* Array = CastField<FArrayProperty>(Property); Array && Segment.IsNumeric())
			{
				Property = Array->Inner;
				OutEnginePath += (OutEnginePath.IsEmpty() ? TEXT("") : TEXT(".")) + Segment;
				continue;
			}
			const FStructProperty* Struct = CastField<FStructProperty>(Property);
			const FProperty* Member = Struct ? FindDcrStructMember(*Struct->Struct, Segment) : nullptr;
			if (!Member)
			{
				return false;
			}
			OutEnginePath += (OutEnginePath.IsEmpty() ? TEXT("") : TEXT(".")) + Member->GetName();
			Property = Member;
		}
		OutType = GetDcrType(*Property);
		return true;
	}

	const TCHAR* const DefaultExecuteName = TEXT("ExecuteContext");

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

	FString MakeNodeBaseName(const FString& Name)
	{
		FString Result;
		for (const TCHAR Char : Name)
		{
			Result.AppendChar(FChar::IsAlnum(Char) || Char == TEXT('_') ? Char : TEXT('_'));
		}
		if (Result.IsEmpty() || FChar::IsDigit(Result[0]))
		{
			Result = TEXT("Node") + Result;
		}
		return Result;
	}

	const TCHAR* ClassifyValueError(const FString& Error)
	{
		if (Error.Contains(TEXT("has no member")) || Error.Contains(TEXT(" members")))
		{
			return TEXT("DAB9011");
		}
		if (Error.Contains(TEXT("has no value")) || Error.Contains(TEXT("no such asset")))
		{
			return TEXT("DAB9001");
		}
		return TEXT("DAB9004");
	}
}
