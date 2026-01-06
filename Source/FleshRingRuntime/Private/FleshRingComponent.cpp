// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "FleshRingUtils.h"
#include "FleshRingMeshComponent.h"
#include "FleshRingMeshExtractor.h"
#include "FleshRingSDF.h"
#include "FleshRingDeformerInstance.h"
#include "FleshRingBulgeTypes.h"
#include "Engine/StaticMesh.h"
#include "Engine/VolumeTexture.h"
#include "GameFramework/Actor.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#if WITH_EDITOR
#include "DrawDebugHelpers.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/StaticMeshComponent.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#endif

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

// Helper: 스켈레탈 메시의 유효성 검사 (공통 유틸리티 래퍼)
static bool IsSkeletalMeshSkeletonValid(USkeletalMesh* Mesh)
{
	return FleshRingUtils::IsSkeletalMeshValid(Mesh, /*bLogWarnings=*/ true);
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

		// SDF를 먼저 생성하고 완료 대기
		// AffectedVertices 등록 시 SDF Bounds 사용 가능하도록
		GenerateSDF();
		FlushRenderingCommands();  // SDF 생성 완료 대기

		// 유효한 SDF 캐시가 하나라도 있으면 Deformer 설정 (메시 없는 Ring은 개별 스킵)
		if (HasAnyValidSDFCaches())
		{
			SetupDeformer();
		}

		// Ring 메시는 OnRegister()에서 이미 설정됨
	}
}

void UFleshRingComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Ring 메시는 OnUnregister()에서 정리됨
	CleanupDeformer();

#if WITH_EDITOR
	CleanupDebugResources();
#endif

	Super::EndPlay(EndPlayReason);
}

void UFleshRingComponent::BeginDestroy()
{
	// GC 시점에 Deformer 정리 보장
	// 에셋 전환 시 FMeshBatch 유효성 문제 방지
	CleanupDeformer();

	Super::BeginDestroy();
}

void UFleshRingComponent::OnRegister()
{
	Super::OnRegister();

#if WITH_EDITOR
	// 에셋 변경 델리게이트 구독
	BindToAssetDelegate();
#endif

	// 에디터 및 런타임 모두에서 Ring 메시 설정
	// OnRegister는 컴포넌트가 월드에 등록될 때 호출됨 (에디터 포함)
	ResolveTargetMesh();
	SetupRingMeshes();
}

void UFleshRingComponent::OnUnregister()
{
#if WITH_EDITOR
	// 에셋 변경 델리게이트 구독 해제
	UnbindFromAssetDelegate();
#endif

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
		// 에셋 변경 시 델리게이트 재바인딩
		UnbindFromAssetDelegate();
		BindToAssetDelegate();

		ResolveTargetMesh();
		SetupRingMeshes();
	}

	// Ring 메시 가시성 변경
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UFleshRingComponent, bShowRingMesh))
	{
		UpdateRingMeshVisibility();
	}
}

void UFleshRingComponent::BindToAssetDelegate()
{
	if (FleshRingAsset && !AssetChangedDelegateHandle.IsValid())
	{
		AssetChangedDelegateHandle = FleshRingAsset->OnAssetChanged.AddUObject(
			this, &UFleshRingComponent::OnFleshRingAssetChanged);
	}
}

void UFleshRingComponent::UnbindFromAssetDelegate()
{
	if (FleshRingAsset && AssetChangedDelegateHandle.IsValid())
	{
		FleshRingAsset->OnAssetChanged.Remove(AssetChangedDelegateHandle);
		AssetChangedDelegateHandle.Reset();
	}
}

void UFleshRingComponent::OnFleshRingAssetChanged(UFleshRingAsset* ChangedAsset)
{
	// 동일한 에셋인지 확인
	if (ChangedAsset == FleshRingAsset)
	{
		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Asset changed, reapplying..."));

		// 전체 재설정 (SubdividedMesh 적용 포함)
		ApplyAsset();
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

#if WITH_EDITOR
	// 디버그 시각화
	DrawDebugVisualization();
#endif

#if WITH_EDITOR
	// 디버그 시각화
	DrawDebugVisualization();
#endif
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
		// SubdividedMesh 적용을 위해 return하지 않고 아래로 계속
	}
	else
	{
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

	// SubdividedMesh 또는 원본 메시 적용
	UE_LOG(LogFleshRingComponent, Log, TEXT("ResolveTargetMesh: Checking SubdividedMesh... ResolvedTargetMesh.IsValid()=%d, FleshRingAsset=%p"),
		ResolvedTargetMesh.IsValid(), FleshRingAsset.Get());

	if (ResolvedTargetMesh.IsValid() && FleshRingAsset)
	{
		USkeletalMeshComponent* TargetMeshComp = ResolvedTargetMesh.Get();
		USkeletalMesh* CurrentMesh = TargetMeshComp->GetSkeletalMeshAsset();

		UE_LOG(LogFleshRingComponent, Log, TEXT("ResolveTargetMesh: HasSubdividedMesh=%d, SubdividedMesh=%p, CurrentMesh='%s'"),
			FleshRingAsset->HasSubdividedMesh(),
			FleshRingAsset->SubdividedMesh.Get(),
			CurrentMesh ? *CurrentMesh->GetName() : TEXT("null"));

		if (FleshRingAsset->HasSubdividedMesh())
		{
			// SubdividedMesh가 있으면 적용
			USkeletalMesh* SubdivMesh = FleshRingAsset->SubdividedMesh;
			if (CurrentMesh != SubdivMesh)
			{
				// 스켈레톤 유효성 검사 (Undo/Redo 크래시 방지)
				if (!IsSkeletalMeshSkeletonValid(SubdivMesh))
				{
					UE_LOG(LogFleshRingComponent, Warning,
						TEXT("FleshRingComponent: SubdividedMesh '%s' has invalid skeleton, skipping SetSkeletalMesh"),
						SubdivMesh ? *SubdivMesh->GetName() : TEXT("null"));
					return;
				}

				// 원본 메시 캐싱 (컴포넌트 제거 시 복원용)
				// 이미 캐싱된 경우 덮어쓰지 않음 (최초 원본 유지)
				if (!CachedOriginalMesh.IsValid())
				{
					CachedOriginalMesh = CurrentMesh;
					UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Cached original mesh '%s' for restoration"),
						CurrentMesh ? *CurrentMesh->GetName() : TEXT("null"));
				}

				TargetMeshComp->SetSkeletalMesh(SubdivMesh);
				UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Applied SubdividedMesh '%s' to target mesh component"),
					*SubdivMesh->GetName());
			}
			else
			{
				UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: SubdividedMesh already applied"));
			}
		}
		else if (!FleshRingAsset->TargetSkeletalMesh.IsNull())
		{
			// SubdividedMesh가 없으면 원본 메시로 복원
			USkeletalMesh* OriginalMesh = FleshRingAsset->TargetSkeletalMesh.LoadSynchronous();
			if (OriginalMesh && CurrentMesh != OriginalMesh)
			{
				// 스켈레톤 유효성 검사 (Undo/Redo 크래시 방지)
				if (!IsSkeletalMeshSkeletonValid(OriginalMesh))
				{
					UE_LOG(LogFleshRingComponent, Warning,
						TEXT("FleshRingComponent: OriginalMesh '%s' has invalid skeleton, skipping SetSkeletalMesh"),
						OriginalMesh ? *OriginalMesh->GetName() : TEXT("null"));
					return;
				}

				TargetMeshComp->SetSkeletalMesh(OriginalMesh);
				UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Restored original mesh '%s' to target mesh component"),
					*OriginalMesh->GetName());
			}
		}
	}
	else
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("ResolveTargetMesh: Cannot apply SubdividedMesh - ResolvedTargetMesh or FleshRingAsset is invalid"));
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
		// 1. 먼저 진행중인 렌더 작업 완료 대기
		FlushRenderingCommands();

		// 2. Deformer 해제
		TargetMesh->SetMeshDeformer(nullptr);

		// 3. Render State를 dirty로 마킹하여 Scene Proxy 재생성 트리거
		// VertexFactory가 올바르게 재초기화되도록 함
		TargetMesh->MarkRenderStateDirty();

		// 4. 새 렌더 스테이트가 적용될 때까지 대기
		// FMeshBatch 유효성 문제 방지
		FlushRenderingCommands();

		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Deformer unregistered from target mesh"));
	}

	// 원본 메시 복원 (SubdividedMesh가 적용되어 있던 경우)
	if (TargetMesh && CachedOriginalMesh.IsValid())
	{
		USkeletalMesh* CurrentMesh = TargetMesh->GetSkeletalMeshAsset();
		USkeletalMesh* OriginalMesh = CachedOriginalMesh.Get();

		// 현재 메시가 원본과 다른 경우에만 복원 (SubdividedMesh 적용 상태)
		if (CurrentMesh != OriginalMesh)
		{
			TargetMesh->SetSkeletalMesh(OriginalMesh);
			TargetMesh->MarkRenderStateDirty();
			UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Restored original mesh '%s' on cleanup"),
				OriginalMesh ? *OriginalMesh->GetName() : TEXT("null"));
		}
	}
	CachedOriginalMesh.Reset();

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

		// 2. OBB 방식: 로컬 스페이스 유지, 트랜스폼은 별도 저장
		// Ring Mesh Local → MeshTransform → BoneTransform → Component Space
		FTransform LocalToComponentTransform;
		{
			// Mesh Transform (Ring Local → Bone Local)
			FTransform MeshTransform;
			MeshTransform.SetLocation(Ring.MeshOffset);
			MeshTransform.SetRotation(FQuat(Ring.MeshRotation));
			MeshTransform.SetScale3D(Ring.MeshScale);

			// Bone Transform (Bone Local → Component Space)
			FTransform BoneTransform = GetBoneBindPoseTransform(ResolvedTargetMesh.Get(), Ring.BoneName);

			// Full Transform: Ring Local → Component Space (OBB용으로 저장)
			LocalToComponentTransform = MeshTransform * BoneTransform;

			// 버텍스는 변환하지 않음 (로컬 스페이스 유지)
			// SDF는 로컬 스페이스에서 생성, 샘플링 시 역변환 사용

			UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Ring[%d] OBB Transform saved. Local Bounds: (%s) to (%s)"),
				RingIndex, *MeshData.Bounds.Min.ToString(), *MeshData.Bounds.Max.ToString());
		}

		// 3. SDF 해상도 결정 (고정값 64)
		const int32 Resolution = 64;
		const FIntVector SDFResolution(Resolution, Resolution, Resolution);

		// 4. Bounds 계산 (로컬 스페이스 메시 bounds + 패딩)
		const float BoundsPadding = 0.0f; // SDF 경계 여유 공간 이거 무조건 프로퍼티화 해야함 TODOTODOTODOTODO
		FVector3f BoundsMin = MeshData.Bounds.Min - FVector3f(BoundsPadding);
		FVector3f BoundsMax = MeshData.Bounds.Max + FVector3f(BoundsPadding);

		// 5. GPU SDF 생성 (렌더 스레드에서 실행)
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
		CachePtr->LocalToComponent = LocalToComponentTransform;

		// 경계 버텍스 기반 Bulge 방향 자동 감지 (CPU)
		// SDF 중심 = (BoundsMin + BoundsMax) / 2
		const FVector3f SDFCenter = (BoundsMin + BoundsMax) * 0.5f;
		CachePtr->DetectedBulgeDirection = FBulgeDirectionDetector::DetectFromBoundaryVertices(
			CapturedVertices,
			CapturedIndices,
			SDFCenter
		);

		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Ring[%d] Bulge direction auto-detected: %d (SDFCenter: %s)"),
			RingIndex, CachePtr->DetectedBulgeDirection, *SDFCenter.ToString());

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

