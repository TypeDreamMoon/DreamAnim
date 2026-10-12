// Copyright DreamMoon. All Rights Reserved.

#include "DreamAnimEditorSubsystem.h"

#include "Algo/Count.h"
#include "Animation/AnimBlueprint.h"
#include "DirectoryWatcherModule.h"
#include "DreamAnimEditorSettings.h"
#include "Driver/DabDocumentHandler.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Generate/DabGenerate.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IDirectoryWatcher.h"
#include "Lang/DabSource.h"
#include "Logging/MessageLog.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/TransactionObjectEvent.h"
#include "Modules/ModuleManager.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/ObjectSaveContext.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "WriteBack/DabTextMerge.h"
#include "WriteBack/DabWriteBack.h"

#define LOCTEXT_NAMESPACE "DreamAnimEditor"

using namespace UE::DreamAnim;
using namespace UE::DreamAnim::Compiler;

namespace DreamAnimEditorPrivate
{
	const FName LogName(TEXT("DreamAnim"));
	/** A save often arrives as several file events; they are handled together once the file is quiet. */
	constexpr double SettleSeconds = 0.3;

	FString NormalizeSourcePath(const FString& Path)
	{
		FString Full = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Full);
		return Full;
	}

	bool LoadSourceText(const FString& Path, FString& OutText)
	{
		return FPaths::FileExists(Path) && FFileHelper::LoadFileToString(OutText, *Path);
	}

	bool HasUtf8ByteOrderMark(const FString& Path)
	{
		const TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
		if (!Reader || Reader->TotalSize() < 3)
		{
			return false;
		}
		uint8 Head[3] = {};
		Reader->Serialize(Head, 3);
		return Head[0] == 0xEF && Head[1] == 0xBB && Head[2] == 0xBF;
	}

	FText FileText(const FString& Path)
	{
		return FText::FromString(FPaths::GetCleanFilename(Path));
	}

	void OpenLog()
	{
		FMessageLog(LogName).Open(EMessageSeverity::Info, true);
	}
}

using namespace DreamAnimEditorPrivate;

// ---------------------------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------------------------

void UDreamAnimEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UAssetEditorSubsystem* AssetEditors = Collection.InitializeDependency<UAssetEditorSubsystem>();

	Index.Scan(GetDabSourceRoots());
	WatchRoots();
	TransactedHandle = FCoreUObjectDelegates::OnObjectTransacted.AddUObject(this, &UDreamAnimEditorSubsystem::OnObjectTransacted);
	PreSaveHandle = FCoreUObjectDelegates::OnObjectPreSave.AddUObject(this, &UDreamAnimEditorSubsystem::OnObjectPreSave);
	if (AssetEditors)
	{
		AssetOpenedHandle = AssetEditors->OnAssetOpenedInEditor().AddUObject(this, &UDreamAnimEditorSubsystem::OnAssetOpened);
	}
}

void UDreamAnimEditorSubsystem::Deinitialize()
{
	FCoreUObjectDelegates::OnObjectTransacted.Remove(TransactedHandle);
	FCoreUObjectDelegates::OnObjectPreSave.Remove(PreSaveHandle);
	if (UAssetEditorSubsystem* AssetEditors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
	{
		AssetEditors->OnAssetOpenedInEditor().Remove(AssetOpenedHandle);
	}
	UnwatchRoots();
	Super::Deinitialize();
}

TStatId UDreamAnimEditorSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UDreamAnimEditorSubsystem, STATGROUP_Tickables);
}

ETickableTickType UDreamAnimEditorSubsystem::GetTickableTickType() const
{
	return IsTemplate() ? ETickableTickType::Never : ETickableTickType::Always;
}

void UDreamAnimEditorSubsystem::Tick(float DeltaTime)
{
	const double Now = FPlatformTime::Seconds();

	TArray<FString> Settled;
	for (auto It = ChangedSources.CreateIterator(); It; ++It)
	{
		if (Now - It.Value() >= SettleSeconds)
		{
			Settled.Add(It.Key());
			It.RemoveCurrent();
		}
	}
	if (!Settled.IsEmpty())
	{
		HandleChangedSources(Settled);
	}

	// Edits are written once they pause, never in the middle of a transaction or a drag.
	const UDreamAnimEditorSettings* Settings = GetDefault<UDreamAnimEditorSettings>();
	if ((EditedBlueprints.IsEmpty() && EditedAssets.IsEmpty()) || !Settings->bWriteBackEditorChanges || (GEditor && GEditor->IsTransactionActive())
		|| (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetPressedMouseButtons().Num() > 0))
	{
		return;
	}
	TArray<UObject*> DueAssets;
	for (auto It = EditedAssets.CreateIterator(); It; ++It)
	{
		UObject* Asset = It.Key().Get();
		if (!Asset)
		{
			It.RemoveCurrent();
		}
		else if (Now - It.Value() >= Settings->WriteBackDelay)
		{
			DueAssets.Add(Asset);
			It.RemoveCurrent();
		}
	}
	for (UObject* Asset : DueAssets)
	{
		WriteBackAsset(*Asset);
	}
	TArray<UAnimBlueprint*> Due;
	for (auto It = EditedBlueprints.CreateIterator(); It; ++It)
	{
		UAnimBlueprint* Blueprint = It.Key().Get();
		if (!Blueprint)
		{
			It.RemoveCurrent();
		}
		else if (Now - It.Value() >= Settings->WriteBackDelay && !IsInConflict(*Blueprint))
		{
			Due.Add(Blueprint);
			It.RemoveCurrent();
		}
	}
	for (UAnimBlueprint* Blueprint : Due)
	{
		WriteBack(*Blueprint);
	}
}

