// Copyright DreamMoon. All Rights Reserved.
//
// What the canonical dumps of the models (DumpDabModel, DumpDcrModel) write their text with.

#pragma once

#include "CoreMinimal.h"

namespace UE::DreamAnim::Model::DumpText
{
	// FString's operator< ignores case; a canonical dump must not depend on it.
	inline int32 CompareText(const FString& A, const FString& B)
	{
		return A.Compare(B, ESearchCase::CaseSensitive);
	}

	inline bool LessText(const FString& A, const FString& B)
	{
		return CompareText(A, B) < 0;
	}

	/** Text on a line of its own, two spaces in for each level of Depth. */
	inline void Line(FString& Out, int32 Depth, const FString& Text)
	{
		for (int32 Level = 0; Level < Depth; ++Level)
		{
			Out += TEXT("  ");
		}
		Out += Text;
		Out += TEXT("\n");
	}
}