void UFleshRingComponent::InitializeForEditorPreview()
{
	// 비활성화 상태면 스킵
	if (!bEnableFleshRing)
	{
		return;
	}

	// 이미 초기화되었으면 스킵
	if (bEditorPreviewInitialized)
	{
		return;
	}

	UE_LOG(LogFleshRingComponent, Log, TEXT("InitializeForEditorPreview: Starting..."));

	// 대상 메시 해석
	ResolveTargetMesh();

	if (!ResolvedTargetMesh.IsValid())
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("InitializeForEditorPreview: No target mesh"));
		return;
	}

	// SDF 생성 및 완료 대기
	GenerateSDF();
	FlushRenderingCommands();

	// 유효한 SDF 캐시가 하나라도 있어야 Deformer 설정 (메시 없는 Ring은 개별 스킵)
	if (!HasAnyValidSDFCaches())
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("InitializeForEditorPreview: No valid SDF caches, skipping Deformer setup"));
		bEditorPreviewInitialized = true;
		return;
	}

	// Deformer 설정
	SetupDeformer();

	// Ring 메시 설정 (이미 OnRegister에서 호출되었을 수 있음)
	if (RingMeshComponents.Num() == 0)
	{
		SetupRingMeshes();
	}

	bEditorPreviewInitialized = true;

	UE_LOG(LogFleshRingComponent, Log, TEXT("InitializeForEditorPreview: Completed"));
}

