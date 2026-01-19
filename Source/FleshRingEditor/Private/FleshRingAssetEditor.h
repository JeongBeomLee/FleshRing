// Copyright Epic Games, Inc. All Rights Reserved.

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
 * FleshRing Asset 전용 에디터
 * Physics Asset Editor처럼 3D 뷰포트와 Details 패널 제공
 */
class FFleshRingAssetEditor : public FAssetEditorToolkit
{
public:
	FFleshRingAssetEditor();
	virtual ~FFleshRingAssetEditor();

	/** 에디터 초기화 */
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

	/** 편집 중인 Asset 반환 */
	UFleshRingAsset* GetEditingAsset() const { return EditingAsset; }

	/** 뷰포트 갱신 (전체 재생성) */
	void RefreshViewport();

	/** Ring 트랜스폼만 업데이트 (깜빡임 방지) */
	void UpdateRingTransformsOnly(int32 DirtyRingIndex = INDEX_NONE);

	/** SDF만 재생성 (VirtualBand 드래그 중 - 컴포넌트 재생성 없이) */
	void RefreshSDFOnly();

	/** PreviewScene의 FleshRingComponent 반환 (Subdivision 데이터 접근용) */
	UFleshRingComponent* GetPreviewFleshRingComponent() const;

	/** 프리뷰 메시 강제 재생성 (캐시 무효화 후 재생성) */
	void ForceRefreshPreviewMesh();

	/** PreviewScene 강제 틱 (모달 다이얼로그에서 뷰포트 렌더링용) */
	void TickPreviewScene(float DeltaTime);

	/** 베이크 오버레이 표시/숨김 (입력 차단 + 진행 표시) */
	void ShowBakeOverlay(bool bShow, const FText& Message = FText::GetEmpty());

	/** 베이크 오버레이 상태 */
	bool IsBakeOverlayVisible() const { return bBakeOverlayVisible; }

private:
	/** Skeleton Tree 탭 생성 */
	TSharedRef<SDockTab> SpawnTab_SkeletonTree(const FSpawnTabArgs& Args);

	/** Viewport 탭 생성 */
	TSharedRef<SDockTab> SpawnTab_Viewport(const FSpawnTabArgs& Args);

	/** Details 탭 생성 */
	TSharedRef<SDockTab> SpawnTab_Details(const FSpawnTabArgs& Args);

	/** Preview Settings 탭 생성 */
	TSharedRef<SDockTab> SpawnTab_PreviewSettings(const FSpawnTabArgs& Args);

	/** Details View 생성 */
	void CreateDetailsView();

	/** 본 선택 콜백 (Skeleton Tree에서) */
	void OnBoneSelected(FName BoneName);

	/** Ring 선택 콜백 (Skeleton Tree에서) */
	void OnRingSelected(int32 RingIndex);

	/** Ring 선택 콜백 (뷰포트에서 피킹) */
	void OnRingSelectedInViewport(int32 RingIndex, EFleshRingSelectionType SelectionType);

	/** 본 선택 해제 콜백 (뷰포트에서) */
	void OnBoneSelectionCleared();

	/** 본 선택 콜백 (뷰포트에서 피킹) */
	void OnBoneSelectedInViewport(FName BoneName);

	/** Ring 추가 요청 콜백 (스켈레톤 트리에서 - 메쉬 선택 포함) */
	void OnAddRingRequested(FName BoneName, UStaticMesh* SelectedMesh);

	/** Ring 추가 요청 콜백 (뷰포트 우클릭에서 - 위치 및 메쉬 포함) */
	void OnAddRingAtPositionRequested(FName BoneName, const FVector& LocalOffset, const FRotator& LocalRotation, UStaticMesh* SelectedMesh);

	/** 카메라 포커스 요청 콜백 */
	void OnFocusCameraRequested();

	/** Ring 삭제 공통 처리 (뷰포트/트리/디테일 모두에서 호출) */
	void HandleRingDeleted();

	/** 프로퍼티 변경 콜백 */
	void OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent);

	/** Undo/Redo 콜백 */
	void OnObjectTransacted(UObject* Object, const FTransactionObjectEvent& TransactionEvent);

	/** 에셋의 선택 상태를 뷰포트에 반영 (Undo/Redo 시 호출) */
	void ApplySelectionFromAsset();

	/** Ring 선택 변경 콜백 (디테일 패널에서) */
	void OnRingSelectionChangedFromDetails(int32 RingIndex);

private:
	/** 편집 중인 Asset */
	UFleshRingAsset* EditingAsset = nullptr;

	/** Skeleton Tree 위젯 */
	TSharedPtr<SFleshRingSkeletonTree> SkeletonTreeWidget;

	/** 뷰포트 위젯 */
	TSharedPtr<SFleshRingEditorViewport> ViewportWidget;

	/** Details View */
	TSharedPtr<IDetailsView> DetailsView;

	/** 프로퍼티 변경 델리게이트 핸들 */
	FDelegateHandle OnPropertyChangedHandle;

	/** Undo/Redo 델리게이트 핸들 */
	FDelegateHandle OnObjectTransactedHandle;

	/** 뷰포트에서 Ring 선택 중 플래그 (순환 호출 방지) */
	bool bSyncingFromViewport = false;

	/** 베이크 오버레이 표시 상태 */
	bool bBakeOverlayVisible = false;

	/** 베이크 오버레이 윈도우 */
	TSharedPtr<SWindow> BakeOverlayWindow;
};
