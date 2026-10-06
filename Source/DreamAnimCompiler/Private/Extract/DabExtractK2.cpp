// Copyright DreamMoon. All Rights Reserved.

#include "Extract/DabExtractPrivate.h"
#include "Kismet/KismetMathLibrary.h"

#include "AnimGraphNode_StateMachineBase.h"
#include "AnimStateNodeBase.h"
#include "AnimationStateMachineGraph.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/MemberReference.h"
#include "GameplayTagContainer.h"
#include "K2Node_AnimGetter.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Knot.h"
#include "K2Node_PromotableOperator.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Compiler::ExtractPrivate
{
	using namespace UE::DreamAnim::Lang;
	using Model::FDabModelArgument;
	using Model::FDabModelCall;

	void FDabLowering::Fail(const UEdGraphNode* Node, const FString& Description)
	{
		++Failures;
		Ctx.AddUnsupported(Node ? FString::Printf(TEXT("%s/%s"), *Location, *Node->GetName()) : Location, Description);
	}

	namespace
	{
		const UEdGraphSchema_K2& GetK2Schema()
		{
			return *GetDefault<UEdGraphSchema_K2>();
		}

		/** Comments, reroutes and disabled nodes change nothing a model holds. */
		bool IsInert(const UEdGraphNode* Node)
		{
			return !Node
				|| Node->IsA<UEdGraphNode_Comment>()
				|| Node->IsA<UK2Node_Knot>()
				|| !Node->IsNodeEnabled()
				|| Node->IsAutomaticallyPlacedGhostNode();
		}

		/** An input pin that carries a value: not exec, not the target, not hidden plumbing such as WorldContext. */
		bool IsDataInput(const UEdGraphPin* Pin)
		{
			return Pin
				&& Pin->Direction == EGPD_Input
				&& !UEdGraphSchema_K2::IsExecPin(*Pin)
				&& Pin->PinName != UEdGraphSchema_K2::PN_Self
				&& !Pin->bOrphanedPin
				&& Pin->ParentPin == nullptr
				&& (!Pin->bHidden || Pin->SubPins.Num() > 0);
		}

		TArray<const UEdGraphPin*> GetDataInputs(const UEdGraphNode& Node)
		{
			TArray<const UEdGraphPin*> Inputs;
			for (const UEdGraphPin* Pin : Node.Pins)
			{
				if (IsDataInput(Pin))
				{
					Inputs.Add(Pin);
				}
			}
			return Inputs;
		}

		const UEdGraphPin* GetLinkedSource(const UEdGraphPin* Pin)
		{
			return (Pin && Pin->LinkedTo.Num() > 0) ? Pin->LinkedTo[0] : nullptr;
		}

		/** The parameter a call pin stands for; pins an operator node adds (`C`, `D`) share the type of its last input. */
		const FProperty* GetParameterProperty(const UFunction* Function, const UEdGraphPin& Pin)
		{
			if (!Function)
			{
				return nullptr;
			}
			if (const FProperty* Parameter = FindFProperty<FProperty>(Function, Pin.PinName))
			{
				return Parameter->HasAnyPropertyFlags(CPF_Parm) && !Parameter->HasAnyPropertyFlags(CPF_ReturnParm) ? Parameter : nullptr;
			}

			const FProperty* LastInput = nullptr;
			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				if (!It->HasAnyPropertyFlags(CPF_ReturnParm | CPF_OutParm))
				{
					LastInput = *It;
				}
			}
			FEdGraphPinType ParameterType;
			if (LastInput && GetK2Schema().ConvertPropertyToPinType(LastInput, ParameterType)
				&& ParameterType.PinCategory == Pin.PinType.PinCategory
				&& ParameterType.PinSubCategoryObject == Pin.PinType.PinSubCategoryObject)
			{
				return LastInput;
			}
			return nullptr;
		}

		/** The node an exec output leads to, through reroutes. */
		const UEdGraphNode* FollowExec(FDabLowering& Lowering, const UEdGraphPin* Pin)
		{
			const UEdGraphPin* Current = Pin;
			while (const UEdGraphPin* Target = GetLinkedSource(Current))
			{
				const UEdGraphNode* Node = Target->GetOwningNodeUnchecked();
				if (const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node))
				{
					Lowering.Visited.Add(Knot);
					Current = Knot->GetOutputPin();
					continue;
				}
				return Node;
			}
			return nullptr;
		}

		FString DescribeFunction(const UK2Node_CallFunction& Node)
		{
			const UClass* Owner = Node.FunctionReference.GetMemberParentClass();
			if (const UFunction* Function = Node.GetTargetFunction())
			{
				Owner = Function->GetOwnerClass();
			}
			const FString Name = Node.FunctionReference.GetMemberName().ToString();
			return Owner ? FString::Printf(TEXT("%s::%s"), *Owner->GetName(), *Name) : Name;
		}

		struct FOperatorPrefix
		{
			const TCHAR* Prefix;
			EDabBinaryOp Op;
		};

		// Longer prefixes first: `GreaterEqual_` must win over `Greater_`.
		const FOperatorPrefix ComparisonPrefixes[] =
		{
			{TEXT("EqualEqual_"), EDabBinaryOp::Equal},
			{TEXT("NotEqual_"), EDabBinaryOp::NotEqual},
			{TEXT("GreaterEqual_"), EDabBinaryOp::GreaterEqual},
			{TEXT("Greater_"), EDabBinaryOp::Greater},
			{TEXT("LessEqual_"), EDabBinaryOp::LessEqual},
			{TEXT("Less_"), EDabBinaryOp::Less},
		};

		const FOperatorPrefix ArithmeticPrefixes[] =
		{
			{TEXT("Add_"), EDabBinaryOp::Add},
			{TEXT("Subtract_"), EDabBinaryOp::Subtract},
			{TEXT("Multiply_"), EDabBinaryOp::Multiply},
			{TEXT("Divide_"), EDabBinaryOp::Divide},
		};

		template <SIZE_T N>
		bool MatchPrefix(const FString& Name, const FOperatorPrefix (&Table)[N], EDabBinaryOp& OutOp)
		{
			for (const FOperatorPrefix& Entry : Table)
			{
				if (Name.StartsWith(Entry.Prefix, ESearchCase::CaseSensitive))
				{
					OutOp = Entry.Op;
					return true;
				}
			}
			return false;
		}

		/** `Abs`, `Min`, `Max`, `Clamp` for the KismetMathLibrary functions of any numeric type; empty for others. */
		FString GetBuiltinName(const FString& Name)
		{
			if (Name == TEXT("Abs") || Name == TEXT("Abs_Int") || Name == TEXT("Abs_Int64"))
			{
				return TEXT("Abs");
			}
			if (Name == TEXT("Min") || Name == TEXT("FMin") || Name == TEXT("MinInt64"))
			{
				return TEXT("Min");
			}
			if (Name == TEXT("Max") || Name == TEXT("FMax") || Name == TEXT("MaxInt64"))
			{
				return TEXT("Max");
			}
			if (Name == TEXT("Clamp") || Name == TEXT("FClamp") || Name == TEXT("ClampInt64"))
			{
				return TEXT("Clamp");
			}
			return FString();
		}

		bool IsKnownOperatorName(const FString& Name)
		{
			EDabBinaryOp Op = EDabBinaryOp::And;
			return MatchPrefix(Name, ComparisonPrefixes, Op) || MatchPrefix(Name, ArithmeticPrefixes, Op);
		}

		FDabExprPtr LowerOutputPin(FDabLowering& Lowering, const UEdGraphPin& Pin, int32 Depth);

		FDabExprPtr LowerVariableGet(FDabLowering& Lowering, const UK2Node_VariableGet& Node, int32 Depth)
		{
			const FMemberReference& Reference = Node.VariableReference;
			const FName Name = Reference.GetMemberName();

			if (const UEdGraphPin* Target = GetLinkedSource(Node.FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input)))
			{
				return MakeMember(LowerOutputPin(Lowering, *Target, Depth + 1), Name.ToString());
			}
			if (Reference.IsLocalScope())
			{
				Lowering.Fail(&Node, FString::Printf(TEXT("reads the local variable %s"), *Name.ToString()));
				return MakePlaceholder(Name.ToString());
			}

			const UClass* Parent = Reference.GetMemberParentClass();
			UClass* Instance = Lowering.Ctx.InstanceClass;
			if (!Reference.IsSelfContext() && Parent && Instance && !Instance->IsChildOf(Parent))
			{
				Lowering.Fail(&Node, FString::Printf(TEXT("reads %s::%s without a target"), *Parent->GetName(), *Name.ToString()));
				return MakePlaceholder(Name.ToString());
			}

			const FProperty* Property = Instance ? FindFProperty<FProperty>(Instance, Name) : nullptr;
			const UClass* Owner = Property ? Property->GetOwnerClass() : nullptr;
			const bool bBlueprintVariable = Lowering.Ctx.BlueprintVariables.Contains(Name) || (Owner && !Owner->HasAnyClassFlags(CLASS_Native));
			return MakeMember(MakeName(bBlueprintVariable ? TEXT("Props") : TEXT("Self")), Name.ToString());
		}

		FDabExprPtr LowerAnimGetter(FDabLowering& Lowering, const UK2Node_AnimGetter& Node)
		{
			const FString Function = Node.FunctionReference.GetMemberName().ToString();
			if (Function == TEXT("GetInstanceStateWeight"))
			{
				const FString State = GetStateNodeName(Node.SourceStateNode.Get());
				if (State.IsEmpty())
				{
					Lowering.Fail(&Node, TEXT("a state weight getter without a state"));
					return MakePlaceholder(Function);
				}
				TArray<FDabExprPtr> Args;
				Args.Add(MakeName(State));
				return MakeCall(MakeName(TEXT("StateWeight")), MoveTemp(Args));
			}
			if (Function == TEXT("GetInstanceCurrentStateElapsedTime"))
			{
				return MakeCall(MakeName(TEXT("StateTime")), TArray<FDabExprPtr>());
			}
			if (Function == TEXT("GetInstanceMachineWeight"))
			{
				const UAnimGraphNode_StateMachineBase* Machine = Cast<UAnimGraphNode_StateMachineBase>(Node.SourceNode.Get());
				const FString* MachineName = Machine ? Lowering.Ctx.MachineNames.Find(Machine->EditorStateMachineGraph.Get()) : nullptr;
				if (!MachineName)
				{
					Lowering.Fail(&Node, TEXT("a machine weight getter whose state machine is not in this Blueprint"));
					return MakePlaceholder(Function);
				}
				TArray<FDabExprPtr> Args;
				Args.Add(MakeName(*MachineName));
				return MakeCall(MakeName(TEXT("MachineWeight")), MoveTemp(Args));
			}
			if (Function == TEXT("GetRelevantAnimTimeRemaining"))
			{
				return MakeCall(MakeName(TEXT("TimeRemaining")), TArray<FDabExprPtr>());
			}
			if (Function == TEXT("GetRelevantAnimTimeRemainingFraction"))
			{
				return MakeCall(MakeName(TEXT("TimeRemainingRatio")), TArray<FDabExprPtr>());
			}
			Lowering.Fail(&Node, FString::Printf(TEXT("the anim getter %s has no counterpart in the language"), *Function));
			return MakePlaceholder(Function);
		}

		FDabExprPtr LowerPureCall(FDabLowering& Lowering, const UK2Node_CallFunction& Node, const UEdGraphPin& Output, int32 Depth)
		{
			const UFunction* Function = Node.GetTargetFunction();
			const FString FunctionName = Node.FunctionReference.GetMemberName().ToString();

			if (Node.GetReturnValuePin() != &Output)
			{
				Lowering.Fail(&Node, FString::Printf(TEXT("reads the output parameter %s of %s"), *Output.PinName.ToString(), *DescribeFunction(Node)));
				return MakePlaceholder(FunctionName);
			}

			// A promotable operator still on wildcards names no concrete function; its operation does.
			FString OperatorName = FunctionName;
			if (const UK2Node_PromotableOperator* Promotable = Cast<UK2Node_PromotableOperator>(&Node))
			{
				if (!IsKnownOperatorName(OperatorName))
				{
					OperatorName = Promotable->GetOperationName().ToString() + TEXT("_");
				}
			}

			const TArray<const UEdGraphPin*> Inputs = GetDataInputs(Node);
			const auto LowerArgument = [&Lowering, &Inputs, Function](int32 Index)
			{
				return LowerInputPin(Lowering, *Inputs[Index], GetParameterProperty(Function, *Inputs[Index]));
			};
			const auto RequireInputs = [&Lowering, &Node, &Inputs, &FunctionName](int32 Count)
			{
				if (Inputs.Num() < Count)
				{
					Lowering.Fail(&Node, FString::Printf(TEXT("%s has %d inputs, expected %d"), *FunctionName, Inputs.Num(), Count));
					return false;
				}
				return true;
			};
			const auto Chain = [&](EDabBinaryOp Op) -> FDabExprPtr
			{
				if (!RequireInputs(2))
				{
					return MakePlaceholder(FunctionName);
				}
				FDabExprPtr Result = LowerArgument(0);
				for (int32 Index = 1; Index < Inputs.Num(); ++Index)
				{
					Result = MakeBinary(Op, MoveTemp(Result), LowerArgument(Index));
				}
				return Result;
			};

			if (OperatorName == TEXT("BooleanAND"))
			{
				return Chain(EDabBinaryOp::And);
			}
			if (OperatorName == TEXT("BooleanOR"))
			{
				return Chain(EDabBinaryOp::Or);
			}

			EDabBinaryOp Op = EDabBinaryOp::And;
			if (MatchPrefix(OperatorName, ArithmeticPrefixes, Op))
			{
				return Chain(Op);
			}
			if (MatchPrefix(OperatorName, ComparisonPrefixes, Op))
			{
				if (!RequireInputs(2))
				{
					return MakePlaceholder(FunctionName);
				}
				// Float and vector equality take an ErrorTolerance; the language's `==` has none.
				for (int32 Index = 2; Index < Inputs.Num(); ++Index)
				{
					if (GetLinkedSource(Inputs[Index]) || !GetK2Schema().DoesDefaultValueMatchAutogenerated(*Inputs[Index]))
					{
						Lowering.Fail(&Node, FString::Printf(TEXT("%s sets %s, which `%s` cannot say"), *FunctionName, *Inputs[Index]->PinName.ToString(), GetDabBinaryOpSpelling(Op)));
					}
				}
				FDabExprPtr Lhs = LowerArgument(0);
				return MakeBinary(Op, MoveTemp(Lhs), LowerArgument(1));
			}

			if (OperatorName == TEXT("Not_PreBool"))
			{
				return RequireInputs(1) ? MakeUnary(EDabUnaryOp::Not, LowerArgument(0)) : MakePlaceholder(FunctionName);
			}

			const FString Builtin = GetBuiltinName(OperatorName);
			if (!Builtin.IsEmpty())
			{
				TArray<FDabExprPtr> Args;
				for (int32 Index = 0; Index < Inputs.Num(); ++Index)
				{
					Args.Add(LowerArgument(Index));
				}
				return MakeCall(MakeName(Builtin), MoveTemp(Args));
			}

			if (OperatorName == TEXT("IsGameplayTagValid"))
			{
				if (!RequireInputs(1))
				{
					return MakePlaceholder(FunctionName);
				}
				TArray<FDabExprPtr> Args;
				Args.Add(LowerArgument(0));
				return MakeCall(MakeName(TEXT("IsValid")), MoveTemp(Args));
			}

			if (OperatorName == TEXT("MatchesTag"))
			{
				const UEdGraphPin* ExactPin = Node.FindPin(TEXT("bExactMatch"), EGPD_Input);
				if (!RequireInputs(2))
				{
					return MakePlaceholder(FunctionName);
				}
				if (GetLinkedSource(ExactPin))
				{
					Lowering.Fail(&Node, TEXT("MatchesTag with a computed bExactMatch"));
				}
				const bool bExact = ExactPin && ExactPin->DefaultValue.ToBool();
				FDabExprPtr Lhs = LowerArgument(0);
				FDabExprPtr Rhs = LowerArgument(1);
				if (bExact)
				{
					// An exact match is plain equality.
					return MakeBinary(EDabBinaryOp::Equal, MoveTemp(Lhs), MoveTemp(Rhs));
				}
				TArray<FDabExprPtr> Args;
				Args.Add(MoveTemp(Lhs));
				Args.Add(MoveTemp(Rhs));
				return MakeCall(MakeName(TEXT("Matches")), MoveTemp(Args));
			}

			// Select<Type>(A, B, bPickA): what a generated `c ? a : b` is.
			if (OperatorName.StartsWith(TEXT("Select")) && Function && Function->GetOuterUClass() == UKismetMathLibrary::StaticClass()
				&& Inputs.Num() == 3)
			{
				FDabExprPtr Then = LowerArgument(0);
				FDabExprPtr Else = LowerArgument(1);
				return MakeConditional(LowerArgument(2), MoveTemp(Then), MoveTemp(Else));
			}

			if (OperatorName == TEXT("Conv_BoolToDouble") || OperatorName == TEXT("Conv_BoolToFloat")
				|| OperatorName == TEXT("Conv_BoolToInt") || OperatorName == TEXT("Conv_BoolToByte"))
			{
				return RequireInputs(1)
					? MakeConditional(LowerArgument(0), MakeNumber(1.0), MakeNumber(0.0))
					: MakePlaceholder(FunctionName);
			}

			// A getter on an object with no arguments is one step of a path: `Self.GetParent()`.
			const bool bStatic = Function && Function->HasAnyFunctionFlags(FUNC_Static);
			if (Function && !bStatic && Inputs.IsEmpty())
			{
				FDabExprPtr Target;
				if (const UEdGraphPin* SelfSource = GetLinkedSource(Node.FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input)))
				{
					Target = LowerOutputPin(Lowering, *SelfSource, Depth + 1);
				}
				else if (Lowering.Ctx.InstanceClass && Lowering.Ctx.InstanceClass->IsChildOf(Function->GetOwnerClass()))
				{
					Target = MakeName(TEXT("Self"));
				}
				if (Target.IsValid())
				{
					return MakeCall(MakeMember(MoveTemp(Target), FunctionName), TArray<FDabExprPtr>());
				}
			}

			Lowering.Fail(&Node, FString::Printf(TEXT("calls %s, which has no counterpart in the language"), *DescribeFunction(Node)));
			return MakePlaceholder(FunctionName);
		}

		FDabExprPtr LowerOutputPin(FDabLowering& Lowering, const UEdGraphPin& Pin, int32 Depth)
		{
			const UEdGraphNode* Node = Pin.GetOwningNodeUnchecked();
			if (!Node)
			{
				Lowering.Fail(nullptr, FString::Printf(TEXT("the pin %s has no node"), *Pin.PinName.ToString()));
				return MakePlaceholder(Pin.PinName.ToString());
			}
			if (Depth > 128)
			{
				Lowering.Fail(Node, TEXT("the expression nests too deep"));
				return MakePlaceholder(Node->GetName());
			}
			Lowering.Visited.Add(Node);

			// A split struct output reads one member of the whole.
			if (Pin.ParentPin)
			{
				FString Member = Pin.PinName.ToString();
				const FString Prefix = Pin.ParentPin->PinName.ToString() + TEXT("_");
				if (Member.StartsWith(Prefix, ESearchCase::CaseSensitive))
				{
					Member.RightChopInline(Prefix.Len());
				}
				return MakeMember(LowerOutputPin(Lowering, *Pin.ParentPin, Depth + 1), Member);
			}

			if (const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Node))
			{
				if (const UEdGraphPin* Source = GetLinkedSource(Knot->GetInputPin()))
				{
					return LowerOutputPin(Lowering, *Source, Depth + 1);
				}
				Lowering.Fail(Node, TEXT("a reroute node with nothing linked into it"));
				return MakePlaceholder(Node->GetName());
			}

			if (IsInstanceOfClassPath(Node, TEXT("/Script/PropertyAccessNode"), TEXT("K2Node_PropertyAccess")))
			{
				// A private class: its Path is read by reflection.
				const FArrayProperty* PathProperty = FindFProperty<FArrayProperty>(Node->GetClass(), TEXT("Path"));
				if (PathProperty && PathProperty->Inner->IsA<FStrProperty>())
				{
					const TArray<FString>& Segments = *PathProperty->ContainerPtrToValuePtr<TArray<FString>>(Node);
					return MakeAccessPath(Lowering.Ctx, Segments, FString::Printf(TEXT("%s/%s"), *Lowering.Location, *Node->GetName()));
				}
				Lowering.Fail(Node, TEXT("a property access node without a readable Path"));
				return MakePlaceholder(Node->GetName());
			}

			if (const UK2Node_VariableGet* Variable = Cast<UK2Node_VariableGet>(Node))
			{
				return LowerVariableGet(Lowering, *Variable, Depth);
			}
			if (Node->IsA<UK2Node_Self>())
			{
				return MakeName(TEXT("Self"));
			}
			if (const UK2Node_AnimGetter* Getter = Cast<UK2Node_AnimGetter>(Node))
			{
				return LowerAnimGetter(Lowering, *Getter);
			}
			if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
			{
				return LowerPureCall(Lowering, *Call, Pin, Depth);
			}
			if (const UK2Node_BreakStruct* Break = Cast<UK2Node_BreakStruct>(Node))
			{
				const TArray<const UEdGraphPin*> Inputs = GetDataInputs(*Break);
				if (Inputs.Num() == 1)
				{
					return MakeMember(LowerInputPin(Lowering, *Inputs[0], nullptr), Pin.PinName.ToString());
				}
			}

			Lowering.Fail(Node, FString::Printf(TEXT("a %s node cannot be expressed"), *Node->GetClass()->GetName()));
			return MakePlaceholder(Node->GetName());
		}

		/**
		 * A forwarder: a function that is an entry, one impure call on the exec chain (optionally followed by the
		 * result node) and the pure nodes feeding that call.
		 */
		TOptional<FDabModelCall> ExtractForwarder(FDabExtractContext& Ctx, const UEdGraph& Graph, const FString& Location)
		{
			const FString FunctionName = Graph.GetName();
			const auto NotForwarder = [&Ctx, &Location, &FunctionName](const FString& Reason) -> TOptional<FDabModelCall>
			{
				Ctx.AddUnsupported(Location, FString::Printf(TEXT("refers to the Blueprint function %s, which is not a forwarder: %s"), *FunctionName, *Reason));
				return TOptional<FDabModelCall>();
			};

			FDabLowering Lowering(Ctx, FunctionName);

			const UK2Node_FunctionEntry* Entry = nullptr;
			int32 EntryCount = 0;
			for (const UEdGraphNode* Node : Graph.Nodes)
			{
				if (const UK2Node_FunctionEntry* Candidate = Cast<UK2Node_FunctionEntry>(Node))
				{
					Entry = Candidate;
					++EntryCount;
				}
			}
			if (EntryCount != 1)
			{
				return NotForwarder(TEXT("it has no single entry node"));
			}
			Lowering.Visited.Add(Entry);

			const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(FollowExec(Lowering, GetK2Schema().FindExecutionPin(*Entry, EGPD_Output)));
			if (!Call || Call->IsNodePure() || Call->IsA<UK2Node_AnimGetter>())
			{
				return NotForwarder(TEXT("its entry does not lead to a function call"));
			}

			const UEdGraphNode* After = FollowExec(Lowering, GetK2Schema().FindExecutionPin(*Call, EGPD_Output));
			if (After)
			{
				if (!After->IsA<UK2Node_FunctionResult>())
				{
					return NotForwarder(FString::Printf(TEXT("%s follows the call"), *After->GetName()));
				}
				Lowering.Visited.Add(After);
			}

			TOptional<FDabModelCall> Result = LowerCallNode(Lowering, *Call);

			for (const UEdGraphNode* Node : Graph.Nodes)
			{
				if (!Lowering.Visited.Contains(Node) && !IsInert(Node) && !Node->IsA<UK2Node_FunctionResult>())
				{
					return NotForwarder(FString::Printf(TEXT("it also holds %s"), *Node->GetName()));
				}
			}
			return Result;
		}
	}

	FDabExprPtr LowerInputPin(FDabLowering& Lowering, const UEdGraphPin& Pin, const FProperty* Property)
	{
		const UEdGraphNode* Node = Pin.GetOwningNodeUnchecked();
		const FString PinLocation = FString::Printf(TEXT("%s/%s.%s"), *Lowering.Location, Node ? *Node->GetName() : TEXT("?"), *Pin.PinName.ToString());

		if (Pin.SubPins.Num() > 0)
		{
			Lowering.Fail(Node, FString::Printf(TEXT("the pin %s is split into its members"), *Pin.PinName.ToString()));
			return MakePlaceholder(Pin.PinName.ToString());
		}
		if (const UEdGraphPin* Source = GetLinkedSource(&Pin))
		{
			return LowerOutputPin(Lowering, *Source, 0);
		}
		// Enum and tag literals read best by the pin's type: a byte parameter of EqualEqual_ByteByte knows no enum,
		// and the pin says which one it is.
		const UObject* SubCategoryObject = Pin.PinType.PinSubCategoryObject.Get();
		const bool bEnumPin = Cast<UEnum>(SubCategoryObject) != nullptr;
		const bool bTagPin = SubCategoryObject == FGameplayTag::StaticStruct();
		if (Property && !bEnumPin && !bTagPin)
		{
			if (FDabExprPtr Value = ConvertPinDefaultAs(Lowering.Ctx, Pin, *Property, PinLocation))
			{
				return Value;
			}
		}
		return ConvertPinDefaultByType(Lowering.Ctx, Pin, PinLocation);
	}

	TOptional<FDabModelCall> LowerCallNode(FDabLowering& Lowering, const UK2Node_CallFunction& Node)
	{
		Lowering.Visited.Add(&Node);

		const UFunction* Function = Node.GetTargetFunction();
		if (Function && Function->HasAnyFunctionFlags(FUNC_Static))
		{
			Lowering.Fail(&Node, FString::Printf(TEXT("calls the library function %s; only functions on an object can be called"), *DescribeFunction(Node)));
			return TOptional<FDabModelCall>();
		}

		FDabModelCall Call;
		Call.Function = Node.FunctionReference.GetMemberName().ToString();

		if (const UEdGraphPin* Target = GetLinkedSource(Node.FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input)))
		{
			Call.Target = LowerOutputPin(Lowering, *Target, 0);
		}
		else if (!Function || (Lowering.Ctx.InstanceClass && Lowering.Ctx.InstanceClass->IsChildOf(Function->GetOwnerClass())))
		{
			Call.Target = MakeName(TEXT("Self"));
		}
		else
		{
			Lowering.Fail(&Node, FString::Printf(TEXT("calls %s without a target"), *DescribeFunction(Node)));
			return TOptional<FDabModelCall>();
		}

		// Arguments left at the function's own default stay out, as the text leaves them out.
		for (const UEdGraphPin* Pin : GetDataInputs(Node))
		{
			if (!GetLinkedSource(Pin) && Pin->SubPins.Num() == 0 && GetK2Schema().DoesDefaultValueMatchAutogenerated(*Pin))
			{
				continue;
			}
			FDabModelArgument Argument;
			Argument.Name = Pin->PinName.ToString();
			Argument.Value = LowerInputPin(Lowering, *Pin, GetParameterProperty(Function, *Pin));
			Call.Args.Add(MoveTemp(Argument));
		}
		return TOptional<FDabModelCall>(MoveTemp(Call));
	}

	TOptional<FDabModelCall> ExtractFunctionReference(FDabExtractContext& Ctx, const FMemberReference& Reference, const FString& Location)
	{
		const FName Name = Reference.GetMemberName();
		if (Name.IsNone())
		{
			return TOptional<FDabModelCall>();
		}

		if (const UEdGraph* const* Graph = Ctx.FunctionGraphs.Find(Name))
		{
			Ctx.UsedFunctions.Add(Name);
			if (!Ctx.Options.bRecogniseForwarders)
			{
				Ctx.AddUnsupported(Location, FString::Printf(TEXT("refers to the Blueprint function %s and forwarder recognition is off"), *Name.ToString()));
				return TOptional<FDabModelCall>();
			}
			return ExtractForwarder(Ctx, **Graph, Location);
		}

		const UFunction* Function = Ctx.InstanceClass ? Reference.ResolveMember<UFunction>(Ctx.InstanceClass, false) : nullptr;
		if (Function && Function->HasAnyFunctionFlags(FUNC_Static))
		{
			Ctx.AddUnsupported(Location, FString::Printf(TEXT("refers to the library function %s::%s; only functions on the instance can be called"),
				*Function->GetOwnerClass()->GetName(), *Name.ToString()));
			return TOptional<FDabModelCall>();
		}
		if (Function || Reference.IsSelfContext())
		{
			FDabModelCall Call;
			Call.Target = MakeName(TEXT("Self"));
			Call.Function = Name.ToString();
			return TOptional<FDabModelCall>(MoveTemp(Call));
		}

		Ctx.AddUnsupported(Location, FString::Printf(TEXT("refers to the function %s, which cannot be found"), *Name.ToString()));
		return TOptional<FDabModelCall>();
	}

	void ExtractEventGraph(FDabExtractContext& Ctx, const UEdGraph& Graph)
	{
		const FString GraphName = Graph.GetName();
		FDabLowering Lowering(Ctx, GraphName);

		for (const UEdGraphNode* Node : Graph.Nodes)
		{
			const UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
			if (!Event || IsInert(Event))
			{
				continue;
			}

			const FString EventName = Event->CustomFunctionName.IsNone()
				? Event->EventReference.GetMemberName().ToString()
				: Event->CustomFunctionName.ToString();
			static const FString NotifyPrefix(TEXT("AnimNotify_"));
			if (!EventName.StartsWith(NotifyPrefix, ESearchCase::CaseSensitive))
			{
				// Left unvisited: the event and what hangs off it are reported below, node by node.
				continue;
			}
			Lowering.Visited.Add(Event);

			Model::FDabModelNotify Notify;
			Notify.Name = EventName.RightChop(NotifyPrefix.Len());

			const UEdGraphNode* Next = FollowExec(Lowering, GetK2Schema().FindExecutionPin(*Event, EGPD_Output));
			while (Next)
			{
				Lowering.Visited.Add(Next);
				const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Next);
				if (!Call || Call->IsNodePure())
				{
					Ctx.AddUnsupported(FString::Printf(TEXT("%s/%s"), *GraphName, *Next->GetName()),
						FString::Printf(TEXT("a %s node in the %s event is Blueprint logic; only calls can be expressed"), *Next->GetClass()->GetName(), *EventName));
					break;
				}
				if (TOptional<FDabModelCall> Lowered = LowerCallNode(Lowering, *Call))
				{
					Notify.Calls.Add(MoveTemp(Lowered.GetValue()));
				}
				Next = FollowExec(Lowering, GetK2Schema().FindExecutionPin(*Call, EGPD_Output));
			}

			Ctx.OutModel.Notifies.Add(MoveTemp(Notify));
		}

		for (const UEdGraphNode* Node : Graph.Nodes)
		{
			if (Lowering.Visited.Contains(Node) || IsInert(Node))
			{
				continue;
			}
			const FString NodeLocation = FString::Printf(TEXT("%s/%s"), *GraphName, *Node->GetName());
			if (const UK2Node_Event* Event = Cast<UK2Node_Event>(Node))
			{
				const FString EventName = Event->CustomFunctionName.IsNone()
					? Event->EventReference.GetMemberName().ToString()
					: Event->CustomFunctionName.ToString();
				Ctx.AddUnsupported(NodeLocation, FString::Printf(TEXT("the event %s is Blueprint logic; move it to C++"), *EventName));
			}
			else if (const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
			{
				Ctx.AddUnsupported(NodeLocation, FString::Printf(TEXT("calls %s outside an AnimNotify event"), *DescribeFunction(*Call)));
			}
			else
			{
				Ctx.AddUnsupported(NodeLocation, FString::Printf(TEXT("a %s node outside an AnimNotify event"), *Node->GetClass()->GetName()));
			}
		}
	}
}