void UFleshRingComponent::UpdateRingTransforms()
{
	if (!FleshRingAsset || !ResolvedTargetMesh.IsValid())
	{
		return;
	}

	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();

	for (int32 RingIndex = 0; RingIndex < FleshRingAsset->Rings.Num(); ++RingIndex)
	{
		const FFleshRingSettings& Ring = FleshRingAsset->Rings[RingIndex];

		// Bone Transform 계산
		FTransform BoneTransform = GetBoneBindPoseTransform(SkelMesh, Ring.BoneName);
		FQuat BoneRotation = BoneTransform.GetRotation();

		// Mesh Transform (Ring Local → Bone Local)
		FTransform MeshTransform;
		MeshTransform.SetLocation(Ring.MeshOffset);
		MeshTransform.SetRotation(FQuat(Ring.MeshRotation));
		MeshTransform.SetScale3D(Ring.MeshScale);

		// Full Transform: Ring Local → Component Space
		FTransform LocalToComponentTransform = MeshTransform * BoneTransform;

		// 1. SDF 캐시의 LocalToComponent 업데이트
		if (RingSDFCaches.IsValidIndex(RingIndex))
		{
			RingSDFCaches[RingIndex].LocalToComponent = LocalToComponentTransform;
		}

		// 2. Ring 메시 컴포넌트의 트랜스폼 업데이트
		if (RingMeshComponents.IsValidIndex(RingIndex) && RingMeshComponents[RingIndex])
		{
			FVector MeshLocation = BoneTransform.GetLocation() + BoneRotation.RotateVector(Ring.MeshOffset);
			FQuat WorldRotation = BoneRotation * Ring.MeshRotation;
			RingMeshComponents[RingIndex]->SetWorldLocationAndRotation(MeshLocation, WorldRotation);
			RingMeshComponents[RingIndex]->SetWorldScale3D(Ring.MeshScale);
		}
	}

	// 3. DeformerInstance의 TightenedBindPose 캐시 무효화 (재계산 트리거)
	if (USkeletalMeshComponent* SkelMeshComp = ResolvedTargetMesh.Get())
	{
		if (UMeshDeformerInstance* DeformerInstance = SkelMeshComp->GetMeshDeformerInstance())
		{
			if (UFleshRingDeformerInstance* FleshRingInstance = Cast<UFleshRingDeformerInstance>(DeformerInstance))
			{
				FleshRingInstance->InvalidateTightnessCache();
			}
		}

		// 4. 렌더 시스템에 동적 데이터 변경 알림 (실시간 변형 반영)
		// InvalidateTightnessCache만으로는 다음 프레임의 EnqueueWork 호출이 보장되지 않을 수 있음
		SkelMeshComp->MarkRenderDynamicDataDirty();
	}

#if WITH_EDITORONLY_DATA
	// 5. 디버그 시각화 캐시 무효화 (Ring 이동 시 AffectedVertices 재계산)
	bDebugAffectedVerticesCached = false;
	bDebugBulgeVerticesCached = false;
#endif
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
#if WITH_EDITOR
	CleanupDebugResources();
#endif

	// 에디터 프리뷰 상태 리셋
	bEditorPreviewInitialized = false;

	if (bEnableFleshRing)
	{
		ResolveTargetMesh();

		// SkeletalMesh 일치 검증 (에디터 프리뷰 = 게임 결과 보장)
		USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
		if (TargetMesh && !FleshRingAsset->TargetSkeletalMesh.IsNull())
		{
			USkeletalMesh* ExpectedMesh = FleshRingAsset->TargetSkeletalMesh.LoadSynchronous();
			USkeletalMesh* ActualMesh = TargetMesh->GetSkeletalMeshAsset();

			// SubdividedMesh가 적용된 경우는 정상이므로 검증 통과
			bool bIsSubdividedMesh = FleshRingAsset->HasSubdividedMesh() && ActualMesh == FleshRingAsset->SubdividedMesh;

			if (ExpectedMesh && ActualMesh && ExpectedMesh != ActualMesh && !bIsSubdividedMesh)
			{
				UE_LOG(LogFleshRingComponent, Warning,
					TEXT("FleshRingComponent: SkeletalMesh mismatch! Asset expects '%s' but target has '%s'. Effect may differ from editor preview."),
					*ExpectedMesh->GetName(), *ActualMesh->GetName());
			}
		}

		// SDF 생성 (Deformer는 BeginPlay() 또는 InitializeForEditorPreview()에서 설정)
		// 에디터 프리뷰에서는 SkeletalMesh 렌더 상태가 준비된 후 타이머로 Deformer 초기화
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

		// FleshRingMeshComponent 생성 (에디터에서 본보다 높은 피킹 우선순위)
		FName ComponentName = FName(*FString::Printf(TEXT("RingMesh_%d"), RingIndex));
		UFleshRingMeshComponent* MeshComp = NewObject<UFleshRingMeshComponent>(Owner, ComponentName);
		if (!MeshComp)
		{
			UE_LOG(LogFleshRingComponent, Error, TEXT("FleshRingComponent: Failed to create FleshRingMeshComponent for Ring[%d]"), RingIndex);
			RingMeshComponents.Add(nullptr);
			continue;
		}

		// Ring 인덱스 설정 (HitProxy에서 사용)
		MeshComp->SetRingIndex(RingIndex);

		// StaticMesh 설정
		MeshComp->SetStaticMesh(RingMesh);

		// Construction Script로 생성된 것처럼 처리 (에디터에서 삭제 시도해도 다시 생성됨)
		MeshComp->CreationMethod = EComponentCreationMethod::Native;
		MeshComp->bIsEditorOnly = false;  // 게임에서도 보임
		MeshComp->SetCastShadow(true);    // 그림자 캐스팅

		// Visibility 설정 (RegisterComponent 전에 설정해야 SceneProxy 생성 시 반영됨)
		MeshComp->SetVisibility(bShowRingMesh);

		// 컴포넌트 등록
		MeshComp->RegisterComponent();

		// 본에 먼저 부착 (본 위치에 스냅)
		MeshComp->AttachToComponent(SkelMesh, FAttachmentTransformRules::SnapToTargetNotIncludingScale, Ring.BoneName);

		// 상대 트랜스폼 설정 (본 로컬 공간 기준)
		// MeshRotation 기본값 FRotator(-90, 0, 0)으로 메시 Z축이 본 X축과 일치
		MeshComp->SetRelativeLocation(Ring.MeshOffset);
		MeshComp->SetRelativeRotation(Ring.MeshRotation);
		MeshComp->SetRelativeScale3D(Ring.MeshScale);

		RingMeshComponents.Add(MeshComp);

		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Ring[%d] mesh '%s' attached to bone '%s'"),
			RingIndex, *RingMesh->GetName(), *Ring.BoneName.ToString());
	}

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: SetupRingMeshes completed, %d meshes created"),
		RingMeshComponents.Num());

	// bShowRingMesh 상태에 따라 Visibility 적용 (에디터 Show Flag 동기화)
	UpdateRingMeshVisibility();
}

void UFleshRingComponent::CleanupRingMeshes()
{
	if (RingMeshComponents.Num() > 0)
	{
		// 렌더 스레드가 컴포넌트 리소스 사용을 완료할 때까지 대기
		FlushRenderingCommands();

		for (UStaticMeshComponent* MeshComp : RingMeshComponents)
		{
			if (MeshComp)
			{
				MeshComp->DestroyComponent();
			}
		}
		RingMeshComponents.Empty();
	}
}

void UFleshRingComponent::UpdateRingMeshVisibility()
{
	for (UStaticMeshComponent* MeshComp : RingMeshComponents)
	{
		if (MeshComp)
		{
			MeshComp->SetVisibility(bShowRingMesh);
		}
	}
}

// =====================================
// Debug Drawing (에디터 전용)
// =====================================

void UFleshRingComponent::SetDebugSlicePlanesVisible(bool bVisible)
{
#if WITH_EDITORONLY_DATA
	for (AActor* PlaneActor : DebugSlicePlaneActors)
	{
		if (PlaneActor)
		{
			// 에디터에서는 SetIsTemporarilyHiddenInEditor 사용 (SetActorHiddenInGame은 에디터에서 동작 안 함)
			PlaneActor->SetIsTemporarilyHiddenInEditor(!bVisible);
		}
	}
#endif
}

#if WITH_EDITOR

void UFleshRingComponent::DrawDebugVisualization()
{
	// TargetMesh가 없으면 디버그 시각화 스킵
	if (!ResolvedTargetMesh.IsValid() || !ResolvedTargetMesh->GetSkeletalMeshAsset())
	{
		return;
	}

	// 마스터 스위치가 꺼지면 슬라이스 평면 숨기기
	if (!bShowDebugVisualization || !bShowSDFSlice)
	{
		for (AActor* PlaneActor : DebugSlicePlaneActors)
		{
			if (PlaneActor)
			{
				PlaneActor->SetActorHiddenInGame(true);
			}
		}
	}

	if (!bShowDebugVisualization)
	{
		return;
	}

	const int32 NumRings = RingSDFCaches.Num();

	// Ring 개수가 변경되면 디버그 리소스 정리 후 재생성
	// (중간 Ring 삭제 시 인덱스 어긋남 방지)
	if (DebugSlicePlaneActors.Num() != NumRings)
	{
		for (AActor* PlaneActor : DebugSlicePlaneActors)
		{
			if (PlaneActor)
			{
				PlaneActor->Destroy();
			}
		}
		DebugSlicePlaneActors.Empty();
		DebugSliceRenderTargets.Empty();
	}
	if (DebugAffectedData.Num() != NumRings)
	{
		bDebugAffectedVerticesCached = false;
	}
	if (DebugBulgeData.Num() != NumRings)
	{
		bDebugBulgeVerticesCached = false;
	}

	for (int32 RingIndex = 0; RingIndex < NumRings; ++RingIndex)
	{
		if (bShowSdfVolume)
		{
			DrawSdfVolume(RingIndex);
		}

		if (bShowAffectedVertices)
		{
			DrawAffectedVertices(RingIndex);
		}

		if (bShowSDFSlice)
		{
			DrawSDFSlice(RingIndex);
		}

		if (bShowBulgeHeatmap)
		{
			DrawBulgeHeatmap(RingIndex);
			DrawBulgeDirectionArrow(RingIndex);
		}
	}
}

