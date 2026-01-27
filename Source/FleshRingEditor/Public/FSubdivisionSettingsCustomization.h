// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"
#include "PropertyHandle.h"
#include "Containers/Ticker.h"

class IDetailChildrenBuilder;
class UFleshRingAsset;
class UFleshRingComponent;
class USkeletalMesh;

/**
 * Property type customization for the FSubdivisionSettings struct
 * Organized into Editor Preview / Runtime Settings subgroups
 */
class FSubdivisionSettingsCustomization : public IPropertyTypeCustomization
{
public:
	FSubdivisionSettingsCustomization();
	virtual ~FSubdivisionSettingsCustomization();

	/** Creates a customization instance (factory function) */
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	/** IPropertyTypeCustomization interface implementation */

	// Header row customization (collapsed view)
	virtual void CustomizeHeader(
		TSharedRef<IPropertyHandle> PropertyHandle,
		FDetailWidgetRow& HeaderRow,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;

	// Children customization (expanded view)
	virtual void CustomizeChildren(
		TSharedRef<IPropertyHandle> PropertyHandle,
		IDetailChildrenBuilder& ChildBuilder,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;

private:
	/** Gets the outer Asset */
	UFleshRingAsset* GetOuterAsset() const;

	/** Whether Subdivision is enabled */
	bool IsSubdivisionEnabled() const;

	/** Refresh Preview button clicked */
	FReply OnRefreshPreviewClicked();

	/** Generate Subdivided Mesh button clicked */
	FReply OnGenerateSubdividedMeshClicked();

	/** Clear Subdivided Mesh button clicked */
	FReply OnClearSubdividedMeshClicked();

	/** Bake button clicked */
	FReply OnBakeMeshClicked();

	/** Clear Baked Mesh button clicked */
	FReply OnClearBakedMeshClicked();

	/** Saves the asset (includes Perforce checkout prompt) */
	void SaveAsset(UFleshRingAsset* Asset);

	// ===== Async Baking Related =====

	/** Async bake tick callback */
	bool OnAsyncBakeTick(float DeltaTime);

	/** Async bake cleanup (called on success/failure) */
	void CleanupAsyncBake(bool bRestorePreviewMesh);

	/** Restores the original preview mesh (for sync bake) */
	void RestoreOriginalPreviewMesh(UFleshRingComponent* PreviewComponent);

	/** Cached main property handle */
	TSharedPtr<IPropertyHandle> MainPropertyHandle;

	// ===== Async Baking State =====

	/** Whether async bake is in progress */
	bool bAsyncBakeInProgress = false;

	/** Async bake frame counter */
	int32 AsyncBakeFrameCount = 0;

	/** Additional wait frame counter after cache becomes valid */
	int32 PostCacheValidFrameCount = 0;

	/** Maximum wait frames for async bake */
	static constexpr int32 MaxAsyncBakeFrames = 30;

	/** Number of wait frames after cache validation (ensures GPU computation completion) */
	static constexpr int32 PostCacheValidWaitFrames = 3;

	/** Asset for async bake (weak reference) */
	TWeakObjectPtr<UFleshRingAsset> AsyncBakeAsset;

	/** Component for async bake (weak reference) */
	TWeakObjectPtr<UFleshRingComponent> AsyncBakeComponent;

	/** Original preview mesh (for restoration, weak reference) */
	TWeakObjectPtr<USkeletalMesh> OriginalPreviewMesh;

	/** Ticker delegate handle */
	FTSTicker::FDelegateHandle TickerHandle;
};
