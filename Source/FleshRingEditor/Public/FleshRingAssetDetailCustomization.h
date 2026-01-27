// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class IDetailLayoutBuilder;
class UFleshRingAsset;

/**
 * Detail Panel customizer for UFleshRingAsset
 * Handles grouping and button placement for Subdivision Settings category
 */
class FFleshRingAssetDetailCustomization : public IDetailCustomization
{
public:
	/** Create DetailCustomization instance (factory function) */
	static TSharedRef<IDetailCustomization> MakeInstance();

	/** IDetailCustomization interface implementation */
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	/** Cache Asset being edited */
	TWeakObjectPtr<UFleshRingAsset> CachedAsset;

	/** Check bEnableSubdivision (for button activation) */
	bool IsSubdivisionEnabled() const;

	/** Button click handlers */
	FReply OnRefreshPreviewClicked();
	FReply OnGenerateRuntimeMeshClicked();
	FReply OnClearRuntimeMeshClicked();
};