void UFleshRingComponent::DrawSdfVolume(int32 RingIndex)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// SDF 캐시 가져오기
	const FRingSDFCache* SDFCache = GetRingSDFCache(RingIndex);
	if (!SDFCache || !SDFCache->IsValid())
	{
		// 캐시 없으면 화면에 경고 표시
		if (GEngine)
		{
			GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Red,
				FString::Printf(TEXT("Ring[%d]: SDF not cached!"), RingIndex));
		}
		return;
	}

	// OBB 방식: 로컬 바운드 + 트랜스폼
	FVector LocalBoundsMin = FVector(SDFCache->BoundsMin);
	FVector LocalBoundsMax = FVector(SDFCache->BoundsMax);

	// 로컬 스페이스에서 Center와 Extent 계산
	FVector LocalCenter = (LocalBoundsMin + LocalBoundsMax) * 0.5f;
	FVector LocalExtent = (LocalBoundsMax - LocalBoundsMin) * 0.5f;

	// 전체 트랜스폼: Local → Component → World
	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();
	FTransform LocalToWorld = SDFCache->LocalToComponent;
	if (SkelMesh)
	{
		LocalToWorld = LocalToWorld * SkelMesh->GetComponentTransform();
	}

	// 월드 공간에서의 Center
	FVector WorldCenter = LocalToWorld.TransformPosition(LocalCenter);

	// OBB 회전
	FQuat WorldRotation = LocalToWorld.GetRotation();

	// 스케일 적용된 Extent
	FVector ScaledExtent = LocalExtent * LocalToWorld.GetScale3D();

	// [조건부 로그] 첫 프레임만 출력 - DrawSdfVolume Debug
	static bool bLoggedOBBDebug = false;
	if (!bLoggedOBBDebug)
	{
		UE_LOG(LogTemp, Log, TEXT(""));
		UE_LOG(LogTemp, Log, TEXT("======== DrawSdfVolume OBB Debug ========"));
		UE_LOG(LogTemp, Log, TEXT("  [Local Space]"));
		UE_LOG(LogTemp, Log, TEXT("    LocalBoundsMin: %s"), *LocalBoundsMin.ToString());
		UE_LOG(LogTemp, Log, TEXT("    LocalBoundsMax: %s"), *LocalBoundsMax.ToString());
		UE_LOG(LogTemp, Log, TEXT("    LocalSize: %s"), *(LocalBoundsMax - LocalBoundsMin).ToString());
		UE_LOG(LogTemp, Log, TEXT("  [LocalToComponent Transform]"));
		UE_LOG(LogTemp, Log, TEXT("    Location: %s"), *SDFCache->LocalToComponent.GetLocation().ToString());
		UE_LOG(LogTemp, Log, TEXT("    Rotation: %s"), *SDFCache->LocalToComponent.GetRotation().Rotator().ToString());
		UE_LOG(LogTemp, Log, TEXT("    Scale: %s"), *SDFCache->LocalToComponent.GetScale3D().ToString());
		// SubdivideRegion과 비교용 - Component Space OBB
		{
			FVector CompCenter = SDFCache->LocalToComponent.TransformPosition(LocalCenter);
			FQuat CompRotation = SDFCache->LocalToComponent.GetRotation();
			FVector CompAxisX = CompRotation.RotateVector(FVector(1, 0, 0));
			FVector CompAxisY = CompRotation.RotateVector(FVector(0, 1, 0));
			FVector CompAxisZ = CompRotation.RotateVector(FVector(0, 0, 1));
			FVector CompHalfExtents = LocalExtent * SDFCache->LocalToComponent.GetScale3D();
			UE_LOG(LogTemp, Log, TEXT("  [Component Space OBB (SubdivideRegion과 비교)]"));
			UE_LOG(LogTemp, Log, TEXT("    Center: %s"), *CompCenter.ToString());
			UE_LOG(LogTemp, Log, TEXT("    HalfExtents: %s"), *CompHalfExtents.ToString());
			UE_LOG(LogTemp, Log, TEXT("    AxisX: %s"), *CompAxisX.ToString());
			UE_LOG(LogTemp, Log, TEXT("    AxisY: %s"), *CompAxisY.ToString());
			UE_LOG(LogTemp, Log, TEXT("    AxisZ: %s"), *CompAxisZ.ToString());
		}
		UE_LOG(LogTemp, Log, TEXT("  [LocalToWorld (includes ComponentToWorld)]"));
		UE_LOG(LogTemp, Log, TEXT("    Location: %s"), *LocalToWorld.GetLocation().ToString());
		UE_LOG(LogTemp, Log, TEXT("    Rotation: %s"), *LocalToWorld.GetRotation().Rotator().ToString());
		UE_LOG(LogTemp, Log, TEXT("    Scale: %s"), *LocalToWorld.GetScale3D().ToString());
		UE_LOG(LogTemp, Log, TEXT("  [Visualization]"));
		UE_LOG(LogTemp, Log, TEXT("    WorldCenter: %s"), *WorldCenter.ToString());
		UE_LOG(LogTemp, Log, TEXT("    ScaledExtent: %s"), *ScaledExtent.ToString());
		UE_LOG(LogTemp, Log, TEXT("    WorldRotation: %s"), *WorldRotation.Rotator().ToString());
		UE_LOG(LogTemp, Log, TEXT("=========================================="));
		UE_LOG(LogTemp, Log, TEXT(""));
		bLoggedOBBDebug = true;
	}

	// OBB 와이어프레임 박스 (회전 포함)
	FColor BoxColor = FColor(0, 200, 255, 255);  // 밝은 시안
	DrawDebugBox(World, WorldCenter, ScaledExtent, WorldRotation, BoxColor, false, -1.0f, 0, 0.3f);

	// Min/Max 코너 강조 표시 (월드 공간)
	FVector WorldMin = LocalToWorld.TransformPosition(LocalBoundsMin);
	FVector WorldMax = LocalToWorld.TransformPosition(LocalBoundsMax);
	DrawDebugSphere(World, WorldMin, 1.0f, 8, FColor::Blue, false, -1.0f, 0, 0.5f);
	DrawDebugSphere(World, WorldMax, 1.0f, 8, FColor::Red, false, -1.0f, 0, 0.5f);

	// 해상도 텍스트 표시
	if (GEngine)
	{
		FIntVector Res = SDFCache->Resolution;
		GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Yellow,
			FString::Printf(TEXT("Ring[%d] SDF: %dx%dx%d"), RingIndex, Res.X, Res.Y, Res.Z));
	}
}