// ---------------------------------------------------------------------------------------------
// Sources on disk
// ---------------------------------------------------------------------------------------------

FString UDreamAnimEditorSubsystem::FindSource(const UObject& Asset) const
{
	const FDabSourceEntry* Entry = Index.FindByAsset(Asset.GetPathName());
	return Entry ? Entry->FilePath : FString();
}

UAnimBlueprint* UDreamAnimEditorSubsystem::FindLoadedBlueprint(const FString& FilePath) const
{
	const FDabSourceEntry* Entry = Index.FindByFile(FilePath);
	return Entry && !Entry->AssetPath.IsEmpty() ? FindObject<UAnimBlueprint>(nullptr, *Entry->AssetPath) : nullptr;
}

void UDreamAnimEditorSubsystem::RefreshSources()
{
	Index.Scan(GetDabSourceRoots());
	WatchRoots();
}

void UDreamAnimEditorSubsystem::WatchRoots()
{
	IDirectoryWatcher* Watcher = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")).Get();
	if (!Watcher)
	{
		return;
	}
	for (const FString& Root : Index.GetRoots())
	{
		if (Watches.Contains(Root) || !FPaths::DirectoryExists(Root))
		{
			continue;
		}
		FDelegateHandle Handle;
		if (Watcher->RegisterDirectoryChangedCallback_Handle(Root, IDirectoryWatcher::FDirectoryChanged::CreateUObject(this, &UDreamAnimEditorSubsystem::OnDirectoryChanged), Handle))
		{
			Watches.Add(Root, Handle);
		}
	}
}

void UDreamAnimEditorSubsystem::UnwatchRoots()
{
	FDirectoryWatcherModule* Module = FModuleManager::GetModulePtr<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
	IDirectoryWatcher* Watcher = Module ? Module->Get() : nullptr;
	for (const TPair<FString, FDelegateHandle>& Watch : Watches)
	{
		if (Watcher)
		{
			Watcher->UnregisterDirectoryChangedCallback_Handle(Watch.Key, Watch.Value);
		}
	}
	Watches.Reset();
}

void UDreamAnimEditorSubsystem::OnDirectoryChanged(const TArray<FFileChangeData>& Changes)
{
	const double Now = FPlatformTime::Seconds();
	for (const FFileChangeData& Change : Changes)
	{
		if (IsDabSourceFile(Change.Filename))
		{
			ChangedSources.Add(NormalizeSourcePath(Change.Filename), Now);
		}
	}
}

void UDreamAnimEditorSubsystem::HandleChangedSources(const TArray<FString>& FilePaths)
{
	// Each document once; one reached only through a library may still have its own text.
	TMap<FString, bool> Documents;
	for (const FString& RawPath : FilePaths)
	{
		const FString Path = NormalizeSourcePath(RawPath);
		FString Text;
		const bool bExists = LoadSourceText(Path, Text);
		Index.Rescan(Path);
		if (const FString* Written = WrittenTexts.Find(Path))
		{
			const bool bOurs = bExists && Text.Equals(*Written, ESearchCase::CaseSensitive);
			WrittenTexts.Remove(Path);
			if (bOurs)
			{
				continue;
			}
		}
		if (!bExists)
		{
			continue;
		}
		if (Lang::GetDabFileKindFromPath(Path) == Lang::EDabFileKind::Templates)
		{
			for (const FString& Dependent : Index.FindDependents(Path))
			{
				Documents.FindOrAdd(Dependent, true);
			}
		}
		else
		{
			Documents.Add(Path, false);
		}
	}
	if (Documents.IsEmpty() || !GetDefault<UDreamAnimEditorSettings>()->bRebuildWhenTextChanges)
	{
		return;
	}
	TArray<FString> Files;
	Documents.GetKeys(Files);
	for (const FString& File : OrderDabSourcesForBuild(Files, Index, Index.GetRoots()))
	{
		HandleChangedSource(File, Documents.FindRef(File));
	}
}

void UDreamAnimEditorSubsystem::HandleChangedSource(const FString& FilePath, bool bViaLibrary)
{
	FString Text;
	if (!LoadSourceText(FilePath, Text))
	{
		return;
	}
	if (UAnimBlueprint* Blueprint = FindLoadedBlueprint(FilePath))
	{
		const bool bEdited = EditedBlueprints.Contains(Blueprint) || IsInConflict(*Blueprint) || !LastRefusals.FindRef(Blueprint).IsEmpty();
		const FString Base = GetDabGeneratedText(*Blueprint);
		if (!bViaLibrary && !bEdited && Base.Equals(Text, ESearchCase::CaseSensitive))
		{
			// Already in step: an echo of a save, or a change undone.
			return;
		}
		// Compared with the text it was made from: a library that changed would make every dependent differ.
		TArray<FString> EditorChanges;
		if ((!bViaLibrary || bEdited) && HasEditorChanges(*Blueprint, &EditorChanges))
		{
			ShowConflict(*Blueprint, FilePath, EditorChanges);
			return;
		}
	}
	// An asset of another kind with editor changes not yet in the text: the file is not built over them.
	const FDabSourceEntry* Entry = Index.FindByFile(FilePath);
	UObject* Asset = Entry && !Entry->AssetPath.IsEmpty() ? FindObject<UObject>(nullptr, *Entry->AssetPath) : nullptr;
	if (Asset && EditedAssets.Contains(Asset))
	{
		EditedAssets.Remove(Asset);
		FMessageLog(LogName).Warning(FText::Format(LOCTEXT("AssetConflict",
			"{0} changed on disk while {1} has changes in the editor that are not in it. Neither was overwritten: Rebuild from DreamAnim Source takes the text, Write Changes to DreamAnim Source the asset (after the text's change is undone)."),
			FileText(FilePath), FText::FromString(Asset->GetName())));
		Notify(FText::Format(LOCTEXT("AssetConflictShort", "{0} and its text both changed; the log says how to choose."), FText::FromString(Asset->GetName())), false, true);
		return;
	}
	RebuildSource(FilePath, true);
}

