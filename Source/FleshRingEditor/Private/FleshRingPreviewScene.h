// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AdvancedPreviewScene.h"
#include "FleshRingTypes.h"
#include "Animation/DebugSkelMeshComponent.h"

class USkeletalMesh;
class UStaticMeshComponent;
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

	/** 선택된 Ring 인덱스 설정 */
	void SetSelectedRingIndex(int32 Index);

	/** 선택된 Ring 인덱스 반환 */
	int32 GetSelectedRingIndex() const { return SelectedRingIndex; }

	/** 스켈레탈 메시 컴포넌트 반환 (DebugSkelMesh) */
	UDebugSkelMeshComponent* GetSkeletalMeshComponent() const { return SkeletalMeshComponent; }

	/** FleshRing 컴포넌트 반환 */
	UFleshRingComponent* GetFleshRingComponent() const { return FleshRingComponent; }

	/** Ring 메시 컴포넌트 배열 반환 */
	const TArray<UStaticMeshComponent*>& GetRingMeshComponents() const { return RingMeshComponents; }

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
	TArray<UStaticMeshComponent*> RingMeshComponents;

	/** 현재 편집 중인 Asset */
	UFleshRingAsset* CurrentAsset = nullptr;

	/** 현재 선택된 Ring 인덱스 (-1 = 선택 없음) */
	int32 SelectedRingIndex = -1;
};
