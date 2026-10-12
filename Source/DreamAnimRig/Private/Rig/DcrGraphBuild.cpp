// Copyright DreamMoon. All Rights Reserved.
//
// One model graph into one RigVM graph, for top-level graphs, library functions and collapse nodes alike:
// the nodes in the model's order, each under the name the model gives it; then the pin values; then the
// links; then the layout. Every call is made with undo off.
//
// A collapse node is made the way the engine's own Python export makes it: its contents are built as a
// function in the library (with the exposed pins its Entry and Return stand for), a reference to that
// function is added to the graph, the reference is turned into a collapse node, and the function is removed.

#include "DcrGenerateContext.h"
#include "DcrLayout.h"
#include "DcrValues.h"

#include "RigVMCore/RigVMStruct.h"
#include "RigVMModel/Nodes/RigVMCollapseNode.h"
#include "RigVMModel/Nodes/RigVMCommentNode.h"
#include "RigVMModel/Nodes/RigVMFunctionEntryNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReferenceNode.h"
#include "RigVMModel/Nodes/RigVMInvokeEntryNode.h"
#include "RigVMModel/Nodes/RigVMFunctionReturnNode.h"
#include "RigVMModel/Nodes/RigVMLibraryNode.h"
#include "RigVMModel/Nodes/RigVMRerouteNode.h"
#include "RigVMModel/Nodes/RigVMTemplateNode.h"
#include "RigVMModel/RigVMController.h"
#include "RigVMModel/RigVMFunctionLibrary.h"
#include "RigVMModel/RigVMGraph.h"
#include "RigVMModel/RigVMLink.h"
#include "RigVMModel/RigVMNode.h"
#include "RigVMModel/RigVMPin.h"
#include "Rigs/RigModuleDefines.h"
#include "Units/Modules/RigUnit_ConnectionCandidates.h"
#include "Units/Modules/RigUnit_ConnectorExecution.h"
#include "UObject/StructOnScope.h"

namespace UE::DreamAnim::Rig::Gen
{
	using namespace UE::DreamAnim::Model;

	namespace
	{
		/** One pin the Entry or the Return of a collapse node stands for. */
		struct FInterfacePin
		{
			FString Name;
			FDcrType Type;
			FString Default;
		};

		/** What a collapse node's contents say its pins are. */
		struct FInterface
		{
			TArray<FInterfacePin> Inputs;
			TArray<FInterfacePin> Outputs;
			bool bMutable = false;
			/** Whether the Entry and Return nodes list the pins and their types; if not, the links tell. */
			bool bDeclared = false;
			FString ExecuteName;
		};

		const FDcrModelNode* FindKind(const FDcrModelGraph& Graph, EDcrNodeKind Kind)
		{
			return Graph.Nodes.FindByPredicate([Kind](const FDcrModelNode& Node) { return Node.Kind == Kind; });
		}

		/**
		 * The pins of a graph's Entry and Return. A model that comes from an asset lists them in the
		 * ResolvedTypes of those two nodes (name and type, the default in Values); the execute pins are the
		 * ones ExecuteOut and ExecuteIn name.
		 */
		FInterface ReadInterface(const FDcrModelGraph& Graph)
		{
			static const TArray<FString> NoNames;
			FInterface Interface;
			const FDcrModelNode* Entry = FindKind(Graph, EDcrNodeKind::Entry);
			const FDcrModelNode* Return = FindKind(Graph, EDcrNodeKind::Return);

			auto Read = [&Interface](const FDcrModelNode* Node, const TArray<FString>& Execute, TArray<FInterfacePin>& Out)
			{
				if (!Node)
				{
					return;
				}
				for (const TPair<FString, FDcrType>& Pair : Node->ResolvedTypes)
				{
					if (Execute.Contains(Pair.Key) || Pair.Value.CPPType.Contains(TEXT("ExecuteContext")))
					{
						continue;
					}
					FInterfacePin& Pin = Out.AddDefaulted_GetRef();
					Pin.Name = Pair.Key;
					Pin.Type = Pair.Value;
					for (const FDcrValue& Value : Node->Values)
					{
						if (Value.Name == Pair.Key)
						{
							Pin.Default = Value.Value;
						}
					}
					Interface.bDeclared = true;
				}
			};
			Read(Entry, Entry ? Entry->ExecuteOut : NoNames, Interface.Inputs);
			Read(Return, Return ? Return->ExecuteIn : NoNames, Interface.Outputs);

			if (Entry && !Entry->ExecuteOut.IsEmpty())
			{
				Interface.bMutable = true;
				Interface.ExecuteName = Entry->ExecuteOut[0];
			}
			if (Return && !Return->ExecuteIn.IsEmpty())
			{
				Interface.bMutable = true;
				if (Interface.ExecuteName.IsEmpty())
				{
					Interface.ExecuteName = Return->ExecuteIn[0];
				}
			}
			if (!Interface.bMutable)
			{
				// A model that does not list the execute pins can still link them.
				const FString EntryPrefix = (Entry ? Entry->Name : FString(TEXT("Entry"))) + TEXT(".ExecuteContext");
				const FString ReturnPrefix = (Return ? Return->Name : FString(TEXT("Return"))) + TEXT(".ExecuteContext");
				for (const FDcrModelLink& Link : Graph.Links)
				{
					if (Link.Source.StartsWith(EntryPrefix) || Link.Target.StartsWith(ReturnPrefix))
					{
						Interface.bMutable = true;
						break;
					}
				}
			}
			if (Interface.ExecuteName.IsEmpty())
			{
				Interface.ExecuteName = TEXT("ExecuteContext");
			}
			return Interface;
		}

