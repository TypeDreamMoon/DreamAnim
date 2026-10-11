// Copyright DreamMoon. All Rights Reserved.

#include "DcrRunHarness.h"

#include "ControlRig.h"
#include "EulerTransform.h"
#include "Rigs/RigHierarchy.h"
#include "Rigs/RigHierarchyElements.h"

namespace UE::DreamAnim::Rig::Tests
{
	FDcrRun RunDcrRig(const FDcrRig& Rig, int32 FrameCount, TFunctionRef<void(UControlRig&, int32)> Drive)
	{
		FDcrRun Run;
		UControlRig* Instance = Rig.IsValid() ? Rig.ControlRig->CreateControlRig() : nullptr;
		if (!Instance)
		{
			Run.Error = TEXT("the rig could not be instantiated");
			return Run;
		}
		// Every variable, private ones too: an instance's script-accessible list has only the public ones.
		for (const FRigVMGraphVariableDescription& Variable : Rig.RigVM->GetAssetVariables())
		{
			Run.VariableNames.Add(Variable.Name);
		}
		Run.VariableNames.Sort(FNameLexicalLess());

		for (int32 Index = 0; Index < FrameCount; ++Index)
		{
			Drive(*Instance, Index);
			Instance->SetDeltaTime(1.f / 30.f);
			Instance->Evaluate_AnyThread();

			URigHierarchy* Hierarchy = Instance->GetHierarchy();
			if (Index == 0)
			{
				// After the first frame, so that what Construction spawns is included.
				Run.Keys = Hierarchy->GetBoneKeys(true);
				Run.Keys.Append(Hierarchy->GetNullKeys(true));
				Run.Keys.Append(Hierarchy->GetControlKeys(true));
				Run.CurveKeys = Hierarchy->GetCurveKeys();
			}
			FDcrPoseFrame& Frame = Run.Frames.AddDefaulted_GetRef();
			for (const FRigElementKey& Key : Run.Keys)
			{
				Frame.Transforms.Add(Hierarchy->GetGlobalTransform(Key, false));
			}
			for (const FRigElementKey& Key : Run.CurveKeys)
			{
				Frame.Curves.Add(Hierarchy->GetCurveValue(Key));
			}
			for (const FName& Name : Run.VariableNames)
			{
				Frame.Variables.Add(Instance->GetVariableAsString(Name));
			}
		}
		Instance->MarkAsGarbage();
		return Run;
	}

	void DriveDcrDefault(UControlRig& Rig, int32 Frame)
	{
		URigHierarchy* Hierarchy = Rig.GetHierarchy();
		Hierarchy->ResetPoseToInitial(ERigElementType::Bone);
		const double Time = Frame / 30.0;
		int32 Index = 0;
		for (const FRigElementKey& Key : Hierarchy->GetControlKeys(true))
		{
			const FRigControlElement* Control = Hierarchy->Find<FRigControlElement>(Key);
			if (!Control)
			{
				continue;
			}
			const double Phase = Time * 2.0 + Index * 0.37;
			++Index;
			switch (Control->Settings.ControlType)
			{
			case ERigControlType::Bool:
				Hierarchy->SetControlValue(Key, FRigControlValue::Make<bool>(((Frame / 20) + Index) % 2 == 0));
				break;
			case ERigControlType::Float:
			case ERigControlType::ScaleFloat:
				Hierarchy->SetControlValue(Key, FRigControlValue::Make<float>(float(FMath::Sin(Phase) * 0.5 + 0.5)));
				break;
			case ERigControlType::Integer:
				Hierarchy->SetControlValue(Key, FRigControlValue::Make<int32>((Frame / 15 + Index) % 3));
				break;
			case ERigControlType::EulerTransform:
			{
				const FEulerTransform Value(
					FVector(FMath::Sin(Phase) * 3.0, FMath::Cos(Phase) * 3.0, FMath::Sin(Phase * 2.0) * 2.0),
					FRotator(FMath::Sin(Phase) * 10.0, FMath::Cos(Phase) * 10.0, 0.0),
					FVector::OneVector);
				Hierarchy->SetControlValue(Key, FRigControlValue::Make<FRigControlValue::FEulerTransform_Float>(FRigControlValue::FEulerTransform_Float(Value)));
				break;
			}
			case ERigControlType::Transform:
			{
				const FTransform Value(FRotator(FMath::Sin(Phase) * 10.0, 0.0, 0.0), FVector(FMath::Sin(Phase) * 3.0, 0.0, 0.0));
				Hierarchy->SetControlValue(Key, FRigControlValue::Make<FRigControlValue::FTransform_Float>(FRigControlValue::FTransform_Float(Value)));
				break;
			}
			case ERigControlType::Position:
				Hierarchy->SetControlValue(Key, FRigControlValue::Make<FVector3f>(FVector3f(FMath::Sin(Phase) * 3.0, FMath::Cos(Phase) * 3.0, 0.f)));
				break;
			case ERigControlType::Rotator:
				Hierarchy->SetControlValue(Key, FRigControlValue::Make<FVector3f>(FVector3f(FMath::Sin(Phase) * 10.0, 0.f, 0.f)));
				break;
			default:
				break;
			}
		}

		// The bones foot IK rigs read from the incoming pose.
		static const TCHAR* const Bones[] = { TEXT("ik_foot_l"), TEXT("ik_foot_r"), TEXT("pelvis") };
		for (int32 Bone = 0; Bone < UE_ARRAY_COUNT(Bones); ++Bone)
		{
			const FRigElementKey Key(Bones[Bone], ERigElementType::Bone);
			if (Hierarchy->Contains(Key))
			{
				FTransform Global = Hierarchy->GetGlobalTransform(Key, false);
				Global.AddToTranslation(FVector(0.0, 0.0, FMath::Sin(Time * 3.0 + Bone) * 6.0));
				Hierarchy->SetGlobalTransform(Key, Global, false, true);
			}
		}
	}

