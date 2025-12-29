// FleshRingSubdivisionComponent.cpp
// FleshRing Subdivision Component Implementation

#include "FleshRingSubdivisionComponent.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "FleshRingSubdivisionProcessor.h"
#include "FleshRingSubdivisionShader.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

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
	RingParams.Center = Ring.RingOffset;
	RingParams.Axis = FVector::UpVector; // TODO: Bone 방향에서 계산
	RingParams.Radius = Ring.RingRadius;
	RingParams.Width = Ring.RingWidth;
	RingParams.InfluenceMultiplier = InfluenceRadiusMultiplier;

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
#if WITH_EDITORONLY_DATA
		if (bLogSubdivisionStats)
		{
			UE_LOG(LogFleshRingSubdivision, Log,
				TEXT("Subdivision complete: %d -> %d vertices, %d -> %d triangles"),
				TopologyResult.OriginalVertexCount, TopologyResult.SubdividedVertexCount,
				TopologyResult.OriginalTriangleCount, TopologyResult.SubdividedTriangleCount);
		}
#endif

		// GPU 보간 실행
		ExecuteGPUInterpolation();
	}
	else
	{
		UE_LOG(LogFleshRingSubdivision, Warning,
			TEXT("FleshRingSubdivisionComponent: CPU subdivision failed"));
	}
}

void UFleshRingSubdivisionComponent::ExecuteGPUInterpolation()
{
	if (!Processor.IsValid() || !Processor->IsCacheValid())
	{
		return;
	}

	const FSubdivisionTopologyResult& TopologyResult = Processor->GetCachedResult();

	// 소스 메시 데이터를 복사 (렌더 스레드에서 Processor 접근 불가)
	TArray<FVector> SourcePositions = Processor->GetSourcePositions();
	TArray<FVector2D> SourceUVs = Processor->GetSourceUVs();

	// 노멀은 SkeletalMesh에서 별도 추출 필요 - 현재는 기본값 사용
	TArray<FVector> SourceNormals;
	SourceNormals.SetNum(SourcePositions.Num());
	for (int32 i = 0; i < SourceNormals.Num(); ++i)
	{
		SourceNormals[i] = FVector::UpVector;
	}

	// Bone Weight/Index는 현재 기본값 사용 (TODO: SkeletalMesh에서 추출)
	const uint32 NumBoneInfluences = 4;
	TArray<float> SourceBoneWeights;
	TArray<uint32> SourceBoneIndices;
	SourceBoneWeights.SetNumZeroed(SourcePositions.Num() * NumBoneInfluences);
	SourceBoneIndices.SetNumZeroed(SourcePositions.Num() * NumBoneInfluences);
	for (int32 i = 0; i < SourcePositions.Num(); ++i)
	{
		SourceBoneWeights[i * NumBoneInfluences] = 1.0f; // 첫 번째 본에 100% 가중치
	}

	// Render Thread에서 GPU 작업 실행
	ENQUEUE_RENDER_COMMAND(FleshRingSubdivisionGPU)(
		[TopologyResult, SourcePositions, SourceNormals, SourceUVs, SourceBoneWeights, SourceBoneIndices, NumBoneInfluences]
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
				SourceUVs,
				SourceBoneWeights,
				SourceBoneIndices,
				NumBoneInfluences,
				Buffers);

			// 토폴로지 결과에서 GPU 버퍼 생성
			CreateSubdivisionGPUBuffersFromTopology(GraphBuilder, TopologyResult, Params, Buffers);

			// GPU 보간 Dispatch
			DispatchFleshRingBarycentricInterpolationCS(GraphBuilder, Params, Buffers);

			GraphBuilder.Execute();
		});
}
