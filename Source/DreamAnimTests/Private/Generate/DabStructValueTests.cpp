// Copyright DreamMoon. All Rights Reserved.
//
// Constants on a node. A tuple inside a tuple is the whole struct, so a field it leaves out (or writes at the
// struct's own default) is that default, not the node's: Look At's axes default to +Y and +X, and text that asks
// for +X and +Z must get exactly those. A blend list's blend settings and its players' sync group settings come
// out of the decompiler. Both hold before and after a round trip through it.

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_Base.h"
#include "Driver/DabDriver.h"
#include "EdGraph/EdGraph.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Tests::StructValuePrivate
{
	using namespace UE::DreamAnim;
	using namespace UE::DreamAnim::Compiler;

	const TCHAR* const LookAtText = TEXT(
		"AnimBlueprint(Name=\"DreamAnimTests/Structs/AB_LookAtAxes\", Root=\"Game\")\n"
		"{\n"
		"    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }\n"
		"    Graph = {\n"
		"        Output = {\n"
		"            LocalRefPose();\n"
		"            LocalToComponentSpace();\n"
		"            LookAt(\n"
		"                BoneToModify = (BoneName = \"spine_03\"),\n"
		"                LookAt_Axis  = (Axis = (X = 1, Y = 0, Z = 0)),\n"
		"                LookUp_Axis  = (Axis = (Z = 1))\n"
		"            );\n"
		"            ComponentToLocalSpace();\n"
		"        };\n"
		"    }\n"
		"}\n");

	UAnimBlueprint* Build(FAutomationTestBase& Test, const FString& Text)
	{
		FDabCompileOptions Options;
		Options.bSave = false;
		const FDabCompileResult Result = CompileDabText(TEXT("Structs.dab"), Text, Options);
		for (const Lang::FDabDiagnostic& Diagnostic : Result.Diagnostics)
		{
			if (Diagnostic.Severity == Lang::EDabSeverity::Error)
			{
				Test.AddError(FormatDabDiagnosticLine(Diagnostic));
			}
		}
		return Result.Succeeded() ? Result.Blueprint : nullptr;
	}

	/** The Axis vector of one FAxis field of the Blueprint's Look At node, read by reflection. */
	bool ReadAxis(FAutomationTestBase& Test, const UAnimBlueprint& Blueprint, const TCHAR* Field, FVector& OutAxis)
	{
		TArray<UEdGraph*> Graphs;
		Blueprint.GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				const UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
				const FStructProperty* NodeProperty = AnimNode ? AnimNode->GetFNodeProperty() : nullptr;
				if (!NodeProperty || NodeProperty->Struct->GetName() != TEXT("AnimNode_LookAt"))
				{
					continue;
				}
				const void* NodeData = NodeProperty->ContainerPtrToValuePtr<void>(AnimNode);
				const FStructProperty* AxisField = FindFProperty<FStructProperty>(NodeProperty->Struct, Field);
				const FStructProperty* Vector = AxisField ? FindFProperty<FStructProperty>(AxisField->Struct, TEXT("Axis")) : nullptr;
				if (!Test.TestNotNull(FString::Printf(TEXT("Look At has %s.Axis"), Field), Vector))
				{
					return false;
				}
				OutAxis = *Vector->ContainerPtrToValuePtr<FVector>(AxisField->ContainerPtrToValuePtr<void>(NodeData));
				return true;
			}
		}
		Test.AddError(TEXT("The Blueprint has no Look At node."));
		return false;
	}

	/** A blend list with every blend setting away from its default, over players in a sync group. */
	const TCHAR* const BlendListText = TEXT(
		"AnimBlueprint(Name=\"DreamAnimTests/Structs/AB_BlendListSettings\", Root=\"Game\")\n"
		"{\n"
		"    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }\n"
		"    Graph = {\n"
		"        Output = BlendListByInt(\n"
		"            ActiveChildIndex        = 1,\n"
		"            BlendTime               = [ 0.2, 0.4 ],\n"
		"            TransitionType          = Inertialization,\n"
		"            BlendType               = Cubic,\n"
		"            BlendProfile            = Path(Plugin.ALS, \"ALS/Character/SK_Als:MoveDirectionChange\"),\n"
		"            ChildUpateMode          = ResetChildOnActivate,\n"
		"            BlendPose               = [\n"
		"                SequencePlayer(GroupName = \"Movement\", GroupRole = AlwaysLeader, Method = SyncGroup, Sequence = A_Als_Sprint),\n"
		"                SequencePlayer(GroupName = \"Movement\", GroupRole = TransitionFollower, Method = SyncGroup, Sequence = A_Als_Sprint_Acceleration)\n"
		"            ]\n"
		"        );\n"
		"    }\n"
		"}\n");

	/** `Node.Field = value` for each of Fields that an anim node of the Blueprint has, sorted. */
	TArray<FString> ReadFields(const UAnimBlueprint& Blueprint, TConstArrayView<const TCHAR*> Fields)
	{
		TArray<FString> Out;
		TArray<UEdGraph*> Graphs;
		Blueprint.GetAllGraphs(Graphs);
		for (const UEdGraph* Graph : Graphs)
		{
			if (!Graph)
			{
				continue;
			}
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				const UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node);
				const FStructProperty* NodeProperty = AnimNode ? AnimNode->GetFNodeProperty() : nullptr;
				if (!NodeProperty)
				{
					continue;
				}
				const void* NodeData = NodeProperty->ContainerPtrToValuePtr<void>(AnimNode);
				for (const TCHAR* Field : Fields)
				{
					if (const FProperty* Property = FindFProperty<FProperty>(NodeProperty->Struct, Field))
					{
						FString Text;
						Property->ExportTextItem_Direct(Text, Property->ContainerPtrToValuePtr<void>(NodeData), nullptr, nullptr, PPF_None);
						Out.Add(FString::Printf(TEXT("%s.%s = %s"), *NodeProperty->Struct->GetName(), Field, *Text));
					}
				}
			}
		}
		Out.Sort();
		return Out;
	}

	void ExpectAxes(FAutomationTestBase& Test, const UAnimBlueprint& Blueprint, const TCHAR* When)
	{
		FVector LookAt;
		FVector LookUp;
		if (ReadAxis(Test, Blueprint, TEXT("LookAt_Axis"), LookAt) && ReadAxis(Test, Blueprint, TEXT("LookUp_Axis"), LookUp))
		{
			Test.TestEqual(FString::Printf(TEXT("%s: LookAt_Axis is +X"), When), LookAt, FVector(1.0, 0.0, 0.0));
			Test.TestEqual(FString::Printf(TEXT("%s: LookUp_Axis is +Z"), When), LookUp, FVector(0.0, 0.0, 1.0));
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabNestedStructDefaultsTest, "DreamAnim.Generate.NestedStructDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDabNestedStructDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::StructValuePrivate;

	const UAnimBlueprint* Blueprint = Build(*this, LookAtText);
	if (!Blueprint)
	{
		return false;
	}
	ExpectAxes(*this, *Blueprint, TEXT("built"));

	const FDabDecompileResult Decompiled = DecompileDabBlueprint(*Blueprint);
	TestEqual(TEXT("decompiles completely"), Decompiled.NumUnsupported, 0);
	if (const UAnimBlueprint* Rebuilt = Build(*this, Decompiled.Text))
	{
		ExpectAxes(*this, *Rebuilt, TEXT("rebuilt from its decompiled text"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabBlendListSettingsTest, "DreamAnim.Generate.BlendListAndSyncSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDabBlendListSettingsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::StructValuePrivate;

	static const TCHAR* const Fields[] = {
		// ChildUpateMode (spelled so by the engine) replaced a blend list's bResetChildOnActivation in 5.6.
		TEXT("BlendTime"), TEXT("TransitionType"), TEXT("BlendType"), TEXT("BlendProfile"), TEXT("ChildUpateMode"),
		TEXT("GroupName"), TEXT("GroupRole"), TEXT("Method"),
	};
	const UAnimBlueprint* Blueprint = Build(*this, BlendListText);
	if (!Blueprint)
	{
		return false;
	}
	const TArray<FString> Built = ReadFields(*Blueprint, Fields);
	for (const FString& Line : Built)
	{
		AddInfo(Line);
	}

	// Every setting the text gave is in the decompiled text, and building that gives the same node values.
	const FDabDecompileResult Decompiled = DecompileDabBlueprint(*Blueprint);
	TestEqual(TEXT("decompiles completely"), Decompiled.NumUnsupported, 0);
	for (const TCHAR* Field : Fields)
	{
		TestTrue(FString::Printf(TEXT("the decompiled text has %s"), Field), Decompiled.Text.Contains(FString(Field) + TEXT(" ")));
	}
	if (const UAnimBlueprint* Rebuilt = Build(*this, Decompiled.Text))
	{
		TestEqual(TEXT("the rebuilt nodes hold the same settings"), FString::Join(ReadFields(*Rebuilt, Fields), TEXT("\n")), FString::Join(Built, TEXT("\n")));
	}
	return true;
}

#endif
