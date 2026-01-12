// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorViewportClient.h"
#include "ScopedTransaction.h"
#include "FleshRingTypes.h"
#include "AssetViewerSettings.h"

class FFleshRingPreviewScene;
class SFleshRingEditorViewport;
class UFleshRingAsset;

/** 본 그리기 모드 (Persona 스타일) */
namespace EFleshRingBoneDrawMode
{
	enum Type
	{
		None,						// 본 표시 안 함
		Selected,					// 선택된 본만
		SelectedAndParents,			// 선택된 본 + 부모 본들
		SelectedAndChildren,		// 선택된 본 + 자식 본들
		SelectedAndParentsAndChildren,	// 선택된 본 + 부모 + 자식 본들
		All							// 모든 본 (모든 계층 구조)
	};
}

/** 본 선택 해제 델리게이트 */
DECLARE_DELEGATE(FOnBoneSelectionCleared);

/** 뷰포트에서 Ring 선택 델리게이트 (RingIndex, SelectionType) */
DECLARE_DELEGATE_TwoParams(FOnRingSelectedInViewport, int32 /*RingIndex*/, EFleshRingSelectionType /*SelectionType*/);

/** 뷰포트에서 Ring 삭제 델리게이트 */
DECLARE_DELEGATE(FOnRingDeletedInViewport);

/** 뷰포트에서 본 선택 델리게이트 */
DECLARE_DELEGATE_OneParam(FOnBoneSelectedInViewport, FName /*BoneName*/);

