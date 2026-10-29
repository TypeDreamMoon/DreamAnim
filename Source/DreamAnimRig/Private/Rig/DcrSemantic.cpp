// Copyright DreamMoon. All Rights Reserved.
//
// The semantic pass for ControlRig documents: the sections of the document in the order their parts need
// each other, and the graphs they make. DcrSemantic.h describes what the text means; the sections that are
// data are in DcrSemanticHierarchy.cpp, the code in DcrSemanticCode.cpp and DcrSemanticExpr.cpp.

#include "Rig/DcrSemantic.h"

#include "DcrSemanticInternal.h"

namespace UE::DreamAnim::Rig
{
	using namespace UE::DreamAnim::Lang;
	using namespace UE::DreamAnim::Model;
	using namespace UE::DreamAnim::Rig::Sem;

	namespace Sem
	{
		FString MakeAssetPath(const FString& Name, const FString& Root)
		{
			FString Mount = TEXT("Game");
			if (!Root.IsEmpty())
			{
				if (Root == TEXT("Game") || Root == TEXT("Engine"))
				{
					Mount = Root;
				}
				else if (Root.StartsWith(TEXT("Plugin."), ESearchCase::CaseSensitive) && Root.Len() > 7)
				{
					Mount = Root.RightChop(7);
				}
				else
				{
					return FString();
				}
			}
			FString Relative = Name;
			Relative.RemoveFromStart(TEXT("/"));
			Relative.RemoveFromEnd(TEXT("/"));
			if (Relative.IsEmpty())
			{
				return FString();
			}
			int32 Slash = INDEX_NONE;
			Relative.FindLastChar(TEXT('/'), Slash);
			return FString::Printf(TEXT("/%s/%s.%s"), *Mount, *Relative, *Relative.Mid(Slash + 1));
		}

		const FDcrModelVariable* FGlobals::FindVariable(const FString& Name) const
		{
			return Model.Variables.FindByPredicate([&Name](const FDcrModelVariable& Variable) { return Variable.Name == Name; });
		}

		FFunctionDecl* FGlobals::FindFunction(const FString& Name) const
		{
			for (const TUniquePtr<FFunctionDecl>& Function : Functions)
			{
				if (Function->Name == Name)
				{
					return Function.Get();
				}
			}
			return nullptr;
		}

		FLibrary* FGlobals::FindLibrary(const FString& Name) const
		{
			for (const TUniquePtr<FLibrary>& Library : Libraries)
			{
				if (Library->Name == Name)
				{
					return Library.Get();
				}
			}
			return nullptr;
		}

		FString FGlobals::ResolveAssetPath(const FDabExpr& Expr, const UClass* Class, FString& OutProblem, bool& bOutAmbiguous) const
		{
			bOutAmbiguous = false;
			const FDabExpr* Inner = &StripDabParens(Expr);
			const FString PathObject = GetDcrPathExprObjectPath(*Inner);
			if (!PathObject.IsEmpty())
			{
				return PathObject;
			}
			const FDabNameExpr* Name = Inner->As<FDabNameExpr>();
			if (!Name)
			{
				OutProblem = TEXT("An asset is written by its name, or as Path(Root, \"Folder/Name\").");
				return FString();
			}
			const TArray<FString> Found = FindDcrAssetsByName(Name->Name, Class);
			if (Found.Num() == 1)
			{
				return Found[0];
			}
			if (Found.IsEmpty())
			{
				OutProblem = FString::Printf(TEXT("There is no asset called `%s`."), *Name->Name);
				return FString();
			}
			bOutAmbiguous = true;
			OutProblem = FString::Printf(TEXT("More than one asset is called `%s`: %s."), *Name->Name, *FString::Join(Found, TEXT(", ")));
			return FString();
		}
	}

	namespace
	{
		/** The graphs events and detached code are collected into, in the order they first appear. */
		class FGraphSet
		{
		public:
			explicit FGraphSet(FGlobals& InGlobals) : G(InGlobals) {}

			FBody& Get(const FString& Name)
			{
				const int32 Existing = Names.IndexOfByPredicate([&Name](const FString& Candidate) { return Candidate == Name; });
				if (Existing != INDEX_NONE)
				{
					return *Bodies[Existing];
				}
				Names.Add(Name);
				TUniquePtr<FDcrModelGraph> Graph = MakeUnique<FDcrModelGraph>();
				Graph->Name = Name;
				TUniquePtr<FBody> Body = MakeUnique<FBody>(G, *Graph, nullptr, Name);
				Graphs.Add(MoveTemp(Graph));
				Bodies.Add(MoveTemp(Body));
				return *Bodies.Last();
			}

