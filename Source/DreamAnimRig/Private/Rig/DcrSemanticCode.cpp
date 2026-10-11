// Copyright DreamMoon. All Rights Reserved.
//
// Statements: the chain of execute links they make, the scopes lets and loop names live in, `if`, `for`,
// `return`, `Detached`, assignments and `#Region`.

#include "DcrSemanticInternal.h"

#include "DcrExecPins.h"
#include "RigVMTypeUtils.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Rig::Sem
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	namespace
	{
		FDcrType MakeBoolType()
		{
			return MakeDcrBuiltinType(TEXT("bool"));
		}

		FDcrType MakeInt32Type()
		{
			return MakeDcrBuiltinType(TEXT("int32"));
		}

		/** The node's own pin as a value: a name for a parameter or a loop name. */
		FVal MakeOwnPin(FNodeRec& Rec, const FString& PinName, const FDabSpan& Span)
		{
			FVal Value;
			Value.Kind = FVal::EKind::Pin;
			Value.Rec = &Rec;
			Value.Pin = PinName;
			Value.Span = Span;
			if (const FPin* Pin = Rec.Sig.Find(PinName))
			{
				Value.Type = Pin->Type;
			}
			return Value;
		}

		/** Whether a type has a member of that name (a struct's member, by either name). */
		bool TypeHasMember(const FDcrType& Type, const FString& Name)
		{
			FDcrType MemberType;
			FString EnginePath;
			return GetTypeAtSubPath(Type, Name, MemberType, EnginePath);
		}
	}

	FBody::FBody(FGlobals& InGlobals, FDcrModelGraph& InGraph, FFunctionDecl* InFunction, const FString& InWhere)
		: G(InGlobals)
		, Sink(InGlobals.Sink)
		, Graph(InGraph)
		, Function(InFunction)
		, Where(InWhere)
	{
		Scopes.AddDefaulted();
	}

	// ------------------------------------------------------------------------------------------
	// Nodes, links, names
	// ------------------------------------------------------------------------------------------

	const FDcrModelVariable* FBody::FindLocal(const FString& Name) const
	{
		for (const FBody* Body = this; Body; Body = Body->Outer)
		{
			if (const FDcrModelVariable* Local = Body->Function ? Body->Function->Locals.FindByPredicate([&Name](const FDcrModelVariable& Variable) { return Variable.Name == Name; }) : nullptr)
			{
				return Local;
			}
		}
		return nullptr;
	}

	bool FBody::IsEnumValueHere(const FString& Name, const UEnum& Enum) const
	{
		if (Enum.GetValueByNameString(Name) == INDEX_NONE)
		{
			return false;
		}
		FDcrType Named;
		if (const FVal* Bound = LookupScope(Name))
		{
			Named = Bound->Type;
		}
		else if (const FDcrModelVariable* Local = FindLocal(Name))
		{
			Named = Local->Type;
		}
		else if (const FDcrModelVariable* Variable = G.FindVariable(Name))
		{
			Named = Variable->Type;
		}
		return Named.ObjectPath != Enum.GetPathName();
	}

	FString FBody::ReserveName(const FString& Preferred, const FString& Fallback)
	{
		const FString Base = MakeNodeBaseName(Preferred.IsEmpty() ? Fallback : Preferred);
		if (!Preferred.IsEmpty() && !UsedNames.Contains(Base))
		{
			UsedNames.Add(Base);
			return Base;
		}
		int32& Counter = NameCounters.FindOrAdd(Base);
		FString Name;
		do
		{
			++Counter;
			Name = FString::Printf(TEXT("%s_%d"), *Base, Counter);
		}
		while (UsedNames.Contains(Name) || LetNames.Contains(Name));
		UsedNames.Add(Name);
		return Name;
	}

	void FBody::ReserveLetNames(const FDabCodeBlock& Code)
	{
		for (const FDabStmtPtr& Stmt : Code.Statements)
		{
			if (!Stmt)
			{
				continue;
			}
			if (const FDabLetStmt* Let = Stmt->As<FDabLetStmt>(); Let && Let->LetKind != EDabLetKind::Local)
			{
				LetNames.Add(MakeNodeBaseName(Let->Name));
			}
			else if (const FDabIfStmt* If = Stmt->As<FDabIfStmt>())
			{
				for (const FDabIfStmt* Current = If; Current; Current = Current->ElseIf.Get())
				{
					for (const FDabCodeBlock* Block : { Current->Then.Get(), Current->Else.Get() })
					{
						if (Block)
						{
							ReserveLetNames(*Block);
						}
					}
				}
			}
			else if (const FDabForStmt* For = Stmt->As<FDabForStmt>(); For && For->Body)
			{
				ReserveLetNames(*For->Body);
			}
			else if (const FDabDetachedStmt* Detached = Stmt->As<FDabDetachedStmt>(); Detached && Detached->Body)
			{
				ReserveLetNames(*Detached->Body);
			}
		}
	}

	FNodeRec& FBody::NewRec(const FString& Preferred, const FString& Fallback)
	{
		TUniquePtr<FNodeRec> Rec = MakeUnique<FNodeRec>();
		Rec->Node.Name = ReserveName(Preferred, Fallback);
		FNodeRec& Result = *Rec;
		Recs.Add(MoveTemp(Rec));
		return Result;
	}

	void FBody::AddLink(const FString& Source, const FString& Target)
	{
		FDcrModelLink& Link = Graph.Links.AddDefaulted_GetRef();
		Link.Source = Source;
		Link.Target = Target;
	}

	void FBody::Sequence(FNodeRec& Rec)
	{
		Rec.Node.ExecuteIn = Rec.Sig.ExecIn;
		Rec.Node.ExecuteOut = Rec.Sig.ExecOut;
		if (Rec.Sig.ExecIn.IsEmpty())
		{
			return;
		}
		if (!Cursor.IsEmpty())
		{
			AddLink(Cursor, Rec.Node.Name + TEXT(".") + Rec.Sig.ExecIn[0]);
		}
		const FString Continue = GetDcrContinuation(Rec.Node);
		Cursor = Continue.IsEmpty() ? FString() : Rec.Node.Name + TEXT(".") + Continue;
	}

	FVal FBody::MakeAbsent(const FDabSpan& Span) const
	{
		FVal Value;
		Value.Kind = FVal::EKind::Invalid;
		Value.Span = Span;
		return Value;
	}

	FVal FBody::MakeOutput(FNodeRec& Rec, const FDabSpan& Span)
	{
		FVal Value;
		Value.Rec = &Rec;
		Value.Span = Span;
		Value.Pending = Rec.Pending;
		const int32 Outputs = Rec.Sig.NumOutputs();
		if (Outputs == 0)
		{
			Value.Kind = FVal::EKind::Void;
			return Value;
		}
		Value.Kind = FVal::EKind::Pin;
		if (Outputs == 1)
		{
			for (const FPin& Pin : Rec.Sig.Pins)
			{
				if (Pin.bOutput)
				{
					Value.Pin = Pin.Name;
					Value.Type = Pin.Type;
					break;
				}
			}
		}
		return Value;
	}

	void FBody::PushScope()
	{
		Scopes.AddDefaulted();
	}

	void FBody::PopScope()
	{
		if (Scopes.Num() > 1)
		{
			// A name a block gave stays readable after it, unless the code around names it too: it names a node,
			// whose value is there to read (from the node's last run) wherever the graph links it.
			FScope Closed = Scopes.Pop();
			for (TPair<FString, FVal>& Pair : Closed.Names)
			{
				if (!LookupScope(Pair.Key))
				{
					Scopes.Last().Names.Add(Pair.Key, MoveTemp(Pair.Value));
				}
			}
		}
	}

	void FBody::Bind(const FString& Name, const FVal& Value)
	{
		Scopes.Last().Names.Add(Name, Value);
	}

	const FVal* FBody::LookupScope(const FString& Name) const
	{
		for (int32 Index = Scopes.Num() - 1; Index >= 0; --Index)
		{
			if (const FVal* Found = Scopes[Index].Names.Find(Name))
			{
				return Found;
			}
		}
		return nullptr;
	}

	bool FBody::HasName(const FString& Name) const
	{
		if (LookupScope(Name))
		{
			return true;
		}
		if (FindLocal(Name))
		{
			return true;
		}
		return G.FindVariable(Name) != nullptr;
	}

	// ------------------------------------------------------------------------------------------
	// Starting and finishing
	// ------------------------------------------------------------------------------------------

	void FBody::BeginFunction()
	{
		if (!Function)
		{
			return;
		}
		UsedNames.Add(TEXT("Entry"));
		UsedNames.Add(TEXT("Return"));
		bReturned = false;

		TUniquePtr<FNodeRec> Entry = MakeUnique<FNodeRec>();
		Entry->Node.Name = TEXT("Entry");
		Entry->Node.Kind = EDcrNodeKind::Entry;
		for (const FDcrModelFunctionPin& Input : Function->Inputs)
		{
			FPin& Pin = Entry->Sig.Pins.AddDefaulted_GetRef();
			Pin.Name = Input.Name;
			Pin.bOutput = true;
			Pin.Type = Input.Type;
		}
		TUniquePtr<FNodeRec> Return = MakeUnique<FNodeRec>();
		Return->Node.Name = TEXT("Return");
		Return->Node.Kind = EDcrNodeKind::Return;
		for (const FDcrModelFunctionPin& Output : Function->Outputs)
		{
			FPin& Pin = Return->Sig.Pins.AddDefaulted_GetRef();
			Pin.Name = Output.Name;
			Pin.bInput = true;
			Pin.Type = Output.Type;
		}
		// Code runs from the entry to the return, in a pure function's graph too (only its callers see no execute pins).
		const bool bRuns = Function->bMutable || (Function->Stmt && Function->Stmt->Code);
		if (bRuns)
		{
			Entry->Sig.ExecOut.Add(DefaultExecuteName);
			Return->Sig.ExecIn.Add(DefaultExecuteName);
		}
		Entry->Node.ExecuteOut = Entry->Sig.ExecOut;
		Return->Node.ExecuteIn = Return->Sig.ExecIn;
		EntryRec = Entry.Get();
		ReturnRec = Return.Get();
		Recs.Add(MoveTemp(Entry));
		Recs.Add(MoveTemp(Return));

		for (const FDcrModelFunctionPin& Input : Function->Inputs)
		{
			Bind(Input.Name, MakeOwnPin(*EntryRec, Input.Name, FDabSpan()));
		}
		Cursor = bRuns ? FString(TEXT("Entry.")) + DefaultExecuteName : FString();
	}

	void FBody::JoinReturn()
	{
		if (bReturned || !ReturnRec || ReturnRec->Sig.ExecIn.IsEmpty())
		{
			return;
		}
		if (!Cursor.IsEmpty())
		{
			AddLink(Cursor, ReturnRec->Node.Name + TEXT(".") + ReturnRec->Sig.ExecIn[0]);
		}
		Cursor.Reset();
	}

	void FBody::RunFunction()
	{
		if (!Function || !Function->Stmt)
		{
			return;
		}
		const FDabNamedBlockStmt& Stmt = *Function->Stmt;
		// The comment above the function: a box of its own in its graph.
		AddCommentBoxes(Stmt, Recs.Num());
		if (Stmt.Code)
		{
			ReserveLetNames(*Stmt.Code);
		}
		if (Function->bMutable)
		{
			if (Stmt.Code)
			{
				RunBlock(*Stmt.Code, true);
			}
			JoinReturn();
			return;
		}

		// A pure function written as code: its graph runs it like any function's; when nothing in it runs (lets
		// and the return only), the entry and the return have no execute pins.
		if (Stmt.Code)
		{
			const FString Start = FString(TEXT("Entry.")) + DefaultExecuteName;
			RunBlock(*Stmt.Code, true);
			const FString ReturnPrefix = ReturnRec->Node.Name + TEXT(".");
			const bool bRan = Graph.Links.ContainsByPredicate([&Start, &ReturnPrefix](const FDcrModelLink& Link)
			{
				return Link.Source == Start && !Link.Target.StartsWith(ReturnPrefix);
			});
			if (bRan)
			{
				JoinReturn();
			}
			else
			{
				Graph.Links.RemoveAll([&Start](const FDcrModelLink& Link) { return Link.Source == Start; });
				for (FNodeRec* Rec : { EntryRec, ReturnRec })
				{
					Rec->Sig.ExecIn.Reset();
					Rec->Sig.ExecOut.Reset();
					Rec->Node.ExecuteIn.Reset();
					Rec->Node.ExecuteOut.Reset();
				}
			}
			return;
		}

		// A pure function is its value: one expression for a single output, a tuple naming the outputs for more.
		if (!Stmt.Value || !ReturnRec)
		{
			return;
		}
		const FDabExpr* Value = Stmt.Value.Get();
		StatementSpan = Value->Span;
		while (const FDabParenExpr* Paren = Value->As<FDabParenExpr>())
		{
			if (!Paren->Inner)
			{
				break;
			}
			Value = Paren->Inner.Get();
		}
		const FDabTupleExpr* Tuple = Value->As<FDabTupleExpr>();
		bool bPerOutput = false;
		if (Tuple && !Tuple->Items.IsEmpty())
		{
			bool bAllNamed = true;
			for (const FDabArgumentPtr& Item : Tuple->Items)
			{
				bAllNamed &= Item && Item->IsNamed();
			}
			if (bAllNamed && Function->Outputs.Num() > 1)
			{
				bPerOutput = true;
			}
			else if (bAllNamed && Function->Outputs.Num() == 1 && Tuple->Items.Num() == 1 && Tuple->Items[0]->Name == Function->Outputs[0].Name
				&& !TypeHasMember(Function->Outputs[0].Type, Function->Outputs[0].Name))
			{
				bPerOutput = true;
			}
		}
		if (bPerOutput)
		{
			for (const FDabArgumentPtr& Item : Tuple->Items)
			{
				const FPin* Pin = ReturnRec->Sig.Find(Item->Name);
				if (!Pin)
				{
					Sink.Error(TEXT("DAB9001"), Item->NameSpan, FString::Printf(TEXT("The function `%s` has no output called `%s`."), *Function->Name, *Item->Name));
					continue;
				}
				if (Item->Value)
				{
					ApplyArg(*ReturnRec, *Pin, *Item->Value);
				}
			}
			return;
		}
		if (Function->Outputs.Num() == 1)
		{
			ApplyArg(*ReturnRec, ReturnRec->Sig.Pins[0], *Value);
			return;
		}
		Sink.Error(TEXT("DAB9004"), Value->Span, FString::Printf(TEXT("The function `%s` has %d outputs; give its value as a tuple naming each, (A = ..., B = ...)."), *Function->Name, Function->Outputs.Num()));
	}

	bool FBody::BeginEvent(const FDabNamedBlockStmt& Stmt, const FString& Word)
	{
		if (EventWords.Contains(Word))
		{
			Sink.Error(TEXT("DAB9010"), Stmt.NameSpan, FString::Printf(TEXT("The event `%s` is defined twice in the graph `%s`."), *Word, *Graph.Name),
				TEXT("An event can be written once per graph. Put the second one in another graph with [ Graph = \"...\" ], or merge them."));
			return false;
		}
		EventWords.Add(Word);
		Scopes.Reset();
		Scopes.AddDefaulted();
		Regions.Reset();

		const FDcrEventWord* Event = FindDcrEventByWord(Word);
		const FString StructPath = Event ? FString(Event->StructPath) : FString(DcrUserEventStructPath);
		const UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, *StructPath);
		if (!Struct)
		{
			Struct = LoadObject<UScriptStruct>(nullptr, *StructPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		if (!Struct)
		{
			Sink.Error(TEXT("DAB9001"), Stmt.NameSpan, FString::Printf(TEXT("The engine has no unit for the event `%s` (%s)."), *Word, *StructPath));
			return false;
		}

		FNodeRec& Rec = NewRec(FString(), GetDcrShortName(Struct->GetName()));
		Rec.Sig = MakeUnitSignature(*Struct);
		Rec.Node.Kind = EDcrNodeKind::Unit;
		Rec.Node.StructPath = Struct->GetPathName();
		Rec.Node.Notation = GetUnitNotation(*Struct);
		if (!Event)
		{
			// A user event: the name is the unit's EventName pin.
			if (const FPin* NamePin = Rec.Sig.Find(DcrUserEventNamePin); NamePin && NamePin->Property)
			{
				FDcrValueSlot Slot;
				FDcrValueSlot DefaultSlot;
				if (Slot.Init(*NamePin->Property) && DefaultSlot.Init(*NamePin->Property))
				{
					Slot.ImportText(NamePin->Default);
					DefaultSlot.ImportText(NamePin->Default);
					*static_cast<FName*>(Slot.GetMemory()) = FName(*Word);
					const FString Text = Slot.ExportText();
					if (Text != DefaultSlot.ExportText())
					{
						SetPinValue(Rec, NamePin->Name, Text);
					}
				}
			}
		}
		Rec.Node.ExecuteIn = Rec.Sig.ExecIn;
		Rec.Node.ExecuteOut = Rec.Sig.ExecOut;
		const FString Continue = GetDcrContinuation(Rec.Node);
		Cursor = Continue.IsEmpty() ? FString() : Rec.Node.Name + TEXT(".") + Continue;
		return true;
	}

	void FBody::BeginDetached()
	{
		Scopes.Reset();
		Scopes.AddDefaulted();
		Regions.Reset();
		Cursor.Reset();
	}

	void FBody::RunCode(const FDabCodeBlock& Code)
	{
		RunBlock(Code, false);
	}

	void FBody::Finish()
	{
		CloseRegions(0);
		for (const TUniquePtr<FPendingDispatch>& Pending : Pendings)
		{
			if (!Pending->bResolved && !Pending->bFailed)
			{
				Sink.Error(TEXT("DAB9003"), Pending->Span, FString::Printf(TEXT("The types of `%s` cannot be told from its arguments or from where its result goes."), Pending->Struct ? *GetDcrShortName(Pending->Struct->GetName()) : TEXT("this call")),
					TEXT("Give the result a type with a typed let, as float Scale = ...;"));
			}
		}
		for (TUniquePtr<FNodeRec>& Rec : Recs)
		{
			Graph.Nodes.Add(MoveTemp(Rec->Node));
		}
		Recs.Reset();
	}

	// ------------------------------------------------------------------------------------------
	// Statements
	// ------------------------------------------------------------------------------------------

	void FBody::RunBlock(const FDabCodeBlock& Code, bool bFunctionTop)
	{
		const int32 RegionDepth = Regions.Num();
		// `#EndRegion` after a closing return only closes the region the return is in.
		int32 Last = Code.Statements.Num() - 1;
		while (Last >= 0 && Code.Statements[Last] && Code.Statements[Last]->Is<FDabRegionStmt>())
		{
			--Last;
		}
		for (int32 Index = 0; Index < Code.Statements.Num(); ++Index)
		{
			if (Code.Statements[Index])
			{
				RunStatement(*Code.Statements[Index], Index == Last, bFunctionTop);
			}
		}
		CloseRegions(RegionDepth);
	}

	void FBody::AddCommentBoxes(const FDabNode& Node, int32 FirstRec)
	{
		const FDabTrivia* Trivia = G.Unit.Main ? G.Unit.Main->FindTrivia(&Node) : nullptr;
		if (!Trivia || Trivia->Leading.IsEmpty())
		{
			return;
		}
		TArray<FString> Members;
		for (int32 Index = FirstRec; Index < Recs.Num(); ++Index)
		{
			if (Recs[Index]->Node.Kind != EDcrNodeKind::Comment)
			{
				Members.Add(Recs[Index]->Node.Name);
			}
		}
		// Consecutive `//` lines are one box, each `/* */` one of its own.
		TArray<FString> Boxes;
		TArray<FString> Lines;
		auto Flush = [&Boxes, &Lines]()
		{
			if (!Lines.IsEmpty())
			{
				Boxes.Add(FString::Join(Lines, TEXT("\n")));
				Lines.Reset();
			}
		};
		for (const FDabComment& Comment : Trivia->Leading)
		{
			FString Text = Comment.Text;
			if (Comment.bBlock)
			{
				Flush();
				Text.RemoveFromStart(TEXT("/*"));
				Text.RemoveFromEnd(TEXT("*/"));
				Text.RemoveFromStart(TEXT(" "));
				Text.RemoveFromEnd(TEXT(" "));
				Boxes.Add(Text);
				continue;
			}
			Text.RemoveFromStart(TEXT("//"));
			Text.RemoveFromStart(TEXT(" "));
			Lines.Add(Text);
		}
		Flush();
		for (const FString& Box : Boxes)
		{
			FNodeRec& Comment = NewRec(FString(), TEXT("Comment"));
			Comment.Node.Kind = EDcrNodeKind::Comment;
			Comment.Node.CommentText = Box;
			Comment.Node.CommentMembers = Members;
		}
	}

	void FBody::RunStatement(const FDabStmt& Stmt, bool bLast, bool bFunctionTop)
	{
		// The comment above a statement is a box around the nodes it makes.
		const int32 FirstRec = Recs.Num();
		RunStatementKind(Stmt, bLast, bFunctionTop);
		AddCommentBoxes(Stmt, FirstRec);
	}

	void FBody::RunStatementKind(const FDabStmt& Stmt, bool bLast, bool bFunctionTop)
	{
		StatementSpan = Stmt.Span;
		switch (Stmt.Kind)
		{
		case EDabNodeKind::LetStmt:
			RunLet(static_cast<const FDabLetStmt&>(Stmt));
			break;
		case EDabNodeKind::SetStmt:
			RunSet(static_cast<const FDabSetStmt&>(Stmt));
			break;
		case EDabNodeKind::ExprStmt:
			RunExpression(static_cast<const FDabExprStmt&>(Stmt));
			break;
		case EDabNodeKind::IfStmt:
			RunIf(static_cast<const FDabIfStmt&>(Stmt));
			break;
		case EDabNodeKind::ForStmt:
			RunFor(static_cast<const FDabForStmt&>(Stmt));
			break;
		case EDabNodeKind::ReturnStmt:
			RunReturn(static_cast<const FDabReturnStmt&>(Stmt), bLast, bFunctionTop);
			break;
		case EDabNodeKind::DetachedStmt:
			RunDetached(static_cast<const FDabDetachedStmt&>(Stmt));
			break;
		case EDabNodeKind::RegionStmt:
			RunRegion(static_cast<const FDabRegionStmt&>(Stmt));
			break;
		default:
			Sink.Error(TEXT("DAB9004"), Stmt.Span, TEXT("This statement does not belong in code."));
			break;
		}
	}

	void FBody::RunLet(const FDabLetStmt& Stmt)
	{
		// A local belongs to the whole function and was declared with it.
		if (Stmt.LetKind == EDabLetKind::Local)
		{
			if (!Function)
			{
				Sink.Error(TEXT("DAB9004"), Stmt.NameSpan, FString::Printf(TEXT("`local %s` belongs in a function: an event has no local variables."), *Stmt.Name),
					TEXT("Declare a variable in `Variables`, or use a `let`."));
			}
			return;
		}
		if (!Stmt.Value)
		{
			return;
		}
		FDcrType Declared;
		bool bTyped = false;
		if (Stmt.LetKind == EDabLetKind::Typed)
		{
			bTyped = ResolveTypeName(G, Stmt.TypeName, Stmt.TypeSpan, Declared);
		}
		FVal Value = EvalOperand(*Stmt.Value, bTyped ? &Declared : nullptr, Stmt.Name);
		if (Value.Kind == FVal::EKind::Invalid)
		{
			// Reported already: the name still stands for it, so its uses say nothing more.
			Bind(Stmt.Name, Value);
			return;
		}
		if (Value.Kind == FVal::EKind::Void)
		{
			Sink.Error(TEXT("DAB9004"), Stmt.Value->Span, FString::Printf(TEXT("`%s` gets no value: the call gives none."), *Stmt.Name));
			return;
		}
		if (bTyped && Value.Kind == FVal::EKind::Literal)
		{
			// The declared type travels with the literal: it decides the types of the nodes the name is given to.
			Value.Type = Declared;
		}
		if (bTyped && Value.IsPin())
		{
			if (!SettlePending(Value, &Declared))
			{
				return;
			}
			if (!Value.Pin.IsEmpty() && !AreTypesCompatible(Value.Type, Declared))
			{
				Sink.Error(TEXT("DAB9004"), Stmt.Value->Span, FString::Printf(TEXT("`%s` is declared %s, and the value is %s."), *Stmt.Name, *Stmt.TypeName,
					*GetDcrTypeName(Value.Type)));
			}
		}
		Bind(Stmt.Name, Value);
	}

	void FBody::RunSet(const FDabSetStmt& Stmt)
	{
		if (!Stmt.Target || !Stmt.Value)
		{
			return;
		}
		if (const FDabNameExpr* Name = Stmt.Target->As<FDabNameExpr>())
		{
			SetVariable(Stmt, Name->Name, *Stmt.Value);
			return;
		}
		if (const FDabIndexExpr* Index = Stmt.Target->As<FDabIndexExpr>())
		{
			const FDabNameExpr* Object = Index->Object ? Index->Object->As<FDabNameExpr>() : nullptr;
			if (Object && Index->Index)
			{
				SetArrayElement(Stmt, Object->Name, *Index->Index, *Stmt.Value);
				return;
			}
		}
		Sink.Error(TEXT("DAB9006"), Stmt.Target->Span, TEXT("Only a whole variable or an element of an array variable can be assigned."),
			TEXT("A variable is set whole: write all of it, `V = (X = 1, Y = V.Y, Z = V.Z);`, not `V.X = 1;`."));
	}

	void FBody::SetVariable(const FDabSetStmt& Stmt, const FString& Name, const FDabExpr& Value)
	{
		const FDcrModelVariable* Local = FindLocal(Name);
		if (!Local && LookupScope(Name))
		{
			Sink.Error(TEXT("DAB9006"), Stmt.Target->Span, FString::Printf(TEXT("`%s` is a parameter, a let or a loop name; only variables can be assigned."), *Name));
			return;
		}
		const FDcrModelVariable* Variable = Local ? Local : G.FindVariable(Name);
		if (!Variable)
		{
			Sink.Error(TEXT("DAB9001"), Stmt.Target->Span, FString::Printf(TEXT("There is no variable called `%s`."), *Name));
			return;
		}
		if (!Local && Variable->bReadOnly)
		{
			Sink.Error(TEXT("DAB9006"), Stmt.Target->Span, FString::Printf(TEXT("`%s` is read only."), *Name));
			return;
		}

		FNodeRec& Rec = NewRec(FString(), TEXT("Set_") + Name);
		Rec.Node.Kind = EDcrNodeKind::VariableSet;
		Rec.Node.Variable = Variable->Name;
		Rec.Node.VariableType = Variable->Type;
		Rec.Node.bLocalVariable = Local != nullptr;
		Rec.Sig.ExecIn.Add(DefaultExecuteName);
		Rec.Sig.ExecOut.Add(DefaultExecuteName);
		FPin& Pin = Rec.Sig.Pins.AddDefaulted_GetRef();
		Pin.Name = TEXT("Value");
		Pin.bInput = true;
		Pin.Type = Variable->Type;
		ApplyArg(Rec, Rec.Sig.Pins[0], Value);
		Sequence(Rec);
	}

	void FBody::SetArrayElement(const FDabSetStmt& Stmt, const FString& Name, const FDabExpr& Index, const FDabExpr& Value)
	{
		const FDcrModelVariable* Local = FindLocal(Name);
		const FDcrModelVariable* Variable = Local ? Local : G.FindVariable(Name);
		if (!Variable)
		{
			Sink.Error(TEXT("DAB9001"), Stmt.Target->Span, FString::Printf(TEXT("There is no variable called `%s`."), *Name));
			return;
		}
		if (!Local && Variable->bReadOnly)
		{
			Sink.Error(TEXT("DAB9006"), Stmt.Target->Span, FString::Printf(TEXT("`%s` is read only."), *Name));
			return;
		}
		if (!IsArrayType(Variable->Type))
		{
			Sink.Error(TEXT("DAB9004"), Stmt.Target->Span, FString::Printf(TEXT("`%s` is not an array; only an array's elements can be assigned by index."), *Name));
			return;
		}
		const UScriptStruct* Struct = FindDispatchByFullName(TEXT("RigVMDispatch_ArraySetAtIndex"));
		if (!Struct)
		{
			Sink.Error(TEXT("DAB9001"), Stmt.Span, TEXT("The engine has no ArraySetAtIndex node."));
			return;
		}
		FDcrType ElementType = Variable->Type;
		ElementType.CPPType = RigVMTypeUtils::BaseTypeFromArrayType(Variable->Type.CPPType);
		const FDcrType IndexType = MakeInt32Type();

		TArray<TPair<FString, FVal>> Args;
		Args.Emplace(TEXT("Array"), MakeVariableGet(*Variable, Local != nullptr, FString(), Stmt.Target->Span));
		Args.Emplace(TEXT("Index"), EvalOperand(Index, &IndexType, FString()));
		Args.Emplace(TEXT("Element"), EvalOperand(Value, &ElementType, FString()));
		BuildDispatchFrom(*Struct, MoveTemp(Args), nullptr, FString(), Stmt.Span);
	}

	void FBody::RunExpression(const FDabExprStmt& Stmt)
	{
		if (!Stmt.Expr)
		{
			return;
		}
		const FVal Value = Eval(*Stmt.Expr, nullptr, FString());
		if (Value.Kind == FVal::EKind::Invalid || Value.Kind == FVal::EKind::Void)
		{
			return;
		}
		if (Value.Kind == FVal::EKind::Literal || !Value.Rec || !Value.Rec->Sig.IsMutable())
		{
			Sink.Error(TEXT("DAB9005"), Stmt.Span, TEXT("This call has no effect: it only computes a value."),
				TEXT("Use the value (`let X = ...;`, an argument, an assignment), or remove the statement. Only calls that run (they have execute pins) can stand alone."));
		}
	}

	void FBody::RunIf(const FDabIfStmt& Stmt)
	{
		if (!Stmt.Condition)
		{
			return;
		}
		const FDcrType BoolType = MakeBoolType();
		const FVal Condition = EvalOperand(*Stmt.Condition, &BoolType, FString());

		const UScriptStruct* Struct = FindUnitByFullName(TEXT("RigVMFunction_ControlFlowBranch"));
		if (!Struct)
		{
			Sink.Error(TEXT("DAB9001"), Stmt.Span, TEXT("The engine has no Branch node."));
			return;
		}
		FNodeRec& Branch = NewRec(FString(), TEXT("Branch"));
		Branch.Sig = MakeUnitSignature(*Struct);
		Branch.Node.Kind = EDcrNodeKind::Unit;
		Branch.Node.StructPath = Struct->GetPathName();
		Branch.Node.Notation = GetUnitNotation(*Struct);
		if (const FPin* ConditionPin = Branch.Sig.Find(TEXT("Condition")))
		{
			ApplyValue(Branch, *ConditionPin, Condition, Stmt.Condition->Span);
		}
		Sequence(Branch);
		const FString After = Cursor;

		if (!Branch.Sig.ExecOut.Contains(TEXT("True")) || !Branch.Sig.ExecOut.Contains(TEXT("False")))
		{
			Sink.Error(TEXT("DAB9001"), Stmt.Span, TEXT("The engine's Branch node has no True and False pins."));
			return;
		}
		Cursor = Branch.Node.Name + TEXT(".True");
		PushScope();
		if (Stmt.Then)
		{
			RunBlock(*Stmt.Then, false);
		}
		PopScope();

		Cursor = Branch.Node.Name + TEXT(".False");
		if (Stmt.ElseIf)
		{
			RunIf(*Stmt.ElseIf);
		}
		else if (Stmt.Else)
		{
			PushScope();
			RunBlock(*Stmt.Else, false);
			PopScope();
		}
		Cursor = After;
	}

	void FBody::RunFor(const FDabForStmt& Stmt)
	{
		if (!Stmt.Iterable || !Stmt.Body)
		{
			return;
		}
		FNodeRec* Loop = nullptr;
		TArray<FString> Outputs;

		const FDabCallExpr* RangeCall = Stmt.Iterable->As<FDabCallExpr>();
		const FDabNameExpr* RangeName = RangeCall && RangeCall->Callee ? RangeCall->Callee->As<FDabNameExpr>() : nullptr;
		if (RangeName && RangeName->Name == TEXT("Range") && !G.FindFunction(RangeName->Name))
		{
			if (RangeCall->Args.Num() != 1 || !RangeCall->Args[0] || !RangeCall->Args[0]->Value)
			{
				Sink.Error(TEXT("DAB9004"), Stmt.Iterable->Span, TEXT("Range takes the count of iterations: Range(5)."));
				return;
			}
			const UScriptStruct* Struct = FindUnitByFullName(TEXT("RigVMFunction_ForLoopCount"));
			if (!Struct)
			{
				Sink.Error(TEXT("DAB9001"), Stmt.Span, TEXT("The engine has no ForLoopCount node."));
				return;
			}
			Loop = &NewRec(FString(), TEXT("ForLoopCount"));
			Loop->Sig = MakeUnitSignature(*Struct);
			Loop->Node.Kind = EDcrNodeKind::Unit;
			Loop->Node.StructPath = Struct->GetPathName();
			Loop->Node.Notation = GetUnitNotation(*Struct);
			if (const FPin* CountPin = Loop->Sig.Find(TEXT("Count")))
			{
				ApplyArg(*Loop, *CountPin, *RangeCall->Args[0]->Value);
			}
			Sequence(*Loop);
			Outputs = { TEXT("Index"), TEXT("Ratio") };
		}
		else
		{
			const UScriptStruct* Struct = FindDispatchByFullName(TEXT("RigVMDispatch_ArrayIterator"));
			if (!Struct)
			{
				Sink.Error(TEXT("DAB9001"), Stmt.Span, TEXT("The engine has no ArrayIterator node."));
				return;
			}
			TArray<TPair<FString, FVal>> Args;
			Args.Emplace(TEXT("Array"), EvalOperand(*Stmt.Iterable, nullptr, FString()));
			const FVal Made = BuildDispatchFrom(*Struct, MoveTemp(Args), nullptr, FString(), Stmt.Iterable->Span);
			Loop = Made.Rec;
			Outputs = { TEXT("Element"), TEXT("Index"), TEXT("Count"), TEXT("Ratio") };
		}
		if (!Loop)
		{
			return;
		}

		if (Stmt.Names.Num() > Outputs.Num())
		{
			Sink.Error(TEXT("DAB9004"), Stmt.Span, FString::Printf(TEXT("This loop has %d values to name, and %d names."), Outputs.Num(), Stmt.Names.Num()));
			return;
		}
		const FString After = Cursor;
		FString BodyPin;
		for (const FString& Pin : Loop->Sig.ExecIn)
		{
			if (Loop->Sig.ExecOut.Contains(Pin) && Pin != TEXT("Completed"))
			{
				BodyPin = Pin;
				break;
			}
		}
		if (BodyPin.IsEmpty())
		{
			Sink.Error(TEXT("DAB9001"), Stmt.Span, TEXT("The engine's loop node has no pin the body runs from."));
			return;
		}

		PushScope();
		for (int32 Index = 0; Index < Stmt.Names.Num(); ++Index)
		{
			FVal Name = MakeOwnPin(*Loop, Outputs[Index], Stmt.NameSpans.IsValidIndex(Index) ? Stmt.NameSpans[Index] : Stmt.Span);
			Bind(Stmt.Names[Index], Name);
		}
		Cursor = Loop->Node.Name + TEXT(".") + BodyPin;
		RunBlock(*Stmt.Body, false);
		PopScope();
		Cursor = After;
	}

	void FBody::RunReturn(const FDabReturnStmt& Stmt, bool bLast, bool bFunctionTop)
	{
		if (!Function || !bLast || !bFunctionTop || !ReturnRec)
		{
			Sink.Error(TEXT("DAB9007"), Stmt.Span, TEXT("`return` can only be the last statement of a function's code."),
				TEXT("RigVM has one Return node per function. Write the rest of the function as the else side of an `if`."));
			return;
		}
		if (Stmt.Value)
		{
			if (const FDabTupleExpr* Tuple = Stmt.Value->As<FDabTupleExpr>())
			{
				for (const FDabArgumentPtr& Item : Tuple->Items)
				{
					if (!Item || !Item->Value)
					{
						continue;
					}
					const FPin* Pin = ReturnRec->Sig.Find(Item->Name);
					if (!Pin)
					{
						Sink.Error(TEXT("DAB9001"), Item->NameSpan, FString::Printf(TEXT("The function `%s` has no output called `%s`."), *Function->Name, *Item->Name));
						continue;
					}
					ApplyArg(*ReturnRec, *Pin, *Item->Value);
				}
			}
		}
		if (!ReturnRec->Sig.ExecIn.IsEmpty() && !Cursor.IsEmpty())
		{
			AddLink(Cursor, ReturnRec->Node.Name + TEXT(".") + ReturnRec->Sig.ExecIn[0]);
		}
		Cursor.Reset();
		bReturned = true;
	}

	void FBody::RunDetached(const FDabDetachedStmt& Stmt)
	{
		if (!Stmt.Body)
		{
			return;
		}
		// Nothing runs into it, and nothing after it runs on from it; its lets are the enclosing code's, as the
		// values of nodes another chain may read.
		const FString Saved = Cursor;
		Cursor.Reset();
		RunBlock(*Stmt.Body, false);
		Cursor = Saved;
	}

	void FBody::RunRegion(const FDabRegionStmt& Stmt)
	{
		if (!Stmt.bEnd)
		{
			FRegionStart& Start = Regions.AddDefaulted_GetRef();
			Start.Label = Stmt.Label;
			Start.FirstRec = Recs.Num();
			return;
		}
		// An #EndRegion without its #Region is already a warning of the parser.
		CloseRegions(FMath::Max(0, Regions.Num() - 1));
	}

	void FBody::CloseRegions(int32 Depth)
	{
		while (Regions.Num() > Depth)
		{
			const FRegionStart Start = Regions.Pop();
			TArray<FString> Members;
			for (int32 Index = Start.FirstRec; Index < Recs.Num(); ++Index)
			{
				if (Recs[Index]->Node.Kind != EDcrNodeKind::Comment)
				{
					Members.Add(Recs[Index]->Node.Name);
				}
			}
			if (Members.IsEmpty())
			{
				continue;
			}
			FNodeRec& Comment = NewRec(FString(), TEXT("Comment"));
			Comment.Node.Kind = EDcrNodeKind::Comment;
			Comment.Node.CommentText = Start.Label;
			Comment.Node.CommentMembers = MoveTemp(Members);
		}
	}
}
