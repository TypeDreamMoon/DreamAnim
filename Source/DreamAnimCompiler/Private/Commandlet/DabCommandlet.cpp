// Copyright DreamMoon. All Rights Reserved.

#include "Commandlet/DabCommandlet.h"

#include "Animation/AnimBlueprint.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Driver/DabDriver.h"
#include "Driver/DabDriverPrivate.h"
#include "HAL/FileManager.h"
#include "Lang/DabParser.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

#include <stdio.h>

DEFINE_LOG_CATEGORY_STATIC(LogDreamAnimCommandlet, Log, All);

namespace UE::DreamAnim::Compiler::CommandletPrivate
{
	enum class EDabCommandletMode : uint8
	{
		Build,
		Check,
		Format,
		Decompile,
	};

	/** One run: what was asked, and what it found. */
	struct FDabCommandletRun
	{
		EDabCommandletMode Mode = EDabCommandletMode::Build;
		bool bNoSave = false;
		bool bWrite = false;
		bool bVerify = false;
		bool bStdout = false;
		bool bForce = false;
		FString File;
		TArray<FString> Roots;
		TArray<FString> Assets;
		FString Under;
		FString Out;
		FString Json;

		TArray<Lang::FDabDiagnostic> Diagnostics;
		int32 NumErrors = 0;
		int32 NumWarnings = 0;

		/** Logs a diagnostic in the form tools read, keeps it for -Json and counts it. */
		void Report(const Lang::FDabDiagnostic& Diagnostic)
		{
			const FString Line = FormatDabDiagnosticLine(Diagnostic);
			switch (Diagnostic.Severity)
			{
			case Lang::EDabSeverity::Error:
				UE_LOG(LogDreamAnimCommandlet, Error, TEXT("%s"), *Line);
				++NumErrors;
				break;
			case Lang::EDabSeverity::Warning:
				UE_LOG(LogDreamAnimCommandlet, Warning, TEXT("%s"), *Line);
				++NumWarnings;
				break;
			default:
				UE_LOG(LogDreamAnimCommandlet, Display, TEXT("%s"), *Line);
				break;
			}
			if (!Diagnostic.Hint.IsEmpty())
			{
				UE_LOG(LogDreamAnimCommandlet, Display, TEXT("    hint: %s"), *Diagnostic.Hint);
			}
			Diagnostics.Add(Diagnostic);
		}

		/** A problem of the run itself (a file that cannot be read or written, a bad option): not a diagnostic of any text, but an error all the same. */
		void Fail(const FString& Message)
		{
			UE_LOG(LogDreamAnimCommandlet, Error, TEXT("%s"), *Message);
			++NumErrors;
		}
	};

	/** Splits `a,b;c` into its non-empty, trimmed parts. */
	void AppendDabListValue(const FString& Value, TArray<FString>& Out)
	{
		TArray<FString> Parts;
		Value.Replace(TEXT(";"), TEXT(",")).ParseIntoArray(Parts, TEXT(","), true);
		for (const FString& Part : Parts)
		{
			const FString Trimmed = Part.TrimStartAndEnd().TrimQuotes();
			if (!Trimmed.IsEmpty())
			{
				Out.AddUnique(Trimmed);
			}
		}
	}

