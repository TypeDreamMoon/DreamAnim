// Copyright DreamMoon. All Rights Reserved.
//
// Graphs: nodes matched by NodeGuid, each node's arguments compared as the lowering writes them, a value
// that changed spliced into its own text, a node whose arguments came or went reprinted, and a graph whose
// nodes or links changed rewritten from the Blueprint.

#include "WriteBack/DabWriteBackInternal.h"

namespace UE::DreamAnim::Compiler::WriteBackPrivate
{
	/** A pose argument as LowerDabNodeCall writes it: the marker, or a list or block holding markers. */
	bool ContainsPoseMarker(const FDabExpr* Expr)
	{
		if (!Expr)
		{
			return false;
		}
		if (const FDabNameExpr* Name = Expr->As<FDabNameExpr>())
		{
			return Name->Name == DabPoseMarker;
		}
		bool bFound = false;
		ForEachChild(*Expr, [&bFound](const FDabNode& Child)
		{
			if (bFound)
			{
				return;
			}
			if (Child.Kind <= EDabNodeKind::ParenExpr)
			{
				bFound = ContainsPoseMarker(static_cast<const FDabExpr*>(&Child));
			}
			else if (const FDabArgument* Argument = Child.As<FDabArgument>())
			{
				bFound = ContainsPoseMarker(Argument->Value.Get());
			}
			else if (const FDabAssignStmt* Assign = Child.As<FDabAssignStmt>())
			{
				bFound = ContainsPoseMarker(Assign->Value.Get());
			}
		});
		return bFound;
	}

	/** `N:Name` for a named argument, `P:<index>` for a positional one. */
	TArray<TPair<FString, FDabArgument*>> KeyArguments(const FDabCallExpr& Call)
	{
		TArray<TPair<FString, FDabArgument*>> Keyed;
		int32 Positional = 0;
		for (const FDabArgumentPtr& Argument : Call.Args)
		{
			if (!Argument)
			{
				continue;
			}
			const FString Key = Argument->IsNamed() ? TEXT("N:") + Argument->Name : FString::Printf(TEXT("P:%d"), Positional++);
			Keyed.Add({ Key, Argument.Get() });
		}
		return Keyed;
	}

	FDabArgument* FindKey(const TArray<TPair<FString, FDabArgument*>>& Keyed, const FString& Key)
	{
		const TPair<FString, FDabArgument*>* Found = Keyed.FindByPredicate([&Key](const TPair<FString, FDabArgument*>& Pair) { return Pair.Key == Key; });
		return Found ? Found->Value : nullptr;
	}

	FString DescribeKey(const FString& Key)
	{
		return Key.StartsWith(TEXT("N:")) ? Key.RightChop(2) : FString::Printf(TEXT("argument %s"), *Key.RightChop(2));
	}

	FString DescribeNode(const FDabModelNode& Node, const FString& Location)
	{
		FString Type = Node.ClassName;
		int32 At = Type.Find(TEXT("AnimGraphNode_"));
		if (At != INDEX_NONE)
		{
			Type = Type.Mid(At + 14);
		}
		return FString::Printf(TEXT("%s: %s"), *Location, *Type);
	}

