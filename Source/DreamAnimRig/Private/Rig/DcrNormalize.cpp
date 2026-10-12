// Copyright DreamMoon. All Rights Reserved.

#include "Rig/DcrNormalize.h"

#include "DcrExecPins.h"
#include "DcrNames.h"
#include "DcrValues.h"
#include "Rig/DcrAsset.h"
#include "Rig/DcrHierarchyImport.h"
#include "RigVMCore/RigVMDispatchFactory.h"
#include "RigVMCore/RigVMRegistry.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMModel/RigVMSchema.h"
#include "RigVMTypeUtils.h"
#include "UObject/StructOnScope.h"

namespace UE::DreamAnim::Rig
{
	using namespace Model;

	namespace
	{
		FString NodeOf(const FString& PinPath)
		{
			FString Node;
			FString Rest;
			SplitDcrPinPath(PinPath, Node, Rest);
			return Node;
		}

		/** The part of a pin path under the node's root pin: `Pin.Sub.X` gives `.Sub.X`, `Pin` gives nothing. */
		FString SubPathOf(const FString& Rest)
		{
			int32 Dot = INDEX_NONE;
			return Rest.FindChar(TEXT('.'), Dot) ? Rest.Mid(Dot) : FString();
		}

		TArray<FString> FindTargets(const FDcrModelGraph& Graph, const FString& SourcePath)
		{
			TArray<FString> Targets;
			for (const FDcrModelLink& Link : Graph.Links)
			{
				if (Link.Source == SourcePath)
				{
					Targets.Add(Link.Target);
				}
			}
			return Targets;
		}

		TArray<FString> FindSources(const FDcrModelGraph& Graph, const FString& TargetPath)
		{
			TArray<FString> Sources;
			for (const FDcrModelLink& Link : Graph.Links)
			{
				if (Link.Target == TargetPath)
				{
					Sources.Add(Link.Source);
				}
			}
			return Sources;
		}

		void RemoveNodeAndLinks(FDcrModelGraph& Graph, const FString& NodeName)
		{
			Graph.Nodes.RemoveAll([&NodeName](const FDcrModelNode& Node) { return Node.Name == NodeName; });
			Graph.Links.RemoveAll([&NodeName](const FDcrModelLink& Link) { return NodeOf(Link.Source) == NodeName || NodeOf(Link.Target) == NodeName; });
		}

		void AddLink(FDcrModelGraph& Graph, const FString& Source, const FString& Target)
		{
			FDcrModelLink& Link = Graph.Links.AddDefaulted_GetRef();
			Link.Source = Source;
			Link.Target = Target;
		}

		// ---- Reroutes ------------------------------------------------------------------------------------------

		struct FRerouteSide
		{
			int32 Link = INDEX_NONE;
			/** Where on the reroute's pin the link attaches: nothing for the pin itself, `.X` for a member. */
			FString Suffix;
		};

		/** Replaces a reroute and its links with direct links. False, and nothing changed, when a link cannot be carried across. */
		bool FoldReroute(FDcrModelGraph& Graph, const FString& RerouteName, FString& OutReason)
		{
			const FDcrModelNode* Reroute = Graph.FindNode(RerouteName);
			if (!Reroute)
			{
				return false;
			}
			const bool bHasLiteral = Reroute->Values.Num() > 0;
			const FString Literal = bHasLiteral ? Reroute->Values[0].Value : FString();

			TArray<FRerouteSide> Incoming;
			TArray<FRerouteSide> Outgoing;
			for (int32 Index = 0; Index < Graph.Links.Num(); ++Index)
			{
				FString Node;
				FString Rest;
				SplitDcrPinPath(Graph.Links[Index].Target, Node, Rest);
				if (Node == RerouteName)
				{
					FRerouteSide& Side = Incoming.AddDefaulted_GetRef();
					Side.Link = Index;
					Side.Suffix = SubPathOf(Rest);
				}
				SplitDcrPinPath(Graph.Links[Index].Source, Node, Rest);
				if (Node == RerouteName)
				{
					FRerouteSide& Side = Outgoing.AddDefaulted_GetRef();
					Side.Link = Index;
					Side.Suffix = SubPathOf(Rest);
				}
			}

			struct FLiteralTarget
			{
				FString Node;
				FString Pin;
			};
			TArray<FDcrModelLink> NewLinks;
			TArray<FLiteralTarget> LiteralTargets;
			for (const FRerouteSide& Out : Outgoing)
			{
				const FDcrModelLink& OutLink = Graph.Links[Out.Link];

				// The incoming link that reaches this one: the pin itself, or the member it is a part of. The deepest wins.
				int32 Best = INDEX_NONE;
				for (int32 Index = 0; Index < Incoming.Num(); ++Index)
				{
					const FString& Suffix = Incoming[Index].Suffix;
					const bool bReaches = Suffix.IsEmpty() || Out.Suffix == Suffix || Out.Suffix.StartsWith(Suffix + TEXT("."));
					if (bReaches && (Best == INDEX_NONE || Suffix.Len() > Incoming[Best].Suffix.Len()))
					{
						Best = Index;
					}
				}

				if (Best != INDEX_NONE)
				{
					const FRerouteSide& In = Incoming[Best];
					FDcrModelLink& NewLink = NewLinks.AddDefaulted_GetRef();
					NewLink.Source = Graph.Links[In.Link].Source + Out.Suffix.Mid(In.Suffix.Len());
					NewLink.Target = OutLink.Target;
				}
				else if (Incoming.IsEmpty() && bHasLiteral && Out.Suffix.IsEmpty())
				{
					// A reroute with nothing behind it holds a value; the pins it feeds take it over.
					FString TargetNode;
					FString TargetPin;
					SplitDcrPinPath(OutLink.Target, TargetNode, TargetPin);
					if (TargetPin.IsEmpty() || TargetPin.Contains(TEXT(".")) || !Graph.FindNode(TargetNode))
					{
						OutReason = TEXT("the value it holds cannot be moved to a member pin");
						return false;
					}
					FLiteralTarget& Target = LiteralTargets.AddDefaulted_GetRef();
					Target.Node = TargetNode;
					Target.Pin = TargetPin;
				}
				else if (Incoming.IsEmpty())
				{
					// Nothing behind it and nothing held: the links out of it carry nothing.
				}
				else
				{
					OutReason = TEXT("a link out of it needs a member the link into it does not carry");
					return false;
				}
			}

			TSet<int32> Dropped;
			for (const FRerouteSide& Side : Incoming)
			{
				Dropped.Add(Side.Link);
			}
			for (const FRerouteSide& Side : Outgoing)
			{
				Dropped.Add(Side.Link);
			}
			TArray<FDcrModelLink> Kept;
			for (int32 Index = 0; Index < Graph.Links.Num(); ++Index)
			{
				if (!Dropped.Contains(Index))
				{
					Kept.Add(Graph.Links[Index]);
				}
			}
			Kept.Append(NewLinks);
			Graph.Links = MoveTemp(Kept);

			for (const FLiteralTarget& Target : LiteralTargets)
			{
				if (FDcrModelNode* Node = Graph.FindNode(Target.Node))
				{
					FDcrValue* Existing = Node->Values.FindByPredicate([&Target](const FDcrValue& Value) { return Value.Name == Target.Pin; });
					if (Existing)
					{
						Existing->Value = Literal;
					}
					else
					{
						FDcrValue& Value = Node->Values.AddDefaulted_GetRef();
						Value.Name = Target.Pin;
						Value.Value = Literal;
					}
				}
			}

			Graph.Nodes.RemoveAll([&RerouteName](const FDcrModelNode& Node) { return Node.Name == RerouteName; });
			return true;
		}

