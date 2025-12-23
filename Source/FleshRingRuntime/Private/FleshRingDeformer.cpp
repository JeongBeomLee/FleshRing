// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingDeformer.h"
#include "FleshRingDeformerInstance.h"
#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(FleshRingDeformer)

UFleshRingDeformer::UFleshRingDeformer()
{
}

UMeshDeformerInstanceSettings* UFleshRingDeformer::CreateSettingsInstance(UMeshComponent* InMeshComponent)
{
	// No special settings needed for this simple test
	return nullptr;
}

UMeshDeformerInstance* UFleshRingDeformer::CreateInstance(UMeshComponent* InMeshComponent, UMeshDeformerInstanceSettings* InSettings)
{
	// Bounds 확장: Deformer 변형이 원래 bounds를 벗어날 수 있으므로
	// VSM(Virtual Shadow Maps) 등 bounds 기반 캐싱 시스템이 정상 작동하도록 확장
	InMeshComponent->SetBoundsScale(BoundsScale);

	// VSM Shadow Cache Invalidation: Deformer가 GPU에서 버텍스를 변형하므로
	// VSM에게 매 프레임 그림자 캐시 무효화하도록 알림
	InMeshComponent->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Always;

	UFleshRingDeformerInstance* Instance = NewObject<UFleshRingDeformerInstance>(InMeshComponent);
	Instance->SetupFromDeformer(this, InMeshComponent);
	return Instance;
}
