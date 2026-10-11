// Copyright DreamMoon. All Rights Reserved.

#include "Driver/DabDriver.h"
#include "Driver/DabDriverPrivate.h"

#include "Lang/DabParser.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace UE::DreamAnim::Compiler::DriverPrivate
{
	/**
	 * Every name a source mentions that could be another DreamAnim asset: bare names (`AB_Als_Grounded`)
	 * and the last segment of every string (`Path(Plugin.ALS, "ALS/Character/AB_Als")`). More than the
	 * real references, which only costs an ordering constraint that was not needed.
	 */
	void CollectDabMentionedNames(const Lang::FDabNode& Node, TSet<FString>& OutNames)
	{
		if (const Lang::FDabNameExpr* Name = Node.As<Lang::FDabNameExpr>())
		{
			OutNames.Add(Name->Name);
		}
		else if (const Lang::FDabLiteralExpr* Literal = Node.As<Lang::FDabLiteralExpr>(); Literal && Literal->LiteralKind == Lang::EDabLiteralKind::String)
		{
			OutNames.Add(FPaths::GetBaseFilename(Literal->Text));
		}
		Lang::ForEachChild(Node, [&OutNames](const Lang::FDabNode& Child) { CollectDabMentionedNames(Child, OutNames); });
	}
}

namespace UE::DreamAnim::Compiler
{
	TArray<FString> OrderDabSourcesForBuild(const TArray<FString>& Files, const FDabSourceIndex& Index, const TArray<FString>& Roots)
	{
		TMap<FString, FDabSourceEntry> Entries;
		const auto EntryOf = [&Entries, &Index, &Roots](const FString& Path) -> const FDabSourceEntry&
		{
			if (const FDabSourceEntry* Known = Entries.Find(Path))
			{
				return *Known;
			}
			FDabSourceEntry Entry;
			if (const FDabSourceEntry* Indexed = Index.FindByFile(Path))
			{
				Entry = *Indexed;
			}
			else
			{
				DriverPrivate::ReadDabSourceEntry(Path, Roots, Entry);
			}
			return Entries.Add(Path, MoveTemp(Entry));
		};

		TMap<FString, TSet<FString>> NamesByFile;
		const auto OwnNamesOf = [&NamesByFile](const FString& Path) -> const TSet<FString>&
		{
			if (const TSet<FString>* Known = NamesByFile.Find(Path))
			{
				return *Known;
			}
			TSet<FString> Names;
			FString Text;
			if (FFileHelper::LoadFileToString(Text, *Path))
			{
				const Lang::FDabSourceText Source(Path, MoveTemp(Text));
				Lang::FDabDiagnosticSink Sink(Path);
				Lang::FDabParseOptions ParseOptions;
				ParseOptions.bKeepTrivia = false;
				const TUniquePtr<Lang::FDabFile> Parsed = Lang::ParseDab(Source, Sink, ParseOptions);
				if (Parsed->Document)
				{
					DriverPrivate::CollectDabMentionedNames(*Parsed->Document, Names);
				}
			}
			return NamesByFile.Add(Path, MoveTemp(Names));
		};

		// What each document mentions, through the templates it includes as well.
		TMap<FString, TSet<FString>> MentionedByFile;
		for (const FString& File : Files)
		{
			TSet<FString> Mentioned;
			TSet<FString> Visited;
			TArray<FString> Pending;
			Pending.Add(File);
			while (!Pending.IsEmpty())
			{
				const FString Current = Pending.Pop(EAllowShrinking::No);
				bool bAlreadyVisited = false;
				Visited.Add(Current, &bAlreadyVisited);
				if (bAlreadyVisited)
				{
					continue;
				}
				Mentioned.Append(OwnNamesOf(Current));
				Pending.Append(EntryOf(Current).Includes);
			}
			MentionedByFile.Add(File, MoveTemp(Mentioned));
		}

		TMap<FString, TArray<FString>> ProducersByName;
		for (const FString& File : Files)
		{
			const FString& AssetPath = EntryOf(File).AssetPath;
			if (!AssetPath.IsEmpty())
			{
				ProducersByName.FindOrAdd(FPackageName::ObjectPathToObjectName(AssetPath)).Add(File);
			}
		}

		// Control Rigs first (an Animation Blueprint may run one), then interfaces, then the rest; by path within each.
		const auto Rank = [](const FString& File)
		{
			switch (Lang::GetDabFileKindFromPath(File))
			{
			case Lang::EDabFileKind::ControlRig:
				return 0;
			case Lang::EDabFileKind::AnimLayerInterface:
				return 1;
			default:
				return 2;
			}
		};
		const auto Precedes = [&Rank](const FString& A, const FString& B)
		{
			const int32 RankA = Rank(A);
			const int32 RankB = Rank(B);
			return RankA != RankB ? RankA < RankB : A < B;
		};

		TMap<FString, TArray<FString>> DependsOn;
		for (const FString& File : Files)
		{
			const TSet<FString>& Mentioned = MentionedByFile.FindChecked(File);
			TArray<FString>& Dependencies = DependsOn.Add(File);
			for (const TPair<FString, TArray<FString>>& Producers : ProducersByName)
			{
				if (!Mentioned.Contains(Producers.Key))
				{
					continue;
				}
				for (const FString& Producer : Producers.Value)
				{
					if (!Producer.Equals(File, ESearchCase::IgnoreCase))
					{
						Dependencies.AddUnique(Producer);
					}
				}
			}
			Dependencies.Sort(Precedes);
		}

		TArray<FString> Sorted = Files;
		Sorted.Sort(Precedes);
		TArray<FString> Ordered;
		TSet<FString> Done;
		TSet<FString> Visiting;
		TFunction<void(const FString&)> Visit;
		Visit = [&Visit, &Ordered, &Done, &Visiting, &DependsOn](const FString& File)
		{
			if (Done.Contains(File) || Visiting.Contains(File))
			{
				return;
			}
			Visiting.Add(File);
			for (const FString& Dependency : DependsOn.FindChecked(File))
			{
				Visit(Dependency);
			}
			Visiting.Remove(File);
			Done.Add(File);
			Ordered.Add(File);
		};
		for (const FString& File : Sorted)
		{
			Visit(File);
		}
		return Ordered;
	}
}