/** 뷰포트에서 Ring 추가 요청 델리게이트 (우클릭 메뉴에서 호출) */
DECLARE_DELEGATE_FourParams(FOnAddRingAtPositionRequested, FName /*BoneName*/, const FVector& /*LocalOffset*/, const FRotator& /*LocalRotation*/, UStaticMesh* /*SelectedMesh*/);

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
	virtual bool InputAxis(const FInputKeyEventArgs& EventArgs) override;
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

	/** 편집 중인 Asset 반환 */
	UFleshRingAsset* GetEditingAsset() const { return EditingAsset.Get(); }

	/** 카메라를 메시에 맞게 포커스 */
	void FocusOnMesh();

	/** 현재 선택 타입 반환 */
	EFleshRingSelectionType GetSelectionType() const { return SelectionType; }

	/** 선택 타입 설정 (Undo 후 복원용) */
	void SetSelectionType(EFleshRingSelectionType InType) { SelectionType = InType; }

	/** 선택 검증 스킵 설정 (Undo/Redo 중 사용) */
	void SetSkipSelectionValidation(bool bSkip) { bSkipSelectionValidation = bSkip; }

	/** 현재 선택된 Ring의 AlignRotation 반환 (기즈모 좌표계용) */
	FMatrix GetSelectedRingAlignMatrix() const;

	/** 선택 해제 */
	void ClearSelection();

	/** Ring 선택 (트리에서 호출, 부착된 본도 하이라이트) */
	void SelectRing(int32 RingIndex, FName AttachedBoneName = NAME_None);

	// Show 플래그 토글
	void ToggleShowSkeletalMesh();
	void ToggleShowRingGizmos() { bShowRingGizmos = !bShowRingGizmos; Invalidate(); }
	void ToggleShowRingMeshes();
	void ToggleShowBones() { bShowBones = !bShowBones; Invalidate(); }
	void ToggleShowGrid();

	// Show 플래그 상태
	bool ShouldShowSkeletalMesh() const { return bShowSkeletalMesh; }
	bool ShouldShowRingGizmos() const { return bShowRingGizmos; }
	bool ShouldShowRingMeshes() const { return bShowRingMeshes; }
	bool ShouldShowBones() const { return bShowBones; }
	bool ShouldShowGrid() const;

	/** 현재 Show Flag를 프리뷰 씬에 적용 (리프레시 후 호출) */
	void ApplyShowFlagsToScene();

	// 본 그리기 옵션 토글/설정
	void ToggleShowBoneNames() { bShowBoneNames = !bShowBoneNames; Invalidate(); }
	void ToggleShowMultiColorBones() { bShowMultiColorBones = !bShowMultiColorBones; Invalidate(); }
	void SetBoneDrawSize(float InSize) { BoneDrawSize = FMath::Clamp(InSize, 0.1f, 5.0f); Invalidate(); }
	void SetBoneDrawMode(EFleshRingBoneDrawMode::Type InMode);

	/** 그릴 본 비트 배열 업데이트 (Persona 스타일) */
	void UpdateBonesToDraw();

	// 본 그리기 옵션 상태
	bool ShouldShowBoneNames() const { return bShowBoneNames; }
	bool ShouldShowMultiColorBones() const { return bShowMultiColorBones; }
	float GetBoneDrawSize() const { return BoneDrawSize; }
	EFleshRingBoneDrawMode::Type GetBoneDrawMode() const { return BoneDrawMode; }
	bool IsBoneDrawModeSet(EFleshRingBoneDrawMode::Type InMode) const { return BoneDrawMode == InMode; }

	// 디버그 시각화 토글
	void ToggleShowDebugVisualization();
	void ToggleShowSdfVolume();
	void ToggleShowAffectedVertices();
	void ToggleShowSDFSlice();
	void ToggleShowBulgeHeatmap();
	void ToggleShowBulgeArrows();

	// 디버그 시각화 상태
	bool ShouldShowDebugVisualization() const;
	bool ShouldShowSdfVolume() const;
	bool ShouldShowAffectedVertices() const;
	bool ShouldShowSDFSlice() const;
	bool ShouldShowBulgeHeatmap() const;
	bool ShouldShowBulgeArrows() const;

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

	// 본 선택 (스켈레톤 트리 연동)
	void SetSelectedBone(FName BoneName);
	void ClearSelectedBone();
	FName GetSelectedBoneName() const { return SelectedBoneName; }

	/** 본 선택 해제 델리게이트 설정 */
	void SetOnBoneSelectionCleared(FOnBoneSelectionCleared InDelegate) { OnBoneSelectionCleared = InDelegate; }

	/** Ring 선택 델리게이트 설정 (뷰포트에서 Ring 피킹 시 호출) */
	void SetOnRingSelectedInViewport(FOnRingSelectedInViewport InDelegate) { OnRingSelectedInViewport = InDelegate; }

	/** Ring 삭제 델리게이트 설정 (뷰포트에서 Ring 삭제 시 호출) */
	void SetOnRingDeletedInViewport(FOnRingDeletedInViewport InDelegate) { OnRingDeletedInViewport = InDelegate; }

	/** 본 선택 델리게이트 설정 (뷰포트에서 본 피킹 시 호출) */
	void SetOnBoneSelectedInViewport(FOnBoneSelectedInViewport InDelegate) { OnBoneSelectedInViewport = InDelegate; }

	/** Ring 추가 요청 델리게이트 설정 (우클릭 메뉴에서 Ring 추가 시 호출) */
	void SetOnAddRingAtPositionRequested(FOnAddRingAtPositionRequested InDelegate) { OnAddRingAtPositionRequested = InDelegate; }

	/** 본 우클릭 컨텍스트 메뉴 표시 */
	void ShowBoneContextMenu(FName BoneName, const FVector2D& ScreenPos);

	/** 컨텍스트 메뉴에서 Ring 추가 선택 시 콜백 */
	void OnContextMenu_AddRing();

	/** 스크린 좌표에서 본 로컬 오프셋 계산 (본 라인에 투영) */
	FVector CalculateBoneLocalOffsetFromScreenPos(const FVector2D& ScreenPos, FName BoneName, FRotator* OutLocalRotation = nullptr);

	/** 본에 대한 기본 Ring 회전 계산 (가중치 자식 기반) */
	FRotator CalculateDefaultRingRotationForBone(FName BoneName);

	/** 선택된 Ring 삭제 */
	void DeleteSelectedRing();

	/** 선택된 Ring 삭제 가능 여부 */
	bool CanDeleteSelectedRing() const;

