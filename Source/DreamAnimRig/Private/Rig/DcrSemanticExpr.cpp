// Copyright DreamMoon. All Rights Reserved.
//
// Expressions: what a name, a member, a call or an operator is, and the node each call becomes. A call
// binds its arguments to the node's pins, positional ones in the order of the pins, named ones by name;
// the node's wildcard types come from the arguments, and where those do not settle them, from where the
// result goes.

#include "DcrSemanticInternal.h"

#include "RigVMCore/RigVMDispatchFactory.h"
#include "RigVMCore/RigVMFunction.h"
#include "RigVMCore/RigVMRegistry.h"
#include "RigVMCore/RigVMStruct.h"
#include "RigVMCore/RigVMTemplate.h"
#include "RigVMTypeUtils.h"
#include "Rigs/RigHierarchyDefines.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Rig::Sem
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	namespace
	{
		bool IsElementKeyName(const FString& Name);

		/** `Bone(name)`: a call named after an element type, with one value. */
		bool IsElementKeyLiteral(const FDabExpr& Expr)
		{
			const FDabCallExpr* Call = Expr.As<FDabCallExpr>();
			const FDabNameExpr* Callee = Call && Call->Callee ? Call->Callee->As<FDabNameExpr>() : nullptr;
			return Callee && IsElementKeyName(Callee->Name) && Call->Args.Num() == 1;
		}

		bool IsElementKeyName(const FString& Name)
		{
			static const TCHAR* const Names[] = { TEXT("Bone"), TEXT("Null"), TEXT("Control"), TEXT("Curve"), TEXT("Connector"), TEXT("Socket"), TEXT("None") };
			for (const TCHAR* Candidate : Names)
			{
				if (Name == Candidate)
				{
					return true;
				}
			}
			return false;
		}

		FVal MakeLiteralValue(const FDabExpr& Expr)
		{
			FVal Value;
			Value.Kind = FVal::EKind::Literal;
			Value.Expr = &StripDabParens(Expr);
			Value.Span = Expr.Span;
			return Value;
		}

		FString DescribeTypeOf(const FDcrType& Type)
		{
			if (!Type.IsSet())
			{
				return TEXT("an unknown type");
			}
			const FString Name = GetDcrTypeName(Type);
			return Name.IsEmpty() ? Type.CPPType : Name;
		}

		/** A unit's node: its signature, kind, struct and execute pins. */
		void InitUnitRec(FNodeRec& Rec, const UScriptStruct& Struct)
		{
			Rec.Sig = MakeUnitSignature(Struct);
			Rec.Node.Kind = EDcrNodeKind::Unit;
			Rec.Node.StructPath = Struct.GetPathName();
			Rec.Node.Notation = GetUnitNotation(Struct);
			Rec.Node.ExecuteIn = Rec.Sig.ExecIn;
			Rec.Node.ExecuteOut = Rec.Sig.ExecOut;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Operands and the shape of a literal
	// ------------------------------------------------------------------------------------------

	bool FBody::IsLiteralShape(const FDabExpr& Expr) const
	{
		const FDabExpr& Inner = StripDabParens(Expr);
		if (Inner.Is<FDabLiteralExpr>() || Inner.Is<FDabTupleExpr>() || Inner.Is<FDabListExpr>())
		{
			return true;
		}
		if (const FDabUnaryExpr* Unary = Inner.As<FDabUnaryExpr>())
		{
			return Unary->Op == EDabUnaryOp::Negate && Unary->Operand && StripDabParens(*Unary->Operand).Is<FDabLiteralExpr>();
		}
		if (const FDabCallExpr* Call = Inner.As<FDabCallExpr>())
		{
			const FDabNameExpr* Callee = Call->Callee ? Call->Callee->As<FDabNameExpr>() : nullptr;
			// `Injected(Node(...))` waits for the pin it goes on; `Injected(Node(...), Output)` is a value like any call.
			const bool bInputInjection = Callee && Callee->Name == TEXT("Injected") && Call->Args.Num() == 1;
			return Callee && (IsElementKeyName(Callee->Name) || Callee->Name == TEXT("Path") || bInputInjection) && !G.FindFunction(Callee->Name);
		}
		return false;
	}

	FVal FBody::EvalOperand(const FDabExpr& Expr, const FDcrType* Expected, const FString& Preferred)
	{
		if (IsLiteralShape(Expr))
		{
			return MakeLiteralValue(Expr);
		}
		return Eval(Expr, Expected, Preferred);
	}

	FVal FBody::Eval(const FDabExpr& Expr, const FDcrType* Expected, const FString& Preferred)
	{
		if (IsLiteralShape(Expr))
		{
			return MakeLiteralValue(Expr);
		}
		switch (Expr.Kind)
		{
		case EDabNodeKind::ParenExpr:
		{
			const FDabParenExpr& Paren = static_cast<const FDabParenExpr&>(Expr);
			return Paren.Inner ? EvalOperand(*Paren.Inner, Expected, Preferred) : MakeAbsent(Expr.Span);
		}
		case EDabNodeKind::NameExpr:
		{
			// `Kinematic` written for an enum: its value, though a variable of another type has that name too.
			const FDabNameExpr& Name = static_cast<const FDabNameExpr&>(Expr);
			const UEnum* Enum = Expected && !Expected->ObjectPath.IsEmpty() ? Cast<UEnum>(RigVMTypeUtils::FindObjectFromCPPTypeObjectPath(Expected->ObjectPath)) : nullptr;
			if (Enum && IsEnumValueHere(Name.Name, *Enum))
			{
				return MakeLiteralValue(Expr);
			}
			return EvalName(Name);
		}
		case EDabNodeKind::MemberExpr:
			return EvalMember(static_cast<const FDabMemberExpr&>(Expr), Expected, Preferred);
		case EDabNodeKind::CallExpr:
			return EvalCall(static_cast<const FDabCallExpr&>(Expr), Expected, Preferred);
		case EDabNodeKind::UnaryExpr:
			return EvalUnary(static_cast<const FDabUnaryExpr&>(Expr), Expected, Preferred);
		case EDabNodeKind::BinaryExpr:
			return EvalBinary(static_cast<const FDabBinaryExpr&>(Expr), Expected, Preferred);
		case EDabNodeKind::ConditionalExpr:
			return EvalConditional(static_cast<const FDabConditionalExpr&>(Expr), Expected, Preferred);
		case EDabNodeKind::IndexExpr:
			return EvalIndex(static_cast<const FDabIndexExpr&>(Expr), Expected, Preferred);
		default:
			break;
		}
		Sink.Error(TEXT("DAB9004"), Expr.Span, TEXT("This is not a value."));
		return MakeAbsent(Expr.Span);
	}

	// ------------------------------------------------------------------------------------------
	// Names and members
	// ------------------------------------------------------------------------------------------

	FVal FBody::MakeVariableGet(const FDcrModelVariable& Variable, bool bLocal, const FString& Preferred, const FDabSpan& Span)
	{
		FNodeRec& Rec = NewRec(Preferred, TEXT("Get_") + Variable.Name);
		Rec.Node.Kind = EDcrNodeKind::VariableGet;
		Rec.Node.Variable = Variable.Name;
		Rec.Node.VariableType = Variable.Type;
		Rec.Node.bLocalVariable = bLocal;
		FPin& Pin = Rec.Sig.Pins.AddDefaulted_GetRef();
		Pin.Name = TEXT("Value");
		Pin.bOutput = true;
		Pin.Type = Variable.Type;
		return MakeOutput(Rec, Span);
	}

	void FBody::Settle(FVal& Value)
	{
		if (Value.Pending == INDEX_NONE || !Pendings.IsValidIndex(Value.Pending) || !Pendings[Value.Pending]->bResolved)
		{
			return;
		}
		Value.Pending = INDEX_NONE;
		if (Value.Rec)
		{
			if (const FPin* Pin = Value.Rec->Sig.Find(Value.Pin))
			{
				Value.Type = Pin->Type;
			}
		}
	}

	FVal FBody::EvalName(const FDabNameExpr& Expr)
	{
		if (const FVal* Found = LookupScope(Expr.Name))
		{
			FVal Value = *Found;
			Settle(Value);
			return Value;
		}
		if (const FDcrModelVariable* Local = FindLocal(Expr.Name))
		{
			return MakeVariableGet(*Local, true, FString(), Expr.Span);
		}
		if (const FDcrModelVariable* Variable = G.FindVariable(Expr.Name))
		{
			return MakeVariableGet(*Variable, false, FString(), Expr.Span);
		}
		// Nothing here has this name: it may be a value of the enum it is written for, which the pin it goes to says.
		return MakeLiteralValue(Expr);
	}

	FVal FBody::EvalMember(const FDabMemberExpr& Expr, const FDcrType* Expected, const FString& Preferred)
	{
		if (!Expr.Object)
		{
			return MakeAbsent(Expr.Span);
		}
		if (const FDabNameExpr* Object = StripDabParens(*Expr.Object).As<FDabNameExpr>(); Object && !HasName(Object->Name))
		{
			// `ERigVMTransformSpace.GlobalSpace`: the enum named, then its value.
			return MakeLiteralValue(Expr);
		}
		const FVal Base = EvalOperand(*Expr.Object, nullptr, Preferred);
		if (Base.Kind == FVal::EKind::Invalid)
		{
			return Base;
		}
		return SelectMember(Base, Expr.Member, Expr.MemberSpan);
	}

	FVal FBody::SelectMember(const FVal& InBase, const FString& Member, const FDabSpan& Span)
	{
		FVal Base = InBase;
		if (Base.Kind != FVal::EKind::Pin || !Base.Rec)
		{
			Sink.Error(TEXT("DAB9004"), Span, FString::Printf(TEXT("This has no member `%s`: it is not a value of a node."), *Member));
			return MakeAbsent(Span);
		}
		Settle(Base);
		FNodeRec& Rec = *Base.Rec;
		// Picking an output of a call whose types are not known yet keeps it open: where the output goes tells them.
		const bool bPicksOutput = Base.Pin.IsEmpty() || (Base.Sub.IsEmpty() && Member == Base.Pin);
		if (Base.Pending != INDEX_NONE && !bPicksOutput)
		{
			Sink.Error(TEXT("DAB9003"), Span, FString::Printf(TEXT("The type of this call is not known yet, so `.%s` cannot be read from it."), *Member),
				TEXT("Give the result a type with a typed let, as float Scale = ...;"));
			return MakeAbsent(Span);
		}
		if (Base.Pin.IsEmpty())
		{
			// A call with several outputs: the member picks one.
			const FPin* Output = Rec.Sig.Find(Member);
			if (!Output || !Output->bOutput)
			{
				Sink.Error(TEXT("DAB9001"), Span, FString::Printf(TEXT("`%s` has no output called `%s`."), *Rec.Node.Name, *Member));
				return MakeAbsent(Span);
			}
			FVal Value = Base;
			Value.Pin = Member;
			Value.Sub.Reset();
			Value.Type = Output->Type;
			Value.Span = Span;
			return Value;
		}

		const FPin* Root = Rec.Sig.Find(Base.Pin);
		const FDcrType RootType = Root ? Root->Type : FDcrType();
		const FString Path = Base.Sub.IsEmpty() ? Member : Base.Sub + TEXT(".") + Member;
		FDcrType MemberType;
		FString EnginePath;
		if (RootType.IsSet() && GetTypeAtSubPath(RootType, Path, MemberType, EnginePath))
		{
			FVal Value = Base;
			Value.Sub = EnginePath;
			Value.Type = MemberType;
			Value.Span = Span;
			return Value;
		}
		// The only output's own name: `GetTransform(x).Transform`.
		if (Base.Sub.IsEmpty() && Member == Base.Pin)
		{
			return Base;
		}
		Sink.Error(TEXT("DAB9011"), Span, FString::Printf(TEXT("%s has no member `%s`."), *DescribeTypeOf(Base.Sub.IsEmpty() ? RootType : Base.Type), *Member));
		return MakeAbsent(Span);
	}

	// ------------------------------------------------------------------------------------------
	// Calls
	// ------------------------------------------------------------------------------------------

	bool FBody::ResolveCallee(const FDabCallExpr& Call, FCallee& Out)
	{
		FString Name;
		FString LibraryName;
		const FDabSpan Span = Call.Callee ? Call.Callee->Span : Call.Span;
		if (const FDabNameExpr* Plain = Call.Callee ? Call.Callee->As<FDabNameExpr>() : nullptr)
		{
			Name = Plain->Name;
		}
		else if (const FDabMemberExpr* Qualified = Call.Callee ? Call.Callee->As<FDabMemberExpr>() : nullptr)
		{
			const FDabNameExpr* Object = Qualified->Object ? Qualified->Object->As<FDabNameExpr>() : nullptr;
			if (!Object)
			{
				Sink.Error(TEXT("DAB9001"), Span, TEXT("A function is called by its name, or as `Library.Name`."));
				return false;
			}
			LibraryName = Object->Name;
			Name = Qualified->Member;
		}
		else
		{
			Sink.Error(TEXT("DAB9001"), Span, TEXT("A function is called by its name, or as `Library.Name`."));
			return false;
		}

		if (LibraryName.IsEmpty() && !G.FindFunction(Name))
		{
			if (Name == TEXT("Injected"))
			{
				Out.Kind = FCallee::EKind::Injected;
				return true;
			}
			if (Name == TEXT("Range") && FindDcrUnits(Name).IsEmpty())
			{
				Out.Kind = FCallee::EKind::Range;
				return true;
			}
			if (Name == DcrRunEventCallee && FindDcrUnits(Name).IsEmpty())
			{
				Out.Kind = FCallee::EKind::RunEvent;
				return true;
			}
		}

		if (!LibraryName.IsEmpty())
		{
			FLibrary* Library = G.FindLibrary(LibraryName);
			if (!Library)
			{
				Sink.Error(TEXT("DAB9001"), Span, FString::Printf(TEXT("`%s` is not a function library: list it in `Uses` first."), *LibraryName));
				return false;
			}
			if (const FExternalFunction* External = Library->Public.FindByPredicate([&Name](const FExternalFunction& Candidate) { return Candidate.Function.Name == Name; }))
			{
				Out.Kind = FCallee::EKind::External;
				Out.Library = Library;
				Out.External = External;
				return true;
			}
			if (Library->PrivateNames.Contains(Name))
			{
				Sink.Error(TEXT("DAB9012"), Span, FString::Printf(TEXT("`%s` is not a public function of `%s`."), *Name, *LibraryName),
					TEXT("Mark the function [ Public ] in its library, or call another."));
				return false;
			}
			Sink.Error(TEXT("DAB9001"), Span, FString::Printf(TEXT("`%s` has no function called `%s`."), *LibraryName, *Name));
			return false;
		}

		// Functions first: the rig's own, then the libraries'. Then units, dispatches and templates.
		FFunctionDecl* Local = G.FindFunction(Name);
		TArray<TPair<FLibrary*, const FExternalFunction*>> Externals;
		TArray<FString> PrivateIn;
		for (const TUniquePtr<FLibrary>& Library : G.Libraries)
		{
			if (const FExternalFunction* External = Library->Public.FindByPredicate([&Name](const FExternalFunction& Candidate) { return Candidate.Function.Name == Name; }))
			{
				Externals.Emplace(Library.Get(), External);
			}
			else if (Library->PrivateNames.Contains(Name))
			{
				PrivateIn.Add(Library->Name);
			}
		}
		TArray<FDcrNodeType> Units = FindDcrUnits(Name);
		TArray<FDcrNodeType> Dispatches = FindDcrDispatches(Name);
		TArray<FName> Notations = FindDcrTemplateNotations(Name);

		// The rig's own function hides everything else of its name: the engine's node is then written by its
		// full name, a library's function as `Library.Name`. A library's function meeting an engine node, or
		// another library's, has nothing to win by.
		if (Local)
		{
			Out.Kind = FCallee::EKind::Function;
			Out.Function = Local;
			return true;
		}
		const bool bStruct = !Units.IsEmpty() || !Dispatches.IsEmpty() || !Notations.IsEmpty();
		if (Externals.Num() > 1 || (Externals.Num() == 1 && bStruct))
		{
			Sink.Error(TEXT("DAB9002"), Span, FString::Printf(TEXT("`%s` names more than one thing: %s."), *Name,
				Externals.Num() > 1 ? TEXT("functions of several libraries in Uses") : TEXT("a library's function and a node of the engine")),
				TEXT("Write the library's name before it (`Library.Name`), or the node's full name (`RigUnit_...`)."));
			return false;
		}
		if (Externals.Num() == 1)
		{
			Out.Kind = FCallee::EKind::External;
			Out.Library = Externals[0].Key;
			Out.External = Externals[0].Value;
			return true;
		}
		if (!Units.IsEmpty())
		{
			if (Units.Num() > 1)
			{
				TArray<FString> Names;
				for (const FDcrNodeType& Unit : Units)
				{
					Names.Add(Unit.Struct->GetName());
				}
				Sink.Error(TEXT("DAB9002"), Span, FString::Printf(TEXT("`%s` names more than one unit."), *Name), TEXT("Write the full name: ") + FString::Join(Names, TEXT(", ")) + TEXT("."));
				return false;
			}
			Out.Kind = FCallee::EKind::Unit;
			Out.Struct = Units[0].Struct;
			return true;
		}
		if (!Dispatches.IsEmpty())
		{
			if (Dispatches.Num() > 1)
			{
				TArray<FString> Names;
				for (const FDcrNodeType& Dispatch : Dispatches)
				{
					Names.Add(Dispatch.Struct->GetName());
				}
				Sink.Error(TEXT("DAB9002"), Span, FString::Printf(TEXT("`%s` names more than one dispatch."), *Name), TEXT("Write the full name: ") + FString::Join(Names, TEXT(", ")) + TEXT("."));
				return false;
			}
			Out.Kind = FCallee::EKind::Dispatch;
			Out.Struct = Dispatches[0].Struct;
			return true;
		}
		if (!Notations.IsEmpty())
		{
			Out.Kind = FCallee::EKind::Template;
			Out.Notations = Notations;
			return true;
		}
		if (!PrivateIn.IsEmpty())
		{
			Sink.Error(TEXT("DAB9012"), Span, FString::Printf(TEXT("`%s` is not a public function of `%s`."), *Name, *PrivateIn[0]),
				TEXT("Mark the function [ Public ] in its library, or call another."));
			return false;
		}
		Sink.Error(TEXT("DAB9001"), Span, FString::Printf(TEXT("There is no unit, dispatch, template or function called `%s`."), *Name),
			TEXT("Check the spelling. Units are written without their RigUnit_ / RigVMFunction_ prefix; a function of another rig needs that rig in `Uses`."));
		return false;
	}

	bool FBody::BindArgs(const FDabCallExpr& Call, const FSignature& Sig, const FString& What, bool bAggregate, TArray<FBoundArg>& OutArgs, TArray<const FDabExpr*>& OutExtra,
		const TSet<const FDabArgument*>* Skip)
	{
		bool bOk = true;
		int32 PositionalIndex = 0;
		TSet<FString> Seen;
		for (const FDabArgumentPtr& Arg : Call.Args)
		{
			if (!Arg || !Arg->Value || (Skip && Skip->Contains(Arg.Get())))
			{
				continue;
			}
			const FPin* Pin = nullptr;
			if (!Arg->IsNamed())
			{
				Pin = Sig.GetInput(PositionalIndex++);
				if (!Pin)
				{
					if (bAggregate)
					{
						OutExtra.Add(Arg->Value.Get());
						continue;
					}
					Sink.Error(TEXT("DAB9004"), Arg->Span, FString::Printf(TEXT("`%s` takes %d arguments or fewer."), *What, Sig.NumInputs()),
						TEXT("Only a node that adds up many values (Add, Multiply, Or, ...) takes more."));
					bOk = false;
					continue;
				}
			}
			else
			{
				Pin = Sig.Find(Arg->Name);
				if (!Pin || !Pin->bInput)
				{
					Sink.Error(TEXT("DAB9001"), Arg->NameSpan, FString::Printf(TEXT("`%s` has no input called `%s`."), *What, *Arg->Name));
					bOk = false;
					continue;
				}
			}
			if (Seen.Contains(Pin->Name))
			{
				Sink.Error(TEXT("DAB9004"), Arg->Span, FString::Printf(TEXT("`%s` is given twice to `%s`."), *Pin->Name, *What));
				bOk = false;
				continue;
			}
			Seen.Add(Pin->Name);
			FBoundArg& Bound = OutArgs.AddDefaulted_GetRef();
			Bound.Pin = Pin->Name;
			Bound.Expr = Arg->Value.Get();
			Bound.Source = Arg.Get();
		}
		return bOk;
	}

	FVal FBody::EvalCall(const FDabCallExpr& Expr, const FDcrType* Expected, const FString& Preferred)
	{
		// `Unsupported("...", Pin = value)`: what a decompile could not say. Its values are still built, so what they
		// call is called and what they name is used; the call itself is reported as nothing the engine has.
		const FDabNameExpr* CalleeName = Expr.Callee ? Expr.Callee->As<FDabNameExpr>() : nullptr;
		if (CalleeName && CalleeName->Name == TEXT("Unsupported") && !G.FindFunction(CalleeName->Name))
		{
			for (const FDabArgumentPtr& Arg : Expr.Args)
			{
				if (Arg && Arg->Value && !IsLiteralShape(*Arg->Value))
				{
					// Where the value goes is not known: a call it leaves untyped is not reported again.
					const FVal Value = EvalOperand(*Arg->Value, nullptr, FString());
					if (Value.Pending != INDEX_NONE && Pendings.IsValidIndex(Value.Pending))
					{
						Pendings[Value.Pending]->bFailed = true;
					}
				}
			}
		}
		FCallee Callee;
		if (!ResolveCallee(Expr, Callee))
		{
			return MakeAbsent(Expr.Span);
		}
		switch (Callee.Kind)
		{
		case FCallee::EKind::Function:
		case FCallee::EKind::External:
			return BuildFunction(Expr, Callee, Expected, Preferred);
		case FCallee::EKind::Unit:
			return BuildUnit(Expr, *Callee.Struct, Expected, Preferred);
		case FCallee::EKind::Dispatch:
			return BuildDispatch(Expr, *Callee.Struct, Expected, Preferred);
		case FCallee::EKind::Template:
			return BuildTemplate(Expr, Callee, Expected, Preferred);
		case FCallee::EKind::Injected:
			if (Expr.Args.Num() == 2 && Expr.Args[1] && !Expr.Args[1]->IsNamed() && Expr.Args[1]->Value && Expr.Args[1]->Value->Is<FDabNameExpr>()
				&& Expr.Args[1]->Value->As<FDabNameExpr>()->Name == TEXT("Output"))
			{
				return BuildInjectedOutput(Expr, Expected, Preferred);
			}
			Sink.Error(TEXT("DAB9004"), Expr.Span, TEXT("`Injected(...)` goes where a node's input takes a value: `Pin = Injected(Node(...))`, or on the output its node reads: `Injected(Node(Other.Pin), Output)`."));
			break;
		case FCallee::EKind::Range:
			Sink.Error(TEXT("DAB9004"), Expr.Span, TEXT("`Range(n)` is the iterable of a `for`: `for (Index, Ratio in Range(5))`."));
			break;
		case FCallee::EKind::RunEvent:
			return BuildRunEvent(Expr, Preferred);
		case FCallee::EKind::None:
			break;
		}
		return MakeAbsent(Expr.Span);
	}

	// ------------------------------------------------------------------------------------------
	// Units
	// ------------------------------------------------------------------------------------------

	bool FBody::AddAggregates(FNodeRec& Rec, const UScriptStruct& Struct, const FSignature& Sig, const TArray<const FDabExpr*>& Extra)
	{
		const FString Second = GetUnitAggregateSecond(Struct);
		const FPin* First = Sig.Pins.FindByPredicate([](const FPin& Pin) { return Pin.bAggregate && Pin.bInput; });
		if (Second.IsEmpty() || !First)
		{
			Sink.Error(TEXT("DAB9004"), Extra.IsEmpty() ? FDabSpan() : Extra[0]->Span, FString::Printf(TEXT("`%s` takes %d arguments or fewer."), *GetDcrNodeTypeName(Struct), Sig.NumInputs()));
			return false;
		}
		FString Last = Second;
		for (const FDabExpr* Expr : Extra)
		{
			Last = GetNextAggregateName(Last);
			Rec.Node.AggregatePins.Add(Last);
			FPin Added;
			Added.Name = Last;
			Added.bInput = true;
			Added.Property = First->Property;
			Added.Type = First->Type;
			ApplyArg(Rec, Added, *Expr);
		}
		return true;
	}

	FVal FBody::BuildUnit(const FDabCallExpr& Call, const UScriptStruct& Struct, const FDcrType* Expected, const FString& Preferred)
	{
		const FSignature Sig = MakeUnitSignature(Struct);
		// `Name = TraitType(...)` where the unit has no input of that name: a trait put on the node.
		TArray<TPair<const FDabArgument*, const UScriptStruct*>> Traits;
		TSet<const FDabArgument*> TraitArgs;
		for (const FDabArgumentPtr& Arg : Call.Args)
		{
			if (!Arg || !Arg->Value || !Arg->IsNamed() || Sig.Find(Arg->Name))
			{
				continue;
			}
			const FDabCallExpr* TraitCall = StripDabParens(*Arg->Value).As<FDabCallExpr>();
			const FDabNameExpr* TraitType = TraitCall && TraitCall->Callee ? TraitCall->Callee->As<FDabNameExpr>() : nullptr;
			if (const UScriptStruct* TraitStruct = TraitType ? FindDcrTraitStruct(TraitType->Name) : nullptr)
			{
				Traits.Emplace(Arg.Get(), TraitStruct);
				TraitArgs.Add(Arg.Get());
			}
		}
		TArray<FBoundArg> Args;
		TArray<const FDabExpr*> Extra;
		const FString What = GetDcrNodeTypeName(Struct);
		const bool bBound = BindArgs(Call, Sig, What, !GetUnitAggregateSecond(Struct).IsEmpty(), Args, Extra, &TraitArgs);

		FNodeRec& Rec = NewRec(Preferred, GetDcrShortName(Struct.GetName()));
		InitUnitRec(Rec, Struct);
		if (!bBound)
		{
			return MakeAbsent(Call.Span);
		}
		for (const FBoundArg& Arg : Args)
		{
			if (const FPin* Pin = Rec.Sig.Find(Arg.Pin))
			{
				ApplyArg(Rec, *Pin, *Arg.Expr);
			}
		}
		for (const TPair<const FDabArgument*, const UScriptStruct*>& Trait : Traits)
		{
			// A trait is an input of its struct past the unit's own; a new one holds its members' defaults.
			FPin Pin;
			Pin.Name = Trait.Key->Name;
			Pin.bInput = true;
			Pin.Type = MakeDcrType(*Trait.Value);
			FStructOnScope Fresh(Trait.Value);
			Pin.Default = ExportDcrStructText(*Trait.Value, Fresh.GetStructMemory());
			Rec.Sig.Pins.Add(Pin);
			FDcrModelTrait& Added = Rec.Node.Traits.AddDefaulted_GetRef();
			Added.Name = Pin.Name;
			Added.StructPath = Trait.Value->GetPathName();
			ApplyLiteral(Rec, Pin, StripDabParens(*Trait.Key->Value));
		}
		if (!Extra.IsEmpty() && !AddAggregates(Rec, Struct, Sig, Extra))
		{
			return MakeAbsent(Call.Span);
		}
		Sequence(Rec);
		return MakeOutput(Rec, Call.Span);
	}

	FVal FBody::BuildRunEvent(const FDabCallExpr& Call, const FString& Preferred)
	{
		const FDabNameExpr* Event = Call.Args.Num() == 1 && !Call.Args[0]->IsNamed() && Call.Args[0]->Value ? Call.Args[0]->Value->As<FDabNameExpr>() : nullptr;
		if (!Event)
		{
			Sink.Error(TEXT("DAB9004"), Call.Span, TEXT("`RunEvent` takes the event to run, named as `Event` names it: `RunEvent(Backward)`."));
			return MakeAbsent(Call.Span);
		}
		FNodeRec& Rec = NewRec(Preferred, TEXT("InvokeEntryNode"));
		Rec.Node.Kind = EDcrNodeKind::InvokeEntry;
		const FDcrEventWord* Word = FindDcrEventByWord(Event->Name);
		Rec.Node.Function = Word ? FString(Word->EventName) : Event->Name;
		// The node's one execute pin, which it both takes and gives (URigVMController::AddInvokeEntryNode).
		Rec.Sig.ExecIn.Add(TEXT("ExecuteContext"));
		Rec.Sig.ExecOut.Add(TEXT("ExecuteContext"));
		Sequence(Rec);
		FVal Value;
		Value.Kind = FVal::EKind::Void;
		Value.Rec = &Rec;
		Value.Span = Call.Span;
		return Value;
	}

	FVal FBody::BuildUnitFrom(const UScriptStruct& Struct, const TArray<TPair<FString, FVal>>& Args, const FString& Preferred, const FDabSpan& Span)
	{
		FNodeRec& Rec = NewRec(Preferred, GetDcrShortName(Struct.GetName()));
		InitUnitRec(Rec, Struct);
		for (const TPair<FString, FVal>& Arg : Args)
		{
			if (const FPin* Pin = Rec.Sig.Find(Arg.Key))
			{
				ApplyValue(Rec, *Pin, Arg.Value, Arg.Value.Span);
			}
			else
			{
				Sink.Error(TEXT("DAB9001"), Span, FString::Printf(TEXT("`%s` has no input called `%s`."), *GetDcrNodeTypeName(Struct), *Arg.Key));
			}
		}
		Sequence(Rec);
		return MakeOutput(Rec, Span);
	}

	// ------------------------------------------------------------------------------------------
	// Dispatches
	// ------------------------------------------------------------------------------------------

	void FBody::FinishDispatch(FPendingDispatch& Pending, const TMap<FString, FDcrType>& Types)
	{
		FNodeRec& Rec = *Pending.Rec;
		FDispatchInfo Info;
		if (!GetDispatchInfo(*Pending.Struct, Info))
		{
			Pending.bFailed = true;
			return;
		}
		Rec.Node.Kind = EDcrNodeKind::Dispatch;
		Rec.Node.Notation = GetDcrDispatchNotation(*Pending.Struct);
		Rec.Node.ExecuteIn = Rec.Sig.ExecIn;
		Rec.Node.ExecuteOut = Rec.Sig.ExecOut;
		for (FPin& Pin : Rec.Sig.Pins)
		{
			if (!Pin.Type.IsSet())
			{
				if (const FDcrType* Type = Types.Find(Pin.Name))
				{
					Pin.Type = *Type;
				}
			}
			if (Pin.bInput && Pin.Type.IsSet())
			{
				const int32 TypeIndex = GetTypeIndexOf(Pin.Type);
				if (TypeIndex != INDEX_NONE)
				{
					Pin.Default = Info.Factory->GetArgumentDefaultValue(FName(*Pin.Name), TypeIndex);
				}
			}
			if (Pin.bWildcard && Pin.Type.IsSet())
			{
				Rec.Node.ResolvedTypes.Emplace(Pin.Name, Pin.Type);
			}
		}
		Pending.bResolved = true;
		Rec.Pending = INDEX_NONE;

		for (TPair<FString, FVal>& Arg : Pending.Args)
		{
			if (const FPin* Pin = Rec.Sig.Find(Arg.Key))
			{
				ApplyValue(Rec, *Pin, Arg.Value, Arg.Value.Span);
			}
		}
		// A resolved wildcard keeps its value whether or not it was written.
		for (const FPin& Pin : Rec.Sig.Pins)
		{
			if (Pin.bWildcard && Pin.bInput && Pin.Type.IsSet() && !Pending.Args.ContainsByPredicate([&Pin](const TPair<FString, FVal>& Arg) { return Arg.Key == Pin.Name; }))
			{
				ApplyDefault(Rec, Pin);
			}
		}
	}

	bool FBody::SettlePending(FVal& Value, const FDcrType* Target)
	{
		if (Value.Pending == INDEX_NONE || !Pendings.IsValidIndex(Value.Pending))
		{
			return true;
		}
		FPendingDispatch& Pending = *Pendings[Value.Pending];
		if (!Pending.bResolved && !Pending.bFailed && Target && Target->IsSet() && Pending.Rec)
		{
			FDispatchInfo Info;
			if (GetDispatchInfo(*Pending.Struct, Info))
			{
				TMap<FString, FDcrType> Known = Pending.Known;
				FString OutputPin = Value.Pin;
				if (OutputPin.IsEmpty())
				{
					int32 Wildcards = 0;
					for (const FPin& Pin : Pending.Rec->Sig.Pins)
					{
						if (Pin.bOutput && Pin.bWildcard)
						{
							OutputPin = Pin.Name;
							++Wildcards;
						}
					}
					if (Wildcards != 1)
					{
						OutputPin.Reset();
					}
				}
				const FPin* Output = OutputPin.IsEmpty() ? nullptr : Pending.Rec->Sig.Find(OutputPin);
				if (Output && Output->bWildcard && Value.Sub.IsEmpty() && !Known.Contains(OutputPin))
				{
					Known.Add(OutputPin, *Target);
					TMap<FString, FDcrType> Types;
					const EResolveResult Result = ResolveTemplateTypes(*Info.Template, Known, Types);
					if (Result == EResolveResult::Resolved)
					{
						FinishDispatch(Pending, Types);
					}
					else if (Result == EResolveResult::Mismatch)
					{
						Pending.bFailed = true;
						Sink.Error(TEXT("DAB9004"), Pending.Span, FString::Printf(TEXT("`%s` cannot give %s."), *GetDcrShortName(Pending.Struct->GetName()), *DescribeTypeOf(*Target)));
					}
				}
			}
		}
		Settle(Value);
		return !Pending.bFailed;
	}

	FVal FBody::BuildDispatch(const FDabCallExpr& Call, const UScriptStruct& Struct, const FDcrType* Expected, const FString& Preferred)
	{
		const FSignature Sig = MakeDispatchSignature(Struct);
		TArray<FBoundArg> Args;
		TArray<const FDabExpr*> Extra;
		const FString What = GetDcrNodeTypeName(Struct);
		if (!BindArgs(Call, Sig, What, false, Args, Extra))
		{
			return MakeAbsent(Call.Span);
		}
		TArray<TPair<FString, FVal>> Values;
		for (const FBoundArg& Arg : Args)
		{
			const FPin* Pin = Sig.Find(Arg.Pin);
			Values.Emplace(Arg.Pin, EvalOperand(*Arg.Expr, Pin && Pin->Type.IsSet() ? &Pin->Type : nullptr, FString()));
		}
		return BuildDispatchFrom(Struct, MoveTemp(Values), Expected, Preferred, Call.Span);
	}

	FVal FBody::BuildDispatchFrom(const UScriptStruct& Struct, TArray<TPair<FString, FVal>> Args, const FDcrType* Expected, const FString& Preferred, const FDabSpan& Span)
	{
		FDispatchInfo Info;
		if (!GetDispatchInfo(Struct, Info))
		{
			Sink.Error(TEXT("DAB9001"), Span, FString::Printf(TEXT("`%s` is not a node the engine can make."), *Struct.GetName()));
			return MakeAbsent(Span);
		}
		for (const TPair<FString, FVal>& Arg : Args)
		{
			if (Arg.Value.Kind == FVal::EKind::Invalid)
			{
				return MakeAbsent(Span);
			}
		}

		FNodeRec& Rec = NewRec(Preferred, GetDcrShortName(Struct.GetName()));
		Rec.Sig = MakeDispatchSignature(Struct);
		Rec.Node.Kind = EDcrNodeKind::Dispatch;
		Rec.Node.Notation = GetDcrDispatchNotation(Struct);
		Rec.Node.ExecuteIn = Rec.Sig.ExecIn;
		Rec.Node.ExecuteOut = Rec.Sig.ExecOut;

		TUniquePtr<FPendingDispatch> Owned = MakeUnique<FPendingDispatch>();
		FPendingDispatch& Pending = *Owned;
		Pending.Rec = &Rec;
		Pending.Struct = &Struct;
		Pending.Span = Span;
		Pending.Args = MoveTemp(Args);
		Rec.Pending = Pendings.Num();
		Pendings.Add(MoveTemp(Owned));

		for (TPair<FString, FVal>& Arg : Pending.Args)
		{
			Settle(Arg.Value);
			const FPin* Pin = Rec.Sig.Find(Arg.Key);
			if (!Pin || Pin->Type.IsSet())
			{
				continue;
			}
			// What tells a wildcard: a typed value, a typed let's literal, or `true` / `false` (which is nothing but a bool).
			const FDabLiteralExpr* Literal = Arg.Value.Kind == FVal::EKind::Literal && Arg.Value.Expr ? StripDabParens(*Arg.Value.Expr).As<FDabLiteralExpr>() : nullptr;
			if ((Arg.Value.IsPin() && !Arg.Value.Pin.IsEmpty() && Arg.Value.Type.IsSet()) || (Arg.Value.Kind == FVal::EKind::Literal && Arg.Value.Type.IsSet()))
			{
				Pending.Known.Add(Arg.Key, Arg.Value.Type);
			}
			else if (Literal && Literal->LiteralKind == EDabLiteralKind::Bool)
			{
				Pending.Known.Add(Arg.Key, MakeDcrBuiltinType(TEXT("bool")));
			}
			else if (Arg.Value.Kind == FVal::EKind::Literal && Arg.Value.Expr && IsElementKeyLiteral(StripDabParens(*Arg.Value.Expr)))
			{
				// `Bone(name)` is nothing but an element key.
				Pending.Known.Add(Arg.Key, MakeDcrType(*FRigElementKey::StaticStruct()));
			}
		}

		TMap<FString, FDcrType> Types;
		EResolveResult Result = ResolveTemplateTypes(*Info.Template, Pending.Known, Types);
		if (Result == EResolveResult::Open && Expected && Expected->IsSet())
		{
			// Where the result goes: the one wildcard output takes the type of the pin it is linked to.
			const FPin* Output = nullptr;
			int32 Wildcards = 0;
			for (const FPin& Pin : Rec.Sig.Pins)
			{
				if (Pin.bOutput && Pin.bWildcard && !Pin.bInput)
				{
					Output = &Pin;
					++Wildcards;
				}
			}
			if (Output && Wildcards == 1 && !Pending.Known.Contains(Output->Name))
			{
				TMap<FString, FDcrType> WithTarget = Pending.Known;
				WithTarget.Add(Output->Name, *Expected);
				TMap<FString, FDcrType> TargetTypes;
				if (ResolveTemplateTypes(*Info.Template, WithTarget, TargetTypes) == EResolveResult::Resolved)
				{
					Types = MoveTemp(TargetTypes);
					Result = EResolveResult::Resolved;
				}
			}
		}

		if (Result == EResolveResult::Mismatch)
		{
			Pending.bFailed = true;
			Sink.Error(TEXT("DAB9004"), Span, FString::Printf(TEXT("The arguments fit no version of `%s`."), *GetDcrNodeTypeName(Struct)));
			return MakeAbsent(Span);
		}
		if (Result == EResolveResult::Resolved)
		{
			FinishDispatch(Pending, Types);
			Sequence(Rec);
			return MakeOutput(Rec, Span);
		}
		if (Rec.Sig.IsMutable())
		{
			Pending.bFailed = true;
			Sink.Error(TEXT("DAB9003"), Span, FString::Printf(TEXT("The types of `%s` cannot be told from its arguments."), *GetDcrNodeTypeName(Struct)),
				TEXT("Give the arguments types, or write a typed let for the value."));
			return MakeAbsent(Span);
		}
		return MakeOutput(Rec, Span);
	}

	// ------------------------------------------------------------------------------------------
	// Templates named without a struct
	// ------------------------------------------------------------------------------------------

	FVal FBody::BuildTemplate(const FDabCallExpr& Call, const FCallee& Callee, const FDcrType* Expected, const FString& Preferred)
	{
		TArray<const UScriptStruct*> Candidates;
		for (const FName& Notation : Callee.Notations)
		{
			const FRigVMTemplate* Template = FRigVMRegistry::Get().FindTemplate(Notation);
			for (int32 Index = 0; Template && Index < Template->NumPermutations(); ++Index)
			{
				const FRigVMFunction* Permutation = Template->GetPermutation(Index);
				const UScriptStruct* Unit = Permutation ? Permutation->Struct : nullptr;
				if (Unit && !Unit->HasMetaData(FRigVMStruct::DeprecatedMetaName))
				{
					Candidates.AddUnique(Unit);
				}
			}
		}
		const FString What = Call.Callee && Call.Callee->Is<FDabNameExpr>() ? static_cast<const FDabNameExpr&>(*Call.Callee).Name : FString(TEXT("this call"));
		if (Candidates.IsEmpty())
		{
			Sink.Error(TEXT("DAB9001"), Call.Span, FString::Printf(TEXT("The template `%s` has no node to make."), *What));
			return MakeAbsent(Call.Span);
		}

		const FSignature First = MakeUnitSignature(*Candidates[0]);
		TArray<FBoundArg> Args;
		TArray<const FDabExpr*> Extra;
		if (!BindArgs(Call, First, What, !GetUnitAggregateSecond(*Candidates[0]).IsEmpty(), Args, Extra))
		{
			return MakeAbsent(Call.Span);
		}
		TArray<TPair<FString, FVal>> Values;
		for (const FBoundArg& Arg : Args)
		{
			FVal Value = EvalOperand(*Arg.Expr, nullptr, FString());
			if (Value.Kind == FVal::EKind::Invalid)
			{
				return MakeAbsent(Call.Span);
			}
			if (Value.IsPin() && !SettlePending(Value, nullptr))
			{
				return MakeAbsent(Call.Span);
			}
			Values.Emplace(Arg.Pin, Value);
		}

		// The version whose pins take the arguments: the fewest float/double swaps, then the most doubles, then the first.
		const UScriptStruct* Best = nullptr;
		int32 BestSwaps = MAX_int32;
		int32 BestDoubles = -1;
		int32 Fitting = 0;
		bool bTyped = Expected && Expected->IsSet();
		for (const TPair<FString, FVal>& Arg : Values)
		{
			bTyped |= Arg.Value.IsPin() && Arg.Value.Type.IsSet();
		}
		for (const UScriptStruct* Candidate : Candidates)
		{
			const FSignature Sig = MakeUnitSignature(*Candidate);
			int32 Swaps = 0;
			int32 Doubles = 0;
			bool bFits = true;
			for (const TPair<FString, FVal>& Arg : Values)
			{
				const FPin* Pin = Sig.Find(Arg.Key);
				if (!Pin || !Pin->bInput)
				{
					bFits = false;
					break;
				}
				if (Arg.Value.IsPin())
				{
					if (Arg.Value.Pin.IsEmpty() || !Arg.Value.Type.IsSet())
					{
						bFits = false;
					}
					else if (Arg.Value.Type != Pin->Type)
					{
						if (AreTypesCompatible(Arg.Value.Type, Pin->Type))
						{
							++Swaps;
						}
						else
						{
							bFits = false;
						}
					}
				}
				else if (Arg.Value.Kind == FVal::EKind::Literal)
				{
					bFits = Arg.Value.Expr && FitsType(*Arg.Value.Expr, Pin->Type);
				}
				Doubles += Pin->Type.CPPType.Contains(TEXT("double")) ? 1 : 0;
				if (!bFits)
				{
					break;
				}
			}
			Fitting += bFits ? 1 : 0;
			if (bFits && (Swaps < BestSwaps || (Swaps == BestSwaps && Doubles > BestDoubles)))
			{
				Best = Candidate;
				BestSwaps = Swaps;
				BestDoubles = Doubles;
			}
		}
		if (Best && !bTyped && Candidates.Num() > 1 && Fitting == Candidates.Num() && Callee.Notations.Num() == 1)
		{
			// Every version takes these arguments and nothing says which: the node stays a template no type is picked
			// for, as the editor leaves one that nothing is linked to yet.
			return BuildUnresolvedTemplate(Call, Callee.Notations[0], *Candidates[0], Values, Preferred);
		}
		if (!Best)
		{
			Sink.Error(TEXT("DAB9004"), Call.Span, FString::Printf(TEXT("The arguments fit no version of `%s`."), *What),
				TEXT("Write the node by its struct name (`RigUnit_...`) to pick one."));
			return MakeAbsent(Call.Span);
		}
		FVal Result = BuildUnitFrom(*Best, Values, Preferred, Call.Span);
		if (!Extra.IsEmpty() && Result.Rec)
		{
			AddAggregates(*Result.Rec, *Best, MakeUnitSignature(*Best), Extra);
		}
		return Result;
	}

	FVal FBody::BuildUnresolvedTemplate(const FDabCallExpr& Call, const FName& Notation, const UScriptStruct& First, const TArray<TPair<FString, FVal>>& Args,
		const FString& Preferred)
	{
		const FRigVMTemplate* Template = FRigVMRegistry::Get().FindTemplate(Notation);
		FString TemplateName;
		if (!Notation.ToString().Split(TEXT("::"), &TemplateName, nullptr))
		{
			TemplateName = Notation.ToString();
		}
		FNodeRec& Rec = NewRec(Preferred, TemplateName);
		// The pins every version has; a wildcard has no type, so nothing gives it a value or reads it yet.
		Rec.Sig = MakeUnitSignature(First);
		for (FPin& Pin : Rec.Sig.Pins)
		{
			const FRigVMTemplateArgument* Argument = Template ? Template->FindArgument(FName(*Pin.Name)) : nullptr;
			if (Argument && !Argument->IsSingleton())
			{
				Pin.bWildcard = true;
				Pin.Type = FDcrType();
				Pin.Property = nullptr;
				Pin.Default.Reset();
			}
		}
		Rec.Node.Kind = EDcrNodeKind::Unit;
		Rec.Node.Notation = Notation.ToString();
		Rec.Node.ExecuteIn = Rec.Sig.ExecIn;
		Rec.Node.ExecuteOut = Rec.Sig.ExecOut;
		for (const TPair<FString, FVal>& Arg : Args)
		{
			const FPin* Pin = Rec.Sig.Find(Arg.Key);
			if (Pin && !Pin->bWildcard)
			{
				ApplyValue(Rec, *Pin, Arg.Value, Arg.Value.Span);
			}
			else
			{
				Sink.Error(TEXT("DAB9003"), Arg.Value.Span, FString::Printf(TEXT("`%s` takes several types here, and nothing says which."), *Arg.Key),
					TEXT("Give an argument a type, or write the node by its struct name."));
			}
		}
		Sink.Warning(TEXT("DAB9003"), Call.Span, FString::Printf(TEXT("Nothing says which version of `%s` this is; it is left a template with no type picked, and does nothing."), *TemplateName),
			TEXT("Give an argument a type, or write the node by its struct name."));
		Sequence(Rec);
		return MakeOutput(Rec, Call.Span);
	}

	// ------------------------------------------------------------------------------------------
	// Functions
	// ------------------------------------------------------------------------------------------

	FVal FBody::BuildFunction(const FDabCallExpr& Call, const FCallee& Callee, const FDcrType* Expected, const FString& Preferred)
	{
		if (Callee.Function && Callee.Function->bInline)
		{
			return BuildInline(Call, *Callee.Function, Preferred);
		}
		const bool bExternal = Callee.External != nullptr;
		const FString Name = bExternal ? Callee.External->Function.Name : Callee.Function->Name;
		const TArray<FDcrModelFunctionPin>& Inputs = bExternal ? Callee.External->Function.Inputs : Callee.Function->Inputs;
		const TArray<FDcrModelFunctionPin>& Outputs = bExternal ? Callee.External->Function.Outputs : Callee.Function->Outputs;
		const bool bMutable = bExternal ? Callee.External->Function.bMutable : Callee.Function->bMutable;
		if (!bExternal && !Callee.Function->bValid)
		{
			return MakeAbsent(Call.Span);
		}

		FSignature Sig;
		for (const FDcrModelFunctionPin& Input : Inputs)
		{
			FPin& Pin = Sig.Pins.AddDefaulted_GetRef();
			Pin.Name = Input.Name;
			Pin.bInput = true;
			Pin.Type = Input.Type;
			Pin.Default = Input.Default;
		}
		for (const FDcrModelFunctionPin& Output : Outputs)
		{
			if (FPin* Existing = Sig.Find(Output.Name))
			{
				Existing->bOutput = true;
				continue;
			}
			FPin& Pin = Sig.Pins.AddDefaulted_GetRef();
			Pin.Name = Output.Name;
			Pin.bOutput = true;
			Pin.Type = Output.Type;
		}
		if (bMutable)
		{
			const FString ExecuteName = bExternal && !Callee.External->ExecuteName.IsEmpty() ? Callee.External->ExecuteName : FString(DefaultExecuteName);
			Sig.ExecIn.Add(ExecuteName);
			Sig.ExecOut.Add(ExecuteName);
		}

		TArray<FBoundArg> Args;
		TArray<const FDabExpr*> Extra;
		const bool bBound = BindArgs(Call, Sig, Name, false, Args, Extra);

		FNodeRec& Rec = NewRec(Preferred, Name);
		Rec.Sig = Sig;
		Rec.Node.Kind = bExternal ? EDcrNodeKind::ExternalCall : EDcrNodeKind::LocalCall;
		Rec.Node.Function = Name;
		if (bExternal)
		{
			Rec.Node.FunctionHost = Callee.Library->Host;
		}
		Rec.Node.ExecuteIn = Sig.ExecIn;
		Rec.Node.ExecuteOut = Sig.ExecOut;
		if (!bBound)
		{
			return MakeAbsent(Call.Span);
		}
		for (const FBoundArg& Arg : Args)
		{
			if (const FPin* Pin = Rec.Sig.Find(Arg.Pin))
			{
				ApplyArg(Rec, *Pin, *Arg.Expr);
			}
		}
		Sequence(Rec);
		return MakeOutput(Rec, Call.Span);
	}

	FVal FBody::BuildInline(const FDabCallExpr& Call, FFunctionDecl& Decl, const FString& Preferred)
	{
		if (Decl.Calls > 0)
		{
			Sink.Error(TEXT("DAB9008"), Call.Span, FString::Printf(TEXT("The Inline function `%s` is called more than once."), *Decl.Name),
				TEXT("An Inline function is the body of one collapse node. Drop [ Inline ] to call it from several places."));
			return MakeAbsent(Call.Span);
		}
		++Decl.Calls;
		if (!Decl.bValid)
		{
			return MakeAbsent(Call.Span);
		}
		// The collapse node goes by the Inline function's name (the text names the function after the node), not by a let's.
		FNodeRec& Rec = NewRec(Decl.Name, Decl.Name);
		TUniquePtr<FDcrModelGraph> Inner = MakeUnique<FDcrModelGraph>();
		Inner->Name = Rec.Node.Name;
		// An Inline function's locals are its collapse graph's own.
		Inner->Locals = Decl.Locals;
		{
			FBody InnerBody(G, *Inner, &Decl, Where + TEXT("/") + Decl.Name);
			InnerBody.Outer = this;
			InnerBody.BeginFunction();
			InnerBody.RunFunction();
			InnerBody.Finish();
		}

		FSignature& Sig = Rec.Sig;
		for (const FDcrModelFunctionPin& Input : Decl.Inputs)
		{
			FPin& Pin = Sig.Pins.AddDefaulted_GetRef();
			Pin.Name = Input.Name;
			Pin.bInput = true;
			Pin.Type = Input.Type;
		}
		for (const FDcrModelFunctionPin& Output : Decl.Outputs)
		{
			FPin& Pin = Sig.Pins.AddDefaulted_GetRef();
			Pin.Name = Output.Name;
			Pin.bOutput = true;
			Pin.Type = Output.Type;
		}
		if (Decl.bMutable)
		{
			Sig.ExecIn.Add(DefaultExecuteName);
			Sig.ExecOut.Add(DefaultExecuteName);
		}
		Rec.Node.Kind = EDcrNodeKind::Collapse;
		Rec.Node.ExecuteIn = Sig.ExecIn;
		Rec.Node.ExecuteOut = Sig.ExecOut;

		TArray<FBoundArg> Args;
		TArray<const FDabExpr*> Extra;
		const bool bBound = BindArgs(Call, Sig, Decl.Name, false, Args, Extra);
		TSet<FString> Given;
		if (bBound)
		{
			for (const FBoundArg& Arg : Args)
			{
				Given.Add(Arg.Pin);
				if (const FPin* Pin = Rec.Sig.Find(Arg.Pin))
				{
					ApplyArg(Rec, *Pin, *Arg.Expr);
				}
			}
		}
		// A parameter nothing was given for holds its declared default.
		for (const FDcrModelFunctionPin& Input : Decl.Inputs)
		{
			if (!Given.Contains(Input.Name) && !Input.Default.IsEmpty())
			{
				SetPinValue(Rec, Input.Name, Input.Default);
			}
		}

		// The interface the collapse node shows: Entry the inputs with their defaults, Return the outputs.
		for (FDcrModelNode& Node : Inner->Nodes)
		{
			if (Node.Kind == EDcrNodeKind::Entry)
			{
				for (const FDcrModelFunctionPin& Input : Decl.Inputs)
				{
					Node.ResolvedTypes.Emplace(Input.Name, Input.Type);
					const FDcrValue* Written = Rec.Node.Values.FindByPredicate([&Input](const FDcrValue& Value) { return Value.Name == Input.Name; });
					const FString& Text = Written ? Written->Value : Input.Default;
					if (!Text.IsEmpty())
					{
						FDcrValue& Value = Node.Values.AddDefaulted_GetRef();
						Value.Name = Input.Name;
						Value.Value = Text;
					}
				}
			}
			else if (Node.Kind == EDcrNodeKind::Return)
			{
				for (const FDcrModelFunctionPin& Output : Decl.Outputs)
				{
					Node.ResolvedTypes.Emplace(Output.Name, Output.Type);
				}
			}
		}
		Rec.Node.Contained = MoveTemp(Inner);
		if (!bBound)
		{
			return MakeAbsent(Call.Span);
		}
		Sequence(Rec);
		return MakeOutput(Rec, Call.Span);
	}

	// ------------------------------------------------------------------------------------------
	// Injected nodes
	// ------------------------------------------------------------------------------------------

	void FBody::BuildInjected(const FDabCallExpr& Wrapper, FNodeRec& Host, const FPin& HostPin)
	{
		const FDabCallExpr* Inner = (Wrapper.Args.Num() == 1 && Wrapper.Args[0] && !Wrapper.Args[0]->IsNamed() && Wrapper.Args[0]->Value)
			? StripDabParens(*Wrapper.Args[0]->Value).As<FDabCallExpr>() : nullptr;
		if (!Inner)
		{
			Sink.Error(TEXT("DAB9004"), Wrapper.Span, TEXT("`Injected(...)` takes one call, `Injected(Node(...))`."));
			return;
		}
		if (!HostPin.bInput)
		{
			Sink.Error(TEXT("DAB9004"), Wrapper.Span, FString::Printf(TEXT("`%s` is not an input: nothing can be injected on it."), *HostPin.Name));
			return;
		}
		FCallee Callee;
		if (!ResolveCallee(*Inner, Callee))
		{
			return;
		}
		if (Callee.Kind != FCallee::EKind::Unit)
		{
			Sink.Error(TEXT("DAB9004"), Inner->Span, TEXT("Only a unit can be injected on a pin."));
			return;
		}
		const FVal Made = BuildUnit(*Inner, *Callee.Struct, &HostPin.Type, FString());
		FNodeRec* Injected = Made.Rec;
		if (!Injected)
		{
			return;
		}
		if (Injected->Sig.IsMutable())
		{
			Sink.Error(TEXT("DAB9004"), Inner->Span, TEXT("A node with execute pins cannot be injected on a pin."));
			return;
		}

		// The pins the value passes through: one that is both an input and an output, else an input and an output of the pin's type.
		const FPin* InPin = nullptr;
		const FPin* OutPin = nullptr;
		for (const FPin& Pin : Injected->Sig.Pins)
		{
			if (Pin.bInput && Pin.bOutput && Pin.Type == HostPin.Type)
			{
				InPin = OutPin = &Pin;
				break;
			}
		}
		if (!InPin)
		{
			for (const FPin& Pin : Injected->Sig.Pins)
			{
				if (!InPin && Pin.bInput && Pin.Type == HostPin.Type)
				{
					InPin = &Pin;
				}
				if (!OutPin && Pin.bOutput && Pin.Type == HostPin.Type)
				{
					OutPin = &Pin;
				}
			}
		}
		if (!InPin || !OutPin)
		{
			Sink.Error(TEXT("DAB9004"), Inner->Span, FString::Printf(TEXT("`%s` has no input and output of %s for the value to pass through."),
				*GetDcrNodeTypeName(*Callee.Struct), *DescribeTypeOf(HostPin.Type)));
			return;
		}
		Injected->Node.InjectedOn = Host.Node.Name + TEXT(".") + HostPin.Name;
		Injected->Node.bInjectedAsInput = true;
		Injected->Node.InjectedInputPin = InPin->Name;
		Injected->Node.InjectedOutputPin = OutPin->Name;
		AddLink(Injected->Node.Name + TEXT(".") + OutPin->Name, Host.Node.Name + TEXT(".") + HostPin.Name);
	}

	FVal FBody::BuildInjectedOutput(const FDabCallExpr& Wrapper, const FDcrType* Expected, const FString& Preferred)
	{
		const FDabCallExpr* Inner = Wrapper.Args[0]->Value ? StripDabParens(*Wrapper.Args[0]->Value).As<FDabCallExpr>() : nullptr;
		FCallee Callee;
		if (!Inner || !ResolveCallee(*Inner, Callee) || Callee.Kind != FCallee::EKind::Unit)
		{
			Sink.Error(TEXT("DAB9004"), Wrapper.Span, TEXT("`Injected(Node(...), Output)` takes a call of a unit."));
			return MakeAbsent(Wrapper.Span);
		}
		const FVal Made = BuildUnit(*Inner, *Callee.Struct, Expected, Preferred);
		FNodeRec* Injected = Made.Rec;
		if (!Injected)
		{
			return Made;
		}
		if (Injected->Sig.IsMutable())
		{
			Sink.Error(TEXT("DAB9004"), Inner->Span, TEXT("A node with execute pins cannot be injected on a pin."));
			return Made;
		}
		// The output it sits on is what feeds the pin the value passes through: one that is both an input and
		// an output, else the first linked input and an output of its type.
		const FPin* InPin = nullptr;
		const FPin* OutPin = nullptr;
		FString Source;
		auto SourceOf = [this, Injected](const FPin& Pin)
		{
			const FString Target = Injected->Node.Name + TEXT(".") + Pin.Name;
			const FDcrModelLink* Link = Graph.Links.FindByPredicate([&Target](const FDcrModelLink& Candidate) { return Candidate.Target == Target; });
			return Link ? Link->Source : FString();
		};
		for (const FPin& Pin : Injected->Sig.Pins)
		{
			if (Pin.bInput && Pin.bOutput && !SourceOf(Pin).IsEmpty())
			{
				InPin = OutPin = &Pin;
				break;
			}
		}
		for (const FPin& Pin : Injected->Sig.Pins)
		{
			if (!InPin && Pin.bInput && !SourceOf(Pin).IsEmpty())
			{
				InPin = &Pin;
			}
		}
		for (const FPin& Pin : Injected->Sig.Pins)
		{
			if (InPin && !OutPin && Pin.bOutput && Pin.Type == InPin->Type)
			{
				OutPin = &Pin;
			}
		}
		if (!InPin || !OutPin)
		{
			Sink.Error(TEXT("DAB9004"), Inner->Span, TEXT("An output injection reads the output it sits on: `Injected(Node(Other.Pin), Output)`."));
			return Made;
		}
		Injected->Node.InjectedOn = SourceOf(*InPin);
		Injected->Node.bInjectedAsInput = false;
		Injected->Node.InjectedInputPin = InPin->Name;
		Injected->Node.InjectedOutputPin = OutPin->Name;
		return Made;
	}

	// ------------------------------------------------------------------------------------------
	// Operators
	// ------------------------------------------------------------------------------------------

	namespace
	{
		/** A pending or pinless operand cannot be typed: reports it. */
		bool CheckOperand(FDabDiagnosticSink& Sink, const FVal& Value, const FDabExpr& Expr)
		{
			if (Value.Kind == FVal::EKind::Invalid)
			{
				return false;
			}
			if (Value.Kind == FVal::EKind::Void)
			{
				Sink.Error(TEXT("DAB9004"), Expr.Span, TEXT("This gives no value to compute with."));
				return false;
			}
			if (Value.IsPin() && Value.Pin.IsEmpty() && Value.Rec)
			{
				Sink.Error(TEXT("DAB9004"), Expr.Span, FString::Printf(TEXT("`%s` has several outputs; pick one, as `%s.Name`."), *Value.Rec->Node.Name, *Value.Rec->Node.Name));
				return false;
			}
			return true;
		}
	}

	FVal FBody::EvalUnary(const FDabUnaryExpr& Expr, const FDcrType* Expected, const FString& Preferred)
	{
		if (!Expr.Operand)
		{
			return MakeAbsent(Expr.Span);
		}
		FVal Operand = EvalOperand(*Expr.Operand, nullptr, FString());
		if (!CheckOperand(Sink, Operand, *Expr.Operand))
		{
			return MakeAbsent(Expr.Span);
		}
		if (Operand.IsPin() && !SettlePending(Operand, nullptr))
		{
			return MakeAbsent(Expr.Span);
		}
		if (Operand.IsPin() && Operand.Pending != INDEX_NONE)
		{
			Sink.Error(TEXT("DAB9003"), Expr.Operand->Span, TEXT("The type of this operand is not known."), TEXT("Give it a type with a typed let."));
			return MakeAbsent(Expr.Span);
		}
		const FDcrOperator* Operator = GetDcrUnaryOperator(Expr.Op);
		if (!Operator)
		{
			return MakeAbsent(Expr.Span);
		}
		FDcrOperand Known;
		if (Operand.IsPin())
		{
			Known.Type = Operand.Type;
		}
		else
		{
			Known.Literal = Operand.Expr;
			Known.Fits = [this, Literal = Operand.Expr](const FDcrType& Type) { return Literal && FitsType(*Literal, Type); };
		}
		const TArray<const FDcrOperator*> Candidates = { Operator };
		const TArray<FDcrOperand> Operands = { Known };
		const FDcrOperatorChoice Choice = ResolveDcrOperatorUnit(Candidates, Operands, Expected);
		if (!Choice.Unit)
		{
			Sink.Error(TEXT("DAB9004"), Expr.Span, FString::Printf(TEXT("`%s` does not take %s."), GetDabUnaryOpSpelling(Expr.Op), Operand.IsPin() ? *DescribeTypeOf(Operand.Type) : TEXT("this value")));
			return MakeAbsent(Expr.Span);
		}
		TArray<TPair<FString, FVal>> Args;
		Args.Emplace(FString(Choice.Operator->Operands[0]), Operand);
		return BuildUnitFrom(*Choice.Unit, Args, Preferred, Expr.Span);
	}

	FVal FBody::EvalBinary(const FDabBinaryExpr& Expr, const FDcrType* Expected, const FString& Preferred)
	{
		if (!Expr.Lhs || !Expr.Rhs)
		{
			return MakeAbsent(Expr.Span);
		}
		FVal Lhs = EvalOperand(*Expr.Lhs, nullptr, FString());
		FVal Rhs = EvalOperand(*Expr.Rhs, nullptr, FString());
		const bool bLhsOk = CheckOperand(Sink, Lhs, *Expr.Lhs);
		const bool bRhsOk = CheckOperand(Sink, Rhs, *Expr.Rhs);
		if (!bLhsOk || !bRhsOk)
		{
			return MakeAbsent(Expr.Span);
		}
		for (FVal* Operand : { &Lhs, &Rhs })
		{
			if (Operand->IsPin())
			{
				if (!SettlePending(*Operand, nullptr))
				{
					return MakeAbsent(Expr.Span);
				}
				if (Operand->Pending != INDEX_NONE)
				{
					Sink.Error(TEXT("DAB9003"), Operand->Span, TEXT("The type of this operand is not known."), TEXT("Give it a type with a typed let."));
					return MakeAbsent(Expr.Span);
				}
			}
		}

		const TArray<const FDcrOperator*> Candidates = GetDcrBinaryOperators(Expr.Op);
		if (Candidates.IsEmpty())
		{
			return MakeAbsent(Expr.Span);
		}
		if (Candidates[0]->bDispatch)
		{
			const UScriptStruct* Struct = FindDispatchByFullName(Candidates[0]->NodeName);
			if (!Struct)
			{
				Sink.Error(TEXT("DAB9001"), Expr.Span, FString::Printf(TEXT("The engine has no `%s` node for `%s`."), Candidates[0]->NodeName, GetDabBinaryOpSpelling(Expr.Op)));
				return MakeAbsent(Expr.Span);
			}
			TArray<TPair<FString, FVal>> Args;
			Args.Emplace(FString(Candidates[0]->Operands[0]), Lhs);
			Args.Emplace(FString(Candidates[0]->Operands[1]), Rhs);
			return BuildDispatchFrom(*Struct, MoveTemp(Args), nullptr, Preferred, Expr.Span);
		}

		TArray<FDcrOperand> Operands;
		for (const FVal* Operand : { &Lhs, &Rhs })
		{
			FDcrOperand& Known = Operands.AddDefaulted_GetRef();
			if (Operand->IsPin())
			{
				Known.Type = Operand->Type;
			}
			else
			{
				Known.Literal = Operand->Expr;
				Known.Fits = [this, Literal = Operand->Expr](const FDcrType& Type) { return Literal && FitsType(*Literal, Type); };
			}
		}
		const FDcrOperatorChoice Choice = ResolveDcrOperatorUnit(Candidates, Operands, Expected);
		if (!Choice.Unit)
		{
			Sink.Error(TEXT("DAB9004"), Expr.OpSpan, FString::Printf(TEXT("`%s` does not take %s and %s."), GetDabBinaryOpSpelling(Expr.Op),
				Lhs.IsPin() ? *DescribeTypeOf(Lhs.Type) : TEXT("this value"), Rhs.IsPin() ? *DescribeTypeOf(Rhs.Type) : TEXT("this value")),
				TEXT("Both sides must be the same type; a vector times a number is the one mix."));
			return MakeAbsent(Expr.Span);
		}
		TArray<TPair<FString, FVal>> Args;
		Args.Emplace(FString(Choice.Operator->Operands[0]), Lhs);
		Args.Emplace(FString(Choice.Operator->Operands[1]), Rhs);
		return BuildUnitFrom(*Choice.Unit, Args, Preferred, Expr.Span);
	}

	FVal FBody::EvalConditional(const FDabConditionalExpr& Expr, const FDcrType* Expected, const FString& Preferred)
	{
		if (!Expr.Condition || !Expr.Then || !Expr.Else)
		{
			return MakeAbsent(Expr.Span);
		}
		const UScriptStruct* Struct = FindDispatchByFullName(GetDcrConditionalOperator().NodeName);
		if (!Struct)
		{
			Sink.Error(TEXT("DAB9001"), Expr.Span, TEXT("The engine has no If node."));
			return MakeAbsent(Expr.Span);
		}
		const FDcrType BoolType = MakeDcrBuiltinType(TEXT("bool"));
		TArray<TPair<FString, FVal>> Args;
		Args.Emplace(TEXT("Condition"), EvalOperand(*Expr.Condition, &BoolType, FString()));
		Args.Emplace(TEXT("True"), EvalOperand(*Expr.Then, Expected, FString()));
		Args.Emplace(TEXT("False"), EvalOperand(*Expr.Else, Expected, FString()));
		return BuildDispatchFrom(*Struct, MoveTemp(Args), Expected, Preferred, Expr.Span);
	}

	FVal FBody::EvalIndex(const FDabIndexExpr& Expr, const FDcrType* Expected, const FString& Preferred)
	{
		if (!Expr.Object || !Expr.Index)
		{
			return MakeAbsent(Expr.Span);
		}
		const UScriptStruct* Struct = FindDispatchByFullName(GetDcrIndexOperator().NodeName);
		if (!Struct)
		{
			Sink.Error(TEXT("DAB9001"), Expr.Span, TEXT("The engine has no ArrayGetAtIndex node."));
			return MakeAbsent(Expr.Span);
		}
		const FDcrType IndexType = MakeDcrBuiltinType(TEXT("int32"));
		TArray<TPair<FString, FVal>> Args;
		Args.Emplace(TEXT("Array"), EvalOperand(*Expr.Object, nullptr, FString()));
		Args.Emplace(TEXT("Index"), EvalOperand(*Expr.Index, &IndexType, FString()));
		return BuildDispatchFrom(*Struct, MoveTemp(Args), Expected, Preferred, Expr.Span);
	}
}