		/** Adds the interface's pins as exposed pins of the function whose graph the controller edits. */
		void ExposeInterface(FContext& Ctx, URigVMController& FunctionController, const FInterface& Interface, const FString& Where)
		{
			auto Expose = [&](const FInterfacePin& Pin, ERigVMPinDirection Direction)
			{
				const FName Wanted(*Pin.Name);
				const FString Default = Direction == ERigVMPinDirection::Input ? MakeDcrEngineDefault(Pin.Type, Pin.Default) : Pin.Default;
				const FName Added = FunctionController.AddExposedPin(Wanted, Direction, Pin.Type.CPPType, TypeObjectPath(Pin.Type), Default, false, false, false);
				if (Added.IsNone())
				{
					Ctx.Error(Where, FString::Printf(TEXT("the pin '%s' (%s) could not be exposed"), *Pin.Name, *Pin.Type.CPPType));
				}
				else if (Added != Wanted)
				{
					Ctx.Error(Where, FString::Printf(TEXT("the pin '%s' was exposed as '%s'"), *Pin.Name, *Added.ToString()));
				}
			};
			for (const FInterfacePin& Pin : Interface.Inputs)
			{
				Expose(Pin, ERigVMPinDirection::Input);
			}
			for (const FInterfacePin& Pin : Interface.Outputs)
			{
				Expose(Pin, ERigVMPinDirection::Output);
			}
		}

		bool IsStringLike(const URigVMPin* Pin)
		{
			const FString Type = Pin->GetCPPType();
			return Type == TEXT("FString") || Type == TEXT("FName");
		}

		/** A guess at the size a node will be drawn with: the controller knows no sizes, and the layout needs some. */
		FVector2D EstimateSize(URigVMNode* Node)
		{
			if (Cast<URigVMRerouteNode>(Node))
			{
				return FVector2D(60.0, 40.0);
			}
			return FVector2D(260.0, 64.0 + 24.0 * Node->GetPins().Num());
		}

		/** The node a pin path starts with. */
		FString NodeOfPin(const FString& PinPath)
		{
			int32 Dot = INDEX_NONE;
			return PinPath.FindChar(TEXT('.'), Dot) ? PinPath.Left(Dot) : PinPath;
		}

		void BuildGraphPart(FContext& Ctx, URigVMGraph* Graph, const FDcrModelGraph& Source, const FString& Where, EGraphRole Role,
			TFunction<bool(const FDcrModelNode&)> Wanted, bool bAppend);

		class FGraphBuilder
		{
		public:
			FGraphBuilder(FContext& InCtx, URigVMGraph* InGraph, const FDcrModelGraph& InSource, const FString& InWhere, EGraphRole InRole)
				: Ctx(InCtx)
				, Graph(InGraph)
				, Source(InSource)
				, Where(InWhere)
				, Role(InRole)
			{
			}

			/**
			 * Only the nodes Wanted takes are made, and the links whose two nodes are there. With bAppend the graph has
			 * the others already: its interface and its execute links are left as they are.
			 */
			TFunction<bool(const FDcrModelNode&)> Wanted;
			bool bAppend = false;

			void Run()
			{
				Controller = Graph ? Ctx.Controller(Graph) : nullptr;
				if (!Controller)
				{
					Ctx.Error(Where, TEXT("the graph has no controller"));
					return;
				}

				// Everything but what sits on another node's pin, in the model's order.
				for (const FDcrModelNode& Node : Source.Nodes)
				{
					if (!Node.InjectedOn.IsEmpty())
					{
						continue;
					}
					switch (Node.Kind)
					{
					case EDcrNodeKind::Entry:
					case EDcrNodeKind::Return:
						// A function's or a collapse node's graph has them already; any other graph cannot.
						if (Role == EGraphRole::TopLevel)
						{
							Ctx.Warning(NodeWhere(Node), TEXT("a top-level graph has no Entry or Return; skipped"));
						}
						break;
					case EDcrNodeKind::Comment:
					case EDcrNodeKind::Reroute:
						break;
					case EDcrNodeKind::Unknown:
						Ctx.Warning(NodeWhere(Node), TEXT("a node of unknown kind is skipped"));
						break;
					default:
						AddNode(Node);
						break;
					}
				}
				for (const FDcrModelNode& Node : Source.Nodes)
				{
					if (!Node.InjectedOn.IsEmpty())
					{
						AddNode(Node);
					}
				}
				AddReroutes();
				for (const FDcrModelNode& Node : Source.Nodes)
				{
					if (Node.Kind == EDcrNodeKind::Comment)
					{
						AddNode(Node);
					}
				}

				if (Role == EGraphRole::Collapse && !bAppend)
				{
					InferInterface();
				}
				ApplyValues();
				AddLinks();
				if (!bAppend)
				{
					LinkEmptyExecution();
				}
				Arrange();
			}

			/**
			 * A function's graph whose entry runs nothing (a pure function's, or one whose code is empty) still has
			 * execute pins on its entry and return: execution goes straight from one to the other, as in a new function.
			 */
			void LinkEmptyExecution()
			{
				if (Role == EGraphRole::TopLevel)
				{
					return;
				}
				URigVMNode* Entry = Graph->GetEntryNode();
				URigVMNode* Return = Graph->GetReturnNode();
				URigVMPin* From = nullptr;
				URigVMPin* To = nullptr;
				for (URigVMPin* Pin : Entry ? Entry->GetPins() : TArray<URigVMPin*>())
				{
					From = From ? From : (Pin->IsExecuteContext() && Pin->GetDirection() != ERigVMPinDirection::Input ? Pin : nullptr);
				}
				for (URigVMPin* Pin : Return ? Return->GetPins() : TArray<URigVMPin*>())
				{
					To = To ? To : (Pin->IsExecuteContext() && Pin->GetDirection() != ERigVMPinDirection::Output ? Pin : nullptr);
				}
				if (From && To && From->GetLinkedTargetPins().IsEmpty() && To->GetLinkedSourcePins().IsEmpty())
				{
					Controller->AddLink(From, To, false, ERigVMPinDirection::Output, false, false);
				}
			}

		private:
			FContext& Ctx;
			URigVMGraph* Graph;
			URigVMController* Controller = nullptr;
			const FDcrModelGraph& Source;
			FString Where;
			EGraphRole Role;
			/** The model's names of the nodes that exist now. */
			TSet<FString> Built;