			void Finish()
			{
				for (int32 Index = 0; Index < Bodies.Num(); ++Index)
				{
					Bodies[Index]->Finish();
					// The default graph left empty while code went into named ones: the first named one is the
					// asset's default graph, under its own name.
					if (Index == 0 && Graphs[Index]->Nodes.IsEmpty() && Bodies.Num() > 1)
					{
						continue;
					}
					G.Model.Graphs.Add(MoveTemp(*Graphs[Index]));
				}
			}

		private:
			FGlobals& G;
			TArray<FString> Names;
			TArray<TUniquePtr<FDcrModelGraph>> Graphs;
			TArray<TUniquePtr<FBody>> Bodies;
		};

		void CollectLocals(FGlobals& G, const FDabCodeBlock* Code, FFunctionDecl& Decl);

		void CollectLocalsIn(FGlobals& G, const FDabStmt& Stmt, FFunctionDecl& Decl)
		{
			if (const FDabLetStmt* Let = Stmt.As<FDabLetStmt>())
			{
				if (Let->LetKind != EDabLetKind::Local)
				{
					for (const FDabFlowBlock& Block : Let->Blocks)
					{
						CollectLocals(G, Block.Body.Get(), Decl);
					}
					return;
				}
				FDcrModelVariable Local;
				Local.Name = Let->Name;
				if (Decl.Locals.ContainsByPredicate([&Local](const FDcrModelVariable& Other) { return Other.Name == Local.Name; })
					|| Decl.Inputs.ContainsByPredicate([&Local](const FDcrModelFunctionPin& Input) { return Input.Name == Local.Name; }))
				{
					G.Sink.Error(TEXT("DAB6008"), Let->NameSpan, FString::Printf(TEXT("The name `%s` is used twice in the function `%s`."), *Let->Name, *Decl.Name));
					return;
				}
				if (!ResolveTypeName(G, Let->TypeName, Let->TypeSpan, Local.Type))
				{
					return;
				}
				if (Let->Value)
				{
					ReadConstantText(G, Local.Type, *Let->Value, Local.Default);
				}
				Decl.Locals.Add(MoveTemp(Local));
			}
			else if (const FDabIfStmt* If = Stmt.As<FDabIfStmt>())
			{
				CollectLocals(G, If->Then.Get(), Decl);
				CollectLocals(G, If->Else.Get(), Decl);
				if (If->ElseIf)
				{
					CollectLocalsIn(G, *If->ElseIf, Decl);
				}
			}
			else if (const FDabForStmt* For = Stmt.As<FDabForStmt>())
			{
				CollectLocals(G, For->Body.Get(), Decl);
			}
			else if (const FDabDetachedStmt* Detached = Stmt.As<FDabDetachedStmt>())
			{
				CollectLocals(G, Detached->Body.Get(), Decl);
			}
			else if (const FDabExprStmt* Expr = Stmt.As<FDabExprStmt>())
			{
				for (const FDabFlowBlock& Block : Expr->Blocks)
				{
					CollectLocals(G, Block.Body.Get(), Decl);
				}
			}
		}

		void CollectLocals(FGlobals& G, const FDabCodeBlock* Code, FFunctionDecl& Decl)
		{
			if (!Code)
			{
				return;
			}
			for (const FDabStmtPtr& Stmt : Code->Statements)
			{
				if (Stmt)
				{
					CollectLocalsIn(G, *Stmt, Decl);
				}
			}
		}

