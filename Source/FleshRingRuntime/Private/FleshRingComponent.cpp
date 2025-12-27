// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "FleshRingMeshExtractor.h"
#include "FleshRingSDF.h"
#include "Engine/StaticMesh.h"
#include "Engine/VolumeTexture.h"
#include "GameFramework/Actor.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include "TextureResource.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingComponent, Log, All);

// Helper: 본의 바인드 포즈 트랜스폼 획득 (컴포넌트 스페이스)
static FTransform GetBoneBindPoseTransform(USkeletalMeshComponent* SkelMesh, FName BoneName)
{
	if (!SkelMesh || BoneName.IsNone())
	{
		return FTransform::Identity;
	}

	const USkeletalMesh* SkeletalMesh = SkelMesh->GetSkeletalMeshAsset();
	if (!SkeletalMesh)
	{
		return FTransform::Identity;
	}

	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
	const int32 BoneIndex = RefSkeleton.FindBoneIndex(BoneName);

	if (BoneIndex == INDEX_NONE)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("GetBoneBindPoseTransform: Bone '%s' not found"), *BoneName.ToString());
		return FTransform::Identity;
	}

	// Component Space Transform 계산 (부모 체인 포함)
	// RefBonePose는 부모 기준 로컬이므로 체인을 따라 누적해야 함
	FTransform ComponentSpaceTransform = FTransform::Identity;
	int32 CurrentIndex = BoneIndex;

	while (CurrentIndex != INDEX_NONE)
	{
		const FTransform& LocalTransform = RefSkeleton.GetRefBonePose()[CurrentIndex];
		ComponentSpaceTransform = ComponentSpaceTransform * LocalTransform;
		CurrentIndex = RefSkeleton.GetParentIndex(CurrentIndex);
	}

	return ComponentSpaceTransform;
}

UFleshRingComponent::UFleshRingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UFleshRingComponent::BeginPlay()
{
	Super::BeginPlay();

	if (bEnableFleshRing)
	{
		ResolveTargetMesh();
		SetupDeformer();
		GenerateSDF();
		// Ring 메시는 OnRegister()에서 이미 설정됨
	}
}

void UFleshRingComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Ring 메시는 OnUnregister()에서 정리됨
	CleanupDeformer();
	Super::EndPlay(EndPlayReason);
}

void UFleshRingComponent::OnRegister()
{
	Super::OnRegister();

	// 에디터 및 런타임 모두에서 Ring 메시 설정
	// OnRegister는 컴포넌트가 월드에 등록될 때 호출됨 (에디터 포함)
	ResolveTargetMesh();
	SetupRingMeshes();
}

void UFleshRingComponent::OnUnregister()
{
	CleanupRingMeshes();
	Super::OnUnregister();
}

#if WITH_EDITOR
void UFleshRingComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// FleshRingAsset이나 관련 프로퍼티 변경 시 Ring 메시 재설정
	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UFleshRingComponent, FleshRingAsset) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(UFleshRingComponent, bEnableFleshRing))
	{
		ResolveTargetMesh();
		SetupRingMeshes();
	}
}
#endif

void UFleshRingComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!bEnableFleshRing)
	{
		return;
	}

	// NOTE: MarkRenderDynamicDataDirty/MarkRenderTransformDirty는 TickComponent에서 호출하지 않음
	// Optimus 방식: 엔진의 SendRenderDynamicData_Concurrent()가 자동으로 deformer의 EnqueueWork를 호출
	// 초기화 시점(SetupDeformer)에서만 MarkRenderStateDirty/MarkRenderDynamicDataDirty 호출

	// SDF 업데이트 (각 Ring의 UpdateMode에 따라 GenerateSDF에서 처리)
	if (FleshRingAsset)
	{
		// OnTick 모드인 Ring이 있으면 SDF 갱신
		for (const FFleshRingSettings& Ring : FleshRingAsset->Rings)
		{
			if (Ring.SdfSettings.UpdateMode == EFleshRingSdfUpdateMode::OnTick)
			{
				GenerateSDF();
				break;
			}
		}
	}
}