void UFleshRingComponent::DrawAffectedVertices(int32 RingIndex)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 캐싱되지 않았으면 먼저 캐싱
	if (!bDebugAffectedVerticesCached)
	{
		CacheAffectedVerticesForDebug();
	}

	// 데이터 유효성 검사
	if (!DebugAffectedData.IsValidIndex(RingIndex) ||
		DebugBindPoseVertices.Num() == 0)
	{
		return;
	}

	const FRingAffectedData& RingData = DebugAffectedData[RingIndex];
	if (RingData.Vertices.Num() == 0)
	{
		return;
	}

	// 현재 스켈레탈 메시 컴포넌트
	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();
	if (!SkelMesh)
	{
		return;
	}

	// 컴포넌트 → 월드 트랜스폼
	FTransform CompTransform = SkelMesh->GetComponentTransform();

	// 각 영향받는 버텍스에 대해
	for (const FAffectedVertex& AffectedVert : RingData.Vertices)
	{
		if (!DebugBindPoseVertices.IsValidIndex(AffectedVert.VertexIndex))
		{
			continue;
		}

		// 바인드 포즈 위치 (컴포넌트 스페이스)
		const FVector3f& BindPosePos = DebugBindPoseVertices[AffectedVert.VertexIndex];

		// 월드 공간으로 변환 (바인드 포즈 기준 - 애니메이션 미반영)
		FVector WorldPos = CompTransform.TransformPosition(FVector(BindPosePos));

		// Influence에 따른 색상 (0=파랑, 0.5=초록, 1=빨강)
		float Influence = AffectedVert.Influence;
		FColor PointColor;
		if (Influence < 0.5f)
		{
			// 파랑 → 초록
			float T = Influence * 2.0f;
			PointColor = FColor(
				0,
				FMath::RoundToInt(255 * T),
				FMath::RoundToInt(255 * (1.0f - T))
			);
		}
		else
		{
			// 초록 → 빨강
			float T = (Influence - 0.5f) * 2.0f;
			PointColor = FColor(
				FMath::RoundToInt(255 * T),
				FMath::RoundToInt(255 * (1.0f - T)),
				0
			);
		}

		// 점 그리기 (크기 = Influence에 비례)
		float PointSize = 2.0f + Influence * 6.0f; // 2~8 범위
		DrawDebugPoint(World, WorldPos, PointSize, PointColor, false, -1.0f, SDPG_Foreground);
	}

	// 화면에 정보 표시
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Green,
			FString::Printf(TEXT("Ring[%d] Affected: %d vertices"),
				RingIndex, RingData.Vertices.Num()));
	}
}

void UFleshRingComponent::DrawSDFSlice(int32 RingIndex)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FRingSDFCache* SDFCache = GetRingSDFCache(RingIndex);
	if (!SDFCache || !SDFCache->IsValid())
	{
		return;
	}

	// 배열 크기 확보
	if (DebugSlicePlaneActors.Num() <= RingIndex)
	{
		DebugSlicePlaneActors.SetNum(RingIndex + 1);
	}
	if (DebugSliceRenderTargets.Num() <= RingIndex)
	{
		DebugSliceRenderTargets.SetNum(RingIndex + 1);
	}

	// 평면 액터가 없으면 생성
	if (!DebugSlicePlaneActors[RingIndex])
	{
		DebugSlicePlaneActors[RingIndex] = CreateDebugSlicePlane(RingIndex);
	}

	AActor* PlaneActor = DebugSlicePlaneActors[RingIndex];
	if (!PlaneActor)
	{
		return;
	}

	// 평면 보이게 설정
	PlaneActor->SetActorHiddenInGame(false);

	// OBB 방식: 로컬 바운드 계산
	FVector LocalBoundsMin = FVector(SDFCache->BoundsMin);
	FVector LocalBoundsMax = FVector(SDFCache->BoundsMax);
	FVector LocalBoundsSize = LocalBoundsMax - LocalBoundsMin;

	// Z 슬라이스 위치 계산 (로컬 스페이스)
	float ZRatio = (SDFCache->Resolution.Z > 1)
		? (float)DebugSliceZ / (float)(SDFCache->Resolution.Z - 1)
		: 0.5f;
	ZRatio = FMath::Clamp(ZRatio, 0.0f, 1.0f);

	FVector LocalSliceCenter = LocalBoundsMin + FVector(
		LocalBoundsSize.X * 0.5f,
		LocalBoundsSize.Y * 0.5f,
		LocalBoundsSize.Z * ZRatio
	);

	// OBB 트랜스폼: Local → Component → World
	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();
	FTransform LocalToWorld = SDFCache->LocalToComponent;
	if (SkelMesh)
	{
		LocalToWorld = LocalToWorld * SkelMesh->GetComponentTransform();
	}

	// 월드 공간에서의 슬라이스 위치/회전
	FVector WorldSliceCenter = LocalToWorld.TransformPosition(LocalSliceCenter);
	FQuat WorldRotation = LocalToWorld.GetRotation();

	// 평면 위치/회전 설정
	PlaneActor->SetActorLocation(WorldSliceCenter);
	PlaneActor->SetActorRotation(WorldRotation.Rotator());

	// 평면 스케일 (로컬 바운드 크기 + OBB 스케일 적용, 기본 Plane은 100x100 유닛)
	FVector OBBScale = LocalToWorld.GetScale3D();
	float ScaleX = (LocalBoundsSize.X * OBBScale.X) / 100.0f;
	float ScaleY = (LocalBoundsSize.Y * OBBScale.Y) / 100.0f;
	PlaneActor->SetActorScale3D(FVector(ScaleX, ScaleY, 1.0f));

	// 슬라이스 텍스처 업데이트
	UpdateSliceTexture(RingIndex, DebugSliceZ);

	// 화면에 슬라이스 정보 표시
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Cyan,
			FString::Printf(TEXT("Ring[%d] Slice Z: %d/%d"),
				RingIndex, DebugSliceZ, SDFCache->Resolution.Z));
	}
}

AActor* UFleshRingComponent::CreateDebugSlicePlane(int32 RingIndex)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return nullptr;
	}

	// 평면 액터 스폰
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* PlaneActor = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	if (!PlaneActor)
	{
		return nullptr;
	}

	// 루트 컴포넌트 생성
	USceneComponent* RootComp = NewObject<USceneComponent>(PlaneActor, TEXT("RootComponent"));
	PlaneActor->SetRootComponent(RootComp);
	RootComp->RegisterComponent();

	// StaticMeshComponent 생성 (기본 Plane 메시 사용) - 앞면
	UStaticMeshComponent* PlaneMeshFront = NewObject<UStaticMeshComponent>(PlaneActor, TEXT("PlaneMeshFront"));

	// 엔진 기본 Plane 메시 로드
	UStaticMesh* DefaultPlane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (DefaultPlane)
	{
		PlaneMeshFront->SetStaticMesh(DefaultPlane);
	}

	// 충돌 비활성화
	PlaneMeshFront->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PlaneMeshFront->SetCollisionResponseToAllChannels(ECR_Ignore);
	PlaneMeshFront->SetGenerateOverlapEvents(false);

	// 그림자 비활성화
	PlaneMeshFront->SetCastShadow(false);

	// 컴포넌트 등록 및 부착
	PlaneMeshFront->AttachToComponent(RootComp, FAttachmentTransformRules::KeepRelativeTransform);
	PlaneMeshFront->RegisterComponent();

	// 뒷면용 평면 추가 (180도 회전)
	UStaticMeshComponent* PlaneMeshBack = NewObject<UStaticMeshComponent>(PlaneActor, TEXT("PlaneMeshBack"));
	if (DefaultPlane)
	{
		PlaneMeshBack->SetStaticMesh(DefaultPlane);
	}
	PlaneMeshBack->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PlaneMeshBack->SetCollisionResponseToAllChannels(ECR_Ignore);
	PlaneMeshBack->SetGenerateOverlapEvents(false);
	PlaneMeshBack->SetCastShadow(false);
	PlaneMeshBack->AttachToComponent(RootComp, FAttachmentTransformRules::KeepRelativeTransform);
	PlaneMeshBack->SetRelativeRotation(FRotator(180.0f, 0.0f, 0.0f));  // X축으로 180도 회전
	PlaneMeshBack->RegisterComponent();

	// 렌더 타겟 생성
	if (DebugSliceRenderTargets.Num() <= RingIndex)
	{
		DebugSliceRenderTargets.SetNum(RingIndex + 1);
	}

	const FRingSDFCache* SDFCache = GetRingSDFCache(RingIndex);
	int32 Resolution = SDFCache ? SDFCache->Resolution.X : 64;

	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(this);
	RenderTarget->InitCustomFormat(Resolution, Resolution, PF_B8G8R8A8, false);
	RenderTarget->UpdateResourceImmediate(true);
	DebugSliceRenderTargets[RingIndex] = RenderTarget;

	// Widget3DPassThrough 머티리얼 사용 (텍스처를 그대로 표시)
	UMaterial* BaseMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/Widget3DPassThrough.Widget3DPassThrough"));
	if (!BaseMaterial)
	{
		BaseMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	}

	UMaterialInstanceDynamic* DynMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, PlaneActor);
	if (DynMaterial && RenderTarget)
	{
		DynMaterial->SetTextureParameterValue(TEXT("SlateUI"), RenderTarget);
		PlaneMeshFront->SetMaterial(0, DynMaterial);
		PlaneMeshBack->SetMaterial(0, DynMaterial);  // 뒷면에도 동일 머티리얼
	}

	UE_LOG(LogFleshRingComponent, Log, TEXT("Created debug slice plane for Ring[%d]"), RingIndex);

	return PlaneActor;
}