		/** The signature of a function: parameter and output types, defaults, locals, metadata. The body comes later. */
		void DeclareFunction(FGlobals& G, const FDabNamedBlockStmt& Stmt)
		{
			if (G.FindFunction(Stmt.Name))
			{
				G.Sink.Error(TEXT("DAB6008"), Stmt.NameSpan, FString::Printf(TEXT("The function `%s` is declared twice."), *Stmt.Name));
				return;
			}
			TUniquePtr<FFunctionDecl> Owned = MakeUnique<FFunctionDecl>();
			FFunctionDecl& Decl = *Owned;
			Decl.Stmt = &Stmt;
			Decl.Name = Stmt.Name;
			bool bPure = false;

			FString Category;
			FString Keywords;
			FString Description;
			if (Stmt.Meta)
			{
				for (const TUniquePtr<FDabMetaEntry>& Entry : Stmt.Meta->Entries)
				{
					if (!Entry)
					{
						continue;
					}
					if (Entry->Key == TEXT("Inline"))
					{
						Decl.bInline = true;
					}
					else if (Entry->Key == TEXT("Pure"))
					{
						bPure = true;
					}
					else if (Entry->Key == TEXT("Public"))
					{
						Decl.bPublic = true;
					}
					else if (Entry->Key == TEXT("Category"))
					{
						ReadStringMeta(G, *Entry, Category);
					}
					else if (Entry->Key == TEXT("Keywords"))
					{
						ReadStringMeta(G, *Entry, Keywords);
					}
					else if (Entry->Key == TEXT("Description"))
					{
						ReadStringMeta(G, *Entry, Description);
					}
					else
					{
						G.Sink.Error(TEXT("DAB9001"), Entry->KeySpan, FString::Printf(TEXT("A function has no metadata called `%s`. The metadata are Inline, Pure, Public, Category, Keywords and Description."), *Entry->Key));
					}
				}
			}
			// Code runs, unless it is marked pure: then it only names values for the outputs.
			Decl.bMutable = Stmt.Code.IsValid() && !bPure;
			if (bPure && !Stmt.Code)
			{
				G.Sink.Error(TEXT("DAB9013"), Stmt.NameSpan, FString::Printf(TEXT("`%s` is marked Pure and has no code: a function written = value; is pure already."), *Stmt.Name));
			}
			if (!Stmt.Code && !Stmt.Value)
			{
				G.Sink.Error(TEXT("DAB9004"), Stmt.NameSpan, FString::Printf(TEXT("The function `%s` has no body: write { code } or = value;."), *Stmt.Name));
				Decl.bValid = false;
			}

			for (const TUniquePtr<FDabParam>& Param : Stmt.Params)
			{
				if (!Param)
				{
					continue;
				}
				FDcrModelFunctionPin Pin;
				Pin.Name = Param->Name;
				if (!ResolveTypeName(G, Param->TypeName, Param->TypeSpan, Pin.Type))
				{
					Decl.bValid = false;
					continue;
				}
				if (Param->Default)
				{
					ReadConstantText(G, Pin.Type, *Param->Default, Pin.Default);
				}
				Decl.Inputs.Add(MoveTemp(Pin));
			}
			for (const TUniquePtr<FDabParam>& Param : Stmt.Outputs)
			{
				if (!Param)
				{
					continue;
				}
				FDcrModelFunctionPin Pin;
				Pin.Name = Param->Name;
				if (!ResolveTypeName(G, Param->TypeName, Param->TypeSpan, Pin.Type))
				{
					Decl.bValid = false;
					continue;
				}
				Decl.Outputs.Add(MoveTemp(Pin));
			}
			CollectLocals(G, Stmt.Code.Get(), Decl);

			if (!Decl.bInline && Decl.bValid)
			{
				FDcrModelFunction& Function = G.Model.Functions.AddDefaulted_GetRef();
				Function.Name = Decl.Name;
				Function.bMutable = Decl.bMutable;
				Function.bPublic = Decl.bPublic;
				Function.Category = Category;
				Function.Keywords = Keywords;
				Function.Description = Description;
				Function.Inputs = Decl.Inputs;
				Function.Outputs = Decl.Outputs;
				Function.Locals = Decl.Locals;
				Function.Graph.Name = Decl.Name;
				Decl.ModelIndex = G.Model.Functions.Num() - 1;
			}
			G.Functions.Add(MoveTemp(Owned));
		}