	bool FWriteBack::HasStructuralChange(const FString& GraphPath, const FDabModelGraph& TextGraph, const FDabModelGraph& EditedGraph) const
	{
		TMap<FString, FGuid> TextGuids;
		TSet<FGuid> TextGuidSet;
		for (const FDabModelNode& Node : TextGraph.Nodes)
		{
			const FGuid Guid = TextGuid(GraphPath, Node.Id);
			TextGuids.Add(Node.Id, Guid);
			TextGuidSet.Add(Guid);
		}
		TMap<FString, FGuid> EditedGuids;
		for (const FDabModelNode& Node : EditedGraph.Nodes)
		{
			EditedGuids.Add(Node.Id, Node.Guid);
			if (!TextGuidSet.Contains(Node.Guid))
			{
				return true;
			}
		}

		const auto LinksOf = [](const FDabModelNode& Node, const TMap<FString, FGuid>& Guids)
		{
			TMap<FString, FGuid> Links;
			for (const FDabModelPoseLink& Link : Node.PoseInputs)
			{
				const FGuid* Source = Guids.Find(Link.SourceNodeId);
				Links.Add(Link.PinName, Source ? *Source : FGuid());
			}
			return Links;
		};

		for (const FDabModelNode& Node : TextGraph.Nodes)
		{
			const FEditedNode* Match = EditedNodes.Find(TextGuids[Node.Id]);
			if (!Match || Match->Graph != &EditedGraph)
			{
				return true;
			}
			const FDabModelNode& Other = *Match->Node;
			if (Node.ClassName != Other.ClassName || Node.CacheName != Other.CacheName || Node.StateMachineName != Other.StateMachineName
				|| Node.InputPoseName != Other.InputPoseName)
			{
				return true;
			}
			const TMap<FString, FGuid> TextLinks = LinksOf(Node, TextGuids);
			const TMap<FString, FGuid> EditedLinks = LinksOf(Other, EditedGuids);
			if (TextLinks.Num() != EditedLinks.Num())
			{
				return true;
			}
			for (const TPair<FString, FGuid>& Link : TextLinks)
			{
				const FGuid* OtherSource = EditedLinks.Find(Link.Key);
				if (!OtherSource || *OtherSource != Link.Value)
				{
					return true;
				}
			}
		}

		const FGuid* TextResult = TextGuids.Find(TextGraph.ResultNodeId);
		const FGuid* EditedResult = EditedGuids.Find(EditedGraph.ResultNodeId);
		if ((TextResult ? *TextResult : FGuid()) != (EditedResult ? *EditedResult : FGuid()))
		{
			return true;
		}
		return TextGraph.InputPoses != EditedGraph.InputPoses;
	}

	void FWriteBack::CompareGraph(const FString& GraphPath, const FDabModelGraph& TextGraph, const FDabModelGraph* EditedGraph, const FString& Location)
	{
		if (!EditedGraph)
		{
			return;
		}
		if (HasStructuralChange(GraphPath, TextGraph, *EditedGraph))
		{
			if (Options.bStructure)
			{
				RewriteGraph(GraphPath, *EditedGraph, Location);
			}
			else
			{
				Change(Location, TEXT("the graph's nodes or links"), false);
				Refuse(TEXT("DAB8003"), FDabSpan(), FString::Printf(TEXT("%s: nodes or links changed in the editor; structural changes are not written back."), *Location),
					TEXT("Make the change in the text, or turn on writing structural changes back."));
			}
			return;
		}

		// Result properties (a layer's group, blend options) live on the layer's statement: see CompareDocument.
		for (const FDabModelNode& Node : TextGraph.Nodes)
		{
			if (RewrittenGraphs.Contains(GraphPath))
			{
				return;
			}
			if (const FEditedNode* Match = EditedNodes.Find(TextGuid(GraphPath, Node.Id)))
			{
				CompareNode(GraphPath, TextGraph, Node, *Match->Graph, *Match->Node, Location);
			}
		}
	}

	bool FWriteBack::ReplaceArgumentValue(FDabArgument& Arg, const FDabExpr& Value)
	{
		if (!Arg.Value || Arg.Value->Span.Length == 0)
		{
			// Written by an earlier change (a template argument two nodes share).
			return false;
		}
		const FDabSpan OldSpan = Arg.Value->Span;
		GraveyardExprs.Add(MoveTemp(Arg.Value));
		Arg.Value = CloneExpr(Value);
		Arg.Value->Span = FDabSpan();
		Text->ReprintExpr(OldSpan, *Arg.Value);
		return true;
	}

