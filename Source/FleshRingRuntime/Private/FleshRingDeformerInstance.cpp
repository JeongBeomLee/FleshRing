// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingDeformerInstance.h"
#include "FleshRingDeformer.h"
#include "FleshRingComponent.h"
#include "FleshRingTightnessShader.h"
#include "FleshRingSkinningShader.h"
#include "FleshRingComputeWorker.h"
#include "Components/SkinnedMeshComponent.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SkeletalMeshDeformerHelpers.h"
#include "SkeletalRenderPublic.h"
#include "RenderingThread.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRing, Log, All);

#include UE_INLINE_GENERATED_CPP_BY_NAME(FleshRingDeformerInstance)

UFleshRingDeformerInstance::UFleshRingDeformerInstance()
{
}

void UFleshRingDeformerInstance::SetupFromDeformer(UFleshRingDeformer* InDeformer, UMeshComponent* InMeshComponent)
{
	Deformer = InDeformer;
	MeshComponent = InMeshComponent;
	Scene = InMeshComponent ? InMeshComponent->GetScene() : nullptr;
	LastLodIndex = INDEX_NONE;

	// FleshRingComponent 찾기 및 AffectedVertices 등록
	if (AActor* Owner = InMeshComponent->GetOwner())
	{
		FleshRingComponent = Owner->FindComponentByClass<UFleshRingComponent>();
		if (FleshRingComponent.IsValid())
		{
			USkeletalMeshComponent* SkelMesh = Cast<USkeletalMeshComponent>(InMeshComponent);
			if (SkelMesh)
			{
				bAffectedVerticesRegistered = AffectedVerticesManager.RegisterAffectedVertices(
					FleshRingComponent.Get(), SkelMesh);

				if (bAffectedVerticesRegistered)
				{
					UE_LOG(LogFleshRing, Log, TEXT("AffectedVertices 등록 완료: %d개 Ring, 총 %d개 버텍스"),
						AffectedVerticesManager.GetAllRingData().Num(),
						AffectedVerticesManager.GetTotalAffectedCount());
				}
				else
				{
					UE_LOG(LogFleshRing, Warning, TEXT("AffectedVertices 등록 실패"));
				}
			}
		}
		else
		{
			UE_LOG(LogFleshRing, Warning, TEXT("FleshRingComponent를 찾을 수 없음"));
		}
	}
}

void UFleshRingDeformerInstance::AllocateResources()
{
	// Resources are allocated on-demand in EnqueueWork
}

void UFleshRingDeformerInstance::ReleaseResources()
{
	// Release cached TightenedBindPose buffer
	// 캐싱된 TightenedBindPose 버퍼 해제
	CachedTightenedBindPose.SafeRelease();
	bTightenedBindPoseCached = false;
	CachedTightnessLODIndex = INDEX_NONE;
	CachedTightnessVertexCount = 0;

	// Release cached source positions
	// 캐싱된 소스 위치 해제
	CachedSourcePositions.Empty();
	bSourcePositionsCached = false;
}