	FString FDcrRunDiff::Describe() const
	{
		if (!bComparable)
		{
			return TEXT("not comparable: ") + Worst;
		}
		return FString::Printf(TEXT("max translation %.6g, max rotation %.6g deg, max scale %.6g, max curve %.6g, variable mismatches %d%s%s"),
			MaxTranslation, MaxRotationDegrees, MaxScale, MaxCurve, VariableMismatches,
			Worst.IsEmpty() ? TEXT("") : *(TEXT(" (worst: ") + Worst + TEXT(")")),
			FirstVariable.IsEmpty() ? TEXT("") : *(TEXT(" (first variable: ") + FirstVariable + TEXT(")")));
	}

	FDcrRunDiff CompareDcrRuns(const FDcrRun& A, const FDcrRun& B)
	{
		FDcrRunDiff Diff;
		TArray<int32> KeyMap;
		for (const FRigElementKey& Key : A.Keys)
		{
			KeyMap.Add(B.Keys.IndexOfByKey(Key));
		}
		TArray<int32> CurveMap;
		for (const FRigElementKey& Key : A.CurveKeys)
		{
			CurveMap.Add(B.CurveKeys.IndexOfByKey(Key));
		}
		if (A.Keys.Num() != B.Keys.Num() || KeyMap.Contains(INDEX_NONE) || A.CurveKeys.Num() != B.CurveKeys.Num() || CurveMap.Contains(INDEX_NONE)
			|| A.Frames.Num() != B.Frames.Num() || !A.Error.IsEmpty() || !B.Error.IsEmpty())
		{
			Diff.bComparable = false;
			Diff.Worst = FString::Printf(TEXT("elements %d/%d, curves %d/%d, frames %d/%d%s%s"), A.Keys.Num(), B.Keys.Num(), A.CurveKeys.Num(), B.CurveKeys.Num(),
				A.Frames.Num(), B.Frames.Num(), A.Error.IsEmpty() ? TEXT("") : *(TEXT("; ") + A.Error), B.Error.IsEmpty() ? TEXT("") : *(TEXT("; ") + B.Error));
			return Diff;
		}
		for (int32 FrameIndex = 0; FrameIndex < A.Frames.Num(); ++FrameIndex)
		{
			const FDcrPoseFrame& FrameA = A.Frames[FrameIndex];
			const FDcrPoseFrame& FrameB = B.Frames[FrameIndex];
			for (int32 Index = 0; Index < A.Keys.Num(); ++Index)
			{
				const FTransform& TA = FrameA.Transforms[Index];
				const FTransform& TB = FrameB.Transforms[KeyMap[Index]];
				const double Translation = (TA.GetTranslation() - TB.GetTranslation()).Size();
				// Stable for tiny angles, unlike acos of the dot product (which reads 0.03 deg of pure rounding).
				const FQuat QA = TA.GetRotation();
				const FQuat QB = TB.GetRotation();
				const double Chord = FMath::Min((QA - QB).Size(), (QA + QB).Size());
				const double Rotation = FMath::RadiansToDegrees(4.0 * FMath::Asin(FMath::Min(1.0, Chord * 0.5)));
				const double Scale = (TA.GetScale3D() - TB.GetScale3D()).GetAbsMax();
				if (Translation > Diff.MaxTranslation)
				{
					Diff.MaxTranslation = Translation;
					Diff.Worst = FString::Printf(TEXT("%s at frame %d"), *A.Keys[Index].ToString(), FrameIndex);
				}
				Diff.MaxRotationDegrees = FMath::Max(Diff.MaxRotationDegrees, Rotation);
				Diff.MaxScale = FMath::Max(Diff.MaxScale, Scale);
			}
			for (int32 Index = 0; Index < FrameA.Curves.Num(); ++Index)
			{
				Diff.MaxCurve = FMath::Max<double>(Diff.MaxCurve, FMath::Abs(FrameA.Curves[Index] - FrameB.Curves[CurveMap[Index]]));
			}
			if (A.VariableNames != B.VariableNames)
			{
				Diff.VariableMismatches = -1;
				continue;
			}
			for (int32 Index = 0; Index < FrameA.Variables.Num(); ++Index)
			{
				if (FrameA.Variables[Index] != FrameB.Variables[Index])
				{
					if (Diff.VariableMismatches == 0)
					{
						Diff.FirstVariable = FString::Printf(TEXT("%s at frame %d: '%s' vs '%s'"), *A.VariableNames[Index].ToString(), FrameIndex,
							*FrameA.Variables[Index].Left(80), *FrameB.Variables[Index].Left(80));
					}
					++Diff.VariableMismatches;
				}
			}
		}
		return Diff;
	}
}
