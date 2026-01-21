// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingDeformerInstance.h"
#include "FleshRingDeformer.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "FleshRingTightnessShader.h"
#include "FleshRingSkinningShader.h"
#include "FleshRingComputeWorker.h"
#include "FleshRingBulgeProviders.h"
#include "FleshRingBulgeTypes.h"
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

void UFleshRingDeformerInstance::BeginDestroy()
{
	// 렌더 스레드의 pending work items 취소
	// PIE 종료 시 MeshObject dangling pointer 크래시 방지
	if (Scene)
	{
		if (FFleshRingComputeWorker* Worker = FFleshRingComputeSystem::Get().GetWorker(Scene))
		{
			Worker->AbortWork(this);
		}
	}
	Scene = nullptr;

	// 렌더 스레드가 현재 작업을 완료할 때까지 대기
	// 이미 큐잉된 작업이 실행 중일 수 있으므로 flush 필요
	FlushRenderingCommands();

	// ★ GPU 버퍼 및 캐시 명시적 해제 (메모리 누수 방지)
	ReleaseResources();

	// ★ DeformerGeometry 명시적 해제
	DeformerGeometry.Reset();

	// LODData 배열 완전 정리 (ReleaseResources에서 이미 ClearAll 호출됨)
	LODData.Empty();

	// ★ 약한 참조들도 명시적 해제
	Deformer.Reset();
	MeshComponent.Reset();
	FleshRingComponent.Reset();

	Super::BeginDestroy();
}

void UFleshRingDeformerInstance::SetupFromDeformer(
	UFleshRingDeformer* InDeformer,
	UMeshComponent* InMeshComponent,
	UFleshRingComponent* InOwnerFleshRingComponent)
{
	Deformer = InDeformer;
	MeshComponent = InMeshComponent;
	Scene = InMeshComponent ? InMeshComponent->GetScene() : nullptr;
	LastLodIndex = INDEX_NONE;

	// NOTE: 명시적으로 전달된 FleshRingComponent 사용 (다중 컴포넌트 환경 지원)
	if (InOwnerFleshRingComponent)
	{
		FleshRingComponent = InOwnerFleshRingComponent;
	}
	else if (AActor* Owner = InMeshComponent->GetOwner())
	{
		// 하위 호환성: 전달되지 않은 경우 기존 방식 (단일 컴포넌트 환경)
		FleshRingComponent = Owner->FindComponentByClass<UFleshRingComponent>();
	}

	// FleshRingComponent가 유효하면 모든 LOD에 대해 AffectedVertices 등록
	if (FleshRingComponent.IsValid())
	{
		USkeletalMeshComponent* SkelMesh = Cast<USkeletalMeshComponent>(InMeshComponent);
		if (SkelMesh)
		{
			// LOD 개수 파악
			USkeletalMesh* Mesh = SkelMesh->GetSkeletalMeshAsset();
			if (Mesh)
			{
				const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
				if (RenderData)
				{
					NumLODs = RenderData->LODRenderData.Num();
					LODData.SetNum(NumLODs);

					// 각 LOD에 대해 AffectedVertices 등록
					// Ring별 InfluenceMode에 따라 Selector가 자동 결정됨 (RegisterAffectedVertices 내부)
					int32 SuccessCount = 0;
					for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
					{
						LODData[LODIndex].bAffectedVerticesRegistered =
							LODData[LODIndex].AffectedVerticesManager.RegisterAffectedVertices(
								FleshRingComponent.Get(), SkelMesh, LODIndex);

						if (LODData[LODIndex].bAffectedVerticesRegistered)
						{
							SuccessCount++;
						}
					}
				}
			}
		}
	}
}

void UFleshRingDeformerInstance::AllocateResources()
{
	// Resources are allocated on-demand in EnqueueWork
}

