// Copyright DreamMoon. All Rights Reserved.
//
// Generate: a model built by hand into both asset kinds and run; the same model generated over itself
// without breaking a rig that calls into it; and the engine's own rigs read into the model, generated into
// another asset, read again -- the two models must dump alike, and a rig that runs on its own must run
// as its original does.

#include "DcrCorpus.h"
#include "DcrRunHarness.h"

#include "ControlRig.h"
#include "Misc/AutomationTest.h"
#include "Model/DcrModel.h"
#include "Rig/DcrAsset.h"
#include "Rig/DcrExtract.h"
#include "Rig/DcrGenerate.h"
#include "Rig/DcrLower.h"
#include "Rig/DcrNormalize.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/RigVMGraph.h"
#include "Rigs/RigHierarchy.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Rig::Tests::GeneratePrivate
{
	using namespace UE::DreamAnim::Model;

	constexpr int32 MaxLinesPerReport = 20;

	FDcrType MakeType(const TCHAR* CPPType, const TCHAR* ObjectPath = TEXT(""))
	{
		FDcrType Type;
		Type.CPPType = CPPType;
		Type.ObjectPath = ObjectPath;
		return Type;
	}

	FDcrModelNode MakeNode(const TCHAR* Name, EDcrNodeKind Kind)
	{
		FDcrModelNode Node;
		Node.Name = Name;
		Node.Kind = Kind;
		return Node;
	}

	FDcrModelNode MakeUnit(const TCHAR* Name, const TCHAR* StructPath)
	{
		FDcrModelNode Node = MakeNode(Name, EDcrNodeKind::Unit);
		Node.StructPath = StructPath;
		return Node;
	}

	FDcrModelLink MakeLink(const TCHAR* Source, const TCHAR* Target)
	{
		FDcrModelLink Link;
		Link.Source = Source;
		Link.Target = Target;
		return Link;
	}

	FDcrValue MakeValue(const TCHAR* Name, const TCHAR* Value)
	{
		FDcrValue Out;
		Out.Name = Name;
		Out.Value = Value;
		return Out;
	}

	/**
	 * A null with a control under it; variables Offset (5) and Result; a public mutable function `My Func` that
	 * doubles its input; and the default graph: Forwards Solve, N1 moved up by Offset, the function called with
	 * Offset, its result in Result.
	 */
	FDcrModel MakeHandBuiltModel(const FString& AssetPath, EDcrModelAssetKind Kind)
	{
		FDcrModel Model;
		Model.AssetPath = AssetPath;
		Model.AssetKind = Kind;

		FDcrModelElement Null;
		Null.Kind = EDcrElementKind::Null;
		Null.Name = TEXT("N1");
		Model.Hierarchy.Add(Null);

		FDcrModelElement Control;
		Control.Kind = EDcrElementKind::Control;
		Control.Name = TEXT("C1");
		Control.ParentKind = EDcrElementKind::Null;
		Control.Parent = TEXT("N1");
		Control.Properties.Add(MakeValue(TEXT("ControlType"), TEXT("EulerTransform")));
		Control.Properties.Add(MakeValue(TEXT("ShapeName"), TEXT("Circle_Thick")));
		Model.Hierarchy.Add(Control);

		FDcrModelVariable Offset;
		Offset.Name = TEXT("Offset");
		Offset.Type = MakeType(TEXT("double"));
		Offset.Default = TEXT("5.000000");
		Offset.bPublic = true;
		Model.Variables.Add(Offset);
		FDcrModelVariable Result;
		Result.Name = TEXT("Result");
		Result.Type = MakeType(TEXT("double"));
		Model.Variables.Add(Result);

		FDcrModelFunction Function;
		Function.Name = TEXT("My Func");
		Function.bMutable = true;
		Function.bPublic = true;
		Function.Category = TEXT("Tests");
		Function.Description = TEXT("Doubles a number.");
		FDcrModelFunctionPin In;
		In.Name = TEXT("In Value");
		In.Type = MakeType(TEXT("double"));
		In.Default = TEXT("1.000000");
		Function.Inputs.Add(In);
		FDcrModelFunctionPin Out;
		Out.Name = TEXT("Out Value");
		Out.Type = MakeType(TEXT("double"));
		Function.Outputs.Add(Out);
		{
			FDcrModelGraph& Graph = Function.Graph;
			Graph.Name = Function.Name;
			FDcrModelNode Entry = MakeNode(TEXT("Entry"), EDcrNodeKind::Entry);
			Entry.ExecuteOut.Add(TEXT("ExecuteContext"));
			Graph.Nodes.Add(MoveTemp(Entry));
			FDcrModelNode Return = MakeNode(TEXT("Return"), EDcrNodeKind::Return);
			Return.ExecuteIn.Add(TEXT("ExecuteContext"));
			Graph.Nodes.Add(MoveTemp(Return));
			FDcrModelNode Mul = MakeNode(TEXT("Mul"), EDcrNodeKind::Dispatch);
			Mul.Notation = TEXT("Multiply::Execute(in A,in B,out Result)");
			Mul.ResolvedTypes.Emplace(TEXT("A"), MakeType(TEXT("double")));
			Mul.Values.Add(MakeValue(TEXT("B"), TEXT("2.000000")));
			Graph.Nodes.Add(MoveTemp(Mul));
			Graph.Links.Add(MakeLink(TEXT("Entry.ExecuteContext"), TEXT("Return.ExecuteContext")));
			Graph.Links.Add(MakeLink(TEXT("Entry.In Value"), TEXT("Mul.A")));
			Graph.Links.Add(MakeLink(TEXT("Mul.Result"), TEXT("Return.Out Value")));
		}
		Model.Functions.Add(MoveTemp(Function));

		FDcrModelGraph& Main = Model.Graphs.AddDefaulted_GetRef();
		Main.Name = TEXT("RigVMModel");
		const TCHAR* const Item = TEXT("(Type=Null,Name=\"N1\")");
		{
			FDcrModelNode Begin = MakeUnit(TEXT("BeginExecution"), TEXT("/Script/ControlRig.RigUnit_BeginExecution"));
			Begin.ExecuteOut.Add(TEXT("ExecutePin"));
			Main.Nodes.Add(MoveTemp(Begin));

			FDcrModelNode GetN1 = MakeUnit(TEXT("GetN1"), TEXT("/Script/ControlRig.RigUnit_GetTransform"));
			GetN1.Values.Add(MakeValue(TEXT("Item"), Item));
			Main.Nodes.Add(MoveTemp(GetN1));

			FDcrModelNode GetOffset = MakeNode(TEXT("GetOffset"), EDcrNodeKind::VariableGet);
			GetOffset.Variable = TEXT("Offset");
			GetOffset.VariableType = MakeType(TEXT("double"));
			Main.Nodes.Add(MoveTemp(GetOffset));

			Main.Nodes.Add(MakeUnit(TEXT("AddVec"), TEXT("/Script/RigVM.RigVMFunction_MathVectorAdd")));

			FDcrModelNode SetN1 = MakeUnit(TEXT("SetN1"), TEXT("/Script/ControlRig.RigUnit_SetTranslation"));
			SetN1.Values.Add(MakeValue(TEXT("Item"), Item));
			SetN1.ExecuteIn.Add(TEXT("ExecuteContext"));
			SetN1.ExecuteOut.Add(TEXT("ExecuteContext"));
			Main.Nodes.Add(MoveTemp(SetN1));

			FDcrModelNode GetOffsetAgain = MakeNode(TEXT("GetOffsetAgain"), EDcrNodeKind::VariableGet);
			GetOffsetAgain.Variable = TEXT("Offset");
			GetOffsetAgain.VariableType = MakeType(TEXT("double"));
			Main.Nodes.Add(MoveTemp(GetOffsetAgain));

			FDcrModelNode Call = MakeNode(TEXT("CallMyFunc"), EDcrNodeKind::LocalCall);
			Call.Function = TEXT("My Func");
			Main.Nodes.Add(MoveTemp(Call));

			FDcrModelNode SetResult = MakeNode(TEXT("SetResult"), EDcrNodeKind::VariableSet);
			SetResult.Variable = TEXT("Result");
			SetResult.VariableType = MakeType(TEXT("double"));
			Main.Nodes.Add(MoveTemp(SetResult));

			Main.Links.Add(MakeLink(TEXT("BeginExecution.ExecutePin"), TEXT("SetN1.ExecuteContext")));
			Main.Links.Add(MakeLink(TEXT("GetN1.Transform.Translation"), TEXT("AddVec.A")));
			Main.Links.Add(MakeLink(TEXT("GetOffset.Value"), TEXT("AddVec.B.Z")));
			Main.Links.Add(MakeLink(TEXT("AddVec.Result"), TEXT("SetN1.Value")));
			Main.Links.Add(MakeLink(TEXT("SetN1.ExecuteContext"), TEXT("CallMyFunc.ExecuteContext")));
			Main.Links.Add(MakeLink(TEXT("GetOffsetAgain.Value"), TEXT("CallMyFunc.In Value")));
			Main.Links.Add(MakeLink(TEXT("CallMyFunc.ExecuteContext"), TEXT("SetResult.ExecuteContext")));
			Main.Links.Add(MakeLink(TEXT("CallMyFunc.Out Value"), TEXT("SetResult.Value")));
		}
		return Model;
	}

	/** A rig with one event that calls `My Func` of another rig. */
	FDcrModel MakeCallerModel(const FString& AssetPath, EDcrModelAssetKind Kind, const FString& HostPath)
	{
		FDcrModel Model;
		Model.AssetPath = AssetPath;
		Model.AssetKind = Kind;
		FDcrModelGraph& Main = Model.Graphs.AddDefaulted_GetRef();
		Main.Name = TEXT("RigVMModel");
		Main.Nodes.Add(MakeUnit(TEXT("BeginExecution"), TEXT("/Script/ControlRig.RigUnit_BeginExecution")));
		FDcrModelNode Call = MakeNode(TEXT("Call"), EDcrNodeKind::ExternalCall);
		Call.Function = TEXT("My Func");
		Call.FunctionHost = HostPath;
		Call.Values.Add(MakeValue(TEXT("In Value"), TEXT("3.000000")));
		Main.Nodes.Add(MoveTemp(Call));
		Main.Links.Add(MakeLink(TEXT("BeginExecution.ExecutePin"), TEXT("Call.ExecuteContext")));
		return Model;
	}

	/** Puts what a generation said into the test: every error, a few warnings as information. */
	bool ReportGeneration(FAutomationTestBase& Test, const FString& Label, const FDcrGenerateResult& Result)
	{
		for (int32 Index = 0; Index < Result.Errors.Num() && Index < MaxLinesPerReport; ++Index)
		{
			Test.AddError(FString::Printf(TEXT("%s: %s"), *Label, *Result.Errors[Index]));
		}
		if (Result.Errors.Num() > MaxLinesPerReport)
		{
			Test.AddError(FString::Printf(TEXT("%s: %d more errors"), *Label, Result.Errors.Num() - MaxLinesPerReport));
		}
		for (int32 Index = 0; Index < Result.Warnings.Num() && Index < MaxLinesPerReport; ++Index)
		{
			Test.AddInfo(FString::Printf(TEXT("%s (warning): %s"), *Label, *Result.Warnings[Index]));
		}
		return Result.Succeeded();
	}

	/** The lines of A that B lacks (`-`) and the lines of B that A lacks (`+`), in order, at most Max. */
	TArray<FString> DifferingLines(const FString& A, const FString& B, int32 Max)
	{
		TArray<FString> LinesA;
		TArray<FString> LinesB;
		A.ParseIntoArrayLines(LinesA, false);
		B.ParseIntoArrayLines(LinesB, false);
		TMap<FString, int32> CountA;
		TMap<FString, int32> CountB;
		for (const FString& Line : LinesA)
		{
			++CountA.FindOrAdd(Line);
		}
		for (const FString& Line : LinesB)
		{
			++CountB.FindOrAdd(Line);
		}
		TArray<FString> Out;
		int32 Total = 0;
		for (const FString& Line : LinesA)
		{
			int32* Other = CountB.Find(Line);
			if (Other && *Other > 0)
			{
				--*Other;
			}
			else
			{
				++Total;
				if (Out.Num() < Max)
				{
					Out.Add(TEXT("- ") + Line);
				}
			}
		}
		for (const FString& Line : LinesB)
		{
			int32* Other = CountA.Find(Line);
			if (Other && *Other > 0)
			{
				--*Other;
			}
			else
			{
				++Total;
				if (Out.Num() < Max)
				{
					Out.Add(TEXT("+ ") + Line);
				}
			}
		}
		if (Total > Out.Num())
		{
			Out.Add(FString::Printf(TEXT("... and %d more differing lines"), Total - Out.Num()));
		}
		return Out;
	}

	bool HasFloatMath(const FDcrModelGraph& Graph)
	{
		for (const FDcrModelNode& Node : Graph.Nodes)
		{
			if (Node.StructPath.Contains(TEXT("RigVMFunction_MathFloat")) || (Node.Contained && HasFloatMath(*Node.Contained)))
			{
				return true;
			}
		}
		return false;
	}

	bool HasFloatMath(const FDcrModel& Model)
	{
		for (const FDcrModelGraph& Graph : Model.Graphs)
		{
			if (HasFloatMath(Graph))
			{
				return true;
			}
		}
		for (const FDcrModelFunction& Function : Model.Functions)
		{
			if (HasFloatMath(Function.Graph))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * One corpus rig through the whole circle. Returns whether it held; says why not through the test.
	 * Rigs that run on their own are also run next to a recompiled copy of the original.
	 */
	bool RoundTripRig(FAutomationTestBase& Test, const FDcrCorpusEntry& Entry, bool bRunBehavior)
	{
		const FString Name = Entry.Name;
		FDcrRig Original = LoadDcrRig(Entry.ObjectPath);
		if (!Original.IsValid())
		{
			Test.AddError(FString::Printf(TEXT("%s: the corpus rig cannot be loaded (%s)"), *Name, Entry.ObjectPath));
			return false;
		}

		FDcrModel Model;
		const FDcrExtractResult Extracted = ExtractDcrModel(Original, Model);
		if (!Extracted.bSuccess)
		{
			Test.AddError(FString::Printf(TEXT("%s: extract failed: %s"), *Name, *Extracted.Error));
			return false;
		}
		NormalizeDcrModel(Model);

		FDcrDumpOptions DumpOptions;
		DumpOptions.bCanonicalNodeNames = true;
		DumpOptions.bFloatAsDouble = true;
		const FString OriginalPath = Model.AssetPath;
		const FString Expected = DumpDcrModel(Model, DumpOptions);
		const bool bFloatMath = HasFloatMath(Model);

		// Parts the text cannot say (a node the engine has no struct for, a unit only module rigs may hold) need not
		// build again or come back: as through the text, the rig is still built and compared, and what differs is told.
		const int32 Unsupported = LowerDcrModel(Model).Unsupported.Num();

		Model.AssetPath = FString::Printf(TEXT("/Game/DreamAnimRigTests/Generate/RoundTrip/%s.%s"), *Name, *Name);
		const FDcrGenerateResult Generated = GenerateDcrRig(Model, FDcrGenerateOptions());
		if (Unsupported > 0)
		{
			FDcrModel Again;
			if (Generated.Rig.IsValid() && ExtractDcrModel(Generated.Rig, Again).bSuccess)
			{
				NormalizeDcrModel(Again);
				Again.AssetPath = OriginalPath;
			}
			const TArray<FString> Differences = DifferingLines(Expected, DumpDcrModel(Again, DumpOptions), MAX_int32);
			Test.AddWarning(FString::Printf(TEXT("%s holds %d things the text cannot say: generating it gave %d errors, and %d lines of its model differ"),
				*Name, Unsupported, Generated.Errors.Num(), Differences.Num()));
			return true;
		}
		bool bHeld = ReportGeneration(Test, Name + TEXT(" generate"), Generated);
		if (!Generated.Rig.IsValid())
		{
			return false;
		}

		FDcrModel Again;
		const FDcrExtractResult ExtractedAgain = ExtractDcrModel(Generated.Rig, Again);
		if (!ExtractedAgain.bSuccess)
		{
			Test.AddError(FString::Printf(TEXT("%s: the generated rig cannot be extracted: %s"), *Name, *ExtractedAgain.Error));
			return false;
		}
		NormalizeDcrModel(Again);
		Again.AssetPath = OriginalPath;
		const TArray<FString> Differences = DifferingLines(Expected, DumpDcrModel(Again, DumpOptions), MaxLinesPerReport);
		if (!Differences.IsEmpty())
		{
			bHeld = false;
			Test.AddError(FString::Printf(TEXT("%s: the generated rig differs from the original; first lines:"), *Name));
			for (const FString& Line : Differences)
			{
				Test.AddInfo(FString::Printf(TEXT("    %s"), *Line));
			}
		}

		if (bRunBehavior)
		{
			FDcrRig Baseline = DuplicateDcrRig(Original, TEXT("/Game/DreamAnimRigTests/Generate/Baseline"), Name);
			if (!Baseline.IsValid())
			{
				Test.AddError(FString::Printf(TEXT("%s: the baseline copy could not be made"), *Name));
				return false;
			}
			// The original recompiled is the baseline: a rig as the engine last saved it can differ from itself recompiled by rounding.
			CompileDcrRig(Baseline);
			const FDcrRun BaselineRun = RunDcrRig(Baseline, 60, DriveDcrDefault);
			const FDcrRun GeneratedRun = RunDcrRig(Generated.Rig, 60, DriveDcrDefault);
			const FDcrRunDiff Diff = CompareDcrRuns(BaselineRun, GeneratedRun);
			// A rig rebuilt from its own model can still differ by rounding (a global transform worked out again from
			// its local one): 1e-4 holds; bit for bit is reported.
			if (!Diff.IsWithin(1e-4))
			{
				bHeld = false;
				Test.AddError(FString::Printf(TEXT("%s: runs differently from its original past 1e-4%s: %s"), *Name, bFloatMath ? TEXT(" (float math)") : TEXT(""), *Diff.Describe()));
			}
			else
			{
				Test.AddInfo(FString::Printf(TEXT("%s: runs as its original does, %s (%s)"), *Name, Diff.IsExact() ? TEXT("bit for bit") : TEXT("within 1e-4"), *Diff.Describe()));
			}
		}
		return bHeld;
	}

	bool RoundTripGroup(FAutomationTestBase& Test, EDcrCorpusGroup Group, bool bRunBehavior)
	{

		int32 Count = 0;
		int32 Held = 0;
		for (const FDcrCorpusEntry& Entry : GetDcrCorpus())
		{
			if (Entry.Group != Group)
			{
				continue;
			}
			++Count;
			Held += RoundTripRig(Test, Entry, bRunBehavior) ? 1 : 0;
		}
		Test.AddInfo(FString::Printf(TEXT("%d of %d rigs came back alike"), Held, Count));
		return Held == Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrGenerateHandBuiltTest, "DreamAnim.Rig.Generate.HandBuilt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** A small model built by hand into both asset kinds: no errors, compiles, and moves N1 by Offset a frame with Result set by the function. */
bool FDcrGenerateHandBuiltTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Model;
	using namespace UE::DreamAnim::Rig;
	using namespace UE::DreamAnim::Rig::Tests;
	using namespace UE::DreamAnim::Rig::Tests::GeneratePrivate;

	for (const EDcrModelAssetKind Kind : { EDcrModelAssetKind::Independent, EDcrModelAssetKind::Legacy })
	{
		const FString Label = Kind == EDcrModelAssetKind::Legacy ? TEXT("Legacy") : TEXT("Independent");
		const FString Path = FString::Printf(TEXT("/Game/DreamAnimRigTests/Generate/HandBuilt/HandBuilt_%s.HandBuilt_%s"), *Label, *Label);
		const FDcrModel Model = MakeHandBuiltModel(Path, Kind);

		const FDcrGenerateResult Result = GenerateDcrRig(Model, FDcrGenerateOptions());
		if (!TestTrue(Label + TEXT(": generated without errors"), ReportGeneration(*this, Label, Result)))
		{
			continue;
		}
		TestTrue(Label + TEXT(": the asset kind"), Result.Rig.GetKind() == (Kind == EDcrModelAssetKind::Legacy ? EDcrAssetKind::Legacy : EDcrAssetKind::Independent));
		TestTrue(Label + TEXT(": the asset path"), Result.Rig.Asset->GetPathName() == Path);

		const FDcrRun Run = RunDcrRig(Result.Rig, 3, [](UControlRig&, int32) {});
		if (!TestTrue(Label + TEXT(": ran three frames"), Run.Error.IsEmpty() && Run.Frames.Num() == 3))
		{
			AddError(Label + TEXT(": ") + Run.Error);
			continue;
		}

		const int32 N1 = Run.Keys.IndexOfByKey(FRigElementKey(TEXT("N1"), ERigElementType::Null));
		if (TestTrue(Label + TEXT(": N1 is in the hierarchy"), N1 != INDEX_NONE))
		{
			for (int32 Frame = 0; Frame < Run.Frames.Num(); ++Frame)
			{
				TestEqual(FString::Printf(TEXT("%s: N1 height after frame %d"), *Label, Frame + 1), Run.Frames[Frame].Transforms[N1].GetTranslation().Z, 5.0 * (Frame + 1), 1e-3);
			}
		}
		TestTrue(Label + TEXT(": C1 is in the hierarchy"), Run.Keys.Contains(FRigElementKey(TEXT("C1"), ERigElementType::Control)));

		const int32 ResultVariable = Run.VariableNames.IndexOfByKey(FName(TEXT("Result")));
		if (TestTrue(Label + TEXT(": Result is a variable"), ResultVariable != INDEX_NONE))
		{
			TestEqual(Label + TEXT(": Result"), FCString::Atod(*Run.Frames.Last().Variables[ResultVariable]), 10.0, 1e-6);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrGenerateRebuildTest, "DreamAnim.Rig.Generate.RebuildInPlace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** The same model generated twice into one path: the same asset object, and a rig that calls its public function by an external call keeps the call. */
bool FDcrGenerateRebuildTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Model;
	using namespace UE::DreamAnim::Rig;
	using namespace UE::DreamAnim::Rig::Tests;
	using namespace UE::DreamAnim::Rig::Tests::GeneratePrivate;

	for (const EDcrModelAssetKind Kind : { EDcrModelAssetKind::Independent, EDcrModelAssetKind::Legacy })
	{
		const FString Label = Kind == EDcrModelAssetKind::Legacy ? TEXT("Legacy") : TEXT("Independent");
		const FString CalleePath = FString::Printf(TEXT("/Game/DreamAnimRigTests/Generate/Rebuild/Callee_%s.Callee_%s"), *Label, *Label);
		const FString CallerPath = FString::Printf(TEXT("/Game/DreamAnimRigTests/Generate/Rebuild/Caller_%s.Caller_%s"), *Label, *Label);

		const FDcrGenerateResult First = GenerateDcrRig(MakeHandBuiltModel(CalleePath, Kind), FDcrGenerateOptions());
		if (!TestTrue(Label + TEXT(": first generation"), ReportGeneration(*this, Label + TEXT(" first"), First)))
		{
			continue;
		}
		const FDcrGenerateResult Caller = GenerateDcrRig(MakeCallerModel(CallerPath, Kind, First.Rig.Asset->GetPathName()), FDcrGenerateOptions());
		if (!TestTrue(Label + TEXT(": the caller"), ReportGeneration(*this, Label + TEXT(" caller"), Caller)))
		{
			continue;
		}

		const FDcrGenerateResult Second = GenerateDcrRig(MakeHandBuiltModel(CalleePath, Kind), FDcrGenerateOptions());
		TestTrue(Label + TEXT(": second generation"), ReportGeneration(*this, Label + TEXT(" second"), Second));
		TestTrue(Label + TEXT(": the asset object is the same"), Second.Rig.Asset == First.Rig.Asset);
		TestTrue(Label + TEXT(": no caller was broken"), Second.BrokenCallers.IsEmpty());
		for (const FString& Broken : Second.BrokenCallers)
		{
			AddError(Label + TEXT(": broken caller ") + Broken);
		}

		URigVMGraph* Graph = FindDcrModelGraph(Caller.Rig, TEXT("RigVMModel"));
		URigVMNode* Call = Graph ? Graph->FindNodeByName(FName(TEXT("Call"))) : nullptr;
		TestTrue(Label + TEXT(": the caller's call node still exists"), Cast<URigVMFunctionReferenceNode>(Call) != nullptr);
		const TArray<FString> CompileErrors = CompileDcrRig(Caller.Rig);
		TestTrue(Label + TEXT(": the caller compiles"), CompileErrors.IsEmpty());
		for (const FString& Error : CompileErrors)
		{
			AddError(Label + TEXT(": caller compile: ") + Error);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrGenerateRoundTripStandaloneTest, "DreamAnim.Rig.Generate.RoundTrip.Standalone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** The rigs that run on their own: the circle, and 60 frames next to the recompiled original. */
bool FDcrGenerateRoundTripStandaloneTest::RunTest(const FString& Parameters)
{
	return UE::DreamAnim::Rig::Tests::GeneratePrivate::RoundTripGroup(*this, UE::DreamAnim::Rig::Tests::EDcrCorpusGroup::Standalone, true);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrGenerateRoundTripLibraryTest, "DreamAnim.Rig.Generate.RoundTrip.Library",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** The root module and the function libraries: the circle. */
bool FDcrGenerateRoundTripLibraryTest::RunTest(const FString& Parameters)
{
	return UE::DreamAnim::Rig::Tests::GeneratePrivate::RoundTripGroup(*this, UE::DreamAnim::Rig::Tests::EDcrCorpusGroup::Library, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrGenerateRoundTripModuleTest, "DreamAnim.Rig.Generate.RoundTrip.Module",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** The modular rig modules: the circle. Module settings are not applied by the generator yet, so those lines can differ. */
bool FDcrGenerateRoundTripModuleTest::RunTest(const FString& Parameters)
{
	return UE::DreamAnim::Rig::Tests::GeneratePrivate::RoundTripGroup(*this, UE::DreamAnim::Rig::Tests::EDcrCorpusGroup::Module, false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrGenerateRoundTripIndependentTest, "DreamAnim.Rig.Generate.RoundTrip.Independent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** The rigs already in the independent kind: the circle. */
bool FDcrGenerateRoundTripIndependentTest::RunTest(const FString& Parameters)
{
	return UE::DreamAnim::Rig::Tests::GeneratePrivate::RoundTripGroup(*this, UE::DreamAnim::Rig::Tests::EDcrCorpusGroup::Independent, false);
}

#endif
