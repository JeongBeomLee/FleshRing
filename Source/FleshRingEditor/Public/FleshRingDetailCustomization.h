// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class IDetailLayoutBuilder;
class IPropertyHandle;
class UFleshRingComponent;
class USkeletalMesh;
class FAssetThumbnailPool;
struct FAssetData;

/**
 * Detail Panel customizer for UFleshRingComponent
 * Handles property grouping, category organization, custom widgets, etc.
 */
class FFleshRingDetailCustomization : public IDetailCustomization
{
public:
	/** Create DetailCustomization instance (factory function) */
	static TSharedRef<IDetailCustomization> MakeInstance();

	/** IDetailCustomization interface implementation */
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	/** Cache selected components */
	TArray<TWeakObjectPtr<UObject>> SelectedObjects;

	/** FleshRingAsset filtering: Only show Assets matching Owner's SkeletalMesh */
	bool OnShouldFilterAsset(const FAssetData& AssetData) const;

	/** Get Owner's SkeletalMesh */
	USkeletalMesh* GetOwnerSkeletalMesh() const;

	/** Get first selected FleshRingComponent */
	UFleshRingComponent* GetFirstSelectedComponent() const;
};
