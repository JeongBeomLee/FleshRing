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

	// TangentRecomputeCS 출력 버퍼 (SkinningCS에서 사용)
	FRDGBufferRef RecomputedTangentsBuffer = nullptr;

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

					// [DEBUG] 역행렬 검증: M × M^-1 = Identity 확인
					//{
					//	FMatrix VerifyIdentity = ForwardMatrix * InverseMatrix;
					//	float MaxError = 0.0f;
					//	for (int32 Row = 0; Row < 4; ++Row)
					//	{
					//		for (int32 Col = 0; Col < 4; ++Col)
					//		{
					//			float Expected = (Row == Col) ? 1.0f : 0.0f;
					//			float Actual = VerifyIdentity.M[Row][Col];
					//			MaxError = FMath::Max(MaxError, FMath::Abs(Actual - Expected));
					//		}
					//	}
					//
					//	bool bLoggedMatrixVerify = false;
					//	if (!bLoggedMatrixVerify)
					//	{
					//		UE_LOG(LogFleshRingWorker, Log, TEXT("[MATRIX] Inverse Verification - MaxError: %e"), MaxError);
					//		UE_LOG(LogFleshRingWorker, Log, TEXT("[MATRIX] ForwardMatrix (SDFLocalToComponent):"));
					//		UE_LOG(LogFleshRingWorker, Log, TEXT("[MATRIX]   [%.4f, %.4f, %.4f, %.4f]"), ForwardMatrix.M[0][0], ForwardMatrix.M[0][1], ForwardMatrix.M[0][2], ForwardMatrix.M[0][3]);
					//		UE_LOG(LogFleshRingWorker, Log, TEXT("[MATRIX]   [%.4f, %.4f, %.4f, %.4f]"), ForwardMatrix.M[1][0], ForwardMatrix.M[1][1], ForwardMatrix.M[1][2], ForwardMatrix.M[1][3]);
					//		UE_LOG(LogFleshRingWorker, Log, TEXT("[MATRIX]   [%.4f, %.4f, %.4f, %.4f]"), ForwardMatrix.M[2][0], ForwardMatrix.M[2][1], ForwardMatrix.M[2][2], ForwardMatrix.M[2][3]);
					//		UE_LOG(LogFleshRingWorker, Log, TEXT("[MATRIX]   [%.4f, %.4f, %.4f, %.4f]"), ForwardMatrix.M[3][0], ForwardMatrix.M[3][1], ForwardMatrix.M[3][2], ForwardMatrix.M[3][3]);
					//		UE_LOG(LogFleshRingWorker, Log, TEXT("[MATRIX] InverseMatrix (ComponentToSDFLocal):"));
					//		UE_LOG(LogFleshRingWorker, Log, TEXT("[MATRIX]   [%.4f, %.4f, %.4f, %.4f]"), InverseMatrix.M[0][0], InverseMatrix.M[0][1], InverseMatrix.M[0][2], InverseMatrix.M[0][3]);
					//		UE_LOG(LogFleshRingWorker, Log, TEXT("[MATRIX]   [%.4f, %.4f, %.4f, %.4f]"), InverseMatrix.M[1][0], InverseMatrix.M[1][1], InverseMatrix.M[1][2], InverseMatrix.M[1][3]);
					//		UE_LOG(LogFleshRingWorker, Log, TEXT("[MATRIX]   [%.4f, %.4f, %.4f, %.4f]"), InverseMatrix.M[2][0], InverseMatrix.M[2][1], InverseMatrix.M[2][2], InverseMatrix.M[2][3]);
					//		UE_LOG(LogFleshRingWorker, Log, TEXT("[MATRIX]   [%.4f, %.4f, %.4f, %.4f]"), InverseMatrix.M[3][0], InverseMatrix.M[3][1], InverseMatrix.M[3][2], InverseMatrix.M[3][3]);
					//
					//		if (MaxError > 1e-5f)
					//		{
					//			UE_LOG(LogFleshRingWorker, Warning, TEXT("[MATRIX] WARNING: Large inverse error detected!"));
					//		}
					//		bLoggedMatrixVerify = true;
					//	}
					//}

					// Ring Center/Axis (SDF Local Space) - 바운드 확장 시에도 정확한 위치 전달
					Params.SDFLocalRingCenter = DispatchData.SDFLocalRingCenter;
					Params.SDFLocalRingAxis = DispatchData.SDFLocalRingAxis;

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
					RepresentativeIndicesBuffer,  // UV seam welding용 대표 버텍스 인덱스
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
					// TODO: Tight에서 행렬 변환 잘 되어서 들어가게 되면 Bulge도 밑 코드로 교체!
					// FTransform::Inverse() 대신 FMatrix::Inverse() 사용 (비균일 스케일+회전 시 Shear 보존)
					//FMatrix ForwardMatrix = DispatchData.SDFLocalToComponent.ToMatrixWithScale();
					//FMatrix InverseMatrix = ForwardMatrix.Inverse();
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

				// SDF 모드 vs Manual 모드 분기
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
					// Manual 모드: Component Space 파라미터 설정
					BulgeParams.RingCenter = DispatchData.Params.RingCenter;
					BulgeParams.RingAxis = DispatchData.Params.RingAxis;
					BulgeParams.RingHeight = DispatchData.Params.RingHeight;
				}

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

				// [조건부 로그] 첫 프레임만
				static TSet<int32> LoggedBoneRatioRings;
				if (!LoggedBoneRatioRings.Contains(RingIdx))
				{
					UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] BoneRatioCS Dispatch Ring[%d]: AffectedVerts=%d, Slices=%d"),
						RingIdx, NumAffected, DispatchData.SlicePackedData.Num() / 33);
					LoggedBoneRatioRings.Add(RingIdx);
				}
			}
		}

		// ===== LaplacianCS Dispatch (BoneRatioCS 이후, NormalRecomputeCS 이전) =====
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

				// ===== 스무딩 영역 선택 =====
				// [설계] SmoothingVolumeMode에 따라 확장 방식 결정:
				//   - HopBased:     ExtendedSmoothingIndices (토폴로지 확장)
				//   - BoundsExpand: PostProcessingIndices (Z 기반 BoundsZTop/Bottom 확장)
				const bool bUseExtendedRegion = DispatchData.bUseHopBasedSmoothing &&
					DispatchData.ExtendedSmoothingIndices.Num() > 0 &&
					DispatchData.ExtendedInfluences.Num() == DispatchData.ExtendedSmoothingIndices.Num() &&
					DispatchData.ExtendedLaplacianAdjacency.Num() > 0;

				const bool bUsePostProcessingRegion = !bUseExtendedRegion &&
					DispatchData.PostProcessingIndices.Num() > 0 &&
					DispatchData.PostProcessingInfluences.Num() == DispatchData.PostProcessingIndices.Num() &&
					DispatchData.PostProcessingLaplacianAdjacencyData.Num() > 0;

				// 사용할 데이터 소스 선택 (우선순위: Extended(Hop) > PostProcessing(Z) > Original)
				const TArray<uint32>& IndicesSource = bUseExtendedRegion
					? DispatchData.ExtendedSmoothingIndices
					: (bUsePostProcessingRegion ? DispatchData.PostProcessingIndices : DispatchData.Indices);
				const TArray<float>& InfluenceSource = bUseExtendedRegion
					? DispatchData.ExtendedInfluences
					: (bUsePostProcessingRegion ? DispatchData.PostProcessingInfluences : DispatchData.Influences);
				const TArray<uint32>& AdjacencySource = bUseExtendedRegion
					? DispatchData.ExtendedLaplacianAdjacency
					: (bUsePostProcessingRegion ? DispatchData.PostProcessingLaplacianAdjacencyData : DispatchData.LaplacianAdjacencyData);

				// 인접 데이터가 없으면 스킵
				if (AdjacencySource.Num() == 0)
				{
					continue;
				}

				const uint32 NumSmoothingVertices = IndicesSource.Num();
				if (NumSmoothingVertices == 0) continue;

				// [DEBUG] 영역 사용 로그
				static TSet<int32> LoggedRegionStatus;
				if (!LoggedRegionStatus.Contains(RingIdx))
				{
					if (bUsePostProcessingRegion)
					{
						UE_LOG(LogFleshRingWorker, Log,
							TEXT("[DEBUG] Ring[%d] LaplacianCS: POSTPROCESSING region (Z-extended, %d vertices, %d original)"),
							RingIdx, NumSmoothingVertices, DispatchData.Indices.Num());
					}
					else if (bUseExtendedRegion)
					{
						UE_LOG(LogFleshRingWorker, Log,
							TEXT("[DEBUG] Ring[%d] LaplacianCS: HOP-EXTENDED region (%d vertices, %d original seeds)"),
							RingIdx, NumSmoothingVertices, DispatchData.Indices.Num());
					}
					else
					{
						UE_LOG(LogFleshRingWorker, Log,
							TEXT("[DEBUG] Ring[%d] LaplacianCS: ORIGINAL region (%d vertices)"),
							RingIdx, NumSmoothingVertices);
					}
					LoggedRegionStatus.Add(RingIdx);
				}

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
				// 영역에 따라 적절한 RepresentativeIndices 선택
				const TArray<uint32>& RepresentativeSource = bUsePostProcessingRegion
					? DispatchData.PostProcessingRepresentativeIndices
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
				const TArray<uint32>& IsAnchorSource = bUseExtendedRegion
					? DispatchData.ExtendedIsAnchor
					: (bUsePostProcessingRegion ? DispatchData.PostProcessingIsAnchor : TArray<uint32>());

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

				// [조건부 로그] 첫 프레임만
				static TSet<int32> LoggedLaplacianRings;
				if (!LoggedLaplacianRings.Contains(RingIdx))
				{
					const TCHAR* RegionMode = bUseExtendedRegion ? TEXT("EXTENDED") : TEXT("ORIGINAL");
					if (LaplacianParams.bUseTaubinSmoothing)
					{
						UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] TaubinCS Ring[%d]: %s region, %d verts, Lambda=%.2f, Mu=%.2f, Iter=%d"),
							RingIdx, RegionMode, NumSmoothingVertices, LaplacianParams.SmoothingLambda, LaplacianParams.TaubinMu, LaplacianParams.NumIterations);
					}
					else
					{
						UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] LaplacianCS Ring[%d]: %s region, %d verts, Lambda=%.2f, Iter=%d"),
							RingIdx, RegionMode, NumSmoothingVertices, LaplacianParams.SmoothingLambda, LaplacianParams.NumIterations);
					}
					LoggedLaplacianRings.Add(RingIdx);
				}
			}
		}

		// ===== PBD Edge Constraint (LaplacianCS 이후, LayerPenetrationCS 이전) =====
		// 변형량이 큰 버텍스를 앵커로 하여 에지 제약으로 변형을 전파
		// "역 PBD": 고정점이 아닌, 변형된 점을 기준으로 주변으로 퍼져나감
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

				// ===== PBD 영역 선택 (LaplacianCS와 동일한 범위 사용) =====
				// Note: Hop 기반 확장은 PBD 인접 데이터가 없으므로, PostProcessing만 지원
				const bool bUsePostProcessingRegion =
					DispatchData.PostProcessingIndices.Num() > 0 &&
					DispatchData.PostProcessingInfluences.Num() == DispatchData.PostProcessingIndices.Num() &&
					DispatchData.PostProcessingPBDAdjacencyWithRestLengths.Num() > 0;

				// 사용할 데이터 소스 선택
				const TArray<uint32>& IndicesSource = bUsePostProcessingRegion
					? DispatchData.PostProcessingIndices : DispatchData.Indices;
				const TArray<float>& InfluenceSource = bUsePostProcessingRegion
					? DispatchData.PostProcessingInfluences : DispatchData.Influences;
				const TArray<uint32>& AdjacencySource = bUsePostProcessingRegion
					? DispatchData.PostProcessingPBDAdjacencyWithRestLengths : DispatchData.PBDAdjacencyWithRestLengths;

				// 인접 데이터가 없으면 스킵
				if (AdjacencySource.Num() == 0)
				{
					continue;
				}

				const uint32 NumAffected = IndicesSource.Num();
				if (NumAffected == 0) continue;

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

				// Influence 버퍼
				FRDGBufferRef PBDInfluencesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumAffected),
					*FString::Printf(TEXT("FleshRing_PBDInfluences_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					PBDInfluencesBuffer,
					InfluenceSource.GetData(),
					NumAffected * sizeof(float),
					ERDGInitialDataFlags::None
				);

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

				// Full Influence Map 버퍼
				FRDGBufferRef FullInfluenceMapBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateStructuredDesc(sizeof(float), DispatchData.FullInfluenceMap.Num()),
					*FString::Printf(TEXT("FleshRing_FullInfluenceMap_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					FullInfluenceMapBuffer,
					DispatchData.FullInfluenceMap.GetData(),
					DispatchData.FullInfluenceMap.Num() * sizeof(float),
					ERDGInitialDataFlags::None
				);

				// ===== UV Seam Welding: RepresentativeIndices 버퍼 생성 (PBD용) =====
				// 영역에 따라 적절한 RepresentativeIndices 선택
				const TArray<uint32>& PBDRepresentativeSource = bUsePostProcessingRegion
					? DispatchData.PostProcessingRepresentativeIndices
					: DispatchData.RepresentativeIndices;

				FRDGBufferRef PBDRepresentativeIndicesBuffer = nullptr;
				if (PBDRepresentativeSource.Num() > 0 && PBDRepresentativeSource.Num() == static_cast<int32>(NumAffected))
				{
					PBDRepresentativeIndicesBuffer = GraphBuilder.CreateBuffer(
						FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), NumAffected),
						*FString::Printf(TEXT("FleshRing_PBDRepIndices_Ring%d"), RingIdx)
					);
					GraphBuilder.QueueBufferUpload(
						PBDRepresentativeIndicesBuffer,
						PBDRepresentativeSource.GetData(),
						NumAffected * sizeof(uint32),
						ERDGInitialDataFlags::None
					);
				}

				// ===== FullDeformAmountMap 버퍼 (bUseDeformAmountWeight용) =====
				// 현재는 nullptr로 전달 (기존 Influence 방식 사용)
				// TODO: DispatchData.FullDeformAmountMap 추가 후 활성화
				FRDGBufferRef FullDeformAmountMapBuffer = nullptr;
				FRDGBufferRef PBDDeformAmountsBuffer = nullptr;

				// PBD 디스패치 파라미터
				FPBDEdgeDispatchParams PBDParams;
				PBDParams.NumAffectedVertices = NumAffected;
				PBDParams.NumTotalVertices = ActualNumVertices;
				PBDParams.Stiffness = DispatchData.PBDStiffness;
				PBDParams.NumIterations = DispatchData.PBDIterations;
				// BoundsScale은 기본값(1.5f) 사용
				PBDParams.bUseDeformAmountWeight = DispatchData.bPBDUseDeformAmountWeight;

				// PBD Edge Constraint 디스패치 (in-place, ping-pong 내부 처리)
				DispatchFleshRingPBDEdgeCS_MultiPass(
					GraphBuilder,
					PBDParams,
					TightenedBindPoseBuffer,
					PBDIndicesBuffer,
					PBDRepresentativeIndicesBuffer,  // UV seam welding용 대표 버텍스 인덱스
					PBDInfluencesBuffer,
					PBDDeformAmountsBuffer,  // 현재 nullptr (기존 Influence 방식)
					PBDAdjacencyBuffer,
					FullInfluenceMapBuffer,
					FullDeformAmountMapBuffer  // 현재 nullptr
				);

				// [조건부 로그] 첫 프레임만
				static TSet<int32> LoggedPBDRings;
				if (!LoggedPBDRings.Contains(RingIdx))
				{
					UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] PBDEdgeCS Ring[%d]: %s region (%d vertices, %d original), Stiffness=%.2f, Iterations=%d"),
						RingIdx,
						bUsePostProcessingRegion ? TEXT("POSTPROCESSING") : TEXT("ORIGINAL"),
						NumAffected, DispatchData.Indices.Num(),
						PBDParams.Stiffness, PBDParams.NumIterations);
					LoggedPBDRings.Add(RingIdx);
				}
			}
		}

		// ===== Self-Collision Detection & Resolution (비활성화됨) =====
		// NOTE: O(n²) 복잡도로 인해 비활성화
		// 대신 Layer Penetration Resolution (LayerPenetrationCS)이
		// 레이어 기반으로 더 효율적으로 스타킹-스킨 분리를 처리함
		//
		// 필요시 FleshRingAsset에 bEnableSelfCollision 플래그 추가하여 제어 가능