private:
	/** Invalidate() + Viewport->Draw() 헬퍼 (드롭박스 열린 상태에서도 즉시 렌더링) */
	void InvalidateAndDraw();

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

	/** Undo/Redo 중 선택 검증 스킵 플래그 */
	bool bSkipSelectionValidation = false;

	// Show 플래그
	bool bShowSkeletalMesh = true;
	bool bShowRingGizmos = true;
	bool bShowRingMeshes = true;
	bool bShowBones = true;

	// 본 그리기 옵션
	bool bShowBoneNames = false;
	bool bShowMultiColorBones = false;
	float BoneDrawSize = 1.0f;
	EFleshRingBoneDrawMode::Type BoneDrawMode = EFleshRingBoneDrawMode::All;

	/** 그릴 본 비트 배열 (Persona 스타일) */
	TBitArray<> BonesToDraw;

	/** 캐시된 본 HitProxy 배열 (매 프레임 재생성 방지) */
	TArray<TRefCountPtr<HHitProxy>> CachedBoneHitProxies;

	/** 마지막 캐싱 시 스켈레탈 메시 (변경 감지용) */
	TWeakObjectPtr<USkeletalMesh> CachedSkeletalMesh;

	/** 가중치가 있는 본 인덱스 캐시 */
	TSet<int32> WeightedBoneIndices;

	/** 가중치 본 캐시 빌드 */
	void BuildWeightedBoneCache();

	/** 본이 가중치를 가지고 있는지 확인 */
	bool IsBoneWeighted(int32 BoneIndex) const;

	/** 가중치가 있는 자식 본 찾기 (없으면 INDEX_NONE 반환) */
	int32 FindWeightedChildBone(int32 ParentBoneIndex) const;

	/** 가중치가 있는 직속 자식 본 개수 반환 */
	int32 CountWeightedChildBones(int32 ParentBoneIndex) const;

	// 디버그 시각화 옵션 (캐싱 - 컴포넌트 생명주기와 독립적으로 저장/로드)
	bool bCachedShowDebugVisualization = false;
	bool bCachedShowSdfVolume = false;
	bool bCachedShowAffectedVertices = false;
	bool bCachedShowSDFSlice = false;
	bool bCachedShowBulgeHeatmap = false;
	bool bCachedShowBulgeArrows = true;  // 기본값 켜짐
	int32 CachedDebugSliceZ = 32;

	// Local/World 좌표계 플래그 (커스텀 관리)
	bool bUseLocalCoordSystem = true;

	// 설정 로드 완료 플래그 (첫 Tick에서 로드)
	bool bSettingsLoaded = false;

	// 선택된 본 이름 (스켈레톤 트리 연동)
	FName SelectedBoneName = NAME_None;

	// 본 선택 해제 델리게이트
	FOnBoneSelectionCleared OnBoneSelectionCleared;

	// Ring 선택 델리게이트 (뷰포트에서 피킹 시)
	FOnRingSelectedInViewport OnRingSelectedInViewport;

	// Ring 삭제 델리게이트 (뷰포트에서 삭제 시)
	FOnRingDeletedInViewport OnRingDeletedInViewport;

	// 본 선택 델리게이트 (뷰포트에서 피킹 시)
	FOnBoneSelectedInViewport OnBoneSelectedInViewport;

	// Ring 추가 요청 델리게이트 (우클릭 메뉴에서 호출)
	FOnAddRingAtPositionRequested OnAddRingAtPositionRequested;

	// 우클릭 Ring 추가용 임시 저장
	FName PendingRingAddBoneName = NAME_None;
	FVector2D PendingRingAddScreenPos = FVector2D::ZeroVector;

	// Preview Scene Settings 변경 델리게이트 핸들
	FDelegateHandle AssetViewerSettingsChangedHandle;

	/** Preview Scene Settings 변경 시 콜백 */
	void OnAssetViewerSettingsChanged(const FName& PropertyName);

	/** Preview Scene Settings를 EngineShowFlags에 적용 */
	void ApplyPreviewSceneShowFlags();
};
