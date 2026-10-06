// Copyright DreamMoon. All Rights Reserved.
//
// The printer: syntax tree -> canonical text, as Docs/Grammar.md "Canonical format" lays it out, and
// the tree dump the corpus goldens use.
//
// Every layout decision is a function of the tree and of the comments and blank-line flags in the
// file's trivia, never of where things sat in the source. The parse of the output carries the same
// tree and the same trivia, so printing it again makes the same decisions; that is the idempotence
// the tests hold the printer to.
//
//   * An expression prints on one line when it fits from the column it starts at, counting what
//     follows it on that line (`;`, `,`, metadata). Otherwise the container that does not fit is
//     broken -- one item per line, one level deeper -- and each item gets the same choice.
//   * Statement-level blocks and stacks are always broken; inside an argument list, list or tuple
//     they are containers like any other.
//   * A node carrying a comment cannot sit on a shared line (a `//` would swallow the rest), and a
//     block holding `#Region` cannot be one line (a directive starts its line): every container
//     around such a node breaks.
//   * Alignment pads the statements of a run so their `=` (or `->`, `:` and `[`) share a column. A
//     statement that the padding pushes onto several lines leaves the run and the runs are laid out
//     again, so a statement is either aligned on one line or printed on its own terms.
//
// Text is built with `\n` and converted to FDabPrintOptions::LineEnd at the very end, so the width
// arithmetic can read a column off the last `\n`. The printer is total: a null child (what a broken
// parse leaves) prints as nothing rather than crashing.

#include "Lang/DabPrinter.h"

#include "Lang/DabAst.h"

namespace UE::DreamAnim::Lang
{
	// Kept apart from the other front-end files' helpers: the module may be built as one unity blob.
	namespace DabPrinterPrivate
	{
		// ------------------------------------------------------------------------------------------
		// Small text helpers
		// ------------------------------------------------------------------------------------------

		/** The escapes the lexer resolves, put back. */
		static FString EscapeString(const FString& Value)
		{
			FString Result;
			Result.Reserve(Value.Len() + 2);
			for (int32 Index = 0; Index < Value.Len(); ++Index)
			{
				const TCHAR Char = Value[Index];
				switch (Char)
				{
				case TEXT('\\'): Result += TEXT("\\\\"); break;
				case TEXT('\"'): Result += TEXT("\\\""); break;
				case TEXT('\n'): Result += TEXT("\\n");  break;
				case TEXT('\t'): Result += TEXT("\\t");  break;
				case TEXT('\r'): Result += TEXT("\\r");  break;
				default:         Result.AppendChar(Char); break;
				}
			}
			return Result;
		}

		static FString QuoteString(const FString& Value)
		{
			return FString(TEXT("\"")) + EscapeString(Value) + TEXT("\"");
		}

		/** An assignment key, argument name or metadata key: quoted when it was written as a string. */
		static FString GetKeyText(const FString& Name, bool bIsString)
		{
			return bIsString ? QuoteString(Name) : Name;
		}

		static FString MakeSpaces(int32 Count)
		{
			return Count > 0 ? FString::ChrN(Count, TEXT(' ')) : FString();
		}

		static FString PadRight(const FString& Text, int32 Width)
		{
			return Text + MakeSpaces(Width - Text.Len());
		}

		static bool IsMultiLine(const FString& Text)
		{
			int32 BreakIndex = INDEX_NONE;
			return Text.FindChar(TEXT('\n'), BreakIndex);
		}

		/** The column right after Text when its first line starts at StartColumn (later lines carry their own indent). */
		static int32 GetEndColumn(const FString& Text, int32 StartColumn)
		{
			int32 LastBreak = INDEX_NONE;
			if (Text.FindLastChar(TEXT('\n'), LastBreak))
			{
				return Text.Len() - LastBreak - 1;
			}
			return StartColumn + Text.Len();
		}

		/** A comment as it is written back: verbatim, with its own line breaks as `\n` and a line comment's trailing blanks dropped. */
		static FString GetCommentText(const FDabComment& Comment)
		{
			FString Text = Comment.Text.Replace(TEXT("\r\n"), TEXT("\n"), ESearchCase::CaseSensitive);
			Text.ReplaceInline(TEXT("\r"), TEXT("\n"), ESearchCase::CaseSensitive);
			if (!Comment.bBlock)
			{
				Text.TrimEndInline();
			}
			return Text;
		}

		static FString GetNumberText(const FDabLiteralExpr& Literal)
		{
			// The lexeme is the truth; the value is only a fallback for a tree built by hand.
			return Literal.Text.IsEmpty() ? FString::SanitizeFloat(Literal.Number) : Literal.Text;
		}

		static FString GetLiteralText(const FDabLiteralExpr& Literal)
		{
			switch (Literal.LiteralKind)
			{
			case EDabLiteralKind::String: return QuoteString(Literal.Text);
			case EDabLiteralKind::Bool:   return Literal.bBool ? TEXT("true") : TEXT("false");
			default:                      return GetNumberText(Literal);
			}
		}

		static FString GetDocumentKindText(const FDabDocument& Document)
		{
			return Document.KindName.IsEmpty() ? FString(LexToString(Document.DocumentKind)) : Document.KindName;
		}

		static FString GetRegionText(const FDabRegionStmt& Region)
		{
			return Region.bEnd ? FString(TEXT("#EndRegion")) : FString(TEXT("#Region ")) + QuoteString(Region.Label);
		}

		template <typename T>
		static bool HasAnyChild(const TArray<TUniquePtr<T>>& Children)
		{
			for (const TUniquePtr<T>& Child : Children)
			{
				if (Child)
				{
					return true;
				}
			}
			return false;
		}

		static bool IsAlignableKind(EDabNodeKind Kind)
		{
			return Kind == EDabNodeKind::AssignStmt || Kind == EDabNodeKind::DeclStmt || Kind == EDabNodeKind::TransitionStmt;
		}

		// ------------------------------------------------------------------------------------------
		// Operator grouping
		// ------------------------------------------------------------------------------------------
		//
		// A parsed tree never needs a parenthesis the author did not write (its grouping is a ParenExpr),
		// so for it these all answer false. A tree made by template expansion can hold `a + b` as the
		// operand of `*`; printing it must not change what it means.

		static constexpr int32 ConditionalPrecedence = 0;
		static constexpr int32 UnaryPrecedence = 7;
		static constexpr int32 PostfixPrecedence = 8;

		static int32 GetOperandPrecedence(const FDabExpr& Expr)
		{
			switch (Expr.Kind)
			{
			case EDabNodeKind::BinaryExpr:      return GetDabBinaryOpPrecedence(static_cast<const FDabBinaryExpr&>(Expr).Op);
			case EDabNodeKind::ConditionalExpr: return ConditionalPrecedence;
			case EDabNodeKind::UnaryExpr:       return UnaryPrecedence;
			default:                            return PostfixPrecedence;
			}
		}

		/** The object of `.Member` or the callee of `(...)`. */
		static bool NeedsPostfixParens(const FDabExpr* Operand)
		{
			return Operand && GetOperandPrecedence(*Operand) < PostfixPrecedence;
		}