		void FoldReroutes(FDcrModelGraph& Graph, const FString& Location, FDcrNormalizeReport& Report)
		{
			TMap<FString, FString> Stuck;
			bool bProgress = true;
			while (bProgress)
			{
				bProgress = false;
				for (int32 Index = 0; Index < Graph.Nodes.Num(); ++Index)
				{
					if (Graph.Nodes[Index].Kind != EDcrNodeKind::Reroute || Stuck.Contains(Graph.Nodes[Index].Name))
					{
						continue;
					}
					const FString Name = Graph.Nodes[Index].Name;
					FString Reason;
					if (FoldReroute(Graph, Name, Reason))
					{
						++Report.ReroutesFolded;
						bProgress = true;
						break;
					}
					Stuck.Add(Name, Reason);
				}
			}
			for (const TPair<FString, FString>& Entry : Stuck)
			{
				Report.Notes.Add(FString::Printf(TEXT("%s/%s: reroute kept: %s"), *Location, *Entry.Key, *Entry.Value));
			}
		}

		// ---- Sequences -----------------------------------------------------------------------------------------

		/**
		 * The last pin of the chain that runs on from Start: the one nothing is linked to yet. False when the
		 * chain ends in a node execution does not continue from (a Return) or comes round to a node it has seen.
		 */
		bool FindChainEnd(const FDcrModelGraph& Graph, const FDcrModelNode& Start, FString& OutPinPath, FString* OutReturnTarget = nullptr)
		{
			FString Continuation = GetDcrContinuation(Start);
			if (Continuation.IsEmpty())
			{
				return false;
			}
			FString PinPath = Start.Name + TEXT(".") + Continuation;
			TSet<FString> Seen;
			while (true)
			{
				const TArray<FString> Targets = FindTargets(Graph, PinPath);
				if (Targets.IsEmpty())
				{
					OutPinPath = PinPath;
					return true;
				}
				const FString NextName = NodeOf(Targets[0]);
				if (Seen.Contains(NextName))
				{
					return false;
				}
				Seen.Add(NextName);
				const FDcrModelNode* Next = Graph.FindNode(NextName);
				if (!Next)
				{
					return false;
				}
				// A chain that runs into the function's Return: the pin that runs into it, and where it goes.
				if (Next->Kind == EDcrNodeKind::Return && OutReturnTarget)
				{
					OutPinPath = PinPath;
					*OutReturnTarget = Targets[0];
					return true;
				}
				Continuation = GetDcrContinuation(*Next);
				if (Continuation.IsEmpty())
				{
					return false;
				}
				PinPath = Next->Name + TEXT(".") + Continuation;
			}
		}

