// Copyright DreamMoon. All Rights Reserved.
//
// Extract, dump and normalize against the engine's own rigs (DcrCorpus.h): every rig reads without failing, a
// rig dumps the same however often and from whichever copy it is read, normalization leaves no reroute and
// no sequence it did not report, and the facts about CR_Mannequin_BasicFootIK that were checked by hand
// hold on the extracted model.

#include "DcrCorpus.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Model/DcrModel.h"
#include "Rig/DcrAsset.h"
#include "Rig/DcrExtract.h"
#include "Rig/DcrNormalize.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMGraph.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Rig::Tests::ExtractTestsPrivate
{
	using namespace Model;

	const TCHAR* const FootIKPath = TEXT("/MoverExamples/Characters/Mannequins/Rigs/CR_Mannequin_BasicFootIK.CR_Mannequin_BasicFootIK");

	/** Calls Visit for the graph and for the graphs inside its collapse nodes. */
	void VisitGraphs(const FDcrModelGraph& Graph, TFunctionRef<void(const FDcrModelGraph&)> Visit)
	{
		Visit(Graph);
		for (const FDcrModelNode& Node : Graph.Nodes)
		{
			if (Node.Contained.IsValid())
			{
				VisitGraphs(*Node.Contained, Visit);
			}
		}
	}

	/** Every graph of the model: the top-level ones, the functions', and the collapse nodes' inside them. */
	void VisitAllGraphs(const FDcrModel& Extracted, TFunctionRef<void(const FDcrModelGraph&)> Visit)
	{
		for (const FDcrModelGraph& Graph : Extracted.Graphs)
		{
			VisitGraphs(Graph, Visit);
		}
		for (const FDcrModelFunction& Function : Extracted.Functions)
		{
			VisitGraphs(Function.Graph, Visit);
		}
	}

	int32 CountNodes(const FDcrModel& Extracted)
	{
		int32 Count = 0;
		VisitAllGraphs(Extracted, [&Count](const FDcrModelGraph& Graph) { Count += Graph.Nodes.Num(); });
		return Count;
	}

	int32 CountLinks(const FDcrModel& Extracted)
	{
		int32 Count = 0;
		VisitAllGraphs(Extracted, [&Count](const FDcrModelGraph& Graph) { Count += Graph.Links.Num(); });
		return Count;
	}

	int32 CountGraphs(const FDcrModel& Extracted)
	{
		int32 Count = 0;
		VisitAllGraphs(Extracted, [&Count](const FDcrModelGraph&) { ++Count; });
		return Count;
	}

	int32 CountKind(const FDcrModel& Extracted, EDcrNodeKind Kind)
	{
		int32 Count = 0;
		VisitAllGraphs(Extracted, [&Count, Kind](const FDcrModelGraph& Graph)
		{
			for (const FDcrModelNode& Node : Graph.Nodes)
			{
				Count += Node.Kind == Kind ? 1 : 0;
			}
		});
		return Count;
	}

	bool IsSequenceNode(const FDcrModelNode& Node)
	{
		return Node.Kind == EDcrNodeKind::Unit
			&& (Node.StructPath.EndsWith(TEXT("RigVMFunction_Sequence")) || Node.StructPath.EndsWith(TEXT("RigUnit_SequenceExecution")));
	}

	int32 CountSequences(const FDcrModel& Extracted)
	{
		int32 Count = 0;
		VisitAllGraphs(Extracted, [&Count](const FDcrModelGraph& Graph)
		{
			for (const FDcrModelNode& Node : Graph.Nodes)
			{
				Count += IsSequenceNode(Node) ? 1 : 0;
			}
		});
		return Count;
	}

	const FDcrCorpusEntry* FindEntry(const TCHAR* Name)
	{
		for (const FDcrCorpusEntry& Entry : GetDcrCorpus())
		{
			if (FCString::Strcmp(Entry.Name, Name) == 0)
			{
				return &Entry;
			}
		}
		return nullptr;
	}

	/** Loads and extracts, reporting a rig that does not load or extract as an error. */
	bool LoadAndExtract(FAutomationTestBase& Test, const TCHAR* ObjectPath, const TCHAR* Name, FDcrRig& OutRig, FDcrModel& OutModel)
	{
		OutRig = LoadDcrRig(ObjectPath);
		if (!OutRig.IsValid())
		{
			Test.AddError(FString::Printf(TEXT("%s: could not load %s"), Name, ObjectPath));
			return false;
		}
		const FDcrExtractResult Result = ExtractDcrModel(OutRig, OutModel);
		if (!Result.bSuccess)
		{
			Test.AddError(FString::Printf(TEXT("%s: extraction failed: %s"), Name, *Result.Error));
			return false;
		}
		return true;
	}

	/** Whether two lists of elements are the same elements in the same order with the same properties; says where not. */
	bool SameElements(const TArray<FDcrModelElement>& A, const TArray<FDcrModelElement>& B, FString& OutWhere)
	{
		if (A.Num() != B.Num())
		{
			OutWhere = FString::Printf(TEXT("%d elements against %d"), A.Num(), B.Num());
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			const FDcrModelElement& ElementA = A[Index];
			const FDcrModelElement& ElementB = B[Index];
			bool bSame = ElementA.Kind == ElementB.Kind && ElementA.Name.Equals(ElementB.Name, ESearchCase::CaseSensitive)
				&& ElementA.ParentKind == ElementB.ParentKind && ElementA.Parent.Equals(ElementB.Parent, ESearchCase::CaseSensitive)
				&& ElementA.Properties.Num() == ElementB.Properties.Num();
			for (int32 Property = 0; bSame && Property < ElementA.Properties.Num(); ++Property)
			{
				bSame = ElementA.Properties[Property].Name.Equals(ElementB.Properties[Property].Name, ESearchCase::CaseSensitive)
					&& ElementA.Properties[Property].Value.Equals(ElementB.Properties[Property].Value, ESearchCase::CaseSensitive);
			}
			if (!bSame)
			{
				OutWhere = FString::Printf(TEXT("element %d (%s %s)"), Index, LexToString(ElementA.Kind), *ElementA.Name);
				return false;
			}
		}
		return true;
	}

	FString DescribeFirstDifference(const FString& A, const FString& B)
	{
		TArray<FString> LinesA;
		TArray<FString> LinesB;
		A.ParseIntoArrayLines(LinesA, false);
		B.ParseIntoArrayLines(LinesB, false);
		const int32 Shared = FMath::Min(LinesA.Num(), LinesB.Num());
		for (int32 Index = 0; Index < Shared; ++Index)
		{
			if (!LinesA[Index].Equals(LinesB[Index], ESearchCase::CaseSensitive))
			{
				return FString::Printf(TEXT("line %d: '%s' against '%s'"), Index + 1, *LinesA[Index], *LinesB[Index]);
			}
		}
		return FString::Printf(TEXT("%d lines against %d"), LinesA.Num(), LinesB.Num());
	}

	/** The nodes of the graph that execution reaches from its events and entry, following execute links. */
	TSet<FString> FindExecuted(const FDcrModelGraph& Graph)
	{
		TSet<FString> Reached;
		TArray<FString> Pending;
		for (const FDcrModelNode& Node : Graph.Nodes)
		{
			const bool bEvent = Node.Kind == EDcrNodeKind::Unit && Node.ExecuteIn.IsEmpty() && !Node.ExecuteOut.IsEmpty();
			if (bEvent || Node.Kind == EDcrNodeKind::Entry)
			{
				Pending.Add(Node.Name);
			}
		}
		while (!Pending.IsEmpty())
		{
			const FString Name = Pending.Pop();
			if (Reached.Contains(Name))
			{
				continue;
			}
			Reached.Add(Name);
			const FDcrModelNode* Node = Graph.FindNode(Name);
			if (!Node)
			{
				continue;
			}
			for (const FDcrModelLink& Link : Graph.Links)
			{
				int32 SourceDot = INDEX_NONE;
				if (!Link.Source.FindChar(TEXT('.'), SourceDot) || Link.Source.Left(SourceDot) != Name)
				{
					continue;
				}
				if (!Node->ExecuteOut.Contains(Link.Source.Mid(SourceDot + 1)))
				{
					continue;
				}
				int32 TargetDot = INDEX_NONE;
				if (Link.Target.FindChar(TEXT('.'), TargetDot))
				{
					Pending.Add(Link.Target.Left(TargetDot));
				}
			}
		}
		return Reached;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrExtractCorpusTest, "DreamAnim.Rig.Extract.Corpus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDcrExtractCorpusTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig;
	using namespace UE::DreamAnim::Rig::Tests;
	using namespace UE::DreamAnim::Rig::Tests::ExtractTestsPrivate;
	namespace Model = UE::DreamAnim::Model;

	int32 Extracted = 0;
	int32 TotalUnsupported = 0;
	for (const FDcrCorpusEntry& Entry : GetDcrCorpus())
	{
		FDcrRig Rig;
		Model::FDcrModel ExtractedModel;
		if (!LoadAndExtract(*this, Entry.ObjectPath, Entry.Name, Rig, ExtractedModel))
		{
			continue;
		}
		++Extracted;
		TotalUnsupported += ExtractedModel.Unsupported.Num();
		// The hierarchy on its own is the one the model holds.
		TArray<Model::FDcrModelElement> Alone;
		TArray<Model::FDcrModelUnsupported> AloneUnsupported;
		if (URigHierarchy* Hierarchy = Rig.ControlRig->GetHierarchy())
		{
			ExtractDcrHierarchy(*Hierarchy, Alone, AloneUnsupported);
			FString Where;
			if (!SameElements(ExtractedModel.Hierarchy, Alone, Where))
			{
				AddError(FString::Printf(TEXT("%s: the hierarchy extracted on its own differs from the model's: %s"), Entry.Name, *Where));
			}
		}
		AddInfo(FString::Printf(TEXT("%s (%s): %d graphs, %d nodes, %d links, %d functions, %d elements, %d variables, %d unsupported"),
			Entry.Name, LexToString(Rig.GetKind()), CountGraphs(ExtractedModel), CountNodes(ExtractedModel), CountLinks(ExtractedModel),
			ExtractedModel.Functions.Num(), ExtractedModel.Hierarchy.Num(), ExtractedModel.Variables.Num(), ExtractedModel.Unsupported.Num()));
		for (int32 Index = 0; Index < ExtractedModel.Unsupported.Num() && Index < 4; ++Index)
		{
			AddInfo(FString::Printf(TEXT("    %s: %s"), *ExtractedModel.Unsupported[Index].Location, *ExtractedModel.Unsupported[Index].Description));
		}
	}
	AddInfo(FString::Printf(TEXT("%d of %d rigs extracted, %d unsupported entries in all"), Extracted, GetDcrCorpus().Num(), TotalUnsupported));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrExtractDumpTest, "DreamAnim.Rig.Extract.Dump",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDcrExtractDumpTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig;
	using namespace UE::DreamAnim::Rig::Tests;
	using namespace UE::DreamAnim::Rig::Tests::ExtractTestsPrivate;
	namespace Model = UE::DreamAnim::Model;

	const FString Folder = FString::Printf(TEXT("/Game/DreamAnimRigTests/Extract/%s"), *FDateTime::Now().ToString(TEXT("%H%M%S")));
	static const TCHAR* const Names[] = { TEXT("CR_Mannequin_BasicFootIK"), TEXT("CR_Mannequin_Procedural"), TEXT("Hinge"), TEXT("CRU_ChainSim"), TEXT("CR_PhysCube") };

	for (const TCHAR* Name : Names)
	{
		const FDcrCorpusEntry* Entry = FindEntry(Name);
		if (!TestNotNull(FString::Printf(TEXT("%s is in the corpus"), Name), Entry))
		{
			continue;
		}
		FDcrRig Rig;
		Model::FDcrModel First;
		if (!LoadAndExtract(*this, Entry->ObjectPath, Name, Rig, First))
		{
			continue;
		}

		// The same rig read twice.
		Model::FDcrModel Second;
		if (!ExtractDcrModel(Rig, Second).bSuccess)
		{
			AddError(FString::Printf(TEXT("%s: second extraction failed"), Name));
			continue;
		}
		const FString DumpFirst = Model::DumpDcrModel(First);
		const FString DumpSecond = Model::DumpDcrModel(Second);
		if (!DumpFirst.Equals(DumpSecond, ESearchCase::CaseSensitive))
		{
			AddError(FString::Printf(TEXT("%s: two extractions dump differently: %s"), Name, *DescribeFirstDifference(DumpFirst, DumpSecond)));
		}
		TestTrue(FString::Printf(TEXT("%s: the dump is not empty"), Name), DumpFirst.Len() > 0);

		// Its own names, and the float math read as double: still the same text each time.
		Model::FDcrDumpOptions Plain;
		Plain.bCanonicalNodeNames = false;
		Plain.bFloatAsDouble = true;
		if (!Model::DumpDcrModel(First, Plain).Equals(Model::DumpDcrModel(Second, Plain), ESearchCase::CaseSensitive))
		{
			AddError(FString::Printf(TEXT("%s: the dump with the model's own names differs between two extractions"), Name));
		}

		// A copy of the asset.
		const FDcrRig Copy = DuplicateDcrRig(Rig, Folder, FString(Name) + TEXT("_Copy"));
		if (!Copy.IsValid())
		{
			AddError(FString::Printf(TEXT("%s: could not duplicate the asset"), Name));
			continue;
		}
		Model::FDcrModel Copied;
		if (!ExtractDcrModel(Copy, Copied).bSuccess)
		{
			AddError(FString::Printf(TEXT("%s: the copy did not extract"), Name));
			continue;
		}
		// The engine sets the copy's hierarchy up again (rotations may come back as the same rotation with the
		// other sign) and names a module after its asset: the graphs, functions and variables are what must match.
		Copied.Hierarchy = First.Hierarchy;
		Copied.ModuleSettings = First.ModuleSettings;
		const FString DumpCopied = Model::DumpDcrModel(Copied);
		if (!DumpFirst.Equals(DumpCopied, ESearchCase::CaseSensitive))
		{
			AddError(FString::Printf(TEXT("%s: the copy dumps differently from the original: %s"), Name, *DescribeFirstDifference(DumpFirst, DumpCopied)));
		}
		TArray<FString> DumpLines;
		DumpFirst.ParseIntoArrayLines(DumpLines, false);
		AddInfo(FString::Printf(TEXT("%s: %d characters, %d lines"), Name, DumpFirst.Len(), DumpLines.Num()));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrExtractNormalizeTest, "DreamAnim.Rig.Extract.Normalize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDcrExtractNormalizeTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig;
	using namespace UE::DreamAnim::Rig::Tests;
	using namespace UE::DreamAnim::Rig::Tests::ExtractTestsPrivate;
	namespace Model = UE::DreamAnim::Model;

	// Sequences flattened in seven of the engine's rigs before the normalizer existed: the numbers to meet.
	struct FKnownCount
	{
		const TCHAR* Name;
		int32 Flattened;
	};
	static const FKnownCount Known[] = {
		{ TEXT("CR_Mannequin_BasicFootIK"), 1 },
		{ TEXT("CR_Mannequin_Procedural"), 1 },
		{ TEXT("CR_Mannequin_Body"), 32 },
		{ TEXT("CRD_Lattice"), 19 },
	};

	int32 TotalFlattened = 0;
	int32 TotalFolded = 0;
	for (const FDcrCorpusEntry& Entry : GetDcrCorpus())
	{
		FDcrRig Rig;
		Model::FDcrModel Extracted;
		if (!LoadAndExtract(*this, Entry.ObjectPath, Entry.Name, Rig, Extracted))
		{
			continue;
		}
		const int32 SequencesBefore = CountSequences(Extracted);
		const int32 ReroutesBefore = CountKind(Extracted, Model::EDcrNodeKind::Reroute);
		const FDcrNormalizeReport Report = NormalizeDcrModel(Extracted);
		TotalFlattened += Report.SequencesFlattened;
		TotalFolded += Report.ReroutesFolded;

		const int32 ReroutesLeft = CountKind(Extracted, Model::EDcrNodeKind::Reroute);
		const int32 SequencesLeft = CountSequences(Extracted);
		TestEqual(FString::Printf(TEXT("%s: reroutes left"), Entry.Name), ReroutesLeft, 0);
		TestEqual(FString::Printf(TEXT("%s: sequences left are the ones kept"), Entry.Name), SequencesLeft, Report.SequencesKept);
		TestEqual(FString::Printf(TEXT("%s: sequences before = flattened + kept"), Entry.Name), SequencesBefore, Report.SequencesFlattened + Report.SequencesKept);
		TestEqual(FString::Printf(TEXT("%s: reroutes before = folded + left"), Entry.Name), ReroutesBefore, Report.ReroutesFolded + ReroutesLeft);

		for (const FKnownCount& Count : Known)
		{
			if (FCString::Strcmp(Count.Name, Entry.Name) == 0)
			{
				TestEqual(FString::Printf(TEXT("%s: sequences flattened"), Entry.Name), Report.SequencesFlattened, Count.Flattened);
			}
		}

		// A normalized model has nothing left to normalize.
		const FDcrNormalizeReport Again = NormalizeDcrModel(Extracted);
		TestEqual(FString::Printf(TEXT("%s: a second pass flattens nothing"), Entry.Name), Again.SequencesFlattened, 0);
		TestEqual(FString::Printf(TEXT("%s: a second pass folds nothing"), Entry.Name), Again.ReroutesFolded, 0);
		TestEqual(FString::Printf(TEXT("%s: a second pass drops nothing"), Entry.Name), Again.LinkedValuesDropped, 0);

		AddInfo(FString::Printf(TEXT("%s: %d sequences flattened, %d kept, %d reroutes folded, %d values on linked pins dropped"),
			Entry.Name, Report.SequencesFlattened, Report.SequencesKept, Report.ReroutesFolded, Report.LinkedValuesDropped));
		for (const FString& Note : Report.Notes)
		{
			AddInfo(TEXT("    ") + Note);
		}
	}
	AddInfo(FString::Printf(TEXT("%d sequences flattened and %d reroutes folded in all"), TotalFlattened, TotalFolded));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrExtractFootIKTest, "DreamAnim.Rig.Extract.BasicFootIK",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDcrExtractFootIKTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig;
	using namespace UE::DreamAnim::Rig::Tests;
	using namespace UE::DreamAnim::Rig::Tests::ExtractTestsPrivate;
	namespace Model = UE::DreamAnim::Model;

	FDcrRig Rig;
	Model::FDcrModel Extracted;
	if (!LoadAndExtract(*this, FootIKPath, TEXT("CR_Mannequin_BasicFootIK"), Rig, Extracted))
	{
		return false;
	}

	// The FootTrace function: one input, one output, one local.
	const Model::FDcrModelFunction* FootTrace = Extracted.Functions.FindByPredicate([](const Model::FDcrModelFunction& Function) { return Function.Name == TEXT("FootTrace"); });
	if (TestNotNull(TEXT("the FootTrace function"), FootTrace))
	{
		TestTrue(TEXT("FootTrace is mutable"), FootTrace->bMutable);
		const Model::FDcrModelFunctionPin* Input = FootTrace->Inputs.FindByPredicate([](const Model::FDcrModelFunctionPin& Pin) { return Pin.Name == TEXT("Ik Foot Bone"); });
		if (TestNotNull(TEXT("FootTrace input 'Ik Foot Bone'"), Input))
		{
			TestEqual(TEXT("'Ik Foot Bone' type"), Input->Type.CPPType, FString(TEXT("FRigElementKey")));
			TestEqual(TEXT("'Ik Foot Bone' type object"), Input->Type.ObjectPath, FString(TEXT("/Script/ControlRig.RigElementKey")));
		}
		TestEqual(TEXT("FootTrace inputs"), FootTrace->Inputs.Num(), 1);
		const Model::FDcrModelFunctionPin* Output = FootTrace->Outputs.FindByPredicate([](const Model::FDcrModelFunctionPin& Pin) { return Pin.Name == TEXT("Offset"); });
		if (TestNotNull(TEXT("FootTrace output 'Offset'"), Output))
		{
			TestEqual(TEXT("'Offset' type"), Output->Type.CPPType, FString(TEXT("FVector")));
		}
		TestEqual(TEXT("FootTrace outputs"), FootTrace->Outputs.Num(), 1);
		TestTrue(TEXT("FootTrace local 'AnimatedFloorLocation'"), FootTrace->Locals.ContainsByPredicate([](const Model::FDcrModelVariable& Local) { return Local.Name == TEXT("AnimatedFloorLocation"); }));
		TestTrue(TEXT("FootTrace has an Entry"), FootTrace->Graph.Nodes.ContainsByPredicate([](const Model::FDcrModelNode& Node) { return Node.Kind == Model::EDcrNodeKind::Entry; }));
		TestTrue(TEXT("FootTrace has a Return"), FootTrace->Graph.Nodes.ContainsByPredicate([](const Model::FDcrModelNode& Node) { return Node.Kind == Model::EDcrNodeKind::Return; }));
	}

	// Three Visual Debug nodes sit on pins instead of in the flow.
	int32 VisualDebug = 0;
	int32 VisualDebugInjected = 0;
	VisitAllGraphs(Extracted, [&VisualDebug, &VisualDebugInjected](const Model::FDcrModelGraph& Graph)
	{
		for (const Model::FDcrModelNode& Node : Graph.Nodes)
		{
			if (Node.Kind == Model::EDcrNodeKind::Unit && Node.StructPath.EndsWith(TEXT("RigUnit_VisualDebugVectorItemSpace")))
			{
				++VisualDebug;
				VisualDebugInjected += Node.InjectedOn.IsEmpty() ? 0 : 1;
			}
		}
	});
	TestEqual(TEXT("VisualDebugVectorItemSpace nodes"), VisualDebug, 3);
	TestEqual(TEXT("VisualDebugVectorItemSpace nodes that are injected"), VisualDebugInjected, 3);

	// The default graph.
	if (!TestTrue(TEXT("the rig has a graph"), Extracted.Graphs.Num() > 0))
	{
		return false;
	}
	const Model::FDcrModelGraph& Graph = Extracted.Graphs[0];
	TestEqual(TEXT("the first graph is the default one"), Graph.Name, FString(TEXT("RigVMModel")));

	const Model::FDcrModelNode* Begin = Graph.Nodes.FindByPredicate([](const Model::FDcrModelNode& Node) { return Node.Kind == Model::EDcrNodeKind::Unit && Node.StructPath.EndsWith(TEXT("RigUnit_BeginExecution")); });
	if (TestNotNull(TEXT("the Forwards Solve event"), Begin))
	{
		TestEqual(TEXT("the event has one execute output"), Begin->ExecuteOut.Num(), 1);
		TestEqual(TEXT("the event takes no execution in"), Begin->ExecuteIn.Num(), 0);
	}

	// A Branch reads ShouldDoIKTrace.
	const Model::FDcrModelNode* Branch = Graph.Nodes.FindByPredicate([](const Model::FDcrModelNode& Node) { return Node.Kind == Model::EDcrNodeKind::Unit && Node.StructPath.EndsWith(TEXT("RigVMFunction_ControlFlowBranch")); });
	if (TestNotNull(TEXT("the Branch"), Branch))
	{
		const FString Condition = Branch->Name + TEXT(".Condition");
		const Model::FDcrModelLink* Link = Graph.Links.FindByPredicate([&Condition](const Model::FDcrModelLink& Candidate) { return Candidate.Target == Condition; });
		if (TestNotNull(TEXT("a link into the Branch's Condition"), Link))
		{
			FString SourceNode;
			FString SourcePin;
			TestTrue(TEXT("the Condition's link names a pin"), Link->Source.Split(TEXT("."), &SourceNode, &SourcePin));
			const Model::FDcrModelNode* Source = Graph.FindNode(SourceNode);
			if (TestNotNull(TEXT("the node the Condition is read from"), Source))
			{
				TestTrue(TEXT("the Condition reads a variable"), Source->Kind == Model::EDcrNodeKind::VariableGet);
				TestEqual(TEXT("the variable the Condition reads"), Source->Variable, FString(TEXT("ShouldDoIKTrace")));
			}
		}
	}

	// Two setters of the targets are in the graph without anything running them.
	const TSet<FString> Executed = FindExecuted(Graph);
	int32 Setters[2] = { 0, 0 };
	int32 SettersNotExecuted[2] = { 0, 0 };
	static const TCHAR* const Targets[2] = { TEXT("ZOffset_L_Target"), TEXT("ZOffset_R_Target") };
	for (const Model::FDcrModelNode& Node : Graph.Nodes)
	{
		if (Node.Kind != Model::EDcrNodeKind::VariableSet)
		{
			continue;
		}
		for (int32 Index = 0; Index < 2; ++Index)
		{
			if (Node.Variable == Targets[Index])
			{
				++Setters[Index];
				SettersNotExecuted[Index] += Executed.Contains(Node.Name) ? 0 : 1;
			}
		}
	}
	for (int32 Index = 0; Index < 2; ++Index)
	{
		TestEqual(FString::Printf(TEXT("setters of %s"), Targets[Index]), Setters[Index], 2);
		TestEqual(FString::Printf(TEXT("setters of %s that nothing runs"), Targets[Index]), SettersNotExecuted[Index], 1);
	}

	// The aggregated Sequence is one node, not a graph of its own.
	const Model::FDcrModelNode* Sequence = Graph.Nodes.FindByPredicate([](const Model::FDcrModelNode& Node) { return IsSequenceNode(Node); });
	if (TestNotNull(TEXT("the Sequence"), Sequence))
	{
		TestEqual(TEXT("the pins the Sequence has past its first two"), FString::Join(Sequence->AggregatePins, TEXT(",")), FString(TEXT("C,D")));
	}
	TestTrue(TEXT("the rig has reroutes before it is normalized"), CountKind(Extracted, Model::EDcrNodeKind::Reroute) > 0);

	// Normalizing flattens the Sequence and folds the reroutes.
	const FDcrNormalizeReport Report = NormalizeDcrModel(Extracted);
	TestEqual(TEXT("sequences flattened"), Report.SequencesFlattened, 1);
	TestEqual(TEXT("sequences left"), CountSequences(Extracted), 0);
	TestEqual(TEXT("reroutes left"), CountKind(Extracted, Model::EDcrNodeKind::Reroute), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDcrExtractFreshNodesTest, "DreamAnim.Rig.Extract.FreshNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDcrExtractFreshNodesTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Rig;
	using namespace UE::DreamAnim::Rig::Tests;
	using namespace UE::DreamAnim::Rig::Tests::ExtractTestsPrivate;
	namespace Model = UE::DreamAnim::Model;

	// Nodes added with nothing set hold nothing worth keeping; one value set on one pin is the only one found.
	const FString Folder = FString::Printf(TEXT("/Game/DreamAnimRigTests/Extract/%s"), *FDateTime::Now().ToString(TEXT("%H%M%S")));
	FDcrRig Rig = CreateDcrRig(Folder / TEXT("CR_Fresh"), EDcrAssetKind::Independent);
	if (!TestTrue(TEXT("the rig is created"), Rig.IsValid()))
	{
		return false;
	}
	URigVMGraph* Graph = Rig.RigVM->GetDefaultModel();
	URigVMController* Controller = Graph ? GetDcrController(Rig, Graph) : nullptr;
	if (!TestNotNull(TEXT("the default graph's controller"), Controller))
	{
		return false;
	}

	Controller->AddUnitNodeFromStructPath(TEXT("/Script/ControlRig.RigUnit_BeginExecution"), TEXT("Execute"), FVector2D(0.f, 0.f), TEXT("Begin"), false, false);
	Controller->AddUnitNodeFromStructPath(TEXT("/Script/ControlRig.RigUnit_GetTransform"), TEXT("Execute"), FVector2D(250.f, 0.f), TEXT("Get"), false, false);
	Controller->AddUnitNodeFromStructPath(TEXT("/Script/RigVM.RigVMFunction_MathVectorAdd"), TEXT("Execute"), FVector2D(500.f, 0.f), TEXT("Add"), false, false);
	Controller->SetPinDefaultValue(TEXT("Add.B"), TEXT("(X=1.000000,Y=2.000000,Z=3.000000)"), true, false, false, false);
	URigVMNode* If = Controller->AddTemplateNode(TEXT("DISPATCH_RigVMDispatch_If(in Condition,in True,in False,out Result)"), FVector2D(750.f, 0.f), TEXT("If"), false, false);
	if (TestNotNull(TEXT("the If node"), If))
	{
		TestTrue(TEXT("the If node resolves to double"), Controller->ResolveWildCardPin(TEXT("If.True"), TEXT("double"), NAME_None, false, false));
	}

	Model::FDcrModel Extracted;
	if (!TestTrue(TEXT("the new rig extracts"), ExtractDcrModel(Rig, Extracted).bSuccess) || !TestTrue(TEXT("the new rig has a graph"), Extracted.Graphs.Num() > 0))
	{
		return false;
	}
	const Model::FDcrModelGraph& Extract = Extracted.Graphs[0];

	const Model::FDcrModelNode* Begin = Extract.FindNode(TEXT("Begin"));
	if (TestNotNull(TEXT("Begin"), Begin))
	{
		TestEqual(TEXT("Begin has no values"), Begin->Values.Num(), 0);
	}
	const Model::FDcrModelNode* Get = Extract.FindNode(TEXT("Get"));
	if (TestNotNull(TEXT("Get"), Get))
	{
		TestEqual(TEXT("a new GetTransform has no values"), Get->Values.Num(), 0);
		TestTrue(TEXT("GetTransform is a unit"), Get->Kind == Model::EDcrNodeKind::Unit);
	}
	const Model::FDcrModelNode* Add = Extract.FindNode(TEXT("Add"));
	if (TestNotNull(TEXT("Add"), Add))
	{
		TestEqual(TEXT("Add holds one value"), Add->Values.Num(), 1);
		if (Add->Values.Num() == 1)
		{
			TestEqual(TEXT("the pin Add holds a value on"), Add->Values[0].Name, FString(TEXT("B")));
		}
	}
	const Model::FDcrModelNode* IfNode = Extract.FindNode(TEXT("If"));
	if (TestNotNull(TEXT("If"), IfNode))
	{
		TestTrue(TEXT("If is a dispatch"), IfNode->Kind == Model::EDcrNodeKind::Dispatch);
		TestFalse(TEXT("If has a notation"), IfNode->Notation.IsEmpty());
		TArray<FString> Resolved;
		for (const TPair<FString, Model::FDcrType>& Pair : IfNode->ResolvedTypes)
		{
			Resolved.Add(Pair.Key + TEXT("=") + Pair.Value.CPPType);
		}
		Resolved.Sort();
		TestEqual(TEXT("the pins If resolved to double"), FString::Join(Resolved, TEXT(",")), FString(TEXT("False=double,Result=double,True=double")));
		// Resolved wildcard pins always carry their value; the condition, a plain bool, holds nothing.
		TArray<FString> ValueNames;
		for (const Model::FDcrValue& Value : IfNode->Values)
		{
			ValueNames.Add(Value.Name);
		}
		ValueNames.Sort();
		TestEqual(TEXT("the pins If holds values on"), FString::Join(ValueNames, TEXT(",")), FString(TEXT("False,True")));
	}
	return true;
}

#endif