		static bool NeedsUnaryParens(const FDabExpr* Operand)
		{
			return Operand && GetOperandPrecedence(*Operand) < UnaryPrecedence;
		}

		/** Binary operators associate to the left, so an equal-strength operand needs parentheses on the right only. */
		static bool NeedsBinaryParens(const FDabExpr* Operand, EDabBinaryOp Op, bool bRightHandSide)
		{
			if (!Operand)
			{
				return false;
			}
			const int32 OperandPrecedence = GetOperandPrecedence(*Operand);
			const int32 OpPrecedence = GetDabBinaryOpPrecedence(Op);
			return bRightHandSide ? OperandPrecedence <= OpPrecedence : OperandPrecedence < OpPrecedence;
		}

		/** The condition of `?:` is an `||`-level expression; the branches take any expression. */
		static bool NeedsConditionParens(const FDabExpr* Operand)
		{
			return Operand && GetOperandPrecedence(*Operand) <= ConditionalPrecedence;
		}

		// ------------------------------------------------------------------------------------------
		// The layout printer
		// ------------------------------------------------------------------------------------------

		/** Column widths shared by one run of aligned statements; zero means "no padding". */
		struct FDabAlign
		{
			/** Assignments: the key, so the `=` lines up. */
			int32 KeyWidth = 0;
			/** Declarations: the type, so the names line up. */
			int32 TypeWidth = 0;
			/** Declarations with a value: the name, so the `=` lines up. */
			int32 NameWidth = 0;
			/** Transitions: the source state, so the `->` lines up. */
			int32 FromWidth = 0;
			/** Transitions: the target state, so the `:` lines up. */
			int32 ToWidth = 0;
			/** Transitions with metadata: the condition, so the `[` lines up. */
			int32 ConditionWidth = 0;
		};

		/**
		 * Formatting functions return text whose first line carries no indentation (the caller has
		 * already written whatever precedes it) and whose later lines are fully indented, with no line
		 * end after the last line. The Append* functions add whole lines, each ending with `\n`.
		 */
		class FDabLayoutPrinter
		{
		public:
			FDabLayoutPrinter(const FDabPrintOptions& InOptions, const FDabFile* InTriviaFile)
				: Options(InOptions)
				, TriviaFile(InTriviaFile)
				, IndentWidth(FMath::Max(InOptions.IndentWidth, 0))
			{
			}

			FString PrintFile(const FDabFile& File);

			/** Expr starting at Column, breaking onto lines indented from Level, with SuffixWidth characters to follow it on its last line. */
			FString FormatExpr(const FDabExpr& Expr, int32 Level, int32 Column, int32 SuffixWidth);

			FString ToOutputLineEnds(FString Text) const
			{
				if (!Options.LineEnd.Equals(TEXT("\n"), ESearchCase::CaseSensitive))
				{
					Text.ReplaceInline(TEXT("\n"), *Options.LineEnd, ESearchCase::CaseSensitive);
				}
				return Text;
			}

		private:
			// -- trivia

			const FDabTrivia* FindTrivia(const FDabNode* Node) const
			{
				return (TriviaFile && Node) ? TriviaFile->FindTrivia(Node) : nullptr;
			}

			/** A Leading or Trailing comment: the node cannot share its line with anything after it. */
			bool HasOwnComments(const FDabNode* Node) const
			{
				const FDabTrivia* Trivia = FindTrivia(Node);
				return Trivia && (Trivia->Leading.Num() > 0 || Trivia->Trailing.IsSet());
			}

			bool HasInnerComments(const FDabNode* Node) const
			{
				const FDabTrivia* Trivia = FindTrivia(Node);
				return Trivia && Trivia->Inner.Num() > 0;
			}

			bool HasTrailingComment(const FDabNode* Node) const
			{
				const FDabTrivia* Trivia = FindTrivia(Node);
				return Trivia && Trivia->Trailing.IsSet();
			}

			bool HasBlankLineBefore(const FDabNode* Node) const
			{
				const FDabTrivia* Trivia = FindTrivia(Node);
				return Trivia && Trivia->BlankLinesBefore > 0;
			}

			/** A blank line or a comment line above a statement ends the alignment run before it. */
			bool StartsNewRun(const FDabNode* Node) const
			{
				const FDabTrivia* Trivia = FindTrivia(Node);
				return Trivia && (Trivia->BlankLinesBefore > 0 || Trivia->Leading.Num() > 0);
			}

			void AppendLeadingComments(FString& Out, int32 Level, const FDabNode* Node) const
			{
				if (const FDabTrivia* Trivia = FindTrivia(Node))
				{
					for (const FDabComment& Comment : Trivia->Leading)
					{
						Out += MakeIndent(Level);
						Out += GetCommentText(Comment);
						Out += TEXT("\n");
					}
				}
			}

			/** After the node's text on the same line, its `;` or `,` already written. */
			void AppendTrailingComment(FString& Out, const FDabNode* Node) const
			{
				const FDabTrivia* Trivia = FindTrivia(Node);
				if (Trivia && Trivia->Trailing.IsSet())
				{
					Out += TEXT(" ");
					Out += GetCommentText(Trivia->Trailing.GetValue());
				}
			}

			/** A container's comments after its last child, each on its own line before the closing bracket. */
			void AppendInnerComments(FString& Out, int32 Level, const FDabNode* Node) const
			{
				if (const FDabTrivia* Trivia = FindTrivia(Node))
				{
					for (const FDabComment& Comment : Trivia->Inner)
					{
						Out += MakeIndent(Level);
						Out += GetCommentText(Comment);
						Out += TEXT("\n");
					}
				}
			}

			/** True when Node cannot print on one line: it holds a comment or a region somewhere inside. */
			bool ForcesBreak(const FDabNode& Node);

			// -- one-line forms

			FString Flat(const FDabExpr& Expr);
			FString FlatOrEmpty(const FDabExpr* Expr) { return Expr ? Flat(*Expr) : FString(); }
			FString FlatOperand(const FDabExpr* Operand, bool bParenthesize);
			FString BuildFlat(const FDabExpr& Expr);
			FString FlatArgument(const FDabArgument& Argument, bool bHeaderStyle);
			FString FlatArguments(const TArray<FDabArgumentPtr>& Arguments, bool bHeaderStyle);
			FString FlatStatement(const FDabStmt& Stmt);
			FString FlatMeta(const FDabMeta& Meta);
			/** ` [ ... ]` with its leading space, or nothing. */
			FString MetaSuffix(const FDabMeta* Meta) { return Meta ? FString(TEXT(" ")) + FlatMeta(*Meta) : FString(); }
			FString NamedBlockHead(const FDabNamedBlockStmt& NamedBlock);

			// -- broken forms