	/** False, with the reason reported, when the options contradict each other. */
	bool ParseDabCommandletParams(const FString& Params, FDabCommandletRun& Run)
	{
		// The switches one by one rather than ParseCommandLine's map: -Asset may be given several times.
		TArray<FString> Tokens;
		TArray<FString> Switches;
		UCommandlet::ParseCommandLine(*Params, Tokens, Switches);

		int32 NumModes = 0;
		bool bRootsGiven = false;
		TArray<FString> GivenRoots;
		for (const FString& Switch : Switches)
		{
			FString Key = Switch;
			FString Value;
			if (Switch.Split(TEXT("="), &Key, &Value))
			{
				Value = Value.TrimQuotes();
				if (Key == TEXT("File"))
				{
					Run.File = Value;
				}
				else if (Key == TEXT("Root"))
				{
					bRootsGiven = true;
					AppendDabListValue(Value, GivenRoots);
				}
				else if (Key == TEXT("Asset"))
				{
					AppendDabListValue(Value, Run.Assets);
				}
				else if (Key == TEXT("Under"))
				{
					Run.Under = Value;
				}
				else if (Key == TEXT("Out"))
				{
					Run.Out = Value;
				}
				else if (Key == TEXT("Json"))
				{
					Run.Json = Value;
				}
				continue;
			}

			if (Key == TEXT("Check"))
			{
				Run.Mode = EDabCommandletMode::Check;
				++NumModes;
			}
			else if (Key == TEXT("Format"))
			{
				Run.Mode = EDabCommandletMode::Format;
				++NumModes;
			}
			else if (Key == TEXT("Decompile"))
			{
				Run.Mode = EDabCommandletMode::Decompile;
				++NumModes;
			}
			else if (Key == TEXT("NoSave"))
			{
				Run.bNoSave = true;
			}
			else if (Key == TEXT("Write"))
			{
				Run.bWrite = true;
			}
			else if (Key == TEXT("Verify"))
			{
				Run.bVerify = true;
			}
			else if (Key == TEXT("Stdout"))
			{
				Run.bStdout = true;
			}
			else if (Key == TEXT("Force"))
			{
				Run.bForce = true;
			}
		}

		if (bRootsGiven)
		{
			for (const FString& Root : GivenRoots)
			{
				Run.Roots.AddUnique(DriverPrivate::NormalizeDabPath(Root));
			}
		}
		else
		{
			Run.Roots = GetDabSourceRoots();
		}

		if (NumModes > 1)
		{
			Run.Fail(TEXT("Give at most one of -Check, -Format and -Decompile."));
			return false;
		}
		if (Run.bWrite && Run.bVerify)
		{
			Run.Fail(TEXT("-Write rewrites the files and -Verify only checks them; give one of the two."));
			return false;
		}
		if ((Run.bWrite || Run.bVerify) && Run.Mode != EDabCommandletMode::Format)
		{
			Run.Fail(TEXT("-Write and -Verify go with -Format."));
			return false;
		}
		if (Run.Mode == EDabCommandletMode::Decompile && Run.Assets.IsEmpty() && Run.Under.IsEmpty())
		{
			Run.Fail(TEXT("-Decompile needs -Asset=<object or package path> or -Under=<content path>."));
			return false;
		}
		return true;
	}

	// ---------------------------------------------------------------------------------------------
	// Text on stdout
	// ---------------------------------------------------------------------------------------------

	/**
	 * Straight to stdout rather than through the log: a log line carries a category and maybe a time, and
	 * the log reaches stdout only with the engine's -stdout. The log is flushed first so the text lands
	 * after the lines logged before it.
	 */
	void WriteDabStdout(const FString& Text)
	{
		GLog->Flush();
		const FTCHARToUTF8 Utf8(*Text);
		fwrite(Utf8.Get(), 1, static_cast<size_t>(Utf8.Length()), stdout);
		fflush(stdout);
	}

	void PrintDabTextBlock(const FString& Name, const FString& Text)
	{
		FString Block = FString::Printf(TEXT("===== begin %s =====\n"), *Name);
		Block += Text;
		if (!Text.IsEmpty() && !Text.EndsWith(TEXT("\n")))
		{
			Block += TEXT("\n");
		}
		Block += FString::Printf(TEXT("===== end %s =====\n"), *Name);
		WriteDabStdout(Block);
	}

	// ---------------------------------------------------------------------------------------------
	// Line ends
	// ---------------------------------------------------------------------------------------------

