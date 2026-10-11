// Copyright DreamMoon. All Rights Reserved.
//
// The document's sections: class settings, class defaults, tuning values, asset slots, notifies and the
// layers' settings. A section is edited entry by entry where the entries are (untouched ones keep their
// text, comments and alignment); the Blueprint's parent, skeleton and interfaces are only ever changed in
// the text.

#include "WriteBack/DabWriteBackInternal.h"

namespace UE::DreamAnim::Compiler::WriteBackPrivate
{
	FDabStmtPtr MakeAssignStmt(const FString& Name, FDabExprPtr Value)
	{
		TUniquePtr<FDabAssignStmt> Assign = MakeUnique<FDabAssignStmt>();
		Assign->Name = Name;
		Assign->bHasSemicolon = !Value || !Value->Is<FDabBlockExpr>();
		Assign->Value = MoveTemp(Value);
		return Assign;
	}

	FString StatementName(const FDabStmt& Statement)
	{
		if (const FDabAssignStmt* Assign = Statement.As<FDabAssignStmt>())
		{
			return Assign->Name;
		}
		if (const FDabDeclStmt* Decl = Statement.As<FDabDeclStmt>())
		{
			return Decl->Name;
		}
		return FString();
	}

	/** Where an assignment's or declaration's value is held. */
	FDabExprPtr* GetStatementValue(FDabStmt& Statement)
	{
		if (FDabAssignStmt* Assign = Statement.As<FDabAssignStmt>())
		{
			return &Assign->Value;
		}
		if (FDabDeclStmt* Decl = Statement.As<FDabDeclStmt>())
		{
			return &Decl->Value;
		}
		return nullptr;
	}

	/** The same statement apart from its value: name, type and metadata alike. */
	bool SameButValue(const FDabStmt& A, const FDabStmt& B)
	{
		if (A.Kind != B.Kind)
		{
			return false;
		}
		const FDabStmtPtr CopyA = CloneStmt(A);
		const FDabStmtPtr CopyB = CloneStmt(B);
		FDabExprPtr* SlotA = GetStatementValue(*CopyA);
		FDabExprPtr* SlotB = GetStatementValue(*CopyB);
		if (!SlotA || !SlotB)
		{
			return false;
		}
		*SlotA = nullptr;
		*SlotB = nullptr;
		return PrintDabStmt(*CopyA).Equals(PrintDabStmt(*CopyB), ESearchCase::CaseSensitive);
	}

	void FWriteBack::RewriteSection(const FString& Section, const TArray<FSectionEntry>& Before, const TArray<FSectionEntry>& After,
		const TSet<FString>& Unmanaged)
	{
		TMap<FString, const FSectionEntry*> Old;
		for (const FSectionEntry& Entry : Before)
		{
			Old.Add(Entry.Name, &Entry);
		}
		TArray<FString> Changed;
		TSet<FString> Wanted;
		for (const FSectionEntry& Entry : After)
		{
			Wanted.Add(Entry.Name);
			const FSectionEntry* const* Found = Old.Find(Entry.Name);
			if (!Found || !(*Found)->Printed.Equals(Entry.Printed, ESearchCase::CaseSensitive))
			{
				Changed.Add(Entry.Name);
			}
		}
		for (const FSectionEntry& Entry : Before)
		{
			if (!Wanted.Contains(Entry.Name))
			{
				Changed.Add(Entry.Name);
			}
		}
		if (Changed.IsEmpty())
		{
			return;
		}

		const FDabAssignStmt* const* Compiled2 = Provenance.Sections.Find(Section);
		FDabNode* Node = Compiled2 ? Text->FindSame(**Compiled2) : nullptr;
		FDabAssignStmt* Fresh = Node ? Node->As<FDabAssignStmt>() : nullptr;
		FDabBlockExpr* Block = Fresh && Fresh->Value ? Fresh->Value->As<FDabBlockExpr>() : nullptr;
		if (!Block)
		{
			// No such section yet: a new one at the end of the document.
			FDabBlockExpr* DocumentBody = Text->GetFile().Document->Body.Get();
			TUniquePtr<FDabBlockExpr> NewBlock = MakeUnique<FDabBlockExpr>();
			for (const FSectionEntry& Entry : After)
			{
				NewBlock->Statements.Add(Entry.Make());
			}
			FDabStmtPtr Assign = MakeAssignStmt(Section, MoveTemp(NewBlock));
			const FDabStmt& Added = *Assign;
			DocumentBody->Statements.Add(MoveTemp(Assign));
			Text->AppendStmt(*DocumentBody, Added);
			Change(Section, FString::Join(Changed, TEXT(", ")), true);
			return;
		}

		// Edited where the entries are, so the rest of the section keeps its text, order and alignment: a value
		// that changed is replaced, an entry that changed otherwise is reprinted, new ones go at the end and
		// entries that are gone are removed.
		const auto FindStatement = [Block](const FString& Name) -> int32
		{
			return Block->Statements.IndexOfByPredicate([&Name](const FDabStmtPtr& Statement)
			{
				return Statement && StatementName(*Statement) == Name;
			});
		};
		for (const FSectionEntry& Entry : After)
		{
			if (!Changed.Contains(Entry.Name))
			{
				continue;
			}
			FDabStmtPtr Made = Entry.Make();
			const int32 Position = FindStatement(Entry.Name);
			if (Position == INDEX_NONE)
			{
				const FDabStmt& Added = *Made;
				Block->Statements.Add(MoveTemp(Made));
				Text->AppendStmt(*Block, Added);
				continue;
			}
			FDabStmtPtr& Existing = Block->Statements[Position];
			FDabExprPtr* OldValue = GetStatementValue(*Existing);
			FDabExprPtr* NewValue = GetStatementValue(*Made);
			if (OldValue && NewValue && *OldValue && *NewValue && !(*OldValue)->Span.IsEmpty() && SameButValue(*Existing, *Made))
			{
				const FDabSpan OldSpan = (*OldValue)->Span;
				GraveyardExprs.Add(MoveTemp(*OldValue));
				*OldValue = MoveTemp(*NewValue);
				Graveyard.Add(MoveTemp(Made));
				Text->ReprintExpr(OldSpan, **OldValue);
				continue;
			}
			const FDabSpan OldSpan = Existing->Span;
			Graveyard.Add(MoveTemp(Existing));
			Existing = MoveTemp(Made);
			Text->ReprintStmt(OldSpan, *Existing);
		}
		for (const FSectionEntry& Entry : Before)
		{
			const int32 Position = Wanted.Contains(Entry.Name) || Unmanaged.Contains(Entry.Name) ? INDEX_NONE : FindStatement(Entry.Name);
			if (Position != INDEX_NONE)
			{
				Text->RemoveStmt(*Block->Statements[Position]);
				Graveyard.Add(MoveTemp(Block->Statements[Position]));
				Block->Statements.RemoveAt(Position);
			}
		}
		Change(Section, FString::Join(Changed, TEXT(", ")), true);
	}

