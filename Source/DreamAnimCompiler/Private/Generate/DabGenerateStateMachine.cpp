// Copyright DreamMoon. All Rights Reserved.
//
// State machines: a model machine -> states, conduits, aliases, the entry and transitions in the
// machine node's graph. A state's graph is not built here: it is appended to the context's graph list
// and built with every other pose graph. Conditions and state functions need every machine and every
// function to exist, so they are deferred.

#include "Generate/DabGenerateInternal.h"

#include "AnimGraphNode_StateMachine.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimStateAliasNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "Animation/BlendProfile.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationTransitionGraph.h"
#include "EdGraph/EdGraph.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Compiler::GeneratePrivate
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	static void DabGenApplyStateFunctions(FDabGenContext& Ctx, UAnimStateNode* StateNode, const FDabModelState& State, const FString& Location)
	{
		UAnimGraphNode_StateResult* Result = StateNode->GetResultNodeInsideState();
		if (!Result)
		{
			Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: the state has no result node for its functions"), *Location));
			return;
		}

		const auto Apply = [&Ctx, Result, &Location](const TOptional<FDabModelCall>& Call, FMemberReference& Reference, FName MemberName)
		{
			if (!Call.IsSet())
			{
				return;
			}
			const FName FunctionName = DabGenResolveCallFunction(Ctx, Call.GetValue(), EDabGenForwarder::AnimUpdateCall, Location + TEXT(".") + MemberName.ToString());
			if (FunctionName.IsNone())
			{
				return;
			}
			Reference.SetSelfMember(FunctionName);
			// What picking the function in the details panel runs: resolve it into the runtime node, refresh extensions.
			FPropertyChangedEvent Event(FindFProperty<FProperty>(UAnimGraphNode_StateResult::StaticClass(), MemberName));
			Result->PostEditChangeProperty(Event);
		};

		Apply(State.OnEnter, Result->StateEntryFunction, GET_MEMBER_NAME_CHECKED(UAnimGraphNode_StateResult, StateEntryFunction));
		Apply(State.OnExit, Result->StateExitFunction, GET_MEMBER_NAME_CHECKED(UAnimGraphNode_StateResult, StateExitFunction));
		Apply(State.OnBlended, Result->StateFullyBlendedInFunction, GET_MEMBER_NAME_CHECKED(UAnimGraphNode_StateResult, StateFullyBlendedInFunction));
	}

	static void DabGenApplyTransitionOptions(FDabGenContext& Ctx, UAnimStateTransitionNode* Node, const TArray<FDabModelProperty>& Options, const FString& Location)
	{
		for (const FDabModelProperty& Option : Options)
		{
			if (!Option.Value)
			{
				continue;
			}
			if (const FDabExpr* Unsupported = DabGenFindUnsupported(*Option.Value))
			{
				Ctx.Sink.Error(TEXT("DAB7002"), FDabSpan(), FString::Printf(TEXT("%s: %s cannot be generated: %s"), *Location, *Option.Name, *DabGenPrint(*Unsupported)));
				continue;
			}

			// Since 5.6 the transition keeps its blend profile in a wrapper; the text still calls it BlendProfile.
			// The old field is still found under that name (`_DEPRECATED` is not part of it) but nothing reads it.
			const FProperty* LegacyProfile = FindFProperty<FProperty>(Node->GetClass(), TEXT("BlendProfile"));
			if (Option.Name == TEXT("BlendProfile") && (!LegacyProfile || LegacyProfile->HasAnyPropertyFlags(CPF_Deprecated)))
			{
				FString ObjectPath;
				const FDabNameExpr* None = StripDabParens(*Option.Value).As<FDabNameExpr>();
				if (None && None->Name == TEXT("None"))
				{
					Node->BlendProfileWrapper = FBlendProfileInterfaceWrapper();
				}
				else if (UBlendProfile* Profile = DabGenTryGetAssetPath(*Option.Value, ObjectPath) ? Cast<UBlendProfile>(DabGenLoadObject(ObjectPath)) : nullptr)
				{
					Node->BlendProfileWrapper.SetSkeletonBlendProfile(Profile);
				}
				else
				{
					Ctx.Sink.Error(TEXT("DAB7003"), FDabSpan(), FString::Printf(TEXT("%s: BlendProfile = %s: expects Path(...) to a blend profile"), *Location, *DabGenPrint(*Option.Value)));
				}
				continue;
			}

			FString Error;
			const EDabGenApply Result = DabGenApplyPath(Ctx, Node->GetClass(), Node, Option.Name, *Option.Value, Node, Error);
			if (Result == EDabGenApply::UnknownProperty)
			{
				Ctx.Sink.Error(TEXT("DAB7001"), FDabSpan(), FString::Printf(TEXT("%s: a transition has no property %s"), *Location, *Option.Name));
				continue;
			}
			if (Result == EDabGenApply::Failed)
			{
				Ctx.Sink.Error(TEXT("DAB7003"), FDabSpan(), FString::Printf(TEXT("%s: %s = %s: %s"), *Location, *Option.Name, *DabGenPrint(*Option.Value), *Error));
				continue;
			}
			if (Option.Name == TEXT("LogicType"))
			{
				// A custom blend needs its blend graph, which the node creates when the logic type is edited.
				FPropertyChangedEvent Event(FindFProperty<FProperty>(Node->GetClass(), TEXT("LogicType")));
				Node->PostEditChangeProperty(Event);
			}
		}
	}

	void DabGenBuildMachine(FDabGenContext& Ctx, UAnimGraphNode_StateMachine* Node, const FDabModelStateMachine& Machine, const FString& Location)
	{
		if (Ctx.Machines.Contains(Machine.Name))
		{
			Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: the state machine %s is used by a second node"), *Location, *Machine.Name));
			return;
		}
		UAnimationStateMachineGraph* MachineGraph = Node->EditorStateMachineGraph;
		if (!MachineGraph)
		{
			Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: the state machine node has no graph"), *Location));
			return;
		}

		TUniquePtr<FDabGenMachine> NewInfo = MakeUnique<FDabGenMachine>();
		NewInfo->Model = &Machine;
		NewInfo->Node = Node;
		NewInfo->Path = TEXT("Machine/") + Machine.Name;
		FDabGenMachine& Info = *NewInfo;
		Ctx.Machines.Add(Machine.Name, MoveTemp(NewInfo));

		// A machine's name is its graph's name.
		Node->OnRenameNode(Machine.Name);
		if (MachineGraph->GetName() != Machine.Name)
		{
			Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: the state machine graph cannot be named %s"), *Location, *Machine.Name));
		}
		if (MachineGraph->EntryNode)
		{
			Ctx.AssignGuid(MachineGraph->EntryNode, Info.Path, TEXT("#Entry"));
		}

		for (const FDabModelState& State : Machine.States)
		{
			if (Info.StateNodes.Contains(State.Name))
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: two states named %s"), *Info.Path, *State.Name));
				continue;
			}
			const FString StatePath = Info.Path + TEXT("/State/") + State.Name;

			FGraphNodeCreator<UAnimStateNode> Creator(*MachineGraph);
			UAnimStateNode* StateNode = Creator.CreateNode(false);
			Creator.Finalize();
			Ctx.AssignGuid(StateNode, Info.Path, TEXT("State/") + State.Name);
			// A state's name is its graph's name; renaming does not check for duplicates, the model is unique.
			StateNode->OnRenameNode(State.Name);
			if (StateNode->GetStateName() != State.Name)
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: the state's graph cannot be named %s"), *StatePath, *State.Name));
			}
			Info.StateNodes.Add(State.Name, StateNode);
			DabGenApplyObjectProperties(Ctx, StateNode, State.Properties, StatePath);

			TUniquePtr<FDabGenGraph> Graph = MakeUnique<FDabGenGraph>();
			Graph->Model = &State.Graph;
			Graph->Graph = StateNode->BoundGraph;
			Graph->Path = StatePath;
			Graph->ResultNode = StateNode->GetResultNodeInsideState();
			Graph->ResultPin = StateNode->GetPoseSinkPinInsideState();
			Graph->MachineName = Machine.Name;
			Ctx.Graphs.Add(MoveTemp(Graph));

			if (State.OnEnter.IsSet() || State.OnExit.IsSet() || State.OnBlended.IsSet())
			{
				const FDabModelState* StateModel = &State;
				Ctx.Deferred.Add([&Ctx, StateNode, StateModel, StatePath]()
				{
					DabGenApplyStateFunctions(Ctx, StateNode, *StateModel, StatePath);
				});
			}
		}

		for (const FDabModelConduit& Conduit : Machine.Conduits)
		{
			if (Info.StateNodes.Contains(Conduit.Name))
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: a conduit named %s like another state"), *Info.Path, *Conduit.Name));
				continue;
			}
			const FString ConduitPath = Info.Path + TEXT("/Conduit/") + Conduit.Name;

			FGraphNodeCreator<UAnimStateConduitNode> Creator(*MachineGraph);
			UAnimStateConduitNode* ConduitNode = Creator.CreateNode(false);
			Creator.Finalize();
			Ctx.AssignGuid(ConduitNode, Info.Path, TEXT("Conduit/") + Conduit.Name);
			ConduitNode->OnRenameNode(Conduit.Name);
			Info.StateNodes.Add(Conduit.Name, ConduitNode);

			UAnimationTransitionGraph* ConduitGraph = Cast<UAnimationTransitionGraph>(ConduitNode->BoundGraph);
			UAnimGraphNode_Base* Result = ConduitGraph ? ConduitGraph->GetResultNode() : nullptr;
			if (Result)
			{
				Ctx.AssignGuid(Result, ConduitPath, TEXT("#Result"));
			}
			const FDabExpr* Condition = Conduit.Condition.Get();
			FDabGenScope Scope;
			Scope.MachineName = Machine.Name;
			Ctx.Deferred.Add([&Ctx, Result, Condition, ConduitPath, Scope]()
			{
				DabGenBuildCondition(Ctx, Result, Condition, ConduitPath, Scope, ConduitPath);
			});
		}

		for (const FDabModelAlias& Alias : Machine.Aliases)
		{
			if (Info.StateNodes.Contains(Alias.Name))
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: an alias named %s like a state"), *Info.Path, *Alias.Name));
				continue;
			}
			FGraphNodeCreator<UAnimStateAliasNode> Creator(*MachineGraph);
			UAnimStateAliasNode* AliasNode = Creator.CreateNode(false);
			// PostPlacedNewNode keeps a name that is already unique.
			AliasNode->StateAliasName = Alias.Name;
			Creator.Finalize();
			if (AliasNode->StateAliasName != Alias.Name)
			{
				AliasNode->OnRenameNode(Alias.Name);
			}
			Ctx.AssignGuid(AliasNode, Info.Path, TEXT("Alias/") + Alias.Name);
			AliasNode->bGlobalAlias = Alias.bGlobal;
			for (const FString& StateName : Alias.States)
			{
				UAnimStateNodeBase* const* Target = Info.StateNodes.Find(StateName);
				if (!Target)
				{
					Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s/Alias/%s: no state %s to alias"), *Info.Path, *Alias.Name, *StateName));
					continue;
				}
				AliasNode->GetAliasedStates().Add(TWeakObjectPtr<UAnimStateNodeBase>(*Target));
			}
			Info.StateNodes.Add(Alias.Name, AliasNode);
		}

		if (!Machine.Entry.IsEmpty())
		{
			UAnimStateNodeBase* const* EntryState = Info.StateNodes.Find(Machine.Entry);
			UEdGraphPin* EntryPin = MachineGraph->EntryNode ? MachineGraph->EntryNode->GetOutputPin() : nullptr;
			UEdGraphPin* StateInput = EntryState ? (*EntryState)->GetInputPin() : nullptr;
			if (!EntryPin || !StateInput)
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: no state %s to enter"), *Info.Path, *Machine.Entry));
			}
			else
			{
				EntryPin->MakeLinkTo(StateInput);
			}
		}

		TMap<FString, int32> PairCounts;
		TMap<FString, UAnimStateTransitionNode*> SharedRules;
		for (const FDabModelTransition& Transition : Machine.Transitions)
		{
			const FString Pair = Transition.From + TEXT("->") + Transition.To;
			const int32 Occurrence = PairCounts.FindOrAdd(Pair)++;
			const FString Id = FString::Printf(TEXT("Transition/%s#%d"), *Pair, Occurrence);
			const FString TransitionPath = Info.Path + TEXT("/") + Id;

			UAnimStateNodeBase* const* From = Info.StateNodes.Find(Transition.From);
			UAnimStateNodeBase* const* To = Info.StateNodes.Find(Transition.To);
			if (!From || !To)
			{
				Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: no state %s"), *TransitionPath, From ? *Transition.To : *Transition.From));
				continue;
			}

			FGraphNodeCreator<UAnimStateTransitionNode> Creator(*MachineGraph);
			UAnimStateTransitionNode* TransitionNode = Creator.CreateNode(false);
			Creator.Finalize();
			// PostPlacedNewNode turns this on for nodes placed by hand; the text records it against the class
			// default, so start from that.
			TransitionNode->bAllowInertializationForSelfTransitions = GetDefault<UAnimStateTransitionNode>()->bAllowInertializationForSelfTransitions;
			TransitionNode->CreateConnections(*From, *To);
			Ctx.AssignGuid(TransitionNode, Info.Path, Id);
			DabGenApplyTransitionOptions(Ctx, TransitionNode, Transition.Options, TransitionPath);
			if (Transition.ConditionKind == EDabConditionKind::Auto)
			{
				TransitionNode->bAutomaticRuleBasedOnSequencePlayerInState = true;
			}

			bool bBuildRule = true;
			if (!Transition.SharedRuleName.IsEmpty())
			{
				if (UAnimStateTransitionNode** First = SharedRules.Find(Transition.SharedRuleName))
				{
					// Later users take the first one's rule graph; their own one is removed.
					TransitionNode->UseSharedRules(*First);
					bBuildRule = false;
				}
				else
				{
					TransitionNode->MakeRulesShareable(Transition.SharedRuleName);
					TransitionNode->SharedRulesGuid = Ctx.MakeGuid(Info.Path + TEXT("/Rule/") + Transition.SharedRuleName);
					SharedRules.Add(Transition.SharedRuleName, TransitionNode);
				}
			}

			if (bBuildRule)
			{
				UAnimationTransitionGraph* RuleGraph = Cast<UAnimationTransitionGraph>(TransitionNode->BoundGraph);
				UAnimGraphNode_Base* Result = RuleGraph ? RuleGraph->GetResultNode() : nullptr;
				if (Result)
				{
					Ctx.AssignGuid(Result, TransitionPath, TEXT("#Result"));
				}
				if (Transition.ConditionKind == EDabConditionKind::Expression)
				{
					const FDabExpr* Condition = Transition.Condition.Get();
					FDabGenScope Scope;
					Scope.MachineName = Machine.Name;
					Scope.FromState = Transition.From;
					if (!Condition)
					{
						// An expression condition left empty can never be taken; say so rather than build `true`.
						Ctx.Sink.Error(TEXT("DAB7002"), FDabSpan(), FString::Printf(TEXT("%s: the transition has no condition"), *TransitionPath));
					}
					else
					{
						Ctx.Deferred.Add([&Ctx, Result, Condition, TransitionPath, Scope]()
						{
							DabGenBuildCondition(Ctx, Result, Condition, TransitionPath, Scope, TransitionPath);
						});
					}
				}
			}
		}

		// Spread the machine out so the editor shows something readable: states in a row, transitions between.
		int32 Column = 0;
		for (UEdGraphNode* GraphNode : MachineGraph->Nodes)
		{
			if (GraphNode && !GraphNode->IsA<UAnimStateTransitionNode>() && GraphNode != MachineGraph->EntryNode.Get())
			{
				GraphNode->NodePosX = 300 + (Column % 4) * 320;
				GraphNode->NodePosY = (Column / 4) * 220;
				++Column;
			}
		}
	}
}