void UFleshRingComponent::ResolveTargetMesh()
{
	// 수동 지정 모드
	if (bUseCustomTarget)
	{
		if (CustomTargetMesh)
		{
			ResolvedTargetMesh = CustomTargetMesh;
			UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Using custom target mesh '%s'"),
				*CustomTargetMesh->GetName());
		}
		else
		{
			UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: bUseCustomTarget is true but CustomTargetMesh is null"));
		}
		return;
	}

	// 자동 탐색 모드: Owner에서 SkeletalMeshComponent 찾기
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: No owner actor found"));
		return;
	}

	// Owner의 모든 컴포넌트에서 SkeletalMeshComponent 탐색
	TArray<USkeletalMeshComponent*> SkeletalMeshComponents;
	Owner->GetComponents<USkeletalMeshComponent>(SkeletalMeshComponents);

	if (SkeletalMeshComponents.Num() == 0)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: No SkeletalMeshComponent found on owner '%s'"),
			*Owner->GetName());
		return;
	}

	// 첫 번째 SkeletalMeshComponent 사용
	ResolvedTargetMesh = SkeletalMeshComponents[0];
	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Auto-discovered target mesh '%s' on owner '%s'"),
		*SkeletalMeshComponents[0]->GetName(), *Owner->GetName());

	if (SkeletalMeshComponents.Num() > 1)
	{
		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Found %d SkeletalMeshComponents, using first one. Use bUseCustomTarget for manual selection."),
			SkeletalMeshComponents.Num());
	}
}

void UFleshRingComponent::SetupDeformer()
{
	USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
	if (!TargetMesh)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: Cannot setup deformer - no target mesh"));
		return;
	}

	// 내부 Deformer 생성
	InternalDeformer = NewObject<UFleshRingDeformer>(this, TEXT("InternalFleshRingDeformer"));
	if (!InternalDeformer)
	{
		UE_LOG(LogFleshRingComponent, Error, TEXT("FleshRingComponent: Failed to create internal deformer"));
		return;
	}

	// SkeletalMeshComponent에 Deformer 등록
	TargetMesh->SetMeshDeformer(InternalDeformer);

	// Bounds 확장: Deformer 변형이 원래 bounds를 벗어날 수 있으므로
	// VSM(Virtual Shadow Maps) 등 bounds 기반 캐싱 시스템이 정상 작동하도록 확장
	TargetMesh->SetBoundsScale(BoundsScale);

	// Optimus와 동일하게 초기화 시점에 render state 갱신 요청
	// - MarkRenderStateDirty: PassthroughVertexFactory 생성을 위해 render state 재생성
	// - MarkRenderDynamicDataDirty: 동적 데이터 갱신 요청
	// 주의: TickComponent에서는 호출하지 않음 (엔진이 자동으로 처리)
	TargetMesh->MarkRenderStateDirty();
	TargetMesh->MarkRenderDynamicDataDirty();

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Deformer registered to target mesh '%s'"),
		*TargetMesh->GetName());
}

void UFleshRingComponent::CleanupDeformer()
{
	USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
	if (TargetMesh && InternalDeformer)
	{
		TargetMesh->SetMeshDeformer(nullptr);
		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Deformer unregistered from target mesh"));
	}

	InternalDeformer = nullptr;
	ResolvedTargetMesh.Reset();

	// SDF 캐시 해제 (IPooledRenderTarget은 UPROPERTY가 아니므로 수동 해제 필요)
	for (FRingSDFCache& Cache : RingSDFCaches)
	{
		Cache.Reset();
	}
	RingSDFCaches.Empty();
}