#if 0  // Self-Collision Detection 비활성화
		if (WorkItem.RingDispatchDataPtr.IsValid())
		{
			for (int32 RingIdx = 0; RingIdx < WorkItem.RingDispatchDataPtr->Num(); ++RingIdx)
			{
				const FFleshRingWorkItem::FRingDispatchData& DispatchData = (*WorkItem.RingDispatchDataPtr)[RingIdx];

				// CollisionTriangleIndices가 없거나 너무 적으면 스킵
				const uint32 NumCollisionTriangles = DispatchData.CollisionTriangleIndices.Num() / 3;
				if (NumCollisionTriangles < 2)
				{
					continue;
				}

				// 삼각형 인덱스 버퍼 생성
				FRDGBufferRef CollisionTriIndicesBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), DispatchData.CollisionTriangleIndices.Num()),
					*FString::Printf(TEXT("FleshRing_CollisionTriIndices_Ring%d"), RingIdx)
				);
				GraphBuilder.QueueBufferUpload(
					CollisionTriIndicesBuffer,
					DispatchData.CollisionTriangleIndices.GetData(),
					DispatchData.CollisionTriangleIndices.Num() * sizeof(uint32),
					ERDGInitialDataFlags::None
				);

				// Collision dispatch 파라미터
				FCollisionDispatchParams CollisionParams;
				CollisionParams.NumTriangles = NumCollisionTriangles;
				CollisionParams.NumTotalVertices = ActualNumVertices;
				CollisionParams.MaxCollisionPairs = FMath::Min(NumCollisionTriangles * 10, 1024u);  // 예상 충돌 수 제한
				CollisionParams.ResolutionStrength = 1.0f;
				CollisionParams.NumIterations = 1;

				// Collision dispatch
				DispatchFleshRingCollisionCS(
					GraphBuilder,
					CollisionParams,
					TightenedBindPoseBuffer,
					CollisionTriIndicesBuffer
				);

				// [조건부 로그] 첫 프레임만
				static TSet<int32> LoggedCollisionRings;
				if (!LoggedCollisionRings.Contains(RingIdx))
				{
					UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] CollisionCS Dispatch Ring[%d]: %d triangles, MaxPairs=%d"),
						RingIdx, NumCollisionTriangles, CollisionParams.MaxCollisionPairs);
					LoggedCollisionRings.Add(RingIdx);
				}
			}
		}
