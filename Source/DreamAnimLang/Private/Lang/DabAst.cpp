// Copyright DreamMoon. All Rights Reserved.
//
// The tree's out-of-line pieces: names for its enums, the lookups on metadata and the document
// header, deep copies, child traversal and path reading. ForEachChild visits children in source
// order; the tree dump and the later stages both rely on that order.

#include "Lang/DabAst.h"

#include "Misc/CString.h"

namespace UE::DreamAnim::Lang
{
	// Kept apart from the other front-end files' helpers: the module may be built as one unity blob,
	// and names like CloneBlock are what any of them would pick.
	namespace DabAstPrivate
	{
		static FDabExprPtr CloneOptionalExpr(const FDabExprPtr& Expr)
		{
			if (!Expr)
			{
				return FDabExprPtr();
			}
			return CloneExpr(*Expr);
		}

		static TUniquePtr<FDabMeta> CloneOptionalMeta(const TUniquePtr<FDabMeta>& Meta)
		{
			if (!Meta)
			{
				return TUniquePtr<FDabMeta>();
			}
			return CloneMeta(*Meta);
		}

		static FDabArgumentPtr CloneOptionalArgument(const FDabArgumentPtr& Argument)
		{
			if (!Argument)
			{
				return FDabArgumentPtr();
			}
			return CloneArgument(*Argument);
		}

		static TUniquePtr<FDabParam> CloneParam(const FDabParam& Param)
		{
			TUniquePtr<FDabParam> Result = MakeUnique<FDabParam>();
			Result->Span = Param.Span;
			Result->TypeName = Param.TypeName;
			Result->TypeSpan = Param.TypeSpan;
			Result->Name = Param.Name;
			Result->NameSpan = Param.NameSpan;
			Result->Default = DabAstPrivate::CloneOptionalExpr(Param.Default);
			return Result;
		}

		static TArray<TUniquePtr<FDabParam>> CloneParams(const TArray<TUniquePtr<FDabParam>>& Params)
		{
			TArray<TUniquePtr<FDabParam>> Result;
			Result.Reserve(Params.Num());
			for (const TUniquePtr<FDabParam>& Param : Params)
			{
				Result.Add(Param ? CloneParam(*Param) : TUniquePtr<FDabParam>());
			}
			return Result;
		}

		static TUniquePtr<FDabBlockExpr> CloneBlock(const FDabBlockExpr& Block)
		{
			TUniquePtr<FDabBlockExpr> Result = MakeUnique<FDabBlockExpr>();
			Result->Span = Block.Span;
			Result->Statements.Reserve(Block.Statements.Num());
			for (const FDabStmtPtr& Statement : Block.Statements)
			{
				Result->Statements.Add(Statement ? CloneStmt(*Statement) : FDabStmtPtr());
			}
			return Result;
		}

		static TUniquePtr<FDabCodeBlock> CloneCodeBlock(const FDabCodeBlock& Block)
		{
			TUniquePtr<FDabCodeBlock> Result = MakeUnique<FDabCodeBlock>();
			Result->Span = Block.Span;
			Result->Statements.Reserve(Block.Statements.Num());
			for (const FDabStmtPtr& Statement : Block.Statements)
			{
				Result->Statements.Add(Statement ? CloneStmt(*Statement) : FDabStmtPtr());
			}
			return Result;
		}

		static TUniquePtr<FDabCodeBlock> CloneOptionalCodeBlock(const TUniquePtr<FDabCodeBlock>& Block)
		{
			return Block ? CloneCodeBlock(*Block) : TUniquePtr<FDabCodeBlock>();
		}

		static TArray<FDabFlowBlock> CloneFlowBlocks(const TArray<FDabFlowBlock>& Blocks)
		{
			TArray<FDabFlowBlock> Result;
			for (const FDabFlowBlock& Block : Blocks)
			{
				FDabFlowBlock& Copy = Result.AddDefaulted_GetRef();
				Copy.Pin = Block.Pin;
				Copy.PinSpan = Block.PinSpan;
				Copy.Body = CloneOptionalCodeBlock(Block.Body);
			}
			return Result;
		}

		static TUniquePtr<FDabIfStmt> CloneIf(const FDabIfStmt& If)
		{
			TUniquePtr<FDabIfStmt> Result = MakeUnique<FDabIfStmt>();
			Result->Span = If.Span;
			Result->Condition = CloneOptionalExpr(If.Condition);
			Result->Then = CloneOptionalCodeBlock(If.Then);
			Result->ElseIf = If.ElseIf ? CloneIf(*If.ElseIf) : TUniquePtr<FDabIfStmt>();
			Result->Else = CloneOptionalCodeBlock(If.Else);
			Result->ElseSpan = If.ElseSpan;
			return Result;
		}

