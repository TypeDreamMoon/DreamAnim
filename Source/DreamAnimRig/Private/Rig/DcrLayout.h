// Copyright DreamMoon. All Rights Reserved.
//
// Node layout for a generated graph. Layered and deterministic: a node's column is one more than the
// furthest column of the nodes that feed it (execute and data links alike), so events and entries stand
// at the left, and rows follow the model's node order. Positions are the node's top-left corner.

#pragma once

#include "CoreMinimal.h"
#include "Model/DcrModel.h"

namespace UE::DreamAnim::Rig::Gen
{
	/** The size a node is given when nothing better is known (the controller learns real sizes only from a graph editor). */
	inline FVector2D DefaultNodeSize()
	{
		return FVector2D(260.0, 120.0);
	}

	/**
	 * Positions for every node of the graph but its comments. Sizes holds the size of each node by name;
	 * a node it does not list gets DefaultNodeSize.
	 */
	void LayoutGraphNodes(const Model::FDcrModelGraph& Graph, const TMap<FString, FVector2D>& Sizes, TMap<FString, FVector2D>& OutPositions);

	/**
	 * The box of a comment that goes around the named nodes: their rectangles with a margin, and room on
	 * top for the title. False when none of the names has a position.
	 */
	bool FitCommentBox(const TArray<FString>& Members, const TMap<FString, FVector2D>& Positions, const TMap<FString, FVector2D>& Sizes, FVector2D& OutPosition, FVector2D& OutSize);
}