#endif

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

					// ===== 영역 선택 =====
					// - ANY Smoothing ON:  PostProcessingIndices (Z 확장) 또는 ExtendedSmoothingIndices (Hop 기반)
					// - ALL Smoothing OFF: Indices (기본 SDF 볼륨) - Tightness/Bulge만 동작
					// Note: ExtendedSmoothingIndices 사용 시 LayerTypes 호환성 체크 필요
					const bool bAnySmoothingEnabled =
						DispatchData.bEnableRadialSmoothing ||
						DispatchData.bEnableLaplacianSmoothing ||
						DispatchData.bEnablePBDEdgeConstraint;

					const bool bUseExtendedRegion =
						bAnySmoothingEnabled &&
						DispatchData.bUseHopBasedSmoothing &&
						DispatchData.ExtendedSmoothingIndices.Num() > 0 &&
						DispatchData.LayerTypes.Num() >= DispatchData.ExtendedSmoothingIndices.Num();
					const bool bUsePostProcessingRegion =
						bAnySmoothingEnabled &&
						!bUseExtendedRegion &&
						DispatchData.PostProcessingIndices.Num() > 0 &&
						DispatchData.FullMeshLayerTypes.Num() > 0;

					const TArray<uint32>& PPIndices = bUseExtendedRegion
						? DispatchData.ExtendedSmoothingIndices
						: (bUsePostProcessingRegion ? DispatchData.PostProcessingIndices : DispatchData.Indices);

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

					// [조건부 로그] 첫 프레임만
					static TSet<int32> LoggedLayerPenetrationRings;
					if (!LoggedLayerPenetrationRings.Contains(RingIdx))
					{
						const TCHAR* RegionMode = bUseExtendedRegion ? TEXT("EXTENDED(Hop)")
							: (bUsePostProcessingRegion ? TEXT("PostProcessing(Z)") : TEXT("Affected(SDF)"));
						UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] LayerPenetrationCS Dispatch Ring[%d]: %s Verts=%d (original=%d), Triangles=%d"),
							RingIdx,
							RegionMode,
							NumAffected,
							DispatchData.Indices.Num(),
							NumTriangles);
						LoggedLayerPenetrationRings.Add(RingIdx);
					}
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

					// [조건부 로그] 첫 프레임만
					static TSet<int32> LoggedSkinSDFRings;
					if (!LoggedSkinSDFRings.Contains(RingIdx))
					{
						UE_LOG(LogFleshRingWorker, Log,
							TEXT("[DEBUG] SkinSDFCS Dispatch Ring[%d]: SkinVerts=%d, StockingVerts=%d, MaxIter=%d"),
							RingIdx, SkinSDFParams.NumSkinVertices, SkinSDFParams.NumStockingVertices, SkinSDFParams.MaxIterations);
						LoggedSkinSDFRings.Add(RingIdx);
					}
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

					// ===== Normal 영역 선택 (ANY 스무딩 활성화 시에만 확장 영역 사용) =====
					// Note: Hop 기반 확장은 Normal 인접 데이터가 없으므로, PostProcessing만 지원
					const bool bAnySmoothingEnabled =
						DispatchData.bEnableRadialSmoothing ||
						DispatchData.bEnableLaplacianSmoothing ||
						DispatchData.bEnablePBDEdgeConstraint;

					const bool bUsePostProcessingRegion =
						bAnySmoothingEnabled &&
						DispatchData.PostProcessingIndices.Num() > 0 &&
						DispatchData.PostProcessingAdjacencyOffsets.Num() > 0 &&
						DispatchData.PostProcessingAdjacencyTriangles.Num() > 0;

					// 사용할 데이터 소스 선택
					const TArray<uint32>& IndicesSource = bUsePostProcessingRegion
						? DispatchData.PostProcessingIndices : DispatchData.Indices;
					const TArray<uint32>& AdjacencyOffsetsSource = bUsePostProcessingRegion
						? DispatchData.PostProcessingAdjacencyOffsets : DispatchData.AdjacencyOffsets;
					const TArray<uint32>& AdjacencyTrianglesSource = bUsePostProcessingRegion
						? DispatchData.PostProcessingAdjacencyTriangles : DispatchData.AdjacencyTriangles;

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

					// NormalRecomputeCS 디스패치 (표면 회전 방식)
					FNormalRecomputeDispatchParams NormalParams(NumAffected, ActualNumVertices);

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
						RecomputedNormalsBuffer        // 출력: 재계산된 노멀
					);

					// [조건부 로그] 첫 프레임만
					static TSet<int32> LoggedNormalRings;
					if (!LoggedNormalRings.Contains(RingIdx))
					{
						UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] NormalRecomputeCS Ring[%d]: %s region (%d vertices, %d original), AdjTriangles=%d (SurfaceRotation)"),
							RingIdx,
							bUsePostProcessingRegion ? TEXT("POSTPROCESSING") : TEXT("ORIGINAL"),
							NumAffected, DispatchData.Indices.Num(),
							AdjacencyTrianglesSource.Num());
						LoggedNormalRings.Add(RingIdx);
					}
				}
			}
		}

		// ===== TangentRecomputeCS Dispatch (NormalRecomputeCS 이후) =====
		// Gram-Schmidt 정규직교화로 탄젠트 재계산
		// Note: bEnableNormalRecompute가 false면 RecomputedNormalsBuffer가 null이므로 자동 스킵
		if (WorkItem.bEnableTangentRecompute && RecomputedNormalsBuffer && WorkItem.RingDispatchDataPtr.IsValid())
		{
			// SourceTangents SRV 가져오기
			FRHIShaderResourceView* SourceTangentsSRV = LODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetTangentsSRV();

			if (SourceTangentsSRV)
			{
				UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] TangentRecomputeCS: SourceTangentsSRV is valid, proceeding"));

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

					// Normal과 동일한 영역 사용 (ANY 스무딩 활성화 시에만 확장 영역)
					const bool bAnySmoothingEnabled =
						DispatchData.bEnableRadialSmoothing ||
						DispatchData.bEnableLaplacianSmoothing ||
						DispatchData.bEnablePBDEdgeConstraint;

					const bool bUsePostProcessingRegion =
						bAnySmoothingEnabled &&
						DispatchData.PostProcessingIndices.Num() > 0 &&
						DispatchData.PostProcessingAdjacencyOffsets.Num() > 0 &&
						DispatchData.PostProcessingAdjacencyTriangles.Num() > 0;

					const TArray<uint32>& IndicesSource = bUsePostProcessingRegion
						? DispatchData.PostProcessingIndices : DispatchData.Indices;

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

					// TangentRecomputeCS 디스패치
					FTangentRecomputeDispatchParams TangentParams(NumAffected, ActualNumVertices);

					DispatchFleshRingTangentRecomputeCS(
						GraphBuilder,
						TangentParams,
						RecomputedNormalsBuffer,
						SourceTangentsSRV,
						TangentAffectedIndicesBuffer,
						RecomputedTangentsBuffer
					);

					// [조건부 로그] 첫 프레임만
					static TSet<int32> LoggedTangentRings;
					if (!LoggedTangentRings.Contains(RingIdx))
					{
						UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] TangentRecomputeCS Ring[%d]: %d vertices"),
							RingIdx, NumAffected);
						LoggedTangentRings.Add(RingIdx);
					}
				}
			}
			else
			{
				UE_LOG(LogFleshRingWorker, Warning, TEXT("[DEBUG] TangentRecomputeCS: SourceTangentsSRV is NULL! Tangent recomputation skipped."));
			}
		}

		// 영구 버퍼로 변환하여 캐싱
		if (WorkItem.CachedBufferSharedPtr.IsValid())
		{
			*WorkItem.CachedBufferSharedPtr = GraphBuilder.ConvertToExternalBuffer(TightenedBindPoseBuffer);
		}

		// 재계산된 노멀 버퍼도 캐싱 (SkinningCS에서 사용)
		if (WorkItem.CachedNormalsBufferSharedPtr.IsValid() && RecomputedNormalsBuffer)
		{
			*WorkItem.CachedNormalsBufferSharedPtr = GraphBuilder.ConvertToExternalBuffer(RecomputedNormalsBuffer);
		}

		// 재계산된 탄젠트 버퍼도 캐싱 (Gram-Schmidt 정규직교화 결과)
		if (WorkItem.CachedTangentsBufferSharedPtr.IsValid() && RecomputedTangentsBuffer)
		{
			*WorkItem.CachedTangentsBufferSharedPtr = GraphBuilder.ConvertToExternalBuffer(RecomputedTangentsBuffer);
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

		// 캐싱된 노멀 버퍼 복구
		if (WorkItem.CachedNormalsBufferSharedPtr.IsValid() && WorkItem.CachedNormalsBufferSharedPtr->IsValid())
		{
			RecomputedNormalsBuffer = GraphBuilder.RegisterExternalBuffer(*WorkItem.CachedNormalsBufferSharedPtr);
		}

		// 캐싱된 탄젠트 버퍼 복구
		if (WorkItem.CachedTangentsBufferSharedPtr.IsValid() && WorkItem.CachedTangentsBufferSharedPtr->IsValid())
		{
			RecomputedTangentsBuffer = GraphBuilder.RegisterExternalBuffer(*WorkItem.CachedTangentsBufferSharedPtr);
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

			// 디버그 로그: RecomputedTangentsBuffer 상태 확인
			static bool bLoggedSkinningFlags = false;
			if (!bLoggedSkinningFlags)
			{
				UE_LOG(LogFleshRingWorker, Log, TEXT("[DEBUG] SkinningCS: RecomputedNormalsBuffer=%s, RecomputedTangentsBuffer=%s"),
					RecomputedNormalsBuffer ? TEXT("VALID") : TEXT("NULL"),
					RecomputedTangentsBuffer ? TEXT("VALID") : TEXT("NULL"));
				bLoggedSkinningFlags = true;
			}

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
