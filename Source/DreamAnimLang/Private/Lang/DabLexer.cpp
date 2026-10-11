// Copyright DreamMoon. All Rights Reserved.
//
// The lexer. One pass, at most three characters of lookahead. It never gives up: every problem is
// reported and still yields a token (an unclosed string is one String, an unclosed name in backticks
// one Identifier, a malformed number one Number, a stray character one Unknown), so the parser stays
// aligned with the source and one typo does not turn the rest of the file into noise. Positions always come from FDabSourceText::MakeSpan, so a
// CRLF file and an LF file report the same lines.

#include "Lang/DabToken.h"

#include "Lang/DabDiagnostic.h"
#include "Lang/DabPrinter.h"
#include "Lang/DabSource.h"
#include "Misc/Char.h"
#include "Misc/CString.h"

namespace UE::DreamAnim::Lang
{
	namespace DabLexerDetail
	{
		static bool IsDecimalDigit(const TCHAR Character)
		{
			return Character >= TEXT('0') && Character <= TEXT('9');
		}

		static bool IsLineBreak(const TCHAR Character)
		{
			return Character == TEXT('\n') || Character == TEXT('\r');
		}

		/**
		 * Non-ASCII spacing. An explicit list rather than iswspace: the C library answer depends on the
		 * process locale, and the ideographic space (U+3000) that a Chinese input method types must
		 * separate tokens on every machine.
		 */
		static bool IsWideWhitespace(const TCHAR Character)
		{
			const uint32 Code = static_cast<uint32>(Character);
			return Code == 0x0085u
				|| Code == 0x00A0u
				|| Code == 0x1680u
				|| (Code >= 0x2000u && Code <= 0x200Bu)
				|| Code == 0x2028u
				|| Code == 0x2029u
				|| Code == 0x202Fu
				|| Code == 0x205Fu
				|| Code == 0x3000u
				|| Code == 0xFEFFu;
		}

		/** Whitespace that does not end a line. */
		static bool IsInlineWhitespace(const TCHAR Character)
		{
			if (static_cast<uint32>(Character) >= 0x80u)
			{
				return IsWideWhitespace(Character);
			}
			return Character == TEXT(' ') || Character == TEXT('\t') || Character == TEXT('\v') || Character == TEXT('\f');
		}

		/**
		 * Any character from U+0080 up that is not spacing counts as a letter, so Chinese names work without
		 * a Unicode table. Below that, only ASCII letters and `_`.
		 */
		static bool IsIdentifierStart(const TCHAR Character)
		{
			if (Character == TEXT('_'))
			{
				return true;
			}
			if (static_cast<uint32>(Character) < 0x80u)
			{
				return FChar::IsAlpha(Character);
			}
			return !IsWideWhitespace(Character);
		}

		static bool IsIdentifierChar(const TCHAR Character)
		{
			return IsIdentifierStart(Character) || IsDecimalDigit(Character);
		}

		static const TCHAR* const GDirectiveWords[] = { TEXT("include"), TEXT("Region"), TEXT("EndRegion") };