	/** The line end a file uses, by its first one; `\n` for a file of one line. */
	FString DetectDabLineEnd(const FString& Text)
	{
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			if (Text[Index] == TEXT('\r'))
			{
				return (Index + 1 < Text.Len() && Text[Index + 1] == TEXT('\n')) ? TEXT("\r\n") : TEXT("\r");
			}
			if (Text[Index] == TEXT('\n'))
			{
				return TEXT("\n");
			}
		}
		return TEXT("\n");
	}

	/** Text with every line end made LineEnd (block comments may carry the file's own). */
	FString WithDabLineEnds(const FString& Text, const FString& LineEnd)
	{
		FString Result = Text.Replace(TEXT("\r\n"), TEXT("\n")).Replace(TEXT("\r"), TEXT("\n"));
		if (!LineEnd.Equals(TEXT("\n"), ESearchCase::CaseSensitive))
		{
			Result.ReplaceInline(TEXT("\n"), *LineEnd, ESearchCase::CaseSensitive);
		}
		return Result;
	}

	/** The 1-based line where two texts first differ, line ends aside; 1 when only the line ends differ. */
	int32 FindFirstDifferentDabLine(const FString& A, const FString& B)
	{
		TArray<FString> LinesA;
		TArray<FString> LinesB;
		WithDabLineEnds(A, TEXT("\n")).ParseIntoArray(LinesA, TEXT("\n"), false);
		WithDabLineEnds(B, TEXT("\n")).ParseIntoArray(LinesB, TEXT("\n"), false);
		for (int32 Index = 0; Index < FMath::Max(LinesA.Num(), LinesB.Num()); ++Index)
		{
			if (!LinesA.IsValidIndex(Index) || !LinesB.IsValidIndex(Index) || !LinesA[Index].Equals(LinesB[Index], ESearchCase::CaseSensitive))
			{
				return Index + 1;
			}
		}
		return 1;
	}

	// ---------------------------------------------------------------------------------------------
	// Which files
	// ---------------------------------------------------------------------------------------------

	/**
	 * The documents a build or check covers: -File (a template library standing for the documents that
	 * include it), else every .dab and .dai the index holds. False when -File does not name a source.
	 */
	bool GatherDabDocuments(FDabCommandletRun& Run, const FDabSourceIndex& Index, TArray<FString>& OutFiles)
	{
		if (Run.File.IsEmpty())
		{
			for (const FDabSourceEntry& Entry : Index.GetEntries())
			{
				// A library is read through the documents that include it, or on its own when none does.
				if (Lang::GetDabFileKindFromPath(Entry.FilePath) != Lang::EDabFileKind::Templates || Index.FindDependents(Entry.FilePath).IsEmpty())
				{
					OutFiles.Add(Entry.FilePath);
				}
			}
			return true;
		}

		const FString Path = DriverPrivate::NormalizeDabPath(Run.File);
		if (!IsDabSourceFile(Path))
		{
			Run.Fail(FString::Printf(TEXT("%s is not a DreamAnim source (.dab, .dai or .dat)."), *Path));
			return false;
		}
		if (!IFileManager::Get().FileExists(*Path))
		{
			Run.Fail(FString::Printf(TEXT("%s does not exist."), *Path));
			return false;
		}
		if (Lang::GetDabFileKindFromPath(Path) == Lang::EDabFileKind::Templates)
		{
			// A library makes nothing itself; a change to it is a change to every document that includes it.
			OutFiles = Index.FindDependents(Path);
			if (OutFiles.IsEmpty())
			{
				// Still read on its own, so a mistake in it shows before anything includes it.
				UE_LOG(LogDreamAnimCommandlet, Display, TEXT("%s is a template library no document under the roots includes; it is checked on its own."), *Path);
				OutFiles.Add(Path);
			}
			else
			{
				UE_LOG(LogDreamAnimCommandlet, Display, TEXT("%s is a template library; %d document(s) under the roots include it."), *Path, OutFiles.Num());
			}
			return true;
		}
		OutFiles.Add(Path);
		return true;
	}

	// ---------------------------------------------------------------------------------------------
	// Modes
	// ---------------------------------------------------------------------------------------------

	void RunDabBuildOrCheck(FDabCommandletRun& Run)
	{
		const bool bBuild = Run.Mode == EDabCommandletMode::Build;
		const TCHAR* const ModeName = bBuild ? TEXT("build") : TEXT("check");

		FDabSourceIndex Index;
		Index.Scan(Run.Roots);
		TArray<FString> Files;
		if (!GatherDabDocuments(Run, Index, Files))
		{
			UE_LOG(LogDreamAnimCommandlet, Display, TEXT("DreamAnim %s: nothing done, %d error(s)"), ModeName, Run.NumErrors);
			return;
		}
		if (Files.IsEmpty() && Run.File.IsEmpty())
		{
			UE_LOG(LogDreamAnimCommandlet, Warning, TEXT("No .dab or .dai sources under %s."),
				Run.Roots.IsEmpty() ? TEXT("any DAnim/ folder (none in the project or its enabled plugins)") : *FString::Join(Run.Roots, TEXT(", ")));
		}
		if (bBuild && Files.Num() > 1)
		{
			Files = OrderDabSourcesForBuild(Files, Index, Run.Roots);
		}

		FDabCompileOptions Options;
		Options.bGenerate = bBuild;
		Options.bSave = !Run.bNoSave;
		Options.IncludeRoots = Run.Roots;
		// A source may name a Blueprint another one makes before that one is built.
		Options.ResolveDreamAnimAsset = [&Index](const FString& AssetName) { return Index.FindByAssetName(AssetName); };

		int32 NumGenerated = 0;
		for (const FString& File : Files)
		{
			const FDabCompileResult Result = CompileDabFile(File, Options);
			for (const Lang::FDabDiagnostic& Diagnostic : Result.Diagnostics)
			{
				Run.Report(Diagnostic);
			}
			if (bBuild && Result.Blueprint && Result.Succeeded())
			{
				++NumGenerated;
				UE_LOG(LogDreamAnimCommandlet, Display, TEXT("Generated %s from %s"), *Result.Blueprint->GetPathName(), *File);
			}
		}

		if (bBuild)
		{
			UE_LOG(LogDreamAnimCommandlet, Display, TEXT("DreamAnim build: %d file(s), %d generated%s, %d error(s), %d warning(s)"),
				Files.Num(), NumGenerated, Run.bNoSave ? TEXT(" (in memory, not saved)") : TEXT(" and saved"), Run.NumErrors, Run.NumWarnings);
		}
		else
		{
			UE_LOG(LogDreamAnimCommandlet, Display, TEXT("DreamAnim check: %d file(s), %d error(s), %d warning(s)"),
				Files.Num(), Run.NumErrors, Run.NumWarnings);
		}
	}

	void RunDabFormat(FDabCommandletRun& Run)
	{
		TArray<FString> Files;
		if (!Run.File.IsEmpty())
		{
			const FString Path = DriverPrivate::NormalizeDabPath(Run.File);
			if (!IsDabSourceFile(Path))
			{
				Run.Fail(FString::Printf(TEXT("%s is not a DreamAnim source (.dab, .dai or .dat)."), *Path));
			}
			else
			{
				Files.Add(Path);
			}
		}
		else
		{
			FDabSourceIndex Index;
			Index.Scan(Run.Roots);
			for (const FDabSourceEntry& Entry : Index.GetEntries())
			{
				Files.Add(Entry.FilePath);
			}
		}

		int32 NumRewritten = 0;
		int32 NumNotCanonical = 0;
		for (const FString& File : Files)
		{
			FString Original;
			if (!FFileHelper::LoadFileToString(Original, *File))
			{
				Run.Fail(FString::Printf(TEXT("Cannot read %s."), *File));
				continue;
			}
			FString Canonical;
			TArray<Lang::FDabDiagnostic> Diagnostics;
			const bool bFormatted = FormatDabText(File, Original, Canonical, Diagnostics);
			for (const Lang::FDabDiagnostic& Diagnostic : Diagnostics)
			{
				Run.Report(Diagnostic);
			}
			if (!bFormatted)
			{
				continue;
			}

			// The file's own line ends: a reformat should not also turn every line of a CRLF checkout into a change.
			const FString InFileStyle = WithDabLineEnds(Canonical, DetectDabLineEnd(Original));
			const bool bDiffers = !InFileStyle.Equals(Original, ESearchCase::CaseSensitive);
			if (Run.bWrite)
			{
				if (!bDiffers)
				{
					continue;
				}
				if (FFileHelper::SaveStringToFile(InFileStyle, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
				{
					++NumRewritten;
					UE_LOG(LogDreamAnimCommandlet, Display, TEXT("Formatted %s"), *File);
				}
				else
				{
					Run.Fail(FString::Printf(TEXT("Cannot write %s."), *File));
				}
			}
			else if (Run.bVerify)
			{
				if (bDiffers)
				{
					++NumNotCanonical;
					Run.Fail(FString::Printf(TEXT("%s(%d,1): error: not in canonical format (dab fmt -Write rewrites it)."),
						*File, FindFirstDifferentDabLine(Original, InFileStyle)));
				}
			}
			else
			{
				PrintDabTextBlock(File, WithDabLineEnds(Canonical, TEXT("\n")));
			}
		}

		if (Run.bWrite)
		{
			UE_LOG(LogDreamAnimCommandlet, Display, TEXT("DreamAnim format: %d file(s), %d rewritten, %d error(s), %d warning(s)"),
				Files.Num(), NumRewritten, Run.NumErrors, Run.NumWarnings);
		}
		else if (Run.bVerify)
		{
			UE_LOG(LogDreamAnimCommandlet, Display, TEXT("DreamAnim format: %d file(s), %d not canonical, %d error(s), %d warning(s)"),
				Files.Num(), NumNotCanonical, Run.NumErrors, Run.NumWarnings);
		}
		else
		{
			UE_LOG(LogDreamAnimCommandlet, Display, TEXT("DreamAnim format: %d file(s), %d error(s), %d warning(s)"),
				Files.Num(), Run.NumErrors, Run.NumWarnings);
		}
	}

	void RunDabDecompile(FDabCommandletRun& Run)
	{
		TArray<FString> ObjectPaths;
		for (const FString& Asset : Run.Assets)
		{
			ObjectPaths.AddUnique(DriverPrivate::ToDabObjectPath(Asset));
		}
		if (!Run.Under.IsEmpty())
		{
			FString Under = Run.Under;
			while (Under.Len() > 1 && Under.EndsWith(TEXT("/")))
			{
				Under.LeftChopInline(1);
			}
			// The registry dumps a callstack for a path outside every mount; say what is wrong instead.
			if (!FPackageName::IsValidPath(Under + TEXT("/")))
			{
				Run.Fail(FString::Printf(TEXT("%s is not a content path: it starts with a mount such as /Game/ or /ALS/."), *Under));
				UE_LOG(LogDreamAnimCommandlet, Display, TEXT("DreamAnim decompile: nothing done, %d error(s)"), Run.NumErrors);
				return;
			}
			IAssetRegistry& Registry = IAssetRegistry::GetChecked();
			Registry.ScanPathsSynchronous({ Under });
			FARFilter Filter;
			Filter.PackagePaths.Add(FName(*Under));
			Filter.bRecursivePaths = true;
			Filter.ClassPaths.Add(UAnimBlueprint::StaticClass()->GetClassPathName());
			Filter.bRecursiveClasses = true;
			TArray<FAssetData> Found;
			Registry.GetAssets(Filter, Found);
			Found.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
			if (Found.IsEmpty())
			{
				Run.Fail(FString::Printf(TEXT("No Animation Blueprint under %s."), *Under));
			}
			for (const FAssetData& Asset : Found)
			{
				ObjectPaths.AddUnique(Asset.GetObjectPathString());
			}
		}

		const FString OutPath = Run.Out.IsEmpty() ? FString() : DriverPrivate::NormalizeDabPath(Run.Out);
		const bool bOutIsFile = !OutPath.IsEmpty() && IsDabSourceFile(OutPath);
		if (bOutIsFile && ObjectPaths.Num() > 1 && !Run.bStdout)
		{
			Run.Fail(FString::Printf(TEXT("-Out names one file, but %d Blueprints are being decompiled; give a folder."), ObjectPaths.Num()));
			ObjectPaths.Reset();
		}

		int32 NumDone = 0;
		int32 NumUnchanged = 0;
		int32 NumUnsupported = 0;
		for (const FString& ObjectPath : ObjectPaths)
		{
			const UAnimBlueprint* Blueprint = LoadObject<UAnimBlueprint>(nullptr, *ObjectPath);
			if (!Blueprint)
			{
				Run.Fail(FString::Printf(TEXT("Cannot load the Animation Blueprint %s."), *ObjectPath));
				continue;
			}
			const FDabDecompileResult Decompiled = DecompileDabBlueprint(*Blueprint);
			const int32 ErrorsBefore = Run.NumErrors;
			for (const Lang::FDabDiagnostic& Diagnostic : Decompiled.Diagnostics)
			{
				Run.Report(Diagnostic);
			}
			if (!Decompiled.bSucceeded)
			{
				if (Run.NumErrors == ErrorsBefore)
				{
					Run.Fail(FString::Printf(TEXT("%s could not be decompiled."), *ObjectPath));
				}
				continue;
			}
			NumUnsupported += Decompiled.NumUnsupported;

			if (Run.bStdout)
			{
				PrintDabTextBlock(Blueprint->GetPathName(), Decompiled.Text);
				++NumDone;
				continue;
			}

			const FString Target = bOutIsFile ? OutPath
				: OutPath.IsEmpty() ? GetDefaultDecompilePath(*Blueprint)
				: DriverPrivate::NormalizeDabPath(FPaths::Combine(OutPath, DriverPrivate::GetDabDecompileRelativePath(Blueprint->GetPackage()->GetName())
					+ DriverPrivate::GetDabDecompileExtension(*Blueprint)));
			FString Text = Decompiled.Text;
			FString Existing;
			if (IFileManager::Get().FileExists(*Target) && FFileHelper::LoadFileToString(Existing, *Target))
			{
				Text = WithDabLineEnds(Text, DetectDabLineEnd(Existing));
				if (Text.Equals(Existing, ESearchCase::CaseSensitive))
				{
					++NumUnchanged;
					UE_LOG(LogDreamAnimCommandlet, Display, TEXT("Unchanged %s"), *Target);
					continue;
				}
				// The default place is DAnim/, where the sources live: never replace one without being told to.
				if (!Run.bForce)
				{
					Run.Fail(FString::Printf(TEXT("%s exists and differs from %s decompiled; -Force overwrites it (it may be a source written by hand)."),
						*Target, *ObjectPath));
					continue;
				}
			}
			if (!FFileHelper::SaveStringToFile(Text, *Target, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				Run.Fail(FString::Printf(TEXT("Cannot write %s."), *Target));
				continue;
			}
			++NumDone;
			UE_LOG(LogDreamAnimCommandlet, Display, TEXT("Wrote %s (%d unsupported construct(s))"), *Target, Decompiled.NumUnsupported);
		}

		UE_LOG(LogDreamAnimCommandlet, Display, TEXT("DreamAnim decompile: %d asset(s), %d %s, %d unchanged, %d unsupported construct(s), %d error(s), %d warning(s)"),
			ObjectPaths.Num(), NumDone, Run.bStdout ? TEXT("printed") : TEXT("written"), NumUnchanged, NumUnsupported, Run.NumErrors, Run.NumWarnings);
	}
}

UDreamAnimCommandlet::UDreamAnimCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	// The diagnostics are already logged once each, and the exit code is the error count of the sources,
	// not whatever else the engine happened to log as an error.
	ShowErrorCount = false;
	UseCommandletResultAsExitCode = true;
}

int32 UDreamAnimCommandlet::Main(const FString& Params)
{
	using namespace UE::DreamAnim::Compiler;
	using namespace UE::DreamAnim::Compiler::CommandletPrivate;

	FDabCommandletRun Run;
	if (!ParseDabCommandletParams(Params, Run))
	{
		UE_LOG(LogDreamAnimCommandlet, Display, TEXT("DreamAnim: nothing done, %d error(s)"), Run.NumErrors);
		return Run.NumErrors;
	}

	// A commandlet's asset registry starts empty; bare asset names in the text are looked up in it. The
	// editor scans on its own.
	if (Run.Mode != EDabCommandletMode::Format && IsRunningCommandlet())
	{
		IAssetRegistry::GetChecked().SearchAllAssets(true);
	}

	switch (Run.Mode)
	{
	case EDabCommandletMode::Build:
	case EDabCommandletMode::Check:
		RunDabBuildOrCheck(Run);
		break;
	case EDabCommandletMode::Format:
		RunDabFormat(Run);
		break;
	case EDabCommandletMode::Decompile:
		RunDabDecompile(Run);
		break;
	}

	if (!Run.Json.IsEmpty())
	{
		const FString JsonPath = DriverPrivate::NormalizeDabPath(Run.Json);
		if (!FFileHelper::SaveStringToFile(DabDiagnosticsToJson(Run.Diagnostics), *JsonPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Run.Fail(FString::Printf(TEXT("Cannot write %s."), *JsonPath));
		}
	}
	return Run.NumErrors;
}
