// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorViewportClient.h"

class FFleshRingPreviewScene;
class SFleshRingEditorViewport;
class UFleshRingAsset;

/**
 * FleshRing 에디터 뷰포트 클라이언트
 * 렌더링, 카메라 컨트롤, 입력 처리 담당
 */
class FFleshRingEditorViewportClient : public FEditorViewportClient
{
public:
	FFleshRingEditorViewportClient(
		FFleshRingPreviewScene* InPreviewScene,
		const TWeakPtr<SFleshRingEditorViewport>& InViewportWidget);

	virtual ~FFleshRingEditorViewportClient();

	// FEditorViewportClient interface
	virtual void Tick(float DeltaSeconds) override;
	virtual void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override;
	virtual void DrawCanvas(FViewport& InViewport, FSceneView& View, FCanvas& Canvas) override;
	virtual bool InputKey(const FInputKeyEventArgs& EventArgs) override;
	virtual void ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY) override;

	/** 편집 중인 Asset 설정 */
	void SetAsset(UFleshRingAsset* InAsset);

	/** 프리뷰 씬 반환 */
	FFleshRingPreviewScene* GetPreviewScene() const { return PreviewScene; }

	/** 카메라를 메시에 맞게 포커스 */
	void FocusOnMesh();

private:
	/** Ring 기즈모 그리기 */
	void DrawRingGizmos(FPrimitiveDrawInterface* PDI);

	/** 프리뷰 씬 */
	FFleshRingPreviewScene* PreviewScene;

	/** 뷰포트 위젯 참조 */
	TWeakPtr<SFleshRingEditorViewport> ViewportWidget;

	/** 편집 중인 Asset */
	TWeakObjectPtr<UFleshRingAsset> EditingAsset;
};
