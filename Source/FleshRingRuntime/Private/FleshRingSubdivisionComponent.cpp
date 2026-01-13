// FleshRingSubdivisionComponent.cpp
// FleshRing Subdivision Component Implementation

#include "FleshRingSubdivisionComponent.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "FleshRingTypes.h"
#include "FleshRingSubdivisionProcessor.h"
#include "FleshRingSubdivisionShader.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "GameFramework/PlayerController.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "DrawDebugHelpers.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"

#if WITH_EDITOR
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingSubdivision, Log, All);

UFleshRingSubdivisionComponent::UFleshRingSubdivisionComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	bTickInEditor = true;
}

void UFleshRingSubdivisionComponent::BeginPlay()
{
	Super::BeginPlay();
	Initialize();
}

void UFleshRingSubdivisionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Cleanup();
	Super::EndPlay(EndPlayReason);
}

void UFleshRingSubdivisionComponent::OnRegister()
{
	Super::OnRegister();

	if (GetWorld() && GetWorld()->IsGameWorld())
	{
		Initialize();
	}
}

void UFleshRingSubdivisionComponent::OnUnregister()
{
	Cleanup();
	Super::OnUnregister();
}

#if WITH_EDITOR
void UFleshRingSubdivisionComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	// Settings 변경 시 재계산 필요
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UFleshRingSubdivisionComponent, MaxSubdivisionLevel) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UFleshRingSubdivisionComponent, MinEdgeLength) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UFleshRingSubdivisionComponent, InfluenceRadiusMultiplier) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UFleshRingSubdivisionComponent, SubdivisionMode))
	{
		InvalidateCache();
	}
}
#endif

void UFleshRingSubdivisionComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bEnableSubdivision || !bIsInitialized)
	{
		return;
	}

	// 거리 스케일 업데이트
	if (bEnableDistanceFalloff)
	{
		UpdateDistanceScale();
	}
	else
	{
		CurrentDistanceScale = 1.0f;
	}

	// Subdivision 필요한 경우 실행
	if (CurrentDistanceScale > 0.0f && bNeedsRecompute)
	{
		ComputeSubdivision();
		bNeedsRecompute = false;
	}

#if WITH_EDITORONLY_DATA
	// 디버그: Subdivided 버텍스 시각화
	if (bShowSubdividedVertices && Processor.IsValid() && Processor->IsCacheValid())
	{
		DrawSubdividedVerticesDebug();
	}

	// 디버그: Subdivided 와이어프레임 시각화
	if (bShowSubdividedWireframe && Processor.IsValid() && Processor->IsCacheValid())
	{
		DrawSubdividedWireframeDebug();
	}
#endif
}

void UFleshRingSubdivisionComponent::ForceRecompute()
{
	if (Processor.IsValid())
	{
		Processor->InvalidateCache();
	}
	ResultCache.Reset();
	bNeedsRecompute = true;
}

void UFleshRingSubdivisionComponent::InvalidateCache()
{
	if (Processor.IsValid())
	{
		Processor->InvalidateCache();
	}
	ResultCache.Reset();
	bNeedsRecompute = true;
}

int32 UFleshRingSubdivisionComponent::GetOriginalVertexCount() const
{
	if (Processor.IsValid() && Processor->IsCacheValid())
	{
		return Processor->GetCachedResult().OriginalVertexCount;
	}
	return 0;
}

int32 UFleshRingSubdivisionComponent::GetSubdividedVertexCount() const
{
	if (Processor.IsValid() && Processor->IsCacheValid())
	{
		return Processor->GetCachedResult().SubdividedVertexCount;
	}
	return 0;
}

int32 UFleshRingSubdivisionComponent::GetSubdividedTriangleCount() const
{
	if (Processor.IsValid() && Processor->IsCacheValid())
	{
		return Processor->GetCachedResult().SubdividedTriangleCount;
	}
	return 0;
}

void UFleshRingSubdivisionComponent::FindDependencies()
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	// FleshRingComponent 찾기
	FleshRingComp = Owner->FindComponentByClass<UFleshRingComponent>();

	if (!FleshRingComp.IsValid())
	{
		UE_LOG(LogFleshRingSubdivision, Warning,
			TEXT("FleshRingSubdivisionComponent: FleshRingComponent not found on owner '%s'"),
			*Owner->GetName());
	}

	// SkeletalMeshComponent 찾기
	if (FleshRingComp.IsValid())
	{
		TargetMeshComp = FleshRingComp->GetResolvedTargetMesh();
	}

	if (!TargetMeshComp.IsValid())
	{
		TargetMeshComp = Owner->FindComponentByClass<USkeletalMeshComponent>();
	}

	if (!TargetMeshComp.IsValid())
	{
		UE_LOG(LogFleshRingSubdivision, Warning,
			TEXT("FleshRingSubdivisionComponent: SkeletalMeshComponent not found"));
	}
}

