// Copyright DreamMoon. All Rights Reserved.
//
// LowerDabModel: model -> syntax tree, the second half of the decompiler. Docs/Mapping.md lists the
// choices; this file makes them in the order the "Lowering" section gives.

#include "Lower/DabLower.h"

#include "AnimGraphNode_Base.h"
#include "Animation/AnimClassInterface.h"
#include "Engine/Blueprint.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Animation/AnimNodeBase.h"
#include "Lang/DabPrinter.h"
#include "UObject/UObjectIterator.h"

namespace UE::DreamAnim::Compiler::LowerPrivate
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	// -- syntax-tree construction ---------------------------------------------------------------

	/** The lexer's identifier rule (Docs/Grammar.md): ASCII letters, `_`, any non-space character from U+0080 up; digits after the first. */
	bool IsDabIdentifierText(const FString& Text)
	{
		if (Text.IsEmpty() || Text == TEXT("true") || Text == TEXT("false"))
		{
			return false;
		}
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const TCHAR C = Text[Index];
			const bool bLetter = C == TEXT('_') || (static_cast<uint32>(C) < 0x80u ? FChar::IsAlpha(C) : !FChar::IsWhitespace(C));
			if (!bLetter && !(Index > 0 && FChar::IsDigit(C)))
			{
				return false;
			}
		}
		return true;
	}

	FDabExprPtr MakeName(const FString& Name)
	{
		TUniquePtr<FDabNameExpr> Expr = MakeUnique<FDabNameExpr>();
		Expr->Name = Name;
		return Expr;
	}

	FDabExprPtr MakeString(const FString& Value)
	{
		TUniquePtr<FDabLiteralExpr> Expr = MakeUnique<FDabLiteralExpr>();
		Expr->LiteralKind = EDabLiteralKind::String;
		Expr->Text = Value;
		return Expr;
	}

	FDabExprPtr MakeBool(bool bValue)
	{
		TUniquePtr<FDabLiteralExpr> Expr = MakeUnique<FDabLiteralExpr>();
		Expr->LiteralKind = EDabLiteralKind::Bool;
		Expr->bBool = bValue;
		Expr->Text = bValue ? TEXT("true") : TEXT("false");
		return Expr;
	}

	FDabExprPtr MakeNumber(int32 Value)
	{
		TUniquePtr<FDabLiteralExpr> Expr = MakeUnique<FDabLiteralExpr>();
		Expr->LiteralKind = EDabLiteralKind::Number;
		Expr->Number = Value;
		Expr->Text = FString::FromInt(Value);
		return Expr;
	}

	FDabExprPtr MakeMember(FDabExprPtr Object, const FString& Member)
	{
		TUniquePtr<FDabMemberExpr> Expr = MakeUnique<FDabMemberExpr>();
		Expr->Object = MoveTemp(Object);
		Expr->Member = Member;
		return Expr;
	}

	FDabArgumentPtr MakeArgument(const FString& Name, FDabExprPtr Value)
	{
		FDabArgumentPtr Argument = MakeUnique<FDabArgument>();
		Argument->Name = Name;
		// A name an identifier cannot spell (a curve or tag) is written as a string.
		Argument->bNameIsString = !Name.IsEmpty() && !IsDabIdentifierText(Name);
		Argument->Value = MoveTemp(Value);
		return Argument;
	}

	TUniquePtr<FDabCallExpr> MakeCall(FDabExprPtr Callee)
	{
		TUniquePtr<FDabCallExpr> Call = MakeUnique<FDabCallExpr>();
		Call->Callee = MoveTemp(Callee);
		return Call;
	}

	TUniquePtr<FDabAssignStmt> MakeAssign(const FString& Name, FDabExprPtr Value, TUniquePtr<FDabMeta> Meta = nullptr)
	{
		TUniquePtr<FDabAssignStmt> Assign = MakeUnique<FDabAssignStmt>();
		Assign->Name = Name;
		Assign->bNameIsString = !IsDabIdentifierText(Name);
		Assign->bHasSemicolon = !Value->Is<FDabBlockExpr>();
		Assign->Value = MoveTemp(Value);
		Assign->Meta = MoveTemp(Meta);
		return Assign;
	}

	TUniquePtr<FDabMetaEntry> MakeMetaEntry(const FString& Key, FDabExprPtr Value)
	{
		TUniquePtr<FDabMetaEntry> Entry = MakeUnique<FDabMetaEntry>();
		Entry->Key = Key;
		Entry->bKeyIsString = !IsDabIdentifierText(Key);
		Entry->Value = MoveTemp(Value);
		return Entry;
	}

	FDabExprPtr Clone(const FDabExprPtr& Expr)
	{
		return Expr ? CloneExpr(*Expr) : MakeName(TEXT("None"));
	}

	bool IsLiteralTrue(const FDabExpr* Expr)
	{
		const FDabLiteralExpr* Literal = Expr ? Expr->As<FDabLiteralExpr>() : nullptr;
		return Literal && Literal->LiteralKind == EDabLiteralKind::Bool && Literal->bBool;
	}

	bool IsLiteralBool(const FDabExpr* Expr, bool bValue)
	{
		const FDabLiteralExpr* Literal = Expr ? Expr->As<FDabLiteralExpr>() : nullptr;
		return Literal && Literal->LiteralKind == EDabLiteralKind::Bool && Literal->bBool == bValue;
	}

	/** Every expression slot directly under an expression, so a slot can be replaced. */
	void ForEachSubExpr(FDabExpr& Expr, TFunctionRef<void(FDabExprPtr&)> Visit)
	{
		switch (Expr.Kind)
		{
		case EDabNodeKind::MemberExpr: Visit(static_cast<FDabMemberExpr&>(Expr).Object); break;
		case EDabNodeKind::CallExpr:
			Visit(static_cast<FDabCallExpr&>(Expr).Callee);
			for (FDabArgumentPtr& Arg : static_cast<FDabCallExpr&>(Expr).Args)
			{
				Visit(Arg->Value);
			}
			break;
		case EDabNodeKind::ListExpr:
			for (FDabExprPtr& Item : static_cast<FDabListExpr&>(Expr).Items)
			{
				Visit(Item);
			}
			break;
		case EDabNodeKind::TupleExpr:
			for (FDabArgumentPtr& Item : static_cast<FDabTupleExpr&>(Expr).Items)
			{
				Visit(Item->Value);
			}
			break;
		case EDabNodeKind::StackExpr:
			for (FDabExprPtr& Step : static_cast<FDabStackExpr&>(Expr).Steps)
			{
				Visit(Step);
			}
			break;
		case EDabNodeKind::UnaryExpr: Visit(static_cast<FDabUnaryExpr&>(Expr).Operand); break;
		case EDabNodeKind::BinaryExpr:
			Visit(static_cast<FDabBinaryExpr&>(Expr).Lhs);
			Visit(static_cast<FDabBinaryExpr&>(Expr).Rhs);
			break;
		case EDabNodeKind::ConditionalExpr:
			Visit(static_cast<FDabConditionalExpr&>(Expr).Condition);
			Visit(static_cast<FDabConditionalExpr&>(Expr).Then);
			Visit(static_cast<FDabConditionalExpr&>(Expr).Else);
			break;
		case EDabNodeKind::ParenExpr: Visit(static_cast<FDabParenExpr&>(Expr).Inner); break;
		default: break;
		}
	}

	/** `Self.GetParent()` with no arguments. */
	bool IsSelfGetParent(const FDabExpr& Expr)
	{
		const FDabCallExpr* Call = Expr.As<FDabCallExpr>();
		if (!Call || !Call->Args.IsEmpty() || !Call->Callee)
		{
			return false;
		}
		const FDabMemberExpr* Member = Call->Callee->As<FDabMemberExpr>();
		const FDabNameExpr* Self = Member && Member->Object ? Member->Object->As<FDabNameExpr>() : nullptr;
		return Self && Self->Name == TEXT("Self") && Member->Member == TEXT("GetParent");
	}

	// -- engine knowledge -------------------------------------------------------------------------

	FString ReadNameOrString(const FDabExpr* Expr);

	UClass* FindNodeClass(const FString& ClassName)
	{
		return FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);
	}

	/**
	 * The name of the node's only pose pin, when its runtime struct has exactly one pose link that is not
	 * an array. Such a node can be a step of a stack.
	 */
	/** `Path(Plugin.ALS, "A/B")` -> `/ALS/A/B.B`; empty for anything else. */
	FString ObjectPathOf(const FDabExpr* Expr)
	{
		const FDabCallExpr* Call = Expr ? Expr->As<FDabCallExpr>() : nullptr;
		const FDabNameExpr* Callee = Call && Call->Callee ? Call->Callee->As<FDabNameExpr>() : nullptr;
		if (!Callee || Callee->Name != TEXT("Path") || Call->Args.Num() != 2 || !Call->Args[0]->Value || !Call->Args[1]->Value)
		{
			return FString();
		}
		TArray<FString> RootSegments;
		const FDabLiteralExpr* Rel = Call->Args[1]->Value->As<FDabLiteralExpr>();
		if (!Rel || !TryGetDabPath(*Call->Args[0]->Value, RootSegments))
		{
			return FString();
		}
		const FString Root = FString::Join(RootSegments, TEXT("."));
		FString Mount;
		if (Root == TEXT("Game") || Root == TEXT("Engine"))
		{
			Mount = Root;
		}
		else if (Root.StartsWith(TEXT("Plugin.")))
		{
			Mount = Root.RightChop(7);
		}
		else
		{
			return FString();
		}
		int32 Slash = INDEX_NONE;
		const FString Leaf = Rel->Text.FindLastChar(TEXT('/'), Slash) ? Rel->Text.Mid(Slash + 1) : Rel->Text;
		return FString::Printf(TEXT("/%s/%s.%s"), *Mount, *Rel->Text, *Leaf);
	}

	const FDabExpr* FindPropertyValue(const FDabModelNode& Node, const TCHAR* Name)
	{
		for (const FDabModelProperty& Property : Node.Properties)
		{
			if (Property.Name == Name)
			{
				return Property.Value.Get();
			}
		}
		return nullptr;
	}

	/** The input poses of the anim function FunctionName in the class an Animation Blueprint path names; -1 when unknown. */
	int32 CountClassInputPoses(const FDabExpr* ClassValue, FName FunctionName)
	{
		const FString ObjectPath = ObjectPathOf(ClassValue);
		const UBlueprint* Blueprint = ObjectPath.IsEmpty() ? nullptr : LoadObject<UBlueprint>(nullptr, *ObjectPath);
		const UClass* Class = Blueprint ? (Blueprint->GeneratedClass ? Blueprint->GeneratedClass.Get() : Blueprint->SkeletonGeneratedClass.Get()) : nullptr;
		const IAnimClassInterface* AnimClass = Class ? IAnimClassInterface::GetFromClass(Class) : nullptr;
		if (!AnimClass)
		{
			return -1;
		}
		for (const FAnimBlueprintFunction& Function : AnimClass->GetAnimBlueprintFunctions())
		{
			if (Function.Name == FunctionName)
			{
				return Function.InputPoseNames.Num();
			}
		}
		return -1;
	}

	/** How many input poses the graph a linked graph or layer node runs has; -1 when that cannot be found out. */
	int32 CountLinkedTargetInputs(const FDabModel& Model, const FDabModelNode& Node)
	{
		if (Node.ClassName == TEXT("AnimGraphNode_LinkedAnimGraph"))
		{
			return CountClassInputPoses(FindPropertyValue(Node, TEXT("InstanceClass")), TEXT("AnimGraph"));
		}
		const FString Layer = ReadNameOrString(FindPropertyValue(Node, TEXT("Layer")));
		if (const FDabExpr* Interface = FindPropertyValue(Node, TEXT("Interface")))
		{
			return CountClassInputPoses(Interface, FName(*Layer));
		}
		// A layer of this Blueprint: count the inputs its graph reads.
		for (const FDabModelLayer& Own : Model.Layers)
		{
			if (Own.Name == Layer)
			{
				TSet<FString> Inputs;
				for (const FDabModelNode& Inner : Own.Graph.Nodes)
				{
					if (!Inner.InputPoseName.IsEmpty())
					{
						Inputs.Add(Inner.InputPoseName);
					}
				}
				return Inputs.Num();
			}
		}
		return -1;
	}

	bool TryGetSinglePosePin(const FDabModel& Model, const FDabModelNode& Node, FString& OutPin)
	{
		if (Node.ClassName == TEXT("AnimGraphNode_LinkedAnimGraph") || Node.ClassName == TEXT("AnimGraphNode_LinkedAnimLayer"))
		{
			// Pins come from the target's input poses: stackable when the target has exactly one and it is linked.
			if (Node.PoseInputs.Num() == 1 && CountLinkedTargetInputs(Model, Node) == 1)
			{
				OutPin = Node.PoseInputs[0].PinName;
				return true;
			}
			return false;
		}

		UClass* Class = FindNodeClass(Node.ClassName);
		const FStructProperty* NodeProperty = Class ? CastField<FStructProperty>(Class->FindPropertyByName(TEXT("Node"))) : nullptr;
		if (!NodeProperty)
		{
			return false;
		}
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(NodeProperty->Struct); It; ++It)
		{
			if (const FStructProperty* Struct = CastField<FStructProperty>(*It))
			{
				if (Struct->Struct->IsChildOf(FPoseLinkBase::StaticStruct()))
				{
					++Count;
					OutPin = Struct->GetName();
				}
			}
			else if (const FArrayProperty* Array = CastField<FArrayProperty>(*It))
			{
				const FStructProperty* Inner = CastField<FStructProperty>(Array->Inner);
				if (Inner && Inner->Struct->IsChildOf(FPoseLinkBase::StaticStruct()))
				{
					Count += 2;
				}
			}
		}
		return Count == 1;
	}

	/** The type name the text uses for a node class: the part after `AnimGraphNode_`, unless another node class ends the same way. */
	FString GetNodeTypeName(const FString& ClassName)
	{
		static TMap<FString, FString> Cache;
		if (const FString* Cached = Cache.Find(ClassName))
		{
			return *Cached;
		}
		FString Short = ClassName;
		const int32 At = ClassName.Find(TEXT("AnimGraphNode_"), ESearchCase::CaseSensitive);
		if (At != INDEX_NONE)
		{
			Short = ClassName.Mid(At + 14);
		}
		int32 Matches = 0;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(UAnimGraphNode_Base::StaticClass()) && It->GetName().EndsWith(TEXT("AnimGraphNode_") + Short, ESearchCase::CaseSensitive))
			{
				++Matches;
			}
		}
		const FString Result = Matches > 1 ? ClassName : Short;
		Cache.Add(ClassName, Result);
		return Result;
	}

	// -- property grouping ------------------------------------------------------------------------

	/** `BlendTime[1]` -> ("BlendTime", 1); anything else -> (Name, INDEX_NONE). */
	void SplitIndexed(const FString& Name, FString& OutBase, int32& OutIndex)
	{
		OutBase = Name;
		OutIndex = INDEX_NONE;
		int32 Open = INDEX_NONE;
		if (Name.EndsWith(TEXT("]")) && Name.FindLastChar(TEXT('['), Open))
		{
			OutBase = Name.Left(Open);
			OutIndex = FCString::Atoi(*Name.Mid(Open + 1, Name.Len() - Open - 2));
		}
	}

	/** `Poses_2` -> ("Poses", 2) when the suffix is a number. */
	bool SplitPinIndex(const FString& Pin, FString& OutBase, int32& OutIndex)
	{
		int32 Underscore = INDEX_NONE;
		if (!Pin.FindLastChar(TEXT('_'), Underscore) || Underscore == Pin.Len() - 1)
		{
			return false;
		}
		const FString Suffix = Pin.Mid(Underscore + 1);
		for (const TCHAR C : Suffix)
		{
			if (!FChar::IsDigit(C))
			{
				return false;
			}
		}
		OutBase = Pin.Left(Underscore);
		OutIndex = FCString::Atoi(*Suffix);
		return true;
	}

	/** A node's arguments before they are written out, keyed by argument name, in first-seen order. */
	struct FArgs
	{
		TArray<FString> Order;
		TMap<FString, FDabExprPtr> Values;

		void Set(const FString& Name, FDabExprPtr Value)
		{
			if (!Values.Contains(Name))
			{
				Order.Add(Name);
			}
			Values.Add(Name, MoveTemp(Value));
		}

		FDabExprPtr Take(const FString& Name)
		{
			FDabExprPtr Value;
			if (Values.RemoveAndCopyValue(Name, Value))
			{
				Order.Remove(Name);
			}
			return Value;
		}

		bool Has(const FString& Name) const { return Values.Contains(Name); }
	};

	/** Lists built from indexed entries: base name -> index -> value. */
	using FIndexed = TMap<FString, TMap<int32, FDabExprPtr>>;

	/** A list from a list value overlaid with indexed entries. Missing indices get Default (cloned). */
	FDabExprPtr BuildList(FDabExprPtr Whole, TMap<int32, FDabExprPtr>* Overrides, int32 MinCount, const FDabExpr* Default)
	{
		TArray<FDabExprPtr> Items;
		if (Whole)
		{
			if (FDabListExpr* List = Whole->As<FDabListExpr>())
			{
				Items = MoveTemp(List->Items);
			}
		}
		int32 Count = FMath::Max(Items.Num(), MinCount);
		if (Overrides)
		{
			for (const TPair<int32, FDabExprPtr>& Pair : *Overrides)
			{
				Count = FMath::Max(Count, Pair.Key + 1);
			}
		}
		Items.SetNum(Count);
		if (Overrides)
		{
			for (TPair<int32, FDabExprPtr>& Pair : *Overrides)
			{
				Items[Pair.Key] = MoveTemp(Pair.Value);
			}
		}
		for (FDabExprPtr& Item : Items)
		{
			if (!Item)
			{
				Item = Default ? CloneExpr(*Default) : MakeNumber(0);
			}
		}
		TUniquePtr<FDabListExpr> List = MakeUnique<FDabListExpr>();
		List->Items = MoveTemp(Items);
		return List;
	}

	/** True when every item of a list prints the same. */
	bool AllItemsEqual(const FDabListExpr& List)
	{
		if (List.Items.Num() < 2)
		{
			return List.Items.Num() == 1;
		}
		const FString First = PrintDabExpr(*List.Items[0]);
		for (int32 Index = 1; Index < List.Items.Num(); ++Index)
		{
			if (PrintDabExpr(*List.Items[Index]) != First)
			{
				return false;
			}
		}
		return true;
	}

	/** An enumerator as the text writes it: `EAlsHipsDirection::LeftBackward` -> `LeftBackward`. */
	FString ShortEnumerator(const FString& Name)
	{
		int32 At = INDEX_NONE;
		return Name.FindLastChar(TEXT(':'), At) ? Name.Mid(At + 1) : Name;
	}

	FString ReadNameOrString(const FDabExpr* Expr)
	{
		if (!Expr)
		{
			return FString();
		}
		if (const FDabLiteralExpr* Literal = Expr->As<FDabLiteralExpr>())
		{
			return Literal->Text;
		}
		if (const FDabNameExpr* Name = Expr->As<FDabNameExpr>())
		{
			return Name->Name;
		}
		return PrintDabExpr(*Expr);
	}

	// -- lowering ---------------------------------------------------------------------------------

	/**
	 * Asset name -> how many registered assets carry it. Shared by every lowering: writing back lowers
	 * node by node, and walking the whole registry each time costs far more than the lowering itself.
	 */
	const TMap<FString, int32>& GetAssetNameCounts()
	{
		static TMap<FString, int32> Counts;
		static double LoadedAt = -1.0;
		const double Now = FPlatformTime::Seconds();
		if (LoadedAt >= 0.0 && Now - LoadedAt < 5.0)
		{
			return Counts;
		}
		LoadedAt = Now;
		Counts.Reset();
		IAssetRegistry& Registry = IAssetRegistry::GetChecked();
		if (Registry.IsLoadingAssets())
		{
			Registry.SearchAllAssets(true);
		}
		TArray<FAssetData> Assets;
		Registry.GetAllAssets(Assets, true);
		for (const FAssetData& Asset : Assets)
		{
			++Counts.FindOrAdd(Asset.AssetName.ToString());
		}
		return Counts;
	}

	struct FFileLowering
	{
		const FDabModel& Model;
		const FDabLowerOptions& Options;
		FDabFile& File;
		bool bUsedMain = false;
		TSet<FString> TakenNames;
		TMap<FString, int32> Counters;

		FFileLowering(const FDabModel& InModel, const FDabLowerOptions& InOptions, FDabFile& InFile)
			: Model(InModel), Options(InOptions), File(InFile)
		{
			for (const FDabModelStateMachine& Machine : Model.StateMachines)
			{
				TakenNames.Add(Machine.Name);
			}
			// A named pose called like a layer reads as the layer; keep them apart.
			for (const FDabModelLayer& Layer : Model.Layers)
			{
				TakenNames.Add(Layer.Name);
			}
			ForEachGraph([this](const FDabModelGraph& Graph)
			{
				for (const FDabModelNode& Node : Graph.Nodes)
				{
					if (!Node.CacheName.IsEmpty())
					{
						TakenNames.Add(Node.CacheName);
					}
				}
			});
		}

		void ForEachGraph(TFunctionRef<void(const FDabModelGraph&)> Visit) const
		{
			Visit(Model.AnimGraph);
			for (const FDabModelLayer& Layer : Model.Layers)
			{
				Visit(Layer.Graph);
			}
			for (const FDabModelStateMachine& Machine : Model.StateMachines)
			{
				for (const FDabModelState& State : Machine.States)
				{
					Visit(State.Graph);
				}
			}
		}

		/** Base itself when free, else Base + `Pose`, else numbered. */
		FString MakeUniqueName(const FString& Base)
		{
			if (!TakenNames.Contains(Base))
			{
				TakenNames.Add(Base);
				return Base;
			}
			if (!TakenNames.Contains(Base + TEXT("Pose")))
			{
				TakenNames.Add(Base + TEXT("Pose"));
				return Base + TEXT("Pose");
			}
			return MakeNumberedName(Base);
		}

		/** `MultiWayBlend1`, `MultiWayBlend2`: a name made from a node type is never the type's own name. */
		FString MakeNumberedName(const FString& Base)
		{
			int32& Counter = Counters.FindOrAdd(Base);
			FString Candidate;
			do
			{
				Candidate = FString::Printf(TEXT("%s%d"), *Base, ++Counter);
			}
			while (TakenNames.Contains(Candidate));
			TakenNames.Add(Candidate);
			return Candidate;
		}

		void BlankLineBefore(const FDabNode* Node)
		{
			File.Trivia.FindOrAdd(Node).BlankLinesBefore = 1;
		}

		FDabExprPtr Short(FDabExprPtr Expr)
		{
			if (Options.bShortAssetNames)
			{
				ShortenPaths(Expr);
			}
			return Expr;
		}

		/** A copy of a model value with `Self.GetParent()` written as `Main` and assets by name where that is unambiguous. */
		FDabExprPtr Value(const FDabExprPtr& Expr)
		{
			FDabExprPtr Copy = Clone(Expr);
			if (Options.bUseAliases)
			{
				RewriteAlias(Copy);
			}
			if (Options.bShortAssetNames)
			{
				ShortenPaths(Copy);
			}
			return Copy;
		}

		bool IsUniqueAssetName(const FString& Name)
		{
			return GetAssetNameCounts().FindRef(Name) == 1;
		}

		/** `Path(Plugin.ALS, "ALS/Animations/A_Als_Idle")` -> `A_Als_Idle` when no other asset has that name. */
		void ShortenPaths(FDabExprPtr& Slot)
		{
			if (!Slot)
			{
				return;
			}
			if (const FDabCallExpr* Call = Slot->As<FDabCallExpr>())
			{
				const FDabNameExpr* Callee = Call->Callee ? Call->Callee->As<FDabNameExpr>() : nullptr;
				const FDabLiteralExpr* Rel = Call->Args.Num() == 2 && Call->Args[1]->Value ? Call->Args[1]->Value->As<FDabLiteralExpr>() : nullptr;
				if (Callee && Callee->Name == TEXT("Path") && Rel && Rel->LiteralKind == EDabLiteralKind::String)
				{
					int32 Slash = INDEX_NONE;
					const FString Leaf = Rel->Text.FindLastChar(TEXT('/'), Slash) ? Rel->Text.Mid(Slash + 1) : Rel->Text;
					if (IsDabIdentifierText(Leaf) && IsUniqueAssetName(Leaf))
					{
						Slot = MakeName(Leaf);
					}
					return;
				}
			}
			ForEachSubExpr(*Slot, [this](FDabExprPtr& Child) { ShortenPaths(Child); });
		}

		void RewriteAlias(FDabExprPtr& Slot)
		{
			if (!Slot)
			{
				return;
			}
			if (IsSelfGetParent(*Slot))
			{
				Slot = MakeName(Options.ParentAlias);
				bUsedMain = true;
				return;
			}
			ForEachSubExpr(*Slot, [this](FDabExprPtr& Child) { RewriteAlias(Child); });
		}

		FDabExprPtr Call(const FDabModelCall& ModelCall)
		{
			TUniquePtr<FDabCallExpr> Result = MakeCall(MakeMember(Value(ModelCall.Target), ModelCall.Function));
			for (const FDabModelArgument& Arg : ModelCall.Args)
			{
				Result->Args.Add(MakeArgument(Arg.Name, Value(Arg.Value)));
			}
			return Result;
		}

		TUniquePtr<FDabMeta> Meta(const TArray<FDabModelProperty>& Properties)
		{
			if (Properties.IsEmpty())
			{
				return nullptr;
			}
			TUniquePtr<FDabMeta> Result = MakeUnique<FDabMeta>();
			for (const FDabModelProperty& Property : Properties)
			{
				// A null value is a flag (`[ Cached ]`-style).
				Result->Entries.Add(MakeMetaEntry(Property.Name, Property.Value ? Value(Property.Value) : nullptr));
			}
			return Result;
		}
	};

	/** Lowers one graph into the statements of the block it lives in. */
	struct FGraphLowering
	{
		FFileLowering& File;
		const FDabModelGraph& Graph;
		TArray<FDabStmtPtr>& Out;
		TSet<FString> Reached;
		/** Write DabPoseMarker for every pose argument instead of lowering the pose (one node at a time). */
		bool bPoseMarkers = false;

		FGraphLowering(FFileLowering& InFile, const FDabModelGraph& InGraph, TArray<FDabStmtPtr>& InOut)
			: File(InFile), Graph(InGraph), Out(InOut)
		{
		}

		const FDabModelNode* SourceOf(const FDabModelNode& Node, const FString& Pin) const
		{
			for (const FDabModelPoseLink& Link : Node.PoseInputs)
			{
				if (Link.PinName == Pin)
				{
					return Link.SourceNodeId.IsEmpty() ? nullptr : Graph.FindNode(Link.SourceNodeId);
				}
			}
			return nullptr;
		}

		/**
		 * Pulls an expression out into a named pose and returns its name: PreferredName (from the pin it
		 * feeds), else the node's Tag, else its type numbered.
		 */
		FDabExprPtr Hoist(const FDabModelNode& Node, FDabExprPtr Expr, TUniquePtr<FDabMeta> Meta = nullptr, const FString& PreferredName = FString())
		{
			FString Base = PreferredName;
			if (Base.IsEmpty() || !IsDabIdentifierText(Base))
			{
				Base.Reset();
				for (const FDabModelProperty& Property : Node.Properties)
				{
					if (Property.Name == TEXT("Tag"))
					{
						Base = ReadNameOrString(Property.Value.Get());
					}
				}
			}
			const bool bFromType = Base.IsEmpty() || !IsDabIdentifierText(Base);
			const FString Name = bFromType
				? File.MakeNumberedName(GetNodeTypeName(Node.ClassName).Replace(TEXT("_"), TEXT("")))
				: File.MakeUniqueName(Base);
			Out.Add(MakeAssign(Name, MoveTemp(Expr), MoveTemp(Meta)));
			return MakeName(Name);
		}

		/** A pose expression for a node, and how deeply its calls nest. */
		FDabExprPtr Pose(const FDabModelNode* Node, int32& OutDepth)
		{
			OutDepth = 0;
			if (!Node)
			{
				return MakeName(TEXT("None"));
			}
			Reached.Add(Node->Id);

			if (Node->ClassName == TEXT("AnimGraphNode_UseCachedPose") || Node->ClassName == TEXT("AnimGraphNode_StateMachine"))
			{
				FDabExprPtr Name = MakeName(Node->ClassName == TEXT("AnimGraphNode_StateMachine") ? Node->StateMachineName : Node->CacheName);
				if (Node->NodeFunctions.IsEmpty())
				{
					return Name;
				}
				// Functions hang on this use; the metadata of a named pose is where the text keeps them.
				return Hoist(*Node, MoveTemp(Name), NodeFunctionMeta(*Node));
			}

			// A chain of single-input nodes becomes a stack: walk down to its source.
			TArray<const FDabModelNode*> Chain;
			const FDabModelNode* Bottom = Node;
			FString Pin;
			while (TryGetSinglePosePin(File.Model, *Bottom, Pin) && Bottom->NodeFunctions.IsEmpty())
			{
				const FDabModelNode* Source = SourceOf(*Bottom, Pin);
				if (!Source)
				{
					break;
				}
				Chain.Add(Bottom);
				Reached.Add(Bottom->Id);
				Bottom = Source;
			}

			if (Chain.IsEmpty())
			{
				return Single(*Node, FString(), OutDepth);
			}

			TUniquePtr<FDabStackExpr> Stack = MakeUnique<FDabStackExpr>();
			int32 Depth = 0;
			int32 SourceDepth = 0;
			Stack->Steps.Add(Pose(Bottom, SourceDepth));
			Depth = FMath::Max(Depth, SourceDepth);
			for (int32 Index = Chain.Num() - 1; Index >= 0; --Index)
			{
				FString StepPin;
				TryGetSinglePosePin(File.Model, *Chain[Index], StepPin);
				int32 StepDepth = 0;
				Stack->Steps.Add(Single(*Chain[Index], StepPin, StepDepth));
				Depth = FMath::Max(Depth, StepDepth);
			}
			OutDepth = Depth;
			return Stack;
		}

		/** One node as a call. SkipPin names the pose pin a stack feeds, left out of the arguments. */
		FDabExprPtr Single(const FDabModelNode& Node, const FString& SkipPin, int32& OutDepth)
		{
			Reached.Add(Node.Id);
			int32 Depth = 0;
			FDabExprPtr Result;

			if (Node.ClassName == TEXT("AnimGraphNode_LinkedInputPose"))
			{
				TUniquePtr<FDabCallExpr> Input = MakeCall(MakeName(TEXT("Input")));
				// Input poses keep their engine names: a linked graph is found by them at run time.
				Input->Args.Add(MakeArgument(FString(), IsDabIdentifierText(Node.InputPoseName) ? MakeName(Node.InputPoseName) : MakeString(Node.InputPoseName)));
				Result = MoveTemp(Input);
			}
			else
			{
				Result = NodeCall(Node, SkipPin, Depth);
			}
			OutDepth = Depth + 1;

			if (!Node.NodeFunctions.IsEmpty())
			{
				OutDepth = 1;
				return Hoist(Node, MoveTemp(Result), NodeFunctionMeta(Node));
			}
			return Result;
		}

		/** `[ OnUpdate = Call; OnBecomeRelevant = Call ]` for a node's functions. */
		TUniquePtr<FDabMeta> NodeFunctionMeta(const FDabModelNode& Node)
		{
			TUniquePtr<FDabMeta> Meta = MakeUnique<FDabMeta>();
			for (const FDabModelNodeFunction& Function : Node.NodeFunctions)
			{
				Meta->Entries.Add(MakeMetaEntry(TEXT("On") + Function.Site, File.Call(Function.Call)));
			}
			return Meta;
		}

		/** `LocomotionInput` -> `Locomotion`; nothing for array pins and one-letter pins (`Poses_2`, `A`). */
		static FString NameForPin(const FString& Pin)
		{
			FString Base;
			int32 Index = INDEX_NONE;
			if (SplitPinIndex(Pin, Base, Index))
			{
				return FString();
			}
			Base = MakeDabIdentifier(Pin);
			for (const TCHAR* Suffix : { TEXT("Input"), TEXT("Pose") })
			{
				const int32 SuffixLength = FCString::Strlen(Suffix);
				if (Base.Len() > SuffixLength + 2 && Base.EndsWith(Suffix, ESearchCase::CaseSensitive))
				{
					Base.LeftChopInline(SuffixLength);
				}
			}
			return Base.Len() >= 3 ? Base : FString();
		}

		/** A pose argument: inline, or pulled out when it is a stack or nests too deep. */
		FDabExprPtr PoseArgument(const FDabModelNode* Source, int32& InOutDepth, const FString& Pin = FString())
		{
			if (bPoseMarkers)
			{
				return MakeName(DabPoseMarker);
			}
			int32 Depth = 0;
			FDabExprPtr Expr = Pose(Source, Depth);
			// A stack inside an argument list is hard to follow; it reads better as a named pose.
			if (Source && !Expr->Is<FDabNameExpr>() && (Depth >= File.Options.MaxInlineDepth || Expr->Is<FDabStackExpr>()))
			{
				Expr = Hoist(*Source, MoveTemp(Expr), nullptr, NameForPin(Pin));
				Depth = 0;
			}
			InOutDepth = FMath::Max(InOutDepth, Depth);
			return Expr;
		}

		FDabExprPtr NodeCall(const FDabModelNode& Node, const FString& SkipPin, int32& OutDepth)
		{
			const FString& Class = Node.ClassName;
			FArgs Args;
			FIndexed Indexed;

			for (const FDabModelProperty& Property : Node.Properties)
			{
				FString Base;
				int32 Index = INDEX_NONE;
				SplitIndexed(Property.Name, Base, Index);
				if (Index != INDEX_NONE)
				{
					Indexed.FindOrAdd(Base).Add(Index, File.Value(Property.Value));
				}
				else
				{
					Args.Set(Property.Name, File.Value(Property.Value));
				}
			}
			for (const FDabModelBinding& Binding : Node.Bindings)
			{
				FString Base;
				int32 Index = INDEX_NONE;
				if (SplitPinIndex(Binding.PinName, Base, Index) && (Args.Has(Base) || Indexed.Contains(Base)
					|| Base == TEXT("DesiredAlphas") || Base == TEXT("CurveValues") || Base == TEXT("BlendTime")))
				{
					Indexed.FindOrAdd(Base).Add(Index, File.Value(Binding.Value));
				}
				else
				{
					Args.Set(Binding.PinName, File.Value(Binding.Value));
				}
			}

			// Pose pins: arrays of pose links become lists (or keyed blocks below), linked-graph inputs an `Inputs` block.
			const bool bLinkedGraph = Class == TEXT("AnimGraphNode_LinkedAnimGraph") || Class == TEXT("AnimGraphNode_LinkedAnimLayer");
			TMap<FString, TMap<int32, FDabExprPtr>> PoseArrays;
			TUniquePtr<FDabBlockExpr> Inputs;
			for (const FDabModelPoseLink& Link : Node.PoseInputs)
			{
				if (Link.PinName == SkipPin)
				{
					continue;
				}
				const FDabModelNode* Source = Link.SourceNodeId.IsEmpty() ? nullptr : Graph.FindNode(Link.SourceNodeId);
				FDabExprPtr PoseExpr = PoseArgument(Source, OutDepth, Link.PinName);
				FString Base;
				int32 Index = INDEX_NONE;
				if (bLinkedGraph)
				{
					if (!Inputs)
					{
						Inputs = MakeUnique<FDabBlockExpr>();
					}
					Inputs->Statements.Add(MakeAssign(Link.PinName, MoveTemp(PoseExpr)));
				}
				else if (SplitPinIndex(Link.PinName, Base, Index))
				{
					PoseArrays.FindOrAdd(Base).Add(Index, MoveTemp(PoseExpr));
				}
				else
				{
					Args.Set(Link.PinName, MoveTemp(PoseExpr));
				}
			}

			int32 PoseArrayPins = 0;
			for (const TPair<FString, TMap<int32, FDabExprPtr>>& Pair : PoseArrays)
			{
				PoseArrayPins = FMath::Max(PoseArrayPins, Pair.Value.Num());
			}

			const bool bByEnum = Class == TEXT("AnimGraphNode_BlendListByEnum");
			const bool bByTag = Class.EndsWith(TEXT("AnimGraphNode_GameplayTagsBlend"));
			if ((bByEnum || bByTag) && PoseArrays.Contains(TEXT("BlendPose")) && HasKeyForEveryPose(bByEnum, Args, PoseArrays.FindChecked(TEXT("BlendPose"))))
			{
				LowerKeyedBlend(bByEnum, Args, Indexed, PoseArrays);
			}

			for (TPair<FString, TMap<int32, FDabExprPtr>>& Pair : PoseArrays)
			{
				Args.Set(Pair.Key, BuildList(nullptr, &Pair.Value, 0, nullptr));
			}

			if (Class == TEXT("AnimGraphNode_ModifyCurve"))
			{
				LowerCurves(Args, Indexed);
			}
			if (Class == TEXT("AnimGraphNode_SequenceEvaluator"))
			{
				// `ExplicitFrame = N` implies the flag; written alone, the flag must say when it is off.
				FDabExprPtr UseFrame = Args.Take(TEXT("bUseExplicitFrame"));
				const bool bUseFrame = UseFrame && IsLiteralBool(UseFrame.Get(), true);
				if (!bUseFrame && Args.Has(TEXT("ExplicitFrame")))
				{
					Args.Set(TEXT("bUseExplicitFrame"), UseFrame ? MoveTemp(UseFrame) : MakeBool(false));
				}
				else if (UseFrame && !bUseFrame)
				{
					Args.Set(TEXT("bUseExplicitFrame"), MoveTemp(UseFrame));
				}
				else if (bUseFrame && !Args.Has(TEXT("ExplicitFrame")))
				{
					Args.Set(TEXT("ExplicitFrame"), MakeNumber(0));
				}
			}
			if (Class == TEXT("AnimGraphNode_LayeredBoneBlend"))
			{
				LowerBones(Args);
			}

			// Whatever indexed entries are left become lists; a list whose entries are all equal and that
			// covers every pose pin becomes one value.
			for (TPair<FString, TMap<int32, FDabExprPtr>>& Pair : Indexed)
			{
				FDabExprPtr List = BuildList(Args.Take(Pair.Key), &Pair.Value, 0, nullptr);
				Args.Set(Pair.Key, MoveTemp(List));
			}
			CollapseUniformList(Args, TEXT("BlendTime"), PoseArrayPins);

			if (Inputs)
			{
				Args.Set(TEXT("Inputs"), MoveTemp(Inputs));
			}

			// Builtins with a positional spelling.
			if (Class == TEXT("AnimGraphNode_Slot"))
			{
				// The engine's default slot is not recorded in the model.
				TUniquePtr<FDabCallExpr> Slot = MakeCall(MakeName(TEXT("Slot")));
				const FDabExprPtr SlotName = Args.Take(TEXT("SlotName"));
				Slot->Args.Add(MakeArgument(FString(), MakeString(SlotName ? ReadNameOrString(SlotName.Get()) : FString(TEXT("DefaultSlot")))));
				AppendArgs(*Slot, Args);
				return Slot;
			}
			if (Node.Call.IsSet())
			{
				TUniquePtr<FDabCallExpr> CallNode = MakeCall(MakeName(TEXT("Call")));
				CallNode->Args.Add(MakeArgument(FString(), MakeMember(File.Value(Node.Call->Target), Node.Call->Function)));
				for (const FDabModelArgument& Arg : Node.Call->Args)
				{
					CallNode->Args.Add(MakeArgument(Arg.Name, File.Value(Arg.Value)));
				}
				if (FDabExprPtr Site = Args.Take(TEXT("CallSite")))
				{
					// EAnimFunctionCallSite::OnBecomeRelevant -> `On = BecomeRelevant`.
					FString SiteName = ShortEnumerator(ReadNameOrString(Site.Get()));
					SiteName.RemoveFromStart(TEXT("On"));
					CallNode->Args.Add(MakeArgument(TEXT("On"), MakeName(SiteName)));
				}
				AppendArgs(*CallNode, Args);
				return CallNode;
			}

			TUniquePtr<FDabCallExpr> Result = MakeCall(MakeName(GetNodeTypeName(Class)));
			AppendArgs(*Result, Args);
			return Result;
		}

		static void AppendArgs(FDabCallExpr& Call, FArgs& Args)
		{
			for (const FString& Name : Args.Order)
			{
				if (FDabExprPtr* Value = Args.Values.Find(Name))
				{
					Call.Args.Add(MakeArgument(Name, MoveTemp(*Value)));
				}
			}
		}

		/** One value for a per-pose list whose entries are all equal, when it has one entry per pose pin (the semantic pass widens it back). */
		static void CollapseUniformList(FArgs& Args, const TCHAR* Name, int32 PoseCount)
		{
			FDabExprPtr* Value = Args.Values.Find(Name);
			if (Value && *Value)
			{
				if (FDabListExpr* List = (*Value)->As<FDabListExpr>(); List && List->Items.Num() > 1 && List->Items.Num() == PoseCount && AllItemsEqual(*List))
				{
					*Value = MoveTemp(List->Items[0]);
				}
			}
		}

		/** The keyed form needs a key for every pose pin: pin 0 is Default, pin i the i-th enumerator or tag. */
		static bool HasKeyForEveryPose(bool bByEnum, const FArgs& Args, const TMap<int32, FDabExprPtr>& Poses)
		{
			const FDabExprPtr* KeySource = Args.Values.Find(bByEnum ? TEXT("VisibleEnumEntries") : TEXT("Tags"));
			const FDabListExpr* List = KeySource && *KeySource ? (*KeySource)->As<FDabListExpr>() : nullptr;
			const int32 KeyCount = 1 + (List ? List->Items.Num() : 0);
			for (const TPair<int32, FDabExprPtr>& Pose : Poses)
			{
				if (Pose.Key >= KeyCount)
				{
					return false;
				}
			}
			return true;
		}

		/** BlendListByEnum / GameplayTagsBlend: `BlendPose = { Default = …; Key = …; }`, `BlendTime` keyed the same way. */
		void LowerKeyedBlend(bool bByEnum, FArgs& Args, FIndexed& Indexed, TMap<FString, TMap<int32, FDabExprPtr>>& PoseArrays)
		{
			TMap<int32, FDabExprPtr> Poses = MoveTemp(PoseArrays.FindChecked(TEXT("BlendPose")));
			PoseArrays.Remove(TEXT("BlendPose"));

			// Keys: pin 0 is Default; pin i is the i-th visible enumerator or the i-th tag.
			TArray<FString> Keys;
			Keys.Add(TEXT("Default"));
			FDabExprPtr KeySource = Args.Take(bByEnum ? TEXT("VisibleEnumEntries") : TEXT("Tags"));
			if (KeySource)
			{
				if (const FDabListExpr* List = KeySource->As<FDabListExpr>())
				{
					for (const FDabExprPtr& Item : List->Items)
					{
						const FString Key = ReadNameOrString(Item.Get());
						Keys.Add(bByEnum ? ShortEnumerator(Key) : Key);
					}
				}
			}
			if (bByEnum)
			{
				// Implied by the keys; the semantic pass recomputes it.
				Args.Take(TEXT("EnumToPoseIndex"));
			}

			TUniquePtr<FDabBlockExpr> Block = MakeUnique<FDabBlockExpr>();
			TArray<int32> Indices;
			Poses.GetKeys(Indices);
			Indices.Sort();
			for (const int32 Index : Indices)
			{
				Block->Statements.Add(MakeAssign(Keys[Index], MoveTemp(Poses[Index])));
			}
			Args.Set(TEXT("BlendPose"), MoveTemp(Block));

			// BlendTime: one value when all equal, else keyed like BlendPose.
			TMap<int32, FDabExprPtr>* Overrides = Indexed.Find(TEXT("BlendTime"));
			FDabExprPtr Times = BuildList(Args.Take(TEXT("BlendTime")), Overrides, Indices.Num(), nullptr);
			Indexed.Remove(TEXT("BlendTime"));
			FDabListExpr* TimeList = Times->As<FDabListExpr>();
			if (TimeList && TimeList->Items.Num() != Indices.Num())
			{
				// Not one per pose: only the plain list says it exactly.
				Args.Set(TEXT("BlendTime"), MoveTemp(Times));
			}
			else if (TimeList && AllItemsEqual(*TimeList))
			{
				Args.Set(TEXT("BlendTime"), MoveTemp(TimeList->Items[0]));
			}
			else if (TimeList)
			{
				TUniquePtr<FDabBlockExpr> TimeBlock = MakeUnique<FDabBlockExpr>();
				for (int32 Index = 0; Index < TimeList->Items.Num(); ++Index)
				{
					const FString Key = Keys.IsValidIndex(Index) ? Keys[Index] : FString::Printf(TEXT("Pose%d"), Index);
					TimeBlock->Statements.Add(MakeAssign(Key, MoveTemp(TimeList->Items[Index])));
				}
				Args.Set(TEXT("BlendTime"), MoveTemp(TimeBlock));
			}
			if (bByEnum)
			{
				// Keep the enum explicit: it decides which keys exist.
				if (FDabExprPtr BoundEnum = Args.Take(TEXT("BoundEnum")))
				{
					// A native enum is recorded by its path; the text names it (`EAlsHipsDirection`).
					const FDabLiteralExpr* EnumPath = BoundEnum->As<FDabLiteralExpr>();
					int32 Dot = INDEX_NONE;
					if (EnumPath && EnumPath->LiteralKind == EDabLiteralKind::String && EnumPath->Text.StartsWith(TEXT("/Script/"))
						&& EnumPath->Text.FindLastChar(TEXT('.'), Dot))
					{
						BoundEnum = MakeName(EnumPath->Text.Mid(Dot + 1));
					}
					Args.Order.Insert(TEXT("BoundEnum"), 0);
					Args.Values.Add(TEXT("BoundEnum"), MoveTemp(BoundEnum));
				}
			}
		}

		/** ModifyCurve: `CurveNames` + `CurveValues` -> one argument per curve. */
		void LowerCurves(FArgs& Args, FIndexed& Indexed)
		{
			FDabExprPtr Names = Args.Take(TEXT("CurveNames"));
			const FDabListExpr* NameList = Names ? Names->As<FDabListExpr>() : nullptr;
			if (!NameList)
			{
				if (Names)
				{
					Args.Set(TEXT("CurveNames"), MoveTemp(Names));
				}
				return;
			}
			FDabExprPtr Values = BuildList(Args.Take(TEXT("CurveValues")), Indexed.Find(TEXT("CurveValues")), NameList->Items.Num(), nullptr);
			Indexed.Remove(TEXT("CurveValues"));
			FDabListExpr* ValueList = Values->As<FDabListExpr>();
			for (int32 Index = 0; Index < NameList->Items.Num(); ++Index)
			{
				const FString Curve = ReadNameOrString(NameList->Items[Index].Get());
				FDabExprPtr CurveValue = ValueList && ValueList->Items.IsValidIndex(Index) ? MoveTemp(ValueList->Items[Index]) : MakeNumber(0);
				Args.Set(Curve, MoveTemp(CurveValue));
			}
		}

		/** LayeredBoneBlend: each `(BranchFilters = [(BoneName = "x"), …])` with one shared depth -> `bones(x, …, Depth = n)`. */
		void LowerBones(FArgs& Args)
		{
			FDabExprPtr* Setup = Args.Values.Find(TEXT("LayerSetup"));
			FDabListExpr* Layers = Setup && *Setup ? (*Setup)->As<FDabListExpr>() : nullptr;
			if (!Layers)
			{
				return;
			}
			for (FDabExprPtr& Layer : Layers->Items)
			{
				const FDabTupleExpr* Tuple = Layer ? Layer->As<FDabTupleExpr>() : nullptr;
				if (!Tuple || Tuple->Items.Num() != 1 || Tuple->Items[0]->Name != TEXT("BranchFilters"))
				{
					continue;
				}
				const FDabListExpr* Filters = Tuple->Items[0]->Value ? Tuple->Items[0]->Value->As<FDabListExpr>() : nullptr;
				if (!Filters)
				{
					continue;
				}
				TArray<FString> Bones;
				FString Depth;
				bool bSimple = true;
				for (const FDabExprPtr& Filter : Filters->Items)
				{
					const FDabTupleExpr* Fields = Filter ? Filter->As<FDabTupleExpr>() : nullptr;
					if (!Fields)
					{
						bSimple = false;
						break;
					}
					FString Bone;
					FString ThisDepth = TEXT("0");
					for (const FDabArgumentPtr& Field : Fields->Items)
					{
						if (Field->Name == TEXT("BoneName"))
						{
							Bone = ReadNameOrString(Field->Value.Get());
						}
						else if (Field->Name == TEXT("BlendDepth"))
						{
							ThisDepth = ReadNameOrString(Field->Value.Get());
						}
						else
						{
							bSimple = false;
						}
					}
					if (Bones.IsEmpty())
					{
						Depth = ThisDepth;
					}
					bSimple &= (ThisDepth == Depth) && !Bone.IsEmpty();
					Bones.Add(Bone);
				}
				if (!bSimple || Bones.IsEmpty())
				{
					continue;
				}
				TUniquePtr<FDabCallExpr> BonesCall = MakeCall(MakeName(TEXT("bones")));
				for (const FString& Bone : Bones)
				{
					BonesCall->Args.Add(MakeArgument(FString(), IsDabIdentifierText(Bone) ? MakeName(Bone) : MakeString(Bone)));
				}
				if (Depth != TEXT("0"))
				{
					TUniquePtr<FDabLiteralExpr> DepthLiteral = MakeUnique<FDabLiteralExpr>();
					DepthLiteral->LiteralKind = EDabLiteralKind::Number;
					DepthLiteral->Text = Depth;
					DepthLiteral->Number = FCString::Atod(*Depth);
					BonesCall->Args.Add(MakeArgument(TEXT("Depth"), MoveTemp(DepthLiteral)));
				}
				Layer = MoveTemp(BonesCall);
			}
		}

		/** The graph's statements: its input order, caches, named poses, then the result under ResultName. */
		void Lower(const FString& ResultName)
		{
			if (Graph.InputPoses.Num() > 1)
			{
				// A linked graph feeds its pins to these by position.
				TUniquePtr<FDabListExpr> Inputs = MakeUnique<FDabListExpr>();
				for (const FString& Input : Graph.InputPoses)
				{
					Inputs->Items.Add(IsDabIdentifierText(Input) ? MakeName(Input) : MakeString(Input));
				}
				Out.Add(MakeAssign(TEXT("Inputs"), MoveTemp(Inputs)));
			}

			// Caches first, in name order, each as a named pose marked Cached.
			TArray<const FDabModelNode*> Saves;
			for (const FDabModelNode& Node : Graph.Nodes)
			{
				if (Node.ClassName == TEXT("AnimGraphNode_SaveCachedPose"))
				{
					Saves.Add(&Node);
				}
			}
			Saves.Sort([](const FDabModelNode& A, const FDabModelNode& B) { return A.CacheName < B.CacheName; });
			for (const FDabModelNode* Save : Saves)
			{
				Reached.Add(Save->Id);
				int32 Depth = 0;
				FDabExprPtr Expr = Pose(SourceOf(*Save, TEXT("Pose")), Depth);
				TUniquePtr<FDabMeta> CachedMeta = MakeUnique<FDabMeta>();
				CachedMeta->Entries.Add(MakeMetaEntry(TEXT("Cached"), nullptr));
				Out.Add(MakeAssign(Save->CacheName, MoveTemp(Expr), MoveTemp(CachedMeta)));
			}

			int32 Depth = 0;
			FDabExprPtr Result = Pose(Graph.ResultNodeId.IsEmpty() ? nullptr : Graph.FindNode(Graph.ResultNodeId), Depth);

			// Nodes no result or cache reaches: keep them as unused named poses so nothing is lost.
			for (const FDabModelNode& Node : Graph.Nodes)
			{
				if (!Reached.Contains(Node.Id) && !IsConsumed(Node))
				{
					int32 UnusedDepth = 0;
					FDabExprPtr Expr = Pose(&Node, UnusedDepth);
					Out.Add(MakeAssign(File.MakeNumberedName(TEXT("Unused")), MoveTemp(Expr)));
				}
			}

			Out.Add(MakeAssign(ResultName, MoveTemp(Result)));

			// Air around anything that spans lines, and before the result when something precedes it.
			for (int32 Index = 1; Index < Out.Num(); ++Index)
			{
				if (Index == Out.Num() - 1 || IsBulky(*Out[Index]) || IsBulky(*Out[Index - 1]))
				{
					File.BlankLineBefore(Out[Index].Get());
				}
			}
		}

		/** A statement that will print over several lines: a stack, a block, or a call holding other calls. */
		static bool IsBulky(const FDabStmt& Stmt)
		{
			const FDabAssignStmt* Assign = Stmt.As<FDabAssignStmt>();
			const FDabExpr* Value = Assign ? Assign->Value.Get() : nullptr;
			if (!Value)
			{
				return false;
			}
			if (Value->Is<FDabStackExpr>() || Value->Is<FDabBlockExpr>())
			{
				return true;
			}
			const FDabCallExpr* Call = Value->As<FDabCallExpr>();
			if (!Call)
			{
				return false;
			}
			if (Call->Args.Num() > 3)
			{
				return true;
			}
			for (const FDabArgumentPtr& Arg : Call->Args)
			{
				const FDabExpr* ArgValue = Arg->Value.Get();
				if (ArgValue && (ArgValue->Is<FDabCallExpr>() || ArgValue->Is<FDabStackExpr>() || ArgValue->Is<FDabBlockExpr>() || ArgValue->Is<FDabListExpr>()))
				{
					return true;
				}
			}
			return false;
		}

		bool IsConsumed(const FDabModelNode& Node) const
		{
			for (const FDabModelNode& Other : Graph.Nodes)
			{
				for (const FDabModelPoseLink& Link : Other.PoseInputs)
				{
					if (Link.SourceNodeId == Node.Id)
					{
						return true;
					}
				}
			}
			return false;
		}
	};

	FString RootOfAssetPath(const FString& AssetPath, FString& OutName)
	{
		// `/Mount/Rel/Path.Leaf` -> Root, `Rel/Path`.
		FString Package = AssetPath;
		int32 Dot = INDEX_NONE;
		if (Package.FindLastChar(TEXT('.'), Dot))
		{
			Package = Package.Left(Dot);
		}
		Package.RemoveFromStart(TEXT("/"));
		FString Mount;
		if (!Package.Split(TEXT("/"), &Mount, &OutName))
		{
			Mount = TEXT("Game");
			OutName = Package;
		}
		if (Mount == TEXT("Game") || Mount == TEXT("Engine"))
		{
			return Mount;
		}
		return TEXT("Plugin.") + Mount;
	}

	FDabExprPtr PathValue(const FString& ObjectPath)
	{
		FString Rel;
		const FString Root = RootOfAssetPath(ObjectPath, Rel);
		TUniquePtr<FDabCallExpr> Call = MakeCall(MakeName(TEXT("Path")));
		FString Left;
		FString Right;
		if (Root.Split(TEXT("."), &Left, &Right))
		{
			Call->Args.Add(MakeArgument(FString(), MakeMember(MakeName(Left), Right)));
		}
		else
		{
			Call->Args.Add(MakeArgument(FString(), MakeName(Root)));
		}
		Call->Args.Add(MakeArgument(FString(), MakeString(Rel)));
		return Call;
	}

	FDabExprPtr ClassValue(const FString& ClassPath)
	{
		// `/Script/Module.Class` -> the bare class name; a Blueprint class -> its Blueprint's Path.
		if (ClassPath.StartsWith(TEXT("/Script/")))
		{
			int32 Dot = INDEX_NONE;
			return MakeName(ClassPath.FindLastChar(TEXT('.'), Dot) ? ClassPath.Mid(Dot + 1) : ClassPath);
		}
		FString Path = ClassPath;
		Path.RemoveFromEnd(TEXT("_C"));
		return PathValue(Path);
	}
}

