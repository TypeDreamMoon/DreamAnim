// Copyright DreamMoon. All Rights Reserved.
//
// State machines: settings, state settings and functions, conduit conditions, transition conditions and
// options, shared rules written in place; states, conduits, aliases or transitions that came or went
// rewrite the machine's statement, keeping the states that did not change as they were written.

#include "WriteBack/DabWriteBackInternal.h"

namespace UE::DreamAnim::Compiler::WriteBackPrivate
{
	/** A transition row's options as text, whatever its endpoints and condition. */
	FString PrintRowOptions(const FDabMeta* Meta)
	{
		if (!Meta)
		{
			return FString();
		}
		FDabTransitionStmt Probe;
		Probe.From = TEXT("A");
		Probe.To = TEXT("B");
		TUniquePtr<FDabNameExpr> Condition = MakeUnique<FDabNameExpr>();
		Condition->Name = TEXT("C");
		Probe.Condition = MoveTemp(Condition);
		Probe.Meta = CloneMeta(*Meta);
		return PrintDabStmt(Probe);
	}

	const FDabModelState* FindState(const FDabModelStateMachine& Machine, const FString& Name)
	{
		return Machine.States.FindByPredicate([&Name](const FDabModelState& State) { return State.Name == Name; });
	}

	TArray<FString> SortedNames(const FDabModelStateMachine& Machine)
	{
		TArray<FString> Names;
		for (const FDabModelState& State : Machine.States)
		{
			Names.Add(TEXT("S:") + State.Name);
		}
		for (const FDabModelConduit& Conduit : Machine.Conduits)
		{
			Names.Add(TEXT("C:") + Conduit.Name);
		}
		for (const FDabModelAlias& Alias : Machine.Aliases)
		{
			FString Members = Alias.bGlobal ? FString(TEXT("All")) : FString::Join(Alias.States, TEXT(","));
			Names.Add(TEXT("A:") + Alias.Name + TEXT("=") + Members);
		}
		Names.Sort();
		return Names;
	}

	/** Each transition's key (`From->To#n`, n counting that pair from 0) in a machine's order. */
	TArray<FString> TransitionKeys(const FDabModelStateMachine& Machine)
	{
		TArray<FString> Keys;
		TMap<FString, int32> Counts;
		for (const FDabModelTransition& Transition : Machine.Transitions)
		{
			const FString Pair = Transition.From + TEXT("->") + Transition.To;
			Keys.Add(FString::Printf(TEXT("%s#%d"), *Pair, Counts.FindOrAdd(Pair)++));
		}
		return Keys;
	}