void UFleshRingSubdivisionComponent::Initialize()
{
	if (bIsInitialized)
	{
		return;
	}

	FindDependencies();

	if (!TargetMeshComp.IsValid())
	{
		return;
	}

	// Processor 생성
	Processor = MakeUnique<FFleshRingSubdivisionProcessor>();

	// SkeletalMesh에서 소스 데이터 추출
	USkeletalMesh* SkelMesh = TargetMeshComp->GetSkeletalMeshAsset();
	if (SkelMesh && Processor->SetSourceMeshFromSkeletalMesh(SkelMesh, 0))
	{
		bIsInitialized = true;
		bNeedsRecompute = true;

		UE_LOG(LogFleshRingSubdivision, Log,
			TEXT("FleshRingSubdivisionComponent initialized for '%s'"),
			*TargetMeshComp->GetName());
	}
	else
	{
		UE_LOG(LogFleshRingSubdivision, Warning,
			TEXT("FleshRingSubdivisionComponent: Failed to extract mesh data from '%s'"),
			SkelMesh ? *SkelMesh->GetName() : TEXT("null"));
	}
}

void UFleshRingSubdivisionComponent::Cleanup()
{
	ResultCache.Reset();
	Processor.Reset();
	FleshRingComp.Reset();
	TargetMeshComp.Reset();
	bIsInitialized = false;
}

void UFleshRingSubdivisionComponent::UpdateDistanceScale()
{
	if (!TargetMeshComp.IsValid())
	{
		CurrentDistanceScale = 1.0f;
		return;
	}

	FVector CameraLocation = FVector::ZeroVector;

	UWorld* World = GetWorld();
	if (World)
	{
		APlayerController* PC = World->GetFirstPlayerController();
		if (PC)
		{
			FVector CameraLoc;
			FRotator CameraRot;
			PC->GetPlayerViewPoint(CameraLoc, CameraRot);
			CameraLocation = CameraLoc;
		}
	}

	FVector MeshLocation = TargetMeshComp->GetComponentLocation();
	float Distance = FVector::Dist(MeshLocation, CameraLocation);

	if (Distance >= SubdivisionFadeDistance)
	{
		CurrentDistanceScale = 0.0f;
	}
	else if (Distance <= SubdivisionFullDistance)
	{
		CurrentDistanceScale = 1.0f;
	}
	else
	{
		float T = (Distance - SubdivisionFullDistance) / (SubdivisionFadeDistance - SubdivisionFullDistance);
		CurrentDistanceScale = 1.0f - FMath::Clamp(T, 0.0f, 1.0f);
	}
}