bool UDreamAnimEditorSubsystem::SaveSource(const FString& FilePath, const FString& Text)
{
	const FFileHelper::EEncodingOptions Encoding = HasUtf8ByteOrderMark(FilePath) ? FFileHelper::EEncodingOptions::ForceUTF8 : FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM;
	if (!FFileHelper::SaveStringToFile(Text, *FilePath, Encoding))
	{
		FMessageLog(LogName).Error(FText::Format(LOCTEXT("CannotWrite", "Cannot write {0}; is it read-only or open elsewhere?"), FText::FromString(FilePath)));
		Notify(FText::Format(LOCTEXT("CannotWriteShort", "Cannot write {0}."), FileText(FilePath)), false, true);
		return false;
	}
	WrittenTexts.Add(FilePath, Text);
	Index.Rescan(FilePath);
	return true;
}

// ---------------------------------------------------------------------------------------------
// Text to Blueprint
// ---------------------------------------------------------------------------------------------

UAnimBlueprint* UDreamAnimEditorSubsystem::Rebuild(const FString& FilePath)
{
	return RebuildSource(NormalizeSourcePath(FilePath), true);
}

UAnimBlueprint* UDreamAnimEditorSubsystem::RebuildSource(const FString& FilePath, bool bNotify)
{
	const UDreamAnimEditorSettings* Settings = GetDefault<UDreamAnimEditorSettings>();
	UAssetEditorSubsystem* AssetEditors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;

	// An open editor would hold graphs the rebuild replaces. The asset is an Animation Blueprint, or what another
	// kind of document builds (a Control Rig).
	UAnimBlueprint* Existing = FindLoadedBlueprint(FilePath);
	UObject* ExistingAsset = Existing;
	if (!ExistingAsset)
	{
		const FDabSourceEntry* Entry = Index.FindByFile(FilePath);
		ExistingAsset = Entry && !Entry->AssetPath.IsEmpty() ? FindObject<UObject>(nullptr, *Entry->AssetPath) : nullptr;
	}
	const bool bWasOpen = ExistingAsset && AssetEditors && AssetEditors->FindEditorForAsset(ExistingAsset, false) != nullptr;
	if (bWasOpen)
	{
		AssetEditors->CloseAllEditorsForAsset(ExistingAsset);
	}

	FDabCompileOptions Options;
	Options.bGenerate = true;
	Options.bSave = Settings->bSaveRebuiltBlueprints;
	Options.ResolveDreamAnimAsset = [this](const FString& AssetName) { return Index.FindByAssetName(AssetName); };
	++RebuildDepth;
	const FDabCompileResult Result = CompileDabFile(FilePath, Options);
	--RebuildDepth;

	UAnimBlueprint* Blueprint = Result.Blueprint ? Result.Blueprint : Existing;
	if (Blueprint && Result.Succeeded())
	{
		EditedBlueprints.Remove(Blueprint);
		LastRefusals.Remove(Blueprint);
		CloseConflict(*Blueprint);
	}
	UObject* Built = Result.Asset ? Result.Asset : (Blueprint ? Blueprint : ExistingAsset);
	if (bWasOpen && Built)
	{
		AssetEditors->OpenEditorForAsset(Built);
	}

	Report(FString::Printf(TEXT("Rebuilding from %s"), *FPaths::GetCleanFilename(FilePath)), Result.Diagnostics);
	if (bNotify)
	{
		if (Result.Succeeded() && Result.Asset)
		{
			Notify(FText::Format(LOCTEXT("Rebuilt", "Rebuilt {0} from {1}"), FText::FromString(Result.Asset->GetName()), FileText(FilePath)), true, false);
		}
		else
		{
			Notify(FText::Format(LOCTEXT("RebuildFailed", "{0} has errors; its asset was not rebuilt."), FileText(FilePath)), false, true);
		}
	}
	return Result.Succeeded() ? Result.Blueprint : nullptr;
}

void UDreamAnimEditorSubsystem::RebuildAll()
{
	TArray<FString> Files;
	for (const FDabSourceEntry& Entry : Index.GetEntries())
	{
		if (!Entry.AssetPath.IsEmpty())
		{
			Files.Add(Entry.FilePath);
		}
	}
	int32 NumBuilt = 0;
	for (const FString& File : OrderDabSourcesForBuild(Files, Index, Index.GetRoots()))
	{
		NumBuilt += RebuildSource(File, false) != nullptr ? 1 : 0;
	}
	const bool bAll = NumBuilt == Files.Num();
	Notify(bAll ? FText::Format(LOCTEXT("RebuiltAll", "Rebuilt {0} Blueprint(s) from their sources"), NumBuilt)
		: FText::Format(LOCTEXT("RebuiltSome", "Rebuilt {0} of {1} Blueprints; the rest have errors."), NumBuilt, Files.Num()), bAll, !bAll);
}