void UFleshRingDeformerInstance::EnqueueWork(FEnqueueWorkDesc const& InDesc)
{
	// Only process during Update workload, skip Setup/Trigger phases
	if (InDesc.WorkLoadType != EWorkLoad::WorkLoad_Update)
	{
		if (InDesc.FallbackDelegate.IsBound())
		{
			InDesc.FallbackDelegate.ExecuteIfBound();
		}
		return;
	}

	UFleshRingDeformer* DeformerPtr = Deformer.Get();
	USkinnedMeshComponent* SkinnedMeshComp = Cast<USkinnedMeshComponent>(MeshComponent.Get());

	if (!DeformerPtr || !SkinnedMeshComp)
	{
		if (InDesc.FallbackDelegate.IsBound())
		{
			ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
			{
				FallbackDelegate.ExecuteIfBound();
			});
		}
		return;
	}

	// AffectedVertices가 등록되지 않았으면 Fallback
	if (!bAffectedVerticesRegistered || AffectedVerticesManager.GetTotalAffectedCount() == 0)
	{
		if (InDesc.FallbackDelegate.IsBound())
		{
			ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
			{
				FallbackDelegate.ExecuteIfBound();
			});
		}
		return;
	}

	FSkeletalMeshObject* MeshObject = SkinnedMeshComp->MeshObject;
	if (!MeshObject || MeshObject->IsCPUSkinned())
	{
		if (InDesc.FallbackDelegate.IsBound())
		{
			ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
			{
				FallbackDelegate.ExecuteIfBound();
			});
		}
		return;
	}

	// Check if MeshObject has been updated at least once
	if (!MeshObject->bHasBeenUpdatedAtLeastOnce)
	{
		if (InDesc.FallbackDelegate.IsBound())
		{
			ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
			{
				FallbackDelegate.ExecuteIfBound();
			});
		}
		return;
	}

	// FleshRingComputeWorker 가져오기
	FFleshRingComputeWorker* Worker = FFleshRingComputeSystem::Get().GetWorker(Scene);
	if (!Worker)
	{
		UE_LOG(LogFleshRing, Warning, TEXT("FleshRing: ComputeWorker를 찾을 수 없음"));
		if (InDesc.FallbackDelegate.IsBound())
		{
			ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
			{
				FallbackDelegate.ExecuteIfBound();
			});
		}
		return;
	}

	const int32 LODIndex = SkinnedMeshComp->GetPredictedLODLevel();

	// Track LOD changes for invalidating previous position
	bool bInvalidatePreviousPosition = false;
	if (LODIndex != LastLodIndex)
	{
		bInvalidatePreviousPosition = true;
		LastLodIndex = LODIndex;

		// LOD 변경 시 TightenedBindPose 캐시 무효화
		if (LODIndex != CachedTightnessLODIndex)
		{
			bTightenedBindPoseCached = false;
			CachedTightnessLODIndex = LODIndex;
			UE_LOG(LogFleshRing, Log, TEXT("LOD 변경 감지 (%d -> %d): TightenedBindPose 캐시 무효화"),
				CachedTightnessLODIndex, LODIndex);
		}
	}

	// ================================================================
	// 소스 버텍스 캐싱 (첫 프레임에만)
	// ================================================================
	if (!bSourcePositionsCached)
	{
		USkeletalMeshComponent* SkelMeshComp = Cast<USkeletalMeshComponent>(SkinnedMeshComp);
		USkeletalMesh* SkelMesh = SkelMeshComp ? SkelMeshComp->GetSkeletalMeshAsset() : nullptr;
		if (SkelMesh)
		{
			const FSkeletalMeshRenderData* RenderData = SkelMesh->GetResourceForRendering();
			if (RenderData && RenderData->LODRenderData.Num() > LODIndex)
			{
				const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];
				const uint32 NumVerts = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

				CachedSourcePositions.SetNum(NumVerts * 3);
				for (uint32 i = 0; i < NumVerts; ++i)
				{
					const FVector3f& Pos = LODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i);
					CachedSourcePositions[i * 3 + 0] = Pos.X;
					CachedSourcePositions[i * 3 + 1] = Pos.Y;
					CachedSourcePositions[i * 3 + 2] = Pos.Z;
				}
				bSourcePositionsCached = true;

				UE_LOG(LogFleshRing, Log, TEXT("소스 버텍스 캐싱 완료: %d개"), NumVerts);
			}
		}
	}

	if (!bSourcePositionsCached)
	{
		if (InDesc.FallbackDelegate.IsBound())
		{
			ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
			{
				FallbackDelegate.ExecuteIfBound();
			});
		}
		return;
	}

	// ================================================================
	// 작업 아이템 생성 및 큐잉
	// ================================================================
	const TArray<FRingAffectedData>& AllRingData = AffectedVerticesManager.GetAllRingData();
	const uint32 TotalVertexCount = CachedSourcePositions.Num() / 3;

	// Ring 데이터 준비
	TSharedPtr<TArray<FFleshRingWorkItem::FRingDispatchData>> RingDispatchDataPtr =
		MakeShared<TArray<FFleshRingWorkItem::FRingDispatchData>>();
	RingDispatchDataPtr->Reserve(AllRingData.Num());

	for (const FRingAffectedData& RingData : AllRingData)
	{
		if (RingData.Vertices.Num() == 0)
		{
			continue;
		}

		FFleshRingWorkItem::FRingDispatchData DispatchData;
		DispatchData.Params = CreateTightnessParams(RingData, TotalVertexCount);
		DispatchData.Indices = RingData.PackedIndices;
		DispatchData.Influences = RingData.PackedInfluences;
		RingDispatchDataPtr->Add(MoveTemp(DispatchData));
	}

	if (RingDispatchDataPtr->Num() == 0)
	{
		if (InDesc.FallbackDelegate.IsBound())
		{
			ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
			{
				FallbackDelegate.ExecuteIfBound();
			});
		}
		return;
	}

	// TightenedBindPose 캐싱 여부 결정
	bool bNeedTightnessCaching = !bTightenedBindPoseCached;
	if (bNeedTightnessCaching)
	{
		bTightenedBindPoseCached = true;
		CachedTightnessVertexCount = TotalVertexCount;
		bInvalidatePreviousPosition = true;
		UE_LOG(LogFleshRing, Log, TEXT("TightenedBindPose 캐싱 시작 (%d 버텍스)"), TotalVertexCount);
	}

	// 작업 아이템 생성
	FFleshRingWorkItem WorkItem;
	WorkItem.DeformerInstance = this;
	WorkItem.MeshObject = MeshObject;
	WorkItem.LODIndex = LODIndex;
	WorkItem.TotalVertexCount = TotalVertexCount;
	WorkItem.SourceDataPtr = MakeShared<TArray<float>>(CachedSourcePositions);
	WorkItem.RingDispatchDataPtr = RingDispatchDataPtr;
	WorkItem.bNeedTightnessCaching = bNeedTightnessCaching;
	WorkItem.bInvalidatePreviousPosition = bInvalidatePreviousPosition;
	WorkItem.CachedBufferPtr = &CachedTightenedBindPose;
	WorkItem.FallbackDelegate = InDesc.FallbackDelegate;

	// 렌더 스레드에서 Worker에 작업 큐잉
	// ENQUEUE_RENDER_COMMAND는 작업을 큐잉만 하고, 실제 실행은
	// 렌더러가 EndOfFrameUpdate에서 SubmitWork를 호출할 때 수행됨
	ENQUEUE_RENDER_COMMAND(FleshRingEnqueueWork)(
		[Worker, WorkItem = MoveTemp(WorkItem)](FRHICommandListImmediate& RHICmdList) mutable
		{
			Worker->EnqueueWork(MoveTemp(WorkItem));
		});
}

EMeshDeformerOutputBuffer UFleshRingDeformerInstance::GetOutputBuffers() const
{
	// Position + Tangent 둘 다 출력해야 라이팅 일치
	// Position만 출력하면 엔진 기본 스키닝 Tangent와 불일치 → 잔상 발생
	return EMeshDeformerOutputBuffer::SkinnedMeshPosition | EMeshDeformerOutputBuffer::SkinnedMeshTangents;
}