	void FWriteBack::CompareNode(const FString& GraphPath, const FDabModelGraph& TextGraph, const FDabModelNode& TextNode,
		const FDabModelGraph& EditedGraph, const FDabModelNode& EditedNode, const FString& Location)
	{
		const FDabProvenance::FNodeOrigin* Origin = Provenance.Nodes.Find(GraphPath + TEXT("/") + TextNode.Id);
		const FString Where = DescribeNode(TextNode, Location);

		// Node functions live in the metadata of the named pose the node roots.
		TArray<FString> TextFunctions;
		TArray<FString> EditedFunctions;
		for (const FDabModelNodeFunction& Function : TextNode.NodeFunctions)
		{
			TextFunctions.Add(Function.Site + TEXT("=") + PrintCall(Function.Call));
		}
		for (const FDabModelNodeFunction& Function : EditedNode.NodeFunctions)
		{
			EditedFunctions.Add(Function.Site + TEXT("=") + PrintCall(Function.Call));
		}
		TextFunctions.Sort();
		EditedFunctions.Sort();
		if (TextFunctions != EditedFunctions)
		{
			const FDabAssignStmt* const* Named = Provenance.NamedPoses.Find(GraphPath + TEXT("/") + TextNode.Id);
			FDabAssignStmt* Fresh = Named ? static_cast<FDabAssignStmt*>(Text->FindSame(**Named)) : nullptr;
			if (!Fresh)
			{
				Change(Where, TEXT("node functions"), false);
				Refuse(TEXT("DAB8006"), Origin && Origin->Call ? Origin->Call->Span : FDabSpan(),
					FString::Printf(TEXT("%s: node functions changed on a node that has no name in the text."), *Where),
					TEXT("Write the node as a named pose (`Name = Node(...) [ OnUpdate = ... ];`) and change it in the text."));
			}
			else
			{
				TUniquePtr<FDabMeta> Meta = MakeUnique<FDabMeta>();
				if (Fresh->Meta)
				{
					// Everything but the functions stays (Cached).
					for (const TUniquePtr<FDabMetaEntry>& Entry : Fresh->Meta->Entries)
					{
						if (Entry && !Entry->Key.StartsWith(TEXT("On")))
						{
							TUniquePtr<FDabMetaEntry> Kept = MakeUnique<FDabMetaEntry>();
							Kept->Key = Entry->Key;
							Kept->bKeyIsString = Entry->bKeyIsString;
							Kept->Value = Entry->Value ? CloneExpr(*Entry->Value) : nullptr;
							Meta->Entries.Add(MoveTemp(Kept));
						}
					}
				}
				for (const FDabModelNodeFunction& Function : EditedNode.NodeFunctions)
				{
					TUniquePtr<FDabMetaEntry> Entry = MakeUnique<FDabMetaEntry>();
					Entry->Key = TEXT("On") + Function.Site;
					Entry->Value = LowerDabCall(Edited, Function.Call, LowerOptions);
					Meta->Entries.Add(MoveTemp(Entry));
				}
				Fresh->Meta = Meta->Entries.IsEmpty() ? nullptr : MoveTemp(Meta);
				Text->ReprintStmt(Fresh->Span, *Fresh);
				Change(Where, TEXT("node functions"), true);
			}
		}

		if (!Origin || !Origin->Call)
		{
			// Cache uses, machines and inputs are names: nothing more on them can change without a structural change.
			return;
		}

		const FDabExprPtr OldLowered = LowerDabNodeCall(Compiled.Model, TextGraph, TextNode, Origin->FedPin, LowerOptions);
		const FDabExprPtr NewLowered = LowerDabNodeCall(Edited, EditedGraph, EditedNode, Origin->FedPin, LowerOptions);
		const FDabCallExpr* OldCall = OldLowered ? OldLowered->As<FDabCallExpr>() : nullptr;
		const FDabCallExpr* NewCall = NewLowered ? NewLowered->As<FDabCallExpr>() : nullptr;
		if (!OldCall || !NewCall || PrintDabExpr(*OldCall) == PrintDabExpr(*NewCall))
		{
			return;
		}

		const TArray<TPair<FString, FDabArgument*>> OldArgs = KeyArguments(*OldCall);
		const TArray<TPair<FString, FDabArgument*>> NewArgs = KeyArguments(*NewCall);
		TArray<FString> Changed;
		TArray<FString> Added;
		TArray<FString> Removed;
		for (const TPair<FString, FDabArgument*>& New : NewArgs)
		{
			const FDabArgument* Old = FindKey(OldArgs, New.Key);
			if (!Old)
			{
				Added.Add(New.Key);
			}
			else if (PrintDabExpr(*Old->Value) != PrintDabExpr(*New.Value->Value))
			{
				Changed.Add(New.Key);
			}
		}
		for (const TPair<FString, FDabArgument*>& Old : OldArgs)
		{
			if (!FindKey(NewArgs, Old.Key))
			{
				Removed.Add(Old.Key);
			}
		}

		TArray<FString> Described;
		for (const FString& Key : Changed)
		{
			Described.Add(DescribeKey(Key));
		}
		for (const FString& Key : Added)
		{
			Described.Add(DescribeKey(Key));
		}
		for (const FString& Key : Removed)
		{
			Described.Add(DescribeKey(Key));
		}
		const FString What = FString::Join(Described, TEXT(", "));

		// A pose argument that changed (a keyed blend's keys, a linked graph's inputs) is wiring: rewrite the graph.
		for (const FString& Key : Changed)
		{
			if (ContainsPoseMarker(FindKey(NewArgs, Key)->Value.Get()))
			{
				if (Options.bStructure)
				{
					RewriteGraph(GraphPath, EditedGraph, Location);
				}
				else
				{
					Change(Where, What, false);
					Refuse(TEXT("DAB8003"), Origin->Call->Span, FString::Printf(TEXT("%s: how the node's poses are wired changed; structural changes are not written back."), *Where));
				}
				return;
			}
		}

		FDabCallExpr* Fresh = static_cast<FDabCallExpr*>(Text->FindSame(*Origin->Call));
		if (!Fresh)
		{
			// The node comes from a template body: only values passed in as the template's arguments can be written,
			// unless the template may be written out.
			const auto WrittenInBody = [&]()
			{
				for (const FString& Key : Changed)
				{
					const FDabArgument* ExpandedArg = FindKey(KeyArguments(*Origin->Call), Key);
					const FDabNode* RealValue = ExpandedArg && ExpandedArg->Value ? Text->FindSame(*ExpandedArg->Value) : nullptr;
					const FDabNode* Parent = RealValue ? Text->GetParent(*RealValue) : nullptr;
					if (!Parent || !Parent->Is<FDabArgument>())
					{
						return true;
					}
				}
				return false;
			};
			if (Options.bExpandTemplates && Options.bStructure && (!Added.IsEmpty() || !Removed.IsEmpty() || WrittenInBody()))
			{
				RewriteGraph(GraphPath, EditedGraph, Location);
				return;
			}
			if (!Added.IsEmpty() || !Removed.IsEmpty())
			{
				Change(Where, What, false);
				Refuse(TEXT("DAB8001"), Origin->Call->Span, FString::Printf(TEXT("%s: the node is written inside a template, which has no place for `%s`."), *Where, *What),
					TEXT("Expand the template here, or change the template."));
				return;
			}
			const TArray<TPair<FString, FDabArgument*>> Expanded = KeyArguments(*Origin->Call);
			bool bAllWritten = true;
			for (const FString& Key : Changed)
			{
				const FDabArgument* ExpandedArg = FindKey(Expanded, Key);
				FDabNode* RealValue = ExpandedArg && ExpandedArg->Value ? Text->FindSame(*ExpandedArg->Value) : nullptr;
				FDabNode* Parent = RealValue ? Text->GetParent(*RealValue) : nullptr;
				FDabArgument* Owner = Parent ? Parent->As<FDabArgument>() : nullptr;
				if (!Owner)
				{
					bAllWritten = false;
					Refuse(TEXT("DAB8001"), Origin->Call->Span, FString::Printf(TEXT("%s: %s is written inside the template, not passed to it."), *Where, *DescribeKey(Key)),
						TEXT("Expand the template here, make the value a template parameter, or change the template."));
					continue;
				}
				if (Owner->Value.Get() == RealValue)
				{
					ReplaceArgumentValue(*Owner, *FindKey(NewArgs, Key)->Value);
				}
			}
			Change(Where, What, bAllWritten);
			return;
		}

		const TArray<TPair<FString, FDabArgument*>> FreshArgs = KeyArguments(*Fresh);
		bool bSplice = Added.IsEmpty() && Removed.IsEmpty();
		for (const FString& Key : Changed)
		{
			bSplice &= FindKey(FreshArgs, Key) != nullptr;
		}
		if (bSplice)
		{
			// Only values changed and each is written: replace just their text.
			for (const FString& Key : Changed)
			{
				ReplaceArgumentValue(*FindKey(FreshArgs, Key), *FindKey(NewArgs, Key)->Value);
			}
			Change(Where, What, true);
			return;
		}

		// Arguments came or went (or a value written through a convenience changed): the node's call is rebuilt,
		// keeping every argument of the text that still says the same thing, poses included.
		TArray<FDabArgumentPtr> Rebuilt;
		for (const TPair<FString, FDabArgument*>& New : NewArgs)
		{
			FDabArgument* Kept = FindKey(FreshArgs, New.Key);
			const FDabArgument* Old = FindKey(OldArgs, New.Key);
			const bool bPose = ContainsPoseMarker(New.Value->Value.Get());
			const bool bSame = Old && PrintDabExpr(*Old->Value) == PrintDabExpr(*New.Value->Value);
			if (Kept && (bPose || bSame))
			{
				const int32 Index = Fresh->Args.IndexOfByPredicate([Kept](const FDabArgumentPtr& Candidate) { return Candidate.Get() == Kept; });
				Rebuilt.Add(MoveTemp(Fresh->Args[Index]));
			}
			else if (bPose)
			{
				Change(Where, What, false);
				Refuse(TEXT("DAB8006"), Fresh->Span, FString::Printf(TEXT("%s: the text writes the node's poses in a way write-back cannot follow."), *Where),
					TEXT("Make the change in the text."));
				return;
			}
			else
			{
				FDabArgumentPtr Argument = MakeUnique<FDabArgument>();
				Argument->Name = New.Value->Name;
				Argument->bNameIsString = New.Value->bNameIsString;
				Argument->Value = CloneExpr(*New.Value->Value);
				Rebuilt.Add(MoveTemp(Argument));
			}
		}
		GraveyardArgs.Append(MoveTemp(Fresh->Args));
		Fresh->Args = MoveTemp(Rebuilt);
		Text->ReprintExpr(Fresh->Span, *Fresh);
		Change(Where, What, true);
	}

