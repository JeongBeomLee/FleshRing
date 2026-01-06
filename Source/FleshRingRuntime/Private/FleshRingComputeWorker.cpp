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

	// NormalRecomputeCS 출력 버퍼 (SkinningCS에서 사용)
	FRDGBufferRef RecomputedNormalsBuffer = nullptr;

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
		// [버그 수정] 각 Ring이 독립된 VolumeAccum 슬롯을 사용하도록 변경
		// 이전: 크기 1 버퍼를 모든 Ring이 공유 → Ring A의 압축량이 Ring B에 영향
		// 수정: Ring 개수만큼 버퍼 생성 → 각 Ring이 자신의 슬롯만 사용
		FRDGBufferRef VolumeAccumBuffer = nullptr;
		const int32 NumRings = WorkItem.RingDispatchDataPtr.IsValid() ? WorkItem.RingDispatchDataPtr->Num() : 0;

		if (WorkItem.bAnyRingHasBulge && NumRings > 0)
		{
			VolumeAccumBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumRings),
				TEXT("FleshRing_VolumeAccum")
			);
			// 0으로 초기화 (Atomic 연산 전)
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(VolumeAccumBuffer, PF_R32_UINT), 0u);
		}

		// TightnessCS 적용
		if (WorkItem.RingDispatchDataPtr.IsValid())
		{
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

					// [조건부 로그] 첫 프레임만 출력
					static bool bLoggedSDFDispatch = false;
					if (!bLoggedSDFDispatch)
					{
						UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] TightnessCS Dispatch: SDF Mode (OBB), Verts=%d, Strength=%.2f"),
							Params.NumAffectedVertices, Params.TightnessStrength);
						bLoggedSDFDispatch = true;
					}
				}
				else
				{
					// [조건부 로그] 첫 프레임만 출력
					static bool bLoggedManualDispatch = false;
					if (!bLoggedManualDispatch)
					{
						UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] TightnessCS Dispatch: Manual Mode, Verts=%d, Strength=%.2f"),
							Params.NumAffectedVertices, Params.TightnessStrength);
						bLoggedManualDispatch = true;
					}
				}

				// Bulge 활성화 시 부피 누적 활성화 (이 Ring 또는 다른 Ring에서 Bulge 사용)
				if (WorkItem.bAnyRingHasBulge && VolumeAccumBuffer)
				{
					Params.bAccumulateVolume = 1;
					Params.FixedPointScale = 1000.0f;  // float → uint 변환 스케일
					Params.RingIndex = RingIdx;       // Ring별 VolumeAccumBuffer 슬롯 지정
				}

				DispatchFleshRingTightnessCS(
					GraphBuilder,
					Params,
					SourceBuffer,
					IndicesBuffer,
					InfluencesBuffer,
					TightenedBindPoseBuffer,
					SDFTextureRDG,
					VolumeAccumBuffer
				);
			}
		}

		// ===== BulgeCS Dispatch (TightnessCS 이후, 각 Ring별로) =====
		if (WorkItem.bAnyRingHasBulge && VolumeAccumBuffer && WorkItem.RingDispatchDataPtr.IsValid())
		{
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
					FMatrix InverseMatrix = DispatchData.SDFLocalToComponent.Inverse().ToMatrixWithScale();
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
				BulgeParams.RingIndex = RingIdx;      // Ring별 VolumeAccumBuffer 슬롯 지정
				BulgeParams.BulgeRadialRatio = DispatchData.BulgeRadialRatio;  // Radial vs Axial 비율

				// 이 Ring의 SDF 파라미터
				BulgeParams.SDFBoundsMin = DispatchData.SDFBoundsMin;
				BulgeParams.SDFBoundsMax = DispatchData.SDFBoundsMax;
				BulgeParams.ComponentToSDFLocal = RingComponentToSDFLocal;

				// [조건부 로그] 각 Ring별 첫 프레임만 출력
				static TSet<int32> LoggedBulgeRings;
				if (!LoggedBulgeRings.Contains(RingIdx))
				{
					UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] BulgeCS Dispatch Ring[%d]: Verts=%d, Strength=%.2f, MaxDist=%.2f, Direction=%d"),
						RingIdx, NumBulgeVertices, BulgeParams.BulgeStrength, BulgeParams.MaxBulgeDistance, BulgeParams.BulgeAxisDirection);
					LoggedBulgeRings.Add(RingIdx);
				}

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

		// ===== NormalRecomputeCS Dispatch (BulgeCS 이후) =====
		// 변형된 위치에 대해 Face Normal 평균으로 노멀 재계산
		if (WorkItem.RingDispatchDataPtr.IsValid() && WorkItem.MeshIndicesPtr.IsValid())
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

				// 원본 노멀 버퍼 (SourceTangents에서 Normal만 추출하여 업로드)
				// SourceTangents 포맷: 버텍스당 Normal(float3) + Tangent(float4) = 7 float
				// 여기서는 원본 Position 데이터에서 Normal을 가져와야 함
				// 실제로는 SourceTangents SRV를 사용해야 하지만, 현재 구조에서는 별도 버퍼 필요
				// 일단 BindPose Normal을 SourceDataPtr에서 사용할 수 없으므로
				// SourceTangentsSRV를 그대로 사용하도록 셰이더를 수정하거나
				// 바인드포즈 노멀을 별도로 전달해야 함

				// TODO: 바인드포즈 노멀 데이터 필요
				// 현재는 기본 구현으로 각 Ring별 노멀 재계산 수행

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

					// 인접 데이터가 없으면 스킵
					if (DispatchData.AdjacencyOffsets.Num() == 0 || DispatchData.AdjacencyTriangles.Num() == 0)
					{
						continue;
					}

					const uint32 NumAffected = DispatchData.Indices.Num();
					if (NumAffected == 0) continue;

					// 영향받는 버텍스 인덱스 버퍼
					FRDGBufferRef AffectedIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumAffected),
						*FString::Printf(TEXT("FleshRing_NormalAffectedIndices_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						AffectedIndicesBuffer,
						DispatchData.Indices.GetData(),
						NumAffected * sizeof(uint32),
						ERDGInitialDataFlags::None
					);

					// 인접 오프셋 버퍼
					FRDGBufferRef AdjacencyOffsetsBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DispatchData.AdjacencyOffsets.Num()),
						*FString::Printf(TEXT("FleshRing_AdjacencyOffsets_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						AdjacencyOffsetsBuffer,
						DispatchData.AdjacencyOffsets.GetData(),
						DispatchData.AdjacencyOffsets.Num() * sizeof(uint32),
						ERDGInitialDataFlags::None
					);

					// 인접 삼각형 버퍼
					FRDGBufferRef AdjacencyTrianglesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), DispatchData.AdjacencyTriangles.Num()),
						*FString::Printf(TEXT("FleshRing_AdjacencyTriangles_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						AdjacencyTrianglesBuffer,
						DispatchData.AdjacencyTriangles.GetData(),
						DispatchData.AdjacencyTriangles.Num() * sizeof(uint32),
						ERDGInitialDataFlags::None
					);

					// 원본 노멀 버퍼 (바인드포즈 노멀 - 0으로 초기화)
					// 0 노멀은 SafeNormalize에서 기본값으로 대체됨
					FRDGBufferRef OriginalNormalsBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
						*FString::Printf(TEXT("FleshRing_OriginalNormals_Ring%d"), RingIdx)
					);
					// RDG 버퍼는 반드시 초기화해야 읽을 수 있음
					AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OriginalNormalsBuffer, PF_R32_FLOAT), 0);

					// NormalRecomputeCS 디스패치
					FNormalRecomputeDispatchParams NormalParams(NumAffected, ActualNumVertices);

					DispatchFleshRingNormalRecomputeCS(
						GraphBuilder,
						NormalParams,
						TightenedBindPoseBuffer,      // 변형된 위치
						AffectedIndicesBuffer,         // 영향받는 버텍스 인덱스
						AdjacencyOffsetsBuffer,        // 인접 오프셋
						AdjacencyTrianglesBuffer,      // 인접 삼각형
						MeshIndexBuffer,               // 메시 인덱스 버퍼
						OriginalNormalsBuffer,         // 원본 노멀 (폴백)
						RecomputedNormalsBuffer        // 출력: 재계산된 노멀
					);

					// [조건부 로그] 첫 프레임만
					static TSet<int32> LoggedNormalRings;
					if (!LoggedNormalRings.Contains(RingIdx))
					{
						UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] NormalRecomputeCS Dispatch Ring[%d]: AffectedVerts=%d, AdjTriangles=%d"),
							RingIdx, NumAffected, DispatchData.AdjacencyTriangles.Num());
						LoggedNormalRings.Add(RingIdx);
					}
				}
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
				OutputTangentBuffer, BoneMatricesSRV, nullptr, InputWeightStreamSRV,
				RecomputedNormalsBuffer);
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
