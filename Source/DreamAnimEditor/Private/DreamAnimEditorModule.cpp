// Copyright DreamMoon. All Rights Reserved.

#include "DreamAnimEditorMenus.h"
#include "DreamAnimSet.h"
#include "DreamAnimSetSlotCustomization.h"
#include "MessageLogModule.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "DreamAnimEditor"

class FDreamAnimEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		PropertyEditor.RegisterCustomPropertyTypeLayout(FDreamAnimSetSlot::StaticStruct()->GetFName(),
			FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FDreamAnimSetSlotCustomization::MakeInstance));

		FMessageLogModule& MessageLog = FModuleManager::LoadModuleChecked<FMessageLogModule>(TEXT("MessageLog"));
		FMessageLogInitializationOptions LogOptions;
		LogOptions.bShowFilters = true;
		LogOptions.bAllowClear = true;
		MessageLog.RegisterLogListing(TEXT("DreamAnim"), LOCTEXT("LogLabel", "Dream Anim"), LogOptions);

		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([this]()
		{
			FToolMenuOwnerScoped OwnerScoped(this);
			DreamAnimEditorMenus::RegisterMenus();
		}));
	}

	virtual void ShutdownModule() override
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		if (FMessageLogModule* MessageLog = FModuleManager::GetModulePtr<FMessageLogModule>(TEXT("MessageLog")))
		{
			MessageLog->UnregisterLogListing(TEXT("DreamAnim"));
		}
		if (FPropertyEditorModule* PropertyEditor = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
		{
			PropertyEditor->UnregisterCustomPropertyTypeLayout(FDreamAnimSetSlot::StaticStruct()->GetFName());
		}
	}
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FDreamAnimEditorModule, DreamAnimEditor)