void UFleshRingSubdivisionComponent::ComputeSubdivision()
{
	if (!Processor.IsValid() || !FleshRingComp.IsValid())
	{
		return;
	}

	UFleshRingAsset* Asset = FleshRingComp->FleshRingAsset;
	if (!Asset || Asset->Rings.Num() == 0)
	{
		UE_LOG(LogFleshRingSubdivision, Warning,
			TEXT("FleshRingSubdivisionComponent: No rings in FleshRingAsset"));
		return;
	}

	// 첫 번째 Ring 사용 (TODO: 다중 Ring 지원)
	const FFleshRingSettings& Ring = Asset->Rings[0];

	// Ring 파라미터 설정
	FSubdivisionRingParams RingParams;

	// InfluenceMode에 따라 SDF 모드 또는 Manual 모드 결정
	if (Ring.InfluenceMode == EFleshRingInfluenceMode::Auto)
	{
		// Auto 모드: SDF 캐시에서 바운드 정보 사용
		const FRingSDFCache* SDFCache = FleshRingComp->GetRingSDFCache(0);
		if (SDFCache && SDFCache->IsValid())
		{
			RingParams.bUseSDFBounds = true;
			RingParams.SDFBoundsMin = FVector(SDFCache->BoundsMin);
			RingParams.SDFBoundsMax = FVector(SDFCache->BoundsMax);
			RingParams.SDFLocalToComponent = SDFCache->LocalToComponent;
			RingParams.SDFInfluenceMultiplier = InfluenceRadiusMultiplier;

			UE_LOG(LogFleshRingSubdivision, Log,
				TEXT("Using SDF mode - Bounds: [%s] to [%s]"),
				*RingParams.SDFBoundsMin.ToString(),
				*RingParams.SDFBoundsMax.ToString());
		}
		else
		{
			// SDF 캐시가 없으면 Manual 모드로 폴백
			UE_LOG(LogFleshRingSubdivision, Warning,
				TEXT("SDF cache not available, falling back to Manual mode"));
			RingParams.bUseSDFBounds = false;
			RingParams.Center = Ring.RingOffset;
			RingParams.Axis = FVector::UpVector;
			RingParams.Radius = Ring.RingRadius;
			RingParams.Width = Ring.RingHeight;
			RingParams.InfluenceMultiplier = InfluenceRadiusMultiplier;
		}
	}
	else
	{
		// Manual 모드: 기존 기하학적 방식
		RingParams.bUseSDFBounds = false;
		RingParams.Center = Ring.RingOffset;
		RingParams.Axis = FVector::UpVector; // TODO: Bone 방향에서 계산
		RingParams.Radius = Ring.RingRadius;
		RingParams.Width = Ring.RingHeight;
		RingParams.InfluenceMultiplier = InfluenceRadiusMultiplier;
	}

	Processor->SetRingParams(RingParams);

	// Processor 설정
	FSubdivisionProcessorSettings Settings;
	Settings.MaxSubdivisionLevel = MaxSubdivisionLevel;
	Settings.MinEdgeLength = MinEdgeLength;

	switch (SubdivisionMode)
	{
	case EFleshRingSubdivisionMode::BindPoseFixed:
		Settings.Mode = FSubdivisionProcessorSettings::EMode::BindPoseFixed;
		break;
	case EFleshRingSubdivisionMode::DynamicAsync:
		Settings.Mode = FSubdivisionProcessorSettings::EMode::DynamicAsync;
		break;
	case EFleshRingSubdivisionMode::PreSubdivideRegion:
		Settings.Mode = FSubdivisionProcessorSettings::EMode::PreSubdivideRegion;
		Settings.PreSubdivideMargin = PreSubdivideMargin;
		break;
	}

	Processor->SetSettings(Settings);

	// CPU Subdivision 실행
	FSubdivisionTopologyResult TopologyResult;
	if (Processor->Process(TopologyResult))
	{
		// 통계 계산
		const int32 AddedVertices = TopologyResult.SubdividedVertexCount - TopologyResult.OriginalVertexCount;
		const int32 AddedTriangles = TopologyResult.SubdividedTriangleCount - TopologyResult.OriginalTriangleCount;
		const bool bWasSubdivided = (AddedVertices > 0 || AddedTriangles > 0);
		const FString ModeStr = RingParams.bUseSDFBounds ? TEXT("SDF") : TEXT("Manual");

		// 항상 로그 출력 (Subdivision 발생 여부 확인용)
		if (bWasSubdivided)
		{
			UE_LOG(LogFleshRingSubdivision, Log,
				TEXT("[%s] Subdivision SUCCESS - Mode: %s | Vertices: %d -> %d (+%d) | Triangles: %d -> %d (+%d)"),
				*GetOwner()->GetName(),
				*ModeStr,
				TopologyResult.OriginalVertexCount, TopologyResult.SubdividedVertexCount, AddedVertices,
				TopologyResult.OriginalTriangleCount, TopologyResult.SubdividedTriangleCount, AddedTriangles);
		}
		else
		{
			UE_LOG(LogFleshRingSubdivision, Warning,
				TEXT("[%s] Subdivision NO CHANGE - Mode: %s | Vertices: %d | Triangles: %d (영향 영역에 삼각형 없음?)"),
				*GetOwner()->GetName(),
				*ModeStr,
				TopologyResult.OriginalVertexCount,
				TopologyResult.OriginalTriangleCount);
		}

#if WITH_EDITORONLY_DATA
		// 화면 디버그 메시지 (에디터에서만)
		if (bLogSubdivisionStats && GEngine)
		{
			const FColor MsgColor = bWasSubdivided ? FColor::Green : FColor::Yellow;
			GEngine->AddOnScreenDebugMessage(-1, 5.0f, MsgColor,
				FString::Printf(TEXT("Subdivision [%s]: V %d->%d (+%d), T %d->%d (+%d)"),
					*ModeStr,
					TopologyResult.OriginalVertexCount, TopologyResult.SubdividedVertexCount, AddedVertices,
					TopologyResult.OriginalTriangleCount, TopologyResult.SubdividedTriangleCount, AddedTriangles));
		}
#endif

		// GPU 보간 실행
		ExecuteGPUInterpolation();
	}
	else
	{
		UE_LOG(LogFleshRingSubdivision, Warning,
			TEXT("[%s] Subdivision FAILED - CPU subdivision 실패"),
			*GetOwner()->GetName());
	}
}

