// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SEditorViewport.h"
#include "SCommonEditorViewportToolbarBase.h"
#include "EditorModeManager.h"

class FFleshRingPreviewScene;
class FFleshRingEditorViewportClient;
class UFleshRingAsset;
class FFleshRingEdMode;

/**
 * FleshRing editor viewport widget
 * Displays skeletal mesh and Rings in 3D
 */
class SFleshRingEditorViewport : public SEditorViewport, public ICommonEditorViewportToolbarInfoProvider
{
public:
	SLATE_BEGIN_ARGS(SFleshRingEditorViewport) {}
		SLATE_ARGUMENT(UFleshRingAsset*, Asset)
		SLATE_ARGUMENT(FEditorModeTools*, ModeTools)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SFleshRingEditorViewport();

	/** Set Asset */
	void SetAsset(UFleshRingAsset* InAsset);

	/** Refresh preview scene (full recreation - on slider drag end) */
	void RefreshPreview();

	/** Update only Ring transforms (without flickering - during slider drag) */
	void UpdateRingTransformsOnly(int32 DirtyRingIndex = INDEX_NONE);

	/** Regenerate SDF only (during VirtualBand drag - without component recreation) */
	void RefreshSDFOnly();

	/** Return preview scene */
	TSharedPtr<FFleshRingPreviewScene> GetPreviewScene() const { return PreviewScene; }

	/** Return viewport client */
	TSharedPtr<FFleshRingEditorViewportClient> GetViewportClient() const { return ViewportClient; }

	/** Create toolbar widget */
	TSharedRef<SWidget> MakeToolbar();

	// ICommonEditorViewportToolbarInfoProvider interface
	virtual TSharedRef<SEditorViewport> GetViewportWidget() override;
	virtual TSharedPtr<FExtender> GetExtenders() const override;
	virtual void OnFloatingButtonClicked() override;

protected:
	// SEditorViewport interface
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;
	virtual void PopulateViewportOverlays(TSharedRef<SOverlay> Overlay) override;
	virtual void OnFocusViewportToSelection() override;
	virtual void BindCommands() override;

private:
	/** Preview scene */
	TSharedPtr<FFleshRingPreviewScene> PreviewScene;

	/** Viewport client */
	TSharedPtr<FFleshRingEditorViewportClient> ViewportClient;

	/** Editor Mode Tools (owned by FAssetEditorToolkit) */
	FEditorModeTools* ModeTools = nullptr;

	/** FleshRing EdMode */
	FFleshRingEdMode* FleshRingEdMode = nullptr;

	/** Asset being edited */
	TWeakObjectPtr<UFleshRingAsset> EditingAsset;
};
