// Copyright DreamMoon. All Rights Reserved.
//
// Semantic: text into the model.
//
//   Sections      variables, the hierarchy and a module's settings read from small texts.
//   Statements    events, lets, assignments, branches, loops, detached code, regions, literals with expressions in them.
//   Functions     the library's functions, calls to them, Inline functions as collapse nodes.
//   Spelling      two ways to write the same rig (an operator and its node's call) give the same model.
//   Files         the canonical files of Tests/Corpus/Dcr build, and lowering what they build and reading it again
//                 gives the same model.
//   Corpus        the engine's own rigs: extract, normalize, lower, print, parse, build -- the model must dump as the
//                 normalized one does. The text of each rig is written to Saved/DreamAnimRigTests/Semantic.
//   Validate      every file under Tests/Corpus/Validate/Rig is checked as the driver checks a .dcr (parse, templates,
//                 semantic; nothing generated); `// EXPECT DABnnnn (line,col)` lines list the diagnostics it must
//                 raise, exactly, and a file without them must check clean.
//
// The model of a rig read from an asset and the model of its text differ in three ways the text cannot keep, and the
// corpus test makes the asset's side alike before it compares: a getter that feeds several pins is one getter to the
// asset and one per use to the text; a function's input read through a getter node is a link from the Entry node in
// the text; and a graph that holds nothing but is not the default one has no place in the text.

