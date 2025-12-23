// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingDeformer.h"
#include "FleshRingDeformerInstance.h"
#include "Components/MeshComponent.h"

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
	UFleshRingDeformerInstance* Instance = NewObject<UFleshRingDeformerInstance>(InMeshComponent);
	Instance->SetupFromDeformer(this, InMeshComponent);
	return Instance;
}