void UFleshRingSubdivisionComponent::ExecuteGPUInterpolation()
{
	if (!Processor.IsValid() || !Processor->IsCacheValid())
	{
		return;
	}

	const FSubdivisionTopologyResult& TopologyResult = Processor->GetCachedResult();

	// Subdivision이 없으면 스킵
	if (TopologyResult.SubdividedVertexCount <= TopologyResult.OriginalVertexCount)
	{
		UE_LOG(LogFleshRingSubdivision, Log, TEXT("No subdivision occurred, skipping GPU interpolation"));
		return;
	}

	// SkeletalMesh LOD 데이터 접근
	if (!TargetMeshComp.IsValid())
	{
		UE_LOG(LogFleshRingSubdivision, Warning, TEXT("ExecuteGPUInterpolation: TargetMeshComp is invalid"));
		return;
	}

	USkeletalMesh* SkelMesh = TargetMeshComp->GetSkeletalMeshAsset();
	if (!SkelMesh)
	{
		UE_LOG(LogFleshRingSubdivision, Warning, TEXT("ExecuteGPUInterpolation: No SkeletalMesh asset"));
		return;
	}

	FSkeletalMeshRenderData* RenderData = SkelMesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		UE_LOG(LogFleshRingSubdivision, Warning, TEXT("ExecuteGPUInterpolation: No render data available"));
		return;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0]; // LOD 0 사용
	const uint32 SourceVertexCount = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

	// 소스 메시 데이터를 복사 (렌더 스레드에서 Processor 접근 불가)
	TArray<FVector> SourcePositions = Processor->GetSourcePositions();
	TArray<FVector2D> SourceUVs = Processor->GetSourceUVs();

	// Position 수와 SkeletalMesh 버텍스 수가 일치하는지 확인
	if (SourcePositions.Num() != static_cast<int32>(SourceVertexCount))
	{
		UE_LOG(LogFleshRingSubdivision, Warning,
			TEXT("ExecuteGPUInterpolation: Vertex count mismatch (Processor=%d, Mesh=%d) - using defaults"),
			SourcePositions.Num(), SourceVertexCount);
		// 일치하지 않으면 기본값 사용으로 폴백
		goto FallbackToDefaults;
	}

	{
		// ===== Normal 추출 (TangentZ) =====
		TArray<FVector> SourceNormals;
		SourceNormals.SetNum(SourcePositions.Num());
		for (int32 i = 0; i < SourceNormals.Num(); ++i)
		{
			FVector4f TangentZ = LODData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(i);
			SourceNormals[i] = FVector(TangentZ.X, TangentZ.Y, TangentZ.Z);
		}

		// ===== Tangent 추출 (TangentX, w = binormal sign) =====
		TArray<FVector4> SourceTangents;
		SourceTangents.SetNum(SourcePositions.Num());
		for (int32 i = 0; i < SourceTangents.Num(); ++i)
		{
			FVector4f TangentX = LODData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentX(i);
			SourceTangents[i] = FVector4(TangentX.X, TangentX.Y, TangentX.Z, TangentX.W);
		}

		// ===== Bone Weight/Index 추출 =====
		const uint32 NumBoneInfluences = 4;
		TArray<float> SourceBoneWeights;
		TArray<uint32> SourceBoneIndices;
		SourceBoneWeights.SetNumZeroed(SourcePositions.Num() * NumBoneInfluences);
		SourceBoneIndices.SetNumZeroed(SourcePositions.Num() * NumBoneInfluences);

		const FSkinWeightVertexBuffer* SkinWeightBuffer = LODData.GetSkinWeightVertexBuffer();
		if (SkinWeightBuffer && SkinWeightBuffer->GetNumVertices() > 0)
		{
			// 버텍스별 섹션 인덱스 매핑 (BoneMap 변환용)
			TArray<int32> VertexToSectionIndex;
			VertexToSectionIndex.SetNumZeroed(SourcePositions.Num());
			for (int32 SectionIdx = 0; SectionIdx < LODData.RenderSections.Num(); ++SectionIdx)
			{
				const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIdx];
				for (uint32 VertexIdx = Section.BaseVertexIndex;
					 VertexIdx < Section.BaseVertexIndex + Section.NumVertices && VertexIdx < static_cast<uint32>(SourcePositions.Num());
					 ++VertexIdx)
				{
					VertexToSectionIndex[VertexIdx] = SectionIdx;
				}
			}

			for (int32 i = 0; i < SourcePositions.Num(); ++i)
			{
				// 해당 버텍스의 섹션 BoneMap 가져오기
				const TArray<FBoneIndexType>* BoneMap = nullptr;
				int32 SectionIdx = VertexToSectionIndex[i];
				if (SectionIdx >= 0 && SectionIdx < LODData.RenderSections.Num())
				{
					BoneMap = &LODData.RenderSections[SectionIdx].BoneMap;
				}

				for (uint32 j = 0; j < NumBoneInfluences; ++j)
				{
					uint16 LocalBoneIdx = SkinWeightBuffer->GetBoneIndex(i, j);
					uint8 Weight = SkinWeightBuffer->GetBoneWeight(i, j);

					// BoneMap을 사용하여 Local -> Global 본 인덱스 변환
					uint16 GlobalBoneIdx = LocalBoneIdx;
					if (BoneMap && LocalBoneIdx < BoneMap->Num())
					{
						GlobalBoneIdx = (*BoneMap)[LocalBoneIdx];
					}

					// Weight는 0-255를 0.0-1.0으로 변환
					SourceBoneWeights[i * NumBoneInfluences + j] = static_cast<float>(Weight) / 255.0f;
					SourceBoneIndices[i * NumBoneInfluences + j] = static_cast<uint32>(GlobalBoneIdx);
				}
			}
		}
		else
		{
			UE_LOG(LogFleshRingSubdivision, Warning,
				TEXT("ExecuteGPUInterpolation: No SkinWeightBuffer - using default bone weights"));
			for (int32 i = 0; i < SourcePositions.Num(); ++i)
			{
				SourceBoneWeights[i * NumBoneInfluences] = 1.0f; // 첫 번째 본에 100% 가중치
			}
		}

		UE_LOG(LogFleshRingSubdivision, Log,
			TEXT("ExecuteGPUInterpolation: Extracted real mesh data - %d vertices (normals, tangents, bone weights)"),
			SourcePositions.Num());

		// 결과 캐시 포인터 (렌더 스레드에서 접근용)
		FSubdivisionResultCache* ResultCachePtr = &ResultCache;
		const uint32 NumVertices = TopologyResult.SubdividedVertexCount;
		const uint32 NumIndices = TopologyResult.Indices.Num();

		// Render Thread에서 GPU 작업 실행 및 결과 캐싱
		ENQUEUE_RENDER_COMMAND(FleshRingSubdivisionGPU)(
			[TopologyResult, SourcePositions, SourceNormals, SourceTangents, SourceUVs, SourceBoneWeights, SourceBoneIndices,
			 NumBoneInfluences, ResultCachePtr, NumVertices, NumIndices]
			(FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);

				FSubdivisionInterpolationParams Params;
				Params.NumBoneInfluences = NumBoneInfluences;

				FSubdivisionGPUBuffers Buffers;

				// 소스 메시 데이터 업로드
				UploadSourceMeshToGPU(
					GraphBuilder,
					SourcePositions,
					SourceNormals,
					SourceTangents,
					SourceUVs,
					SourceBoneWeights,
					SourceBoneIndices,
					NumBoneInfluences,
					Buffers);

				// 토폴로지 결과에서 GPU 버퍼 생성
				CreateSubdivisionGPUBuffersFromTopology(GraphBuilder, TopologyResult, Params, Buffers);

				// GPU 보간 Dispatch
				DispatchFleshRingBarycentricInterpolationCS(GraphBuilder, Params, Buffers);

				// 결과를 Pooled 버퍼로 변환하여 캐싱
				// RDG 버퍼를 외부 버퍼로 추출 (GraphBuilder.Execute() 후에도 유지됨)
				GraphBuilder.QueueBufferExtraction(Buffers.OutputPositions, &ResultCachePtr->PositionsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputNormals, &ResultCachePtr->NormalsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputTangents, &ResultCachePtr->TangentsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputUVs, &ResultCachePtr->UVsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputIndices, &ResultCachePtr->IndicesBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputBoneWeights, &ResultCachePtr->BoneWeightsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputBoneIndices, &ResultCachePtr->BoneIndicesBuffer);

				GraphBuilder.Execute();

				// 캐시 메타데이터 업데이트
				ResultCachePtr->NumVertices = NumVertices;
				ResultCachePtr->NumIndices = NumIndices;
				ResultCachePtr->bCached = true;

				UE_LOG(LogFleshRingSubdivision, Log,
					TEXT("GPU interpolation complete and cached: %d vertices, %d indices"),
					NumVertices, NumIndices);
			});

		return; // 성공적으로 처리됨
	}