namespace UE::DreamAnim::Compiler::LowerPrivate
{
	// The body lives in this namespace, not behind a using-directive: unity builds merge .cpp files, and a
	// directive at UE::DreamAnim::Compiler scope would leak these helpers into the files after this one.
	/** A one-line state (`State Stop { Pose = StopStates; }`), a conduit, an alias or `Entry`: runs of these need no blank lines. */
	bool IsSmallMachineMember(const FDabStmt& Stmt)
	{
		if (const FDabAssignStmt* Assign = Stmt.As<FDabAssignStmt>())
		{
			return Assign->Name == TEXT("Entry");
		}
		const FDabNamedBlockStmt* Named = Stmt.As<FDabNamedBlockStmt>();
		if (!Named)
		{
			return false;
		}
		if (Named->Word != TEXT("State"))
		{
			return true;
		}
		if (!Named->Body || Named->Body->Statements.Num() != 1)
		{
			return false;
		}
		const FDabAssignStmt* Only = Named->Body->Statements[0]->As<FDabAssignStmt>();
		return Only && Only->Value && Only->Value->Is<FDabNameExpr>();
	}

	/** One row of a Transitions table. */
	TUniquePtr<FDabTransitionStmt> LowerTransitionRow(FFileLowering& File, const FDabModelTransition& Transition)
	{
		TUniquePtr<FDabTransitionStmt> Row = MakeUnique<FDabTransitionStmt>();
		Row->From = Transition.From;
		Row->To = Transition.To;
		if (Transition.ConditionKind == EDabConditionKind::Auto)
		{
			Row->Condition = MakeName(TEXT("Auto"));
		}
		else if (!Transition.SharedRuleName.IsEmpty())
		{
			Row->Condition = MakeName(Transition.SharedRuleName);
		}
		else
		{
			Row->Condition = File.Value(Transition.Condition);
		}

		TUniquePtr<FDabMeta> RowOptions = MakeUnique<FDabMeta>();
		for (const FDabModelProperty& Option : Transition.Options)
		{
			if (Option.Name == TEXT("CrossfadeDuration"))
			{
				RowOptions->Entries.Add(MakeMetaEntry(TEXT("Blend"), File.Value(Option.Value)));
			}
			else if (Option.Name == TEXT("BlendMode"))
			{
				RowOptions->Entries.Add(MakeMetaEntry(TEXT("Curve"), File.Value(Option.Value)));
			}
			else if (Option.Name == TEXT("PriorityOrder"))
			{
				RowOptions->Entries.Add(MakeMetaEntry(TEXT("Priority"), File.Value(Option.Value)));
			}
			else if (Option.Name == TEXT("LogicType") && ReadNameOrString(Option.Value.Get()).Contains(TEXT("Inertialization")))
			{
				RowOptions->Entries.Add(MakeMetaEntry(TEXT("Inertialize"), nullptr));
			}
			else
			{
				RowOptions->Entries.Add(MakeMetaEntry(Option.Name, File.Value(Option.Value)));
			}
		}
		if (!RowOptions->Entries.IsEmpty())
		{
			Row->Meta = MoveTemp(RowOptions);
		}
		return Row;
	}

