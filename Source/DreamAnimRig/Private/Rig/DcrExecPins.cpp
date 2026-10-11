// Copyright DreamMoon. All Rights Reserved.

#include "DcrExecPins.h"

#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"

namespace UE::DreamAnim::Rig
{
	using namespace Model;

	void FillDcrExecutePins(URigVMNode* Node, FDcrModelNode& Out)
	{
		Out.ExecuteIn.Reset();
		Out.ExecuteOut.Reset();
		if (!Node)
		{
			return;
		}
		for (const URigVMPin* Pin : Node->GetPins())
		{
			if (!Pin || !Pin->IsExecuteContext())
			{
				continue;
			}
			switch (Pin->GetDirection())
			{
			case ERigVMPinDirection::Input:
				Out.ExecuteIn.Add(Pin->GetName());
				break;
			case ERigVMPinDirection::Output:
				Out.ExecuteOut.Add(Pin->GetName());
				break;
			case ERigVMPinDirection::IO:
				Out.ExecuteIn.Add(Pin->GetName());
				Out.ExecuteOut.Add(Pin->GetName());
				break;
			default:
				break;
			}
		}
	}

	void SplitDcrPinPath(const FString& Path, FString& OutNode, FString& OutRest)
	{
		int32 Dot = INDEX_NONE;
		if (Path.FindChar(TEXT('.'), Dot))
		{
			OutNode = Path.Left(Dot);
			OutRest = Path.Mid(Dot + 1);
		}
		else
		{
			OutNode = Path;
			OutRest.Reset();
		}
	}

	bool IsDcrSequence(const FDcrModelNode& Node)
	{
		return Node.Kind == EDcrNodeKind::Unit
			&& (Node.StructPath.EndsWith(TEXT("RigVMFunction_Sequence")) || Node.StructPath.EndsWith(TEXT("RigUnit_SequenceExecution")));
	}

	TArray<FString> GetDcrSequenceOutputs(const FDcrModelNode& Node)
	{
		TArray<FString> Outputs;
		for (const FString& Pin : Node.ExecuteOut)
		{
			if (!Node.ExecuteIn.Contains(Pin))
			{
				Outputs.Add(Pin);
			}
		}
		return Outputs;
	}

	FString GetDcrContinuation(const FDcrModelNode& Node)
	{
		const TArray<FString> Outputs = GetDcrSequenceOutputs(Node);
		if (IsDcrSequence(Node))
		{
			return Outputs.Num() > 0 ? Outputs.Last() : FString();
		}
		if (Node.ExecuteOut.Contains(TEXT("Completed")))
		{
			return TEXT("Completed");
		}
		for (const FString& Pin : Node.ExecuteOut)
		{
			if (Node.ExecuteIn.Contains(Pin))
			{
				return Pin;
			}
		}
		// An event: its only execute pin is an output.
		return Outputs.Num() == 1 ? Outputs[0] : FString();
	}
}