		/** Replaces a Sequence with one chain: the end of each output's chain runs into the next output's first node. */
		bool FlattenSequence(FDcrModelGraph& Graph, const FString& SequenceName, FString& OutReason)
		{
			const FDcrModelNode* Sequence = Graph.FindNode(SequenceName);
			if (!Sequence)
			{
				return false;
			}

			TArray<FString> Sources;
			if (Sequence->ExecuteIn.Num() > 0)
			{
				Sources = FindSources(Graph, SequenceName + TEXT(".") + Sequence->ExecuteIn[0]);
			}

			TArray<FString> Heads;
			TArray<FString> HeadNodes;
			for (const FString& Output : GetDcrSequenceOutputs(*Sequence))
			{
				const TArray<FString> Targets = FindTargets(Graph, SequenceName + TEXT(".") + Output);
				if (Targets.Num() > 0)
				{
					Heads.Add(Targets[0]);
					HeadNodes.Add(NodeOf(Targets[0]));
				}
			}

			// An earlier output's chain may run into the function's Return. Reaching the Return does not end the
			// function (RigVM only exits at the end of an entry; the Return is where the outputs are read), so the
			// Return moves to the end of the last output's chain and the earlier chain runs on into the next one.
			TArray<FString> Ends;
			FString ReturnTarget;
			TArray<FString> CutIntoReturn;
			for (int32 Index = 0; Index + 1 < HeadNodes.Num(); ++Index)
			{
				const FDcrModelNode* Head = Graph.FindNode(HeadNodes[Index]);
				FString End;
				FString IntoReturn;
				if (Head && Head->Kind == EDcrNodeKind::Return)
				{
					// The output runs straight into the Return: nothing of its own to run.
					ReturnTarget = Heads[Index];
					Ends.Add(FString());
					continue;
				}
				if (!Head || !FindChainEnd(Graph, *Head, End, &IntoReturn))
				{
					OutReason = FString::Printf(TEXT("the chain of output %d does not end in a free execute pin"), Index);
					return false;
				}
				if (!IntoReturn.IsEmpty())
				{
					ReturnTarget = IntoReturn;
					CutIntoReturn.Add(End);
				}
				Ends.Add(End);
			}
			FString LastEnd;
			bool bLastIntoReturn = false;
			if (!ReturnTarget.IsEmpty() && HeadNodes.Num() > 0)
			{
				const FDcrModelNode* Last = Graph.FindNode(HeadNodes.Last());
				FString IntoReturn;
				if (!Last || (Last->Kind != EDcrNodeKind::Return && !FindChainEnd(Graph, *Last, LastEnd, &IntoReturn)))
				{
					OutReason = TEXT("the chain of the last output does not end in a free execute pin");
					return false;
				}
				bLastIntoReturn = Last->Kind == EDcrNodeKind::Return || !IntoReturn.IsEmpty();
			}

			for (const FString& End : CutIntoReturn)
			{
				Graph.Links.RemoveAll([&End, &ReturnTarget](const FDcrModelLink& Link) { return Link.Source == End && Link.Target == ReturnTarget; });
			}
			// Outputs that ran straight into the Return drop out of the chain.
			for (int32 Index = Ends.Num() - 1; Index >= 0; --Index)
			{
				if (Ends[Index].IsEmpty())
				{
					Ends.RemoveAt(Index);
					Heads.RemoveAt(Index);
					HeadNodes.RemoveAt(Index);
				}
			}

			RemoveNodeAndLinks(Graph, SequenceName);
			if (!ReturnTarget.IsEmpty() && !bLastIntoReturn && !LastEnd.IsEmpty())
			{
				AddLink(Graph, LastEnd, ReturnTarget);
			}
			if (Heads.Num() > 0)
			{
				for (const FString& Source : Sources)
				{
					AddLink(Graph, Source, Heads[0]);
				}
			}
			for (int32 Index = 0; Index < Ends.Num(); ++Index)
			{
				AddLink(Graph, Ends[Index], Heads[Index + 1]);
			}
			return true;
		}

		void FlattenSequences(FDcrModelGraph& Graph, const FString& Location, FDcrNormalizeReport& Report)
		{
			TMap<FString, FString> Refused;
			bool bProgress = true;
			while (bProgress)
			{
				bProgress = false;
				for (int32 Index = 0; Index < Graph.Nodes.Num(); ++Index)
				{
					if (!IsDcrSequence(Graph.Nodes[Index]) || Refused.Contains(Graph.Nodes[Index].Name))
					{
						continue;
					}
					const FString Name = Graph.Nodes[Index].Name;
					FString Reason;
					if (FlattenSequence(Graph, Name, Reason))
					{
						++Report.SequencesFlattened;
						// The chains around it changed, so a sequence refused before may go through now.
						Refused.Reset();
						bProgress = true;
						break;
					}
					Refused.Add(Name, Reason);
				}
			}
			for (const TPair<FString, FString>& Entry : Refused)
			{
				++Report.SequencesKept;
				Report.Notes.Add(FString::Printf(TEXT("%s/%s: sequence kept: %s"), *Location, *Entry.Key, *Entry.Value));
			}
		}

		// ---- Values --------------------------------------------------------------------------------------------

		/** A value on a pin that has a link into exactly that pin does nothing. Pins with only member links keep theirs. */
		void DropLinkedValues(FDcrModelGraph& Graph, FDcrNormalizeReport& Report)
		{
			TSet<FString> Linked;
			for (const FDcrModelLink& Link : Graph.Links)
			{
				Linked.Add(Link.Target);
			}
			for (FDcrModelNode& Node : Graph.Nodes)
			{
				Node.Values.RemoveAll([&Node, &Linked, &Report](const FDcrValue& Value)
				{
					if (Linked.Contains(Node.Name + TEXT(".") + Value.Name))
					{
						++Report.LinkedValuesDropped;
						return true;
					}
					return false;
				});
			}
		}