	/** A whole `StateMachine Name [meta] { ... }` statement. */
	TUniquePtr<FDabNamedBlockStmt> LowerMachine(FFileLowering& File, const FDabModelStateMachine& Machine)
	{
		TUniquePtr<FDabNamedBlockStmt> MachineStmt = MakeUnique<FDabNamedBlockStmt>();
		MachineStmt->Word = TEXT("StateMachine");
		MachineStmt->Name = Machine.Name;
		MachineStmt->Meta = File.Meta(Machine.Properties);
		MachineStmt->Body = MakeUnique<FDabBlockExpr>();
		TArray<FDabStmtPtr>& Members = MachineStmt->Body->Statements;
		if (!Machine.Entry.IsEmpty())
		{
			Members.Add(MakeAssign(TEXT("Entry"), MakeName(Machine.Entry)));
		}

		for (const FDabModelState& State : Machine.States)
		{
			TUniquePtr<FDabNamedBlockStmt> StateStmt = MakeUnique<FDabNamedBlockStmt>();
			StateStmt->Word = TEXT("State");
			StateStmt->Name = State.Name;
			StateStmt->Meta = File.Meta(State.Properties);
			StateStmt->Body = MakeUnique<FDabBlockExpr>();
			if (State.OnEnter.IsSet())
			{
				StateStmt->Body->Statements.Add(MakeAssign(TEXT("OnEnter"), File.Call(*State.OnEnter)));
			}
			if (State.OnExit.IsSet())
			{
				StateStmt->Body->Statements.Add(MakeAssign(TEXT("OnExit"), File.Call(*State.OnExit)));
			}
			if (State.OnBlended.IsSet())
			{
				StateStmt->Body->Statements.Add(MakeAssign(TEXT("OnBlended"), File.Call(*State.OnBlended)));
			}
			FGraphLowering(File, State.Graph, StateStmt->Body->Statements).Lower(TEXT("Pose"));
			Members.Add(MoveTemp(StateStmt));
		}

		for (const FDabModelConduit& Conduit : Machine.Conduits)
		{
			TUniquePtr<FDabNamedBlockStmt> ConduitStmt = MakeUnique<FDabNamedBlockStmt>();
			ConduitStmt->Word = TEXT("Conduit");
			ConduitStmt->Name = Conduit.Name;
			if (Conduit.Condition && !IsLiteralTrue(Conduit.Condition.Get()))
			{
				ConduitStmt->Value = File.Value(Conduit.Condition);
			}
			Members.Add(MoveTemp(ConduitStmt));
		}

		for (const FDabModelAlias& Alias : Machine.Aliases)
		{
			TUniquePtr<FDabNamedBlockStmt> AliasStmt = MakeUnique<FDabNamedBlockStmt>();
			AliasStmt->Word = TEXT("Alias");
			AliasStmt->Name = Alias.Name;
			if (Alias.bGlobal)
			{
				AliasStmt->Value = MakeName(TEXT("All"));
			}
			else
			{
				TUniquePtr<FDabListExpr> States = MakeUnique<FDabListExpr>();
				for (const FString& State : Alias.States)
				{
					States->Items.Add(MakeName(State));
				}
				AliasStmt->Value = MoveTemp(States);
			}
			Members.Add(MoveTemp(AliasStmt));
		}

		// Shared rules, each once, in first-use order.
		TUniquePtr<FDabBlockExpr> Rules = MakeUnique<FDabBlockExpr>();
		TSet<FString> RulesSeen;
		for (const FDabModelTransition& Transition : Machine.Transitions)
		{
			if (!Transition.SharedRuleName.IsEmpty() && !RulesSeen.Contains(Transition.SharedRuleName))
			{
				RulesSeen.Add(Transition.SharedRuleName);
				Rules->Statements.Add(MakeAssign(Transition.SharedRuleName, File.Value(Transition.Condition)));
			}
		}
		if (!Rules->Statements.IsEmpty())
		{
			Members.Add(MakeAssign(TEXT("Rules"), MoveTemp(Rules)));
		}

		if (!Machine.Transitions.IsEmpty())
		{
			TUniquePtr<FDabBlockExpr> Transitions = MakeUnique<FDabBlockExpr>();
			for (const FDabModelTransition& Transition : Machine.Transitions)
			{
				Transitions->Statements.Add(LowerTransitionRow(File, Transition));
			}
			Members.Add(MakeAssign(TEXT("Transitions"), MoveTemp(Transitions)));
		}
		for (int32 Index = 1; Index < Members.Num(); ++Index)
		{
			if (!IsSmallMachineMember(*Members[Index]) || !IsSmallMachineMember(*Members[Index - 1])
				|| Members[Index]->Kind != Members[Index - 1]->Kind)
			{
				File.BlankLineBefore(Members[Index].Get());
			}
		}
		return MachineStmt;
	}

