// Copyright DreamMoon. All Rights Reserved.
//
// The parser: recursive descent over LexDab's tokens, following Docs/Grammar.md.
//
// Error handling. Every "Expected X, found Y" goes through ShouldReportAtCurrent first, which keeps
// one mistake from being reported several times: a stray character the lexer already reported (an
// Unknown token) is not reported again, a token right after an unclosed string is not blamed for it,
// and the end of the file is reported once -- as DAB2007 on the innermost bracket still open, or as
// the caller's own code when nothing is open. After a failed statement the containing block skips to
// the next `;` at its own depth, or stops before its `}`; a broken argument or list item skips to the
// next `,` of its list. Every loop makes progress on every iteration.
//
// Control Rig documents. The kind word `ControlRig` switches on a few things that no other document has:
// more block words (`Event`, `Function`, `Detached` with a body of code; `Bone`, `Null`, `Control`,
// `Curve`, `Connector`, `Socket` with an ordinary body), generic types in declarations
// (`Array<RigElementKey> Chain`), and code blocks. Statements of code are told apart by their first
// token, by position only and only when it is not written in backticks; inside a code block a `[` after
// an expression is an index. Code statements take no metadata, so the trivia of a code statement is
// recorded exactly like the trivia of a statement of a block.
//
// Trivia. When asked to keep it, the lexer emits comments and the constructor moves them out of the
// token stream, remembering for every token how many comments precede it. Comments are then claimed
// strictly in source order:
//   - before a child of a container (statement, stack step, list item, argument, tuple item, include)
//     is parsed, the unclaimed comments before its first token become its Leading;
//   - after the child (and its `;` or `,`), the first unclaimed comment that starts on the child's
//     last line becomes its Trailing;
//   - at the closing bracket, what is left becomes the container's Inner;
//   - any other comment (between a key and its `=`, inside metadata, ...) is swept up as the cursor
//     moves past it into the innermost child being parsed, and printed among its Leading comments.
// Nodes can still be destroyed while the parse recovers, so nothing is keyed by node address until
// the end: pending trivia is keyed by the offset of the child's first token (no two children start at
// the same token) or of a container's opening bracket, and CollectTrivia moves it onto the nodes that
// survived. The document is an anchor too: comments above it are its Leading, comments before the `)`
// of its header its Inner, and a comment after its `}` on that line its Trailing. Comments after that,
// to the end of the file, have no slot of their own and join the Inner of the document's body.

#include "Lang/DabParser.h"

#include "Lang/DabAst.h"
#include "Lang/DabDiagnostic.h"
#include "Lang/DabSource.h"
#include "Lang/DabToken.h"

namespace UE::DreamAnim::Lang
{
	namespace DabParserDetail
	{
		/** What a statement-parsing function did. */
		enum class EStmtResult : uint8
		{
			/** A node was produced (possibly after reporting a missing `;`). */
			Parsed,
			/** Tokens were consumed and reported or ignored; there is nothing to add and nothing to skip. */
			Skipped,
			/** An error was reported and the caller must skip to the statement boundary. */
			Failed,
		};

		/** Trivia of one child, by comment index, until CollectTrivia turns it into FDabTrivia. */
		struct FPendingTrivia
		{
			TArray<int32> Leading;
			int32 BlankLinesBefore = 0;
			int32 Trailing = INDEX_NONE;
			TArray<int32> Inner;

			bool IsEmpty() const
			{
				return Leading.Num() == 0 && BlankLinesBefore == 0 && Trailing == INDEX_NONE && Inner.Num() == 0;
			}
		};

		/** What BeginAnchor claimed for a child before it was parsed. */
		struct FAnchorStart
		{
			TArray<int32> Leading;
			int32 BlankLinesBefore = 0;
		};

		struct FOpenBracket
		{
			FDabSpan Span;
			const TCHAR* Open = TEXT("");
			const TCHAR* Close = TEXT("");
		};

		class FDabParser
		{
		public:
			FDabParser(const FDabSourceText& InSource, TArray<FDabToken>&& InTokens, FDabDiagnosticSink& InSink,
				bool bInKeepTrivia, bool bInEndAlreadyExplained)
				: Source(InSource)
				, Sink(InSink)
				, bKeepTrivia(bInKeepTrivia)
				, bEndReported(bInEndAlreadyExplained)
			{
				Tokens.Reserve(InTokens.Num());
				CommentsBefore.Reserve(InTokens.Num());
				for (FDabToken& Token : InTokens)
				{
					if (Token.Is(EDabTokenKind::Comment))
					{
						Comments.Add(MoveTemp(Token));
						continue;
					}
					CommentsBefore.Add(Comments.Num());
					Tokens.Add(MoveTemp(Token));
				}

				// Every cursor operation relies on the last token being the end of the file.
				if (Tokens.Num() == 0 || !Tokens.Last().Is(EDabTokenKind::EndOfFile))
				{
					FDabToken End;
					End.Kind = EDabTokenKind::EndOfFile;
					End.Span = Source.MakeSpan(Source.Len(), 0);
					CommentsBefore.Add(Comments.Num());
					Tokens.Add(MoveTemp(End));
				}
			}

			// ------------------------------------------------------------------------------------
			// Entry points
			// ------------------------------------------------------------------------------------

			TUniquePtr<FDabFile> ParseFile()
			{
				TUniquePtr<FDabFile> File = MakeUnique<FDabFile>();
				File->Path = Source.GetPath();

				TArray<int32> Carry;
				// Junk before any document is reported only once a document turns up; with no document
				// at all, DAB2002 alone says what is wrong.
				TArray<TPair<FDabSpan, FString>> JunkBeforeDocument;
				bool bInJunk = false;

				while (!AtEnd())
				{
					const int32 StepStart = Index;
					const FDabToken& First = Current();

					if (IsDirective(First, TEXT("include")))
					{
						bInJunk = false;
						FAnchorStart Anchor = BeginAnchor(Carry);
						TUniquePtr<FDabInclude> Include = ParseInclude();
						if (Include && File->Document)
						{
							Sink.Error(TEXT("DAB2009"), Include->Span, TEXT("'#include' must come before the document."),
								TEXT("Move this line above the document."));
						}
						EndAnchor(MoveTemp(Anchor), Include.Get(), Carry);
						if (Include)
						{
							File->Includes.Add(MoveTemp(Include));
						}
					}
					else if (First.Is(EDabTokenKind::Directive) && !IsKnownDirective(First))
					{
						SkipUnknownDirective();
					}
					else if (IsDocumentStart(!File->Document.IsValid()))
					{
						bool bDocumentFailed = false;
						FAnchorStart Anchor = BeginAnchor(Carry);
						if (File->Document)
						{
							Sink.Error(TEXT("DAB2003"), First.Span, TEXT("The file already has a document; a file holds exactly one."),
								TEXT("Move this document to a file of its own."));
							// Parsed for its diagnostics and to get past it, then dropped.
							ParseDocument(bDocumentFailed);
							EndAnchor(MoveTemp(Anchor), nullptr, Carry);
						}
						else
						{
							for (const TPair<FDabSpan, FString>& Junk : JunkBeforeDocument)
							{
								Sink.Error(TEXT("DAB2001"), Junk.Key, Junk.Value);
							}
							JunkBeforeDocument.Reset();
							File->Document = ParseDocument(bDocumentFailed);
							EndAnchor(MoveTemp(Anchor), File->Document.Get(), Carry);
						}
						// After a broken header or a missing body, what follows is fallout, not news.
						bInJunk = bDocumentFailed;
					}
					else
					{
						if (!bInJunk && !First.Is(EDabTokenKind::Unknown))
						{
							if (File->Document)
							{
								Sink.Error(TEXT("DAB2001"), First.Span, Expected(TEXT("the end of the file after the document")));
							}
							else
							{
								JunkBeforeDocument.Emplace(First.Span,
									Expected(TEXT("'#include' or a document such as 'AnimBlueprint(Name=\"...\") { ... }'")));
							}
						}
						bInJunk = true;
						SkipFileLevelJunk();
					}

					if (Index == StepStart && !AtEnd())
					{
						Advance();
					}
				}

				if (!File->Document)
				{
					const FDabSpan Span = JunkBeforeDocument.Num() > 0 ? JunkBeforeDocument[0].Key : Source.MakeSpan(0, 0);
					Sink.Error(TEXT("DAB2002"), Span,
						TEXT("The file has no document; expected AnimBlueprint(...), AnimLayerInterface(...), AnimTemplates(...) or ControlRig(...) followed by '{ ... }'."),
						TEXT("A .dab file holds AnimBlueprint(Name=\"...\", Root=\"...\") { ... }."));
				}
				else if (bKeepTrivia && File->Document->Body)
				{
					// FDabFile has no place for comments after the document, so they become the body's
					// last comments: moved inside the closing `}` by a reformat, but never lost.
					TArray<int32> Tail = MoveTemp(Carry);
					ClaimCommentsBefore(Index, Tail);
					if (Tail.Num() > 0)
					{
						PendingInner.FindOrAdd(File->Document->Body->Span.Offset).Append(Tail);
					}
				}

				CollectTrivia(*File);
				return File;
			}

