// Copyright DreamMoon. All Rights Reserved.

#include "DreamAnimSetSlotCustomization.h"

#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetData.h"
#include "DetailWidgetRow.h"
#include "DreamAnimSet.h"
#include "IDetailChildrenBuilder.h"
#include "IPropertyTypeCustomization.h"
#include "PropertyCustomizationHelpers.h"
#include "PropertyHandle.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "DreamAnimSetSlotCustomization"

namespace UE::DreamAnim::Editor::SlotCustomizationPrivate
{
	/** `AnimSequence` -> UAnimSequence; anything that is not an animation asset type -> UAnimationAsset. */
	const UClass* ResolveSlotClass(FName Type)
	{
		const UClass* Class = Type.IsNone() ? nullptr : FindFirstObject<UClass>(*Type.ToString(), EFindFirstObjectOptions::NativeFirst);
		return Class && Class->IsChildOf(UAnimationAsset::StaticClass()) ? Class : UAnimationAsset::StaticClass();
	}

	/** The skeleton of the set being edited, when there is exactly one set. */
	const USkeleton* FindSetSkeleton(const IPropertyHandle& Handle)
	{
		TArray<UObject*> Outers;
		Handle.GetOuterObjects(Outers);
		const UDreamAnimSet* Set = Outers.Num() == 1 ? Cast<UDreamAnimSet>(Outers[0]) : nullptr;
		return Set ? Set->Skeleton.Get() : nullptr;
	}
}

TSharedRef<IPropertyTypeCustomization> FDreamAnimSetSlotCustomization::MakeInstance()
{
	return MakeShared<FDreamAnimSetSlotCustomization>();
}

void FDreamAnimSetSlotCustomization::CustomizeHeader(TSharedRef<IPropertyHandle> StructHandle, FDetailWidgetRow& Row, IPropertyTypeCustomizationUtils& Utils)
{
	using namespace UE::DreamAnim::Editor::SlotCustomizationPrivate;

	const TSharedPtr<IPropertyHandle> NameHandle = StructHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FDreamAnimSetSlot, Name));
	const TSharedPtr<IPropertyHandle> TypeHandle = StructHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FDreamAnimSetSlot, Type));
	const TSharedPtr<IPropertyHandle> AssetHandle = StructHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FDreamAnimSetSlot, Asset));
	const TSharedPtr<IPropertyHandle> OrphanHandle = StructHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FDreamAnimSetSlot, bOrphan));
	if (!NameHandle || !TypeHandle || !AssetHandle || !OrphanHandle)
	{
		return;
	}

	FName SlotName;
	FName SlotType;
	bool bOrphan = false;
	NameHandle->GetValue(SlotName);
	TypeHandle->GetValue(SlotType);
	OrphanHandle->GetValue(bOrphan);

	FText Label = FText::FromName(SlotName);
	if (bOrphan)
	{
		Label = FText::Format(LOCTEXT("OrphanLabel", "{0} (no longer used)"), Label);
	}
	const FText ToolTip = bOrphan
		? FText::Format(LOCTEXT("OrphanToolTip", "{0}. The Blueprint no longer declares this slot; the animation is kept until you remove it."), FText::FromName(SlotType))
		: FText::FromName(SlotType);

	// Only assets of the slot's type, and only those whose skeleton fits the set's.
	const USkeleton* Skeleton = FindSetSkeleton(*StructHandle);
	const TWeakObjectPtr<const USkeleton> WeakSkeleton(Skeleton);

	Row.NameContent()
	[
		SNew(STextBlock)
		.Text(Label)
		.ToolTipText(ToolTip)
		.Font(Utils.GetRegularFont())
		.ColorAndOpacity(bOrphan ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground())
	]
	.ValueContent()
	.MinDesiredWidth(300.f)
	[
		SNew(SObjectPropertyEntryBox)
		.PropertyHandle(AssetHandle)
		.AllowedClass(ResolveSlotClass(SlotType))
		.ThumbnailPool(Utils.GetThumbnailPool())
		.OnShouldFilterAsset_Lambda([WeakSkeleton](const FAssetData& AssetData)
		{
			const USkeleton* SetSkeleton = WeakSkeleton.Get();
			return SetSkeleton && !SetSkeleton->IsCompatibleForEditor(AssetData);
		})
	];
}

void FDreamAnimSetSlotCustomization::CustomizeChildren(TSharedRef<IPropertyHandle> StructHandle, IDetailChildrenBuilder& Builder, IPropertyTypeCustomizationUtils& Utils)
{
	// Everything a slot has is on its header row.
}

#undef LOCTEXT_NAMESPACE
