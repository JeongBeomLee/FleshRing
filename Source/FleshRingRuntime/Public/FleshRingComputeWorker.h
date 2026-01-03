// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ComputeWorkerInterface.h"
#include "ComputeSystemInterface.h"
#include "RenderGraphResources.h"
#include "RendererInterface.h"
#include "FleshRingTightnessShader.h"
#include "FleshRingBulgeShader.h"
#include "FleshRingNormalRecomputeShader.h"

class FSkeletalMeshObject;
class UFleshRingDeformerInstance;
struct IPooledRenderTarget;

// ============================================================================
// FFleshRingWorkItem - 큐잉된 작업 항목
// ============================================================================
struct FFleshRingWorkItem
{
	// 작업 식별
	TWeakObjectPtr<UFleshRingDeformerInstance> DeformerInstance;
	FSkeletalMeshObject* MeshObject = nullptr;
	int32 LODIndex = 0;

	// 버텍스 데이터
	uint32 TotalVertexCount = 0;
	TSharedPtr<TArray<float>> SourceDataPtr;

	// Ring 데이터 (Tightness + Bulge용)
	struct FRingDispatchData
	{
		FTightnessDispatchParams Params;
		TArray<uint32> Indices;
		TArray<float> Influences;

		// SDF 캐시 데이터 (렌더 스레드로 안전하게 전달)
		TRefCountPtr<IPooledRenderTarget> SDFPooledTexture;
		FVector3f SDFBoundsMin = FVector3f::ZeroVector;
		FVector3f SDFBoundsMax = FVector3f::ZeroVector;
		bool bHasValidSDF = false;

		/**
		 * SDF 로컬 → 컴포넌트 스페이스 변환 (OBB 지원)
		 * SDF는 로컬 스페이스에서 생성되므로, 셰이더에서 버텍스를
		 * 컴포넌트 → 로컬로 역변환해야 올바른 SDF 샘플링 가능
		 */
		FTransform SDFLocalToComponent = FTransform::Identity;

		// ===== Ring별 Bulge 데이터 =====
		bool bEnableBulge = false;
		TArray<uint32> BulgeIndices;
		TArray<float> BulgeInfluences;
		float BulgeStrength = 1.0f;
		float MaxBulgeDistance = 10.0f;

		// ===== Normal Recomputation용 인접 데이터 =====
		// AdjacencyOffsets[i] = AffectedVertex i의 인접 삼각형 시작 인덱스
		// AdjacencyOffsets[NumAffected] = 총 인접 삼각형 수 (sentinel)
		TArray<uint32> AdjacencyOffsets;
		// 인접 삼각형 인덱스의 평탄화된 리스트
		TArray<uint32> AdjacencyTriangles;
	};
	TSharedPtr<TArray<FRingDispatchData>> RingDispatchDataPtr;

	// ===== Bulge 전역 플래그 =====
	// 하나 이상의 Ring에서 Bulge가 활성화되어 있는지 여부
	// (VolumeAccumBuffer 생성 여부 결정용)
	bool bAnyRingHasBulge = false;

	// ===== Normal Recomputation용 메시 인덱스 버퍼 =====
	// 모든 Ring이 공유하는 메시 인덱스 버퍼 (3 indices per triangle)
	TSharedPtr<TArray<uint32>> MeshIndicesPtr;

	// 캐싱 상태
	bool bNeedTightnessCaching = false;
	bool bInvalidatePreviousPosition = false;

	// 캐시 버퍼 (렌더 스레드에서 접근)
	// TSharedPtr로 래핑하여 DeformerInstance 파괴 후에도 안전하게 접근 가능
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedBufferSharedPtr;

	// Fallback 델리게이트
	FSimpleDelegate FallbackDelegate;
};

// ============================================================================
// FFleshRingComputeWorker - IComputeTaskWorker 구현
// ============================================================================
// 렌더러가 적절한 시점에 호출하여 FleshRing 작업 실행
class FLESHRINGRUNTIME_API FFleshRingComputeWorker : public IComputeTaskWorker
{
public:
	FFleshRingComputeWorker(FSceneInterface const* InScene);
	virtual ~FFleshRingComputeWorker();

	// IComputeTaskWorker interface
	virtual bool HasWork(FName InExecutionGroupName) const override;
	virtual void SubmitWork(FComputeContext& Context) override;

	// 작업 큐잉 (렌더 스레드에서 호출)
	void EnqueueWork(FFleshRingWorkItem&& InWorkItem);

	// 작업 취소 (특정 DeformerInstance의 작업 제거)
	void AbortWork(UFleshRingDeformerInstance* InDeformerInstance);

private:
	// 실제 작업 실행
	void ExecuteWorkItem(FRDGBuilder& GraphBuilder, FFleshRingWorkItem& WorkItem);
	
	FSceneInterface const* Scene;

	// 대기 중인 작업 목록 (렌더 스레드 전용)
	TArray<FFleshRingWorkItem> PendingWorkItems;

	// 스레드 안전성을 위한 락
	mutable FCriticalSection WorkItemsLock;
};

// ============================================================================
// FFleshRingComputeSystem - IComputeSystem 구현
// ============================================================================
// Scene별 FleshRingComputeWorker 생성/관리
class FLESHRINGRUNTIME_API FFleshRingComputeSystem : public IComputeSystem
{
public:
	static FFleshRingComputeSystem& Get();

	// IComputeSystem interface
	virtual void CreateWorkers(FSceneInterface const* InScene, TArray<IComputeTaskWorker*>& OutWorkers) override;
	virtual void DestroyWorkers(FSceneInterface const* InScene, TArray<IComputeTaskWorker*>& InOutWorkers) override;

	// Scene에 대한 Worker 조회
	FFleshRingComputeWorker* GetWorker(FSceneInterface const* InScene) const;

	// 시스템 등록/해제
	static void Register();
	static void Unregister();

private:
	FFleshRingComputeSystem() = default;

	// Scene별 Worker 매핑
	TMap<FSceneInterface const*, FFleshRingComputeWorker*> SceneWorkers;
	mutable FCriticalSection WorkersLock;

	static FFleshRingComputeSystem* Instance;
	static bool bIsRegistered;
};