		static bool IsKnownDirectiveWord(const FString& Word)
		{
			for (const TCHAR* Known : GDirectiveWords)
			{
				if (Word.Equals(Known, ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

		/**
		 * Punctuation and operators, longest first: the lexer takes the first spelling that matches, so
		 * `->` must be tried before `-` and `<=` before `<`. The spellings come from GetDabTokenSpelling
		 * so the lexer and the printer cannot disagree about them.
		 */
		static const EDabTokenKind GOperatorsLongestFirst[] =
		{
			EDabTokenKind::Arrow,
			EDabTokenKind::EqualEqual,
			EDabTokenKind::BangEqual,
			EDabTokenKind::LessEqual,
			EDabTokenKind::GreaterEqual,
			EDabTokenKind::AmpersandAmpersand,
			EDabTokenKind::PipePipe,

			EDabTokenKind::LeftParen,
			EDabTokenKind::RightParen,
			EDabTokenKind::LeftBrace,
			EDabTokenKind::RightBrace,
			EDabTokenKind::LeftBracket,
			EDabTokenKind::RightBracket,
			EDabTokenKind::Comma,
			EDabTokenKind::Semicolon,
			EDabTokenKind::Colon,
			EDabTokenKind::Dot,
			EDabTokenKind::Question,
			EDabTokenKind::Assign,
			EDabTokenKind::Plus,
			EDabTokenKind::Minus,
			EDabTokenKind::Star,
			EDabTokenKind::Slash,
			EDabTokenKind::Percent,
			EDabTokenKind::Bang,
			EDabTokenKind::Less,
			EDabTokenKind::Greater,
		};
	}

	const TCHAR* LexToString(const EDabTokenKind Kind)
	{
		switch (Kind)
		{
		case EDabTokenKind::EndOfFile: return TEXT("EndOfFile");
		case EDabTokenKind::Identifier: return TEXT("Identifier");
		case EDabTokenKind::Number: return TEXT("Number");
		case EDabTokenKind::String: return TEXT("String");
		case EDabTokenKind::Directive: return TEXT("Directive");
		case EDabTokenKind::LeftParen: return TEXT("LeftParen");
		case EDabTokenKind::RightParen: return TEXT("RightParen");
		case EDabTokenKind::LeftBrace: return TEXT("LeftBrace");
		case EDabTokenKind::RightBrace: return TEXT("RightBrace");
		case EDabTokenKind::LeftBracket: return TEXT("LeftBracket");
		case EDabTokenKind::RightBracket: return TEXT("RightBracket");
		case EDabTokenKind::Comma: return TEXT("Comma");
		case EDabTokenKind::Semicolon: return TEXT("Semicolon");
		case EDabTokenKind::Colon: return TEXT("Colon");
		case EDabTokenKind::Dot: return TEXT("Dot");
		case EDabTokenKind::Question: return TEXT("Question");
		case EDabTokenKind::Assign: return TEXT("Assign");
		case EDabTokenKind::Arrow: return TEXT("Arrow");
		case EDabTokenKind::Plus: return TEXT("Plus");
		case EDabTokenKind::Minus: return TEXT("Minus");
		case EDabTokenKind::Star: return TEXT("Star");
		case EDabTokenKind::Slash: return TEXT("Slash");
		case EDabTokenKind::Percent: return TEXT("Percent");
		case EDabTokenKind::Bang: return TEXT("Bang");
		case EDabTokenKind::AmpersandAmpersand: return TEXT("AmpersandAmpersand");
		case EDabTokenKind::PipePipe: return TEXT("PipePipe");
		case EDabTokenKind::Less: return TEXT("Less");
		case EDabTokenKind::Greater: return TEXT("Greater");
		case EDabTokenKind::LessEqual: return TEXT("LessEqual");
		case EDabTokenKind::GreaterEqual: return TEXT("GreaterEqual");
		case EDabTokenKind::EqualEqual: return TEXT("EqualEqual");
		case EDabTokenKind::BangEqual: return TEXT("BangEqual");
		case EDabTokenKind::Comment: return TEXT("Comment");
		case EDabTokenKind::Unknown: return TEXT("Unknown");
		default: return TEXT("Unknown");
		}
	}

	const TCHAR* GetDabTokenSpelling(const EDabTokenKind Kind)
	{
		switch (Kind)
		{
		case EDabTokenKind::LeftParen: return TEXT("(");
		case EDabTokenKind::RightParen: return TEXT(")");
		case EDabTokenKind::LeftBrace: return TEXT("{");
		case EDabTokenKind::RightBrace: return TEXT("}");
		case EDabTokenKind::LeftBracket: return TEXT("[");
		case EDabTokenKind::RightBracket: return TEXT("]");
		case EDabTokenKind::Comma: return TEXT(",");
		case EDabTokenKind::Semicolon: return TEXT(";");
		case EDabTokenKind::Colon: return TEXT(":");
		case EDabTokenKind::Dot: return TEXT(".");
		case EDabTokenKind::Question: return TEXT("?");
		case EDabTokenKind::Assign: return TEXT("=");
		case EDabTokenKind::Arrow: return TEXT("->");
		case EDabTokenKind::Plus: return TEXT("+");
		case EDabTokenKind::Minus: return TEXT("-");
		case EDabTokenKind::Star: return TEXT("*");
		case EDabTokenKind::Slash: return TEXT("/");
		case EDabTokenKind::Percent: return TEXT("%");
		case EDabTokenKind::Bang: return TEXT("!");
		case EDabTokenKind::AmpersandAmpersand: return TEXT("&&");
		case EDabTokenKind::PipePipe: return TEXT("||");
		case EDabTokenKind::Less: return TEXT("<");
		case EDabTokenKind::Greater: return TEXT(">");
		case EDabTokenKind::LessEqual: return TEXT("<=");
		case EDabTokenKind::GreaterEqual: return TEXT(">=");
		case EDabTokenKind::EqualEqual: return TEXT("==");
		case EDabTokenKind::BangEqual: return TEXT("!=");
		default: return TEXT("");
		}
	}

	namespace DabLexerDetail
	{
		/** Inside this namespace so a unity build finds these helpers before any same-named one of another file. */
		static TArray<FDabToken> LexSource(const FDabSourceText& Source, FDabDiagnosticSink& Sink, const FDabLexOptions& Options)
		{
			const FString& SourceText = Source.GetText();
			const TCHAR* const Chars = *SourceText;
			const int32 Length = SourceText.Len();

			TArray<FDabToken> Tokens;
			Tokens.Reserve((Length / 4) + 8);

			int32 Cursor = 0;
			// Nothing but spaces and tabs seen on this line yet. A comment counts as something, which is
			// why `/* x */ #include` is not a directive line.
			bool bAtLineStart = true;
			// Line breaks since the last emitted token. A skipped comment does not reset it, so the count
			// always means "lines between this token and the one before it in the array".
			int32 Newlines = 0;

			auto Emit = [&Tokens, &bAtLineStart, &Newlines](FDabToken& Token)
			{
				Token.bAtLineStart = bAtLineStart;
				Token.NewlinesBefore = Newlines;
				bAtLineStart = false;
				Newlines = 0;
				Tokens.Add(MoveTemp(Token));
			};

			while (Cursor < Length)
			{
				const TCHAR Character = Chars[Cursor];

				// ---------------------------------------------------------------- whitespace
				if (IsLineBreak(Character))
				{
					++Cursor;
					if (Character == TEXT('\r') && Cursor < Length && Chars[Cursor] == TEXT('\n'))
					{
						++Cursor;
					}
					++Newlines;
					bAtLineStart = true;
					continue;
				}
				if (IsInlineWhitespace(Character))
				{
					++Cursor;
					continue;
				}

				// ---------------------------------------------------------------- comments
				if (Character == TEXT('/') && Cursor + 1 < Length && Chars[Cursor + 1] == TEXT('/'))
				{
					const int32 CommentStart = Cursor;
					while (Cursor < Length && !IsLineBreak(Chars[Cursor]))
					{
						++Cursor;
					}

					if (Options.bEmitComments)
					{
						FDabToken Comment;
						Comment.Kind = EDabTokenKind::Comment;
						Comment.Span = Source.MakeSpan(CommentStart, Cursor - CommentStart);
						Comment.Text = SourceText.Mid(CommentStart, Cursor - CommentStart);
						Emit(Comment);
					}
					else
					{
						bAtLineStart = false;
					}
					continue;
				}

				if (Character == TEXT('/') && Cursor + 1 < Length && Chars[Cursor + 1] == TEXT('*'))
				{
					const int32 CommentStart = Cursor;
					Cursor += 2;

					int32 InnerBreaks = 0;
					bool bClosed = false;
					while (Cursor < Length)
					{
						const TCHAR Inner = Chars[Cursor];
						if (Inner == TEXT('*') && Cursor + 1 < Length && Chars[Cursor + 1] == TEXT('/'))
						{
							Cursor += 2;
							bClosed = true;
							break;
						}
						if (Inner == TEXT('\r'))
						{
							++InnerBreaks;
							++Cursor;
							if (Cursor < Length && Chars[Cursor] == TEXT('\n'))
							{
								++Cursor;
							}
							continue;
						}
						if (Inner == TEXT('\n'))
						{
							++InnerBreaks;
						}
						++Cursor;
					}

					if (!bClosed)
					{
						// At the opening `/*`: that is what the reader has to fix; the end of the file says nothing.
						Sink.Error(TEXT("DAB1003"), Source.MakeSpan(CommentStart, 2),
							TEXT("The block comment is not closed; expected '*/'."),
							TEXT("Everything from here to the end of the file is inside the comment."));
					}

					if (Options.bEmitComments)
					{
						FDabToken Comment;
						Comment.Kind = EDabTokenKind::Comment;
						Comment.Span = Source.MakeSpan(CommentStart, Cursor - CommentStart);
						Comment.Text = SourceText.Mid(CommentStart, Cursor - CommentStart);
						Emit(Comment);
					}
					else
					{
						Newlines += InnerBreaks;
						bAtLineStart = false;
					}
					continue;
				}

				// ---------------------------------------------------------------- directives
				if (Character == TEXT('#') && bAtLineStart && Cursor + 1 < Length && IsIdentifierStart(Chars[Cursor + 1]))
				{
					const int32 DirectiveStart = Cursor;
					int32 WordEnd = Cursor + 1;
					while (WordEnd < Length && IsIdentifierChar(Chars[WordEnd]))
					{
						++WordEnd;
					}

					FDabToken Directive;
					Directive.Kind = EDabTokenKind::Directive;
					Directive.Span = Source.MakeSpan(DirectiveStart, WordEnd - DirectiveStart);
					Directive.Text = SourceText.Mid(DirectiveStart + 1, WordEnd - (DirectiveStart + 1));

					if (!IsKnownDirectiveWord(Directive.Text))
					{
						// Still a Directive token: the parser skips an unknown one quietly instead of
						// stumbling over its argument.
						FString Hint;
						for (const TCHAR* Known : GDirectiveWords)
						{
							if (Directive.Text.Equals(Known, ESearchCase::IgnoreCase))
							{
								Hint = FString::Printf(TEXT("Did you mean '#%s'?"), Known);
								break;
							}
						}
						Sink.Error(TEXT("DAB1005"), Directive.Span,
							FString::Printf(TEXT("Unknown directive '#%s'; the directives are #include, #Region and #EndRegion."), *Directive.Text),
							MoveTemp(Hint));
					}

					Cursor = WordEnd;
					Emit(Directive);
					continue;
				}

				// ---------------------------------------------------------------- strings
				if (Character == TEXT('"'))
				{
					const int32 StringStart = Cursor;
					++Cursor;

					FString Value;
					bool bClosed = false;
					while (Cursor < Length)
					{
						const TCHAR Current = Chars[Cursor];

						// A string never spans a line, so a missing quote costs one line, not the rest of the file.
						if (IsLineBreak(Current))
						{
							break;
						}
						if (Current == TEXT('"'))
						{
							++Cursor;
							bClosed = true;
							break;
						}
						if (Current == TEXT('\\'))
						{
							if (Cursor + 1 >= Length || IsLineBreak(Chars[Cursor + 1]))
							{
								// A backslash at the end of the line: the unclosed-string report says it all.
								++Cursor;
								continue;
							}

							const TCHAR Escape = Chars[Cursor + 1];
							switch (Escape)
							{
							case TEXT('"'):
								Value.AppendChar(TEXT('"'));
								break;
							case TEXT('\\'):
								Value.AppendChar(TEXT('\\'));
								break;
							case TEXT('n'):
								Value.AppendChar(TEXT('\n'));
								break;
							case TEXT('t'):
								Value.AppendChar(TEXT('\t'));
								break;
							case TEXT('r'):
								Value.AppendChar(TEXT('\r'));
								break;
							default:
								Sink.Error(TEXT("DAB1006"), Source.MakeSpan(Cursor, 2),
									FString::Printf(TEXT("Unknown escape '\\%s' in a string."), *FString::Chr(Escape)),
									TEXT("The escapes are \\\", \\\\, \\n, \\t and \\r."));
								// Keep both characters so the value still shows what was written.
								Value.AppendChar(TEXT('\\'));
								Value.AppendChar(Escape);
								break;
							}
							Cursor += 2;
							continue;
						}

						Value.AppendChar(Current);
						++Cursor;
					}

					if (!bClosed)
					{
						Sink.Error(TEXT("DAB1002"), Source.MakeSpan(StringStart, 1),
							TEXT("The string is not closed on its line."),
							TEXT("Add the closing '\"'; a string cannot span lines."));
					}

					FDabToken StringToken;
					StringToken.Kind = EDabTokenKind::String;
					StringToken.Span = Source.MakeSpan(StringStart, Cursor - StringStart);
					StringToken.Text = MoveTemp(Value);
					Emit(StringToken);
					continue;
				}

				// ---------------------------------------------------------------- numbers
				if (IsDecimalDigit(Character)
					|| (Character == TEXT('.') && Cursor + 1 < Length && IsDecimalDigit(Chars[Cursor + 1])))
				{
					const int32 NumberStart = Cursor;
					bool bMalformed = false;

					while (Cursor < Length && IsDecimalDigit(Chars[Cursor]))
					{
						++Cursor;
					}

					if (Cursor < Length && Chars[Cursor] == TEXT('.'))
					{
						++Cursor;
						if (Cursor < Length && IsDecimalDigit(Chars[Cursor]))
						{
							while (Cursor < Length && IsDecimalDigit(Chars[Cursor]))
							{
								++Cursor;
							}
						}
						else
						{
							// `1.`: the grammar wants digits after the point.
							bMalformed = true;
						}
					}

					if (Cursor < Length && (Chars[Cursor] == TEXT('e') || Chars[Cursor] == TEXT('E')))
					{
						int32 Probe = Cursor + 1;
						if (Probe < Length && (Chars[Probe] == TEXT('+') || Chars[Probe] == TEXT('-')))
						{
							++Probe;
						}
						const bool bHasExponentDigits = Probe < Length && IsDecimalDigit(Chars[Probe]);

						// `1e` stays one token: split into `1` and the name `e` it would read as a plausible
						// expression and hide the mistake.
						Cursor = Probe;
						if (bHasExponentDigits)
						{
							while (Cursor < Length && IsDecimalDigit(Chars[Cursor]))
							{
								++Cursor;
							}
						}
						else
						{
							bMalformed = true;
						}
					}

					// Anything still glued on (`1.2.3`, `2px`, `0x1F`) belongs to this token, not the next one.
					while (Cursor < Length
						&& (IsIdentifierChar(Chars[Cursor])
							|| (Chars[Cursor] == TEXT('.') && Cursor + 1 < Length && IsIdentifierChar(Chars[Cursor + 1]))))
					{
						bMalformed = true;
						++Cursor;
					}

					FDabToken NumberToken;
					NumberToken.Kind = EDabTokenKind::Number;
					NumberToken.Span = Source.MakeSpan(NumberStart, Cursor - NumberStart);
					NumberToken.Text = SourceText.Mid(NumberStart, Cursor - NumberStart);
					// Atod stops at the first character it cannot use, which gives a usable value for the malformed forms too.
					NumberToken.Number = FCString::Atod(*NumberToken.Text);

					if (bMalformed)
					{
						Sink.Error(TEXT("DAB1004"), NumberToken.Span,
							FString::Printf(TEXT("Malformed number '%s'."), *NumberToken.Text),
							TEXT("Numbers are written like 12, 0.5, .5, 1e-3 or 2.5E+4, with no suffix."));
					}

					Emit(NumberToken);
					continue;
				}

				// ---------------------------------------------------------------- names in backticks
				if (Character == TEXT('`'))
				{
					const int32 NameStart = Cursor;
					++Cursor;
					const int32 TextStart = Cursor;
					// Like a string, a name never spans a line.
					while (Cursor < Length && !IsLineBreak(Chars[Cursor]) && Chars[Cursor] != TEXT('`'))
					{
						++Cursor;
					}
					const bool bClosed = Cursor < Length && Chars[Cursor] == TEXT('`');

					FDabToken Name;
					Name.Kind = EDabTokenKind::Identifier;
					Name.bQuoted = true;
					Name.Text = SourceText.Mid(TextStart, Cursor - TextStart);
					if (bClosed)
					{
						++Cursor;
					}
					Name.Span = Source.MakeSpan(NameStart, Cursor - NameStart);

					if (!bClosed)
					{
						Sink.Error(TEXT("DAB1007"), Source.MakeSpan(NameStart, 1),
							TEXT("The name in backticks is not closed on its line."),
							TEXT("Add the closing '`'; a name cannot span lines."));
					}
					else if (Name.Text.IsEmpty())
					{
						Sink.Error(TEXT("DAB1007"), Name.Span,
							TEXT("A name in backticks cannot be empty."),
							TEXT("Write the name between the backticks, as in `Module Settings`."));
					}

					Emit(Name);
					continue;
				}

				// ---------------------------------------------------------------- identifiers
				if (IsIdentifierStart(Character))
				{
					const int32 WordStart = Cursor;
					while (Cursor < Length && IsIdentifierChar(Chars[Cursor]))
					{
						++Cursor;
					}

					FDabToken Identifier;
					Identifier.Kind = EDabTokenKind::Identifier;
					Identifier.Span = Source.MakeSpan(WordStart, Cursor - WordStart);
					Identifier.Text = SourceText.Mid(WordStart, Cursor - WordStart);
					Emit(Identifier);
					continue;
				}

				// ---------------------------------------------------------------- punctuation and operators
				{
					bool bMatched = false;
					for (const EDabTokenKind OperatorKind : GOperatorsLongestFirst)
					{
						const TCHAR* const Spelling = GetDabTokenSpelling(OperatorKind);
						const int32 SpellingLength = FCString::Strlen(Spelling);
						if (SpellingLength > 0 && Cursor + SpellingLength <= Length
							&& FCString::Strncmp(Chars + Cursor, Spelling, static_cast<SIZE_T>(SpellingLength)) == 0)
						{
							FDabToken Operator;
							Operator.Kind = OperatorKind;
							Operator.Span = Source.MakeSpan(Cursor, SpellingLength);
							Cursor += SpellingLength;
							Emit(Operator);
							bMatched = true;
							break;
						}
					}
					if (bMatched)
					{
						continue;
					}
				}

				// ---------------------------------------------------------------- anything else
				{
					FDabToken Unknown;
					Unknown.Kind = EDabTokenKind::Unknown;
					Unknown.Span = Source.MakeSpan(Cursor, 1);
					Unknown.Text = FString::Chr(Character);

					FString Message;
					FString Hint;
					if (Character == TEXT('#'))
					{
						Message = bAtLineStart
							? FString(TEXT("Expected a directive name after '#'."))
							: FString(TEXT("Unexpected character '#'; a directive must be the first thing on its line."));
						Hint = TEXT("The directives are #include, #Region and #EndRegion.");
					}
					else if (static_cast<uint32>(Character) < 0x20u || Character == static_cast<TCHAR>(0x7F))
					{
						Message = FString::Printf(TEXT("Unexpected character U+%04X."), static_cast<uint32>(Character));
					}
					else
					{
						Message = FString::Printf(TEXT("Unexpected character '%s'."), *Unknown.Text);
						if (Character == TEXT('&'))
						{
							Hint = TEXT("Did you mean '&&'?");
						}
						else if (Character == TEXT('|'))
						{
							Hint = TEXT("Did you mean '||'?");
						}
						else if (Character == TEXT('\''))
						{
							Hint = TEXT("Strings use double quotes.");
						}
					}

					Sink.Error(TEXT("DAB1001"), Unknown.Span, MoveTemp(Message), MoveTemp(Hint));

					++Cursor;
					Emit(Unknown);
				}
			}

			// Always exactly one, with an empty span at the end, so the parser's cursor has somewhere to rest.
			FDabToken End;
			End.Kind = EDabTokenKind::EndOfFile;
			End.Span = Source.MakeSpan(Length, 0);
			Emit(End);

			return Tokens;
		}
	}

	TArray<FDabToken> LexDab(const FDabSourceText& Source, FDabDiagnosticSink& Sink, const FDabLexOptions& Options)
	{
		return DabLexerDetail::LexSource(Source, Sink, Options);
	}

	bool IsDabPlainIdentifier(const FString& Name)
	{
		if (Name.IsEmpty() || !DabLexerDetail::IsIdentifierStart(Name[0]))
		{
			return false;
		}
		for (int32 Index = 1; Index < Name.Len(); ++Index)
		{
			if (!DabLexerDetail::IsIdentifierChar(Name[Index]))
			{
				return false;
			}
		}
		return true;
	}
}
