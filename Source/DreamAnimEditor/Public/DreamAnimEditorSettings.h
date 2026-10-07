// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "DreamAnimEditorSettings.generated.h"

/** How the editor keeps Animation Blueprints and their DreamAnim sources in step (Editor Preferences > Plugins > Dream Anim). */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Dream Anim"))
class DREAMANIMEDITOR_API UDreamAnimEditorSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/** Rebuild a Blueprint when its source (or a library it includes) changes on disk. */
	UPROPERTY(config, EditAnywhere, Category = "Text to Blueprint")
	bool bRebuildWhenTextChanges = true;

	/** Save a Blueprint after rebuilding it, so the asset on disk matches the text. */
	UPROPERTY(config, EditAnywhere, Category = "Text to Blueprint")
	bool bSaveRebuiltBlueprints = true;

	/** Write changes made to a Blueprint in the editor back into its source. */
	UPROPERTY(config, EditAnywhere, Category = "Blueprint to Text")
	bool bWriteBackEditorChanges = true;

	/** Also write added and removed nodes, links, states and transitions, by rewriting the graph or machine they are in. */
	UPROPERTY(config, EditAnywhere, Category = "Blueprint to Text", meta = (EditCondition = "bWriteBackEditorChanges"))
	bool bWriteBackStructure = true;

	/** Seconds without further edits before the changes are written. */
	UPROPERTY(config, EditAnywhere, Category = "Blueprint to Text", meta = (EditCondition = "bWriteBackEditorChanges", ClampMin = "0.1", ClampMax = "30"))
	float WriteBackDelay = 1.0f;

	/**
	 * How a source is opened at a line: {file}, {line} and {column} are replaced. Run through the shell, so
	 * a script on the PATH works (`code` is VS Code's). Empty opens the file with its associated program.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Sources")
	FString OpenSourceCommand = TEXT("code -g \"{file}:{line}:{column}\"");

	virtual FName GetContainerName() const override { return TEXT("Editor"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
};
