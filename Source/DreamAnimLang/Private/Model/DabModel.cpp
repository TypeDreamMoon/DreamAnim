// Copyright DreamMoon. All Rights Reserved.

#include "Model/DabModel.h"

#include "Lang/DabPrinter.h"
#include "Misc/CString.h"

namespace UE::DreamAnim::Model
{
	FString FormatDabNumber(double Value)
	{
		if (FMath::IsNaN(Value))
		{
			return TEXT("nan");
		}
		if (!FMath::IsFinite(Value))
		{
			return Value < 0.0 ? TEXT("-inf") : TEXT("inf");
		}
		if (Value == 0.0)
		{
			// Also folds -0 into 0: the two are the same value to every consumer of a model.
			return TEXT("0");
		}

		// Whole numbers print as integers while every digit is exact, so 100 is `100` and not `1e+02`.
		if (FMath::Abs(Value) < 1e15)
		{
			const int64 Whole = static_cast<int64>(Value);
			if (static_cast<double>(Whole) == Value)
			{
				return FString::Printf(TEXT("%lld"), Whole);
			}
		}

		// The first precision that reads back as the same double is the shortest spelling of it.
		for (int32 Precision = 1; Precision <= 17; ++Precision)
		{
			FString Text = FString::Printf(TEXT("%.*g"), Precision, Value);
			if (FCString::Atod(*Text) == Value)
			{
				return Text;
			}
		}
		return FString::Printf(TEXT("%.17g"), Value);
	}

	const FDabModelNode* FDabModelGraph::FindNode(const FString& Id) const
	{
		for (const FDabModelNode& Node : Nodes)
		{
			if (Node.Id.Equals(Id, ESearchCase::CaseSensitive))
			{
				return &Node;
			}
		}
		return nullptr;
	}

	const FDabModelStateMachine* FDabModel::FindStateMachine(const FString& Name) const
	{
		for (const FDabModelStateMachine& Machine : StateMachines)
		{
			if (Machine.Name.Equals(Name, ESearchCase::CaseSensitive))
			{
				return &Machine;
			}
		}
		return nullptr;
	}

	namespace
	{
		// FString's operator< ignores case; a canonical dump must not depend on it.
		bool LessText(const FString& A, const FString& B)
		{
			return A.Compare(B, ESearchCase::CaseSensitive) < 0;
		}

		FString PrintValue(const Lang::FDabExprPtr& Expr)
		{
			if (!Expr.IsValid())
			{
				return TEXT("<null>");
			}

			// One line however long the value is, so a dump line never breaks.
			Lang::FDabPrintOptions Options;
			Options.MaxWidth = 1 << 24;
			FString Text = Lang::PrintDabExpr(*Expr, 0, 0, nullptr, Options);
			Text.ReplaceInline(TEXT("\r\n"), TEXT(" "), ESearchCase::CaseSensitive);
			Text.ReplaceInline(TEXT("\n"), TEXT(" "), ESearchCase::CaseSensitive);
			Text.ReplaceInline(TEXT("\r"), TEXT(" "), ESearchCase::CaseSensitive);
			return Text;
		}

		FString PrintCall(const FDabModelCall& Call)
		{
			FString Text;
			if (Call.Target.IsValid())
			{
				Text = PrintValue(Call.Target);
				Text += TEXT(".");
			}
			Text += Call.Function;
			Text += TEXT("(");
			for (int32 Index = 0; Index < Call.Args.Num(); ++Index)
			{
				const FDabModelArgument& Argument = Call.Args[Index];
				if (Index > 0)
				{
					Text += TEXT(", ");
				}
				if (!Argument.Name.IsEmpty())
				{
					Text += Argument.Name;
					Text += TEXT(" = ");
				}
				Text += PrintValue(Argument.Value);
			}
			Text += TEXT(")");
			return Text;
		}

		void Line(FString& Out, int32 Depth, const FString& Text)
		{
			for (int32 Level = 0; Level < Depth; ++Level)
			{
				Out += TEXT("  ");
			}
			Out += Text;
			Out += TEXT("\n");
		}

		/** `Word` alone when Value is empty, so no line ends in a space. */
		FString Labelled(const TCHAR* Word, const FString& Value)
		{
			return Value.IsEmpty() ? FString(Word) : FString::Printf(TEXT("%s %s"), Word, *Value);
		}

