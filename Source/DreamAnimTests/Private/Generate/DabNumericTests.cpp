// Copyright DreamMoon. All Rights Reserved.
//
// Integers through checking, generation and decompiling: a comparison widens to fit its constant, a
// conditional picks byte and int64 values, and int64 constants keep every digit, also above 2^53 where a
// double no longer holds every integer. Each expression is run, decompiled, built again and run again.

#include "Animation/AnimBlueprint.h"
#include "Driver/DabDriver.h"
#include "Misc/AutomationTest.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Tests::NumericPrivate
{
	using namespace UE::DreamAnim;
	using namespace UE::DreamAnim::Compiler;

	FString MakeSource(const FString& Name, const FString& Body)
	{
		return FString::Printf(TEXT("AnimBlueprint(Name=\"DreamAnimTests/Numbers/%s\", Root=\"Game\")\n{\n    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }\n    %s\n}\n"),
			*Name, *Body);
	}

	UAnimBlueprint* Build(FAutomationTestBase& Test, const FString& Text)
	{
		FDabCompileOptions Options;
		Options.bSave = false;
		const FDabCompileResult Result = CompileDabText(TEXT("Numbers.dab"), Text, Options);
		for (const Lang::FDabDiagnostic& Diagnostic : Result.Diagnostics)
		{
			if (Diagnostic.Severity == Lang::EDabSeverity::Error)
			{
				Test.AddError(FormatDabDiagnosticLine(Diagnostic));
			}
		}
		return Result.Succeeded() ? Result.Blueprint : nullptr;
	}

	/** Runs the Blueprint's one expression function on its class defaults; false when there is not exactly one. */
	bool RunExpression(FAutomationTestBase& Test, const UAnimBlueprint& Blueprint, double& OutValue)
	{
		UFunction* Function = nullptr;
		for (TFieldIterator<UFunction> It(Blueprint.GeneratedClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			if (It->GetName().StartsWith(TEXT("DreamAnimExpr_")))
			{
				if (Function)
				{
					Test.AddError(TEXT("more than one expression function"));
					return false;
				}
				Function = *It;
			}
		}
		const FNumericProperty* Return = Function ? CastField<FNumericProperty>(Function->GetReturnProperty()) : nullptr;
		if (!Test.TestNotNull(TEXT("an expression function with a numeric result"), Return))
		{
			return false;
		}
		FStructOnScope Arguments(Function);
		Blueprint.GeneratedClass->GetDefaultObject()->ProcessEvent(Function, Arguments.GetStructMemory());
		const void* Value = Return->ContainerPtrToValuePtr<void>(Arguments.GetStructMemory());
		OutValue = Return->IsFloatingPoint() ? Return->GetFloatingPointPropertyValue(Value) : double(Return->GetSignedIntPropertyValue(Value));
		return true;
	}

	/**
	 * Builds a Blueprint whose Alpha is Expression, checks the value it computes, then decompiles it, builds the
	 * text again and checks the rebuilt one computes the same.
	 */
	void ExpectValue(FAutomationTestBase& Test, const FString& Name, const FString& Properties, const FString& Expression, double Expected)
	{
		const FString Body = FString::Printf(TEXT("Properties = { %s } Graph = { Output = TwoWayBlend(A = LocalRefPose(), B = LocalRefPose(), Alpha = %s); }"),
			*Properties, *Expression);
		const UAnimBlueprint* Blueprint = Build(Test, MakeSource(Name, Body));
		double Value = 0.0;
		if (!Blueprint || !RunExpression(Test, *Blueprint, Value))
		{
			return;
		}
		Test.AddInfo(FString::Printf(TEXT("%s: %s gives %.17g"), *Name, *Expression, Value));
		Test.TestEqual(FString::Printf(TEXT("%s gives %.17g"), *Expression, Expected), Value, Expected);

		const FDabDecompileResult Decompiled = DecompileDabBlueprint(*Blueprint);
		Test.TestEqual(FString::Printf(TEXT("%s decompiles completely"), *Expression), Decompiled.NumUnsupported, 0);
		const UAnimBlueprint* Rebuilt = Build(Test, Decompiled.Text);
		double RebuiltValue = 0.0;
		if (Rebuilt && RunExpression(Test, *Rebuilt, RebuiltValue))
		{
			Test.TestEqual(FString::Printf(TEXT("%s gives the same after a round trip"), *Expression), RebuiltValue, Expected);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabConditionalInt64Test, "DreamAnim.Generate.ConditionalInt64",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/** `Pick ? Count : 0` with an int64 Count. */
bool FDabConditionalInt64Test::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::NumericPrivate;
	ExpectValue(*this, TEXT("AB_ConditionalInt64"), TEXT("bool Pick = true; int64 Count = 42;"), TEXT("Props.Pick ? Props.Count : 0"), 42.0);
	ExpectValue(*this, TEXT("AB_ConditionalInt64Else"), TEXT("bool Pick = false; int64 Count = 42;"), TEXT("Props.Pick ? Props.Count : 7"), 7.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabConditionalByteTest, "DreamAnim.Generate.ConditionalByte",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/** `Pick ? Count : 0` with a byte Count. */
bool FDabConditionalByteTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::NumericPrivate;
	ExpectValue(*this, TEXT("AB_ConditionalByte"), TEXT("bool Pick = true; byte Count = 42;"), TEXT("Props.Pick ? Props.Count : 0"), 42.0);
	ExpectValue(*this, TEXT("AB_ConditionalByteElse"), TEXT("bool Pick = false; byte Count = 42;"), TEXT("Props.Pick ? Props.Count : 255"), 255.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabComparisonConstantRangeTest, "DreamAnim.Generate.ComparisonConstantRange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/**
 * A comparison whose constant does not fit the variable's type is made in a wider type, not with the
 * constant cut down: a byte against 256 or -1, an int against 2^31. Both orders of the operands, and the
 * constants at the edge of the type.
 */
bool FDabComparisonConstantRangeTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::NumericPrivate;
	const TCHAR* const Properties = TEXT("byte Small = 1; int Count = 1;");
	const TPair<const TCHAR*, double> Cases[] = {
		{ TEXT("Props.Small < 256"), 1.0 },
		{ TEXT("256 > Props.Small"), 1.0 },
		{ TEXT("Props.Small > -1"), 1.0 },
		{ TEXT("-1 < Props.Small"), 1.0 },
		{ TEXT("Props.Small <= 255"), 1.0 },
		{ TEXT("Props.Small > 0"), 1.0 },
		{ TEXT("Props.Count < 2147483648"), 1.0 },
		{ TEXT("Props.Count > -2147483648"), 1.0 },
		{ TEXT("Props.Count > 2147483647"), 0.0 },
	};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Cases); ++Index)
	{
		ExpectValue(*this, FString::Printf(TEXT("AB_Range%d"), Index), Properties, FString::Printf(TEXT("(%s) ? 1 : 0"), Cases[Index].Key), Cases[Index].Value);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabInt64ExactTest, "DreamAnim.Generate.IntegerBounds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

/**
 * int64 constants keep every digit: as tuning value defaults in every spelling the language reads, written
 * back by the decompiler, and inside expressions. A value outside int64 or with a fraction is refused, with
 * a message that says which.
 */
bool FDabInt64ExactTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests::NumericPrivate;
	const TPair<const TCHAR*, int64> Literals[] = {
		{ TEXT("9007199254740993"), 9007199254740993LL },
		{ TEXT("-9007199254740993"), -9007199254740993LL },
		{ TEXT("9223372036854775807"), MAX_int64 },
		{ TEXT("-9223372036854775808"), MIN_int64 },
		{ TEXT("9.007199254740993e15"), 9007199254740993LL },
		{ TEXT("9007199254740993.0"), 9007199254740993LL },
	};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Literals); ++Index)
	{
		const UAnimBlueprint* Blueprint = Build(*this, MakeSource(FString::Printf(TEXT("AB_IntegerBounds%d"), Index),
			FString::Printf(TEXT("Properties = { int64 Exact = %s; } Graph = { Output = LocalRefPose(); }"), Literals[Index].Key)));
		const FInt64Property* Property = Blueprint ? FindFProperty<FInt64Property>(Blueprint->GeneratedClass, TEXT("Exact")) : nullptr;
		if (!TestNotNull(FString::Printf(TEXT("%s builds"), Literals[Index].Key), Property))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s is exact"), Literals[Index].Key), Property->GetPropertyValue_InContainer(Blueprint->GeneratedClass->GetDefaultObject()),
			Literals[Index].Value);
		TestTrue(FString::Printf(TEXT("%s decompiles exactly"), Literals[Index].Key),
			DecompileDabBlueprint(*Blueprint).Text.Contains(FString::Printf(TEXT("%lld"), Literals[Index].Value)));
	}

	FDabCompileOptions CheckOnly;
	CheckOnly.bGenerate = false;
	const TPair<const TCHAR*, const TCHAR*> Refused[] = {
		{ TEXT("9223372036854775808"), TEXT("must lie in") },
		{ TEXT("-9223372036854775809"), TEXT("must lie in") },
		{ TEXT("1.5"), TEXT("whole number") },
	};
	for (const TPair<const TCHAR*, const TCHAR*>& Case : Refused)
	{
		const FDabCompileResult Result = CompileDabText(TEXT("Numbers.dab"),
			MakeSource(TEXT("AB_Refused"), FString::Printf(TEXT("Properties = { int64 Exact = %s; } Graph = { Output = LocalRefPose(); }"), Case.Key)), CheckOnly);
		const bool bSaysWhy = Result.Diagnostics.ContainsByPredicate([&Case](const Lang::FDabDiagnostic& Diagnostic)
		{
			return Diagnostic.Severity == Lang::EDabSeverity::Error && Diagnostic.Message.Contains(Case.Value);
		});
		TestTrue(FString::Printf(TEXT("%s is refused, saying it %s"), Case.Key, Case.Value), bSaysWhy);
	}

	ExpectValue(*this, TEXT("AB_ExactComparison"), TEXT("int64 Exact = 9007199254740993;"), TEXT("(Props.Exact == 9007199254740992) ? 1 : 0"), 0.0);
	ExpectValue(*this, TEXT("AB_ExactConditional"), TEXT("bool Pick = true; int64 Exact = 9007199254740993;"),
		TEXT("((Props.Pick ? Props.Exact : 0) == 9007199254740993) ? 1 : 0"), 1.0);
	return true;
}

#endif
