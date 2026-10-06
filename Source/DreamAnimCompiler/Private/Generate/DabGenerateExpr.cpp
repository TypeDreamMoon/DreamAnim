// Copyright DreamMoon. All Rights Reserved.
//
// K2: expressions -> K2 nodes (transition and conduit rules, generated functions), and the functions
// the generator adds to a Blueprint: slot getters (DreamAnimAsset_<Slot>), expression functions bound to
// pins (DreamAnimExpr_<hash>) and call forwarders for state, node and Call Function nodes
// (DreamAnimCall_<Function>_<hash>). Every function is BlueprintThreadSafe: the graph calls them from the
// animation worker threads. Self-rooted reads become Property Access nodes, the thread-safe way to read
// the instance; a call target is a chain of pure getters (`Self.GetParent()`), as hand-written forwarders hold.

#include "Generate/DabGenerateInternal.h"

#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateNodeBase.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimationAsset.h"
#include "Animation/BlendSpace.h"
#include "DreamAnimLibrary.h"
#include "DreamAnimSet.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "GameplayTagContainer.h"
#include "K2Node_AnimGetter.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Compiler::GeneratePrivate
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	/** What an operand is, for picking the typed library function an operator compiles to. */
	enum class EDabGenKind : uint8
	{
		Unknown,
		Bool,
		Int,
		Int64,
		Real,
		Byte,
		Name,
		String,
		Text,
		Tag,
		TagContainer,
		Object,
		Class,
		Other,
	};

	static UClass* DabGenMathLibrary()
	{
		return FindObject<UClass>(nullptr, TEXT("/Script/Engine.KismetMathLibrary"));
	}

	static UClass* DabGenStringLibrary()
	{
		return FindObject<UClass>(nullptr, TEXT("/Script/Engine.KismetStringLibrary"));
	}

	static UClass* DabGenTagLibrary()
	{
		return FindObject<UClass>(nullptr, TEXT("/Script/GameplayTags.BlueprintGameplayTagLibrary"));
	}

	static const UFunction* DabGenUpdatePrototype()
	{
		const UClass* Library = FindObject<UClass>(nullptr, TEXT("/Script/AnimGraphRuntime.AnimExecutionContextLibrary"));
		return Library ? Library->FindFunctionByName(TEXT("Prototype_ThreadSafeAnimUpdateCall")) : nullptr;
	}

	static EDabGenKind DabGenKindOfType(const FEdGraphPinType& Type)
	{
		if (Type.ContainerType != EPinContainerType::None)
		{
			return EDabGenKind::Other;
		}
		const FName Category = Type.PinCategory;
		if (Category == UEdGraphSchema_K2::PC_Boolean)
		{
			return EDabGenKind::Bool;
		}
		if (Category == UEdGraphSchema_K2::PC_Int)
		{
			return EDabGenKind::Int;
		}
		if (Category == UEdGraphSchema_K2::PC_Int64)
		{
			return EDabGenKind::Int64;
		}
		if (Category == UEdGraphSchema_K2::PC_Real || Category == UEdGraphSchema_K2::PC_Float || Category == UEdGraphSchema_K2::PC_Double)
		{
			return EDabGenKind::Real;
		}
		if (Category == UEdGraphSchema_K2::PC_Byte || Category == UEdGraphSchema_K2::PC_Enum)
		{
			return EDabGenKind::Byte;
		}
		if (Category == UEdGraphSchema_K2::PC_Name)
		{
			return EDabGenKind::Name;
		}
		if (Category == UEdGraphSchema_K2::PC_String)
		{
			return EDabGenKind::String;
		}
		if (Category == UEdGraphSchema_K2::PC_Text)
		{
			return EDabGenKind::Text;
		}
		if (Category == UEdGraphSchema_K2::PC_Struct)
		{
			const UObject* Struct = Type.PinSubCategoryObject.Get();
			if (Struct == FGameplayTag::StaticStruct())
			{
				return EDabGenKind::Tag;
			}
			if (Struct == FGameplayTagContainer::StaticStruct())
			{
				return EDabGenKind::TagContainer;
			}
			return EDabGenKind::Other;
		}
		if (Category == UEdGraphSchema_K2::PC_Object || Category == UEdGraphSchema_K2::PC_Interface || Category == UEdGraphSchema_K2::PC_SoftObject)
		{
			return EDabGenKind::Object;
		}
		if (Category == UEdGraphSchema_K2::PC_Class || Category == UEdGraphSchema_K2::PC_SoftClass)
		{
			return EDabGenKind::Class;
		}
		return EDabGenKind::Other;
	}

	static EDabGenKind DabGenKindOfConstant(const FDabExpr& Expr)
	{
		double Number = 0.0;
		bool bValue = false;
		FString Path;
		if (DabGenTryGetNumber(Expr, Number))
		{
			return EDabGenKind::Real;
		}
		if (DabGenTryGetBool(Expr, bValue))
		{
			return EDabGenKind::Bool;
		}
		if (DabGenTryGetAssetPath(Expr, Path))
		{
			return EDabGenKind::Object;
		}
		const FDabExpr& Unwrapped = DabGenUnwrap(Expr);
		if (const FDabLiteralExpr* Literal = Unwrapped.As<FDabLiteralExpr>(); Literal && Literal->LiteralKind == EDabLiteralKind::String)
		{
			return EDabGenKind::String;
		}
		if (Unwrapped.Is<FDabNameExpr>())
		{
			return EDabGenKind::Name;
		}
		return EDabGenKind::Unknown;
	}

	static bool DabGenIsNumericKind(EDabGenKind Kind)
	{
		return Kind == EDabGenKind::Int || Kind == EDabGenKind::Int64 || Kind == EDabGenKind::Real || Kind == EDabGenKind::Byte;
	}

	static EDabGenKind DabGenUnifyKinds(EDabGenKind A, EDabGenKind B)
	{
		if (A == EDabGenKind::Unknown)
		{
			return B;
		}
		if (B == EDabGenKind::Unknown || A == B)
		{
			return A;
		}
		if (DabGenIsNumericKind(A) && DabGenIsNumericKind(B))
		{
			if (A == EDabGenKind::Real || B == EDabGenKind::Real)
			{
				return EDabGenKind::Real;
			}
			if (A == EDabGenKind::Int64 || B == EDabGenKind::Int64)
			{
				return EDabGenKind::Int64;
			}
			return EDabGenKind::Int;
		}
		return A;
	}

	/** The typed library function an operator compiles to: `EqualEqual_DoubleDouble`, `Less_IntInt`, `EqualEqual_GameplayTag`... */
	static bool DabGenFindOperator(EDabBinaryOp Op, EDabGenKind Kind, UClass*& OutLibrary, FString& OutFunction)
	{
		const TCHAR* OpName = nullptr;
		switch (Op)
		{
		case EDabBinaryOp::Equal:        OpName = TEXT("EqualEqual"); break;
		case EDabBinaryOp::NotEqual:     OpName = TEXT("NotEqual"); break;
		case EDabBinaryOp::Less:         OpName = TEXT("Less"); break;
		case EDabBinaryOp::LessEqual:    OpName = TEXT("LessEqual"); break;
		case EDabBinaryOp::Greater:      OpName = TEXT("Greater"); break;
		case EDabBinaryOp::GreaterEqual: OpName = TEXT("GreaterEqual"); break;
		case EDabBinaryOp::Add:          OpName = TEXT("Add"); break;
		case EDabBinaryOp::Subtract:     OpName = TEXT("Subtract"); break;
		case EDabBinaryOp::Multiply:     OpName = TEXT("Multiply"); break;
		case EDabBinaryOp::Divide:       OpName = TEXT("Divide"); break;
		default: return false;
		}

		UClass* Library = DabGenMathLibrary();
		const TCHAR* Suffix = nullptr;
		switch (Kind)
		{
		case EDabGenKind::Bool:   Suffix = TEXT("BoolBool"); break;
		case EDabGenKind::Int:    Suffix = TEXT("IntInt"); break;
		case EDabGenKind::Int64:  Suffix = TEXT("Int64Int64"); break;
		case EDabGenKind::Real:   Suffix = TEXT("DoubleDouble"); break;
		case EDabGenKind::Byte:   Suffix = TEXT("ByteByte"); break;
		case EDabGenKind::Name:   Suffix = TEXT("NameName"); break;
		case EDabGenKind::Object: Suffix = TEXT("ObjectObject"); break;
		case EDabGenKind::Class:  Suffix = TEXT("ClassClass"); break;
		case EDabGenKind::String:
			Library = DabGenStringLibrary();
			Suffix = TEXT("StrStr");
			break;
		case EDabGenKind::Tag:
			Library = DabGenTagLibrary();
			Suffix = TEXT("GameplayTag");
			break;
		default:
			return false;
		}

		if (Kind == EDabGenKind::String && Op == EDabBinaryOp::Add)
		{
			OutFunction = TEXT("Concat_StrStr");
		}
		else
		{
			OutFunction = FString::Printf(TEXT("%s_%s"), OpName, Suffix);
		}
		OutLibrary = Library;
		return Library && Library->FindFunctionByName(FName(*OutFunction)) != nullptr;
	}

	/** Builds K2 nodes into one graph. Constants stay unplaced until the pin they feed gives them a type. */
	class FDabGenK2Builder
	{
	public:
		struct FValue
		{
			UEdGraphPin* Pin = nullptr;
			const FDabExpr* Constant = nullptr;

			bool IsValid() const { return Pin != nullptr || Constant != nullptr; }
		};

		FDabGenK2Builder(FDabGenContext& InCtx, UEdGraph* InGraph, const FString& InGraphPath, const FDabGenScope& InScope, const FString& InLocation,
			int32 InAnchorX, int32 InAnchorY)
			: Ctx(InCtx)
			, Graph(InGraph)
			, GraphPath(InGraphPath)
			, Scope(InScope)
			, Location(InLocation)
			, AnchorX(InAnchorX)
			, AnchorY(InAnchorY)
		{
		}

		bool HasFailed() const { return bFailed; }

		void Fail(const FDabExpr& Expr, const FString& Why)
		{
			Ctx.Sink.Error(TEXT("DAB7002"), FDabSpan(), FString::Printf(TEXT("%s: cannot build `%s`: %s"), *Location, *DabGenPrint(Expr), *Why));
			bFailed = true;
		}

		void FailText(const FString& Why)
		{
			Ctx.Sink.Error(TEXT("DAB7002"), FDabSpan(), FString::Printf(TEXT("%s: %s"), *Location, *Why));
			bFailed = true;
		}

		/** Gives a new node its place and its GUID. Columns run left of the anchor, one per depth. */
		void Finish(UEdGraphNode* Node, int32 Depth)
		{
			int32& Row = Rows.FindOrAdd(Depth);
			Node->NodePosX = AnchorX - 320 * (Depth + 1);
			Node->NodePosY = AnchorY + 140 * Row;
			++Row;
			Ctx.AssignGuid(Node, GraphPath, FString::Printf(TEXT("#k%d"), NodeCount++));
		}

		UK2Node_CallFunction* AddCall(const UFunction* Function, int32 Depth)
		{
			FGraphNodeCreator<UK2Node_CallFunction> Creator(*Graph);
			UK2Node_CallFunction* Node = Creator.CreateNode(false);
			Node->SetFromFunction(Function);
			Creator.Finalize();
			Finish(Node, Depth);
			return Node;
		}

		UEdGraphPin* AddSelf(int32 Depth)
		{
			FGraphNodeCreator<UK2Node_Self> Creator(*Graph);
			UK2Node_Self* Node = Creator.CreateNode(false);
			Creator.Finalize();
			Finish(Node, Depth);
			return Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Output);
		}

		UEdGraphPin* AddVariableGet(FName Variable, int32 Depth)
		{
			FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
			UK2Node_VariableGet* Node = Creator.CreateNode(false);
			Node->VariableReference.SetSelfMember(Variable);
			Creator.Finalize();
			Finish(Node, Depth);
			return Node->GetValuePin();
		}

		/** A Property Access node reading Segments from Self; its class is private to the PropertyAccessNode plugin. */
		UEdGraphPin* AddPropertyAccess(const TArray<FString>& Segments, int32 Depth, const FDabExpr& ForMessage)
		{
			UClass* AccessClass = FindObject<UClass>(nullptr, TEXT("/Script/PropertyAccessNode.K2Node_PropertyAccess"));
			const FArrayProperty* PathProperty = AccessClass ? FindFProperty<FArrayProperty>(AccessClass, TEXT("Path")) : nullptr;
			if (!PathProperty || !PathProperty->Inner->IsA<FStrProperty>())
			{
				Fail(ForMessage, TEXT("the Property Access Node plugin is not loaded"));
				return nullptr;
			}

			FGraphNodeCreator<UK2Node> Creator(*Graph);
			UK2Node* Node = Creator.CreateNode(false, AccessClass);
			// The path is private; set before Finalize so the output pin is made for the resolved type.
			*PathProperty->ContainerPtrToValuePtr<TArray<FString>>(Node) = Segments;
			Creator.Finalize();
			Finish(Node, Depth);

			UEdGraphPin* Output = Node->FindPin(TEXT("Value"), EGPD_Output);
			if (!Output || Output->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
			{
				UClass* SkeletonClass = Ctx.GetSkeletonClass();
				Fail(ForMessage, FString::Printf(TEXT("%s does not resolve on %s"), *FString::Join(Segments, TEXT(".")),
					SkeletonClass ? *SkeletonClass->GetName() : TEXT("the Blueprint")));
				return nullptr;
			}
			return Output;
		}

		UEdGraphPin* AddAnimGetter(FName FunctionName, UAnimGraphNode_Base* SourceNode, UAnimStateNodeBase* SourceState, const FString& Subject,
			int32 Depth, const FDabExpr& ForMessage)
		{
			// Getters are native functions of the instance class the Blueprint derives from.
			UClass* NativeClass = Ctx.Blueprint->ParentClass.Get();
			while (NativeClass && !NativeClass->HasAnyClassFlags(CLASS_Native))
			{
				NativeClass = NativeClass->GetSuperClass();
			}
			UFunction* Getter = NativeClass ? NativeClass->FindFunctionByName(FunctionName) : nullptr;
			if (!Getter)
			{
				Fail(ForMessage, FString::Printf(TEXT("the instance class has no %s"), *FunctionName.ToString()));
				return nullptr;
			}

			FGraphNodeCreator<UK2Node_AnimGetter> Creator(*Graph);
			UK2Node_AnimGetter* Node = Creator.CreateNode(false);
			// What the getter's menu spawner fills in (PostSpawnNodeSetup is protected); the compiler turns the
			// source nodes into the machine and state indices.
			Node->SourceNode = SourceNode;
			Node->SourceStateNode = SourceState;
			Node->GetterClass = NativeClass;
			Node->SourceAnimBlueprint = Ctx.Blueprint;
			Node->SetFromFunction(Getter);
			Node->CachedTitle = Subject.IsEmpty()
				? Getter->GetDisplayNameText()
				: FText::FromString(FString::Printf(TEXT("%s (%s)"), *Getter->GetDisplayNameText().ToString(), *Subject));
			Getter->GetMetaData(TEXT("GetterContext")).ParseIntoArray(Node->Contexts, TEXT("|"), true);
			Creator.Finalize();
			Finish(Node, Depth);
			return Node->GetReturnValuePin();
		}

		/** Feeds Input from Value: a link for a pin, the pin's default for a constant (typed by Parameter when there is one). */
		bool Feed(const FValue& Value, UEdGraphPin* Input, const FProperty* Parameter, const UEnum* EnumHint, const FDabExpr& ForMessage)
		{
			if (!Input)
			{
				Fail(ForMessage, TEXT("there is no pin to take it"));
				return false;
			}
			if (Value.Pin)
			{
				if (!Graph->GetSchema()->TryCreateConnection(Value.Pin, Input))
				{
					Fail(ForMessage, FString::Printf(TEXT("a %s cannot feed the %s pin %s"), *Value.Pin->PinType.PinCategory.ToString(),
						*Input->PinType.PinCategory.ToString(), *Input->PinName.ToString()));
					return false;
				}
				return true;
			}
			if (!Value.Constant)
			{
				return true;
			}

			FString Error;
			if (Parameter)
			{
				FDabGenTempValue Temp(Parameter);
				if (DabGenImportValue(Ctx, Parameter, Temp.Get(), *Value.Constant, nullptr, Error, EnumHint)
					&& DabGenSetPinFromValue(Input, Parameter, Temp.Get(), Error))
				{
					return true;
				}
			}
			else
			{
				FString Text;
				UObject* Object = nullptr;
				if (DabGenMakeDefault(Ctx, Input->PinType, *Value.Constant, Text, Object, Error) && DabGenSetPinDefault(Input, Text, Object, Error))
				{
					return true;
				}
			}
			Fail(*Value.Constant, Error);
			return false;
		}

		FValue Build(const FDabExpr& InExpr, int32 Depth)
		{
			const FDabExpr& Expr = DabGenUnwrap(InExpr);
			if (DabGenIsConstant(Expr))
			{
				FValue Constant;
				Constant.Constant = &Expr;
				return Constant;
			}

			FDabGenPath Path;
			if (DabGenTryGetPath(Expr, Path))
			{
				return BuildPath(Expr, Path, Depth);
			}
			if (const FDabUnaryExpr* Unary = Expr.As<FDabUnaryExpr>())
			{
				return BuildUnary(*Unary, Depth);
			}
			if (const FDabBinaryExpr* Binary = Expr.As<FDabBinaryExpr>())
			{
				return BuildBinary(*Binary, Depth);
			}
			if (const FDabConditionalExpr* Conditional = Expr.As<FDabConditionalExpr>())
			{
				return BuildConditional(*Conditional, Depth);
			}
			if (const FDabCallExpr* Call = Expr.As<FDabCallExpr>())
			{
				const FDabNameExpr* Callee = Call->Callee ? DabGenUnwrap(*Call->Callee).As<FDabNameExpr>() : nullptr;
				if (Callee)
				{
					return BuildBuiltin(*Call, Callee->Name, Depth);
				}
				Fail(Expr, TEXT("only zero-argument calls on a path from Self can be read"));
				return FValue();
			}
			Fail(Expr, TEXT("this kind of expression has no K2 form"));
			return FValue();
		}

		/** The object a call goes to and its class; null pin and the skeleton class for Self. */
		UEdGraphPin* BuildObject(const FDabExpr& Target, int32 Depth, UClass*& OutClass)
		{
			OutClass = nullptr;
			FDabGenPath Path;
			if (!DabGenTryGetPath(Target, Path) || (Path.Root != EDabGenPathRoot::Self && Path.Root != EDabGenPathRoot::Props))
			{
				Fail(Target, TEXT("a call target is Self or a path from Self"));
				return nullptr;
			}
			if (Path.Root == EDabGenPathRoot::Self && Path.Segments.IsEmpty())
			{
				OutClass = Ctx.GetSkeletonClass();
				return nullptr;
			}

			if (Path.Root == EDabGenPathRoot::Self && Path.AllCalls())
			{
				// A chain of pure getters: thread-safe getters hand their result on as a thread-safe object.
				UClass* Class = Ctx.GetSkeletonClass();
				UEdGraphPin* Pin = nullptr;
				for (int32 Index = 0; Index < Path.Segments.Num(); ++Index)
				{
					const FString& Segment = Path.Segments[Index];
					UFunction* Getter = Class ? Class->FindFunctionByName(FName(*Segment)) : nullptr;
					const FObjectPropertyBase* ReturnProperty = Getter ? CastField<FObjectPropertyBase>(Getter->GetReturnProperty()) : nullptr;
					if (!ReturnProperty)
					{
						Fail(Target, FString::Printf(TEXT("%s has no getter %s returning an object"), Class ? *Class->GetName() : TEXT("?"), *Segment));
						return nullptr;
					}
					UK2Node_CallFunction* Node = AddCall(Getter, Depth + Path.Segments.Num() - Index);
					if (Pin)
					{
						UEdGraphPin* SelfPin = Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
						if (!SelfPin || !Graph->GetSchema()->TryCreateConnection(Pin, SelfPin))
						{
							Fail(Target, FString::Printf(TEXT("%s cannot be called on the result before it"), *Segment));
							return nullptr;
						}
					}
					Pin = Node->GetReturnValuePin();
					Class = ReturnProperty->PropertyClass;
				}
				OutClass = Class;
				return Pin;
			}

			UEdGraphPin* Pin = AddPropertyAccess(Path.Segments, Depth + 1, Target);
			if (!Pin)
			{
				return nullptr;
			}
			OutClass = Cast<UClass>(Pin->PinType.PinSubCategoryObject.Get());
			if (!OutClass && Pin->PinType.PinSubCategory == UEdGraphSchema_K2::PSC_Self)
			{
				OutClass = Ctx.GetSkeletonClass();
			}
			if (!OutClass)
			{
				Fail(Target, TEXT("the path does not read an object"));
				return nullptr;
			}
			return Pin;
		}

		/** An impure call node for Call: the function on its target, the arguments on its pins. */
		UK2Node_CallFunction* BuildCall(const FDabModelCall& Call, int32 Depth)
		{
			UClass* TargetClass = nullptr;
			UEdGraphPin* TargetPin = nullptr;
			if (DabGenIsSelf(Call.Target.Get()))
			{
				TargetClass = Ctx.GetSkeletonClass();
			}
			else
			{
				TargetPin = BuildObject(*Call.Target, Depth + 1, TargetClass);
				if (bFailed)
				{
					return nullptr;
				}
			}

			UFunction* Function = TargetClass ? TargetClass->FindFunctionByName(FName(*Call.Function)) : nullptr;
			if (!Function)
			{
				FailText(FString::Printf(TEXT("%s has no function %s"), TargetClass ? *TargetClass->GetName() : TEXT("the target"), *Call.Function));
				return nullptr;
			}
			if (Function->HasAnyFunctionFlags(FUNC_BlueprintPure))
			{
				FailText(FString::Printf(TEXT("%s is pure; a call that runs must have an effect"), *Call.Function));
				return nullptr;
			}

			UK2Node_CallFunction* Node = AddCall(Function, Depth);
			if (TargetPin)
			{
				UEdGraphPin* SelfPin = Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
				if (!SelfPin || !Graph->GetSchema()->TryCreateConnection(TargetPin, SelfPin))
				{
					FailText(FString::Printf(TEXT("%s cannot be called on %s"), *Call.Function, *DabGenPrint(*Call.Target)));
					return nullptr;
				}
			}

			TArray<FProperty*> Parameters;
			DabGenGetInputParameters(Function, Parameters);
			int32 Positional = 0;
			for (const FDabModelArgument& Argument : Call.Args)
			{
				const FProperty* Parameter = Argument.Name.IsEmpty()
					? (Parameters.IsValidIndex(Positional) ? Parameters[Positional++] : nullptr)
					: FindFProperty<FProperty>(Function, FName(*Argument.Name));
				if (!Parameter || !Argument.Value)
				{
					FailText(FString::Printf(TEXT("%s has no parameter %s"), *Call.Function, Argument.Name.IsEmpty() ? TEXT("at that position") : *Argument.Name));
					continue;
				}
				const FValue Value = Build(*Argument.Value, Depth + 1);
				if (Value.IsValid())
				{
					Feed(Value, Node->FindPin(Parameter->GetFName(), EGPD_Input), Parameter, nullptr, *Argument.Value);
				}
			}
			return Node;
		}

	private:
		EDabGenKind KindOf(const FValue& Value) const
		{
			return Value.Pin ? DabGenKindOfType(Value.Pin->PinType) : EDabGenKind::Unknown;
		}

		EDabGenKind KindOfAll(TArrayView<const FValue> Values) const
		{
			EDabGenKind Kind = EDabGenKind::Unknown;
			for (const FValue& Value : Values)
			{
				Kind = DabGenUnifyKinds(Kind, KindOf(Value));
			}
			if (Kind == EDabGenKind::Unknown)
			{
				// Only constants: their own spelling decides.
				for (const FValue& Value : Values)
				{
					if (Value.Constant)
					{
						Kind = DabGenUnifyKinds(Kind, DabGenKindOfConstant(*Value.Constant));
					}
				}
			}
			else if (Kind == EDabGenKind::Int || Kind == EDabGenKind::Int64 || Kind == EDabGenKind::Byte)
			{
				// `Count > 0.5` compares as reals; rounding the constant would change the condition.
				for (const FValue& Value : Values)
				{
					double Number = 0.0;
					if (Value.Constant && DabGenTryGetNumber(*Value.Constant, Number) && FMath::Frac(Number) != 0.0)
					{
						return EDabGenKind::Real;
					}
				}
			}
			return Kind;
		}

		/** The enum of the first enum-typed pin, for constants written as value names next to it. */
		static const UEnum* EnumOf(TArrayView<const FValue> Values)
		{
			for (const FValue& Value : Values)
			{
				if (Value.Pin)
				{
					if (const UEnum* Enum = Cast<UEnum>(Value.Pin->PinType.PinSubCategoryObject.Get()))
					{
						return Enum;
					}
				}
			}
			return nullptr;
		}

		/** A library function fed with Inputs in parameter order; an invalid input leaves its pin at the default. */
		FValue CallLibrary(UClass* Library, const FString& FunctionName, TArrayView<const FValue> Inputs, const UEnum* EnumHint, const FDabExpr& ForMessage,
			int32 Depth)
		{
			const UFunction* Function = Library ? Library->FindFunctionByName(FName(*FunctionName)) : nullptr;
			if (!Function)
			{
				Fail(ForMessage, FString::Printf(TEXT("the library function %s does not exist"), *FunctionName));
				return FValue();
			}
			UK2Node_CallFunction* Node = AddCall(Function, Depth);
			TArray<FProperty*> Parameters;
			DabGenGetInputParameters(Function, Parameters);
			for (int32 Index = 0; Index < Inputs.Num() && Index < Parameters.Num(); ++Index)
			{
				if (Inputs[Index].IsValid() && !Feed(Inputs[Index], Node->FindPin(Parameters[Index]->GetFName(), EGPD_Input), Parameters[Index], EnumHint, ForMessage))
				{
					return FValue();
				}
			}
			FValue Result;
			Result.Pin = Node->GetReturnValuePin();
			return Result;
		}

		FValue BuildPath(const FDabExpr& Expr, const FDabGenPath& Path, int32 Depth)
		{
			FValue Result;
			switch (Path.Root)
			{
			case EDabGenPathRoot::Self:
				Result.Pin = Path.Segments.IsEmpty() ? AddSelf(Depth) : AddPropertyAccess(Path.Segments, Depth, Expr);
				break;
			case EDabGenPathRoot::Props:
				if (Path.Segments.IsEmpty())
				{
					Fail(Expr, TEXT("Props needs a variable name"));
					break;
				}
				Result.Pin = AddPropertyAccess(Path.Segments, Depth, Expr);
				break;
			case EDabGenPathRoot::Assets:
				if (Path.Segments.Num() != 1)
				{
					Fail(Expr, TEXT("an asset slot is read as Assets.<Slot>"));
					break;
				}
				else
				{
					const FName Function = DabGenEnsureAssetFunction(Ctx, Path.Segments[0], Location);
					if (Function.IsNone())
					{
						bFailed = true;
						break;
					}
					Result.Pin = AddPropertyAccess({ Function.ToString() }, Depth, Expr);
				}
				break;
			default:
				Fail(Expr, TEXT("not a path from Self, Props or Assets"));
				break;
			}
			return Result;
		}

		FValue BuildUnary(const FDabUnaryExpr& Expr, int32 Depth)
		{
			if (!Expr.Operand)
			{
				Fail(Expr, TEXT("the operator has no operand"));
				return FValue();
			}
			const FValue Operand = Build(*Expr.Operand, Depth + 1);
			if (!Operand.IsValid())
			{
				return FValue();
			}
			if (Expr.Op == EDabUnaryOp::Not)
			{
				const FValue Inputs[] = { Operand };
				return CallLibrary(DabGenMathLibrary(), TEXT("Not_PreBool"), Inputs, nullptr, Expr, Depth);
			}

			// No negate function for numbers: 0 - x.
			const EDabGenKind Kind = KindOf(Operand);
			const TCHAR* FunctionName = Kind == EDabGenKind::Int ? TEXT("Subtract_IntInt")
				: Kind == EDabGenKind::Int64 ? TEXT("Subtract_Int64Int64")
				: TEXT("Subtract_DoubleDouble");
			const FValue Inputs[] = { FValue(), Operand };
			const FValue Result = CallLibrary(DabGenMathLibrary(), FunctionName, Inputs, nullptr, Expr, Depth);
			if (Result.Pin)
			{
				UK2Node_CallFunction* Node = CastChecked<UK2Node_CallFunction>(Result.Pin->GetOwningNode());
				FString Error;
				TArray<FProperty*> Parameters;
				DabGenGetInputParameters(Node->GetTargetFunction(), Parameters);
				if (Parameters.IsEmpty() || !DabGenSetPinDefault(Node->FindPin(Parameters[0]->GetFName(), EGPD_Input), TEXT("0"), nullptr, Error))
				{
					Fail(Expr, Error);
					return FValue();
				}
			}
			return Result;
		}

		FValue BuildBinary(const FDabBinaryExpr& Expr, int32 Depth)
		{
			if (!Expr.Lhs || !Expr.Rhs)
			{
				Fail(Expr, TEXT("the operator lacks an operand"));
				return FValue();
			}
			const FValue Lhs = Build(*Expr.Lhs, Depth + 1);
			const FValue Rhs = Build(*Expr.Rhs, Depth + 1);
			if (!Lhs.IsValid() || !Rhs.IsValid())
			{
				return FValue();
			}
			const FValue Inputs[] = { Lhs, Rhs };

			if (Expr.Op == EDabBinaryOp::And || Expr.Op == EDabBinaryOp::Or)
			{
				return CallLibrary(DabGenMathLibrary(), Expr.Op == EDabBinaryOp::And ? TEXT("BooleanAND") : TEXT("BooleanOR"), Inputs, nullptr, Expr, Depth);
			}

			const EDabGenKind Kind = KindOfAll(Inputs);
			UClass* Library = nullptr;
			FString FunctionName;
			if (!DabGenFindOperator(Expr.Op, Kind, Library, FunctionName))
			{
				Fail(Expr, FString::Printf(TEXT("%s has no form for these operand types"), GetDabBinaryOpSpelling(Expr.Op)));
				return FValue();
			}
			return CallLibrary(Library, FunctionName, Inputs, EnumOf(Inputs), Expr, Depth);
		}

		FValue BuildConditional(const FDabConditionalExpr& Expr, int32 Depth)
		{
			if (!Expr.Condition || !Expr.Then || !Expr.Else)
			{
				Fail(Expr, TEXT("the conditional lacks a part"));
				return FValue();
			}
			const FValue Condition = Build(*Expr.Condition, Depth + 1);
			const FValue Then = Build(*Expr.Then, Depth + 1);
			const FValue Else = Build(*Expr.Else, Depth + 1);
			if (!Condition.IsValid() || !Then.IsValid() || !Else.IsValid())
			{
				return FValue();
			}

			const FValue Branches[] = { Then, Else };
			const EDabGenKind Kind = KindOfAll(Branches);
			if (Kind == EDabGenKind::Bool)
			{
				// No bool select: (c && a) || (!c && b).
				const FValue TakeThenInputs[] = { Condition, Then };
				const FValue TakeThen = CallLibrary(DabGenMathLibrary(), TEXT("BooleanAND"), TakeThenInputs, nullptr, Expr, Depth + 1);
				const FValue NotInputs[] = { Condition };
				const FValue NotCondition = CallLibrary(DabGenMathLibrary(), TEXT("Not_PreBool"), NotInputs, nullptr, Expr, Depth + 2);
				const FValue TakeElseInputs[] = { NotCondition, Else };
				const FValue TakeElse = CallLibrary(DabGenMathLibrary(), TEXT("BooleanAND"), TakeElseInputs, nullptr, Expr, Depth + 1);
				if (!TakeThen.IsValid() || !TakeElse.IsValid())
				{
					return FValue();
				}
				const FValue OrInputs[] = { TakeThen, TakeElse };
				return CallLibrary(DabGenMathLibrary(), TEXT("BooleanOR"), OrInputs, nullptr, Expr, Depth);
			}

			// `c ? 1 : 0` is the bool-to-number conversion, the node an author would place.
			if ((Kind == EDabGenKind::Real || Kind == EDabGenKind::Int) && IsNumberConstant(Then, 1.0) && IsNumberConstant(Else, 0.0))
			{
				const FValue ConvertInputs[] = { Condition };
				return CallLibrary(DabGenMathLibrary(), Kind == EDabGenKind::Real ? TEXT("Conv_BoolToDouble") : TEXT("Conv_BoolToInt"), ConvertInputs, nullptr, Expr, Depth);
			}

			const TCHAR* FunctionName = nullptr;
			switch (Kind)
			{
			case EDabGenKind::Real:   FunctionName = TEXT("SelectFloat"); break;
			case EDabGenKind::Int:    FunctionName = TEXT("SelectInt"); break;
			case EDabGenKind::String: FunctionName = TEXT("SelectString"); break;
			case EDabGenKind::Name:   FunctionName = TEXT("SelectName"); break;
			case EDabGenKind::Text:   FunctionName = TEXT("SelectText"); break;
			default:
				Fail(Expr, TEXT("a conditional chooses between numbers, bools, names or strings"));
				return FValue();
			}
			const FValue Inputs[] = { Then, Else, Condition };
			return CallLibrary(DabGenMathLibrary(), FunctionName, Inputs, nullptr, Expr, Depth);
		}

		static bool IsNumberConstant(const FValue& Value, double Number)
		{
			const FDabLiteralExpr* Literal = Value.Constant ? DabGenUnwrap(*Value.Constant).As<FDabLiteralExpr>() : nullptr;
			return Literal && Literal->LiteralKind == EDabLiteralKind::Number && Literal->Number == Number;
		}

		FValue BuildBuiltin(const FDabCallExpr& Expr, const FString& Name, int32 Depth)
		{
			if (Name == TEXT("Unsupported"))
			{
				Fail(Expr, TEXT("the extractor could not express this part of the Blueprint"));
				return FValue();
			}
			if (Name == TEXT("StateWeight") || Name == TEXT("StateTime") || Name == TEXT("MachineWeight") || Name == TEXT("TimeRemaining")
				|| Name == TEXT("TimeRemainingRatio"))
			{
				return BuildGetter(Expr, Name, Depth);
			}

			TArray<FValue> Positional;
			FValue Exact;
			for (const FDabArgumentPtr& Argument : Expr.Args)
			{
				if (!Argument || !Argument->Value)
				{
					continue;
				}
				const FValue Value = Build(*Argument->Value, Depth + 1);
				if (!Value.IsValid())
				{
					return FValue();
				}
				if (Argument->Name == TEXT("Exact") || Argument->Name == TEXT("bExactMatch"))
				{
					Exact = Value;
				}
				else
				{
					Positional.Add(Value);
				}
			}

			const auto Expect = [this, &Expr, &Positional, &Name](int32 Count) -> bool
			{
				if (Positional.Num() != Count)
				{
					Fail(Expr, FString::Printf(TEXT("%s takes %d argument%s"), *Name, Count, Count == 1 ? TEXT("") : TEXT("s")));
					return false;
				}
				return true;
			};

			const EDabGenKind Kind = KindOfAll(Positional);
			if (Name == TEXT("Abs"))
			{
				if (!Expect(1))
				{
					return FValue();
				}
				const TCHAR* FunctionName = Kind == EDabGenKind::Int64 ? TEXT("Abs_Int64") : (Kind == EDabGenKind::Int || Kind == EDabGenKind::Byte) ? TEXT("Abs_Int") : TEXT("Abs");
				return CallLibrary(DabGenMathLibrary(), FunctionName, Positional, nullptr, Expr, Depth);
			}
			if (Name == TEXT("Min") || Name == TEXT("Max"))
			{
				if (!Expect(2))
				{
					return FValue();
				}
				const bool bMin = Name == TEXT("Min");
				const TCHAR* FunctionName = Kind == EDabGenKind::Int64 ? (bMin ? TEXT("MinInt64") : TEXT("MaxInt64"))
					: (Kind == EDabGenKind::Int || Kind == EDabGenKind::Byte) ? (bMin ? TEXT("Min") : TEXT("Max"))
					: (bMin ? TEXT("FMin") : TEXT("FMax"));
				return CallLibrary(DabGenMathLibrary(), FunctionName, Positional, nullptr, Expr, Depth);
			}
			if (Name == TEXT("Clamp"))
			{
				if (!Expect(3))
				{
					return FValue();
				}
				const TCHAR* FunctionName = Kind == EDabGenKind::Int64 ? TEXT("ClampInt64") : (Kind == EDabGenKind::Int || Kind == EDabGenKind::Byte) ? TEXT("Clamp") : TEXT("FClamp");
				return CallLibrary(DabGenMathLibrary(), FunctionName, Positional, nullptr, Expr, Depth);
			}
			if (Name == TEXT("IsValid"))
			{
				if (!Expect(1))
				{
					return FValue();
				}
				const EDabGenKind ValueKind = KindOf(Positional[0]);
				if (ValueKind == EDabGenKind::Tag)
				{
					return CallLibrary(DabGenTagLibrary(), TEXT("IsGameplayTagValid"), Positional, nullptr, Expr, Depth);
				}
				if (ValueKind == EDabGenKind::Object || ValueKind == EDabGenKind::Class)
				{
					// IsValid of the system library is not thread-safe; comparing with None is.
					return CallLibrary(DabGenMathLibrary(), ValueKind == EDabGenKind::Object ? TEXT("NotEqual_ObjectObject") : TEXT("NotEqual_ClassClass"),
						Positional, nullptr, Expr, Depth);
				}
				Fail(Expr, TEXT("IsValid takes a gameplay tag or an object"));
				return FValue();
			}
			if (Name == TEXT("Matches"))
			{
				if (Positional.Num() == 3 && !Exact.IsValid())
				{
					Exact = Positional.Pop();
				}
				if (!Expect(2))
				{
					return FValue();
				}
				const EDabGenKind ValueKind = KindOf(Positional[0]);
				const FValue Inputs[] = { Positional[0], Positional[1], Exact };
				if (ValueKind == EDabGenKind::Tag)
				{
					return CallLibrary(DabGenTagLibrary(), TEXT("MatchesTag"), Inputs, nullptr, Expr, Depth);
				}
				if (ValueKind == EDabGenKind::TagContainer)
				{
					return CallLibrary(DabGenTagLibrary(), TEXT("HasTag"), Inputs, nullptr, Expr, Depth);
				}
				Fail(Expr, TEXT("Matches takes a gameplay tag or a tag container first"));
				return FValue();
			}

			Fail(Expr, FString::Printf(TEXT("%s is not a function the language knows"), *Name));
			return FValue();
		}

		/** A state of the expression's machine, else the only state of that name in the Blueprint. */
		UAnimStateNodeBase* FindState(const FString& StateName, FDabGenMachine*& OutMachine) const
		{
			if (FDabGenMachine* Machine = Ctx.FindMachine(Scope.MachineName))
			{
				if (UAnimStateNodeBase* const* State = Machine->StateNodes.Find(StateName))
				{
					OutMachine = Machine;
					return *State;
				}
			}
			UAnimStateNodeBase* Found = nullptr;
			for (const TPair<FString, TUniquePtr<FDabGenMachine>>& Entry : Ctx.Machines)
			{
				if (UAnimStateNodeBase* const* State = Entry.Value->StateNodes.Find(StateName))
				{
					if (Found)
					{
						return nullptr;
					}
					Found = *State;
					OutMachine = Entry.Value.Get();
				}
			}
			return Found;
		}

		FValue BuildGetter(const FDabCallExpr& Expr, const FString& Name, int32 Depth)
		{
			FString Argument;
			if (!Expr.Args.IsEmpty())
			{
				if (Expr.Args.Num() > 1 || !Expr.Args[0] || !Expr.Args[0]->Value || !DabGenTryGetText(*Expr.Args[0]->Value, Argument))
				{
					Fail(Expr, FString::Printf(TEXT("%s takes at most one state or machine name"), *Name));
					return FValue();
				}
			}

			FDabGenMachine* Machine = nullptr;
			UAnimStateNodeBase* State = nullptr;
			FName GetterName;
			FString Subject;
			if (Name == TEXT("MachineWeight"))
			{
				Machine = Ctx.FindMachine(Argument.IsEmpty() ? Scope.MachineName : Argument);
				GetterName = TEXT("GetInstanceMachineWeight");
				Subject = Argument;
			}
			else if (Name == TEXT("StateTime"))
			{
				Machine = Ctx.FindMachine(Scope.MachineName);
				GetterName = TEXT("GetInstanceCurrentStateElapsedTime");
			}
			else
			{
				// StateWeight(S), TimeRemaining(), TimeRemainingRatio(): a state, by default the transition's source.
				const FString StateName = Argument.IsEmpty() ? Scope.FromState : Argument;
				if (StateName.IsEmpty())
				{
					Fail(Expr, FString::Printf(TEXT("%s needs a state here"), *Name));
					return FValue();
				}
				State = FindState(StateName, Machine);
				if (!State)
				{
					Fail(Expr, FString::Printf(TEXT("no state %s (or more than one) to read"), *StateName));
					return FValue();
				}
				GetterName = Name == TEXT("StateWeight") ? FName(TEXT("GetInstanceStateWeight"))
					: Name == TEXT("TimeRemaining") ? FName(TEXT("GetRelevantAnimTimeRemaining"))
					: FName(TEXT("GetRelevantAnimTimeRemainingFraction"));
				Subject = StateName;
			}
			if (!Machine || !Machine->Node)
			{
				Fail(Expr, TEXT("no state machine to read"));
				return FValue();
			}
			FValue Result;
			Result.Pin = AddAnimGetter(GetterName, Machine->Node, State, Subject, Depth, Expr);
			return Result;
		}

		FDabGenContext& Ctx;
		UEdGraph* Graph = nullptr;
		FString GraphPath;
		FDabGenScope Scope;
		FString Location;
		int32 AnchorX = 0;
		int32 AnchorY = 0;
		int32 NodeCount = 0;
		TMap<int32, int32> Rows;
		bool bFailed = false;
	};

	// ------------------------------------------------------------------------------------------
	// Generated functions
	// ------------------------------------------------------------------------------------------

	/** A generated name, kept clear of a parent Blueprint that DreamAnim generated too. */
	static FName DabGenMakeFunctionName(FDabGenContext& Ctx, const FString& Base)
	{
		const UClass* Parent = Ctx.Blueprint->ParentClass.Get();
		if (Parent && Parent->FindFunctionByName(FName(*Base)))
		{
			return FName(*(Base + TEXT("_") + Ctx.GetBlueprintAssetName().ToString()));
		}
		return FName(*Base);
	}

	struct FDabGenFunction
	{
		UEdGraph* Graph = nullptr;
		UK2Node_FunctionEntry* Entry = nullptr;
		FString Path;
	};

	/** A new thread-safe function graph: entry only, or the signature of Signature (Prototype_ThreadSafeAnimUpdateCall). */
	static FDabGenFunction DabGenAddFunction(FDabGenContext& Ctx, FName Name, const UFunction* Signature, bool bPure)
	{
		FDabGenFunction Function;
		Function.Path = TEXT("Function/") + Name.ToString();
		Function.Graph = FBlueprintEditorUtils::CreateNewGraph(Ctx.Blueprint, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		Function.Graph->GraphGuid = Ctx.MakeGuid(Function.Path);
		if (Signature)
		{
			FBlueprintEditorUtils::AddFunctionGraph(Ctx.Blueprint, Function.Graph, true, Signature);
		}
		else
		{
			FBlueprintEditorUtils::AddFunctionGraph<UClass>(Ctx.Blueprint, Function.Graph, true, nullptr);
		}

		TArray<UK2Node_FunctionEntry*> Entries;
		Function.Graph->GetNodesOfClass(Entries);
		Function.Entry = Entries.IsEmpty() ? nullptr : Entries[0];
		if (Function.Entry)
		{
			// The graph runs it on a worker thread.
			Function.Entry->MetaData.bThreadSafe = true;
			Function.Entry->MetaData.Category = FText::FromString(TEXT("DreamAnim"));
			if (bPure)
			{
				Function.Entry->AddExtraFlags(FUNC_BlueprintPure);
			}
			Ctx.AssignGuid(Function.Entry, Function.Path, TEXT("#Entry"));
		}
		Ctx.GeneratedFunctions.Add(Name);
		return Function;
	}

	/** A result node with one ReturnValue pin, reached from the entry's exec (pure functions still run that chain). */
	static UEdGraphPin* DabGenAddReturnValue(FDabGenContext& Ctx, const FDabGenFunction& Function, const FEdGraphPinType& Type)
	{
		FGraphNodeCreator<UK2Node_FunctionResult> Creator(*Function.Graph);
		UK2Node_FunctionResult* Result = Creator.CreateNode(false);
		Result->FunctionReference = Function.Entry->FunctionReference;
		Creator.Finalize();
		Result->NodePosX = Function.Entry->NodePosX + 400;
		Result->NodePosY = Function.Entry->NodePosY + 200;
		Ctx.AssignGuid(Result, Function.Path, TEXT("#Result"));

		FEdGraphPinType ReturnType = Type;
		ReturnType.bIsReference = false;
		ReturnType.bIsConst = false;
		UEdGraphPin* ReturnPin = Result->CreateUserDefinedPin(UEdGraphSchema_K2::PN_ReturnValue, ReturnType, EGPD_Input, false);

		UEdGraphPin* EntryThen = Function.Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* ResultExec = Result->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		if (EntryThen && ResultExec)
		{
			EntryThen->MakeLinkTo(ResultExec);
		}
		return ReturnPin;
	}

	static void DabGenDropFunction(FDabGenContext& Ctx, const FDabGenFunction& Function)
	{
		if (Function.Graph)
		{
			FBlueprintEditorUtils::RemoveGraph(Ctx.Blueprint, Function.Graph, EGraphRemoveFlags::MarkTransient);
		}
	}

	/** `DreamAnimSlot AnimSequence`: the keywords of a slot getter's entry. */
	static const FString DabGenSlotKeywordPrefix(TEXT("DreamAnimSlot "));

	FName DabGenEnsureAssetFunction(FDabGenContext& Ctx, const FString& Slot, const FString& Location)
	{
		if (const FName* Found = Ctx.AssetFunctions.Find(Slot))
		{
			return *Found;
		}
		const FDabModelAssetSlot* SlotModel = Ctx.Model.AssetSlots.FindByPredicate([&Slot](const FDabModelAssetSlot& Candidate)
		{
			return Candidate.Name == Slot;
		});
		if (!SlotModel)
		{
			Ctx.Sink.Error(TEXT("DAB7004"), FDabSpan(), FString::Printf(TEXT("%s: no asset slot %s (Assets = { ... })"), *Location, *Slot));
			Ctx.AssetFunctions.Add(Slot, NAME_None);
			return NAME_None;
		}

		// The getter returns what the slot's pins take, read from the active set with the library's typed getter.
		const UClass* SlotClass = Cast<UClass>(DabGenFindType(SlotModel->Type, UClass::StaticClass()));
		const TCHAR* LibraryFunction = TEXT("GetAnimSetAsset");
		UClass* ReturnClass = UAnimationAsset::StaticClass();
		if (SlotClass && SlotClass->IsChildOf(UAnimMontage::StaticClass()))
		{
			LibraryFunction = TEXT("GetAnimSetMontage");
			ReturnClass = UAnimMontage::StaticClass();
		}
		else if (SlotClass && SlotClass->IsChildOf(UBlendSpace::StaticClass()))
		{
			LibraryFunction = TEXT("GetAnimSetBlendSpace");
			ReturnClass = UBlendSpace::StaticClass();
		}
		else if (SlotClass && SlotClass->IsChildOf(UAnimSequenceBase::StaticClass()))
		{
			LibraryFunction = TEXT("GetAnimSetSequence");
			ReturnClass = UAnimSequenceBase::StaticClass();
		}
		const UFunction* Getter = UDreamAnimLibrary::StaticClass()->FindFunctionByName(LibraryFunction);
		if (!Getter)
		{
			Ctx.Sink.Error(TEXT("DAB7004"), FDabSpan(), FString::Printf(TEXT("%s: UDreamAnimLibrary has no %s"), *Location, LibraryFunction));
			Ctx.AssetFunctions.Add(Slot, NAME_None);
			return NAME_None;
		}

		const FName Name = DabGenMakeFunctionName(Ctx, TEXT("DreamAnimAsset_") + Slot);
		const FDabGenFunction Function = DabGenAddFunction(Ctx, Name, nullptr, true);
		if (!Function.Entry)
		{
			Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: the function %s has no entry"), *Location, *Name.ToString()));
			Ctx.AssetFunctions.Add(Slot, NAME_None);
			return NAME_None;
		}
		// The declared type, read back by the extractor; the return type is only its nearest getter's.
		Function.Entry->MetaData.Keywords = FText::FromString(DabGenSlotKeywordPrefix + SlotModel->Type);
		FEdGraphPinType ReturnType;
		ReturnType.PinCategory = UEdGraphSchema_K2::PC_Object;
		ReturnType.PinSubCategoryObject = ReturnClass;
		UEdGraphPin* ReturnPin = DabGenAddReturnValue(Ctx, Function, ReturnType);

		FDabGenK2Builder Builder(Ctx, Function.Graph, Function.Path, FDabGenScope(), Location, 400, 200);
		UK2Node_CallFunction* Call = Builder.AddCall(Getter, 0);
		UEdGraphPin* Set = Builder.AddVariableGet(UDreamAnimLibrary::ActiveSetVariable, 1);
		UEdGraphPin* SetInput = Call->FindPin(TEXT("Set"), EGPD_Input);
		UEdGraphPin* CallReturn = Call->GetReturnValuePin();
		const UEdGraphSchema* Schema = Function.Graph->GetSchema();
		FString Error;
		const bool bLinked = Set && SetInput && CallReturn && ReturnPin
			&& Schema->TryCreateConnection(Set, SetInput)
			&& Schema->TryCreateConnection(CallReturn, ReturnPin)
			&& DabGenSetPinDefault(Call->FindPin(TEXT("Blueprint"), EGPD_Input), Ctx.GetBlueprintAssetName().ToString(), nullptr, Error)
			&& DabGenSetPinDefault(Call->FindPin(TEXT("Slot"), EGPD_Input), Slot, nullptr, Error);
		if (!bLinked)
		{
			Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: the slot getter %s cannot be built %s"), *Location, *Name.ToString(), *Error));
			DabGenDropFunction(Ctx, Function);
			Ctx.AssetFunctions.Add(Slot, NAME_None);
			return NAME_None;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);
		Ctx.AssetFunctions.Add(Slot, Name);
		return Name;
	}

	static FString DabGenPinTypeKey(const FEdGraphPinType& Type)
	{
		const UObject* SubObject = Type.PinSubCategoryObject.Get();
		return FString::Printf(TEXT("%s/%s/%s/%d"), *Type.PinCategory.ToString(), *Type.PinSubCategory.ToString(),
			SubObject ? *SubObject->GetPathName() : TEXT(""), static_cast<int32>(Type.ContainerType));
	}

	FName DabGenEnsureExprFunction(FDabGenContext& Ctx, const FDabExpr& Expr, const FEdGraphPinType& ReturnType, const FDabGenScope& Scope,
		const FString& Location)
	{
		const FString Key = DabGenPrint(Expr) + TEXT("|") + DabGenPinTypeKey(ReturnType) + TEXT("|") + Scope.MachineName + TEXT("|") + Scope.FromState;
		if (const FName* Found = Ctx.ExprFunctions.Find(Key))
		{
			return *Found;
		}

		const FName Name = DabGenMakeFunctionName(Ctx, TEXT("DreamAnimExpr_") + DabGenShortHash(Key));
		const FDabGenFunction Function = DabGenAddFunction(Ctx, Name, nullptr, true);
		if (!Function.Entry)
		{
			Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: the function %s has no entry"), *Location, *Name.ToString()));
			Ctx.ExprFunctions.Add(Key, NAME_None);
			return NAME_None;
		}
		UEdGraphPin* ReturnPin = DabGenAddReturnValue(Ctx, Function, ReturnType);

		FDabGenK2Builder Builder(Ctx, Function.Graph, Function.Path, Scope, Location, 400, 200);
		const FDabGenK2Builder::FValue Value = Builder.Build(Expr, 0);
		if (!Builder.HasFailed() && Value.IsValid())
		{
			Builder.Feed(Value, ReturnPin, nullptr, nullptr, Expr);
		}
		if (Builder.HasFailed() || !Value.IsValid())
		{
			DabGenDropFunction(Ctx, Function);
			Ctx.ExprFunctions.Add(Key, NAME_None);
			return NAME_None;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);
		Ctx.ExprFunctions.Add(Key, Name);
		return Name;
	}

	static bool DabGenMatchesSignature(const UFunction* Function, const UFunction* Prototype)
	{
		TArray<const FProperty*> Parameters;
		TArray<const FProperty*> Expected;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			Parameters.Add(*It);
		}
		for (TFieldIterator<FProperty> It(Prototype); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			Expected.Add(*It);
		}
		if (Parameters.Num() != Expected.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Parameters.Num(); ++Index)
		{
			if (Parameters[Index]->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				return false;
			}
			const FStructProperty* Have = CastField<FStructProperty>(Parameters[Index]);
			const FStructProperty* Want = CastField<FStructProperty>(Expected[Index]);
			if (Have && Want)
			{
				// The engine passes the prototype's argument; the function may take one of its bases.
				if (!Want->Struct->IsChildOf(Have->Struct))
				{
					return false;
				}
			}
			else if (Parameters[Index]->GetClass() != Expected[Index]->GetClass())
			{
				return false;
			}
		}
		return true;
	}

	static bool DabGenCanReferDirectly(const UFunction* Function, const FDabModelCall& Call, EDabGenForwarder Kind)
	{
		if (!FBlueprintEditorUtils::HasFunctionBlueprintThreadSafeMetaData(Function) || Function->HasAnyFunctionFlags(FUNC_BlueprintPure))
		{
			return false;
		}
		if (Kind == EDabGenForwarder::AnimUpdateCall)
		{
			const UFunction* Prototype = DabGenUpdatePrototype();
			return Call.Args.IsEmpty() && Prototype && DabGenMatchesSignature(Function, Prototype);
		}
		// A Call Function node takes the function's parameters as pins: constants only, and nothing comes back.
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm) || (It->HasAnyPropertyFlags(CPF_OutParm) && !It->HasAnyPropertyFlags(CPF_ReferenceParm)))
			{
				return false;
			}
		}
		for (const FDabModelArgument& Argument : Call.Args)
		{
			if (!Argument.Value || !DabGenIsConstant(*Argument.Value))
			{
				return false;
			}
		}
		return true;
	}

	static FName DabGenEnsureForwarder(FDabGenContext& Ctx, const FDabModelCall& Call, EDabGenForwarder Kind, const FString& Location)
	{
		const FString Key = DabGenPrintCall(Call) + (Kind == EDabGenForwarder::AnimUpdateCall ? TEXT("|update") : TEXT("|plain"));
		if (const FName* Found = Ctx.Forwarders.Find(Key))
		{
			return *Found;
		}

		const UFunction* Signature = nullptr;
		if (Kind == EDabGenForwarder::AnimUpdateCall)
		{
			Signature = DabGenUpdatePrototype();
			if (!Signature)
			{
				Ctx.Sink.Error(TEXT("DAB7004"), FDabSpan(), FString::Printf(TEXT("%s: Prototype_ThreadSafeAnimUpdateCall is not loaded"), *Location));
				Ctx.Forwarders.Add(Key, NAME_None);
				return NAME_None;
			}
		}

		// Entry -> one call: the shape the extractor reads back as the call it forwards.
		const FName Name = DabGenMakeFunctionName(Ctx, FString::Printf(TEXT("DreamAnimCall_%s_%s"), *Call.Function, *DabGenShortHash(Key)));
		const FDabGenFunction Function = DabGenAddFunction(Ctx, Name, Signature, false);
		UEdGraphPin* EntryThen = Function.Entry ? Function.Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output) : nullptr;
		if (!EntryThen)
		{
			Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: the function %s has no entry"), *Location, *Name.ToString()));
			DabGenDropFunction(Ctx, Function);
			Ctx.Forwarders.Add(Key, NAME_None);
			return NAME_None;
		}

		FDabGenK2Builder Builder(Ctx, Function.Graph, Function.Path, FDabGenScope(), Location, 700, 0);
		UK2Node_CallFunction* CallNode = Builder.BuildCall(Call, 0);
		if (CallNode && !Builder.HasFailed())
		{
			CallNode->NodePosX = Function.Entry->NodePosX + 350;
			CallNode->NodePosY = Function.Entry->NodePosY;
			if (UEdGraphPin* Exec = CallNode->GetExecPin())
			{
				// The update-call signature's const-ref parameters read as outputs, so the graph may come with a
				// result node already wired to the entry; the call goes in between.
				const TArray<UEdGraphPin*> Previous = EntryThen->LinkedTo;
				EntryThen->BreakAllPinLinks();
				EntryThen->MakeLinkTo(Exec);
				if (UEdGraphPin* Then = CallNode->GetThenPin())
				{
					for (UEdGraphPin* Next : Previous)
					{
						Then->MakeLinkTo(Next);
					}
				}
			}
		}
		if (!CallNode || Builder.HasFailed())
		{
			DabGenDropFunction(Ctx, Function);
			Ctx.Forwarders.Add(Key, NAME_None);
			return NAME_None;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Ctx.Blueprint);
		Ctx.Forwarders.Add(Key, Name);
		return Name;
	}

	FName DabGenResolveCallFunction(FDabGenContext& Ctx, const FDabModelCall& Call, EDabGenForwarder Kind, const FString& Location)
	{
		if (DabGenIsSelf(Call.Target.Get()))
		{
			for (const FDabModelArgument& Argument : Call.Args)
			{
				if (Argument.Value && DabGenFindUnsupported(*Argument.Value))
				{
					Ctx.Sink.Error(TEXT("DAB7002"), FDabSpan(), FString::Printf(TEXT("%s: the call cannot be generated: %s"), *Location, *DabGenPrintCall(Call)));
					return NAME_None;
				}
			}
			UClass* SkeletonClass = Ctx.GetSkeletonClass();
			const UFunction* Function = SkeletonClass ? SkeletonClass->FindFunctionByName(FName(*Call.Function)) : nullptr;
			if (Function && DabGenCanReferDirectly(Function, Call, Kind))
			{
				return Function->GetFName();
			}
		}
		else if (Call.Target && DabGenFindUnsupported(*Call.Target))
		{
			Ctx.Sink.Error(TEXT("DAB7002"), FDabSpan(), FString::Printf(TEXT("%s: the call cannot be generated: %s"), *Location, *DabGenPrintCall(Call)));
			return NAME_None;
		}
		return DabGenEnsureForwarder(Ctx, Call, Kind, Location);
	}

	void DabGenBuildCondition(FDabGenContext& Ctx, UAnimGraphNode_Base* ResultNode, const FDabExpr* Condition, const FString& GraphPath,
		const FDabGenScope& Scope, const FString& Location)
	{
		UEdGraphPin* Pin = ResultNode ? ResultNode->FindPin(TEXT("bCanEnterTransition"), EGPD_Input) : nullptr;
		if (!Pin)
		{
			Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), FString::Printf(TEXT("%s: the rule graph has no result to feed"), *Location));
			return;
		}

		FString Error;
		bool bValue = true;
		if (!Condition || DabGenTryGetBool(*Condition, bValue))
		{
			if (!DabGenSetPinDefault(Pin, bValue ? TEXT("true") : TEXT("false"), nullptr, Error))
			{
				Ctx.Sink.Error(TEXT("DAB7003"), FDabSpan(), FString::Printf(TEXT("%s: %s"), *Location, *Error));
			}
			return;
		}
		if (const FDabExpr* Unsupported = DabGenFindUnsupported(*Condition))
		{
			Ctx.Sink.Error(TEXT("DAB7002"), FDabSpan(), FString::Printf(TEXT("%s: the condition cannot be generated: %s"), *Location, *DabGenPrint(*Unsupported)));
			return;
		}

		FDabGenPath Path;
		if (DabGenTryGetPath(*Condition, Path) && (Path.Root == EDabGenPathRoot::Self || Path.Root == EDabGenPathRoot::Props) && !Path.Segments.IsEmpty())
		{
			// A plain read binds the result pin: no K2 nodes, what the editor's binding menu writes.
			DabGenBindPin(Ctx, ResultNode, TEXT("bCanEnterTransition"), Path.Segments, Path.LastIsCall(), Location);
			return;
		}

		FDabGenK2Builder Builder(Ctx, ResultNode->GetGraph(), GraphPath, Scope, Location, ResultNode->NodePosX, ResultNode->NodePosY);
		const FDabGenK2Builder::FValue Value = Builder.Build(*Condition, 0);
		if (!Builder.HasFailed() && Value.IsValid())
		{
			Builder.Feed(Value, Pin, nullptr, nullptr, *Condition);
		}
	}

	void DabGenBuildEventGraph(FDabGenContext& Ctx)
	{
		const bool bSlots = !Ctx.Model.AssetSlots.IsEmpty();
		if (Ctx.Model.Kind != EDabDocumentKind::AnimBlueprint || (Ctx.Model.Notifies.IsEmpty() && !bSlots))
		{
			return;
		}
		UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(Ctx.Blueprint);
		if (!EventGraph)
		{
			Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), TEXT("EventGraph: the Blueprint has no event graph for its notifies and animation set"));
			return;
		}

		int32 Row = 0;
		if (bSlots)
		{
			// The set swap: Pending becomes Active on the game thread, before the worker threads read Active.
			const UFunction* UpdateEvent = UAnimInstance::StaticClass()->FindFunctionByName(TEXT("BlueprintUpdateAnimation"));
			const UFunction* Apply = UDreamAnimLibrary::StaticClass()->FindFunctionByName(TEXT("ApplyPendingAnimSet"));
			if (!UpdateEvent || !Apply)
			{
				Ctx.Sink.Error(TEXT("DAB7004"), FDabSpan(), TEXT("EventGraph: BlueprintUpdateAnimation or ApplyPendingAnimSet is missing"));
			}
			else
			{
				FGraphNodeCreator<UK2Node_Event> Creator(*EventGraph);
				UK2Node_Event* Event = Creator.CreateNode(false);
				Event->EventReference.SetFromField<UFunction>(UpdateEvent, false);
				Event->bOverrideFunction = true;
				Creator.Finalize();
				Event->NodePosY = Row * 300;
				Ctx.AssignGuid(Event, TEXT("EventGraph"), TEXT("#UpdateAnimation"));

				FDabGenK2Builder Builder(Ctx, EventGraph, TEXT("EventGraph/UpdateAnimation"), FDabGenScope(), TEXT("EventGraph/BlueprintUpdateAnimation"), 0, Row * 300);
				UK2Node_CallFunction* Call = Builder.AddCall(Apply, 0);
				Call->NodePosX = 350;
				Call->NodePosY = Row * 300;
				UEdGraphPin* Self = Builder.AddSelf(0);
				const UEdGraphSchema* Schema = EventGraph->GetSchema();
				UEdGraphPin* EventThen = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
				UEdGraphPin* InstanceInput = Call->FindPin(TEXT("AnimInstance"), EGPD_Input);
				if (!EventThen || !Call->GetExecPin() || !Schema->TryCreateConnection(EventThen, Call->GetExecPin())
					|| !Self || !InstanceInput || !Schema->TryCreateConnection(Self, InstanceInput))
				{
					Ctx.Sink.Error(TEXT("DAB7005"), FDabSpan(), TEXT("EventGraph: the animation set update cannot be wired"));
				}
				++Row;
			}
		}

		for (const FDabModelNotify& Notify : Ctx.Model.Notifies)
		{
			const FString EventName = TEXT("AnimNotify_") + Notify.Name;
			const FString Location = TEXT("EventGraph/") + EventName;

			// What the notify's menu entry places: a custom-named event the instance calls when the notify fires.
			FGraphNodeCreator<UK2Node_Event> Creator(*EventGraph);
			UK2Node_Event* Event = Creator.CreateNode(false);
			Event->CustomFunctionName = FName(*EventName);
			Event->EventReference.SetExternalMember(Event->CustomFunctionName, UAnimInstance::StaticClass());
			Creator.Finalize();
			Event->NodePosY = Row * 300;
			Ctx.AssignGuid(Event, TEXT("EventGraph"), TEXT("Notify/") + Notify.Name);

			FDabGenK2Builder Builder(Ctx, EventGraph, TEXT("EventGraph/Notify/") + Notify.Name, FDabGenScope(), Location, 0, Row * 300 + 160);
			UEdGraphPin* Exec = Event->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
			int32 Column = 1;
			for (const FDabModelCall& Call : Notify.Calls)
			{
				if (Call.Target && DabGenFindUnsupported(*Call.Target))
				{
					Ctx.Sink.Error(TEXT("DAB7002"), FDabSpan(), FString::Printf(TEXT("%s: the call cannot be generated: %s"), *Location, *DabGenPrintCall(Call)));
					continue;
				}
				UK2Node_CallFunction* Node = Builder.BuildCall(Call, 0);
				if (!Node || !Exec)
				{
					continue;
				}
				Node->NodePosX = Column * 350;
				Node->NodePosY = Row * 300;
				++Column;
				if (UEdGraphPin* NodeExec = Node->GetExecPin())
				{
					Exec->MakeLinkTo(NodeExec);
					Exec = Node->GetThenPin();
				}
			}
			++Row;
		}
	}
}