		/** Appends a block rendered at depth 0, indented to Depth. */
		void AppendIndented(FString& Out, const FString& Block, int32 Depth)
		{
			TArray<FString> Lines;
			Block.ParseIntoArray(Lines, TEXT("\n"), false);
			for (const FString& Text : Lines)
			{
				if (!Text.IsEmpty())
				{
					Line(Out, Depth, Text);
				}
			}
		}

		void DumpProperties(FString& Out, int32 Depth, const TCHAR* Word, const TArray<FDabModelProperty>& Properties)
		{
			TArray<const FDabModelProperty*> Sorted;
			for (const FDabModelProperty& Property : Properties)
			{
				Sorted.Add(&Property);
			}
			Sorted.StableSort([](const FDabModelProperty& A, const FDabModelProperty& B) { return LessText(A.Name, B.Name); });
			for (const FDabModelProperty* Property : Sorted)
			{
				Line(Out, Depth, FString::Printf(TEXT("%s %s = %s"), Word, *Property->Name, *PrintValue(Property->Value)));
			}
		}

		// -- graphs ---------------------------------------------------------------------------

		struct FGraphDumper
		{
			const FDabModelGraph& Graph;
			TSet<FString> Reached;
			TArray<FString> Stack;

			void DumpNode(FString& Out, int32 Depth, const FDabModelNode& Node)
			{
				Reached.Add(Node.Id);

				if (Node.ClassName == TEXT("AnimGraphNode_UseCachedPose"))
				{
					Line(Out, Depth, Labelled(TEXT("UseCache"), Node.CacheName));
					return;
				}

				Line(Out, Depth, Labelled(TEXT("Node"), Node.ClassName));
				DumpProperties(Out, Depth + 1, TEXT("Property"), Node.Properties);

				TArray<const FDabModelBinding*> Bindings;
				for (const FDabModelBinding& Binding : Node.Bindings)
				{
					Bindings.Add(&Binding);
				}
				Bindings.StableSort([](const FDabModelBinding& A, const FDabModelBinding& B) { return LessText(A.PinName, B.PinName); });
				for (const FDabModelBinding* Binding : Bindings)
				{
					Line(Out, Depth + 1, FString::Printf(TEXT("Bind %s = %s"), *Binding->PinName, *PrintValue(Binding->Value)));
				}

				TArray<const FDabModelNodeFunction*> Functions;
				for (const FDabModelNodeFunction& Function : Node.NodeFunctions)
				{
					Functions.Add(&Function);
				}
				Functions.StableSort([](const FDabModelNodeFunction& A, const FDabModelNodeFunction& B) { return LessText(A.Site, B.Site); });
				for (const FDabModelNodeFunction* Function : Functions)
				{
					Line(Out, Depth + 1, FString::Printf(TEXT("Function %s = %s"), *Function->Site, *PrintCall(Function->Call)));
				}

				if (!Node.CacheName.IsEmpty())
				{
					Line(Out, Depth + 1, Labelled(TEXT("Cache"), Node.CacheName));
				}
				if (!Node.StateMachineName.IsEmpty())
				{
					Line(Out, Depth + 1, Labelled(TEXT("Machine"), Node.StateMachineName));
				}
				if (!Node.InputPoseName.IsEmpty())
				{
					Line(Out, Depth + 1, Labelled(TEXT("InputPose"), Node.InputPoseName));
				}
				if (Node.Call.IsSet())
				{
					Line(Out, Depth + 1, FString::Printf(TEXT("Call %s"), *PrintCall(Node.Call.GetValue())));
				}

				TArray<const FDabModelPoseLink*> Links;
				for (const FDabModelPoseLink& Link : Node.PoseInputs)
				{
					Links.Add(&Link);
				}
				Links.StableSort([](const FDabModelPoseLink& A, const FDabModelPoseLink& B) { return LessText(A.PinName, B.PinName); });

				Stack.Push(Node.Id);
				for (const FDabModelPoseLink* Link : Links)
				{
					Line(Out, Depth + 1, Labelled(TEXT("Pose"), Link->PinName));
					if (Link->SourceNodeId.IsEmpty())
					{
						// An unlinked pin of a pose array: listed so the array keeps its size.
						continue;
					}
					const FDabModelNode* Source = Graph.FindNode(Link->SourceNodeId);
					if (!Source)
					{
						Line(Out, Depth + 2, Labelled(TEXT("Missing"), Link->SourceNodeId));
					}
					else if (Stack.Contains(Source->Id))
					{
						Line(Out, Depth + 2, Labelled(TEXT("Cycle"), Source->Id));
					}
					else
					{
						DumpNode(Out, Depth + 2, *Source);
					}
				}
				Stack.Pop();
			}

