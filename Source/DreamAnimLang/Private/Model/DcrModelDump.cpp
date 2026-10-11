// Copyright DreamMoon. All Rights Reserved.
//
// DumpDcrModel: the canonical text of a model.
//
// What the text leaves out is what two rigs that behave the same can differ in: node positions and comment
// sizes, the asset's path and kind, the diagnostics in Unsupported, the execute pin lists (they follow
// from the node's kind; the dump only uses them to walk execution) and the template notation of a unit (a
// unit is its struct; whether the engine recorded a notation on it depends on how the node was added, and
// it changes nothing at run time). Everything that is a set -- settings,
// properties, values, links -- is sorted. Nodes come in the order they are reached from the graph's events
// and entry, so renaming nodes does not change the text.

#include "Model/DcrModel.h"

namespace UE::DreamAnim::Model
{
	namespace
	{
		// FString's operator< ignores case; a canonical dump must not depend on it.
		int32 CompareText(const FString& A, const FString& B)
		{
			return A.Compare(B, ESearchCase::CaseSensitive);
		}

		bool LessText(const FString& A, const FString& B)
		{
			return CompareText(A, B) < 0;
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

		/** One line however long the text is. */
		FString OneLine(const FString& Text)
		{
			FString Result = Text.Replace(TEXT("\\"), TEXT("\\\\"), ESearchCase::CaseSensitive);
			Result.ReplaceInline(TEXT("\r"), TEXT("\\r"), ESearchCase::CaseSensitive);
			Result.ReplaceInline(TEXT("\n"), TEXT("\\n"), ESearchCase::CaseSensitive);
			Result.ReplaceInline(TEXT("\t"), TEXT("\\t"), ESearchCase::CaseSensitive);
			return Result;
		}

		FString Quote(const FString& Text)
		{
			FString Result = OneLine(Text);
			Result.ReplaceInline(TEXT("\""), TEXT("\\\""), ESearchCase::CaseSensitive);
			return TEXT("\"") + Result + TEXT("\"");
		}

		FString ShortName(const FString& ObjectPath)
		{
			int32 Dot = INDEX_NONE;
			return ObjectPath.FindLastChar(TEXT('.'), Dot) ? ObjectPath.Mid(Dot + 1) : ObjectPath;
		}

		/** The units whose nodes start execution. UserDefinedEvent is the RigVM one; the Control Rig name is the older spelling. */
		bool IsEventStruct(const FString& StructPath)
		{
			static const TCHAR* const Names[] = {
				TEXT("RigUnit_BeginExecution"),
				TEXT("RigUnit_InverseExecution"),
				TEXT("RigUnit_PrepareForExecution"),
				TEXT("RigUnit_PostPrepareForExecution"),
				TEXT("RigUnit_PreBeginExecution"),
				TEXT("RigUnit_PostBeginExecution"),
				TEXT("RigUnit_InteractionExecution"),
				TEXT("RigUnit_ConnectorExecution"),
				TEXT("RigVMFunction_UserDefinedEvent"),
				TEXT("RigUnit_UserDefinedEvent"),
			};
			const FString Short = ShortName(StructPath);
			for (const TCHAR* Name : Names)
			{
				if (Short.Equals(Name, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

		/** The name of the event a user-defined event unit carries; empty for the others. */
		FString EventName(const FDcrModelNode& Node)
		{
			for (const FDcrValue& Value : Node.Values)
			{
				if (Value.Name.Equals(TEXT("EventName"), ESearchCase::CaseSensitive))
				{
					return Value.Value;
				}
			}
			return FString();
		}

		// ---- Float as double -----------------------------------------------------------------------------------

		FString MapCPPType(const FString& CPPType, bool bFloatAsDouble)
		{
			if (!bFloatAsDouble)
			{
				return CPPType;
			}
			if (CPPType.Equals(TEXT("float"), ESearchCase::CaseSensitive))
			{
				return TEXT("double");
			}
			return CPPType.Replace(TEXT("<float>"), TEXT("<double>"), ESearchCase::CaseSensitive);
		}

		FString TypeText(const FDcrType& Type, bool bFloatAsDouble)
		{
			const FString CPPType = MapCPPType(Type.CPPType, bFloatAsDouble);
			return Type.ObjectPath.IsEmpty() ? CPPType : CPPType + TEXT("@") + Type.ObjectPath;
		}

		FString MapStructPath(const FString& StructPath, bool bFloatAsDouble)
		{
			static const TCHAR* const FloatPrefix = TEXT("RigVMFunction_MathFloat");
			if (!bFloatAsDouble)
			{
				return StructPath;
			}
			const FString Short = ShortName(StructPath);
			if (!Short.StartsWith(FloatPrefix, ESearchCase::CaseSensitive))
			{
				return StructPath;
			}
			return StructPath.Left(StructPath.Len() - Short.Len()) + TEXT("RigVMFunction_MathDouble") + Short.Mid(FCString::Strlen(FloatPrefix));
		}

		// ---- Sorting -------------------------------------------------------------------------------------------

		TArray<int32> SortedValueOrder(const TArray<FDcrValue>& Values)
		{
			TArray<int32> Order;
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				Order.Add(Index);
			}
			Order.StableSort([&Values](int32 A, int32 B)
			{
				const int32 ByName = CompareText(Values[A].Name, Values[B].Name);
				return ByName != 0 ? ByName < 0 : LessText(Values[A].Value, Values[B].Value);
			});
			return Order;
		}

		void DumpValues(FString& Out, int32 Depth, const TCHAR* Word, const TArray<FDcrValue>& Values)
		{
			for (const int32 Index : SortedValueOrder(Values))
			{
				Line(Out, Depth, FString::Printf(TEXT("%s %s = %s"), Word, *Values[Index].Name, *OneLine(Values[Index].Value)));
			}
		}

		/** A node described without its name: what it is and what it holds. Orders nodes that execution does not reach. */
		FString Signature(const FDcrModelNode& Node)
		{
			const FString Notation = Node.Kind == EDcrNodeKind::Dispatch ? Node.Notation : FString();
			FString Text = FString::Printf(TEXT("%02d|%s|%s|%s|%s|%s"), static_cast<int32>(Node.Kind), *Node.StructPath, *Notation, *Node.Function, *Node.Variable, *Node.CommentText);
			for (const int32 Index : SortedValueOrder(Node.Values))
			{
				Text += FString::Printf(TEXT("|%s=%s"), *Node.Values[Index].Name, *Node.Values[Index].Value);
			}
			return Text;
		}

		// ---- Naming --------------------------------------------------------------------------------------------

		struct FNaming
		{
			/** The nodes in the order they print. */
			TArray<const FDcrModelNode*> Order;
			/** Each node's name in the model, and the name it prints under. */
			TMap<FString, FString> Names;

			/** `Node.Pin.Sub` with the node part renamed. */
			FString Rename(const FString& PinPath) const
			{
				int32 Dot = INDEX_NONE;
				const bool bHasPin = PinPath.FindChar(TEXT('.'), Dot);
				const FString Node = bHasPin ? PinPath.Left(Dot) : PinPath;
				const FString* Printed = Names.Find(Node);
				if (!Printed)
				{
					return PinPath;
				}
				return bHasPin ? *Printed + PinPath.Mid(Dot) : *Printed;
			}
		};

		struct FEdge
		{
			int32 From = INDEX_NONE;
			int32 To = INDEX_NONE;
			/** The pin paths without their node: `Pin.Sub`. */
			FString FromPin;
			FString ToPin;
		};

		void SplitPath(const FString& Path, FString& OutNode, FString& OutRest)
		{
			int32 Dot = INDEX_NONE;
			if (Path.FindChar(TEXT('.'), Dot))
			{
				OutNode = Path.Left(Dot);
				OutRest = Path.Mid(Dot + 1);
			}
			else
			{
				OutNode = Path;
				OutRest.Reset();
			}
		}

		/**
		 * Names the nodes n1, n2, ... in the order they are reached: from the graph's entry and its events (in
		 * their own order), along execute links by the pin they leave, and then, for every node reached, back along
		 * the links into its inputs. A node nothing reaches is named afterwards, in the order of what it is, then
		 * of its name. Comments come last so that they never move another node's number.
		 */
		FNaming NameNodes(const FDcrModelGraph& Graph, const FDcrDumpOptions& Options)
		{
			FNaming Naming;
			TArray<const FDcrModelNode*> Nodes;
			for (const FDcrModelNode& Node : Graph.Nodes)
			{
				if (Options.bSkipComments && Node.Kind == EDcrNodeKind::Comment)
				{
					continue;
				}
				Nodes.Add(&Node);
			}
			const int32 Count = Nodes.Num();

			if (!Options.bCanonicalNodeNames)
			{
				TArray<int32> Indices;
				for (int32 Index = 0; Index < Count; ++Index)
				{
					Indices.Add(Index);
				}
				Indices.StableSort([&Nodes](int32 A, int32 B) { return LessText(Nodes[A]->Name, Nodes[B]->Name); });
				for (const int32 Index : Indices)
				{
					Naming.Order.Add(Nodes[Index]);
					Naming.Names.Add(Nodes[Index]->Name, Nodes[Index]->Name);
				}
				return Naming;
			}

			TMap<FString, int32> IndexByName;
			TArray<FString> Signatures;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (!IndexByName.Contains(Nodes[Index]->Name))
				{
					IndexByName.Add(Nodes[Index]->Name, Index);
				}
				Signatures.Add(Signature(*Nodes[Index]));
			}

			TArray<FEdge> Edges;
			TArray<TArray<int32>> OutEdges;
			TArray<TArray<int32>> InEdges;
			OutEdges.SetNum(Count);
			InEdges.SetNum(Count);
			for (const FDcrModelLink& Link : Graph.Links)
			{
				FString SourceNode;
				FString SourcePin;
				FString TargetNode;
				FString TargetPin;
				SplitPath(Link.Source, SourceNode, SourcePin);
				SplitPath(Link.Target, TargetNode, TargetPin);
				const int32* From = IndexByName.Find(SourceNode);
				const int32* To = IndexByName.Find(TargetNode);
				if (!From || !To)
				{
					continue;
				}
				FEdge& Edge = Edges.AddDefaulted_GetRef();
				Edge.From = *From;
				Edge.To = *To;
				Edge.FromPin = SourcePin;
				Edge.ToPin = TargetPin;
				OutEdges[*From].Add(Edges.Num() - 1);
				InEdges[*To].Add(Edges.Num() - 1);
			}

			// Nodes alike in themselves, told apart by what they link to and from (before their own names, which differ
			// between two models of one rig).
			TArray<FString> Around;
			Around.SetNum(Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				TArray<FString> Parts;
				for (const int32 EdgeIndex : InEdges[Index])
				{
					const FEdge& Edge = Edges[EdgeIndex];
					Parts.Add(FString::Printf(TEXT("<%s<%s<%s"), *Edge.ToPin, *Edge.FromPin, *Signatures[Edge.From]));
				}
				for (const int32 EdgeIndex : OutEdges[Index])
				{
					const FEdge& Edge = Edges[EdgeIndex];
					Parts.Add(FString::Printf(TEXT(">%s>%s>%s"), *Edge.FromPin, *Edge.ToPin, *Signatures[Edge.To]));
				}
				Parts.Sort([](const FString& A, const FString& B) { return CompareText(A, B) < 0; });
				Around[Index] = FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*FString::Join(Parts, TEXT(";"))));
			}
			auto LessAlike = [&Around, &Nodes](int32 A, int32 B)
			{
				const int32 Result = CompareText(Around[A], Around[B]);
				return Result != 0 ? Result < 0 : LessText(Nodes[A]->Name, Nodes[B]->Name);
			};

			// By the pin an edge leaves, the pin it enters, and then what is on the other end.
			auto LessOut = [&Edges, &Signatures, &LessAlike](int32 A, int32 B)
			{
				const FEdge& EdgeA = Edges[A];
				const FEdge& EdgeB = Edges[B];
				int32 Result = CompareText(EdgeA.FromPin, EdgeB.FromPin);
				if (Result == 0)
				{
					Result = CompareText(EdgeA.ToPin, EdgeB.ToPin);
				}
				if (Result == 0)
				{
					Result = CompareText(Signatures[EdgeA.To], Signatures[EdgeB.To]);
				}
				if (Result == 0)
				{
					return LessAlike(EdgeA.To, EdgeB.To);
				}
				return Result < 0;
			};
			auto LessIn = [&Edges, &Signatures, &LessAlike](int32 A, int32 B)
			{
				const FEdge& EdgeA = Edges[A];
				const FEdge& EdgeB = Edges[B];
				int32 Result = CompareText(EdgeA.ToPin, EdgeB.ToPin);
				if (Result == 0)
				{
					Result = CompareText(EdgeA.FromPin, EdgeB.FromPin);
				}
				if (Result == 0)
				{
					Result = CompareText(Signatures[EdgeA.From], Signatures[EdgeB.From]);
				}
				if (Result == 0)
				{
					return LessAlike(EdgeA.From, EdgeB.From);
				}
				return Result < 0;
			};
			for (int32 Index = 0; Index < Count; ++Index)
			{
				OutEdges[Index].StableSort(LessOut);
				InEdges[Index].StableSort(LessIn);
			}

			TArray<int32> Number;
			Number.Init(0, Count);
			TArray<int32> Order;

			// Names the node and everything execution runs on to from it.
			auto NameFrom = [&](int32 Start)
			{
				TArray<int32> Stack;
				Stack.Add(Start);
				while (!Stack.IsEmpty())
				{
					const int32 Current = Stack.Pop();
					if (Number[Current] != 0)
					{
						continue;
					}
					Order.Add(Current);
					Number[Current] = Order.Num();
					TArray<int32> Targets;
					for (const int32 EdgeIndex : OutEdges[Current])
					{
						if (Nodes[Current]->ExecuteOut.Contains(Edges[EdgeIndex].FromPin))
						{
							Targets.Add(Edges[EdgeIndex].To);
						}
					}
					for (int32 Target = Targets.Num() - 1; Target >= 0; --Target)
					{
						Stack.Add(Targets[Target]);
					}
				}
			};

			// Reaches back into the inputs of every node named so far, and of the ones that brings in.
			int32 Head = 0;
			auto Drain = [&]()
			{
				while (Head < Order.Num())
				{
					const int32 Current = Order[Head++];
					for (const int32 EdgeIndex : InEdges[Current])
					{
						NameFrom(Edges[EdgeIndex].From);
					}
				}
			};

			TArray<int32> Entries;
			TArray<int32> Events;
			TArray<int32> Returns;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (Nodes[Index]->Kind == EDcrNodeKind::Entry)
				{
					Entries.Add(Index);
				}
				else if (Nodes[Index]->Kind == EDcrNodeKind::Return)
				{
					Returns.Add(Index);
				}
				else if (Nodes[Index]->Kind == EDcrNodeKind::Unit && IsEventStruct(Nodes[Index]->StructPath))
				{
					Events.Add(Index);
				}
			}
			Entries.StableSort([&](int32 A, int32 B)
			{
				const int32 Result = CompareText(Signatures[A], Signatures[B]);
				return Result != 0 ? Result < 0 : LessAlike(A, B);
			});
			Returns.StableSort([&](int32 A, int32 B)
			{
				const int32 Result = CompareText(Signatures[A], Signatures[B]);
				return Result != 0 ? Result < 0 : LessAlike(A, B);
			});
			Events.StableSort([&](int32 A, int32 B)
			{
				int32 Result = CompareText(ShortName(Nodes[A]->StructPath), ShortName(Nodes[B]->StructPath));
				if (Result == 0)
				{
					Result = CompareText(EventName(*Nodes[A]), EventName(*Nodes[B]));
				}
				if (Result == 0)
				{
					Result = CompareText(Signatures[A], Signatures[B]);
				}
				return Result != 0 ? Result < 0 : LessText(Nodes[A]->Name, Nodes[B]->Name);
			});
			for (const int32 Index : Entries)
			{
				NameFrom(Index);
			}
			for (const int32 Index : Events)
			{
				NameFrom(Index);
			}
			Drain();
			// A pure graph is reached from what it gives back.
			for (const int32 Index : Returns)
			{
				NameFrom(Index);
				Drain();
			}

			// What nothing reaches, then the comments.
			for (const bool bComments : { false, true })
			{
				TArray<int32> Rest;
				for (int32 Index = 0; Index < Count; ++Index)
				{
					if (Number[Index] == 0 && (Nodes[Index]->Kind == EDcrNodeKind::Comment) == bComments)
					{
						Rest.Add(Index);
					}
				}
				Rest.StableSort([&](int32 A, int32 B)
				{
					const int32 Result = CompareText(Signatures[A], Signatures[B]);
					return Result != 0 ? Result < 0 : LessAlike(A, B);
				});
				for (const int32 Index : Rest)
				{
					NameFrom(Index);
					Drain();
				}
			}

			for (const int32 Index : Order)
			{
				Naming.Order.Add(Nodes[Index]);
				Naming.Names.Add(Nodes[Index]->Name, FString::Printf(TEXT("n%d"), Number[Index]));
			}
			return Naming;
		}