	FDabBlockExpr* FWriteBack::FindGraphBlock(const FString& GraphPath) const
	{
		if (GraphPath == TEXT("AnimGraph"))
		{
			const FDabAssignStmt* const* Section = Provenance.Sections.Find(TEXT("Graph"));
			const FDabNode* Fresh = Section ? Text->FindSame(**Section) : nullptr;
			const FDabAssignStmt* Assign = Fresh ? Fresh->As<FDabAssignStmt>() : nullptr;
			return Assign && Assign->Value ? Assign->Value->As<FDabBlockExpr>() : nullptr;
		}
		const FDabNamedBlockStmt* const* Owner = nullptr;
		if (GraphPath.StartsWith(TEXT("Layer/")))
		{
			Owner = Provenance.Layers.Find(GraphPath.RightChop(6));
		}
		else if (GraphPath.StartsWith(TEXT("Machine/")))
		{
			FString Machine;
			FString State;
			if (GraphPath.RightChop(8).Split(TEXT("/State/"), &Machine, &State))
			{
				Owner = Provenance.States.Find(Machine + TEXT("/") + State);
			}
		}
		FDabNode* Fresh = Owner ? Text->FindSame(**Owner) : nullptr;
		FDabNamedBlockStmt* Named = Fresh ? Fresh->As<FDabNamedBlockStmt>() : nullptr;
		return Named ? Named->Body.Get() : nullptr;
	}

