// Copyright DreamMoon. All Rights Reserved.
//
// Where sources live and which document makes which asset: the source roots, the path spellings every
// part of the driver compares, and the source index.

#include "Driver/DabDriver.h"
#include "Driver/DabDriverPrivate.h"

#include "Animation/AnimBlueprint.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Lang/DabParser.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace UE::DreamAnim::Compiler::DriverPrivate
{
	FString NormalizeDabPath(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return FString();
		}
		// FPaths::ConvertRelativePathToFull alone would resolve against the engine's binaries folder; a
		// path a user writes on the command line or in a setting means the project.
		FString Result = FPaths::IsRelative(Path)
			? FPaths::ConvertRelativePathToFull(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), Path)
			: FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Result);
		FPaths::CollapseRelativeDirectories(Result);
		// A drive root keeps its slash: `C:` alone means the drive's current directory.
		while (Result.Len() > 3 && Result.EndsWith(TEXT("/")))
		{
			Result.LeftChopInline(1);
		}
		return Result;
	}

	FString MakeDabDocumentAssetPath(const FString& Name, const FString& Root)
	{
		FString Mount = TEXT("Game");
		if (!Root.IsEmpty())
		{
			if (Root == TEXT("Game") || Root == TEXT("Engine"))
			{
				Mount = Root;
			}
			else if (Root.StartsWith(TEXT("Plugin."), ESearchCase::CaseSensitive) && Root.Len() > 7)
			{
				Mount = Root.RightChop(7);
			}
			else
			{
				return FString();
			}
		}
		FString Relative = Name;
		Relative.RemoveFromStart(TEXT("/"));
		Relative.RemoveFromEnd(TEXT("/"));
		if (Relative.IsEmpty())
		{
			return FString();
		}
		int32 Slash = INDEX_NONE;
		Relative.FindLastChar(TEXT('/'), Slash);
		const FString AssetName = Relative.Mid(Slash + 1);
		return FString::Printf(TEXT("/%s/%s.%s"), *Mount, *Relative, *AssetName);
	}

	FString ToDabObjectPath(const FString& AssetPath)
	{
		FString Path = AssetPath.TrimStartAndEnd();
		if (Path.Contains(TEXT("'")))
		{
			Path = FPackageName::ExportTextPathToObjectPath(Path);
		}
		int32 Slash = INDEX_NONE;
		Path.FindLastChar(TEXT('/'), Slash);
		const bool bHasObjectName = Path.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd) > Slash;
		if (!bHasObjectName && Slash != INDEX_NONE && Slash + 1 < Path.Len())
		{
			Path += TEXT(".") + Path.Mid(Slash + 1);
		}
		return Path;
	}

	FString GetDabDecompileRelativePath(const FString& PackageName)
	{
		FString Relative = PackageName;
		Relative.RemoveFromStart(TEXT("/"));
		Relative.RemoveFromStart(TEXT("Game/"), ESearchCase::CaseSensitive);
		return Relative;
	}

	const TCHAR* GetDabDecompileExtension(const UAnimBlueprint& Blueprint)
	{
		return Blueprint.BlueprintType == BPTYPE_Interface ? TEXT(".dai") : TEXT(".dab");
	}

	bool ResolveDabIncludePath(const FString& FromFile, const FString& IncludePath, const TArray<FString>& Roots, FString& OutPath)
	{
		// The candidates and their order are FDabFileSystemIncludeLoader's: the index must name the file the
		// compile will read.
		TArray<FString> Candidates;
		if (!FPaths::IsRelative(IncludePath))
		{
			Candidates.Add(IncludePath);
		}
		else
		{
			Candidates.Add(FPaths::Combine(FPaths::GetPath(FromFile), IncludePath));
			for (const FString& Root : Roots)
			{
				Candidates.Add(FPaths::Combine(Root, IncludePath));
			}
		}
		for (const FString& Candidate : Candidates)
		{
			const FString Normalized = NormalizeDabPath(Candidate);
			if (IFileManager::Get().FileExists(*Normalized))
			{
				OutPath = Normalized;
				return true;
			}
		}
		OutPath = NormalizeDabPath(Candidates[0]);
		return false;
	}

	/** The document kind a file's extension promises, for a file whose document did not parse. */
	Lang::EDabDocumentKind GetDabDocumentKindFromExtension(const FString& FilePath)
	{
		switch (Lang::GetDabFileKindFromPath(FilePath))
		{
		case Lang::EDabFileKind::AnimBlueprint:
			return Lang::EDabDocumentKind::AnimBlueprint;
		case Lang::EDabFileKind::AnimLayerInterface:
			return Lang::EDabDocumentKind::AnimLayerInterface;
		case Lang::EDabFileKind::Templates:
			return Lang::EDabDocumentKind::AnimTemplates;
		default:
			return Lang::EDabDocumentKind::Unknown;
		}
	}

	bool ReadDabSourceEntry(const FString& FilePath, const TArray<FString>& Roots, FDabSourceEntry& OutEntry)
	{
		OutEntry = FDabSourceEntry();
		OutEntry.FilePath = NormalizeDabPath(FilePath);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *OutEntry.FilePath))
		{
			return false;
		}

		const Lang::FDabSourceText Source(OutEntry.FilePath, MoveTemp(Text));
		Lang::FDabDiagnosticSink Sink(OutEntry.FilePath);
		Lang::FDabParseOptions ParseOptions;
		ParseOptions.bKeepTrivia = false;
		const TUniquePtr<Lang::FDabFile> File = Lang::ParseDab(Source, Sink, ParseOptions);

		const Lang::FDabDocument* Document = File->Document.Get();
		OutEntry.Kind = Document ? Document->DocumentKind : GetDabDocumentKindFromExtension(OutEntry.FilePath);
		if (Document && OutEntry.Kind != Lang::EDabDocumentKind::AnimTemplates && OutEntry.Kind != Lang::EDabDocumentKind::Unknown)
		{
			OutEntry.AssetPath = MakeDabDocumentAssetPath(Document->GetHeaderString(TEXT("Name")), Document->GetHeaderString(TEXT("Root")));
		}
		for (const TUniquePtr<Lang::FDabInclude>& Include : File->Includes)
		{
			if (Include && !Include->Path.IsEmpty())
			{
				FString Resolved;
				ResolveDabIncludePath(OutEntry.FilePath, Include->Path, Roots, Resolved);
				OutEntry.Includes.AddUnique(Resolved);
			}
		}
		return true;
	}
}