		// ---- Printing ------------------------------------------------------------------------------------------

		void DumpGraphBody(FString& Out, int32 Depth, const FDcrModelGraph& Graph, const FDcrDumpOptions& Options);

		void DumpNode(FString& Out, int32 Depth, const FDcrModelNode& Node, const FNaming& Naming, const FDcrDumpOptions& Options)
		{
			const bool bFloatAsDouble = Options.bFloatAsDouble;
			FString Head = FString::Printf(TEXT("Node %s %s"), *Naming.Names.FindRef(Node.Name), LexToString(Node.Kind));
			switch (Node.Kind)
			{
			case EDcrNodeKind::Unit:
				Head += TEXT(" ") + MapStructPath(Node.StructPath, bFloatAsDouble);
				break;
			case EDcrNodeKind::VariableGet:
			case EDcrNodeKind::VariableSet:
				Head += TEXT(" ") + Quote(Node.Variable) + TEXT(" ") + TypeText(Node.VariableType, bFloatAsDouble);
				if (Node.bLocalVariable)
				{
					Head += TEXT(" local");
				}
				break;
			case EDcrNodeKind::LocalCall:
			case EDcrNodeKind::InvokeEntry:
				Head += TEXT(" ") + Quote(Node.Function);
				break;
			case EDcrNodeKind::ExternalCall:
				Head += TEXT(" ") + Quote(Node.Function) + TEXT(" ") + Node.FunctionHost;
				break;
			case EDcrNodeKind::Comment:
				Head += TEXT(" ") + Quote(Node.CommentText);
				break;
			default:
				break;
			}
			Line(Out, Depth, Head);

			// A unit is its struct; the notation belongs to dispatches.
			if (Node.Kind == EDcrNodeKind::Dispatch && !Node.Notation.IsEmpty())
			{
				Line(Out, Depth + 1, TEXT("Notation ") + Node.Notation);
			}
			for (const TPair<FString, FDcrType>& Resolved : Node.ResolvedTypes)
			{
				Line(Out, Depth + 1, FString::Printf(TEXT("Type %s = %s"), *Resolved.Key, *TypeText(Resolved.Value, bFloatAsDouble)));
			}
			DumpValues(Out, Depth + 1, TEXT("Value"), Node.Values);
			if (!Node.AggregatePins.IsEmpty())
			{
				Line(Out, Depth + 1, TEXT("Aggregate ") + FString::Join(Node.AggregatePins, TEXT(" ")));
			}
			if (!Node.InjectedOn.IsEmpty())
			{
				Line(Out, Depth + 1, FString::Printf(TEXT("Injected on %s as %s in %s out %s"), *Naming.Rename(Node.InjectedOn),
					Node.bInjectedAsInput ? TEXT("input") : TEXT("output"), *Node.InjectedInputPin, *Node.InjectedOutputPin));
			}
			if (!Node.CommentMembers.IsEmpty())
			{
				TArray<FString> Members;
				for (const FString& Member : Node.CommentMembers)
				{
					Members.Add(Naming.Names.Contains(Member) ? Naming.Names.FindRef(Member) : Member);
				}
				Members.StableSort(LessText);
				Line(Out, Depth + 1, TEXT("Members ") + FString::Join(Members, TEXT(" ")));
			}
			if (Node.Contained.IsValid())
			{
				Line(Out, Depth + 1, TEXT("Graph"));
				DumpGraphBody(Out, Depth + 2, *Node.Contained, Options);
			}
		}

