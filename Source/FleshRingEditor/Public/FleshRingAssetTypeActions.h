// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetTypeActions_Base.h"

class UFleshRingAsset;

/**
 * Content Browser integration for FleshRing Asset
 * - Open dedicated editor on double-click
 * - Icon/color configuration
 * - Right-click menu customization
 */
class FFleshRingAssetTypeActions : public FAssetTypeActions_Base
{
public:
	// FAssetTypeActions_Base interface
	virtual FText GetName() const override;
	virtual FColor GetTypeColor() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual uint32 GetCategories() override;

	/** Open editor on double-click */
	virtual void OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<class IToolkitHost> EditWithinLevelEditor = TSharedPtr<IToolkitHost>()) override;
};