namespace UE::DreamAnim::Compiler
{
	TArray<FString> GetDabSourceRoots()
	{
		TArray<FString> Roots;
		const auto AddRoot = [&Roots](const FString& BaseDir)
		{
			const FString Root = DriverPrivate::NormalizeDabPath(FPaths::Combine(BaseDir, TEXT("DAnim")));
			if (IFileManager::Get().DirectoryExists(*Root))
			{
				Roots.AddUnique(Root);
			}
		};

		AddRoot(FPaths::ProjectDir());
		// By name, so the order includes are tried in does not depend on how the plugins were discovered.
		TArray<TSharedRef<IPlugin>> Plugins = IPluginManager::Get().GetEnabledPlugins();
		Plugins.Sort([](const TSharedRef<IPlugin>& A, const TSharedRef<IPlugin>& B) { return A->GetName() < B->GetName(); });
		for (const TSharedRef<IPlugin>& Plugin : Plugins)
		{
			AddRoot(Plugin->GetBaseDir());
		}
		return Roots;
	}

	bool IsDabSourceFile(const FString& FilePath)
	{
		return Lang::GetDabFileKindFromPath(FilePath) != Lang::EDabFileKind::Unknown;
	}

	void FDabSourceIndex::Scan(const TArray<FString>& InRoots)
	{
		TArray<FString> NewRoots;
		for (const FString& Root : InRoots)
		{
			const FString Normalized = DriverPrivate::NormalizeDabPath(Root);
			if (!Normalized.IsEmpty())
			{
				NewRoots.AddUnique(Normalized);
			}
		}
		Roots = MoveTemp(NewRoots);
		Entries.Reset();

		TSet<FString> Seen;
		TArray<FString> Files;
		for (const FString& Root : Roots)
		{
			TArray<FString> Found;
			IFileManager::Get().FindFilesRecursive(Found, *Root, TEXT("*"), true, false);
			for (const FString& File : Found)
			{
				if (!IsDabSourceFile(File))
				{
					continue;
				}
				FString Normalized = DriverPrivate::NormalizeDabPath(File);
				bool bAlreadySeen = false;
				Seen.Add(Normalized, &bAlreadySeen);
				if (!bAlreadySeen)
				{
					Files.Add(MoveTemp(Normalized));
				}
			}
		}
		Files.Sort();

		Entries.Reserve(Files.Num());
		for (const FString& File : Files)
		{
			FDabSourceEntry Entry;
			if (DriverPrivate::ReadDabSourceEntry(File, Roots, Entry))
			{
				Entries.Add(MoveTemp(Entry));
			}
		}
	}