// ---------------------------------------------------------------------------------------------
// Blueprint to text
// ---------------------------------------------------------------------------------------------

UAnimBlueprint* UDreamAnimEditorSubsystem::FindOwningBlueprint(UObject* Object)
{
	if (UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(Object))
	{
		return Blueprint;
	}
	if (UAnimBlueprint* Outer = Object->GetTypedOuter<UAnimBlueprint>())
	{
		return Outer;
	}
	// Defaults are edited on the generated class's default object.
	if (Object->HasAnyFlags(RF_ClassDefaultObject))
	{
		return Cast<UAnimBlueprint>(UBlueprint::GetBlueprintFromClass(Object->GetClass()));
	}
	return nullptr;
}

UObject* UDreamAnimEditorSubsystem::FindOwningHandlerAsset(UObject* Object)
{
	// The asset is the package's top-level object; a Blueprint's generated class default object leads to it too.
	UObject* Asset = Object ? Object->GetOutermostObject() : nullptr;
	if (Object && Object->HasAnyFlags(RF_ClassDefaultObject))
	{
		Asset = UBlueprint::GetBlueprintFromClass(Object->GetClass());
	}
	const IDabDocumentHandler* Handler = Asset ? FindDabDecompileHandler(*Asset) : nullptr;
	return Handler && Handler->SupportsWriteBack() ? Asset : nullptr;
}

void UDreamAnimEditorSubsystem::OnObjectTransacted(UObject* Object, const FTransactionObjectEvent& Event)
{
	if (RebuildDepth > 0 || !Object)
	{
		return;
	}
	UAnimBlueprint* Blueprint = FindOwningBlueprint(Object);
	if (Blueprint && !FindSource(*Blueprint).IsEmpty())
	{
		EditedBlueprints.Add(Blueprint, FPlatformTime::Seconds());
		return;
	}
	if (UObject* Asset = FindOwningHandlerAsset(Object); Asset && !FindSource(*Asset).IsEmpty())
	{
		EditedAssets.Add(Asset, FPlatformTime::Seconds());
	}
}

void UDreamAnimEditorSubsystem::OnObjectPreSave(UObject* Object, FObjectPreSaveContext Context)
{
	// Not the generator's own save, not a cook, not a command line build.
	if (Object && !Cast<UAnimBlueprint>(Object) && RebuildDepth == 0 && !Context.IsProceduralSave() && !IsRunningCommandlet()
		&& GetDefault<UDreamAnimEditorSettings>()->bWriteBackEditorChanges && FindOwningHandlerAsset(Object) == Object && !FindSource(*Object).IsEmpty())
	{
		WriteBackAsset(*Object);
		return;
	}
	UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(Object);
	if (!Blueprint || RebuildDepth > 0 || Context.IsProceduralSave() || IsRunningCommandlet()
		|| !GetDefault<UDreamAnimEditorSettings>()->bWriteBackEditorChanges || IsInConflict(*Blueprint) || FindSource(*Blueprint).IsEmpty())
	{
		return;
	}
	// Whatever changed since the last write, transacted or not, goes into the text with the asset.
	WriteBack(*Blueprint);
}

bool UDreamAnimEditorSubsystem::WriteBackAsset(UObject& Asset)
{
	IDabDocumentHandler* Handler = FindDabDecompileHandler(Asset);
	const FString Path = FindSource(Asset);
	FString Text;
	if (!Handler || !Handler->SupportsWriteBack() || Path.IsEmpty() || !LoadSourceText(Path, Text))
	{
		return false;
	}
	EditedAssets.Remove(&Asset);

	// The file changed since the two were last in step: writing now would undo that change.
	const FString Base = Handler->GetGeneratedText(Asset);
	if (!Base.IsEmpty() && !Base.Equals(Text, ESearchCase::CaseSensitive))
	{
		FMessageLog(LogName).Warning(FText::Format(LOCTEXT("AssetTextMoved",
			"{0} changed on disk since {1} was last built from it, so the asset's changes were not written into it. Rebuild from DreamAnim Source takes the text."),
			FileText(Path), FText::FromString(Asset.GetName())));
		Notify(FText::Format(LOCTEXT("AssetTextMovedShort", "{0}'s changes were not written: its text changed on disk."), FText::FromString(Asset.GetName())), false, true);
		return false;
	}

	FDabWriteBackOptions Options;
	Options.bStructure = GetDefault<UDreamAnimEditorSettings>()->bWriteBackStructure;
	const FDabWriteBackResult Result = Handler->WriteBack(Asset, Path, Text, Options);
	const bool bWrote = !Result.Text.Equals(Text, ESearchCase::CaseSensitive);
	if (bWrote)
	{
		if (!SaveSource(Path, Result.Text))
		{
			return false;
		}
		Handler->SetGeneratedText(Asset, Result.Text);
	}
	if (bWrote || !Result.Diagnostics.IsEmpty())
	{
		Report(FString::Printf(TEXT("Writing %s back to %s"), *Asset.GetName(), *FPaths::GetCleanFilename(Path)), Result.Diagnostics);
	}
	if (bWrote)
	{
		const int32 NumWritten = Algo::CountIf(Result.Changes, [](const FDabWriteBackChange& Change) { return Change.bWritten; });
		Notify(FText::Format(LOCTEXT("WroteBackAsset", "Wrote {0} change(s) in {1} to {2}"), NumWritten, FText::FromString(Asset.GetName()), FileText(Path)), true,
			!Result.Diagnostics.IsEmpty());
	}
	else if (Result.bRefused)
	{
		Notify(FText::Format(LOCTEXT("NotWrittenAsset", "Some changes in {0} are not in {1}; the log says why."), FText::FromString(Asset.GetName()), FileText(Path)), false, true);
	}
	return bWrote;
}

