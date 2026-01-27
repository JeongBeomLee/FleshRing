// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/UObjectGlobals.h"
#include "FleshRingEditorViewportClient.h"

class UFleshRingAsset;
class UFleshRingComponent;
class SFleshRingEditorViewport;
class SFleshRingSkeletonTree;
class IDetailsView;

/**
 * Dedicated editor for FleshRing Asset
 * Provides 3D viewport and Details panel like Physics Asset Editor
 */
class FFleshRingAssetEditor : public FAssetEditorToolkit
{
public:
	FFleshRingAssetEditor();
	virtual ~FFleshRingAssetEditor();

	/** Initialize editor */
	void InitFleshRingAssetEditor(
		const EToolkitMode::Type Mode,
		const TSharedPtr<IToolkitHost>& InitToolkitHost,
		UFleshRingAsset* InAsset);

	// FAssetEditorToolkit interface
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FText GetToolkitName() const override;
	virtual FText GetToolkitToolTipText() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FString GetDocumentationLink() const override;

	// IToolkit interface
	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& TabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& TabManager) override;

	/** Return the Asset being edited */
	UFleshRingAsset* GetEditingAsset() const { return EditingAsset; }

	/** Refresh viewport (full regeneration) */
	void RefreshViewport();

	/** Update only Ring transforms (prevents flickering) */
	void UpdateRingTransformsOnly(int32 DirtyRingIndex = INDEX_NONE);

	/** Regenerate SDF only (during VirtualBand drag - without component regeneration) */
	void RefreshSDFOnly();

	/** Return FleshRingComponent from PreviewScene (for Subdivision data access) */
	UFleshRingComponent* GetPreviewFleshRingComponent() const;

	/** Force regenerate preview mesh (invalidate cache then regenerate) */
	void ForceRefreshPreviewMesh();

	/** Force tick PreviewScene (for viewport rendering in modal dialog) */
	void TickPreviewScene(float DeltaTime);

	/** Show/hide bake overlay (blocks input + shows progress) */
	void ShowBakeOverlay(bool bShow, const FText& Message = FText::GetEmpty());

	/** Bake overlay state */
	bool IsBakeOverlayVisible() const { return bBakeOverlayVisible; }

	/** Return viewport widget (for PreviewScene access during bake) */
	TSharedPtr<SFleshRingEditorViewport> GetViewportWidget() const { return ViewportWidget; }

private:
	/** Spawn Skeleton Tree tab */
	TSharedRef<SDockTab> SpawnTab_SkeletonTree(const FSpawnTabArgs& Args);

	/** Spawn Viewport tab */
	TSharedRef<SDockTab> SpawnTab_Viewport(const FSpawnTabArgs& Args);

	/** Spawn Details tab */
	TSharedRef<SDockTab> SpawnTab_Details(const FSpawnTabArgs& Args);

	/** Spawn Preview Settings tab */
	TSharedRef<SDockTab> SpawnTab_PreviewSettings(const FSpawnTabArgs& Args);

	/** Create Details View */
	void CreateDetailsView();

	/** Bone selection callback (from Skeleton Tree) */
	void OnBoneSelected(FName BoneName);

	/** Ring selection callback (from Skeleton Tree) */
	void OnRingSelected(int32 RingIndex);

	/** Ring selection callback (picking from viewport) */
	void OnRingSelectedInViewport(int32 RingIndex, EFleshRingSelectionType SelectionType);

	/** Bone selection cleared callback (from viewport) */
	void OnBoneSelectionCleared();

	/** Bone selection callback (picking from viewport) */
	void OnBoneSelectedInViewport(FName BoneName);

	/** Ring add request callback (from skeleton tree - includes mesh selection) */
	void OnAddRingRequested(FName BoneName, UStaticMesh* SelectedMesh);

	/** Ring add request callback (from viewport right-click - includes position and mesh) */
	void OnAddRingAtPositionRequested(FName BoneName, const FVector& LocalOffset, const FRotator& LocalRotation, UStaticMesh* SelectedMesh);

	/** Camera focus request callback */
	void OnFocusCameraRequested();

	/** Ring deletion common handler (called from viewport/tree/details) */
	void HandleRingDeleted();

	/** Property change callback */
	void OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent);

	/** Undo/Redo callback */
	void OnObjectTransacted(UObject* Object, const FTransactionObjectEvent& TransactionEvent);

	/** Apply selection state from asset to viewport (called on Undo/Redo) */
	void ApplySelectionFromAsset();

	/** Ring selection change callback (from details panel) */
	void OnRingSelectionChangedFromDetails(int32 RingIndex);

private:
	/** Asset being edited */
	UFleshRingAsset* EditingAsset = nullptr;

	/** Skeleton Tree widget */
	TSharedPtr<SFleshRingSkeletonTree> SkeletonTreeWidget;

	/** Viewport widget */
	TSharedPtr<SFleshRingEditorViewport> ViewportWidget;

	/** Details View */
	TSharedPtr<IDetailsView> DetailsView;

	/** Property change delegate handle */
	FDelegateHandle OnPropertyChangedHandle;

	/** Undo/Redo delegate handle */
	FDelegateHandle OnObjectTransactedHandle;

	/** Flag for Ring selection from viewport (prevents circular calls) */
	bool bSyncingFromViewport = false;

	/** Bake overlay visibility state */
	bool bBakeOverlayVisible = false;

	/** Bake overlay window */
	TSharedPtr<SWindow> BakeOverlayWindow;
};