void UFleshRingComponent::GenerateSDF()
{
	if (!FleshRingAsset)
	{
		return;
	}

	// 기존 SDF 캐시 초기화
	for (FRingSDFCache& Cache : RingSDFCaches)
	{
		Cache.Reset();
	}
	RingSDFCaches.Empty();

	// Ring 개수만큼 캐시 배열 미리 할당 (렌더 스레드에서 인덱스로 접근)
	RingSDFCaches.SetNum(FleshRingAsset->Rings.Num());

	// 각 Ring의 RingMesh에서 SDF 생성
	for (int32 RingIndex = 0; RingIndex < FleshRingAsset->Rings.Num(); ++RingIndex)
	{
		const FFleshRingSettings& Ring = FleshRingAsset->Rings[RingIndex];
		UStaticMesh* RingMesh = Ring.RingMesh.LoadSynchronous();
		if (!RingMesh)
		{
			UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: Ring[%d] has no valid RingMesh"), RingIndex);
			continue;
		}

		// 1. StaticMesh(RingMesh)에서 버텍스/인덱스/노말 데이터 추출
		FFleshRingMeshData MeshData;
		if (!UFleshRingMeshExtractor::ExtractMeshData(RingMesh, MeshData))
		{
			UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: Failed to extract mesh data from Ring[%d] mesh '%s'"),
				RingIndex, *RingMesh->GetName());
			continue;
		}

		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Ring[%d] extracted %d vertices, %d triangles from '%s'"),
			RingIndex, MeshData.GetVertexCount(), MeshData.GetTriangleCount(), *RingMesh->GetName());

		// 2. Ring Mesh를 Component Space로 변환
		// Ring Mesh Local → MeshTransform → BoneTransform → Component Space
		{
			// Mesh Transform (Ring Local → Bone Local)
			FTransform MeshTransform;
			MeshTransform.SetLocation(Ring.MeshOffset);
			MeshTransform.SetRotation(Ring.MeshRotation.Quaternion());
			MeshTransform.SetScale3D(Ring.MeshScale);

			// Bone Transform (Bone Local → Component Space)
			FTransform BoneTransform = GetBoneBindPoseTransform(ResolvedTargetMesh.Get(), Ring.BoneName);

			// Full Transform: Ring Local → Component Space
			FTransform FullTransform = MeshTransform * BoneTransform;

			// 버텍스 변환
			for (FVector3f& Vertex : MeshData.Vertices)
			{
				FVector WorldPos = FullTransform.TransformPosition(FVector(Vertex));
				Vertex = FVector3f(WorldPos);
			}

			// Bounds 재계산
			FVector3f MinBounds(FLT_MAX, FLT_MAX, FLT_MAX);
			FVector3f MaxBounds(-FLT_MAX, -FLT_MAX, -FLT_MAX);
			for (const FVector3f& V : MeshData.Vertices)
			{
				MinBounds = FVector3f::Min(MinBounds, V);
				MaxBounds = FVector3f::Max(MaxBounds, V);
			}
			MeshData.Bounds = FBox3f(MinBounds, MaxBounds);

			UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Ring[%d] transformed to Component Space. Bounds: (%s) to (%s)"),
				RingIndex, *MinBounds.ToString(), *MaxBounds.ToString());
		}

		// 3. SDF 해상도 결정
		const int32 Resolution = Ring.SdfSettings.Resolution;
		const FIntVector SDFResolution(Resolution, Resolution, Resolution);

		// 4. Bounds 계산 (메시 bounds + 패딩)
		const float BoundsPadding = 2.0f; // SDF 경계 여유 공간
		FVector3f BoundsMin = MeshData.Bounds.Min - FVector3f(BoundsPadding);
		FVector3f BoundsMax = MeshData.Bounds.Max + FVector3f(BoundsPadding);

		// 4. GPU SDF 생성 (렌더 스레드에서 실행)
		// MeshData를 값으로 캡처 (렌더 스레드로 전달)
		TArray<FVector3f> CapturedVertices = MoveTemp(MeshData.Vertices);
		TArray<uint32> CapturedIndices = MoveTemp(MeshData.Indices);
		FIntVector CapturedResolution = SDFResolution;
		FVector3f CapturedBoundsMin = BoundsMin;
		FVector3f CapturedBoundsMax = BoundsMax;

		// 캐시 포인터 캡처 (렌더 스레드에서 직접 업데이트)
		// TRefCountPtr은 스레드 세이프하므로 직접 참조 가능
		FRingSDFCache* CachePtr = &RingSDFCaches[RingIndex];

		// 메타데이터 미리 설정 (게임 스레드에서)
		CachePtr->BoundsMin = BoundsMin;
		CachePtr->BoundsMax = BoundsMax;
		CachePtr->Resolution = SDFResolution;

		ENQUEUE_RENDER_COMMAND(GenerateFleshRingSDF)(
			[CapturedVertices = MoveTemp(CapturedVertices),
			 CapturedIndices = MoveTemp(CapturedIndices),
			 CapturedResolution,
			 CapturedBoundsMin,
			 CapturedBoundsMax,
			 RingIndex,
			 CachePtr](FRHICommandListImmediate& RHICmdList)
			{
				FRDGBuilder GraphBuilder(RHICmdList);

				// SDF 텍스처 생성 (중간 결과용)
				FRDGTextureDesc SDFTextureDesc = FRDGTextureDesc::Create3D(
					FIntVector(CapturedResolution.X, CapturedResolution.Y, CapturedResolution.Z),
					PF_R32_FLOAT,
					FClearValueBinding::Black,
					TexCreate_ShaderResource | TexCreate_UAV);

				FRDGTextureRef RawSDFTexture = GraphBuilder.CreateTexture(SDFTextureDesc, TEXT("FleshRing_RawSDF"));
				FRDGTextureRef CorrectedSDFTexture = GraphBuilder.CreateTexture(SDFTextureDesc, TEXT("FleshRing_CorrectedSDF"));

				// SDF 생성 (Point-to-Triangle 거리 계산)
				GenerateMeshSDF(
					GraphBuilder,
					RawSDFTexture,
					CapturedVertices,
					CapturedIndices,
					CapturedBoundsMin,
					CapturedBoundsMax,
					CapturedResolution);

				// 도넛홀 보정 (2D Slice Flood Fill)
				Apply2DSliceFloodFill(
					GraphBuilder,
					RawSDFTexture,
					CorrectedSDFTexture,
					CapturedResolution);

				// 핵심: RDG 텍스처 → Pooled 텍스처로 변환 (Execute 전에!)
				// ConvertToExternalTexture는 Execute 전에 호출해야 함
				// Execute 후에도 텍스처가 유지되어 다음 프레임에서 사용 가능
				CachePtr->PooledTexture = GraphBuilder.ConvertToExternalTexture(CorrectedSDFTexture);
				CachePtr->bCached = true;

				// RDG 실행
				GraphBuilder.Execute();

				UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: SDF cached for Ring[%d], Resolution=%d"),
					RingIndex, CapturedResolution.X);
			});
	}

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: GenerateSDF completed for %d rings"), FleshRingAsset->Rings.Num());
}