			FString FormatOperand(const FDabExpr* Operand, bool bParenthesize, int32 Level, int32 Column, int32 SuffixWidth);
			FString FormatBrokenArguments(const TCHAR* Open, const TCHAR* Close, const TArray<FDabArgumentPtr>& Arguments,
				int32 Level, const FDabNode& Owner, bool bHeaderStyle);
			FString FormatBrokenList(const FDabListExpr& List, int32 Level);
			FString FormatBrokenStack(const FDabStackExpr& Stack, int32 Level);
			FString FormatBrokenBlock(const FDabBlockExpr& Block, int32 Level);
			/** A block of one plain statement with no comments: it may print on one line (`State Stop { Pose = StopStates; }`). */
			bool IsOneLineBlock(const FDabBlockExpr& Block);
			/** The value of an assignment or of `Word Name = Value;`: a block or stack there is always broken. */
			FString FormatStatementValue(const FDabExpr& Value, int32 Level, int32 Column, int32 SuffixWidth);

			// -- statements

			FString FormatStatement(const FDabStmt& Stmt, int32 Level, const FDabAlign& Align);
			FDabAlign ComputeAlign(const TArray<FDabStmtPtr>& Statements, int32 RunStart, int32 RunEnd);
			void AppendStatements(FString& Out, const TArray<FDabStmtPtr>& Statements, int32 Level);
			FString FormatDocumentHeader(const FDabDocument& Document);

			FString MakeIndent(int32 Level) const { return MakeSpaces(Level * IndentWidth); }
			int32 GetIndentColumn(int32 Level) const { return Level * IndentWidth; }

			const FDabPrintOptions& Options;
			/** Where trivia comes from; null when printing without it. */
			const FDabFile* TriviaFile = nullptr;
			int32 IndentWidth = 4;
			TMap<const FDabNode*, bool> BreakCache;
			TMap<const FDabExpr*, FString> FlatCache;
		};

		bool FDabLayoutPrinter::ForcesBreak(const FDabNode& Node)
		{
			if (const bool* Cached = BreakCache.Find(&Node))
			{
				return *Cached;
			}

			bool bBreak = Node.Is<FDabRegionStmt>() || HasInnerComments(&Node);
			if (!bBreak)
			{
				ForEachChild(Node, [this, &bBreak](const FDabNode& Child)
				{
					if (!bBreak && (HasOwnComments(&Child) || ForcesBreak(Child)))
					{
						bBreak = true;
					}
				});
			}

			BreakCache.Add(&Node, bBreak);
			return bBreak;
		}

		// ------------------------------------------------------------------------------------------
		// One-line forms
		// ------------------------------------------------------------------------------------------

		FString FDabLayoutPrinter::Flat(const FDabExpr& Expr)
		{
			// Every fit test asks for the one-line width of the subtree it stands on; caching keeps that linear.
			if (const FString* Cached = FlatCache.Find(&Expr))
			{
				return *Cached;
			}
			FString Text = BuildFlat(Expr);
			FlatCache.Add(&Expr, Text);
			return Text;
		}

		FString FDabLayoutPrinter::FlatOperand(const FDabExpr* Operand, bool bParenthesize)
		{
			if (!Operand)
			{
				return FString();
			}
			return bParenthesize ? FString(TEXT("(")) + Flat(*Operand) + TEXT(")") : Flat(*Operand);
		}

		FString FDabLayoutPrinter::FlatArgument(const FDabArgument& Argument, bool bHeaderStyle)
		{
			if (!Argument.IsNamed())
			{
				return FlatOrEmpty(Argument.Value.Get());
			}
			const TCHAR* Separator = bHeaderStyle ? TEXT("=") : TEXT(" = ");
			return GetKeyText(Argument.Name, Argument.bNameIsString) + Separator + FlatOrEmpty(Argument.Value.Get());
		}

		FString FDabLayoutPrinter::FlatArguments(const TArray<FDabArgumentPtr>& Arguments, bool bHeaderStyle)
		{
			TArray<FString> Parts;
			Parts.Reserve(Arguments.Num());
			for (const FDabArgumentPtr& Argument : Arguments)
			{
				if (Argument)
				{
					Parts.Add(FlatArgument(*Argument, bHeaderStyle));
				}
			}
			return FString::Join(Parts, TEXT(", "));
		}

		FString FDabLayoutPrinter::FlatMeta(const FDabMeta& Meta)
		{
			TArray<FString> Parts;
			Parts.Reserve(Meta.Entries.Num());
			for (const TUniquePtr<FDabMetaEntry>& Entry : Meta.Entries)
			{
				if (!Entry)
				{
					continue;
				}
				FString Part = GetKeyText(Entry->Key, Entry->bKeyIsString);
				if (Entry->Value)
				{
					Part += TEXT(" = ");
					Part += Flat(*Entry->Value);
				}
				Parts.Add(MoveTemp(Part));
			}
			if (Parts.Num() == 0)
			{
				return TEXT("[]");
			}
			return FString(TEXT("[ ")) + FString::Join(Parts, TEXT("; ")) + TEXT(" ]");
		}

		FString FDabLayoutPrinter::NamedBlockHead(const FDabNamedBlockStmt& NamedBlock)
		{
			FString Head = NamedBlock.Word + TEXT(" ") + NamedBlock.Name;
			if (NamedBlock.bHasParams)
			{
				TArray<FString> Parts;
				for (const TUniquePtr<FDabParam>& Param : NamedBlock.Params)
				{
					if (!Param)
					{
						continue;
					}
					FString Part = Param->Name;
					if (Param->Default)
					{
						Part += TEXT(" = ");
						Part += Flat(*Param->Default);
					}
					Parts.Add(MoveTemp(Part));
				}
				Head += TEXT("(");
				Head += FString::Join(Parts, TEXT(", "));
				Head += TEXT(")");
			}
			Head += MetaSuffix(NamedBlock.Meta.Get());
			return Head;
		}

		FString FDabLayoutPrinter::FlatStatement(const FDabStmt& Stmt)
		{
			switch (Stmt.Kind)
			{
			case EDabNodeKind::AssignStmt:
			{
				const FDabAssignStmt& Assign = static_cast<const FDabAssignStmt&>(Stmt);
				const bool bBlockValue = Assign.Value && Assign.Value->Is<FDabBlockExpr>();
				FString Text = GetKeyText(Assign.Name, Assign.bNameIsString) + TEXT(" = ") + FlatOrEmpty(Assign.Value.Get());
				Text += MetaSuffix(Assign.Meta.Get());
				if (!bBlockValue)
				{
					Text += TEXT(";");
				}
				return Text;
			}
			case EDabNodeKind::DeclStmt:
			{
				const FDabDeclStmt& Decl = static_cast<const FDabDeclStmt&>(Stmt);
				FString Text = Decl.TypeName + TEXT(" ") + Decl.Name;
				if (Decl.Value)
				{
					Text += TEXT(" = ");
					Text += Flat(*Decl.Value);
				}
				Text += MetaSuffix(Decl.Meta.Get());
				Text += TEXT(";");
				return Text;
			}
			case EDabNodeKind::TransitionStmt:
			{
				const FDabTransitionStmt& Transition = static_cast<const FDabTransitionStmt&>(Stmt);
				FString Text = Transition.From + TEXT(" -> ") + Transition.To + TEXT(" : ") + FlatOrEmpty(Transition.Condition.Get());
				Text += MetaSuffix(Transition.Meta.Get());
				Text += TEXT(";");
				return Text;
			}
			case EDabNodeKind::NamedBlockStmt:
			{
				const FDabNamedBlockStmt& NamedBlock = static_cast<const FDabNamedBlockStmt&>(Stmt);
				FString Text = NamedBlockHead(NamedBlock);
				if (NamedBlock.Body)
				{
					Text += TEXT(" ");
					Text += Flat(*NamedBlock.Body);
				}
				else if (NamedBlock.Value)
				{
					Text += TEXT(" = ");
					Text += Flat(*NamedBlock.Value);
					Text += TEXT(";");
				}
				else
				{
					Text += TEXT(";");
				}
				return Text;
			}
			case EDabNodeKind::RegionStmt:
				// Never reached through a fitting block (a region forces its block to break); kept total.
				return GetRegionText(static_cast<const FDabRegionStmt&>(Stmt));
			default:
				return FString();
			}
		}