bool UDreamAnimEditorSubsystem::HasEditorChanges(UAnimBlueprint& Blueprint, TArray<FString>* OutChanges) const
{
	const FString Path = FindSource(Blueprint);
	const FString Base = GetDabGeneratedText(Blueprint);
	if (Path.IsEmpty() || Base.IsEmpty())
	{
		// Made before the text was kept with it: nothing to compare with.
		return false;
	}
	const FDabWriteBackResult Result = WriteBackDabBlueprint(Blueprint, Path, Base);
	if (Result.IsComplete() && Result.Changes.IsEmpty())
	{
		return false;
	}
	if (OutChanges)
	{
		for (const FDabWriteBackChange& Change : Result.Changes)
		{
			OutChanges->Add(FString::Printf(TEXT("%s: %s"), *Change.Location, *Change.What));
		}
		// What was refused says why: an unexpressible function, a structural change while those are off, ...
		for (const Lang::FDabDiagnostic& Diagnostic : Result.Diagnostics)
		{
			OutChanges->Add(FString::Printf(TEXT("%s: %s"), *Diagnostic.Code, *Diagnostic.Message));
		}
		if (OutChanges->IsEmpty())
		{
			OutChanges->Add(Result.bCompared ? FString(TEXT("differences write-back cannot place"))
				: FString(TEXT("the text the Blueprint was made from no longer checks, so it cannot be compared")));
		}
	}
	return true;
}

bool UDreamAnimEditorSubsystem::WriteBack(UAnimBlueprint& Blueprint, bool bExpandTemplates)
{
	const FString Path = FindSource(Blueprint);
	FString Text;
	if (Path.IsEmpty() || !LoadSourceText(Path, Text))
	{
		return false;
	}
	EditedBlueprints.Remove(&Blueprint);

	// The file changed since the two were last in step: writing now would undo that change.
	const FString Base = GetDabGeneratedText(Blueprint);
	if (!Base.IsEmpty() && !Base.Equals(Text, ESearchCase::CaseSensitive))
	{
		TArray<FString> EditorChanges;
		if (HasEditorChanges(Blueprint, &EditorChanges))
		{
			ShowConflict(Blueprint, Path, EditorChanges);
		}
		return false;
	}

	const UDreamAnimEditorSettings* Settings = GetDefault<UDreamAnimEditorSettings>();
	FDabWriteBackOptions Options;
	Options.bStructure = Settings->bWriteBackStructure;
	Options.bExpandTemplates = bExpandTemplates;
	const FDabWriteBackResult Result = WriteBackDabBlueprint(Blueprint, Path, Text, Options);

	const bool bWrote = !Result.Text.Equals(Text, ESearchCase::CaseSensitive);
	if (bWrote)
	{
		if (!SaveSource(Path, Result.Text))
		{
			return false;
		}
		SetDabGeneratedText(Blueprint, Result.Text);
	}

	// Where the nodes are, so a rebuild puts them back there; moving nodes changes nothing else.
	const FString LayoutPath = GetDabLayoutPath(Path);
	const FString Layout = MakeDabLayout(Blueprint);
	FString OldLayout;
	if ((!FFileHelper::LoadFileToString(OldLayout, *LayoutPath) || !OldLayout.Equals(Layout, ESearchCase::CaseSensitive))
		&& !FFileHelper::SaveStringToFile(Layout, *LayoutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		FMessageLog(LogName).Warning(FText::Format(LOCTEXT("CannotWriteLayout", "Cannot write {0}; node positions will not survive a rebuild."), FText::FromString(LayoutPath)));
	}

	// Refusals are repeated only when they change: moving nodes about writes nothing and says nothing.
	TArray<FString> RefusalKeys;
	bool bTemplateBody = false;
	for (const Lang::FDabDiagnostic& Diagnostic : Result.Diagnostics)
	{
		RefusalKeys.Add(FString::Printf(TEXT("%s@%d:%s"), *Diagnostic.Code, Diagnostic.Span.Line, *Diagnostic.Message));
		bTemplateBody |= Diagnostic.Code == TEXT("DAB8001");
	}
	const FString Refusals = FString::Join(RefusalKeys, TEXT("\n"));
	const bool bNewRefusals = !Refusals.IsEmpty() && LastRefusals.FindRef(&Blueprint) != Refusals;
	if (Refusals.IsEmpty())
	{
		LastRefusals.Remove(&Blueprint);
	}
	else
	{
		LastRefusals.Add(&Blueprint, Refusals);
	}
	if (bWrote || bNewRefusals)
	{
		Report(FString::Printf(TEXT("Writing %s back to %s"), *Blueprint.GetName(), *FPaths::GetCleanFilename(Path)), Result.Diagnostics);
	}

	if (bWrote)
	{
		const int32 NumWritten = Algo::CountIf(Result.Changes, [](const FDabWriteBackChange& Change) { return Change.bWritten; });
		Notify(FText::Format(LOCTEXT("WroteBack", "Wrote {0} change(s) in {1} to {2}"), NumWritten, FText::FromString(Blueprint.GetName()), FileText(Path)), true,
			!Result.Diagnostics.IsEmpty());
	}
	if (bNewRefusals && FSlateApplication::IsInitialized())
	{
		FNotificationInfo Info(FText::Format(LOCTEXT("NotWritten", "Some changes in {0} are not in {1}; the log says why."), FText::FromString(Blueprint.GetName()), FileText(Path)));
		Info.ExpireDuration = 8.0f;
		Info.bUseSuccessFailIcons = true;
		Info.Hyperlink = FSimpleDelegate::CreateStatic(&OpenLog);
		Info.HyperlinkText = LOCTEXT("ShowLog", "Show Log");
		if (bTemplateBody)
		{
			const TWeakObjectPtr<UDreamAnimEditorSubsystem> WeakThis(this);
			const TWeakObjectPtr<UAnimBlueprint> WeakBlueprint(&Blueprint);
			Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("ExpandTemplates", "Write the Template Out"),
				LOCTEXT("ExpandTemplatesTip", "Rewrite the graph the node is in from the Blueprint, with its template calls written out, so the change can be kept."),
				FSimpleDelegate::CreateLambda([WeakThis, WeakBlueprint]()
				{
					if (WeakThis.IsValid() && WeakBlueprint.IsValid())
					{
						WeakThis->WriteBack(*WeakBlueprint, true);
					}
				}),
				SNotificationItem::CS_Fail));
		}
		if (const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(SNotificationItem::CS_Fail);
		}
	}
	return bWrote;
}