			FDabExprPtr ParseStandaloneExpression()
			{
				FDabExprPtr Expr = ParseExpression();
				if (!Expr)
				{
					return nullptr;
				}
				if (!AtEnd())
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("the end of the expression")));
					}
					return nullptr;
				}
				return Expr;
			}

		private:
			// ------------------------------------------------------------------------------------
			// Token classification. Static members rather than free functions: a member found by
			// ordinary lookup turns argument-dependent lookup off, so a unity build cannot pair these
			// calls with a same-named function of another file in UE::DreamAnim::Lang.
			// ------------------------------------------------------------------------------------

			/** A word written in backticks is a name, never a word the parser reads by position. */
			static bool IsWord(const FDabToken& Token, const TCHAR* Word)
			{
				return Token.Is(EDabTokenKind::Identifier) && !Token.bQuoted && Token.Text.Equals(Word, ESearchCase::CaseSensitive);
			}

			/** The block words; a Control Rig document has more of them. */
			bool IsBlockWord(const FDabToken& Token) const
			{
				if (!Token.Is(EDabTokenKind::Identifier) || Token.bQuoted)
				{
					return false;
				}
				static const TCHAR* const BlockWords[] =
				{
					TEXT("StateMachine"), TEXT("State"), TEXT("Layer"), TEXT("Template"), TEXT("Conduit"), TEXT("Alias"),
				};
				for (const TCHAR* Word : BlockWords)
				{
					if (Token.Text.Equals(Word, ESearchCase::CaseSensitive))
					{
						return true;
					}
				}
				if (!bControlRig)
				{
					return false;
				}
				static const TCHAR* const RigBlockWords[] =
				{
					TEXT("Event"), TEXT("Function"), TEXT("Detached"),
					TEXT("Bone"), TEXT("Null"), TEXT("Control"), TEXT("Curve"), TEXT("Connector"), TEXT("Socket"),
				};
				for (const TCHAR* Word : RigBlockWords)
				{
					if (Token.Text.Equals(Word, ESearchCase::CaseSensitive))
					{
						return true;
					}
				}
				return false;
			}

			/** The block words whose body is code. */
			static bool IsCodeBlockWord(const FDabToken& Word)
			{
				return IsWord(Word, TEXT("Event")) || IsWord(Word, TEXT("Function")) || IsWord(Word, TEXT("Detached"));
			}

			static bool IsDirective(const FDabToken& Token, const TCHAR* Word)
			{
				return Token.Is(EDabTokenKind::Directive) && Token.Text.Equals(Word, ESearchCase::CaseSensitive);
			}

			static bool IsRegionDirective(const FDabToken& Token)
			{
				return IsDirective(Token, TEXT("Region")) || IsDirective(Token, TEXT("EndRegion"));
			}

			static bool IsKnownDirective(const FDabToken& Token)
			{
				return IsDirective(Token, TEXT("include")) || IsRegionDirective(Token);
			}

			static EDabDocumentKind GetDocumentKind(const FString& Name)
			{
				if (Name.Equals(TEXT("AnimBlueprint"), ESearchCase::CaseSensitive))
				{
					return EDabDocumentKind::AnimBlueprint;
				}
				if (Name.Equals(TEXT("AnimLayerInterface"), ESearchCase::CaseSensitive))
				{
					return EDabDocumentKind::AnimLayerInterface;
				}
				if (Name.Equals(TEXT("AnimTemplates"), ESearchCase::CaseSensitive))
				{
					return EDabDocumentKind::AnimTemplates;
				}
				if (Name.Equals(TEXT("ControlRig"), ESearchCase::CaseSensitive))
				{
					return EDabDocumentKind::ControlRig;
				}
				return EDabDocumentKind::Unknown;
			}

			/** Higher binds tighter, as GetDabBinaryOpPrecedence documents. */
			static bool GetBinaryOperator(const EDabTokenKind Kind, EDabBinaryOp& OutOp, int32& OutPrecedence)
			{
				switch (Kind)
				{
				case EDabTokenKind::PipePipe: OutOp = EDabBinaryOp::Or; OutPrecedence = 1; return true;
				case EDabTokenKind::AmpersandAmpersand: OutOp = EDabBinaryOp::And; OutPrecedence = 2; return true;
				case EDabTokenKind::EqualEqual: OutOp = EDabBinaryOp::Equal; OutPrecedence = 3; return true;
				case EDabTokenKind::BangEqual: OutOp = EDabBinaryOp::NotEqual; OutPrecedence = 3; return true;
				case EDabTokenKind::Less: OutOp = EDabBinaryOp::Less; OutPrecedence = 4; return true;
				case EDabTokenKind::LessEqual: OutOp = EDabBinaryOp::LessEqual; OutPrecedence = 4; return true;
				case EDabTokenKind::Greater: OutOp = EDabBinaryOp::Greater; OutPrecedence = 4; return true;
				case EDabTokenKind::GreaterEqual: OutOp = EDabBinaryOp::GreaterEqual; OutPrecedence = 4; return true;
				case EDabTokenKind::Plus: OutOp = EDabBinaryOp::Add; OutPrecedence = 5; return true;
				case EDabTokenKind::Minus: OutOp = EDabBinaryOp::Subtract; OutPrecedence = 5; return true;
				case EDabTokenKind::Star: OutOp = EDabBinaryOp::Multiply; OutPrecedence = 6; return true;
				case EDabTokenKind::Slash: OutOp = EDabBinaryOp::Divide; OutPrecedence = 6; return true;
				case EDabTokenKind::Percent: OutOp = EDabBinaryOp::Modulo; OutPrecedence = 6; return true;
				default: return false;
				}
			}

			static bool CanStartExpression(const FDabToken& Token)
			{
				switch (Token.Kind)
				{
				case EDabTokenKind::Number:
				case EDabTokenKind::String:
				case EDabTokenKind::Identifier:
				case EDabTokenKind::LeftParen:
				case EDabTokenKind::LeftBracket:
				case EDabTokenKind::LeftBrace:
				case EDabTokenKind::Bang:
				case EDabTokenKind::Minus:
					return true;
				default:
					return false;
				}
			}

			static bool IsOpeningBracket(const EDabTokenKind Kind)
			{
				return Kind == EDabTokenKind::LeftParen || Kind == EDabTokenKind::LeftBracket || Kind == EDabTokenKind::LeftBrace;
			}

			static bool IsClosingBracket(const EDabTokenKind Kind)
			{
				return Kind == EDabTokenKind::RightParen || Kind == EDabTokenKind::RightBracket || Kind == EDabTokenKind::RightBrace;
			}

			// ------------------------------------------------------------------------------------
			// Cursor
			// ------------------------------------------------------------------------------------

			const FDabToken& Peek(const int32 Ahead) const
			{
				return Tokens[FMath::Min(Index + Ahead, Tokens.Num() - 1)];
			}

			const FDabToken& Current() const
			{
				return Tokens[Index];
			}

			/** The last consumed token; the first token when nothing was consumed yet. */
			const FDabToken& Previous() const
			{
				return Tokens[FMath::Max(Index - 1, 0)];
			}

			bool AtEnd() const
			{
				return Current().Is(EDabTokenKind::EndOfFile);
			}

			bool Check(const EDabTokenKind Kind) const
			{
				return Current().Is(Kind);
			}

			/** Consumes the current token. Never moves past the end of the file. */
			const FDabToken& Advance()
			{
				const int32 Consumed = Index;
				if (Consumed < Tokens.Num() - 1)
				{
					SweepCommentsBefore(Consumed);
					++Index;
				}
				return Tokens[Consumed];
			}

			bool Match(const EDabTokenKind Kind)
			{
				if (!Check(Kind))
				{
					return false;
				}
				Advance();
				return true;
			}

			/** From the token at StartIndex to the last consumed one. */
			FDabSpan SpanFrom(const int32 StartIndex) const
			{
				const FDabSpan& StartSpan = Tokens[StartIndex].Span;
				if (Index <= StartIndex)
				{
					return StartSpan;
				}
				return FDabSpan::Join(StartSpan, Previous().Span);
			}

			// ------------------------------------------------------------------------------------
			// Diagnostics
			// ------------------------------------------------------------------------------------

			/** `'Priority'`, `the string "abc"`, `the end of the file`. */
			FString Describe(const FDabToken& Token) const
			{
				if (Token.Is(EDabTokenKind::EndOfFile))
				{
					return TEXT("the end of the file");
				}
				FString Text = Source.Slice(Token.Span);
				if (Text.Len() > 40)
				{
					Text = Text.Left(37) + TEXT("...");
				}
				if (Token.Is(EDabTokenKind::String))
				{
					return FString::Printf(TEXT("the string %s"), *Text);
				}
				return FString::Printf(TEXT("'%s'"), *Text);
			}

			/** "Expected <What>, found <current token>." */
			FString Expected(const FString& What) const
			{
				return FString::Printf(TEXT("Expected %s, found %s."), *What, *Describe(Current()));
			}

			/** A name in backticks whose closing backtick is missing: it ran to the end of its line (the lexer reported DAB1007). */
			bool IsUnterminatedName(const FDabToken& Token) const
			{
				if (!Token.Is(EDabTokenKind::Identifier) || !Token.bQuoted)
				{
					return false;
				}
				const FString Written = Source.Slice(Token.Span);
				return Written.Len() < 2 || Written[Written.Len() - 1] != TEXT('`');
			}

			bool IsUnterminatedString(const FDabToken& Token) const
			{
				if (!Token.Is(EDabTokenKind::String))
				{
					return false;
				}
				const FString Written = Source.Slice(Token.Span);
				if (Written.Len() < 2 || Written[Written.Len() - 1] != TEXT('"'))
				{
					return true;
				}
				// A final quote behind an odd run of backslashes is escaped and closes nothing.
				int32 Backslashes = 0;
				for (int32 Position = Written.Len() - 2; Position >= 1 && Written[Position] == TEXT('\\'); --Position)
				{
					++Backslashes;
				}
				return (Backslashes % 2) == 1;
			}

			/**
			 * Whether an error about the current token says anything new. False for a character the lexer
			 * already reported, right after an unclosed string or name, and for every end-of-file error after the
			 * first; the first one, while a bracket is open, becomes DAB2007 on that bracket.
			 */
			bool ShouldReportAtCurrent()
			{
				const FDabToken& Token = Current();
				if (Token.Is(EDabTokenKind::Unknown))
				{
					return false;
				}
				if (Index > 0 && (IsUnterminatedString(Previous()) || IsUnterminatedName(Previous())))
				{
					return false;
				}
				if (Token.Is(EDabTokenKind::EndOfFile))
				{
					if (bEndReported)
					{
						return false;
					}
					if (OpenBrackets.Num() > 0)
					{
						ReportUnclosedAtEnd();
						return false;
					}
					bEndReported = true;
				}
				return true;
			}

			/** DAB2007 on the innermost open bracket, once per file. */
			void ReportUnclosedAtEnd()
			{
				if (bEndReported || OpenBrackets.Num() == 0)
				{
					return;
				}
				bEndReported = true;
				const FOpenBracket& Innermost = OpenBrackets.Last();
				Sink.Error(TEXT("DAB2007"), Innermost.Span,
					FString::Printf(TEXT("This '%s' is not closed before the end of the file."), Innermost.Open),
					FString::Printf(TEXT("Add the matching '%s'."), Innermost.Close));
			}

			/** Keeps OpenBrackets in step with the brackets being parsed, whichever way a parse function returns. */
			struct FBracketScope
			{
				FDabParser& Owner;

				FBracketScope(FDabParser& InOwner, const FDabToken& OpenToken, const TCHAR* OpenSpelling, const TCHAR* CloseSpelling)
					: Owner(InOwner)
				{
					FOpenBracket& Bracket = Owner.OpenBrackets.AddDefaulted_GetRef();
					Bracket.Span = OpenToken.Span;
					Bracket.Open = OpenSpelling;
					Bracket.Close = CloseSpelling;
				}

				~FBracketScope()
				{
					Owner.OpenBrackets.Pop();
				}

				FBracketScope(const FBracketScope&) = delete;
				FBracketScope& operator=(const FBracketScope&) = delete;
			};

			/** Keeps CodeDepth up while a code block (or the value of a function) is being parsed. */
			struct FCodeScope
			{
				FDabParser& Owner;
				const bool bActive;

				explicit FCodeScope(FDabParser& InOwner, const bool bInActive = true)
					: Owner(InOwner)
					, bActive(bInActive)
				{
					if (bActive)
					{
						++Owner.CodeDepth;
					}
				}

				~FCodeScope()
				{
					if (bActive)
					{
						--Owner.CodeDepth;
					}
				}

				FCodeScope(const FCodeScope&) = delete;
				FCodeScope& operator=(const FCodeScope&) = delete;
			};

			// ------------------------------------------------------------------------------------
			// Recovery
			// ------------------------------------------------------------------------------------

			/**
			 * After a failed statement or step: skip past the next `;` at this depth, or stop before the
			 * `}` that closes the enclosing block. A balanced `{ ... }` that ends its line also ends the
			 * skip, because an assignment whose value is a block needs no `;` and the next line is then
			 * most likely the next statement.
			 */
			void SkipToStatementBoundary()
			{
				int32 Depth = 0;
				while (!AtEnd())
				{
					const EDabTokenKind Kind = Current().Kind;
					if (Kind == EDabTokenKind::Semicolon && Depth == 0)
					{
						Advance();
						return;
					}
					if (Kind == EDabTokenKind::RightBrace)
					{
						if (Depth == 0)
						{
							return;
						}
						--Depth;
						Advance();
						if (Depth == 0 && Current().Span.Line > Previous().Span.Line)
						{
							return;
						}
						continue;
					}
					if (IsOpeningBracket(Kind))
					{
						++Depth;
					}
					else if (IsClosingBracket(Kind) && Depth > 0)
					{
						--Depth;
					}
					Advance();
				}
			}

			/**
			 * After a broken argument or list item: skip past the next `,` of this list, or stop at its
			 * closer. False when a `;`, a `}` or the end of the file comes first -- the list was never
			 * closed and its owner gives up. bStopAtLineStartBrace serves the document header, whose body
			 * brace on its own line is a better place to resume than anything after it.
			 */
			bool SkipToListBoundary(const EDabTokenKind Closer, const bool bStopAtLineStartBrace)
			{
				int32 Depth = 0;
				while (!AtEnd())
				{
					const FDabToken& Token = Current();
					if (Depth == 0)
					{
						if (Token.Is(EDabTokenKind::Comma))
						{
							Advance();
							return true;
						}
						if (Token.Is(Closer))
						{
							return true;
						}
						if (Token.Is(EDabTokenKind::Semicolon) || Token.Is(EDabTokenKind::RightBrace))
						{
							return false;
						}
						if (bStopAtLineStartBrace && Token.Is(EDabTokenKind::LeftBrace) && Token.bAtLineStart)
						{
							return false;
						}
					}
					if (IsOpeningBracket(Token.Kind))
					{
						++Depth;
					}
					else if (IsClosingBracket(Token.Kind) && Depth > 0)
					{
						--Depth;
					}
					Advance();
				}
				return false;
			}

			/** Inside metadata, where `;` separates entries: stop at the `]` of this depth; false at a `}` or the end of the file. */
			bool SkipToMetaEnd()
			{
				int32 Depth = 0;
				while (!AtEnd())
				{
					const EDabTokenKind Kind = Current().Kind;
					if (Depth == 0 && Kind == EDabTokenKind::RightBracket)
					{
						return true;
					}
					if (Depth == 0 && Kind == EDabTokenKind::RightBrace)
					{
						return false;
					}
					if (IsOpeningBracket(Kind))
					{
						++Depth;
					}
					else if (IsClosingBracket(Kind) && Depth > 0)
					{
						--Depth;
					}
					Advance();
				}
				return false;
			}

			/** At a `(`: through the matching `)`, or to the end of the file. */
			void SkipBalancedParens()
			{
				int32 Depth = 0;
				do
				{
					const EDabTokenKind Kind = Advance().Kind;
					if (Kind == EDabTokenKind::LeftParen)
					{
						++Depth;
					}
					else if (Kind == EDabTokenKind::RightParen)
					{
						--Depth;
					}
				}
				while (Depth > 0 && !AtEnd());
			}

			/** At a `[`: through the matching `]`, or up to a `}` or the end of the file when it is never closed. */
			void SkipBracketGroup()
			{
				int32 Depth = 0;
				do
				{
					const EDabTokenKind Kind = Advance().Kind;
					if (Kind == EDabTokenKind::LeftBracket)
					{
						++Depth;
					}
					else if (Kind == EDabTokenKind::RightBracket)
					{
						--Depth;
					}
				}
				while (Depth > 0 && !AtEnd() && !Check(EDabTokenKind::RightBrace));
			}

			/** An unknown directive was reported by the lexer; drop it and whatever else is on its line. */
			void SkipUnknownDirective()
			{
				const int32 Line = Current().Span.Line;
				Advance();
				while (!AtEnd() && Current().Span.Line == Line && !Check(EDabTokenKind::LeftBrace) && !Check(EDabTokenKind::RightBrace))
				{
					Advance();
				}
			}

			/** Junk at file level: skip, keeping brackets balanced, to the next line that can start an include or a document. */
			void SkipFileLevelJunk()
			{
				int32 Depth = 0;
				do
				{
					const EDabTokenKind Kind = Current().Kind;
					if (IsOpeningBracket(Kind))
					{
						++Depth;
					}
					else if (IsClosingBracket(Kind) && Depth > 0)
					{
						--Depth;
					}
					Advance();
				}
				while (!AtEnd() && !(Depth == 0
					&& (Check(EDabTokenKind::Directive) || (Current().bAtLineStart && IsDocumentStart(true)))));
			}

			/** The kind word of a document. Before the document any `Name(` counts (DAB2004 names it); after it, only the real kinds do. */
			bool IsDocumentStart(const bool bAcceptAnyName) const
			{
				const FDabToken& Token = Current();
				if (!Token.Is(EDabTokenKind::Identifier) || Token.bQuoted)
				{
					return false;
				}
				if (GetDocumentKind(Token.Text) != EDabDocumentKind::Unknown)
				{
					return true;
				}
				return bAcceptAnyName && Peek(1).Is(EDabTokenKind::LeftParen);
			}

			// ------------------------------------------------------------------------------------
			// Trivia bookkeeping
			// ------------------------------------------------------------------------------------

			void ClaimCommentsBefore(const int32 TokenIndex, TArray<int32>& Out)
			{
				const int32 End = CommentsBefore[TokenIndex];
				for (; CommentCursor < End; ++CommentCursor)
				{
					Out.Add(CommentCursor);
				}
			}

			/** Comments the cursor walks past belong to the innermost child being parsed. */
			void SweepCommentsBefore(const int32 TokenIndex)
			{
				const int32 End = CommentsBefore[TokenIndex];
				if (CommentCursor >= End)
				{
					return;
				}
				if (Frames.Num() > 0)
				{
					ClaimCommentsBefore(TokenIndex, Frames.Last());
				}
				else
				{
					// Only junk outside the document gets here; its comments go with it.
					CommentCursor = End;
				}
			}

			/** Empty lines above a token or comment, from the token or comment before it. */
			int32 BlankLinesAbove(const FDabToken& Token, const bool bFirstInFile) const
			{
				return bFirstInFile ? Token.NewlinesBefore : FMath::Max(0, Token.NewlinesBefore - 1);
			}

			/** Before a child is parsed: its Leading comments and the blank lines above them. */
			FAnchorStart BeginAnchor(TArray<int32>& Carry)
			{
				FAnchorStart Start;
				Frames.AddDefaulted();
				if (!bKeepTrivia)
				{
					Carry.Reset();
					return Start;
				}

				Start.Leading = MoveTemp(Carry);
				Carry.Reset();
				ClaimCommentsBefore(Index, Start.Leading);

				if (Start.Leading.Num() > 0)
				{
					const int32 FirstComment = Start.Leading[0];
					// The first comment of the file has nothing above it but the start of the file.
					const bool bFirstInFile = FirstComment == 0 && CommentsBefore[0] > 0;
					Start.BlankLinesBefore = BlankLinesAbove(Comments[FirstComment], bFirstInFile);
				}
				else
				{
					const bool bFirstInFile = Index == 0 && CommentsBefore[0] == 0;
					Start.BlankLinesBefore = BlankLinesAbove(Current(), bFirstInFile);
				}
				return Start;
			}

			/**
			 * After a child and its separator. A child that failed passes null: its comments are carried to
			 * the next sibling (or the container's Inner) instead of being lost.
			 */
			void EndAnchor(FAnchorStart&& Start, const FDabNode* Node, TArray<int32>& Carry)
			{
				TArray<int32> Inside = Frames.Pop();
				if (!bKeepTrivia)
				{
					return;
				}

				if (!Node)
				{
					Carry = MoveTemp(Start.Leading);
					Carry.Append(Inside);
					return;
				}

				FPendingTrivia Pending;
				Pending.Leading = MoveTemp(Start.Leading);
				Pending.Leading.Append(Inside);
				Pending.BlankLinesBefore = Start.BlankLinesBefore;

				if (CommentCursor < CommentsBefore[Index])
				{
					int32 LastLine = 1;
					int32 LastColumn = 1;
					Source.GetLineAndColumn(FMath::Max(Node->Span.Offset, Node->Span.End() - 1), LastLine, LastColumn);
					if (Comments[CommentCursor].Span.Line == LastLine)
					{
						Pending.Trailing = CommentCursor++;
					}
				}

				// The document's header comments were recorded before the document itself ended.
				if (FPendingTrivia* Existing = PendingAnchors.Find(Node->Span.Offset))
				{
					Pending.Inner = MoveTemp(Existing->Inner);
				}

				if (!Pending.IsEmpty())
				{
					PendingAnchors.Add(Node->Span.Offset, MoveTemp(Pending));
				}
			}

			/** At a container's closing bracket: what is left before it is the container's Inner. */
			void ClaimInner(const int32 OpenOffset, TArray<int32>& Carry)
			{
				if (!bKeepTrivia)
				{
					Carry.Reset();
					return;
				}
				TArray<int32> Inner = MoveTemp(Carry);
				Carry.Reset();
				ClaimCommentsBefore(Index, Inner);
				if (Inner.Num() > 0)
				{
					PendingInner.FindOrAdd(OpenOffset).Append(Inner);
				}
			}

			/** A container that never closed: its carried comments go to whatever encloses it. */
			void ReleaseCarry(TArray<int32>& Carry)
			{
				if (Carry.Num() > 0 && Frames.Num() > 0)
				{
					Frames.Last().Append(Carry);
				}
				Carry.Reset();
			}

			/**
			 * The one item of a parenthesised expression was recorded as a tuple item; a ParenExpr has
			 * neither items nor Inner, so its comments go to the enclosing child.
			 */
			void ReleaseTrivia(const int32 AnchorOffset, const int32 OpenOffset)
			{
				if (!bKeepTrivia)
				{
					return;
				}
				TArray<int32> Released;
				FPendingTrivia Pending;
				if (PendingAnchors.RemoveAndCopyValue(AnchorOffset, Pending))
				{
					Released.Append(Pending.Leading);
					if (Pending.Trailing != INDEX_NONE)
					{
						Released.Add(Pending.Trailing);
					}
					Released.Append(Pending.Inner);
				}
				TArray<int32> Inner;
				if (PendingInner.RemoveAndCopyValue(OpenOffset, Inner))
				{
					Released.Append(Inner);
				}
				if (Released.Num() > 0 && Frames.Num() > 0)
				{
					Released.Sort();
					Frames.Last().Append(Released);
				}
			}

			// ------------------------------------------------------------------------------------
			// File level
			// ------------------------------------------------------------------------------------

			TUniquePtr<FDabInclude> ParseInclude()
			{
				const int32 Start = Index;
				Advance();

				if (!Check(EDabTokenKind::String))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("the path string after '#include'")),
							TEXT("Write #include \"Library.dat\"."));
					}
					return nullptr;
				}

				const FDabToken& PathToken = Advance();
				TUniquePtr<FDabInclude> Include = MakeUnique<FDabInclude>();
				Include->Path = PathToken.Text;
				Include->PathSpan = PathToken.Span;
				Include->Span = SpanFrom(Start);
				return Include;
			}

			TUniquePtr<FDabDocument> ParseDocument(bool& bOutFailed)
			{
				const int32 Start = Index;
				bOutFailed = false;

				TUniquePtr<FDabDocument> Document = MakeUnique<FDabDocument>();
				const FDabToken& KindToken = Advance();
				Document->KindName = KindToken.Text;
				Document->KindSpan = KindToken.Span;
				Document->DocumentKind = GetDocumentKind(KindToken.Text);
				bControlRig = Document->DocumentKind == EDabDocumentKind::ControlRig;

				if (Document->DocumentKind == EDabDocumentKind::Unknown)
				{
					Sink.Error(TEXT("DAB2004"), KindToken.Span,
						FString::Printf(TEXT("Unknown document kind '%s'; expected AnimBlueprint, AnimLayerInterface, AnimTemplates or ControlRig."), *KindToken.Text));
				}

				if (Check(EDabTokenKind::LeftParen))
				{
					const int32 HeaderOpenOffset = Current().Span.Offset;
					bool bTrailingComma = false;
					if (ParseArgumentList(Document->HeaderArgs, bTrailingComma, true))
					{
						CheckDocumentHeader(*Document, SpanFrom(Start));
					}
					else
					{
						bOutFailed = true;
					}
					// The header is not a call node, so the comments before its `)` are the document's Inner.
					TArray<int32> HeaderInner;
					if (bKeepTrivia && PendingInner.RemoveAndCopyValue(HeaderOpenOffset, HeaderInner))
					{
						PendingAnchors.FindOrAdd(KindToken.Span.Offset).Inner.Append(HeaderInner);
					}
				}
				else
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span,
							Expected(FString::Printf(TEXT("'(' and the header after '%s'"), *KindToken.Text)),
							TEXT("Write the header as AnimBlueprint(Name=\"...\", Root=\"...\")."));
					}
					bOutFailed = true;
				}

				if (Check(EDabTokenKind::LeftBrace))
				{
					// A document body is a block whatever its first entry looks like.
					Document->Body = ParseBlockExpr();
				}
				else
				{
					if (!bOutFailed && ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("'{' to open the body of the document")));
					}
					bOutFailed = true;
				}

				Document->Span = SpanFrom(Start);
				return Document;
			}

			void CheckDocumentHeader(const FDabDocument& Document, const FDabSpan& HeaderSpan)
			{
				bool bHasName = false;
				for (const FDabArgumentPtr& Argument : Document.HeaderArgs)
				{
					if (!Argument->IsNamed())
					{
						Sink.Error(TEXT("DAB2005"), Argument->Span,
							TEXT("The document header takes only named arguments: Name = \"...\" and, optionally, Root = \"...\"."));
						continue;
					}

					const bool bIsName = Argument->Name.Equals(TEXT("Name"), ESearchCase::CaseSensitive);
					const bool bIsRoot = Argument->Name.Equals(TEXT("Root"), ESearchCase::CaseSensitive);
					if (!bIsName && !bIsRoot)
					{
						Sink.Error(TEXT("DAB2005"), Argument->NameSpan,
							FString::Printf(TEXT("The document header cannot take '%s'; only 'Name' and 'Root' are allowed."), *Argument->Name));
						continue;
					}

					bHasName |= bIsName;
					const FDabLiteralExpr* Literal = Argument->Value ? Argument->Value->As<FDabLiteralExpr>() : nullptr;
					if (!Literal || Literal->LiteralKind != EDabLiteralKind::String)
					{
						Sink.Error(TEXT("DAB2005"), Argument->Value ? Argument->Value->Span : Argument->Span,
							FString::Printf(TEXT("The document header's '%s' must be a string."), *Argument->Name));
					}
				}

				if (!bHasName)
				{
					Sink.Error(TEXT("DAB2005"), HeaderSpan, TEXT("The document header has no 'Name'."),
						FString::Printf(TEXT("Write the asset path relative to Root: %s(Name=\"Folder/Asset\", Root=\"Game\")."), *Document.KindName));
				}
			}

			// ------------------------------------------------------------------------------------
			// Blocks, stacks and statements
			// ------------------------------------------------------------------------------------

			/** At an entry start: does it read as a statement rather than an expression? Ahead is the entry's first token. */
			bool LooksLikeStatementAt(const int32 Ahead) const
			{
				const FDabToken& First = Peek(Ahead);
				const FDabToken& Second = Peek(Ahead + 1);
				if (First.Is(EDabTokenKind::Directive))
				{
					return true;
				}
				if ((First.Is(EDabTokenKind::Identifier) || First.Is(EDabTokenKind::String)) && Second.Is(EDabTokenKind::Assign))
				{
					return true;
				}
				if (First.Is(EDabTokenKind::Identifier) && (Second.Is(EDabTokenKind::Identifier) || Second.Is(EDabTokenKind::Arrow)))
				{
					return true;
				}
				// `Array<RigElementKey> Chain`: a generic type followed by the name it declares.
				return bControlRig && IsGenericDeclarationAt(Ahead);
			}

			/**
			 * At Ahead: a type name, with its `<...>` arguments when it is a generic type of a Control Rig
			 * document, as far as a lookahead can tell. Returns the index of the token after it, or INDEX_NONE
			 * (as an offset from the current token) when no type starts there.
			 */
			int32 ScanTypeEnd(const int32 Ahead, const int32 Depth = 0) const
			{
				if (!Peek(Ahead).Is(EDabTokenKind::Identifier) || Depth > MaxTypeDepth)
				{
					return INDEX_NONE;
				}
				int32 At = Ahead + 1;
				if (bControlRig && Peek(At).Is(EDabTokenKind::Less))
				{
					++At;
					while (true)
					{
						const int32 InnerEnd = ScanTypeEnd(At, Depth + 1);
						if (InnerEnd == INDEX_NONE)
						{
							return INDEX_NONE;
						}
						At = InnerEnd;
						if (!Peek(At).Is(EDabTokenKind::Comma))
						{
							break;
						}
						++At;
					}
					if (!Peek(At).Is(EDabTokenKind::Greater))
					{
						return INDEX_NONE;
					}
					++At;
				}
				return At;
			}

			/** `Ident < ... > Ident` at Ahead: a declaration of a generic type. A plain `Ident Ident` is not asked here. */
			bool IsGenericDeclarationAt(const int32 Ahead) const
			{
				const int32 TypeEnd = ScanTypeEnd(Ahead);
				return TypeEnd != INDEX_NONE && TypeEnd > Ahead + 1 && Peek(TypeEnd).Is(EDabTokenKind::Identifier);
			}

			/** At a `{`: the lookahead table of Grammar.md, "Telling a block from a stack". */
			bool LooksLikeBlock() const
			{
				return Peek(1).Is(EDabTokenKind::RightBrace) || LooksLikeStatementAt(1);
			}

			FDabExprPtr ParseBraced()
			{
				if (LooksLikeBlock())
				{
					return ParseBlockExpr();
				}
				return ParseStackExpr();
			}

			/** Pairs `#EndRegion` with the `#Region` before it in the same block; one without a partner is a warning. */
			void NoteRegion(const FDabStmt& Statement, TArray<const FDabRegionStmt*>& OpenRegions)
			{
				const FDabRegionStmt* Region = Statement.As<FDabRegionStmt>();
				if (!Region)
				{
					return;
				}
				if (!Region->bEnd)
				{
					OpenRegions.Add(Region);
				}
				else if (OpenRegions.Num() > 0)
				{
					OpenRegions.Pop();
				}
				else
				{
					Sink.Warning(TEXT("DAB2008"), Region->Span, TEXT("'#EndRegion' has no matching '#Region' in this block."));
				}
			}

			void WarnUnclosedRegions(const TArray<const FDabRegionStmt*>& OpenRegions)
			{
				for (const FDabRegionStmt* Region : OpenRegions)
				{
					Sink.Warning(TEXT("DAB2008"), Region->Span,
						FString::Printf(TEXT("'#Region \"%s\"' has no matching '#EndRegion' in this block."), *Region->Label));
				}
			}

			/** At a `{` that opens a block. Returns the block even when the file ends inside it. */
			TUniquePtr<FDabBlockExpr> ParseBlockExpr()
			{
				const int32 Start = Index;
				const FDabToken& Open = Advance();
				FBracketScope Scope(*this, Open, TEXT("{"), TEXT("}"));

				TUniquePtr<FDabBlockExpr> Block = MakeUnique<FDabBlockExpr>();
				TArray<int32> Carry;
				TArray<const FDabRegionStmt*> OpenRegions;

				while (!Check(EDabTokenKind::RightBrace) && !AtEnd())
				{
					const int32 StatementStart = Index;
					FAnchorStart Anchor = BeginAnchor(Carry);

					FDabStmtPtr Statement;
					if (ParseStatement(Statement) == EStmtResult::Failed)
					{
						SkipToStatementBoundary();
					}

					EndAnchor(MoveTemp(Anchor), Statement.Get(), Carry);

					if (Statement)
					{
						NoteRegion(*Statement, OpenRegions);
						Block->Statements.Add(MoveTemp(Statement));
					}

					if (Index == StatementStart && !AtEnd() && !Check(EDabTokenKind::RightBrace))
					{
						Advance();
					}
				}

				WarnUnclosedRegions(OpenRegions);

				if (Check(EDabTokenKind::RightBrace))
				{
					ClaimInner(Open.Span.Offset, Carry);
					Advance();
				}
				else
				{
					ReportUnclosedAtEnd();
					ReleaseCarry(Carry);
				}

				Block->Span = SpanFrom(Start);
				return Block;
			}

			/** At a `{` that opens a stack. Returns the stack even when the file ends inside it. */
			TUniquePtr<FDabStackExpr> ParseStackExpr()
			{
				const int32 Start = Index;
				const FDabToken& Open = Advance();
				FBracketScope Scope(*this, Open, TEXT("{"), TEXT("}"));

				TUniquePtr<FDabStackExpr> Stack = MakeUnique<FDabStackExpr>();
				TArray<int32> Carry;

				while (!Check(EDabTokenKind::RightBrace) && !AtEnd())
				{
					const int32 StepStart = Index;
					FAnchorStart Anchor = BeginAnchor(Carry);

					FDabExprPtr Step;
					if (LooksLikeStatementAt(0))
					{
						ParseStatementInStack();
					}
					else
					{
						Step = ParseExpression();
						if (!Step)
						{
							SkipToStatementBoundary();
						}
						else if (!Match(EDabTokenKind::Semicolon))
						{
							ReportMissingSemicolon(TEXT("';' after the stack step"),
								Check(EDabTokenKind::LeftBracket)
									? FString(TEXT("Metadata belongs to a statement: put it after the whole stack, as in 'Pose = { ... } [ Cached ];'."))
									: FString());
						}
					}

					EndAnchor(MoveTemp(Anchor), Step.Get(), Carry);
					if (Step)
					{
						Stack->Steps.Add(MoveTemp(Step));
					}

					if (Index == StepStart && !AtEnd() && !Check(EDabTokenKind::RightBrace))
					{
						Advance();
					}
				}

				if (Check(EDabTokenKind::RightBrace))
				{
					ClaimInner(Open.Span.Offset, Carry);
					Advance();
				}
				else
				{
					ReportUnclosedAtEnd();
					ReleaseCarry(Carry);
				}

				Stack->Span = SpanFrom(Start);
				return Stack;
			}

			/** A statement where a stack step was expected: DAB2010, parsed only so the stack can go on after it. */
			void ParseStatementInStack()
			{
				const FDabToken& First = Current();
				const FDabToken& Second = Peek(1);

				if (First.Is(EDabTokenKind::Directive) && !IsRegionDirective(First))
				{
					// `#include` (DAB2009) or an unknown directive: reported as such, not as a mixed stack.
					FDabStmtPtr Ignored;
					ParseDirectiveStatement(Ignored);
					return;
				}

				const TCHAR* What = TEXT("an assignment");
				if (First.Is(EDabTokenKind::Directive))
				{
					What = TEXT("a region marker");
				}
				else if (Second.Is(EDabTokenKind::Arrow))
				{
					What = TEXT("a transition");
				}
				else if (Second.Is(EDabTokenKind::Identifier))
				{
					What = IsBlockWord(First) ? TEXT("a named block") : TEXT("a declaration");
				}

				const FDabSpan FirstSpan = First.Span;
				FDabStmtPtr Statement;
				if (ParseStatement(Statement) == EStmtResult::Failed)
				{
					SkipToStatementBoundary();
				}

				Sink.Error(TEXT("DAB2010"), Statement.IsValid() ? Statement->Span : FirstSpan,
					FString::Printf(TEXT("A stack cannot hold %s; its entries are pose steps such as 'Node(...);'."), What),
					TEXT("Write every entry of a stack as an expression ending with ';', or make every entry a statement to write a block."));
			}

			/** One entry of a block. */
			EStmtResult ParseStatement(FDabStmtPtr& Out)
			{
				const FDabToken& First = Current();
				const FDabToken& Second = Peek(1);

				if (First.Is(EDabTokenKind::Directive))
				{
					return ParseDirectiveStatement(Out);
				}
				if ((First.Is(EDabTokenKind::Identifier) || First.Is(EDabTokenKind::String)) && Second.Is(EDabTokenKind::Assign))
				{
					return ParseAssign(Out);
				}
				if (First.Is(EDabTokenKind::Identifier))
				{
					if (Second.Is(EDabTokenKind::Arrow))
					{
						return ParseTransition(Out);
					}
					if (Second.Is(EDabTokenKind::Identifier))
					{
						return IsBlockWord(First) ? ParseNamedBlock(Out) : ParseDecl(Out);
					}
					if (bControlRig)
					{
						// `Detached { ... }` and `Detached [ ... ] { ... }`: the one block word that takes no name.
						if (IsWord(First, TEXT("Detached")) && (Second.Is(EDabTokenKind::LeftBrace) || Second.Is(EDabTokenKind::LeftBracket)))
						{
							return ParseNamedBlock(Out);
						}
						// `Array<RigElementKey> Chain [ Public ];`
						if (IsGenericDeclarationAt(0))
						{
							return ParseDecl(Out);
						}
					}
					if (IsBlockWord(First))
					{
						Advance();
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DAB2001"), Current().Span, Expected(FString::Printf(TEXT("a name after '%s'"), *First.Text)));
						}
						return EStmtResult::Failed;
					}
				}
				return ParseBareExpressionInBlock();
			}

			EStmtResult ParseDirectiveStatement(FDabStmtPtr& Out)
			{
				const FDabToken& Directive = Current();

				if (IsRegionDirective(Directive))
				{
					const int32 Start = Index;
					Advance();
					TUniquePtr<FDabRegionStmt> Region = MakeUnique<FDabRegionStmt>();
					Region->bEnd = IsDirective(Directive, TEXT("EndRegion"));
					if (!Region->bEnd)
					{
						// The label must be on the directive's line; a string on the next line is the next statement's key.
						if (Check(EDabTokenKind::String) && Current().Span.Line == Directive.Span.Line)
						{
							Region->Label = Advance().Text;
						}
						else if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("a label string after '#Region'")),
								TEXT("Write #Region \"Label\"."));
						}
					}
					Region->Span = SpanFrom(Start);
					Out = MoveTemp(Region);
					return EStmtResult::Parsed;
				}

				if (IsDirective(Directive, TEXT("include")))
				{
					TUniquePtr<FDabInclude> Include = ParseInclude();
					if (Include)
					{
						Sink.Error(TEXT("DAB2009"), Include->Span, TEXT("'#include' must come before the document, not inside it."),
							TEXT("Move this line to the top of the file."));
					}
					return EStmtResult::Skipped;
				}

				SkipUnknownDirective();
				return EStmtResult::Skipped;
			}

			/** An entry of a block that starts like an expression. */
			EStmtResult ParseBareExpressionInBlock()
			{
				if (!CanStartExpression(Current()))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("a statement")));
					}
					return EStmtResult::Failed;
				}

				FDabExprPtr Expr = ParseExpression();
				if (!Expr)
				{
					return EStmtResult::Failed;
				}

				if (Check(EDabTokenKind::Semicolon) || Check(EDabTokenKind::RightBrace))
				{
					// A well-formed stack step: the author mixed the two kinds of `{ }`.
					Sink.Error(TEXT("DAB2010"), Expr->Span,
						TEXT("A block cannot hold a bare expression; its entries are statements such as 'Key = Value;'."),
						TEXT("Make every entry a statement, or make every entry an expression ending with ';' to write a stack."));
					Match(EDabTokenKind::Semicolon);
					return EStmtResult::Skipped;
				}

				// Not a step either; a lone key most likely lost its `=`.
				const FDabLiteralExpr* Literal = Expr->As<FDabLiteralExpr>();
				const bool bLoneKey = Expr->Is<FDabNameExpr>() || (Literal && Literal->LiteralKind == EDabLiteralKind::String);
				if (ShouldReportAtCurrent())
				{
					Sink.Error(TEXT("DAB2001"), Current().Span,
						Expected(bLoneKey ? FString::Printf(TEXT("'=' after '%s'"), *Source.Slice(Expr->Span)) : FString(TEXT("';' after the expression"))));
				}
				return EStmtResult::Failed;
			}

			/**
			 * A statement that parsed but lacks its `;`. The statement is kept; when the next token begins
			 * a new line it most likely begins the next statement, so parsing resumes there.
			 */
			void ReportMissingSemicolon(const FString& What, const FString& Hint = FString())
			{
				if (ShouldReportAtCurrent())
				{
					Sink.Error(TEXT("DAB2001"), Current().Span, Expected(What), Hint);
				}
				if (AtEnd() || Check(EDabTokenKind::RightBrace) || Current().Span.Line > Previous().Span.Line)
				{
					return;
				}
				SkipToStatementBoundary();
			}

			/** `Key = Expr [Meta] ;` -- the `;` may be left out when the value ends with `}`. */
			EStmtResult ParseAssign(FDabStmtPtr& Out)
			{
				const int32 Start = Index;
				TUniquePtr<FDabAssignStmt> Assign = MakeUnique<FDabAssignStmt>();

				const FDabToken& Key = Advance();
				Assign->Name = Key.Text;
				Assign->NameSpan = Key.Span;
				Assign->bNameIsString = Key.Is(EDabTokenKind::String);
				Advance(); // =

				Assign->Value = ParseExpression();
				if (!Assign->Value)
				{
					return EStmtResult::Failed;
				}
				const bool bValueEndsWithBrace = Previous().Is(EDabTokenKind::RightBrace);

				if (Check(EDabTokenKind::LeftBracket))
				{
					Assign->Meta = ParseMeta();
					if (!Assign->Meta)
					{
						return EStmtResult::Failed;
					}
				}

				if (Match(EDabTokenKind::Semicolon))
				{
					Assign->bHasSemicolon = true;
				}
				else if (bValueEndsWithBrace)
				{
					Assign->bHasSemicolon = false;
				}
				else
				{
					ReportMissingSemicolon(FString::Printf(TEXT("';' after the assignment to '%s'"), *Assign->Name));
				}

				Assign->Span = SpanFrom(Start);
				Out = MoveTemp(Assign);
				return EStmtResult::Parsed;
			}

			/** `Type Name [= Expr] [Meta] ;` */
			EStmtResult ParseDecl(FDabStmtPtr& Out)
			{
				const int32 Start = Index;
				TUniquePtr<FDabDeclStmt> Decl = MakeUnique<FDabDeclStmt>();

				// The caller has seen a name after the type, so this cannot fail on the way to it.
				if (!ParseTypeName(Decl->TypeName, Decl->TypeSpan, false))
				{
					return EStmtResult::Failed;
				}
				const FDabToken& Name = Advance();
				Decl->Name = Name.Text;
				Decl->NameSpan = Name.Span;

				if (Match(EDabTokenKind::Assign))
				{
					Decl->Value = ParseExpression();
					if (!Decl->Value)
					{
						return EStmtResult::Failed;
					}
				}

				if (Check(EDabTokenKind::LeftBracket))
				{
					Decl->Meta = ParseMeta();
					if (!Decl->Meta)
					{
						return EStmtResult::Failed;
					}
				}

				if (!Match(EDabTokenKind::Semicolon))
				{
					ReportMissingSemicolon(Decl->Value || Decl->Meta
						? FString::Printf(TEXT("';' after the declaration of '%s'"), *Decl->Name)
						: FString::Printf(TEXT("'=' or ';' after the declaration of '%s'"), *Decl->Name));
				}

				Decl->Span = SpanFrom(Start);
				Out = MoveTemp(Decl);
				return EStmtResult::Parsed;
			}

			/** `From -> To : Expr [Meta] ;` */
			EStmtResult ParseTransition(FDabStmtPtr& Out)
			{
				const int32 Start = Index;
				TUniquePtr<FDabTransitionStmt> Transition = MakeUnique<FDabTransitionStmt>();

				const FDabToken& From = Advance();
				Transition->From = From.Text;
				Transition->FromSpan = From.Span;
				Advance(); // ->

				if (!Check(EDabTokenKind::Identifier))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("the target state after '->'")));
					}
					return EStmtResult::Failed;
				}
				const FDabToken& To = Advance();
				Transition->To = To.Text;
				Transition->ToSpan = To.Span;

				if (!Match(EDabTokenKind::Colon))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span,
							Expected(FString::Printf(TEXT("':' and a condition after '%s -> %s'"), *Transition->From, *Transition->To)));
					}
					return EStmtResult::Failed;
				}

				Transition->Condition = ParseExpression();
				if (!Transition->Condition)
				{
					return EStmtResult::Failed;
				}

				if (Check(EDabTokenKind::LeftBracket))
				{
					Transition->Meta = ParseMeta();
					if (!Transition->Meta)
					{
						return EStmtResult::Failed;
					}
				}

				if (!Match(EDabTokenKind::Semicolon))
				{
					ReportMissingSemicolon(FString::Printf(TEXT("';' after the transition '%s -> %s'"), *Transition->From, *Transition->To));
				}

				Transition->Span = SpanFrom(Start);
				Out = MoveTemp(Transition);
				return EStmtResult::Parsed;
			}

			/**
			 * `Word Name [(Params)] [-> (Outputs)] [Meta]` then `{ ... } [;]`, `= Expr ;` or `;`. A Function takes
			 * typed parameters and outputs, a Template plain parameters; a Detached block takes no name.
			 */
			EStmtResult ParseNamedBlock(FDabStmtPtr& Out)
			{
				const int32 Start = Index;
				TUniquePtr<FDabNamedBlockStmt> Named = MakeUnique<FDabNamedBlockStmt>();

				const FDabToken& Word = Advance();
				Named->Word = Word.Text;
				Named->WordSpan = Word.Span;
				// The callers have seen a name after the word, except for `Detached` followed by `{` or `[`.
				if (Check(EDabTokenKind::Identifier))
				{
					const FDabToken& Name = Advance();
					Named->Name = Name.Text;
					Named->NameSpan = Name.Span;
				}
				const bool bIsFunction = bControlRig && IsWord(Word, TEXT("Function"));

				if (Check(EDabTokenKind::LeftParen))
				{
					if (Word.IsIdentifier(TEXT("Template")))
					{
						Named->bHasParams = true;
						if (!ParseParams(Named->Params))
						{
							return EStmtResult::Failed;
						}
					}
					else if (bIsFunction)
					{
						Named->bHasParams = true;
						if (!ParseTypedParams(Named->Params, true, TEXT("parameter")))
						{
							return EStmtResult::Failed;
						}
					}
					else
					{
						// Reported, then stepped over, so the rest of the block still parses as written.
						Sink.Error(TEXT("DAB2001"), Current().Span,
							Expected(FString::Printf(TEXT("'{', '=' or ';' after '%s %s'"), *Named->Word, *Named->Name)),
							bControlRig ? TEXT("Only a Template or a Function takes parameters.") : TEXT("Only a Template takes parameters."));
						SkipBalancedParens();
					}
				}

				if (bIsFunction && Check(EDabTokenKind::Arrow))
				{
					Advance(); // ->
					if (!Check(EDabTokenKind::LeftParen))
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("'(' and the outputs after '->'")),
								TEXT("Write the outputs as -> (Type Name, ...)."));
						}
						return EStmtResult::Failed;
					}
					Named->bHasOutputs = true;
					if (!ParseTypedParams(Named->Outputs, false, TEXT("output")))
					{
						return EStmtResult::Failed;
					}
				}

				if (Check(EDabTokenKind::LeftBracket))
				{
					Named->Meta = ParseMeta();
					if (!Named->Meta)
					{
						return EStmtResult::Failed;
					}
				}

				if (Check(EDabTokenKind::LeftBrace))
				{
					// The body is a block whatever its first entry looks like; for Event, Function and Detached it is code.
					if (bControlRig && IsCodeBlockWord(Word))
					{
						Named->Code = ParseCodeBlock();
					}
					else
					{
						Named->Body = ParseBlockExpr();
					}
					Match(EDabTokenKind::Semicolon);
				}
				else if (Match(EDabTokenKind::Assign))
				{
					{
						// The value of a function is code too: `[` after an expression is an index there.
						const FCodeScope ValueScope(*this, bIsFunction);
						Named->Value = ParseExpression();
					}
					if (!Named->Value)
					{
						return EStmtResult::Failed;
					}
					if (!Match(EDabTokenKind::Semicolon))
					{
						ReportMissingSemicolon(FString::Printf(TEXT("';' after the value of '%s %s'"), *Named->Word, *Named->Name),
							Check(EDabTokenKind::LeftBracket)
								? FString::Printf(TEXT("A named block's metadata goes after its name: %s %s [ ... ] = ...;"), *Named->Word, *Named->Name)
								: FString());
					}
				}
				else if (!Match(EDabTokenKind::Semicolon))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span,
							Expected(FString::Printf(TEXT("'{', '=' or ';' after '%s %s'"), *Named->Word, *Named->Name)));
					}
					return EStmtResult::Failed;
				}

				Named->Span = SpanFrom(Start);
				Out = MoveTemp(Named);
				return EStmtResult::Parsed;
			}

			/** At the `(` of a template: `Param [= Default] { , Param [= Default] }` then `)`. */
			bool ParseParams(TArray<TUniquePtr<FDabParam>>& OutParams)
			{
				const FDabToken& Open = Advance();
				FBracketScope Scope(*this, Open, TEXT("("), TEXT(")"));

				if (Match(EDabTokenKind::RightParen))
				{
					return true;
				}

				bool bSawDefault = false;
				while (true)
				{
					if (!Check(EDabTokenKind::Identifier))
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("a parameter name")));
						}
						return false;
					}

					const int32 ParamStart = Index;
					TUniquePtr<FDabParam> Param = MakeUnique<FDabParam>();
					const FDabToken& NameToken = Advance();
					Param->Name = NameToken.Text;
					Param->NameSpan = NameToken.Span;

					if (Match(EDabTokenKind::Assign))
					{
						Param->Default = ParseExpression();
						if (!Param->Default)
						{
							return false;
						}
					}
					Param->Span = SpanFrom(ParamStart);

					bool bDuplicate = false;
					for (const TUniquePtr<FDabParam>& Earlier : OutParams)
					{
						if (Earlier->Name.Equals(Param->Name, ESearchCase::CaseSensitive))
						{
							bDuplicate = true;
							break;
						}
					}
					if (bDuplicate)
					{
						Sink.Error(TEXT("DAB2014"), Param->NameSpan,
							FString::Printf(TEXT("The parameter '%s' is declared twice."), *Param->Name));
					}
					else if (!Param->Default && bSawDefault)
					{
						Sink.Error(TEXT("DAB2014"), Param->NameSpan,
							FString::Printf(TEXT("The parameter '%s' has no default, but a parameter before it has one."), *Param->Name),
							TEXT("Parameters with defaults come last."));
					}
					bSawDefault |= Param->Default.IsValid();

					const FString ParamName = Param->Name;
					OutParams.Add(MoveTemp(Param));

					if (Match(EDabTokenKind::Comma))
					{
						continue;
					}
					if (Match(EDabTokenKind::RightParen))
					{
						return true;
					}
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span,
							Expected(FString::Printf(TEXT("',' or ')' after the parameter '%s'"), *ParamName)));
					}
					return false;
				}
			}

			/**
			 * At the `(` of a function: `Type Name [= Default] { , Type Name [= Default] }` then `)`. The outputs
			 * (`-> (Type Name, ...)`) take no defaults. What is "parameter" or "output", for the messages.
			 */
			bool ParseTypedParams(TArray<TUniquePtr<FDabParam>>& OutParams, const bool bAllowDefaults, const TCHAR* What)
			{
				const FDabToken& Open = Advance();
				FBracketScope Scope(*this, Open, TEXT("("), TEXT(")"));

				if (Match(EDabTokenKind::RightParen))
				{
					return true;
				}

				while (true)
				{
					const int32 ParamStart = Index;
					TUniquePtr<FDabParam> Param = MakeUnique<FDabParam>();
					if (!ParseTypeName(Param->TypeName, Param->TypeSpan, false))
					{
						return false;
					}
					if (!Check(EDabTokenKind::Identifier))
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DAB2001"), Current().Span,
								Expected(FString::Printf(TEXT("the name of the %s after its type '%s'"), What, *Param->TypeName)));
						}
						return false;
					}
					const FDabToken& NameToken = Advance();
					Param->Name = NameToken.Text;
					Param->NameSpan = NameToken.Span;

					if (bAllowDefaults && Match(EDabTokenKind::Assign))
					{
						Param->Default = ParseExpression();
						if (!Param->Default)
						{
							return false;
						}
					}
					Param->Span = SpanFrom(ParamStart);

					for (const TUniquePtr<FDabParam>& Earlier : OutParams)
					{
						if (Earlier->Name.Equals(Param->Name, ESearchCase::CaseSensitive))
						{
							Sink.Error(TEXT("DAB2014"), Param->NameSpan,
								FString::Printf(TEXT("The %s '%s' is declared twice."), What, *Param->Name));
							break;
						}
					}

					const FString ParamName = Param->Name;
					OutParams.Add(MoveTemp(Param));

					if (Match(EDabTokenKind::Comma))
					{
						continue;
					}
					if (Match(EDabTokenKind::RightParen))
					{
						return true;
					}
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span,
							Expected(FString::Printf(TEXT("',' or ')' after the %s '%s'"), What, *ParamName)));
					}
					return false;
				}
			}

			/**
			 * At an identifier: a type name. In a Control Rig document it may have type arguments (`Array<double>`,
			 * nested too), and the text keeps them without spaces. A part written in backticks keeps its backticks
			 * inside a generic type, so the text can be printed as it is. False, after reporting, when no type starts here.
			 */
			bool ParseTypeName(FString& OutText, FDabSpan& OutSpan, const bool bNested, const int32 Depth = 0)
			{
				const int32 Start = Index;
				if (!Check(EDabTokenKind::Identifier))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("a type name")));
					}
					return false;
				}

				const FDabToken& Head = Advance();
				const bool bGeneric = bControlRig && Check(EDabTokenKind::Less);
				OutText = (Head.bQuoted && (bNested || bGeneric)) ? FString::Printf(TEXT("`%s`"), *Head.Text) : Head.Text;
				if (!bGeneric)
				{
					OutSpan = Head.Span;
					return true;
				}

				if (Depth >= MaxTypeDepth)
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("a type with fewer type arguments inside each other")));
					}
					return false;
				}

				Advance(); // <
				OutText += TEXT("<");
				while (true)
				{
					FString Inner;
					FDabSpan InnerSpan;
					if (!ParseTypeName(Inner, InnerSpan, true, Depth + 1))
					{
						return false;
					}
					OutText += Inner;
					if (!Match(EDabTokenKind::Comma))
					{
						break;
					}
					OutText += TEXT(",");
				}

				if (!Check(EDabTokenKind::Greater))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(FString::Printf(TEXT("'>' to close the type '%s'"), *OutText)));
					}
					return false;
				}
				Advance(); // >
				OutText += TEXT(">");
				OutSpan = SpanFrom(Start);
				return true;
			}

			/** At a `[` after a value: `Key [= Expr]` entries separated by `;` or `,`, a trailing one allowed. */
			TUniquePtr<FDabMeta> ParseMeta()
			{
				const int32 Start = Index;
				const FDabToken& Open = Advance();
				FBracketScope Scope(*this, Open, TEXT("["), TEXT("]"));

				TUniquePtr<FDabMeta> Meta = MakeUnique<FDabMeta>();
				while (!Check(EDabTokenKind::RightBracket))
				{
					if (!Check(EDabTokenKind::Identifier) && !Check(EDabTokenKind::String))
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DAB2013"), Current().Span, Expected(TEXT("a metadata key")),
								TEXT("Metadata is written as [ Key = Value; Flag ]."));
						}
						// One report per metadata list; the rest of it is skipped.
						if (!SkipToMetaEnd())
						{
							return nullptr;
						}
						break;
					}

					const int32 EntryStart = Index;
					TUniquePtr<FDabMetaEntry> Entry = MakeUnique<FDabMetaEntry>();
					const FDabToken& Key = Advance();
					Entry->Key = Key.Text;
					Entry->KeySpan = Key.Span;
					Entry->bKeyIsString = Key.Is(EDabTokenKind::String);

					bool bValueFailed = false;
					if (Match(EDabTokenKind::Assign))
					{
						Entry->Value = ParseExpression();
						bValueFailed = !Entry->Value;
					}
					Entry->Span = SpanFrom(EntryStart);

					if (bValueFailed)
					{
						// The expression reported; resume at the `]` instead of letting the statement's
						// recovery stop at a `;` that separates metadata entries.
						if (!SkipToMetaEnd())
						{
							return nullptr;
						}
						break;
					}

					const FString KeyText = Entry->Key;
					Meta->Entries.Add(MoveTemp(Entry));

					if (Match(EDabTokenKind::Semicolon) || Match(EDabTokenKind::Comma))
					{
						continue;
					}
					if (Check(EDabTokenKind::RightBracket))
					{
						break;
					}

					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2013"), Current().Span,
							Expected(FString::Printf(TEXT("';', ',' or ']' after the metadata entry '%s'"), *KeyText)));
					}
					if (!SkipToMetaEnd())
					{
						return nullptr;
					}
					break;
				}

				Advance(); // ]
				Meta->Span = SpanFrom(Start);
				return Meta;
			}

			// ------------------------------------------------------------------------------------
			// Code blocks (Control Rig documents)
			// ------------------------------------------------------------------------------------

			/** At a `{` that opens code. Returns the block even when the file ends inside it. */
			TUniquePtr<FDabCodeBlock> ParseCodeBlock()
			{
				const int32 Start = Index;
				const FDabToken& Open = Advance();
				FBracketScope Scope(*this, Open, TEXT("{"), TEXT("}"));
				const FCodeScope CodeScope(*this);

				TUniquePtr<FDabCodeBlock> Block = MakeUnique<FDabCodeBlock>();
				TArray<int32> Carry;
				TArray<const FDabRegionStmt*> OpenRegions;

				while (!Check(EDabTokenKind::RightBrace) && !AtEnd())
				{
					const int32 StatementStart = Index;
					FAnchorStart Anchor = BeginAnchor(Carry);

					FDabStmtPtr Statement;
					if (ParseCodeStatement(Statement) == EStmtResult::Failed)
					{
						SkipToStatementBoundary();
					}

					EndAnchor(MoveTemp(Anchor), Statement.Get(), Carry);

					if (Statement)
					{
						NoteRegion(*Statement, OpenRegions);
						Block->Statements.Add(MoveTemp(Statement));
					}

					if (Index == StatementStart && !AtEnd() && !Check(EDabTokenKind::RightBrace))
					{
						Advance();
					}
				}

				WarnUnclosedRegions(OpenRegions);

				if (Check(EDabTokenKind::RightBrace))
				{
					ClaimInner(Open.Span.Offset, Carry);
					Advance();
				}
				else
				{
					ReportUnclosedAtEnd();
					ReleaseCarry(Carry);
				}

				Block->Span = SpanFrom(Start);
				return Block;
			}

			/**
			 * One statement of code. The words `let`, `local`, `if`, `else`, `for`, `return` and `Detached` start a
			 * statement of their own kind wherever they begin one, unless they are written in backticks; `Type Name`
			 * (a type, perhaps generic) starts a typed let; anything else is an expression, an assignment when a `=` follows.
			 */
			EStmtResult ParseCodeStatement(FDabStmtPtr& Out)
			{
				const FDabToken& First = Current();

				if (First.Is(EDabTokenKind::Directive))
				{
					return ParseDirectiveStatement(Out);
				}
				if (IsWord(First, TEXT("let")))
				{
					return ParseLet(Out, EDabLetKind::Let);
				}
				if (IsWord(First, TEXT("local")))
				{
					return ParseLet(Out, EDabLetKind::Local);
				}
				if (IsWord(First, TEXT("if")))
				{
					return ParseIf(Out);
				}
				if (IsWord(First, TEXT("else")))
				{
					return ParseStrayElse();
				}
				if (IsWord(First, TEXT("for")))
				{
					return ParseFor(Out);
				}
				if (IsWord(First, TEXT("return")))
				{
					return ParseReturn(Out);
				}
				if (IsWord(First, TEXT("Detached")))
				{
					return ParseDetached(Out);
				}

				const int32 TypeEnd = ScanTypeEnd(0);
				if (TypeEnd != INDEX_NONE && Peek(TypeEnd).Is(EDabTokenKind::Identifier))
				{
					return ParseTypedLet(Out);
				}
				return ParseSimpleCodeStatement(Out);
			}

			/** `let Name = Expr;` or `local Type Name [= Expr];`, at the word. */
			EStmtResult ParseLet(FDabStmtPtr& Out, const EDabLetKind Kind)
			{
				const int32 Start = Index;
				const FDabToken& Keyword = Advance();
				TUniquePtr<FDabLetStmt> Let = MakeUnique<FDabLetStmt>();
				Let->LetKind = Kind;

				if (Kind == EDabLetKind::Local && !ParseTypeName(Let->TypeName, Let->TypeSpan, false))
				{
					return EStmtResult::Failed;
				}
				if (!Check(EDabTokenKind::Identifier))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span,
							Expected(Kind == EDabLetKind::Local
								? FString::Printf(TEXT("a name after the type '%s'"), *Let->TypeName)
								: FString::Printf(TEXT("a name after '%s'"), *Keyword.Text)));
					}
					return EStmtResult::Failed;
				}
				const FDabToken& Name = Advance();
				Let->Name = Name.Text;
				Let->NameSpan = Name.Span;

				if (Match(EDabTokenKind::Assign))
				{
					Let->Value = ParseExpression();
					if (!Let->Value)
					{
						return EStmtResult::Failed;
					}
				}
				else if (Kind == EDabLetKind::Let)
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(FString::Printf(TEXT("'=' after 'let %s'"), *Let->Name)),
							TEXT("Write let Name = value;."));
					}
					return EStmtResult::Failed;
				}

				if (!Match(EDabTokenKind::Semicolon))
				{
					ReportMissingSemicolon(FString::Printf(TEXT("';' after the declaration of '%s'"), *Let->Name));
				}

				Let->Span = SpanFrom(Start);
				Out = MoveTemp(Let);
				return EStmtResult::Parsed;
			}

			/** `Type Name = Expr;`: a let whose type is written. */
			EStmtResult ParseTypedLet(FDabStmtPtr& Out)
			{
				const int32 Start = Index;
				TUniquePtr<FDabLetStmt> Let = MakeUnique<FDabLetStmt>();
				Let->LetKind = EDabLetKind::Typed;

				if (!ParseTypeName(Let->TypeName, Let->TypeSpan, false))
				{
					return EStmtResult::Failed;
				}
				const FDabToken& Name = Advance();
				Let->Name = Name.Text;
				Let->NameSpan = Name.Span;

				if (!Match(EDabTokenKind::Assign))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(FString::Printf(TEXT("'=' after '%s %s'"), *Let->TypeName, *Let->Name)),
							TEXT("Write let Name = value; to name a value, or local Type Name; to declare a variable."));
					}
					return EStmtResult::Failed;
				}
				Let->Value = ParseExpression();
				if (!Let->Value)
				{
					return EStmtResult::Failed;
				}

				if (!Match(EDabTokenKind::Semicolon))
				{
					ReportMissingSemicolon(FString::Printf(TEXT("';' after the declaration of '%s'"), *Let->Name));
				}

				Let->Span = SpanFrom(Start);
				Out = MoveTemp(Let);
				return EStmtResult::Parsed;
			}

			/** `Target = Expr;` or `Expr;`. */
			EStmtResult ParseSimpleCodeStatement(FDabStmtPtr& Out)
			{
				const int32 Start = Index;
				if (!CanStartExpression(Current()))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("a statement")));
					}
					return EStmtResult::Failed;
				}

				FDabExprPtr Expr = ParseExpression();
				if (!Expr)
				{
					return EStmtResult::Failed;
				}

				if (Match(EDabTokenKind::Assign))
				{
					TUniquePtr<FDabSetStmt> Set = MakeUnique<FDabSetStmt>();
					Set->Target = MoveTemp(Expr);
					Set->Value = ParseExpression();
					if (!Set->Value)
					{
						return EStmtResult::Failed;
					}
					if (!Match(EDabTokenKind::Semicolon))
					{
						ReportMissingSemicolon(TEXT("';' after the assignment"));
					}
					Set->Span = SpanFrom(Start);
					Out = MoveTemp(Set);
					return EStmtResult::Parsed;
				}

				TUniquePtr<FDabExprStmt> ExprStmt = MakeUnique<FDabExprStmt>();
				ExprStmt->Expr = MoveTemp(Expr);
				if (!Match(EDabTokenKind::Semicolon))
				{
					ReportMissingSemicolon(TEXT("';' after the statement"));
				}
				ExprStmt->Span = SpanFrom(Start);
				Out = MoveTemp(ExprStmt);
				return EStmtResult::Parsed;
			}

			EStmtResult ParseIf(FDabStmtPtr& Out)
			{
				TUniquePtr<FDabIfStmt> If;
				if (!ParseIfChain(If))
				{
					return EStmtResult::Failed;
				}
				Out = MoveTemp(If);
				return EStmtResult::Parsed;
			}

			/** At `if`: the condition, the block, and the `else if` / `else` after it. False, after reporting, when it cannot go on. */
			bool ParseIfChain(TUniquePtr<FDabIfStmt>& OutIf)
			{
				const int32 Start = Index;
				Advance(); // if
				TUniquePtr<FDabIfStmt> If = MakeUnique<FDabIfStmt>();

				if (!Check(EDabTokenKind::LeftParen))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("'(' after 'if'")), TEXT("Write if (condition) { ... }."));
					}
					return false;
				}
				{
					const FDabToken& Open = Advance();
					FBracketScope Scope(*this, Open, TEXT("("), TEXT(")"));
					If->Condition = ParseExpression();
					if (!If->Condition)
					{
						return false;
					}
					if (!Check(EDabTokenKind::RightParen))
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("')' after the condition")));
						}
						return false;
					}
					Advance(); // )
				}

				if (!Check(EDabTokenKind::LeftBrace))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("'{' to open the body of the 'if'")));
					}
					return false;
				}
				If->Then = ParseCodeBlock();

				if (IsWord(Current(), TEXT("else")))
				{
					If->ElseSpan = Advance().Span;
					if (IsWord(Current(), TEXT("if")))
					{
						if (!ParseIfChain(If->ElseIf))
						{
							return false;
						}
					}
					else if (Check(EDabTokenKind::LeftBrace))
					{
						If->Else = ParseCodeBlock();
					}
					else
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("'if' or '{' after 'else'")));
						}
						return false;
					}
				}

				If->Span = SpanFrom(Start);
				OutIf = MoveTemp(If);
				return true;
			}

			/** An `else` that follows no `if`: DAB2016. The branch it opens is parsed and dropped so it does not report again. */
			EStmtResult ParseStrayElse()
			{
				const FDabToken& Else = Advance();
				Sink.Error(TEXT("DAB2016"), Else.Span, TEXT("'else' has no 'if' before it."),
					TEXT("An 'else' follows the '}' of an 'if' or of an 'else if'."));

				if (IsWord(Current(), TEXT("if")))
				{
					TUniquePtr<FDabIfStmt> Ignored;
					ParseIfChain(Ignored);
				}
				else if (Check(EDabTokenKind::LeftBrace))
				{
					ParseCodeBlock();
				}
				return EStmtResult::Skipped;
			}

			/** `for (Name [, Name [, Name [, Name]]] in Expr) { ... }`. */
			EStmtResult ParseFor(FDabStmtPtr& Out)
			{
				const int32 Start = Index;
				Advance(); // for
				TUniquePtr<FDabForStmt> For = MakeUnique<FDabForStmt>();

				if (!Check(EDabTokenKind::LeftParen))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("'(' after 'for'")),
							TEXT("Write for (Element in Items) { ... }."));
					}
					return EStmtResult::Failed;
				}

				{
					const FDabToken& Open = Advance();
					FBracketScope Scope(*this, Open, TEXT("("), TEXT(")"));

					while (true)
					{
						if (!Check(EDabTokenKind::Identifier) || IsWord(Current(), TEXT("in")))
						{
							if (ShouldReportAtCurrent())
							{
								Sink.Error(TEXT("DAB2017"), Current().Span,
									Expected(For->Names.Num() == 0 ? FString(TEXT("a name for each element of the loop")) : FString(TEXT("a name after ','"))),
									TEXT("Write for (Element, Index, Count, Ratio in Items): one to four names, then 'in'."));
							}
							return EStmtResult::Failed;
						}
						const FDabToken& Name = Advance();
						For->Names.Add(Name.Text);
						For->NameSpans.Add(Name.Span);
						if (!Match(EDabTokenKind::Comma))
						{
							break;
						}
					}

					if (For->Names.Num() > 4)
					{
						Sink.Error(TEXT("DAB2017"), For->NameSpans[4], TEXT("A for loop binds at most four names: Element, Index, Count and Ratio."));
					}

					if (!IsWord(Current(), TEXT("in")))
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DAB2017"), Current().Span, Expected(TEXT("'in' after the names of the loop")),
								TEXT("Write for (Element in Items) { ... }."));
						}
						return EStmtResult::Failed;
					}
					Advance(); // in

					For->Iterable = ParseExpression();
					if (!For->Iterable)
					{
						return EStmtResult::Failed;
					}
					if (!Check(EDabTokenKind::RightParen))
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("')' after the array to loop over")));
						}
						return EStmtResult::Failed;
					}
					Advance(); // )
				}

				if (!Check(EDabTokenKind::LeftBrace))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("'{' to open the body of the 'for'")));
					}
					return EStmtResult::Failed;
				}
				For->Body = ParseCodeBlock();

				For->Span = SpanFrom(Start);
				Out = MoveTemp(For);
				return EStmtResult::Parsed;
			}

			/** `return;` or `return (Name = Expr, ...);`. */
			EStmtResult ParseReturn(FDabStmtPtr& Out)
			{
				const int32 Start = Index;
				Advance(); // return
				TUniquePtr<FDabReturnStmt> Return = MakeUnique<FDabReturnStmt>();

				if (Check(EDabTokenKind::LeftParen))
				{
					Return->Value = ParseParenOrTuple();
					if (!Return->Value)
					{
						return EStmtResult::Failed;
					}
					CheckReturnValues(*Return->Value);
				}
				else if (!Check(EDabTokenKind::Semicolon))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("'(' or ';' after 'return'")),
							TEXT("Write return (Name = value, ...); to give the outputs, or return; for none."));
					}
					return EStmtResult::Failed;
				}

				if (!Match(EDabTokenKind::Semicolon))
				{
					ReportMissingSemicolon(TEXT("';' after 'return'"));
				}

				Return->Span = SpanFrom(Start);
				Out = MoveTemp(Return);
				return EStmtResult::Parsed;
			}

			/** DAB2018: a return gives its outputs by name, so what follows `return` is a tuple of named items. */
			void CheckReturnValues(const FDabExpr& Value)
			{
				const TCHAR* const Hint = TEXT("Write return (Name = value, ...); with the name of each output.");
				const FDabTupleExpr* Tuple = Value.As<FDabTupleExpr>();
				if (!Tuple)
				{
					Sink.Error(TEXT("DAB2018"), Value.Span, TEXT("A return gives each output by name."), Hint);
					return;
				}
				for (const FDabArgumentPtr& Item : Tuple->Items)
				{
					if (Item && !Item->IsNamed())
					{
						Sink.Error(TEXT("DAB2018"), Item->Span, TEXT("A return gives each output by name."), Hint);
					}
				}
			}

			/** `Detached { ... }` in code. */
			EStmtResult ParseDetached(FDabStmtPtr& Out)
			{
				const int32 Start = Index;
				Advance(); // Detached
				if (!Check(EDabTokenKind::LeftBrace))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("'{' after 'Detached'")));
					}
					return EStmtResult::Failed;
				}

				TUniquePtr<FDabDetachedStmt> Detached = MakeUnique<FDabDetachedStmt>();
				Detached->Body = ParseCodeBlock();
				Detached->Span = SpanFrom(Start);
				Out = MoveTemp(Detached);
				return EStmtResult::Parsed;
			}

			/** At a `[` in code: `[ Key = ...`, `[ Key;` and `[ Key,` can only be metadata; `[ Key ]` is an index like any other. */
			bool LooksLikeMetadataAtBracket() const
			{
				const FDabToken& Key = Peek(1);
				if (!Key.Is(EDabTokenKind::Identifier) && !Key.Is(EDabTokenKind::String))
				{
					return false;
				}
				const FDabToken& After = Peek(2);
				return After.Is(EDabTokenKind::Assign) || After.Is(EDabTokenKind::Semicolon) || After.Is(EDabTokenKind::Comma);
			}

			// ------------------------------------------------------------------------------------
			// Arguments and lists
			// ------------------------------------------------------------------------------------

			/** At a `(`: arguments through `)`. False, after reporting, when the list could not be closed. */
			bool ParseArgumentList(TArray<FDabArgumentPtr>& OutArguments, bool& bOutTrailingComma, const bool bDocumentHeader = false)
			{
				const FDabToken& Open = Advance();
				FBracketScope Scope(*this, Open, TEXT("("), TEXT(")"));
				TArray<int32> Carry;

				while (!Check(EDabTokenKind::RightParen))
				{
					if (AtEnd())
					{
						ReportUnclosedAtEnd();
						ReleaseCarry(Carry);
						return false;
					}

					FAnchorStart Anchor = BeginAnchor(Carry);
					FDabArgumentPtr Argument = ParseArgument();
					bool bRecover = !Argument;
					if (Argument && !Match(EDabTokenKind::Comma) && !Check(EDabTokenKind::RightParen))
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("',' or ')' after the argument")));
						}
						bRecover = true;
					}
					EndAnchor(MoveTemp(Anchor), Argument.Get(), Carry);
					if (Argument)
					{
						OutArguments.Add(MoveTemp(Argument));
					}

					if (bRecover && !SkipToListBoundary(EDabTokenKind::RightParen, bDocumentHeader))
					{
						ReleaseCarry(Carry);
						return false;
					}
				}

				bOutTrailingComma = OutArguments.Num() > 0 && Previous().Is(EDabTokenKind::Comma);
				ClaimInner(Open.Span.Offset, Carry);
				Advance(); // )

				CheckArgumentNames(OutArguments);
				return true;
			}

			/** `[Key =] Expr`. */
			FDabArgumentPtr ParseArgument()
			{
				const int32 Start = Index;
				FDabArgumentPtr Argument = MakeUnique<FDabArgument>();

				if ((Check(EDabTokenKind::Identifier) || Check(EDabTokenKind::String)) && Peek(1).Is(EDabTokenKind::Assign))
				{
					const FDabToken& Key = Advance();
					Argument->Name = Key.Text;
					Argument->NameSpan = Key.Span;
					Argument->bNameIsString = Key.Is(EDabTokenKind::String);
					Advance(); // =
				}

				Argument->Value = ParseExpression();
				if (!Argument->Value)
				{
					return nullptr;
				}
				Argument->Span = SpanFrom(Start);
				return Argument;
			}

			/** DAB2012 once per list, DAB2011 for every repeated name (case-sensitive, like every name). */
			void CheckArgumentNames(const TArray<FDabArgumentPtr>& Arguments)
			{
				bool bSawNamed = false;
				bool bReportedOrder = false;
				for (int32 ArgIndex = 0; ArgIndex < Arguments.Num(); ++ArgIndex)
				{
					const FDabArgument& Argument = *Arguments[ArgIndex];
					if (!Argument.IsNamed())
					{
						if (bSawNamed && !bReportedOrder)
						{
							Sink.Error(TEXT("DAB2012"), Argument.Span, TEXT("A positional argument cannot follow a named one."),
								TEXT("Put the positional arguments first, or name this one."));
							bReportedOrder = true;
						}
						continue;
					}

					bSawNamed = true;
					for (int32 EarlierIndex = 0; EarlierIndex < ArgIndex; ++EarlierIndex)
					{
						const FDabArgument& Earlier = *Arguments[EarlierIndex];
						if (Earlier.IsNamed() && Earlier.Name.Equals(Argument.Name, ESearchCase::CaseSensitive))
						{
							Sink.Error(TEXT("DAB2011"), Argument.NameSpan,
								FString::Printf(TEXT("The argument '%s' is given twice."), *Argument.Name));
							break;
						}
					}
				}
			}

			/** At a `(` in expression position: one unnamed item without a trailing comma is a grouping, anything else a tuple. */
			FDabExprPtr ParseParenOrTuple()
			{
				const int32 Start = Index;
				const int32 OpenOffset = Current().Span.Offset;

				TArray<FDabArgumentPtr> Items;
				bool bTrailingComma = false;
				if (!ParseArgumentList(Items, bTrailingComma))
				{
					return nullptr;
				}

				// `()` is the empty tuple: a struct with every field at its default.
				if (Items.Num() == 1 && !Items[0]->IsNamed() && !bTrailingComma)
				{
					ReleaseTrivia(Items[0]->Span.Offset, OpenOffset);
					TUniquePtr<FDabParenExpr> Paren = MakeUnique<FDabParenExpr>();
					Paren->Inner = MoveTemp(Items[0]->Value);
					Paren->Span = SpanFrom(Start);
					return MoveTemp(Paren);
				}

				TUniquePtr<FDabTupleExpr> Tuple = MakeUnique<FDabTupleExpr>();
				Tuple->Items = MoveTemp(Items);
				Tuple->Span = SpanFrom(Start);
				return MoveTemp(Tuple);
			}

			/** At a `[` in expression position. */
			FDabExprPtr ParseList()
			{
				const int32 Start = Index;
				const FDabToken& Open = Advance();
				FBracketScope Scope(*this, Open, TEXT("["), TEXT("]"));

				TUniquePtr<FDabListExpr> List = MakeUnique<FDabListExpr>();
				TArray<int32> Carry;

				while (!Check(EDabTokenKind::RightBracket))
				{
					if (AtEnd())
					{
						ReportUnclosedAtEnd();
						ReleaseCarry(Carry);
						return nullptr;
					}

					FAnchorStart Anchor = BeginAnchor(Carry);
					FDabExprPtr Item = ParseExpression();
					bool bRecover = !Item;
					if (Item && !Match(EDabTokenKind::Comma) && !Check(EDabTokenKind::RightBracket))
					{
						if (ShouldReportAtCurrent())
						{
							Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("',' or ']' after the list item")));
						}
						bRecover = true;
					}
					EndAnchor(MoveTemp(Anchor), Item.Get(), Carry);
					if (Item)
					{
						List->Items.Add(MoveTemp(Item));
					}

					if (bRecover && !SkipToListBoundary(EDabTokenKind::RightBracket, false))
					{
						ReleaseCarry(Carry);
						return nullptr;
					}
				}

				ClaimInner(Open.Span.Offset, Carry);
				Advance(); // ]
				List->Span = SpanFrom(Start);
				return MoveTemp(List);
			}

			// ------------------------------------------------------------------------------------
			// Expressions
			// ------------------------------------------------------------------------------------

			FDabExprPtr ParseExpression()
			{
				return ParseConditional();
			}

			/** `Or [ ? Expr : Expr ]`, right-associative through the branches. */
			FDabExprPtr ParseConditional()
			{
				const int32 Start = Index;
				FDabExprPtr Condition = ParseBinary(1);
				if (!Condition || !Check(EDabTokenKind::Question))
				{
					return Condition;
				}
				Advance(); // ?

				FDabExprPtr Then = ParseExpression();
				if (!Then)
				{
					return nullptr;
				}
				if (!Match(EDabTokenKind::Colon))
				{
					if (ShouldReportAtCurrent())
					{
						Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("':' in the conditional expression")));
					}
					return nullptr;
				}
				FDabExprPtr Else = ParseExpression();
				if (!Else)
				{
					return nullptr;
				}

				TUniquePtr<FDabConditionalExpr> Node = MakeUnique<FDabConditionalExpr>();
				Node->Condition = MoveTemp(Condition);
				Node->Then = MoveTemp(Then);
				Node->Else = MoveTemp(Else);
				Node->Span = SpanFrom(Start);
				return MoveTemp(Node);
			}

			/** Precedence climbing over GetBinaryOperator; every level is left-associative. */
			FDabExprPtr ParseBinary(const int32 MinPrecedence)
			{
				const int32 Start = Index;
				FDabExprPtr Left = ParseUnary();
				if (!Left)
				{
					return nullptr;
				}

				while (true)
				{
					EDabBinaryOp Op = EDabBinaryOp::Or;
					int32 Precedence = 0;
					if (!GetBinaryOperator(Current().Kind, Op, Precedence) || Precedence < MinPrecedence)
					{
						break;
					}

					const FDabToken& OpToken = Advance();
					FDabExprPtr Right = ParseBinary(Precedence + 1);
					if (!Right)
					{
						return nullptr;
					}

					TUniquePtr<FDabBinaryExpr> Node = MakeUnique<FDabBinaryExpr>();
					Node->Op = Op;
					Node->OpSpan = OpToken.Span;
					Node->Lhs = MoveTemp(Left);
					Node->Rhs = MoveTemp(Right);
					Node->Span = SpanFrom(Start);
					Left = MoveTemp(Node);
				}
				return Left;
			}

			FDabExprPtr ParseUnary()
			{
				if (!Check(EDabTokenKind::Bang) && !Check(EDabTokenKind::Minus))
				{
					return ParsePostfix();
				}

				const int32 Start = Index;
				const EDabUnaryOp Op = Check(EDabTokenKind::Bang) ? EDabUnaryOp::Not : EDabUnaryOp::Negate;
				Advance();

				FDabExprPtr Operand = ParseUnary();
				if (!Operand)
				{
					return nullptr;
				}

				TUniquePtr<FDabUnaryExpr> Node = MakeUnique<FDabUnaryExpr>();
				Node->Op = Op;
				Node->Operand = MoveTemp(Operand);
				Node->Span = SpanFrom(Start);
				return MoveTemp(Node);
			}

			/**
			 * `Primary { . Ident | ( Args ) | [ Expr ] }`. A `[` is postfix only inside code; anywhere else, after a
			 * value, it opens metadata.
			 */
			FDabExprPtr ParsePostfix()
			{
				const int32 Start = Index;
				FDabExprPtr Expr = ParsePrimary();
				if (!Expr)
				{
					return nullptr;
				}

				while (true)
				{
					if (Check(EDabTokenKind::Dot))
					{
						Advance();
						if (!Check(EDabTokenKind::Identifier))
						{
							if (ShouldReportAtCurrent())
							{
								Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("a member name after '.'")));
							}
							return nullptr;
						}
						const FDabToken& Member = Advance();
						TUniquePtr<FDabMemberExpr> Node = MakeUnique<FDabMemberExpr>();
						Node->Object = MoveTemp(Expr);
						Node->Member = Member.Text;
						Node->MemberSpan = Member.Span;
						Node->Span = SpanFrom(Start);
						Expr = MoveTemp(Node);
					}
					else if (Check(EDabTokenKind::LeftParen))
					{
						const int32 OpenIndex = Index;
						TUniquePtr<FDabCallExpr> Call = MakeUnique<FDabCallExpr>();
						Call->Callee = MoveTemp(Expr);
						bool bTrailingComma = false;
						if (!ParseArgumentList(Call->Args, bTrailingComma))
						{
							return nullptr;
						}
						Call->ArgsSpan = SpanFrom(OpenIndex);
						Call->Span = SpanFrom(Start);
						Expr = MoveTemp(Call);
					}
					else if (CodeDepth > 0 && Check(EDabTokenKind::LeftBracket))
					{
						if (LooksLikeMetadataAtBracket())
						{
							// Code statements take no metadata. Reported once, the bracket skipped, and the statement goes on.
							Sink.Error(TEXT("DAB2015"), Current().Span,
								TEXT("A code statement takes no metadata; a '[' after an expression is an index here."),
								TEXT("Put the metadata on the Event or Function, or on the variable's declaration."));
							SkipBracketGroup();
							continue;
						}

						const FDabToken& Open = Advance();
						FBracketScope Scope(*this, Open, TEXT("["), TEXT("]"));
						TUniquePtr<FDabIndexExpr> Node = MakeUnique<FDabIndexExpr>();
						Node->Object = MoveTemp(Expr);
						Node->Index = ParseExpression();
						if (!Node->Index)
						{
							return nullptr;
						}
						if (!Check(EDabTokenKind::RightBracket))
						{
							if (ShouldReportAtCurrent())
							{
								Sink.Error(TEXT("DAB2001"), Current().Span, Expected(TEXT("']' after the index")));
							}
							return nullptr;
						}
						Advance(); // ]
						Node->Span = SpanFrom(Start);
						Expr = MoveTemp(Node);
					}
					else
					{
						break;
					}
				}
				return Expr;
			}

			FDabExprPtr ParsePrimary()
			{
				const FDabToken& Token = Current();
				switch (Token.Kind)
				{
				case EDabTokenKind::Number:
				{
					TUniquePtr<FDabLiteralExpr> Literal = MakeUnique<FDabLiteralExpr>();
					Literal->LiteralKind = EDabLiteralKind::Number;
					Literal->Text = Token.Text;
					Literal->Number = Token.Number;
					Literal->Span = Token.Span;
					Advance();
					return MoveTemp(Literal);
				}
				case EDabTokenKind::String:
				{
					TUniquePtr<FDabLiteralExpr> Literal = MakeUnique<FDabLiteralExpr>();
					Literal->LiteralKind = EDabLiteralKind::String;
					Literal->Text = Token.Text;
					Literal->Span = Token.Span;
					Advance();
					return MoveTemp(Literal);
				}
				case EDabTokenKind::Identifier:
				{
					// In backticks `true` is a name like any other.
					if (!Token.bQuoted && (Token.IsIdentifier(TEXT("true")) || Token.IsIdentifier(TEXT("false"))))
					{
						TUniquePtr<FDabLiteralExpr> Literal = MakeUnique<FDabLiteralExpr>();
						Literal->LiteralKind = EDabLiteralKind::Bool;
						Literal->Text = Token.Text;
						Literal->bBool = Token.IsIdentifier(TEXT("true"));
						Literal->Span = Token.Span;
						Advance();
						return MoveTemp(Literal);
					}
					TUniquePtr<FDabNameExpr> Name = MakeUnique<FDabNameExpr>();
					Name->Name = Token.Text;
					Name->Span = Token.Span;
					Advance();
					return MoveTemp(Name);
				}
				case EDabTokenKind::LeftParen:
					return ParseParenOrTuple();
				case EDabTokenKind::LeftBracket:
					return ParseList();
				case EDabTokenKind::LeftBrace:
					return ParseBraced();
				default:
					break;
				}

				if (ShouldReportAtCurrent())
				{
					Sink.Error(TEXT("DAB2006"), Token.Span, Expected(TEXT("an expression")));
				}
				return nullptr;
			}

			// ------------------------------------------------------------------------------------
			// Trivia collection
			// ------------------------------------------------------------------------------------

			FDabComment MakeComment(const int32 CommentIndex) const
			{
				const FDabToken& Token = Comments[CommentIndex];
				FDabComment Comment;
				Comment.Text = Token.Text;
				Comment.Span = Token.Span;
				Comment.bBlock = Token.Text.StartsWith(TEXT("/*"), ESearchCase::CaseSensitive);
				return Comment;
			}

			void TakeAnchorTrivia(FDabFile& File, const FDabNode* Node)
			{
				FPendingTrivia Pending;
				if (!Node || !PendingAnchors.RemoveAndCopyValue(Node->Span.Offset, Pending))
				{
					return;
				}
				FDabTrivia& Trivia = File.Trivia.FindOrAdd(Node);
				for (const int32 CommentIndex : Pending.Leading)
				{
					Trivia.Leading.Add(MakeComment(CommentIndex));
				}
				Trivia.BlankLinesBefore = Pending.BlankLinesBefore;
				if (Pending.Trailing != INDEX_NONE)
				{
					Trivia.Trailing.Emplace(MakeComment(Pending.Trailing));
				}
				for (const int32 CommentIndex : Pending.Inner)
				{
					Trivia.Inner.Add(MakeComment(CommentIndex));
				}
			}

			void TakeInnerTrivia(FDabFile& File, const FDabNode* Node, const int32 OpenOffset)
			{
				TArray<int32> Inner;
				if (!PendingInner.RemoveAndCopyValue(OpenOffset, Inner))
				{
					return;
				}
				FDabTrivia& Trivia = File.Trivia.FindOrAdd(Node);
				for (const int32 CommentIndex : Inner)
				{
					Trivia.Inner.Add(MakeComment(CommentIndex));
				}
			}

			/** Moves pending trivia onto the nodes of the finished tree, in the roles each one plays there. */
			void CollectTrivia(FDabFile& File)
			{
				if (!bKeepTrivia)
				{
					return;
				}
				for (const TUniquePtr<FDabInclude>& Include : File.Includes)
				{
					TakeAnchorTrivia(File, Include.Get());
				}
				if (const FDabDocument* Document = File.Document.Get())
				{
					TakeAnchorTrivia(File, Document);
					for (const FDabArgumentPtr& Argument : Document->HeaderArgs)
					{
						TakeAnchorTrivia(File, Argument.Get());
						WalkExpr(File, Argument->Value.Get());
					}
					if (Document->Body)
					{
						WalkBlock(File, *Document->Body);
					}
				}
			}

			void WalkBlock(FDabFile& File, const FDabBlockExpr& Block)
			{
				TakeInnerTrivia(File, &Block, Block.Span.Offset);
				for (const FDabStmtPtr& Statement : Block.Statements)
				{
					if (Statement)
					{
						TakeAnchorTrivia(File, Statement.Get());
						WalkStmt(File, *Statement);
					}
				}
			}

			void WalkCodeBlock(FDabFile& File, const FDabCodeBlock& Block)
			{
				TakeInnerTrivia(File, &Block, Block.Span.Offset);
				for (const FDabStmtPtr& Statement : Block.Statements)
				{
					if (Statement)
					{
						TakeAnchorTrivia(File, Statement.Get());
						WalkStmt(File, *Statement);
					}
				}
			}

			void WalkMeta(FDabFile& File, const FDabMeta* Meta)
			{
				if (!Meta)
				{
					return;
				}
				for (const TUniquePtr<FDabMetaEntry>& Entry : Meta->Entries)
				{
					WalkExpr(File, Entry->Value.Get());
				}
			}

			void WalkStmt(FDabFile& File, const FDabStmt& Statement)
			{
				switch (Statement.Kind)
				{
				case EDabNodeKind::AssignStmt:
				{
					const FDabAssignStmt& Assign = static_cast<const FDabAssignStmt&>(Statement);
					WalkExpr(File, Assign.Value.Get());
					WalkMeta(File, Assign.Meta.Get());
					break;
				}
				case EDabNodeKind::DeclStmt:
				{
					const FDabDeclStmt& Decl = static_cast<const FDabDeclStmt&>(Statement);
					WalkExpr(File, Decl.Value.Get());
					WalkMeta(File, Decl.Meta.Get());
					break;
				}
				case EDabNodeKind::TransitionStmt:
				{
					const FDabTransitionStmt& Transition = static_cast<const FDabTransitionStmt&>(Statement);
					WalkExpr(File, Transition.Condition.Get());
					WalkMeta(File, Transition.Meta.Get());
					break;
				}
				case EDabNodeKind::NamedBlockStmt:
				{
					const FDabNamedBlockStmt& Named = static_cast<const FDabNamedBlockStmt&>(Statement);
					for (const TUniquePtr<FDabParam>& Param : Named.Params)
					{
						WalkExpr(File, Param->Default.Get());
					}
					for (const TUniquePtr<FDabParam>& Output : Named.Outputs)
					{
						WalkExpr(File, Output->Default.Get());
					}
					WalkMeta(File, Named.Meta.Get());
					if (Named.Body)
					{
						WalkBlock(File, *Named.Body);
					}
					if (Named.Code)
					{
						WalkCodeBlock(File, *Named.Code);
					}
					WalkExpr(File, Named.Value.Get());
					break;
				}
				case EDabNodeKind::LetStmt:
					WalkExpr(File, static_cast<const FDabLetStmt&>(Statement).Value.Get());
					break;
				case EDabNodeKind::SetStmt:
				{
					const FDabSetStmt& Set = static_cast<const FDabSetStmt&>(Statement);
					WalkExpr(File, Set.Target.Get());
					WalkExpr(File, Set.Value.Get());
					break;
				}
				case EDabNodeKind::ExprStmt:
					WalkExpr(File, static_cast<const FDabExprStmt&>(Statement).Expr.Get());
					break;
				case EDabNodeKind::IfStmt:
				{
					// An `else if` is the chain's own node, not a child of the block: it has no trivia slot of its own.
					const FDabIfStmt& If = static_cast<const FDabIfStmt&>(Statement);
					WalkExpr(File, If.Condition.Get());
					if (If.Then)
					{
						WalkCodeBlock(File, *If.Then);
					}
					if (If.ElseIf)
					{
						WalkStmt(File, *If.ElseIf);
					}
					if (If.Else)
					{
						WalkCodeBlock(File, *If.Else);
					}
					break;
				}
				case EDabNodeKind::ForStmt:
				{
					const FDabForStmt& For = static_cast<const FDabForStmt&>(Statement);
					WalkExpr(File, For.Iterable.Get());
					if (For.Body)
					{
						WalkCodeBlock(File, *For.Body);
					}
					break;
				}
				case EDabNodeKind::ReturnStmt:
					WalkExpr(File, static_cast<const FDabReturnStmt&>(Statement).Value.Get());
					break;
				case EDabNodeKind::DetachedStmt:
				{
					const FDabDetachedStmt& Detached = static_cast<const FDabDetachedStmt&>(Statement);
					if (Detached.Body)
					{
						WalkCodeBlock(File, *Detached.Body);
					}
					break;
				}
				default:
					break;
				}
			}

			void WalkExpr(FDabFile& File, const FDabExpr* Expr)
			{
				if (!Expr)
				{
					return;
				}
				switch (Expr->Kind)
				{
				case EDabNodeKind::MemberExpr:
					WalkExpr(File, static_cast<const FDabMemberExpr*>(Expr)->Object.Get());
					break;
				case EDabNodeKind::CallExpr:
				{
					const FDabCallExpr* Call = static_cast<const FDabCallExpr*>(Expr);
					WalkExpr(File, Call->Callee.Get());
					TakeInnerTrivia(File, Call, Call->ArgsSpan.Offset);
					for (const FDabArgumentPtr& Argument : Call->Args)
					{
						TakeAnchorTrivia(File, Argument.Get());
						WalkExpr(File, Argument->Value.Get());
					}
					break;
				}
				case EDabNodeKind::ListExpr:
				{
					const FDabListExpr* List = static_cast<const FDabListExpr*>(Expr);
					TakeInnerTrivia(File, List, List->Span.Offset);
					for (const FDabExprPtr& Item : List->Items)
					{
						TakeAnchorTrivia(File, Item.Get());
						WalkExpr(File, Item.Get());
					}
					break;
				}
				case EDabNodeKind::TupleExpr:
				{
					const FDabTupleExpr* Tuple = static_cast<const FDabTupleExpr*>(Expr);
					TakeInnerTrivia(File, Tuple, Tuple->Span.Offset);
					for (const FDabArgumentPtr& Item : Tuple->Items)
					{
						TakeAnchorTrivia(File, Item.Get());
						WalkExpr(File, Item->Value.Get());
					}
					break;
				}
				case EDabNodeKind::BlockExpr:
					WalkBlock(File, *static_cast<const FDabBlockExpr*>(Expr));
					break;
				case EDabNodeKind::StackExpr:
				{
					const FDabStackExpr* Stack = static_cast<const FDabStackExpr*>(Expr);
					TakeInnerTrivia(File, Stack, Stack->Span.Offset);
					for (const FDabExprPtr& Step : Stack->Steps)
					{
						TakeAnchorTrivia(File, Step.Get());
						WalkExpr(File, Step.Get());
					}
					break;
				}
				case EDabNodeKind::UnaryExpr:
					WalkExpr(File, static_cast<const FDabUnaryExpr*>(Expr)->Operand.Get());
					break;
				case EDabNodeKind::BinaryExpr:
				{
					const FDabBinaryExpr* Binary = static_cast<const FDabBinaryExpr*>(Expr);
					WalkExpr(File, Binary->Lhs.Get());
					WalkExpr(File, Binary->Rhs.Get());
					break;
				}
				case EDabNodeKind::ConditionalExpr:
				{
					const FDabConditionalExpr* Conditional = static_cast<const FDabConditionalExpr*>(Expr);
					WalkExpr(File, Conditional->Condition.Get());
					WalkExpr(File, Conditional->Then.Get());
					WalkExpr(File, Conditional->Else.Get());
					break;
				}
				case EDabNodeKind::ParenExpr:
					WalkExpr(File, static_cast<const FDabParenExpr*>(Expr)->Inner.Get());
					break;
				case EDabNodeKind::IndexExpr:
				{
					const FDabIndexExpr* IndexExpr = static_cast<const FDabIndexExpr*>(Expr);
					WalkExpr(File, IndexExpr->Object.Get());
					WalkExpr(File, IndexExpr->Index.Get());
					break;
				}
				default:
					break;
				}
			}

			// ------------------------------------------------------------------------------------
			// State
			// ------------------------------------------------------------------------------------

			const FDabSourceText& Source;
			FDabDiagnosticSink& Sink;
			/** Every token but comments; the last one is always EndOfFile. */
			TArray<FDabToken> Tokens;
			int32 Index = 0;

			bool bKeepTrivia = false;
			/** The document being parsed is a ControlRig: more block words, generic types and code blocks. */
			bool bControlRig = false;
			/** Above zero while code is being parsed: a `[` after an expression is an index. */
			int32 CodeDepth = 0;
			/** How deep type arguments may nest (`Array<Array<double>>` is two). */
			static constexpr int32 MaxTypeDepth = 16;
			/** Set once the end of the file has been reported (or another diagnostic already explains it). */
			bool bEndReported = false;
			TArray<FOpenBracket> OpenBrackets;

			/** Comment tokens in source order. */
			TArray<FDabToken> Comments;
			/** Per token: how many comments come before it in the source. */
			TArray<int32> CommentsBefore;
			/** Comments before this index are claimed. */
			int32 CommentCursor = 0;
			/** Per child being parsed, innermost last: the comments swept up inside it. */
			TArray<TArray<int32>> Frames;
			/** By the offset of a child's first token. */
			TMap<int32, FPendingTrivia> PendingAnchors;
			/** By the offset of a container's opening bracket. */
			TMap<int32, TArray<int32>> PendingInner;
		};

		/** Lexer and parser report into their own sinks; the caller gets both, in source order. */
		static void MergeDiagnostics(FDabDiagnosticSink& Sink, const FDabDiagnosticSink& Lexical, const FDabDiagnosticSink& Parsed)
		{
			TArray<FDabDiagnostic> All;
			All.Reserve(Lexical.Num() + Parsed.Num());
			All.Append(Lexical.GetDiagnostics());
			All.Append(Parsed.GetDiagnostics());
			All.StableSort([](const FDabDiagnostic& A, const FDabDiagnostic& B)
			{
				return A.Span.Offset < B.Span.Offset;
			});

			for (FDabDiagnostic& Diagnostic : All)
			{
				switch (Diagnostic.Severity)
				{
				case EDabSeverity::Error:
					Sink.ErrorIn(*Diagnostic.Code, Diagnostic.FilePath, Diagnostic.Span, MoveTemp(Diagnostic.Message), MoveTemp(Diagnostic.Hint));
					break;
				case EDabSeverity::Warning:
					Sink.WarningIn(*Diagnostic.Code, Diagnostic.FilePath, Diagnostic.Span, MoveTemp(Diagnostic.Message), MoveTemp(Diagnostic.Hint));
					break;
				case EDabSeverity::Info:
				default:
					Sink.Info(*Diagnostic.Code, Diagnostic.Span, MoveTemp(Diagnostic.Message), MoveTemp(Diagnostic.Hint));
					break;
				}
			}
		}

		/** An unclosed block comment ran to the end of the file and took the closing brackets with it. */
		static bool CommentSwallowedTheEnd(const FDabDiagnosticSink& Lexical)
		{
			for (const FDabDiagnostic& Diagnostic : Lexical.GetDiagnostics())
			{
				if (Diagnostic.Code.Equals(TEXT("DAB1003"), ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}
	}

	TUniquePtr<FDabFile> ParseDab(const FDabSourceText& Source, FDabDiagnosticSink& Sink, const FDabParseOptions& Options)
	{
		FDabDiagnosticSink Lexical(Sink.GetFilePath());
		FDabDiagnosticSink Parsed(Sink.GetFilePath());

		FDabLexOptions LexOptions;
		LexOptions.bEmitComments = Options.bKeepTrivia;
		TArray<FDabToken> Tokens = LexDab(Source, Lexical, LexOptions);

		DabParserDetail::FDabParser Parser(Source, MoveTemp(Tokens), Parsed, Options.bKeepTrivia,
			DabParserDetail::CommentSwallowedTheEnd(Lexical));
		TUniquePtr<FDabFile> File = Parser.ParseFile();

		DabParserDetail::MergeDiagnostics(Sink, Lexical, Parsed);
		return File;
	}

	FDabExprPtr ParseDabExpression(const FDabSourceText& Source, FDabDiagnosticSink& Sink)
	{
		FDabDiagnosticSink Lexical(Sink.GetFilePath());
		FDabDiagnosticSink Parsed(Sink.GetFilePath());

		TArray<FDabToken> Tokens = LexDab(Source, Lexical);

		DabParserDetail::FDabParser Parser(Source, MoveTemp(Tokens), Parsed, false,
			DabParserDetail::CommentSwallowedTheEnd(Lexical));
		FDabExprPtr Expr = Parser.ParseStandaloneExpression();

		const bool bFailed = Lexical.HasErrors() || Parsed.HasErrors();
		DabParserDetail::MergeDiagnostics(Sink, Lexical, Parsed);

		// A partial expression (a call that kept its good arguments) is still not the expression written.
		if (bFailed)
		{
			return nullptr;
		}
		return Expr;
	}
}
