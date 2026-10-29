// Copyright DreamMoon. All Rights Reserved.
//
// `dab.ps1 vocab`: the JSON reads back, holds the grammar version and every kind of document, describes the
// nodes a few known facts are true of, and every node type it names is one the checker finds by that name.

#include "Dom/JsonObject.h"
#include "Driver/DabDriver.h"
#include "Driver/DabVocabularyExport.h"
#include "Lang/DabVocabulary.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Tests::VocabularyExportPrivate
{
	TSharedPtr<FJsonObject> FindByName(const TArray<TSharedPtr<FJsonValue>>& Items, const FString& Name)
	{
		for (const TSharedPtr<FJsonValue>& Item : Items)
		{
			const TSharedPtr<FJsonObject> Object = Item.IsValid() ? Item->AsObject() : nullptr;
			if (Object.IsValid() && Object->GetStringField(TEXT("name")) == Name)
			{
				return Object;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabVocabularyExportTest, "DreamAnim.Driver.Vocabulary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDabVocabularyExportTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim;
	using namespace UE::DreamAnim::Compiler;
	using namespace UE::DreamAnim::Tests::VocabularyExportPrivate;

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(MakeDabVocabularyJson());
	if (!TestTrue(TEXT("the vocabulary is JSON"), FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("format"), static_cast<int32>(Root->GetNumberField(TEXT("format"))), 1);
	TestEqual(TEXT("grammarVersion"), static_cast<int32>(Root->GetNumberField(TEXT("grammarVersion"))), Lang::DabGrammarVersion);
	TestEqual(TEXT("one entry for each kind of document"), Root->GetArrayField(TEXT("documents")).Num(), Lang::GetDabDocumentWords().Num());
	TestFalse(TEXT("no Control Rig units unless asked"), Root->HasField(TEXT("rigUnits")));

	const TArray<TSharedPtr<FJsonValue>>& Nodes = Root->GetArrayField(TEXT("animNodes"));
	TestTrue(TEXT("the engine's node types are there"), Nodes.Num() > 50);

	// A few facts about known nodes.
	if (const TSharedPtr<FJsonObject> Player = FindByName(Nodes, TEXT("SequencePlayer")); TestTrue(TEXT("SequencePlayer"), Player.IsValid()))
	{
		const TSharedPtr<FJsonObject> Sequence = FindByName(Player->GetArrayField(TEXT("pins")), TEXT("Sequence"));
		if (TestTrue(TEXT("SequencePlayer has Sequence"), Sequence.IsValid()))
		{
			TestEqual(TEXT("Sequence is a value"), Sequence->GetStringField(TEXT("kind")), FString(TEXT("value")));
			TestEqual(TEXT("Sequence's type"), Sequence->GetStringField(TEXT("type")), FString(TEXT("AnimSequenceBase")));
			TestTrue(TEXT("Sequence can be bound"), Sequence->GetBoolField(TEXT("bindable")));
		}
		const TSharedPtr<FJsonObject> Method = FindByName(Player->GetArrayField(TEXT("pins")), TEXT("Method"));
		if (TestTrue(TEXT("SequencePlayer has Method"), Method.IsValid()))
		{
			TestEqual(TEXT("Method's default"), Method->GetStringField(TEXT("default")), FString(TEXT("DoNotSync")));
			TestTrue(TEXT("Method's values"), Method->GetArrayField(TEXT("values")).Num() >= 3);
		}
	}
	if (const TSharedPtr<FJsonObject> Blend = FindByName(Nodes, TEXT("BlendListByEnum")); TestTrue(TEXT("BlendListByEnum"), Blend.IsValid()))
	{
		TestEqual(TEXT("BlendListByEnum is keyed by enum entries"), Blend->GetStringField(TEXT("keyedBy")), FString(TEXT("enum")));
		const TSharedPtr<FJsonObject> Poses = FindByName(Blend->GetArrayField(TEXT("pins")), TEXT("BlendPose"));
		TestTrue(TEXT("BlendPose is a pose array"), Poses.IsValid() && Poses->GetStringField(TEXT("kind")) == TEXT("poseArray"));
		const TSharedPtr<FJsonObject> Times = FindByName(Blend->GetArrayField(TEXT("pins")), TEXT("BlendTime"));
		TestTrue(TEXT("BlendTime has one value per pose"), Times.IsValid() && Times->GetBoolField(TEXT("perPose")));
	}
	if (const TSharedPtr<FJsonObject> LookAt = FindByName(Nodes, TEXT("LookAt")); TestTrue(TEXT("LookAt"), LookAt.IsValid()))
	{
		TestEqual(TEXT("LookAt gives a component space pose"), LookAt->GetStringField(TEXT("outputSpace")), FString(TEXT("component")));
		const TSharedPtr<FJsonObject> Axis = FindByName(LookAt->GetArrayField(TEXT("pins")), TEXT("LookAt_Axis"));
		TestTrue(TEXT("LookAt_Axis defaults to +Y, written as the decompiler writes it"),
			Axis.IsValid() && Axis->GetStringField(TEXT("default")) == TEXT("(Axis = (Y = 1))"));
	}
	if (const TSharedPtr<FJsonObject> Result = FindByName(Nodes, TEXT("Root")); TestTrue(TEXT("Root"), Result.IsValid()))
	{
		TestTrue(TEXT("Root is a sink"), Result->GetBoolField(TEXT("sink")));
		TestTrue(TEXT("Root says how it is written"), Result->HasField(TEXT("writtenAs")));
	}

	if (const TSharedPtr<FJsonObject> Inertia = FindByName(Nodes, TEXT("Inertialization")); TestTrue(TEXT("Inertialization"), Inertia.IsValid()))
	{
		// The runtime node has a Tag too; the text's Tag is the editor node's, which the compiler copies over.
		const TSharedPtr<FJsonObject> Tag = FindByName(Inertia->GetArrayField(TEXT("pins")), TEXT("Tag"));
		TestTrue(TEXT("Inertialization's Tag is the editor node's setting"), Tag.IsValid() && Tag->GetStringField(TEXT("kind")) == TEXT("setting"));
	}

	// An argument name means one property, so a node lists it once.
	for (const TSharedPtr<FJsonValue>& Item : Nodes)
	{
		const TSharedPtr<FJsonObject> Node = Item->AsObject();
		TSet<FString> Seen;
		for (const TSharedPtr<FJsonValue>& Pin : Node.IsValid() ? Node->GetArrayField(TEXT("pins")) : TArray<TSharedPtr<FJsonValue>>())
		{
			const FString PinName = Pin->AsObject()->GetStringField(TEXT("name"));
			if (Seen.Contains(PinName))
			{
				AddError(FString::Printf(TEXT("%s lists `%s` twice"), *Node->GetStringField(TEXT("name")), *PinName));
			}
			Seen.Add(PinName);
		}
	}

	// Every name the vocabulary gives a node type is one the checker finds that node by.
	int32 Checked = 0;
	for (const TSharedPtr<FJsonValue>& Item : Nodes)
	{
		const TSharedPtr<FJsonObject> Node = Item->AsObject();
		if (!Node.IsValid() || Node->HasField(TEXT("writtenAs")))
		{
			continue;
		}
		const FString Name = Node->GetStringField(TEXT("name"));
		const FString Text = FString::Printf(TEXT("AnimBlueprint(Name=\"DreamAnimTests/Vocabulary/AB_Names\", Root=\"Game\")\n{\n    Graph = { Output = %s(); }\n}\n"), *Name);
		FDabCompileOptions Options;
		Options.bGenerate = false;
		const FDabCompileResult Result = CompileDabText(TEXT("Names.dab"), Text, Options);
		for (const Lang::FDabDiagnostic& Diagnostic : Result.Diagnostics)
		{
			if (Diagnostic.Code == TEXT("DAB3001") || Diagnostic.Code == TEXT("DAB3002"))
			{
				AddError(FString::Printf(TEXT("%s: %s"), *Name, *Diagnostic.Message));
			}
		}
		++Checked;
	}
	AddInfo(FString::Printf(TEXT("%d node type names checked"), Checked));
	return true;
}

#endif