	TUniquePtr<FDabFile> LowerModel(const FDabModel& Model, const FDabLowerOptions& Options)
	{
		TUniquePtr<FDabFile> Result = MakeUnique<FDabFile>();
		FFileLowering File(Model, Options, *Result);
		Result->Path = Model.AssetPath;
		TUniquePtr<FDabDocument> Document = MakeUnique<FDabDocument>();
		Document->DocumentKind = Model.Kind;
		Document->KindName = LexToString(Model.Kind);
		FString Name;
		const FString Root = RootOfAssetPath(Model.AssetPath, Name);
		Document->HeaderArgs.Add(MakeArgument(TEXT("Name"), MakeString(Name)));
		Document->HeaderArgs.Add(MakeArgument(TEXT("Root"), MakeString(Root)));
		Document->Body = MakeUnique<FDabBlockExpr>();
		TArray<FDabStmtPtr>& Body = Document->Body->Statements;

		// Settings.
		{
			TUniquePtr<FDabBlockExpr> Settings = MakeUnique<FDabBlockExpr>();
			if (!Model.ParentClass.IsEmpty())
			{
				Settings->Statements.Add(MakeAssign(TEXT("Parent"), File.Short(ClassValue(Model.ParentClass))));
			}
			if (!Model.Skeleton.IsEmpty())
			{
				Settings->Statements.Add(MakeAssign(TEXT("Skeleton"), File.Short(PathValue(Model.Skeleton))));
			}
			if (!Model.Interfaces.IsEmpty())
			{
				TUniquePtr<FDabListExpr> List = MakeUnique<FDabListExpr>();
				for (const FString& Interface : Model.Interfaces)
				{
					List->Items.Add(File.Short(PathValue(Interface)));
				}
				Settings->Statements.Add(MakeAssign(TEXT("Implements"), MoveTemp(List)));
			}
			if (!Model.DefaultAnimSet.IsEmpty())
			{
				Settings->Statements.Add(MakeAssign(TEXT("DefaultAnimSet"), File.Short(PathValue(Model.DefaultAnimSet))));
			}
			for (const FDabModelProperty& Setting : Model.ClassSettings)
			{
				Settings->Statements.Add(MakeAssign(Setting.Name, File.Value(Setting.Value)));
			}
			if (!Settings->Statements.IsEmpty())
			{
				Body.Add(MakeAssign(TEXT("Settings"), MoveTemp(Settings)));
			}
		}
		if (!Model.ClassDefaults.IsEmpty())
		{
			TUniquePtr<FDabBlockExpr> Defaults = MakeUnique<FDabBlockExpr>();
			for (const FDabModelProperty& Default : Model.ClassDefaults)
			{
				Defaults->Statements.Add(MakeAssign(Default.Name, File.Value(Default.Value)));
			}
			Body.Add(MakeAssign(TEXT("Defaults"), MoveTemp(Defaults)));
		}
		const int32 AliasInsertAt = Body.Num();

		if (!Model.Variables.IsEmpty())
		{
			TUniquePtr<FDabBlockExpr> Properties = MakeUnique<FDabBlockExpr>();
			for (const FDabModelVariable& Variable : Model.Variables)
			{
				TUniquePtr<FDabDeclStmt> Decl = MakeUnique<FDabDeclStmt>();
				Decl->TypeName = Variable.Type;
				Decl->Name = Variable.Name;
				if (Variable.Default)
				{
					Decl->Value = File.Value(Variable.Default);
				}
				Decl->Meta = File.Meta(Variable.Meta);
				Properties->Statements.Add(MoveTemp(Decl));
			}
			Body.Add(MakeAssign(TEXT("Properties"), MoveTemp(Properties)));
		}

		if (!Model.AssetSlots.IsEmpty())
		{
			TUniquePtr<FDabBlockExpr> Assets = MakeUnique<FDabBlockExpr>();
			for (const FDabModelAssetSlot& Slot : Model.AssetSlots)
			{
				TUniquePtr<FDabDeclStmt> Decl = MakeUnique<FDabDeclStmt>();
				Decl->TypeName = Slot.Type;
				Decl->Name = Slot.Name;
				Assets->Statements.Add(MoveTemp(Decl));
			}
			Body.Add(MakeAssign(TEXT("Assets"), MoveTemp(Assets)));
		}

		if (!Model.Notifies.IsEmpty())
		{
			TUniquePtr<FDabBlockExpr> Notifies = MakeUnique<FDabBlockExpr>();
			for (const FDabModelNotify& Notify : Model.Notifies)
			{
				if (Notify.Calls.Num() == 1)
				{
					Notifies->Statements.Add(MakeAssign(Notify.Name, File.Call(Notify.Calls[0])));
				}
				else
				{
					TUniquePtr<FDabStackExpr> Calls = MakeUnique<FDabStackExpr>();
					for (const FDabModelCall& Call : Notify.Calls)
					{
						Calls->Steps.Add(File.Call(Call));
					}
					Notifies->Statements.Add(MakeAssign(Notify.Name, MoveTemp(Calls)));
				}
			}
			Body.Add(MakeAssign(TEXT("Notifies"), MoveTemp(Notifies)));
		}

		if (Model.Kind != EDabDocumentKind::AnimLayerInterface)
		{
			TUniquePtr<FDabBlockExpr> GraphBlock = MakeUnique<FDabBlockExpr>();
			FGraphLowering(File, Model.AnimGraph, GraphBlock->Statements).Lower(TEXT("Output"));
			Body.Add(MakeAssign(TEXT("Graph"), MoveTemp(GraphBlock)));
		}

		for (const FDabModelLayer& Layer : Model.Layers)
		{
			TUniquePtr<FDabNamedBlockStmt> LayerStmt = MakeUnique<FDabNamedBlockStmt>();
			LayerStmt->Word = TEXT("Layer");
			LayerStmt->Name = Layer.Name;
			LayerStmt->Meta = File.Meta(Layer.Graph.ResultProperties);
			if (!Layer.bDeclarationOnly && Model.Kind != EDabDocumentKind::AnimLayerInterface)
			{
				TArray<FDabStmtPtr> Statements;
				FGraphLowering(File, Layer.Graph, Statements).Lower(TEXT("Output"));
				if (Statements.Num() == 1)
				{
					LayerStmt->Value = MoveTemp(static_cast<FDabAssignStmt&>(*Statements[0]).Value);
				}
				else
				{
					LayerStmt->Body = MakeUnique<FDabBlockExpr>();
					LayerStmt->Body->Statements = MoveTemp(Statements);
				}
			}
			Body.Add(MoveTemp(LayerStmt));
		}

		for (const FDabModelStateMachine& Machine : Model.StateMachines)
		{
			Body.Add(LowerMachine(File, Machine));
		}

		if (File.bUsedMain)
		{
			TUniquePtr<FDabBlockExpr> Aliases = MakeUnique<FDabBlockExpr>();
			TUniquePtr<FDabCallExpr> GetParent = MakeCall(MakeMember(MakeName(TEXT("Self")), TEXT("GetParent")));
			Aliases->Statements.Add(MakeAssign(Options.ParentAlias, MoveTemp(GetParent)));
			Body.Insert(MakeAssign(TEXT("Aliases"), MoveTemp(Aliases)), AliasInsertAt);
		}

		for (int32 Index = 1; Index < Body.Num(); ++Index)
		{
			File.BlankLineBefore(Body[Index].Get());
		}

		FDabTrivia& Trivia = Result->Trivia.FindOrAdd(Document.Get());
		if (!Options.Renames.IsEmpty())
		{
			FDabComment Header;
			Header.Text = TEXT("// Renamed from the Blueprint:");
			Trivia.Leading.Add(Header);
			// Not GetKeys: it folds keys that differ only in case.
			TArray<FString> Originals;
			for (const TPair<FString, FString>& Pair : Options.Renames)
			{
				Originals.Add(Pair.Key);
			}
			Originals.Sort();
			for (const FString& Original : Originals)
			{
				FDabComment Line;
				Line.Text = FString::Printf(TEXT("//   %s <- \"%s\""), *Options.Renames[Original], *Original);
				Trivia.Leading.Add(Line);
			}
		}
		// What the language cannot say goes in a comment at the top, so it is visible where it was lost.
		if (!Model.Unsupported.IsEmpty())
		{
			FDabComment Header;
			Header.Text = TEXT("// Not expressible in DreamAnim; move into C++:");
			Trivia.Leading.Add(Header);
			for (const FDabModelUnsupported& Unsupported : Model.Unsupported)
			{
				FDabComment Line;
				Line.Text = FString::Printf(TEXT("//   %s: %s"), *Unsupported.Location, *Unsupported.Description);
				Trivia.Leading.Add(Line);
			}
		}

		Result->Document = MoveTemp(Document);
		return Result;
	}
}

