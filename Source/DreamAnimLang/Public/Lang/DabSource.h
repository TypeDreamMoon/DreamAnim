// Copyright DreamMoon. All Rights Reserved.
//
// Source text and positions. Everything the lexer, the parser and the diagnostics say about "where"
// is an FDabSpan: a range of characters in one source text, with the 1-based line and column of its
// start already resolved.

#pragma once

#include "CoreMinimal.h"

namespace UE::DreamAnim::Lang
{
	/**
	 * A half-open range [Offset, Offset + Length) of TCHARs into one FDabSourceText, plus the 1-based
	 * line and column of Offset. Offset and Length are authoritative (the printer and the write-back
	 * slice text with them); Line and Column are cached so a diagnostic needs no source to print.
	 */
	struct FDabSpan
	{
		int32 Offset = 0;
		int32 Length = 0;
		int32 Line = 1;
		int32 Column = 1;

		int32 End() const { return Offset + Length; }
		bool IsEmpty() const { return Length <= 0; }

		/** The smallest span covering both. Line and Column follow whichever starts first. */
		static FDabSpan Join(const FDabSpan& A, const FDabSpan& B)
		{
			const FDabSpan& First = (B.Offset < A.Offset) ? B : A;
			FDabSpan Result;
			Result.Offset = First.Offset;
			Result.Length = FMath::Max(A.End(), B.End()) - Result.Offset;
			Result.Line = First.Line;
			Result.Column = First.Column;
			return Result;
		}
	};

	/** Which kind of DreamAnim file a path names, from its extension. */
	enum class EDabFileKind : uint8
	{
		/** `.dab`: an Animation Blueprint. */
		AnimBlueprint,
		/** `.dai`: an Animation Layer Interface. */
		AnimLayerInterface,
		/** `.dat`: a template library, only ever #included. */
		Templates,
		/** `.dcr`: a Control Rig. */
		ControlRig,
		Unknown,
	};

	/** `.dab` / `.dai` / `.dat`, case-insensitively; anything else is Unknown. */
	DREAMANIMLANG_API EDabFileKind GetDabFileKindFromPath(const FString& Path);
	DREAMANIMLANG_API const TCHAR* LexToString(EDabFileKind Kind);

	/**
	 * One source text with its line table. Line terminators are `\n`, `\r\n` and a lone `\r`, each
	 * counting as one line. A leading byte-order mark is not part of Text.
	 */
	class DREAMANIMLANG_API FDabSourceText
	{
	public:
		FDabSourceText() = default;
		FDabSourceText(FString InPath, FString InText);

		const FString& GetPath() const { return Path; }
		const FString& GetText() const { return Text; }
		int32 Len() const { return Text.Len(); }
		int32 GetLineCount() const { return LineStarts.Num(); }

		/** 1-based line and column of an offset; an offset past the end maps to the last position. */
		void GetLineAndColumn(int32 Offset, int32& OutLine, int32& OutColumn) const;

		/** The text of a 1-based line without its terminator; empty when out of range. */
		FString GetLineText(int32 Line) const;

		/** A span with its line and column resolved. */
		FDabSpan MakeSpan(int32 Offset, int32 Length) const;

		/** The text a span covers. */
		FString Slice(const FDabSpan& Span) const;

	private:
		FString Path;
		FString Text;
		/** Offset of the first TCHAR of every line; LineStarts[0] is always 0. */
		TArray<int32> LineStarts;
	};
}
