// Copyright DreamMoon. All Rights Reserved.
//
// Rebuilding a Blueprint that exists already: a text that does not compile leaves it as it was, and what
// the text no longer says (a setting, a class default, a tuning value's default) goes back to the default
// instead of keeping the previous build's value.

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Driver/DabDriver.h"
#include "DreamAnimLibrary.h"
#include "DreamAnimSet.h"
#include "Generate/DabGenerate.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Tests::RebuildPrivate
{
	using namespace UE::DreamAnim;
	using namespace UE::DreamAnim::Compiler;

	/** A document under /Game/DreamAnimTests/Rebuild with the ALS skeleton. */
	FString MakeSource(const TCHAR* Name, const TCHAR* Settings, const TCHAR* Body)
	{
		return FString::Printf(TEXT("AnimBlueprint(Name=\"DreamAnimTests/Rebuild/%s\", Root=\"Game\")\n{\n    Settings = { Skeleton = SK_Als; %s }\n    %s\n}\n"),
			Name, Settings, Body);
	}

	/** The text built in memory; the Blueprint, or null after reporting the errors. */
	UAnimBlueprint* Build(FAutomationTestBase& Test, const FString& Text)
	{
		FDabCompileOptions Options;
		Options.bSave = false;
		const FDabCompileResult Result = CompileDabText(TEXT("Rebuild.dab"), Text, Options);
		for (const Lang::FDabDiagnostic& Diagnostic : Result.Diagnostics)
		{
			if (Diagnostic.Severity == Lang::EDabSeverity::Error)
			{
				Test.AddError(FormatDabDiagnosticLine(Diagnostic));
			}
		}
		return Result.Succeeded() ? Result.Blueprint : nullptr;
	}

	UObject* GetDefaults(const UAnimBlueprint& Blueprint)
	{
		return Blueprint.GeneratedClass->GetDefaultObject();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabRebuildFailureKeepsAssetTest, "DreamAnim.Generate.FailedBuildPreservesAsset",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/**
 * A text the language accepts but the Blueprint compiler rejects (a player set to sync with no group): the
 * existing Blueprint keeps its class, graphs and behaviour, the text it was built from stays the last one
 * that compiled, and the package is not marked dirty.
 */
bool FDabRebuildFailureKeepsAssetTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::RebuildPrivate;

	const FString Good = MakeSource(TEXT("AB_Protected"), TEXT("Parent = AnimInstance;"), TEXT("Graph = { Output = SequencePlayer(Sequence = A_Als_Idle); }"));
	UAnimBlueprint* Blueprint = Build(*this, Good);
	if (!TestNotNull(TEXT("the first build"), Blueprint))
	{
		return false;
	}
	UClass* OriginalClass = Blueprint->GeneratedClass;
	TArray<UEdGraph*> OriginalGraphs;
	Blueprint->GetAllGraphs(OriginalGraphs);
	const FString Before = DecompileDabBlueprint(*Blueprint).Text;
	Blueprint->GetPackage()->SetDirtyFlag(false);

	FDabCompileOptions Options;
	Options.bSave = false;
	const FDabCompileResult Failed = CompileDabText(TEXT("Rebuild.dab"), Good.Replace(TEXT("Sequence = A_Als_Idle"), TEXT("Sequence = A_Als_Idle, Method = SyncGroup")), Options);
	TestFalse(TEXT("the broken text fails"), Failed.Succeeded());
	TestTrue(TEXT("the generated class survives"), Blueprint->GeneratedClass == OriginalClass);
	TArray<UEdGraph*> AfterGraphs;
	Blueprint->GetAllGraphs(AfterGraphs);
	TestTrue(TEXT("the graphs survive"), OriginalGraphs == AfterGraphs);
	TestEqualSensitive(TEXT("the text it was built from stays the last that compiled"), GetDabGeneratedText(*Blueprint), Good);
	TestEqualSensitive(TEXT("it decompiles as before"), DecompileDabBlueprint(*Blueprint).Text, Before);
	TestFalse(TEXT("the package is not dirtied"), Blueprint->GetPackage()->IsDirty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabRebuildRemovedDefaultsTest, "DreamAnim.Generate.RemovedDefaults",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/**
 * A setting, a class default, a tuning value's default and a DefaultAnimSet removed from the text go back
 * to their defaults on the next build: the result depends on the text, not on what was built before.
 */
bool FDabRebuildRemovedDefaultsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::RebuildPrivate;

	UDreamAnimSet* Set = NewObject<UDreamAnimSet>(CreatePackage(TEXT("/Game/DreamAnimTests/Rebuild/DAS_Removed")), TEXT("DAS_Removed"), RF_Public | RF_Standalone);
	UAnimBlueprint* Blueprint = Build(*this, MakeSource(TEXT("AB_Defaults"),
		TEXT("Parent = AnimInstance; bEnableLinkedAnimLayerInstanceSharing = true; DefaultAnimSet = Path(Game, \"DreamAnimTests/Rebuild/DAS_Removed\");"),
		TEXT("Defaults = { RootMotionMode = IgnoreRootMotion; } Properties = { float Speed = 3; } Assets = { AnimSequence Pose; } Graph = { Output = LocalRefPose(); }")));
	if (!TestNotNull(TEXT("the first build"), Blueprint))
	{
		return false;
	}
	// A Blueprint float is a double.
	const FDoubleProperty* Speed = FindFProperty<FDoubleProperty>(Blueprint->GeneratedClass, TEXT("Speed"));
	TestTrue(TEXT("the setting is applied"), Blueprint->bEnableLinkedAnimLayerInstanceSharing);
	TestEqual(TEXT("the class default is applied"), CastChecked<UAnimInstance>(GetDefaults(*Blueprint))->RootMotionMode.GetValue(), ERootMotionMode::IgnoreRootMotion);
	TestTrue(TEXT("the tuning value has its default"), Speed && Speed->GetPropertyValue_InContainer(GetDefaults(*Blueprint)) == 3.0);
	TestTrue(TEXT("the set is the default"), UDreamAnimLibrary::GetAnimSet(CastChecked<UAnimInstance>(GetDefaults(*Blueprint))) == Set);

	UAnimBlueprint* Rebuilt = Build(*this, MakeSource(TEXT("AB_Defaults"), TEXT("Parent = AnimInstance;"),
		TEXT("Properties = { float Speed; } Assets = { AnimSequence Pose; } Graph = { Output = LocalRefPose(); }")));
	if (!TestNotNull(TEXT("the rebuild"), Rebuilt))
	{
		return false;
	}
	Speed = FindFProperty<FDoubleProperty>(Rebuilt->GeneratedClass, TEXT("Speed"));
	TestTrue(TEXT("the asset is the same object"), Rebuilt == Blueprint);
	TestFalse(TEXT("a removed setting returns to its default"), Rebuilt->bEnableLinkedAnimLayerInstanceSharing);
	TestEqual(TEXT("a removed class default returns to the parent's"), CastChecked<UAnimInstance>(GetDefaults(*Rebuilt))->RootMotionMode.GetValue(),
		GetDefault<UAnimInstance>()->RootMotionMode.GetValue());
	TestTrue(TEXT("a removed tuning value default returns to zero"), Speed && Speed->GetPropertyValue_InContainer(GetDefaults(*Rebuilt)) == 0.0);
	TestNull(TEXT("a removed DefaultAnimSet leaves no set"), UDreamAnimLibrary::GetAnimSet(CastChecked<UAnimInstance>(GetDefaults(*Rebuilt))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabRebuildForwarderArgumentTest, "DreamAnim.Generate.ForwarderPropertyArgument",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/**
 * A state function passing a tuning value to C++ needs the variable while its forwarder is built: the
 * variables are all in place before the skeleton class is made again, and the call round-trips.
 */
bool FDabRebuildForwarderArgumentTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::RebuildPrivate;

	const FString Text = TEXT(
		"AnimBlueprint(Name=\"DreamAnimTests/Rebuild/AB_Forwarder\", Root=\"Game\")\n"
		"{\n"
		"    Settings = { Parent = AlsAnimationInstance; Skeleton = SK_Als; }\n"
		"    Properties = { float Duration = 0.3; }\n"
		"    Graph = { Output = Machine; }\n"
		"    StateMachine Machine\n"
		"    {\n"
		"        Entry = Idle;\n"
		"        State Idle { OnEnter = Self.StopTransitionAndTurnInPlaceAnimations(Props.Duration); Pose = LocalRefPose(); }\n"
		"    }\n"
		"}\n");
	UAnimBlueprint* Blueprint = Build(*this, Text);
	if (!TestNotNull(TEXT("the build"), Blueprint))
	{
		return false;
	}
	const FDabDecompileResult Decompiled = DecompileDabBlueprint(*Blueprint);
	TestTrue(TEXT("the forwarder keeps its argument"), Decompiled.Text.Contains(TEXT("Props.Duration")));
	TestNotNull(TEXT("the text builds again"), Build(*this, Decompiled.Text));
	return true;
}

#endif