			FString DumpTree(const FDabModelNode& Root)
			{
				FString Block;
				DumpNode(Block, 0, Root);
				return Block;
			}
		};

		void DumpGraph(FString& Out, int32 Depth, const FDabModelGraph& Graph)
		{
			if (!Graph.InputPoses.IsEmpty())
			{
				// In order: the order is what a linked graph node relies on.
				Line(Out, Depth, TEXT("Inputs [") + FString::Join(Graph.InputPoses, TEXT(", ")) + TEXT("]"));
			}
			DumpProperties(Out, Depth, TEXT("ResultProperty"), Graph.ResultProperties);

			FGraphDumper Dumper{Graph};

			Line(Out, Depth, TEXT("Result"));
			if (!Graph.ResultNodeId.IsEmpty())
			{
				if (const FDabModelNode* Root = Graph.FindNode(Graph.ResultNodeId))
				{
					AppendIndented(Out, Dumper.DumpTree(*Root), Depth + 1);
				}
				else
				{
					Line(Out, Depth + 1, Labelled(TEXT("Missing"), Graph.ResultNodeId));
				}
			}

			// Caches are roots of their own: nothing links to a SaveCachedPose node.
			TArray<const FDabModelNode*> Caches;
			for (const FDabModelNode& Node : Graph.Nodes)
			{
				if (Node.ClassName == TEXT("AnimGraphNode_SaveCachedPose"))
				{
					Caches.Add(&Node);
				}
			}
			Caches.StableSort([](const FDabModelNode& A, const FDabModelNode& B)
			{
				const int32 ByName = A.CacheName.Compare(B.CacheName, ESearchCase::CaseSensitive);
				return ByName != 0 ? ByName < 0 : LessText(A.Id, B.Id);
			});
			for (const FDabModelNode* Cache : Caches)
			{
				Line(Out, Depth, Labelled(TEXT("Cache"), Cache->CacheName));
				AppendIndented(Out, Dumper.DumpTree(*Cache), Depth + 1);
			}

			// What is left: nodes nothing reached. Each block starts at a node no other leftover feeds, so a
			// dangling chain prints once; members of a pure cycle, which have no such start, follow alone.
			TSet<FString> Fed;
			for (const FDabModelNode& Node : Graph.Nodes)
			{
				if (!Dumper.Reached.Contains(Node.Id))
				{
					for (const FDabModelPoseLink& Link : Node.PoseInputs)
					{
						Fed.Add(Link.SourceNodeId);
					}
				}
			}

			TArray<FString> Blocks;
			for (const FDabModelNode& Node : Graph.Nodes)
			{
				if (!Dumper.Reached.Contains(Node.Id) && !Fed.Contains(Node.Id))
				{
					Blocks.Add(Dumper.DumpTree(Node));
				}
			}
			for (const FDabModelNode& Node : Graph.Nodes)
			{
				if (!Dumper.Reached.Contains(Node.Id))
				{
					Blocks.Add(Dumper.DumpTree(Node));
				}
			}
			Blocks.StableSort(LessText);
			for (const FString& Block : Blocks)
			{
				Line(Out, Depth, TEXT("Unreached"));
				AppendIndented(Out, Block, Depth + 1);
			}
		}

		// -- state machines ---------------------------------------------------------------------

		FString DumpTransition(const FDabModelTransition& Transition)
		{
			FString Block;
			Line(Block, 0, FString::Printf(TEXT("Transition %s -> %s"), *Transition.From, *Transition.To));
			if (Transition.ConditionKind == EDabConditionKind::Auto)
			{
				Line(Block, 1, TEXT("Auto"));
			}
			else
			{
				Line(Block, 1, FString::Printf(TEXT("Condition %s"), Transition.Condition.IsValid() ? *PrintValue(Transition.Condition) : TEXT("true")));
			}
			if (!Transition.SharedRuleName.IsEmpty())
			{
				Line(Block, 1, Labelled(TEXT("SharedRule"), Transition.SharedRuleName));
			}
			DumpProperties(Block, 1, TEXT("Option"), Transition.Options);
			return Block;
		}