	void FWriteBack::CompareDocument()
	{
		const FDabModel& Model = Compiled.Model;

		// What only the text changes: refuse, so the text stays the source of truth for the Blueprint's identity.
		TArray<FString> Interfaces = Model.Interfaces;
		TArray<FString> EditedInterfaces = Edited.Interfaces;
		Interfaces.Sort();
		EditedInterfaces.Sort();
		if (Model.ParentClass != Edited.ParentClass || Model.Skeleton != Edited.Skeleton || Interfaces != EditedInterfaces
			|| Model.DefaultAnimSet != Edited.DefaultAnimSet)
		{
			Change(TEXT("Settings"), TEXT("the parent class, skeleton, interfaces or default animation set"), false);
			Refuse(TEXT("DAB8008"), FDabSpan(), TEXT("The parent class, skeleton, interfaces or default animation set changed in the editor; these are only changed in the text."),
				TEXT("Change them in Settings and rebuild."));
		}

		const auto PropertyEntries = [this](const FDabModel& Source, const TArray<FDabModelProperty>& Properties)
		{
			TArray<FSectionEntry> Entries;
			for (const FDabModelProperty& Property : Properties)
			{
				FSectionEntry& Entry = Entries.AddDefaulted_GetRef();
				Entry.Name = Property.Name;
				const FDabExprPtr Value = Property.Value ? LowerDabValue(Source, *Property.Value, LowerOptions) : nullptr;
				Entry.Printed = Print(Value.Get());
				const FDabModel* SourcePtr = &Source;
				const FDabModelProperty* PropertyPtr = &Property;
				Entry.Make = [this, SourcePtr, PropertyPtr]()
				{
					return MakeAssignStmt(PropertyPtr->Name, PropertyPtr->Value ? LowerDabValue(*SourcePtr, *PropertyPtr->Value, LowerOptions) : nullptr);
				};
			}
			return Entries;
		};
		const TSet<FString> SettingsKept = { TEXT("Parent"), TEXT("Skeleton"), TEXT("Implements"), TEXT("DefaultAnimSet") };
		RewriteSection(TEXT("Settings"), PropertyEntries(Model, Model.ClassSettings), PropertyEntries(Edited, Edited.ClassSettings), SettingsKept);
		RewriteSection(TEXT("Defaults"), PropertyEntries(Model, Model.ClassDefaults), PropertyEntries(Edited, Edited.ClassDefaults), TSet<FString>());

		// Tuning values: type, default and metadata.
		const auto VariableEntries = [this](const FDabModel& Source)
		{
			TArray<FSectionEntry> Entries;
			for (const FDabModelVariable& Variable : Source.Variables)
			{
				FSectionEntry& Entry = Entries.AddDefaulted_GetRef();
				Entry.Name = Variable.Name;
				const FDabExprPtr Default = Variable.Default ? LowerDabValue(Source, *Variable.Default, LowerOptions) : nullptr;
				Entry.Printed = Variable.Type + TEXT("|") + Print(Default.Get()) + TEXT("|") + PrintMeta(Variable.Meta);
				const FDabModel* SourcePtr = &Source;
				const FDabModelVariable* VariablePtr = &Variable;
				Entry.Make = [this, SourcePtr, VariablePtr]()
				{
					TUniquePtr<FDabDeclStmt> Decl = MakeUnique<FDabDeclStmt>();
					Decl->TypeName = VariablePtr->Type;
					Decl->Name = VariablePtr->Name;
					Decl->Value = VariablePtr->Default ? LowerDabValue(*SourcePtr, *VariablePtr->Default, LowerOptions) : nullptr;
					Decl->Meta = LowerDabMeta(*SourcePtr, VariablePtr->Meta, LowerOptions);
					return FDabStmtPtr(MoveTemp(Decl));
				};
			}
			return Entries;
		};
		RewriteSection(TEXT("Properties"), VariableEntries(Model), VariableEntries(Edited), TSet<FString>());

		const auto SlotEntries = [](const FDabModel& Source)
		{
			TArray<FSectionEntry> Entries;
			for (const FDabModelAssetSlot& Slot : Source.AssetSlots)
			{
				FSectionEntry& Entry = Entries.AddDefaulted_GetRef();
				Entry.Name = Slot.Name;
				Entry.Printed = Slot.Type;
				const FString Type = Slot.Type;
				const FString Name = Slot.Name;
				Entry.Make = [Type, Name]()
				{
					TUniquePtr<FDabDeclStmt> Decl = MakeUnique<FDabDeclStmt>();
					Decl->TypeName = Type;
					Decl->Name = Name;
					return FDabStmtPtr(MoveTemp(Decl));
				};
			}
			return Entries;
		};
		RewriteSection(TEXT("Assets"), SlotEntries(Model), SlotEntries(Edited), TSet<FString>());

		const auto NotifyEntries = [this](const FDabModel& Source)
		{
			TArray<FSectionEntry> Entries;
			for (const FDabModelNotify& Notify : Source.Notifies)
			{
				FSectionEntry& Entry = Entries.AddDefaulted_GetRef();
				Entry.Name = Notify.Name;
				TArray<FString> Calls;
				for (const FDabModelCall& Call : Notify.Calls)
				{
					const FDabExprPtr Lowered = LowerDabCall(Source, Call, LowerOptions);
					Calls.Add(Print(Lowered.Get()));
				}
				Entry.Printed = FString::Join(Calls, TEXT(";"));
				const FDabModel* SourcePtr = &Source;
				const FDabModelNotify* NotifyPtr = &Notify;
				Entry.Make = [this, SourcePtr, NotifyPtr]()
				{
					if (NotifyPtr->Calls.Num() == 1)
					{
						return MakeAssignStmt(NotifyPtr->Name, LowerDabCall(*SourcePtr, NotifyPtr->Calls[0], LowerOptions));
					}
					TUniquePtr<FDabStackExpr> Stack = MakeUnique<FDabStackExpr>();
					for (const FDabModelCall& Call : NotifyPtr->Calls)
					{
						Stack->Steps.Add(LowerDabCall(*SourcePtr, Call, LowerOptions));
					}
					return MakeAssignStmt(NotifyPtr->Name, MoveTemp(Stack));
				};
			}
			return Entries;
		};
		RewriteSection(TEXT("Notifies"), NotifyEntries(Model), NotifyEntries(Edited), TSet<FString>());

		// Layers: their settings sit on their statements.
		for (const FDabModelLayer& Layer : Model.Layers)
		{
			const FDabModelLayer* EditedLayer = Edited.Layers.FindByPredicate([&Layer](const FDabModelLayer& Candidate) { return Candidate.Name == Layer.Name; });
			if (!EditedLayer)
			{
				Change(TEXT("Layer ") + Layer.Name, TEXT("the layer"), false);
				Refuse(TEXT("DAB8008"), FDabSpan(), FString::Printf(TEXT("Layer %s is gone from the Blueprint; layers come from the interfaces, which are only changed in the text."), *Layer.Name));
				continue;
			}
			if (PrintMeta(Layer.Graph.ResultProperties).Equals(PrintMeta(EditedLayer->Graph.ResultProperties), ESearchCase::CaseSensitive))
			{
				continue;
			}
			const FDabNamedBlockStmt* const* Compiled2 = Provenance.Layers.Find(Layer.Name);
			FDabNode* Node = Compiled2 ? Text->FindSame(**Compiled2) : nullptr;
			if (FDabNamedBlockStmt* Fresh = Node ? Node->As<FDabNamedBlockStmt>() : nullptr)
			{
				Fresh->Meta = LowerDabMeta(Edited, EditedLayer->Graph.ResultProperties, LowerOptions);
				Text->ReprintStmt(Fresh->Span, *Fresh);
				Change(TEXT("Layer ") + Layer.Name, TEXT("the layer's settings"), true);
			}
		}
	}
}
