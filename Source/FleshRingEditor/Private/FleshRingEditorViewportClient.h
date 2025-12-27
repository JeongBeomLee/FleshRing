// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorViewportClient.h"
#include "ScopedTransaction.h"

class FFleshRingPreviewScene;
class SFleshRingEditorViewport;
class UFleshRingAsset;

/** Ring 선택 타입 */
enum class EFleshRingSelectionType : uint8
{
	None,		// 선택 없음
	Gizmo,		// Ring 기즈모 선택 (이동 + Scale로 반경 조절)
	Mesh		// Ring 메시 선택 (메시 이동/회전)
};

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

	// Widget 관련 오버라이드
	virtual FVector GetWidgetLocation() const override;
	virtual FMatrix GetWidgetCoordSystem() const override;
	virtual UE::Widget::EWidgetMode GetWidgetMode() const override;
	virtual bool InputWidgetDelta(FViewport* InViewport, EAxisList::Type CurrentAxis, FVector& Drag, FRotator& Rot, FVector& Scale) override;
	virtual void TrackingStarted(const FInputEventState& InInputState, bool bIsDraggingWidget, bool bNudge) override;
	virtual void TrackingStopped() override;

	/** 편집 중인 Asset 설정 */
	void SetAsset(UFleshRingAsset* InAsset);

	/** 프리뷰 씬 반환 */
	FFleshRingPreviewScene* GetPreviewScene() const { return PreviewScene; }

	/** 카메라를 메시에 맞게 포커스 */
	void FocusOnMesh();

	/** 현재 선택 타입 반환 */
	EFleshRingSelectionType GetSelectionType() const { return SelectionType; }

	/** 선택 해제 */
	void ClearSelection();

private:
	/** 본 렌더링 (Persona 스타일) */
	void DrawMeshBones(FPrimitiveDrawInterface* PDI);

	/** Ring 기즈모 그리기 */
	void DrawRingGizmos(FPrimitiveDrawInterface* PDI);

	/** 프리뷰 씬 */
	FFleshRingPreviewScene* PreviewScene;

	/** 뷰포트 위젯 참조 */
	TWeakPtr<SFleshRingEditorViewport> ViewportWidget;

	/** 편집 중인 Asset */
	TWeakObjectPtr<UFleshRingAsset> EditingAsset;

	/** 현재 선택 타입 */
	EFleshRingSelectionType SelectionType = EFleshRingSelectionType::None;

	/** Undo용 트랜잭션 */
	TUniquePtr<FScopedTransaction> ScopedTransaction;
};
