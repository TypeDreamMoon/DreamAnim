// Copyright DreamMoon. All Rights Reserved.
//
// NormalizeDabModelNames: Docs/Mapping.md, lowering rule 7. Engine names may hold spaces and
// punctuation ("Rotate Left", "(N) RB Movement", "View Input"); the language's names are identifiers.

#include "Lower/DabLower.h"

namespace UE::DreamAnim::Compiler::LowerNamesPrivate
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	bool IsIdentifierStart(TCHAR C)
	{
		return C == TEXT('_') || FChar::IsAlpha(C) || C >= 0x80;
	}

	bool IsIdentifierPart(TCHAR C)
	{
		return IsIdentifierStart(C) || FChar::IsDigit(C);
	}

	/** Drops what an identifier cannot hold, upper-casing the letter after each dropped run. */
	FString SanitizeName(const FString& Name)
	{
		FString Result;
		bool bUpperNext = false;
		for (const TCHAR C : Name)
		{
			if (FChar::IsWhitespace(C) || !IsIdentifierPart(C))
			{
				bUpperNext = !Result.IsEmpty();
				continue;
			}
			Result.AppendChar(bUpperNext ? FChar::ToUpper(C) : C);
			bUpperNext = false;
		}
		if (Result.IsEmpty())
		{
			Result = TEXT("Unnamed");
		}
		if (!IsIdentifierStart(Result[0]))
		{
			Result = TEXT("_") + Result;
		}
		return Result;
	}

	/** One naming scope: names that must stay distinct from each other after sanitising. */
	struct FScope
	{
		TMap<FString, FString> Renames;
		TSet<FString> Taken;

		const FString& Get(const FString& Original)
		{
			if (const FString* Existing = Renames.Find(Original))
			{
				return *Existing;
			}
			FString Base = SanitizeName(Original);
			FString Candidate = Base;
			for (int32 Suffix = 2; Taken.Contains(Candidate); ++Suffix)
			{
				Candidate = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
			}
			Taken.Add(Candidate);
			return Renames.Add(Original, MoveTemp(Candidate));
		}

		/** The new name if the original was registered, else the sanitised form (for names seen only in references). */
		FString Lookup(const FString& Original) const
		{
			const FString* Found = Renames.Find(Original);
			return Found ? *Found : SanitizeName(Original);
		}
	};

	/** State and machine names appear as bare names inside the getters StateWeight(S) and MachineWeight(M). */
	void RenameGetterArguments(FDabExpr* Expr, const FScope& States, const FScope& Machines)
	{
		if (!Expr)
		{
			return;
		}
		if (FDabCallExpr* Call = Expr->As<FDabCallExpr>())
		{
			const FDabNameExpr* Callee = Call->Callee ? Call->Callee->As<FDabNameExpr>() : nullptr;
			if (Callee && Call->Args.Num() == 1 && Call->Args[0]->Value)
			{
				if (FDabNameExpr* Arg = Call->Args[0]->Value->As<FDabNameExpr>())
				{
					if (Callee->Name == TEXT("StateWeight"))
					{
						Arg->Name = States.Lookup(Arg->Name);
					}
					else if (Callee->Name == TEXT("MachineWeight"))
					{
						Arg->Name = Machines.Lookup(Arg->Name);
					}
				}
			}
		}
		ForEachChild(*Expr, [&States, &Machines](const FDabNode& Child)
		{
			if (const FDabExpr* ChildExpr = static_cast<const FDabExpr*>(&Child); Child.Kind <= EDabNodeKind::ParenExpr)
			{
				RenameGetterArguments(const_cast<FDabExpr*>(ChildExpr), States, Machines);
			}
			else if (const FDabArgument* Argument = Child.As<FDabArgument>())
			{
				RenameGetterArguments(Argument->Value.Get(), States, Machines);
			}
		});
	}

	void RenameGraph(FDabModelGraph& Graph, FScope& Caches, const FScope& Machines, const FScope& States, const FScope& Layers)
	{
		for (FDabModelNode& Node : Graph.Nodes)
		{
			// A linked layer names a layer of this Blueprint by string; follow the layer's new name. Layers of
			// other Blueprints' interfaces are left alone.
			if (Node.ClassName == TEXT("AnimGraphNode_LinkedAnimLayer"))
			{
				for (FDabModelProperty& Property : Node.Properties)
				{
					FDabLiteralExpr* Literal = Property.Name == TEXT("Layer") && Property.Value ? Property.Value->As<FDabLiteralExpr>() : nullptr;
					if (Literal && Literal->LiteralKind == EDabLiteralKind::String && Layers.Renames.Contains(Literal->Text))
					{
						Literal->Text = Layers.Lookup(Literal->Text);
					}
				}
			}
			if (!Node.CacheName.IsEmpty())
			{
				Node.CacheName = Caches.Get(Node.CacheName);
			}
			if (!Node.StateMachineName.IsEmpty())
			{
				Node.StateMachineName = Machines.Lookup(Node.StateMachineName);
			}
			for (FDabModelBinding& Binding : Node.Bindings)
			{
				RenameGetterArguments(Binding.Value.Get(), States, Machines);
			}
		}
	}
}

