// Copyright 2026 LgThx. All Rights Reserved.

#include "FMaterialLayerMappingCustomization.h"
#include "DetailWidgetRow.h"
#include "DetailLayoutBuilder.h"
#include "IDetailChildrenBuilder.h"
#include "PropertyCustomizationHelpers.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "AssetThumbnail.h"
#include "FleshRingAsset.h"
#include "FleshRingTypes.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"

#define LOCTEXT_NAMESPACE "FMaterialLayerMappingCustomization"

// Thumbnail size (pixels)
static constexpr int32 ThumbnailSize = 64;

FMaterialLayerMappingCustomization::FMaterialLayerMappingCustomization()
{
	// Create thumbnail pool (cache up to 64 thumbnails)
	ThumbnailPool = MakeShared<FAssetThumbnailPool>(64);
}

FMaterialLayerMappingCustomization::~FMaterialLayerMappingCustomization()
{
	// Unbind asset changed delegate
	if (UFleshRingAsset* Asset = CachedAsset.Get())
	{
		if (AssetChangedDelegateHandle.IsValid())
		{
			Asset->OnAssetChanged.Remove(AssetChangedDelegateHandle);
			AssetChangedDelegateHandle.Reset();
		}
	}
}

TSharedRef<IPropertyTypeCustomization> FMaterialLayerMappingCustomization::MakeInstance()
{
	return MakeShared<FMaterialLayerMappingCustomization>();
}

void FMaterialLayerMappingCustomization::CustomizeHeader(
	TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// Cache property handle
	MainPropertyHandle = PropertyHandle;
	CachedSlotIndexHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FMaterialLayerMapping, MaterialSlotIndex));
	TSharedPtr<IPropertyHandle> SlotNameHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FMaterialLayerMapping, MaterialSlotName));

	// Subscribe to asset changed delegate (using AddRaw - released in destructor)
	if (UFleshRingAsset* Asset = GetOuterAsset())
	{
		if (!AssetChangedDelegateHandle.IsValid())
		{
			CachedAsset = Asset;
			AssetChangedDelegateHandle = Asset->OnAssetChanged.AddRaw(
				this, &FMaterialLayerMappingCustomization::OnAssetChanged);
		}
	}

	// Dynamic text binding: automatically updates when property value changes
	TSharedPtr<IPropertyHandle> LocalSlotIndexHandle = CachedSlotIndexHandle;
	auto GetSlotIndexText = [LocalSlotIndexHandle]() -> FText
	{
		int32 SlotIndex = 0;
		if (LocalSlotIndexHandle.IsValid())
		{
			LocalSlotIndexHandle->GetValue(SlotIndex);
		}
		return FText::Format(LOCTEXT("SlotIndexFormat", "[{0}]"), FText::AsNumber(SlotIndex));
	};

	auto GetSlotNameText = [SlotNameHandle]() -> FText
	{
		FName SlotName = NAME_None;
		if (SlotNameHandle.IsValid())
		{
			SlotNameHandle->GetValue(SlotName);
		}
		return FText::FromName(SlotName);
	};

	// Create thumbnail container (stored as member variable for later updates)
	ThumbnailContainer = SNew(SBox)
		.WidthOverride(ThumbnailSize)
		.HeightOverride(ThumbnailSize)
		.Padding(2.0f);

	// Set initial thumbnail
	UpdateThumbnailContent();

	// Header row layout: [Thumbnail] [Index] [Slot Name]
	HeaderRow
		.NameContent()
		[
			SNew(SHorizontalBox)
			// Slot 1: Thumbnail
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				ThumbnailContainer.ToSharedRef()
			]
			// Slot 2: Index (dynamic binding)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda(GetSlotIndexText)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			// Slot 3: Slot name (dynamic binding)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda(GetSlotNameText)
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		.ValueContent()
		.MinDesiredWidth(200.0f)
		[
			PropertyHandle->CreatePropertyValueWidget()
		];
}

void FMaterialLayerMappingCustomization::CustomizeChildren(
	TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// Only display LayerType (MaterialSlotIndex, MaterialSlotName already shown in header)
	TSharedPtr<IPropertyHandle> LayerTypeHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FMaterialLayerMapping, LayerType));

	if (LayerTypeHandle.IsValid())
	{
		ChildBuilder.AddProperty(LayerTypeHandle.ToSharedRef());
	}
}

void FMaterialLayerMappingCustomization::UpdateThumbnailContent()
{
	if (!ThumbnailContainer.IsValid())
	{
		return;
	}

	int32 SlotIndex = 0;
	if (CachedSlotIndexHandle.IsValid())
	{
		CachedSlotIndexHandle->GetValue(SlotIndex);
	}

	UMaterialInterface* Material = GetMaterialForSlot(SlotIndex);

	TSharedPtr<SWidget> ContentWidget;
	if (Material && ThumbnailPool.IsValid())
	{
		TSharedPtr<FAssetThumbnail> Thumbnail = MakeShared<FAssetThumbnail>(
			Material, ThumbnailSize, ThumbnailSize, ThumbnailPool.ToSharedRef());
		ContentWidget = Thumbnail->MakeThumbnailWidget();
	}
	else
	{
		ContentWidget = SNew(STextBlock)
			.Text(LOCTEXT("NoMaterial", "?"))
			.Justification(ETextJustify::Center);
	}

	ThumbnailContainer->SetContent(ContentWidget.ToSharedRef());
}

void FMaterialLayerMappingCustomization::OnAssetChanged(UFleshRingAsset* ChangedAsset)
{
	// Update thumbnail when asset changes
	UpdateThumbnailContent();
}

UFleshRingAsset* FMaterialLayerMappingCustomization::GetOuterAsset() const
{
	if (!MainPropertyHandle.IsValid())
	{
		return nullptr;
	}

	TArray<UObject*> OuterObjects;
	MainPropertyHandle->GetOuterObjects(OuterObjects);

	for (UObject* Obj : OuterObjects)
	{
		if (UFleshRingAsset* Asset = Cast<UFleshRingAsset>(Obj))
		{
			return Asset;
		}
	}

	return nullptr;
}

UMaterialInterface* FMaterialLayerMappingCustomization::GetMaterialForSlot(int32 SlotIndex) const
{
	// Find parent FleshRingAsset
	UFleshRingAsset* Asset = GetOuterAsset();
	if (!Asset)
	{
		return nullptr;
	}

	// Load SkeletalMesh from TSoftObjectPtr
	if (Asset->TargetSkeletalMesh.IsNull())
	{
		return nullptr;
	}

	USkeletalMesh* Mesh = Asset->TargetSkeletalMesh.LoadSynchronous();
	if (!Mesh)
	{
		return nullptr;
	}

	// Get material list
	const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
	if (!Materials.IsValidIndex(SlotIndex))
	{
		return nullptr;
	}

	// Return material for this slot
	return Materials[SlotIndex].MaterialInterface;
}

#undef LOCTEXT_NAMESPACE
