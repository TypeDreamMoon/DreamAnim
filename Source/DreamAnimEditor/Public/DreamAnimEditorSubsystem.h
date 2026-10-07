// Copyright DreamMoon. All Rights Reserved.
//
// Keeps Animation Blueprints and their DreamAnim sources in step while the editor runs. A source saved on
// disk rebuilds its Blueprint (a library rebuilds the documents that include it). A change made to a
// Blueprint in the editor is written back into its source once edits pause. Each Blueprint keeps the text
// it was last built from or written to (GetDabGeneratedText); when the text changed on disk while the
// Blueprint has changes that are not in it, nothing is overwritten: the conflict is listed and the user
// picks the text, or a merge of both.

#pragma once

#include "CoreMinimal.h"
#include "Driver/DabDriver.h"
#include "EditorSubsystem.h"
#include "Lang/DabDiagnostic.h"
#include "TickableEditorObject.h"
#include "DreamAnimEditorSubsystem.generated.h"

class IAssetEditorInstance;
class SNotificationItem;
class UAnimBlueprint;
struct FFileChangeData;
class FObjectPreSaveContext;

UCLASS()
class DREAMANIMEDITOR_API UDreamAnimEditorSubsystem : public UEditorSubsystem, public FTickableEditorObject
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override;

	const UE::DreamAnim::Compiler::FDabSourceIndex& GetIndex() const { return Index; }

	/** The source an asset is made from; empty when there is none. */
	FString FindSource(const UObject& Asset) const;

	/** Scans the source folders again and watches any that appeared. */
	void RefreshSources();

	/**
	 * Sources that changed on disk, as the folder watcher reports them once they settle: each rebuilds
	 * (a library, the documents that include it), unless the change is one this subsystem wrote or the
	 * Blueprint has changes of its own (then a conflict is listed).
	 */
	void HandleChangedSources(const TArray<FString>& FilePaths);

	/** Compiles a source into its asset; with errors the asset is left as it is. */
	UAnimBlueprint* Rebuild(const FString& FilePath);
	/** Every source: interfaces first, then the rest. */
	void RebuildAll();

	/**
	 * The Blueprint's changes into its source now. Refused when the file no longer holds the text the
	 * Blueprint was last in step with: that is a conflict. bExpandTemplates writes a template out where a
	 * value inside its body changed.
	 */
	bool WriteBack(UAnimBlueprint& Blueprint, bool bExpandTemplates = false);

	/** The Blueprint has changes that are not in the text it was last in step with. */
	bool HasEditorChanges(UAnimBlueprint& Blueprint, TArray<FString>* OutChanges = nullptr) const;
	/** The Blueprint waits for the user to pick between its changes and a text changed on disk. */
	bool IsInConflict(const UAnimBlueprint& Blueprint) const;
	/** Resolves a conflict for the text: the Blueprint is rebuilt from the file. */
	UAnimBlueprint* UseText(UAnimBlueprint& Blueprint);
	/** Resolves a conflict with both: the editor's changes are merged into the file, then the Blueprint is rebuilt from it. */
	bool MergeEditorChanges(UAnimBlueprint& Blueprint);

	/**
	 * Writes a Blueprint not made from text as a source at its default place (DAnim/...). bRebuild then
	 * rebuilds the Blueprint from that text, which write-back needs; it is skipped when the text cannot hold
	 * everything the Blueprint has. Returns the file, or empty.
	 */
	FString Decompile(UAnimBlueprint& Blueprint, bool bOverwrite, bool bRebuild);

	/** Opens a source at a line, with the command in the settings. */
	void OpenSource(const FString& FilePath, int32 Line = 1, int32 Column = 1) const;

private:
	void WatchRoots();
	void UnwatchRoots();
	void OnDirectoryChanged(const TArray<FFileChangeData>& Changes);
	void OnObjectTransacted(UObject* Object, const class FTransactionObjectEvent& Event);
	/** Saving a Blueprint catches what changed outside a transaction (scripts, tools) before it reaches disk. */
	void OnObjectPreSave(UObject* Object, FObjectPreSaveContext Context);
	void OnAssetOpened(UObject* Asset, IAssetEditorInstance* Editor);

	/** The Blueprint the object belongs to (itself, its graphs and nodes, its class default object). */
	static UAnimBlueprint* FindOwningBlueprint(UObject* Object);
	UAnimBlueprint* FindLoadedBlueprint(const FString& FilePath) const;

	/** bViaLibrary: the document's own text may be the same; a library it includes changed. */
	void HandleChangedSource(const FString& FilePath, bool bViaLibrary);
	UAnimBlueprint* RebuildSource(const FString& FilePath, bool bNotify);
	void ShowConflict(UAnimBlueprint& Blueprint, const FString& FilePath, const TArray<FString>& EditorChanges);
	void CloseConflict(UAnimBlueprint& Blueprint);
	bool SaveSource(const FString& FilePath, const FString& Text);

	void Report(const FString& Title, const TArray<UE::DreamAnim::Lang::FDabDiagnostic>& Diagnostics) const;
	TSharedPtr<SNotificationItem> Notify(const FText& Message, bool bSuccess, bool bShowLogLink) const;

	UE::DreamAnim::Compiler::FDabSourceIndex Index;
	TMap<FString, FDelegateHandle> Watches;
	/** Sources the watcher reported, by when it last did: handled once they settle. */
	TMap<FString, double> ChangedSources;
	/** Blueprints edited in the editor, by when they last were: written back once edits pause. */
	TMap<TWeakObjectPtr<UAnimBlueprint>, double> EditedBlueprints;
	/** What this subsystem wrote to each file: the watcher's report of it is not a change. */
	TMap<FString, FString> WrittenTexts;
	/** Blueprints waiting for the user's choice in a conflict, with its notification. */
	TMap<TWeakObjectPtr<UAnimBlueprint>, TWeakPtr<SNotificationItem>> Conflicts;
	/** The last refusals reported for each Blueprint, so moving nodes about does not repeat them. */
	TMap<TWeakObjectPtr<UAnimBlueprint>, FString> LastRefusals;
	/** Rebuilding: the generator's own edits are not the user's. */
	int32 RebuildDepth = 0;
	FDelegateHandle TransactedHandle;
	FDelegateHandle PreSaveHandle;
	FDelegateHandle AssetOpenedHandle;
};