	void FWriteBack::CompareMachines()
	{
		const FDabModel& Model = Compiled.Model;
		FDabBlockExpr* DocumentBody = Text->GetFile().Document->Body.Get();

		for (const FDabModelStateMachine& TextMachine : Model.StateMachines)
		{
			const FString Location = TEXT("StateMachine ") + TextMachine.Name;
			const FDabNamedBlockStmt* const* Compiled2 = Provenance.Machines.Find(TextMachine.Name);
			FDabNode* FreshNode = Compiled2 ? Text->FindSame(**Compiled2) : nullptr;
			FDabNamedBlockStmt* Fresh = FreshNode ? FreshNode->As<FDabNamedBlockStmt>() : nullptr;
			const FDabModelStateMachine* EditedMachine = Edited.FindStateMachine(TextMachine.Name);
			if (!Fresh)
			{
				continue;
			}
			if (!EditedMachine)
			{
				// Its node went away (the graph that ran it was rewritten); the statement goes too.
				if (Options.bStructure)
				{
					Text->RemoveStmt(*Fresh);
					Change(Location, TEXT("the state machine"), true, true);
				}
				else
				{
					Change(Location, TEXT("the state machine"), false);
					Refuse(TEXT("DAB8003"), Fresh->Span, FString::Printf(TEXT("%s was removed in the editor; structural changes are not written back."), *Location));
				}
				continue;
			}

			// Structure: members, aliases' members, the entry, the transitions' endpoints.
			const TArray<FString> TextKeys = TransitionKeys(TextMachine);
			TSet<FGuid> TextTransitionGuids;
			for (const FString& Key : TextKeys)
			{
				TextTransitionGuids.Add(TextGuid(TEXT("Machine/") + TextMachine.Name, TEXT("Transition/") + Key));
			}
			bool bStructure = TextMachine.Entry != EditedMachine->Entry || SortedNames(TextMachine) != SortedNames(*EditedMachine)
				|| TextMachine.Transitions.Num() != EditedMachine->Transitions.Num();
			for (const FDabModelTransition& Transition : EditedMachine->Transitions)
			{
				bStructure |= !TextTransitionGuids.Contains(Transition.Guid);
			}
			if (bStructure)
			{
				if (Options.bStructure)
				{
					RewriteMachine(TextMachine, *EditedMachine, *Fresh, Location);
				}
				else
				{
					Change(Location, TEXT("states or transitions"), false);
					Refuse(TEXT("DAB8003"), Fresh->Span, FString::Printf(TEXT("%s: states or transitions changed in the editor; structural changes are not written back."), *Location));
				}
				continue;
			}
			CompareMachineValues(TextMachine, *EditedMachine, *Fresh, Location);
		}

		for (const FDabModelStateMachine& EditedMachine : Edited.StateMachines)
		{
			if (Model.FindStateMachine(EditedMachine.Name) || !DocumentBody)
			{
				continue;
			}
			const FString Location = TEXT("StateMachine ") + EditedMachine.Name;
			if (!Options.bStructure)
			{
				Change(Location, TEXT("a new state machine"), false);
				Refuse(TEXT("DAB8003"), FDabSpan(), FString::Printf(TEXT("%s was added in the editor; structural changes are not written back."), *Location));
				continue;
			}
			TUniquePtr<FDabNamedBlockStmt> Machine = LowerDabStateMachine(Edited, EditedMachine, Text->GetFile(), LowerOptions);
			const FDabNamedBlockStmt& Added = *Machine;
			DocumentBody->Statements.Add(MoveTemp(Machine));
			Text->AppendStmt(*DocumentBody, Added);
			Change(Location, TEXT("a new state machine"), true, true);
		}
	}

	bool FWriteBack::StateSettingsDiffer(const FDabModelState& TextState, const FDabModelState& EditedState) const
	{
		return PrintMeta(TextState.Properties) != PrintMeta(EditedState.Properties)
			|| PrintCall(TextState.OnEnter) != PrintCall(EditedState.OnEnter)
			|| PrintCall(TextState.OnExit) != PrintCall(EditedState.OnExit)
			|| PrintCall(TextState.OnBlended) != PrintCall(EditedState.OnBlended);
	}

	void FWriteBack::WriteStateSettings(FDabNamedBlockStmt& Fresh, const FDabModelState& EditedState)
	{
		Fresh.Meta = LowerDabMeta(Edited, EditedState.Properties, LowerOptions);
		if (!Fresh.Body)
		{
			return;
		}
		// The functions are the body's first statements; replace them in place, add or drop as needed.
		const TPair<const TCHAR*, const TOptional<FDabModelCall>*> Functions[] = {
			{ TEXT("OnEnter"), &EditedState.OnEnter }, { TEXT("OnExit"), &EditedState.OnExit }, { TEXT("OnBlended"), &EditedState.OnBlended } };
		int32 InsertAt = 0;
		for (const TPair<const TCHAR*, const TOptional<FDabModelCall>*>& Function : Functions)
		{
			const int32 Existing = Fresh.Body->Statements.IndexOfByPredicate([&Function](const FDabStmtPtr& Statement)
			{
				const FDabAssignStmt* Assign = Statement ? Statement->As<FDabAssignStmt>() : nullptr;
				return Assign && Assign->Name == Function.Key;
			});
			if (!Function.Value->IsSet())
			{
				if (Existing != INDEX_NONE)
				{
					Graveyard.Add(MoveTemp(Fresh.Body->Statements[Existing]));
					Fresh.Body->Statements.RemoveAt(Existing);
				}
				continue;
			}
			FDabExprPtr Call = LowerDabCall(Edited, **Function.Value, LowerOptions);
			if (Existing != INDEX_NONE)
			{
				FDabAssignStmt& Assign = static_cast<FDabAssignStmt&>(*Fresh.Body->Statements[Existing]);
				GraveyardExprs.Add(MoveTemp(Assign.Value));
				Assign.Value = MoveTemp(Call);
				InsertAt = Existing + 1;
			}
			else
			{
				TUniquePtr<FDabAssignStmt> Assign = MakeUnique<FDabAssignStmt>();
				Assign->Name = Function.Key;
				Assign->Value = MoveTemp(Call);
				Fresh.Body->Statements.Insert(MoveTemp(Assign), InsertAt++);
			}
		}
	}