void UFleshRingDeformerInstance::ReleaseResources()
{
	// 모든 LOD의 캐싱된 리소스 해제
	// ★ CL 320 복원: AffectedVerticesManager와 bAffectedVerticesRegistered는 유지
	//    (Deformer 재사용 시 AffectedVertices 데이터가 필요함)
	for (FLODDeformationData& Data : LODData)
	{
		// TightenedBindPose 버퍼 해제
		if (Data.CachedTightenedBindPoseShared.IsValid())
		{
			Data.CachedTightenedBindPoseShared->SafeRelease();
			Data.CachedTightenedBindPoseShared.Reset();
		}
		Data.bTightenedBindPoseCached = false;
		Data.CachedTightnessVertexCount = 0;

		// 재계산된 노멀 버퍼 해제
		if (Data.CachedNormalsShared.IsValid())
		{
			Data.CachedNormalsShared->SafeRelease();
			Data.CachedNormalsShared.Reset();
		}

		// 재계산된 탄젠트 버퍼 해제
		if (Data.CachedTangentsShared.IsValid())
		{
			Data.CachedTangentsShared->SafeRelease();
			Data.CachedTangentsShared.Reset();
		}

		// 디버그 Influence 버퍼 해제
		if (Data.CachedDebugInfluencesShared.IsValid())
		{
			Data.CachedDebugInfluencesShared->SafeRelease();
			Data.CachedDebugInfluencesShared.Reset();
		}

		// 디버그 포인트 버퍼 해제
		if (Data.CachedDebugPointBufferShared.IsValid())
		{
			Data.CachedDebugPointBufferShared->SafeRelease();
			Data.CachedDebugPointBufferShared.Reset();
		}

		// Bulge 디버그 포인트 버퍼 해제
		if (Data.CachedDebugBulgePointBufferShared.IsValid())
		{
			Data.CachedDebugBulgePointBufferShared->SafeRelease();
			Data.CachedDebugBulgePointBufferShared.Reset();
		}

		// Readback 관련 SharedPtr 해제
		Data.DebugInfluenceReadbackResult.Reset();
		Data.bDebugInfluenceReadbackComplete.Reset();

		// 소스 위치 해제
		Data.CachedSourcePositions.Empty();
		Data.bSourcePositionsCached = false;
	}
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

	const int32 LODIndex = SkinnedMeshComp->GetPredictedLODLevel();

	// LOD 유효성 검사
	if (LODIndex < 0 || LODIndex >= NumLODs)
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

	// 현재 LOD의 데이터 참조
	FLODDeformationData& CurrentLODData = LODData[LODIndex];

	// AffectedVertices가 등록되지 않았으면 Fallback 또는 Passthrough
	const int32 TotalAffectedCount = CurrentLODData.AffectedVerticesManager.GetTotalAffectedCount();
	if (!CurrentLODData.bAffectedVerticesRegistered || TotalAffectedCount == 0)
	{
		// 이전에 변형이 있었는지 확인 (캐시 버퍼 유효성으로 판단)
		const bool bHadPreviousDeformation =
			CurrentLODData.CachedTightenedBindPoseShared.IsValid() &&
			CurrentLODData.CachedTightenedBindPoseShared->IsValid();

		if (bHadPreviousDeformation)
		{
			// ===== Passthrough Mode =====
			// 이전에 변형이 있었는데 AffectedVertices가 0이 됨
			// → 원본 데이터로 SkinningCS를 한 번 실행하여 탄젠트 잔상 제거
			FSkeletalMeshObject* MeshObjectForPassthrough = SkinnedMeshComp->MeshObject;
			if (MeshObjectForPassthrough && !MeshObjectForPassthrough->IsCPUSkinned())
			{
				FFleshRingWorkItem PassthroughWorkItem;
				PassthroughWorkItem.DeformerInstance = this;
				PassthroughWorkItem.MeshObject = MeshObjectForPassthrough;
				PassthroughWorkItem.LODIndex = LODIndex;
				PassthroughWorkItem.bPassthroughMode = true;
				PassthroughWorkItem.FallbackDelegate = InDesc.FallbackDelegate;

				// 버텍스 수 설정
				const FSkeletalMeshRenderData& RenderData = MeshObjectForPassthrough->GetSkeletalMeshRenderData();
				const FSkeletalMeshLODRenderData& LODData_Render = RenderData.LODRenderData[LODIndex];
				PassthroughWorkItem.TotalVertexCount = LODData_Render.GetNumVertices();

				// 원본 소스 포지션 전달 (SkinningCS에서 원본 탄젠트 출력용)
				if (CurrentLODData.CachedSourcePositions.Num() > 0)
				{
					PassthroughWorkItem.SourceDataPtr = MakeShared<TArray<float>>(CurrentLODData.CachedSourcePositions);
				}

				FFleshRingComputeWorker* Worker = FFleshRingComputeSystem::Get().GetWorker(Scene);
				if (Worker)
				{
					Worker->EnqueueWork(MoveTemp(PassthroughWorkItem));
				}
			}

			// 캐시 클리어 (Passthrough 작업 후 다시 실행되지 않도록)
			CurrentLODData.CachedTightenedBindPoseShared.Reset();
			CurrentLODData.bTightenedBindPoseCached = false;
			CurrentLODData.CachedTightnessVertexCount = 0;

			// 노말/탄젠트 캐시도 클리어
			if (CurrentLODData.CachedNormalsShared.IsValid())
			{
				CurrentLODData.CachedNormalsShared->SafeRelease();
				CurrentLODData.CachedNormalsShared.Reset();
			}
			if (CurrentLODData.CachedTangentsShared.IsValid())
			{
				CurrentLODData.CachedTangentsShared->SafeRelease();
				CurrentLODData.CachedTangentsShared.Reset();
			}
		}
		else
		{
			// 이전 변형 없음 → 기존 Fallback
			if (InDesc.FallbackDelegate.IsBound())
			{
				ENQUEUE_RENDER_COMMAND(FleshRingFallback)([FallbackDelegate = InDesc.FallbackDelegate](FRHICommandListImmediate& RHICmdList)
				{
					FallbackDelegate.ExecuteIfBound();
				});
			}
		}

		// GPU 디버그 버퍼들 클리어
		if (CurrentLODData.CachedDebugInfluencesShared.IsValid())
		{
			CurrentLODData.CachedDebugInfluencesShared->SafeRelease();
			CurrentLODData.CachedDebugInfluencesShared.Reset();
		}
		if (CurrentLODData.CachedDebugPointBufferShared.IsValid())
		{
			CurrentLODData.CachedDebugPointBufferShared->SafeRelease();
			CurrentLODData.CachedDebugPointBufferShared.Reset();
		}
		if (CurrentLODData.CachedDebugBulgePointBufferShared.IsValid())
		{
			CurrentLODData.CachedDebugBulgePointBufferShared->SafeRelease();
			CurrentLODData.CachedDebugBulgePointBufferShared.Reset();
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

	// Track LOD changes for invalidating previous position
	// 각 LOD는 별도의 캐시를 가지므로 캐시 무효화는 필요없음
	bool bInvalidatePreviousPosition = false;
	if (LODIndex != LastLodIndex)
	{
		bInvalidatePreviousPosition = true;
		LastLodIndex = LODIndex;
	}

	// ================================================================
	// 소스 버텍스 캐싱 (해당 LOD의 첫 프레임에만)
	// ================================================================
	if (!CurrentLODData.bSourcePositionsCached)
	{
		USkeletalMeshComponent* SkelMeshComp = Cast<USkeletalMeshComponent>(SkinnedMeshComp);
		USkeletalMesh* SkelMesh = SkelMeshComp ? SkelMeshComp->GetSkeletalMeshAsset() : nullptr;
		if (SkelMesh)
		{
			const FSkeletalMeshRenderData* RenderData = SkelMesh->GetResourceForRendering();
			if (RenderData && RenderData->LODRenderData.Num() > LODIndex)
			{
				const FSkeletalMeshLODRenderData& RenderLODData = RenderData->LODRenderData[LODIndex];
				const uint32 NumVerts = RenderLODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

				CurrentLODData.CachedSourcePositions.SetNum(NumVerts * 3);
				for (uint32 i = 0; i < NumVerts; ++i)
				{
					const FVector3f& Pos = RenderLODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i);
					CurrentLODData.CachedSourcePositions[i * 3 + 0] = Pos.X;
					CurrentLODData.CachedSourcePositions[i * 3 + 1] = Pos.Y;
					CurrentLODData.CachedSourcePositions[i * 3 + 2] = Pos.Z;
				}
				CurrentLODData.bSourcePositionsCached = true;
			}
		}
	}

	if (!CurrentLODData.bSourcePositionsCached)
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
	const TArray<FRingAffectedData>& AllRingData = CurrentLODData.AffectedVerticesManager.GetAllRingData();
	const uint32 TotalVertexCount = CurrentLODData.CachedSourcePositions.Num() / 3;

	// Ring 데이터 준비
	TSharedPtr<TArray<FFleshRingWorkItem::FRingDispatchData>> RingDispatchDataPtr =
		MakeShared<TArray<FFleshRingWorkItem::FRingDispatchData>>();
	RingDispatchDataPtr->Reserve(AllRingData.Num());

	// FleshRingAsset에서 Ring 설정 가져오기
	const TArray<FFleshRingSettings>* RingSettingsPtr = nullptr;
	if (FleshRingComponent.IsValid() && FleshRingComponent->FleshRingAsset)
	{
		RingSettingsPtr = &FleshRingComponent->FleshRingAsset->Rings;
	}

	// ===== 전체 메시 LayerTypes 변환 (한 번만, 모든 Ring 공유) =====
	// EFleshRingLayerType -> uint32 변환
	// GPU에서 VertexIndex로 직접 조회 가능한 lookup 테이블
	TArray<uint32> FullMeshLayerTypes;
	{
		const TArray<EFleshRingLayerType>& CachedLayerTypes = CurrentLODData.AffectedVerticesManager.GetCachedVertexLayerTypes();
		FullMeshLayerTypes.SetNum(CachedLayerTypes.Num());
		for (int32 i = 0; i < CachedLayerTypes.Num(); ++i)
		{
			FullMeshLayerTypes[i] = static_cast<uint32>(CachedLayerTypes[i]);
		}
	}

	for (int32 RingIndex = 0; RingIndex < AllRingData.Num(); ++RingIndex)
	{
		const FRingAffectedData& RingData = AllRingData[RingIndex];
		if (RingData.Vertices.Num() == 0)
		{
			continue;
		}

		FFleshRingWorkItem::FRingDispatchData DispatchData;
		DispatchData.OriginalRingIndex = RingIndex;  // 원본 인덱스 저장 (설정 조회용)
		DispatchData.Params = CreateTightnessParams(RingData, TotalVertexCount);

		// SmoothingBoundsZTop/Bottom 설정 (스무딩 영역 Z 확장)
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
		{
			const FFleshRingSettings& RingSettings = (*RingSettingsPtr)[RingIndex];
			DispatchData.Params.BoundsZTop = RingSettings.SmoothingBoundsZTop;
			DispatchData.Params.BoundsZBottom = RingSettings.SmoothingBoundsZBottom;
		}

		DispatchData.Indices = RingData.PackedIndices;
		DispatchData.Influences = RingData.PackedInfluences;
		DispatchData.LayerTypes = RingData.PackedLayerTypes;
		DispatchData.FullMeshLayerTypes = FullMeshLayerTypes;  // 전체 메시 LayerTypes (GPU 직접 업로드용)
		DispatchData.RepresentativeIndices = RingData.RepresentativeIndices;  // UV seam welding용
		DispatchData.bHasUVDuplicates = RingData.bHasUVDuplicates;  // UV Sync 스킵 최적화용

		// ===== 스무딩 영역 데이터 복사 (통합된 SmoothingRegion*) =====
		// 설계: Indices = Tightness용 (원본 SDF AABB)
		//       SmoothingRegion* = 스무딩/침투해결용 (BoundsExpand 또는 HopBased 모드)
		// Note: BoundsExpand/HopBased 모드 상관없이 동일한 변수 사용
		DispatchData.SmoothingRegionIndices = RingData.SmoothingRegionIndices;
		DispatchData.SmoothingRegionInfluences = RingData.SmoothingRegionInfluences;
		DispatchData.SmoothingRegionIsAnchor = RingData.SmoothingRegionIsAnchor;  // 앵커 플래그
		DispatchData.SmoothingRegionRepresentativeIndices = RingData.SmoothingRegionRepresentativeIndices;  // UV seam welding용
		DispatchData.bSmoothingRegionHasUVDuplicates = RingData.bSmoothingRegionHasUVDuplicates;  // UV Sync 스킵 최적화용
		DispatchData.SmoothingRegionLaplacianAdjacency = RingData.SmoothingRegionLaplacianAdjacency;
		DispatchData.SmoothingRegionPBDAdjacency = RingData.SmoothingRegionPBDAdjacency;
		DispatchData.SmoothingRegionAdjacencyOffsets = RingData.SmoothingRegionAdjacencyOffsets;
		DispatchData.SmoothingRegionAdjacencyTriangles = RingData.SmoothingRegionAdjacencyTriangles;

		// SkinSDF 레이어 분리용 데이터 복사
		DispatchData.SkinVertexIndices = RingData.SkinVertexIndices;
		DispatchData.SkinVertexNormals = RingData.SkinVertexNormals;
		DispatchData.StockingVertexIndices = RingData.StockingVertexIndices;

		// Normal Recomputation용 인접 데이터 복사
		DispatchData.AdjacencyOffsets = RingData.AdjacencyOffsets;
		DispatchData.AdjacencyTriangles = RingData.AdjacencyTriangles;

		// Laplacian Smoothing용 인접 데이터 복사
		DispatchData.LaplacianAdjacencyData = RingData.LaplacianAdjacencyData;

		// Bone Ratio Preserve용 슬라이스 데이터 복사
		DispatchData.OriginalBoneDistances = RingData.OriginalBoneDistances;
		DispatchData.AxisHeights = RingData.AxisHeights;
		DispatchData.SlicePackedData = RingData.SlicePackedData;

		// ===== DeformAmounts 계산 (Laplacian Smoothing에서 Bulge 영역 스무딩 감소용) =====
		// AxisHeight 기반으로 Bulge/Tightness 구분:
		//   - Ring 중앙(AxisHeight ≈ 0): Tightness (음수) → 스무딩 적용
		//   - Ring 가장자리(|AxisHeight| > threshold): Bulge (양수) → 스무딩 감소
		{
			const int32 NumAffected = DispatchData.Indices.Num();
			DispatchData.DeformAmounts.Reset(NumAffected);
			DispatchData.DeformAmounts.AddZeroed(NumAffected);

			// Ring 높이의 절반을 threshold로 사용 (이 안쪽이 tightness zone)
			const float RingHalfWidth = RingData.RingHeight * 0.5f;

			for (int32 i = 0; i < NumAffected; ++i)
			{
				const float AxisHeight = RingData.AxisHeights.IsValidIndex(i) ? RingData.AxisHeights[i] : 0.0f;
				const float Influence = DispatchData.Influences.IsValidIndex(i) ? DispatchData.Influences[i] : 0.0f;

				// Ring 중앙으로부터의 거리 비율 (0 = 중앙, 1 = 가장자리)
				const float EdgeRatio = FMath::Clamp(FMath::Abs(AxisHeight) / FMath::Max(RingHalfWidth, 0.01f), 0.0f, 2.0f);

				// EdgeRatio > 1 이면 Bulge 영역 (양수)
				// EdgeRatio < 1 이면 Tightness 영역 (음수)
				// Influence를 곱해서 실제 영향을 받는 정도 반영
				DispatchData.DeformAmounts[i] = (EdgeRatio - 1.0f) * Influence;
			}
		}

		// Ring별 RadialSmoothing 설정 복사
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
		{
			const FFleshRingSettings& Settings = (*RingSettingsPtr)[RingIndex];
			// bEnablePostProcess/bEnableSmoothing가 false면 모든 스무딩 비활성화
			DispatchData.bEnableRadialSmoothing = Settings.bEnablePostProcess && Settings.bEnableSmoothing && Settings.bEnableRadialSmoothing;
			DispatchData.RadialBlendStrength = Settings.RadialBlendStrength;
			DispatchData.RadialSliceHeight = Settings.RadialSliceHeight;
		}

		// Ring별 Laplacian/Taubin Smoothing 설정 복사
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
		{
			const FFleshRingSettings& Settings = (*RingSettingsPtr)[RingIndex];
			// bEnablePostProcess/bEnableSmoothing가 false면 모든 스무딩 비활성화
			DispatchData.bEnableLaplacianSmoothing = Settings.bEnablePostProcess && Settings.bEnableSmoothing && Settings.bEnableLaplacianSmoothing;
			DispatchData.bUseTaubinSmoothing = (Settings.LaplacianSmoothingType == ELaplacianSmoothingType::Taubin);
			DispatchData.SmoothingLambda = Settings.SmoothingLambda;
			DispatchData.TaubinMu = Settings.TaubinMu;
			DispatchData.SmoothingIterations = Settings.SmoothingIterations;

			// Anchor Mode: 원본 Affected Vertices를 앵커로 고정
			DispatchData.bAnchorDeformedVertices = Settings.bAnchorDeformedVertices;

			// 스무딩 확장 모드 설정
			// NOTE: 데이터는 항상 복사 (런타임 토글 지원)
			DispatchData.SmoothingExpandMode = Settings.SmoothingVolumeMode;
			DispatchData.HopBasedInfluences = RingData.HopBasedInfluences;

			// Note: SmoothingRegion* 데이터는 위에서 이미 복사됨 (통합된 변수)
			// HopBased 전용 데이터: HopDistances, SeedThreadIndices는 RingData에서 직접 접근

			// Heat Propagation 설정 복사 (HopBased 모드에서만 유효)
			DispatchData.bEnableHeatPropagation = Settings.bEnablePostProcess &&
				Settings.SmoothingVolumeMode == ESmoothingVolumeMode::HopBased &&
				Settings.bEnableHeatPropagation;
			DispatchData.HeatPropagationIterations = Settings.HeatPropagationIterations;
			DispatchData.HeatPropagationLambda = Settings.HeatPropagationLambda;
			DispatchData.bIncludeBulgeVerticesAsSeeds = Settings.bIncludeBulgeVerticesAsSeeds;
		}

		// Ring별 PBD Edge Constraint 설정 복사 (Tolerance 기반)
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
		{
			const FFleshRingSettings& Settings = (*RingSettingsPtr)[RingIndex];
			// bEnablePostProcess가 false면 모든 후처리 비활성화
			DispatchData.bEnablePBDEdgeConstraint = Settings.bEnablePostProcess && Settings.bEnablePBDEdgeConstraint;
			DispatchData.PBDStiffness = Settings.PBDStiffness;
			DispatchData.PBDIterations = Settings.PBDIterations;
			DispatchData.PBDTolerance = Settings.PBDTolerance;
			DispatchData.bPBDAnchorAffectedVertices = Settings.bPBDAnchorAffectedVertices;
		}

		// PBD용 인접 데이터 및 전체 맵 복사
		DispatchData.PBDAdjacencyWithRestLengths = RingData.PBDAdjacencyWithRestLengths;
		DispatchData.FullInfluenceMap = RingData.FullInfluenceMap;
		DispatchData.FullDeformAmountMap = RingData.FullDeformAmountMap;
		DispatchData.FullVertexAnchorFlags = RingData.FullVertexAnchorFlags;

		// bPBDAnchorAffectedVertices=false일 때 사용할 Zero 배열 캐시 (매 틱 할당 방지)
		if (!DispatchData.bPBDAnchorAffectedVertices && DispatchData.bEnablePBDEdgeConstraint)
		{
			// PBD 대상 버텍스 수 (통합된 SmoothingRegion 사용)
			const int32 NumPBDVertices = DispatchData.SmoothingRegionIndices.Num();
			const int32 NumTotalVertices = DispatchData.FullVertexAnchorFlags.Num();

			if (NumPBDVertices > 0 && NumTotalVertices > 0)
			{
				DispatchData.CachedZeroIsAnchorFlags.SetNumZeroed(NumPBDVertices);
				DispatchData.CachedZeroFullVertexAnchorFlags.SetNumZeroed(NumTotalVertices);
			}
		}

		// ===== Self-Collision Detection용 삼각형 추출 =====
		// 스타킹-살 충돌 검사를 위해 메시의 모든 삼각형 포함
		// (SDF 영향권 내 삼각형만으로는 스타킹만 포함되고 살은 포함 안 됨)
		{
			const TArray<uint32>& MeshIndices = CurrentLODData.AffectedVerticesManager.GetCachedMeshIndices();
			const int32 NumTriangles = MeshIndices.Num() / 3;

			if (NumTriangles > 0 && DispatchData.Indices.Num() > 0)
			{
				// 모든 메시 삼각형을 충돌 검사 대상으로 포함
				// 성능 제한은 CollisionShader에서 MaxPairsToProcess로 처리
				DispatchData.CollisionTriangleIndices = MeshIndices;

				}
		}

		// Ring별 InfluenceMode 확인
		EFleshRingInfluenceMode RingInfluenceMode = EFleshRingInfluenceMode::Auto;
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
		{
			RingInfluenceMode = (*RingSettingsPtr)[RingIndex].InfluenceMode;
		}

		// ===== VirtualBand 파라미터 설정 (SDF 무관하게 항상 설정) =====
		// GPU InfluenceMode: 0=Auto/SDF, 1=VirtualRing, 2=VirtualBand
		// Note: bUseSDFInfluence가 1이면 SDF 모드 사용, 0이면 InfluenceMode에 따라 분기
		switch (RingInfluenceMode)
		{
		case EFleshRingInfluenceMode::Auto:
			DispatchData.Params.InfluenceMode = 0;
			break;
		case EFleshRingInfluenceMode::VirtualRing:
			DispatchData.Params.InfluenceMode = 1;
			break;
		case EFleshRingInfluenceMode::VirtualBand:
			DispatchData.Params.InfluenceMode = 2;
			// VirtualBand 가변 반경 파라미터 설정
			if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
			{
				const FVirtualBandSettings& BandSettings = (*RingSettingsPtr)[RingIndex].VirtualBand;
				DispatchData.Params.LowerRadius = BandSettings.Lower.Radius;
				DispatchData.Params.MidLowerRadius = BandSettings.MidLowerRadius;
				DispatchData.Params.MidUpperRadius = BandSettings.MidUpperRadius;
				DispatchData.Params.UpperRadius = BandSettings.Upper.Radius;
				DispatchData.Params.LowerHeight = BandSettings.Lower.Height;
				DispatchData.Params.BandSectionHeight = BandSettings.BandHeight;
				DispatchData.Params.UpperHeight = BandSettings.Upper.Height;
			}
			break;
		}

		// SDF 캐시 데이터 전달 (렌더 스레드로 안전하게 복사)
		// Auto 모드 + SDF 유효할 때만 SDF 모드 사용 (VirtualBand는 SDF 미생성)
		if (FleshRingComponent.IsValid())
		{
			const FRingSDFCache* SDFCache = FleshRingComponent->GetRingSDFCache(RingIndex);
			const bool bUseSDFForThisRing =
				(RingInfluenceMode == EFleshRingInfluenceMode::Auto) &&
				(SDFCache && SDFCache->IsValid());

			if (bUseSDFForThisRing)
			{
				DispatchData.SDFPooledTexture = SDFCache->PooledTexture;
				DispatchData.SDFBoundsMin = SDFCache->BoundsMin;
				DispatchData.SDFBoundsMax = SDFCache->BoundsMax;
				DispatchData.bHasValidSDF = true;

				// OBB 지원: LocalToComponent 트랜스폼 복사
				DispatchData.SDFLocalToComponent = SDFCache->LocalToComponent;

				// Params에도 SDF 바운드 설정
				DispatchData.Params.SDFBoundsMin = SDFCache->BoundsMin;
				DispatchData.Params.SDFBoundsMax = SDFCache->BoundsMax;
				DispatchData.Params.bUseSDFInfluence = 1;

				// SDF Falloff 거리 계산: SDF 볼륨의 최소 축 크기 기반
				// 표면에서 멀어질수록 변형량이 부드럽게 감소
				FVector3f SDFExtent = SDFCache->BoundsMax - SDFCache->BoundsMin;
				float MinAxisSize = FMath::Min3(SDFExtent.X, SDFExtent.Y, SDFExtent.Z);
				DispatchData.Params.SDFInfluenceFalloffDistance = FMath::Max(MinAxisSize * 0.5f, 1.0f);

				// Ring Center: SDF 바운드 중심 사용 (본 위치보다 정확한 링 메시 중심)
				// 본 위치는 링 메시 중심과 다를 수 있음 (MeshOffset 등)
				DispatchData.SDFLocalRingCenter = (SDFCache->BoundsMin + SDFCache->BoundsMax) * 0.5f;

				// Ring Axis: SDF Local Space에서 링 메시의 구멍 방향 (가장 짧은 축)
				// CPU의 FSDFBulgeProvider::DetectRingAxis()와 동일한 로직 사용
				// 불일치 시 BulgeAxisDirection 필터링이 잘못됨
				if (SDFExtent.X <= SDFExtent.Y && SDFExtent.X <= SDFExtent.Z)
					DispatchData.SDFLocalRingAxis = FVector3f(1, 0, 0);
				else if (SDFExtent.Y <= SDFExtent.X && SDFExtent.Y <= SDFExtent.Z)
					DispatchData.SDFLocalRingAxis = FVector3f(0, 1, 0);
				else
					DispatchData.SDFLocalRingAxis = FVector3f(0, 0, 1);

				}
		}

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

	// ================================================================
	// 각 Ring별 Bulge 데이터 준비 (SDF 모드일 때만)
	// ================================================================
	bool bAnyRingHasBulge = false;

	// 소스 위치를 FVector3f 배열로 변환 (모든 Ring이 공유)
	TArray<FVector3f> AllVertexPositions;
	AllVertexPositions.SetNum(TotalVertexCount);
	for (uint32 i = 0; i < TotalVertexCount; ++i)
	{
		AllVertexPositions[i] = FVector3f(
			CurrentLODData.CachedSourcePositions[i * 3 + 0],
			CurrentLODData.CachedSourcePositions[i * 3 + 1],
			CurrentLODData.CachedSourcePositions[i * 3 + 2]);
	}

	// 각 Ring별로 Bulge 데이터 계산
	for (int32 RingIdx = 0; RingIdx < RingDispatchDataPtr->Num(); ++RingIdx)
	{
		FFleshRingWorkItem::FRingDispatchData& DispatchData = (*RingDispatchDataPtr)[RingIdx];

		// Ring별 Bulge 설정 가져오기 (OriginalRingIndex 사용)
		const int32 OriginalIdx = DispatchData.OriginalRingIndex;
		bool bBulgeEnabledInSettings = true;
		float RingBulgeStrength = 1.0f;
		float RingMaxBulgeDistance = 10.0f;
		float RingBulgeAxialRange = 3.0f;
		float RingBulgeRadialRange = 1.5f;
		float RingBulgeRadialRatio = 0.7f;
		float RingUpperBulgeStrength = 1.0f;
		float RingLowerBulgeStrength = 1.0f;
		EFleshRingFalloffType RingBulgeFalloff = EFleshRingFalloffType::WendlandC2;
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(OriginalIdx))
		{
			bBulgeEnabledInSettings = (*RingSettingsPtr)[OriginalIdx].bEnableBulge;
			RingBulgeStrength = (*RingSettingsPtr)[OriginalIdx].BulgeIntensity;
			RingBulgeAxialRange = (*RingSettingsPtr)[OriginalIdx].BulgeAxialRange;
			RingBulgeRadialRange = (*RingSettingsPtr)[OriginalIdx].BulgeRadialRange;
			RingBulgeRadialRatio = (*RingSettingsPtr)[OriginalIdx].BulgeRadialRatio;
			RingUpperBulgeStrength = (*RingSettingsPtr)[OriginalIdx].UpperBulgeStrength;
			RingLowerBulgeStrength = (*RingSettingsPtr)[OriginalIdx].LowerBulgeStrength;
			RingBulgeFalloff = (*RingSettingsPtr)[OriginalIdx].BulgeFalloff;
		}

		// bEnableBulge가 true이고 BulgeIntensity > 0이면 Bulge 활성화
		if (!bBulgeEnabledInSettings || RingBulgeStrength <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		// Bulge 영역 계산 (Spatial Hash로 O(N) → O(후보수) 최적화)
		TArray<uint32> BulgeIndices;
		TArray<float> BulgeInfluences;
		TArray<FVector3f> BulgeDirections;  // GPU에서 계산하므로 비어있음

		// AffectedVerticesManager에서 Spatial Hash 가져오기
		const FVertexSpatialHash* SpatialHash = &CurrentLODData.AffectedVerticesManager.GetSpatialHash();

		// ===== Bulge Provider 선택: SDF 유무 및 InfluenceMode에 따라 분기 =====
		// Ring InfluenceMode 가져오기
		EFleshRingInfluenceMode BulgeRingInfluenceMode = EFleshRingInfluenceMode::Auto;
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(OriginalIdx))
		{
			BulgeRingInfluenceMode = (*RingSettingsPtr)[OriginalIdx].InfluenceMode;
		}

		if (DispatchData.bHasValidSDF)
		{
			// Auto/VirtualBand 모드 + SDF 유효: SDF 바운드 기반 Bulge
			FSDFBulgeProvider BulgeProvider;
			BulgeProvider.InitFromSDFCache(
				DispatchData.SDFBoundsMin,
				DispatchData.SDFBoundsMax,
				DispatchData.SDFLocalToComponent,
				RingBulgeAxialRange,
				RingBulgeRadialRange);
			BulgeProvider.FalloffType = RingBulgeFalloff;

			BulgeProvider.CalculateBulgeRegion(
				AllVertexPositions,
				SpatialHash,
				BulgeIndices,
				BulgeInfluences,
				BulgeDirections);
		}
		else if (BulgeRingInfluenceMode == EFleshRingInfluenceMode::VirtualBand &&
				 RingSettingsPtr && RingSettingsPtr->IsValidIndex(OriginalIdx))
		{
			// VirtualBand 모드 + SDF 무효: 가변 반경 기반 Bulge
			const FVirtualBandSettings& BandSettings = (*RingSettingsPtr)[OriginalIdx].VirtualBand;

			// Band 중심/축 계산 (DispatchData에서 가져옴)
			FVector3f BandCenter = FVector3f(DispatchData.Params.RingCenter);
			FVector3f BandAxis = FVector3f(DispatchData.Params.RingAxis);

			FVirtualBandInfluenceProvider BulgeProvider;
			BulgeProvider.InitFromBandSettings(
				BandSettings.Lower.Radius,
				BandSettings.MidLowerRadius,
				BandSettings.MidUpperRadius,
				BandSettings.Upper.Radius,
				BandSettings.Lower.Height,
				BandSettings.BandHeight,
				BandSettings.Upper.Height,
				BandCenter,
				BandAxis,
				RingBulgeAxialRange,
				RingBulgeRadialRange);
			BulgeProvider.FalloffType = RingBulgeFalloff;

			BulgeProvider.CalculateBulgeRegion(
				AllVertexPositions,
				SpatialHash,
				BulgeIndices,
				BulgeInfluences,
				BulgeDirections);
		}
		else
		{
			// VirtualRing 모드: 고정 반경 기반 Bulge
			FVirtualRingBulgeProvider BulgeProvider;
			BulgeProvider.InitFromRingParams(
				FVector3f(DispatchData.Params.RingCenter),
				FVector3f(DispatchData.Params.RingAxis),
				DispatchData.Params.RingRadius,
				DispatchData.Params.RingHeight,
				RingBulgeAxialRange,
				RingBulgeRadialRange);
			BulgeProvider.FalloffType = RingBulgeFalloff;

			BulgeProvider.CalculateBulgeRegion(
				AllVertexPositions,
				SpatialHash,
				BulgeIndices,
				BulgeInfluences,
				BulgeDirections);
		}

		if (BulgeIndices.Num() > 0)
		{
			DispatchData.bEnableBulge = true;
			DispatchData.BulgeIndices = MoveTemp(BulgeIndices);
			DispatchData.BulgeInfluences = MoveTemp(BulgeInfluences);
			DispatchData.BulgeStrength = RingBulgeStrength;
			DispatchData.MaxBulgeDistance = RingMaxBulgeDistance;
			DispatchData.BulgeRadialRatio = RingBulgeRadialRatio;
			DispatchData.UpperBulgeStrength = RingUpperBulgeStrength;
			DispatchData.LowerBulgeStrength = RingLowerBulgeStrength;
			bAnyRingHasBulge = true;

			// ===== Bulge 방향 데이터 설정 =====
			// SDF 캐시에서 감지된 방향 가져오기 (OriginalRingIndex 사용)
			if (FleshRingComponent.IsValid())
			{
				const FRingSDFCache* SDFCache = FleshRingComponent->GetRingSDFCache(OriginalIdx);
				int32 DetectedDirection = SDFCache ? SDFCache->DetectedBulgeDirection : 0;
				DispatchData.DetectedBulgeDirection = DetectedDirection;

				// Ring 설정에서 BulgeDirection 모드 가져오기
				EBulgeDirectionMode BulgeDirectionMode = EBulgeDirectionMode::Auto;
				if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(OriginalIdx))
				{
					BulgeDirectionMode = (*RingSettingsPtr)[OriginalIdx].BulgeDirection;
				}

				// 최종 방향 계산 (Auto 모드면 감지된 방향, 아니면 수동 지정)
				switch (BulgeDirectionMode)
				{
				case EBulgeDirectionMode::Auto:
					// DetectedDirection == 0이면 폐쇄 메시(Torus) → 양방향 Bulge
					DispatchData.BulgeAxisDirection = DetectedDirection;  // 0, +1, or -1
					break;
				case EBulgeDirectionMode::Bidirectional:
					DispatchData.BulgeAxisDirection = 0;  // 양방향
					break;
				case EBulgeDirectionMode::Positive:
					DispatchData.BulgeAxisDirection = 1;
					break;
				case EBulgeDirectionMode::Negative:
					DispatchData.BulgeAxisDirection = -1;
					break;
				}
			}

			}
	}

	// TightenedBindPose 캐싱 여부 결정
	bool bNeedTightnessCaching = !CurrentLODData.bTightenedBindPoseCached;

	if (bNeedTightnessCaching)
	{
		CurrentLODData.bTightenedBindPoseCached = true;
		CurrentLODData.CachedTightnessVertexCount = TotalVertexCount;
		bInvalidatePreviousPosition = true;

		// TSharedPtr 생성 (첫 캐싱 시)
		if (!CurrentLODData.CachedTightenedBindPoseShared.IsValid())
		{
			CurrentLODData.CachedTightenedBindPoseShared = MakeShared<TRefCountPtr<FRDGPooledBuffer>>();
		}

		// 노멀 버퍼 TSharedPtr 생성 (첫 캐싱 시)
		if (!CurrentLODData.CachedNormalsShared.IsValid())
		{
			CurrentLODData.CachedNormalsShared = MakeShared<TRefCountPtr<FRDGPooledBuffer>>();
		}

		// 탄젠트 버퍼 TSharedPtr 생성 (첫 캐싱 시)
		if (!CurrentLODData.CachedTangentsShared.IsValid())
		{
			CurrentLODData.CachedTangentsShared = MakeShared<TRefCountPtr<FRDGPooledBuffer>>();
		}

		// 디버그 Influence 버퍼 TSharedPtr 생성 (첫 캐싱 시)
		if (!CurrentLODData.CachedDebugInfluencesShared.IsValid())
		{
			CurrentLODData.CachedDebugInfluencesShared = MakeShared<TRefCountPtr<FRDGPooledBuffer>>();
		}

		// 디버그 포인트 버퍼 TSharedPtr 생성 (첫 캐싱 시)
		if (!CurrentLODData.CachedDebugPointBufferShared.IsValid())
		{
			CurrentLODData.CachedDebugPointBufferShared = MakeShared<TRefCountPtr<FRDGPooledBuffer>>();
		}

		// Bulge 디버그 포인트 버퍼 TSharedPtr 생성 (첫 캐싱 시)
		if (!CurrentLODData.CachedDebugBulgePointBufferShared.IsValid())
		{
			CurrentLODData.CachedDebugBulgePointBufferShared = MakeShared<TRefCountPtr<FRDGPooledBuffer>>();
		}
	}

	// 디버그 Influence 출력 필요 여부 결정
	// 에디터에서 bShowDebugVisualization && bShowAffectedVertices가 활성화되어 있을 때만 출력
	bool bOutputDebugInfluences = false;
	bool bOutputDebugPoints = false;  // GPU 렌더링용 디버그 포인트 출력
	bool bOutputDebugBulgePoints = false;  // GPU 렌더링용 Bulge 디버그 포인트 출력
	uint32 MaxAffectedVertexCount = 0;
	uint32 MaxBulgeVertexCount = 0;
#if WITH_EDITORONLY_DATA
	if (FleshRingComponent.IsValid() && FleshRingComponent->bShowDebugVisualization && FleshRingComponent->bShowAffectedVertices)
	{
		bOutputDebugInfluences = true;

		// GPU 렌더링 모드에서는 DebugPointBuffer도 출력
		if (FleshRingComponent->IsGPUDebugRenderingEnabled())
		{
			bOutputDebugPoints = true;
		}

		// Readback을 위한 최대 영향받는 버텍스 수 계산
		if (RingDispatchDataPtr.IsValid())
		{
			for (const auto& RingData : *RingDispatchDataPtr)
			{
				MaxAffectedVertexCount = FMath::Max(MaxAffectedVertexCount, RingData.Params.NumAffectedVertices);
			}
		}

		// Readback 관련 포인터 초기화 (첫 사용 시)
		if (MaxAffectedVertexCount > 0)
		{
			if (!CurrentLODData.DebugInfluenceReadbackResult.IsValid())
			{
				CurrentLODData.DebugInfluenceReadbackResult = MakeShared<TArray<float>>();
			}
			if (!CurrentLODData.bDebugInfluenceReadbackComplete.IsValid())
			{
				CurrentLODData.bDebugInfluenceReadbackComplete = MakeShared<std::atomic<bool>>(false);
			}
			CurrentLODData.DebugInfluenceCount = MaxAffectedVertexCount;
		}
	}

	// Bulge 디버그 포인트 출력 활성화
	// bShowDebugVisualization && bShowBulgeHeatmap && GPU 렌더링 모드일 때
	if (FleshRingComponent.IsValid() && FleshRingComponent->bShowDebugVisualization && FleshRingComponent->bShowBulgeHeatmap)
	{
		if (FleshRingComponent->IsGPUDebugRenderingEnabled())
		{
			bOutputDebugBulgePoints = true;

			// Bulge 버텍스 수 계산
			if (RingDispatchDataPtr.IsValid())
			{
				for (const auto& RingData : *RingDispatchDataPtr)
				{
					MaxBulgeVertexCount += RingData.BulgeIndices.Num();
				}
			}

			// ★ MaxBulgeVertexCount == 0이면 기존 캐시 버퍼 클리어
			// (bEnableBulge가 꺼졌을 때 이전 프레임의 버퍼가 남아있는 문제 해결)
			if (MaxBulgeVertexCount == 0 && CurrentLODData.CachedDebugBulgePointBufferShared.IsValid())
			{
				CurrentLODData.CachedDebugBulgePointBufferShared->SafeRelease();
				CurrentLODData.CachedDebugBulgePointBufferShared.Reset();
			}
		}
	}
#endif

	// GPU 디버그 렌더링용 버퍼 초기화
	// ★ DrawDebug 방식: 캐싱 없이 매 프레임 새로 계산 (정확성 > 성능)
	// 디버깅 목적이므로 성능 저하는 허용
	if (bOutputDebugPoints || bOutputDebugBulgePoints)
	{
		// 디버그 렌더링 활성화 시 매 프레임 TightnessCS/BulgeCS 재실행
		bNeedTightnessCaching = true;

		// Affected 디버그 포인트 버퍼 TSharedPtr 생성
		if (bOutputDebugPoints && !CurrentLODData.CachedDebugPointBufferShared.IsValid())
		{
			CurrentLODData.CachedDebugPointBufferShared = MakeShared<TRefCountPtr<FRDGPooledBuffer>>();
		}

		// Bulge 디버그 포인트 버퍼 TSharedPtr 생성
		if (bOutputDebugBulgePoints && !CurrentLODData.CachedDebugBulgePointBufferShared.IsValid())
		{
			CurrentLODData.CachedDebugBulgePointBufferShared = MakeShared<TRefCountPtr<FRDGPooledBuffer>>();
		}
	}

	// 작업 아이템 생성
	FFleshRingWorkItem WorkItem;
	WorkItem.DeformerInstance = this;
	WorkItem.MeshObject = MeshObject;
	WorkItem.LODIndex = LODIndex;
	WorkItem.TotalVertexCount = TotalVertexCount;
	WorkItem.SourceDataPtr = MakeShared<TArray<float>>(CurrentLODData.CachedSourcePositions);
	WorkItem.RingDispatchDataPtr = RingDispatchDataPtr;

	// Normal Recomputation용 메시 인덱스 전달
	const TArray<uint32>& MeshIndices = CurrentLODData.AffectedVerticesManager.GetCachedMeshIndices();
	if (MeshIndices.Num() > 0)
	{
		WorkItem.MeshIndicesPtr = MakeShared<TArray<uint32>>(MeshIndices);
	}

	WorkItem.bNeedTightnessCaching = bNeedTightnessCaching;
	WorkItem.bInvalidatePreviousPosition = bInvalidatePreviousPosition;
	WorkItem.CachedBufferSharedPtr = CurrentLODData.CachedTightenedBindPoseShared;  // TSharedPtr 복사 (ref count 증가)
	WorkItem.CachedNormalsBufferSharedPtr = CurrentLODData.CachedNormalsShared;  // 노멀 캐시 버퍼 (ref count 증가)
	WorkItem.CachedTangentsBufferSharedPtr = CurrentLODData.CachedTangentsShared;  // 탄젠트 캐시 버퍼 (ref count 증가)
	WorkItem.CachedDebugInfluencesBufferSharedPtr = CurrentLODData.CachedDebugInfluencesShared;  // 디버그 Influence 캐시 버퍼
	WorkItem.bOutputDebugInfluences = bOutputDebugInfluences;  // 디버그 Influence 출력 활성화
	WorkItem.DebugInfluenceReadbackResultPtr = CurrentLODData.DebugInfluenceReadbackResult;  // Readback 결과 저장 배열
	WorkItem.bDebugInfluenceReadbackComplete = CurrentLODData.bDebugInfluenceReadbackComplete;  // Readback 완료 플래그
	WorkItem.DebugInfluenceCount = CurrentLODData.DebugInfluenceCount;  // Readback할 버텍스 수

	// GPU 디버그 렌더링용 DebugPointBuffer 관련 필드
	WorkItem.CachedDebugPointBufferSharedPtr = CurrentLODData.CachedDebugPointBufferShared;
	WorkItem.bOutputDebugPoints = bOutputDebugPoints;

	// GPU 디버그 렌더링용 Bulge DebugPointBuffer 관련 필드
	WorkItem.CachedDebugBulgePointBufferSharedPtr = CurrentLODData.CachedDebugBulgePointBufferShared;
	WorkItem.bOutputDebugBulgePoints = bOutputDebugBulgePoints;
	WorkItem.DebugBulgePointCount = MaxBulgeVertexCount;


	// LocalToWorld 매트릭스 설정 - ResolvedTargetMesh 우선 사용
	USkeletalMeshComponent* TargetMeshComp = nullptr;
	if (FleshRingComponent.IsValid())
	{
		TargetMeshComp = FleshRingComponent->GetResolvedTargetMesh();
	}
	if (!TargetMeshComp && MeshComponent.IsValid())
	{
		TargetMeshComp = Cast<USkeletalMeshComponent>(MeshComponent.Get());
	}

	if (TargetMeshComp)
	{
		FTransform WorldTransform = TargetMeshComp->GetComponentTransform();
		WorkItem.LocalToWorldMatrix = FMatrix44f(WorldTransform.ToMatrixWithScale());
	}

	WorkItem.FallbackDelegate = InDesc.FallbackDelegate;

	// Bulge 전역 플래그 설정 (VolumeAccumBuffer 생성 여부 결정용)
	WorkItem.bAnyRingHasBulge = bAnyRingHasBulge;

	// Layer Penetration Resolution 플래그 설정
	if (FleshRingComponent.IsValid() && FleshRingComponent->FleshRingAsset)
	{
		WorkItem.bEnableLayerPenetrationResolution =
			FleshRingComponent->FleshRingAsset->bEnableLayerPenetrationResolution;

		// Normal/Tangent Recompute 플래그 설정
		WorkItem.bEnableNormalRecompute =
			FleshRingComponent->FleshRingAsset->bEnableNormalRecompute;
		WorkItem.NormalRecomputeMode =
			static_cast<uint32>(FleshRingComponent->FleshRingAsset->NormalRecomputeMethod);
		WorkItem.bEnableTangentRecompute =
			FleshRingComponent->FleshRingAsset->bEnableTangentRecompute;
		WorkItem.TangentRecomputeMode =
			static_cast<uint32>(FleshRingComponent->FleshRingAsset->TangentRecomputeMethod);
	}

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

#if WITH_EDITORONLY_DATA
bool UFleshRingDeformerInstance::HasCachedDeformedGeometry(int32 LODIndex) const
{
	if (!LODData.IsValidIndex(LODIndex))
	{
		return false;
	}

	const FLODDeformationData& Data = LODData[LODIndex];
	return Data.bTightenedBindPoseCached &&
		Data.CachedTightenedBindPoseShared.IsValid() &&
		Data.CachedTightenedBindPoseShared->IsValid();
}

bool UFleshRingDeformerInstance::ReadbackDeformedGeometry(
	TArray<FVector3f>& OutPositions,
	TArray<FVector3f>& OutNormals,
	TArray<FVector4f>& OutTangents,
	int32 LODIndex)
{
	if (!HasCachedDeformedGeometry(LODIndex))
	{
		UE_LOG(LogFleshRing, Warning, TEXT("ReadbackDeformedGeometry: No cached deformed geometry for LOD %d"), LODIndex);
		return false;
	}

	const FLODDeformationData& Data = LODData[LODIndex];
	const uint32 NumVertices = Data.CachedTightnessVertexCount;

	if (NumVertices == 0)
	{
		UE_LOG(LogFleshRing, Warning, TEXT("ReadbackDeformedGeometry: NumVertices is 0"));
		return false;
	}

	// GPU 작업 완료 대기
	FlushRenderingCommands();

	// ===== Position Readback =====
	bool bPositionSuccess = false;
	if (Data.CachedTightenedBindPoseShared.IsValid() && Data.CachedTightenedBindPoseShared->IsValid())
	{
		TRefCountPtr<FRDGPooledBuffer> PooledBuffer = *Data.CachedTightenedBindPoseShared;
		FBufferRHIRef BufferRHI = PooledBuffer->GetRHI();

		if (BufferRHI.IsValid())
		{
			// ★ RDG 버퍼 풀링으로 인해 버퍼 크기가 요청보다 클 수 있음
			// BufferRHI->GetSize()가 아닌 CachedTightnessVertexCount를 사용해야 함
			const uint32 ActualBufferSize = BufferRHI->GetSize();
			const uint32 AllocatedVertexCount = ActualBufferSize / (3 * sizeof(float));

			// 실제로 의미있는 데이터 수는 캐싱 시점에 저장된 값
			const uint32 CachedVertexCount = Data.CachedTightnessVertexCount;

			// 버퍼가 충분한지 확인
			if (AllocatedVertexCount < CachedVertexCount)
			{
				UE_LOG(LogFleshRing, Error, TEXT("ReadbackDeformedGeometry: Buffer too small! Allocated=%u, Cached=%u"),
					AllocatedVertexCount, CachedVertexCount);
				return false;
			}

			// 디버그 로그 (크기가 다른 경우에만)
			if (CachedVertexCount != NumVertices)
			{
				UE_LOG(LogFleshRing, Warning, TEXT("ReadbackDeformedGeometry: CachedVertexCount (%u) != expected (%u)"),
					CachedVertexCount, NumVertices);
			}

			// ★ 캐싱된 버텍스 수만큼만 읽기 (RDG 풀링된 여분 데이터 무시)
			const uint32 VertexCountToRead = CachedVertexCount;
			const uint32 SizeToRead = VertexCountToRead * 3 * sizeof(float);

			TArray<float> TempPositions;
			TempPositions.SetNumUninitialized(VertexCountToRead * 3);

			// UE5.7 동기 Readback 방식: RenderThread에서 Lock/Unlock 수행
			TArray<float>* DestPtr = &TempPositions;
			uint32 ReadSize = SizeToRead;
			ENQUEUE_RENDER_COMMAND(ReadbackPositions)(
				[BufferRHI, ReadSize, DestPtr](FRHICommandListImmediate& RHICmdList)
				{
					void* MappedData = RHICmdList.LockBuffer(BufferRHI, 0, ReadSize, RLM_ReadOnly);
					if (MappedData)
					{
						FMemory::Memcpy(DestPtr->GetData(), MappedData, ReadSize);
						RHICmdList.UnlockBuffer(BufferRHI);
					}
				});
			FlushRenderingCommands();

			OutPositions.SetNum(VertexCountToRead);
			for (uint32 i = 0; i < VertexCountToRead; ++i)
			{
				OutPositions[i] = FVector3f(
					TempPositions[i * 3 + 0],
					TempPositions[i * 3 + 1],
					TempPositions[i * 3 + 2]);
			}
			bPositionSuccess = true;
		}
	}

	if (!bPositionSuccess)
	{
		UE_LOG(LogFleshRing, Warning, TEXT("ReadbackDeformedGeometry: Position readback failed"));
		return false;
	}

	// ===== Normal Readback =====
	// ★ Normal 버퍼는 float3 형식! (셰이더에서 버텍스당 3 float로 저장)
	bool bNormalSuccess = false;
	if (Data.CachedNormalsShared.IsValid() && Data.CachedNormalsShared->IsValid())
	{
		TRefCountPtr<FRDGPooledBuffer> PooledBuffer = *Data.CachedNormalsShared;
		FBufferRHIRef BufferRHI = PooledBuffer->GetRHI();

		if (BufferRHI.IsValid())
		{
			// ★ Normal 버퍼는 float3 형식 (버텍스당 3 float)
			const uint32 ActualBufferSize = BufferRHI->GetSize();
			const uint32 AllocatedVertexCount = ActualBufferSize / (3 * sizeof(float));  // float3!
			const uint32 CachedVertexCount = Data.CachedTightnessVertexCount;

			// 버퍼가 충분한지 확인
			if (AllocatedVertexCount < CachedVertexCount)
			{
				UE_LOG(LogFleshRing, Warning, TEXT("ReadbackDeformedGeometry: Normal buffer too small! Allocated=%u, Cached=%u"),
					AllocatedVertexCount, CachedVertexCount);
				// Normal은 선택사항이므로 에러 아님
			}

			// ★ 캐싱된 버텍스 수만큼만 읽기
			const uint32 VertexCountToRead = FMath::Min(CachedVertexCount, AllocatedVertexCount);
			const uint32 SizeToRead = VertexCountToRead * 3 * sizeof(float);  // float3!

			TArray<float> TempNormals;
			TempNormals.SetNumUninitialized(VertexCountToRead * 3);  // float3!

			// UE5.7 동기 Readback 방식
			TArray<float>* DestPtr = &TempNormals;
			uint32 ReadSize = SizeToRead;
			ENQUEUE_RENDER_COMMAND(ReadbackNormals)(
				[BufferRHI, ReadSize, DestPtr](FRHICommandListImmediate& RHICmdList)
				{
					void* MappedData = RHICmdList.LockBuffer(BufferRHI, 0, ReadSize, RLM_ReadOnly);
					if (MappedData)
					{
						FMemory::Memcpy(DestPtr->GetData(), MappedData, ReadSize);
						RHICmdList.UnlockBuffer(BufferRHI);
					}
				});
			FlushRenderingCommands();

			OutNormals.SetNum(VertexCountToRead);
			for (uint32 i = 0; i < VertexCountToRead; ++i)
			{
				OutNormals[i] = FVector3f(
					TempNormals[i * 3 + 0],  // float3!
					TempNormals[i * 3 + 1],
					TempNormals[i * 3 + 2]);
			}
			bNormalSuccess = true;
		}
	}

	if (!bNormalSuccess)
	{
		UE_LOG(LogFleshRing, Warning, TEXT("ReadbackDeformedGeometry: Normal readback failed (may be disabled)"));
		// Normal은 선택사항이므로 에러 아님, 빈 배열 반환
		OutNormals.Empty();
	}

	// ===== Tangent Readback =====
	bool bTangentSuccess = false;
	if (Data.CachedTangentsShared.IsValid() && Data.CachedTangentsShared->IsValid())
	{
		TRefCountPtr<FRDGPooledBuffer> PooledBuffer = *Data.CachedTangentsShared;
		FBufferRHIRef BufferRHI = PooledBuffer->GetRHI();

		if (BufferRHI.IsValid())
		{
			// ★ Position과 동일하게 CachedTightnessVertexCount 사용 (RDG 버퍼 풀링 대응)
			const uint32 ActualBufferSize = BufferRHI->GetSize();
			const uint32 AllocatedVertexCount = ActualBufferSize / (4 * sizeof(float));
			const uint32 CachedVertexCount = Data.CachedTightnessVertexCount;

			// 버퍼가 충분한지 확인
			if (AllocatedVertexCount < CachedVertexCount)
			{
				UE_LOG(LogFleshRing, Warning, TEXT("ReadbackDeformedGeometry: Tangent buffer too small! Allocated=%u, Cached=%u"),
					AllocatedVertexCount, CachedVertexCount);
				// Tangent은 선택사항이므로 에러 아님
			}

			// ★ 캐싱된 버텍스 수만큼만 읽기
			const uint32 VertexCountToRead = FMath::Min(CachedVertexCount, AllocatedVertexCount);
			const uint32 SizeToRead = VertexCountToRead * 4 * sizeof(float);

			TArray<float> TempTangents;
			TempTangents.SetNumUninitialized(VertexCountToRead * 4);

			// UE5.7 동기 Readback 방식
			TArray<float>* DestPtr = &TempTangents;
			uint32 ReadSize = SizeToRead;
			ENQUEUE_RENDER_COMMAND(ReadbackTangents)(
				[BufferRHI, ReadSize, DestPtr](FRHICommandListImmediate& RHICmdList)
				{
					void* MappedData = RHICmdList.LockBuffer(BufferRHI, 0, ReadSize, RLM_ReadOnly);
					if (MappedData)
					{
						FMemory::Memcpy(DestPtr->GetData(), MappedData, ReadSize);
						RHICmdList.UnlockBuffer(BufferRHI);
					}
				});
			FlushRenderingCommands();

			OutTangents.SetNum(VertexCountToRead);
			for (uint32 i = 0; i < VertexCountToRead; ++i)
			{
				OutTangents[i] = FVector4f(
					TempTangents[i * 4 + 0],
					TempTangents[i * 4 + 1],
					TempTangents[i * 4 + 2],
					TempTangents[i * 4 + 3]);
			}
			bTangentSuccess = true;
		}
	}

	if (!bTangentSuccess)
	{
		UE_LOG(LogFleshRing, Warning, TEXT("ReadbackDeformedGeometry: Tangent readback failed (may be disabled)"));
		// Tangent도 선택사항이므로 에러 아님, 빈 배열 반환
		OutTangents.Empty();
	}

	UE_LOG(LogFleshRing, Log, TEXT("ReadbackDeformedGeometry: Success - %d vertices, Normals=%d, Tangents=%d"),
		OutPositions.Num(), OutNormals.Num(), OutTangents.Num());

	return true;
}
#endif

void UFleshRingDeformerInstance::InvalidateTightnessCache(int32 DirtyRingIndex)
{
    // 1. AffectedVertices 재등록 (Ring 트랜스폼 변경 시 영향받는 정점이 달라질 수 있음)
    if (FleshRingComponent.IsValid())
    {
        USkeletalMeshComponent* SkelMesh = Cast<USkeletalMeshComponent>(MeshComponent.Get());
        if (SkelMesh)
        {
            for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
            {
                // Dirty Flag 설정: 특정 Ring만 또는 전체
                if (DirtyRingIndex == INDEX_NONE)
                {
                    // 전체 무효화
                    LODData[LODIndex].AffectedVerticesManager.MarkAllRingsDirty();
                }
                else
                {
                    // 특정 Ring만 무효화
                    LODData[LODIndex].AffectedVerticesManager.MarkRingDirty(DirtyRingIndex);
                }

                // RegisterAffectedVertices는 dirty한 Ring만 처리함
                LODData[LODIndex].bAffectedVerticesRegistered =
                    LODData[LODIndex].AffectedVerticesManager.RegisterAffectedVertices(
                        FleshRingComponent.Get(), SkelMesh, LODIndex);
            }
        }
    }

    // 2. 모든 LOD의 TightenedBindPose 캐시 무효화
    // 다음 프레임에서 TightnessCS가 새 트랜스폼으로 재계산됨
    for (FLODDeformationData& Data : LODData)
    {
        Data.bTightenedBindPoseCached = false;

        // Note: CachedTightenedBindPoseShared/CachedNormalsShared/CachedTangentsShared는
        // 여기서 해제하지 않음! EnqueueWork()에서 AffectedVertices == 0일 때
        // Passthrough Skinning을 위해 버퍼 유효성이 필요함.
        // Passthrough 완료 후 EnqueueWork()에서 해제됨.

        // GPU 디버그 포인트 버퍼는 여기서 클리어하지 않음
        // 드래그 중에도 포인트가 보여야 하므로, AffectedCount == 0일 때만
        // EnqueueWork Fallback에서 클리어함

        // 3. GPU Influence Readback 캐시도 무효화
        // 새 TightnessCS 결과가 Readback될 때까지 CPU fallback 사용
        if (Data.bDebugInfluenceReadbackComplete.IsValid())
        {
            Data.bDebugInfluenceReadbackComplete->store(false);
        }
        if (Data.DebugInfluenceReadbackResult.IsValid())
        {
            Data.DebugInfluenceReadbackResult->Empty();
        }
    }

    // 4. CPU 디버그 캐시도 무효화 (GPU 재계산과 동기화)
    if (FleshRingComponent.IsValid())
    {
        FleshRingComponent->InvalidateDebugCaches(DirtyRingIndex);
    }
}

void UFleshRingDeformerInstance::InvalidateForMeshChange()
{
	// ★ 메시 변경 시 완전 재초기화
	// 기존 GPU 버퍼 해제 + NumLODs/LODData 재설정 + AffectedVertices 재등록

	// Step 1: 기존 리소스 완전 해제
	ReleaseResources();

	// Step 2: 새 메시에서 LOD 구조 재초기화
	USkeletalMeshComponent* SkelMesh = Cast<USkeletalMeshComponent>(MeshComponent.Get());
	if (SkelMesh)
	{
		USkeletalMesh* Mesh = SkelMesh->GetSkeletalMeshAsset();
		if (Mesh)
		{
			const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
			if (RenderData)
			{
				const int32 NewNumLODs = RenderData->LODRenderData.Num();

				// LOD 개수가 다르면 배열 재생성
				if (NewNumLODs != NumLODs)
				{
					LODData.Empty();
					NumLODs = NewNumLODs;
					LODData.SetNum(NumLODs);
				}
				else
				{
					// LOD 개수가 같아도 모든 데이터 초기화
					for (FLODDeformationData& Data : LODData)
					{
						Data.CachedSourcePositions.Empty();
						Data.bSourcePositionsCached = false;
						Data.bTightenedBindPoseCached = false;
						Data.CachedTightnessVertexCount = 0;
						Data.bAffectedVerticesRegistered = false;
						Data.AffectedVerticesManager.MarkAllRingsDirty();
					}
				}

				// Step 3: 각 LOD에 대해 AffectedVertices 재등록
				if (FleshRingComponent.IsValid())
				{
					for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
					{
						LODData[LODIndex].bAffectedVerticesRegistered =
							LODData[LODIndex].AffectedVerticesManager.RegisterAffectedVertices(
								FleshRingComponent.Get(), SkelMesh, LODIndex);
					}
				}
			}
		}
	}

	// Step 4: GPU 명령 플러시하여 버퍼 해제 완료 보장
	FlushRenderingCommands();

	// LOD 변경 추적 리셋
	LastLodIndex = INDEX_NONE;
}