		FString FDabLayoutPrinter::BuildFlat(const FDabExpr& Expr)
		{
			switch (Expr.Kind)
			{
			case EDabNodeKind::LiteralExpr:
				return GetLiteralText(static_cast<const FDabLiteralExpr&>(Expr));
			case EDabNodeKind::NameExpr:
				return static_cast<const FDabNameExpr&>(Expr).Name;
			case EDabNodeKind::MemberExpr:
			{
				const FDabMemberExpr& Member = static_cast<const FDabMemberExpr&>(Expr);
				return FlatOperand(Member.Object.Get(), NeedsPostfixParens(Member.Object.Get())) + TEXT(".") + Member.Member;
			}
			case EDabNodeKind::CallExpr:
			{
				const FDabCallExpr& Call = static_cast<const FDabCallExpr&>(Expr);
				return FlatOperand(Call.Callee.Get(), NeedsPostfixParens(Call.Callee.Get()))
					+ TEXT("(") + FlatArguments(Call.Args, false) + TEXT(")");
			}
			case EDabNodeKind::ListExpr:
			{
				const FDabListExpr& List = static_cast<const FDabListExpr&>(Expr);
				TArray<FString> Parts;
				Parts.Reserve(List.Items.Num());
				for (const FDabExprPtr& Item : List.Items)
				{
					if (Item)
					{
						Parts.Add(Flat(*Item));
					}
				}
				return Parts.Num() == 0 ? FString(TEXT("[]")) : FString(TEXT("[ ")) + FString::Join(Parts, TEXT(", ")) + TEXT(" ]");
			}
			case EDabNodeKind::TupleExpr:
				return FString(TEXT("(")) + FlatArguments(static_cast<const FDabTupleExpr&>(Expr).Items, false) + TEXT(")");
			case EDabNodeKind::BlockExpr:
			{
				const FDabBlockExpr& Block = static_cast<const FDabBlockExpr&>(Expr);
				TArray<FString> Parts;
				Parts.Reserve(Block.Statements.Num());
				for (const FDabStmtPtr& Statement : Block.Statements)
				{
					if (Statement)
					{
						Parts.Add(FlatStatement(*Statement));
					}
				}
				return Parts.Num() == 0 ? FString(TEXT("{}")) : FString(TEXT("{ ")) + FString::Join(Parts, TEXT(" ")) + TEXT(" }");
			}
			case EDabNodeKind::StackExpr:
			{
				const FDabStackExpr& Stack = static_cast<const FDabStackExpr&>(Expr);
				TArray<FString> Parts;
				Parts.Reserve(Stack.Steps.Num());
				for (const FDabExprPtr& Step : Stack.Steps)
				{
					if (Step)
					{
						Parts.Add(Flat(*Step) + TEXT(";"));
					}
				}
				return Parts.Num() == 0 ? FString(TEXT("{}")) : FString(TEXT("{ ")) + FString::Join(Parts, TEXT(" ")) + TEXT(" }");
			}
			case EDabNodeKind::UnaryExpr:
			{
				const FDabUnaryExpr& Unary = static_cast<const FDabUnaryExpr&>(Expr);
				return FString(GetDabUnaryOpSpelling(Unary.Op)) + FlatOperand(Unary.Operand.Get(), NeedsUnaryParens(Unary.Operand.Get()));
			}
			case EDabNodeKind::BinaryExpr:
			{
				const FDabBinaryExpr& Binary = static_cast<const FDabBinaryExpr&>(Expr);
				return FlatOperand(Binary.Lhs.Get(), NeedsBinaryParens(Binary.Lhs.Get(), Binary.Op, false))
					+ TEXT(" ") + GetDabBinaryOpSpelling(Binary.Op) + TEXT(" ")
					+ FlatOperand(Binary.Rhs.Get(), NeedsBinaryParens(Binary.Rhs.Get(), Binary.Op, true));
			}
			case EDabNodeKind::ConditionalExpr:
			{
				const FDabConditionalExpr& Conditional = static_cast<const FDabConditionalExpr&>(Expr);
				return FlatOperand(Conditional.Condition.Get(), NeedsConditionParens(Conditional.Condition.Get()))
					+ TEXT(" ? ") + FlatOrEmpty(Conditional.Then.Get())
					+ TEXT(" : ") + FlatOrEmpty(Conditional.Else.Get());
			}
			case EDabNodeKind::ParenExpr:
				return FString(TEXT("(")) + FlatOrEmpty(static_cast<const FDabParenExpr&>(Expr).Inner.Get()) + TEXT(")");
			default:
				return FString();
			}
		}

		// ------------------------------------------------------------------------------------------
		// Broken forms
		// ------------------------------------------------------------------------------------------