	void FWriteBack::CompareMachineValues(const FDabModelStateMachine& TextMachine, const FDabModelStateMachine& EditedMachine,
		FDabNamedBlockStmt& Fresh, const FString& Location)
	{
		const FString MachinePath = TEXT("Machine/") + TextMachine.Name;

		for (const FDabModelState& TextState : TextMachine.States)
		{
			const FDabModelState* EditedState = FindState(EditedMachine, TextState.Name);
			if (!EditedState)
			{
				continue;
			}
			const FString StateLocation = FString::Printf(TEXT("%s, state %s"), *Location, *TextState.Name);
			CompareGraph(MachinePath + TEXT("/State/") + TextState.Name, TextState.Graph, &EditedState->Graph, StateLocation);
			if (StateSettingsDiffer(TextState, *EditedState))
			{
				const FDabNamedBlockStmt* const* Compiled2 = Provenance.States.Find(TextMachine.Name + TEXT("/") + TextState.Name);
				FDabNode* Node = Compiled2 ? Text->FindSame(**Compiled2) : nullptr;
				if (FDabNamedBlockStmt* State = Node ? Node->As<FDabNamedBlockStmt>() : nullptr)
				{
					WriteStateSettings(*State, *EditedState);
					Text->ReprintStmt(State->Span, *State);
					Change(StateLocation, TEXT("the state's settings or functions"), true);
				}
			}
		}

		for (const FDabModelConduit& TextConduit : TextMachine.Conduits)
		{
			const FDabModelConduit* EditedConduit = EditedMachine.Conduits.FindByPredicate([&TextConduit](const FDabModelConduit& Candidate)
			{
				return Candidate.Name == TextConduit.Name;
			});
			const auto ConditionText = [this](const FDabExprPtr& Condition)
			{
				const FString Printed = Print(Condition.Get());
				return Printed.IsEmpty() ? FString(TEXT("true")) : Printed;
			};
			if (!EditedConduit || ConditionText(TextConduit.Condition) == ConditionText(EditedConduit->Condition))
			{
				continue;
			}
			const FDabNamedBlockStmt* const* Compiled2 = Provenance.States.Find(TextMachine.Name + TEXT("/") + TextConduit.Name);
			FDabNode* Node = Compiled2 ? Text->FindSame(**Compiled2) : nullptr;
			if (FDabNamedBlockStmt* Conduit = Node ? Node->As<FDabNamedBlockStmt>() : nullptr)
			{
				GraveyardExprs.Add(MoveTemp(Conduit->Value));
				if (EditedConduit->Condition && ConditionText(EditedConduit->Condition) != TEXT("true"))
				{
					Conduit->Value = LowerDabValue(Edited, *EditedConduit->Condition, LowerOptions);
				}
				Text->ReprintStmt(Conduit->Span, *Conduit);
				Change(FString::Printf(TEXT("%s, conduit %s"), *Location, *TextConduit.Name), TEXT("the condition"), true);
			}
		}

		// Transitions by the NodeGuid the generator gave each row.
		TMap<FGuid, const FDabModelTransition*> EditedByGuid;
		for (const FDabModelTransition& Transition : EditedMachine.Transitions)
		{
			EditedByGuid.Add(Transition.Guid, &Transition);
		}
		const TArray<FString> Keys = TransitionKeys(TextMachine);
		TSet<FString> RulesWritten;
		for (int32 Index = 0; Index < TextMachine.Transitions.Num(); ++Index)
		{
			const FDabModelTransition& TextTransition = TextMachine.Transitions[Index];
			const FDabModelTransition* const* EditedTransition = EditedByGuid.Find(TextGuid(MachinePath, TEXT("Transition/") + Keys[Index]));
			if (!EditedTransition)
			{
				continue;
			}
			const FString RowLocation = FString::Printf(TEXT("%s: %s -> %s"), *Location, *TextTransition.From, *TextTransition.To);

			// A shared rule's condition is the rule's, written in Rules once for every row that uses it.
			if (!TextTransition.SharedRuleName.IsEmpty() && TextTransition.SharedRuleName == (*EditedTransition)->SharedRuleName
				&& !RulesWritten.Contains(TextTransition.SharedRuleName))
			{
				const FDabExprPtr OldRule = LowerDabValue(Compiled.Model, *TextTransition.Condition, LowerOptions);
				const FDabExprPtr NewRule = (*EditedTransition)->Condition ? LowerDabValue(Edited, *(*EditedTransition)->Condition, LowerOptions) : nullptr;
				if (NewRule && Print(OldRule.Get()) != Print(NewRule.Get()))
				{
					RulesWritten.Add(TextTransition.SharedRuleName);
					const FDabAssignStmt* const* Rule = Provenance.Rules.Find(TextMachine.Name + TEXT("/") + TextTransition.SharedRuleName);
					FDabNode* Node = Rule ? Text->FindSame(**Rule) : nullptr;
					FDabAssignStmt* FreshRule = Node ? Node->As<FDabAssignStmt>() : nullptr;
					FDabNode* Block = FreshRule ? Text->GetParent(*FreshRule) : nullptr;
					if (FreshRule && Block && Block->Is<FDabBlockExpr>())
					{
						GraveyardExprs.Add(MoveTemp(FreshRule->Value));
						FreshRule->Value = CloneExpr(*NewRule);
						Text->ReprintExpr(Block->Span, static_cast<const FDabBlockExpr&>(*Block));
						Change(FString::Printf(TEXT("%s, rule %s"), *Location, *TextTransition.SharedRuleName), TEXT("the condition"), true);
					}
				}
			}

			const TUniquePtr<FDabTransitionStmt> OldRow = LowerDabTransition(Compiled.Model, TextTransition, LowerOptions);
			const TUniquePtr<FDabTransitionStmt> NewRow = LowerDabTransition(Edited, **EditedTransition, LowerOptions);
			if (PrintDabStmt(*OldRow) == PrintDabStmt(*NewRow))
			{
				continue;
			}
			const FDabTransitionStmt* const* Compiled2 = Provenance.Transitions.Find(TextMachine.Name + TEXT("/") + Keys[Index]);
			FDabNode* Node = Compiled2 ? Text->FindSame(**Compiled2) : nullptr;
			FDabTransitionStmt* Row = Node ? Node->As<FDabTransitionStmt>() : nullptr;
			FDabNode* Block = Row ? Text->GetParent(*Row) : nullptr;
			if (!Row || !Block || !Block->Is<FDabBlockExpr>())
			{
				continue;
			}
			TArray<FString> What;
			if (Print(OldRow->Condition.Get()) != Print(NewRow->Condition.Get()))
			{
				// The row keeps what it wrote unless the condition itself changed (rule names inside it go then).
				GraveyardExprs.Add(MoveTemp(Row->Condition));
				Row->Condition = MoveTemp(NewRow->Condition);
				What.Add(TEXT("condition"));
			}
			if (PrintRowOptions(OldRow->Meta.Get()) != PrintRowOptions(NewRow->Meta.Get()))
			{
				Row->Meta = MoveTemp(NewRow->Meta);
				What.Add(TEXT("options"));
			}
			Text->ReprintExpr(Block->Span, static_cast<const FDabBlockExpr&>(*Block));
			Change(RowLocation, FString::Join(What, TEXT(", ")), true);
		}

		if (PrintMeta(TextMachine.Properties) != PrintMeta(EditedMachine.Properties))
		{
			Fresh.Meta = LowerDabMeta(Edited, EditedMachine.Properties, LowerOptions);
			Text->ReprintStmt(Fresh.Span, Fresh);
			Change(Location, TEXT("the machine's settings"), true);
		}
	}

