// Copyright DreamMoon. All Rights Reserved.
//
// Decompiling into an animation set changes the set only when it has to: a preview leaves it alone, a
// Blueprint with nothing left to move keeps its set, and a decompile that refuses to replace a source file
// touches no set at all.

#include "Animation/AnimBlueprint.h"
#include "Commandlets/Commandlet.h"
#include "Driver/DabDriver.h"
#include "DreamAnimSet.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Tests::MigrationPrivate
{
	using namespace UE::DreamAnim;
	using namespace UE::DreamAnim::Compiler;

	UAnimBlueprint* CompileText(FAutomationTestBase& Test, const FString& Text)
	{
		FDabCompileOptions Options;
		Options.bSave = false;
		const FDabCompileResult Result = CompileDabText(TEXT("Migration.dab"), Text, Options);
		for (const Lang::FDabDiagnostic& Diagnostic : Result.Diagnostics)
		{
			if (Diagnostic.Severity == Lang::EDabSeverity::Error)
			{
				Test.AddError(FormatDabDiagnosticLine(Diagnostic));
			}
		}
		return Result.Succeeded() ? Result.Blueprint : nullptr;
	}

	UAnimBlueprint* BuildPlayer(FAutomationTestBase& Test, const TCHAR* Name)
	{
		const FString Text = FString::Printf(TEXT(
			"AnimBlueprint(Name=\"DreamAnimTests/Migration/%s\", Root=\"Game\")\n"
			"{\n"
			"    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }\n"
			"    Graph = { Output = SequencePlayer(Sequence = A_Als_Idle); }\n"
			"}\n"), Name);
		return CompileText(Test, Text);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabMigrationPreviewAndRepeatTest, "DreamAnim.Driver.MigrationPreviewAndRepeat",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/**
 * A preview gives the text a migration would write and creates no set. Once the Blueprint reads its
 * animations from slots, decompiling it into another set has nothing to move: the text keeps its set and
 * the other set is never made.
 */
bool FDabMigrationPreviewAndRepeatTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::MigrationPrivate;

	UAnimBlueprint* Blueprint = BuildPlayer(*this, TEXT("AB_Migrate"));
	if (!TestNotNull(TEXT("the Blueprint"), Blueprint))
	{
		return false;
	}
	FDabAnimSetMigration Migration;
	Migration.AnimSetPath = TEXT("/Game/DreamAnimTests/Migration/DAS_Migrate");
	Migration.bSave = false;
	Migration.bPreview = true;
	const FDabDecompileResult Preview = DecompileDabBlueprint(*Blueprint, FDabLowerOptions(), &Migration);
	TestTrue(TEXT("the preview succeeds"), Preview.bSucceeded);
	TestNull(TEXT("and creates no set"), FindObject<UDreamAnimSet>(nullptr, TEXT("/Game/DreamAnimTests/Migration/DAS_Migrate.DAS_Migrate")));

	Migration.bPreview = false;
	const FDabDecompileResult Applied = DecompileDabBlueprint(*Blueprint, FDabLowerOptions(), &Migration);
	TestTrue(TEXT("the migration succeeds"), Applied.bSucceeded);
	TestEqualSensitive(TEXT("the preview showed the text written"), Preview.Text, Applied.Text);
	Blueprint = CompileText(*this, Applied.Text);
	if (!TestNotNull(TEXT("the migrated Blueprint"), Blueprint))
	{
		return false;
	}

	const FDabDecompileResult Plain = DecompileDabBlueprint(*Blueprint);
	Migration.AnimSetPath = TEXT("/Game/DreamAnimTests/Migration/DAS_Unused");
	const FDabDecompileResult Repeated = DecompileDabBlueprint(*Blueprint, FDabLowerOptions(), &Migration);
	TestTrue(TEXT("a repeat succeeds"), Repeated.bSucceeded);
	TestEqual(TEXT("with nothing new to move"), Repeated.LiftedAssets.Num(), 0);
	TestEqualSensitive(TEXT("the text keeps its set"), Repeated.Text, Plain.Text);
	TestNull(TEXT("the other set is never made"), FindObject<UDreamAnimSet>(nullptr, TEXT("/Game/DreamAnimTests/Migration/DAS_Unused.DAS_Unused")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabMigrationOutputProtectionTest, "DreamAnim.Driver.DecompileOutputProtection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/**
 * `-Decompile -AnimSet` onto a source that exists and differs, without -Force: the command fails, the
 * source stays as it is, an existing set gains nothing and a missing one is not created.
 */
bool FDabMigrationOutputProtectionTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::MigrationPrivate;

	UAnimBlueprint* Blueprint = BuildPlayer(*this, TEXT("AB_ProtectedOutput"));
	if (!TestNotNull(TEXT("the Blueprint"), Blueprint))
	{
		return false;
	}
	const FString Output = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("DreamAnimTests") / (FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".dab")));
	const FString Original = TEXT("hand-written source\n");
	if (!TestTrue(TEXT("the source to protect is written"), FFileHelper::SaveStringToFile(Original, *Output)))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		IFileManager::Get().Delete(*Output);
	};
	UClass* CommandletClass = FindObject<UClass>(nullptr, TEXT("/Script/DreamAnimCompiler.DreamAnimCommandlet"));
	if (!TestNotNull(TEXT("the commandlet"), CommandletClass))
	{
		return false;
	}
	UDreamAnimSet* Set = NewObject<UDreamAnimSet>(CreatePackage(TEXT("/Game/DreamAnimTests/Migration/DAS_ProtectedOutput")), TEXT("DAS_ProtectedOutput"), RF_Public | RF_Standalone);
	Set->GetPackage()->SetDirtyFlag(false);

	AddExpectedErrorPlain(TEXT("exists and differs"), EAutomationExpectedErrorFlags::Contains, 2);
	for (const FString& Target : { Set->GetPathName(), FString(TEXT("/Game/DreamAnimTests/Migration/DAS_NeverCreated")) })
	{
		UCommandlet* Commandlet = NewObject<UCommandlet>(GetTransientPackage(), CommandletClass);
		const int32 Code = Commandlet->Main(FString::Printf(TEXT("-Decompile -Asset=\"%s\" -Out=\"%s\" -AnimSet=\"%s\""), *Blueprint->GetPathName(), *Output, *Target));
		TestTrue(TEXT("replacing the source is refused"), Code != 0);
	}
	FString After;
	FFileHelper::LoadFileToString(After, *Output);
	TestEqualSensitive(TEXT("the source is unchanged"), After, Original);
	TestTrue(TEXT("the existing set gains nothing"), Set->Groups.IsEmpty());
	TestFalse(TEXT("and stays clean"), Set->GetPackage()->IsDirty());
	TestNull(TEXT("the missing set is not created"), FindObject<UDreamAnimSet>(nullptr, TEXT("/Game/DreamAnimTests/Migration/DAS_NeverCreated.DAS_NeverCreated")));
	return true;
}

#endif