FallbackToDefaults:
	// 폴백: 기본값 사용 (기존 동작)
	{
		UE_LOG(LogFleshRingSubdivision, Warning,
			TEXT("ExecuteGPUInterpolation: Using fallback default values for normals/tangents/bone weights"));

		TArray<FVector> SourceNormals;
		SourceNormals.SetNum(SourcePositions.Num());
		for (int32 i = 0; i < SourceNormals.Num(); ++i)
		{
			SourceNormals[i] = FVector::UpVector;
		}

		TArray<FVector4> SourceTangents;
		SourceTangents.SetNum(SourcePositions.Num());
		for (int32 i = 0; i < SourceTangents.Num(); ++i)
		{
			SourceTangents[i] = FVector4(1.0f, 0.0f, 0.0f, 1.0f);
		}

		const uint32 NumBoneInfluences = 4;
		TArray<float> SourceBoneWeights;
		TArray<uint32> SourceBoneIndices;
		SourceBoneWeights.SetNumZeroed(SourcePositions.Num() * NumBoneInfluences);
		SourceBoneIndices.SetNumZeroed(SourcePositions.Num() * NumBoneInfluences);
		for (int32 i = 0; i < SourcePositions.Num(); ++i)
		{
			SourceBoneWeights[i * NumBoneInfluences] = 1.0f;
		}

		// 결과 캐시 포인터 (렌더 스레드에서 접근용)
		FSubdivisionResultCache* ResultCachePtr = &ResultCache;
		const uint32 NumVertices = TopologyResult.SubdividedVertexCount;
		const uint32 NumIndices = TopologyResult.Indices.Num();

		// Render Thread에서 GPU 작업 실행 및 결과 캐싱
		ENQUEUE_RENDER_COMMAND(FleshRingSubdivisionGPUFallback)(
			[TopologyResult, SourcePositions, SourceNormals, SourceTangents, SourceUVs, SourceBoneWeights, SourceBoneIndices,
			 NumBoneInfluences, ResultCachePtr, NumVertices, NumIndices]
			(FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);

				FSubdivisionInterpolationParams Params;
				Params.NumBoneInfluences = NumBoneInfluences;

				FSubdivisionGPUBuffers Buffers;

				// 소스 메시 데이터 업로드
				UploadSourceMeshToGPU(
					GraphBuilder,
					SourcePositions,
					SourceNormals,
					SourceTangents,
					SourceUVs,
					SourceBoneWeights,
					SourceBoneIndices,
					NumBoneInfluences,
					Buffers);

				// 토폴로지 결과에서 GPU 버퍼 생성
				CreateSubdivisionGPUBuffersFromTopology(GraphBuilder, TopologyResult, Params, Buffers);

				// GPU 보간 Dispatch
				DispatchFleshRingBarycentricInterpolationCS(GraphBuilder, Params, Buffers);

				// 결과를 Pooled 버퍼로 변환하여 캐싱
				GraphBuilder.QueueBufferExtraction(Buffers.OutputPositions, &ResultCachePtr->PositionsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputNormals, &ResultCachePtr->NormalsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputTangents, &ResultCachePtr->TangentsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputUVs, &ResultCachePtr->UVsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputIndices, &ResultCachePtr->IndicesBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputBoneWeights, &ResultCachePtr->BoneWeightsBuffer);
				GraphBuilder.QueueBufferExtraction(Buffers.OutputBoneIndices, &ResultCachePtr->BoneIndicesBuffer);

				GraphBuilder.Execute();

				// 캐시 메타데이터 업데이트
				ResultCachePtr->NumVertices = NumVertices;
				ResultCachePtr->NumIndices = NumIndices;
				ResultCachePtr->bCached = true;

				UE_LOG(LogFleshRingSubdivision, Log,
					TEXT("GPU interpolation (fallback) complete and cached: %d vertices, %d indices"),
					NumVertices, NumIndices);
			});
	}
}

