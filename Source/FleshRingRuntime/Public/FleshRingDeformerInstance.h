// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include <atomic>
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

	// UObject interface
	virtual void BeginDestroy() override;

	/**
	 * Deformer로부터 설정 초기화
	 * @param InDeformer - 소스 Deformer
	 * @param InMeshComponent - 대상 MeshComponent
	 * @param InOwnerFleshRingComponent - 이 Deformer를 소유하는 FleshRingComponent (다중 컴포넌트 환경 지원)
	 *                                    nullptr이면 기존 방식(FindComponentByClass) 사용
	 */
	void SetupFromDeformer(
		UFleshRingDeformer* InDeformer,
		UMeshComponent* InMeshComponent,
		UFleshRingComponent* InOwnerFleshRingComponent = nullptr);

	// UMeshDeformerInstance interface
	virtual void AllocateResources() override;
	virtual void ReleaseResources() override;
	virtual void EnqueueWork(FEnqueueWorkDesc const& InDesc) override;
	virtual EMeshDeformerOutputBuffer GetOutputBuffers() const override;
	virtual UMeshDeformerInstance* GetInstanceForSourceDeformer() override { return this; }

	/**
	 * TightenedBindPose 캐시 무효화 (트랜스폼 변경 시 재계산 트리거)
	 * @param DirtyRingIndex - 특정 Ring만 무효화 (INDEX_NONE이면 전체 무효화)
	 */
	void InvalidateTightnessCache(int32 DirtyRingIndex = INDEX_NONE);

	/**
	 * 메시 변경 시 전체 캐시 무효화 (베이킹용)
	 * 소스 포지션 캐시와 TightenedBindPose 캐시를 모두 무효화하여
	 * 다음 프레임에서 새 메시 기준으로 버퍼를 재생성하도록 함
	 */
	void InvalidateForMeshChange();

	/**
	 * 디버그용: LOD별 AffectedVertices 데이터 반환
	 * @param LODIndex - LOD 인덱스 (0 = 최고 품질)
	 * @return 해당 LOD의 Ring별 Affected 데이터 배열, 없으면 nullptr
	 */
	const TArray<FRingAffectedData>* GetAffectedRingDataForDebug(int32 LODIndex = 0) const
	{
		if (LODData.IsValidIndex(LODIndex) && LODData[LODIndex].bAffectedVerticesRegistered)
		{
			return &LODData[LODIndex].AffectedVerticesManager.GetAllRingData();
		}
		return nullptr;
	}

	/**
	 * GPU Influence Readback 완료 여부 확인
	 * @param LODIndex - LOD 인덱스
	 * @return Readback 완료 시 true
	 */
	bool IsDebugInfluenceReadbackComplete(int32 LODIndex = 0) const
	{
		if (LODData.IsValidIndex(LODIndex) &&
			LODData[LODIndex].bDebugInfluenceReadbackComplete.IsValid())
		{
			return LODData[LODIndex].bDebugInfluenceReadbackComplete->load();
		}
		return false;
	}

	/**
	 * GPU Influence Readback 결과 반환
	 * @param LODIndex - LOD 인덱스
	 * @return Readback된 Influence 배열 포인터, 없으면 nullptr
	 */
	const TArray<float>* GetDebugInfluenceReadbackResult(int32 LODIndex = 0) const
	{
		if (LODData.IsValidIndex(LODIndex) &&
			LODData[LODIndex].DebugInfluenceReadbackResult.IsValid() &&
			IsDebugInfluenceReadbackComplete(LODIndex))
		{
			return LODData[LODIndex].DebugInfluenceReadbackResult.Get();
		}
		return nullptr;
	}

	/**
	 * GPU Influence Readback 완료 플래그 리셋 (다음 Readback 준비용)
	 * @param LODIndex - LOD 인덱스
	 */
	void ResetDebugInfluenceReadback(int32 LODIndex = 0)
	{
		if (LODData.IsValidIndex(LODIndex) &&
			LODData[LODIndex].bDebugInfluenceReadbackComplete.IsValid())
		{
			LODData[LODIndex].bDebugInfluenceReadbackComplete->store(false);
		}
	}

	/**
	 * GPU 디버그 렌더링용 캐시된 포인트 버퍼 가져오기
	 * @param LODIndex - LOD 인덱스
	 * @return 캐시된 DebugPointBuffer, 없으면 빈 포인터
	 */
	TRefCountPtr<FRDGPooledBuffer> GetCachedDebugPointBuffer(int32 LODIndex = 0) const
	{
		if (LODData.IsValidIndex(LODIndex) &&
			LODData[LODIndex].CachedDebugPointBufferShared.IsValid() &&
			LODData[LODIndex].CachedDebugPointBufferShared->IsValid())
		{
			return *LODData[LODIndex].CachedDebugPointBufferShared;
		}
		return nullptr;
	}

	/**
	 * GPU 디버그 렌더링용 캐시된 포인트 버퍼 SharedPtr 가져오기
	 * @param LODIndex - LOD 인덱스
	 * @return CachedDebugPointBufferShared의 SharedPtr, 없으면 nullptr
	 */
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> GetCachedDebugPointBufferSharedPtr(int32 LODIndex = 0) const
	{
		if (LODData.IsValidIndex(LODIndex))
		{
			return LODData[LODIndex].CachedDebugPointBufferShared;
		}
		return nullptr;
	}

	/**
	 * Bulge GPU 디버그 렌더링용 캐시된 포인트 버퍼 SharedPtr 가져오기
	 * @param LODIndex - LOD 인덱스
	 * @return CachedDebugBulgePointBufferShared의 SharedPtr, 없으면 nullptr
	 */
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> GetCachedDebugBulgePointBufferSharedPtr(int32 LODIndex = 0) const
	{
		if (LODData.IsValidIndex(LODIndex))
		{
			return LODData[LODIndex].CachedDebugBulgePointBufferShared;
		}
		return nullptr;
	}

	/**
	 * Affected 디버그 포인트 수 가져오기 (실제 변형에 사용되는 값)
	 * @param LODIndex - LOD 인덱스
	 * @return 총 Affected 버텍스 수
	 */
	uint32 GetTotalAffectedVertexCount(int32 LODIndex = 0) const
	{
		if (LODData.IsValidIndex(LODIndex) && LODData[LODIndex].bAffectedVerticesRegistered)
		{
			return static_cast<uint32>(LODData[LODIndex].AffectedVerticesManager.GetTotalAffectedCount());
		}
		return 0;
	}

