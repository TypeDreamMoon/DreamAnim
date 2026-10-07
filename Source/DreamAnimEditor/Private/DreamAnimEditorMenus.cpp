// Copyright DreamMoon. All Rights Reserved.
//
// Menu entries: on an Animation Blueprint in the Content Browser (open its source, rebuild it, write its
// changes back; or write one not made from text as text), and under Tools for every source at once.

#include "DreamAnimEditorMenus.h"

#include "Animation/AnimBlueprint.h"
#include "ContentBrowserMenuContexts.h"
#include "ContentBrowserModule.h"
#include "DreamAnimEditorSubsystem.h"
#include "DreamAnimSet.h"
#include "Driver/DabDriver.h"
#include "Editor.h"
#include "Logging/MessageLog.h"
#include "IContentBrowserSingleton.h"
#include "Misc/MessageDialog.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "DreamAnimEditor"

namespace DreamAnimEditorMenus
{
	UDreamAnimEditorSubsystem* GetSubsystem()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UDreamAnimEditorSubsystem>() : nullptr;
	}

	TArray<UAnimBlueprint*> LoadBlueprints(const TArray<FAssetData>& Assets)
	{
		TArray<UAnimBlueprint*> Blueprints;
		for (const FAssetData& Asset : Assets)
		{
			if (UAnimBlueprint* Blueprint = Cast<UAnimBlueprint>(Asset.GetAsset()))
			{
				Blueprints.Add(Blueprint);
			}
		}
		return Blueprints;
	}

	/** The animation set the Blueprints' animations go into, picked or named in a save dialog; empty when cancelled. */
	FString PickAnimSet(const TArray<UAnimBlueprint*>& Blueprints)
	{
		FSaveAssetDialogConfig Config;
		Config.DialogTitleOverride = LOCTEXT("PickAnimSet", "Animation Set for the Blueprints' Animations");
		Config.DefaultPath = FPackageName::GetLongPackagePath(Blueprints[0]->GetPackage()->GetName());
		Config.DefaultAssetName = TEXT("DAS_") + Blueprints[0]->GetName().Replace(TEXT("AB_"), TEXT(""));
		Config.AssetClassNames.Add(UDreamAnimSet::StaticClass()->GetClassPathName());
		// An existing set is fine: the Blueprints' groups are added to it, nothing in it is replaced.
		Config.ExistingAssetPolicy = ESaveAssetDialogExistingAssetPolicy::AllowButWarn;
		return IContentBrowserSingleton::Get().CreateModalSaveAssetDialog(Config);
	}

	/**
	 * Blueprints not made from text, as text; existing files are replaced only when the user says so. With
	 * bAnimSet, their animations move into an animation set the user picks.
	 */
	void Decompile(const TArray<FAssetData>& Assets, bool bAnimSet)
	{
		UDreamAnimEditorSubsystem* Subsystem = GetSubsystem();
		const TArray<UAnimBlueprint*> Blueprints = LoadBlueprints(Assets);
		if (!Subsystem || Blueprints.IsEmpty())
		{
			return;
		}
		const FString AnimSetPath = bAnimSet ? PickAnimSet(Blueprints) : FString();
		if (bAnimSet && AnimSetPath.IsEmpty())
		{
			return;
		}
		TArray<FString> Existing;
		for (const UAnimBlueprint* Blueprint : Blueprints)
		{
			const FString Path = UE::DreamAnim::Compiler::GetDefaultDecompilePath(*Blueprint);
			if (FPaths::FileExists(Path))
			{
				Existing.Add(Path);
			}
		}
		const bool bOverwrite = Existing.IsEmpty() || FMessageDialog::Open(EAppMsgType::YesNo,
			FText::Format(LOCTEXT("ReplaceSources", "These files exist already:\n\n{0}\n\nReplace them?"), FText::FromString(FString::Join(Existing, TEXT("\n"))))) == EAppReturnType::Yes;
		const bool bRebuild = FMessageDialog::Open(EAppMsgType::YesNo, LOCTEXT("RebuildAfterDecompile",
			"Rebuild the Blueprints from the new text?\n\nFrom then on the text is their source: changes made in the editor are written back to it. "
			"A Blueprint with something the text cannot hold is left as it is.")) == EAppReturnType::Yes;
		FString First;
		for (UAnimBlueprint* Blueprint : Blueprints)
		{
			const FString Path = Subsystem->Decompile(*Blueprint, bOverwrite, bRebuild, AnimSetPath);
			if (First.IsEmpty())
			{
				First = Path;
			}
		}
		if (!First.IsEmpty())
		{
			Subsystem->OpenSource(First);
		}
	}

	void AddAssetActions(FToolMenuSection& Section)
	{
		const UContentBrowserAssetContextMenuContext* Context = Section.FindContext<UContentBrowserAssetContextMenuContext>();
		const UDreamAnimEditorSubsystem* Subsystem = GetSubsystem();
		if (!Context || !Subsystem)
		{
			return;
		}
		TArray<FAssetData> WithSource;
		TArray<FAssetData> WithoutSource;
		for (const FAssetData& Asset : Context->SelectedAssets)
		{
			(Subsystem->GetIndex().FindByAsset(Asset.GetObjectPathString()) ? WithSource : WithoutSource).Add(Asset);
		}

		if (!WithSource.IsEmpty())
		{
			Section.AddMenuEntry(TEXT("DreamAnim_OpenSource"), LOCTEXT("OpenSource", "Open DreamAnim Source"),
				LOCTEXT("OpenSourceTip", "Open the text the Blueprint is made from."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([WithSource]()
				{
					if (UDreamAnimEditorSubsystem* Current = GetSubsystem())
					{
						for (const FAssetData& Asset : WithSource)
						{
							if (const UE::DreamAnim::Compiler::FDabSourceEntry* Entry = Current->GetIndex().FindByAsset(Asset.GetObjectPathString()))
							{
								Current->OpenSource(Entry->FilePath);
							}
						}
					}
				})));
			Section.AddMenuEntry(TEXT("DreamAnim_Rebuild"), LOCTEXT("Rebuild", "Rebuild from DreamAnim Source"),
				LOCTEXT("RebuildTip", "Compile the text into the Blueprint again; changes in the Blueprint that are not in the text are dropped."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([WithSource]()
				{
					if (UDreamAnimEditorSubsystem* Current = GetSubsystem())
					{
						for (const FAssetData& Asset : WithSource)
						{
							if (const UE::DreamAnim::Compiler::FDabSourceEntry* Entry = Current->GetIndex().FindByAsset(Asset.GetObjectPathString()))
							{
								Current->Rebuild(Entry->FilePath);
							}
						}
					}
				})));
			Section.AddMenuEntry(TEXT("DreamAnim_WriteBack"), LOCTEXT("WriteBack", "Write Changes to DreamAnim Source"),
				LOCTEXT("WriteBackTip", "Write what changed in the Blueprint into its text now."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([WithSource]()
				{
					if (UDreamAnimEditorSubsystem* Current = GetSubsystem())
					{
						for (UAnimBlueprint* Blueprint : LoadBlueprints(WithSource))
						{
							Current->WriteBack(*Blueprint);
						}
					}
				})));
		}
		if (!WithoutSource.IsEmpty())
		{
			Section.AddMenuEntry(TEXT("DreamAnim_Decompile"), LOCTEXT("Decompile", "Write as DreamAnim Text"),
				LOCTEXT("DecompileTip", "Write the Blueprint as a DreamAnim source under the project's DAnim folder."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([WithoutSource]() { Decompile(WithoutSource, false); })));
			Section.AddMenuEntry(TEXT("DreamAnim_DecompileToSet"), LOCTEXT("DecompileToSet", "Write as DreamAnim Text with an Animation Set..."),
				LOCTEXT("DecompileToSetTip", "Write the Blueprint as a DreamAnim source whose animations are slots of an animation set, and fill the set with them."),
				FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([WithoutSource]() { Decompile(WithoutSource, true); })));
		}
	}

	void RegisterMenus()
	{
		if (UToolMenu* AssetMenu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(UAnimBlueprint::StaticClass()))
		{
			FToolMenuSection& Section = AssetMenu->FindOrAddSection(TEXT("GetAssetActions"));
			Section.AddDynamicEntry(TEXT("DreamAnim"), FNewToolMenuSectionDelegate::CreateStatic(&AddAssetActions));
		}

		if (UToolMenu* Tools = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools")))
		{
			FToolMenuSection& Section = Tools->FindOrAddSection(TEXT("DreamAnim"), LOCTEXT("ToolsSection", "Dream Anim"));
			Section.AddMenuEntry(TEXT("DreamAnim_RebuildAll"), LOCTEXT("RebuildAll", "Rebuild All from DreamAnim Sources"),
				LOCTEXT("RebuildAllTip", "Compile every .dab and .dai under the DAnim folders into its asset."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([]()
				{
					if (UDreamAnimEditorSubsystem* Subsystem = GetSubsystem())
					{
						Subsystem->RebuildAll();
					}
				})));
			Section.AddMenuEntry(TEXT("DreamAnim_Rescan"), LOCTEXT("Rescan", "Rescan DreamAnim Sources"),
				LOCTEXT("RescanTip", "Find the sources under the DAnim folders again."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([]()
				{
					if (UDreamAnimEditorSubsystem* Subsystem = GetSubsystem())
					{
						Subsystem->RefreshSources();
					}
				})));
			Section.AddMenuEntry(TEXT("DreamAnim_Log"), LOCTEXT("ShowLog", "DreamAnim Log"),
				LOCTEXT("ShowLogTip", "What rebuilds and write-backs reported."), FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([]() { FMessageLog(TEXT("DreamAnim")).Open(EMessageSeverity::Info, true); })));
		}
	}
}

#undef LOCTEXT_NAMESPACE
