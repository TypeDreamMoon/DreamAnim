// Copyright DreamMoon. All Rights Reserved.

#include "Lower/DabAssetSlots.h"

#include "AnimGraphNode_Base.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "DreamAnimSet.h"
#include "Generate/DabGenerate.h"
#include "Lower/DabLower.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Compiler::LowerPrivate
{
	/** `Path(Plugin.ALS, "A/B")` -> `/ALS/A/B.B`; empty for anything else. In DabLower.cpp. */
	FString ObjectPathOf(const Lang::FDabExpr* Expr);
}

namespace UE::DreamAnim::Compiler::AssetSlotsPrivate
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;

	/** The animation asset a model value names, as `Path(...)` or as an object path string; null for anything else. */
	UAnimationAsset* FindAnimationAsset(const FDabExpr* Value)
	{
		FString ObjectPath = LowerPrivate::ObjectPathOf(Value);
		const FDabLiteralExpr* Literal = Value ? Value->As<FDabLiteralExpr>() : nullptr;
		if (ObjectPath.IsEmpty() && Literal && Literal->LiteralKind == EDabLiteralKind::String && Literal->Text.StartsWith(TEXT("/")))
		{
			ObjectPath = Literal->Text;
		}
		return ObjectPath.IsEmpty() ? nullptr : LoadObject<UAnimationAsset>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** A slot read fits where the slot's getter returns something the pin or parameter takes. */
	bool TakesSlotRead(const FProperty* Property, const UAnimationAsset& Asset)
	{
		const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property);
		return Object && Object->PropertyClass && GetDabAssetSlotReadClass(Asset.GetClass())->IsChildOf(Object->PropertyClass);
	}

	/** A field of a node's runtime struct the editor can show as a pin, and so bind. */
	bool IsBindableField(const FProperty& Field)
	{
		return !Field.HasMetaData(TEXT("NeverAsPin"))
			&& (Field.HasMetaData(TEXT("AlwaysAsPin")) || Field.HasMetaData(TEXT("PinShownByDefault")) || Field.HasMetaData(TEXT("PinHiddenByDefault")));
	}

	/** The class a call's target evaluates to, from Self; null when it cannot be told. */
	const UClass* ResolveTargetClass(const FDabExpr* Target, const UClass* SelfClass)
	{
		if (!Target)
		{
			return SelfClass;
		}
		if (const FDabNameExpr* Name = Target->As<FDabNameExpr>())
		{
			return Name->Name == TEXT("Self") ? SelfClass : nullptr;
		}
		if (const FDabMemberExpr* Member = Target->As<FDabMemberExpr>())
		{
			const UClass* Outer = ResolveTargetClass(Member->Object.Get(), SelfClass);
			const FObjectPropertyBase* Property = Outer ? FindFProperty<FObjectPropertyBase>(Outer, FName(*Member->Member)) : nullptr;
			return Property ? Property->PropertyClass : nullptr;
		}
		if (const FDabCallExpr* Call = Target->As<FDabCallExpr>())
		{
			const FDabMemberExpr* Callee = Call->Callee ? Call->Callee->As<FDabMemberExpr>() : nullptr;
			const UClass* Outer = Callee ? ResolveTargetClass(Callee->Object.Get(), SelfClass) : nullptr;
			const UFunction* Function = Outer ? Outer->FindFunctionByName(FName(*Callee->Member)) : nullptr;
			const FObjectPropertyBase* Return = Function ? CastField<FObjectPropertyBase>(Function->GetReturnProperty()) : nullptr;
			return Return ? Return->PropertyClass : nullptr;
		}
		return nullptr;
	}

	/** The parameter an argument goes to: by name, or the Index-th one when it has none. */
	const FProperty* FindParameter(const UFunction& Function, const FDabModelArgument& Argument, int32 Index)
	{
		int32 Position = 0;
		for (TFieldIterator<FProperty> It(&Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				continue;
			}
			if (Argument.Name.IsEmpty() ? Position == Index : It->GetName() == Argument.Name)
			{
				return *It;
			}
			++Position;
		}
		return nullptr;
	}

	FDabExprPtr MakeSlotRead(const FString& Slot)
	{
		TUniquePtr<FDabNameExpr> Root = MakeUnique<FDabNameExpr>();
		Root->Name = TEXT("Assets");
		TUniquePtr<FDabMemberExpr> Read = MakeUnique<FDabMemberExpr>();
		Read->Object = MoveTemp(Root);
		Read->Member = Slot;
		return Read;
	}

	struct FLifter
	{
		FDabModel& Model;
		const UClass* SelfClass = nullptr;
		/** Asset object path -> its slot. */
		TMap<FString, FString> SlotsByAsset;
		TSet<FString> TakenNames;
		TArray<FDabLiftedAsset> Lifted;

		explicit FLifter(FDabModel& InModel)
			: Model(InModel)
		{
			SelfClass = Model.ParentClass.IsEmpty() ? nullptr : LoadObject<UClass>(nullptr, *Model.ParentClass, nullptr, LOAD_NoWarn | LOAD_Quiet);
			for (const FDabModelAssetSlot& Slot : Model.AssetSlots)
			{
				TakenNames.Add(Slot.Name);
			}
		}

		FString SlotFor(const UAnimationAsset& Asset)
		{
			const FString AssetPath = Asset.GetPathName();
			if (const FString* Found = SlotsByAsset.Find(AssetPath))
			{
				return *Found;
			}
			const FString Base = MakeDabIdentifier(Asset.GetName());
			FString Name = Base;
			for (int32 Suffix = 2; TakenNames.Contains(Name); ++Suffix)
			{
				Name = FString::Printf(TEXT("%s_%d"), *Base, Suffix);
			}
			TakenNames.Add(Name);
			SlotsByAsset.Add(AssetPath, Name);
			Lifted.Add({ Name, Asset.GetClass()->GetName(), AssetPath });
			return Name;
		}

		void LiftNode(FDabModelNode& Node)
		{
			const UClass* NodeClass = FindFirstObject<UClass>(*Node.ClassName, EFindFirstObjectOptions::NativeFirst);
			const UAnimGraphNode_Base* Default = NodeClass ? Cast<UAnimGraphNode_Base>(NodeClass->GetDefaultObject()) : nullptr;
			const FStructProperty* RuntimeNode = Default ? Default->GetFNodeProperty() : nullptr;
			if (RuntimeNode)
			{
				for (int32 Index = Node.Properties.Num() - 1; Index >= 0; --Index)
				{
					FDabModelProperty& Property = Node.Properties[Index];
					const FProperty* Field = FindFProperty<FProperty>(RuntimeNode->Struct, FName(*Property.Name));
					const UAnimationAsset* Asset = Field && IsBindableField(*Field) ? FindAnimationAsset(Property.Value.Get()) : nullptr;
					if (!Asset || !TakesSlotRead(Field, *Asset))
					{
						continue;
					}
					FDabModelBinding Binding;
					Binding.PinName = Property.Name;
					Binding.Value = MakeSlotRead(SlotFor(*Asset));
					Node.Bindings.Add(MoveTemp(Binding));
					Node.Properties.RemoveAt(Index);
				}
			}
			for (FDabModelNodeFunction& Function : Node.NodeFunctions)
			{
				LiftCall(Function.Call);
			}
			if (Node.Call.IsSet())
			{
				LiftCall(Node.Call.GetValue());
			}
		}

		void LiftCall(FDabModelCall& Call)
		{
			const UClass* TargetClass = ResolveTargetClass(Call.Target.Get(), SelfClass);
			const UFunction* Function = TargetClass ? TargetClass->FindFunctionByName(FName(*Call.Function)) : nullptr;
			if (!Function)
			{
				return;
			}
			for (int32 Index = 0; Index < Call.Args.Num(); ++Index)
			{
				FDabModelArgument& Argument = Call.Args[Index];
				const UAnimationAsset* Asset = FindAnimationAsset(Argument.Value.Get());
				if (Asset && TakesSlotRead(FindParameter(*Function, Argument, Index), *Asset))
				{
					Argument.Value = MakeSlotRead(SlotFor(*Asset));
				}
			}
		}

		void LiftCall(TOptional<FDabModelCall>& Call)
		{
			if (Call.IsSet())
			{
				LiftCall(Call.GetValue());
			}
		}

		void LiftGraph(FDabModelGraph& Graph)
		{
			for (FDabModelNode& Node : Graph.Nodes)
			{
				LiftNode(Node);
			}
		}

		void Run()
		{
			LiftGraph(Model.AnimGraph);
			for (FDabModelLayer& Layer : Model.Layers)
			{
				LiftGraph(Layer.Graph);
			}
			for (FDabModelStateMachine& Machine : Model.StateMachines)
			{
				for (FDabModelState& State : Machine.States)
				{
					LiftCall(State.OnEnter);
					LiftCall(State.OnExit);
					LiftCall(State.OnBlended);
					LiftGraph(State.Graph);
				}
			}
			for (FDabModelNotify& Notify : Model.Notifies)
			{
				for (FDabModelCall& Call : Notify.Calls)
				{
					LiftCall(Call);
				}
			}
		}
	};
}

