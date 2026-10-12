// Copyright DreamMoon. All Rights Reserved.
//
// Write-back for Control Rigs: what someone changed on a rig built from a .dcr source, written into that
// source. The rig is read again and compared with the model its text builds to. A changed value is replaced
// where the text writes it, and nothing around it is touched. What has no place to be changed in (a value the
// text leaves at its default, nodes or links added or removed, an element added) rewrites the block it is in
// -- an event, a function, a section -- from the rig. The new text is checked: it must build to what the rig
// holds, or nothing is written. Docs/WriteBack.md says what is written and how.

#pragma once

#include "CoreMinimal.h"
#include "Rig/DcrAsset.h"
#include "WriteBack/DabWriteBack.h"

namespace UE::DreamAnim::Rig
{
	/**
	 * What Rig holds that Text (the source at FilePath it was built from) does not, written into the text.
	 * FilePath resolves #include and names the file in diagnostics; nothing is read from or written to disk.
	 */
	DREAMANIMRIG_API Compiler::FDabWriteBackResult WriteBackDcrRig(const FDcrRig& Rig, const FString& FilePath, const FString& Text,
		const Compiler::FDabWriteBackOptions& Options = Compiler::FDabWriteBackOptions());

	/** The text a rig was last built from or written to, kept in its package; empty when there is none. */
	DREAMANIMRIG_API FString GetDcrGeneratedText(const UObject& Asset);
	DREAMANIMRIG_API void SetDcrGeneratedText(UObject& Asset, const FString& Text);
}
