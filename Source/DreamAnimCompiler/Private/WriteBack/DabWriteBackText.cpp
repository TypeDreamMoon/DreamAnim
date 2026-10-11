// Copyright DreamMoon. All Rights Reserved.

#include "WriteBack/DabWriteBackInternal.h"

#include "Lang/DabParser.h"

namespace UE::DreamAnim::Compiler::WriteBackPrivate
{
	FEditableText::FEditableText(const FString& Path, const FString& Text)
		: Source(Path, Text)
	{
		FDabDiagnosticSink Sink(Path);
		File = ParseDab(Source, Sink);
		if (!File)
		{
			return;
		}

		// Every node by where it starts, and what it sits in.
		const TFunction<void(FDabNode&, FDabNode*)> Walk = [this, &Walk](FDabNode& Node, FDabNode* Parent)
		{
			ByOffset.Add(Node.Span.Offset, &Node);
			Parents.Add(&Node, Parent);
			ForEachChild(Node, [&Walk, &Node](const FDabNode& Child) { Walk(const_cast<FDabNode&>(Child), &Node); });
		};
		for (const TUniquePtr<FDabInclude>& Include : File->Includes)
		{
			if (Include)
			{
				Walk(*Include, nullptr);
			}
		}
		if (File->Document)
		{
			Walk(*File->Document, nullptr);
		}
	}

	FDabNode* FEditableText::Find(const FDabSpan& Span, EDabNodeKind Kind) const
	{
		TArray<FDabNode*> Candidates;
		ByOffset.MultiFind(Span.Offset, Candidates);
		for (FDabNode* Candidate : Candidates)
		{
			if (Candidate->Span.Length == Span.Length && Candidate->Kind == Kind)
			{
				return Candidate;
			}
		}
		return nullptr;
	}

	FDabNode* FEditableText::FindSame(const FDabNode& Compiled) const
	{
		FDabNode* Found = Find(Compiled.Span, Compiled.Kind);
		if (!Found)
		{
			return nullptr;
		}
		// A template body's copy carries the template call's span; only real text prints the same.
		if (Compiled.Kind <= EDabNodeKind::ParenExpr
			&& !PrintDabExpr(static_cast<const FDabExpr&>(*Found)).Equals(PrintDabExpr(static_cast<const FDabExpr&>(Compiled)), ESearchCase::CaseSensitive))
		{
			return nullptr;
		}
		return Found;
	}

	FDabNode* FEditableText::GetParent(const FDabNode& Node) const
	{
		FDabNode* const* Parent = Parents.Find(&Node);
		return Parent ? *Parent : nullptr;
	}

	void FEditableText::ReprintExpr(const FDabSpan& OriginalSpan, const FDabExpr& Printed)
	{
		Edits.Add({ EEditKind::Expr, OriginalSpan, &Printed });
	}

	void FEditableText::ReprintStmt(const FDabSpan& OriginalSpan, const FDabStmt& Printed)
	{
		Edits.Add({ EEditKind::Stmt, OriginalSpan, &Printed });
	}

	void FEditableText::AppendStmt(const FDabBlockExpr& Block, const FDabStmt& Printed)
	{
		// At the block's closing brace; MakeText decides between a new line there and reprinting the block.
		FDabSpan At = Block.Span;
		At.Offset = FMath::Max(Block.Span.End() - 1, Block.Span.Offset);
		At.Length = 0;
		FEdit Edit;
		Edit.Kind = EEditKind::Append;
		Edit.Span = At;
		Edit.Printed = &Printed;
		Edits.Add(Edit);
		AppendBlocks.Add(&Printed, &Block);
	}

	void FEditableText::RemoveStmt(const FDabStmt& Stmt)
	{
		FDabSpan Span = Stmt.Span;
		// Its own comments above it go with it.
		if (const FDabTrivia* Trivia = File->FindTrivia(&Stmt); Trivia && !Trivia->Leading.IsEmpty())
		{
			const int32 Start = Trivia->Leading[0].Span.Offset;
			Span.Length += Span.Offset - Start;
			Span.Offset = Start;
		}
		Edits.Add({ EEditKind::Remove, Span, &Stmt });
	}

	int32 FEditableText::GetLineStart(int32 Offset) const
	{
		const FString& Text = Source.GetText();
		int32 Start = FMath::Clamp(Offset, 0, Text.Len());
		while (Start > 0 && Text[Start - 1] != TEXT('\n') && Text[Start - 1] != TEXT('\r'))
		{
			--Start;
		}
		return Start;
	}

	int32 FEditableText::GetIndentLevel(int32 Offset) const
	{
		const FString& Text = Source.GetText();
		int32 Columns = 0;
		for (int32 Index = GetLineStart(Offset); Index < Text.Len(); ++Index)
		{
			if (Text[Index] == TEXT(' '))
			{
				++Columns;
			}
			else if (Text[Index] == TEXT('\t'))
			{
				Columns += 4;
			}
			else
			{
				break;
			}
		}
		return Columns / 4;
	}

