// Copyright DreamMoon. All Rights Reserved.

#include "DcrLayout.h"

namespace UE::DreamAnim::Rig::Gen
{
	using namespace UE::DreamAnim::Model;

	namespace
	{
		constexpr double ColumnGap = 120.0;
		constexpr double RowGap = 40.0;
		constexpr double CommentMargin = 40.0;
		constexpr double CommentTitleRoom = 50.0;

		/** The node a pin path starts with. */
		FString LayoutNodeOfPin(const FString& PinPath)
		{
			int32 Dot = INDEX_NONE;
			return PinPath.FindChar(TEXT('.'), Dot) ? PinPath.Left(Dot) : PinPath;
		}
	}

	void LayoutGraphNodes(const FDcrModelGraph& Graph, const TMap<FString, FVector2D>& Sizes, TMap<FString, FVector2D>& OutPositions)
	{
		// The nodes that take part, in the model's order.
		TArray<const FDcrModelNode*> Nodes;
		TMap<FString, int32> IndexByName;
		for (const FDcrModelNode& Node : Graph.Nodes)
		{
			if (Node.Kind == EDcrNodeKind::Comment || IndexByName.Contains(Node.Name))
			{
				continue;
			}
			IndexByName.Add(Node.Name, Nodes.Num());
			Nodes.Add(&Node);
		}

		const int32 Count = Nodes.Num();
		TArray<TPair<int32, int32>> Edges;
		Edges.Reserve(Graph.Links.Num());
		for (const FDcrModelLink& Link : Graph.Links)
		{
			const int32* From = IndexByName.Find(LayoutNodeOfPin(Link.Source));
			const int32* To = IndexByName.Find(LayoutNodeOfPin(Link.Target));
			if (From && To && *From != *To)
			{
				Edges.Emplace(*From, *To);
			}
		}

		// Longest path from the nodes nothing feeds. A cycle cannot settle, so the passes are bounded.
		TArray<int32> Column;
		Column.Init(0, Count);
		for (int32 Pass = 0; Pass <= Count; ++Pass)
		{
			bool bChanged = false;
			for (const TPair<int32, int32>& Edge : Edges)
			{
				if (Column[Edge.Value] < Column[Edge.Key] + 1)
				{
					Column[Edge.Value] = Column[Edge.Key] + 1;
					bChanged = true;
				}
			}
			if (!bChanged)
			{
				break;
			}
		}

		int32 ColumnCount = 0;
		for (const int32 Value : Column)
		{
			ColumnCount = FMath::Max(ColumnCount, Value + 1);
		}

		TArray<double> ColumnWidth;
		ColumnWidth.Init(0.0, ColumnCount);
		TArray<FVector2D> NodeSize;
		NodeSize.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FVector2D* Known = Sizes.Find(Nodes[Index]->Name);
			NodeSize.Add(Known ? *Known : DefaultNodeSize());
			ColumnWidth[Column[Index]] = FMath::Max(ColumnWidth[Column[Index]], NodeSize[Index].X);
		}

		TArray<double> ColumnX;
		ColumnX.Init(0.0, ColumnCount);
		double X = 0.0;
		for (int32 Index = 0; Index < ColumnCount; ++Index)
		{
			ColumnX[Index] = X;
			X += ColumnWidth[Index] + ColumnGap;
		}

		TArray<double> ColumnY;
		ColumnY.Init(0.0, ColumnCount);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const int32 Where = Column[Index];
			OutPositions.Add(Nodes[Index]->Name, FVector2D(ColumnX[Where], ColumnY[Where]));
			ColumnY[Where] += NodeSize[Index].Y + RowGap;
		}
	}

	bool FitCommentBox(const TArray<FString>& Members, const TMap<FString, FVector2D>& Positions, const TMap<FString, FVector2D>& Sizes, FVector2D& OutPosition, FVector2D& OutSize)
	{
		bool bAny = false;
		FVector2D Min(0.0, 0.0);
		FVector2D Max(0.0, 0.0);
		for (const FString& Member : Members)
		{
			const FVector2D* Position = Positions.Find(Member);
			if (!Position)
			{
				continue;
			}
			const FVector2D* Known = Sizes.Find(Member);
			const FVector2D Size = Known ? *Known : DefaultNodeSize();
			if (!bAny)
			{
				Min = *Position;
				Max = *Position + Size;
				bAny = true;
			}
			else
			{
				Min.X = FMath::Min(Min.X, Position->X);
				Min.Y = FMath::Min(Min.Y, Position->Y);
				Max.X = FMath::Max(Max.X, Position->X + Size.X);
				Max.Y = FMath::Max(Max.Y, Position->Y + Size.Y);
			}
		}
		if (!bAny)
		{
			return false;
		}
		OutPosition = FVector2D(Min.X - CommentMargin, Min.Y - CommentMargin - CommentTitleRoom);
		OutSize = FVector2D(Max.X - Min.X + 2.0 * CommentMargin, Max.Y - Min.Y + 2.0 * CommentMargin + CommentTitleRoom);
		return true;
	}
}
