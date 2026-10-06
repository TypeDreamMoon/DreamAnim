// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "DreamAnimSetFactory.generated.h"

/** Content Browser > Animation > Dream Anim Set. The slots are added by the Blueprints that use the set. */
UCLASS()
class DREAMANIMEDITOR_API UDreamAnimSetFactory : public UFactory
{
	GENERATED_BODY()

public:
	UDreamAnimSetFactory();

	virtual UObject* FactoryCreateNew(UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn) override;
	virtual uint32 GetMenuCategories() const override;
	virtual FText GetDisplayName() const override;
};
