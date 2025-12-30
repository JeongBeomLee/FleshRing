// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingComputeWorker.h"
#include "FleshRingDeformerInstance.h"
#include "FleshRingSkinningShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SkeletalMeshDeformerHelpers.h"
#include "SkeletalRenderPublic.h"
#include "SkeletalMeshUpdater.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingWorker, Log, All);

// ============================================================================
// FFleshRingComputeSystem - 싱글톤 인스턴스
// ============================================================================
FFleshRingComputeSystem* FFleshRingComputeSystem::Instance = nullptr;
bool FFleshRingComputeSystem::bIsRegistered = false;

// ============================================================================
// FFleshRingComputeWorker 구현
// ============================================================================

FFleshRingComputeWorker::FFleshRingComputeWorker(FSceneInterface const* InScene)
	: Scene(InScene)
{
}

FFleshRingComputeWorker::~FFleshRingComputeWorker()
{
}

bool FFleshRingComputeWorker::HasWork(FName InExecutionGroupName) const
{
	// EndOfFrameUpdate 실행 그룹에서만 작업 처리
	if (InExecutionGroupName != ComputeTaskExecutionGroup::EndOfFrameUpdate)
	{
		return false;
	}

	FScopeLock Lock(&WorkItemsLock);
	return PendingWorkItems.Num() > 0;
}

void FFleshRingComputeWorker::SubmitWork(FComputeContext& Context)
{
	// EndOfFrameUpdate 실행 그룹에서만 처리
	if (Context.ExecutionGroupName != ComputeTaskExecutionGroup::EndOfFrameUpdate)
	{
		return;
	}

	// 대기 중인 작업 가져오기
	TArray<FFleshRingWorkItem> WorkItemsToProcess;
	{
		FScopeLock Lock(&WorkItemsLock);
		WorkItemsToProcess = MoveTemp(PendingWorkItems);
		PendingWorkItems.Reset();
	}

	if (WorkItemsToProcess.Num() == 0)
	{
		return;
	}

	// MeshDeformer 단계 대기 - 이것이 핵심!
	// UpdatedFrameNumber가 올바르게 설정된 후 실행되도록 보장
	FSkeletalMeshUpdater::WaitForStage(Context.GraphBuilder, ESkeletalMeshUpdateStage::MeshDeformer);

	// 각 작업 실행
	for (FFleshRingWorkItem& WorkItem : WorkItemsToProcess)
	{
		ExecuteWorkItem(Context.GraphBuilder, WorkItem);
	}
}

void FFleshRingComputeWorker::EnqueueWork(FFleshRingWorkItem&& InWorkItem)
{
	FScopeLock Lock(&WorkItemsLock);
	PendingWorkItems.Add(MoveTemp(InWorkItem));
}

void FFleshRingComputeWorker::AbortWork(UFleshRingDeformerInstance* InDeformerInstance)
{
	FScopeLock Lock(&WorkItemsLock);

	for (int32 i = PendingWorkItems.Num() - 1; i >= 0; --i)
	{
		if (PendingWorkItems[i].DeformerInstance.Get() == InDeformerInstance)
		{
			// Fallback 실행
			if (PendingWorkItems[i].FallbackDelegate.IsBound())
			{
				PendingWorkItems[i].FallbackDelegate.Execute();
			}
			PendingWorkItems.RemoveAt(i);
		}
	}
}