#if WITH_EDITOR

void UFleshRingSubdivisionComponent::BakeSubdividedMesh()
{
	// 1. Subdivision이 계산되어 있는지 확인
	if (!Processor.IsValid() || !Processor->IsCacheValid())
	{
		// 먼저 Subdivision 계산
		ForceRecompute();

		// Tick을 수동으로 호출하여 계산 완료
		TickComponent(0.0f, ELevelTick::LEVELTICK_All, nullptr);

		if (!Processor.IsValid() || !Processor->IsCacheValid())
		{
			UE_LOG(LogFleshRingSubdivision, Error, TEXT("BakeSubdividedMesh: Subdivision 계산 실패"));
			return;
		}
	}

	if (!TargetMeshComp.IsValid())
	{
		UE_LOG(LogFleshRingSubdivision, Error, TEXT("BakeSubdividedMesh: TargetMeshComponent가 유효하지 않음"));
		return;
	}

	USkeletalMesh* SourceMesh = TargetMeshComp->GetSkeletalMeshAsset();
	if (!SourceMesh)
	{
		UE_LOG(LogFleshRingSubdivision, Error, TEXT("BakeSubdividedMesh: SourceMesh가 없음"));
		return;
	}

	const FSubdivisionTopologyResult& Result = Processor->GetCachedResult();

	// Subdivision이 발생하지 않았으면 스킵
	if (Result.SubdividedVertexCount <= Result.OriginalVertexCount)
	{
		UE_LOG(LogFleshRingSubdivision, Warning, TEXT("BakeSubdividedMesh: Subdivision이 발생하지 않음 (새 버텍스 없음)"));
		return;
	}

	UE_LOG(LogFleshRingSubdivision, Log,
		TEXT("BakeSubdividedMesh 시작: %d -> %d vertices, %d -> %d triangles"),
		Result.OriginalVertexCount, Result.SubdividedVertexCount,
		Result.OriginalTriangleCount, Result.SubdividedTriangleCount);

	// TODO: 새 SkeletalMesh 에셋 생성
	// 이 기능은 복잡한 SkeletalMesh 생성 API를 사용해야 하므로
	// FleshRingEditor 모듈에서 구현하는 것이 좋습니다.
	// 현재는 플레이스홀더로 로그만 출력합니다.

	UE_LOG(LogFleshRingSubdivision, Warning,
		TEXT("BakeSubdividedMesh: SkeletalMesh 생성은 FleshRingEditor 모듈에서 구현 필요"));
	UE_LOG(LogFleshRingSubdivision, Log,
		TEXT("  저장 경로: %s"),
		*BakedMeshSavePath);
	UE_LOG(LogFleshRingSubdivision, Log,
		TEXT("  접미사: %s"),
		*BakedMeshSuffix);

	// 에디터 알림
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Yellow,
			FString::Printf(TEXT("BakeSubdividedMesh: %d -> %d vertices (SkeletalMesh 생성은 Editor 모듈에서 구현 필요)"),
				Result.OriginalVertexCount, Result.SubdividedVertexCount));
	}
}