			FString NodeWhere(const FDcrModelNode& Node) const
			{
				return Where + TEXT("/") + Node.Name;
			}

			URigVMNode* Find(const FString& Name) const
			{
				return Graph->FindNodeByName(FName(*Name));
			}

			void AddNode(const FDcrModelNode& Node)
			{
				if (Wanted && !Wanted(Node))
				{
					return;
				}
				const FString Here = NodeWhere(Node);
				if (!Node.InjectedOn.IsEmpty() && Node.Kind != EDcrNodeKind::Unit)
				{
					Ctx.Error(Here, TEXT("only a unit can be injected on a pin"));
					return;
				}
				URigVMNode* Created = nullptr;
				switch (Node.Kind)
				{
				case EDcrNodeKind::Unit:
					if (!Node.InjectedOn.IsEmpty())
					{
						Created = AddInjected(Node);
					}
					else if (!Node.StructPath.IsEmpty())
					{
						Created = AddUnit(Node);
					}
					else if (!Node.Notation.IsEmpty())
					{
						Created = AddTemplate(Node);
					}
					else
					{
						Ctx.Error(Here, TEXT("a unit needs a struct or a notation"));
						return;
					}
					break;
				case EDcrNodeKind::Dispatch:
					Created = AddTemplate(Node);
					break;
				case EDcrNodeKind::VariableGet:
				case EDcrNodeKind::VariableSet:
					Created = AddVariable(Node);
					break;
				case EDcrNodeKind::LocalCall:
					Created = AddLocalCall(Node);
					break;
				case EDcrNodeKind::ExternalCall:
					Created = AddExternalCall(Node);
					break;
				case EDcrNodeKind::Collapse:
					Created = AddCollapse(Node);
					break;
				case EDcrNodeKind::InvokeEntry:
					Created = Controller->AddInvokeEntryNode(FName(*Node.Function), Node.Position, Node.Name, false, false);
					break;
				case EDcrNodeKind::Comment:
					Created = Controller->AddCommentNode(Node.CommentText, Node.Position, Node.CommentSize.IsZero() ? FVector2D(400.0, 300.0) : Node.CommentSize, FLinearColor::White, Node.Name, false, false);
					break;
				default:
					Ctx.Warning(Here, FString::Printf(TEXT("a %s node cannot be built here"), LexToString(Node.Kind)));
					return;
				}

				if (!Created)
				{
					// The cases that know why have said it already.
					if (!HasErrorFor(Here))
					{
						Ctx.Error(Here, FString::Printf(TEXT("the %s node could not be added"), LexToString(Node.Kind)));
					}
					return;
				}
				if (Created->GetFName() != FName(*Node.Name))
				{
					Ctx.Error(Here, FString::Printf(TEXT("the node was added as '%s'"), *Created->GetName()));
				}
				Built.Add(Node.Name);

				if (Node.Kind == EDcrNodeKind::Unit || Node.Kind == EDcrNodeKind::Dispatch)
				{
					ResolveTypes(Node);
					AddAggregatePins(Node);
					AddTraits(Node);
				}
			}

