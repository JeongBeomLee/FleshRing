// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingComputeWorker.h"
#include "FleshRingDeformerInstance.h"
#include "FleshRingSkinningShader.h"
#include "FleshRingHeatPropagationShader.h"
#include "FleshRingUVSyncShader.h"
#include "FleshRingDebugPointOutputShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SkeletalMeshDeformerHelpers.h"
#include "SkeletalRenderPublic.h"
#include "SkeletalMeshUpdater.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "RHIGPUReadback.h"
#include "FleshRingDebugTypes.h"

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

	// ===== Passthrough Mode =====
	// AffectedVertices가 0이 되었을 때 원본 데이터로 SkinningCS 한 번 실행
	// 이전 변형의 탄젠트 잔상을 제거하기 위해 필요
	if (WorkItem.bPassthroughMode)
	{
		// 원본 소스 포지션이 없으면 Fallback
		if (!WorkItem.SourceDataPtr.IsValid() || WorkItem.SourceDataPtr->Num() == 0)
		{
			UE_LOG(LogFleshRingWorker, Warning, TEXT("FleshRing: Passthrough 모드지만 SourceDataPtr 없음"));
			ExternalAccessQueue.Submit(GraphBuilder);
			WorkItem.FallbackDelegate.ExecuteIfBound();
			return;
		}

		// 원본 바인드 포즈 버퍼 생성
		FRDGBufferRef PassthroughPositionBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
			TEXT("FleshRing_PassthroughPositions")
		);
		GraphBuilder.QueueBufferUpload(
			PassthroughPositionBuffer,
			WorkItem.SourceDataPtr->GetData(),
			ActualBufferSize * sizeof(float),
			ERDGInitialDataFlags::None
		);

		// SkinningCS 실행 (원본 탄젠트 사용 - RecomputedNormals/Tangents = nullptr)
		const FSkinWeightVertexBuffer* WeightBuffer = LODData.GetSkinWeightVertexBuffer();
		FRHIShaderResourceView* InputWeightStreamSRV = WeightBuffer ?
			WeightBuffer->GetDataVertexBuffer()->GetSRV() : nullptr;

		FRHIShaderResourceView* SourceTangentsSRV = LODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetTangentsSRV();

		if (!InputWeightStreamSRV)
		{
			// 웨이트 없으면 그냥 복사
			AddCopyBufferPass(GraphBuilder, OutputPositionBuffer, PassthroughPositionBuffer);
		}
		else
		{
			// Tangent 출력 버퍼 할당
			FRDGBuffer* OutputTangentBuffer = FSkeletalMeshDeformerHelpers::AllocateVertexFactoryTangentBuffer(
				GraphBuilder, ExternalAccessQueue, MeshObject, LODIndex, TEXT("FleshRingPassthroughTangent"));

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

				// RecomputedNormalsBuffer와 RecomputedTangentsBuffer는 nullptr
				// → SkinningCS가 원본 탄젠트 사용
				DispatchFleshRingSkinningCS(GraphBuilder, SkinParams, PassthroughPositionBuffer,
					SourceTangentsSRV, OutputPositionBuffer, nullptr,
					OutputTangentBuffer, BoneMatricesSRV, nullptr, InputWeightStreamSRV,
					nullptr, nullptr);  // RecomputedNormalsBuffer, RecomputedTangentsBuffer = nullptr
			}
		}

		// VertexFactory 버퍼 업데이트 (이전 위치 무효화)
		FSkeletalMeshDeformerHelpers::UpdateVertexFactoryBufferOverrides(
			GraphBuilder, MeshObject, LODIndex, true);

		ExternalAccessQueue.Submit(GraphBuilder);
		return;
	}

	// TightenedBindPose 버퍼 처리
	FRDGBufferRef TightenedBindPoseBuffer = nullptr;

	// NormalRecomputeCS 출력 버퍼 (SkinningCS에서 사용)
	FRDGBufferRef RecomputedNormalsBuffer = nullptr;

	// TangentRecomputeCS 출력 버퍼 (SkinningCS에서 사용)
	FRDGBufferRef RecomputedTangentsBuffer = nullptr;

	// DebugPointBuffer (GPU 디버그 렌더링용)
	FRDGBufferRef DebugPointBuffer = nullptr;

	// DebugBulgePointBuffer (Bulge GPU 디버그 렌더링용)
	FRDGBufferRef DebugBulgePointBuffer = nullptr;

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

		// ===== VolumeAccumBuffer 생성 (하나 이상의 Ring에서 Bulge 활성화 시) =====
		// 각 Ring이 독립된 VolumeAccum 슬롯을 사용 (OriginalRingIndex 기반)
		FRDGBufferRef VolumeAccumBuffer = nullptr;
		const int32 NumRings = WorkItem.RingDispatchDataPtr.IsValid() ? WorkItem.RingDispatchDataPtr->Num() : 0;

		if (WorkItem.bAnyRingHasBulge && NumRings > 0)
		{
			// OriginalRingIndex의 최대값 계산 (스킵된 Ring이 있어도 정확한 버퍼 크기 확보)
			int32 MaxOriginalRingIndex = 0;
			for (const FFleshRingWorkItem::FRingDispatchData& DispatchData : *WorkItem.RingDispatchDataPtr)
			{
				MaxOriginalRingIndex = FMath::Max(MaxOriginalRingIndex, DispatchData.OriginalRingIndex);
			}
			const int32 VolumeBufferSize = MaxOriginalRingIndex + 1;

			VolumeAccumBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), VolumeBufferSize),
				TEXT("FleshRing_VolumeAccum")
			);
			// 0으로 초기화 (Atomic 연산 전)
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(VolumeAccumBuffer, PF_R32_UINT), 0u);
		}

		// ===== DebugInfluencesBuffer 생성 (디버그 Influence 출력 활성화 시) =====
		// GPU에서 계산된 Influence 값을 캐싱하여 DrawDebugPoint에서 시각화
		// 다중 Ring에서 InfluenceCumulativeOffset이 누적되므로 버퍼 크기도 합산
		FRDGBufferRef DebugInfluencesBuffer = nullptr;
		uint32 TotalInfluenceVertices = 0;

		if (WorkItem.bOutputDebugInfluences && NumRings > 0)
		{
			// 모든 Ring의 NumAffectedVertices 합산 (다중 Ring 지원)
			for (int32 RingIdx = 0; RingIdx < NumRings; ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& Data = (*WorkItem.RingDispatchDataPtr)[RingIdx];
				TotalInfluenceVertices += Data.Params.NumAffectedVertices;
			}

			if (TotalInfluenceVertices > 0)
			{
				DebugInfluencesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), TotalInfluenceVertices),
					TEXT("FleshRing_DebugInfluences")
				);
				// 0으로 초기화
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DebugInfluencesBuffer, PF_R32_FLOAT), 0.0f);
			}
		}

		// ===== DebugPointBuffer 생성 (GPU 렌더링용) =====
		// 다중 Ring에서 DebugPointCumulativeOffset이 누적되므로 버퍼 크기도 합산
		uint32 TotalAffectedVertices = 0;
		if (WorkItem.bOutputDebugPoints && NumRings > 0)
		{
			for (int32 RingIdx = 0; RingIdx < NumRings; ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& Data = (*WorkItem.RingDispatchDataPtr)[RingIdx];
				TotalAffectedVertices += Data.Params.NumAffectedVertices;
			}

			if (TotalAffectedVertices > 0)
			{
				DebugPointBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FFleshRingDebugPoint), TotalAffectedVertices),
					TEXT("FleshRing_DebugPointBuffer")
				);
				// 0으로 초기화
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DebugPointBuffer), 0u);
			}
		}

		// ===== DebugBulgePointBuffer 생성 (Bulge GPU 렌더링용) =====
		uint32 MaxBulgeVertices = 0;
		if (WorkItem.bOutputDebugBulgePoints && WorkItem.bAnyRingHasBulge && NumRings > 0)
		{
			// 전체 Bulge 버텍스 수 합산 (다중 Ring의 모든 Bulge 포인트를 담아야 함)
			for (int32 RingIdx = 0; RingIdx < NumRings; ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& Data = (*WorkItem.RingDispatchDataPtr)[RingIdx];
				if (Data.bEnableBulge)
				{
					// FMath::Max 사용 시 가장 큰 Ring의 포인트만 계산되어 다른 Ring 포인트 누락됨
					MaxBulgeVertices += Data.BulgeIndices.Num();
				}
			}

			if (MaxBulgeVertices > 0)
			{
				DebugBulgePointBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(FFleshRingDebugPoint), MaxBulgeVertices),
					TEXT("FleshRing_DebugBulgePointBuffer")
				);
				// 0으로 초기화
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DebugBulgePointBuffer), 0u);
			}
		}

		// TightnessCS 적용
		if (WorkItem.RingDispatchDataPtr.IsValid())
		{
			// 디버그 포인트/Influence 버퍼 오프셋 (다중 링 지원)
			// DebugPointBaseOffset과 DebugInfluenceBaseOffset은 동일 (같은 NumAffectedVertices 단위)
			uint32 DebugPointCumulativeOffset = 0;

			for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

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

				// Influence는 GPU에서 직접 계산

				// ===== UV Seam Welding: RepresentativeIndices 버퍼 생성 =====
				// 같은 위치의 UV 중복 버텍스들이 동일하게 변형되도록 보장
				FRDGBufferRef RepresentativeIndicesBuffer = nullptr;
				if (DispatchData.RepresentativeIndices.Num() > 0)
				{
					RepresentativeIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DispatchData.RepresentativeIndices.Num()),
						TEXT("FleshRing_RepresentativeIndices")
					);
					GraphBuilder.QueueBufferUpload(
						RepresentativeIndicesBuffer,
						DispatchData.RepresentativeIndices.GetData(),
						DispatchData.RepresentativeIndices.Num() * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// SDF 텍스처 등록 (Pooled → RDG)
				FRDGTextureRef SDFTextureRDG = nullptr;
				if (DispatchData.bHasValidSDF && DispatchData.SDFPooledTexture.IsValid())
				{
					SDFTextureRDG = GraphBuilder.RegisterExternalTexture(DispatchData.SDFPooledTexture);

					// OBB 지원: LocalToComponent의 역변환 계산
					// 셰이더에서 버텍스(컴포넌트 스페이스)를 로컬 스페이스로 변환할 때 사용
					// 주의: FTransform::Inverse()는 비균일 스케일+회전 시 Shear 손실 발생
					// 해결: FMatrix로 변환 후 FMatrix::Inverse() 사용 (Shear 보존)
					FMatrix ForwardMatrix = DispatchData.SDFLocalToComponent.ToMatrixWithScale();
					FMatrix InverseMatrix = ForwardMatrix.Inverse();
					Params.ComponentToSDFLocal = FMatrix44f(InverseMatrix);

					// Local → Component 변환 행렬 (스케일 포함 정확한 역변환용)
					Params.SDFLocalToComponent = FMatrix44f(DispatchData.SDFLocalToComponent.ToMatrixWithScale());

					// Ring Center/Axis (SDF Local Space) - 바운드 확장 시에도 정확한 위치 전달
					Params.SDFLocalRingCenter = DispatchData.SDFLocalRingCenter;
					Params.SDFLocalRingAxis = DispatchData.SDFLocalRingAxis;

				}
				else
				{
				}

				// Bulge 활성화 시 부피 누적 활성화 (이 Ring 또는 다른 Ring에서 Bulge 사용)
				if (WorkItem.bAnyRingHasBulge && VolumeAccumBuffer)
				{
					Params.bAccumulateVolume = 1;
					Params.FixedPointScale = 1000.0f;  // float → uint 변환 스케일
					Params.RingIndex = DispatchData.OriginalRingIndex;  // 실제 Ring 배열 인덱스 (가시성 필터링용)
				}

				// 디버그 Influence 출력 활성화
				// DebugInfluences 버퍼도 DebugPointBaseOffset 사용 (동일한 오프셋)
				if (WorkItem.bOutputDebugInfluences && DebugInfluencesBuffer)
				{
					Params.bOutputDebugInfluences = 1;
					Params.DebugPointBaseOffset = DebugPointCumulativeOffset;
				}

				// DebugPointBuffer는 DebugPointOutputCS에서 최종 위치 기반으로 처리

				DispatchFleshRingTightnessCS(
					GraphBuilder,
					Params,
					SourceBuffer,
					IndicesBuffer,
					// Influence는 GPU에서 직접 계산
					RepresentativeIndicesBuffer,  // UV seam welding용 대표 버텍스 인덱스
					TightenedBindPoseBuffer,
					SDFTextureRDG,
					VolumeAccumBuffer,
					DebugInfluencesBuffer
				);

				// 디버그 포인트/Influence 오프셋 누적 (다음 Ring을 위해)
				DebugPointCumulativeOffset += Params.NumAffectedVertices;
			}
		}

		// ===== BulgeCS Dispatch (TightnessCS 이후, 각 Ring별로) =====
		if (WorkItem.bAnyRingHasBulge && VolumeAccumBuffer && WorkItem.RingDispatchDataPtr.IsValid())
		{
			// 디버그 Bulge 포인트 버퍼 오프셋 (다중 링 지원)
			uint32 DebugBulgePointCumulativeOffset = 0;

			// 각 Ring별로 BulgeCS 디스패치
			for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

				// 이 Ring에서 Bulge가 비활성화되었거나 데이터가 없으면 스킵
				if (!DispatchData.bEnableBulge || DispatchData.BulgeIndices.Num() == 0)
				{
					continue;
				}

				const uint32 NumBulgeVertices = DispatchData.BulgeIndices.Num();

				// Bulge 버텍스 인덱스 버퍼 생성
				FRDGBufferRef BulgeIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumBulgeVertices),
					*FString::Printf(TEXT("FleshRing_BulgeVertexIndices_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					BulgeIndicesBuffer,
					DispatchData.BulgeIndices.GetData(),
					NumBulgeVertices * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Bulge 영향도 버퍼 생성
				FRDGBufferRef BulgeInfluencesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumBulgeVertices),
					*FString::Printf(TEXT("FleshRing_BulgeInfluences_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					BulgeInfluencesBuffer,
					DispatchData.BulgeInfluences.GetData(),
					NumBulgeVertices * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// ===== 별도 출력 버퍼 생성 (SRV/UAV 충돌 방지) =====
				FRDGBufferRef BulgeOutputBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
					*FString::Printf(TEXT("FleshRing_BulgeOutput_Ring%d"), RingIdx)
				);
				// TightenedBindPose를 먼저 복사 (Bulge 대상 아닌 버텍스 유지)
				AddCopyBufferPass(GraphBuilder, BulgeOutputBuffer, TightenedBindPoseBuffer);

				// 이 Ring의 SDF 텍스처 등록
				FRDGTextureRef RingSDFTextureRDG = nullptr;
				FMatrix44f RingComponentToSDFLocal = FMatrix44f::Identity;
				if (DispatchData.bHasValidSDF && DispatchData.SDFPooledTexture.IsValid())
				{
					RingSDFTextureRDG = GraphBuilder.RegisterExternalTexture(DispatchData.SDFPooledTexture);
					// NOTE: FTransform::Inverse() 대신 FMatrix::Inverse() 사용 (비균일 스케일+회전 시 Shear 보존),
					// 링 회전 시 Bulge 영역이 Positive인데 Negative 방향에 잡히거나 Negative인데 Positive 방향에 잡히는 정점이 있었음!
					FMatrix ForwardMatrix = DispatchData.SDFLocalToComponent.ToMatrixWithScale();
					FMatrix InverseMatrix = ForwardMatrix.Inverse();
					// 이 방식으로 하면 안됨!
					//FMatrix InverseMatrix = DispatchData.SDFLocalToComponent.Inverse().ToMatrixWithScale();
					RingComponentToSDFLocal = FMatrix44f(InverseMatrix);
				}

				// Bulge 디스패치 파라미터 설정
				FBulgeDispatchParams BulgeParams;
				BulgeParams.NumBulgeVertices = NumBulgeVertices;
				BulgeParams.NumTotalVertices = ActualNumVertices;
				BulgeParams.BulgeStrength = DispatchData.BulgeStrength;
				BulgeParams.MaxBulgeDistance = DispatchData.MaxBulgeDistance;
				BulgeParams.FixedPointScale = 0.001f;  // uint → float 변환 스케일 (1/1000)
				BulgeParams.BulgeAxisDirection = DispatchData.BulgeAxisDirection;  // 방향 필터링
				BulgeParams.RingIndex = DispatchData.OriginalRingIndex;  // 실제 Ring 배열 인덱스 (가시성 필터링용)
				BulgeParams.BulgeRadialRatio = DispatchData.BulgeRadialRatio;  // Radial vs Axial 비율
				BulgeParams.UpperBulgeStrength = DispatchData.UpperBulgeStrength;  // 상단 강도 배수
				BulgeParams.LowerBulgeStrength = DispatchData.LowerBulgeStrength;  // 하단 강도 배수

				// SDF 모드 vs VirtualRing 모드 분기
				BulgeParams.bUseSDFInfluence = DispatchData.bHasValidSDF ? 1 : 0;

				if (DispatchData.bHasValidSDF)
				{
					// SDF 모드: SDF 관련 파라미터 설정
					BulgeParams.SDFBoundsMin = DispatchData.SDFBoundsMin;
					BulgeParams.SDFBoundsMax = DispatchData.SDFBoundsMax;
					BulgeParams.ComponentToSDFLocal = RingComponentToSDFLocal;
					BulgeParams.SDFLocalRingCenter = DispatchData.SDFLocalRingCenter;
					BulgeParams.SDFLocalRingAxis = DispatchData.SDFLocalRingAxis;
				}
				else
				{
					// VirtualRing 모드: Component Space 파라미터 설정
					BulgeParams.RingCenter = DispatchData.Params.RingCenter;
					BulgeParams.RingAxis = DispatchData.Params.RingAxis;
					BulgeParams.RingHeight = DispatchData.Params.RingHeight;
				}

				// NOTE: 디버그 포인트 출력은 DebugPointOutputCS에서 최종 위치로 처리

				DispatchFleshRingBulgeCS(
					GraphBuilder,
					BulgeParams,
					TightenedBindPoseBuffer,  // INPUT (SRV) - 이전 Ring의 Bulge 결과 포함
					BulgeIndicesBuffer,
					BulgeInfluencesBuffer,
					VolumeAccumBuffer,
					BulgeOutputBuffer,        // OUTPUT (UAV) - 별도 출력 버퍼
					RingSDFTextureRDG
				);

				// 결과를 TightenedBindPoseBuffer로 복사 (다음 Ring이 이 결과 위에 누적)
				AddCopyBufferPass(GraphBuilder, TightenedBindPoseBuffer, BulgeOutputBuffer);
			}
		}

		// ===== BoneRatioCS Dispatch (BulgeCS 이후, NormalRecomputeCS 이전) =====
		// 같은 높이(슬라이스)의 버텍스들이 동일한 반경을 갖도록 균일화
		if (WorkItem.RingDispatchDataPtr.IsValid())
		{
			for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

				// 반경 균일화 스무딩이 비활성화되면 스킵
				if (!DispatchData.bEnableRadialSmoothing)
				{
					continue;
				}

				// 슬라이스 데이터가 없으면 스킵
				if (DispatchData.SlicePackedData.Num() == 0 || DispatchData.OriginalBoneDistances.Num() == 0)
				{
					continue;
				}

				// 축 높이 데이터가 없으면 스킵 (가우시안 가중치 필요)
				if (DispatchData.AxisHeights.Num() == 0)
				{
					continue;
				}

				const uint32 NumAffected = DispatchData.Indices.Num();
				if (NumAffected == 0) continue;

				// 영향받는 버텍스 인덱스 버퍼
				FRDGBufferRef BoneRatioIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumAffected),
					*FString::Printf(TEXT("FleshRing_BoneRatioIndices_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					BoneRatioIndicesBuffer,
					DispatchData.Indices.GetData(),
					NumAffected * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Influence 버퍼
				FRDGBufferRef BoneRatioInfluencesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumAffected),
					*FString::Printf(TEXT("FleshRing_BoneRatioInfluences_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					BoneRatioInfluencesBuffer,
					DispatchData.Influences.GetData(),
					NumAffected * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// 원본 본 거리 버퍼
				FRDGBufferRef OriginalBoneDistancesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumAffected),
					*FString::Printf(TEXT("FleshRing_OriginalBoneDistances_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					OriginalBoneDistancesBuffer,
					DispatchData.OriginalBoneDistances.GetData(),
					NumAffected * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// 축 높이 버퍼 (가우시안 가중치용)
				FRDGBufferRef AxisHeightsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumAffected),
					*FString::Printf(TEXT("FleshRing_AxisHeights_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					AxisHeightsBuffer,
					DispatchData.AxisHeights.GetData(),
					NumAffected * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// 슬라이스 데이터 버퍼
				FRDGBufferRef SliceDataBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DispatchData.SlicePackedData.Num()),
					*FString::Printf(TEXT("FleshRing_SliceData_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					SliceDataBuffer,
					DispatchData.SlicePackedData.GetData(),
					DispatchData.SlicePackedData.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// 출력 버퍼 생성 및 초기화
				// 중요: 셰이더는 영향받는 버텍스만 쓰기 때문에,
				// 나머지 버텍스를 보존하려면 입력 데이터로 초기화해야 함
				FRDGBufferRef BoneRatioOutputBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualNumVertices * 3),
					*FString::Printf(TEXT("FleshRing_BoneRatioOutput_Ring%d"), RingIdx)
				);
				AddCopyBufferPass(GraphBuilder, BoneRatioOutputBuffer, TightenedBindPoseBuffer);

				// BoneRatio 디스패치 파라미터
				FBoneRatioDispatchParams BoneRatioParams;
				BoneRatioParams.NumAffectedVertices = NumAffected;
				BoneRatioParams.NumTotalVertices = ActualNumVertices;
				BoneRatioParams.RingAxis = FVector3f(DispatchData.Params.RingAxis);
				BoneRatioParams.RingCenter = FVector3f(DispatchData.Params.RingCenter);
				BoneRatioParams.BlendStrength = DispatchData.RadialBlendStrength;
				BoneRatioParams.HeightSigma = DispatchData.RadialSliceHeight;  // 슬라이스 높이와 동일한 시그마

				// BoneRatio 디스패치
				DispatchFleshRingBoneRatioCS(
					GraphBuilder,
					BoneRatioParams,
					TightenedBindPoseBuffer,
					BoneRatioOutputBuffer,
					BoneRatioIndicesBuffer,
					BoneRatioInfluencesBuffer,
					OriginalBoneDistancesBuffer,
					AxisHeightsBuffer,
					SliceDataBuffer
				);

				// 결과를 TightenedBindPoseBuffer로 복사
				AddCopyBufferPass(GraphBuilder, TightenedBindPoseBuffer, BoneRatioOutputBuffer);

			}
		}

		// ===== HeatPropagationCS Dispatch (BoneRatioCS 이후, LaplacianCS 이전) =====
		// Delta-based Heat Propagation: Seed의 변형 delta를 SmoothingRegion 영역으로 전파
		// Algorithm: Init → Diffuse × N → Apply
		if (WorkItem.RingDispatchDataPtr.IsValid())
		{
			for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

				// Heat Propagation 활성화 조건: bEnableHeatPropagation && HopBased mode && SmoothingRegion 데이터 존재
				if (!DispatchData.bEnableHeatPropagation || DispatchData.SmoothingExpandMode != ESmoothingVolumeMode::HopBased)
				{
					continue;
				}

				// SmoothingRegion 데이터 검증
				if (DispatchData.SmoothingRegionIndices.Num() == 0 ||
					DispatchData.SmoothingRegionIsAnchor.Num() == 0 ||
					DispatchData.SmoothingRegionLaplacianAdjacency.Num() == 0)
				{
					continue;
				}

				const uint32 NumSmoothingRegionVertices = DispatchData.SmoothingRegionIndices.Num();

				// ★ 배열 크기 일치 검증 (Smoothing Expand 변경 시 크기 불일치 방지)
				// SmoothingRegionIsAnchor는 SmoothingRegionIndices와 동일한 크기여야 함
				if (DispatchData.SmoothingRegionIsAnchor.Num() != (int32)NumSmoothingRegionVertices)
				{
					UE_LOG(LogFleshRingWorker, Warning,
						TEXT("FleshRing: SmoothingRegionIsAnchor 크기 불일치 - IsAnchor:%d, Expected:%d (Ring %d). 캐시 재생성 필요."),
						DispatchData.SmoothingRegionIsAnchor.Num(), NumSmoothingRegionVertices, RingIdx);
					continue;
				}

				// ========================================
				// 1. Original Positions 버퍼 (바인드 포즈)
				// ========================================
				FRDGBufferRef OriginalPositionsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
					*FString::Printf(TEXT("FleshRing_HeatProp_OriginalPos_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					OriginalPositionsBuffer,
					WorkItem.SourceDataPtr->GetData(),
					ActualBufferSize * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// ========================================
				// 2. Output Positions 버퍼
				// TightenedBindPose를 먼저 복사 (non-extended 버텍스 유지)
				// ========================================
				FRDGBufferRef HeatPropOutputBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
					*FString::Printf(TEXT("FleshRing_HeatProp_Output_Ring%d"), RingIdx)
				);
				AddCopyBufferPass(GraphBuilder, HeatPropOutputBuffer, TightenedBindPoseBuffer);

				// ========================================
				// 3. SmoothingRegion Indices 버퍼
				// ========================================
				FRDGBufferRef SmoothingRegionIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSmoothingRegionVertices),
					*FString::Printf(TEXT("FleshRing_HeatProp_ExtIndices_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					SmoothingRegionIndicesBuffer,
					DispatchData.SmoothingRegionIndices.GetData(),
					NumSmoothingRegionVertices * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// ========================================
				// 4. Seed/Barrier Flags 분리
				// ========================================
				// 셰이더가 기대하는 구조:
				//   - IsSeedFlags: 1 = Bulge(delta 전파 source), 0 = 그 외
				//   - IsBarrierFlags: 1 = Tightness(전파 차단), 0 = 그 외
				//
				// SmoothingRegionIsAnchor 데이터: 1 = Tightness, 0 = Non-Seed
				// bIncludeBulgeVerticesAsSeeds가 true면 Bulge도 포함
				// ========================================

				// 먼저 원본 데이터 로드 (0=Non-Seed, 1=Tightness)
				TArray<uint32> SeedTypeData;
				SeedTypeData.SetNumUninitialized(NumSmoothingRegionVertices);
				FMemory::Memcpy(SeedTypeData.GetData(), DispatchData.SmoothingRegionIsAnchor.GetData(), NumSmoothingRegionVertices * sizeof(uint32));

				// Bulge 버텍스도 Seed로 포함시키는 경우 (2로 마킹)
				if (DispatchData.bIncludeBulgeVerticesAsSeeds && DispatchData.BulgeIndices.Num() > 0)
				{
					// BulgeIndices를 Set으로 변환 - O(M) 공간 (M = Bulge 버텍스 수)
					TSet<uint32> BulgeIndicesSet;
					BulgeIndicesSet.Reserve(DispatchData.BulgeIndices.Num());
					for (uint32 BulgeIdx : DispatchData.BulgeIndices)
					{
						BulgeIndicesSet.Add(BulgeIdx);
					}

					// SmoothingRegion 영역 순회하며 Bulge 버텍스를 2로 마킹
					for (uint32 ThreadIdx = 0; ThreadIdx < NumSmoothingRegionVertices; ++ThreadIdx)
					{
						if (SeedTypeData[ThreadIdx] == 0 &&
							BulgeIndicesSet.Contains(DispatchData.SmoothingRegionIndices[ThreadIdx]))
						{
							SeedTypeData[ThreadIdx] = 2;  // Bulge = 2
						}
					}
				}

				// SeedTypeData 분리: IsSeedFlags, IsBarrierFlags
				// bIncludeBulgeVerticesAsSeeds에 따라 동작 변경:
				//   false: Tightness가 Seed (기존 동작)
				//   true:  Bulge가 Seed, Tightness는 Barrier (전파 차단)
				TArray<uint32> IsSeedFlagsData;
				TArray<uint32> IsBarrierFlagsData;
				IsSeedFlagsData.SetNumUninitialized(NumSmoothingRegionVertices);
				IsBarrierFlagsData.SetNumUninitialized(NumSmoothingRegionVertices);

				for (uint32 i = 0; i < NumSmoothingRegionVertices; ++i)
				{
					if (DispatchData.bIncludeBulgeVerticesAsSeeds)
					{
						// Bulge만 Seed, Tightness는 Barrier (전파 차단)
						IsSeedFlagsData[i] = (SeedTypeData[i] == 2) ? 1 : 0;      // Bulge = Seed
						IsBarrierFlagsData[i] = (SeedTypeData[i] == 1) ? 1 : 0;   // Tightness = Barrier
					}
					else
					{
						// Tightness만 Seed, Barrier 없음 (기존 동작)
						IsSeedFlagsData[i] = (SeedTypeData[i] == 1) ? 1 : 0;      // Tightness = Seed
						IsBarrierFlagsData[i] = 0;                                 // No Barrier
					}
				}

				// ========================================
				// 4.5. IsBoundarySeedFlags 계산: Non-Seed 이웃이 있는 Seed만 경계
				// ========================================
				// 목적: 내부 Seed의 강한 변형이 경계를 넘어 전파되는 것 방지
				// 경계 Seed만 delta를 설정, 내부 Seed는 delta=0 (전파 안 함)
				constexpr uint32 MAX_NEIGHBORS_CONST = 12;
				TArray<uint32> IsBoundarySeedFlagsData;
				IsBoundarySeedFlagsData.SetNumZeroed(NumSmoothingRegionVertices);

				// VertexIndex → ThreadIndex 역매핑 생성
				TMap<uint32, uint32> VertexToThreadIndex;
				VertexToThreadIndex.Reserve(NumSmoothingRegionVertices);
				for (uint32 i = 0; i < NumSmoothingRegionVertices; ++i)
				{
					VertexToThreadIndex.Add(DispatchData.SmoothingRegionIndices[i], i);
				}

				// 각 Seed에 대해 이웃 중 Non-Seed가 있는지 확인
				const TArray<uint32>& AdjacencyData = DispatchData.SmoothingRegionLaplacianAdjacency;
				for (uint32 i = 0; i < NumSmoothingRegionVertices; ++i)
				{
					if (IsSeedFlagsData[i] == 0)
					{
						continue;  // Non-Seed는 경계 판정 불필요
					}

					// Seed: 이웃 중 Non-Seed가 있으면 경계
					uint32 AdjOffset = i * (1 + MAX_NEIGHBORS_CONST);
					if (AdjOffset >= (uint32)AdjacencyData.Num())
					{
						continue;
					}

					uint32 NeighborCount = AdjacencyData[AdjOffset];
					bool bHasNonSeedNeighbor = false;

					for (uint32 n = 0; n < NeighborCount && n < MAX_NEIGHBORS_CONST; ++n)
					{
						uint32 NeighborVertexIdx = AdjacencyData[AdjOffset + 1 + n];

						if (const uint32* NeighborThreadIdx = VertexToThreadIndex.Find(NeighborVertexIdx))
						{
							// SmoothingRegion 영역 내 이웃: IsSeedFlags 확인
							if (IsSeedFlagsData[*NeighborThreadIdx] == 0)
							{
								bHasNonSeedNeighbor = true;
								break;
							}
						}
						else
						{
							// SmoothingRegion 영역 밖 이웃 → Non-Seed로 간주
							bHasNonSeedNeighbor = true;
							break;
						}
					}

					IsBoundarySeedFlagsData[i] = bHasNonSeedNeighbor ? 1 : 0;
				}

				// IsSeedFlagsBuffer 생성
				FRDGBufferRef IsSeedFlagsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSmoothingRegionVertices),
					*FString::Printf(TEXT("FleshRing_HeatProp_IsSeed_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					IsSeedFlagsBuffer,
					IsSeedFlagsData.GetData(),
					NumSmoothingRegionVertices * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// IsBoundarySeedFlagsBuffer 생성
				FRDGBufferRef IsBoundarySeedFlagsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSmoothingRegionVertices),
					*FString::Printf(TEXT("FleshRing_HeatProp_IsBoundarySeed_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					IsBoundarySeedFlagsBuffer,
					IsBoundarySeedFlagsData.GetData(),
					NumSmoothingRegionVertices * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// IsBarrierFlagsBuffer 생성
				FRDGBufferRef IsBarrierFlagsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSmoothingRegionVertices),
					*FString::Printf(TEXT("FleshRing_HeatProp_IsBarrier_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					IsBarrierFlagsBuffer,
					IsBarrierFlagsData.GetData(),
					NumSmoothingRegionVertices * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// ========================================
				// 5. Adjacency Data 버퍼 (Laplacian adjacency 재사용)
				// ========================================
				FRDGBufferRef AdjacencyDataBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DispatchData.SmoothingRegionLaplacianAdjacency.Num()),
					*FString::Printf(TEXT("FleshRing_HeatProp_Adjacency_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					AdjacencyDataBuffer,
					DispatchData.SmoothingRegionLaplacianAdjacency.GetData(),
					DispatchData.SmoothingRegionLaplacianAdjacency.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// ========================================
				// 5.5. UV Seam Welding: RepresentativeIndices 버퍼 (HeatPropagation용)
				// ========================================
				FRDGBufferRef HeatPropRepresentativeIndicesBuffer = nullptr;
				if (DispatchData.SmoothingRegionRepresentativeIndices.Num() == NumSmoothingRegionVertices)
				{
					HeatPropRepresentativeIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSmoothingRegionVertices),
						*FString::Printf(TEXT("FleshRing_HeatProp_RepIndices_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						HeatPropRepresentativeIndicesBuffer,
						DispatchData.SmoothingRegionRepresentativeIndices.GetData(),
						NumSmoothingRegionVertices * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// ========================================
				// 6. Heat Propagation Dispatch (Delta-based)
				// ========================================
				FHeatPropagationDispatchParams HeatPropParams;
				HeatPropParams.NumExtendedVertices = NumSmoothingRegionVertices;
				HeatPropParams.NumTotalVertices = ActualNumVertices;
				HeatPropParams.HeatLambda = DispatchData.HeatPropagationLambda;
				HeatPropParams.NumIterations = DispatchData.HeatPropagationIterations;

				DispatchFleshRingHeatPropagationCS(
					GraphBuilder,
					HeatPropParams,
					OriginalPositionsBuffer,       // 원본 바인드 포즈
					TightenedBindPoseBuffer,       // 현재 변형된 위치 (Seed의 delta 계산용)
					HeatPropOutputBuffer,          // 출력 위치
					SmoothingRegionIndicesBuffer,         // SmoothingRegion 영역 버텍스 인덱스
					IsSeedFlagsBuffer,             // Seed 플래그 (1=Bulge, 0=그외)
					IsBoundarySeedFlagsBuffer,     // Boundary Seed 플래그 (1=Non-Seed 이웃 있음, 0=내부 Seed 또는 Non-Seed)
					IsBarrierFlagsBuffer,          // Barrier 플래그 (1=Tightness/전파차단, 0=그외)
					AdjacencyDataBuffer,           // 인접 정보 (diffusion용)
					HeatPropRepresentativeIndicesBuffer   // UV seam welding용 대표 버텍스 인덱스
				);

				// ========================================
				// 7. 결과를 TightenedBindPoseBuffer로 복사
				// ========================================
				AddCopyBufferPass(GraphBuilder, TightenedBindPoseBuffer, HeatPropOutputBuffer);
			}
		}

		// ===== PBD Edge Constraint (BoneRatioCS 이후, LaplacianCS 이전) =====
		// Tolerance 기반 PBD: Affected Vertices(앵커)를 고정하고 주변 버텍스만 보정
		// 허용 범위(Tolerance) 내 변형은 유지, 범위 밖 극단적 변형만 보정
		if (WorkItem.RingDispatchDataPtr.IsValid())
		{
			for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

				// PBD Edge Constraint가 비활성화되었으면 스킵
				if (!DispatchData.bEnablePBDEdgeConstraint)
				{
					continue;
				}

				// ===== PBD 영역 선택 (통합된 SmoothingRegion 사용) =====
				const bool bUseSmoothingRegion =
					DispatchData.SmoothingRegionIndices.Num() > 0 &&
					DispatchData.SmoothingRegionIsAnchor.Num() == DispatchData.SmoothingRegionIndices.Num() &&
					DispatchData.SmoothingRegionPBDAdjacency.Num() > 0;

				// SmoothingRegion 데이터가 없으면 스킵
				if (!bUseSmoothingRegion)
				{
					continue;
				}

				// 통합된 데이터 소스 사용
				const TArray<uint32>& IndicesSource = DispatchData.SmoothingRegionIndices;
				const TArray<uint32>& IsAnchorSource = DispatchData.SmoothingRegionIsAnchor;
				const TArray<uint32>& AdjacencySource = DispatchData.SmoothingRegionPBDAdjacency;
				const TArray<uint32>& RepresentativeSource = DispatchData.SmoothingRegionRepresentativeIndices;

				const uint32 NumAffected = IndicesSource.Num();
				if (NumAffected == 0) continue;

				// 인접 데이터가 없으면 스킵
				if (AdjacencySource.Num() == 0)
				{
					continue;
				}

				// FullVertexAnchorFlags 검증
				if (DispatchData.FullVertexAnchorFlags.Num() == 0)
				{
					continue;
				}

				// 영향받는 버텍스 인덱스 버퍼
				FRDGBufferRef PBDIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumAffected),
					*FString::Printf(TEXT("FleshRing_PBDIndices_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					PBDIndicesBuffer,
					IndicesSource.GetData(),
					NumAffected * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// IsAnchorFlags 버퍼 (per-thread anchor flags)
				// bPBDAnchorAffectedVertices=true: 1 = Affected (앵커, 고정), 0 = SmoothingRegion (자유)
				// bPBDAnchorAffectedVertices=false: 모든 버텍스가 0 (자유, PBD 적용)
				FRDGBufferRef IsAnchorFlagsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumAffected),
					*FString::Printf(TEXT("FleshRing_PBDIsAnchor_Ring%d"), RingIdx)
				);

				// bPBDAnchorAffectedVertices가 false면 모든 앵커를 해제 (모든 버텍스 자유)
				if (DispatchData.bPBDAnchorAffectedVertices)
				{
					// 기존 IsAnchor 데이터 사용 (Affected=1, SmoothingRegion=0)
					GraphBuilder.QueueBufferUpload(
						IsAnchorFlagsBuffer,
						IsAnchorSource.GetData(),
						NumAffected * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}
				else
				{
					// 캐시된 Zero 배열 사용 (매 틱 할당 방지)
					GraphBuilder.QueueBufferUpload(
						IsAnchorFlagsBuffer,
						DispatchData.CachedZeroIsAnchorFlags.GetData(),
						NumAffected * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// FullVertexAnchorFlags 버퍼 (전체 메시 크기, 이웃 앵커 여부 조회용)
				FRDGBufferRef FullVertexAnchorFlagsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DispatchData.FullVertexAnchorFlags.Num()),
					*FString::Printf(TEXT("FleshRing_FullVertexAnchorFlags_Ring%d"), RingIdx)
				);

				if (DispatchData.bPBDAnchorAffectedVertices)
				{
					// 기존 FullVertexAnchorFlags 사용
					GraphBuilder.QueueBufferUpload(
						FullVertexAnchorFlagsBuffer,
						DispatchData.FullVertexAnchorFlags.GetData(),
						DispatchData.FullVertexAnchorFlags.Num() * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}
				else
				{
					// 캐시된 Zero 배열 사용 (매 틱 할당 방지)
					GraphBuilder.QueueBufferUpload(
						FullVertexAnchorFlagsBuffer,
						DispatchData.CachedZeroFullVertexAnchorFlags.GetData(),
						DispatchData.FullVertexAnchorFlags.Num() * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// PBD 인접 데이터 버퍼 (rest length 포함)
				FRDGBufferRef PBDAdjacencyBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), AdjacencySource.Num()),
					*FString::Printf(TEXT("FleshRing_PBDAdjacency_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					PBDAdjacencyBuffer,
					AdjacencySource.GetData(),
					AdjacencySource.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// ===== UV Seam Welding: RepresentativeIndices 버퍼 생성 (PBD용) =====
				// RepresentativeSource는 이미 bUseSmoothingRegion에 따라 선택됨 (위에서 정의)
				FRDGBufferRef PBDRepresentativeIndicesBuffer = nullptr;
				if (RepresentativeSource.Num() > 0 && RepresentativeSource.Num() == static_cast<int32>(NumAffected))
				{
					PBDRepresentativeIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumAffected),
						*FString::Printf(TEXT("FleshRing_PBDRepIndices_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						PBDRepresentativeIndicesBuffer,
						RepresentativeSource.GetData(),
						NumAffected * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// PBD 디스패치 파라미터 (Tolerance 기반)
				FPBDEdgeDispatchParams PBDParams;
				PBDParams.NumAffectedVertices = NumAffected;
				PBDParams.NumTotalVertices = ActualNumVertices;
				PBDParams.Stiffness = DispatchData.PBDStiffness;
				PBDParams.NumIterations = DispatchData.PBDIterations;
				PBDParams.Tolerance = DispatchData.PBDTolerance;

				// PBD Edge Constraint 디스패치 (Tolerance 기반, in-place ping-pong)
				DispatchFleshRingPBDEdgeCS_MultiPass(
					GraphBuilder,
					PBDParams,
					TightenedBindPoseBuffer,
					PBDIndicesBuffer,
					PBDRepresentativeIndicesBuffer,  // UV seam welding용 대표 버텍스 인덱스
					IsAnchorFlagsBuffer,             // per-thread 앵커 플래그
					FullVertexAnchorFlagsBuffer,           // 전체 메시 앵커 맵 (이웃 조회용)
					PBDAdjacencyBuffer
				);

				// [DEBUG] PBDEdgeCS 로그 (필요시 주석 해제)
				// UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] PBDEdgeCS Ring[%d]: Tolerance=%.2f, %d vertices, Stiffness=%.2f, Iterations=%d"),
				// 	RingIdx, PBDParams.Tolerance, NumAffected, PBDParams.Stiffness, PBDParams.NumIterations);
			}
		}

		// ===== LaplacianCS Dispatch (PBD Edge Constraint 이후, LayerPenetrationCS 이전) =====
		// 전체적인 메시 스무딩 적용 (경계 영역 부드럽게)
		if (WorkItem.RingDispatchDataPtr.IsValid())
		{
			for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

				// Laplacian 스무딩이 비활성화되었으면 스킵
				if (!DispatchData.bEnableLaplacianSmoothing)
				{
					continue;
				}

				// ===== 스무딩 영역 선택 (통합된 SmoothingRegion 사용) =====
				// [설계] SmoothingRegion 데이터가 있으면 사용, 없으면 원본 사용
				const bool bUseSmoothingRegion =
					DispatchData.SmoothingRegionIndices.Num() > 0 &&
					DispatchData.SmoothingRegionInfluences.Num() == DispatchData.SmoothingRegionIndices.Num() &&
					DispatchData.SmoothingRegionLaplacianAdjacency.Num() > 0;

				// 사용할 데이터 소스 선택 (통합: SmoothingRegion > Original)
				const TArray<uint32>& IndicesSource = bUseSmoothingRegion
					? DispatchData.SmoothingRegionIndices : DispatchData.Indices;
				const TArray<float>& InfluenceSource = bUseSmoothingRegion
					? DispatchData.SmoothingRegionInfluences : DispatchData.Influences;
				const TArray<uint32>& AdjacencySource = bUseSmoothingRegion
					? DispatchData.SmoothingRegionLaplacianAdjacency : DispatchData.LaplacianAdjacencyData;

				// 인접 데이터가 없으면 스킵
				if (AdjacencySource.Num() == 0)
				{
					continue;
				}

				const uint32 NumSmoothingVertices = IndicesSource.Num();
				if (NumSmoothingVertices == 0) continue;

				// 버텍스 인덱스 버퍼
				FRDGBufferRef LaplacianIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSmoothingVertices),
					*FString::Printf(TEXT("FleshRing_LaplacianIndices_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					LaplacianIndicesBuffer,
					IndicesSource.GetData(),
					NumSmoothingVertices * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Influence 버퍼
				FRDGBufferRef LaplacianInfluencesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumSmoothingVertices),
					*FString::Printf(TEXT("FleshRing_LaplacianInfluences_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					LaplacianInfluencesBuffer,
					InfluenceSource.GetData(),
					NumSmoothingVertices * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// Laplacian 인접 데이터 버퍼
				FRDGBufferRef LaplacianAdjacencyBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), AdjacencySource.Num()),
					*FString::Printf(TEXT("FleshRing_LaplacianAdjacency_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					LaplacianAdjacencyBuffer,
					AdjacencySource.GetData(),
					AdjacencySource.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Laplacian/Taubin 디스패치 파라미터 (UI 설정값 사용)
				FLaplacianDispatchParams LaplacianParams;
				LaplacianParams.NumAffectedVertices = NumSmoothingVertices;
				LaplacianParams.NumTotalVertices = ActualNumVertices;
				LaplacianParams.SmoothingLambda = DispatchData.SmoothingLambda;
				LaplacianParams.NumIterations = DispatchData.SmoothingIterations;
				// Taubin smoothing (수축 방지)
				LaplacianParams.bUseTaubinSmoothing = DispatchData.bUseTaubinSmoothing;
				LaplacianParams.TaubinMu = DispatchData.TaubinMu;
				// 스타킹 레이어 스무딩 제외 활성화 - 분리된 메시에서 크랙 방지
				LaplacianParams.bExcludeStockingFromSmoothing = true;
				// Anchor Mode: 원본 Affected Vertices 고정 (IsAnchorFlags 버퍼 사용)
				LaplacianParams.bAnchorDeformedVertices = DispatchData.bAnchorDeformedVertices;

				// ===== VertexLayerTypes 버퍼 생성 (스타킹 스무딩 제외용) =====
				// [최적화] FullMeshLayerTypes 직접 사용 - 축소→확대 변환 제거
				// 전체 메시 크기 배열이므로 VertexIndex로 직접 조회 가능
				FRDGBufferRef LaplacianLayerTypesBuffer = nullptr;
				if (DispatchData.FullMeshLayerTypes.Num() > 0)
				{
					LaplacianLayerTypesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), DispatchData.FullMeshLayerTypes.Num()),
						*FString::Printf(TEXT("FleshRing_LaplacianLayerTypes_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						LaplacianLayerTypesBuffer,
						DispatchData.FullMeshLayerTypes.GetData(),
						DispatchData.FullMeshLayerTypes.Num() * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// ===== UV Seam Welding: RepresentativeIndices 버퍼 생성 (LaplacianCS용) =====
				const TArray<uint32>& RepresentativeSource = bUseSmoothingRegion
					? DispatchData.SmoothingRegionRepresentativeIndices
					: DispatchData.RepresentativeIndices;

				FRDGBufferRef LaplacianRepresentativeIndicesBuffer = nullptr;
				if (RepresentativeSource.Num() > 0 && RepresentativeSource.Num() == static_cast<int32>(NumSmoothingVertices))
				{
					LaplacianRepresentativeIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSmoothingVertices),
						*FString::Printf(TEXT("FleshRing_LaplacianRepIndices_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						LaplacianRepresentativeIndicesBuffer,
						RepresentativeSource.GetData(),
						NumSmoothingVertices * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// ===== IsAnchor 버퍼 생성 (앵커 모드용) =====
				// 원본 Affected Vertices (Seeds) = 앵커 (스무딩 건너뜀)
				// 확장 영역 = 스무딩 적용
				const TArray<uint32>& IsAnchorSource = bUseSmoothingRegion
					? DispatchData.SmoothingRegionIsAnchor : TArray<uint32>();

				FRDGBufferRef LaplacianIsAnchorBuffer = nullptr;
				if (LaplacianParams.bAnchorDeformedVertices && IsAnchorSource.Num() == static_cast<int32>(NumSmoothingVertices))
				{
					LaplacianIsAnchorBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumSmoothingVertices),
						*FString::Printf(TEXT("FleshRing_LaplacianIsAnchor_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						LaplacianIsAnchorBuffer,
						IsAnchorSource.GetData(),
						NumSmoothingVertices * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// Laplacian MultiPass 디스패치 (in-place smoothing)
				DispatchFleshRingLaplacianCS_MultiPass(
					GraphBuilder,
					LaplacianParams,
					TightenedBindPoseBuffer,
					LaplacianIndicesBuffer,
					LaplacianInfluencesBuffer,
					LaplacianRepresentativeIndicesBuffer,  // UV seam welding용 대표 버텍스 인덱스
					LaplacianAdjacencyBuffer,
					LaplacianLayerTypesBuffer,  // 스타킹 스무딩 제외용
					LaplacianIsAnchorBuffer     // 앵커 모드용 (nullptr이면 비활성화)
				);

			}
		}

		// ===== Layer Penetration Resolution =====
		// 스타킹 레이어가 항상 스킨 레이어 바깥에 위치하도록 보장
		// 단순 ON/OFF 토글: OFF면 전체 디스패치 스킵
		{
			// 상태 변경 추적 (ON↔OFF 토글 감지)
			static bool bLastEnabled = true;  // 기본값 true
			if (bLastEnabled != WorkItem.bEnableLayerPenetrationResolution)
			{
				UE_LOG(LogFleshRingWorker, Warning, TEXT("[LayerPenetration] %s"),
					WorkItem.bEnableLayerPenetrationResolution ? TEXT("ENABLED") : TEXT("DISABLED"));
				bLastEnabled = WorkItem.bEnableLayerPenetrationResolution;
			}
		}

		// ===== LayerPenetrationCS 비활성화 =====
		// 레이어별 Tightness 차등화(50%)로 대체 테스트 중
		// 활성화하려면 아래 조건에서 true로 변경
		constexpr bool bForceDisableLayerPenetration = true;

		if (!WorkItem.bEnableLayerPenetrationResolution || bForceDisableLayerPenetration)
		{
			// OFF: 디스패치 스킵 (아무것도 안 함)
		}
		else if (WorkItem.RingDispatchDataPtr.IsValid() && WorkItem.MeshIndicesPtr.IsValid())
		{
			const TArray<uint32>& MeshIndices = *WorkItem.MeshIndicesPtr;
			const uint32 NumTriangles = MeshIndices.Num() / 3;

			if (NumTriangles > 0)
			{
				// 삼각형 인덱스 버퍼 생성 (모든 Ring이 공유)
				FRDGBufferRef LayerTriIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MeshIndices.Num()),
					TEXT("FleshRing_LayerTriIndices")
				);
				GraphBuilder.QueueBufferUpload(
					LayerTriIndicesBuffer,
					MeshIndices.GetData(),
					MeshIndices.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
				{
					const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

					// 레이어 타입 데이터가 없으면 스킵
					if (DispatchData.LayerTypes.Num() == 0)
					{
						// [디버그] 첫 프레임만
						static TSet<int32> LoggedLayerSkipRings;
						if (!LoggedLayerSkipRings.Contains(RingIdx))
						{
							UE_LOG(LogFleshRingWorker, Warning, TEXT("[LayerPenetration] Ring[%d]: SKIPPED - LayerTypes is EMPTY!"), RingIdx);
							LoggedLayerSkipRings.Add(RingIdx);
						}
						continue;
					}

					// [디버그] 레이어 타입 분포 로그 (첫 프레임만)
					static TSet<int32> LoggedLayerDistribution;
					if (!LoggedLayerDistribution.Contains(RingIdx))
					{
						int32 SkinCount = 0, StockingCount = 0, UnderwearCount = 0, OuterwearCount = 0, UnknownCount = 0;
						for (uint32 LayerType : DispatchData.LayerTypes)
						{
							switch (LayerType)
							{
								case 0: SkinCount++; break;
								case 1: StockingCount++; break;
								case 2: UnderwearCount++; break;
								case 3: OuterwearCount++; break;
								default: UnknownCount++; break;
							}
						}
						UE_LOG(LogFleshRingWorker, Warning,
							TEXT("[LayerPenetration] Ring[%d] LayerTypes: Skin=%d, Stocking=%d, Underwear=%d, Outerwear=%d, Unknown=%d"),
							RingIdx, SkinCount, StockingCount, UnderwearCount, OuterwearCount, UnknownCount);

						// 레이어 분리가 안 되면 경고
						if (SkinCount == 0 || StockingCount == 0)
						{
							UE_LOG(LogFleshRingWorker, Error,
								TEXT("[LayerPenetration] Ring[%d] WARNING: No layer separation possible! Need both Skin AND Stocking."),
								RingIdx);
							UE_LOG(LogFleshRingWorker, Error,
								TEXT("  → Check material names contain keywords: 'skin'/'body' for Skin, 'stocking'/'sock'/'tights' for Stocking"));
							UE_LOG(LogFleshRingWorker, Error,
								TEXT("  → Or configure MaterialLayerMappings in FleshRingAsset"));
						}
						LoggedLayerDistribution.Add(RingIdx);
					}

					// ===== 영역 선택 (통합된 SmoothingRegion 사용) =====
					// - ANY Smoothing ON:  SmoothingRegionIndices 사용
					// - ALL Smoothing OFF: Indices (기본 SDF 볼륨) - Tightness/Bulge만 동작
					const bool bAnySmoothingEnabled =
						DispatchData.bEnableRadialSmoothing ||
						DispatchData.bEnableLaplacianSmoothing ||
						DispatchData.bEnablePBDEdgeConstraint;

					const bool bUseSmoothingRegion =
						bAnySmoothingEnabled &&
						DispatchData.SmoothingRegionIndices.Num() > 0 &&
						DispatchData.FullMeshLayerTypes.Num() > 0;

					const TArray<uint32>& PPIndices = bUseSmoothingRegion
						? DispatchData.SmoothingRegionIndices : DispatchData.Indices;

					const uint32 NumAffected = PPIndices.Num();
					if (NumAffected == 0) continue;

					// 영향받는 버텍스 인덱스 버퍼
					FRDGBufferRef LayerAffectedIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumAffected),
						*FString::Printf(TEXT("FleshRing_LayerAffectedIndices_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						LayerAffectedIndicesBuffer,
						PPIndices.GetData(),
						NumAffected * sizeof(uint32),
						ERDGInitialDataFlags::None
					);

					// [최적화] FullMeshLayerTypes 직접 사용 - 축소→확대 변환 제거
					// 전체 메시 크기 배열이므로 VertexIndex로 직접 조회 가능
					FRDGBufferRef VertexLayerTypesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), DispatchData.FullMeshLayerTypes.Num()),
						*FString::Printf(TEXT("FleshRing_VertexLayerTypes_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						VertexLayerTypesBuffer,
						DispatchData.FullMeshLayerTypes.GetData(),
						DispatchData.FullMeshLayerTypes.Num() * sizeof(uint32),
						ERDGInitialDataFlags::None
					);

					// NOTE: 노멀 버퍼는 이제 사용하지 않음 (방사 방향으로 대체됨)
					// 셰이더는 RingCenter/RingAxis에서 방사 방향을 계산하여 정렬 체크에 사용
					// 함수 시그니처 호환성을 위해 더미 버퍼 생성
					FRDGBufferRef LayerNormalsBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateBufferDesc(sizeof(float), 3),  // 최소 크기 (사용 안 함)
						*FString::Printf(TEXT("FleshRing_LayerNormals_Dummy_Ring%d"), RingIdx)
					);

					// LayerPenetration 디스패치 파라미터
					// v4: 적절한 분리 거리 확보 (너무 작으면 침투 해결 안 됨)
					FLayerPenetrationDispatchParams LayerParams;
					LayerParams.NumAffectedVertices = NumAffected;
					LayerParams.NumTriangles = NumTriangles;
					LayerParams.MinSeparation = 0.02f;   // 0.2mm 최소 분리
					LayerParams.MaxPushDistance = 1.0f;  // 1cm 최대 푸시 per iteration
					LayerParams.RingCenter = FVector3f(DispatchData.Params.RingCenter);
					LayerParams.RingAxis = FVector3f(DispatchData.Params.RingAxis);
					LayerParams.NumIterations = 8;       // 8회 반복 (1cm×8=8cm 최대)
					// 동적 분리 및 푸시 파라미터
					LayerParams.TightnessStrength = DispatchData.Params.TightnessStrength;
					LayerParams.OuterLayerPushRatio = 1.0f;  // 스타킹 100% 바깥으로 (살은 제자리)
					LayerParams.InnerLayerPushRatio = 0.0f;  // 살은 밀지 않음

					// LayerPenetration 디스패치
					DispatchFleshRingLayerPenetrationCS(
						GraphBuilder,
						LayerParams,
						TightenedBindPoseBuffer,
						LayerNormalsBuffer,
						VertexLayerTypesBuffer,
						LayerAffectedIndicesBuffer,
						LayerTriIndicesBuffer
					);

				}
			}
		}

		// ===== SkinSDF Layer Separation (LayerPenetrationCS 이후) =====
		// 스킨 버텍스 기반 implicit surface로 완전한 레이어 분리 보장
		// 스타킹 버텍스가 스킨 안쪽에 있으면 바깥으로 밀어냄
		if (WorkItem.bEnableLayerPenetrationResolution && WorkItem.RingDispatchDataPtr.IsValid())
		{
			for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

				// 스킨/스타킹 버텍스가 모두 있어야 처리
				if (DispatchData.SkinVertexIndices.Num() == 0 || DispatchData.StockingVertexIndices.Num() == 0)
				{
					continue;
				}

				// 스킨 버텍스 인덱스 버퍼
				FRDGBufferRef SkinIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DispatchData.SkinVertexIndices.Num()),
					*FString::Printf(TEXT("FleshRing_SkinIndices_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					SkinIndicesBuffer,
					DispatchData.SkinVertexIndices.GetData(),
					DispatchData.SkinVertexIndices.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// 스킨 노멀 버퍼 (방사 방향)
				FRDGBufferRef SkinNormalsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), DispatchData.SkinVertexNormals.Num()),
					*FString::Printf(TEXT("FleshRing_SkinNormals_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					SkinNormalsBuffer,
					DispatchData.SkinVertexNormals.GetData(),
					DispatchData.SkinVertexNormals.Num() * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// 스타킹 버텍스 인덱스 버퍼
				FRDGBufferRef StockingIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DispatchData.StockingVertexIndices.Num()),
					*FString::Printf(TEXT("FleshRing_StockingIndices_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					StockingIndicesBuffer,
					DispatchData.StockingVertexIndices.GetData(),
					DispatchData.StockingVertexIndices.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// ===== SkinSDF 패스 비활성화 =====
				// 레이어별 Tightness 차등화(50%)로 대체 테스트 중
				// 활성화하려면 bEnableSkinSDFSeparation = true로 변경
				constexpr bool bEnableSkinSDFSeparation = false;

				if (bEnableSkinSDFSeparation)
				{
					// SkinSDF 디스패치 파라미터
					FSkinSDFDispatchParams SkinSDFParams;
					SkinSDFParams.NumStockingVertices = DispatchData.StockingVertexIndices.Num();
					SkinSDFParams.NumSkinVertices = DispatchData.SkinVertexIndices.Num();
					SkinSDFParams.NumTotalVertices = ActualNumVertices;
					SkinSDFParams.MinSeparation = 0.005f;
					SkinSDFParams.TargetSeparation = 0.02f;
					SkinSDFParams.MaxPushDistance = 0.5f;
					SkinSDFParams.MaxPullDistance = 0.0f;
					SkinSDFParams.MaxIterations = 50;
					SkinSDFParams.RingAxis = FVector3f(DispatchData.Params.RingAxis);
					SkinSDFParams.RingCenter = FVector3f(DispatchData.Params.RingCenter);

					DispatchFleshRingSkinSDFCS(
						GraphBuilder,
						SkinSDFParams,
						TightenedBindPoseBuffer,
						SkinIndicesBuffer,
						SkinNormalsBuffer,
						StockingIndicesBuffer
					);

				}
			}
		}

		// ===== NormalRecomputeCS Dispatch (BulgeCS 이후) =====
		// 변형된 위치에 대해 Face Normal 평균으로 노멀 재계산
		if (WorkItem.bEnableNormalRecompute && WorkItem.RingDispatchDataPtr.IsValid() && WorkItem.MeshIndicesPtr.IsValid())
		{
			// 메시 인덱스 버퍼 생성
			const TArray<uint32>& MeshIndices = *WorkItem.MeshIndicesPtr;
			const uint32 NumMeshIndices = MeshIndices.Num();

			if (NumMeshIndices > 0)
			{
				FRDGBufferRef MeshIndexBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumMeshIndices),
					TEXT("FleshRing_MeshIndices")
				);
				GraphBuilder.QueueBufferUpload(
					MeshIndexBuffer,
					MeshIndices.GetData(),
					NumMeshIndices * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// ===== Surface Rotation Method for Normal Recompute =====
				// 표면 회전 방식으로 노멀 재계산
				// 원본 Face Normal → 변형 Face Normal의 회전을 원본 버텍스 노멀에 적용
				// 이 방식은 스무스 셰이딩을 보존합니다

				// SourceTangents SRV 가져오기 (원본 노멀 포함)
				FRHIShaderResourceView* SourceTangentsSRV = LODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetTangentsSRV();
				if (!SourceTangentsSRV)
				{
					UE_LOG(LogFleshRingWorker, Warning, TEXT("[NormalRecompute] SourceTangentsSRV is null, skipping"));
				}

				// 원본 위치 버퍼 생성 (바인드 포즈 - 원본 Face Normal 계산용)
				FRDGBufferRef OriginalPositionsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
					TEXT("FleshRing_OriginalPositions")
				);
				GraphBuilder.QueueBufferUpload(
					OriginalPositionsBuffer,
					WorkItem.SourceDataPtr->GetData(),
					ActualBufferSize * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// 출력 버퍼 생성 (재계산된 노멀)
				// 0으로 초기화 - 영향받지 않는 버텍스는 0 노멀로 남아 SkinningCS에서 폴백
				RecomputedNormalsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
					TEXT("FleshRing_RecomputedNormals")
				);
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(RecomputedNormalsBuffer, PF_R32_FLOAT), 0);

				// 각 Ring별로 NormalRecomputeCS 디스패치
				for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
				{
					const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

					// ===== Normal Recompute 영역 선택 =====
					// 우선순위: SmoothingRegion > Original
					const bool bAnySmoothingEnabled =
						DispatchData.bEnableRadialSmoothing ||
						DispatchData.bEnableLaplacianSmoothing ||
						DispatchData.bEnablePBDEdgeConstraint;

					// SmoothingRegion 사용 가능 여부
					const bool bUseSmoothingRegion = bAnySmoothingEnabled &&
						DispatchData.SmoothingRegionIndices.Num() > 0 &&
						DispatchData.SmoothingRegionAdjacencyOffsets.Num() > 0 &&
						DispatchData.SmoothingRegionAdjacencyTriangles.Num() > 0;

					// 사용할 데이터 소스 선택 (SmoothingRegion > Original)
					const TArray<uint32>& IndicesSource = bUseSmoothingRegion
						? DispatchData.SmoothingRegionIndices : DispatchData.Indices;
					const TArray<uint32>& AdjacencyOffsetsSource = bUseSmoothingRegion
						? DispatchData.SmoothingRegionAdjacencyOffsets : DispatchData.AdjacencyOffsets;
					const TArray<uint32>& AdjacencyTrianglesSource = bUseSmoothingRegion
						? DispatchData.SmoothingRegionAdjacencyTriangles : DispatchData.AdjacencyTriangles;

					// 인접 데이터가 없으면 스킵
					if (AdjacencyOffsetsSource.Num() == 0 || AdjacencyTrianglesSource.Num() == 0)
					{
						continue;
					}

					const uint32 NumAffected = IndicesSource.Num();
					if (NumAffected == 0) continue;

					// 영향받는 버텍스 인덱스 버퍼
					FRDGBufferRef AffectedIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumAffected),
						*FString::Printf(TEXT("FleshRing_NormalAffectedIndices_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						AffectedIndicesBuffer,
						IndicesSource.GetData(),
						NumAffected * sizeof(uint32),
						ERDGInitialDataFlags::None
					);

					// 인접 오프셋 버퍼
					FRDGBufferRef AdjacencyOffsetsBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), AdjacencyOffsetsSource.Num()),
						*FString::Printf(TEXT("FleshRing_AdjacencyOffsets_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						AdjacencyOffsetsBuffer,
						AdjacencyOffsetsSource.GetData(),
						AdjacencyOffsetsSource.Num() * sizeof(uint32),
						ERDGInitialDataFlags::None
					);

					// 인접 삼각형 버퍼
					FRDGBufferRef AdjacencyTrianglesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), AdjacencyTrianglesSource.Num()),
						*FString::Printf(TEXT("FleshRing_AdjacencyTriangles_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						AdjacencyTrianglesBuffer,
						AdjacencyTrianglesSource.GetData(),
						AdjacencyTrianglesSource.Num() * sizeof(uint32),
						ERDGInitialDataFlags::None
					);

					// ========================================
					// UV Sync: Normal Recompute 전 위치 동기화
					// ========================================
					// UV duplicate 버텍스들의 위치를 Representative 기준으로 동기화
					// 이를 통해 UV seam에서 노멀 계산 시 동일한 위치 사용 보장
					{
						// 통합된 SmoothingRegion 데이터 사용
						const TArray<uint32>& RepresentativeSource = bUseSmoothingRegion
							? DispatchData.SmoothingRegionRepresentativeIndices
							: DispatchData.RepresentativeIndices;

						const bool bHasUVDuplicates = bUseSmoothingRegion
							? DispatchData.bSmoothingRegionHasUVDuplicates
							: DispatchData.bHasUVDuplicates;

						// UV duplicate가 없으면 스킵 (최적화)
						if (bHasUVDuplicates && RepresentativeSource.Num() == NumAffected)
						{
							FRDGBufferRef UVSyncRepIndicesBuffer = GraphBuilder.CreateBuffer(
								FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumAffected),
								*FString::Printf(TEXT("FleshRing_UVSyncRepIndices_Ring%d"), RingIdx)
							);
							GraphBuilder.QueueBufferUpload(
								UVSyncRepIndicesBuffer,
								RepresentativeSource.GetData(),
								NumAffected * sizeof(uint32),
								ERDGInitialDataFlags::None
							);

							FUVSyncDispatchParams UVSyncParams(NumAffected);
							DispatchFleshRingUVSyncCS(
								GraphBuilder,
								UVSyncParams,
								TightenedBindPoseBuffer,
								AffectedIndicesBuffer,
								UVSyncRepIndicesBuffer
							);
						}
					}

					// NormalRecomputeCS 디스패치
					FNormalRecomputeDispatchParams NormalParams(NumAffected, ActualNumVertices, WorkItem.NormalRecomputeMode);
					NormalParams.FalloffType = DispatchData.NormalBlendFalloffType;

					// ===== Hop-based 블렌딩 설정 =====
					// HopBased 모드에서 경계 버텍스의 노멀을 원본과 블렌딩
					FRDGBufferRef HopDistancesBuffer = nullptr;
					const bool bIsHopBasedMode = (DispatchData.SmoothingExpandMode == ESmoothingVolumeMode::HopBased);

					if (bUseSmoothingRegion && bIsHopBasedMode &&
						DispatchData.SmoothingRegionHopDistances.Num() == NumAffected &&
						DispatchData.MaxSmoothingHops > 0)
					{
						HopDistancesBuffer = GraphBuilder.CreateBuffer(
							FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), NumAffected),
							*FString::Printf(TEXT("FleshRing_HopDistances_Ring%d"), RingIdx)
						);
						GraphBuilder.QueueBufferUpload(
							HopDistancesBuffer,
							DispatchData.SmoothingRegionHopDistances.GetData(),
							NumAffected * sizeof(int32),
							ERDGInitialDataFlags::None
						);

						NormalParams.bEnableHopBlending = WorkItem.bEnableNormalHopBlending;
						NormalParams.MaxHops = DispatchData.MaxSmoothingHops;
					}

					// ===== Displacement-based 블렌딩 설정 =====
					// 버텍스 이동량에 따라 재계산된 노멀과 원본 노멀 블렌딩
					NormalParams.bEnableDisplacementBlending = WorkItem.bEnableDisplacementBlending;
					NormalParams.MaxDisplacement = WorkItem.MaxDisplacementForBlend;

					// ===== UV Seam Welding: RepresentativeIndices 버퍼 (캐싱 적용) =====
					// UV seam에서 split 버텍스들이 동일한 노멀을 가지도록 대표 버텍스의 인접 데이터 사용
					// 정적 데이터이므로 첫 프레임에만 생성 후 재사용
					const TArray<uint32>& NormalRepSource = bUseSmoothingRegion
						? DispatchData.SmoothingRegionRepresentativeIndices
						: DispatchData.RepresentativeIndices;

					const bool bNormalHasUVDuplicates = bUseSmoothingRegion
						? DispatchData.bSmoothingRegionHasUVDuplicates
						: DispatchData.bHasUVDuplicates;

					// 캐시 버퍼 참조 선택 (SmoothingRegion vs Original)
					TRefCountPtr<FRDGPooledBuffer>& CachedBuffer = bUseSmoothingRegion
						? DispatchData.CachedSmoothingRegionRepresentativeIndicesBuffer
						: DispatchData.CachedRepresentativeIndicesBuffer;

					FRDGBufferRef NormalRepresentativeIndicesBuffer = nullptr;
					if (bNormalHasUVDuplicates && NormalRepSource.Num() == static_cast<int32>(NumAffected))
					{
						if (!CachedBuffer.IsValid())
						{
							// 첫 프레임: 버퍼 생성 및 업로드
							NormalRepresentativeIndicesBuffer = GraphBuilder.CreateBuffer(
								FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumAffected),
								*FString::Printf(TEXT("FleshRing_NormalRepIndices_Ring%d"), RingIdx)
							);
							GraphBuilder.QueueBufferUpload(
								NormalRepresentativeIndicesBuffer,
								NormalRepSource.GetData(),
								NumAffected * sizeof(uint32),
								ERDGInitialDataFlags::None
							);
							// 풀링 버퍼로 캐싱 (다음 프레임에서 재사용)
							CachedBuffer = GraphBuilder.ConvertToExternalBuffer(NormalRepresentativeIndicesBuffer);
						}
						else
						{
							// 이후 프레임: 캐싱된 버퍼 재사용
							NormalRepresentativeIndicesBuffer = GraphBuilder.RegisterExternalBuffer(
								CachedBuffer,
								*FString::Printf(TEXT("FleshRing_NormalRepIndices_Ring%d"), RingIdx)
							);
						}
						NormalParams.bEnableUVSeamWelding = true;
					}

					DispatchFleshRingNormalRecomputeCS(
						GraphBuilder,
						NormalParams,
						TightenedBindPoseBuffer,       // 변형된 위치
						OriginalPositionsBuffer,       // 원본 위치 (원본 Face Normal 계산용)
						AffectedIndicesBuffer,         // 영향받는 버텍스 인덱스
						AdjacencyOffsetsBuffer,        // 인접 오프셋
						AdjacencyTrianglesBuffer,      // 인접 삼각형
						MeshIndexBuffer,               // 메시 인덱스 버퍼
						SourceTangentsSRV,             // 원본 탄젠트 (원본 스무스 노멀 포함)
						RecomputedNormalsBuffer,       // 출력: 재계산된 노멀
						HopDistancesBuffer,            // 홉 거리 (블렌딩용, 선택적)
						NormalRepresentativeIndicesBuffer  // UV seam welding용 대표 버텍스 인덱스
					);

				}
			}
		}

		// ===== TangentRecomputeCS Dispatch (NormalRecomputeCS 이후) =====
		// 탄젠트 재계산: Gram-Schmidt 정규직교화
		// Note: bEnableNormalRecompute가 false면 RecomputedNormalsBuffer가 null이므로 자동 스킵
		if (WorkItem.bEnableTangentRecompute && RecomputedNormalsBuffer && WorkItem.RingDispatchDataPtr.IsValid())
		{
			// SourceTangents SRV 가져오기
			FRHIShaderResourceView* SourceTangentsSRV = LODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetTangentsSRV();

			if (SourceTangentsSRV)
			{
				// [DEBUG] TangentRecomputeCS valid 로그 (필요시 주석 해제)
				// UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] TangentRecomputeCS: SourceTangentsSRV is valid, proceeding"));

				// 탄젠트 출력 버퍼 생성 (버텍스당 8 float: TangentX.xyzw + TangentZ.xyzw)
				const uint32 TangentBufferSize = ActualNumVertices * 8;
				RecomputedTangentsBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), TangentBufferSize),
					TEXT("FleshRing_RecomputedTangents")
				);
				// 0으로 초기화 - 영향받지 않는 버텍스는 SkinningCS에서 원본 사용
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(RecomputedTangentsBuffer, PF_R32_FLOAT), 0);

				// 각 Ring별로 TangentRecomputeCS 디스패치
				for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
				{
					const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

					// ===== Tangent Recompute 영역 선택 =====
					// 우선순위: SmoothingRegion > Original
					const bool bAnySmoothingEnabled =
						DispatchData.bEnableRadialSmoothing ||
						DispatchData.bEnableLaplacianSmoothing ||
						DispatchData.bEnablePBDEdgeConstraint;

					// SmoothingRegion 사용 가능 여부
					const bool bUseSmoothingRegion = bAnySmoothingEnabled &&
						DispatchData.SmoothingRegionIndices.Num() > 0 &&
						DispatchData.SmoothingRegionAdjacencyOffsets.Num() > 0 &&
						DispatchData.SmoothingRegionAdjacencyTriangles.Num() > 0;

					// 데이터 소스 선택 (SmoothingRegion > Original)
					const TArray<uint32>& IndicesSource = bUseSmoothingRegion
						? DispatchData.SmoothingRegionIndices : DispatchData.Indices;
					const TArray<uint32>& AdjacencyOffsetsSource = bUseSmoothingRegion
						? DispatchData.SmoothingRegionAdjacencyOffsets : DispatchData.AdjacencyOffsets;
					const TArray<uint32>& AdjacencyTrianglesSource = bUseSmoothingRegion
						? DispatchData.SmoothingRegionAdjacencyTriangles : DispatchData.AdjacencyTriangles;

					if (IndicesSource.Num() == 0) continue;

					const uint32 NumAffected = IndicesSource.Num();

					// 영향받는 버텍스 인덱스 버퍼
					FRDGBufferRef TangentAffectedIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumAffected),
						*FString::Printf(TEXT("FleshRing_TangentAffectedIndices_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						TangentAffectedIndicesBuffer,
						IndicesSource.GetData(),
						NumAffected * sizeof(uint32),
						ERDGInitialDataFlags::None
					);

					// TangentRecomputeCS 디스패치 (Gram-Schmidt)
					FTangentRecomputeDispatchParams TangentParams(NumAffected, ActualNumVertices);

					DispatchFleshRingTangentRecomputeCS(
						GraphBuilder,
						TangentParams,
						RecomputedNormalsBuffer,
						SourceTangentsSRV,
						TangentAffectedIndicesBuffer,
						RecomputedTangentsBuffer
					);
				}
			}
			else
			{
				// [DEBUG] TangentRecomputeCS NULL 경고 (필요시 주석 해제)
				// UE_LOG(LogFleshRingWorker, Warning, TEXT("[DEBUG] TangentRecomputeCS: SourceTangentsSRV is NULL! Tangent recomputation skipped."));
			}
		}

		// ===== Debug Point Output Pass (모든 CS 완료 후 최종 변형 위치 기반) =====
		// TightnessCS, BulgeCS에서 출력하면 중간 위치가 출력되므로,
		// 모든 변형 패스(스무딩 포함) 완료 후 여기서 통합 출력
		if (WorkItem.RingDispatchDataPtr.IsValid())
		{
			// Tightness 디버그 포인트 출력 (최종 위치)
			// DebugInfluencesBuffer 필수: GPU에서 계산된 Influence 값을 사용
			if (WorkItem.bOutputDebugPoints && DebugPointBuffer && DebugInfluencesBuffer)
			{
				// DebugPointBuffer와 DebugInfluencesBuffer는 동일한 오프셋 구조
				// (둘 다 NumAffectedVertices 단위로 Ring별 연속 저장)
				uint32 DebugCumulativeOffset = 0;

				for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
				{
					const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];
					if (DispatchData.Params.NumAffectedVertices == 0) continue;

					// 인덱스 버퍼 생성
					FRDGBufferRef DebugIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DispatchData.Indices.Num()),
						*FString::Printf(TEXT("FleshRing_DebugTightnessIndices_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						DebugIndicesBuffer,
						DispatchData.Indices.GetData(),
						DispatchData.Indices.Num() * sizeof(uint32),
						ERDGInitialDataFlags::None
					);

					// 디버그 포인트 출력 패스 디스패치
					// GPU에서 계산된 DebugInfluencesBuffer 사용 (CPU Influences 대신)
					FDebugPointOutputDispatchParams DebugParams;
					DebugParams.NumVertices = DispatchData.Params.NumAffectedVertices;
					DebugParams.NumTotalVertices = ActualNumVertices;
					DebugParams.RingIndex = DispatchData.OriginalRingIndex;
					DebugParams.BaseOffset = DebugCumulativeOffset;
					DebugParams.InfluenceBaseOffset = DebugCumulativeOffset;  // 동일한 오프셋 사용
					DebugParams.LocalToWorld = WorkItem.LocalToWorldMatrix;

					DispatchFleshRingDebugPointOutputCS(
						GraphBuilder,
						DebugParams,
						TightenedBindPoseBuffer,  // 최종 변형된 위치
						DebugIndicesBuffer,
						DebugInfluencesBuffer,    // GPU에서 계산된 Influence
						DebugPointBuffer
					);

					DebugCumulativeOffset += DebugParams.NumVertices;
				}
			}

			// Bulge 디버그 포인트 출력 (최종 위치)
			if (WorkItem.bOutputDebugBulgePoints && DebugBulgePointBuffer)
			{
				uint32 DebugBulgePointCumulativeOffset = 0;
				for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
				{
					const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];
					if (!DispatchData.bEnableBulge || DispatchData.BulgeIndices.Num() == 0) continue;

					const uint32 NumBulgeVertices = DispatchData.BulgeIndices.Num();

					// 인덱스 버퍼 생성
					FRDGBufferRef DebugBulgeIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumBulgeVertices),
						*FString::Printf(TEXT("FleshRing_DebugBulgeIndices_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						DebugBulgeIndicesBuffer,
						DispatchData.BulgeIndices.GetData(),
						NumBulgeVertices * sizeof(uint32),
						ERDGInitialDataFlags::None
					);

					// Influence 버퍼 생성
					FRDGBufferRef DebugBulgeInfluenceBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumBulgeVertices),
						*FString::Printf(TEXT("FleshRing_DebugBulgeInfluences_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						DebugBulgeInfluenceBuffer,
						DispatchData.BulgeInfluences.GetData(),
						NumBulgeVertices * sizeof(float),
						ERDGInitialDataFlags::None
					);

					// 디버그 포인트 출력 패스 디스패치
					// Bulge Influence는 CPU에서 계산되어 전달되므로 그대로 사용
					FDebugPointOutputDispatchParams DebugParams;
					DebugParams.NumVertices = NumBulgeVertices;
					DebugParams.NumTotalVertices = ActualNumVertices;
					DebugParams.RingIndex = DispatchData.OriginalRingIndex;
					DebugParams.BaseOffset = DebugBulgePointCumulativeOffset;
					DebugParams.InfluenceBaseOffset = 0;  // CPU 업로드 버퍼는 Ring별로 분리되어 있음
					DebugParams.LocalToWorld = WorkItem.LocalToWorldMatrix;

					DispatchFleshRingDebugPointOutputCS(
						GraphBuilder,
						DebugParams,
						TightenedBindPoseBuffer,  // 최종 변형된 위치
						DebugBulgeIndicesBuffer,
						DebugBulgeInfluenceBuffer,
						DebugBulgePointBuffer
					);

					DebugBulgePointCumulativeOffset += NumBulgeVertices;
				}
			}
		}

		// 영구 버퍼로 변환하여 캐싱
		if (WorkItem.CachedBufferSharedPtr.IsValid())
		{
			*WorkItem.CachedBufferSharedPtr = GraphBuilder.ConvertToExternalBuffer(TightenedBindPoseBuffer);
		}

		// 재계산된 노멀 버퍼 캐싱 (SkinningCS에서 사용)
		if (WorkItem.CachedNormalsBufferSharedPtr.IsValid())
		{
			if (RecomputedNormalsBuffer)
			{
				*WorkItem.CachedNormalsBufferSharedPtr = GraphBuilder.ConvertToExternalBuffer(RecomputedNormalsBuffer);
			}
			else if (WorkItem.CachedNormalsBufferSharedPtr->IsValid())
			{
				// bEnableNormalRecompute가 false면 기존 캐시 클리어
				WorkItem.CachedNormalsBufferSharedPtr->SafeRelease();
			}
		}

		// 재계산된 탄젠트 버퍼 캐싱 (Gram-Schmidt 정규직교화 결과)
		if (WorkItem.CachedTangentsBufferSharedPtr.IsValid())
		{
			if (RecomputedTangentsBuffer)
			{
				*WorkItem.CachedTangentsBufferSharedPtr = GraphBuilder.ConvertToExternalBuffer(RecomputedTangentsBuffer);
			}
			else if (WorkItem.CachedTangentsBufferSharedPtr->IsValid())
			{
				// bEnableTangentRecompute가 false면 기존 캐시 클리어
				WorkItem.CachedTangentsBufferSharedPtr->SafeRelease();
			}
		}

		// 디버그 Influence 버퍼 캐싱 (DrawDebugPoint에서 GPU 값 시각화용)
		if (WorkItem.CachedDebugInfluencesBufferSharedPtr.IsValid() && DebugInfluencesBuffer)
		{
			TRefCountPtr<FRDGPooledBuffer> ExternalDebugBuffer = GraphBuilder.ConvertToExternalBuffer(DebugInfluencesBuffer);
			*WorkItem.CachedDebugInfluencesBufferSharedPtr = ExternalDebugBuffer;

			// ===== GPU Readback 예약 =====
			// 외부 버퍼로 변환 후 FRHIGPUBufferReadback으로 비동기 Readback
			if (WorkItem.DebugInfluenceReadbackResultPtr.IsValid() &&
				WorkItem.bDebugInfluenceReadbackComplete.IsValid() &&
				WorkItem.DebugInfluenceCount > 0 &&
				ExternalDebugBuffer.IsValid())
			{
				// Readback 시작 전 완료 플래그 초기화
				WorkItem.bDebugInfluenceReadbackComplete->store(false);

				// Readback 완료 처리를 위한 캡처 데이터
				TSharedPtr<TArray<float>> ResultPtr = WorkItem.DebugInfluenceReadbackResultPtr;
				TSharedPtr<std::atomic<bool>> CompleteFlag = WorkItem.bDebugInfluenceReadbackComplete;
				uint32 Count = WorkItem.DebugInfluenceCount;
				TRefCountPtr<FRDGPooledBuffer> CapturedBuffer = ExternalDebugBuffer;

				// RDG 실행 후 렌더 스레드에서 Readback 수행
				ENQUEUE_RENDER_COMMAND(FleshRingDebugInfluenceReadback)(
					[ResultPtr, CompleteFlag, Count, CapturedBuffer](FRHICommandListImmediate& RHICmdList)
					{
						if (!CapturedBuffer.IsValid() || !CapturedBuffer->GetRHI())
						{
							UE_LOG(LogFleshRingWorker, Warning, TEXT("FleshRing: Readback 버퍼가 유효하지 않음"));
							return;
						}

						FRHIBuffer* SrcBuffer = CapturedBuffer->GetRHI();
						const uint32 BufferSize = Count * sizeof(float);

						// FRHIGPUBufferReadback을 사용한 비동기 Readback
						FRHIGPUBufferReadback* Readback = new FRHIGPUBufferReadback(TEXT("FleshRing_DebugInfluenceReadback"));
						Readback->EnqueueCopy(RHICmdList, SrcBuffer, BufferSize);

						// GPU 동기화 대기 후 데이터 읽기
						RHICmdList.BlockUntilGPUIdle();

						if (Readback->IsReady())
						{
							const float* SrcData = static_cast<const float*>(Readback->Lock(BufferSize));
							if (SrcData && ResultPtr.IsValid())
							{
								ResultPtr->SetNum(Count);
								FMemory::Memcpy(ResultPtr->GetData(), SrcData, BufferSize);
							}
							Readback->Unlock();

							// 완료 플래그 설정
							if (CompleteFlag.IsValid())
							{
								CompleteFlag->store(true);
							}
						}

						delete Readback;
					});
			}
		}

		// 디버그 포인트 버퍼 캐싱
		if (WorkItem.CachedDebugPointBufferSharedPtr.IsValid() && DebugPointBuffer)
		{
			*WorkItem.CachedDebugPointBufferSharedPtr = GraphBuilder.ConvertToExternalBuffer(DebugPointBuffer);
		}

		// Bulge 디버그 포인트 버퍼 캐싱
		if (WorkItem.CachedDebugBulgePointBufferSharedPtr.IsValid() && DebugBulgePointBuffer)
		{
			*WorkItem.CachedDebugBulgePointBufferSharedPtr = GraphBuilder.ConvertToExternalBuffer(DebugBulgePointBuffer);
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

		// 캐싱된 노멀 버퍼 복구 (bEnableNormalRecompute가 켜져있을 때만)
		if (WorkItem.bEnableNormalRecompute &&
			WorkItem.CachedNormalsBufferSharedPtr.IsValid() && WorkItem.CachedNormalsBufferSharedPtr->IsValid())
		{
			RecomputedNormalsBuffer = GraphBuilder.RegisterExternalBuffer(*WorkItem.CachedNormalsBufferSharedPtr);
		}

		// 캐싱된 탄젠트 버퍼 복구 (bEnableTangentRecompute가 켜져있을 때만)
		if (WorkItem.bEnableTangentRecompute &&
			WorkItem.CachedTangentsBufferSharedPtr.IsValid() && WorkItem.CachedTangentsBufferSharedPtr->IsValid())
		{
			RecomputedTangentsBuffer = GraphBuilder.RegisterExternalBuffer(*WorkItem.CachedTangentsBufferSharedPtr);
		}

		// 캐싱 모드에서 DebugPointBuffer 복구
		if (WorkItem.CachedDebugPointBufferSharedPtr.IsValid() && WorkItem.CachedDebugPointBufferSharedPtr->IsValid())
		{
			DebugPointBuffer = GraphBuilder.RegisterExternalBuffer(*WorkItem.CachedDebugPointBufferSharedPtr);
		}

		// 캐싱 모드에서 DebugBulgePointBuffer 복구
		if (WorkItem.CachedDebugBulgePointBufferSharedPtr.IsValid() && WorkItem.CachedDebugBulgePointBufferSharedPtr->IsValid())
		{
			DebugBulgePointBuffer = GraphBuilder.RegisterExternalBuffer(*WorkItem.CachedDebugBulgePointBufferSharedPtr);
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
				OutputTangentBuffer, BoneMatricesSRV, nullptr, InputWeightStreamSRV,
				RecomputedNormalsBuffer, RecomputedTangentsBuffer);
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