void FFleshRingComputeWorker::ExecuteWorkItem(FRDGBuilder& GraphBuilder, FFleshRingWorkItem& WorkItem)
{
	// DeformerInstance 유효성 검사 (PIE 종료 시 dangling pointer 방지)
	// MeshObject는 DeformerInstance의 수명에 종속되므로, DeformerInstance가 무효화되면
	// MeshObject도 dangling 상태일 가능성이 높음
	if (!WorkItem.DeformerInstance.IsValid())
	{
		UE_LOG(LogFleshRingWorker, Verbose, TEXT("FleshRing: DeformerInstance 무효화됨 - 작업 건너뜀"));
		return;
	}

	FSkeletalMeshObject* MeshObject = WorkItem.MeshObject;
	const int32 LODIndex = WorkItem.LODIndex;
	const uint32 TotalVertexCount = WorkItem.TotalVertexCount;

	// 유효성 검사
	if (!MeshObject || LODIndex < 0)
	{
		WorkItem.FallbackDelegate.ExecuteIfBound();
		return;
	}

	FSkeletalMeshRenderData const& RenderData = MeshObject->GetSkeletalMeshRenderData();
	if (LODIndex >= RenderData.LODRenderData.Num())
	{
		WorkItem.FallbackDelegate.ExecuteIfBound();
		return;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData.LODRenderData[LODIndex];
	if (LODData.RenderSections.Num() == 0 || !LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices())
	{
		WorkItem.FallbackDelegate.ExecuteIfBound();
		return;
	}

	const int32 FirstAvailableSection = FSkeletalMeshDeformerHelpers::GetIndexOfFirstAvailableSection(MeshObject, LODIndex);
	if (FirstAvailableSection == INDEX_NONE)
	{
		WorkItem.FallbackDelegate.ExecuteIfBound();
		return;
	}

	const uint32 ActualNumVertices = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();
	const uint32 ActualBufferSize = ActualNumVertices * 3;

	if (TotalVertexCount != ActualNumVertices)
	{
		UE_LOG(LogFleshRingWorker, Warning, TEXT("FleshRing: 버텍스 수 불일치 - 캐시:%d, 실제:%d"),
			TotalVertexCount, ActualNumVertices);
		WorkItem.FallbackDelegate.ExecuteIfBound();
		return;
	}

	FRDGExternalAccessQueue ExternalAccessQueue;

	// Position 출력 버퍼 할당 (ping-pong 자동 처리)
	FRDGBuffer* OutputPositionBuffer = FSkeletalMeshDeformerHelpers::AllocateVertexFactoryPositionBuffer(
		GraphBuilder, ExternalAccessQueue, MeshObject, LODIndex, TEXT("FleshRingOutput"));

	if (!OutputPositionBuffer)
	{
		UE_LOG(LogFleshRingWorker, Warning, TEXT("FleshRing: Position 버퍼 할당 실패"));
		ExternalAccessQueue.Submit(GraphBuilder);
		WorkItem.FallbackDelegate.ExecuteIfBound();
		return;
	}

	// TightenedBindPose 버퍼 처리
	FRDGBufferRef TightenedBindPoseBuffer = nullptr;

	if (WorkItem.bNeedTightnessCaching)
	{
		// 소스 버퍼 생성
		FRDGBufferRef SourceBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
			TEXT("FleshRing_SourcePositions")
		);
		GraphBuilder.QueueBufferUpload(
			SourceBuffer,
			WorkItem.SourceDataPtr->GetData(),
			ActualBufferSize * sizeof(float),
			ERDGInitialDataFlags::None
		);

		// TightenedBindPose 버퍼 생성
		TightenedBindPoseBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
			TEXT("FleshRing_TightenedBindPose")
		);

		// 소스 복사
		AddCopyBufferPass(GraphBuilder, TightenedBindPoseBuffer, SourceBuffer);

		// TightnessCS 적용
		if (WorkItem.RingDispatchDataPtr.IsValid())
		{
			for (const FFleshRingWorkItem::FRingDispatchData& DispatchData : *WorkItem.RingDispatchDataPtr)
			{
				// 로컬 복사본 생성 (역변환 행렬 설정을 위해)
				FTightnessDispatchParams Params = DispatchData.Params;
				if (Params.NumAffectedVertices == 0) continue;

				FRDGBufferRef IndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Params.NumAffectedVertices),
					TEXT("FleshRing_AffectedIndices")
				);
				GraphBuilder.QueueBufferUpload(
					IndicesBuffer,
					DispatchData.Indices.GetData(),
					DispatchData.Indices.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				FRDGBufferRef InfluencesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Params.NumAffectedVertices),
					TEXT("FleshRing_Influences")
				);
				GraphBuilder.QueueBufferUpload(
					InfluencesBuffer,
					DispatchData.Influences.GetData(),
					DispatchData.Influences.Num() * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// SDF 텍스처 등록 (Pooled → RDG)
				FRDGTextureRef SDFTextureRDG = nullptr;
				if (DispatchData.bHasValidSDF && DispatchData.SDFPooledTexture.IsValid())
				{
					SDFTextureRDG = GraphBuilder.RegisterExternalTexture(DispatchData.SDFPooledTexture);

					// OBB 지원: LocalToComponent의 역변환 계산
					// 셰이더에서 버텍스(컴포넌트 스페이스)를 로컬 스페이스로 변환할 때 사용
					FMatrix InverseMatrix = DispatchData.SDFLocalToComponent.Inverse().ToMatrixWithScale();
					Params.ComponentToSDFLocal = FMatrix44f(InverseMatrix);

					UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] TightnessCS Dispatch: SDF Mode (OBB), Verts=%d, Strength=%.2f"),
						Params.NumAffectedVertices, Params.TightnessStrength);
				}
				else
				{
					UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] TightnessCS Dispatch: Manual Mode, Verts=%d, Strength=%.2f"),
						Params.NumAffectedVertices, Params.TightnessStrength);
				}

				DispatchFleshRingTightnessCS(
					GraphBuilder,
					Params,
					SourceBuffer,
					IndicesBuffer,
					InfluencesBuffer,
					TightenedBindPoseBuffer,
					SDFTextureRDG
				);
			}
		}

		// 영구 버퍼로 변환하여 캐싱
		if (WorkItem.CachedBufferSharedPtr.IsValid())
		{
			*WorkItem.CachedBufferSharedPtr = GraphBuilder.ConvertToExternalBuffer(TightenedBindPoseBuffer);
		}
	}
	else
	{
		// 캐싱된 버퍼 사용
		if (WorkItem.CachedBufferSharedPtr.IsValid() && WorkItem.CachedBufferSharedPtr->IsValid())
		{
			TightenedBindPoseBuffer = GraphBuilder.RegisterExternalBuffer(*WorkItem.CachedBufferSharedPtr);
		}
		else
		{
			UE_LOG(LogFleshRingWorker, Warning, TEXT("FleshRing: 캐싱된 버퍼가 유효하지 않음"));
			ExternalAccessQueue.Submit(GraphBuilder);
			WorkItem.FallbackDelegate.ExecuteIfBound();
			return;
		}
	}

	// 스키닝 적용
	const FSkinWeightVertexBuffer* WeightBuffer = LODData.GetSkinWeightVertexBuffer();
	FRHIShaderResourceView* InputWeightStreamSRV = WeightBuffer ?
		WeightBuffer->GetDataVertexBuffer()->GetSRV() : nullptr;

	FRHIShaderResourceView* SourceTangentsSRV = LODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetTangentsSRV();

	if (!InputWeightStreamSRV)
	{
		UE_LOG(LogFleshRingWorker, Warning, TEXT("FleshRing: 웨이트 스트림 없음"));
		AddCopyBufferPass(GraphBuilder, OutputPositionBuffer, TightenedBindPoseBuffer);
	}
	else
	{
		// Tangent 출력 버퍼 할당
		FRDGBuffer* OutputTangentBuffer = FSkeletalMeshDeformerHelpers::AllocateVertexFactoryTangentBuffer(
			GraphBuilder, ExternalAccessQueue, MeshObject, LODIndex, TEXT("FleshRingTangentOutput"));

		const int32 NumSections = LODData.RenderSections.Num();

		for (int32 SectionIndex = 0; SectionIndex < NumSections; ++SectionIndex)
		{
			const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIndex];

			FRHIShaderResourceView* BoneMatricesSRV = FSkeletalMeshDeformerHelpers::GetBoneBufferForReading(
				MeshObject, LODIndex, SectionIndex, false);
			if (!BoneMatricesSRV) continue;

			FSkinningDispatchParams SkinParams;
			SkinParams.BaseVertexIndex = Section.BaseVertexIndex;
			SkinParams.NumVertices = Section.NumVertices;
			SkinParams.InputWeightStride = WeightBuffer->GetConstantInfluencesVertexStride();
			SkinParams.InputWeightIndexSize = WeightBuffer->GetBoneIndexByteSize() |
				(WeightBuffer->GetBoneWeightByteSize() << 8);
			SkinParams.NumBoneInfluences = WeightBuffer->GetMaxBoneInfluences();

			DispatchFleshRingSkinningCS(GraphBuilder, SkinParams, TightenedBindPoseBuffer,
				SourceTangentsSRV, OutputPositionBuffer, nullptr,
				OutputTangentBuffer, BoneMatricesSRV, nullptr, InputWeightStreamSRV);
		}
	}

	// VertexFactory 버퍼 업데이트
	FSkeletalMeshDeformerHelpers::UpdateVertexFactoryBufferOverrides(
		GraphBuilder, MeshObject, LODIndex, WorkItem.bInvalidatePreviousPosition);

	ExternalAccessQueue.Submit(GraphBuilder);
}

