// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/MeshDeformerInstance.h"
#include "RenderGraphResources.h"
#if WITH_EDITORONLY_DATA
#include "Animation/MeshDeformerGeometryReadback.h"
#endif
#include "FleshRingDeformerInstance.generated.h"

class UFleshRingDeformer;
class UMeshComponent;
class FMeshDeformerGeometry;

UCLASS()
class FLESHRINGRUNTIME_API UFleshRingDeformerInstance : public UMeshDeformerInstance
{
	GENERATED_BODY()

public:
	UFleshRingDeformerInstance();

	void SetupFromDeformer(UFleshRingDeformer* InDeformer, UMeshComponent* InMeshComponent);

	// UMeshDeformerInstance interface
	virtual void AllocateResources() override;
	virtual void ReleaseResources() override;
	virtual void EnqueueWork(FEnqueueWorkDesc const& InDesc) override;
	virtual EMeshDeformerOutputBuffer GetOutputBuffers() const override;
	virtual UMeshDeformerInstance* GetInstanceForSourceDeformer() override { return this; }

#if WITH_EDITORONLY_DATA
	virtual bool RequestReadbackDeformerGeometry(TUniquePtr<FMeshDeformerGeometryReadbackRequest> InRequest) override { return false; }
#endif

private:
	UPROPERTY()
	TWeakObjectPtr<UFleshRingDeformer> Deformer;

	UPROPERTY()
	TWeakObjectPtr<UMeshComponent> MeshComponent;

	FSceneInterface* Scene = nullptr;

	// Deformed geometry output buffers
	TSharedPtr<FMeshDeformerGeometry> DeformerGeometry;

	// Track last LOD index for invalidating previous position on LOD change
	int32 LastLodIndex = INDEX_NONE;

	// Velocity tracking for inertia effect
	FVector PreviousWorldLocation = FVector::ZeroVector;
	FVector CurrentVelocity = FVector::ZeroVector;
	bool bHasPreviousLocation = false;
};