		FString FDabLayoutPrinter::FormatExpr(const FDabExpr& Expr, int32 Level, int32 Column, int32 SuffixWidth)
		{
			if (!ForcesBreak(Expr))
			{
				FString FlatText = Flat(Expr);
				if (Column + FlatText.Len() + SuffixWidth <= Options.MaxWidth)
				{
					return FlatText;
				}
			}

			switch (Expr.Kind)
			{
			case EDabNodeKind::CallExpr:
			{
				const FDabCallExpr& Call = static_cast<const FDabCallExpr&>(Expr);
				// `Foo()` has nothing to put on lines of its own.
				if (!HasAnyChild(Call.Args) && !HasInnerComments(&Call))
				{
					return Flat(Expr);
				}
				FString Text = FormatOperand(Call.Callee.Get(), NeedsPostfixParens(Call.Callee.Get()), Level, Column, 1);
				Text += FormatBrokenArguments(TEXT("("), TEXT(")"), Call.Args, Level, Call, false);
				return Text;
			}
			case EDabNodeKind::ListExpr:
			{
				const FDabListExpr& List = static_cast<const FDabListExpr&>(Expr);
				if (!HasAnyChild(List.Items) && !HasInnerComments(&List))
				{
					return Flat(Expr);
				}
				return FormatBrokenList(List, Level);
			}
			case EDabNodeKind::TupleExpr:
			{
				const FDabTupleExpr& Tuple = static_cast<const FDabTupleExpr&>(Expr);
				if (!HasAnyChild(Tuple.Items) && !HasInnerComments(&Tuple))
				{
					return Flat(Expr);
				}
				return FormatBrokenArguments(TEXT("("), TEXT(")"), Tuple.Items, Level, Tuple, false);
			}
			case EDabNodeKind::BlockExpr:
			{
				const FDabBlockExpr& Block = static_cast<const FDabBlockExpr&>(Expr);
				if (!HasAnyChild(Block.Statements) && !HasInnerComments(&Block))
				{
					return Flat(Expr);
				}
				return FormatBrokenBlock(Block, Level);
			}
			case EDabNodeKind::StackExpr:
			{
				const FDabStackExpr& Stack = static_cast<const FDabStackExpr&>(Expr);
				if (!HasAnyChild(Stack.Steps) && !HasInnerComments(&Stack))
				{
					return Flat(Expr);
				}
				return FormatBrokenStack(Stack, Level);
			}
			case EDabNodeKind::MemberExpr:
			{
				const FDabMemberExpr& Member = static_cast<const FDabMemberExpr&>(Expr);
				FString Text = FormatOperand(Member.Object.Get(), NeedsPostfixParens(Member.Object.Get()), Level, Column,
					SuffixWidth + 1 + Member.Member.Len());
				Text += TEXT(".");
				Text += Member.Member;
				return Text;
			}
			case EDabNodeKind::UnaryExpr:
			{
				const FDabUnaryExpr& Unary = static_cast<const FDabUnaryExpr&>(Expr);
				FString Text = GetDabUnaryOpSpelling(Unary.Op);
				const int32 OperandColumn = Column + Text.Len();
				Text += FormatOperand(Unary.Operand.Get(), NeedsUnaryParens(Unary.Operand.Get()), Level, OperandColumn, SuffixWidth);
				return Text;
			}
			case EDabNodeKind::BinaryExpr:
			{
				const FDabBinaryExpr& Binary = static_cast<const FDabBinaryExpr&>(Expr);
				const FString OpText = FString(TEXT(" ")) + GetDabBinaryOpSpelling(Binary.Op) + TEXT(" ");
				FString Text = FormatOperand(Binary.Lhs.Get(), NeedsBinaryParens(Binary.Lhs.Get(), Binary.Op, false), Level, Column, OpText.Len());
				Text += OpText;
				const int32 RhsColumn = GetEndColumn(Text, Column);
				Text += FormatOperand(Binary.Rhs.Get(), NeedsBinaryParens(Binary.Rhs.Get(), Binary.Op, true), Level, RhsColumn, SuffixWidth);
				return Text;
			}
			case EDabNodeKind::ConditionalExpr:
			{
				const FDabConditionalExpr& Conditional = static_cast<const FDabConditionalExpr&>(Expr);
				FString Text = FormatOperand(Conditional.Condition.Get(), NeedsConditionParens(Conditional.Condition.Get()), Level, Column, 3);
				Text += TEXT(" ? ");
				const int32 ThenColumn = GetEndColumn(Text, Column);
				Text += FormatOperand(Conditional.Then.Get(), false, Level, ThenColumn, 3);
				Text += TEXT(" : ");
				const int32 ElseColumn = GetEndColumn(Text, Column);
				Text += FormatOperand(Conditional.Else.Get(), false, Level, ElseColumn, SuffixWidth);
				return Text;
			}
			case EDabNodeKind::ParenExpr:
			{
				const FDabParenExpr& Paren = static_cast<const FDabParenExpr&>(Expr);
				return FormatOperand(Paren.Inner.Get(), true, Level, Column, SuffixWidth);
			}
			default:
				// Names and literals have no line to break at: they run past the width.
				return Flat(Expr);
			}
		}

		FString FDabLayoutPrinter::FormatOperand(const FDabExpr* Operand, bool bParenthesize, int32 Level, int32 Column, int32 SuffixWidth)
		{
			if (!Operand)
			{
				return bParenthesize ? FString(TEXT("()")) : FString();
			}
			if (!bParenthesize)
			{
				return FormatExpr(*Operand, Level, Column, SuffixWidth);
			}
			return FString(TEXT("(")) + FormatExpr(*Operand, Level, Column + 1, SuffixWidth + 1) + TEXT(")");
		}

		FString FDabLayoutPrinter::FormatBrokenArguments(const TCHAR* Open, const TCHAR* Close, const TArray<FDabArgumentPtr>& Arguments,
			int32 Level, const FDabNode& Owner, bool bHeaderStyle)
		{
			const int32 ItemLevel = Level + 1;

			// The `=` of every named argument in the list shares one column.
			int32 NameWidth = 0;
			int32 LastIndex = INDEX_NONE;
			for (int32 Index = 0; Index < Arguments.Num(); ++Index)
			{
				if (const FDabArgument* Argument = Arguments[Index].Get())
				{
					LastIndex = Index;
					if (Argument->IsNamed())
					{
						NameWidth = FMath::Max(NameWidth, GetKeyText(Argument->Name, Argument->bNameIsString).Len());
					}
				}
			}

			FString Text = Open;
			Text += TEXT("\n");
			bool bFirst = true;
			for (int32 Index = 0; Index <= LastIndex; ++Index)
			{
				const FDabArgument* Argument = Arguments[Index].Get();
				if (!Argument)
				{
					continue;
				}
				if (!bFirst && HasBlankLineBefore(Argument))
				{
					Text += TEXT("\n");
				}
				bFirst = false;
				AppendLeadingComments(Text, ItemLevel, Argument);

				FString Line = MakeIndent(ItemLevel);
				if (Argument->IsNamed())
				{
					const FString Name = GetKeyText(Argument->Name, Argument->bNameIsString);
					Line += bHeaderStyle ? Name + TEXT("=") : PadRight(Name, NameWidth) + TEXT(" = ");
				}
				const bool bLast = Index == LastIndex;
				if (Argument->Value)
				{
					const int32 ValueColumn = Line.Len();
					Line += FormatExpr(*Argument->Value, ItemLevel, ValueColumn, bLast ? 0 : 1);
				}
				if (!bLast)
				{
					Line += TEXT(",");
				}
				Text += Line;
				AppendTrailingComment(Text, Argument);
				Text += TEXT("\n");
			}
			AppendInnerComments(Text, ItemLevel, &Owner);
			Text += MakeIndent(Level);
			Text += Close;
			return Text;
		}

		FString FDabLayoutPrinter::FormatBrokenList(const FDabListExpr& List, int32 Level)
		{
			const int32 ItemLevel = Level + 1;
			int32 LastIndex = INDEX_NONE;
			for (int32 Index = 0; Index < List.Items.Num(); ++Index)
			{
				if (List.Items[Index])
				{
					LastIndex = Index;
				}
			}

			FString Text = TEXT("[\n");
			bool bFirst = true;
			for (int32 Index = 0; Index <= LastIndex; ++Index)
			{
				const FDabExpr* Item = List.Items[Index].Get();
				if (!Item)
				{
					continue;
				}
				if (!bFirst && HasBlankLineBefore(Item))
				{
					Text += TEXT("\n");
				}
				bFirst = false;
				AppendLeadingComments(Text, ItemLevel, Item);

				const bool bLast = Index == LastIndex;
				FString Line = MakeIndent(ItemLevel);
				const int32 ItemColumn = Line.Len();
				Line += FormatExpr(*Item, ItemLevel, ItemColumn, bLast ? 0 : 1);
				if (!bLast)
				{
					Line += TEXT(",");
				}
				Text += Line;
				AppendTrailingComment(Text, Item);
				Text += TEXT("\n");
			}
			AppendInnerComments(Text, ItemLevel, &List);
			Text += MakeIndent(Level);
			Text += TEXT("]");
			return Text;
		}