	FString FEditableText::MakeText() const
	{
		const FString& Text = Source.GetText();
		FDabPrintOptions PrintOptions;
		// Keep the file's own line ends.
		PrintOptions.LineEnd = Text.Contains(TEXT("\r\n")) ? TEXT("\r\n") : TEXT("\n");
		// And its author's layout: what was broken over lines stays broken.
		PrintOptions.KeepBrokenIn = &Text;
		const FString Newline = PrintOptions.LineEnd;

		// An append to a block whose closing brace shares a line with its statements reprints the whole block,
		// which holds the new statement already.
		TArray<FEdit> Sorted;
		for (const FEdit& Edit : Edits)
		{
			if (Edit.Kind == EEditKind::Append)
			{
				const int32 BraceLine = GetLineStart(Edit.Span.Offset);
				const FDabBlockExpr* const* Block = AppendBlocks.Find(Edit.Printed);
				if (Block && !Text.Mid(BraceLine, Edit.Span.Offset - BraceLine).TrimStartAndEnd().IsEmpty())
				{
					Sorted.Add({ EEditKind::Expr, (*Block)->Span, *Block });
					continue;
				}
			}
			Sorted.Add(Edit);
		}

		// An edit inside another's span is already in the outer one's reprint (the tree was changed for both),
		// and the same reprint asked for twice is made once. Appends at one place are all kept.
		Sorted.StableSort([](const FEdit& A, const FEdit& B)
		{
			return A.Span.Offset != B.Span.Offset ? A.Span.Offset < B.Span.Offset : A.Span.Length > B.Span.Length;
		});
		TArray<FEdit> Kept;
		for (const FEdit& Edit : Sorted)
		{
			const bool bSkip = Kept.ContainsByPredicate([&Edit](const FEdit& Outer)
			{
				const bool bSameSpan = Outer.Span.Offset == Edit.Span.Offset && Outer.Span.Length == Edit.Span.Length;
				if (bSameSpan)
				{
					return Edit.Kind != EEditKind::Append || Outer.Printed == Edit.Printed;
				}
				return Outer.Kind != EEditKind::Append && Outer.Span.Length > 0
					&& Outer.Span.Offset <= Edit.Span.Offset && Edit.Span.End() <= Outer.Span.End();
			});
			if (!bSkip)
			{
				Kept.Add(Edit);
			}
		}

		FString Out = Text;
		for (int32 Index = Kept.Num() - 1; Index >= 0; --Index)
		{
			const FEdit& Edit = Kept[Index];
			const int32 Level = GetIndentLevel(Edit.Span.Offset);
			switch (Edit.Kind)
			{
			case EEditKind::Expr:
			{
				const FString Printed = PrintDabExpr(static_cast<const FDabExpr&>(*Edit.Printed), Level, FMath::Max(Edit.Span.Column - 1, 0), File.Get(), PrintOptions);
				Out = Out.Left(Edit.Span.Offset) + Printed + Out.Mid(Edit.Span.End());
				break;
			}
			case EEditKind::Stmt:
			{
				const FString Printed = PrintDabStmt(static_cast<const FDabStmt&>(*Edit.Printed), Level, File.Get(), PrintOptions);
				Out = Out.Left(Edit.Span.Offset) + Printed + Out.Mid(Edit.Span.End());
				break;
			}
			case EEditKind::Append:
			{
				// The closing brace on a line of its own: the statement goes on a new line above it, one level in.
				const int32 BraceLine = GetLineStart(Edit.Span.Offset);
				const FString BeforeBrace = Text.Mid(BraceLine, Edit.Span.Offset - BraceLine);
				if (BeforeBrace.TrimStartAndEnd().IsEmpty())
				{
					const FString Indent = FString::ChrN((Level + 1) * 4, TEXT(' '));
					const FString Printed = PrintDabStmt(static_cast<const FDabStmt&>(*Edit.Printed), Level + 1, File.Get(), PrintOptions);
					Out = Out.Left(BraceLine) + Indent + Printed + Newline + Out.Mid(BraceLine);
				}
				else if (const FDabBlockExpr* const* Block = AppendBlocks.Find(Edit.Printed))
				{
					// A one-line block: reprint it whole (it holds the new statement already).
					const FDabSpan& BlockSpan = (*Block)->Span;
					const int32 BlockLevel = GetIndentLevel(BlockSpan.Offset);
					const FString Printed = PrintDabExpr(**Block, BlockLevel, FMath::Max(BlockSpan.Column - 1, 0), File.Get(), PrintOptions);
					Out = Out.Left(BlockSpan.Offset) + Printed + Out.Mid(BlockSpan.End());
				}
				break;
			}
			case EEditKind::Remove:
			{
				// Whole lines when the statement has them to itself.
				int32 Start = Edit.Span.Offset;
				int32 End = Edit.Span.End();
				const int32 LineStart = GetLineStart(Start);
				if (Text.Mid(LineStart, Start - LineStart).TrimStartAndEnd().IsEmpty())
				{
					int32 LineEnd = End;
					while (LineEnd < Text.Len() && (Text[LineEnd] == TEXT(' ') || Text[LineEnd] == TEXT('\t')))
					{
						++LineEnd;
					}
					if (LineEnd < Text.Len() && (Text[LineEnd] == TEXT('\r') || Text[LineEnd] == TEXT('\n')))
					{
						LineEnd += (Text[LineEnd] == TEXT('\r') && LineEnd + 1 < Text.Len() && Text[LineEnd + 1] == TEXT('\n')) ? 2 : 1;
						Start = LineStart;
						End = LineEnd;
					}
				}
				Out = Out.Left(Start) + Out.Mid(End);
				break;
			}
			}
		}
		return Out;
	}
}