#endif // WITH_EDITOR

#if WITH_EDITORONLY_DATA

void UFleshRingSubdivisionComponent::DrawSubdividedVerticesDebug()
{
	if (!Processor.IsValid() || !Processor->IsCacheValid())
	{
		return;
	}

	if (!TargetMeshComp.IsValid())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FSubdivisionTopologyResult& Result = Processor->GetCachedResult();
	const TArray<FVector>& SourcePositions = Processor->GetSourcePositions();

	if (SourcePositions.Num() == 0)
	{
		return;
	}

	// 월드 트랜스폼 (컴포넌트 스페이스 → 월드 스페이스)
	const FTransform& MeshTransform = TargetMeshComp->GetComponentTransform();

	// 새로 추가된 버텍스만 시각화 (원본 버텍스 제외)
	int32 NewVertexCount = 0;
	for (int32 i = 0; i < Result.VertexData.Num(); ++i)
	{
		const FSubdivisionVertexData& VertexData = Result.VertexData[i];

		// 원본 버텍스는 건너뛰기
		if (VertexData.IsOriginalVertex())
		{
			continue;
		}

		// 부모 버텍스 인덱스 유효성 검사
		const uint32 P0 = VertexData.ParentV0;
		const uint32 P1 = VertexData.ParentV1;
		const uint32 P2 = VertexData.ParentV2;

		if (P0 >= (uint32)SourcePositions.Num() ||
			P1 >= (uint32)SourcePositions.Num() ||
			P2 >= (uint32)SourcePositions.Num())
		{
			continue;
		}

		// Barycentric 보간으로 위치 계산 (컴포넌트 스페이스)
		const FVector3f& Bary = VertexData.BarycentricCoords;
		FVector LocalPosition =
			SourcePositions[P0] * Bary.X +
			SourcePositions[P1] * Bary.Y +
			SourcePositions[P2] * Bary.Z;

		// 월드 스페이스로 변환
		FVector WorldPosition = MeshTransform.TransformPosition(LocalPosition);

		// 흰색 점으로 그리기
		DrawDebugPoint(
			World,
			WorldPosition,
			DebugPointSize,
			FColor::White,
			false,  // bPersistent
			-1.0f   // LifeTime (매 프레임)
		);

		++NewVertexCount;
	}

	// 첫 프레임만 로그 출력
	static bool bFirstFrame = true;
	if (bFirstFrame && NewVertexCount > 0)
	{
		UE_LOG(LogFleshRingSubdivision, Log,
			TEXT("DrawSubdividedVerticesDebug: Drawing %d new vertices (Total: %d, Original: %d)"),
			NewVertexCount,
			Result.VertexData.Num(),
			Result.OriginalVertexCount);
		bFirstFrame = false;
	}
}

