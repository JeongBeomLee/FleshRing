// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SEditorViewport.h"
#include "SCommonEditorViewportToolbarBase.h"

class FFleshRingPreviewScene;
class FFleshRingEditorViewportClient;
class UFleshRingAsset;

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

	/** 프리뷰 씬 갱신 */
	void RefreshPreview();

	/** 프리뷰 씬 반환 */
	TSharedPtr<FFleshRingPreviewScene> GetPreviewScene() const { return PreviewScene; }

	/** 뷰포트 클라이언트 반환 */
	TSharedPtr<FFleshRingEditorViewportClient> GetViewportClient() const { return ViewportClient; }

	// ICommonEditorViewportToolbarInfoProvider interface
	virtual TSharedRef<SEditorViewport> GetViewportWidget() override;
	virtual TSharedPtr<FExtender> GetExtenders() const override;
	virtual void OnFloatingButtonClicked() override;

protected:
	// SEditorViewport interface
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;
	virtual void OnFocusViewportToSelection() override;

private:
	/** 프리뷰 씬 */
	TSharedPtr<FFleshRingPreviewScene> PreviewScene;

	/** 뷰포트 클라이언트 */
	TSharedPtr<FFleshRingEditorViewportClient> ViewportClient;

	/** 편집 중인 Asset */
	TWeakObjectPtr<UFleshRingAsset> EditingAsset;
};