		void DumpMachine(FString& Out, int32 Depth, const FDabModelStateMachine& Machine)
		{
			Line(Out, Depth, Labelled(TEXT("Machine"), Machine.Name));
			DumpProperties(Out, Depth + 1, TEXT("Property"), Machine.Properties);
			Line(Out, Depth + 1, Labelled(TEXT("Entry"), Machine.Entry));

			TArray<const FDabModelState*> States;
			for (const FDabModelState& State : Machine.States)
			{
				States.Add(&State);
			}
			States.StableSort([](const FDabModelState& A, const FDabModelState& B) { return LessText(A.Name, B.Name); });
			for (const FDabModelState* State : States)
			{
				Line(Out, Depth + 1, Labelled(TEXT("State"), State->Name));
				DumpProperties(Out, Depth + 2, TEXT("Property"), State->Properties);
				if (State->OnEnter.IsSet())
				{
					Line(Out, Depth + 2, FString::Printf(TEXT("OnEnter %s"), *PrintCall(State->OnEnter.GetValue())));
				}
				if (State->OnExit.IsSet())
				{
					Line(Out, Depth + 2, FString::Printf(TEXT("OnExit %s"), *PrintCall(State->OnExit.GetValue())));
				}
				if (State->OnBlended.IsSet())
				{
					Line(Out, Depth + 2, FString::Printf(TEXT("OnBlended %s"), *PrintCall(State->OnBlended.GetValue())));
				}
				DumpGraph(Out, Depth + 2, State->Graph);
			}

			TArray<const FDabModelConduit*> Conduits;
			for (const FDabModelConduit& Conduit : Machine.Conduits)
			{
				Conduits.Add(&Conduit);
			}
			Conduits.StableSort([](const FDabModelConduit& A, const FDabModelConduit& B) { return LessText(A.Name, B.Name); });
			for (const FDabModelConduit* Conduit : Conduits)
			{
				Line(Out, Depth + 1, FString::Printf(TEXT("Conduit %s = %s"), *Conduit->Name,
					Conduit->Condition.IsValid() ? *PrintValue(Conduit->Condition) : TEXT("true")));
			}

			TArray<const FDabModelAlias*> Aliases;
			for (const FDabModelAlias& Alias : Machine.Aliases)
			{
				Aliases.Add(&Alias);
			}
			Aliases.StableSort([](const FDabModelAlias& A, const FDabModelAlias& B) { return LessText(A.Name, B.Name); });
			for (const FDabModelAlias* Alias : Aliases)
			{
				const FString Members = Alias->bGlobal
					? FString(TEXT("All"))
					: FString::Printf(TEXT("[%s]"), *FString::Join(Alias->States, TEXT(", ")));
				Line(Out, Depth + 1, FString::Printf(TEXT("Alias %s = %s"), *Alias->Name, *Members));
			}

			struct FTransitionBlock
			{
				const FDabModelTransition* Transition;
				FString Text;
			};
			TArray<FTransitionBlock> Transitions;
			for (const FDabModelTransition& Transition : Machine.Transitions)
			{
				Transitions.Add({&Transition, DumpTransition(Transition)});
			}
			Transitions.StableSort([](const FTransitionBlock& A, const FTransitionBlock& B)
			{
				const int32 ByFrom = A.Transition->From.Compare(B.Transition->From, ESearchCase::CaseSensitive);
				if (ByFrom != 0)
				{
					return ByFrom < 0;
				}
				const int32 ByTo = A.Transition->To.Compare(B.Transition->To, ESearchCase::CaseSensitive);
				if (ByTo != 0)
				{
					return ByTo < 0;
				}
				return LessText(A.Text, B.Text);
			});
			for (const FTransitionBlock& Block : Transitions)
			{
				AppendIndented(Out, Block.Text, Depth + 1);
			}
		}
	}

