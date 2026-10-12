// Copyright DreamMoon. All Rights Reserved.
//
// Write-back of Control Rigs (DreamAnim.Rig.WriteBack.*): a rig is built from a small text, changed in the
// editor's own ways (a pin's value, a variable's default, a control's settings, an element's initial
// transform, a node taken away), and its change written into the text. Every case checks that the new text
// builds to what the rig holds, and how much of the text moved: a value changes its one line, a structural
// change the block it is in, and nothing else.

#include "DcrCorpus.h"

#include "Lang/DabParser.h"
#include "Lang/DabPrinter.h"
#include "Lang/DabTemplates.h"
#include "Misc/AutomationTest.h"
#include "Model/DcrModel.h"
#include "Rig/DcrAsset.h"
#include "Rig/DcrExtract.h"
#include "Rig/DcrGenerate.h"
#include "Rig/DcrLower.h"
#include "Rig/DcrNormalize.h"
#include "Rig/DcrSemantic.h"
#include "Rig/DcrWriteBack.h"
#include "RigVMCore/RigVMPropertyBag.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyController.h"
#include "RigVMModel/Nodes/RigVMUnitNode.h"
#include "RigVMModel/Nodes/RigVMVariableNode.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"
#include "Engine/Blueprint.h"

namespace UE::DreamAnim::Rig::Tests
{
	using namespace UE::DreamAnim::Compiler;
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	namespace
	{
		const TCHAR* const WriteBackText =
			TEXT("ControlRig(Name=\"DreamAnimRigTests/WriteBack/CR_WriteBack\")\n")
			TEXT("{\n")
			TEXT("    Hierarchy = {\n")
			TEXT("        Null pivot { Transform = (Translation = (X = 10)); }\n")
			TEXT("        Control handle\n")
			TEXT("        {\n")
			TEXT("            Parent     = Null(pivot);\n")
			TEXT("            ShapeName  = \"Circle_Thick\";\n")
			TEXT("            ShapeColor = (1, 0, 0, 1);\n")
			TEXT("        }\n")
			TEXT("    }\n")
			TEXT("\n")
			TEXT("    Variables = {\n")
			TEXT("        double Speed   = 2;\n")
			TEXT("        bool   Enabled = true [ Public ];\n")
			TEXT("    }\n")
			TEXT("\n")
			TEXT("    // What the rig does each frame.\n")
			TEXT("    Event Forward\n")
			TEXT("    {\n")
			TEXT("        // Lifts the pivot.\n")
			TEXT("        SetTransform(Item = Null(pivot), Value = (Translation = (Z = 5)), Weight = 0.5);\n")
			TEXT("        Speed = 3;\n")
			TEXT("    }\n")
			TEXT("}\n");

		const TCHAR* const WriteBackPath = TEXT("CR_WriteBack.dcr");

		/** The rig Text builds, made again in place. */
		FDcrRig MakeRig(FAutomationTestBase& Test, const FString& Text, const FString& AssetPath = FString())
		{
			FDabDiagnosticSink Sink(WriteBackPath);
			FDabCompilationUnit Unit;
			Unit.MainSource = FDabSourceText(WriteBackPath, Text);
			Unit.Main = ParseDab(Unit.MainSource, Sink);
			FDcrModel Model;
			FDcrSemanticOptions Options;
			Options.AssetPath = AssetPath;
			if (!Unit.Main || Sink.HasErrors() || !ExpandDabTemplates(Unit, Sink) || !BuildDcrModel(Unit, Model, Sink, Options) || Sink.HasErrors())
			{
				for (const FDabDiagnostic& Diagnostic : Sink.GetDiagnostics())
				{
					Test.AddError(FString::Printf(TEXT("the text does not build: %s (%d,%d) %s"), *Diagnostic.Code, Diagnostic.Span.Line, Diagnostic.Span.Column, *Diagnostic.Message));
				}
				return FDcrRig();
			}
			FDcrGenerateOptions GenerateOptions;
			GenerateOptions.SourceText = Text;
			const FDcrGenerateResult Generated = GenerateDcrRig(Model, GenerateOptions);
			for (const FString& Error : Generated.Errors)
			{
				Test.AddError(TEXT("generate: ") + Error);
			}
			return Generated.Succeeded() ? Generated.Rig : FDcrRig();
		}

		URigVMGraph* EventGraph(const FDcrRig& Rig)
		{
			return FindDcrModelGraph(Rig, TEXT("RigVMModel"));
		}

		URigVMNode* FindUnit(URigVMGraph* Graph, const TCHAR* StructName)
		{
			for (URigVMNode* Node : Graph ? Graph->GetNodes() : TArray<URigVMNode*>())
			{
				const URigVMUnitNode* Unit = Cast<URigVMUnitNode>(Node);
				if (Unit && Unit->GetScriptStruct() && Unit->GetScriptStruct()->GetName() == StructName)
				{
					return Node;
				}
			}
			return nullptr;
		}

		/** Lines one text has and the other lacks, counted on both sides (a changed line counts twice). */
		int32 CountChangedLines(const FString& A, const FString& B, TArray<FString>* OutLines = nullptr)
		{
			TArray<FString> LinesA;
			TArray<FString> LinesB;
			A.ParseIntoArrayLines(LinesA, false);
			B.ParseIntoArrayLines(LinesB, false);
			TMap<FString, int32> Counts;
			for (const FString& Line : LinesA)
			{
				++Counts.FindOrAdd(Line);
			}
			for (const FString& Line : LinesB)
			{
				--Counts.FindOrAdd(Line);
			}
			int32 Changed = 0;
			for (const TPair<FString, int32>& Count : Counts)
			{
				Changed += FMath::Abs(Count.Value);
				if (OutLines && Count.Value != 0)
				{
					OutLines->Add(FString::Printf(TEXT("%s %s"), Count.Value > 0 ? TEXT("-") : TEXT("+"), *Count.Key));
				}
			}
			return Changed;
		}

		/** Writes Rig back into Text and checks the result reaches the rig, moving exactly MaxChanged lines (both sides counted). */
		bool ExpectWrittenBack(FAutomationTestBase& Test, const FDcrRig& Rig, const FString& Text, int32 ExpectedChanged, const TCHAR* MustContain,
			const FDabWriteBackOptions& Options = FDabWriteBackOptions())
		{
			const FDabWriteBackResult Result = WriteBackDcrRig(Rig, WriteBackPath, Text, Options);
			for (const FDabDiagnostic& Diagnostic : Result.Diagnostics)
			{
				Test.AddInfo(FString::Printf(TEXT("    %s %s"), *Diagnostic.Code, *Diagnostic.Message));
			}
			bool bOk = Test.TestTrue(TEXT("the text was compared with the rig"), Result.bCompared);
			bOk &= Test.TestTrue(TEXT("the new text builds to the rig"), Result.bFixedPoint);
			TArray<FString> Lines;
			const int32 Changed = CountChangedLines(Text, Result.Text, &Lines);
			if (Changed != ExpectedChanged)
			{
				Test.AddError(FString::Printf(TEXT("%d lines changed, %d expected:"), Changed, ExpectedChanged));
				for (const FString& Line : Lines)
				{
					Test.AddInfo(TEXT("    ") + Line);
				}
				bOk = false;
			}
			if (MustContain && !Result.Text.Contains(MustContain))
			{
				Test.AddError(FString::Printf(TEXT("the new text does not have `%s`:\n%s"), MustContain, *Result.Text));
				bOk = false;
			}
			return bOk;
		}
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrWriteBackUnchangedTest, "DreamAnim.Rig.WriteBack.Unchanged", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	bool FDcrWriteBackUnchangedTest::RunTest(const FString& Parameters)
	{
		const FDcrRig Rig = MakeRig(*this, WriteBackText);
		if (!Rig.IsValid())
		{
			return false;
		}
		TestEqual(TEXT("the rig keeps the text it was built from"), GetDcrGeneratedText(*Rig.Asset), FString(WriteBackText));
		const FDabWriteBackResult Result = WriteBackDcrRig(Rig, WriteBackPath, WriteBackText);
		TestTrue(TEXT("compared and in step"), Result.bCompared && Result.bFixedPoint);
		TestEqual(TEXT("nothing written"), Result.Text, FString(WriteBackText));
		TestEqual(TEXT("no changes"), Result.Changes.Num(), 0);
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrWriteBackPinValueTest, "DreamAnim.Rig.WriteBack.PinValue", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	bool FDcrWriteBackPinValueTest::RunTest(const FString& Parameters)
	{
		const FDcrRig Rig = MakeRig(*this, WriteBackText);
		URigVMNode* SetTransform = FindUnit(EventGraph(Rig), TEXT("RigUnit_SetTransform"));
		if (!TestNotNull(TEXT("the SetTransform node"), SetTransform))
		{
			return false;
		}
		GetDcrController(Rig, EventGraph(Rig))->SetPinDefaultValue(SetTransform->GetName() + TEXT(".Weight"), TEXT("0.75"), false, false, false);
		// One line: the call's, with the new weight in it.
		return ExpectWrittenBack(*this, Rig, WriteBackText, 2, TEXT("Weight = 0.75"));
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrWriteBackVariableTest, "DreamAnim.Rig.WriteBack.VariableDefault", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	bool FDcrWriteBackVariableTest::RunTest(const FString& Parameters)
	{
		const FDcrRig Rig = MakeRig(*this, WriteBackText);
		if (!Rig.IsValid())
		{
			return false;
		}
		// A Blueprint-based rig keeps a variable's default in its description; the other kind in its variables' bag.
		bool bSet = false;
		if (UBlueprint* Blueprint = Cast<UBlueprint>(Rig.Asset))
		{
			if (FBPVariableDescription* Speed = Blueprint->NewVariables.FindByPredicate([](const FBPVariableDescription& Variable) { return Variable.VarName == TEXT("Speed"); }))
			{
				Speed->DefaultValue = TEXT("4.5");
				bSet = true;
			}
		}
		else if (FRigVMPropertyBag* Variables = Rig.RigVM->GetVariablesPropertyBag())
		{
			bSet = Variables->SetValueDouble(TEXT("Speed"), 4.5) == EPropertyBagResult::Success;
		}
		if (!TestTrue(TEXT("the default of Speed changed"), bSet))
		{
			return false;
		}
		return ExpectWrittenBack(*this, Rig, WriteBackText, 2, TEXT("Speed   = 4.5"));
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrWriteBackControlTest, "DreamAnim.Rig.WriteBack.ControlSettings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	bool FDcrWriteBackControlTest::RunTest(const FString& Parameters)
	{
		const FDcrRig Rig = MakeRig(*this, WriteBackText);
		URigHierarchy* Hierarchy = Rig.IsValid() ? Rig.ControlRig->GetHierarchy() : nullptr;
		const FRigElementKey Handle(TEXT("handle"), ERigElementType::Control);
		FRigControlElement* Control = Hierarchy ? Hierarchy->Find<FRigControlElement>(Handle) : nullptr;
		if (!TestNotNull(TEXT("the control handle"), Control))
		{
			return false;
		}
		FRigControlSettings Settings = Control->Settings;
		Settings.ShapeColor = FLinearColor(0.f, 0.5f, 1.f, 1.f);
		Rig.ControlRig->GetHierarchyController()->SetControlSettings(Handle, Settings, false);
		return ExpectWrittenBack(*this, Rig, WriteBackText, 2, TEXT("ShapeColor = (0, 0.5, 1, 1);"));
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrWriteBackInitialTransformTest, "DreamAnim.Rig.WriteBack.InitialTransform", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	bool FDcrWriteBackInitialTransformTest::RunTest(const FString& Parameters)
	{
		const FDcrRig Rig = MakeRig(*this, WriteBackText);
		URigHierarchy* Hierarchy = Rig.IsValid() ? Rig.ControlRig->GetHierarchy() : nullptr;
		if (!TestNotNull(TEXT("the hierarchy"), Hierarchy))
		{
			return false;
		}
		Hierarchy->SetInitialLocalTransform(FRigElementKey(TEXT("pivot"), ERigElementType::Null), FTransform(FVector(10.0, 20.0, 0.0)), true, false, false);
		return ExpectWrittenBack(*this, Rig, WriteBackText, 2, TEXT("20"));
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrWriteBackStructureTest, "DreamAnim.Rig.WriteBack.Structure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	bool FDcrWriteBackStructureTest::RunTest(const FString& Parameters)
	{
		const FDcrRig Rig = MakeRig(*this, WriteBackText);
		URigVMGraph* Graph = EventGraph(Rig);
		URigVMNode* Setter = nullptr;
		for (URigVMNode* Node : Graph ? Graph->GetNodes() : TArray<URigVMNode*>())
		{
			if (const URigVMVariableNode* Variable = Cast<URigVMVariableNode>(Node); Variable && !Variable->IsGetter())
			{
				Setter = Node;
			}
		}
		if (!TestNotNull(TEXT("the setter of Speed"), Setter))
		{
			return false;
		}
		GetDcrController(Rig, Graph)->RemoveNode(Setter, false, false);

		// Structure off: the change is refused and the text kept.
		FDabWriteBackOptions Off;
		Off.bStructure = false;
		const FDabWriteBackResult Refused = WriteBackDcrRig(Rig, WriteBackPath, WriteBackText, Off);
		TestTrue(TEXT("refused"), Refused.bRefused);
		TestEqual(TEXT("the text kept"), Refused.Text, FString(WriteBackText));
		TestTrue(TEXT("DAB8003 says why"), Refused.Diagnostics.ContainsByPredicate([](const FDabDiagnostic& Diagnostic) { return Diagnostic.Code == TEXT("DAB8003"); }));

		// Structure on: the event is rewritten from the rig, and only the event.
		const FDabWriteBackResult Result = WriteBackDcrRig(Rig, WriteBackPath, WriteBackText);
		TestTrue(TEXT("the new text builds to the rig"), Result.bFixedPoint);
		TestFalse(TEXT("the setter is gone from the text"), Result.Text.Contains(TEXT("Speed = 3;")));
		TestTrue(TEXT("the variables are as they were"), Result.Text.Contains(TEXT("double Speed   = 2;")));
		TestTrue(TEXT("the comment above the event is kept"), Result.Text.Contains(TEXT("// What the rig does each frame.")));
		TestTrue(TEXT("a block was rewritten"), Result.Changes.ContainsByPredicate([](const FDabWriteBackChange& Change) { return Change.bRewroteBlock && Change.bWritten; }));
		return true;
	}

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrWriteBackCorpusTest, "DreamAnim.Rig.WriteBack.CorpusInStep", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	bool FDcrWriteBackCorpusTest::RunTest(const FString& Parameters)
	{
		// An engine rig written as text and built from it: with nothing changed, writing back changes nothing.
		for (const TCHAR* Name : { TEXT("CR_Mannequin_BasicFootIK"), TEXT("CRU_AddLocator"), TEXT("CRD_Lattice") })
		{
			const FDcrCorpusEntry* Entry = GetDcrCorpus().FindByPredicate([Name](const FDcrCorpusEntry& Candidate) { return FCString::Strcmp(Candidate.Name, Name) == 0; });
			const FDcrRig Original = Entry ? LoadDcrRig(Entry->ObjectPath) : FDcrRig();
			FDcrModel Model;
			if (!TestTrue(FString::Printf(TEXT("%s: read"), Name), Original.IsValid() && ExtractDcrModel(Original, Model).bSuccess))
			{
				continue;
			}
			NormalizeDcrModel(Model);
			const FString Text = PrintDab(*LowerDcrModel(Model).File);
			const FDcrRig Rig = MakeRig(*this, Text, FString::Printf(TEXT("/Game/DreamAnimRigTests/WriteBack/%s.%s"), Name, Name));
			if (!Rig.IsValid())
			{
				continue;
			}
			const FDabWriteBackResult Result = WriteBackDcrRig(Rig, WriteBackPath, Text);
			TestTrue(FString::Printf(TEXT("%s: in step"), Name), Result.bCompared && Result.bFixedPoint);
			TestEqual(FString::Printf(TEXT("%s: nothing written"), Name), CountChangedLines(Text, Result.Text), 0);
		}
		return true;
	}
}