	/** Names of the templates the file and its libraries define: calls to them do not survive a rewrite. */
	TSet<FString> CollectTemplateNames(const FDabCompilationUnit& Unit)
	{
		TSet<FString> Names;
		const auto FromFile = [&Names](const FDabFile* File)
		{
			const FDabBlockExpr* Body = File && File->Document ? File->Document->Body.Get() : nullptr;
			if (!Body)
			{
				return;
			}
			for (const FDabStmtPtr& Statement : Body->Statements)
			{
				const FDabNamedBlockStmt* Named = Statement ? Statement->As<FDabNamedBlockStmt>() : nullptr;
				if (Named && Named->Word == TEXT("Template"))
				{
					Names.Add(Named->Name);
				}
			}
		};
		FromFile(Unit.Main.Get());
		for (const TUniquePtr<FDabFile>& Library : Unit.Libraries)
		{
			FromFile(Library.Get());
		}
		return Names;
	}

	bool CallsAny(const FDabNode& Node, const TSet<FString>& Names)
	{
		if (const FDabCallExpr* Call = Node.As<FDabCallExpr>())
		{
			const FDabNameExpr* Callee = Call->Callee ? Call->Callee->As<FDabNameExpr>() : nullptr;
			if (Callee && Names.Contains(Callee->Name))
			{
				return true;
			}
		}
		bool bFound = false;
		ForEachChild(Node, [&bFound, &Names](const FDabNode& Child) { bFound = bFound || CallsAny(Child, Names); });
		return bFound;
	}

