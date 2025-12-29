// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingDeformerInstance.h"
#include "FleshRingDeformer.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
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
		}
		Data.bTightenedBindPoseCached = false;
		Data.CachedTightnessVertexCount = 0;

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
		DispatchData.Params = CreateTightnessParams(RingData, TotalVertexCount);
		DispatchData.Indices = RingData.PackedIndices;
		DispatchData.Influences = RingData.PackedInfluences;

		// Ring별 InfluenceMode 확인
		EFleshRingInfluenceMode RingInfluenceMode = EFleshRingInfluenceMode::Auto;
		if (RingSettingsPtr && RingSettingsPtr->IsValidIndex(RingIndex))
		{
			RingInfluenceMode = (*RingSettingsPtr)[RingIndex].InfluenceMode;
		}

		// SDF 캐시 데이터 전달 (렌더 스레드로 안전하게 복사)
		// Auto 모드 + SDF 유효할 때만 SDF 모드 사용
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

				UE_LOG(LogFleshRing, Log, TEXT("[DEBUG] Ring[%d] SDF Mode (Auto): Bounds=(%.1f,%.1f,%.1f)~(%.1f,%.1f,%.1f), RingCenter=(%.1f,%.1f,%.1f), RingRadius=%.2f"),
					RingIndex,
					SDFCache->BoundsMin.X, SDFCache->BoundsMin.Y, SDFCache->BoundsMin.Z,
					SDFCache->BoundsMax.X, SDFCache->BoundsMax.Y, SDFCache->BoundsMax.Z,
					DispatchData.Params.RingCenter.X, DispatchData.Params.RingCenter.Y, DispatchData.Params.RingCenter.Z,
					DispatchData.Params.RingRadius);
			}
			else
			{
				UE_LOG(LogFleshRing, Log, TEXT("[DEBUG] Ring[%d] Manual Mode (InfluenceMode=%s, SDFValid=%s)"),
					RingIndex,
					RingInfluenceMode == EFleshRingInfluenceMode::Auto ? TEXT("Auto") : TEXT("Manual"),
					(SDFCache && SDFCache->IsValid()) ? TEXT("Yes") : TEXT("No"));
			}
		}

		UE_LOG(LogFleshRing, Log, TEXT("[DEBUG] Ring[%d]: AffectedVerts=%d, TightnessStrength=%.3f, RingCenter=(%.1f,%.1f,%.1f), RingAxis=(%.3f,%.3f,%.3f), RingRadius=%.2f"),
			RingIndex, DispatchData.Params.NumAffectedVertices, DispatchData.Params.TightnessStrength,
			DispatchData.Params.RingCenter.X, DispatchData.Params.RingCenter.Y, DispatchData.Params.RingCenter.Z,
			DispatchData.Params.RingAxis.X, DispatchData.Params.RingAxis.Y, DispatchData.Params.RingAxis.Z,
			DispatchData.Params.RingRadius);

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
	bool bNeedTightnessCaching = !CurrentLODData.bTightenedBindPoseCached;
	UE_LOG(LogFleshRing, Log, TEXT("[DEBUG] bNeedTightnessCaching=%d (first frame = will run TightnessCS)"), bNeedTightnessCaching ? 1 : 0);

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
	}

	// 작업 아이템 생성
	FFleshRingWorkItem WorkItem;
	WorkItem.DeformerInstance = this;
	WorkItem.MeshObject = MeshObject;
	WorkItem.LODIndex = LODIndex;
	WorkItem.TotalVertexCount = TotalVertexCount;
	WorkItem.SourceDataPtr = MakeShared<TArray<float>>(CurrentLODData.CachedSourcePositions);
	WorkItem.RingDispatchDataPtr = RingDispatchDataPtr;
	WorkItem.bNeedTightnessCaching = bNeedTightnessCaching;
	WorkItem.bInvalidatePreviousPosition = bInvalidatePreviousPosition;
	WorkItem.CachedBufferSharedPtr = CurrentLODData.CachedTightenedBindPoseShared;  // TSharedPtr 복사 (ref count 증가)
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