	FString DumpDabModel(const FDabModel& Model)
	{
		FString Out;
		Line(Out, 0, FString::Printf(TEXT("Model %s %s"), Lang::LexToString(Model.Kind), *Model.AssetPath).TrimEnd());
		Line(Out, 1, Labelled(TEXT("Parent"), Model.ParentClass));
		Line(Out, 1, Labelled(TEXT("Skeleton"), Model.Skeleton));

		TArray<FString> Interfaces = Model.Interfaces;
		Interfaces.StableSort(LessText);
		for (const FString& Interface : Interfaces)
		{
			Line(Out, 1, Labelled(TEXT("Interface"), Interface));
		}

		if (!Model.DefaultAnimSet.IsEmpty())
		{
			Line(Out, 1, Labelled(TEXT("DefaultAnimSet"), Model.DefaultAnimSet));
		}

		DumpProperties(Out, 1, TEXT("Setting"), Model.ClassSettings);
		DumpProperties(Out, 1, TEXT("Default"), Model.ClassDefaults);

		TArray<const FDabModelVariable*> Variables;
		for (const FDabModelVariable& Variable : Model.Variables)
		{
			Variables.Add(&Variable);
		}
		Variables.StableSort([](const FDabModelVariable& A, const FDabModelVariable& B) { return LessText(A.Name, B.Name); });
		for (const FDabModelVariable* Variable : Variables)
		{
			FString Text = FString::Printf(TEXT("Variable %s %s"), *Variable->Type, *Variable->Name);
			if (Variable->Default.IsValid())
			{
				Text += FString::Printf(TEXT(" = %s"), *PrintValue(Variable->Default));
			}
			Line(Out, 1, Text);
			DumpProperties(Out, 2, TEXT("Meta"), Variable->Meta);
		}

		TArray<const FDabModelAssetSlot*> Slots;
		for (const FDabModelAssetSlot& Slot : Model.AssetSlots)
		{
			Slots.Add(&Slot);
		}
		Slots.StableSort([](const FDabModelAssetSlot& A, const FDabModelAssetSlot& B) { return LessText(A.Name, B.Name); });
		for (const FDabModelAssetSlot* Slot : Slots)
		{
			Line(Out, 1, FString::Printf(TEXT("Slot %s %s"), *Slot->Type, *Slot->Name));
		}

		TArray<const FDabModelNotify*> Notifies;
		for (const FDabModelNotify& Notify : Model.Notifies)
		{
			Notifies.Add(&Notify);
		}
		Notifies.StableSort([](const FDabModelNotify& A, const FDabModelNotify& B) { return LessText(A.Name, B.Name); });
		for (const FDabModelNotify* Notify : Notifies)
		{
			Line(Out, 1, Labelled(TEXT("Notify"), Notify->Name));
			for (const FDabModelCall& Call : Notify->Calls)
			{
				Line(Out, 2, FString::Printf(TEXT("Call %s"), *PrintCall(Call)));
			}
		}

		Line(Out, 1, TEXT("Graph AnimGraph"));
		DumpGraph(Out, 2, Model.AnimGraph);

		TArray<const FDabModelLayer*> Layers;
		for (const FDabModelLayer& Layer : Model.Layers)
		{
			Layers.Add(&Layer);
		}
		Layers.StableSort([](const FDabModelLayer& A, const FDabModelLayer& B) { return LessText(A.Name, B.Name); });
		for (const FDabModelLayer* Layer : Layers)
		{
			if (Layer->bDeclarationOnly)
			{
				// A declaration has no graph, only what its root says (LayerGroup).
				Line(Out, 1, FString::Printf(TEXT("Layer %s Declared"), *Layer->Name));
				DumpProperties(Out, 2, TEXT("ResultProperty"), Layer->Graph.ResultProperties);
			}
			else
			{
				Line(Out, 1, Labelled(TEXT("Layer"), Layer->Name));
				DumpGraph(Out, 2, Layer->Graph);
			}
		}

		TArray<const FDabModelStateMachine*> Machines;
		for (const FDabModelStateMachine& Machine : Model.StateMachines)
		{
			Machines.Add(&Machine);
		}
		Machines.StableSort([](const FDabModelStateMachine& A, const FDabModelStateMachine& B) { return LessText(A.Name, B.Name); });
		for (const FDabModelStateMachine* Machine : Machines)
		{
			DumpMachine(Out, 1, *Machine);
		}

		TArray<FString> Unsupported;
		for (const FDabModelUnsupported& Entry : Model.Unsupported)
		{
			Unsupported.Add(FString::Printf(TEXT("Unsupported %s: %s"), *Entry.Location, *Entry.Description));
		}
		Unsupported.StableSort(LessText);
		for (const FString& Text : Unsupported)
		{
			Line(Out, 1, Text);
		}

		return Out;
	}
}
