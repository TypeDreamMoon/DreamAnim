// Copyright DreamMoon. All Rights Reserved.
//
// Diagnostics. Every diagnostic is a DABnnnn code, a severity, a message, a span and, where one
// helps, a hint that says how to fix it. Messages are plain English FStrings: the corpus tests and
// the command line compare and print them as they are.
//
// Raise sites are spelled code first -- `Sink.Error(TEXT("DAB2001"), Span, Message)` -- so a script
// can find every code by searching for that shape. Do not wrap them in a helper that hides the code.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabSource.h"

namespace UE::DreamAnim::Lang
{
	enum class EDabSeverity : uint8
	{
		Error,
		Warning,
		Info,
	};

	DREAMANIMLANG_API const TCHAR* LexToString(EDabSeverity Severity);

	struct FDabDiagnostic
	{
		/** DABnnnn. Never empty. */
		FString Code;
		EDabSeverity Severity = EDabSeverity::Error;
		FString Message;
		/** How to fix it, when there is something specific to say. May be empty. */
		FString Hint;
		FDabSpan Span;
		/** The file the span refers to. */
		FString FilePath;
	};

	/**
	 * Collects diagnostics for one run. The front end keeps going after an error where it can, so a
	 * sink may hold several; ask HasErrors() whether the result is usable.
	 */
	class DREAMANIMLANG_API FDabDiagnosticSink
	{
	public:
		FDabDiagnosticSink() = default;
		explicit FDabDiagnosticSink(FString InFilePath);

		/** Returns false so `return Sink.Error(...)` reads naturally in a parse function. */
		bool Error(const TCHAR* Code, const FDabSpan& Span, FString Message, FString Hint = FString());
		void Warning(const TCHAR* Code, const FDabSpan& Span, FString Message, FString Hint = FString());
		void Info(const TCHAR* Code, const FDabSpan& Span, FString Message, FString Hint = FString());

		/** The same, about a file other than the sink's own (an #included library). */
		bool ErrorIn(const TCHAR* Code, const FString& InFilePath, const FDabSpan& Span, FString Message, FString Hint = FString());
		void WarningIn(const TCHAR* Code, const FString& InFilePath, const FDabSpan& Span, FString Message, FString Hint = FString());

		bool HasErrors() const { return ErrorCount > 0; }
		int32 NumErrors() const { return ErrorCount; }
		int32 Num() const { return Diagnostics.Num(); }

		const TArray<FDabDiagnostic>& GetDiagnostics() const { return Diagnostics; }
		const FString& GetFilePath() const { return FilePath; }

		/** Moves every diagnostic of Other onto the end of this sink. */
		void Append(FDabDiagnosticSink&& Other);

		/** The first error, or null. */
		const FDabDiagnostic* FirstError() const;

		/** `DABnnnn (line,col): message` -- the form logs and the corpus expectations use. */
		static FString ToWireString(const FDabDiagnostic& Diagnostic);

	private:
		void Add(EDabSeverity Severity, const TCHAR* Code, const FString& InFilePath, const FDabSpan& Span, FString&& Message, FString&& Hint);

		FString FilePath;
		TArray<FDabDiagnostic> Diagnostics;
		int32 ErrorCount = 0;
	};
}