			/** A unit's traits, each with its value (a new node has none: the editor's workflows add them). */
			void AddTraits(const FDcrModelNode& Node)
			{
				for (const FDcrModelTrait& Trait : Node.Traits)
				{
					const FString Here = NodeWhere(Node) + TEXT(".") + Trait.Name;
					UScriptStruct* TraitStruct = LoadObject<UScriptStruct>(nullptr, *Trait.StructPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
					if (!TraitStruct)
					{
						Ctx.Error(Here, FString::Printf(TEXT("the trait struct '%s' cannot be found"), *Trait.StructPath));
						continue;
					}
					// The value the model holds, with the trait's own name in its `Name` member.
					FStructOnScope Value(TraitStruct);
					if (const FDcrValue* Written = Node.Values.FindByPredicate([&Trait](const FDcrValue& Candidate) { return Candidate.Name == Trait.Name; }))
					{
						TraitStruct->ImportText(*Written->Value, Value.GetStructMemory(), nullptr, PPF_None, nullptr, TraitStruct->GetName());
					}
					if (const FStrProperty* NameProperty = CastField<FStrProperty>(TraitStruct->FindPropertyByName(TEXT("Name"))))
					{
						NameProperty->SetPropertyValue_InContainer(Value.GetStructMemory(), Trait.Name);
					}
					const FName Added = Controller->AddTrait(FName(*Node.Name), FName(*Trait.StructPath), FName(*Trait.Name),
						ExportDcrStructText(*TraitStruct, Value.GetStructMemory()), INDEX_NONE, false, false);
					if (Added.IsNone())
					{
						Ctx.Error(Here, TEXT("the trait could not be added"));
					}
					else if (Added != FName(*Trait.Name))
					{
						Ctx.Error(Here, FString::Printf(TEXT("the trait was added as '%s'"), *Added.ToString()));
					}
				}
			}

			bool HasErrorFor(const FString& Here) const
			{
				for (const FString& Error : Ctx.Result.Errors)
				{
					if (Error.StartsWith(Here + TEXT(": ")))
					{
						return true;
					}
				}
				return false;
			}

			/**
			 * A unit by its struct. An event is added with its values in: the engine names an event from the values it
			 * is added with and refuses a second of the same name, so two user events would both start as the default.
			 */
			URigVMNode* AddUnit(const FDcrModelNode& Node)
			{
				UScriptStruct* Struct = LoadObject<UScriptStruct>(nullptr, *Node.StructPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
				if (Struct && Struct->IsChildOf(FRigVMStruct::StaticStruct()))
				{
					FStructOnScope Defaults(Struct);
					if (!reinterpret_cast<const FRigVMStruct*>(Defaults.GetStructMemory())->GetEventName().IsNone())
					{
						for (const FDcrValue& Value : Node.Values)
						{
							if (const FProperty* Property = Struct->FindPropertyByName(*Value.Name))
							{
								Property->ImportText_InContainer(*Value.Value, Defaults.GetStructMemory(), nullptr, PPF_None);
							}
						}
						return Controller->AddUnitNodeWithDefaults(Struct, FRigStructScope(Defaults), TEXT("Execute"), Node.Position, Node.Name, false, false);
					}
				}
				// The engine takes these into a module rig only, and older rigs and libraries hold them all the same: the
				// rig is a module while the node goes in.
				const bool bModuleOnly = Struct == FRigUnit_ConnectorExecution::StaticStruct() || Struct == FRigUnit_GetCandidates::StaticStruct()
					|| Struct == FRigUnit_DiscardMatches::StaticStruct();
				if (bModuleOnly && !Ctx.Rig.ControlRig->IsControlRigModule())
				{
					FRigModuleSettings& ModuleSettings = Ctx.Rig.ControlRig->GetRigModuleSettings();
					TGuardValue<FString> AsModule(ModuleSettings.Identifier.Name, TEXT("DreamAnim"));
					return Controller->AddUnitNodeFromStructPath(Node.StructPath, TEXT("Execute"), Node.Position, Node.Name, false, false);
				}
				return Controller->AddUnitNodeFromStructPath(Node.StructPath, TEXT("Execute"), Node.Position, Node.Name, false, false);
			}

			URigVMNode* AddTemplate(const FDcrModelNode& Node)
			{
				if (Node.Notation.IsEmpty())
				{
					Ctx.Error(NodeWhere(Node), TEXT("a dispatch needs a notation"));
					return nullptr;
				}
				return Controller->AddTemplateNode(FName(*Node.Notation), Node.Position, Node.Name, false, false);
			}

			/** The wildcard pins of a template node get their types; a pin that has one already is left alone. */
			void ResolveTypes(const FDcrModelNode& Node)
			{
				for (const TPair<FString, FDcrType>& Resolved : Node.ResolvedTypes)
				{
					URigVMNode* Current = Find(Node.Name);
					if (!Current)
					{
						return;
					}
					const FString PinWhere = NodeWhere(Node) + TEXT(".") + Resolved.Key;
					URigVMPin* Pin = Current->FindPin(Resolved.Key);
					if (!Pin)
					{
						Ctx.Error(PinWhere, TEXT("the pin does not exist"));
						continue;
					}
					if (!Pin->IsWildCard())
					{
						if (!PinHasType(Pin, Resolved.Value))
						{
							Ctx.Warning(PinWhere, FString::Printf(TEXT("has the type %s, the model says %s"), *Pin->GetCPPType(), *Resolved.Value.CPPType));
						}
						continue;
					}
					Controller->ResolveWildCardPin(Pin->GetPinPath(), Resolved.Value.CPPType, TypeObjectPath(Resolved.Value), false, false);
					Current = Find(Node.Name);
					Pin = Current ? Current->FindPin(Resolved.Key) : nullptr;
					if (!PinHasType(Pin, Resolved.Value))
					{
						Ctx.Error(PinWhere, FString::Printf(TEXT("could not be resolved to %s"), *Resolved.Value.CPPType));
					}
				}
			}

			/** Aggregate pins turn the node into an aggregate node that keeps its name. */
			void AddAggregatePins(const FDcrModelNode& Node)
			{
				for (const FString& PinName : Node.AggregatePins)
				{
					const FString Added = Controller->AddAggregatePin(Node.Name, PinName, FString(), false, false);
					if (Added.IsEmpty())
					{
						Ctx.Error(NodeWhere(Node), FString::Printf(TEXT("the aggregate pin '%s' could not be added"), *PinName));
						return;
					}
					if (!Added.EndsWith(TEXT(".") + PinName))
					{
						Ctx.Error(NodeWhere(Node), FString::Printf(TEXT("the aggregate pin '%s' was added as '%s'"), *PinName, *Added));
					}
				}
				if (!Node.AggregatePins.IsEmpty() && !Find(Node.Name))
				{
					Ctx.Error(NodeWhere(Node), TEXT("the node is gone after its aggregate pins were added"));
				}
			}

			URigVMNode* AddVariable(const FDcrModelNode& Node)
			{
				if (Node.Variable.IsEmpty())
				{
					Ctx.Error(NodeWhere(Node), TEXT("a variable node needs a variable"));
					return nullptr;
				}
				FName VariableName(*Node.Variable);
				if (!Node.bLocalVariable)
				{
					// By the exact name: a function's input `debug` is not the rig's variable `Debug` (the map's keys ignore case).
					for (const TPair<FString, FName>& Member : Ctx.MemberVariables)
					{
						if (Member.Key.Equals(Node.Variable, ESearchCase::CaseSensitive))
						{
							VariableName = Member.Value;
							break;
						}
					}
				}
				// The engine finds the variable by its name and takes the type from it.
				URigVMNode* Created = Controller->AddVariableNode(VariableName, Node.VariableType.CPPType, nullptr, Node.Kind == EDcrNodeKind::VariableGet, FString(), Node.Position, Node.Name, false, false);
				if (!Created)
				{
					Ctx.Error(NodeWhere(Node), FString::Printf(TEXT("the variable '%s' does not exist here"), *Node.Variable));
				}
				return Created;
			}

			URigVMNode* AddLocalCall(const FDcrModelNode& Node)
			{
				URigVMFunctionLibrary* Library = Ctx.Library();
				URigVMLibraryNode* Definition = Library ? Library->FindFunction(FName(*Node.Function)) : nullptr;
				if (!Definition)
				{
					Ctx.Error(NodeWhere(Node), FString::Printf(TEXT("the function '%s' is not in this rig's library"), *Node.Function));
					return nullptr;
				}
				URigVMNode* Created = Controller->AddFunctionReferenceNode(Definition, Node.Position, Node.Name, false, false);
				if (!Created)
				{
					Ctx.Error(NodeWhere(Node), FString::Printf(TEXT("the call to '%s' could not be added"), *Node.Function));
				}
				return Created;
			}

			URigVMNode* AddExternalCall(const FDcrModelNode& Node)
			{
				// The asset path of the host works for both asset kinds; a class path (`_C`) is tried as well.
				TArray<FString> Hosts;
				Hosts.Add(Node.FunctionHost);
				if (Node.FunctionHost.EndsWith(TEXT("_C")))
				{
					Hosts.Add(Node.FunctionHost.LeftChop(2));
				}
				else
				{
					Hosts.Add(Node.FunctionHost + TEXT("_C"));
				}
				for (const FString& Host : Hosts)
				{
					if (URigVMNode* Created = Controller->AddExternalFunctionReferenceNode(Host, FName(*Node.Function), Node.Position, Node.Name, false, false))
					{
						return Created;
					}
				}
				Ctx.Error(NodeWhere(Node), FString::Printf(TEXT("the public function '%s' of '%s' could not be called"), *Node.Function, *Node.FunctionHost));
				return nullptr;
			}

			URigVMNode* AddInjected(const FDcrModelNode& Node)
			{
				const FString Here = NodeWhere(Node);
				if (Node.StructPath.IsEmpty())
				{
					Ctx.Error(Here, TEXT("only a unit can be injected on a pin"));
					return nullptr;
				}
				if (!Find(NodeOfPin(Node.InjectedOn)))
				{
					Ctx.Error(Here, FString::Printf(TEXT("the pin '%s' it is injected on has no node"), *Node.InjectedOn));
					return nullptr;
				}
				URigVMInjectionInfo* Info = Controller->AddInjectedNodeFromStructPath(Node.InjectedOn, Node.bInjectedAsInput, Node.StructPath, TEXT("Execute"),
					FName(*Node.InjectedInputPin), FName(*Node.InjectedOutputPin), Node.Name, false);
				if (!Info)
				{
					Ctx.Error(Here, FString::Printf(TEXT("could not be injected on '%s'"), *Node.InjectedOn));
					return nullptr;
				}
				return Info->Node;
			}

			URigVMNode* AddCollapse(const FDcrModelNode& Node)
			{
				const FString Here = NodeWhere(Node);
				URigVMFunctionLibrary* Library = Ctx.Library();
				URigVMController* LibraryController = Ctx.LibraryController();
				if (!Node.Contained || !Library || !LibraryController)
				{
					Ctx.Error(Here, Node.Contained ? TEXT("the rig has no function library to build a collapse node in") : TEXT("a collapse node needs its contents"));
					return nullptr;
				}

				const FInterface Interface = ReadInterface(*Node.Contained);
				TSet<FString> LocalNames;
				for (const FDcrModelVariable& Local : Node.Contained->Locals)
				{
					LocalNames.Add(Local.Name);
				}
				const bool bHasLocals = !LocalNames.IsEmpty();
				auto OnLocal = [&LocalNames](const FDcrModelNode& Inner)
				{
					return (Inner.Kind == EDcrNodeKind::VariableGet || Inner.Kind == EDcrNodeKind::VariableSet) && Inner.bLocalVariable && LocalNames.Contains(Inner.Variable);
				};
				URigVMLibraryNode* Temporary = LibraryController->AddFunctionToLibrary(FName(*(TEXT("DcrCollapse_") + Node.Name)), Interface.bMutable, FVector2D::ZeroVector, false, false);
				if (!Temporary)
				{
					Ctx.Error(Here, TEXT("the function its contents are built in could not be added"));
					return nullptr;
				}
				const FName TemporaryName = Temporary->GetFName();

				URigVMGraph* ContentGraph = Temporary->GetContainedGraph();
				URigVMController* ContentController = Ctx.Controller(ContentGraph);
				if (ContentGraph && ContentController)
				{
					if (Interface.bDeclared)
					{
						ExposeInterface(Ctx, *ContentController, Interface, Here);
					}
					ApplyExecutePinName(Ctx, ContentController, ContentGraph, *Node.Contained, Here);
					// The execute link a new function comes with is the model's to make.
					ClearGraph(Ctx, ContentGraph, Here);
					// Locals the collapse graph holds itself (older rigs). The engine checks a collapse node's variable nodes
					// against what its graph holds, and the graph holds nothing while it is made: their nodes come after.
					for (const FDcrModelVariable& Local : Node.Contained->Locals)
					{
						const FRigVMGraphVariableDescription Added = ContentController->AddLocalVariableFromObjectPath(FName(*Local.Name), Local.Type.CPPType,
							Local.Type.ObjectPath, MakeDcrEngineDefault(Local.Type, Local.Default), false);
						if (Added.Name != FName(*Local.Name))
						{
							Ctx.Error(Here + TEXT("/") + Local.Name, TEXT("the local variable could not be added"));
						}
					}
					if (bHasLocals)
					{
						BuildGraphPart(Ctx, ContentGraph, *Node.Contained, Here, EGraphRole::Collapse, [&OnLocal](const FDcrModelNode& Inner) { return !OnLocal(Inner); }, false);
					}
					else
					{
						BuildGraph(Ctx, ContentGraph, *Node.Contained, Here, EGraphRole::Collapse);
					}
				}
				const TArray<FRigVMGraphVariableDescription> Locals = ContentGraph ? ContentGraph->GetLocalVariables(false) : TArray<FRigVMGraphVariableDescription>();

				URigVMNode* Collapsed = nullptr;
				URigVMLibraryNode* Definition = Library->FindFunction(TemporaryName);
				if (Definition)
				{
					if (URigVMNode* Reference = Controller->AddFunctionReferenceNode(Definition, Node.Position, Node.Name, false, false))
					{
						const FName CollapsedName = Controller->PromoteFunctionReferenceNodeToCollapseNode(Reference->GetFName(), false, false, true);
						Collapsed = CollapsedName.IsNone() ? nullptr : Graph->FindNodeByName(CollapsedName);
						// Promoting clears the collapse graph's locals: they are given back, and their nodes made.
						URigVMGraph* CollapsedGraph = Collapsed ? CastChecked<URigVMCollapseNode>(Collapsed)->GetContainedGraph() : nullptr;
						if (TArray<FRigVMGraphVariableDescription>* CollapsedLocals = bHasLocals ? GetGraphLocals(CollapsedGraph) : nullptr)
						{
							*CollapsedLocals = Locals;
							BuildGraphPart(Ctx, CollapsedGraph, *Node.Contained, Here, EGraphRole::Collapse, OnLocal, true);
						}
					}
				}
				if (!Collapsed)
				{
					Ctx.Error(Here, TEXT("the collapse node could not be made from its function"));
				}
				// Promoting removes the function; if something went wrong on the way it is still there.
				if (Library->FindFunction(TemporaryName))
				{
					LibraryController->RemoveFunctionFromLibrary(TemporaryName, false, false);
				}
				return Collapsed;
			}

			/** A graph's own local variables, which the engine lets only its controller's function-graph calls change. */
			static TArray<FRigVMGraphVariableDescription>* GetGraphLocals(URigVMGraph* InGraph)
			{
				static const FArrayProperty* Property = FindFProperty<FArrayProperty>(URigVMGraph::StaticClass(), TEXT("LocalVariables"));
				return Property && InGraph ? Property->ContainerPtrToValuePtr<TArray<FRigVMGraphVariableDescription>>(InGraph) : nullptr;
			}

			/** The type a reroute carries is the type of what it is linked to. */
			bool InferRerouteType(const FDcrModelNode& Node, FDcrType& OutType) const
			{
				for (const FDcrModelLink& Link : Source.Links)
				{
					const bool bFeeds = NodeOfPin(Link.Target) == Node.Name;
					const bool bFed = NodeOfPin(Link.Source) == Node.Name;
					if (!bFeeds && !bFed)
					{
						continue;
					}
					const URigVMPin* Other = Graph->FindPin(bFeeds ? Link.Source : Link.Target);
					if (!Other || Other->IsWildCard() || Other->IsExecuteContext() || Other->GetNode() == Find(Node.Name))
					{
						continue;
					}
					OutType.CPPType = Other->GetCPPType();
					const UObject* TypeObject = Other->GetCPPTypeObject();
					OutType.ObjectPath = TypeObject ? TypeObject->GetPathName() : FString();
					return true;
				}
				return false;
			}

			/** Reroutes come after the nodes they connect, in rounds, because one may only be typed through another. */
			void AddReroutes()
			{
				TArray<const FDcrModelNode*> Pending;
				for (const FDcrModelNode& Node : Source.Nodes)
				{
					if (Node.Kind == EDcrNodeKind::Reroute && (!Wanted || Wanted(Node)))
					{
						Pending.Add(&Node);
					}
				}
				bool bProgress = true;
				while (!Pending.IsEmpty() && bProgress)
				{
					bProgress = false;
					for (int32 Index = 0; Index < Pending.Num();)
					{
						const FDcrModelNode& Node = *Pending[Index];
						FDcrType Type;
						if (!InferRerouteType(Node, Type))
						{
							++Index;
							continue;
						}
						URigVMNode* Created = Controller->AddFreeRerouteNode(Type.CPPType, TypeObjectPath(Type), false, NAME_None, FString(), Node.Position, Node.Name, false);
						if (!Created)
						{
							Ctx.Error(NodeWhere(Node), FString::Printf(TEXT("the reroute (%s) could not be added"), *Type.CPPType));
						}
						else
						{
							if (Created->GetFName() != FName(*Node.Name))
							{
								Ctx.Error(NodeWhere(Node), FString::Printf(TEXT("the reroute was added as '%s'"), *Created->GetName()));
							}
							Built.Add(Node.Name);
						}
						Pending.RemoveAt(Index);
						bProgress = true;
					}
				}
				for (const FDcrModelNode* Node : Pending)
				{
					Ctx.Warning(NodeWhere(*Node), TEXT("a reroute with no typed link to carry is skipped"));
				}
			}

			/** The pins of a collapse node's Entry and Return, read off the links when the nodes do not list them. */
			void InferInterface()
			{
				FInterface Interface = ReadInterface(Source);
				if (Interface.bDeclared)
				{
					return;
				}
				const FDcrModelNode* Entry = FindKind(Source, EDcrNodeKind::Entry);
				const FDcrModelNode* Return = FindKind(Source, EDcrNodeKind::Return);
				const FString EntryName = Entry ? Entry->Name : FString(TEXT("Entry"));
				const FString ReturnName = Return ? Return->Name : FString(TEXT("Return"));

				auto Note = [this, &Interface](TArray<FInterfacePin>& Into, const FString& RootPin, const FString& OtherPath)
				{
					if (RootPin == Interface.ExecuteName || RootPin == TEXT("ExecuteContext"))
					{
						return;
					}
					for (const FInterfacePin& Existing : Into)
					{
						if (Existing.Name == RootPin)
						{
							return;
						}
					}
					const URigVMPin* Other = Graph->FindPin(OtherPath);
					if (!Other || Other->IsWildCard() || Other->IsExecuteContext())
					{
						return;
					}
					FInterfacePin& Pin = Into.AddDefaulted_GetRef();
					Pin.Name = RootPin;
					Pin.Type.CPPType = Other->GetCPPType();
					const UObject* TypeObject = Other->GetCPPTypeObject();
					Pin.Type.ObjectPath = TypeObject ? TypeObject->GetPathName() : FString();
				};

				for (const FDcrModelLink& Link : Source.Links)
				{
					FString SourceNode, SourceRest, TargetNode, TargetRest;
					if (Link.Source.Split(TEXT("."), &SourceNode, &SourceRest) && SourceNode == EntryName && !SourceRest.Contains(TEXT(".")))
					{
						Note(Interface.Inputs, SourceRest, Link.Target);
					}
					if (Link.Target.Split(TEXT("."), &TargetNode, &TargetRest) && TargetNode == ReturnName && !TargetRest.Contains(TEXT(".")))
					{
						Note(Interface.Outputs, TargetRest, Link.Source);
					}
				}
				ExposeInterface(Ctx, *Controller, Interface, Where);
			}

			void SetValue(const FDcrModelNode& Node, const FDcrValue& Value)
			{
				const FString PinPath = Node.Name + TEXT(".") + Value.Name;
				const FString PinWhere = NodeWhere(Node) + TEXT(".") + Value.Name;
				URigVMPin* Pin = Graph->FindPin(PinPath);
				if (!Pin)
				{
					Ctx.Error(PinWhere, TEXT("the pin does not exist"));
					return;
				}
				// The engine refuses an empty value for anything but a string or a name; there is nothing to set then.
				if (Value.Value.IsEmpty() && !IsStringLike(Pin))
				{
					return;
				}
				if (!Controller->SetPinDefaultValue(PinPath, Value.Value, true, false, false, false, true))
				{
					Ctx.Error(PinWhere, FString::Printf(TEXT("the value '%s' was refused"), *Value.Value));
				}
			}

			void ApplyValues()
			{
				// A node injected on a pin comes second: setting the pin it sits on sets its own input as well.
				for (int32 Pass = 0; Pass < 2; ++Pass)
				{
					for (const FDcrModelNode& Node : Source.Nodes)
					{
						if ((Pass == 1) != !Node.InjectedOn.IsEmpty() || !Built.Contains(Node.Name))
						{
							continue;
						}
						for (const FDcrValue& Value : Node.Values)
						{
							// A trait's value went in with the trait.
							if (!Node.Traits.ContainsByPredicate([&Value](const FDcrModelTrait& Trait) { return Trait.Name == Value.Name; }))
							{
								SetValue(Node, Value);
							}
						}
					}
				}
			}

			/** Whether a link is the one that putting a node on a pin makes by itself. */
			bool IsInjectionLink(const FDcrModelLink& Link) const
			{
				for (const FDcrModelNode& Node : Source.Nodes)
				{
					if (Node.InjectedOn.IsEmpty())
					{
						continue;
					}
					if (Node.bInjectedAsInput ? (Link.Target == Node.InjectedOn && NodeOfPin(Link.Source) == Node.Name)
						: (Link.Source == Node.InjectedOn && NodeOfPin(Link.Target) == Node.Name))
					{
						return true;
					}
				}
				return false;
			}

			/** The pin at Path; an array on the way that is too short for it grows to take it, as a link into an element needs. */
			URigVMPin* FindOrGrowPin(const FString& Path)
			{
				if (URigVMPin* Pin = Graph->FindPin(Path))
				{
					return Pin;
				}
				TArray<FString> Segments;
				Path.ParseIntoArray(Segments, TEXT("."));
				FString Current = Segments.IsEmpty() ? FString() : Segments[0];
				for (int32 Index = 1; Index < Segments.Num(); ++Index)
				{
					const FString Next = Current + TEXT(".") + Segments[Index];
					URigVMPin* Parent = Graph->FindPin(Current);
					if (!Graph->FindPin(Next) && Parent && Parent->IsArray() && Segments[Index].IsNumeric())
					{
						const int32 Size = FCString::Atoi(*Segments[Index]) + 1;
						if (Parent->GetArraySize() < Size)
						{
							// Given no value, the engine copies the last element into the new ones: they start new instead.
							FDcrType ElementType;
							ElementType.CPPType = Parent->GetArrayElementCppType();
							if (const UObject* TypeObject = Parent->GetCPPTypeObject())
							{
								ElementType.ObjectPath = TypeObject->GetPathName();
							}
							FDcrValueSlot Element;
							const FString Fresh = Element.Init(ElementType) ? Element.ExportText() : FString();
							Controller->SetArrayPinSize(Current, Size, Fresh, false, false);
						}
					}
					Current = Next;
				}
				return Graph->FindPin(Path);
			}

			void AddLinks()
			{
				for (const FDcrModelLink& Link : Source.Links)
				{
					if (IsInjectionLink(Link))
					{
						continue;
					}
					// Building a part of the graph: a link waits for both of its nodes.
					if (Wanted && (!Find(NodeOfPin(Link.Source)) || !Find(NodeOfPin(Link.Target))))
					{
						continue;
					}
					URigVMPin* Output = Graph->FindPin(Link.Source);
					URigVMPin* Input = FindOrGrowPin(Link.Target);
					if (!Output || !Input)
					{
						Ctx.Error(Where, FString::Printf(TEXT("link '%s' -> '%s': the pin '%s' does not exist"), *Link.Source, *Link.Target, Output ? *Link.Target : *Link.Source));
						continue;
					}
					// Both ends go through the pin a link to them really ends at (a pin with a node injected on it).
					Output = Output->GetPinForLink();
					Input = Input->GetPinForLink();
					if (Output->IsLinkedTo(Input))
					{
						continue;
					}
					FString Reason;
					if (!Controller->AddLink(Output, Input, false, ERigVMPinDirection::Output, false, false, &Reason))
					{
						Ctx.Error(Where, FString::Printf(TEXT("link '%s' -> '%s': %s"), *Link.Source, *Link.Target, Reason.IsEmpty() ? TEXT("refused") : *Reason));
					}
				}
			}

			/** Positions and sizes: the layout (or the model's own positions), and the comments fitted around their nodes. */
			void Arrange()
			{
				TMap<FString, FVector2D> Sizes;
				for (const FDcrModelNode& Node : Source.Nodes)
				{
					if (Node.Kind == EDcrNodeKind::Comment)
					{
						continue;
					}
					if (URigVMNode* Existing = Find(Node.Name))
					{
						Sizes.Add(Node.Name, EstimateSize(Existing));
					}
				}

				TMap<FString, FVector2D> Positions;
				if (Ctx.Options.bAutoLayout)
				{
					LayoutGraphNodes(Source, Sizes, Positions);
				}
				else
				{
					for (const FDcrModelNode& Node : Source.Nodes)
					{
						if (Node.Kind != EDcrNodeKind::Comment)
						{
							Positions.Add(Node.Name, Node.Position);
						}
					}
				}
				for (const TPair<FString, FVector2D>& Entry : Positions)
				{
					const FDcrModelNode* Placed = Wanted ? Source.FindNode(Entry.Key) : nullptr;
					if (Placed && !Wanted(*Placed))
					{
						continue;
					}
					URigVMNode* Existing = Find(Entry.Key);
					if (Existing && Existing->GetPosition() != Entry.Value)
					{
						Controller->SetNodePosition(Existing, Entry.Value, false, false, false);
					}
				}

				// A comment of the text around no node stands left of the graph, one under another, where it holds none.
				FVector2D Corner(TNumericLimits<double>::Max(), TNumericLimits<double>::Max());
				for (const TPair<FString, FVector2D>& Entry : Positions)
				{
					Corner.X = FMath::Min(Corner.X, Entry.Value.X);
					Corner.Y = FMath::Min(Corner.Y, Entry.Value.Y);
				}
				if (Positions.IsEmpty())
				{
					Corner = FVector2D::ZeroVector;
				}
				int32 Loose = 0;

				for (const FDcrModelNode& Node : Source.Nodes)
				{
					if (Node.Kind != EDcrNodeKind::Comment || !Built.Contains(Node.Name))
					{
						continue;
					}
					URigVMNode* Existing = Find(Node.Name);
					if (!Existing)
					{
						continue;
					}
					FVector2D Position = Node.Position;
					FVector2D Size = Node.CommentSize.IsZero() ? FVector2D(400.0, 300.0) : Node.CommentSize;
					if (Node.CommentMembers.IsEmpty() && Node.CommentSize.IsZero())
					{
						Size = FVector2D(400.0, 200.0);
						Position = FVector2D(Corner.X - 600.0, Corner.Y + 250.0 * Loose++);
					}
					if (!Node.CommentMembers.IsEmpty() && !FitCommentBox(Node.CommentMembers, Positions, Sizes, Position, Size))
					{
						Ctx.Warning(NodeWhere(Node), TEXT("none of the nodes the comment goes around exists; it keeps its place"));
					}
					if (Existing->GetPosition() != Position)
					{
						Controller->SetNodePosition(Existing, Position, false, false, false);
					}
					if (Existing->GetSize() != Size)
					{
						Controller->SetNodeSize(Existing, Size, false, false, false);
					}
				}
			}
		};
	}