		void DumpGraphBody(FString& Out, int32 Depth, const FDcrModelGraph& Graph, const FDcrDumpOptions& Options)
		{
			const FNaming Naming = NameNodes(Graph, Options);
			for (const FDcrModelNode* Node : Naming.Order)
			{
				DumpNode(Out, Depth, *Node, Naming, Options);
			}

			TArray<FString> Links;
			for (const FDcrModelLink& Link : Graph.Links)
			{
				FString SourceNode;
				FString Ignored;
				SplitPath(Link.Source, SourceNode, Ignored);
				FString TargetNode;
				SplitPath(Link.Target, TargetNode, Ignored);
				// A link to a comment that was left out would be a link to nothing.
				if (Options.bSkipComments && (!Naming.Names.Contains(SourceNode) || !Naming.Names.Contains(TargetNode)))
				{
					continue;
				}
				Links.Add(FString::Printf(TEXT("Link %s -> %s"), *Naming.Rename(Link.Source), *Naming.Rename(Link.Target)));
			}
			Links.StableSort(LessText);
			for (const FString& Text : Links)
			{
				Line(Out, Depth, Text);
			}
		}

		FString VariableText(const TCHAR* Word, const FDcrModelVariable& Variable, bool bFloatAsDouble)
		{
			FString Text = FString::Printf(TEXT("%s %s %s"), Word, *Quote(Variable.Name), *TypeText(Variable.Type, bFloatAsDouble));
			if (Variable.bPublic)
			{
				Text += TEXT(" public");
			}
			if (Variable.bReadOnly)
			{
				Text += TEXT(" readonly");
			}
			if (!Variable.Category.IsEmpty())
			{
				Text += TEXT(" category ") + Quote(Variable.Category);
			}
			if (!Variable.Tooltip.IsEmpty())
			{
				Text += TEXT(" tooltip ") + Quote(Variable.Tooltip);
			}
			if (!Variable.Default.IsEmpty())
			{
				Text += TEXT(" = ") + OneLine(Variable.Default);
			}
			return Text;
		}

