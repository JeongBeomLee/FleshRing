// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"
#include "PropertyHandle.h"

class IDetailChildrenBuilder;
class UFleshRingAsset;
class FAssetThumbnailPool;
class SBox;

/**
 * Property type customizer for FMaterialLayerMapping struct
 * Displays material thumbnails for easy visual identification
 */
class FMaterialLayerMappingCustomization : public IPropertyTypeCustomization
{
public:
	FMaterialLayerMappingCustomization();
	virtual ~FMaterialLayerMappingCustomization();

	/** Create customizer instance (factory function) */
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	/** IPropertyTypeCustomization interface implementation */

	// Header row customization (collapsed view - thumbnail + name display)
	virtual void CustomizeHeader(
		TSharedRef<IPropertyHandle> PropertyHandle,
		FDetailWidgetRow& HeaderRow,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;

	// Children customization (expanded view - LayerType only)
	virtual void CustomizeChildren(
		TSharedRef<IPropertyHandle> PropertyHandle,
		IDetailChildrenBuilder& ChildBuilder,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;

private:
	/** Get parent FleshRingAsset */
	UFleshRingAsset* GetOuterAsset() const;

	/** Get material by MaterialSlotIndex */
	UMaterialInterface* GetMaterialForSlot(int32 SlotIndex) const;

	/** Update thumbnail content */
	void UpdateThumbnailContent();

	/** Asset changed callback */
	void OnAssetChanged(UFleshRingAsset* ChangedAsset);

	/** Cache main property handle */
	TSharedPtr<IPropertyHandle> MainPropertyHandle;

	/** Cache slot index handle (for thumbnail update) */
	TSharedPtr<IPropertyHandle> CachedSlotIndexHandle;

	/** Thumbnail pool (for editor thumbnail rendering) */
	TSharedPtr<FAssetThumbnailPool> ThumbnailPool;

	/** Thumbnail container (for dynamic updates) */
	TSharedPtr<SBox> ThumbnailContainer;

	/** Asset changed delegate handle */
	FDelegateHandle AssetChangedDelegateHandle;

	/** Cached asset pointer (for delegate release) */
	TWeakObjectPtr<UFleshRingAsset> CachedAsset;
};