	void ClearGraph(FContext& Ctx, URigVMGraph* Graph, const FString& Where)
	{
		URigVMController* Controller = Graph ? Ctx.Controller(Graph) : nullptr;
		if (!Controller)
		{
			return;
		}

		// A node injected on a pin goes with the node it sits on.
		TArray<URigVMNode*> Doomed;
		for (URigVMNode* Node : Graph->GetNodes())
		{
			if (!Node || Node->IsInjected() || Node->IsA<URigVMFunctionEntryNode>() || Node->IsA<URigVMFunctionReturnNode>())
			{
				continue;
			}
			Doomed.Add(Node);
		}
		if (!Doomed.IsEmpty() && !Controller->RemoveNodes(Doomed, false, false))
		{
			// One at a time, so that a stubborn node does not keep the rest.
			for (URigVMNode* Node : Doomed)
			{
				if (Graph->GetNodes().Contains(Node) && !Controller->RemoveNode(Node, false, false))
				{
					Ctx.Error(Where, FString::Printf(TEXT("the node '%s' could not be removed"), *Node->GetName()));
				}
			}
		}

		// What is left linked is the Entry to the Return of a mutable function.
		const TArray<URigVMLink*> Links = Graph->GetLinks();
		for (URigVMLink* Link : Links)
		{
			URigVMPin* SourcePin = Link ? Link->GetSourcePin() : nullptr;
			URigVMPin* TargetPin = Link ? Link->GetTargetPin() : nullptr;
			if (SourcePin && TargetPin)
			{
				Controller->BreakLink(SourcePin, TargetPin, false);
			}
		}
	}