// ---------------------------------------------------------------------------------------------
// Conflicts
// ---------------------------------------------------------------------------------------------

bool UDreamAnimEditorSubsystem::IsInConflict(const UAnimBlueprint& Blueprint) const
{
	return Conflicts.Contains(TWeakObjectPtr<UAnimBlueprint>(const_cast<UAnimBlueprint*>(&Blueprint)));
}

void UDreamAnimEditorSubsystem::ShowConflict(UAnimBlueprint& Blueprint, const FString& FilePath, const TArray<FString>& EditorChanges)
{
	EditedBlueprints.Remove(&Blueprint);
	CloseConflict(Blueprint);

	FMessageLog Log(LogName);
	Log.Warning(FText::Format(LOCTEXT("ConflictLog", "{0} changed on disk while {1} has changes that are not in it. Nothing was overwritten; pick the text or merge both."),
		FText::FromString(FilePath), FText::FromString(Blueprint.GetName())));
	for (const FString& Change : EditorChanges)
	{
		Log.Info(FText::FromString(TEXT("    ") + Change));
	}

	TSharedPtr<SNotificationItem> Item;
	if (FSlateApplication::IsInitialized())
	{
		const TWeakObjectPtr<UDreamAnimEditorSubsystem> WeakThis(this);
		const TWeakObjectPtr<UAnimBlueprint> WeakBlueprint(&Blueprint);
		FNotificationInfo Info(FText::Format(LOCTEXT("Conflict", "{0} changed on disk, and {1} has changes that are not in it."),
			FileText(FilePath), FText::FromString(Blueprint.GetName())));
		Info.bFireAndForget = false;
		Info.ExpireDuration = 0.0f;
		Info.bUseSuccessFailIcons = false;
		Info.Hyperlink = FSimpleDelegate::CreateStatic(&OpenLog);
		Info.HyperlinkText = LOCTEXT("ShowChanges", "Show the Blueprint's Changes");
		Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("UseText", "Use the Text"),
			LOCTEXT("UseTextTip", "Rebuild the Blueprint from the file; its own changes are dropped."),
			FSimpleDelegate::CreateLambda([WeakThis, WeakBlueprint]()
			{
				if (WeakThis.IsValid() && WeakBlueprint.IsValid())
				{
					WeakThis->UseText(*WeakBlueprint);
				}
			})));
		Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("Merge", "Merge Both"),
			LOCTEXT("MergeTip", "Write the Blueprint's changes into the file next to the file's own, then rebuild the Blueprint from it."),
			FSimpleDelegate::CreateLambda([WeakThis, WeakBlueprint]()
			{
				if (WeakThis.IsValid() && WeakBlueprint.IsValid())
				{
					WeakThis->MergeEditorChanges(*WeakBlueprint);
				}
			})));
		Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item)
		{
			Item->SetCompletionState(SNotificationItem::CS_Pending);
		}
	}
	Conflicts.Add(&Blueprint, Item);
}

void UDreamAnimEditorSubsystem::CloseConflict(UAnimBlueprint& Blueprint)
{
	TWeakPtr<SNotificationItem> Item;
	if (Conflicts.RemoveAndCopyValue(&Blueprint, Item))
	{
		if (const TSharedPtr<SNotificationItem> Pinned = Item.Pin())
		{
			Pinned->SetCompletionState(SNotificationItem::CS_None);
			Pinned->ExpireAndFadeout();
		}
	}
}

UAnimBlueprint* UDreamAnimEditorSubsystem::UseText(UAnimBlueprint& Blueprint)
{
	const FString Path = FindSource(Blueprint);
	CloseConflict(Blueprint);
	return Path.IsEmpty() ? nullptr : RebuildSource(Path, true);
}