#if WITH_EDITORONLY_DATA
	virtual bool RequestReadbackDeformerGeometry(TUniquePtr<FMeshDeformerGeometryReadbackRequest> InRequest) override { return false; }

	/**
	 * 베이크용 GPU 변형 결과 Readback
	 * TightenedBindPose + Normals + Tangents를 CPU로 읽어옴
	 *
	 * @param OutPositions - 변형된 버텍스 위치 (float3 packed)
	 * @param OutNormals - 재계산된 노멀 (float3 packed)
	 * @param OutTangents - 재계산된 탄젠트 (float4 packed)
	 * @param LODIndex - LOD 인덱스
	 * @return 성공 여부
	 */
	bool ReadbackDeformedGeometry(
		TArray<FVector3f>& OutPositions,
		TArray<FVector3f>& OutNormals,
		TArray<FVector4f>& OutTangents,
		int32 LODIndex = 0);

	/**
	 * TightenedBindPose가 캐싱되어 있는지 확인
	 * @param LODIndex - LOD 인덱스
	 * @return 캐싱되어 있으면 true
	 */
	bool HasCachedDeformedGeometry(int32 LODIndex = 0) const;
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

		// 재계산된 노멀 캐싱 (NormalRecomputeCS 결과)
		// TightenedBindPose와 함께 캐싱되어 캐싱된 프레임에서도 올바른 노멀 사용
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedNormalsShared;

		// 재계산된 탄젠트 캐싱 (TangentRecomputeCS 결과)
		// Gram-Schmidt 정규직교화된 탄젠트를 캐싱
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedTangentsShared;

		// 디버그 Influence 캐싱 (TightnessCS에서 출력)
		// DrawAffectedVertices에서 GPU 계산 Influence 시각화용
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedDebugInfluencesShared;

		// 디버그 포인트 버퍼 캐싱 (WorldPosition + Influence)
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedDebugPointBufferShared;

		// Bulge 디버그 포인트 버퍼 캐싱 (WorldPosition + Influence)
		// Cyan→Magenta 색상 그라데이션으로 표시
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedDebugBulgePointBufferShared;

		// ===== GPU Readback 관련 =====
		// Readback 결과 저장 (스레드 안전 공유용)
		TSharedPtr<TArray<float>> DebugInfluenceReadbackResult;

		// Readback 완료 플래그 (스레드 안전)
		TSharedPtr<std::atomic<bool>> bDebugInfluenceReadbackComplete;

		// Readback할 버텍스 수
		uint32 DebugInfluenceCount = 0;
	};

	// LOD별 데이터 배열 (인덱스 = LOD 번호)
	TArray<FLODDeformationData> LODData;

	// LOD 개수 (초기화 시 설정)
	int32 NumLODs = 0;
};
