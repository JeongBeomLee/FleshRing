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
	// NOTE: BoundsScale은 FleshRingComponent::SetupDeformer()에서 설정함

	// VSM Shadow Cache Invalidation: Deformer가 GPU에서 버텍스를 변형하므로
	// VSM에게 매 프레임 그림자 캐시 무효화하도록 알림
	InMeshComponent->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Always;

	UFleshRingDeformerInstance* Instance = NewObject<UFleshRingDeformerInstance>(InMeshComponent);
	Instance->SetupFromDeformer(this, InMeshComponent);
	return Instance;
}
