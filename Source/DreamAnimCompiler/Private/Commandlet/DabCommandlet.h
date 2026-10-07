// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "DabCommandlet.generated.h"

/**
 * DreamAnim without the editor UI.
 *
 *   UnrealEditor-Cmd.exe <Project>.uproject -run=DreamAnim [mode] [options]
 *
 * Modes
 *   (none)      build: compile the sources into Animation Blueprint assets and save them
 *   -Check      parse, includes, templates and semantic only; nothing is generated
 *   -Format     print each file's canonical text to stdout between `===== begin <file> =====` and
 *               `===== end <file> =====`; with -Write rewrite the files that differ (keeping each file's
 *               line ends, UTF-8 without BOM); with -Verify write nothing and count every file that is
 *               not canonical as an error
 *   -Decompile  Animation Blueprints to text: -Asset=<object or package path> (repeatable, or
 *               comma-separated) and/or -Under=<content path> (every Animation Blueprint below it)
 *
 * Options
 *   -File=<path>   one source (absolute, or relative to the project directory); a template library
 *                  stands for the documents that include it. Without it, every .dab and .dai under the
 *                  roots (-Format: .dat too)
 *   -Root=<dirs>   source roots instead of the project's and the enabled plugins' DAnim/ (comma-separated)
 *   -NoSave        build: generate in memory without saving packages
 *   -Out=<path>    decompile: a folder (the text goes where it would below DAnim/) or, for one asset, a file;
 *                  default: under the project's DAnim/
 *   -Stdout        decompile: print the text instead of writing it (-stdout also makes the engine echo its
 *                  log to stdout)
 *   -Force         decompile: overwrite a file that exists and differs (it may be a hand-written source)
 *   -Json=<file>   also write every diagnostic as JSON (what the VS Code extension reads)
 *
 * Every diagnostic is logged as `<file>(<line>,<col>): error DABnnnn: message`, then a one-line summary.
 * The exit code is the number of errors, so a CI gate tests for zero.
 */
UCLASS()
class UDreamAnimCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UDreamAnimCommandlet();

	virtual int32 Main(const FString& Params) override;
};