		/** The name of the graph code is written into: `[ Graph = "ConstructionGraph" ]`, else the default graph. */
		bool ReadGraphName(FGlobals& G, const FDabNamedBlockStmt& Stmt, FString& OutName, bool bReport = true)
		{
			OutName = TEXT("RigVMModel");
			if (!Stmt.Meta)
			{
				return true;
			}
			bool bOk = true;
			for (const TUniquePtr<FDabMetaEntry>& Entry : Stmt.Meta->Entries)
			{
				if (!Entry)
				{
					continue;
				}
				if (Entry->Key != TEXT("Graph"))
				{
					if (bReport)
					{
						G.Sink.Error(TEXT("DAB9001"), Entry->KeySpan, FString::Printf(TEXT("%s has no metadata called `%s`. It takes Graph."), *Stmt.Word, *Entry->Key));
					}
					bOk = false;
					continue;
				}
				const FDabLiteralExpr* Literal = Entry->Value ? Entry->Value->As<FDabLiteralExpr>() : nullptr;
				if (!Literal || Literal->LiteralKind != EDabLiteralKind::String || Literal->Text.IsEmpty())
				{
					if (bReport)
					{
						G.Sink.Error(TEXT("DAB9010"), Entry->Span, TEXT("`Graph` names the graph the code belongs to, as a string: [ Graph = \"ConstructionGraph\" ]."));
					}
					bOk = false;
					continue;
				}
				OutName = Literal->Text;
			}
			return bOk;
		}
	}

