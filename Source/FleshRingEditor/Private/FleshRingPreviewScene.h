// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AdvancedPreviewScene.h"

struct FFleshRingSettings;
class USkeletalMesh;
class USkeletalMeshComponent;
class UStaticMeshComponent;

/**
 * FleshRing 에디터용 프리뷰 씬
 * 스켈레탈 메시와 Ring 메시들을 표시
 */
class FFleshRingPreviewScene : public FAdvancedPreviewScene
{
public:
	FFleshRingPreviewScene(const ConstructionValues& CVS);
	virtual ~FFleshRingPreviewScene();

	/** 스켈레탈 메시 설정 */
	void SetSkeletalMesh(USkeletalMesh* InMesh);

	/** Ring 메시들 갱신 */
	void RefreshRings(const TArray<FFleshRingSettings>& Rings);

	/** 특정 Ring의 Transform 업데이트 */
	void UpdateRingTransform(int32 Index, const FTransform& Transform);

	/** 선택된 Ring 인덱스 설정 */
	void SetSelectedRingIndex(int32 Index);

	/** 선택된 Ring 인덱스 반환 */
	int32 GetSelectedRingIndex() const { return SelectedRingIndex; }

	/** 스켈레탈 메시 컴포넌트 반환 */
	USkeletalMeshComponent* GetSkeletalMeshComponent() const { return SkeletalMeshComponent; }

	/** Ring 메시 컴포넌트 배열 반환 */
	const TArray<UStaticMeshComponent*>& GetRingMeshComponents() const { return RingMeshComponents; }

private:
	/** 타겟 스켈레탈 메시 컴포넌트 */
	USkeletalMeshComponent* SkeletalMeshComponent = nullptr;

	/** Ring 메시 컴포넌트 배열 */
	TArray<UStaticMeshComponent*> RingMeshComponents;

	/** 현재 선택된 Ring 인덱스 (-1 = 선택 없음) */
	int32 SelectedRingIndex = -1;
};
