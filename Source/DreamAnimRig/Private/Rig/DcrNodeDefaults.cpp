// Copyright DreamMoon. All Rights Reserved.

#include "DcrNodeDefaults.h"

#include "Rig/DcrValues.h"
#include "RigVMModel/Nodes/RigVMAggregateNode.h"
#include "RigVMModel/Nodes/RigVMDispatchNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "UObject/Class.h"
#include "UObject/StructOnScope.h"

namespace UE::DreamAnim::Rig
{
	using namespace Model;

	FString FDcrNodeDefaults::GetTypeDefault(const FString& CPPType, const UObject* TypeObject)
	{
		if (CPPType.StartsWith(TEXT("TArray<")))
		{
			return TEXT("()");
		}
		if (CPPType == TEXT("bool"))
		{
			return TEXT("False");
		}
		if (CPPType == TEXT("int32"))
		{
			return TEXT("0");
		}
		if (CPPType == TEXT("float") || CPPType == TEXT("double"))
		{
			return TEXT("0.000000");
		}
		if (CPPType == TEXT("FName"))
		{
			return TEXT("None");
		}
		if (CPPType == TEXT("FString"))
		{
			return FString();
		}
		if (const UEnum* Enum = Cast<UEnum>(TypeObject))
		{
			return Enum->GetNameStringByIndex(0);
		}
		if (const UScriptStruct* Struct = Cast<UScriptStruct>(TypeObject))
		{
			FStructOnScope Instance(Struct);
			FString Text;
			Struct->ExportText(Text, Instance.GetStructMemory(), nullptr, nullptr, PPF_None, nullptr, true);
			return Text;
		}
		return FString();
	}

	bool FDcrNodeDefaults::IsTypeDefault(const FString& CPPType, const UObject* TypeObject, const FString& Value)
	{
		if (Value.IsEmpty())
		{
			return true;
		}
		// Compared as values: the same default has many spellings (`(Type=None,Name="")`, `()`, `0.000000`).
		Model::FDcrType Type;
		Type.CPPType = CPPType;
		Type.ObjectPath = TypeObject ? TypeObject->GetPathName() : FString();
		FDcrValueSlot Read;
		FDcrValueSlot Fresh;
		if (Read.Init(Type) && Fresh.Init(Type))
		{
			return Read.ImportText(Value) && AreDcrValuesSame(*Read.GetProperty(), Read.GetMemory(), Fresh.GetMemory());
		}
		return Value == GetTypeDefault(CPPType, TypeObject);
	}

	bool FDcrNodeDefaults::IsFreshValue(URigVMNode* Node, URigVMPin* RootPin)
	{
		if (!Node || !RootPin)
		{
			return true;
		}
		if (Node->IsA<URigVMUnitNode>() || Node->IsA<URigVMDispatchNode>() || Node->IsA<URigVMFunctionReferenceNode>() || Node->IsA<URigVMAggregateNode>())
		{
			if (RootPin->HasOriginalDefaultValue())
			{
				return true;
			}
			// The engine compares the texts; an old asset may spell the same value another way (`False` / `false`,
			// fewer decimals). Compared as values of the pin's type.
			Model::FDcrType Type;
			Type.CPPType = RootPin->GetCPPType();
			Type.ObjectPath = RootPin->GetCPPTypeObject() ? RootPin->GetCPPTypeObject()->GetPathName() : FString();
			FDcrValueSlot Value;
			FDcrValueSlot Original;
			return Value.Init(Type) && Original.Init(Type) && Value.ImportText(RootPin->GetDefaultValue()) && Original.ImportText(RootPin->GetOriginalDefaultValue())
				&& Value.GetProperty()->Identical(Value.GetMemory(), Original.GetMemory());
		}
		return IsTypeDefault(RootPin->GetCPPType(), RootPin->GetCPPTypeObject(), RootPin->GetDefaultValue());
	}

	void FDcrNodeDefaults::CollectValues(URigVMNode* Node, const TSet<FName>& AlwaysPins, FDcrModelNode& Out)
	{
		if (!Node)
		{
			return;
		}
		for (URigVMPin* Pin : Node->GetPins())
		{
			if (!Pin || Pin->IsExecuteContext())
			{
				continue;
			}
			const ERigVMPinDirection Direction = Pin->GetDirection();
			if (Direction != ERigVMPinDirection::Input && Direction != ERigVMPinDirection::IO)
			{
				continue;
			}
			// An unresolved wildcard has no value to keep.
			if (!Pin->CanProvideDefaultValue())
			{
				continue;
			}
			if (!AlwaysPins.Contains(Pin->GetFName()) && IsFreshValue(Node, Pin))
			{
				continue;
			}
			FDcrValue& Value = Out.Values.AddDefaulted_GetRef();
			Value.Name = Pin->GetName();
			Value.Value = Pin->GetDefaultValue();
		}
	}
}