		/**
		 * The members of a pin's value that links feed (`Settings.Shape.Color`) hold whatever was there before the
		 * link, and it does nothing: they are put back to what a new node holds. A value that is then a new node's
		 * is dropped, except on a dispatch's wildcard pin, whose value carries its type.
		 */
		/** The type and the value a new pin of a function's node holds: an input of a function called, an output of the enclosing one. */
		bool FindFunctionPin(const FDcrModel& Model, const FDcrModelFunction* Enclosing, const FDcrModelNode& Node, const FString& PinName, FDcrType& OutType, FString& OutDefault)
		{
			for (const TPair<FString, FDcrType>& Resolved : Node.ResolvedTypes)
			{
				if (Resolved.Key == PinName)
				{
					// A collapse graph's own return: its pins are typed on the node.
					OutType = Resolved.Value;
					OutDefault.Reset();
					return true;
				}
			}
			if (Node.Kind == EDcrNodeKind::ExternalCall)
			{
				// Another rig's function: its pin as that rig's library holds it.
				FString Host = Node.FunctionHost;
				Host.RemoveFromEnd(TEXT("_C"));
				const FDcrRig Rig = LoadDcrRig(Host);
				URigVMFunctionLibrary* Library = Rig.IsValid() ? Rig.RigVM->GetLocalFunctionLibrary() : nullptr;
				URigVMLibraryNode* Function = Library ? Library->FindFunction(FName(*Node.Function)) : nullptr;
				const URigVMPin* Pin = Function ? Function->FindPin(PinName) : nullptr;
				if (!Pin)
				{
					return false;
				}
				OutType.CPPType = Pin->GetCPPType();
				OutType.ObjectPath = Pin->GetCPPTypeObject() ? Pin->GetCPPTypeObject()->GetPathName() : FString();
				OutDefault = Pin->GetDefaultValue();
				return true;
			}
			const TArray<FDcrModelFunctionPin>* Pins = nullptr;
			if (Node.Kind == EDcrNodeKind::Return && Enclosing)
			{
				Pins = &Enclosing->Outputs;
			}
			else if (Node.Kind == EDcrNodeKind::LocalCall)
			{
				const FDcrModelFunction* Callee = Model.Functions.FindByPredicate([&Node](const FDcrModelFunction& Function) { return Function.Name == Node.Function; });
				Pins = Callee ? &Callee->Inputs : nullptr;
			}
			const FDcrModelFunctionPin* Pin = Pins ? Pins->FindByPredicate([&PinName](const FDcrModelFunctionPin& Candidate) { return Candidate.Name == PinName; }) : nullptr;
			if (!Pin)
			{
				return false;
			}
			OutType = Pin->Type;
			OutDefault = Pin->Default;
			return true;
		}