		static bool AppendPathSegments(const FDabExpr& Expr, TArray<FString>& OutSegments)
		{
			if (const FDabNameExpr* Name = Expr.As<FDabNameExpr>())
			{
				OutSegments.Add(Name->Name);
				return true;
			}
			if (const FDabMemberExpr* Member = Expr.As<FDabMemberExpr>())
			{
				if (!Member->Object || !AppendPathSegments(*Member->Object, OutSegments))
				{
					return false;
				}
				OutSegments.Add(Member->Member);
				return true;
			}
			return false;
		}
	}

	// ------------------------------------------------------------------------------------------
	// Names
	// ------------------------------------------------------------------------------------------

	const TCHAR* LexToString(EDabNodeKind Kind)
	{
		switch (Kind)
		{
		case EDabNodeKind::LiteralExpr:     return TEXT("LiteralExpr");
		case EDabNodeKind::NameExpr:        return TEXT("NameExpr");
		case EDabNodeKind::MemberExpr:      return TEXT("MemberExpr");
		case EDabNodeKind::CallExpr:        return TEXT("CallExpr");
		case EDabNodeKind::ListExpr:        return TEXT("ListExpr");
		case EDabNodeKind::TupleExpr:       return TEXT("TupleExpr");
		case EDabNodeKind::BlockExpr:       return TEXT("BlockExpr");
		case EDabNodeKind::StackExpr:       return TEXT("StackExpr");
		case EDabNodeKind::UnaryExpr:       return TEXT("UnaryExpr");
		case EDabNodeKind::BinaryExpr:      return TEXT("BinaryExpr");
		case EDabNodeKind::ConditionalExpr: return TEXT("ConditionalExpr");
		case EDabNodeKind::ParenExpr:       return TEXT("ParenExpr");
		case EDabNodeKind::IndexExpr:       return TEXT("IndexExpr");
		case EDabNodeKind::AssignStmt:      return TEXT("AssignStmt");
		case EDabNodeKind::DeclStmt:        return TEXT("DeclStmt");
		case EDabNodeKind::TransitionStmt:  return TEXT("TransitionStmt");
		case EDabNodeKind::NamedBlockStmt:  return TEXT("NamedBlockStmt");
		case EDabNodeKind::RegionStmt:      return TEXT("RegionStmt");
		case EDabNodeKind::LetStmt:         return TEXT("LetStmt");
		case EDabNodeKind::SetStmt:         return TEXT("SetStmt");
		case EDabNodeKind::ExprStmt:        return TEXT("ExprStmt");
		case EDabNodeKind::IfStmt:          return TEXT("IfStmt");
		case EDabNodeKind::ForStmt:         return TEXT("ForStmt");
		case EDabNodeKind::ReturnStmt:      return TEXT("ReturnStmt");
		case EDabNodeKind::DetachedStmt:    return TEXT("DetachedStmt");
		case EDabNodeKind::Argument:        return TEXT("Argument");
		case EDabNodeKind::MetaEntry:       return TEXT("MetaEntry");
		case EDabNodeKind::Meta:            return TEXT("Meta");
		case EDabNodeKind::Param:           return TEXT("Param");
		case EDabNodeKind::CodeBlock:       return TEXT("CodeBlock");
		case EDabNodeKind::Include:         return TEXT("Include");
		case EDabNodeKind::Document:        return TEXT("Document");
		}
		return TEXT("Unknown");
	}

	const TCHAR* LexToString(EDabDocumentKind Kind)
	{
		switch (Kind)
		{
		case EDabDocumentKind::AnimBlueprint:      return TEXT("AnimBlueprint");
		case EDabDocumentKind::AnimLayerInterface: return TEXT("AnimLayerInterface");
		case EDabDocumentKind::AnimTemplates:      return TEXT("AnimTemplates");
		case EDabDocumentKind::ControlRig:         return TEXT("ControlRig");
		case EDabDocumentKind::Unknown:            return TEXT("Unknown");
		}
		return TEXT("Unknown");
	}

