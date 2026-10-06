// Copyright DreamMoon. All Rights Reserved.
//
// #include resolution and template expansion: the two steps between parsing a file and checking what
// its names mean.

#pragma once

#include "CoreMinimal.h"
#include "Lang/DabAst.h"
#include "Lang/DabDiagnostic.h"

namespace UE::DreamAnim::Lang
{
	/** Finds and reads #included files. */
	class IDabIncludeLoader
	{
	public:
		virtual ~IDabIncludeLoader() = default;

		/**
		 * Resolves IncludePath as written in the file FromFile. On success fills the resolved path (the
		 * same file must always resolve to the same string, it is the cycle and dedupe key) and its text.
		 */
		virtual bool Load(const FString& FromFile, const FString& IncludePath, FString& OutResolvedPath, FString& OutText) = 0;
	};

	/** Resolves relative to the including file's folder first, then against each root in order. */
	class DREAMANIMLANG_API FDabFileSystemIncludeLoader final : public IDabIncludeLoader
	{
	public:
		explicit FDabFileSystemIncludeLoader(TArray<FString> InRoots = {}) : Roots(MoveTemp(InRoots)) {}
		virtual bool Load(const FString& FromFile, const FString& IncludePath, FString& OutResolvedPath, FString& OutText) override;

	private:
		TArray<FString> Roots;
	};

	/** One file and every template library it reaches through #include. */
	struct FDabCompilationUnit
	{
		FDabSourceText MainSource;
		TUniquePtr<FDabFile> Main;
		/** Each library once, in the order first reached. */
		TArray<FDabSourceText> LibrarySources;
		TArray<TUniquePtr<FDabFile>> Libraries;
	};

	/**
	 * Parses every file Unit.Main #includes, recursively, each once. An included file must hold an
	 * AnimTemplates document (DAB3203); a missing file is DAB3201; a cycle is DAB3202. Diagnostics about
	 * a library carry its path. Returns false when any error was reported.
	 */
	DREAMANIMLANG_API bool ResolveDabIncludes(FDabCompilationUnit& Unit, IDabIncludeLoader& Loader, FDabDiagnosticSink& Sink);

	/**
	 * Replaces every template call in Unit.Main's document with the template's body.
	 *
	 * - Templates are the `Template Name(...) = Body;` statements at document level, in Main and in
	 *   every library. Two with the same name is DAB3105.
	 * - A call is a CallExpr whose callee is a bare NameExpr naming a template. Arguments bind to
	 *   parameters positionally, then by name; a parameter without an argument takes its default.
	 *   DAB3101 / 3102 / 3103 for a missing, unknown or extra argument.
	 * - The body is cloned and every NameExpr naming a parameter is replaced by a clone of the bound
	 *   value; this also covers a parameter used as a path prefix (`VB.ForwardAmount`). Argument names,
	 *   assignment keys and metadata keys are never substituted.
	 * - The result is expanded again, so a template may call another; more than 16 levels is DAB3104.
	 * - Spans: nodes that came from the template body take the call's span; nodes that came from an
	 *   argument keep their own spans (they are real positions in Main). Unit.Main->Origins gets one
	 *   entry per expansion, keyed by the root of the expanded expression.
	 * - Template definitions themselves are left in the tree, unexpanded. Expansion does not look
	 *   inside them.
	 *
	 * Returns false when any error was reported.
	 */
	DREAMANIMLANG_API bool ExpandDabTemplates(FDabCompilationUnit& Unit, FDabDiagnosticSink& Sink);
}