		void ResetLinkedMembers(FDcrModelGraph& Graph, const FDcrModel& Model, const FDcrModelFunction* Enclosing, FDcrNormalizeReport& Report)
		{
			TMap<FString, TArray<FString>> MemberLinks;
			for (const FDcrModelLink& Link : Graph.Links)
			{
				FString Node;
				FString Rest;
				SplitDcrPinPath(Link.Target, Node, Rest);
				FString Pin;
				FString Sub;
				if (Rest.Split(TEXT("."), &Pin, &Sub))
				{
					MemberLinks.FindOrAdd(Node + TEXT(".") + Pin).Add(Sub);
				}
			}
			if (MemberLinks.IsEmpty())
			{
				return;
			}
			for (FDcrModelNode& Node : Graph.Nodes)
			{
				for (int32 Index = Node.Values.Num() - 1; Index >= 0; --Index)
				{
					FDcrValue& Value = Node.Values[Index];
					const TArray<FString>* Subs = MemberLinks.Find(Node.Name + TEXT(".") + Value.Name);
					if (!Subs)
					{
						continue;
					}
					FDcrValueSlot Current;
					FDcrValueSlot Fresh;
					bool bWildcard = false;
					const FDcrModelTrait* Trait = Node.Traits.FindByPredicate([&Value](const FDcrModelTrait& Candidate) { return Candidate.Name == Value.Name; });
					if (Trait)
					{
						// A trait's value: a new trait of its struct holds its members' defaults.
						const UScriptStruct* TraitStruct = LoadObject<UScriptStruct>(nullptr, *Trait->StructPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
						if (!TraitStruct || !Current.Init(MakeDcrType(*TraitStruct)) || !Fresh.Init(MakeDcrType(*TraitStruct)))
						{
							continue;
						}
					}
					else if (Node.Kind == EDcrNodeKind::Unit)
					{
						const UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *Node.StructPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
						const FProperty* Property = Struct ? Struct->FindPropertyByName(*Value.Name) : nullptr;
						if (!Property && Struct && Node.AggregatePins.Contains(Value.Name))
						{
							// A pin added past the struct's own: the type of the operands, a new value of it.
							for (const FDcrPin& Pin : GetDcrNodePins(*Struct))
							{
								if (Pin.bInput && Pin.Property)
								{
									if (Current.Init(*Pin.Property) && Fresh.Init(*Pin.Property))
									{
										Property = Pin.Property;
									}
									break;
								}
							}
							if (!Property)
							{
								continue;
							}
						}
						else if (!Property || !Current.Init(*Property) || !Fresh.Init(*Property))
						{
							continue;
						}
						else
						{
							FStructOnScope Defaults(Struct);
							Property->CopyCompleteValue(Fresh.GetMemory(), Property->ContainerPtrToValuePtr<void>(Defaults.GetStructMemory()));
						}
					}
					else if (Node.Kind == EDcrNodeKind::Dispatch)
					{
						const UScriptStruct* Struct = FindDcrDispatchStruct(Node.Notation);
						const FRigVMDispatchFactory* Factory = Struct ? FRigVMRegistry::Get().FindDispatchFactory(FRigVMDispatchFactory::GetFactoryName(Struct)) : nullptr;
						const FDcrPin* Pin = Struct ? GetDcrNodePins(*Struct).FindByPredicate([&Value](const FDcrPin& Candidate) { return Candidate.Name == Value.Name; }) : nullptr;
						if (!Factory || !Pin)
						{
							continue;
						}
						FDcrType Type = Pin->Type;
						bWildcard = !Type.IsSet();
						for (const TPair<FString, FDcrType>& Resolved : Node.ResolvedTypes)
						{
							if (Resolved.Key == Value.Name)
							{
								Type = Resolved.Value;
							}
						}
						UObject* TypeObject = Type.ObjectPath.IsEmpty() ? nullptr : RigVMTypeUtils::FindObjectFromCPPTypeObjectPath(Type.ObjectPath);
						const TRigVMTypeIndex TypeIndex = FRigVMRegistry::Get().GetTypeIndex(FName(*Type.CPPType), TypeObject);
						if (!Type.IsSet() || TypeIndex == INDEX_NONE || !Current.Init(Type) || !Fresh.Init(Type))
						{
							continue;
						}
						Fresh.ImportText(Factory->GetArgumentDefaultValue(FName(*Value.Name), TypeIndex));
					}
					else if (Node.Kind == EDcrNodeKind::VariableSet)
					{
						// A setter's value pin starts as a new value of the variable's type.
						if (!Current.Init(Node.VariableType) || !Fresh.Init(Node.VariableType))
						{
							continue;
						}
					}
					else if (Node.Kind == EDcrNodeKind::Return || Node.Kind == EDcrNodeKind::LocalCall || Node.Kind == EDcrNodeKind::ExternalCall)
					{
						FDcrType Type;
						FString Default;
						if (!FindFunctionPin(Model, Enclosing, Node, Value.Name, Type, Default) || !Current.Init(Type) || !Fresh.Init(Type))
						{
							continue;
						}
						Fresh.ImportText(Default);
					}
					else
					{
						continue;
					}
					if (!Current.ImportText(Value.Value))
					{
						Report.Notes.Add(FString::Printf(TEXT("%s.%s: the value does not read back, so what links feed in it is kept"), *Node.Name, *Value.Name));
						continue;
					}
					// Links into elements past the value's end find nothing to reset: the array grows to take them.
					ResetDcrValueParts(*Current.GetProperty(), Current.GetMemory(), Fresh.GetMemory(), *Subs);
					TrimDcrLinkedElements(*Current.GetProperty(), Current.GetMemory(), Fresh.GetMemory(), *Subs);
					if (!bWildcard && AreDcrValuesSame(*Current.GetProperty(), Current.GetMemory(), Fresh.GetMemory()))
					{
						Node.Values.RemoveAt(Index);
						++Report.LinkedValuesDropped;
					}
					else
					{
						Value.Value = Current.ExportText();
					}
				}
			}
		}

		/**
		 * A variable node spelling a rig variable's name in another case (older rigs have some) names it as declared:
		 * the engine finds it either way, and a rebuilt rig spells it so. A function's input or local of that name
		 * is what the node reads then, and is left alone.
		 */
		void SpellVariablesAsDeclared(FDcrModelGraph& Graph, const FDcrModel& Model, const FDcrModelFunction* Enclosing)
		{
			auto IsNamed = [](const FDcrModelVariable& Variable, const FString& Name) { return Variable.Name.Equals(Name, ESearchCase::IgnoreCase); };
			for (FDcrModelNode& Node : Graph.Nodes)
			{
				if ((Node.Kind != EDcrNodeKind::VariableGet && Node.Kind != EDcrNodeKind::VariableSet) || Node.bLocalVariable)
				{
					continue;
				}
				const FDcrModelVariable* Member = Model.Variables.FindByPredicate([&](const FDcrModelVariable& Variable) { return IsNamed(Variable, Node.Variable); });
				if (!Member || Member->Name.Equals(Node.Variable, ESearchCase::CaseSensitive))
				{
					continue;
				}
				const bool bShadowed = Enclosing && (Enclosing->Locals.ContainsByPredicate([&](const FDcrModelVariable& Local) { return IsNamed(Local, Node.Variable); })
					|| Enclosing->Inputs.ContainsByPredicate([&](const FDcrModelFunctionPin& Input) { return Input.Name.Equals(Node.Variable, ESearchCase::IgnoreCase); }));
				if (!bShadowed)
				{
					Node.Variable = Member->Name;
				}
			}
		}

		/** A getter nothing reads computes nothing, and the text has no place for it. */
		void DropUnreadGetters(FDcrModelGraph& Graph, FDcrNormalizeReport& Report)
		{
			TSet<FString> Read;
			for (const FDcrModelLink& Link : Graph.Links)
			{
				FString Node;
				FString Rest;
				SplitDcrPinPath(Link.Source, Node, Rest);
				Read.Add(Node);
			}
			Report.UnreadGettersDropped += Graph.Nodes.RemoveAll([&Read](const FDcrModelNode& Node)
			{
				return Node.Kind == EDcrNodeKind::VariableGet && Node.InjectedOn.IsEmpty() && !Read.Contains(Node.Name);
			});
		}

		/** A variable's getter injected into a pin reads as the getter linked to it: the text writes the variable's name there. */
		void ReleaseInjectedGetters(FDcrModelGraph& Graph)
		{
			TArray<TPair<FString, FString>> Links;
			for (FDcrModelNode& Node : Graph.Nodes)
			{
				if (Node.Kind != EDcrNodeKind::VariableGet || Node.InjectedOn.IsEmpty() || !Node.bInjectedAsInput)
				{
					continue;
				}
				Links.Emplace(Node.Name + TEXT(".") + (Node.InjectedOutputPin.IsEmpty() ? FString(TEXT("Value")) : Node.InjectedOutputPin), Node.InjectedOn);
				Node.InjectedOn.Reset();
				Node.InjectedInputPin.Reset();
				Node.InjectedOutputPin.Reset();
			}
			for (const TPair<FString, FString>& Link : Links)
			{
				// The link may be there already, as the injection's own.
				if (!Graph.Links.ContainsByPredicate([&Link](const FDcrModelLink& Existing) { return Existing.Source == Link.Key && Existing.Target == Link.Value; }))
				{
					AddLink(Graph, Link.Key, Link.Value);
				}
			}
		}

		/**
		 * A collapse node is written as an Inline function of its name, and the engine makes node names without
		 * spaces (an older rig may still have them): the name as a node made now would have it.
		 */
		void SanitizeCollapseNames(FDcrModelGraph& Graph)
		{
			TSet<FString> Names;
			for (const FDcrModelNode& Node : Graph.Nodes)
			{
				Names.Add(Node.Name);
			}
			TMap<FString, FString> Renamed;
			for (FDcrModelNode& Node : Graph.Nodes)
			{
				if (Node.Kind != EDcrNodeKind::Collapse)
				{
					continue;
				}
				FString Sanitized = Node.Name;
				URigVMSchema::SanitizeName(Sanitized, false, false);
				if (Sanitized == Node.Name || Sanitized.IsEmpty())
				{
					continue;
				}
				FString Unique = Sanitized;
				for (int32 Suffix = 1; Names.Contains(Unique); ++Suffix)
				{
					Unique = FString::Printf(TEXT("%s_%d"), *Sanitized, Suffix);
				}
				Names.Add(Unique);
				Renamed.Add(Node.Name, Unique);
				Node.Name = Unique;
			}
			if (Renamed.IsEmpty())
			{
				return;
			}
			auto Rename = [&Renamed](FString& Path)
			{
				FString Node;
				FString Rest;
				SplitDcrPinPath(Path, Node, Rest);
				if (const FString* New = Renamed.Find(Node))
				{
					Path = Rest.IsEmpty() ? *New : *New + TEXT(".") + Rest;
				}
			};
			for (FDcrModelLink& Link : Graph.Links)
			{
				Rename(Link.Source);
				Rename(Link.Target);
			}
			for (FDcrModelNode& Node : Graph.Nodes)
			{
				if (!Node.InjectedOn.IsEmpty())
				{
					Rename(Node.InjectedOn);
				}
			}
		}

		/**
		 * Older rigs name the execute pin of a function's entry, return and calls `Execute`, and a collapse made
		 * from a selection may name it after the pin it was cut from (`Completed`); a function made now names it
		 * `ExecuteContext`. The name says nothing about what runs.
		 */
		void RenameFunctionExecutePins(FDcrModelGraph& Graph)
		{
			static const FString Current(TEXT("ExecuteContext"));
			// The old names of a node's execute input and output: a collapse may have one of each, named apart.
			TMap<FString, FString> RenamedIn;
			TMap<FString, FString> RenamedOut;
			for (FDcrModelNode& Node : Graph.Nodes)
			{
				const bool bFunctionNode = Node.Kind == EDcrNodeKind::Entry || Node.Kind == EDcrNodeKind::Return || Node.Kind == EDcrNodeKind::LocalCall
					|| Node.Kind == EDcrNodeKind::ExternalCall || Node.Kind == EDcrNodeKind::Collapse;
				if (!bFunctionNode || Node.ExecuteIn.Num() > 1 || Node.ExecuteOut.Num() > 1 || (Node.ExecuteIn.IsEmpty() && Node.ExecuteOut.IsEmpty()))
				{
					continue;
				}
				if (Node.ExecuteIn.Num() == 1 && Node.ExecuteIn[0] != Current)
				{
					RenamedIn.Add(Node.Name, Node.ExecuteIn[0]);
					Node.ExecuteIn[0] = Current;
				}
				if (Node.ExecuteOut.Num() == 1 && Node.ExecuteOut[0] != Current)
				{
					RenamedOut.Add(Node.Name, Node.ExecuteOut[0]);
					Node.ExecuteOut[0] = Current;
				}
			}
			auto Rename = [](FString& End, const TMap<FString, FString>& Renamed)
			{
				FString Node;
				FString Pin;
				SplitDcrPinPath(End, Node, Pin);
				const FString* Old = Renamed.Find(Node);
				if (Old && Pin == *Old)
				{
					End = Node + TEXT(".") + Current;
				}
			};
			for (FDcrModelLink& Link : Graph.Links)
			{
				Rename(Link.Source, RenamedOut);
				Rename(Link.Target, RenamedIn);
			}
		}

		/**
		 * A pure function's graph may link its entry straight to its return by execute pins, with nothing between:
		 * nothing runs along them, as in a pure function's graph without them. (One that runs code keeps them.)
		 */
		void DropEmptyPureExecution(FDcrModelGraph& Graph)
		{
			const FDcrModelNode* Entry = Graph.Nodes.FindByPredicate([](const FDcrModelNode& Node) { return Node.Kind == EDcrNodeKind::Entry; });
			const FDcrModelNode* Return = Graph.Nodes.FindByPredicate([](const FDcrModelNode& Node) { return Node.Kind == EDcrNodeKind::Return; });
			if (!Entry || !Return)
			{
				return;
			}
			const FString EntryPrefix = Entry->Name + TEXT(".");
			const FString ReturnPrefix = Return->Name + TEXT(".");
			auto IsExecuteOut = [Entry, &EntryPrefix](const FString& Path) { return Path.StartsWith(EntryPrefix) && Entry->ExecuteOut.Contains(Path.Mid(EntryPrefix.Len())); };
			const bool bRuns = Graph.Links.ContainsByPredicate([&IsExecuteOut, &ReturnPrefix](const FDcrModelLink& Link)
			{
				return IsExecuteOut(Link.Source) && !Link.Target.StartsWith(ReturnPrefix);
			});
			if (bRuns)
			{
				return;
			}
			Graph.Links.RemoveAll([&IsExecuteOut](const FDcrModelLink& Link) { return IsExecuteOut(Link.Source); });
			for (FDcrModelNode& Node : Graph.Nodes)
			{
				if (Node.Kind == EDcrNodeKind::Entry || Node.Kind == EDcrNodeKind::Return)
				{
					Node.ExecuteIn.Reset();
					Node.ExecuteOut.Reset();
				}
			}
		}

		/** A collapse graph's inputs keep their defaults on its entry: as a function's, no -0 and nothing for a new value. */
		void CanonicalizeEntryDefaults(FDcrModelGraph& Graph)
		{
			for (FDcrModelNode& Node : Graph.Nodes)
			{
				if (Node.Kind != EDcrNodeKind::Entry || Node.ResolvedTypes.IsEmpty())
				{
					continue;
				}
				for (int32 Index = Node.Values.Num() - 1; Index >= 0; --Index)
				{
					FDcrValue& Value = Node.Values[Index];
					const TPair<FString, FDcrType>* Type = Node.ResolvedTypes.FindByPredicate([&Value](const TPair<FString, FDcrType>& Resolved) { return Resolved.Key == Value.Name; });
					if (!Type)
					{
						continue;
					}
					Value.Value = MakeDcrCanonicalDefault(Type->Value, Value.Value);
					if (Value.Value.IsEmpty())
					{
						Node.Values.RemoveAt(Index);
					}
				}
			}
		}

		void NormalizeGraph(FDcrModelGraph& Graph, const FString& Location, const FDcrModel& Model, const FDcrModelFunction* Enclosing, FDcrNormalizeReport& Report)
		{
			CanonicalizeEntryDefaults(Graph);
			if (Enclosing && !Enclosing->bMutable)
			{
				DropEmptyPureExecution(Graph);
			}
			for (FDcrModelNode& Node : Graph.Nodes)
			{
				if (Node.Contained.IsValid())
				{
					// A collapse node without execute pins is pure: its graph's entry and return are as a pure function's.
					if (Node.Kind == EDcrNodeKind::Collapse && Node.ExecuteIn.IsEmpty() && Node.ExecuteOut.IsEmpty())
					{
						DropEmptyPureExecution(*Node.Contained);
					}
					NormalizeGraph(*Node.Contained, Location + TEXT("/") + Node.Name, Model, nullptr, Report);
				}
			}
			SanitizeCollapseNames(Graph);
			SpellVariablesAsDeclared(Graph, Model, Enclosing);
			RenameFunctionExecutePins(Graph);
			ReleaseInjectedGetters(Graph);
			FoldReroutes(Graph, Location, Report);
			FlattenSequences(Graph, Location, Report);
			DropUnreadGetters(Graph, Report);
			DropLinkedValues(Graph, Report);
			ResetLinkedMembers(Graph, Model, Enclosing, Report);
		}
	}