namespace UE::DreamAnim::Compiler::LowerPrivate
{
	FDabExprPtr LowerNodeCall(const FDabModel& Model, const FDabModelGraph& Graph, const FDabModelNode& Node, const FString& SkipPin, const FDabLowerOptions& Options)
	{
		FDabFile Scratch;
		FFileLowering File(Model, Options, Scratch);
		if (Node.ClassName == TEXT("AnimGraphNode_UseCachedPose"))
		{
			return MakeName(Node.CacheName);
		}
		if (Node.ClassName == TEXT("AnimGraphNode_StateMachine"))
		{
			return MakeName(Node.StateMachineName);
		}
		if (Node.ClassName == TEXT("AnimGraphNode_LinkedInputPose"))
		{
			TUniquePtr<FDabCallExpr> Input = MakeCall(MakeName(TEXT("Input")));
			Input->Args.Add(MakeArgument(FString(), IsDabIdentifierText(Node.InputPoseName) ? MakeName(Node.InputPoseName) : MakeString(Node.InputPoseName)));
			return Input;
		}
		TArray<FDabStmtPtr> Hoisted;
		FGraphLowering Lowering(File, Graph, Hoisted);
		Lowering.bPoseMarkers = true;
		int32 Depth = 0;
		return Lowering.NodeCall(Node, SkipPin, Depth);
	}

