// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Animation/MeshDeformerInstance.h"
#include "RenderGraphResources.h"
#include "FleshRingAffectedVertices.h"
#if WITH_EDITORONLY_DATA
#include "Animation/MeshDeformerGeometryReadback.h"
#endif
#include "FleshRingDeformerInstance.generated.h"

class UFleshRingDeformer;
class UMeshComponent;
class FMeshDeformerGeometry;
class UFleshRingComponent;

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

	UPROPERTY()
	TWeakObjectPtr<UFleshRingComponent> FleshRingComponent;

	FSceneInterface* Scene = nullptr;

	// Deformed geometry output buffers
	TSharedPtr<FMeshDeformerGeometry> DeformerGeometry;

	// Track last LOD index for invalidating previous position on LOD change
	int32 LastLodIndex = INDEX_NONE;

	// Velocity tracking for inertia effect (legacy - for WaveCS)
	FVector PreviousWorldLocation = FVector::ZeroVector;
	FVector CurrentVelocity = FVector::ZeroVector;
	bool bHasPreviousLocation = false;

	// ===== LOD별 Tightness Deformation 데이터 =====
	// Per-LOD Tightness Deformation Data
	struct FLODDeformationData
	{
		// 영향받는 버텍스 관리자
		FFleshRingAffectedVerticesManager AffectedVerticesManager;

		// 버텍스 등록 완료 여부
		bool bAffectedVerticesRegistered = false;

		// 캐시된 버텍스 데이터 (RDG 업로드용)
		TArray<float> CachedSourcePositions;
		bool bSourcePositionsCached = false;

		// TightenedBindPose 캐싱
		// Using TSharedPtr wrapper for thread-safe sharing with render thread
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedTightenedBindPoseShared;
		bool bTightenedBindPoseCached = false;
		uint32 CachedTightnessVertexCount = 0;
	};

	// LOD별 데이터 배열 (인덱스 = LOD 번호)
	TArray<FLODDeformationData> LODData;

	// LOD 개수 (초기화 시 설정)
	int32 NumLODs = 0;
};
