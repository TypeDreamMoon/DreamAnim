// Copyright DreamMoon. All Rights Reserved.

#include "Model/DcrModel.h"

namespace UE::DreamAnim::Model
{
	const TCHAR* LexToString(EDcrNodeKind Kind)
	{
		switch (Kind)
		{
		case EDcrNodeKind::Unit: return TEXT("Unit");
		case EDcrNodeKind::Dispatch: return TEXT("Dispatch");
		case EDcrNodeKind::VariableGet: return TEXT("VariableGet");
		case EDcrNodeKind::VariableSet: return TEXT("VariableSet");
		case EDcrNodeKind::LocalCall: return TEXT("LocalCall");
		case EDcrNodeKind::ExternalCall: return TEXT("ExternalCall");
		case EDcrNodeKind::Collapse: return TEXT("Collapse");
		case EDcrNodeKind::Entry: return TEXT("Entry");
		case EDcrNodeKind::Return: return TEXT("Return");
		case EDcrNodeKind::InvokeEntry: return TEXT("InvokeEntry");
		case EDcrNodeKind::Comment: return TEXT("Comment");
		case EDcrNodeKind::Reroute: return TEXT("Reroute");
		case EDcrNodeKind::Unknown: return TEXT("Unknown");
		}
		return TEXT("?");
	}

	const TCHAR* LexToString(EDcrElementKind Kind)
	{
		switch (Kind)
		{
		case EDcrElementKind::Bone: return TEXT("Bone");
		case EDcrElementKind::Null: return TEXT("Null");
		case EDcrElementKind::Control: return TEXT("Control");
		case EDcrElementKind::Curve: return TEXT("Curve");
		case EDcrElementKind::Connector: return TEXT("Connector");
		case EDcrElementKind::Socket: return TEXT("Socket");
		case EDcrElementKind::Unknown: return TEXT("Unknown");
		}
		return TEXT("?");
	}

	const FDcrModelNode* FDcrModelGraph::FindNode(const FString& NodeName) const
	{
		return Nodes.FindByPredicate([&NodeName](const FDcrModelNode& Node) { return Node.Name == NodeName; });
	}

	FDcrModelNode* FDcrModelGraph::FindNode(const FString& NodeName)
	{
		return Nodes.FindByPredicate([&NodeName](const FDcrModelNode& Node) { return Node.Name == NodeName; });
	}

	// DumpDcrModel is in DcrModelDump.cpp.
}
