// Copyright DreamMoon. All Rights Reserved.

#include "DreamAnimSetFactory.h"

#include "AssetTypeCategories.h"
#include "DreamAnimSet.h"

#define LOCTEXT_NAMESPACE "DreamAnimSetFactory"

UDreamAnimSetFactory::UDreamAnimSetFactory()
{
	SupportedClass = UDreamAnimSet::StaticClass();
	bCreateNew = true;
	bEditAfterNew = true;
}

UObject* UDreamAnimSetFactory::FactoryCreateNew(UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
	return NewObject<UDreamAnimSet>(InParent, Class, Name, Flags);
}

uint32 UDreamAnimSetFactory::GetMenuCategories() const
{
	return EAssetTypeCategories::Animation;
}

FText UDreamAnimSetFactory::GetDisplayName() const
{
	return LOCTEXT("DisplayName", "Dream Anim Set");
}

#undef LOCTEXT_NAMESPACE