	const TCHAR* GetDabUnaryOpSpelling(EDabUnaryOp Op)
	{
		switch (Op)
		{
		case EDabUnaryOp::Not:    return TEXT("!");
		case EDabUnaryOp::Negate: return TEXT("-");
		}
		return TEXT("");
	}

	const TCHAR* GetDabBinaryOpSpelling(EDabBinaryOp Op)
	{
		switch (Op)
		{
		case EDabBinaryOp::Or:           return TEXT("||");
		case EDabBinaryOp::And:          return TEXT("&&");
		case EDabBinaryOp::Equal:        return TEXT("==");
		case EDabBinaryOp::NotEqual:     return TEXT("!=");
		case EDabBinaryOp::Less:         return TEXT("<");
		case EDabBinaryOp::LessEqual:    return TEXT("<=");
		case EDabBinaryOp::Greater:      return TEXT(">");
		case EDabBinaryOp::GreaterEqual: return TEXT(">=");
		case EDabBinaryOp::Add:          return TEXT("+");
		case EDabBinaryOp::Subtract:     return TEXT("-");
		case EDabBinaryOp::Multiply:     return TEXT("*");
		case EDabBinaryOp::Divide:       return TEXT("/");
		case EDabBinaryOp::Modulo:       return TEXT("%");
		}
		return TEXT("");
	}

	int32 GetDabBinaryOpPrecedence(EDabBinaryOp Op)
	{
		switch (Op)
		{
		case EDabBinaryOp::Or:           return 1;
		case EDabBinaryOp::And:          return 2;
		case EDabBinaryOp::Equal:
		case EDabBinaryOp::NotEqual:     return 3;
		case EDabBinaryOp::Less:
		case EDabBinaryOp::LessEqual:
		case EDabBinaryOp::Greater:
		case EDabBinaryOp::GreaterEqual: return 4;
		case EDabBinaryOp::Add:
		case EDabBinaryOp::Subtract:     return 5;
		case EDabBinaryOp::Multiply:
		case EDabBinaryOp::Divide:
		case EDabBinaryOp::Modulo:       return 6;
		}
		return 0;
	}

	// ------------------------------------------------------------------------------------------
	// Lookups
	// ------------------------------------------------------------------------------------------

	const FDabMetaEntry* FDabMeta::Find(const TCHAR* Key) const
	{
		if (!Key)
		{
			return nullptr;
		}
		for (const TUniquePtr<FDabMetaEntry>& Entry : Entries)
		{
			if (Entry && FCString::Strcmp(*Entry->Key, Key) == 0)
			{
				return Entry.Get();
			}
		}
		return nullptr;
	}

	FString FDabDocument::GetHeaderString(const TCHAR* ArgName) const
	{
		if (!ArgName)
		{
			return FString();
		}
		for (const FDabArgumentPtr& Argument : HeaderArgs)
		{
			if (!Argument || !Argument->Value || FCString::Strcmp(*Argument->Name, ArgName) != 0)
			{
				continue;
			}
			const FDabLiteralExpr* Literal = Argument->Value->As<FDabLiteralExpr>();
			if (Literal && Literal->LiteralKind == EDabLiteralKind::String)
			{
				return Literal->Text;
			}
		}
		return FString();
	}

	// ------------------------------------------------------------------------------------------
	// Deep copies
	// ------------------------------------------------------------------------------------------