		FString FDabLayoutPrinter::FormatBrokenStack(const FDabStackExpr& Stack, int32 Level)
		{
			const int32 StepLevel = Level + 1;
			FString Text = TEXT("{\n");
			bool bFirst = true;
			for (const FDabExprPtr& Step : Stack.Steps)
			{
				if (!Step)
				{
					continue;
				}
				if (!bFirst && HasBlankLineBefore(Step.Get()))
				{
					Text += TEXT("\n");
				}
				bFirst = false;
				AppendLeadingComments(Text, StepLevel, Step.Get());

				FString Line = MakeIndent(StepLevel);
				const int32 StepColumn = Line.Len();
				Line += FormatExpr(*Step, StepLevel, StepColumn, 1);
				Line += TEXT(";");
				Text += Line;
				AppendTrailingComment(Text, Step.Get());
				Text += TEXT("\n");
			}
			AppendInnerComments(Text, StepLevel, &Stack);
			Text += MakeIndent(Level);
			Text += TEXT("}");
			return Text;
		}

		bool FDabLayoutPrinter::IsOneLineBlock(const FDabBlockExpr& Block)
		{
			if (Block.Statements.Num() != 1 || !Block.Statements[0] || ForcesBreak(Block))
			{
				return false;
			}
			const FDabStmt& Only = *Block.Statements[0];
			if (Only.Is<FDabNamedBlockStmt>() || Only.Is<FDabRegionStmt>())
			{
				return false;
			}
			// A nested section or stack keeps its own lines: `{ X = { ... } }` on one line hides the nesting.
			const FDabAssignStmt* Assign = Only.As<FDabAssignStmt>();
			return !(Assign && Assign->Value && (Assign->Value->Is<FDabBlockExpr>() || Assign->Value->Is<FDabStackExpr>()));
		}

		FString FDabLayoutPrinter::FormatBrokenBlock(const FDabBlockExpr& Block, int32 Level)
		{
			FString Text = TEXT("{\n");
			AppendStatements(Text, Block.Statements, Level + 1);
			AppendInnerComments(Text, Level + 1, &Block);
			Text += MakeIndent(Level);
			Text += TEXT("}");
			return Text;
		}

		FString FDabLayoutPrinter::FormatStatementValue(const FDabExpr& Value, int32 Level, int32 Column, int32 SuffixWidth)
		{
			if (const FDabBlockExpr* Block = Value.As<FDabBlockExpr>())
			{
				// An empty section is `{}`: there is nothing to put between two lines.
				if (!HasAnyChild(Block->Statements) && !HasInnerComments(Block))
				{
					return TEXT("{}");
				}
				if (IsOneLineBlock(*Block))
				{
					const FString FlatText = Flat(*Block);
					if (Column + FlatText.Len() + SuffixWidth <= Options.MaxWidth)
					{
						return FlatText;
					}
				}
				return FormatBrokenBlock(*Block, Level);
			}
			if (const FDabStackExpr* Stack = Value.As<FDabStackExpr>())
			{
				if (!HasAnyChild(Stack->Steps) && !HasInnerComments(Stack))
				{
					return TEXT("{}");
				}
				return FormatBrokenStack(*Stack, Level);
			}
			return FormatExpr(Value, Level, Column, SuffixWidth);
		}

		// ------------------------------------------------------------------------------------------
		// Statements
		// ------------------------------------------------------------------------------------------

		FString FDabLayoutPrinter::FormatStatement(const FDabStmt& Stmt, int32 Level, const FDabAlign& Align)
		{
			const int32 BaseColumn = GetIndentColumn(Level);

			switch (Stmt.Kind)
			{
			case EDabNodeKind::AssignStmt:
			{
				const FDabAssignStmt& Assign = static_cast<const FDabAssignStmt&>(Stmt);
				const FString MetaText = MetaSuffix(Assign.Meta.Get());
				// A block value ends with `}` and takes no `;`; a stack, like every other value, does.
				const bool bBlockValue = Assign.Value && Assign.Value->Is<FDabBlockExpr>();
				const int32 TerminatorWidth = bBlockValue ? 0 : 1;

				FString Text = PadRight(GetKeyText(Assign.Name, Assign.bNameIsString), Align.KeyWidth) + TEXT(" = ");
				if (Assign.Value)
				{
					const int32 ValueColumn = BaseColumn + Text.Len();
					Text += FormatStatementValue(*Assign.Value, Level, ValueColumn, MetaText.Len() + TerminatorWidth);
				}
				Text += MetaText;
				if (!bBlockValue)
				{
					Text += TEXT(";");
				}
				return Text;
			}
			case EDabNodeKind::DeclStmt:
			{
				const FDabDeclStmt& Decl = static_cast<const FDabDeclStmt&>(Stmt);
				const FString MetaText = MetaSuffix(Decl.Meta.Get());

				FString Text = PadRight(Decl.TypeName, Align.TypeWidth) + TEXT(" ");
				if (Decl.Value)
				{
					Text += PadRight(Decl.Name, Align.NameWidth);
					Text += TEXT(" = ");
					const int32 ValueColumn = BaseColumn + Text.Len();
					Text += FormatExpr(*Decl.Value, Level, ValueColumn, MetaText.Len() + 1);
				}
				else
				{
					Text += Decl.Name;
				}
				Text += MetaText;
				Text += TEXT(";");
				return Text;
			}
			case EDabNodeKind::TransitionStmt:
			{
				const FDabTransitionStmt& Transition = static_cast<const FDabTransitionStmt&>(Stmt);
				const FString MetaText = MetaSuffix(Transition.Meta.Get());

				FString Text = PadRight(Transition.From, Align.FromWidth) + TEXT(" -> ") + PadRight(Transition.To, Align.ToWidth) + TEXT(" : ");
				if (Transition.Condition)
				{
					// The padding that lines the `[` up follows the condition on its line, so it counts toward its fit.
					const int32 Gap = Transition.Meta ? FMath::Max(0, Align.ConditionWidth - Flat(*Transition.Condition).Len()) : 0;
					const int32 ConditionColumn = BaseColumn + Text.Len();
					const FString ConditionText = FormatExpr(*Transition.Condition, Level, ConditionColumn, Gap + MetaText.Len() + 1);
					Text += ConditionText;
					if (Transition.Meta && !IsMultiLine(ConditionText))
					{
						Text += MakeSpaces(Align.ConditionWidth - ConditionText.Len());
					}
				}
				Text += MetaText;
				Text += TEXT(";");
				return Text;
			}
			case EDabNodeKind::NamedBlockStmt:
			{
				const FDabNamedBlockStmt& NamedBlock = static_cast<const FDabNamedBlockStmt&>(Stmt);
				FString Text = NamedBlockHead(NamedBlock);
				if (NamedBlock.Body && IsOneLineBlock(*NamedBlock.Body)
					&& BaseColumn + Text.Len() + 1 + Flat(*NamedBlock.Body).Len() <= Options.MaxWidth)
				{
					Text += TEXT(" ");
					Text += Flat(*NamedBlock.Body);
				}
				else if (NamedBlock.Body)
				{
					// The body's `{` goes on its own line at the head's indent, even for an empty body.
					Text += TEXT("\n");
					Text += MakeIndent(Level);
					Text += FormatBrokenBlock(*NamedBlock.Body, Level);
				}
				else if (NamedBlock.Value)
				{
					Text += TEXT(" = ");
					const int32 ValueColumn = BaseColumn + Text.Len();
					Text += FormatStatementValue(*NamedBlock.Value, Level, ValueColumn, 1);
					Text += TEXT(";");
				}
				else
				{
					Text += TEXT(";");
				}
				return Text;
			}
			case EDabNodeKind::RegionStmt:
				return GetRegionText(static_cast<const FDabRegionStmt&>(Stmt));
			default:
				return FString();
			}
		}

