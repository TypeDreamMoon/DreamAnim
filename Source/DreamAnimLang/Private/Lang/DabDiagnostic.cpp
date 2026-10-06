// Copyright DreamMoon. All Rights Reserved.
//
// FDabDiagnosticSink. The wire form `DABnnnn (line,col): message` is what logs and the corpus
// expectations compare, so it carries no hint: a reworded hint must not break a golden.

#include "Lang/DabDiagnostic.h"

namespace UE::DreamAnim::Lang
{
	const TCHAR* LexToString(const EDabSeverity Severity)
	{
		// Lower case, the spelling the DreamShader and DreamFX diagnostics use, so tools that read
		// all three can match on one set of words.
		switch (Severity)
		{
		case EDabSeverity::Warning:
			return TEXT("warning");
		case EDabSeverity::Info:
			return TEXT("info");
		case EDabSeverity::Error:
		default:
			return TEXT("error");
		}
	}

	FDabDiagnosticSink::FDabDiagnosticSink(FString InFilePath)
		: FilePath(MoveTemp(InFilePath))
	{
	}

	bool FDabDiagnosticSink::Error(const TCHAR* Code, const FDabSpan& Span, FString Message, FString Hint)
	{
		Add(EDabSeverity::Error, Code, FilePath, Span, MoveTemp(Message), MoveTemp(Hint));
		return false;
	}

	void FDabDiagnosticSink::Warning(const TCHAR* Code, const FDabSpan& Span, FString Message, FString Hint)
	{
		Add(EDabSeverity::Warning, Code, FilePath, Span, MoveTemp(Message), MoveTemp(Hint));
	}

	void FDabDiagnosticSink::Info(const TCHAR* Code, const FDabSpan& Span, FString Message, FString Hint)
	{
		Add(EDabSeverity::Info, Code, FilePath, Span, MoveTemp(Message), MoveTemp(Hint));
	}

	bool FDabDiagnosticSink::ErrorIn(const TCHAR* Code, const FString& InFilePath, const FDabSpan& Span, FString Message, FString Hint)
	{
		Add(EDabSeverity::Error, Code, InFilePath, Span, MoveTemp(Message), MoveTemp(Hint));
		return false;
	}

	void FDabDiagnosticSink::WarningIn(const TCHAR* Code, const FString& InFilePath, const FDabSpan& Span, FString Message, FString Hint)
	{
		Add(EDabSeverity::Warning, Code, InFilePath, Span, MoveTemp(Message), MoveTemp(Hint));
	}

	void FDabDiagnosticSink::Append(FDabDiagnosticSink&& Other)
	{
		Diagnostics.Reserve(Diagnostics.Num() + Other.Diagnostics.Num());
		for (FDabDiagnostic& Diagnostic : Other.Diagnostics)
		{
			if (Diagnostic.Severity == EDabSeverity::Error)
			{
				++ErrorCount;
			}
			Diagnostics.Add(MoveTemp(Diagnostic));
		}

		// The other sink has handed its contents over; its counters must say so.
		Other.Diagnostics.Reset();
		Other.ErrorCount = 0;
	}

	const FDabDiagnostic* FDabDiagnosticSink::FirstError() const
	{
		for (const FDabDiagnostic& Diagnostic : Diagnostics)
		{
			if (Diagnostic.Severity == EDabSeverity::Error)
			{
				return &Diagnostic;
			}
		}
		return nullptr;
	}

	FString FDabDiagnosticSink::ToWireString(const FDabDiagnostic& Diagnostic)
	{
		return FString::Printf(TEXT("%s (%d,%d): %s"), *Diagnostic.Code, Diagnostic.Span.Line, Diagnostic.Span.Column, *Diagnostic.Message);
	}

	void FDabDiagnosticSink::Add(const EDabSeverity Severity, const TCHAR* Code, const FString& InFilePath, const FDabSpan& Span, FString&& Message, FString&& Hint)
	{
		FDabDiagnostic Diagnostic;
		Diagnostic.Code = Code;
		Diagnostic.Severity = Severity;
		Diagnostic.Message = MoveTemp(Message);
		Diagnostic.Hint = MoveTemp(Hint);
		Diagnostic.Span = Span;
		Diagnostic.FilePath = InFilePath;

		if (Severity == EDabSeverity::Error)
		{
			++ErrorCount;
		}

		Diagnostics.Add(MoveTemp(Diagnostic));
	}
}