	FDabExprPtr CloneExpr(const FDabExpr& Expr)
	{
		switch (Expr.Kind)
		{
		case EDabNodeKind::LiteralExpr:
		{
			const FDabLiteralExpr& Source = static_cast<const FDabLiteralExpr&>(Expr);
			TUniquePtr<FDabLiteralExpr> Result = MakeUnique<FDabLiteralExpr>();
			Result->Span = Source.Span;
			Result->LiteralKind = Source.LiteralKind;
			Result->Text = Source.Text;
			Result->Number = Source.Number;
			Result->bBool = Source.bBool;
			return Result;
		}
		case EDabNodeKind::NameExpr:
		{
			const FDabNameExpr& Source = static_cast<const FDabNameExpr&>(Expr);
			TUniquePtr<FDabNameExpr> Result = MakeUnique<FDabNameExpr>();
			Result->Span = Source.Span;
			Result->Name = Source.Name;
			return Result;
		}
		case EDabNodeKind::MemberExpr:
		{
			const FDabMemberExpr& Source = static_cast<const FDabMemberExpr&>(Expr);
			TUniquePtr<FDabMemberExpr> Result = MakeUnique<FDabMemberExpr>();
			Result->Span = Source.Span;
			Result->Object = DabAstPrivate::CloneOptionalExpr(Source.Object);
			Result->Member = Source.Member;
			Result->MemberSpan = Source.MemberSpan;
			return Result;
		}
		case EDabNodeKind::CallExpr:
		{
			const FDabCallExpr& Source = static_cast<const FDabCallExpr&>(Expr);
			TUniquePtr<FDabCallExpr> Result = MakeUnique<FDabCallExpr>();
			Result->Span = Source.Span;
			Result->Callee = DabAstPrivate::CloneOptionalExpr(Source.Callee);
			Result->Args.Reserve(Source.Args.Num());
			for (const FDabArgumentPtr& Argument : Source.Args)
			{
				Result->Args.Add(DabAstPrivate::CloneOptionalArgument(Argument));
			}
			Result->ArgsSpan = Source.ArgsSpan;
			return Result;
		}
		case EDabNodeKind::ListExpr:
		{
			const FDabListExpr& Source = static_cast<const FDabListExpr&>(Expr);
			TUniquePtr<FDabListExpr> Result = MakeUnique<FDabListExpr>();
			Result->Span = Source.Span;
			Result->Items.Reserve(Source.Items.Num());
			for (const FDabExprPtr& Item : Source.Items)
			{
				Result->Items.Add(DabAstPrivate::CloneOptionalExpr(Item));
			}
			Result->ItemNames = Source.ItemNames;
			Result->ItemNameSpans = Source.ItemNameSpans;
			return Result;
		}
		case EDabNodeKind::TupleExpr:
		{
			const FDabTupleExpr& Source = static_cast<const FDabTupleExpr&>(Expr);
			TUniquePtr<FDabTupleExpr> Result = MakeUnique<FDabTupleExpr>();
			Result->Span = Source.Span;
			Result->Items.Reserve(Source.Items.Num());
			for (const FDabArgumentPtr& Item : Source.Items)
			{
				Result->Items.Add(DabAstPrivate::CloneOptionalArgument(Item));
			}
			return Result;
		}
		case EDabNodeKind::BlockExpr:
		{
			return DabAstPrivate::CloneBlock(static_cast<const FDabBlockExpr&>(Expr));
		}
		case EDabNodeKind::StackExpr:
		{
			const FDabStackExpr& Source = static_cast<const FDabStackExpr&>(Expr);
			TUniquePtr<FDabStackExpr> Result = MakeUnique<FDabStackExpr>();
			Result->Span = Source.Span;
			Result->Steps.Reserve(Source.Steps.Num());
			for (const FDabExprPtr& Step : Source.Steps)
			{
				Result->Steps.Add(DabAstPrivate::CloneOptionalExpr(Step));
			}
			return Result;
		}
		case EDabNodeKind::UnaryExpr:
		{
			const FDabUnaryExpr& Source = static_cast<const FDabUnaryExpr&>(Expr);
			TUniquePtr<FDabUnaryExpr> Result = MakeUnique<FDabUnaryExpr>();
			Result->Span = Source.Span;
			Result->Op = Source.Op;
			Result->Operand = DabAstPrivate::CloneOptionalExpr(Source.Operand);
			return Result;
		}
		case EDabNodeKind::BinaryExpr:
		{
			const FDabBinaryExpr& Source = static_cast<const FDabBinaryExpr&>(Expr);
			TUniquePtr<FDabBinaryExpr> Result = MakeUnique<FDabBinaryExpr>();
			Result->Span = Source.Span;
			Result->Op = Source.Op;
			Result->Lhs = DabAstPrivate::CloneOptionalExpr(Source.Lhs);
			Result->Rhs = DabAstPrivate::CloneOptionalExpr(Source.Rhs);
			Result->OpSpan = Source.OpSpan;
			return Result;
		}
		case EDabNodeKind::ConditionalExpr:
		{
			const FDabConditionalExpr& Source = static_cast<const FDabConditionalExpr&>(Expr);
			TUniquePtr<FDabConditionalExpr> Result = MakeUnique<FDabConditionalExpr>();
			Result->Span = Source.Span;
			Result->Condition = DabAstPrivate::CloneOptionalExpr(Source.Condition);
			Result->Then = DabAstPrivate::CloneOptionalExpr(Source.Then);
			Result->Else = DabAstPrivate::CloneOptionalExpr(Source.Else);
			return Result;
		}
		case EDabNodeKind::ParenExpr:
		{
			const FDabParenExpr& Source = static_cast<const FDabParenExpr&>(Expr);
			TUniquePtr<FDabParenExpr> Result = MakeUnique<FDabParenExpr>();
			Result->Span = Source.Span;
			Result->Inner = DabAstPrivate::CloneOptionalExpr(Source.Inner);
			return Result;
		}
		case EDabNodeKind::IndexExpr:
		{
			const FDabIndexExpr& Source = static_cast<const FDabIndexExpr&>(Expr);
			TUniquePtr<FDabIndexExpr> Result = MakeUnique<FDabIndexExpr>();
			Result->Span = Source.Span;
			Result->Object = DabAstPrivate::CloneOptionalExpr(Source.Object);
			Result->Index = DabAstPrivate::CloneOptionalExpr(Source.Index);
			return Result;
		}
		default:
			break;
		}

		// Not an expression kind: nothing sensible to copy.
		return FDabExprPtr();
	}