	void FWriteBack::RewriteGraph(const FString& GraphPath, const FDabModelGraph& EditedGraph, const FString& Location)
	{
		if (RewrittenGraphs.Contains(GraphPath))
		{
			return;
		}
		RewrittenGraphs.Add(GraphPath);

		const bool bState = GraphPath.StartsWith(TEXT("Machine/"));
		TArray<FDabStmtPtr> Statements = LowerDabGraph(Edited, EditedGraph, bState ? TEXT("Pose") : TEXT("Output"), Text->GetFile(), LowerOptions);
		FDabFile& File = Text->GetFile();

		FDabBlockExpr* Block = FindGraphBlock(GraphPath);
		if (!Block)
		{
			// `Layer X = Pose;` grows a body.
			const FDabNamedBlockStmt* const* Owner = Provenance.Layers.Find(GraphPath.RightChop(6));
			FDabNode* Fresh = Owner ? Text->FindSame(**Owner) : nullptr;
			FDabNamedBlockStmt* Layer = Fresh ? Fresh->As<FDabNamedBlockStmt>() : nullptr;
			if (!Layer)
			{
				Change(Location, TEXT("the graph's nodes"), false);
				Refuse(TEXT("DAB8006"), FDabSpan(), FString::Printf(TEXT("%s: the graph changed but its place in the text was not found."), *Location));
				return;
			}
			if (Layer->Value && CallsAny(*Layer->Value, CollectTemplateNames(*Compiled.Unit)))
			{
				Refuse(TEXT("DAB8004"), Layer->Span, FString::Printf(TEXT("%s was rewritten from the Blueprint; its template calls are written out."), *Location));
			}
			GraveyardExprs.Add(MoveTemp(Layer->Value));
			if (Statements.Num() == 1 && Statements[0]->Is<FDabAssignStmt>())
			{
				Layer->Value = MoveTemp(static_cast<FDabAssignStmt&>(*Statements[0]).Value);
			}
			else
			{
				Layer->Body = MakeUnique<FDabBlockExpr>();
				Layer->Body->Statements = MoveTemp(Statements);
			}
			Text->ReprintStmt(Layer->Span, *Layer);
			Change(Location, TEXT("the graph's nodes"), true, true);
			return;
		}

		// A state's functions stay at the top of its body; the graph's statements follow.
		TArray<FDabStmtPtr> Kept;
		TMap<FString, FDabStmtPtr*> OldByName;
		const TSet<FString> Templates = CollectTemplateNames(*Compiled.Unit);
		TSet<FString> NewNames;
		for (const FDabStmtPtr& New : Statements)
		{
			if (const FDabAssignStmt* Assign = New ? New->As<FDabAssignStmt>() : nullptr)
			{
				NewNames.Add(Assign->Name);
			}
		}
		bool bLosesTemplates = false;
		for (FDabStmtPtr& Old : Block->Statements)
		{
			const FDabAssignStmt* Assign = Old ? Old->As<FDabAssignStmt>() : nullptr;
			if (bState && Assign && (Assign->Name == TEXT("OnEnter") || Assign->Name == TEXT("OnExit") || Assign->Name == TEXT("OnBlended")))
			{
				Kept.Add(MoveTemp(Old));
				continue;
			}
			// A statement that is gone took its template calls with it; one that is still there has them written out.
			if (Old && (!Assign || NewNames.Contains(Assign->Name)) && CallsAny(*Old, Templates))
			{
				bLosesTemplates = true;
			}
			if (Assign)
			{
				OldByName.Add(Assign->Name, &Old);
			}
		}
		// Comments travel with the statement of the same name.
		for (const FDabStmtPtr& New : Statements)
		{
			const FDabAssignStmt* Assign = New ? New->As<FDabAssignStmt>() : nullptr;
			FDabStmtPtr* const* Old = Assign ? OldByName.Find(Assign->Name) : nullptr;
			if (Old && **Old)
			{
				if (const FDabTrivia* Trivia = File.FindTrivia((**Old).Get()))
				{
					FDabTrivia& Moved = File.Trivia.FindOrAdd(New.Get());
					Moved.Leading = Trivia->Leading;
					Moved.Trailing = Trivia->Trailing;
					Moved.BlankLinesBefore = FMath::Max(Moved.BlankLinesBefore, Trivia->BlankLinesBefore);
				}
			}
		}
		if (bLosesTemplates)
		{
			Refuse(TEXT("DAB8004"), Block->Span, FString::Printf(TEXT("%s was rewritten from the Blueprint; its template calls are written out."), *Location),
				TEXT("Put the template calls back by hand if you want them."));
		}
		for (FDabStmtPtr& Old : Block->Statements)
		{
			if (Old)
			{
				Graveyard.Add(MoveTemp(Old));
			}
		}
		Block->Statements = MoveTemp(Kept);
		Block->Statements.Append(MoveTemp(Statements));
		Text->ReprintExpr(Block->Span, *Block);
		Change(Location, TEXT("the graph's nodes"), true, true);
	}
}