namespace UE::DreamAnim::Compiler::LowerNamesPrivate
{
	void NormalizeNames(FDabModel& Model, TMap<FString, FString>* OutRenames)
	{
		FScope Machines;
		for (FDabModelStateMachine& Machine : Model.StateMachines)
		{
			Machines.Get(Machine.Name);
		}
		FScope Caches;
		FScope Layers;
		for (FDabModelLayer& Layer : Model.Layers)
		{
			Layer.Name = Layers.Get(Layer.Name);
		}
		// In the text a bare name means a named pose or a machine, so a cache saved under its machine's
		// name (`LookStates` saving `LookStates`) gets another one.
		for (const TPair<FString, FString>& Machine : Machines.Renames)
		{
			Caches.Taken.Add(Machine.Value);
		}

		// Every state name of every machine, for the getters; per-machine scopes below keep names distinct.
		FScope AllStates;
		for (FDabModelStateMachine& Machine : Model.StateMachines)
		{
			FScope MachineStates;
			for (FDabModelState& State : Machine.States)
			{
				const FString NewName = MachineStates.Get(State.Name);
				AllStates.Renames.Add(State.Name, NewName);
			}
			for (FDabModelConduit& Conduit : Machine.Conduits)
			{
				const FString NewName = MachineStates.Get(Conduit.Name);
				AllStates.Renames.Add(Conduit.Name, NewName);
			}
			for (FDabModelAlias& Alias : Machine.Aliases)
			{
				MachineStates.Get(Alias.Name);
			}

			Machine.Name = Machines.Lookup(Machine.Name);
			Machine.Entry = MachineStates.Lookup(Machine.Entry);
			for (FDabModelState& State : Machine.States)
			{
				State.Name = MachineStates.Lookup(State.Name);
			}
			for (FDabModelConduit& Conduit : Machine.Conduits)
			{
				Conduit.Name = MachineStates.Lookup(Conduit.Name);
			}
			for (FDabModelAlias& Alias : Machine.Aliases)
			{
				Alias.Name = MachineStates.Lookup(Alias.Name);
				for (FString& Aliased : Alias.States)
				{
					Aliased = MachineStates.Lookup(Aliased);
				}
			}
			FScope Rules;
			for (FDabModelTransition& Transition : Machine.Transitions)
			{
				Transition.From = MachineStates.Lookup(Transition.From);
				Transition.To = MachineStates.Lookup(Transition.To);
				if (!Transition.SharedRuleName.IsEmpty())
				{
					Transition.SharedRuleName = Rules.Get(Transition.SharedRuleName);
				}
			}
			if (OutRenames)
			{
				for (const TPair<FString, FString>& Pair : MachineStates.Renames)
				{
					if (Pair.Key != Pair.Value)
					{
						OutRenames->Add(Pair.Key, Pair.Value);
					}
				}
			}
		}

		RenameGraph(Model.AnimGraph, Caches, Machines, AllStates, Layers);
		for (FDabModelLayer& Layer : Model.Layers)
		{
			RenameGraph(Layer.Graph, Caches, Machines, AllStates, Layers);
		}
		for (FDabModelStateMachine& Machine : Model.StateMachines)
		{
			for (FDabModelState& State : Machine.States)
			{
				RenameGraph(State.Graph, Caches, Machines, AllStates, Layers);
			}
			for (FDabModelConduit& Conduit : Machine.Conduits)
			{
				RenameGetterArguments(Conduit.Condition.Get(), AllStates, Machines);
			}
			for (FDabModelTransition& Transition : Machine.Transitions)
			{
				RenameGetterArguments(Transition.Condition.Get(), AllStates, Machines);
			}
		}

		if (OutRenames)
		{
			for (const FScope* Scope : { &Machines, &Caches, &Layers })
			{
				for (const TPair<FString, FString>& Pair : Scope->Renames)
				{
					if (Pair.Key != Pair.Value)
					{
						OutRenames->Add(Pair.Key, Pair.Value);
					}
				}
			}
		}
	}
}

namespace UE::DreamAnim::Compiler
{
	FString MakeDabIdentifier(const FString& Name)
	{
		return LowerNamesPrivate::SanitizeName(Name);
	}

	void NormalizeDabModelNames(Model::FDabModel& Model, TMap<FString, FString>* OutRenames)
	{
		LowerNamesPrivate::NormalizeNames(Model, OutRenames);
	}
}