	FDabStmtPtr CloneStmt(const FDabStmt& Stmt)
	{
		switch (Stmt.Kind)
		{
		case EDabNodeKind::AssignStmt:
		{
			const FDabAssignStmt& Source = static_cast<const FDabAssignStmt&>(Stmt);
			TUniquePtr<FDabAssignStmt> Result = MakeUnique<FDabAssignStmt>();
			Result->Span = Source.Span;
			Result->Name = Source.Name;
			Result->NameSpan = Source.NameSpan;
			Result->bNameIsString = Source.bNameIsString;
			Result->Value = DabAstPrivate::CloneOptionalExpr(Source.Value);
			Result->Meta = DabAstPrivate::CloneOptionalMeta(Source.Meta);
			Result->bHasSemicolon = Source.bHasSemicolon;
			return Result;
		}
		case EDabNodeKind::DeclStmt:
		{
			const FDabDeclStmt& Source = static_cast<const FDabDeclStmt&>(Stmt);
			TUniquePtr<FDabDeclStmt> Result = MakeUnique<FDabDeclStmt>();
			Result->Span = Source.Span;
			Result->TypeName = Source.TypeName;
			Result->TypeSpan = Source.TypeSpan;
			Result->Name = Source.Name;
			Result->NameSpan = Source.NameSpan;
			Result->Value = DabAstPrivate::CloneOptionalExpr(Source.Value);
			Result->Meta = DabAstPrivate::CloneOptionalMeta(Source.Meta);
			return Result;
		}
		case EDabNodeKind::TransitionStmt:
		{
			const FDabTransitionStmt& Source = static_cast<const FDabTransitionStmt&>(Stmt);
			TUniquePtr<FDabTransitionStmt> Result = MakeUnique<FDabTransitionStmt>();
			Result->Span = Source.Span;
			Result->From = Source.From;
			Result->FromSpan = Source.FromSpan;
			Result->To = Source.To;
			Result->ToSpan = Source.ToSpan;
			Result->Condition = DabAstPrivate::CloneOptionalExpr(Source.Condition);
			Result->Meta = DabAstPrivate::CloneOptionalMeta(Source.Meta);
			return Result;
		}
		case EDabNodeKind::NamedBlockStmt:
		{
			const FDabNamedBlockStmt& Source = static_cast<const FDabNamedBlockStmt&>(Stmt);
			TUniquePtr<FDabNamedBlockStmt> Result = MakeUnique<FDabNamedBlockStmt>();
			Result->Span = Source.Span;
			Result->Word = Source.Word;
			Result->WordSpan = Source.WordSpan;
			Result->Name = Source.Name;
			Result->NameSpan = Source.NameSpan;
			Result->bHasParams = Source.bHasParams;
			Result->Params = DabAstPrivate::CloneParams(Source.Params);
			Result->bHasOutputs = Source.bHasOutputs;
			Result->Outputs = DabAstPrivate::CloneParams(Source.Outputs);
			Result->Meta = DabAstPrivate::CloneOptionalMeta(Source.Meta);
			if (Source.Body)
			{
				Result->Body = DabAstPrivate::CloneBlock(*Source.Body);
			}
			Result->Code = DabAstPrivate::CloneOptionalCodeBlock(Source.Code);
			Result->Value = DabAstPrivate::CloneOptionalExpr(Source.Value);
			return Result;
		}
		case EDabNodeKind::RegionStmt:
		{
			const FDabRegionStmt& Source = static_cast<const FDabRegionStmt&>(Stmt);
			TUniquePtr<FDabRegionStmt> Result = MakeUnique<FDabRegionStmt>();
			Result->Span = Source.Span;
			Result->bEnd = Source.bEnd;
			Result->Label = Source.Label;
			return Result;
		}
		case EDabNodeKind::LetStmt:
		{
			const FDabLetStmt& Source = static_cast<const FDabLetStmt&>(Stmt);
			TUniquePtr<FDabLetStmt> Result = MakeUnique<FDabLetStmt>();
			Result->Span = Source.Span;
			Result->LetKind = Source.LetKind;
			Result->TypeName = Source.TypeName;
			Result->TypeSpan = Source.TypeSpan;
			Result->Name = Source.Name;
			Result->NameSpan = Source.NameSpan;
			Result->Value = DabAstPrivate::CloneOptionalExpr(Source.Value);
			Result->Blocks = DabAstPrivate::CloneFlowBlocks(Source.Blocks);
			return Result;
		}
		case EDabNodeKind::SetStmt:
		{
			const FDabSetStmt& Source = static_cast<const FDabSetStmt&>(Stmt);
			TUniquePtr<FDabSetStmt> Result = MakeUnique<FDabSetStmt>();
			Result->Span = Source.Span;
			Result->Target = DabAstPrivate::CloneOptionalExpr(Source.Target);
			Result->Value = DabAstPrivate::CloneOptionalExpr(Source.Value);
			return Result;
		}
		case EDabNodeKind::ExprStmt:
		{
			const FDabExprStmt& Source = static_cast<const FDabExprStmt&>(Stmt);
			TUniquePtr<FDabExprStmt> Result = MakeUnique<FDabExprStmt>();
			Result->Span = Source.Span;
			Result->Expr = DabAstPrivate::CloneOptionalExpr(Source.Expr);
			Result->Blocks = DabAstPrivate::CloneFlowBlocks(Source.Blocks);
			return Result;
		}
		case EDabNodeKind::IfStmt:
			return DabAstPrivate::CloneIf(static_cast<const FDabIfStmt&>(Stmt));
		case EDabNodeKind::ForStmt:
		{
			const FDabForStmt& Source = static_cast<const FDabForStmt&>(Stmt);
			TUniquePtr<FDabForStmt> Result = MakeUnique<FDabForStmt>();
			Result->Span = Source.Span;
			Result->Names = Source.Names;
			Result->NameSpans = Source.NameSpans;
			Result->Iterable = DabAstPrivate::CloneOptionalExpr(Source.Iterable);
			Result->Body = DabAstPrivate::CloneOptionalCodeBlock(Source.Body);
			return Result;
		}
		case EDabNodeKind::ReturnStmt:
		{
			const FDabReturnStmt& Source = static_cast<const FDabReturnStmt&>(Stmt);
			TUniquePtr<FDabReturnStmt> Result = MakeUnique<FDabReturnStmt>();
			Result->Span = Source.Span;
			Result->Value = DabAstPrivate::CloneOptionalExpr(Source.Value);
			return Result;
		}
		case EDabNodeKind::DetachedStmt:
		{
			const FDabDetachedStmt& Source = static_cast<const FDabDetachedStmt&>(Stmt);
			TUniquePtr<FDabDetachedStmt> Result = MakeUnique<FDabDetachedStmt>();
			Result->Span = Source.Span;
			Result->Body = DabAstPrivate::CloneOptionalCodeBlock(Source.Body);
			return Result;
		}
		default:
			break;
		}

		return FDabStmtPtr();
	}

