// Copyright DreamMoon. All Rights Reserved.
//
// The pose recorder itself: two runs of the original ALS character must match exactly, the script must
// move the character and the pose must change. Without this, a pose comparison that passes proves nothing.

#include "Corpus/AlsPoseRecorder.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDabAlsPoseHarnessTest, "DreamAnim.Corpus.Als.PoseHarness",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FDabAlsPoseHarnessTest::RunTest(const FString& Parameters)
{
	using namespace UE::DreamAnim::Tests;
	FAlsPoseScript Script;
	FAlsPoseRecording First;
	FAlsPoseRecording Second;
	FString Error;
	if (!RecordAlsPoses(Script, First, Error) || !RecordAlsPoses(Script, Second, Error))
	{
		AddError(Error);
		return false;
	}

	AddInfo(First.Notes);
	const FString Difference = CompareAlsPoses(First, Second, 0.0001, 0.0001);
	if (!Difference.IsEmpty())
	{
		AddError(FString::Printf(TEXT("Two runs of the same Blueprints differ: %s"), *Difference));
	}

	const double Travelled = FVector::Dist2D(First.Locations[0], First.Locations.Last());
	TestTrue(FString::Printf(TEXT("the script moves the character (%.0f cm)"), Travelled), Travelled > 200.0);

	// Something must animate: the pose while running differs from the idle pose.
	const FString Moved = CompareAlsPoses(
		FAlsPoseRecording{ First.Bones, { First.Frames[10] }, { First.Locations[10] } },
		FAlsPoseRecording{ First.Bones, { First.Frames[90] }, { First.Locations[10] } }, 0.5, 0.5);
	TestFalse(TEXT("the pose changes between idle and running"), Moved.IsEmpty());
	AddInfo(FString::Printf(TEXT("%d bones, %d frames, travelled %.0f cm"), First.Bones.Num(), First.Frames.Num(), Travelled));
	return true;
}

#endif
