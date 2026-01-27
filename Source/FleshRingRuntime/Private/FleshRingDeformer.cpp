// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingDeformer.h"
#include "FleshRingDeformerInstance.h"
#include "FleshRingComponent.h"
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
	// NOTE: BoundsScale is set in FleshRingComponent::SetupDeformer()

	// VSM Shadow Cache Invalidation: Since Deformer transforms vertices on GPU,
	// notify VSM to invalidate shadow cache every frame
	InMeshComponent->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Always;

	UFleshRingDeformerInstance* Instance = NewObject<UFleshRingDeformerInstance>(InMeshComponent);
	// Explicitly pass owner FleshRingComponent (supports multi-component environments)
	Instance->SetupFromDeformer(this, InMeshComponent, OwnerFleshRingComponent.Get());

	// Cache created Instance (for access from FleshRingComponent)
	ActiveInstance = Instance;

	return Instance;
}

void UFleshRingDeformer::SetOwnerFleshRingComponent(UFleshRingComponent* InComponent)
{
	OwnerFleshRingComponent = InComponent;
}

UFleshRingComponent* UFleshRingDeformer::GetOwnerFleshRingComponent() const
{
	return OwnerFleshRingComponent.Get();
}