	TUniquePtr<FDabMeta> CloneMeta(const FDabMeta& Meta)
	{
		TUniquePtr<FDabMeta> Result = MakeUnique<FDabMeta>();
		Result->Span = Meta.Span;
		Result->Entries.Reserve(Meta.Entries.Num());
		for (const TUniquePtr<FDabMetaEntry>& Entry : Meta.Entries)
		{
			if (!Entry)
			{
				Result->Entries.Add(TUniquePtr<FDabMetaEntry>());
				continue;
			}
			TUniquePtr<FDabMetaEntry> Copy = MakeUnique<FDabMetaEntry>();
			Copy->Span = Entry->Span;
			Copy->Key = Entry->Key;
			Copy->KeySpan = Entry->KeySpan;
			Copy->bKeyIsString = Entry->bKeyIsString;
			Copy->Value = DabAstPrivate::CloneOptionalExpr(Entry->Value);
			Result->Entries.Add(MoveTemp(Copy));
		}
		return Result;
	}

	FDabArgumentPtr CloneArgument(const FDabArgument& Argument)
	{
		FDabArgumentPtr Result = MakeUnique<FDabArgument>();
		Result->Span = Argument.Span;
		Result->Name = Argument.Name;
		Result->NameSpan = Argument.NameSpan;
		Result->bNameIsString = Argument.bNameIsString;
		Result->Value = DabAstPrivate::CloneOptionalExpr(Argument.Value);
		return Result;
	}

