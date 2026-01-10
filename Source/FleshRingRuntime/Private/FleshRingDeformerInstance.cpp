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

void UFleshRingDeformerInstance::SetupFromDeformer(UFleshRingDeformer* InDeformer, UMeshComponent* InMeshComponent)
{
	Deformer = InDeformer;
	MeshComponent = InMeshComponent;
	Scene = InMeshComponent ? InMeshComponent->GetScene() : nullptr;
	LastLodIndex = INDEX_NONE;

	// FleshRingComponent 찾기 및 모든 LOD에 대해 AffectedVertices 등록
	if (AActor* Owner = InMeshComponent->GetOwner())
	{
		FleshRingComponent = Owner->FindComponentByClass<UFleshRingComponent>();
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

						UE_LOG(LogFleshRing, Log, TEXT("AffectedVertices 등록 완료: %d/%d LODs"),
							SuccessCount, NumLODs);
					}
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
	// 모든 LOD의 캐싱된 리소스 해제
	for (FLODDeformationData& Data : LODData)
	{
		// TightenedBindPose 버퍼 해제
		if (Data.CachedTightenedBindPoseShared.IsValid())
		{
			Data.CachedTightenedBindPoseShared->SafeRelease();
			Data.CachedTightenedBindPoseShared.Reset();  // ★ TSharedPtr도 Reset (메모리 누수 방지)
		}
		Data.bTightenedBindPoseCached = false;
		Data.CachedTightnessVertexCount = 0;

		// 재계산된 노멀 버퍼 해제
		if (Data.CachedNormalsShared.IsValid())
		{
			Data.CachedNormalsShared->SafeRelease();
			Data.CachedNormalsShared.Reset();  // ★ TSharedPtr도 Reset (메모리 누수 방지)
		}

		// 재계산된 탄젠트 버퍼 해제
		if (Data.CachedTangentsShared.IsValid())
		{
			Data.CachedTangentsShared->SafeRelease();
			Data.CachedTangentsShared.Reset();  // ★ TSharedPtr도 Reset (메모리 누수 방지)
		}

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

	// AffectedVertices가 등록되지 않았으면 Fallback
	if (!CurrentLODData.bAffectedVerticesRegistered || CurrentLODData.AffectedVerticesManager.GetTotalAffectedCount() == 0)
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

	// Track LOD changes for invalidating previous position
	// 각 LOD는 별도의 캐시를 가지므로 캐시 무효화는 필요없음
	bool bInvalidatePreviousPosition = false;
	if (LODIndex != LastLodIndex)
	{
		bInvalidatePreviousPosition = true;
		LastLodIndex = LODIndex;
		UE_LOG(LogFleshRing, Log, TEXT("FleshRing: LOD changed to %d"), LODIndex);
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
		DispatchData.RepresentativeIndices = RingData.RepresentativeIndices;  // UV seam welding용

		// Z 확장 후처리 버텍스 데이터 복사
		// 설계: Indices = Tightness용 (원본 SDF AABB)
		//       PostProcessing* = 스무딩/침투해결용 (원본 + BoundsZTop/Bottom)
		DispatchData.PostProcessingIndices = RingData.PostProcessingIndices;
		DispatchData.PostProcessingInfluences = RingData.PostProcessingInfluences;
		DispatchData.PostProcessingLayerTypes = RingData.PostProcessingLayerTypes;
		DispatchData.PostProcessingRepresentativeIndices = RingData.PostProcessingRepresentativeIndices;  // UV seam welding용
		DispatchData.PostProcessingLaplacianAdjacencyData = RingData.PostProcessingLaplacianAdjacencyData;
		DispatchData.PostProcessingPBDAdjacencyWithRestLengths = RingData.PostProcessingPBDAdjacencyWithRestLengths;
		DispatchData.PostProcessingAdjacencyOffsets = RingData.PostProcessingAdjacencyOffsets;
		DispatchData.PostProcessingAdjacencyTriangles = RingData.PostProcessingAdjacencyTriangles;

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
			const float RingHalfWidth = RingData.RingWidth * 0.5f;

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
			// bEnablePostProcess가 false면 모든 후처리 비활성화
			DispatchData.bEnableRadialSmoothing = Settings.bEnablePostProcess && Settings.bEnableRadialSmoothing;
		}

		// Ring별 Laplacian/Taubin Smoothing 설정 복사
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
		{
			const FFleshRingSettings& Settings = (*RingSettingsPtr)[RingIndex];
			// bEnablePostProcess가 false면 모든 후처리 비활성화
			DispatchData.bEnableLaplacianSmoothing = Settings.bEnablePostProcess && Settings.bEnableLaplacianSmoothing;
			DispatchData.bUseTaubinSmoothing = Settings.bUseTaubinSmoothing;
			DispatchData.SmoothingLambda = Settings.SmoothingLambda;
			DispatchData.TaubinMu = Settings.TaubinMu;
			DispatchData.SmoothingIterations = Settings.SmoothingIterations;
			DispatchData.LaplacianVolumePreservation = Settings.VolumePreservation;

			// 홉 기반 스무딩 설정 및 데이터 복사
			// NOTE: 데이터는 항상 복사 (런타임 토글 지원)
			DispatchData.bUseHopBasedSmoothing = (Settings.SmoothingVolumeMode == ESmoothingVolumeMode::HopBased);
			DispatchData.HopBasedInfluences = RingData.HopBasedInfluences;

			// 확장된 스무딩 영역 데이터 복사 (Seeds + N-hop 도달 버텍스)
			DispatchData.ExtendedSmoothingIndices = RingData.ExtendedSmoothingIndices;
			DispatchData.ExtendedInfluences = RingData.ExtendedInfluences;
			DispatchData.ExtendedLaplacianAdjacency = RingData.ExtendedLaplacianAdjacency;
		}

		// Ring별 PBD Edge Constraint 설정 복사
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
		{
			const FFleshRingSettings& Settings = (*RingSettingsPtr)[RingIndex];
			// bEnablePostProcess가 false면 모든 후처리 비활성화
			DispatchData.bEnablePBDEdgeConstraint = Settings.bEnablePostProcess && Settings.bEnablePBDEdgeConstraint;
			DispatchData.PBDStiffness = Settings.PBDStiffness;
			DispatchData.PBDIterations = Settings.PBDIterations;
			DispatchData.bPBDUseDeformAmountWeight = Settings.bPBDUseDeformAmountWeight;
		}

		// PBD용 인접 데이터 및 전체 맵 복사
		DispatchData.PBDAdjacencyWithRestLengths = RingData.PBDAdjacencyWithRestLengths;
		DispatchData.FullInfluenceMap = RingData.FullInfluenceMap;
		DispatchData.FullDeformAmountMap = RingData.FullDeformAmountMap;

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

				// [조건부 로그] 첫 프레임만
				static TSet<int32> LoggedCollisionRings;
				if (!LoggedCollisionRings.Contains(RingIndex) && DispatchData.CollisionTriangleIndices.Num() > 0)
				{
					UE_LOG(LogFleshRing, Log, TEXT("[DEBUG] Ring[%d] Collision triangles: %d (ALL mesh triangles for stocking-skin detection)"),
						RingIndex, DispatchData.CollisionTriangleIndices.Num() / 3);
					LoggedCollisionRings.Add(RingIndex);
				}
			}
		}

		// Ring별 InfluenceMode 확인
		EFleshRingInfluenceMode RingInfluenceMode = EFleshRingInfluenceMode::Auto;
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
		{
			RingInfluenceMode = (*RingSettingsPtr)[RingIndex].InfluenceMode;
		}

		// SDF 캐시 데이터 전달 (렌더 스레드로 안전하게 복사)
		// Auto 또는 ProceduralBand 모드 + SDF 유효할 때만 SDF 모드 사용
		if (FleshRingComponent.IsValid())
		{
			const FRingSDFCache* SDFCache = FleshRingComponent->GetRingSDFCache(RingIndex);
			const bool bUseSDFForThisRing =
				(RingInfluenceMode == EFleshRingInfluenceMode::Auto ||
				 RingInfluenceMode == EFleshRingInfluenceMode::ProceduralBand) &&
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

				// Ring Axis: SDF 바운드에서 가장 짧은 축 = 링 구멍 방향
				// GPU Depot 버전과 동일한 로직 (본 기반 대신 메시 형상 기반)
				if (SDFExtent.Z < SDFExtent.X && SDFExtent.Z < SDFExtent.Y)
					DispatchData.SDFLocalRingAxis = FVector3f(0, 0, 1);
				else if (SDFExtent.Y < SDFExtent.X)
					DispatchData.SDFLocalRingAxis = FVector3f(0, 1, 0);
				else
					DispatchData.SDFLocalRingAxis = FVector3f(1, 0, 0);

				// [조건부 로그] 첫 프레임만 출력
				static bool bLoggedSDFMode = false;
				if (!bLoggedSDFMode)
				{
					UE_LOG(LogFleshRing, Log, TEXT("[DEBUG] Ring[%d] SDF Mode (Auto): Bounds=(%.1f,%.1f,%.1f)~(%.1f,%.1f,%.1f), RingCenter=(%.1f,%.1f,%.1f), RingRadius=%.2f"),
						RingIndex,
						SDFCache->BoundsMin.X, SDFCache->BoundsMin.Y, SDFCache->BoundsMin.Z,
						SDFCache->BoundsMax.X, SDFCache->BoundsMax.Y, SDFCache->BoundsMax.Z,
						DispatchData.Params.RingCenter.X, DispatchData.Params.RingCenter.Y, DispatchData.Params.RingCenter.Z,
						DispatchData.Params.RingRadius);
					bLoggedSDFMode = true;
				}
			}
			else
			{
				// [조건부 로그] 첫 프레임만 출력
				static bool bLoggedManualMode = false;
				if (!bLoggedManualMode)
				{
					// InfluenceMode 이름 결정
					const TCHAR* InfluenceModeStr = TEXT("Manual");
					if (RingInfluenceMode == EFleshRingInfluenceMode::Auto)
					{
						InfluenceModeStr = TEXT("Auto");
					}
					else if (RingInfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
					{
						InfluenceModeStr = TEXT("ProceduralBand");
					}

					UE_LOG(LogFleshRing, Log, TEXT("[DEBUG] Ring[%d] Manual Mode (InfluenceMode=%s, SDFValid=%s)"),
						RingIndex, InfluenceModeStr,
						(SDFCache && SDFCache->IsValid()) ? TEXT("Yes") : TEXT("No"));
					bLoggedManualMode = true;
				}
			}
		}

		// [조건부 로그] 첫 프레임만 출력
		static bool bLoggedRingInfo = false;
		if (!bLoggedRingInfo)
		{
			UE_LOG(LogFleshRing, Log, TEXT("[DEBUG] Ring[%d]: AffectedVerts=%d, TightnessStrength=%.3f, RingCenter=(%.1f,%.1f,%.1f), RingAxis=(%.3f,%.3f,%.3f), RingRadius=%.2f"),
				RingIndex, DispatchData.Params.NumAffectedVertices, DispatchData.Params.TightnessStrength,
				DispatchData.Params.RingCenter.X, DispatchData.Params.RingCenter.Y, DispatchData.Params.RingCenter.Z,
				DispatchData.Params.RingAxis.X, DispatchData.Params.RingAxis.Y, DispatchData.Params.RingAxis.Z,
				DispatchData.Params.RingRadius);
			bLoggedRingInfo = true;
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
		EFleshRingFalloffType RingBulgeFalloff = EFleshRingFalloffType::WendlandC2;
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(OriginalIdx))
		{
			bBulgeEnabledInSettings = (*RingSettingsPtr)[OriginalIdx].bEnableBulge;
			RingBulgeStrength = (*RingSettingsPtr)[OriginalIdx].BulgeIntensity;
			RingBulgeAxialRange = (*RingSettingsPtr)[OriginalIdx].BulgeAxialRange;
			RingBulgeRadialRange = (*RingSettingsPtr)[OriginalIdx].BulgeRadialRange;
			RingBulgeRadialRatio = (*RingSettingsPtr)[OriginalIdx].BulgeRadialRatio;
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

		// ===== Bulge Provider 선택: SDF 유무에 따라 분기 =====
		if (DispatchData.bHasValidSDF)
		{
			// Auto/ProceduralBand 모드: SDF 바운드 기반 Bulge
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
		else
		{
			// Manual 모드: Ring 파라미터 기반 Bulge
			FManualBulgeProvider BulgeProvider;
			BulgeProvider.InitFromRingParams(
				FVector3f(DispatchData.Params.RingCenter),
				FVector3f(DispatchData.Params.RingAxis),
				DispatchData.Params.RingRadius,
				DispatchData.Params.RingWidth,
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

			// [조건부 로그] 첫 프레임만 출력 (OriginalIdx 사용)
			static TSet<int32> LoggedRings;
			if (!LoggedRings.Contains(OriginalIdx))
			{
				UE_LOG(LogFleshRing, Log, TEXT("[DEBUG] Ring[%d] Bulge 데이터 준비 완료: %d vertices, Strength=%.2f, Direction=%d (Detected=%d)"),
					OriginalIdx, DispatchData.BulgeIndices.Num(), RingBulgeStrength,
					DispatchData.BulgeAxisDirection, DispatchData.DetectedBulgeDirection);
				LoggedRings.Add(OriginalIdx);
			}
		}
	}

	// TightenedBindPose 캐싱 여부 결정
	bool bNeedTightnessCaching = !CurrentLODData.bTightenedBindPoseCached;
	// [조건부 로그] 첫 프레임만 출력
	static bool bLoggedCaching = false;
	if (!bLoggedCaching)
	{
		UE_LOG(LogFleshRing, Log, TEXT("[DEBUG] bNeedTightnessCaching=%d (first frame = will run TightnessCS)"), bNeedTightnessCaching ? 1 : 0);
		bLoggedCaching = true;
	}

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
		WorkItem.bEnableTangentRecompute =
			FleshRingComponent->FleshRingAsset->bEnableTangentRecompute;
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
    }

    if (DirtyRingIndex == INDEX_NONE)
    {
        UE_LOG(LogFleshRing, Log, TEXT("TightnessCache invalidated for ALL rings"));
    }
    else
    {
        UE_LOG(LogFleshRing, Log, TEXT("TightnessCache invalidated for Ring[%d] only"), DirtyRingIndex);
    }
}