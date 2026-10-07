// Copyright DreamMoon. All Rights Reserved.
//
// The three-way merge used when the text changed on disk while the Blueprint had changes of its own.

#include "Misc/AutomationTest.h"
#include "WriteBack/DabTextMerge.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::DreamAnim::Tests::TextMergeTests
{
	using namespace UE::DreamAnim::Compiler;

	IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabTextMergeTest, "DreamAnim.WriteBack.Merge",
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

	bool FDabTextMergeTest::RunTest(const FString& Parameters)
	{
		const FString Base = TEXT("A {\n    One = 1;\n    Two = 2;\n\n    Three = 3;\n    Four = 4;\n}\n");
		FString Merged;
		TArray<FDabMergeConflict> Conflicts;

		// Apart: both are taken.
		TestTrue(TEXT("changes apart merge"), MergeDabTexts(Base,
			TEXT("A {\n    One = 10;\n    Two = 2;\n\n    Three = 3;\n    Four = 4;\n}\n"),
			TEXT("A {\n    One = 1;\n    Two = 2;\n\n    Three = 3;\n    Four = 40;\n    Five = 5;\n}\n"), Merged, Conflicts));
		TestEqualSensitive(TEXT("both changes"), Merged, FString(TEXT("A {\n    One = 10;\n    Two = 2;\n\n    Three = 3;\n    Four = 40;\n    Five = 5;\n}\n")));

		// The same change on both sides is taken once.
		TestTrue(TEXT("the same change merges"), MergeDabTexts(Base,
			TEXT("A {\n    One = 1;\n    Two = 20;\n\n    Three = 3;\n    Four = 4;\n}\n"),
			TEXT("A {\n    One = 1;\n    Two = 20;\n\n    Three = 3;\n    Four = 4;\n}\n"), Merged, Conflicts));
		TestEqualSensitive(TEXT("taken once"), Merged, FString(TEXT("A {\n    One = 1;\n    Two = 20;\n\n    Three = 3;\n    Four = 4;\n}\n")));

		// The same line changed differently: a conflict, nothing merged.
		TestFalse(TEXT("one line changed two ways conflicts"), MergeDabTexts(Base,
			TEXT("A {\n    One = 1;\n    Two = 21;\n\n    Three = 3;\n    Four = 4;\n}\n"),
			TEXT("A {\n    One = 1;\n    Two = 22;\n\n    Three = 3;\n    Four = 4;\n}\n"), Merged, Conflicts));
		TestTrue(TEXT("nothing merged"), Merged.IsEmpty());
		if (TestEqual(TEXT("one conflict"), Conflicts.Num(), 1))
		{
			TestEqual(TEXT("at the line in the base"), Conflicts[0].Line, 3);
			TestEqualSensitive(TEXT("ours"), Conflicts[0].Ours, FString(TEXT("    Two = 21;")));
			TestEqualSensitive(TEXT("theirs"), Conflicts[0].Theirs, FString(TEXT("    Two = 22;")));
		}

		// Neighbouring lines are not taken apart.
		TestFalse(TEXT("neighbouring lines conflict"), MergeDabTexts(Base,
			TEXT("A {\n    One = 11;\n    Two = 2;\n\n    Three = 3;\n    Four = 4;\n}\n"),
			TEXT("A {\n    One = 1;\n    Two = 22;\n\n    Three = 3;\n    Four = 4;\n}\n"), Merged, Conflicts));

		// Line ends stay as they are.
		const FString CrlfBase = Base.Replace(TEXT("\n"), TEXT("\r\n"));
		TestTrue(TEXT("CRLF text merges"), MergeDabTexts(CrlfBase,
			CrlfBase.Replace(TEXT("One = 1;"), TEXT("One = 10;")),
			CrlfBase.Replace(TEXT("Four = 4;"), TEXT("Four = 40;")), Merged, Conflicts));
		TestEqualSensitive(TEXT("CRLF kept"), Merged, CrlfBase.Replace(TEXT("One = 1;"), TEXT("One = 10;")).Replace(TEXT("Four = 4;"), TEXT("Four = 40;")));
		return true;
	}
}

#endif
