// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AdvancedPreviewScene.h"
#include "FleshRingTypes.h"
#include "Animation/DebugSkelMeshComponent.h"

class USkeletalMesh;
class UStaticMeshComponent;
class UFleshRingMeshComponent;
class UFleshRingComponent;
class UFleshRingAsset;
class AActor;

/**
 * FleshRing 에디터용 프리뷰 씬
 * 스켈레탈 메시와 FleshRingComponent를 사용하여 실제 변형을 표시
 */
class FFleshRingPreviewScene : public FAdvancedPreviewScene
{
public:
	FFleshRingPreviewScene(const ConstructionValues& CVS);
	virtual ~FFleshRingPreviewScene();

	/** FleshRing Asset 설정 (메시 + 컴포넌트 갱신) */
	void SetFleshRingAsset(UFleshRingAsset* InAsset);

	/** 스켈레탈 메시 설정 */
	void SetSkeletalMesh(USkeletalMesh* InMesh);

	/** Ring 메시들 갱신 */
	void RefreshRings(const TArray<FFleshRingSettings>& Rings);

	/** 프리뷰 갱신 (Asset 변경 시 호출) */
	void RefreshPreview();

	/** 특정 Ring의 Transform 업데이트 */
	void UpdateRingTransform(int32 Index, const FTransform& Transform);

	/** 모든 Ring Transform을 Asset 기준으로 업데이트 (경량 업데이트) */
	void UpdateAllRingTransforms();

	/** 선택된 Ring 인덱스 설정 */
	void SetSelectedRingIndex(int32 Index);

	/** 선택된 Ring 인덱스 반환 */
	int32 GetSelectedRingIndex() const { return SelectedRingIndex; }

	/** 스켈레탈 메시 컴포넌트 반환 (DebugSkelMesh) */
	UDebugSkelMeshComponent* GetSkeletalMeshComponent() const { return SkeletalMeshComponent; }

	/** FleshRing 컴포넌트 반환 */
	UFleshRingComponent* GetFleshRingComponent() const { return FleshRingComponent; }

	/** Ring 메시 컴포넌트 배열 반환 */
	const TArray<UFleshRingMeshComponent*>& GetRingMeshComponents() const { return RingMeshComponents; }

	/** Ring 메시 가시성 설정 */
	void SetRingMeshesVisible(bool bVisible);

	/** Deformer 초기화 대기 상태 확인 (메시가 렌더링되었는지 체크) */
	bool IsPendingDeformerInit() const;

	/** 대기 중인 Deformer 초기화 실행 */
	void ExecutePendingDeformerInit();

	// =====================================
	// Preview Mesh Management (에셋에서 분리하여 트랜잭션 제외)
	// =====================================

	/** 프리뷰 메시 생성 */
	void GeneratePreviewMesh();

	/** 프리뷰 메시 정리 */
	void ClearPreviewMesh();

	/** 프리뷰 메시 캐시 무효화 */
	void InvalidatePreviewMeshCache();

	/** 프리뷰 메시 유효 여부 */
	bool HasValidPreviewMesh() const { return PreviewSubdividedMesh != nullptr; }

	/** 프리뷰 메시 재생성 필요 여부 */
	bool NeedsPreviewMeshRegeneration() const;

	/** 프리뷰 메시 캐시가 유효한지 확인 (해시 비교) */
	bool IsPreviewMeshCacheValid() const;

	/** 현재 본 구성의 해시 계산 */
	uint32 CalculatePreviewBoneConfigHash() const;

	/** 프리뷰 메시 반환 */
	USkeletalMesh* GetPreviewSubdividedMesh() const { return PreviewSubdividedMesh; }

private:
	/** 프리뷰 액터 생성 */
	void CreatePreviewActor();

	/** 프리뷰 액터 */
	AActor* PreviewActor = nullptr;

	/** 타겟 스켈레탈 메시 컴포넌트 (DebugSkelMesh로 본 색상 고정) */
	UDebugSkelMeshComponent* SkeletalMeshComponent = nullptr;

	/** FleshRing 컴포넌트 (실제 변형 처리) */
	UFleshRingComponent* FleshRingComponent = nullptr;

	/** Ring 메시 컴포넌트 배열 (시각화용) */
	TArray<UFleshRingMeshComponent*> RingMeshComponents;

	/** 현재 편집 중인 Asset */
	UFleshRingAsset* CurrentAsset = nullptr;

	/** PreviewSubdividedMesh 적용 전 원본 메시 (복원용) */
	TWeakObjectPtr<USkeletalMesh> CachedOriginalMesh;

	// =====================================
	// Preview Subdivided Mesh (에셋에서 분리하여 트랜잭션 제외)
	// =====================================

	/** 프리뷰용 Subdivided 메시 (에디터 전용, 트랜잭션에서 제외) */
	TObjectPtr<USkeletalMesh> PreviewSubdividedMesh;

	/** 프리뷰 메시 캐시 유효성 해시 */
	uint32 LastPreviewBoneConfigHash = 0;

	/** 프리뷰 메시 캐시 유효 플래그 */
	bool bPreviewMeshCacheValid = false;

	/** 현재 선택된 Ring 인덱스 (-1 = 선택 없음) */
	int32 SelectedRingIndex = -1;

	/** Ring 메시 가시성 상태 (Show Flag) */
	bool bRingMeshesVisible = true;

	/** Asset 변경 델리게이트 핸들 (전체 리프레시 - Subdivision 생성/제거 시 필요) */
	FDelegateHandle AssetChangedDelegateHandle;

	/** Asset 변경 시 콜백 (전체 리프레시) */
	void OnAssetChanged(UFleshRingAsset* ChangedAsset);

	/** 델리게이트 바인딩/해제 */
	void BindToAssetDelegate();
	void UnbindFromAssetDelegate();

	/** Deformer 초기화 대기 플래그 */
	bool bPendingDeformerInit = false;
};