	namespace NormalizePrivate
	{
		/**
		 * Bones the preview mesh gives a few units in the last place off what the rig saved (an older asset worked
		 * them out differently) hold what importing gives: the text writes `Import = Mesh;`, which is that.
		 */
		void SnapHierarchyToImport(FDcrModel& Model)
		{
			const FDcrValue* Preview = Model.Settings.FindByPredicate([](const FDcrValue& Setting) { return Setting.Name == TEXT("PreviewSkeletalMesh"); });
			const FString MeshPath = Preview ? Preview->Value.TrimQuotes() : FString();
			TArray<FDcrModelElement> Imported;
			if (MeshPath.IsEmpty() || !ImportDcrHierarchy(MeshPath, Imported))
			{
				return;
			}
			const FDcrType TransformType = MakeDcrType(*TBaseStructure<FTransform>::Get());
			for (FDcrModelElement& Element : Model.Hierarchy)
			{
				const FDcrModelElement* Import = Imported.FindByPredicate([&Element](const FDcrModelElement& Candidate)
				{
					return Candidate.Kind == Element.Kind && Candidate.Name == Element.Name && Candidate.Parent == Element.Parent;
				});
				if (!Import)
				{
					continue;
				}
				auto IsTransform = [](const FDcrValue& Value) { return Value.Name == TEXT("Transform"); };
				FDcrValue* Own = Element.Properties.FindByPredicate(IsTransform);
				const FDcrValue* Theirs = Import->Properties.FindByPredicate(IsTransform);
				const FString OwnText = Own ? Own->Value : FString();
				const FString TheirText = Theirs ? Theirs->Value : FString();
				if (OwnText == TheirText || !AreDcrValueTextsNear(TransformType, OwnText, TheirText, DcrRoundingTolerance))
				{
					continue;
				}
				if (!Theirs)
				{
					Element.Properties.RemoveAll(IsTransform);
				}
				else if (Own)
				{
					Own->Value = TheirText;
				}
				else
				{
					Element.Properties.Add(*Theirs);
				}
			}
		}
	}