	void FDabSourceIndex::Rescan(const FString& FilePath)
	{
		const FString Path = DriverPrivate::NormalizeDabPath(FilePath);
		const int32 Index = Entries.IndexOfByPredicate([&Path](const FDabSourceEntry& Entry) { return Entry.FilePath.Equals(Path, ESearchCase::IgnoreCase); });
		const bool bExists = IsDabSourceFile(Path) && IFileManager::Get().FileExists(*Path);

		if (Index != INDEX_NONE && bExists)
		{
			// An edit changes only what this file says; every other entry still resolves the same way.
			FDabSourceEntry Entry;
			if (DriverPrivate::ReadDabSourceEntry(Path, Roots, Entry))
			{
				Entries[Index] = MoveTemp(Entry);
				return;
			}
		}
		else if (Index == INDEX_NONE && !bExists)
		{
			return;
		}

		// A file appeared or went away: an #include anywhere may now resolve to another file (an earlier
		// root shadows a later one), so every entry is read again.
		const TArray<FString> SavedRoots = Roots;
		Scan(SavedRoots);
	}

	const FDabSourceEntry* FDabSourceIndex::FindByFile(const FString& FilePath) const
	{
		const FString Path = DriverPrivate::NormalizeDabPath(FilePath);
		return Entries.FindByPredicate([&Path](const FDabSourceEntry& Entry) { return Entry.FilePath.Equals(Path, ESearchCase::IgnoreCase); });
	}

	const FDabSourceEntry* FDabSourceIndex::FindByAsset(const FString& AssetPath) const
	{
		const FString ObjectPath = DriverPrivate::ToDabObjectPath(AssetPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		// Package names are case-insensitive, as the engine compares them.
		return Entries.FindByPredicate([&ObjectPath](const FDabSourceEntry& Entry)
		{
			return !Entry.AssetPath.IsEmpty() && Entry.AssetPath.Equals(ObjectPath, ESearchCase::IgnoreCase);
		});
	}

	TArray<FString> FDabSourceIndex::FindDependents(const FString& FilePath) const
	{
		const FString Start = DriverPrivate::NormalizeDabPath(FilePath);
		TSet<FString> Reached;
		Reached.Add(Start);
		TArray<FString> Frontier;
		Frontier.Add(Start);
		TArray<FString> Dependents;

		while (!Frontier.IsEmpty())
		{
			const FString Current = Frontier.Pop(EAllowShrinking::No);
			for (const FDabSourceEntry& Entry : Entries)
			{
				if (Reached.Contains(Entry.FilePath)
					|| !Entry.Includes.ContainsByPredicate([&Current](const FString& Include) { return Include.Equals(Current, ESearchCase::IgnoreCase); }))
				{
					continue;
				}
				Reached.Add(Entry.FilePath);
				// A library passes the change on to whatever includes it; only documents are rebuilt.
				Frontier.Add(Entry.FilePath);
				if (Entry.Kind != Lang::EDabDocumentKind::AnimTemplates)
				{
					Dependents.Add(Entry.FilePath);
				}
			}
		}
		Dependents.Sort();
		return Dependents;
	}
}