bool UDreamAnimEditorSubsystem::MergeEditorChanges(UAnimBlueprint& Blueprint)
{
	const FString Path = FindSource(Blueprint);
	const FString Base = GetDabGeneratedText(Blueprint);
	FString Theirs;
	if (Path.IsEmpty() || Base.IsEmpty() || !LoadSourceText(Path, Theirs))
	{
		return false;
	}

	// The editor's changes written into the text both started from, then both sides' changes to it together.
	FDabWriteBackOptions Options;
	Options.bStructure = GetDefault<UDreamAnimEditorSettings>()->bWriteBackStructure;
	const FDabWriteBackResult Ours = WriteBackDabBlueprint(Blueprint, Path, Base, Options);
	if (!Ours.IsComplete())
	{
		Report(FString::Printf(TEXT("Cannot merge %s into %s"), *Blueprint.GetName(), *FPaths::GetCleanFilename(Path)), Ours.Diagnostics);
		Notify(LOCTEXT("MergeIncomplete", "Some Blueprint changes cannot be written to text; nothing was merged. Resolve those changes before merging."), false, true);
		return false;
	}
	FString Merged;
	TArray<FDabMergeConflict> MergeConflicts;
	if (!MergeDabTexts(Base, Ours.Text, Theirs, Merged, MergeConflicts))
	{
		FMessageLog Log(LogName);
		Log.Warning(FText::Format(LOCTEXT("MergeFailedLog", "{0}: the file and {1} changed the same lines; nothing was merged."), FText::FromString(Path),
			FText::FromString(Blueprint.GetName())));
		for (const FDabMergeConflict& Conflict : MergeConflicts)
		{
			Log.Info(FText::FromString(FString::Printf(TEXT("    line %d: the file has `%s`, the Blueprint `%s`"), Conflict.Line, *Conflict.Theirs, *Conflict.Ours)));
		}
		Notify(FText::Format(LOCTEXT("MergeFailed", "{0} and {1} changed the same lines; edit the text by hand or use it as it is."), FileText(Path),
			FText::FromString(Blueprint.GetName())), false, true);
		return false;
	}
	if (!Ours.Diagnostics.IsEmpty())
	{
		Report(FString::Printf(TEXT("Merging %s into %s"), *Blueprint.GetName(), *FPaths::GetCleanFilename(Path)), Ours.Diagnostics);
	}
	if (!SaveSource(Path, Merged))
	{
		return false;
	}
	CloseConflict(Blueprint);
	return RebuildSource(Path, true) != nullptr;
}

void UDreamAnimEditorSubsystem::OnAssetOpened(UObject* Asset, IAssetEditorInstance* Editor)
{
	UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(Asset);
	const FString Path = Blueprint ? FindSource(*Blueprint) : FString();
	FString Text;
	if (Path.IsEmpty() || !LoadSourceText(Path, Text) || IsInConflict(*Blueprint) || !FSlateApplication::IsInitialized())
	{
		return;
	}
	const FString Base = GetDabGeneratedText(*Blueprint);
	if (Base.IsEmpty() || Base.Equals(Text, ESearchCase::CaseSensitive))
	{
		return;
	}
	// The text changed while the editor was closed, or a rebuild failed.
	const TWeakObjectPtr<UDreamAnimEditorSubsystem> WeakThis(this);
	FNotificationInfo Info(FText::Format(LOCTEXT("Stale", "{0} was built from an older {1}."), FText::FromString(Blueprint->GetName()), FileText(Path)));
	Info.ExpireDuration = 10.0f;
	Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("RebuildNow", "Rebuild"), LOCTEXT("RebuildNowTip", "Rebuild the Blueprint from the text."),
		FSimpleDelegate::CreateLambda([WeakThis, Path]()
		{
			if (WeakThis.IsValid())
			{
				WeakThis->Rebuild(Path);
			}
		}),
		SNotificationItem::CS_None));
	FSlateNotificationManager::Get().AddNotification(Info);
}

// ---------------------------------------------------------------------------------------------
// Decompiling and opening sources
// ---------------------------------------------------------------------------------------------