void UFleshRingSubdivisionComponent::DrawSubdividedWireframeDebug()
{
	if (!Processor.IsValid() || !Processor->IsCacheValid())
	{
		return;
	}

	if (!TargetMeshComp.IsValid())
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FSubdivisionTopologyResult& Result = Processor->GetCachedResult();
	const TArray<FVector>& SourcePositions = Processor->GetSourcePositions();

	if (SourcePositions.Num() == 0 || Result.Indices.Num() < 3)
	{
		return;
	}

	// 월드 트랜스폼 (컴포넌트 스페이스 → 월드 스페이스)
	const FTransform& MeshTransform = TargetMeshComp->GetComponentTransform();

	// 모든 버텍스 위치를 미리 계산 (원본 + 새 버텍스)
	TArray<FVector> AllPositions;
	AllPositions.SetNum(Result.VertexData.Num());

	for (int32 i = 0; i < Result.VertexData.Num(); ++i)
	{
		const FSubdivisionVertexData& VertexData = Result.VertexData[i];

		const uint32 P0 = VertexData.ParentV0;
		const uint32 P1 = VertexData.ParentV1;
		const uint32 P2 = VertexData.ParentV2;

		// 유효성 검사
		if (P0 >= (uint32)SourcePositions.Num() ||
			P1 >= (uint32)SourcePositions.Num() ||
			P2 >= (uint32)SourcePositions.Num())
		{
			AllPositions[i] = FVector::ZeroVector;
			continue;
		}

		// Barycentric 보간으로 위치 계산
		const FVector3f& Bary = VertexData.BarycentricCoords;
		FVector LocalPosition =
			SourcePositions[P0] * Bary.X +
			SourcePositions[P1] * Bary.Y +
			SourcePositions[P2] * Bary.Z;

		// 월드 스페이스로 변환
		AllPositions[i] = MeshTransform.TransformPosition(LocalPosition);
	}

	// 모든 삼각형의 엣지를 빨간색 선으로 그리기
	const int32 NumTriangles = Result.Indices.Num() / 3;

	// 새로운 삼각형만 그리기 (원본 삼각형 수 이후)
	const int32 OriginalTriCount = Result.OriginalTriangleCount;

	for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
	{
		const uint32 I0 = Result.Indices[TriIdx * 3 + 0];
		const uint32 I1 = Result.Indices[TriIdx * 3 + 1];
		const uint32 I2 = Result.Indices[TriIdx * 3 + 2];

		if (I0 >= (uint32)AllPositions.Num() ||
			I1 >= (uint32)AllPositions.Num() ||
			I2 >= (uint32)AllPositions.Num())
		{
			continue;
		}

		const FVector& V0 = AllPositions[I0];
		const FVector& V1 = AllPositions[I1];
		const FVector& V2 = AllPositions[I2];

		// 새로 추가된 삼각형은 빨간색, 원본은 초록색
		// 원본 삼각형과 새 삼각형을 구분하기 어려우므로
		// 새 버텍스를 포함하는지로 판단
		const bool bHasNewVertex =
			!Result.VertexData[I0].IsOriginalVertex() ||
			!Result.VertexData[I1].IsOriginalVertex() ||
			!Result.VertexData[I2].IsOriginalVertex();

		FColor LineColor = bHasNewVertex ? FColor::Red : FColor::Green;

		// 삼각형 3개의 엣지 그리기
		DrawDebugLine(World, V0, V1, LineColor, false, -1.0f, 0, 1.0f);
		DrawDebugLine(World, V1, V2, LineColor, false, -1.0f, 0, 1.0f);
		DrawDebugLine(World, V2, V0, LineColor, false, -1.0f, 0, 1.0f);
	}
}
#endif
