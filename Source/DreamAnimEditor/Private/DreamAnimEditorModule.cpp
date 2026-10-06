// Copyright DreamMoon. All Rights Reserved.

#include "DreamAnimSet.h"
#include "DreamAnimSetSlotCustomization.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"

class FDreamAnimEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		PropertyEditor.RegisterCustomPropertyTypeLayout(FDreamAnimSetSlot::StaticStruct()->GetFName(),
			FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FDreamAnimSetSlotCustomization::MakeInstance));
	}

	virtual void ShutdownModule() override
	{
		if (FPropertyEditorModule* PropertyEditor = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
		{
			PropertyEditor->UnregisterCustomPropertyTypeLayout(FDreamAnimSetSlot::StaticStruct()->GetFName());
		}
	}
};

IMPLEMENT_MODULE(FDreamAnimEditorModule, DreamAnimEditor)