#include "DcrCorpus.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Lang/DabParser.h"
#include "Lang/DabPrinter.h"
#include "Lang/DabTemplates.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Model/DcrModel.h"
#include "Rig/DcrAsset.h"
#include "Rig/DcrExtract.h"
#include "Rig/DcrLower.h"
#include "Rig/DcrNormalize.h"
#include "Rig/DcrSemantic.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Rig::Tests::SemanticPrivate
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	// ------------------------------------------------------------------------------------------
	// Building from text
	// ------------------------------------------------------------------------------------------

	struct FBuilt
	{
		explicit FBuilt(const FString& Path) : Sink(Path) {}

		FDabCompilationUnit Unit;
		FDabDiagnosticSink Sink;
		FDcrModel Model;
		bool bOk = false;
	};

	FString DescribeDiagnostics(const FDabDiagnosticSink& Sink, int32 Max = 12)
	{
		TArray<FString> Lines;
		for (const FDabDiagnostic& Diagnostic : Sink.GetDiagnostics())
		{
			if (Lines.Num() >= Max)
			{
				Lines.Add(TEXT("..."));
				break;
			}
			Lines.Add(FDabDiagnosticSink::ToWireString(Diagnostic));
		}
		return FString::Join(Lines, TEXT("\n"));
	}

	/** Parses Text and builds its model. Parse errors stop it; semantic errors are in Sink. */
	TUniquePtr<FBuilt> Build(const FString& Path, const FString& Text, const FDcrSemanticOptions& Options = FDcrSemanticOptions())
	{
		TUniquePtr<FBuilt> Built = MakeUnique<FBuilt>(Path);
		Built->Unit.MainSource = FDabSourceText(Path, Text);
		Built->Unit.Main = ParseDab(Built->Unit.MainSource, Built->Sink);
		if (!Built->Unit.Main || Built->Sink.HasErrors())
		{
			return Built;
		}
		ExpandDabTemplates(Built->Unit, Built->Sink);
		if (Built->Sink.HasErrors())
		{
			return Built;
		}
		Built->bOk = BuildDcrModel(Built->Unit, Built->Model, Built->Sink, Options);
		return Built;
	}

	/** Builds Text and expects it clean; null, after reporting the diagnostics, when it is not. */
	TUniquePtr<FBuilt> BuildClean(FAutomationTestBase& Test, const TCHAR* Label, const FString& Text)
	{
		TUniquePtr<FBuilt> Built = Build(FString::Printf(TEXT("%s.dcr"), Label), Text);
		if (!Built->bOk || Built->Sink.HasErrors())
		{
			Test.AddError(FString::Printf(TEXT("%s: the text does not build:\n%s"), Label, *DescribeDiagnostics(Built->Sink)));
			return nullptr;
		}
		return Built;
	}

	FString ShortStruct(const FDcrModelNode& Node)
	{
		int32 Dot = INDEX_NONE;
		return Node.StructPath.FindLastChar(TEXT('.'), Dot) ? Node.StructPath.Mid(Dot + 1) : Node.StructPath;
	}

	const FDcrModelGraph* FindGraph(const FDcrModel& Model, const TCHAR* Name)
	{
		return Model.Graphs.FindByPredicate([Name](const FDcrModelGraph& Graph) { return Graph.Name == Name; });
	}

	const FDcrModelNode* FindUnit(const FDcrModelGraph& Graph, const TCHAR* StructName)
	{
		return Graph.Nodes.FindByPredicate([StructName](const FDcrModelNode& Node) { return Node.Kind == EDcrNodeKind::Unit && ShortStruct(Node) == StructName; });
	}

	int32 CountKind(const FDcrModelGraph& Graph, EDcrNodeKind Kind)
	{
		int32 Count = 0;
		for (const FDcrModelNode& Node : Graph.Nodes)
		{
			Count += Node.Kind == Kind ? 1 : 0;
		}
		return Count;
	}

	const FDcrModelNode* FindVariableNode(const FDcrModelGraph& Graph, EDcrNodeKind Kind, const TCHAR* Variable, int32 Nth = 0)
	{
		int32 Seen = 0;
		for (const FDcrModelNode& Node : Graph.Nodes)
		{
			if (Node.Kind == Kind && Node.Variable == Variable && Seen++ == Nth)
			{
				return &Node;
			}
		}
		return nullptr;
	}

	bool HasLink(const FDcrModelGraph& Graph, const FString& Source, const FString& Target)
	{
		return Graph.Links.ContainsByPredicate([&Source, &Target](const FDcrModelLink& Link) { return Link.Source == Source && Link.Target == Target; });
	}

	int32 CountLinksFrom(const FDcrModelGraph& Graph, const FString& NodeName)
	{
		int32 Count = 0;
		for (const FDcrModelLink& Link : Graph.Links)
		{
			Count += Link.Source.StartsWith(NodeName + TEXT(".")) ? 1 : 0;
		}
		return Count;
	}

	const FDcrModelElement* FindElement(const FDcrModel& Model, const TCHAR* Name)
	{
		return Model.Hierarchy.FindByPredicate([Name](const FDcrModelElement& Element) { return Element.Name == Name; });
	}

	const FDcrValue* FindProperty(const FDcrModelElement& Element, const TCHAR* Name)
	{
		return Element.Properties.FindByPredicate([Name](const FDcrValue& Value) { return Value.Name == Name; });
	}

	const FDcrValue* FindNodeValue(const FDcrModelNode& Node, const TCHAR* Name)
	{
		return Node.Values.FindByPredicate([Name](const FDcrValue& Value) { return Value.Name == Name; });
	}

	// ------------------------------------------------------------------------------------------
	// Comparing
	// ------------------------------------------------------------------------------------------

	FDcrDumpOptions MakeDumpOptions()
	{
		FDcrDumpOptions Options;
		Options.bCanonicalNodeNames = true;
		Options.bFloatAsDouble = true;
		Options.bSkipComments = true;
		return Options;
	}

	/** The first line where two texts differ, and a few lines only one of them has. Empty when equal. */
	TArray<FString> Differences(const FString& Expected, const FString& Actual, int32 Max)
	{
		TArray<FString> Out;
		TArray<FString> ExpectedLines;
		TArray<FString> ActualLines;
		Expected.ParseIntoArrayLines(ExpectedLines, false);
		Actual.ParseIntoArrayLines(ActualLines, false);
		const int32 Count = FMath::Max(ExpectedLines.Num(), ActualLines.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FString E = ExpectedLines.IsValidIndex(Index) ? ExpectedLines[Index] : FString(TEXT("<end>"));
			const FString A = ActualLines.IsValidIndex(Index) ? ActualLines[Index] : FString(TEXT("<end>"));
			if (E != A)
			{
				Out.Add(FString::Printf(TEXT("first difference, line %d: expected `%s`, got `%s`"), Index + 1, *E, *A));
				break;
			}
		}
		if (Out.IsEmpty())
		{
			return Out;
		}
		TMap<FString, int32> InActual;
		for (const FString& Line : ActualLines)
		{
			++InActual.FindOrAdd(Line);
		}
		TMap<FString, int32> InExpected;
		for (const FString& Line : ExpectedLines)
		{
			++InExpected.FindOrAdd(Line);
		}
		int32 Listed = 0;
		for (const FString& Line : ExpectedLines)
		{
			int32* Other = InActual.Find(Line);
			if (Other && *Other > 0)
			{
				--*Other;
			}
			else if (Listed++ < Max)
			{
				Out.Add(TEXT("  only expected: ") + Line);
			}
		}
		Listed = 0;
		for (const FString& Line : ActualLines)
		{
			int32* Other = InExpected.Find(Line);
			if (Other && *Other > 0)
			{
				--*Other;
			}
			else if (Listed++ < Max)
			{
				Out.Add(TEXT("  only built:    ") + Line);
			}
		}
		return Out;
	}

	// ------------------------------------------------------------------------------------------
	// What the asset's model holds that the text does not
	// ------------------------------------------------------------------------------------------

	/** Lowers a model, prints it and builds it again. */
	TUniquePtr<FBuilt> ThroughText(const FDcrModel& Model, const FString& Path, FString& OutText, TArray<FDcrModelUnsupported>* OutUnsupported = nullptr)
	{
		FDcrLowerResult Lowered = LowerDcrModel(Model);
		OutText = PrintDab(*Lowered.File);
		if (OutUnsupported)
		{
			*OutUnsupported = Lowered.Unsupported;
		}
		FDcrSemanticOptions Options;
		Options.AssetPath = Model.AssetPath;
		return Build(Path, OutText, Options);
	}

	FString GetCorpusDir()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DreamAnim"));
		return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Tests"), TEXT("Corpus")) : FString();
	}

	void SaveText(const FString& Name, const FString& Text)
	{
		FFileHelper::SaveStringToFile(Text, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamAnimRigTests"), TEXT("Semantic"), Name + TEXT(".dcr")),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	// ------------------------------------------------------------------------------------------
	// The engine's rigs
	// ------------------------------------------------------------------------------------------

	bool RoundTripRig(FAutomationTestBase& Test, const FDcrCorpusEntry& Entry)
	{
		const FString Name = Entry.Name;
		const FDcrRig Original = LoadDcrRig(Entry.ObjectPath);
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

		FString Text;
		TArray<FDcrModelUnsupported> Unsupported;
		const TUniquePtr<FBuilt> Built = ThroughText(Model, Name + TEXT(".dcr"), Text, &Unsupported);
		SaveText(Name, Text);
		if (!Built->bOk || Built->Sink.HasErrors())
		{
			// What the text cannot say is written `Unsupported(...)`, which does not build: only other errors are the text's fault.
			TArray<const FDabDiagnostic*> Errors;
			for (const FDabDiagnostic& Diagnostic : Built->Sink.GetDiagnostics())
			{
				if (Diagnostic.Severity == EDabSeverity::Error && !Diagnostic.Message.Contains(TEXT("called `Unsupported`")))
				{
					Errors.Add(&Diagnostic);
				}
			}
			if (Errors.IsEmpty() && Built->Unit.Main && !Unsupported.IsEmpty())
			{
				Test.AddWarning(FString::Printf(TEXT("%s: the text holds %d things it cannot say, and builds but for them"), *Name, Unsupported.Num()));
				return true;
			}
			// One message per diagnostic, and a few: one long message is dropped on its way to the report.
			Test.AddError(FString::Printf(TEXT("%s: the text does not build (%d things the text cannot say are listed in it)"), *Name, Unsupported.Num()));
			int32 Shown = 0;
			for (const FDabDiagnostic* Diagnostic : Errors)
			{
				if (Shown++ < 10)
				{
					Test.AddError(FString::Printf(TEXT("    %s (%d,%d): %s"), *Diagnostic->Code, Diagnostic->Span.Line, Diagnostic->Span.Column, *Diagnostic->Message.Left(300)));
				}
			}
			return false;
		}

		MakeDcrModelAsTextHoldsIt(Model);
		const FDcrDumpOptions Options = MakeDumpOptions();
		const FString RigDump = DumpDcrModel(Model, Options);
		const FString TextDump = DumpDcrModel(Built->Model, Options);
		const TArray<FString> Lines = Differences(RigDump, TextDump, 8);
		if (!Lines.IsEmpty())
		{
			// Both dumps, to compare in full.
			FFileHelper::SaveStringToFile(RigDump, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamAnimRigTests"), TEXT("Semantic"), Name + TEXT(".rig.dump")));
			FFileHelper::SaveStringToFile(TextDump, *FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DreamAnimRigTests"), TEXT("Semantic"), Name + TEXT(".text.dump")));
			if (Unsupported.IsEmpty())
			{
				Test.AddError(FString::Printf(TEXT("%s: the model of its text differs from the rig's:"), *Name));
			}
			else
			{
				Test.AddWarning(FString::Printf(TEXT("%s: the model of its text differs from the rig's, which holds %d things the text cannot say:"), *Name, Unsupported.Num()));
			}
			for (const FString& Line : Lines)
			{
				Test.AddInfo(TEXT("    ") + Line.Left(700));
			}
			return !Unsupported.IsEmpty();
		}
		return true;
	}

	bool RoundTripGroup(FAutomationTestBase& Test, EDcrCorpusGroup Group)
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
			Held += RoundTripRig(Test, Entry) ? 1 : 0;
		}
		Test.AddInfo(FString::Printf(TEXT("%d of %d rigs came back as their text reads"), Held, Count));
		return Held == Count;
	}

	// ------------------------------------------------------------------------------------------
	// Expectations of the validate files
	// ------------------------------------------------------------------------------------------

	struct FExpectation
	{
		FString Code;
		int32 Line = 0;
		int32 Column = 0;
		bool bHasPosition = false;
	};

	/** `// EXPECT DABnnnn` or `// EXPECT DABnnnn (line,col)`, one per line, anywhere in the file. */
	TArray<FExpectation> ReadExpectations(const FString& Text)
	{
		TArray<FExpectation> Result;
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		for (const FString& RawLine : Lines)
		{
			const int32 At = RawLine.Find(TEXT("// EXPECT "), ESearchCase::CaseSensitive);
			if (At == INDEX_NONE)
			{
				continue;
			}
			FString Rest = RawLine.Mid(At + 10).TrimStartAndEnd();
			FExpectation Expectation;
			int32 Space = INDEX_NONE;
			if (Rest.FindChar(TEXT(' '), Space))
			{
				Expectation.Code = Rest.Left(Space);
				const FString Position = Rest.Mid(Space + 1).TrimStartAndEnd();
				FString LineText;
				FString ColumnText;
				if (Position.StartsWith(TEXT("(")) && Position.EndsWith(TEXT(")")) && Position.Mid(1, Position.Len() - 2).Split(TEXT(","), &LineText, &ColumnText))
				{
					Expectation.Line = FCString::Atoi(*LineText.TrimStartAndEnd());
					Expectation.Column = FCString::Atoi(*ColumnText.TrimStartAndEnd());
					Expectation.bHasPosition = true;
				}
			}
			else
			{
				Expectation.Code = Rest;
			}
			Result.Add(Expectation);
		}
		return Result;
	}

	/** Every expectation must match one diagnostic and every diagnostic one expectation. */
	void CheckDiagnostics(FAutomationTestBase& Test, const TArray<FExpectation>& Expected, TConstArrayView<FDabDiagnostic> Diagnostics)
	{
		TArray<FDabDiagnostic> Unmatched(Diagnostics);
		for (const FExpectation& Expectation : Expected)
		{
			const int32 Index = Unmatched.IndexOfByPredicate([&Expectation](const FDabDiagnostic& Diagnostic)
			{
				return Diagnostic.Code.Equals(Expectation.Code, ESearchCase::CaseSensitive)
					&& (!Expectation.bHasPosition || (Diagnostic.Span.Line == Expectation.Line && Diagnostic.Span.Column == Expectation.Column));
			});
			if (Index == INDEX_NONE)
			{
				Test.AddError(Expectation.bHasPosition ? FString::Printf(TEXT("Expected %s at (%d,%d), not raised."), *Expectation.Code, Expectation.Line, Expectation.Column)
					: FString::Printf(TEXT("Expected %s, not raised."), *Expectation.Code));
			}
			else
			{
				Unmatched.RemoveAt(Index);
			}
		}
		for (const FDabDiagnostic& Diagnostic : Unmatched)
		{
			Test.AddError(FString::Printf(TEXT("Unexpected %s"), *FDabDiagnosticSink::ToWireString(Diagnostic)));
		}
	}
}

// ------------------------------------------------------------------------------------------
// Sections
// ------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSemanticSectionsTest, "DreamAnim.Rig.Semantic.Sections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** Variables, hierarchy elements with parents, properties and defaults. */
bool FDcrSemanticSectionsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig::Tests::SemanticPrivate;
	const TUniquePtr<FBuilt> Built = BuildClean(*this, TEXT("Sections"), TEXT(R"(ControlRig(Name="Tests/Sections", Root="Game")
{
    Variables = {
        double Speed = 1.5 [ Public ];
        bool Enabled [ ReadOnly; Category = "State"; Tooltip = "On or off" ];
        Array<double> Weights = [ 0, 0.5, 1 ];
        RigElementKey Target;
    }

    Hierarchy = {
        Bone root_bone { Transform = (Translation = (X = 1, Y = 2, Z = 3)); }
        Bone child_bone { Parent = Bone(root_bone); }
        Null offset_null { Parent = Bone(root_bone); }
        Curve blend { Value = 0.5; }
        Control ctrl { Parent = Null(offset_null); ControlType = Float; }
        Control nested_ctrl { Parent = Control(ctrl); }
    }
}
)"));
	if (!Built)
	{
		return false;
	}
	const FDcrModel& Model = Built->Model;
	TestEqual(TEXT("asset path"), Model.AssetPath, FString(TEXT("/Game/Tests/Sections.Sections")));

	TestEqual(TEXT("variable count"), Model.Variables.Num(), 4);
	if (Model.Variables.Num() == 4)
	{
		TestEqual(TEXT("Speed type"), Model.Variables[0].Type.CPPType, FString(TEXT("double")));
		TestTrue(TEXT("Speed default"), Model.Variables[0].Default.StartsWith(TEXT("1.5")));
		TestTrue(TEXT("Speed public"), Model.Variables[0].bPublic);
		TestTrue(TEXT("Enabled read only"), Model.Variables[1].bReadOnly && !Model.Variables[1].bPublic);
		TestEqual(TEXT("Enabled category"), Model.Variables[1].Category, FString(TEXT("State")));
		TestEqual(TEXT("Enabled tooltip"), Model.Variables[1].Tooltip, FString(TEXT("On or off")));
		TestEqual(TEXT("Weights type"), Model.Variables[2].Type.CPPType, FString(TEXT("TArray<double>")));
		TestFalse(TEXT("Weights default"), Model.Variables[2].Default.IsEmpty());
		TestTrue(TEXT("Target type"), Model.Variables[3].Type.CPPType.Contains(TEXT("RigElementKey")) && !Model.Variables[3].Type.ObjectPath.IsEmpty());
	}

	TestEqual(TEXT("element count"), Model.Hierarchy.Num(), 6);
	if (Model.Hierarchy.Num() == 6)
	{
		const FDcrModelElement& Root = Model.Hierarchy[0];
		TestTrue(TEXT("root_bone"), Root.Kind == EDcrElementKind::Bone && Root.Name == TEXT("root_bone") && Root.Parent.IsEmpty());
		TestNotNull(TEXT("root_bone transform"), FindProperty(Root, TEXT("Transform")));
		const FDcrValue* BoneType = FindProperty(Root, TEXT("BoneType"));
		TestTrue(TEXT("a bone the text adds is a user bone"), BoneType && BoneType->Value == TEXT("User"));
		TestTrue(TEXT("child_bone"), Model.Hierarchy[1].Parent == TEXT("root_bone") && Model.Hierarchy[1].ParentKind == EDcrElementKind::Bone);
		TestTrue(TEXT("offset_null"), Model.Hierarchy[2].Kind == EDcrElementKind::Null && Model.Hierarchy[2].Parent == TEXT("root_bone"));
		const FDcrValue* Curve = FindProperty(Model.Hierarchy[3], TEXT("Value"));
		TestTrue(TEXT("blend"), Model.Hierarchy[3].Kind == EDcrElementKind::Curve && Curve && Curve->Value == TEXT("0.5"));
		const FDcrValue* ControlType = FindProperty(Model.Hierarchy[4], TEXT("ControlType"));
		TestTrue(TEXT("ctrl"), Model.Hierarchy[4].ParentKind == EDcrElementKind::Null && Model.Hierarchy[4].Parent == TEXT("offset_null") && ControlType && ControlType->Value == TEXT("Float"));
		TestTrue(TEXT("nested_ctrl"), Model.Hierarchy[5].ParentKind == EDcrElementKind::Control && Model.Hierarchy[5].Parent == TEXT("ctrl"));
	}
	TestTrue(TEXT("the default graph is there, first"), Model.Graphs.Num() == 1 && Model.Graphs[0].Name == TEXT("RigVMModel"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSemanticNestedHierarchyTest, "DreamAnim.Rig.Semantic.NestedHierarchy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** Nested elements are the same as flat ones with a Parent. */
bool FDcrSemanticNestedHierarchyTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig::Tests::SemanticPrivate;
	const TUniquePtr<FBuilt> Nested = BuildClean(*this, TEXT("Nested"), TEXT(R"(ControlRig(Name="Tests/Hierarchy", Root="Game")
{
    Hierarchy = {
        Bone a
        {
            Bone b { Bone c; }
            Bone d;
        }
        Bone e;
    }
}
)"));
	const TUniquePtr<FBuilt> Flat = BuildClean(*this, TEXT("Flat"), TEXT(R"(ControlRig(Name="Tests/Hierarchy", Root="Game")
{
    Hierarchy = {
        Bone a;
        Bone b { Parent = Bone(a); }
        Bone c { Parent = Bone(b); }
        Bone d { Parent = Bone(a); }
        Bone e;
    }
}
)"));
	if (!Nested || !Flat)
	{
		return false;
	}
	const FDcrDumpOptions Options = MakeDumpOptions();
	TestEqual(TEXT("nested and flat dump alike"), DumpDcrModel(Nested->Model, Options), DumpDcrModel(Flat->Model, Options));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSemanticModuleTest, "DreamAnim.Rig.Semantic.Module",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** A Module section makes a module; its settings are the module settings' properties. */
bool FDcrSemanticModuleTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig::Tests::SemanticPrivate;
	const TUniquePtr<FBuilt> Built = BuildClean(*this, TEXT("Module"), TEXT(R"(ControlRig(Name="Tests/Module", Root="Game")
{
    Module = { Category = "Vehicles"; Description = "A test module."; }
}
)"));
	if (!Built)
	{
		return false;
	}
	TestTrue(TEXT("a module"), Built->Model.bModule);
	const FDcrValue* Category = Built->Model.ModuleSettings.FindByPredicate([](const FDcrValue& Value) { return Value.Name == TEXT("Category"); });
	TestTrue(TEXT("Category"), Category && Category->Value.Contains(TEXT("Vehicles")));
	return true;
}

// ------------------------------------------------------------------------------------------
// Statements
// ------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSemanticEventsTest, "DreamAnim.Rig.Semantic.Events",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** Events, graphs, assignments chained by execute links, an operator's node, a getter for every use. */
bool FDcrSemanticEventsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig::Tests::SemanticPrivate;
	const TUniquePtr<FBuilt> Built = BuildClean(*this, TEXT("Events"), TEXT(R"(ControlRig(Name="Tests/Events", Root="Game")
{
    Variables = { double Speed; double Other; }

    Event Forward
    {
        Speed = Speed + 1;
        Other = Speed;
    }

    Event Construction [ Graph = "ConstructionGraph" ]
    {
    }
}
)"));
	if (!Built)
	{
		return false;
	}
	const FDcrModel& Model = Built->Model;
	TestEqual(TEXT("graph count"), Model.Graphs.Num(), 2);
	const FDcrModelGraph* Main = FindGraph(Model, TEXT("RigVMModel"));
	const FDcrModelGraph* Construction = FindGraph(Model, TEXT("ConstructionGraph"));
	if (!Main || !Construction)
	{
		AddError(TEXT("RigVMModel and ConstructionGraph are expected"));
		return false;
	}
	TestTrue(TEXT("the default graph is first"), &Model.Graphs[0] == Main);
	TestEqual(TEXT("Construction holds its event"), Construction->Nodes.Num(), 1);
	if (Construction->Nodes.Num() == 1)
	{
		TestEqual(TEXT("the construction event"), ShortStruct(Construction->Nodes[0]), FString(TEXT("RigUnit_PrepareForExecution")));
	}

	const FDcrModelNode* Event = FindUnit(*Main, TEXT("RigUnit_BeginExecution"));
	const FDcrModelNode* SetSpeed = FindVariableNode(*Main, EDcrNodeKind::VariableSet, TEXT("Speed"));
	const FDcrModelNode* SetOther = FindVariableNode(*Main, EDcrNodeKind::VariableSet, TEXT("Other"));
	if (!Event || !SetSpeed || !SetOther)
	{
		AddError(TEXT("the event and the two setters are expected"));
		return false;
	}
	TestEqual(TEXT("one node for the addition"), CountKind(*Main, EDcrNodeKind::Unit), 2);
	TestEqual(TEXT("a getter for every read of Speed"), CountKind(*Main, EDcrNodeKind::VariableGet), 2);
	TestTrue(TEXT("the event runs the first setter"), HasLink(*Main, Event->Name + TEXT(".") + Event->ExecuteOut[0], SetSpeed->Name + TEXT(".") + SetSpeed->ExecuteIn[0]));
	TestTrue(TEXT("the first setter runs the second"), HasLink(*Main, SetSpeed->Name + TEXT(".") + SetSpeed->ExecuteOut[0], SetOther->Name + TEXT(".") + SetOther->ExecuteIn[0]));

	// The sum feeds the first setter, and the 1 is a value on the sum's second pin.
	const FDcrModelNode* Sum = Main->Nodes.FindByPredicate([](const FDcrModelNode& Node) { return Node.Kind == EDcrNodeKind::Unit && ShortStruct(Node).Contains(TEXT("Add")); });
	if (Sum)
	{
		TestTrue(TEXT("the sum feeds the setter"), HasLink(*Main, Sum->Name + TEXT(".Result"), SetSpeed->Name + TEXT(".Value")));
		const FDcrValue* B = FindNodeValue(*Sum, TEXT("B"));
		TestTrue(TEXT("the 1 is B"), B && B->Value.StartsWith(TEXT("1")));
	}
	else
	{
		AddError(TEXT("the addition is expected to be a unit"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSemanticLetTest, "DreamAnim.Rig.Semantic.Let",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** A let is one node, named for it, shared by every use; the same call written twice is two nodes. */
bool FDcrSemanticLetTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig::Tests::SemanticPrivate;
	const TUniquePtr<FBuilt> Shared = BuildClean(*this, TEXT("Shared"), TEXT(R"(ControlRig(Name="Tests/Let", Root="Game")
{
    Variables = { double Speed; }

    Event Forward
    {
        let Offset = Speed * 2;
        Speed = Offset + Offset;
    }
}
)"));
	const TUniquePtr<FBuilt> Repeated = BuildClean(*this, TEXT("Repeated"), TEXT(R"(ControlRig(Name="Tests/Let", Root="Game")
{
    Variables = { double Speed; }

    Event Forward
    {
        Speed = Speed * 2 + Speed * 2;
    }
}
)"));
	if (!Shared || !Repeated)
	{
		return false;
	}
	const FDcrModelGraph& Graph = Shared->Model.Graphs[0];
	const FDcrModelNode* Offset = Graph.FindNode(TEXT("Offset"));
	if (!Offset)
	{
		AddError(TEXT("the node of `let Offset` is named Offset"));
		return false;
	}
	TestEqual(TEXT("both pins of the sum take Offset"), CountLinksFrom(Graph, TEXT("Offset")), 2);
	// Event, setter, product, sum, one getter for the read.
	TestEqual(TEXT("nodes with a let"), Graph.Nodes.Num(), 5);
	// Event, setter, two products, sum, two getters.
	TestEqual(TEXT("nodes without"), Repeated->Model.Graphs[0].Nodes.Num(), 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSemanticControlFlowTest, "DreamAnim.Rig.Semantic.ControlFlow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** `if` is a branch with the code of each side on its pins; loops run their body from the loop; Detached is linked to nothing. */
bool FDcrSemanticControlFlowTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig::Tests::SemanticPrivate;
	const TUniquePtr<FBuilt> Built = BuildClean(*this, TEXT("ControlFlow"), TEXT(R"(ControlRig(Name="Tests/ControlFlow", Root="Game")
{
    Variables = { double Speed; Array<double> Weights = [ 1, 2 ]; }

    Event Forward
    {
        if (Speed > 10)
        {
            Speed = 10;
        }
        else
        {
            Speed = Speed + 1;
        }
        for (Weight, Index in Weights)
        {
            Speed = Speed + Weight;
        }
        for (Step, Ratio in Range(3))
        {
            Speed = Speed + Ratio;
        }
        Detached
        {
            Speed = 0;
        }
    }
}
)"));
	if (!Built)
	{
		return false;
	}
	const FDcrModelGraph& Graph = Built->Model.Graphs[0];
	const FDcrModelNode* Branch = FindUnit(Graph, TEXT("RigVMFunction_ControlFlowBranch"));
	const FDcrModelNode* Counted = FindUnit(Graph, TEXT("RigVMFunction_ForLoopCount"));
	const FDcrModelNode* Iterator = Graph.Nodes.FindByPredicate([](const FDcrModelNode& Node) { return Node.Kind == EDcrNodeKind::Dispatch && Node.Notation.Contains(TEXT("Iterator")); });
	if (!Branch || !Counted || !Iterator)
	{
		AddError(TEXT("a Branch, a ForLoopCount and an ArrayIterator are expected"));
		return false;
	}
	TestTrue(TEXT("the branch has True, False and Completed"), Branch->ExecuteOut.Contains(TEXT("True")) && Branch->ExecuteOut.Contains(TEXT("False")) && Branch->ExecuteOut.Contains(TEXT("Completed")));
	int32 FromTrue = 0;
	int32 FromFalse = 0;
	for (const FDcrModelLink& Link : Graph.Links)
	{
		FromTrue += Link.Source == Branch->Name + TEXT(".True") ? 1 : 0;
		FromFalse += Link.Source == Branch->Name + TEXT(".False") ? 1 : 0;
	}
	TestEqual(TEXT("one statement on True"), FromTrue, 1);
	TestEqual(TEXT("one statement on False"), FromFalse, 1);
	TestTrue(TEXT("the loop over the array resolves its element"), Iterator->ResolvedTypes.Num() >= 2);
	TestTrue(TEXT("the branch runs on to the loop"), HasLink(Graph, Branch->Name + TEXT(".Completed"), Iterator->Name + TEXT(".") + Iterator->ExecuteIn[0]));

	// The detached setter: a setter of Speed with no execute link into it.
	int32 Unreached = 0;
	for (const FDcrModelNode& Node : Graph.Nodes)
	{
		if (Node.Kind != EDcrNodeKind::VariableSet || Node.ExecuteIn.IsEmpty())
		{
			continue;
		}
		const FString Target = Node.Name + TEXT(".") + Node.ExecuteIn[0];
		const bool bReached = Graph.Links.ContainsByPredicate([&Target](const FDcrModelLink& Link) { return Link.Target == Target; });
		Unreached += bReached ? 0 : 1;
	}
	TestEqual(TEXT("one setter is not run by anything"), Unreached, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSemanticRegionsTest, "DreamAnim.Rig.Semantic.Regions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** A region is a comment node that names what its statements made. */
bool FDcrSemanticRegionsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig::Tests::SemanticPrivate;
	const TUniquePtr<FBuilt> Built = BuildClean(*this, TEXT("Regions"), TEXT(R"(ControlRig(Name="Tests/Regions", Root="Game")
{
    Variables = { double Speed; double Other; }

    Event Forward
    {
        Other = 1;

        #Region "Speed up"
        Speed = Speed + Other;
        Speed = Speed + Other;
        #EndRegion
    }
}
)"));
	if (!Built)
	{
		return false;
	}
	const FDcrModelGraph& Graph = Built->Model.Graphs[0];
	const FDcrModelNode* Comment = Graph.Nodes.FindByPredicate([](const FDcrModelNode& Node) { return Node.Kind == EDcrNodeKind::Comment; });
	if (!Comment)
	{
		AddError(TEXT("a comment node is expected"));
		return false;
	}
	TestEqual(TEXT("the comment's text"), Comment->CommentText, FString(TEXT("Speed up")));
	// Two setters, two sums, four getters.
	TestEqual(TEXT("the nodes it goes around"), Comment->CommentMembers.Num(), 8);
	const FDcrModelNode* First = FindVariableNode(Graph, EDcrNodeKind::VariableSet, TEXT("Other"));
	TestFalse(TEXT("the statement before it is outside"), First && Comment->CommentMembers.Contains(First->Name));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSemanticLiteralPartsTest, "DreamAnim.Rig.Semantic.LiteralParts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** A literal with an expression in it: the constants are the pin's value, the expression a link to the sub-pin. */
bool FDcrSemanticLiteralPartsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig::Tests::SemanticPrivate;
	const TUniquePtr<FBuilt> Built = BuildClean(*this, TEXT("LiteralParts"), TEXT(R"(ControlRig(Name="Tests/LiteralParts", Root="Game")
{
    Variables = { double Height; }

    Event Forward
    {
        SetTransform(Item = Bone(root), Value = (Translation = (X = 1, Z = Height)));
    }
}
)"));
	if (!Built)
	{
		return false;
	}
	const FDcrModelGraph& Graph = Built->Model.Graphs[0];
	const FDcrModelNode* Set = FindUnit(Graph, TEXT("RigUnit_SetTransform"));
	const FDcrModelNode* Getter = FindVariableNode(Graph, EDcrNodeKind::VariableGet, TEXT("Height"));
	if (!Set || !Getter)
	{
		AddError(TEXT("SetTransform and a getter of Height are expected"));
		return false;
	}
	TestTrue(TEXT("Z is linked"), HasLink(Graph, Getter->Name + TEXT(".Value"), Set->Name + TEXT(".Value.Translation.Z")));
	const FDcrValue* Item = FindNodeValue(*Set, TEXT("Item"));
	TestTrue(TEXT("Item is the root bone"), Item && Item->Value.Contains(TEXT("root")));
	const FDcrValue* Value = FindNodeValue(*Set, TEXT("Value"));
	TestTrue(TEXT("the constant part is the pin's value"), Value && Value->Value.Contains(TEXT("X=1")));
	return true;
}

// ------------------------------------------------------------------------------------------
// Functions
// ------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSemanticFunctionsTest, "DreamAnim.Rig.Semantic.Functions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** A pure and a mutable function; calls to them; defaults; metadata. */
bool FDcrSemanticFunctionsTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig::Tests::SemanticPrivate;
	const TUniquePtr<FBuilt> Built = BuildClean(*this, TEXT("Functions"), TEXT(R"(ControlRig(Name="Tests/Functions", Root="Game")
{
    Variables = { double Speed; }

    Function Scale(double Value, double Factor = 2) -> (double Result) = Value * Factor;

    Function Bump(double Amount = 1) [ Public; Category = "Tests"; Description = "Adds to the speed." ]
    {
        Speed = Speed + Amount;
    }

    Event Forward
    {
        Bump();
        Speed = Scale(Speed);
    }
}
)"));
	if (!Built)
	{
		return false;
	}
	const FDcrModel& Model = Built->Model;
	TestEqual(TEXT("function count"), Model.Functions.Num(), 2);
	if (Model.Functions.Num() != 2)
	{
		return false;
	}
	const FDcrModelFunction& Scale = Model.Functions[0];
	const FDcrModelFunction& Bump = Model.Functions[1];
	TestTrue(TEXT("Scale is pure"), Scale.Name == TEXT("Scale") && !Scale.bMutable && !Scale.bPublic);
	TestEqual(TEXT("Scale inputs"), Scale.Inputs.Num(), 2);
	TestEqual(TEXT("Scale outputs"), Scale.Outputs.Num(), 1);
	TestTrue(TEXT("Factor's default"), Scale.Inputs.Num() == 2 && Scale.Inputs[1].Default.StartsWith(TEXT("2")));
	TestTrue(TEXT("Bump is public and mutable"), Bump.bMutable && Bump.bPublic && Bump.Category == TEXT("Tests") && Bump.Description == TEXT("Adds to the speed."));
	TestTrue(TEXT("Bump's default"), Bump.Inputs.Num() == 1 && Bump.Inputs[0].Default.StartsWith(TEXT("1")));

	// Scale's graph: Entry and Return, one product, Value and Factor linked into it, the product into Result.
	TestEqual(TEXT("Scale's nodes"), Scale.Graph.Nodes.Num(), 3);
	const FDcrModelNode* Product = Scale.Graph.Nodes.FindByPredicate([](const FDcrModelNode& Node) { return Node.Kind == EDcrNodeKind::Unit; });
	if (Product)
	{
		TestTrue(TEXT("Value goes into the product"), HasLink(Scale.Graph, TEXT("Entry.Value"), Product->Name + TEXT(".A")));
		TestTrue(TEXT("the product goes out"), HasLink(Scale.Graph, Product->Name + TEXT(".Result"), TEXT("Return.Result")));
	}
	// Bump's graph runs from Entry to its setter to Return.
	const FDcrModelNode* Setter = FindVariableNode(Bump.Graph, EDcrNodeKind::VariableSet, TEXT("Speed"));
	TestTrue(TEXT("Bump runs its setter"), Setter && HasLink(Bump.Graph, TEXT("Entry.ExecuteContext"), Setter->Name + TEXT(".") + Setter->ExecuteIn[0]) && HasLink(Bump.Graph, Setter->Name + TEXT(".") + Setter->ExecuteOut[0], TEXT("Return.ExecuteContext")));

	const FDcrModelGraph& Main = Model.Graphs[0];
	TestEqual(TEXT("the event calls both"), CountKind(Main, EDcrNodeKind::LocalCall), 2);
	const FDcrModelNode* BumpCall = Main.Nodes.FindByPredicate([](const FDcrModelNode& Node) { return Node.Kind == EDcrNodeKind::LocalCall && Node.Function == TEXT("Bump"); });
	TestTrue(TEXT("a mutable call has execute pins"), BumpCall && !BumpCall->ExecuteIn.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSemanticInlineTest, "DreamAnim.Rig.Semantic.Inline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** An Inline function is a collapse node, its interface in the Entry and Return of its graph. */
bool FDcrSemanticInlineTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig::Tests::SemanticPrivate;
	const TUniquePtr<FBuilt> Built = BuildClean(*this, TEXT("Inline"), TEXT(R"(ControlRig(Name="Tests/Inline", Root="Game")
{
    Variables = { double Speed; }

    Function Twice(double Value) -> (double Result) [ Inline ] = Value + Value;

    Event Forward
    {
        Speed = Twice(Speed);
    }
}
)"));
	if (!Built)
	{
		return false;
	}
	TestEqual(TEXT("an inline function is not in the library"), Built->Model.Functions.Num(), 0);
	const FDcrModelGraph& Graph = Built->Model.Graphs[0];
	const FDcrModelNode* Collapse = Graph.Nodes.FindByPredicate([](const FDcrModelNode& Node) { return Node.Kind == EDcrNodeKind::Collapse; });
	if (!Collapse || !Collapse->Contained.IsValid())
	{
		AddError(TEXT("a collapse node with its graph is expected"));
		return false;
	}
	TestEqual(TEXT("named for the function"), Collapse->Name, FString(TEXT("Twice")));
	const FDcrModelNode* Entry = Collapse->Contained->Nodes.FindByPredicate([](const FDcrModelNode& Node) { return Node.Kind == EDcrNodeKind::Entry; });
	const FDcrModelNode* Return = Collapse->Contained->Nodes.FindByPredicate([](const FDcrModelNode& Node) { return Node.Kind == EDcrNodeKind::Return; });
	TestTrue(TEXT("Entry shows Value"), Entry && Entry->ResolvedTypes.Num() == 1 && Entry->ResolvedTypes[0].Key == TEXT("Value") && Entry->ResolvedTypes[0].Value.CPPType == TEXT("double"));
	TestTrue(TEXT("Return shows Result"), Return && Return->ResolvedTypes.Num() == 1 && Return->ResolvedTypes[0].Key == TEXT("Result"));
	return true;
}

// ------------------------------------------------------------------------------------------
// Spelling
// ------------------------------------------------------------------------------------------

namespace UE::DreamAnim::Rig::Tests::SemanticPrivate
{
	/** Two bodies for the same rig: they must build alike. */
	bool BuildAlike(FAutomationTestBase& Test, const TCHAR* Label, const TCHAR* BodyA, const TCHAR* BodyB)
	{
		const FString Head = TEXT("ControlRig(Name=\"Tests/Spelling\", Root=\"Game\")\n{\n    Variables = { double Speed; double Other; Array<double> Weights = [ 1, 2 ]; }\n    Event Forward\n    {\n        ");
		const FString Tail = TEXT("\n    }\n}\n");
		const TUniquePtr<FBuilt> A = BuildClean(Test, *FString::Printf(TEXT("%s A"), Label), Head + BodyA + Tail);
		const TUniquePtr<FBuilt> B = BuildClean(Test, *FString::Printf(TEXT("%s B"), Label), Head + BodyB + Tail);
		if (!A || !B)
		{
			return false;
		}
		const FDcrDumpOptions Options = MakeDumpOptions();
		const TArray<FString> Lines = Differences(DumpDcrModel(A->Model, Options), DumpDcrModel(B->Model, Options), 6);
		for (const FString& Line : Lines)
		{
			Test.AddInfo(FString::Printf(TEXT("%s: %s"), Label, *Line));
		}
		if (!Lines.IsEmpty())
		{
			Test.AddError(FString::Printf(TEXT("%s: the two spellings build different models"), Label));
		}
		return Lines.IsEmpty();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSemanticSpellingTest, "DreamAnim.Rig.Semantic.Spelling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** An operator and the call of its node; the conditional and its dispatch; an index and its dispatch. */
bool FDcrSemanticSpellingTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig::Tests::SemanticPrivate;
	bool bHeld = true;
	bHeld &= BuildAlike(*this, TEXT("arithmetic"), TEXT("Speed = Speed + Other * 2;"), TEXT("Speed = Add(Speed, Multiply(Other, 2));"));
	bHeld &= BuildAlike(*this, TEXT("conditional"), TEXT("Speed = Other > Speed ? Other : Speed;"), TEXT("Speed = If(Other > Speed, Other, Speed);"));
	bHeld &= BuildAlike(*this, TEXT("index"), TEXT("Speed = Weights[1];"), TEXT("Speed = ArrayGetAtIndex(Weights, 1);"));
	bHeld &= BuildAlike(*this, TEXT("parentheses"), TEXT("Speed = (Speed + Other) * 2;"), TEXT("Speed = Multiply(Add(Speed, Other), 2);"));
	return bHeld;
}

// ------------------------------------------------------------------------------------------
// Files
// ------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSemanticFilesTest, "DreamAnim.Rig.Semantic.Files",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** The canonical examples build, and what they build lowers to text that builds the same model. */
bool FDcrSemanticFilesTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig::Tests::SemanticPrivate;
	static const TCHAR* const Files[] = { TEXT("PureFunctions.dcr"), TEXT("ScaleModuleControls.dcr"), TEXT("BasicFootIK.dcr"), TEXT("Hinge.dcr"), TEXT("FlowBlocks.dcr") };
	bool bHeld = true;
	for (const TCHAR* File : Files)
	{
		const FString Path = FPaths::Combine(GetCorpusDir(), TEXT("Dcr"), File);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			AddError(FString::Printf(TEXT("Cannot read %s"), *Path));
			bHeld = false;
			continue;
		}
		const TUniquePtr<FBuilt> First = Build(Path, Text);
		if (!First->bOk || First->Sink.HasErrors())
		{
			AddError(FString::Printf(TEXT("%s does not build:\n%s"), File, *DescribeDiagnostics(First->Sink)));
			bHeld = false;
			continue;
		}
		FString Lowered;
		const TUniquePtr<FBuilt> Second = ThroughText(First->Model, Path, Lowered);
		if (!Second->bOk || Second->Sink.HasErrors())
		{
			AddError(FString::Printf(TEXT("%s: the text lowered from its model does not build:\n%s"), File, *DescribeDiagnostics(Second->Sink)));
			SaveText(FPaths::GetBaseFilename(File) + TEXT(".again"), Lowered);
			bHeld = false;
			continue;
		}
		const FDcrDumpOptions Options = MakeDumpOptions();
		const TArray<FString> Lines = Differences(DumpDcrModel(First->Model, Options), DumpDcrModel(Second->Model, Options), 6);
		if (!Lines.IsEmpty())
		{
			AddError(FString::Printf(TEXT("%s: lowering its model and reading it again changes it:"), File));
			for (const FString& Line : Lines)
			{
				AddInfo(TEXT("    ") + Line);
			}
			SaveText(FPaths::GetBaseFilename(File) + TEXT(".again"), Lowered);
			bHeld = false;
		}
	}
	return bHeld;
}

// ------------------------------------------------------------------------------------------
// The engine's rigs
// ------------------------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSemanticCorpusStandaloneTest, "DreamAnim.Rig.Semantic.Corpus.Standalone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** The rigs that run on their own: extract, normalize, lower, print, parse, build -- the same model. */
bool FDcrSemanticCorpusStandaloneTest::RunTest(const FString& Parameters)
{
	return UE::DreamAnim::Rig::Tests::SemanticPrivate::RoundTripGroup(*this, UE::DreamAnim::Rig::Tests::EDcrCorpusGroup::Standalone);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSemanticCorpusModuleTest, "DreamAnim.Rig.Semantic.Corpus.Module",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** The modules of a modular rig: the same circle. */
bool FDcrSemanticCorpusModuleTest::RunTest(const FString& Parameters)
{
	return UE::DreamAnim::Rig::Tests::SemanticPrivate::RoundTripGroup(*this, UE::DreamAnim::Rig::Tests::EDcrCorpusGroup::Module);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSemanticCorpusIndependentTest, "DreamAnim.Rig.Semantic.Corpus.Independent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** The rigs of the independent asset kind: the same circle. */
bool FDcrSemanticCorpusIndependentTest::RunTest(const FString& Parameters)
{
	return UE::DreamAnim::Rig::Tests::SemanticPrivate::RoundTripGroup(*this, UE::DreamAnim::Rig::Tests::EDcrCorpusGroup::Independent);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrSemanticCorpusLibraryTest, "DreamAnim.Rig.Semantic.Corpus.Library",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** The root module and the function libraries: the same circle. */
bool FDcrSemanticCorpusLibraryTest::RunTest(const FString& Parameters)
{
	return UE::DreamAnim::Rig::Tests::SemanticPrivate::RoundTripGroup(*this, UE::DreamAnim::Rig::Tests::EDcrCorpusGroup::Library);
}

// ------------------------------------------------------------------------------------------
// Validate
// ------------------------------------------------------------------------------------------

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FDcrSemanticValidateTest, "DreamAnim.Rig.Semantic.Validate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FDcrSemanticValidateTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	const FString Dir = FPaths::Combine(UE::DreamAnim::Rig::Tests::SemanticPrivate::GetCorpusDir(), TEXT("Validate"), TEXT("Rig"));
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Dir, TEXT("*.dcr")), true, false);
	Files.Sort();
	for (const FString& File : Files)
	{
		OutBeautifiedNames.Add(FPaths::GetBaseFilename(File));
		OutTestCommands.Add(FPaths::Combine(Dir, File));
	}
}

bool FDcrSemanticValidateTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig::Tests::SemanticPrivate;
	const FString Path = FPaths::ConvertRelativePathToFull(Parameters);
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		AddError(FString::Printf(TEXT("Cannot read %s"), *Path));
		return false;
	}
	const TUniquePtr<FBuilt> Built = Build(Path, Text);
	CheckDiagnostics(*this, ReadExpectations(Text), Built->Sink.GetDiagnostics());
	return true;
}

#endif
