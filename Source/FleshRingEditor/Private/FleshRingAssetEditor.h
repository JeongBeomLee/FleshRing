// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/UObjectGlobals.h"
#include "FleshRingEditorViewportClient.h"

class UFleshRingAsset;
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
	void UpdateRingTransformsOnly();

private:
	/** Skeleton Tree 탭 생성 */
	TSharedRef<SDockTab> SpawnTab_SkeletonTree(const FSpawnTabArgs& Args);

	/** Viewport 탭 생성 */
	TSharedRef<SDockTab> SpawnTab_Viewport(const FSpawnTabArgs& Args);

	/** Details 탭 생성 */
	TSharedRef<SDockTab> SpawnTab_Details(const FSpawnTabArgs& Args);

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

	/** Ring 추가 요청 콜백 */
	void OnAddRingRequested(FName BoneName);

	/** 카메라 포커스 요청 콜백 */
	void OnFocusCameraRequested();

	/** 프로퍼티 변경 콜백 */
	void OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent);

	/** Undo/Redo 콜백 */
	void OnObjectTransacted(UObject* Object, const FTransactionObjectEvent& TransactionEvent);

	/** 에셋의 선택 상태를 뷰포트에 반영 (Undo/Redo 시 호출) */
	void ApplySelectionFromAsset();

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
};