		FString FunctionPinText(const TCHAR* Word, const FDcrModelFunctionPin& Pin, bool bFloatAsDouble)
		{
			FString Text = FString::Printf(TEXT("%s %s %s"), Word, *Quote(Pin.Name), *TypeText(Pin.Type, bFloatAsDouble));
			if (!Pin.Default.IsEmpty())
			{
				Text += TEXT(" = ") + OneLine(Pin.Default);
			}
			return Text;
		}

		void DumpFunction(FString& Out, int32 Depth, const FDcrModelFunction& Function, const FDcrDumpOptions& Options)
		{
			Line(Out, Depth, FString::Printf(TEXT("Function %s %s%s"), *Quote(Function.Name), Function.bMutable ? TEXT("mutable") : TEXT("pure"), Function.bPublic ? TEXT(" public") : TEXT("")));
			if (!Function.Category.IsEmpty())
			{
				Line(Out, Depth + 1, TEXT("Category ") + Quote(Function.Category));
			}
			if (!Function.Keywords.IsEmpty())
			{
				Line(Out, Depth + 1, TEXT("Keywords ") + Quote(Function.Keywords));
			}
			if (!Function.Description.IsEmpty())
			{
				Line(Out, Depth + 1, TEXT("Description ") + Quote(Function.Description));
			}
			for (const FDcrModelFunctionPin& Pin : Function.Inputs)
			{
				Line(Out, Depth + 1, FunctionPinText(TEXT("Input"), Pin, Options.bFloatAsDouble));
			}
			for (const FDcrModelFunctionPin& Pin : Function.Outputs)
			{
				Line(Out, Depth + 1, FunctionPinText(TEXT("Output"), Pin, Options.bFloatAsDouble));
			}
			for (const FDcrModelVariable& Local : Function.Locals)
			{
				Line(Out, Depth + 1, VariableText(TEXT("Local"), Local, Options.bFloatAsDouble));
			}
			Line(Out, Depth + 1, TEXT("Graph"));
			DumpGraphBody(Out, Depth + 2, Function.Graph, Options);
		}
	}