	void BuildGraph(FContext& Ctx, URigVMGraph* Graph, const FDcrModelGraph& Source, const FString& Where, EGraphRole Role)
	{
		FGraphBuilder Builder(Ctx, Graph, Source, Where, Role);
		Builder.Run();
	}

	namespace
	{
		void BuildGraphPart(FContext& Ctx, URigVMGraph* Graph, const FDcrModelGraph& Source, const FString& Where, EGraphRole Role,
			TFunction<bool(const FDcrModelNode&)> Wanted, bool bAppend)
		{
			FGraphBuilder Builder(Ctx, Graph, Source, Where, Role);
			Builder.Wanted = MoveTemp(Wanted);
			Builder.bAppend = bAppend;
			Builder.Run();
		}
	}

	void ApplyExecutePinName(FContext& Ctx, URigVMController* FunctionController, URigVMGraph* FunctionGraph, const FDcrModelGraph& Source, const FString& Where)
	{
		const FInterface Interface = ReadInterface(Source);
		URigVMLibraryNode* Library = FunctionGraph ? Cast<URigVMLibraryNode>(FunctionGraph->GetOuter()) : nullptr;
		URigVMPin* Execute = Library ? Library->FindExecutePin() : nullptr;
		if (!FunctionController || !Execute || !Interface.bMutable || Execute->GetName() == Interface.ExecuteName)
		{
			return;
		}
		if (!FunctionController->RenameExposedPin(Execute->GetFName(), FName(*Interface.ExecuteName), false, false))
		{
			Ctx.Warning(Where, FString::Printf(TEXT("the execute pin could not be renamed to '%s'"), *Interface.ExecuteName));
		}
	}
}
