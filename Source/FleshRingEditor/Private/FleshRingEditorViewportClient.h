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
	/** 모든 활성 인스턴스 추적 (타입 확인용) */
	static TSet<FFleshRingEditorViewportClient*>& GetAllInstances()
	{
		static TSet<FFleshRingEditorViewportClient*> Instances;
		return Instances;
	}

	/** 특정 ViewportClient가 FleshRing 타입인지 확인 */
	static bool IsFleshRingViewportClient(FEditorViewportClient* Client)
	{
		return Client && GetAllInstances().Contains(static_cast<FFleshRingEditorViewportClient*>(Client));
	}

	FFleshRingEditorViewportClient(
		FEditorModeTools* InModeTools,
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
	virtual ECoordSystem GetWidgetCoordSystemSpace() const override;
	virtual void SetWidgetCoordSystemSpace(ECoordSystem NewCoordSystem) override;
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

	/** 현재 선택된 Ring의 AlignRotation 반환 (기즈모 좌표계용) */
	FMatrix GetSelectedRingAlignMatrix() const;

	/** 선택 해제 */
	void ClearSelection();

	// Show 플래그 토글
	void ToggleShowRingGizmos() { bShowRingGizmos = !bShowRingGizmos; Invalidate(); }
	void ToggleShowRingMeshes();
	void ToggleShowBones() { bShowBones = !bShowBones; Invalidate(); }

	// Show 플래그 상태
	bool ShouldShowRingGizmos() const { return bShowRingGizmos; }
	bool ShouldShowRingMeshes() const { return bShowRingMeshes; }
	bool ShouldShowBones() const { return bShowBones; }

	// 디버그 시각화 토글
	void ToggleShowDebugVisualization();
	void ToggleShowSdfVolume();
	void ToggleShowAffectedVertices();
	void ToggleShowSDFSlice();
	void ToggleShowBulgeHeatmap();

	// 디버그 시각화 상태
	bool ShouldShowDebugVisualization() const;
	bool ShouldShowSdfVolume() const;
	bool ShouldShowAffectedVertices() const;
	bool ShouldShowSDFSlice() const;
	bool ShouldShowBulgeHeatmap() const;

	// Debug Slice Z
	int32 GetDebugSliceZ() const;
	void SetDebugSliceZ(int32 NewValue);

	/** 설정 저장 (카메라, 쇼플래그) */
	void SaveSettings();

	/** 설정 로드 (카메라, 쇼플래그) */
	void LoadSettings();

	// Local/World 좌표계 (커스텀 관리 - 툴바 버튼과 연동)
	void ToggleLocalCoordSystem();
	void SetLocalCoordSystem(bool bLocal);
	bool IsUsingLocalCoordSystem() const;

private:
	/** 에셋별 Config 섹션 이름 생성 */
	FString GetConfigSectionName() const;

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

	/** 드래그 시작 시 초기 회전 (기즈모 고정용) */
	FQuat DragStartWorldRotation = FQuat::Identity;

	/** 드래그 중 누적 델타 회전 (짐벌락 방지용) */
	FQuat AccumulatedDeltaRotation = FQuat::Identity;

	bool bIsDraggingRotation = false;

	// Show 플래그
	bool bShowRingGizmos = true;
	bool bShowRingMeshes = true;
	bool bShowBones = true;

	// Local/World 좌표계 플래그 (커스텀 관리)
	bool bUseLocalCoordSystem = true;

	// 설정 로드 완료 플래그 (첫 Tick에서 로드)
	bool bSettingsLoaded = false;
};
