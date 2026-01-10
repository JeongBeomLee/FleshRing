// Copyright Epic Games, Inc. All Rights Reserved.

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
 * FleshRing 에디터 뷰포트 위젯
 * 스켈레탈 메시와 Ring을 3D로 표시
 */
class SFleshRingEditorViewport : public SEditorViewport, public ICommonEditorViewportToolbarInfoProvider
{
public:
	SLATE_BEGIN_ARGS(SFleshRingEditorViewport) {}
		SLATE_ARGUMENT(UFleshRingAsset*, Asset)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SFleshRingEditorViewport();

	/** Asset 설정 */
	void SetAsset(UFleshRingAsset* InAsset);

	/** 프리뷰 씬 갱신 (전체 재생성 - 슬라이더 드래그 종료 시) */
	void RefreshPreview();

	/** Ring 트랜스폼만 업데이트 (깜빡임 없이 - 슬라이더 드래그 중) */
	void UpdateRingTransformsOnly();

	/** SDF만 재생성 (VirtualBand 드래그 중 - 컴포넌트 재생성 없이) */
	void RefreshSDFOnly();

	/** 프리뷰 씬 반환 */
	TSharedPtr<FFleshRingPreviewScene> GetPreviewScene() const { return PreviewScene; }

	/** 뷰포트 클라이언트 반환 */
	TSharedPtr<FFleshRingEditorViewportClient> GetViewportClient() const { return ViewportClient; }

	/** 툴바 위젯 생성 */
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
	/** 프리뷰 씬 */
	TSharedPtr<FFleshRingPreviewScene> PreviewScene;

	/** 뷰포트 클라이언트 */
	TSharedPtr<FFleshRingEditorViewportClient> ViewportClient;

	/** Editor Mode Tools */
	TSharedPtr<FEditorModeTools> ModeTools;

	/** FleshRing EdMode */
	FFleshRingEdMode* FleshRingEdMode = nullptr;

	/** 편집 중인 Asset */
	TWeakObjectPtr<UFleshRingAsset> EditingAsset;
};