	bool BuildDcrModel(const FDabCompilationUnit& Unit, FDcrModel& OutModel, FDabDiagnosticSink& Sink, const FDcrSemanticOptions& Options)
	{
		OutModel = FDcrModel();
		const int32 ErrorsBefore = Sink.NumErrors();
		const FDabDocument* Document = Unit.Main ? Unit.Main->Document.Get() : nullptr;
		if (!Document || !Document->Body)
		{
			Sink.Error(TEXT("DAB2002"), FDabSpan(), TEXT("The file has no document."));
			return false;
		}
		if (Document->DocumentKind != EDabDocumentKind::ControlRig)
		{
			Sink.Error(TEXT("DAB2004"), Document->KindSpan, FString::Printf(TEXT("`%s` is not a ControlRig document."), *Document->KindName));
			return false;
		}

		FGlobals G(Unit, Sink, OutModel);
		OutModel.AssetPath = Options.AssetPath;
		if (OutModel.AssetPath.IsEmpty())
		{
			OutModel.AssetPath = MakeAssetPath(Document->GetHeaderString(TEXT("Name")), Document->GetHeaderString(TEXT("Root")));
			if (OutModel.AssetPath.IsEmpty())
			{
				Sink.Error(TEXT("DAB3006"), Document->KindSpan, TEXT("The header does not name an asset: Root is Game, Engine or Plugin.<Name>, and Name is not empty."),
					TEXT("Write ControlRig(Name=\"Folder/Asset\", Root=\"Game\")."));
			}
		}

		// The sections, once each.
		const FDabBlockExpr* SettingsBlock = nullptr;
		const FDabBlockExpr* ModuleBlock = nullptr;
		const FDabBlockExpr* HierarchyBlock = nullptr;
		const FDabBlockExpr* VariablesBlock = nullptr;
		TArray<const FDabNamedBlockStmt*> Functions;
		TArray<const FDabNamedBlockStmt*> Code;
		for (const FDabStmtPtr& Item : Document->Body->Statements)
		{
			if (!Item || Item->Is<FDabRegionStmt>())
			{
				continue;
			}
			if (const FDabAssignStmt* Assign = Item->As<FDabAssignStmt>())
			{
				const FDabBlockExpr** Slot = nullptr;
				if (Assign->Name == TEXT("Settings"))
				{
					Slot = &SettingsBlock;
				}
				else if (Assign->Name == TEXT("Module"))
				{
					Slot = &ModuleBlock;
				}
				else if (Assign->Name == TEXT("Hierarchy"))
				{
					Slot = &HierarchyBlock;
				}
				else if (Assign->Name == TEXT("Variables"))
				{
					Slot = &VariablesBlock;
				}
				if (!Slot)
				{
					Sink.Error(TEXT("DAB6011"), Assign->NameSpan, FString::Printf(TEXT("`%s` is not a section of a ControlRig. The sections are Settings, Module, Hierarchy, Variables, Event, Function and Detached."), *Assign->Name));
					continue;
				}
				const FDabBlockExpr* Block = Assign->Value ? Assign->Value->As<FDabBlockExpr>() : nullptr;
				if (!Block)
				{
					Sink.Error(TEXT("DAB6011"), Assign->NameSpan, FString::Printf(TEXT("`%s` is a block: %s = { ... }"), *Assign->Name, *Assign->Name));
					continue;
				}
				if (*Slot)
				{
					Sink.Error(TEXT("DAB6008"), Assign->NameSpan, FString::Printf(TEXT("The section `%s` is written twice."), *Assign->Name));
					continue;
				}
				*Slot = Block;
			}
			else if (const FDabNamedBlockStmt* Named = Item->As<FDabNamedBlockStmt>())
			{
				if (Named->Word == TEXT("Function"))
				{
					Functions.Add(Named);
				}
				else if (Named->Word == TEXT("Event") || Named->Word == TEXT("Detached"))
				{
					Code.Add(Named);
				}
				else
				{
					Sink.Error(TEXT("DAB6011"), Named->WordSpan, FString::Printf(TEXT("`%s` does not belong at the top of a ControlRig. Write Event, Function or Detached."), *Named->Word));
				}
			}
			else
			{
				Sink.Error(TEXT("DAB6011"), Item->Span, TEXT("A ControlRig holds sections (Settings = { ... }), Events, Functions and Detached code."));
			}
		}

		// Parts in the order they need each other.
		FString PreviewMeshPath;
		if (SettingsBlock)
		{
			ReadSettings(G, *SettingsBlock, PreviewMeshPath);
		}
		if (ModuleBlock)
		{
			ReadModuleSettings(G, *ModuleBlock);
		}
		if (VariablesBlock)
		{
			ReadVariablesBlock(G, *VariablesBlock);
		}
		for (const FDabNamedBlockStmt* Function : Functions)
		{
			DeclareFunction(G, *Function);
		}
		if (HierarchyBlock)
		{
			ReadHierarchy(G, *HierarchyBlock, PreviewMeshPath);
		}

		// The library's functions, now that every signature is known.
		for (const TUniquePtr<FFunctionDecl>& Decl : G.Functions)
		{
			if (Decl->bInline || !Decl->bValid || Decl->ModelIndex == INDEX_NONE)
			{
				continue;
			}
			FBody Body(G, OutModel.Functions[Decl->ModelIndex].Graph, Decl.Get(), TEXT("FunctionLibrary/") + Decl->Name);
			Body.BeginFunction();
			Body.RunFunction();
			Body.Finish();
		}

		// Events and detached code, into their graphs. The default graph is always there, and first.
		FGraphSet Graphs(G);
		Graphs.Get(TEXT("RigVMModel"));
		// Every graph's let names first: a node named after its kind in one event does not take a name a let of a later one gives.
		for (const FDabNamedBlockStmt* Block : Code)
		{
			FString GraphName;
			if (Block->Code && ReadGraphName(G, *Block, GraphName, false))
			{
				Graphs.Get(GraphName).ReserveLetNames(*Block->Code);
			}
		}
		for (const FDabNamedBlockStmt* Block : Code)
		{
			FString GraphName;
			const bool bGraphOk = ReadGraphName(G, *Block, GraphName);
			if (!bGraphOk && !Block->Code)
			{
				continue;
			}
			if (!Block->Code)
			{
				Sink.Error(TEXT("DAB9004"), Block->WordSpan, FString::Printf(TEXT("%s has no code: write { ... }."), *Block->Word));
				continue;
			}
			FBody& Body = Graphs.Get(GraphName);
			// The comment above an event or a Detached block: a box of its own in its graph.
			Body.AddCommentBoxes(*Block, Body.NumRecs());
			if (Block->Word == TEXT("Event"))
			{
				if (Body.BeginEvent(*Block, Block->Name))
				{
					Body.RunCode(*Block->Code);
				}
			}
			else
			{
				Body.BeginDetached();
				Body.RunCode(*Block->Code);
			}
		}
		Graphs.Finish();

		// An Inline function is one collapse node: called once. One never called is still checked.
		for (const TUniquePtr<FFunctionDecl>& Decl : G.Functions)
		{
			if (!Decl->bInline || Decl->Calls > 0)
			{
				continue;
			}
			Sink.Error(TEXT("DAB9008"), Decl->Stmt ? Decl->Stmt->NameSpan : FDabSpan(), FString::Printf(TEXT("The Inline function `%s` is never called."), *Decl->Name),
				TEXT("An Inline function is the body of one collapse node, made where it is called. Call it once, or drop [ Inline ]."));
			if (Decl->bValid)
			{
				FDcrModelGraph Scratch;
				FBody Body(G, Scratch, Decl.Get(), TEXT("Inline/") + Decl->Name);
				Body.BeginFunction();
				Body.RunFunction();
				Body.Finish();
			}
		}
		return Sink.NumErrors() == ErrorsBefore;
	}
}
