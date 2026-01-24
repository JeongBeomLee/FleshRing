// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "FleshRingMeshComponent.generated.h"

/**
 * Ring 메시용 커스텀 StaticMeshComponent
 * 에디터에서 커스텀 SceneProxy를 통해 본보다 높은 피킹 우선순위 제공
 */
UCLASS()
class FLESHRINGRUNTIME_API UFleshRingMeshComponent : public UStaticMeshComponent
{
	GENERATED_BODY()

public:
	UFleshRingMeshComponent();

	/** Ring 인덱스 설정 (HitProxy에서 사용) */
	void SetRingIndex(int32 InRingIndex) { RingIndex = InRingIndex; }

	/** Ring 인덱스 반환 */
	int32 GetRingIndex() const { return RingIndex; }

	// UPrimitiveComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;

private:
	/** Ring 인덱스 */
	int32 RingIndex = INDEX_NONE;
};
