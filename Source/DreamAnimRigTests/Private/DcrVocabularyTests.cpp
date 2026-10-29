// Copyright DreamMoon. All Rights Reserved.
//
// The Control Rig part of `dab.ps1 vocab -RigUnits`: the units and dispatches a .dcr can call, with their pins
// and, for nodes with execute outputs of their own, the blocks a call can have.

#include "Dom/JsonObject.h"
#include "Driver/DabVocabularyExport.h"
#include "Misc/AutomationTest.h"
#include "RigVMCore/RigVMDispatchFactory.h"
#include "RigVMCore/RigVMRegistry.h"
#include "Units/RigUnitContext.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Rig::Tests::VocabularyPrivate
{
	TSharedPtr<FJsonObject> FindUnit(const TArray<TSharedPtr<FJsonValue>>& Units, const FString& Name)
	{
		for (const TSharedPtr<FJsonValue>& Item : Units)
		{
			const TSharedPtr<FJsonObject> Unit = Item.IsValid() ? Item->AsObject() : nullptr;
			if (Unit.IsValid() && Unit->GetStringField(TEXT("name")) == Name)
			{
				return Unit;
			}
		}
		return nullptr;
	}

	TArray<FString> GetStrings(const FJsonObject& Object, const TCHAR* Field)
	{
		TArray<FString> Out;
		Object.TryGetStringArrayField(Field, Out);
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrVocabularyTest, "DreamAnim.Rig.Vocabulary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDcrVocabularyTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Compiler;
	using namespace UE::DreamAnim::Rig::Tests::VocabularyPrivate;

	FDabVocabularyOptions Options;
	Options.bRigUnits = true;
	const TSharedRef<FJsonObject> Root = MakeDabVocabulary(Options);
	const TArray<TSharedPtr<FJsonValue>>* Units = nullptr;
	if (!TestTrue(TEXT("rigUnits is there with RigUnits"), Root->TryGetArrayField(TEXT("rigUnits"), Units) && Units))
	{
		return false;
	}
	TestTrue(TEXT("the engine's units and dispatches are there"), Units->Num() > 500);

	// A unit with execute outputs of its own: the blocks after its call, and what execution goes on by.
	if (const TSharedPtr<FJsonObject> Timer = FindUnit(*Units, TEXT("ControlFlowTimer")); TestTrue(TEXT("ControlFlowTimer"), Timer.IsValid()))
	{
		TestTrue(TEXT("Timer runs"), Timer->GetBoolField(TEXT("mutable")));
		TestEqual(TEXT("Timer goes on by Completed"), Timer->GetStringField(TEXT("continuesBy")), FString(TEXT("Completed")));
		TestEqual(TEXT("Timer's blocks"), FString::Join(GetStrings(*Timer, TEXT("blocks")), TEXT(",")), FString(TEXT("JustStarted,IsRunning,JustFinished")));
		const TSharedPtr<FJsonObject> Ratio = FindUnit(Timer->GetArrayField(TEXT("pins")), TEXT("Ratio"));
		TestTrue(TEXT("Ratio is an output"), Ratio.IsValid() && Ratio->GetStringField(TEXT("direction")) == TEXT("output"));
	}
	// A pure unit, by the name the text calls it.
	if (const TSharedPtr<FJsonObject> Get = FindUnit(*Units, TEXT("GetTransform")); TestTrue(TEXT("GetTransform"), Get.IsValid()))
	{
		TestFalse(TEXT("GetTransform does not run"), Get->GetBoolField(TEXT("mutable")));
		const TSharedPtr<FJsonObject> Item = FindUnit(Get->GetArrayField(TEXT("pins")), TEXT("Item"));
		TestTrue(TEXT("Item is a RigElementKey"), Item.IsValid() && Item->GetStringField(TEXT("type")) == TEXT("RigElementKey"));
	}
	// A dispatch: its pins take several types.
	if (const TSharedPtr<FJsonObject> If = FindUnit(*Units, TEXT("If")); TestTrue(TEXT("If"), If.IsValid()))
	{
		TestEqual(TEXT("If is a dispatch"), If->GetStringField(TEXT("kind")), FString(TEXT("dispatch")));
		const TSharedPtr<FJsonObject> Result = FindUnit(If->GetArrayField(TEXT("pins")), TEXT("Result"));
		TestTrue(TEXT("If's Result is a wildcard"), Result.IsValid() && Result->GetBoolField(TEXT("wildcard")));
	}

	// Only what runs in a Control Rig: a unit or dispatch of another execute context (UAF's) cannot be added to one,
	// so it is not listed (and the checker does not find it).
	TSet<FString> Listed;
	for (const TSharedPtr<FJsonValue>& Item : *Units)
	{
		Listed.Add(Item->AsObject()->GetStringField(TEXT("struct")));
	}
	const UScriptStruct* ExecuteContext = StaticStruct<FControlRigExecuteContext>();
	const FRigVMRegistry& Registry = FRigVMRegistry::Get();
	int32 NumOther = 0;
	const TChunkedArray<FRigVMFunction>& Functions = Registry.GetFunctions();
	for (int32 Index = 0; Index < Functions.Num(); ++Index)
	{
		const FRigVMFunction& Function = Functions[Index];
		if (Function.Struct && !Function.Factory && !Function.SupportsExecuteContextStruct(ExecuteContext))
		{
			++NumOther;
			TestFalse(FString::Printf(TEXT("%s runs in another execute context"), *Function.Struct->GetName()), Listed.Contains(Function.Struct->GetPathName()));
		}
	}
	for (const FRigVMDispatchFactory* Factory : Registry.GetFactories())
	{
		if (Factory && Factory->GetScriptStruct() && !Factory->SupportsExecuteContextStruct(ExecuteContext))
		{
			++NumOther;
			TestFalse(FString::Printf(TEXT("%s runs in another execute context"), *Factory->GetScriptStruct()->GetName()), Listed.Contains(Factory->GetScriptStruct()->GetPathName()));
		}
	}
	AddInfo(FString::Printf(TEXT("%d units and dispatches of another execute context left out"), NumOther));
	return true;
}

#endif
