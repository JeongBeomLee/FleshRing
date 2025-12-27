// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ComputeWorkerInterface.h"
#include "ComputeSystemInterface.h"
#include "RenderGraphResources.h"
#include "FleshRingTightnessShader.h"

class FSkeletalMeshObject;
class UFleshRingDeformerInstance;

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

	// Ring 데이터 (Tightness용)
	struct FRingDispatchData
	{
		FTightnessDispatchParams Params;
		TArray<uint32> Indices;
		TArray<float> Influences;
	};
	TSharedPtr<TArray<FRingDispatchData>> RingDispatchDataPtr;

	// 캐싱 상태
	bool bNeedTightnessCaching = false;
	bool bInvalidatePreviousPosition = false;

	// 캐시 버퍼 (렌더 스레드에서 접근)
	TRefCountPtr<FRDGPooledBuffer>* CachedBufferPtr = nullptr;

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