	TArray<FDabStmtPtr> LowerGraph(const FDabModel& Model, const FDabModelGraph& Graph, const FString& ResultName, FDabFile& TriviaFile, const FDabLowerOptions& Options)
	{
		FFileLowering File(Model, Options, TriviaFile);
		TArray<FDabStmtPtr> Statements;
		FGraphLowering(File, Graph, Statements).Lower(ResultName);
		return Statements;
	}
}

namespace UE::DreamAnim::Compiler
{
	const TCHAR* const DabPoseMarker = TEXT("__DabPose__");

	TUniquePtr<Lang::FDabFile> LowerDabModel(const Model::FDabModel& Model, const FDabLowerOptions& Options)
	{
		return LowerPrivate::LowerModel(Model, Options);
	}

	Lang::FDabExprPtr LowerDabNodeCall(const Model::FDabModel& Model, const Model::FDabModelGraph& Graph, const Model::FDabModelNode& Node,
		const FString& SkipPin, const FDabLowerOptions& Options)
	{
		return LowerPrivate::LowerNodeCall(Model, Graph, Node, SkipPin, Options);
	}

	Lang::FDabExprPtr LowerDabValue(const Model::FDabModel& Model, const Lang::FDabExpr& Value, const FDabLowerOptions& Options)
	{
		Lang::FDabFile Scratch;
		LowerPrivate::FFileLowering File(Model, Options, Scratch);
		const Lang::FDabExprPtr Copy = Lang::CloneExpr(Value);
		return File.Value(Copy);
	}