	// ------------------------------------------------------------------------------------------
	// Traversal
	// ------------------------------------------------------------------------------------------

	void ForEachChild(const FDabNode& Node, TFunctionRef<void(const FDabNode&)> Visit)
	{
		auto VisitIfSet = [&Visit](const FDabNode* Child)
		{
			if (Child)
			{
				Visit(*Child);
			}
		};

		switch (Node.Kind)
		{
		case EDabNodeKind::MemberExpr:
			VisitIfSet(static_cast<const FDabMemberExpr&>(Node).Object.Get());
			break;
		case EDabNodeKind::CallExpr:
		{
			const FDabCallExpr& Call = static_cast<const FDabCallExpr&>(Node);
			VisitIfSet(Call.Callee.Get());
			for (const FDabArgumentPtr& Argument : Call.Args)
			{
				VisitIfSet(Argument.Get());
			}
			break;
		}
		case EDabNodeKind::ListExpr:
			for (const FDabExprPtr& Item : static_cast<const FDabListExpr&>(Node).Items)
			{
				VisitIfSet(Item.Get());
			}
			break;
		case EDabNodeKind::TupleExpr:
			for (const FDabArgumentPtr& Item : static_cast<const FDabTupleExpr&>(Node).Items)
			{
				VisitIfSet(Item.Get());
			}
			break;
		case EDabNodeKind::BlockExpr:
			for (const FDabStmtPtr& Statement : static_cast<const FDabBlockExpr&>(Node).Statements)
			{
				VisitIfSet(Statement.Get());
			}
			break;
		case EDabNodeKind::StackExpr:
			for (const FDabExprPtr& Step : static_cast<const FDabStackExpr&>(Node).Steps)
			{
				VisitIfSet(Step.Get());
			}
			break;
		case EDabNodeKind::UnaryExpr:
			VisitIfSet(static_cast<const FDabUnaryExpr&>(Node).Operand.Get());
			break;
		case EDabNodeKind::BinaryExpr:
		{
			const FDabBinaryExpr& Binary = static_cast<const FDabBinaryExpr&>(Node);
			VisitIfSet(Binary.Lhs.Get());
			VisitIfSet(Binary.Rhs.Get());
			break;
		}
		case EDabNodeKind::ConditionalExpr:
		{
			const FDabConditionalExpr& Conditional = static_cast<const FDabConditionalExpr&>(Node);
			VisitIfSet(Conditional.Condition.Get());
			VisitIfSet(Conditional.Then.Get());
			VisitIfSet(Conditional.Else.Get());
			break;
		}
		case EDabNodeKind::ParenExpr:
			VisitIfSet(static_cast<const FDabParenExpr&>(Node).Inner.Get());
			break;
		case EDabNodeKind::IndexExpr:
		{
			const FDabIndexExpr& Index = static_cast<const FDabIndexExpr&>(Node);
			VisitIfSet(Index.Object.Get());
			VisitIfSet(Index.Index.Get());
			break;
		}
		case EDabNodeKind::AssignStmt:
		{
			const FDabAssignStmt& Assign = static_cast<const FDabAssignStmt&>(Node);
			VisitIfSet(Assign.Value.Get());
			VisitIfSet(Assign.Meta.Get());
			break;
		}
		case EDabNodeKind::DeclStmt:
		{
			const FDabDeclStmt& Decl = static_cast<const FDabDeclStmt&>(Node);
			VisitIfSet(Decl.Value.Get());
			VisitIfSet(Decl.Meta.Get());
			break;
		}
		case EDabNodeKind::TransitionStmt:
		{
			const FDabTransitionStmt& Transition = static_cast<const FDabTransitionStmt&>(Node);
			VisitIfSet(Transition.Condition.Get());
			VisitIfSet(Transition.Meta.Get());
			break;
		}
		case EDabNodeKind::NamedBlockStmt:
		{
			const FDabNamedBlockStmt& NamedBlock = static_cast<const FDabNamedBlockStmt&>(Node);
			for (const TUniquePtr<FDabParam>& Param : NamedBlock.Params)
			{
				VisitIfSet(Param.Get());
			}
			for (const TUniquePtr<FDabParam>& Output : NamedBlock.Outputs)
			{
				VisitIfSet(Output.Get());
			}
			VisitIfSet(NamedBlock.Meta.Get());
			VisitIfSet(NamedBlock.Body.Get());
			VisitIfSet(NamedBlock.Code.Get());
			VisitIfSet(NamedBlock.Value.Get());
			break;
		}
		case EDabNodeKind::CodeBlock:
			for (const FDabStmtPtr& Statement : static_cast<const FDabCodeBlock&>(Node).Statements)
			{
				VisitIfSet(Statement.Get());
			}
			break;
		case EDabNodeKind::LetStmt:
			VisitIfSet(static_cast<const FDabLetStmt&>(Node).Value.Get());
			for (const FDabFlowBlock& Block : static_cast<const FDabLetStmt&>(Node).Blocks)
			{
				VisitIfSet(Block.Body.Get());
			}
			break;
		case EDabNodeKind::SetStmt:
		{
			const FDabSetStmt& Set = static_cast<const FDabSetStmt&>(Node);
			VisitIfSet(Set.Target.Get());
			VisitIfSet(Set.Value.Get());
			break;
		}
		case EDabNodeKind::ExprStmt:
			VisitIfSet(static_cast<const FDabExprStmt&>(Node).Expr.Get());
			for (const FDabFlowBlock& Block : static_cast<const FDabExprStmt&>(Node).Blocks)
			{
				VisitIfSet(Block.Body.Get());
			}
			break;
		case EDabNodeKind::IfStmt:
		{
			const FDabIfStmt& If = static_cast<const FDabIfStmt&>(Node);
			VisitIfSet(If.Condition.Get());
			VisitIfSet(If.Then.Get());
			VisitIfSet(If.ElseIf.Get());
			VisitIfSet(If.Else.Get());
			break;
		}
		case EDabNodeKind::ForStmt:
		{
			const FDabForStmt& For = static_cast<const FDabForStmt&>(Node);
			VisitIfSet(For.Iterable.Get());
			VisitIfSet(For.Body.Get());
			break;
		}
		case EDabNodeKind::ReturnStmt:
			VisitIfSet(static_cast<const FDabReturnStmt&>(Node).Value.Get());
			break;
		case EDabNodeKind::DetachedStmt:
			VisitIfSet(static_cast<const FDabDetachedStmt&>(Node).Body.Get());
			break;
		case EDabNodeKind::Argument:
			VisitIfSet(static_cast<const FDabArgument&>(Node).Value.Get());
			break;
		case EDabNodeKind::MetaEntry:
			VisitIfSet(static_cast<const FDabMetaEntry&>(Node).Value.Get());
			break;
		case EDabNodeKind::Meta:
			for (const TUniquePtr<FDabMetaEntry>& Entry : static_cast<const FDabMeta&>(Node).Entries)
			{
				VisitIfSet(Entry.Get());
			}
			break;
		case EDabNodeKind::Param:
			VisitIfSet(static_cast<const FDabParam&>(Node).Default.Get());
			break;
		case EDabNodeKind::Document:
		{
			const FDabDocument& Document = static_cast<const FDabDocument&>(Node);
			for (const FDabArgumentPtr& Argument : Document.HeaderArgs)
			{
				VisitIfSet(Argument.Get());
			}
			VisitIfSet(Document.Body.Get());
			break;
		}
		case EDabNodeKind::LiteralExpr:
		case EDabNodeKind::NameExpr:
		case EDabNodeKind::RegionStmt:
		case EDabNodeKind::Include:
			break;
		}
	}

	bool TryGetDabPath(const FDabExpr& Expr, TArray<FString>& OutSegments)
	{
		OutSegments.Reset();
		if (!DabAstPrivate::AppendPathSegments(Expr, OutSegments))
		{
			OutSegments.Reset();
			return false;
		}
		return true;
	}

	const FDabExpr& StripDabParens(const FDabExpr& Expr)
	{
		const FDabExpr* Current = &Expr;
		while (const FDabParenExpr* Paren = Current->As<FDabParenExpr>())
		{
			if (!Paren->Inner)
			{
				break;
			}
			Current = Paren->Inner.Get();
		}
		return *Current;
	}
}