void UFleshRingComponent::UpdateSliceTexture(int32 RingIndex, int32 SliceZ)
{
	if (!DebugSliceRenderTargets.IsValidIndex(RingIndex))
	{
		return;
	}

	UTextureRenderTarget2D* RenderTarget = DebugSliceRenderTargets[RingIndex];
	if (!RenderTarget)
	{
		return;
	}

	const FRingSDFCache* SDFCache = GetRingSDFCache(RingIndex);
	if (!SDFCache || !SDFCache->IsValid())
	{
		return;
	}

	// GPU 작업: 캐싱된 SDF에서 슬라이스 추출
	TRefCountPtr<IPooledRenderTarget> SDFTexture = SDFCache->PooledTexture;
	FTextureRenderTargetResource* RTResource = RenderTarget->GameThread_GetRenderTargetResource();
	FIntVector Resolution = SDFCache->Resolution;
	int32 CapturedSliceZ = FMath::Clamp(SliceZ, 0, Resolution.Z - 1);

	ENQUEUE_RENDER_COMMAND(ExtractSDFSlice)(
		[SDFTexture, RTResource, Resolution, CapturedSliceZ](FRHICommandListImmediate& RHICmdList)
		{
			if (!SDFTexture.IsValid() || !RTResource)
			{
				return;
			}

			FRDGBuilder GraphBuilder(RHICmdList);

			// 캐싱된 SDF를 RDG에 등록
			FRDGTextureRef SDFTextureRDG = GraphBuilder.RegisterExternalTexture(SDFTexture);

			// 출력 텍스처 설정
			FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
				FIntPoint(Resolution.X, Resolution.Y),
				PF_B8G8R8A8,
				FClearValueBinding::Black,
				TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable
			);
			FRDGTextureRef OutputSlice = GraphBuilder.CreateTexture(OutputDesc, TEXT("DebugSDFSlice"));

			// 슬라이스 시각화 셰이더 실행
			GenerateSDFSlice(
				GraphBuilder,
				SDFTextureRDG,
				OutputSlice,
				Resolution,
				CapturedSliceZ,
				10.0f  // MaxDisplayDist
			);

			// 렌더 타겟으로 복사
			FRHITexture* DestTexture = RTResource->GetRenderTargetTexture();
			if (DestTexture)
			{
				AddCopyTexturePass(GraphBuilder, OutputSlice,
					GraphBuilder.RegisterExternalTexture(CreateRenderTarget(DestTexture, TEXT("DebugSliceRT"))));
			}

			GraphBuilder.Execute();
		}
	);
}

void UFleshRingComponent::CleanupDebugResources()
{
	// 슬라이스 평면 액터 제거
	for (AActor* PlaneActor : DebugSlicePlaneActors)
	{
		if (PlaneActor)
		{
			PlaneActor->Destroy();
		}
	}
	DebugSlicePlaneActors.Empty();

	// 렌더 타겟 정리
	DebugSliceRenderTargets.Empty();

	// 디버그 영향 버텍스 데이터 정리
	DebugAffectedData.Empty();
	DebugBindPoseVertices.Empty();
	bDebugAffectedVerticesCached = false;

	// 디버그 Bulge 버텍스 데이터 정리
	DebugBulgeData.Empty();
	bDebugBulgeVerticesCached = false;
}

void UFleshRingComponent::CacheAffectedVerticesForDebug()
{
	// 이미 캐싱되어 있으면 스킵
	if (bDebugAffectedVerticesCached)
	{
		return;
	}

	// 유효성 검사
	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();
	if (!SkelMesh || !FleshRingAsset)
	{
		return;
	}

	USkeletalMesh* Mesh = SkelMesh->GetSkeletalMeshAsset();
	if (!Mesh)
	{
		return;
	}

	// ===== 1. 바인드 포즈 버텍스 추출 =====
	const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		return;
	}

	// LOD 0 사용
	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];
	const uint32 NumVertices = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

	if (NumVertices == 0)
	{
		return;
	}

	DebugBindPoseVertices.Reset(NumVertices);
	for (uint32 VertexIdx = 0; VertexIdx < NumVertices; ++VertexIdx)
	{
		const FVector3f& Position = LODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(VertexIdx);
		DebugBindPoseVertices.Add(Position);
	}

	// ===== 2. Ring별 영향받는 버텍스 계산 =====
	DebugAffectedData.Reset();
	DebugAffectedData.SetNum(FleshRingAsset->Rings.Num());

	const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
	const TArray<FTransform>& RefBonePose = RefSkeleton.GetRefBonePose();

	for (int32 RingIdx = 0; RingIdx < FleshRingAsset->Rings.Num(); ++RingIdx)
	{
		const FFleshRingSettings& RingSettings = FleshRingAsset->Rings[RingIdx];

		// 본 인덱스 찾기
		const int32 BoneIndex = SkelMesh->GetBoneIndex(RingSettings.BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			continue;
		}

		// 바인드 포즈 본 트랜스폼 계산 (부모 체인 누적)
		FTransform BoneTransform = FTransform::Identity;
		int32 CurrentBoneIdx = BoneIndex;
		while (CurrentBoneIdx != INDEX_NONE)
		{
			BoneTransform = BoneTransform * RefBonePose[CurrentBoneIdx];
			CurrentBoneIdx = RefSkeleton.GetParentIndex(CurrentBoneIdx);
		}

		// SDF 캐시 가져오기
		const FRingSDFCache* SDFCache = GetRingSDFCache(RingIdx);

		// Context 생성
		FVertexSelectionContext Context(
			RingSettings,
			RingIdx,
			BoneTransform,
			DebugBindPoseVertices,
			SDFCache
		);

		// 영향받는 버텍스 선택
		FRingAffectedData& RingData = DebugAffectedData[RingIdx];
		RingData.BoneName = RingSettings.BoneName;
		RingData.RingCenter = BoneTransform.GetLocation();

		// Ring별 InfluenceMode에 따라 Selector 결정 (RegisterAffectedVertices와 동일 로직)
		TSharedPtr<IVertexSelector> RingSelector;
		const bool bUseSDFForThisRing =
			(RingSettings.InfluenceMode == EFleshRingInfluenceMode::Auto) &&
			(SDFCache && SDFCache->IsValid());

		if (bUseSDFForThisRing)
		{
			RingSelector = MakeShared<FSDFBoundsBasedVertexSelector>();
		}
		else
		{
			RingSelector = MakeShared<FDistanceBasedVertexSelector>();
		}
		RingSelector->SelectVertices(Context, RingData.Vertices);

		UE_LOG(LogFleshRingComponent, Log, TEXT("CacheAffectedVerticesForDebug: Ring[%d] '%s' - %d affected vertices, Mode=%s, Selector=%s"),
			RingIdx, *RingSettings.BoneName.ToString(), RingData.Vertices.Num(),
			RingSettings.InfluenceMode == EFleshRingInfluenceMode::Auto ? TEXT("Auto") : TEXT("Manual"),
			bUseSDFForThisRing ? TEXT("SDFBounds") : TEXT("Distance"));
	}

	bDebugAffectedVerticesCached = true;

	UE_LOG(LogFleshRingComponent, Log, TEXT("CacheAffectedVerticesForDebug: Cached %d rings, %d total vertices"),
		DebugAffectedData.Num(), DebugBindPoseVertices.Num());
}