void UFleshRingComponent::UpdateSDF()
{
	GenerateSDF();
}

void UFleshRingComponent::ApplyAsset()
{
	if (!FleshRingAsset)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: ApplyAsset called but FleshRingAsset is null"));
		return;
	}

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Applying asset '%s'"), *FleshRingAsset->GetName());

	// 기존 설정 정리 후 재설정
	CleanupRingMeshes();
	CleanupDeformer();

	if (bEnableFleshRing)
	{
		ResolveTargetMesh();

		// SkeletalMesh 일치 검증 (에디터 프리뷰 = 게임 결과 보장)
		USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
		if (TargetMesh && !FleshRingAsset->TargetSkeletalMesh.IsNull())
		{
			USkeletalMesh* ExpectedMesh = FleshRingAsset->TargetSkeletalMesh.LoadSynchronous();
			USkeletalMesh* ActualMesh = TargetMesh->GetSkeletalMeshAsset();

			if (ExpectedMesh && ActualMesh && ExpectedMesh != ActualMesh)
			{
				UE_LOG(LogFleshRingComponent, Warning,
					TEXT("FleshRingComponent: SkeletalMesh mismatch! Asset expects '%s' but target has '%s'. Effect may differ from editor preview."),
					*ExpectedMesh->GetName(), *ActualMesh->GetName());
			}
		}

		SetupDeformer();
		GenerateSDF();
		SetupRingMeshes();
	}
}