	void FWriteBack::RewriteMachine(const FDabModelStateMachine& TextMachine, const FDabModelStateMachine& EditedMachine,
		FDabNamedBlockStmt& Fresh, const FString& Location)
	{
		const FString MachinePath = TEXT("Machine/") + TextMachine.Name;
		FDabFile& File = Text->GetFile();
		TUniquePtr<FDabNamedBlockStmt> Machine = LowerDabStateMachine(Edited, EditedMachine, File, LowerOptions);

		// States that are still there, unchanged in shape, keep the text they had (values written in place).
		TMap<FString, FDabStmtPtr*> OldStates;
		TMap<FString, const FDabStmtPtr*> OldRows;
		if (Fresh.Body)
		{
			for (FDabStmtPtr& Statement : Fresh.Body->Statements)
			{
				const FDabNamedBlockStmt* Named = Statement ? Statement->As<FDabNamedBlockStmt>() : nullptr;
				if (Named && Named->Word == TEXT("State"))
				{
					OldStates.Add(Named->Name, &Statement);
				}
				const FDabAssignStmt* Assign = Statement ? Statement->As<FDabAssignStmt>() : nullptr;
				const FDabBlockExpr* Table = Assign && Assign->Name == TEXT("Transitions") && Assign->Value ? Assign->Value->As<FDabBlockExpr>() : nullptr;
				if (Table)
				{
					TMap<FString, int32> Counts;
					for (const FDabStmtPtr& RowStatement : Table->Statements)
					{
						if (const FDabTransitionStmt* Row = RowStatement ? RowStatement->As<FDabTransitionStmt>() : nullptr)
						{
							const FString Pair = Row->From + TEXT("->") + Row->To;
							OldRows.Add(FString::Printf(TEXT("%s#%d"), *Pair, Counts.FindOrAdd(Pair)++), &RowStatement);
						}
					}
				}
			}
		}

		if (Machine->Body)
		{
			TMap<FString, int32> Counts;
			for (FDabStmtPtr& Statement : Machine->Body->Statements)
			{
				FDabNamedBlockStmt* Named = Statement ? Statement->As<FDabNamedBlockStmt>() : nullptr;
				if (Named && Named->Word == TEXT("State"))
				{
					const FDabModelState* TextState = FindState(TextMachine, Named->Name);
					const FDabModelState* EditedState = FindState(EditedMachine, Named->Name);
					FDabStmtPtr* const* Old = OldStates.Find(Named->Name);
					const FString GraphPath = MachinePath + TEXT("/State/") + Named->Name;
					if (TextState && EditedState && Old && **Old && !HasStructuralChange(GraphPath, TextState->Graph, EditedState->Graph))
					{
						CompareGraph(GraphPath, TextState->Graph, &EditedState->Graph, FString::Printf(TEXT("%s, state %s"), *Location, *Named->Name));
						if (StateSettingsDiffer(*TextState, *EditedState))
						{
							WriteStateSettings(static_cast<FDabNamedBlockStmt&>(***Old), *EditedState);
						}
						// The lowering's blank line before it carries over.
						const int32 Blank = File.FindTrivia(Statement.Get()) ? File.FindTrivia(Statement.Get())->BlankLinesBefore : 0;
						Graveyard.Add(MoveTemp(Statement));
						Statement = MoveTemp(**Old);
						File.Trivia.FindOrAdd(Statement.Get()).BlankLinesBefore = FMath::Max(Blank, File.Trivia.FindOrAdd(Statement.Get()).BlankLinesBefore);
					}
				}
				FDabAssignStmt* Assign = Statement ? Statement->As<FDabAssignStmt>() : nullptr;
				FDabBlockExpr* Table = Assign && Assign->Name == TEXT("Transitions") && Assign->Value ? Assign->Value->As<FDabBlockExpr>() : nullptr;
				if (Table)
				{
					// Rows keep their comments.
					for (const FDabStmtPtr& RowStatement : Table->Statements)
					{
						const FDabTransitionStmt* Row = RowStatement ? RowStatement->As<FDabTransitionStmt>() : nullptr;
						if (!Row)
						{
							continue;
						}
						const FString Pair = Row->From + TEXT("->") + Row->To;
						const FDabStmtPtr* const* Old = OldRows.Find(FString::Printf(TEXT("%s#%d"), *Pair, Counts.FindOrAdd(Pair)++));
						const FDabTrivia* Trivia = Old && **Old ? File.FindTrivia((**Old).Get()) : nullptr;
						if (Trivia)
						{
							FDabTrivia& Moved = File.Trivia.FindOrAdd(RowStatement.Get());
							Moved.Leading = Trivia->Leading;
							Moved.Trailing = Trivia->Trailing;
						}
					}
				}
			}
		}

		// The machine's own comments stay on it.
		if (const FDabTrivia* Trivia = File.FindTrivia(&Fresh))
		{
			File.Trivia.FindOrAdd(Machine.Get()) = *Trivia;
		}
		// What is left of the old body: states that changed shape and states that are gone. Only the first lose their template calls.
		bool bLosesTemplates = false;
		if (Fresh.Body)
		{
			const TSet<FString> Templates = CollectTemplateNames(*Compiled.Unit);
			for (const FDabStmtPtr& Statement : Fresh.Body->Statements)
			{
				const FDabNamedBlockStmt* Named = Statement ? Statement->As<FDabNamedBlockStmt>() : nullptr;
				const bool bGone = Named && Named->Word == TEXT("State") && !FindState(EditedMachine, Named->Name);
				bLosesTemplates |= Statement && !bGone && CallsAny(*Statement, Templates);
			}
		}
		if (bLosesTemplates)
		{
			Refuse(TEXT("DAB8004"), Fresh.Span, FString::Printf(TEXT("%s was rewritten from the Blueprint; template calls in its changed states are written out."), *Location));
		}

		FDabNode* Parent = Text->GetParent(Fresh);
		FDabBlockExpr* Body = Parent ? Parent->As<FDabBlockExpr>() : nullptr;
		const int32 Index = Body ? Body->Statements.IndexOfByPredicate([&Fresh](const FDabStmtPtr& Statement) { return Statement.Get() == &Fresh; }) : INDEX_NONE;
		if (Index == INDEX_NONE)
		{
			Change(Location, TEXT("states or transitions"), false);
			Refuse(TEXT("DAB8006"), Fresh.Span, FString::Printf(TEXT("%s changed but its place in the text was not found."), *Location));
			return;
		}
		const FDabSpan OldSpan = Fresh.Span;
		const FDabNamedBlockStmt& Printed = *Machine;
		Graveyard.Add(MoveTemp(Body->Statements[Index]));
		Body->Statements[Index] = MoveTemp(Machine);
		Text->ReprintStmt(OldSpan, Printed);
		Change(Location, TEXT("states or transitions"), true, true);
	}
}
