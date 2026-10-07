// Copyright DreamMoon. All Rights Reserved.
//
// Drives an ALS character through a fixed script in a private game world and records its pose every
// frame. Two recordings that differ only in which Animation Blueprints run (the originals, or the ones
// DreamAnim generated from their decompiled text) must match bone for bone: that is the behavioural
// half of the round-trip tests. ALS is reached through reflection only, so the tests module does not
// depend on it.

#pragma once

#include "CoreMinimal.h"

class ACharacter;
class UAnimInstance;

namespace UE::DreamAnim::Tests
{
	struct FAlsPoseRecording
	{
		TArray<FName> Bones;
		/** Per frame, the component-space transform of every bone. */
		TArray<TArray<FTransform>> Frames;
		/** Per frame, the character's location: shows the script actually moved it. */
		TArray<FVector> Locations;
		/** What ran: the anim instance class, world time, movement mode. For failure messages. */
		FString Notes;
		/** Wall-clock seconds spent ticking the world, every frame together: what the Blueprints cost, give or take the rest of the frame. */
		double TickSeconds = 0.0;
	};

	struct FAlsPoseScript
	{
		int32 Frames = 420;
		float DeltaSeconds = 1.f / 30.f;
		/** Called before the character finishes spawning: its mesh's AnimClass can still be replaced. */
		TFunction<void(ACharacter& Character)> BeforeSpawn;
		/** Called once after the character is spawned and before the first frame: swap Animation Blueprints here. */
		TFunction<void(ACharacter& Character)> Setup;
		/** Called after every scripted overlay change (ALS relinks its overlay layers then). */
		TFunction<void(ACharacter& Character)> AfterOverlayChange;
	};

	/** Runs the script. False, with Error set, when the world or the character could not be made. */
	bool RecordAlsPoses(const FAlsPoseScript& Script, FAlsPoseRecording& Out, FString& Error);

	/**
	 * The first frame and bone where two recordings differ by more than the tolerances (centimetres and
	 * degrees), as text; empty when they match.
	 */
	FString CompareAlsPoses(const FAlsPoseRecording& Expected, const FAlsPoseRecording& Actual, double LocationTolerance = 0.01, double AngleTolerance = 0.01);
}