// ============================================================================
// FFleshRingComputeSystem 구현
// ============================================================================

FFleshRingComputeSystem& FFleshRingComputeSystem::Get()
{
	if (!Instance)
	{
		Instance = new FFleshRingComputeSystem();
	}
	return *Instance;
}

void FFleshRingComputeSystem::CreateWorkers(FSceneInterface const* InScene, TArray<IComputeTaskWorker*>& OutWorkers)
{
	FScopeLock Lock(&WorkersLock);

	FFleshRingComputeWorker* Worker = new FFleshRingComputeWorker(InScene);
	SceneWorkers.Add(InScene, Worker);
	OutWorkers.Add(Worker);
}

void FFleshRingComputeSystem::DestroyWorkers(FSceneInterface const* InScene, TArray<IComputeTaskWorker*>& InOutWorkers)
{
	FScopeLock Lock(&WorkersLock);

	FFleshRingComputeWorker** WorkerPtr = SceneWorkers.Find(InScene);
	if (WorkerPtr)
	{
		FFleshRingComputeWorker* Worker = *WorkerPtr;
		InOutWorkers.Remove(Worker);
		delete Worker;
		SceneWorkers.Remove(InScene);
	}
}

FFleshRingComputeWorker* FFleshRingComputeSystem::GetWorker(FSceneInterface const* InScene) const
{
	FScopeLock Lock(&WorkersLock);

	FFleshRingComputeWorker* const* WorkerPtr = SceneWorkers.Find(InScene);
	return WorkerPtr ? *WorkerPtr : nullptr;
}

void FFleshRingComputeSystem::Register()
{
	if (!bIsRegistered)
	{
		ComputeSystemInterface::RegisterSystem(&Get());
		bIsRegistered = true;
	}
}

void FFleshRingComputeSystem::Unregister()
{
	if (bIsRegistered)
	{
		ComputeSystemInterface::UnregisterSystem(&Get());
		bIsRegistered = false;

		if (Instance)
		{
			delete Instance;
			Instance = nullptr;
		}
	}
}