void UFleshRingComponent::DrawBulgeHeatmap(int32 RingIndex)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// 캐싱되지 않았으면 먼저 캐싱
	if (!bDebugBulgeVerticesCached)
	{
		CacheBulgeVerticesForDebug();
	}

	// 데이터 유효성 검사
	if (!DebugBulgeData.IsValidIndex(RingIndex) ||
		DebugBindPoseVertices.Num() == 0)
	{
		return;
	}

	const FRingAffectedData& RingData = DebugBulgeData[RingIndex];
	if (RingData.Vertices.Num() == 0)
	{
		return;
	}

	// 현재 스켈레탈 메시 컴포넌트
	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();
	if (!SkelMesh)
	{
		return;
	}

	// 컴포넌트 → 월드 트랜스폼
	FTransform CompTransform = SkelMesh->GetComponentTransform();

	// 각 Bulge 영향 버텍스에 대해
	for (const FAffectedVertex& AffectedVert : RingData.Vertices)
	{
		if (!DebugBindPoseVertices.IsValidIndex(AffectedVert.VertexIndex))
		{
			continue;
		}

		// 바인드 포즈 위치 (컴포넌트 스페이스)
		const FVector3f& BindPosePos = DebugBindPoseVertices[AffectedVert.VertexIndex];

		// 월드 공간으로 변환
		FVector WorldPos = CompTransform.TransformPosition(FVector(BindPosePos));

		// 영향도에 따른 색상 (시안 → 마젠타 그라데이션, 높은 대비)
		float Influence = AffectedVert.Influence;
		float T = FMath::Clamp(Influence, 0.0f, 1.0f);

		// 시안(약함) → 마젠타(강함) 그라데이션 (스킨톤과 높은 대비)
		FColor PointColor(
			FMath::RoundToInt(255 * T),          // R: 0 → 255
			FMath::RoundToInt(255 * (1.0f - T)), // G: 255 → 0
			255                                  // B: 항상 255 (밝은 색 유지)
		);

		// 점 크기 (영향도에 비례, 더 크게)
		float PointSize = 5.0f + T * 7.0f;  // 5~12 범위

		// 외곽선 효과: 검은색 큰 점 먼저, 그 위에 색상 점
		DrawDebugPoint(World, WorldPos, PointSize + 2.0f, FColor::Black, false, -1.0f, SDPG_Foreground);
		DrawDebugPoint(World, WorldPos, PointSize, PointColor, false, -1.0f, SDPG_Foreground);
	}

	// 화면에 정보 표시
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Orange,
			FString::Printf(TEXT("Ring[%d] Bulge: %d vertices (Smoothstep filtered)"),
				RingIndex, RingData.Vertices.Num()));
	}
}

void UFleshRingComponent::CacheBulgeVerticesForDebug()
{
	// 이미 캐싱되어 있으면 스킵
	if (bDebugBulgeVerticesCached)
	{
		return;
	}

	// 유효성 검사
	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();
	if (!SkelMesh || !FleshRingAsset)
	{
		return;
	}

	// 바인드 포즈 버텍스가 없으면 캐싱
	if (DebugBindPoseVertices.Num() == 0)
	{
		CacheAffectedVerticesForDebug();
	}

	if (DebugBindPoseVertices.Num() == 0)
	{
		return;
	}

	// DebugBulgeData 초기화
	DebugBulgeData.Reset();
	DebugBulgeData.SetNum(FleshRingAsset->Rings.Num());

	for (int32 RingIdx = 0; RingIdx < FleshRingAsset->Rings.Num(); ++RingIdx)
	{
		const FFleshRingSettings& RingSettings = FleshRingAsset->Rings[RingIdx];
		FRingAffectedData& BulgeData = DebugBulgeData[RingIdx];
		BulgeData.BoneName = RingSettings.BoneName;

		// Bulge 비활성화면 스킵
		if (!RingSettings.bEnableBulge)
		{
			continue;
		}

		// SDF 캐시 가져오기
		const FRingSDFCache* SDFCache = GetRingSDFCache(RingIdx);
		if (!SDFCache || !SDFCache->IsValid())
		{
			continue;
		}

		// ===== GPU 셰이더와 동일한 로직으로 Bulge 버텍스 선택 =====

		// Ring 로컬 스페이스 변환
		FTransform ComponentToLocal = SDFCache->LocalToComponent.Inverse();
		FVector3f BoundsMin = SDFCache->BoundsMin;
		FVector3f BoundsMax = SDFCache->BoundsMax;
		FVector3f BoundsSize = BoundsMax - BoundsMin;
		FVector3f RingCenter = (BoundsMin + BoundsMax) * 0.5f;

		// Ring 축 감지 (가장 짧은 축)
		FVector3f RingAxis;
		if (BoundsSize.X <= BoundsSize.Y && BoundsSize.X <= BoundsSize.Z)
			RingAxis = FVector3f(1, 0, 0);
		else if (BoundsSize.Y <= BoundsSize.X && BoundsSize.Y <= BoundsSize.Z)
			RingAxis = FVector3f(0, 1, 0);
		else
			RingAxis = FVector3f(0, 0, 1);

		// Ring 크기 계산 (FleshRingBulgeProviders.cpp와 동일)
		const float RingWidth = FMath::Min3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z);  // 축 방향 크기
		const float RingRadius = FMath::Max3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z) * 0.5f;  // 반경 방향 크기

		// Bulge 시작 거리 (Ring 경계)
		const float BulgeStartDist = RingWidth * 0.5f;

		// 직교 범위 제한 (각 축 독립적으로 제어)
		// AxialLimit = 시작점 + 확장량 (AxialRange=1이면 RingWidth*0.5 만큼 확장)
		const float AxialLimit = BulgeStartDist + RingWidth * 0.5f * RingSettings.BulgeAxialRange;
		const float RadialLimit = RingRadius * RingSettings.BulgeRadialRange;

		// 방향 결정 (0 = 양방향)
		int32 DetectedDirection = SDFCache->DetectedBulgeDirection;
		int32 FinalDirection = 0;
		switch (RingSettings.BulgeDirection)
		{
		case EBulgeDirectionMode::Auto:
			FinalDirection = DetectedDirection;  // 0이면 양방향 (폐쇄 메시)
			break;
		case EBulgeDirectionMode::Bidirectional:
			FinalDirection = 0;  // 양방향
			break;
		case EBulgeDirectionMode::Positive:
			FinalDirection = 1;
			break;
		case EBulgeDirectionMode::Negative:
			FinalDirection = -1;
			break;
		}

		BulgeData.RingCenter = FVector(RingCenter);

		// 모든 버텍스 순회
		for (int32 VertIdx = 0; VertIdx < DebugBindPoseVertices.Num(); ++VertIdx)
		{
			// Component Space → Ring Local Space
			FVector CompSpacePos = FVector(DebugBindPoseVertices[VertIdx]);
			FVector LocalSpacePos = ComponentToLocal.TransformPosition(CompSpacePos);
			FVector3f LocalPos = FVector3f(LocalSpacePos);

			// Ring 중심으로부터의 벡터
			FVector3f ToVertex = LocalPos - RingCenter;

			// 1. 축 방향 거리 (위아래)
			float AxialComponent = FVector3f::DotProduct(ToVertex, RingAxis);
			float AxialDist = FMath::Abs(AxialComponent);

			// Bulge 시작점(Ring 경계) 이전은 제외 - Tightness 영역
			if (AxialDist < BulgeStartDist)
			{
				continue;
			}

			// 축 방향 범위 초과 체크
			if (AxialDist > AxialLimit)
			{
				continue;
			}

			// 2. 반경 방향 거리 (옆)
			FVector3f RadialVec = ToVertex - RingAxis * AxialComponent;
			float RadialDist = RadialVec.Size();

			// Axial 거리에 따라 RadialLimit 동적 확장 (몸이 위아래로 넓어지는 것 보정)
			const float AxialRatio = (AxialDist - BulgeStartDist) / FMath::Max(AxialLimit - BulgeStartDist, 0.001f);
			const float DynamicRadialLimit = RadialLimit * (1.0f + AxialRatio * 0.5f);

			// 반경 방향 범위 초과 체크 (다른 허벅지 영향 방지)
			if (RadialDist > DynamicRadialLimit)
			{
				continue;
			}

			// 3. 방향 필터링 (FinalDirection != 0이면 한쪽만)
			if (FinalDirection != 0)
			{
				int32 VertexSide = (AxialComponent > 0.0f) ? 1 : -1;
				if (VertexSide != FinalDirection)
				{
					continue;
				}
			}

			// 4. 축 방향 거리 기반 Smoothstep 감쇠
			// Ring 경계에서 1.0, AxialLimit에서 0으로 부드럽게 감쇠
			const float AxialFalloffRange = AxialLimit - BulgeStartDist;
			float NormalizedDist = (AxialDist - BulgeStartDist) / FMath::Max(AxialFalloffRange, 0.001f);
			float ClampedDist = FMath::Clamp(NormalizedDist, 0.0f, 1.0f);

			// Smoothstep: 1 → 0 (가까울수록 강함)
			float t = 1.0f - ClampedDist;
			float BulgeInfluence = t * t * (3.0f - 2.0f * t);  // Hermite smoothstep

			if (BulgeInfluence > KINDA_SMALL_NUMBER)
			{
				FAffectedVertex BulgeVert;
				BulgeVert.VertexIndex = VertIdx;
				BulgeVert.Influence = BulgeInfluence;
				BulgeData.Vertices.Add(BulgeVert);
			}
		}

		const TCHAR* ModeStr = TEXT("Unknown");
		switch (RingSettings.BulgeDirection)
		{
		case EBulgeDirectionMode::Auto: ModeStr = TEXT("Auto"); break;
		case EBulgeDirectionMode::Bidirectional: ModeStr = TEXT("Both"); break;
		case EBulgeDirectionMode::Positive: ModeStr = TEXT("Positive"); break;
		case EBulgeDirectionMode::Negative: ModeStr = TEXT("Negative"); break;
		}
		UE_LOG(LogFleshRingComponent, Log, TEXT("CacheBulgeVerticesForDebug: Ring[%d] - %d Bulge vertices (Direction: %d, Detected: %d, Mode: %s, RingAxis: %s)"),
			RingIdx, BulgeData.Vertices.Num(), FinalDirection, DetectedDirection, ModeStr, *RingAxis.ToString());
	}

	bDebugBulgeVerticesCached = true;
}