		FDabAlign FDabLayoutPrinter::ComputeAlign(const TArray<FDabStmtPtr>& Statements, int32 RunStart, int32 RunEnd)
		{
			FDabAlign Align;
			for (int32 Index = RunStart; Index < RunEnd; ++Index)
			{
				const FDabStmt* Stmt = Statements[Index].Get();
				if (const FDabAssignStmt* Assign = Stmt->As<FDabAssignStmt>())
				{
					Align.KeyWidth = FMath::Max(Align.KeyWidth, GetKeyText(Assign->Name, Assign->bNameIsString).Len());
				}
				else if (const FDabDeclStmt* Decl = Stmt->As<FDabDeclStmt>())
				{
					Align.TypeWidth = FMath::Max(Align.TypeWidth, Decl->TypeName.Len());
					if (Decl->Value)
					{
						Align.NameWidth = FMath::Max(Align.NameWidth, Decl->Name.Len());
					}
				}
				else if (const FDabTransitionStmt* Transition = Stmt->As<FDabTransitionStmt>())
				{
					Align.FromWidth = FMath::Max(Align.FromWidth, Transition->From.Len());
					Align.ToWidth = FMath::Max(Align.ToWidth, Transition->To.Len());
					// Only the lines that have a `[` decide where it goes.
					if (Transition->Meta)
					{
						Align.ConditionWidth = FMath::Max(Align.ConditionWidth, FlatOrEmpty(Transition->Condition.Get()).Len());
					}
				}
			}
			return Align;
		}

		void FDabLayoutPrinter::AppendStatements(FString& Out, const TArray<FDabStmtPtr>& Statements, int32 Level)
		{
			const int32 Count = Statements.Num();

			// Each statement on its own terms first; only one-line assignments, declarations and
			// transitions can join a run.
			TArray<FString> PlainTexts;
			TArray<bool> AlignableFlags;
			PlainTexts.SetNum(Count);
			AlignableFlags.Init(false, Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (const FDabStmt* Stmt = Statements[Index].Get())
				{
					PlainTexts[Index] = FormatStatement(*Stmt, Level, FDabAlign());
					AlignableFlags[Index] = IsAlignableKind(Stmt->Kind) && !IsMultiLine(PlainTexts[Index]);
				}
			}

			// Lay the runs out; a statement that padding breaks over lines drops out and the runs are
			// formed again. Every pass drops at least one statement or ends, so this terminates.
			TArray<FString> Texts;
			for (;;)
			{
				Texts = PlainTexts;
				bool bDroppedAny = false;
				int32 RunStart = 0;
				while (RunStart < Count)
				{
					if (!AlignableFlags[RunStart])
					{
						++RunStart;
						continue;
					}

					const EDabNodeKind RunKind = Statements[RunStart]->Kind;
					int32 RunEnd = RunStart + 1;
					while (RunEnd < Count
						&& AlignableFlags[RunEnd]
						&& Statements[RunEnd]->Kind == RunKind
						&& !StartsNewRun(Statements[RunEnd].Get()))
					{
						++RunEnd;
					}

					if (RunEnd - RunStart > 1)
					{
						const FDabAlign Align = ComputeAlign(Statements, RunStart, RunEnd);
						for (int32 Index = RunStart; Index < RunEnd; ++Index)
						{
							FString Aligned = FormatStatement(*Statements[Index], Level, Align);
							if (IsMultiLine(Aligned))
							{
								AlignableFlags[Index] = false;
								bDroppedAny = true;
							}
							else
							{
								Texts[Index] = MoveTemp(Aligned);
							}
						}
					}
					RunStart = RunEnd;
				}

				if (!bDroppedAny)
				{
					break;
				}
			}

			bool bFirst = true;
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FDabStmt* Stmt = Statements[Index].Get();
				if (!Stmt)
				{
					continue;
				}
				// One blank line at most, and never right after the opening `{`.
				if (!bFirst && HasBlankLineBefore(Stmt))
				{
					Out += TEXT("\n");
				}
				bFirst = false;
				AppendLeadingComments(Out, Level, Stmt);
				Out += MakeIndent(Level);
				Out += Texts[Index];
				AppendTrailingComment(Out, Stmt);
				Out += TEXT("\n");
			}
		}

		FString FDabLayoutPrinter::FormatDocumentHeader(const FDabDocument& Document)
		{
			const FString KindText = GetDocumentKindText(Document);

			// The header is one line whatever its width; only a comment among its arguments breaks it.
			bool bBreak = HasInnerComments(&Document);
			for (const FDabArgumentPtr& Argument : Document.HeaderArgs)
			{
				if (Argument && (HasOwnComments(Argument.Get()) || (Argument->Value && ForcesBreak(*Argument->Value))))
				{
					bBreak = true;
				}
			}

			if (!bBreak)
			{
				return KindText + TEXT("(") + FlatArguments(Document.HeaderArgs, true) + TEXT(")");
			}
			return KindText + FormatBrokenArguments(TEXT("("), TEXT(")"), Document.HeaderArgs, 0, Document, true);
		}

		FString FDabLayoutPrinter::PrintFile(const FDabFile& File)
		{
			FString Out;

			bool bAnyInclude = false;
			for (const TUniquePtr<FDabInclude>& Include : File.Includes)
			{
				if (!Include)
				{
					continue;
				}
				if (bAnyInclude && HasBlankLineBefore(Include.Get()))
				{
					Out += TEXT("\n");
				}
				bAnyInclude = true;
				AppendLeadingComments(Out, 0, Include.Get());
				Out += TEXT("#include ");
				Out += QuoteString(Include->Path);
				AppendTrailingComment(Out, Include.Get());
				Out += TEXT("\n");
			}

			if (const FDabDocument* Document = File.Document.Get())
			{
				if (bAnyInclude)
				{
					Out += TEXT("\n");
				}
				AppendLeadingComments(Out, 0, Document);
				Out += FormatDocumentHeader(*Document);
				Out += TEXT("\n{\n");
				if (const FDabBlockExpr* Body = Document->Body.Get())
				{
					AppendStatements(Out, Body->Statements, 1);
					AppendInnerComments(Out, 1, Body);
				}
				Out += TEXT("}");
				// A comment after the closing `}` may be recorded on the document or on its body.
				if (HasTrailingComment(Document))
				{
					AppendTrailingComment(Out, Document);
				}
				else
				{
					AppendTrailingComment(Out, Document->Body.Get());
				}
				Out += TEXT("\n");
			}

			return Out;
		}