	namespace AsTextPrivate
	{
		FString NodeOfPath(const FString& PinPath)
		{
			int32 Dot = INDEX_NONE;
			return PinPath.FindChar(TEXT('.'), Dot) ? PinPath.Left(Dot) : PinPath;
		}

		/** A getter that feeds several pins becomes one getter per link, the way each use of a variable in the text is one. */
		void SplitSharedGetters(FDcrModelGraph& Graph)
		{
			for (FDcrModelNode& Node : Graph.Nodes)
			{
				if (Node.Contained.IsValid())
				{
					SplitSharedGetters(*Node.Contained);
				}
			}
			TArray<FDcrModelNode> Added;
			for (const FDcrModelNode& Node : Graph.Nodes)
			{
				if (Node.Kind != EDcrNodeKind::VariableGet)
				{
					continue;
				}
				TArray<int32> LinkIndices;
				for (int32 Index = 0; Index < Graph.Links.Num(); ++Index)
				{
					if (NodeOfPath(Graph.Links[Index].Source) == Node.Name)
					{
						LinkIndices.Add(Index);
					}
				}
				for (int32 Count = 1; Count < LinkIndices.Num(); ++Count)
				{
					FDcrModelNode Copy;
					Copy.Name = FString::Printf(TEXT("%s_split_%d"), *Node.Name, Count);
					Copy.Kind = Node.Kind;
					Copy.Variable = Node.Variable;
					Copy.VariableType = Node.VariableType;
					Copy.bLocalVariable = Node.bLocalVariable;
					Copy.Values = Node.Values;
					FDcrModelLink& Link = Graph.Links[LinkIndices[Count]];
					Link.Source = Copy.Name + Link.Source.Mid(Node.Name.Len());
					Added.Add(MoveTemp(Copy));
				}
			}
			for (FDcrModelNode& Node : Added)
			{
				Graph.Nodes.Add(MoveTemp(Node));
			}
		}

