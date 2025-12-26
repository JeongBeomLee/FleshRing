// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingDeformerInstance.h"
#include "FleshRingDeformer.h"
#include "FleshRingComponent.h"
#include "FleshRingTightnessShader.h"
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
	// Nothing to release for now
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

	ENQUEUE_RENDER_COMMAND(FleshRingTightnessDeformer)(
		[MeshObject, LODIndex, TotalVertexCount, SourceDataPtr, RingDispatchDataPtr, bInvalidatePreviousPosition, FallbackDelegate = InDesc.FallbackDelegate]
		(FRHICommandListImmediate& RHICmdList)
	{
		// Validate LOD index before proceeding
		if (!MeshObject || LODIndex < 0)
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// Check if render data is valid
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

		// Get TightnessCS shader
		TShaderMapRef<FFleshRingTightnessCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		if (!ComputeShader.IsValid())
		{
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// ============================================
		// RDG 빌더 생성 및 버퍼 할당
		// ============================================
		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGExternalAccessQueue ExternalAccessQueue;

		// VertexFactory용 출력 버퍼 할당
		FRDGBuffer* PositionBuffer = FSkeletalMeshDeformerHelpers::AllocateVertexFactoryPositionBuffer(
			GraphBuilder, ExternalAccessQueue, MeshObject, LODIndex, TEXT("FleshRingTightnessPosition"));

		if (!PositionBuffer)
		{
			ExternalAccessQueue.Submit(GraphBuilder);
			GraphBuilder.Execute();
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// ============================================
		// 실제 LOD 버텍스 수 확인 (렌더 스레드에서)
		// ============================================
		const uint32 ActualNumVertices = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();
		const uint32 ActualBufferSize = ActualNumVertices * 3;

		// 캐싱된 데이터와 실제 LOD 버텍스 수가 다르면 스킵
		if (TotalVertexCount != ActualNumVertices)
		{
			UE_LOG(LogTemp, Warning, TEXT("FleshRing: 버텍스 수 불일치 - 캐시:%d, 실제:%d"), TotalVertexCount, ActualNumVertices);
			ExternalAccessQueue.Submit(GraphBuilder);
			GraphBuilder.Execute();
			FallbackDelegate.ExecuteIfBound();
			return;
		}

		// ============================================
		// 소스 버텍스 버퍼 생성 및 업로드
		// ============================================
		FRDGBufferRef SourceBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), ActualBufferSize),
			TEXT("FleshRingTightness_SourcePositions")
		);
		GraphBuilder.QueueBufferUpload(
			SourceBuffer,
			SourceDataPtr->GetData(),
			ActualBufferSize * sizeof(float),
			ERDGInitialDataFlags::None
		);

		// 소스를 출력 버퍼에 복사 (영향 안 받는 버텍스 보존)
		// T-Pose 테스트: 바인드 포즈를 그대로 출력 버퍼에 복사
		AddCopyBufferPass(GraphBuilder, PositionBuffer, SourceBuffer);

		// ============================================
		// 각 Ring별 TightnessCS 디스패치
		// ============================================
		UE_LOG(LogTemp, Log, TEXT("FleshRing: TightnessCS 디스패치 시작 - %d개 Ring"), RingDispatchDataPtr->Num());

		for (const FRingDispatchData& DispatchData : *RingDispatchDataPtr)
		{
			const FTightnessDispatchParams& Params = DispatchData.Params;

			if (Params.NumAffectedVertices == 0)
			{
				continue;
			}

			UE_LOG(LogTemp, Log, TEXT("FleshRing: Ring 디스패치 - %d개 버텍스, Strength=%.2f"),
				Params.NumAffectedVertices, Params.TightnessStrength);

			// ====== 디버그: Influence 값 분석 ======
			float MinInfluence = FLT_MAX;
			float MaxInfluence = -FLT_MAX;
			float SumInfluence = 0.0f;
			for (int32 i = 0; i < DispatchData.Influences.Num(); ++i)
			{
				float Inf = DispatchData.Influences[i];
				MinInfluence = FMath::Min(MinInfluence, Inf);
				MaxInfluence = FMath::Max(MaxInfluence, Inf);
				SumInfluence += Inf;
			}
			float AvgInfluence = DispatchData.Influences.Num() > 0 ? SumInfluence / DispatchData.Influences.Num() : 0.0f;
			UE_LOG(LogTemp, Warning, TEXT("FleshRing: Influence 분석 - Min=%.4f, Max=%.4f, Avg=%.4f"),
				MinInfluence, MaxInfluence, AvgInfluence);
			UE_LOG(LogTemp, Warning, TEXT("FleshRing: 예상 최대 변위 = Strength(%.2f) * MaxInfluence(%.4f) = %.4f"),
				Params.TightnessStrength, MaxInfluence, Params.TightnessStrength * MaxInfluence);
			UE_LOG(LogTemp, Warning, TEXT("FleshRing: RingCenter=(%.2f,%.2f,%.2f), RingAxis=(%.2f,%.2f,%.2f)"),
				Params.RingCenter.X, Params.RingCenter.Y, Params.RingCenter.Z,
				Params.RingAxis.X, Params.RingAxis.Y, Params.RingAxis.Z);

			// AffectedIndices 버퍼
			FRDGBufferRef IndicesBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Params.NumAffectedVertices),
				TEXT("FleshRingTightness_AffectedIndices")
			);
			GraphBuilder.QueueBufferUpload(
				IndicesBuffer,
				DispatchData.Indices.GetData(),
				DispatchData.Indices.Num() * sizeof(uint32),
				ERDGInitialDataFlags::None
			);

			// Influences 버퍼
			FRDGBufferRef InfluencesBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Params.NumAffectedVertices),
				TEXT("FleshRingTightness_Influences")
			);
			GraphBuilder.QueueBufferUpload(
				InfluencesBuffer,
				DispatchData.Influences.GetData(),
				DispatchData.Influences.Num() * sizeof(float),
				ERDGInitialDataFlags::None
			);

			// TightnessCS 디스패치
			DispatchFleshRingTightnessCS(
				GraphBuilder,
				Params,
				SourceBuffer,
				IndicesBuffer,
				InfluencesBuffer,
				PositionBuffer
			);
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
	return EMeshDeformerOutputBuffer::SkinnedMeshPosition;
}