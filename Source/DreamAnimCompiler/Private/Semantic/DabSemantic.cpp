// Copyright DreamMoon. All Rights Reserved.
//
// BuildDabModel and the document level: Docs/Mapping.md, "Document". The sections are read in the
// order their names are needed -- aliases, tuning values and asset slots before anything reads them, the
// parent class before any path is typed against it, every cache and machine name before any graph.

#include "Semantic/DabSemantic.h"

#include "Semantic/DabSemanticInternal.h"

#include "AnimGraphNode_Base.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimLayerInterface.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "DreamAnimSet.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace UE::DreamAnim::Compiler::SemanticPrivate
{
	FSemantic::FSemantic(const FDabCompilationUnit& InUnit, FDabModel& InModel, FDabDiagnosticSink& InSink, const FDabSemanticOptions& InOptions)
		: Unit(InUnit)
		, Model(InModel)
		, Sink(InSink)
		, Options(InOptions)
	{
	}

	bool FSemantic::Run()
	{
		Model = FDabModel();
		const FDabFile* File = Unit.Main.Get();
		const FDabDocument* Document = File ? File->Document.Get() : nullptr;
		if (!Document || !Document->Body)
		{
			// The parser has said why (DAB2002); without a document there is nothing to build.
			if (!Sink.HasErrors())
			{
				Sink.Error(TEXT("DAB6014"), FDabSpan(), TEXT("There is no document to build."));
			}
			return false;
		}

		Kind = Document->DocumentKind;
		if (Kind == EDabDocumentKind::AnimTemplates)
		{
			Sink.Error(TEXT("DAB6014"), Document->KindSpan, TEXT("A template library is not built on its own; #include it from an AnimBlueprint or AnimLayerInterface."));
			return false;
		}
		if (Kind == EDabDocumentKind::Unknown)
		{
			return false;
		}
		Model.Kind = Kind;
		Model.AnimGraph.Name = TEXT("AnimGraph");
		ReadHeader(*Document);

		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(UAnimGraphNode_Base::StaticClass()) && !It->HasAnyClassFlags(CLASS_NewerVersionExists))
			{
				NodeClasses.Add(*It);
			}
		}

		// Sections by name; layers and machines in written order.
		const FDabAssignStmt* SettingsStmt = nullptr;
		const FDabAssignStmt* DefaultsStmt = nullptr;
		const FDabAssignStmt* AliasesStmt = nullptr;
		const FDabAssignStmt* PropertiesStmt = nullptr;
		const FDabAssignStmt* AssetsStmt = nullptr;
		const FDabAssignStmt* NotifiesStmt = nullptr;
		const FDabAssignStmt* GraphStmt = nullptr;
		TArray<const FDabNamedBlockStmt*> LayerStmts;
		TArray<const FDabNamedBlockStmt*> MachineList;
		const bool bInterface = Kind == EDabDocumentKind::AnimLayerInterface;

		for (const FDabStmtPtr& Statement : Document->Body->Statements)
		{
			if (!Statement || Statement->Is<FDabRegionStmt>())
			{
				continue;
			}
			if (const FDabNamedBlockStmt* Named = Statement->As<FDabNamedBlockStmt>())
			{
				if (Named->Word == TEXT("Layer"))
				{
					if (LayerStmts.ContainsByPredicate([Named](const FDabNamedBlockStmt* Other) { return Other->Name == Named->Name; }))
					{
						Sink.Error(TEXT("DAB6008"), Named->NameSpan, FString::Printf(TEXT("There are two layers named `%s`."), *Named->Name));
						continue;
					}
					LayerStmts.Add(Named);
				}
				else if (Named->Word == TEXT("StateMachine"))
				{
					if (bInterface)
					{
						Sink.Error(TEXT("DAB6015"), Named->WordSpan, TEXT("An AnimLayerInterface only declares layers; it has no state machines."));
						continue;
					}
					if (MachineStmts.Contains(Named->Name))
					{
						Sink.Error(TEXT("DAB6008"), Named->NameSpan, FString::Printf(TEXT("There are two state machines named `%s`."), *Named->Name));
						continue;
					}
					MachineStmts.Add(Named->Name, Named);
					MachineOrder.Add(Named->Name);
					MachineList.Add(Named);
				}
				else if (Named->Word != TEXT("Template"))
				{
					Sink.Error(TEXT("DAB6011"), Named->WordSpan, FString::Printf(TEXT("`%s` cannot be written at the document level."), *Named->Word),
						TEXT("A document holds Settings, Defaults, Aliases, Properties, Assets, Notifies, Graph, Layer, StateMachine and Template."));
				}
				continue;
			}

			const FDabAssignStmt* Assign = Statement->As<FDabAssignStmt>();
			const FDabAssignStmt** Slot = !Assign ? nullptr
				: Assign->Name == TEXT("Settings") ? &SettingsStmt
				: Assign->Name == TEXT("Defaults") ? &DefaultsStmt
				: Assign->Name == TEXT("Aliases") ? &AliasesStmt
				: Assign->Name == TEXT("Properties") ? &PropertiesStmt
				: Assign->Name == TEXT("Assets") ? &AssetsStmt
				: Assign->Name == TEXT("Notifies") ? &NotifiesStmt
				: Assign->Name == TEXT("Graph") ? &GraphStmt
				: nullptr;
			if (!Slot)
			{
				Sink.Error(TEXT("DAB6011"), Assign ? Assign->NameSpan : Statement->Span,
					Assign ? FString::Printf(TEXT("`%s` is not a section of a document."), *Assign->Name) : FString(TEXT("This statement cannot be written at the document level.")),
					TEXT("A document holds Settings, Defaults, Aliases, Properties, Assets, Notifies, Graph, Layer, StateMachine and Template."));
				continue;
			}
			if (*Slot)
			{
				Sink.Error(TEXT("DAB6008"), Assign->NameSpan, FString::Printf(TEXT("The section `%s` is written twice."), *Assign->Name));
				continue;
			}
			if (!Assign->Value || !SkipParens(*Assign->Value).Is<FDabBlockExpr>())
			{
				Sink.Error(TEXT("DAB6011"), Assign->NameSpan, FString::Printf(TEXT("The section `%s` is a block: %s = { ... }."), *Assign->Name, *Assign->Name));
				continue;
			}
			if (bInterface && (Slot == &GraphStmt || Slot == &NotifiesStmt || Slot == &AssetsStmt))
			{
				Sink.Error(TEXT("DAB6015"), Assign->NameSpan, FString::Printf(TEXT("An AnimLayerInterface only declares layers; it has no `%s`."), *Assign->Name));
				continue;
			}
			*Slot = Assign;
		}

		const auto BlockOf = [](const FDabAssignStmt* Section) -> const FDabBlockExpr*
		{
			return Section ? SkipParens(*Section->Value).As<FDabBlockExpr>() : nullptr;
		};

		if (const FDabBlockExpr* Block = BlockOf(AliasesStmt))
		{
			ReadAliases(*Block);
		}
		if (const FDabBlockExpr* Block = BlockOf(PropertiesStmt))
		{
			ReadProperties(*Block);
		}
		if (const FDabBlockExpr* Block = BlockOf(AssetsStmt))
		{
			ReadAssets(*Block);
		}
		if (const FDabBlockExpr* Block = BlockOf(SettingsStmt))
		{
			ReadSettings(*Block);
		}
		if (const FDabBlockExpr* Block = BlockOf(DefaultsStmt))
		{
			ReadClassDefaults(*Block);
		}
		if (const FDabBlockExpr* Block = BlockOf(NotifiesStmt))
		{
			ReadNotifies(*Block);
		}

		// Every name a graph can see, before any graph is built: caches are shared by the whole Blueprint.
		static const TSet<FString> NoReserved;
		static const TSet<FString> StateReserved = {TEXT("OnEnter"), TEXT("OnExit"), TEXT("OnBlended"), TEXT("Pose")};
		static const TSet<FString> GraphReserved = {TEXT("Output"), TEXT("Inputs")};
		if (const FDabBlockExpr* Block = BlockOf(GraphStmt))
		{
			CollectGraphNames(Block->Statements, TEXT("Graph"), GraphReserved);
		}
		for (const FDabNamedBlockStmt* Layer : LayerStmts)
		{
			if (Layer->Body)
			{
				CollectGraphNames(Layer->Body->Statements, FString::Printf(TEXT("Layer %s"), *Layer->Name), GraphReserved);
			}
			CollectLayerInputs(*Layer);
		}
		for (const FDabNamedBlockStmt* Machine : MachineList)
		{
			if (!Machine->Body)
			{
				continue;
			}
			for (const FDabStmtPtr& Statement : Machine->Body->Statements)
			{
				const FDabNamedBlockStmt* State = Statement ? Statement->As<FDabNamedBlockStmt>() : nullptr;
				if (State && State->Word == TEXT("State") && State->Body)
				{
					CollectGraphNames(State->Body->Statements, FString::Printf(TEXT("state %s of StateMachine %s"), *State->Name, *Machine->Name), StateReserved);
				}
			}
		}

		if (const FDabBlockExpr* Block = BlockOf(GraphStmt))
		{
			BuildGraph(Block->Statements, Model.AnimGraph, TEXT("Graph"), TEXT("Output"), NoReserved, GraphStmt->NameSpan);
			// The AnimGraph's result node has no settings worth writing: its group only matters for layers.
			if (GraphStmt->Meta)
			{
				Sink.Error(TEXT("DAB4005"), GraphStmt->Meta->Span, TEXT("Graph takes no metadata."),
					TEXT("LayerGroup and BlendOptions are written on a layer: `Layer Name [ LayerGroup = \"Group\" ] { ... }`."));
			}
		}
		for (const FDabNamedBlockStmt* Layer : LayerStmts)
		{
			ReadLayer(*Layer);
		}
		for (const FDabNamedBlockStmt* Machine : MachineList)
		{
			BuildStateMachine(*Machine);
		}
		CheckMachineUses();

		return !Sink.HasErrors();
	}

	// ------------------------------------------------------------------------------------------
	// Header and sections
	// ------------------------------------------------------------------------------------------

	void FSemantic::ReadHeader(const FDabDocument& Document)
	{
		const FString Name = Document.GetHeaderString(TEXT("Name"));
		const FString Root = Document.GetHeaderString(TEXT("Root"));
		FDabSpan RootSpan = Document.KindSpan;
		for (const FDabArgumentPtr& Argument : Document.HeaderArgs)
		{
			if (Argument && Argument->Name == TEXT("Root") && Argument->Value)
			{
				RootSpan = Argument->Value->Span;
			}
		}
		if (Name.IsEmpty())
		{
			// The parser reports a header without a name (DAB2005).
			return;
		}

		FString Mount = TEXT("Game");
		if (!Root.IsEmpty())
		{
			const FDabExprPtr RootExpr = MakeStringExpr(Root);
			if (!ReadMountRoot(*RootExpr, Mount))
			{
				Sink.Error(TEXT("DAB3006"), RootSpan, FString::Printf(TEXT("`%s` is not a root; a root is \"Game\", \"Engine\" or \"Plugin.<Name>\"."), *Root));
				Mount = TEXT("Game");
			}
		}
		FString Relative = Name;
		Relative.RemoveFromStart(TEXT("/"));
		Relative.RemoveFromEnd(TEXT("/"));
		int32 Slash = INDEX_NONE;
		Relative.FindLastChar(TEXT('/'), Slash);
		BlueprintName = Relative.Mid(Slash + 1);
		Model.AssetPath = FString::Printf(TEXT("/%s/%s.%s"), *Mount, *Relative, *BlueprintName);
	}

	void FSemantic::ReadAliases(const FDabBlockExpr& Block)
	{
		for (const FDabStmtPtr& Statement : Block.Statements)
		{
			if (!Statement || Statement->Is<FDabRegionStmt>())
			{
				continue;
			}
			const FDabAssignStmt* Assign = Statement->As<FDabAssignStmt>();
			if (!Assign || !Assign->Value)
			{
				Sink.Error(TEXT("DAB6011"), Statement->Span, TEXT("Each alias is `Name = Self.Path;`."));
				continue;
			}
			const FString& Name = Assign->Name;
			if (Name == TEXT("Self") || Name == TEXT("Props") || Name == TEXT("Assets") || Name == TEXT("None"))
			{
				Sink.Error(TEXT("DAB5007"), Assign->NameSpan, FString::Printf(TEXT("`%s` is a word of the language; an alias cannot take it."), *Name));
				continue;
			}
			if (Aliases.Contains(Name))
			{
				Sink.Error(TEXT("DAB6008"), Assign->NameSpan, FString::Printf(TEXT("The alias `%s` is declared twice."), *Name));
				continue;
			}
			FDabExprPtr Value = Resolve(*Assign->Value);
			if (!Value)
			{
				continue;
			}
			const FDabNameExpr* Root = GetPathRoot(*Value);
			if (!Root || Root->Name != TEXT("Self"))
			{
				Sink.Error(TEXT("DAB5007"), Assign->Value->Span, FString::Printf(TEXT("An alias stands for a path from Self; `%s` is not one."), *PrintValueText(*Assign->Value)),
					TEXT("Write `Main = Self.GetParent();`."));
				continue;
			}
			Aliases.Add(Name, MoveTemp(Value));
		}
	}

	void FSemantic::ReadProperties(const FDabBlockExpr& Block)
	{
		for (const FDabStmtPtr& Statement : Block.Statements)
		{
			if (!Statement || Statement->Is<FDabRegionStmt>())
			{
				continue;
			}
			const FDabDeclStmt* Decl = Statement->As<FDabDeclStmt>();
			if (!Decl)
			{
				Sink.Error(TEXT("DAB6011"), Statement->Span, TEXT("Each tuning value is `Type Name = Default [ meta ];`."));
				continue;
			}
			if (VariableIndex.Contains(Decl->Name))
			{
				Sink.Error(TEXT("DAB6008"), Decl->NameSpan, FString::Printf(TEXT("The tuning value `%s` is declared twice."), *Decl->Name));
				continue;
			}

			FDabModelVariable Variable;
			Variable.Name = Decl->Name;
			Variable.Type = Decl->TypeName;
			const FSemType Type = FSemType::FromSpelling(Decl->TypeName);
			if (Type.Kind == ESemTypeKind::Unknown)
			{
				Sink.Warning(TEXT("DAB3008"), Decl->TypeSpan, FString::Printf(TEXT("The type `%s` is not known here; its default is kept as written."), *Decl->TypeName),
					TEXT("Types are spelled float, int, int64, bool, FName, FString, FText, a struct, enum or class name without its prefix, or TArray<T>, TSet<T>, TMap<K, V>."));
			}
			if (Decl->Value)
			{
				const FDabExpr& Value = SkipParens(*Decl->Value);
				if (!IsConstant(Value) && !IsPlaceholderCall(Value) && !IsSectionRead(Value, TEXT("Props")))
				{
					Sink.Error(TEXT("DAB4010"), Value.Span, FString::Printf(TEXT("The default of `%s` is a constant; `%s` is read at run time."), *Decl->Name, *PrintValueText(Value)));
				}
				else
				{
					Variable.Default = ConvertTyped(Type, Value, nullptr, FString::Printf(TEXT("The default of `%s`"), *Decl->Name));
				}
			}
			if (Decl->Meta)
			{
				for (const TUniquePtr<FDabMetaEntry>& Entry : Decl->Meta->Entries)
				{
					if (!Entry)
					{
						continue;
					}
					FDabModelProperty Meta;
					Meta.Name = Entry->Key;
					Meta.Value = Entry->Value ? ConvertUntyped(*Entry->Value) : nullptr;
					Variable.Meta.Add(MoveTemp(Meta));
				}
			}
			VariableIndex.Add(Variable.Name, Model.Variables.Num());
			Model.Variables.Add(MoveTemp(Variable));
		}
	}

	void FSemantic::ReadAssets(const FDabBlockExpr& Block)
	{
		for (const FDabStmtPtr& Statement : Block.Statements)
		{
			if (!Statement || Statement->Is<FDabRegionStmt>())
			{
				continue;
			}
			const FDabDeclStmt* Decl = Statement->As<FDabDeclStmt>();
			if (!Decl)
			{
				Sink.Error(TEXT("DAB6011"), Statement->Span, TEXT("Each asset slot is `Type Name;`, such as `AnimSequence Idle;`."));
				continue;
			}
			if (SlotNames.Contains(Decl->Name))
			{
				Sink.Error(TEXT("DAB6008"), Decl->NameSpan, FString::Printf(TEXT("The asset slot `%s` is declared twice."), *Decl->Name));
				continue;
			}
			if (Decl->Value)
			{
				Sink.Error(TEXT("DAB6011"), Decl->Value->Span, FString::Printf(TEXT("The asset slot `%s` takes no value; its asset is chosen in the animation set."), *Decl->Name));
			}
			if (Decl->Meta)
			{
				Sink.Error(TEXT("DAB4005"), Decl->Meta->Span, FString::Printf(TEXT("The asset slot `%s` takes no metadata."), *Decl->Name));
			}
			const UClass* SlotClass = FindNativeClass(Decl->TypeName);
			if (!SlotClass || !SlotClass->IsChildOf(UAnimationAsset::StaticClass()))
			{
				Sink.Error(TEXT("DAB3005"), Decl->TypeSpan, FString::Printf(TEXT("`%s` is not an animation asset type."), *Decl->TypeName),
					TEXT("A slot holds an animation asset: AnimSequence, BlendSpace, AnimMontage, ..."));
			}
			FDabModelAssetSlot Slot;
			Slot.Name = Decl->Name;
			Slot.Type = Decl->TypeName;
			Model.AssetSlots.Add(MoveTemp(Slot));
			SlotNames.Add(Decl->Name);
		}
	}

	FString FSemantic::ReadAssetPathSetting(const FDabExpr& InValue, const UClass* ExpectedClass, const FString& What)
	{
		const FDabExpr& Value = SkipParens(InValue);
		if (IsCallTo(Value, TEXT("Path")))
		{
			FString ObjectPath;
			FString Error;
			if (!TryGetPathCallObjectPath(static_cast<const FDabCallExpr&>(Value), ObjectPath, Error))
			{
				Sink.Error(TEXT("DAB3006"), Value.Span, Error);
				return FString();
			}
			return NormalizeAssetObjectPath(ObjectPath);
		}
		if (const FDabLiteralExpr* Literal = Value.As<FDabLiteralExpr>(); Literal && Literal->LiteralKind == EDabLiteralKind::String
			&& Literal->Text.StartsWith(TEXT("/"), ESearchCase::CaseSensitive))
		{
			return NormalizeAssetObjectPath(Literal->Text);
		}
		if (const FDabNameExpr* Name = Value.As<FDabNameExpr>())
		{
			if (Name->Name == TEXT("None"))
			{
				return FString();
			}
			FString ObjectPath;
			const EAssetLookup Lookup = FindAssetByName(Name->Name, ExpectedClass, nullptr, ObjectPath);
			if (Lookup == EAssetLookup::Found)
			{
				return NormalizeAssetObjectPath(ObjectPath);
			}
			if (Lookup == EAssetLookup::Ambiguous)
			{
				Sink.Error(TEXT("DAB3007"), Value.Span, FString::Printf(TEXT("%s: more than one asset is named `%s`."), *What, *Name->Name),
					TEXT("Write Path(Root, \"Folder/Name\") to say which one."));
			}
			else
			{
				Sink.Error(TEXT("DAB3004"), Value.Span, FString::Printf(TEXT("%s: there is no %s named `%s`."), *What,
					ExpectedClass ? *ExpectedClass->GetName() : TEXT("asset"), *Name->Name),
					TEXT("Check the spelling, or write Path(Root, \"Folder/Name\")."));
			}
			return FString();
		}
		Sink.Error(TEXT("DAB4006"), Value.Span, FString::Printf(TEXT("%s is an asset: Path(Root, \"Folder/Name\") or its name, not `%s`."), *What, *PrintValueText(Value)));
		return FString();
	}

	void FSemantic::ReadParent(const FDabExpr& InValue)
	{
		const FDabExpr& Value = SkipParens(InValue);
		const bool bInterface = Kind == EDabDocumentKind::AnimLayerInterface;
		const UClass* Required = bInterface ? UAnimLayerInterface::StaticClass() : UAnimInstance::StaticClass();

		UClass* ParentClass = nullptr;
		FString BlueprintPath;
		if (const FDabNameExpr* Name = Value.As<FDabNameExpr>())
		{
			ParentClass = FindNativeClass(Name->Name);
			if (!ParentClass)
			{
				const EAssetLookup Lookup = FindAssetByName(Name->Name, nullptr, Required, BlueprintPath);
				if (Lookup == EAssetLookup::Ambiguous)
				{
					Sink.Error(TEXT("DAB3007"), Value.Span, FString::Printf(TEXT("More than one Blueprint is named `%s`."), *Name->Name),
						TEXT("Write the parent Blueprint as Path(Root, \"Folder/Name\")."));
					return;
				}
				if (Lookup == EAssetLookup::NotFound)
				{
					Sink.Error(TEXT("DAB3004"), Value.Span, FString::Printf(TEXT("There is no class or Blueprint named `%s`."), *Name->Name),
						TEXT("Name a C++ class (AlsAnimationInstance) or give a Blueprint's Path(...)."));
					return;
				}
			}
		}
		else if (const FDabLiteralExpr* Literal = Value.As<FDabLiteralExpr>(); Literal && Literal->LiteralKind == EDabLiteralKind::String)
		{
			ParentClass = FindNativeClass(Literal->Text);
			if (!ParentClass)
			{
				BlueprintPath = Literal->Text;
			}
		}
		else if (IsCallTo(Value, TEXT("Path")))
		{
			FString Error;
			if (!TryGetPathCallObjectPath(static_cast<const FDabCallExpr&>(Value), BlueprintPath, Error))
			{
				Sink.Error(TEXT("DAB3006"), Value.Span, Error);
				return;
			}
		}
		else
		{
			Sink.Error(TEXT("DAB4006"), Value.Span, FString::Printf(TEXT("Parent is a class: a C++ class name or a Blueprint's Path(...), not `%s`."), *PrintValueText(Value)));
			return;
		}

		if (ParentClass)
		{
			Model.ParentClass = ParentClass->GetPathName();
		}
		else
		{
			// A Blueprint parent is its generated class, `/Game/X/Y.Y_C`.
			BlueprintPath = NormalizeAssetObjectPath(BlueprintPath);
			Model.ParentClass = BlueprintPath + TEXT("_C");
			const FDabExprPtr Reference = MakeStringExpr(BlueprintPath);
			ParentClass = LoadReferencedClass(*Reference);
			if (!ParentClass)
			{
				Sink.Error(TEXT("DAB3004"), Value.Span, FString::Printf(TEXT("The parent Blueprint %s cannot be loaded."), *BlueprintPath));
				return;
			}
		}

		if (!ParentClass->IsChildOf(Required))
		{
			Sink.Error(TEXT("DAB3003"), Value.Span, FString::Printf(TEXT("The parent class %s does not derive from %s."), *ParentClass->GetName(), *Required->GetName()),
				bInterface ? TEXT("An AnimLayerInterface's parent is AnimLayerInterface.") : TEXT("An Animation Blueprint's parent is AnimInstance or a class derived from it."));
		}
		SelfClass = ParentClass;
	}

	void FSemantic::ReadSettings(const FDabBlockExpr& Block)
	{
		TSet<FString> Seen;
		const UClass* BlueprintClass = UAnimBlueprint::StaticClass();
		const UObject* BlueprintDefaults = GetDefault<UAnimBlueprint>();
		for (const FDabStmtPtr& Statement : Block.Statements)
		{
			if (!Statement || Statement->Is<FDabRegionStmt>())
			{
				continue;
			}
			const FDabAssignStmt* Assign = Statement->As<FDabAssignStmt>();
			if (!Assign || !Assign->Value)
			{
				Sink.Error(TEXT("DAB6011"), Statement->Span, TEXT("Each setting is `Name = Value;`."));
				continue;
			}
			const FString& Key = Assign->Name;
			if (Seen.Contains(Key))
			{
				Sink.Error(TEXT("DAB6008"), Assign->NameSpan, FString::Printf(TEXT("The setting `%s` is given twice."), *Key));
				continue;
			}
			Seen.Add(Key);
			if (Assign->Meta)
			{
				Sink.Error(TEXT("DAB4005"), Assign->Meta->Span, FString::Printf(TEXT("The setting `%s` takes no metadata."), *Key));
			}

			if (Key == TEXT("Parent"))
			{
				ReadParent(*Assign->Value);
			}
			else if (Key == TEXT("Skeleton"))
			{
				Model.Skeleton = ReadAssetPathSetting(*Assign->Value, USkeleton::StaticClass(), TEXT("Skeleton"));
			}
			else if (Key == TEXT("DefaultAnimSet"))
			{
				Model.DefaultAnimSet = ReadAssetPathSetting(*Assign->Value, UDreamAnimSet::StaticClass(), TEXT("DefaultAnimSet"));
			}
			else if (Key == TEXT("Implements"))
			{
				const FDabExpr& Value = SkipParens(*Assign->Value);
				TArray<const FDabExpr*> Items;
				if (const FDabListExpr* List = Value.As<FDabListExpr>())
				{
					for (const FDabExprPtr& Item : List->Items)
					{
						Items.Add(Item.Get());
					}
				}
				else
				{
					Items.Add(&Value);
				}
				for (const FDabExpr* Item : Items)
				{
					const FString Interface = Item ? ReadAssetPathSetting(*Item, UAnimBlueprint::StaticClass(), TEXT("Implements")) : FString();
					if (!Interface.IsEmpty())
					{
						Model.Interfaces.AddUnique(Interface);
					}
				}
			}
			else
			{
				// The Class Settings panel's own settings, as the extractor reads them.
				const FProperty* Property = FindFProperty<FProperty>(BlueprintClass, FName(*Key));
				const bool bSetting = Property && Property->GetOwnerClass() == BlueprintClass
					&& (Property->HasAnyPropertyFlags(CPF_Edit) || Property->GetFName() == FName(TEXT("bIsTemplate")))
					&& Property->GetFName() != FName(TEXT("TargetSkeleton"))
					&& !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated);
				if (!bSetting)
				{
					Sink.Error(TEXT("DAB4001"), Assign->NameSpan, FString::Printf(TEXT("`%s` is not a setting of an Animation Blueprint."), *Key),
						TEXT("Settings are Parent, Skeleton, Implements, DefaultAnimSet and the Class Settings of UAnimBlueprint (bUseMultiThreadedAnimationUpdate, ...)."));
					continue;
				}
				ReadMetaValue(*Property, Assign->Value.Get(), Assign->NameSpan, Property->ContainerPtrToValuePtr<void>(BlueprintDefaults), Model.ClassSettings,
					FString::Printf(TEXT("The setting %s"), *Key));
			}
		}
	}

	void FSemantic::ReadClassDefaults(const FDabBlockExpr& Block)
	{
		const UObject* ParentDefaults = SelfClass ? SelfClass->GetDefaultObject() : nullptr;
		for (const FDabStmtPtr& Statement : Block.Statements)
		{
			if (!Statement || Statement->Is<FDabRegionStmt>())
			{
				continue;
			}
			const FDabAssignStmt* Assign = Statement->As<FDabAssignStmt>();
			if (!Assign || !Assign->Value)
			{
				Sink.Error(TEXT("DAB4014"), Statement->Span, TEXT("Each class default is `Property = Value;`."));
				continue;
			}
			// What the Class Defaults panel edits, as the extractor reads it: editable properties of the parent class.
			const FProperty* Property = SelfClass ? FindFProperty<FProperty>(SelfClass, FName(*Assign->Name)) : nullptr;
			const bool bEditable = Property && Property->HasAnyPropertyFlags(CPF_Edit)
				&& !Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated | CPF_DisableEditOnTemplate)
				&& Property->GetOwnerClass() != UObject::StaticClass();
			if (!bEditable || !ParentDefaults)
			{
				Sink.Error(TEXT("DAB4014"), Assign->NameSpan,
					FString::Printf(TEXT("`%s` is not an editable property of the parent class %s."), *Assign->Name, SelfClass ? *SelfClass->GetName() : TEXT("(unknown)")));
				continue;
			}
			ReadMetaValue(*Property, Assign->Value.Get(), Assign->NameSpan, Property->ContainerPtrToValuePtr<void>(ParentDefaults), Model.ClassDefaults,
				FString::Printf(TEXT("The class default %s"), *Assign->Name));
		}
	}

	void FSemantic::ReadNotifies(const FDabBlockExpr& Block)
	{
		TSet<FString> Seen;
		for (const FDabStmtPtr& Statement : Block.Statements)
		{
			if (!Statement || Statement->Is<FDabRegionStmt>())
			{
				continue;
			}
			const FDabAssignStmt* Assign = Statement->As<FDabAssignStmt>();
			if (!Assign || !Assign->Value)
			{
				Sink.Error(TEXT("DAB6011"), Statement->Span, TEXT("Each notify is `Name = Self.Function();` or `Name = { Call; Call; };`."));
				continue;
			}
			FString Name = Assign->Name;
			Name.RemoveFromStart(TEXT("AnimNotify_"), ESearchCase::CaseSensitive);
			if (Seen.Contains(Name))
			{
				Sink.Error(TEXT("DAB6008"), Assign->NameSpan, FString::Printf(TEXT("The notify `%s` is handled twice."), *Name));
				continue;
			}
			Seen.Add(Name);

			FDabModelNotify Notify;
			Notify.Name = Name;
			const FDabExpr& Value = SkipParens(*Assign->Value);
			TArray<const FDabExpr*> Calls;
			if (const FDabStackExpr* Stack = Value.As<FDabStackExpr>())
			{
				for (const FDabExprPtr& Step : Stack->Steps)
				{
					Calls.Add(Step.Get());
				}
			}
			else if (!Value.Is<FDabBlockExpr>() || !static_cast<const FDabBlockExpr&>(Value).Statements.IsEmpty())
			{
				Calls.Add(&Value);
			}
			for (const FDabExpr* CallExpr : Calls)
			{
				FDabModelCall Call;
				if (CallExpr && ConvertCall(*CallExpr, Call, FString::Printf(TEXT("The notify %s"), *Name)))
				{
					Notify.Calls.Add(MoveTemp(Call));
				}
			}
			Model.Notifies.Add(MoveTemp(Notify));
		}
	}

	void FSemantic::ReadLayer(const FDabNamedBlockStmt& Stmt)
	{
		FDabModelLayer Layer;
		Layer.Name = Stmt.Name;
		Layer.Graph.Name = Stmt.Name;
		const FString Location = FString::Printf(TEXT("Layer %s"), *Stmt.Name);
		ReadResultMeta(Stmt.Meta.Get(), Layer.Graph, Location);

		const bool bInterface = Kind == EDabDocumentKind::AnimLayerInterface;
		if ((Stmt.Body || Stmt.Value) && bInterface)
		{
			Sink.Error(TEXT("DAB6015"), Stmt.NameSpan, TEXT("An AnimLayerInterface only declares its layers: `Layer Name [ meta ];`."),
				TEXT("Write the layer's poses in the Animation Blueprints that implement the interface."));
			Layer.bDeclarationOnly = true;
		}
		else if (Stmt.Body)
		{
			static const TSet<FString> NoReserved;
			BuildGraph(Stmt.Body->Statements, Layer.Graph, Location, TEXT("Output"), NoReserved, Stmt.NameSpan);
		}
		else if (Stmt.Value)
		{
			BuildValueGraph(*Stmt.Value, Layer.Graph, Location);
		}
		else
		{
			// `Layer X [ meta ];`: declared, no graph.
			Layer.bDeclarationOnly = true;
		}
		Model.Layers.Add(MoveTemp(Layer));
	}
}

namespace UE::DreamAnim::Compiler
{
	bool BuildDabModel(const Lang::FDabCompilationUnit& Unit, Model::FDabModel& OutModel, Lang::FDabDiagnosticSink& Sink, const FDabSemanticOptions& Options)
	{
		SemanticPrivate::FSemantic Semantic(Unit, OutModel, Sink, Options);
		return Semantic.Run();
	}
}
