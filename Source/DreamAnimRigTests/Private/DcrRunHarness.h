// Copyright DreamMoon. All Rights Reserved.
//
// Runs a rig frame by frame and records where everything ended up, so two rigs that should behave the
// same can be compared: every bone, null and control's global transform, every curve, every variable.
// The same rig run twice records the same numbers to the last bit, so comparisons default to exact.

#pragma once

#include "CoreMinimal.h"
#include "Rig/DcrAsset.h"
#include "Rigs/RigHierarchyDefines.h"

class UControlRig;

namespace UE::DreamAnim::Rig::Tests
{
	struct FDcrPoseFrame
	{
		TArray<FTransform> Transforms;
		TArray<float> Curves;
		TArray<FString> Variables;
	};

	struct FDcrRun
	{
		TArray<FRigElementKey> Keys;
		TArray<FRigElementKey> CurveKeys;
		TArray<FName> VariableNames;
		TArray<FDcrPoseFrame> Frames;
		FString Error;
	};

	/** Instantiates the rig and evaluates Forwards Solve frame by frame, calling Drive before each frame. */
	FDcrRun RunDcrRig(const FDcrRig& Rig, int32 FrameCount, TFunctionRef<void(UControlRig&, int32)> Drive);

	/**
	 * Resets bones to their initial pose, then moves every control on a fixed path by its type and lifts
	 * ik_foot_l, ik_foot_r and pelvis when the rig has them: the same input for every rig, every run.
	 */
	void DriveDcrDefault(UControlRig& Rig, int32 Frame);

	struct FDcrRunDiff
	{
		double MaxTranslation = 0.0;
		double MaxRotationDegrees = 0.0;
		double MaxScale = 0.0;
		double MaxCurve = 0.0;
		int32 VariableMismatches = 0;
		bool bComparable = true;
		FString Worst;
		FString FirstVariable;

		bool IsExact() const { return bComparable && MaxTranslation == 0.0 && MaxRotationDegrees == 0.0 && MaxScale == 0.0 && MaxCurve == 0.0 && VariableMismatches == 0; }
		bool IsWithin(double Tolerance) const
		{
			return bComparable && MaxTranslation <= Tolerance && MaxRotationDegrees <= Tolerance && MaxScale <= Tolerance && MaxCurve <= Tolerance;
		}
		FString Describe() const;
	};

	/** Element by element by name, so a rig whose elements come in another order still compares. */
	FDcrRunDiff CompareDcrRuns(const FDcrRun& A, const FDcrRun& B);
}
