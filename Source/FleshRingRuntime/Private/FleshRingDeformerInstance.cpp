// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingDeformerInstance.h"
#include "FleshRingDeformer.h"
#include "FleshRingComponent.h"
#include "FleshRingTightnessShader.h"
#include "FleshRingSkinningShader.h"
#include "Components/SkinnedMeshComponent.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "SkeletalMeshDeformerHelpers.h"
#include "SkeletalRenderPublic.h"
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

	const int32 LODIndex = SkinnedMeshComp->GetPredictedLODLevel();

	// Track LOD changes for invalidating previous position
	bool bInvalidatePreviousPosition = false;
	if (LODIndex != LastLodIndex)
	{
		bInvalidatePreviousPosition = true;
		LastLodIndex = LODIndex;

		// LOD 변경 시 TightenedBindPose 캐시 무효화
		// Invalidate TightenedBindPose cache on LOD change
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
	// 렌더 스레드용 데이터 캡처
	// ================================================================
	const TArray<FRingAffectedData>& AllRingData = AffectedVerticesManager.GetAllRingData();
	const uint32 TotalVertexCount = CachedSourcePositions.Num() / 3;

	// 각 Ring 데이터를 TSharedPtr로 캡처 (렌더 스레드 안전)
	struct FRingDispatchData
	{
		FTightnessDispatchParams Params;
		TArray<uint32> Indices;
		TArray<float> Influences;
	};
	TSharedPtr<TArray<FRingDispatchData>> RingDispatchDataPtr = MakeShared<TArray<FRingDispatchData>>();
	RingDispatchDataPtr->Reserve(AllRingData.Num());

	for (const FRingAffectedData& RingData : AllRingData)
	{
		if (RingData.Vertices.Num() == 0)
		{
			continue;
		}

		FRingDispatchData DispatchData;
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

	// 소스 버텍스 데이터를 공유 포인터로 캡처
	TSharedPtr<TArray<float>> SourceDataPtr = MakeShared<TArray<float>>(CachedSourcePositions);

	// ================================================================
	// TightenedBindPose 캐싱 여부 결정
	// ================================================================
	bool bNeedTightnessCaching = !bTightenedBindPoseCached;
	if (bNeedTightnessCaching)
	{
		// 낙관적으로 캐싱 완료 플래그 설정 (렌더 스레드에서 실제 캐싱)
		bTightenedBindPoseCached = true;
		CachedTightnessVertexCount = TotalVertexCount;

		// 캐싱 전환 시점: Previous Position 무효화 (모션블러/TAA 잔상 방지)
		// Invalidate previous position on cache transition to prevent motion blur artifacts
		bInvalidatePreviousPosition = true;

		UE_LOG(LogFleshRing, Log, TEXT("TightenedBindPose 캐싱 시작 (%d 버텍스) - Previous Position 무효화"), TotalVertexCount);
	}

	// 캐시 버퍼 포인터 캡처 (렌더 스레드에서 사용)
	TRefCountPtr<FRDGPooledBuffer>* CachedBufferPtr = &CachedTightenedBindPose;

	ENQUEUE_RENDER_COMMAND(FleshRingDeformer)(
		[MeshObject, LODIndex, TotalVertexCount, SourceDataPtr, RingDispatchDataPtr,
		 bInvalidatePreviousPosition, bNeedTightnessCaching, CachedBufferPtr,
		 FallbackDelegate = InDesc.FallbackDelegate]
		(FRHICommandListImmediate& RHICmdList)
	{
		// ============================================
		// 유효성 검사
		// ============================================
		if (!MeshObject || LODIndex < 0)
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		FSkeletalMeshRenderData const& RenderData = MeshObject->GetSkeletalMeshRenderData();
		if (LODIndex >= RenderData.LODRenderData.Num())
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		const FSkeletalMeshLODRenderData& LODData = RenderData.LODRenderData[LODIndex];
		if (LODData.RenderSections.Num() == 0 || !LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices())
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		const int32 FirstAvailableSection = FSkeletalMeshDeformerHelpers::GetIndexOfFirstAvailableSection(MeshObject, LODIndex);
		if (FirstAvailableSection == INDEX_NONE)
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		const uint32 ActualNumVertices = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();
		const uint32 ActualBufferSize = ActualNumVertices * 3;

		if (TotalVertexCount != ActualNumVertices)
		{
			UE_LOG(LogTemp, Warning, TEXT("FleshRing: 버텍스 수 불일치 - 캐시:%d, 실제:%d"), TotalVertexCount, ActualNumVertices);
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// ============================================
		// RDG 빌더 생성 및 버퍼 할당
		// ============================================
		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGExternalAccessQueue ExternalAccessQueue;

		// VertexFactory용 출력 버퍼 할당
		FRDGBuffer* OutputPositionBuffer = FSkeletalMeshDeformerHelpers::AllocateVertexFactoryPositionBuffer(
			GraphBuilder, ExternalAccessQueue, MeshObject, LODIndex, TEXT("FleshRingOutput"));

		if (!OutputPositionBuffer)
		{
			ExternalAccessQueue.Submit(GraphBuilder);
			GraphBuilder.Execute();
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// ============================================
		// 캐싱 분기: 첫 프레임 vs 이후 프레임
		// ============================================
		FRDGBufferRef TightenedBindPoseBuffer = nullptr;  // 공통 사용을 위해 바깥에서 선언

		if (bNeedTightnessCaching)
		{
			// ========================================
			// 첫 프레임: TightnessCS 실행 + 캐싱
			// ========================================
			UE_LOG(LogFleshRing, Log, TEXT("FleshRing: 첫 프레임 - TightnessCS 실행 및 TightenedBindPose 캐싱"));

			// 소스 버텍스 버퍼 생성 및 업로드
			FRDGBufferRef SourceBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
				TEXT("FleshRing_SourcePositions")
			);
			GraphBuilder.QueueBufferUpload(
				SourceBuffer,
				SourceDataPtr->GetData(),
				ActualBufferSize * sizeof(float),
				ERDGInitialDataFlags::None
			);

			// TightenedBindPose 버퍼 생성 (영구 캐싱용)
			TightenedBindPoseBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
				TEXT("FleshRing_TightenedBindPose")
			);

			// 소스를 TightenedBindPose에 복사 (영향 안 받는 버텍스 보존)
			AddCopyBufferPass(GraphBuilder, TightenedBindPoseBuffer, SourceBuffer);

			// 각 Ring별 TightnessCS 디스패치 (TightenedBindPose에 직접 쓰기)
			for (const FRingDispatchData& DispatchData : *RingDispatchDataPtr)
			{
				const FTightnessDispatchParams& Params = DispatchData.Params;
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

				// TightnessCS: 소스 → TightenedBindPose (스키닝 없음)
				DispatchFleshRingTightnessCS(
					GraphBuilder,
					Params,
					SourceBuffer,
					IndicesBuffer,
					InfluencesBuffer,
					TightenedBindPoseBuffer  // TightenedBindPose에 직접 쓰기
				);
			}

			// TightenedBindPose를 영구 버퍼로 변환하여 캐싱
			*CachedBufferPtr = GraphBuilder.ConvertToExternalBuffer(TightenedBindPoseBuffer);

			UE_LOG(LogFleshRing, Log, TEXT("FleshRing: 첫 프레임 - TightenedBindPose 캐싱 완료, 스키닝 적용"));
			// 첫 프레임에서도 스키닝 적용 (아래 공통 코드로 진행)
		}
		else
		{
			// 이후 프레임: 캐싱된 TightenedBindPose를 RDG에 등록
			TightenedBindPoseBuffer = GraphBuilder.RegisterExternalBuffer(*CachedBufferPtr);
		}

		// ========================================
		// 공통: TightenedBindPose에 스키닝 적용 (첫 프레임 + 이후 프레임)
		// ========================================

		// 캐싱된 버퍼 유효성 검사
		if (!CachedBufferPtr->IsValid())
		{
			UE_LOG(LogFleshRing, Warning, TEXT("FleshRing: 캐싱된 TightenedBindPose 버퍼가 유효하지 않음"));
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// 웨이트 스트림 SRV 가져오기 (LOD 전체 공유)
		const FSkinWeightVertexBuffer* WeightBuffer = LODData.GetSkinWeightVertexBuffer();
		FRHIShaderResourceView* InputWeightStreamSRV = WeightBuffer ?
			WeightBuffer->GetDataVertexBuffer()->GetSRV() : nullptr;

		// 원본 Tangent 버퍼 SRV 가져오기 (바인드 포즈의 Normal/Tangent)
		FRHIShaderResourceView* SourceTangentsSRV = LODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetTangentsSRV();

		if (!InputWeightStreamSRV)
		{
			UE_LOG(LogFleshRing, Warning, TEXT("FleshRing: 웨이트 스트림 SRV 없음 - TightenedBindPose 직접 복사"));
			AddCopyBufferPass(GraphBuilder, OutputPositionBuffer, TightenedBindPoseBuffer);
		}
		else
		{
			// ========================================
			// Optimus 방식: Position + Tangent 스키닝
			// PF_R16G16B16A16_SNORM + GpuSkinCommon.ush
			// ========================================

			// Tangent 출력 버퍼 할당 (Optimus 방식)
			FRDGBuffer* OutputTangentBuffer = FSkeletalMeshDeformerHelpers::AllocateVertexFactoryTangentBuffer(
				GraphBuilder, ExternalAccessQueue, MeshObject, LODIndex, TEXT("FleshRingTangentOutput"));

			const int32 NumSections = LODData.RenderSections.Num();
			UE_LOG(LogFleshRing, Log, TEXT("FleshRing: Position+Tangent 스키닝 (%d Sections, 캐싱=%s)"),
				NumSections, bNeedTightnessCaching ? TEXT("첫프레임") : TEXT("이후"));

			for (int32 SectionIndex = 0; SectionIndex < NumSections; ++SectionIndex)
			{
				const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIndex];

				// 이 Section의 BoneMatrices SRV 가져오기
				FRHIShaderResourceView* BoneMatricesSRV = FSkeletalMeshDeformerHelpers::GetBoneBufferForReading(
					MeshObject, LODIndex, SectionIndex, false);

				if (!BoneMatricesSRV)
				{
					UE_LOG(LogFleshRing, Warning, TEXT("FleshRing: Section %d - BoneMatrices SRV 없음, 스킵"), SectionIndex);
					continue;
				}

				// 스키닝 파라미터 설정 (Section별)
				FSkinningDispatchParams SkinParams;
				SkinParams.BaseVertexIndex = Section.BaseVertexIndex;
				SkinParams.NumVertices = Section.NumVertices;
				SkinParams.InputWeightStride = WeightBuffer->GetConstantInfluencesVertexStride();
				SkinParams.InputWeightIndexSize = WeightBuffer->GetBoneIndexByteSize() |
					(WeightBuffer->GetBoneWeightByteSize() << 8);
				SkinParams.NumBoneInfluences = WeightBuffer->GetMaxBoneInfluences();

				// SkinningCS 디스패치: Position + Tangent 처리
				DispatchFleshRingSkinningCS(
					GraphBuilder,
					SkinParams,
					TightenedBindPoseBuffer,
					SourceTangentsSRV,
					OutputPositionBuffer,
					OutputTangentBuffer,  // Optimus 방식으로 할당된 탄젠트 버퍼
					BoneMatricesSRV,
					InputWeightStreamSRV
				);
			}
		}

		// ============================================
		// VertexFactory 버퍼 업데이트 및 실행
		// ============================================
		FSkeletalMeshDeformerHelpers::UpdateVertexFactoryBufferOverrides(GraphBuilder, MeshObject, LODIndex, bInvalidatePreviousPosition);

		ExternalAccessQueue.Submit(GraphBuilder);
		GraphBuilder.Execute();
	});
}

EMeshDeformerOutputBuffer UFleshRingDeformerInstance::GetOutputBuffers() const
{
	// Optimus 방식 적용: Position + Tangents 출력
	// PF_R16G16B16A16_SNORM format with GpuSkinCommon.ush macros
	return EMeshDeformerOutputBuffer::SkinnedMeshPosition | EMeshDeformerOutputBuffer::SkinnedMeshTangents;
}