namespace UE::DreamAnim::Compiler
{
	using namespace UE::DreamAnim::Model;

	TArray<FDabLiftedAsset> LiftDabAnimationAssets(FDabModel& Model, const FString& AnimSetPath)
	{
		if (Model.Kind != Lang::EDabDocumentKind::AnimBlueprint)
		{
			return {};
		}
		AssetSlotsPrivate::FLifter Lifter(Model);
		Lifter.Run();
		for (const FDabLiftedAsset& Entry : Lifter.Lifted)
		{
			Model.AssetSlots.Add({ Entry.Slot, Entry.Type });
		}
		Model.AssetSlots.Sort([](const FDabModelAssetSlot& A, const FDabModelAssetSlot& B) { return A.Name < B.Name; });
		if (!Lifter.Lifted.IsEmpty() && !AnimSetPath.IsEmpty())
		{
			Model.DefaultAnimSet = AnimSetPath;
		}
		Lifter.Lifted.Sort([](const FDabLiftedAsset& A, const FDabLiftedAsset& B) { return A.Slot < B.Slot; });
		return MoveTemp(Lifter.Lifted);
	}

	bool FillDabAnimSet(UDreamAnimSet& Set, FName Blueprint, const FDabModel& Model, const TArray<FDabLiftedAsset>& Lifted, TArray<FString>* OutKept)
	{
		TArray<TPair<FName, FName>> Slots;
		for (const FDabModelAssetSlot& Slot : Model.AssetSlots)
		{
			Slots.Emplace(FName(*Slot.Name), FName(*Slot.Type));
		}
		bool bChanged = Set.SyncSlots(Blueprint, Slots);
		if (!Set.Skeleton && !Model.Skeleton.IsEmpty())
		{
			if (USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *Model.Skeleton, nullptr, LOAD_NoWarn | LOAD_Quiet))
			{
				Set.Skeleton = Skeleton;
				bChanged = true;
			}
		}
		for (const FDabLiftedAsset& Entry : Lifted)
		{
			UAnimationAsset* Asset = LoadObject<UAnimationAsset>(nullptr, *Entry.AssetPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			const FName Slot(*Entry.Slot);
			const UAnimationAsset* Current = Set.Find(Blueprint, Slot);
			if (!Asset || Current == Asset)
			{
				continue;
			}
			if (Current)
			{
				if (OutKept)
				{
					OutKept->Add(Entry.Slot);
				}
				continue;
			}
			bChanged |= Set.SetAsset(Blueprint, Slot, Asset);
		}
		return bChanged;
	}
}