		/** A function's input read through a getter node is a link from the Entry node's pin. */
		void FoldInputGetters(FDcrModelGraph& Graph, const TArray<FString>& InputNames)
		{
			const FDcrModelNode* Entry = Graph.Nodes.FindByPredicate([](const FDcrModelNode& Node) { return Node.Kind == EDcrNodeKind::Entry; });
			if (!Entry)
			{
				return;
			}
			const FString EntryName = Entry->Name;
			TArray<FString> Folded;
			for (const FDcrModelNode& Node : Graph.Nodes)
			{
				if (Node.Kind == EDcrNodeKind::VariableGet && !Node.bLocalVariable && InputNames.Contains(Node.Variable))
				{
					for (FDcrModelLink& Link : Graph.Links)
					{
						if (NodeOfPath(Link.Source) == Node.Name)
						{
							// `Getter.Value.Sub` -> `Entry.Input.Sub`
							FString Rest = Link.Source.Mid(Node.Name.Len() + 1);
							int32 Dot = INDEX_NONE;
							Rest = Rest.FindChar(TEXT('.'), Dot) ? Rest.Mid(Dot) : FString();
							Link.Source = EntryName + TEXT(".") + Node.Variable + Rest;
						}
					}
					Folded.Add(Node.Name);
				}
			}
			Graph.Nodes.RemoveAll([&Folded](const FDcrModelNode& Node) { return Folded.Contains(Node.Name); });
		}

		void FoldInputGettersIn(FDcrModelGraph& Graph, const TArray<FString>& InputNames)
		{
			for (FDcrModelNode& Node : Graph.Nodes)
			{
				if (Node.Contained.IsValid())
				{
					TArray<FString> Inner;
					for (const FDcrModelNode& Pin : Node.Contained->Nodes)
					{
						if (Pin.Kind == EDcrNodeKind::Entry)
						{
							for (const TPair<FString, FDcrType>& Resolved : Pin.ResolvedTypes)
							{
								Inner.Add(Resolved.Key);
							}
						}
					}
					FoldInputGettersIn(*Node.Contained, Inner);
				}
			}
			FoldInputGetters(Graph, InputNames);
		}

		/** The model of a rig made as alike to its text's model as the text allows. */
		void MakeAsTextHoldsItImpl(FDcrModel& Model)
		{
			for (FDcrModelGraph& Graph : Model.Graphs)
			{
				SplitSharedGetters(Graph);
				FoldInputGettersIn(Graph, TArray<FString>());
			}
			for (FDcrModelFunction& Function : Model.Functions)
			{
				SplitSharedGetters(Function.Graph);
				TArray<FString> Inputs;
				for (const FDcrModelFunctionPin& Input : Function.Inputs)
				{
					Inputs.Add(Input.Name);
				}
				FoldInputGettersIn(Function.Graph, Inputs);
			}
			// An empty graph holds nothing the text can say (the default graph left empty included).
			for (int32 Index = Model.Graphs.Num() - 1; Index >= 0 && Model.Graphs.Num() > 1; --Index)
			{
				if (Model.Graphs[Index].Nodes.IsEmpty())
				{
					Model.Graphs.RemoveAt(Index);
				}
			}
		}
	}

	void MakeDcrModelAsTextHoldsIt(FDcrModel& Model)
	{
		AsTextPrivate::MakeAsTextHoldsItImpl(Model);
	}

	FDcrNormalizeReport NormalizeDcrModel(FDcrModel& InModel)
	{
		FDcrNormalizeReport Report;
		NormalizePrivate::SnapHierarchyToImport(InModel);
		// Defaults the engine parses itself: no -0, and nothing for a new value of the type.
		for (FDcrModelVariable& Variable : InModel.Variables)
		{
			Variable.Default = MakeDcrCanonicalDefault(Variable.Type, Variable.Default);
		}
		for (FDcrModelFunction& Function : InModel.Functions)
		{
			for (FDcrModelVariable& Local : Function.Locals)
			{
				Local.Default = MakeDcrCanonicalDefault(Local.Type, Local.Default);
			}
			for (TArray<FDcrModelFunctionPin>* Pins : { &Function.Inputs, &Function.Outputs })
			{
				for (FDcrModelFunctionPin& Pin : *Pins)
				{
					Pin.Default = MakeDcrCanonicalDefault(Pin.Type, Pin.Default);
				}
			}
		}
		for (FDcrModelGraph& Graph : InModel.Graphs)
		{
			NormalizeGraph(Graph, Graph.Name, InModel, nullptr, Report);
		}
		for (FDcrModelFunction& Function : InModel.Functions)
		{
			NormalizeGraph(Function.Graph, TEXT("FunctionLibrary/") + Function.Name, InModel, &Function, Report);
		}
		return Report;
	}
}