	Lang::FDabExprPtr LowerDabCall(const Model::FDabModel& Model, const Model::FDabModelCall& Call, const FDabLowerOptions& Options)
	{
		Lang::FDabFile Scratch;
		LowerPrivate::FFileLowering File(Model, Options, Scratch);
		return File.Call(Call);
	}

	TUniquePtr<Lang::FDabMeta> LowerDabMeta(const Model::FDabModel& Model, const TArray<Model::FDabModelProperty>& Properties, const FDabLowerOptions& Options)
	{
		Lang::FDabFile Scratch;
		LowerPrivate::FFileLowering File(Model, Options, Scratch);
		return File.Meta(Properties);
	}

	TUniquePtr<Lang::FDabTransitionStmt> LowerDabTransition(const Model::FDabModel& Model, const Model::FDabModelTransition& Transition, const FDabLowerOptions& Options)
	{
		Lang::FDabFile Scratch;
		LowerPrivate::FFileLowering File(Model, Options, Scratch);
		return LowerPrivate::LowerTransitionRow(File, Transition);
	}

	TArray<Lang::FDabStmtPtr> LowerDabGraph(const Model::FDabModel& Model, const Model::FDabModelGraph& Graph, const FString& ResultName,
		Lang::FDabFile& TriviaFile, const FDabLowerOptions& Options)
	{
		return LowerPrivate::LowerGraph(Model, Graph, ResultName, TriviaFile, Options);
	}

	TUniquePtr<Lang::FDabNamedBlockStmt> LowerDabStateMachine(const Model::FDabModel& Model, const Model::FDabModelStateMachine& Machine,
		Lang::FDabFile& TriviaFile, const FDabLowerOptions& Options)
	{
		LowerPrivate::FFileLowering File(Model, Options, TriviaFile);
		return LowerPrivate::LowerMachine(File, Machine);
	}
}