FString UDreamAnimEditorSubsystem::Decompile(UAnimBlueprint& Blueprint, bool bOverwrite, bool bRebuild, const FString& AnimSetPath)
{
	const FString Path = NormalizeSourcePath(GetDefaultDecompilePath(Blueprint));
	FDabAnimSetMigration Migration;
	Migration.AnimSetPath = AnimSetPath;
	Migration.bPreview = true;
	const FDabDecompileResult Result = DecompileDabBlueprint(Blueprint, FDabLowerOptions(), AnimSetPath.IsEmpty() ? nullptr : &Migration);
	Report(FString::Printf(TEXT("Writing %s as text"), *Blueprint.GetName()), Result.Diagnostics);
	if (!Result.bSucceeded)
	{
		Notify(FText::Format(LOCTEXT("DecompileFailed", "{0} could not be written as text."), FText::FromString(Blueprint.GetName())), false, true);
		return FString();
	}
	FString Existing;
	if (LoadSourceText(Path, Existing) && !Existing.Equals(Result.Text, ESearchCase::CaseSensitive) && !bOverwrite)
	{
		Notify(FText::Format(LOCTEXT("DecompileExists", "{0} exists already and was not replaced."), FileText(Path)), false, false);
		return FString();
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	if (!SaveSource(Path, Result.Text))
	{
		return FString();
	}
	if (!AnimSetPath.IsEmpty())
	{
		Migration.bPreview = false;
		const FDabDecompileResult Applied = DecompileDabBlueprint(Blueprint, FDabLowerOptions(), &Migration);
		Report(FString::Printf(TEXT("Migrating %s animations"), *Blueprint.GetName()), Applied.Diagnostics);
		if (!Applied.bSucceeded)
		{
			return FString();
		}
	}
	RefreshSources();

	if (bRebuild && Result.NumUnsupported > 0)
	{
		Notify(FText::Format(LOCTEXT("NotRebuiltUnsupported", "{0} has {1} thing(s) the text cannot hold (listed at the top of {2}); it was not rebuilt from the text."),
			FText::FromString(Blueprint.GetName()), Result.NumUnsupported, FileText(Path)), false, false);
	}
	else if (bRebuild)
	{
		RebuildSource(Path, true);
	}
	else
	{
		Notify(FText::Format(LOCTEXT("Decompiled", "Wrote {0} as {1}"), FText::FromString(Blueprint.GetName()), FileText(Path)), true, false);
	}
	return Path;
}

void UDreamAnimEditorSubsystem::OpenSource(const FString& FilePath, int32 Line, int32 Column) const
{
	const FString Full = NormalizeSourcePath(FilePath);
	const FString Command = GetDefault<UDreamAnimEditorSettings>()->OpenSourceCommand.TrimStartAndEnd();
	if (!Command.IsEmpty())
	{
		const FString Expanded = Command.Replace(TEXT("{file}"), *Full).Replace(TEXT("{line}"), *LexToString(FMath::Max(Line, 1)))
			.Replace(TEXT("{column}"), *LexToString(FMath::Max(Column, 1)));
#if PLATFORM_WINDOWS
		// Through the shell, so `code` (a .cmd on the PATH) is found; cmd strips the outer quotes.
		FProcHandle Process = FPlatformProcess::CreateProc(TEXT("cmd.exe"), *FString::Printf(TEXT("/c \"%s\""), *Expanded), true, true, true, nullptr, 0, nullptr, nullptr);
#else
		FProcHandle Process = FPlatformProcess::CreateProc(TEXT("/bin/sh"), *FString::Printf(TEXT("-c \"%s\""), *Expanded.Replace(TEXT("\""), TEXT("\\\""))), true, true, true, nullptr, 0, nullptr, nullptr);
#endif
		if (Process.IsValid())
		{
			FPlatformProcess::CloseProc(Process);
			return;
		}
	}
	FPlatformProcess::LaunchFileInDefaultExternalApplication(*Full);
}

// ---------------------------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------------------------

void UDreamAnimEditorSubsystem::Report(const FString& Title, const TArray<Lang::FDabDiagnostic>& Diagnostics) const
{
	if (Diagnostics.IsEmpty())
	{
		return;
	}
	FMessageLog Log(LogName);
	Log.Info(FText::FromString(Title));
	const TWeakObjectPtr<const UDreamAnimEditorSubsystem> WeakThis(this);
	for (const Lang::FDabDiagnostic& Diagnostic : Diagnostics)
	{
		const EMessageSeverity::Type Severity = Diagnostic.Severity == Lang::EDabSeverity::Error ? EMessageSeverity::Error
			: Diagnostic.Severity == Lang::EDabSeverity::Warning ? EMessageSeverity::Warning : EMessageSeverity::Info;
		const TSharedRef<FTokenizedMessage> Message = FTokenizedMessage::Create(Severity);
		const FString Where = FString::Printf(TEXT("%s(%d,%d)"), *FPaths::GetCleanFilename(Diagnostic.FilePath), Diagnostic.Span.Line, Diagnostic.Span.Column);
		if (FPaths::FileExists(Diagnostic.FilePath))
		{
			const FString File = Diagnostic.FilePath;
			const int32 Line = Diagnostic.Span.Line;
			const int32 Column = Diagnostic.Span.Column;
			Message->AddToken(FActionToken::Create(FText::FromString(Where), LOCTEXT("OpenAtLine", "Open the source at this line"),
				FOnActionTokenExecuted::CreateLambda([WeakThis, File, Line, Column]()
				{
					if (WeakThis.IsValid())
					{
						WeakThis->OpenSource(File, Line, Column);
					}
				})));
		}
		else
		{
			Message->AddToken(FTextToken::Create(FText::FromString(Where)));
		}
		Message->AddToken(FTextToken::Create(FText::FromString(FString::Printf(TEXT("%s: %s"), *Diagnostic.Code, *Diagnostic.Message))));
		if (!Diagnostic.Hint.IsEmpty())
		{
			Message->AddToken(FTextToken::Create(FText::FromString(FString::Printf(TEXT("(%s)"), *Diagnostic.Hint))));
		}
		Log.AddMessage(Message);
	}
}

TSharedPtr<SNotificationItem> UDreamAnimEditorSubsystem::Notify(const FText& Message, bool bSuccess, bool bShowLogLink) const
{
	if (!FSlateApplication::IsInitialized())
	{
		return nullptr;
	}
	FNotificationInfo Info(Message);
	Info.ExpireDuration = bSuccess ? 3.0f : 6.0f;
	Info.bUseSuccessFailIcons = true;
	if (bShowLogLink)
	{
		Info.Hyperlink = FSimpleDelegate::CreateStatic(&OpenLog);
		Info.HyperlinkText = LOCTEXT("ShowLog", "Show Log");
	}
	TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
	if (Item)
	{
		Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
	}
	return Item;
}

#undef LOCTEXT_NAMESPACE