void UFleshRingComponent::SetupRingMeshes()
{
	// 기존 Ring 메시 정리
	CleanupRingMeshes();

	if (!FleshRingAsset || !ResolvedTargetMesh.IsValid())
	{
		return;
	}

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return;
	}

	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();

	// 각 Ring에 대해 StaticMeshComponent 생성
	for (int32 RingIndex = 0; RingIndex < FleshRingAsset->Rings.Num(); ++RingIndex)
	{
		const FFleshRingSettings& Ring = FleshRingAsset->Rings[RingIndex];

		// RingMesh가 없으면 스킵
		UStaticMesh* RingMesh = Ring.RingMesh.LoadSynchronous();
		if (!RingMesh)
		{
			RingMeshComponents.Add(nullptr);
			continue;
		}

		// BoneName 유효성 검사
		if (Ring.BoneName.IsNone())
		{
			UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: Ring[%d] has no BoneName"), RingIndex);
			RingMeshComponents.Add(nullptr);
			continue;
		}

		// 본 인덱스 확인
		const int32 BoneIndex = SkelMesh->GetBoneIndex(Ring.BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: Ring[%d] bone '%s' not found"),
				RingIndex, *Ring.BoneName.ToString());
			RingMeshComponents.Add(nullptr);
			continue;
		}

		// StaticMeshComponent 생성
		FName ComponentName = FName(*FString::Printf(TEXT("RingMesh_%d"), RingIndex));
		UStaticMeshComponent* MeshComp = NewObject<UStaticMeshComponent>(Owner, ComponentName);
		if (!MeshComp)
		{
			UE_LOG(LogFleshRingComponent, Error, TEXT("FleshRingComponent: Failed to create StaticMeshComponent for Ring[%d]"), RingIndex);
			RingMeshComponents.Add(nullptr);
			continue;
		}

		// StaticMesh 설정
		MeshComp->SetStaticMesh(RingMesh);

		// 트랜스폼 설정 (MeshOffset, MeshRotation, MeshScale)
		MeshComp->SetRelativeLocation(Ring.MeshOffset);
		MeshComp->SetRelativeRotation(Ring.MeshRotation);
		MeshComp->SetRelativeScale3D(Ring.MeshScale);

		// Construction Script로 생성된 것처럼 처리 (에디터에서 삭제 시도해도 다시 생성됨)
		MeshComp->CreationMethod = EComponentCreationMethod::Native;
		MeshComp->bIsEditorOnly = false;  // 게임에서도 보임
		MeshComp->SetCastShadow(true);    // 그림자 캐스팅

		// SkeletalMeshComponent의 본에 부착
		MeshComp->AttachToComponent(SkelMesh, FAttachmentTransformRules::KeepRelativeTransform, Ring.BoneName);

		// 컴포넌트 등록
		MeshComp->RegisterComponent();

		RingMeshComponents.Add(MeshComp);

		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Ring[%d] mesh '%s' attached to bone '%s'"),
			RingIndex, *RingMesh->GetName(), *Ring.BoneName.ToString());
	}

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: SetupRingMeshes completed, %d meshes created"),
		RingMeshComponents.Num());
}

void UFleshRingComponent::CleanupRingMeshes()
{
	for (UStaticMeshComponent* MeshComp : RingMeshComponents)
	{
		if (MeshComp)
		{
			MeshComp->DestroyComponent();
		}
	}
	RingMeshComponents.Empty();
}