		// ------------------------------------------------------------------------------------------
		// Tree dump
		// ------------------------------------------------------------------------------------------

		static FString DescribeNode(const FDabNode& Node)
		{
			switch (Node.Kind)
			{
			case EDabNodeKind::LiteralExpr:
			{
				const FDabLiteralExpr& Literal = static_cast<const FDabLiteralExpr&>(Node);
				switch (Literal.LiteralKind)
				{
				case EDabLiteralKind::String: return FString(TEXT("String ")) + QuoteString(Literal.Text);
				case EDabLiteralKind::Bool:   return Literal.bBool ? TEXT("Bool true") : TEXT("Bool false");
				default:                      return FString(TEXT("Number ")) + GetNumberText(Literal);
				}
			}
			case EDabNodeKind::NameExpr:
				return FString(TEXT("Name ")) + static_cast<const FDabNameExpr&>(Node).Name;
			case EDabNodeKind::MemberExpr:
				return FString(TEXT("Member ")) + static_cast<const FDabMemberExpr&>(Node).Member;
			case EDabNodeKind::CallExpr:        return TEXT("Call");
			case EDabNodeKind::ListExpr:        return TEXT("List");
			case EDabNodeKind::TupleExpr:       return TEXT("Tuple");
			case EDabNodeKind::BlockExpr:       return TEXT("Block");
			case EDabNodeKind::StackExpr:       return TEXT("Stack");
			case EDabNodeKind::ConditionalExpr: return TEXT("Conditional");
			case EDabNodeKind::ParenExpr:       return TEXT("Paren");
			case EDabNodeKind::Meta:            return TEXT("Meta");
			case EDabNodeKind::UnaryExpr:
				return FString(TEXT("Unary ")) + GetDabUnaryOpSpelling(static_cast<const FDabUnaryExpr&>(Node).Op);
			case EDabNodeKind::BinaryExpr:
				return FString(TEXT("Binary ")) + GetDabBinaryOpSpelling(static_cast<const FDabBinaryExpr&>(Node).Op);
			case EDabNodeKind::AssignStmt:
			{
				const FDabAssignStmt& Assign = static_cast<const FDabAssignStmt&>(Node);
				return FString(TEXT("Assign ")) + GetKeyText(Assign.Name, Assign.bNameIsString);
			}
			case EDabNodeKind::DeclStmt:
			{
				const FDabDeclStmt& Decl = static_cast<const FDabDeclStmt&>(Node);
				return FString(TEXT("Decl ")) + Decl.TypeName + TEXT(" ") + Decl.Name;
			}
			case EDabNodeKind::TransitionStmt:
			{
				const FDabTransitionStmt& Transition = static_cast<const FDabTransitionStmt&>(Node);
				return FString(TEXT("Transition ")) + Transition.From + TEXT(" -> ") + Transition.To;
			}
			case EDabNodeKind::NamedBlockStmt:
			{
				const FDabNamedBlockStmt& NamedBlock = static_cast<const FDabNamedBlockStmt&>(Node);
				FString Text = FString(TEXT("NamedBlock ")) + NamedBlock.Word + TEXT(" ") + NamedBlock.Name;
				// `Template X()` and `Template X` are different trees, so an empty list still shows.
				if (NamedBlock.bHasParams)
				{
					TArray<FString> Names;
					for (const TUniquePtr<FDabParam>& Param : NamedBlock.Params)
					{
						if (Param)
						{
							Names.Add(Param->Name);
						}
					}
					Text += TEXT(" (");
					Text += FString::Join(Names, TEXT(", "));
					Text += TEXT(")");
				}
				return Text;
			}
			case EDabNodeKind::RegionStmt:
			{
				const FDabRegionStmt& Region = static_cast<const FDabRegionStmt&>(Node);
				return Region.bEnd ? FString(TEXT("EndRegion")) : FString(TEXT("Region ")) + QuoteString(Region.Label);
			}
			case EDabNodeKind::Argument:
			{
				const FDabArgument& Argument = static_cast<const FDabArgument&>(Node);
				return Argument.IsNamed() ? FString(TEXT("Arg ")) + GetKeyText(Argument.Name, Argument.bNameIsString) : FString(TEXT("Arg"));
			}
			case EDabNodeKind::MetaEntry:
			{
				const FDabMetaEntry& Entry = static_cast<const FDabMetaEntry&>(Node);
				return FString(TEXT("MetaEntry ")) + GetKeyText(Entry.Key, Entry.bKeyIsString);
			}
			case EDabNodeKind::Param:
				return FString(TEXT("Param ")) + static_cast<const FDabParam&>(Node).Name;
			case EDabNodeKind::Include:
				return FString(TEXT("Include ")) + QuoteString(static_cast<const FDabInclude&>(Node).Path);
			case EDabNodeKind::Document:
				return FString(TEXT("Document ")) + GetDocumentKindText(static_cast<const FDabDocument&>(Node));
			}
			return LexToString(Node.Kind);
		}

		static void DumpNode(const FDabNode& Node, int32 Depth, FString& Out)
		{
			Out += MakeSpaces(Depth * 2);
			Out += DescribeNode(Node);
			Out += TEXT("\n");
			ForEachChild(Node, [Depth, &Out](const FDabNode& Child)
			{
				DumpNode(Child, Depth + 1, Out);
			});
		}
	}

	FString PrintDab(const FDabFile& File, const FDabPrintOptions& Options)
	{
		DabPrinterPrivate::FDabLayoutPrinter Printer(Options, &File);
		return Printer.ToOutputLineEnds(Printer.PrintFile(File));
	}

	FString PrintDabExpr(const FDabExpr& Expr, int32 IndentLevel, int32 StartColumn, const FDabFile* File, const FDabPrintOptions& Options)
	{
		DabPrinterPrivate::FDabLayoutPrinter Printer(Options, File);
		return Printer.ToOutputLineEnds(Printer.FormatExpr(Expr, FMath::Max(IndentLevel, 0), FMath::Max(StartColumn, 0), 0));
	}

	FString DumpDabTree(const FDabFile& File)
	{
		FString Out;
		for (const TUniquePtr<FDabInclude>& Include : File.Includes)
		{
			if (Include)
			{
				DabPrinterPrivate::DumpNode(*Include, 0, Out);
			}
		}
		if (File.Document)
		{
			DabPrinterPrivate::DumpNode(*File.Document, 0, Out);
		}
		return Out;
	}
}
