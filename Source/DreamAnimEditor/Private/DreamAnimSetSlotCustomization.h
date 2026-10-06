// Copyright DreamMoon. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"

/**
 * One row per slot: its name, and a picker that offers only assets of the slot's type made for the
 * set's skeleton. Name and type come from the Blueprint and are not edited here.
 */
class FDreamAnimSetSlotCustomization : public IPropertyTypeCustomization
{
public:
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	virtual void CustomizeHeader(TSharedRef<IPropertyHandle> StructHandle, FDetailWidgetRow& Row, IPropertyTypeCustomizationUtils& Utils) override;
	virtual void CustomizeChildren(TSharedRef<IPropertyHandle> StructHandle, IDetailChildrenBuilder& Builder, IPropertyTypeCustomizationUtils& Utils) override;
};