void UFleshRingComponent::DrawBulgeDirectionArrow(int32 RingIndex)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// SDF 캐시와 Ring 설정 가져오기
	const FRingSDFCache* SDFCache = GetRingSDFCache(RingIndex);
	if (!SDFCache || !SDFCache->IsValid())
	{
		return;
	}

	if (!FleshRingAsset || !FleshRingAsset->Rings.IsValidIndex(RingIndex))
	{
		return;
	}

	const FFleshRingSettings& RingSettings = FleshRingAsset->Rings[RingIndex];

	// Bulge 비활성화면 스킵
	if (!RingSettings.bEnableBulge)
	{
		return;
	}

	// 감지된 방향
	int32 DetectedDirection = SDFCache->DetectedBulgeDirection;

	// 최종 사용 방향 결정 (0 = 양방향)
	int32 FinalDirection = 0;
	switch (RingSettings.BulgeDirection)
	{
	case EBulgeDirectionMode::Auto:
		FinalDirection = DetectedDirection;  // 0이면 양방향
		break;
	case EBulgeDirectionMode::Bidirectional:
		FinalDirection = 0;  // 양방향
		break;
	case EBulgeDirectionMode::Positive:
		FinalDirection = 1;
		break;
	case EBulgeDirectionMode::Negative:
		FinalDirection = -1;
		break;
	}

	// OBB 중심 위치 (월드 공간)
	FVector LocalCenter = FVector(SDFCache->BoundsMin + SDFCache->BoundsMax) * 0.5f;

	// Component → World 트랜스폼
	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();
	FTransform LocalToWorld = SDFCache->LocalToComponent;
	if (SkelMesh)
	{
		LocalToWorld = LocalToWorld * SkelMesh->GetComponentTransform();
	}

	FVector WorldCenter = LocalToWorld.TransformPosition(LocalCenter);
	FQuat WorldRotation = LocalToWorld.GetRotation();

	// 로컬 Z축을 월드 공간으로 변환
	FVector LocalZAxis = FVector(0.0f, 0.0f, 1.0f);
	FVector WorldZAxis = WorldRotation.RotateVector(LocalZAxis);

	// 화살표 크기 (SDF 볼륨 크기에 비례, 작게 유지)
	float ArrowLength = FVector(SDFCache->BoundsMax - SDFCache->BoundsMin).Size() * 0.05f;

	// 화살표 색상: 검은색으로 통일
	FColor ArrowColor = FColor::White;

	// 화살표 그리기 (SDPG_Foreground로 메시 앞에 표시)
	if (bShowBulgeArrows)
	{
		const float ArrowHeadSize = 0.5f;  // 화살표 머리 크기
		const float ArrowThickness = 0.5f; // 화살표 두께

		if (FinalDirection == 0)
		{
			// 양방향: 위아래 둘 다 화살표 그리기
			FVector ArrowEndUp = WorldCenter + WorldZAxis * ArrowLength;
			FVector ArrowEndDown = WorldCenter - WorldZAxis * ArrowLength;
			DrawDebugDirectionalArrow(World, WorldCenter, ArrowEndUp, ArrowHeadSize, ArrowColor, false, -1.0f, SDPG_Foreground, ArrowThickness);
			DrawDebugDirectionalArrow(World, WorldCenter, ArrowEndDown, ArrowHeadSize, ArrowColor, false, -1.0f, SDPG_Foreground, ArrowThickness);
		}
		else
		{
			// 단방향
			FVector ArrowDirection = WorldZAxis * static_cast<float>(FinalDirection);
			FVector ArrowEnd = WorldCenter + ArrowDirection * ArrowLength;
			DrawDebugDirectionalArrow(World, WorldCenter, ArrowEnd, ArrowHeadSize, ArrowColor, false, -1.0f, SDPG_Foreground, ArrowThickness);
		}
	}

	// 화면에 정보 표시
	if (GEngine)
	{
		FString ModeStr;
		switch (RingSettings.BulgeDirection)
		{
		case EBulgeDirectionMode::Auto: ModeStr = TEXT("Auto"); break;
		case EBulgeDirectionMode::Bidirectional: ModeStr = TEXT("Both"); break;
		case EBulgeDirectionMode::Positive: ModeStr = TEXT("+Z"); break;
		case EBulgeDirectionMode::Negative: ModeStr = TEXT("-Z"); break;
		}
		GEngine->AddOnScreenDebugMessage(-1, 0.0f, ArrowColor,
			FString::Printf(TEXT("Ring[%d] Bulge Dir: %s (Detected: %d, Final: %d)"),
				RingIndex, *ModeStr, DetectedDirection, FinalDirection));
	}
}

#endif // WITH_EDITOR
