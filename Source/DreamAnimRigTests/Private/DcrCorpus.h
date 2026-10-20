// Copyright DreamMoon. All Rights Reserved.
//
// The engine's own Control Rigs the tests read, rebuild and run, and ALS-Refactored's. They need the test
// host's plugins (Tools/TestHost/Template): ControlRig, ControlRigModules and ControlRigSpline are on by
// default, MoverExamples, AnimatorKit and ALS are enabled by the host.

#pragma once

#include "CoreMinimal.h"

namespace UE::DreamAnim::Rig::Tests
{
	enum class EDcrCorpusGroup : uint8
	{
		/** Rigs that run on their own: MoverExamples' mannequin rigs, AnimatorKit's utility and deformer rigs. */
		Standalone,
		/** Modular rig modules (ControlRigModules/Modules58); they only run inside a modular rig. */
		Module,
		/** The root module and the function libraries other rigs call into. */
		Library,
		/** Rigs already in 5.8's independent asset kind (physics). */
		Independent,
	};

	struct FDcrCorpusEntry
	{
		const TCHAR* ObjectPath;
		const TCHAR* Name;
		EDcrCorpusGroup Group;
	};

	inline TConstArrayView<FDcrCorpusEntry> GetDcrCorpus()
	{
		static const FDcrCorpusEntry Entries[] = {
			{ TEXT("/MoverExamples/Characters/Mannequins/Rigs/CR_Mannequin_BasicFootIK.CR_Mannequin_BasicFootIK"), TEXT("CR_Mannequin_BasicFootIK"), EDcrCorpusGroup::Standalone },
			{ TEXT("/MoverExamples/Characters/Mannequins/Rigs/CR_Mannequin_Procedural.CR_Mannequin_Procedural"), TEXT("CR_Mannequin_Procedural"), EDcrCorpusGroup::Standalone },
			{ TEXT("/MoverExamples/Characters/Mannequins/Rigs/CR_Mannequin_Body.CR_Mannequin_Body"), TEXT("CR_Mannequin_Body"), EDcrCorpusGroup::Standalone },
			{ TEXT("/AnimatorKit/UtilityRigs/CRU_SplinePath.CRU_SplinePath"), TEXT("CRU_SplinePath"), EDcrCorpusGroup::Standalone },
			{ TEXT("/AnimatorKit/UtilityRigs/CRU_SingleSim.CRU_SingleSim"), TEXT("CRU_SingleSim"), EDcrCorpusGroup::Standalone },
			{ TEXT("/AnimatorKit/UtilityRigs/CRU_MorphTargetDriver.CRU_MorphTargetDriver"), TEXT("CRU_MorphTargetDriver"), EDcrCorpusGroup::Standalone },
			{ TEXT("/AnimatorKit/UtilityRigs/CRU_ChainSim.CRU_ChainSim"), TEXT("CRU_ChainSim"), EDcrCorpusGroup::Standalone },
			{ TEXT("/AnimatorKit/UtilityRigs/CRU_BlendParent.CRU_BlendParent"), TEXT("CRU_BlendParent"), EDcrCorpusGroup::Standalone },
			{ TEXT("/AnimatorKit/UtilityRigs/CRU_AddLocator.CRU_AddLocator"), TEXT("CRU_AddLocator"), EDcrCorpusGroup::Standalone },
			{ TEXT("/AnimatorKit/UtilityRigs/CRU_3Node.CRU_3Node"), TEXT("CRU_3Node"), EDcrCorpusGroup::Standalone },
			{ TEXT("/AnimatorKit/DeformerRigs/CameraSpaceLatticeDeformer/CRD_CameraSpaceLattice_10x10.CRD_CameraSpaceLattice_10x10"), TEXT("CRD_CameraSpaceLattice_10x10"), EDcrCorpusGroup::Standalone },
			{ TEXT("/AnimatorKit/DeformerRigs/LatticeDeformer/CRD_Lattice_4x4x4.CRD_Lattice_4x4x4"), TEXT("CRD_Lattice_4x4x4"), EDcrCorpusGroup::Standalone },
			{ TEXT("/AnimatorKit/DeformerRigs/LatticeDeformer/CRD_Lattice_3x3x3.CRD_Lattice_3x3x3"), TEXT("CRD_Lattice_3x3x3"), EDcrCorpusGroup::Standalone },
			{ TEXT("/AnimatorKit/DeformerRigs/LatticeDeformer/CRD_Lattice_2x2x2.CRD_Lattice_2x2x2"), TEXT("CRD_Lattice_2x2x2"), EDcrCorpusGroup::Standalone },
			{ TEXT("/AnimatorKit/DeformerRigs/LatticeDeformer/CRD_Lattice.CRD_Lattice"), TEXT("CRD_Lattice"), EDcrCorpusGroup::Standalone },
			{ TEXT("/AnimatorKit/DeformerRigs/SculptDeformer/CRD_SculptDeformer.CRD_SculptDeformer"), TEXT("CRD_SculptDeformer"), EDcrCorpusGroup::Standalone },
			// A rig of the Blueprint kind from outside the engine: its own control flow unit (Is Game World).
			{ TEXT("/ALS/ALS/Character/CR_Als.CR_Als"), TEXT("CR_Als"), EDcrCorpusGroup::Standalone },

			{ TEXT("/ControlRigModules/Modules58/AddControl.AddControl"), TEXT("AddControl"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/AnimalNeck.AnimalNeck"), TEXT("AnimalNeck"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/AnimalSpine.AnimalSpine"), TEXT("AnimalSpine"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/Arm.Arm"), TEXT("Arm"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/Chain.Chain"), TEXT("Chain"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/Constraint.Constraint"), TEXT("Constraint"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/Finger.Finger"), TEXT("Finger"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/Foot.Foot"), TEXT("Foot"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/HindLeg.HindLeg"), TEXT("HindLeg"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/Hinge.Hinge"), TEXT("Hinge"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/Leg.Leg"), TEXT("Leg"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/Neck.Neck"), TEXT("Neck"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/Piston.Piston"), TEXT("Piston"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/PivotProxy.PivotProxy"), TEXT("PivotProxy"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/Shoulder.Shoulder"), TEXT("Shoulder"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/Spine.Spine"), TEXT("Spine"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/Suspension.Suspension"), TEXT("Suspension"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/Tail.Tail"), TEXT("Tail"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/VehiclePivotProxy.VehiclePivotProxy"), TEXT("VehiclePivotProxy"), EDcrCorpusGroup::Module },
			{ TEXT("/ControlRigModules/Modules58/Wheel.Wheel"), TEXT("Wheel"), EDcrCorpusGroup::Module },

			{ TEXT("/ControlRig/Modules/Modules58/Root.Root"), TEXT("Root"), EDcrCorpusGroup::Library },
			{ TEXT("/ControlRig/StandardFunctionLibrary/StandardFunctionLibrary.StandardFunctionLibrary"), TEXT("StandardFunctionLibrary"), EDcrCorpusGroup::Library },
			{ TEXT("/ControlRigSpline/SplineFunctionLibrary/SplineFunctionLibrary.SplineFunctionLibrary"), TEXT("SplineFunctionLibrary"), EDcrCorpusGroup::Library },

			{ TEXT("/ControlRigModules/Modules58/PhysicsSolver.PhysicsSolver"), TEXT("PhysicsSolver"), EDcrCorpusGroup::Independent },
			{ TEXT("/ControlRigModules/Modules58/LimbPhysics.LimbPhysics"), TEXT("LimbPhysics"), EDcrCorpusGroup::Independent },
			{ TEXT("/ControlRigModules/Modules58/ChainPhysics.ChainPhysics"), TEXT("ChainPhysics"), EDcrCorpusGroup::Independent },
			{ TEXT("/ControlRigModules/Modules58/BipedPhysics.BipedPhysics"), TEXT("BipedPhysics"), EDcrCorpusGroup::Independent },
			{ TEXT("/AnimatorKit/PhysicsRigs/CR_PhysCube.CR_PhysCube"), TEXT("CR_PhysCube"), EDcrCorpusGroup::Independent },
			{ TEXT("/AnimatorKit/PhysicsRigs/CR_PhysBall.CR_PhysBall"), TEXT("CR_PhysBall"), EDcrCorpusGroup::Independent },
		};
		return Entries;
	}
}