	FString DumpDcrModel(const FDcrModel& Model, const FDcrDumpOptions& Options)
	{
		FString Out;
		Line(Out, 0, TEXT("ControlRig"));
		if (Model.bModule)
		{
			Line(Out, 1, TEXT("Module"));
		}
		DumpValues(Out, 1, TEXT("ModuleSetting"), Model.ModuleSettings);
		DumpValues(Out, 1, TEXT("Setting"), Model.Settings);

		// Parents before children, siblings by kind and name: the order of siblings changes nothing a rig does.
		TArray<const FDcrModelElement*> Elements;
		{
			auto KeyOf = [](EDcrElementKind Kind, const FString& Name) { return FString::Printf(TEXT("%s(%s)"), LexToString(Kind), *Name); };
			TMap<FString, TArray<const FDcrModelElement*>> Children;
			TArray<const FDcrModelElement*> Roots;
			TSet<FString> Known;
			for (const FDcrModelElement& Element : Model.Hierarchy)
			{
				Known.Add(KeyOf(Element.Kind, Element.Name));
			}
			for (const FDcrModelElement& Element : Model.Hierarchy)
			{
				const FString ParentKey = KeyOf(Element.ParentKind, Element.Parent);
				if (Element.Parent.IsEmpty() || !Known.Contains(ParentKey))
				{
					Roots.Add(&Element);
				}
				else
				{
					Children.FindOrAdd(ParentKey).Add(&Element);
				}
			}
			auto BySiblingOrder = [&KeyOf](const FDcrModelElement& A, const FDcrModelElement& B) { return KeyOf(A.Kind, A.Name) < KeyOf(B.Kind, B.Name); };
			TFunction<void(TArray<const FDcrModelElement*>&)> Visit = [&](TArray<const FDcrModelElement*>& Siblings)
			{
				Siblings.StableSort(BySiblingOrder);
				for (const FDcrModelElement* Element : Siblings)
				{
					Elements.Add(Element);
					if (TArray<const FDcrModelElement*>* Below = Children.Find(KeyOf(Element->Kind, Element->Name)))
					{
						Visit(*Below);
					}
				}
			};
			Visit(Roots);
		}
		for (const FDcrModelElement* ElementPtr : Elements)
		{
			const FDcrModelElement& Element = *ElementPtr;
			FString Text = FString::Printf(TEXT("Element %s %s"), LexToString(Element.Kind), *Quote(Element.Name));
			if (!Element.Parent.IsEmpty())
			{
				Text += FString::Printf(TEXT(" parent %s %s"), LexToString(Element.ParentKind), *Quote(Element.Parent));
			}
			Line(Out, 1, Text);
			DumpValues(Out, 2, TEXT("Property"), Element.Properties);
		}

		for (const FDcrModelVariable& Variable : Model.Variables)
		{
			Line(Out, 1, VariableText(TEXT("Variable"), Variable, Options.bFloatAsDouble));
		}
		for (const FDcrModelFunction& Function : Model.Functions)
		{
			DumpFunction(Out, 1, Function, Options);
		}
		for (const FDcrModelGraph& Graph : Model.Graphs)
		{
			Line(Out, 1, TEXT("Graph ") + Quote(Graph.Name));
			DumpGraphBody(Out, 2, Graph, Options);
		}
		return Out;
	}
}
