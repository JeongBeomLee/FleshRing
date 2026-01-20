// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "FleshRingUtils.h"
#include "FleshRingMeshComponent.h"
#include "FleshRingMeshExtractor.h"
#include "FleshRingSDF.h"
#include "FleshRingProceduralMesh.h"
#include "FleshRingProceduralBandSDF.h"
#include "FleshRingDeformerInstance.h"
#include "FleshRingBulgeTypes.h"
#include "FleshRingFalloff.h"
#include "Engine/StaticMesh.h"
#include "Engine/VolumeTexture.h"
#include "GameFramework/Actor.h"
#include "RenderGraphBuilder.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#if WITH_EDITOR
#include "DrawDebugHelpers.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Components/StaticMeshComponent.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "SceneViewExtension.h"
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

bool UFleshRingComponent::HasAnyNonSDFRings() const
{
	if (!FleshRingAsset)
	{
		return false;
	}
	for (const FFleshRingSettings& RingSettings : FleshRingAsset->Rings)
	{
		// Manual 또는 ProceduralBand 모드는 SDF 없이 동작 (거리 기반 로직)
		if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::Manual ||
			RingSettings.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
		{
			return true;
		}
	}
	return false;
}

UFleshRingComponent::UFleshRingComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UFleshRingComponent::BeginPlay()
{
	Super::BeginPlay();

	if (bEnableFleshRing && FleshRingAsset)
	{
		// ★ 런타임 로직: 베이크된 메시가 있으면 적용, 없으면 아무것도 안 함
		// - Deformer(실시간 GPU 변형)는 에디터 프리뷰용으로만 사용
		// - 런타임에서는 베이크된 결과만 사용
		if (FleshRingAsset->HasBakedMesh())
		{
			if (!ResolvedTargetMesh.IsValid())
			{
				FindTargetMeshOnly();
			}
			ApplyBakedMesh();
			UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Using baked mesh for runtime"));
		}
		else
		{
			// ★ 베이크된 메시 없음: 원본 메시 그대로 사용
			// - 스켈레탈 메시는 원본 그대로 애니메이션됨
			// - Ring 메시는 OnRegister()에서 이미 본에 부착됨
			UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: No baked mesh, using original mesh with ring attachments"));
		}
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
	//
	// ★ 게임 월드에서는 메시 변경 없이 대상만 찾음
	// SetSkeletalMesh()가 OnRegister 시점에 호출되면 애니메이션 초기화를 방해함
	// 메시 변경이 필요한 경우 BeginPlay에서 처리
	bool bIsGameWorld = GetWorld() && GetWorld()->IsGameWorld();
	if (bIsGameWorld)
	{
		// 대상 메시만 찾음 (메시 변경 없음)
		FindTargetMeshOnly();
	}
	else
	{
		// 에디터: 전체 처리 (프리뷰 메시 적용 등)
		ResolveTargetMesh();
	}
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

	// Bulge Heatmap 활성화 시 캐시 무효화 (즉시 디버그 포인트 표시를 위해)
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UFleshRingComponent, bShowBulgeHeatmap))
	{
		if (bShowBulgeHeatmap && InternalDeformer)
		{
			if (UFleshRingDeformerInstance* DeformerInstance = InternalDeformer->GetActiveInstance())
			{
				DeformerInstance->InvalidateTightnessCache();
			}
		}
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
}

void UFleshRingComponent::FindTargetMeshOnly()
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
}

void UFleshRingComponent::ResolveTargetMesh()
{
	// 대상 메시 찾기
	FindTargetMeshOnly();

	// SubdividedMesh 또는 원본 메시 적용
	UE_LOG(LogFleshRingComponent, Log, TEXT("ResolveTargetMesh: Checking SubdividedMesh... ResolvedTargetMesh.IsValid()=%d, FleshRingAsset=%p"),
		ResolvedTargetMesh.IsValid(), FleshRingAsset.Get());

	if (ResolvedTargetMesh.IsValid() && FleshRingAsset)
	{
		USkeletalMeshComponent* TargetMeshComp = ResolvedTargetMesh.Get();
		USkeletalMesh* CurrentMesh = TargetMeshComp->GetSkeletalMeshAsset();

		UE_LOG(LogFleshRingComponent, Log, TEXT("ResolveTargetMesh: HasSubdividedMesh=%d, SubdividedMesh=%p, CurrentMesh='%s'"),
			FleshRingAsset->HasSubdividedMesh(),
			FleshRingAsset->SubdivisionSettings.SubdividedMesh.Get(),
			CurrentMesh ? *CurrentMesh->GetName() : TEXT("null"));

		if (FleshRingAsset->HasSubdividedMesh())
		{
			// SubdividedMesh가 있으면 적용
			USkeletalMesh* SubdivMesh = FleshRingAsset->SubdivisionSettings.SubdividedMesh;
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
			// (PreviewSubdividedMesh 체크는 함수 상단에서 이미 처리됨)
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

	// ★ CL 320 복원: Optimus와 동일하게 초기화 시점에 render state 갱신 요청
	// - MarkRenderStateDirty: PassthroughVertexFactory 생성을 위해 render state 재생성
	// - MarkRenderDynamicDataDirty: 동적 데이터 갱신 요청
	// 주의: TickComponent에서는 호출하지 않음 (엔진이 자동으로 처리)
	TargetMesh->MarkRenderStateDirty();
	TargetMesh->MarkRenderDynamicDataDirty();

	// Bounds 확장: Deformer 변형이 원래 bounds를 벗어날 수 있으므로
	// VSM(Virtual Shadow Maps) 등 bounds 기반 캐싱 시스템이 정상 작동하도록 확장
	TargetMesh->SetBoundsScale(BoundsScale);

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

		// 2. 이전 DeformerInstance 명시적 파괴 (메모리 누수 방지)
		// SetMeshDeformer(nullptr)는 포인터만 해제하고 Instance를 파괴하지 않음
		if (UMeshDeformerInstance* OldInstance = TargetMesh->GetMeshDeformerInstance())
		{
			OldInstance->MarkAsGarbage();
			OldInstance->ConditionalBeginDestroy();
		}

		// 3. Deformer 해제
		TargetMesh->SetMeshDeformer(nullptr);

		// 4. Render State를 dirty로 마킹하여 Scene Proxy 재생성 트리거
		// VertexFactory가 올바르게 재초기화되도록 함
		TargetMesh->MarkRenderStateDirty();

		// 5. 새 렌더 스테이트가 적용될 때까지 대기
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

	// 베이크 모드 플래그 리셋
	bUsingBakedMesh = false;
}

void UFleshRingComponent::ReinitializeDeformer()
{
	USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
	if (!TargetMesh)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("ReinitializeDeformer: No target mesh"));
		return;
	}

	// 1. 진행 중인 렌더 작업 완료 대기
	FlushRenderingCommands();

	// 2. 이전 DeformerInstance 명시적 파괴
	if (InternalDeformer)
	{
		if (UMeshDeformerInstance* OldInstance = TargetMesh->GetMeshDeformerInstance())
		{
			OldInstance->MarkAsGarbage();
			OldInstance->ConditionalBeginDestroy();
		}
		TargetMesh->SetMeshDeformer(nullptr);
	}

	// 3. Render State 재생성 트리거
	TargetMesh->MarkRenderStateDirty();
	FlushRenderingCommands();

	// 4. 새 Deformer 생성 (기존 InternalDeformer 객체는 재사용하지 않고 새로 생성)
	InternalDeformer = NewObject<UFleshRingDeformer>(this, TEXT("InternalFleshRingDeformer"));
	if (!InternalDeformer)
	{
		UE_LOG(LogFleshRingComponent, Error, TEXT("ReinitializeDeformer: Failed to create new deformer"));
		return;
	}

	// 5. 새 Deformer 등록
	TargetMesh->SetMeshDeformer(InternalDeformer);
	TargetMesh->SetBoundsScale(BoundsScale);
	TargetMesh->MarkRenderStateDirty();
	TargetMesh->MarkRenderDynamicDataDirty();

	UE_LOG(LogFleshRingComponent, Log, TEXT("ReinitializeDeformer: Deformer recreated for mesh '%s' (%d vertices)"),
		*TargetMesh->GetSkeletalMeshAsset()->GetName(),
		TargetMesh->GetSkeletalMeshAsset()->GetResourceForRendering()->LODRenderData[0].StaticVertexBuffers.PositionVertexBuffer.GetNumVertices());
}

void UFleshRingComponent::GenerateSDF()
{
	// 이전 렌더 커맨드 완료 대기
	FlushRenderingCommands();

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

	// 각 Ring의 RingMesh 또는 ProceduralBand에서 SDF 생성
	for (int32 RingIndex = 0; RingIndex < FleshRingAsset->Rings.Num(); ++RingIndex)
	{
		const FFleshRingSettings& Ring = FleshRingAsset->Rings[RingIndex];

		// ===== ProceduralBand 모드: SDF 불필요, 스킵 (거리 기반 로직 사용) =====
		// ProceduralBand 모드는 FVirtualBandVertexSelector/FVirtualBandInfluenceProvider로
		// BandSettings 파라미터를 직접 사용하여 거리 기반 Tight/Bulge 계산
		// SDF 텍스처 없이 동작하므로 생성 스킵
		if (Ring.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
		{
			UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Ring[%d] is ProceduralBand mode, SDF generation skipped (using distance-based logic)"), RingIndex);
			continue;
		}

		// ===== Manual 모드: SDF 불필요, 스킵 =====
		// Manual 모드는 Ring 파라미터(RingOffset/RingRotation/RingRadius 등)만 사용
		// Ring Mesh가 있어도 SDF를 생성하면 안 됨 (시각화용 메시일 뿐)
		if (Ring.InfluenceMode == EFleshRingInfluenceMode::Manual)
		{
			UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Ring[%d] is Manual mode, SDF generation skipped"), RingIndex);
			continue;
		}

		// ===== Auto 모드: StaticMesh에서 SDF 생성 =====
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

		// 4. Bounds 계산 (SDF 텍스처용 - 원래 바운드 유지)
		// NOTE: SDFBoundsExpandX/Y는 SDF 텍스처 바운드에 적용하지 않음
		// 이유 1: 에디터에서 Expand 값 조절 시 매번 SDF 재생성 → 성능/메모리 문제
		// 이유 2: 바운드 확장 시 SDF 해상도 밀도 저하 → 링 형태 품질 저하
		// 이유 3: 패딩 시 얇은 링에서 Flood Fill 실패 (벽이 얇아져 누출)
		// 탄젠트 영역 문제: 셰이더에서 최소 스텝으로 해결 (FleshRingTightnessCS.usf)
		FVector3f BoundsMin = MeshData.Bounds.Min;
		FVector3f BoundsMax = MeshData.Bounds.Max;

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

	// SDF 생성 렌더 커맨드가 완료될 때까지 대기
	// 이렇게 해야 GenerateSDF() 반환 후 SDFCache->IsValid()가 true가 됨
	// (비동기 생성 시 모드 전환 후 첫 프레임에서 SDF가 아직 없는 문제 해결)
	FlushRenderingCommands();

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

	// 유효한 SDF 캐시가 있거나 Manual 모드 Ring이 있어야 Deformer 설정
	// (Auto 모드 SDF 실패는 여전히 개별 스킵, Manual 모드는 SDF 없이 작동)
	if (!HasAnyValidSDFCaches() && !HasAnyNonSDFRings())
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("InitializeForEditorPreview: No valid SDF caches and no Manual mode rings, skipping Deformer setup"));
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

void UFleshRingComponent::ForceInitializeForEditorPreview()
{
	UE_LOG(LogFleshRingComponent, Log, TEXT("ForceInitializeForEditorPreview: Resetting and reinitializing..."));

	// 초기화 플래그 리셋
	bEditorPreviewInitialized = false;

	// 기존 Deformer 정리 (메시 변경 시 버텍스 수 불일치 방지)
	if (InternalDeformer)
	{
		CleanupDeformer();
	}

	// 재초기화
	InitializeForEditorPreview();
}

void UFleshRingComponent::UpdateRingTransforms(int32 DirtyRingIndex)
{
	if (!FleshRingAsset || !ResolvedTargetMesh.IsValid())
	{
		return;
	}

	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();

	// 업데이트할 Ring 범위 결정
	int32 StartIndex = (DirtyRingIndex != INDEX_NONE) ? DirtyRingIndex : 0;
	int32 EndIndex = (DirtyRingIndex != INDEX_NONE) ? DirtyRingIndex + 1 : FleshRingAsset->Rings.Num();

	// 유효 범위 검증
	StartIndex = FMath::Clamp(StartIndex, 0, FleshRingAsset->Rings.Num());
	EndIndex = FMath::Clamp(EndIndex, 0, FleshRingAsset->Rings.Num());

	for (int32 RingIndex = StartIndex; RingIndex < EndIndex; ++RingIndex)
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

			//// [DEBUG] MeshRotation 업데이트 확인
			//FQuat FinalRot = LocalToComponentTransform.GetRotation();
			//UE_LOG(LogFleshRingComponent, Log, TEXT("[DEBUG] UpdateRingTransforms Ring[%d]: MeshRot=(%f,%f,%f,%f), FinalRot=(%f,%f,%f,%f) [Component=%p]"),
			//	RingIndex,
			//	Ring.MeshRotation.X, Ring.MeshRotation.Y, Ring.MeshRotation.Z, Ring.MeshRotation.W,
			//	FinalRot.X, FinalRot.Y, FinalRot.Z, FinalRot.W,
			//	this);
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
	// DirtyRingIndex를 전달하여 해당 Ring만 재처리
	if (USkeletalMeshComponent* SkelMeshComp = ResolvedTargetMesh.Get())
	{
		if (UMeshDeformerInstance* DeformerInstance = SkelMeshComp->GetMeshDeformerInstance())
		{
			if (UFleshRingDeformerInstance* FleshRingInstance = Cast<UFleshRingDeformerInstance>(DeformerInstance))
			{
				FleshRingInstance->InvalidateTightnessCache(DirtyRingIndex);
			}
		}

		// 4. 렌더 시스템에 동적 데이터 변경 알림 (실시간 변형 반영)
		SkelMeshComp->MarkRenderDynamicDataDirty();
	}

#if WITH_EDITORONLY_DATA
	// 5. 디버그 시각화 캐시 무효화 (Ring 이동 시 AffectedVertices 재계산)
	// DirtyRingIndex를 전달하여 해당 Ring만 무효화
	InvalidateDebugCaches(DirtyRingIndex);
#endif
}

void UFleshRingComponent::RefreshRingMeshes()
{
#if WITH_EDITOR
	// Ring 삭제 시 디버그 리소스(SDF 슬라이스 액터 등)도 정리
	CleanupDebugResources();
#endif
	CleanupRingMeshes();
	SetupRingMeshes();
}

bool UFleshRingComponent::RefreshWithDeformerReuse()
{
	// Deformer 재사용 가능 여부 확인
	if (!InternalDeformer || !ResolvedTargetMesh.IsValid() || !bEnableFleshRing)
	{
		return false;
	}

	// ★ Deformer가 실제로 SkeletalMeshComponent에 설정되어 있는지 확인
	// PreviewScene에서 메시 변경 시 Deformer를 먼저 해제하므로 이 체크 필요
	USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
	if (TargetMesh && !TargetMesh->GetMeshDeformerInstance())
	{
		// ★ SDF 캐시 먼저 정리 (InternalDeformer를 null로 설정하면 CleanupDeformer에서 정리 안 됨)
		FlushRenderingCommands();
		for (FRingSDFCache& Cache : RingSDFCaches)
		{
			Cache.Reset();
		}
		RingSDFCaches.Empty();

		// DeformerInstance가 없으면 Deformer가 해제된 것으로 간주
		InternalDeformer = nullptr;
		return false;
	}

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: RefreshWithDeformerReuse - Reusing existing Deformer (avoiding GPU resource leak)"));

	// 렌더 커맨드 완료 대기 (이전 SDF 생성 커맨드가 완료되어야 캐시 해제 가능)
	FlushRenderingCommands();

	// SDF 캐시만 정리 (Deformer는 유지)
	for (FRingSDFCache& Cache : RingSDFCaches)
	{
		Cache.Reset();
	}
	RingSDFCaches.Empty();

	// SDF 재생성
	GenerateSDF();

	// Ring 메시 갱신
	CleanupRingMeshes();
	SetupRingMeshes();

	// DeformerInstance의 Tightness 캐시 무효화 (Ring 변경 반영)
	if (USkeletalMeshComponent* SkelMeshComp = ResolvedTargetMesh.Get())
	{
		if (UFleshRingDeformerInstance* DeformerInstance = Cast<UFleshRingDeformerInstance>(SkelMeshComp->GetMeshDeformerInstance()))
		{
			DeformerInstance->InvalidateTightnessCache();
		}
	}

#if WITH_EDITORONLY_DATA
	// 디버그 캐시 무효화 (Thickness 등 변경 시 AffectedVertices 재계산 필요)
	// GetDebugPointCount()가 옛날 값을 반환하면 버퍼 크기 불일치로 크래시 발생
	bDebugAffectedVerticesCached = false;
	bDebugBulgeVerticesCached = false;

	// 디버그 배열 크기 재조정 (Ring 추가/제거 시 배열 크기 변경 필요)
	DebugAffectedData.Reset();
	DebugBulgeData.Reset();
#endif

	return true;
}

void UFleshRingComponent::ApplyAsset()
{
	if (!FleshRingAsset)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: ApplyAsset called but FleshRingAsset is null"));
		return;
	}

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Applying asset '%s'"), *FleshRingAsset->GetName());

	// Deformer가 이미 존재하면 재사용 (GPU 메모리 누수 방지)
	if (RefreshWithDeformerReuse())
	{
		return;
	}

	// 기존 설정 정리 후 재설정 (최초 설정 또는 Deformer가 없는 경우)
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
			bool bIsSubdividedMesh = FleshRingAsset->HasSubdividedMesh() && ActualMesh == FleshRingAsset->SubdivisionSettings.SubdividedMesh;

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

void UFleshRingComponent::SwapFleshRingAsset(UFleshRingAsset* NewAsset)
{
	// nullptr 전달 시 원본 메시로 복원 + 에셋 해제
	if (!NewAsset)
	{
		UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: SwapFleshRingAsset(nullptr) - restoring original mesh"));

		// 기존 에셋 정리
		CleanupRingMeshes();
		if (InternalDeformer)
		{
			CleanupDeformer();
		}

		// 원본 메시 복원
		// SetSkeletalMeshAsset은 애니메이션 상태를 자동으로 보존함
		USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
		if (TargetMesh && CachedOriginalMesh.IsValid())
		{
			TargetMesh->SetSkeletalMeshAsset(CachedOriginalMesh.Get());
		}

		FleshRingAsset = nullptr;
		bUsingBakedMesh = false;

		return;
	}

	// 베이크된 메시가 없으면 일반 ApplyAsset 사용
	if (!NewAsset->HasBakedMesh())
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: NewAsset has no baked mesh, using regular ApplyAsset"));
		FleshRingAsset = NewAsset;
		ApplyAsset();
		return;
	}

	// 기존 에셋 정리
	CleanupRingMeshes();
	if (InternalDeformer)
	{
		CleanupDeformer();
	}

	// 새 에셋 설정
	FleshRingAsset = NewAsset;

	// 베이크된 메시 적용
	// ResolvedTargetMesh가 이미 유효하면 ResolveTargetMesh 호출 불필요
	// (ResolveTargetMesh는 SubdividedMesh를 적용하려 하므로 애니메이션이 리셋됨)
	if (!ResolvedTargetMesh.IsValid())
	{
		ResolveTargetMesh();
	}
	ApplyBakedMesh();

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Swapped to baked asset '%s'"), *NewAsset->GetName());
}

void UFleshRingComponent::ApplyBakedMesh()
{
	if (!FleshRingAsset || !FleshRingAsset->HasBakedMesh())
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: ApplyBakedMesh called but no baked mesh available"));
		return;
	}

	USkeletalMeshComponent* TargetMesh = ResolvedTargetMesh.Get();
	if (!TargetMesh)
	{
		UE_LOG(LogFleshRingComponent, Warning, TEXT("FleshRingComponent: ApplyBakedMesh - no target mesh"));
		return;
	}

	// 원본 메시 저장 (나중에 복원용)
	if (!CachedOriginalMesh.IsValid())
	{
		CachedOriginalMesh = TargetMesh->GetSkeletalMeshAsset();
	}

	// 베이크된 메시 적용
	// SetSkeletalMeshAsset은 애니메이션 상태를 자동으로 보존함
	USkeletalMesh* BakedMesh = FleshRingAsset->SubdivisionSettings.BakedMesh;
	TargetMesh->SetSkeletalMeshAsset(BakedMesh);

	// Bounds 확장 (변형이 이미 적용되어 있지만 안전을 위해)
	TargetMesh->SetBoundsScale(BoundsScale);

	// 렌더 스테이트 갱신
	TargetMesh->MarkRenderStateDirty();

	// Ring 메시 설정 및 베이크된 트랜스폼 적용
	SetupRingMeshes();
	ApplyBakedRingTransforms();

	// 베이크 모드 플래그 설정
	bUsingBakedMesh = true;

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: Applied baked mesh '%s'"),
		BakedMesh ? *BakedMesh->GetName() : TEXT("null"));
}

void UFleshRingComponent::ApplyBakedRingTransforms()
{
	if (!FleshRingAsset)
	{
		return;
	}

	const TArray<FTransform>& BakedTransforms = FleshRingAsset->SubdivisionSettings.BakedRingTransforms;

	// 베이크된 트랜스폼이 없으면 스킵 (기본 본 위치 사용)
	if (BakedTransforms.Num() == 0)
	{
		return;
	}

	// 각 Ring 메시에 베이크된 트랜스폼 적용
	for (int32 RingIndex = 0; RingIndex < RingMeshComponents.Num(); ++RingIndex)
	{
		UFleshRingMeshComponent* MeshComp = RingMeshComponents[RingIndex];
		if (!MeshComp)
		{
			continue;
		}

		if (BakedTransforms.IsValidIndex(RingIndex))
		{
			// 베이크된 트랜스폼은 컴포넌트 스페이스 기준
			// 본에 부착된 상태이므로 상대 트랜스폼으로 설정
			const FTransform& BakedTransform = BakedTransforms[RingIndex];
			MeshComp->SetRelativeTransform(BakedTransform);

			UE_LOG(LogFleshRingComponent, Verbose, TEXT("FleshRingComponent: Ring[%d] applied baked transform: Loc=%s Rot=%s"),
				RingIndex,
				*BakedTransform.GetLocation().ToString(),
				*BakedTransform.GetRotation().Rotator().ToString());
		}
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

		// ProceduralBand 모드: 기즈모로 피킹 (Manual 모드와 동일 방식)
		// SDF 생성은 GenerateSDF()에서 직접 처리하므로 여기서는 메시 컴포넌트 생성 안 함
		if (Ring.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
		{
			RingMeshComponents.Add(nullptr);
			continue;
		}

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
		// GPU 디버그 렌더링 ViewExtension도 비활성화
		if (bUseGPUDebugRendering && DebugViewExtension.IsValid())
		{
			DebugViewExtension->ClearDebugPointBuffer();
			DebugViewExtension->ClearDebugBulgePointBuffer();
		}
		return;
	}

	// Ring 개수는 Asset 기준
	const int32 NumRings = FleshRingAsset ? FleshRingAsset->Rings.Num() : 0;

	// 유효한 SDF Ring 개수 계산 (DebugSlicePlaneActors는 SDF가 있는 Ring에만 생성됨)
	int32 NumValidSDFRings = 0;
	for (const FRingSDFCache& Cache : RingSDFCaches)
	{
		if (Cache.IsValid())
		{
			NumValidSDFRings++;
		}
	}

	// Ring 개수가 변경되면 디버그 리소스 정리 후 재생성
	// (중간 Ring 삭제 시 인덱스 어긋남 방지)
	// NOTE: DebugSlicePlaneActors는 Ring 인덱스 기반 배열이므로 NumRings와 비교
	if (DebugSlicePlaneActors.Num() != NumRings)
	{
		// [DEBUG] SlicePlane 재생성 로그 (필요시 주석 해제)
		// UE_LOG(LogFleshRingComponent, Warning, TEXT("[DEBUG] SlicePlane RECREATE: DebugSlicePlaneActors=%d, NumRings=%d"),
		// 	DebugSlicePlaneActors.Num(), NumRings);

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

	// 배열 크기를 NumRings로 미리 확보 (Manual 모드 Ring 슬롯도 nullptr로 유지)
	if (DebugSlicePlaneActors.Num() < NumRings)
	{
		DebugSlicePlaneActors.SetNum(NumRings);
	}
	if (DebugSliceRenderTargets.Num() < NumRings)
	{
		DebugSliceRenderTargets.SetNum(NumRings);
	}

	if (DebugAffectedData.Num() != NumRings)
	{
		bDebugAffectedVerticesCached = false;
	}
	if (DebugBulgeData.Num() != NumRings)
	{
		bDebugBulgeVerticesCached = false;
	}

	// GPU 디버그 렌더링 모드: ViewExtension + 셰이더로 원형 포인트 렌더링
	// ★ DrawDebug 방식: 매 프레임 GPU에서 새로 계산, CPU 캐시 불필요
	// PointCount는 렌더 스레드에서 버퍼의 NumElements로 직접 읽음
	if (bUseGPUDebugRendering)
	{
		UpdateDebugPointBuffer();
		UpdateDebugBulgePointBuffer();
	}

	for (int32 RingIndex = 0; RingIndex < NumRings; ++RingIndex)
	{
		if (bShowSdfVolume)
		{
			DrawSdfVolume(RingIndex);
		}

		// GPU 렌더링 모드가 아닐 때만 CPU DrawDebugPoint 사용
		if (bShowAffectedVertices && !bUseGPUDebugRendering)
		{
			DrawAffectedVertices(RingIndex);
		}

		if (bShowSDFSlice)
		{
			DrawSDFSlice(RingIndex);
		}

		// Virtual Band debug wireframe 비활성화 (코드 보존)
		// if (bShowProceduralBandWireframe)
		// {
		// 	DrawProceduralBandWireframe(RingIndex);
		// }

		if (bShowBulgeHeatmap)
		{
			// GPU 렌더링 모드가 아닐 때만 CPU DrawDebugPoint 사용
			if (!bUseGPUDebugRendering)
			{
				DrawBulgeHeatmap(RingIndex);
			}
			// 방향 화살표는 항상 표시
			DrawBulgeDirectionArrow(RingIndex);
		}

		if (bShowBulgeRange)
		{
			DrawBulgeRange(RingIndex);
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

	FColor BracketColor = FColor(130, 200, 255, 160);  // 파란색 (SDF 텍스처 바운드)
	FColor ExpandedBracketColor = FColor(80, 220, 80, 160);  // 초록색 (확장 바운드)
	float LineThickness = 0.20f;
	float BracketRatio = 0.25f;

	// OBB의 로컬 축 방향 (월드 공간)
	FVector AxisX = WorldRotation.RotateVector(FVector::ForwardVector);  // X축
	FVector AxisY = WorldRotation.RotateVector(FVector::RightVector);    // Y축
	FVector AxisZ = WorldRotation.RotateVector(FVector::UpVector);       // Z축

	// 각 축별 브라켓 길이
	float BracketLenX = ScaledExtent.X * 2.0f * BracketRatio;
	float BracketLenY = ScaledExtent.Y * 2.0f * BracketRatio;
	float BracketLenZ = ScaledExtent.Z * 2.0f * BracketRatio;

	// 8개 코너 계산 및 브라켓 그리기 (SDF 텍스처 바운드 - 파란색)
	// 코너 = Center + (±ExtentX * AxisX) + (±ExtentY * AxisY) + (±ExtentZ * AxisZ)
	for (int32 i = 0; i < 8; ++i)
	{
		// 비트마스크로 코너 위치 결정 (0=Min, 1=Max)
		float SignX = (i & 1) ? 1.0f : -1.0f;
		float SignY = (i & 2) ? 1.0f : -1.0f;
		float SignZ = (i & 4) ? 1.0f : -1.0f;

		// 코너 위치 (월드 공간)
		FVector Corner = WorldCenter
			+ AxisX * ScaledExtent.X * SignX
			+ AxisY * ScaledExtent.Y * SignY
			+ AxisZ * ScaledExtent.Z * SignZ;

		// 각 축 방향으로 브라켓 라인 그리기 (코너에서 안쪽으로)
		// SDPG_Foreground로 그려서 히트맵 위에 표시
		// X축 브라켓
		FVector EndX = Corner - AxisX * BracketLenX * SignX;
		DrawDebugLine(World, Corner, EndX, BracketColor, false, -1.0f, SDPG_Foreground, LineThickness);

		// Y축 브라켓
		FVector EndY = Corner - AxisY * BracketLenY * SignY;
		DrawDebugLine(World, Corner, EndY, BracketColor, false, -1.0f, SDPG_Foreground, LineThickness);

		// Z축 브라켓
		FVector EndZ = Corner - AxisZ * BracketLenZ * SignZ;
		DrawDebugLine(World, Corner, EndZ, BracketColor, false, -1.0f, SDPG_Foreground, LineThickness);
	}

	// ===== 확장 바운드 그리기 (초록색) - SDFBoundsExpandX/Y 적용 =====
	if (FleshRingAsset && FleshRingAsset->Rings.IsValidIndex(RingIndex))
	{
		const FFleshRingSettings& Ring = FleshRingAsset->Rings[RingIndex];
		const float ExpandX = Ring.SDFBoundsExpandX;
		const float ExpandY = Ring.SDFBoundsExpandY;

		// 확장이 있을 때만 그리기
		if (ExpandX > 0.01f || ExpandY > 0.01f)
		{
			// 확장된 로컬 바운드 계산
			FVector ExpandedLocalMin = LocalBoundsMin - FVector(ExpandX, ExpandY, 0.0f);
			FVector ExpandedLocalMax = LocalBoundsMax + FVector(ExpandX, ExpandY, 0.0f);

			// 확장된 Center와 Extent
			FVector ExpandedLocalCenter = (ExpandedLocalMin + ExpandedLocalMax) * 0.5f;
			FVector ExpandedLocalExtent = (ExpandedLocalMax - ExpandedLocalMin) * 0.5f;

			// 월드 공간 변환
			FVector ExpandedWorldCenter = LocalToWorld.TransformPosition(ExpandedLocalCenter);
			FVector ExpandedScaledExtent = ExpandedLocalExtent * LocalToWorld.GetScale3D();

			// 확장된 브라켓 길이
			float ExpandedBracketLenX = ExpandedScaledExtent.X * 2.0f * BracketRatio;
			float ExpandedBracketLenY = ExpandedScaledExtent.Y * 2.0f * BracketRatio;
			float ExpandedBracketLenZ = ExpandedScaledExtent.Z * 2.0f * BracketRatio;

			// 확장 바운드 8개 코너 그리기 (초록색)
			for (int32 i = 0; i < 8; ++i)
			{
				float SignX = (i & 1) ? 1.0f : -1.0f;
				float SignY = (i & 2) ? 1.0f : -1.0f;
				float SignZ = (i & 4) ? 1.0f : -1.0f;

				FVector ExpandedCorner = ExpandedWorldCenter
					+ AxisX * ExpandedScaledExtent.X * SignX
					+ AxisY * ExpandedScaledExtent.Y * SignY
					+ AxisZ * ExpandedScaledExtent.Z * SignZ;

				// X축 브라켓 (초록색)
				FVector EndX = ExpandedCorner - AxisX * ExpandedBracketLenX * SignX;
				DrawDebugLine(World, ExpandedCorner, EndX, ExpandedBracketColor, false, -1.0f, SDPG_Foreground, LineThickness);

				// Y축 브라켓 (초록색)
				FVector EndY = ExpandedCorner - AxisY * ExpandedBracketLenY * SignY;
				DrawDebugLine(World, ExpandedCorner, EndY, ExpandedBracketColor, false, -1.0f, SDPG_Foreground, LineThickness);

				// Z축 브라켓 (초록색)
				FVector EndZ = ExpandedCorner - AxisZ * ExpandedBracketLenZ * SignZ;
				DrawDebugLine(World, ExpandedCorner, EndZ, ExpandedBracketColor, false, -1.0f, SDPG_Foreground, LineThickness);
			}
		}
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

	// ===== DeformerInstance에서 GPU Influence Readback 결과 가져오기 =====
	// NOTE: 현재는 단일 Ring(RingIndex == 0)만 지원, 멀티 Ring은 향후 확장
	if (RingIndex == 0 && InternalDeformer)
	{
		UFleshRingDeformerInstance* DeformerInstance = InternalDeformer->GetActiveInstance();
		if (DeformerInstance)
		{
			if (DeformerInstance->IsDebugInfluenceReadbackComplete(0))
			{
				const TArray<float>* ReadbackResult = DeformerInstance->GetDebugInfluenceReadbackResult(0);
				if (ReadbackResult && ReadbackResult->Num() > 0)
				{
					// GPU Influence 캐시 배열 초기화 (필요 시)
					if (!CachedGPUInfluences.IsValidIndex(RingIndex))
					{
						CachedGPUInfluences.SetNum(RingIndex + 1);
						bGPUInfluenceReady.SetNum(RingIndex + 1);
					}

					// Readback 결과 복사
					CachedGPUInfluences[RingIndex] = *ReadbackResult;
					bGPUInfluenceReady[RingIndex] = true;

					// Readback 완료 플래그 리셋 (다음 Readback 준비)
					DeformerInstance->ResetDebugInfluenceReadback(0);
				}
			}
			else
			{
				// Readback 미완료 시 기존 캐시 무효화 (CPU fallback으로 전환)
				// 드래그 중 캐시 무효화 시 이전 데이터가 잘못 표시되는 것 방지
				if (bGPUInfluenceReady.IsValidIndex(RingIndex))
				{
					bGPUInfluenceReady[RingIndex] = false;
				}
			}
		}
	}

	// GPU Influence 사용 가능 여부 확인
	bool bUseGPUInfluence = false;
	if (bGPUInfluenceReady.IsValidIndex(RingIndex) && bGPUInfluenceReady[RingIndex] &&
		CachedGPUInfluences.IsValidIndex(RingIndex) && CachedGPUInfluences[RingIndex].Num() > 0)
	{
		bUseGPUInfluence = true;
	}

	// 각 영향받는 버텍스에 대해
	for (int32 i = 0; i < RingData.Vertices.Num(); ++i)
	{
		const FAffectedVertex& AffectedVert = RingData.Vertices[i];
		if (!DebugBindPoseVertices.IsValidIndex(AffectedVert.VertexIndex))
		{
			continue;
		}

		// 바인드 포즈 위치 (컴포넌트 스페이스)
		const FVector3f& BindPosePos = DebugBindPoseVertices[AffectedVert.VertexIndex];

		// 월드 공간으로 변환 (바인드 포즈 기준 - 애니메이션 미반영)
		FVector WorldPos = CompTransform.TransformPosition(FVector(BindPosePos));

		// Influence 값 결정: GPU 값 우선, 없으면 CPU 값 사용
		float Influence;
		if (bUseGPUInfluence && CachedGPUInfluences[RingIndex].IsValidIndex(i))
		{
			// GPU에서 계산된 Influence 사용
			Influence = CachedGPUInfluences[RingIndex][i];
		}
		else
		{
			// CPU에서 계산된 Influence 사용 (fallback)
			Influence = AffectedVert.Influence;
		}

		// Influence에 따른 색상 (0=파랑, 0.5=초록, 1=빨강)
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
		FString SourceStr = bUseGPUInfluence ? TEXT("GPU") : TEXT("CPU");
		GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Green,
			FString::Printf(TEXT("Ring[%d] Affected: %d vertices (Source: %s)"),
				RingIndex, RingData.Vertices.Num(), *SourceStr));
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
		// SDF가 무효화된 Ring의 Actor 정리 (Ring Mesh 삭제 시)
		if (DebugSlicePlaneActors.IsValidIndex(RingIndex) && DebugSlicePlaneActors[RingIndex])
		{
			DebugSlicePlaneActors[RingIndex]->Destroy();
			DebugSlicePlaneActors[RingIndex] = nullptr;
		}
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
	DebugSpatialHash.Clear();
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

	// ===== 1. 바인드 포즈 버텍스 추출 (없을 때만 - Bulge와 동일 패턴) =====
	// 바인드 포즈는 메시가 바뀌지 않는 한 동일하므로 캐시 재사용
	if (DebugBindPoseVertices.Num() == 0)
	{
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

		// Spatial Hash 빌드 (O(1) 쿼리용)
		DebugSpatialHash.Build(DebugBindPoseVertices);
	}

	// ===== 2. 실제 변형 데이터 재사용 시도 =====
	// Deformer가 활성화되어 있으면 이미 계산된 데이터 재사용
	if (UFleshRingDeformerInstance* DeformerInstance =
		Cast<UFleshRingDeformerInstance>(SkelMesh->GetMeshDeformerInstance()))
	{
		const TArray<FRingAffectedData>* ActualData =
			DeformerInstance->GetAffectedRingDataForDebug(0);  // LOD0

		if (ActualData && ActualData->Num() == FleshRingAsset->Rings.Num())
		{
			// 실제 데이터 복사 (중복 계산 제거)
			DebugAffectedData = *ActualData;
			bDebugAffectedVerticesCached = true;
			return;
		}
	}

	// ===== 3. 폴백: Ring별 영향받는 버텍스 직접 계산 =====
	// 배열 크기 확인 (Ring 수가 변경되었을 때만 초기화)
	if (DebugAffectedData.Num() != FleshRingAsset->Rings.Num())
	{
		DebugAffectedData.Reset();
		DebugAffectedData.SetNum(FleshRingAsset->Rings.Num());
	}

	const FReferenceSkeleton& RefSkeleton = Mesh->GetRefSkeleton();
	const TArray<FTransform>& RefBonePose = RefSkeleton.GetRefBonePose();

	for (int32 RingIdx = 0; RingIdx < FleshRingAsset->Rings.Num(); ++RingIdx)
	{
		// ★ 이미 캐싱된 Ring은 스킵 (Ring별 무효화 지원)
		if (DebugAffectedData[RingIdx].Vertices.Num() > 0)
		{
			continue;
		}

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

		// 영향받는 버텍스 선택
		FRingAffectedData& RingData = DebugAffectedData[RingIdx];
		RingData.BoneName = RingSettings.BoneName;
		RingData.RingCenter = BoneTransform.GetLocation();

		// Ring별 InfluenceMode에 따라 분기
		// - Auto: SDF 유효할 때만 SDF 기반
		// - ProceduralBand: 항상 거리 기반 (가변 반경)
		// - Manual: 항상 거리 기반 (고정 반경)
		const bool bUseSDFForThisRing =
			(RingSettings.InfluenceMode == EFleshRingInfluenceMode::Auto) &&
			(SDFCache && SDFCache->IsValid());

		// Falloff 계산 람다 (CalculateFalloff 인라인 버전)
		auto CalcFalloff = [](float Distance, float MaxDistance, EFalloffType Type) -> float
		{
			const float NormalizedDist = FMath::Clamp(Distance / MaxDistance, 0.0f, 1.0f);
			const float T = 1.0f - NormalizedDist;
			switch (Type)
			{
			case EFalloffType::Quadratic:
				return T * T;
			case EFalloffType::Hermite:
				return T * T * (3.0f - 2.0f * T);
			case EFalloffType::Linear:
			default:
				return T;
			}
		};

		if (bUseSDFForThisRing)
		{
			// ===== SDF 모드: OBB 기반 Spatial Hash 쿼리 =====
			// SDF 모드에서는 Influence = 1.0 (최대값) - GPU 셰이더가 SDF로 정제함
			// 디버그 시각화에서는 모든 선택된 버텍스가 빨간색으로 표시됨
			const FTransform& LocalToComponent = SDFCache->LocalToComponent;
			const FVector BoundsMin = FVector(SDFCache->BoundsMin);
			const FVector BoundsMax = FVector(SDFCache->BoundsMax);

			// Spatial Hash로 OBB 내 후보만 추출 - O(1)
			TArray<int32> CandidateIndices;
			if (DebugSpatialHash.IsBuilt())
			{
				DebugSpatialHash.QueryOBB(LocalToComponent, BoundsMin, BoundsMax, CandidateIndices);
			}
			else
			{
				// 폴백: 전체 순회
				CandidateIndices.Reserve(DebugBindPoseVertices.Num());
				for (int32 i = 0; i < DebugBindPoseVertices.Num(); ++i)
				{
					CandidateIndices.Add(i);
				}
			}

			for (int32 VertexIdx : CandidateIndices)
			{
				// SDF 모드: OBB 안에 있으면 Influence = 1.0 (빨간색)
				FAffectedVertex AffectedVert;
				AffectedVert.VertexIndex = static_cast<uint32>(VertexIdx);
				AffectedVert.Influence = 1.0f;
				RingData.Vertices.Add(AffectedVert);
			}
		}
		else if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
		{
			// Virtual Band debug visualization 비활성화 (코드 보존)
			/*
			// ===== Virtual Band 모드 (SDF 무효): 가변 반경 거리 기반 =====
			// 전용 BandOffset/BandRotation 사용
			const FProceduralBandSettings& BandSettings = RingSettings.ProceduralBand;
			const FQuat BoneRotation = BoneTransform.GetRotation();
			const FVector WorldBandOffset = BoneRotation.RotateVector(BandSettings.BandOffset);
			const FVector BandCenter = BoneTransform.GetLocation() + WorldBandOffset;
			const FQuat WorldBandRotation = BoneRotation * BandSettings.BandRotation;
			const FVector BandAxis = WorldBandRotation.RotateVector(FVector::ZAxisVector);

			// 높이 파라미터
			const float LowerHeight = BandSettings.Lower.Height;
			const float BandHeight = BandSettings.BandHeight;
			const float UpperHeight = BandSettings.Upper.Height;
			const float TotalHeight = LowerHeight + BandHeight + UpperHeight;

			// Tightness 영역: Band Section만 (-BandHeight/2 ~ +BandHeight/2)
			// 새 좌표계: Z=0이 Mid Band 중심
			const float TightnessZMin = -BandHeight * 0.5f;
			const float TightnessZMax = BandHeight * 0.5f;

			// Tightness Falloff 범위: 밴드가 조이면서 밀어내는 거리
			// Upper/Lower 반경 차이 = 불룩한 정도 = 조여야 할 거리
			const float UpperBulge = BandSettings.Upper.Radius - BandSettings.MidUpperRadius;
			const float LowerBulge = BandSettings.Lower.Radius - BandSettings.MidLowerRadius;
			const float TightnessFalloffRange = FMath::Max(FMath::Max(UpperBulge, LowerBulge), 1.0f);

			// 최대 반경 계산 (AABB 쿼리용)
			const float MaxRadius = FMath::Max(
				FMath::Max(BandSettings.Lower.Radius, BandSettings.Upper.Radius),
				FMath::Max(BandSettings.MidLowerRadius, BandSettings.MidUpperRadius)
			) + TightnessFalloffRange;

			auto GetRadiusAtHeight = [&BandSettings](float LocalZ) -> float
			{
				return BandSettings.GetRadiusAtHeight(LocalZ);
			};

			// Spatial Hash로 후보 추출
			TArray<int32> CandidateIndices;
			if (DebugSpatialHash.IsBuilt())
			{
				FTransform BandLocalToComponent;
				BandLocalToComponent.SetLocation(BandCenter);
				BandLocalToComponent.SetRotation(WorldBandRotation);
				BandLocalToComponent.SetScale3D(FVector::OneVector);

				// 새 좌표계: Z=0이 Mid Band 중심
				const float MidOffset = LowerHeight + BandHeight * 0.5f;
				const FVector LocalMin(-MaxRadius, -MaxRadius, -MidOffset);
				const FVector LocalMax(MaxRadius, MaxRadius, TotalHeight - MidOffset);
				DebugSpatialHash.QueryOBB(BandLocalToComponent, LocalMin, LocalMax, CandidateIndices);
			}
			else
			{
				CandidateIndices.Reserve(DebugBindPoseVertices.Num());
				for (int32 i = 0; i < DebugBindPoseVertices.Num(); ++i)
				{
					CandidateIndices.Add(i);
				}
			}

			for (int32 VertexIdx : CandidateIndices)
			{
				const FVector VertexPos = FVector(DebugBindPoseVertices[VertexIdx]);
				const FVector ToVertex = VertexPos - BandCenter;
				const float AxisDistance = FVector::DotProduct(ToVertex, BandAxis);
				const float LocalZ = AxisDistance;

				// Band Section 범위 체크 (Tightness 영역)
				if (LocalZ < TightnessZMin || LocalZ > TightnessZMax)
				{
					continue;
				}

				const FVector RadialVec = ToVertex - BandAxis * AxisDistance;
				const float RadialDistance = RadialVec.Size();
				const float BandRadius = GetRadiusAtHeight(LocalZ);

				// 밴드 표면보다 바깥에 있어야 Tightness 영향
				if (RadialDistance <= BandRadius)
				{
					continue;
				}

				const float DistanceOutside = RadialDistance - BandRadius;
				if (DistanceOutside > TightnessFalloffRange)
				{
					continue;
				}

				const float RadialInfluence = CalcFalloff(DistanceOutside, TightnessFalloffRange, RingSettings.FalloffType);

				// Axial Influence (Band 경계에서 거리에 따른 falloff)
				float AxialInfluence = 1.0f;
				const float AxialFalloffRange = BandHeight * 0.2f;
				if (LocalZ < TightnessZMin + AxialFalloffRange)
				{
					const float Dist = TightnessZMin + AxialFalloffRange - LocalZ;
					AxialInfluence = CalcFalloff(Dist, AxialFalloffRange, RingSettings.FalloffType);
				}
				else if (LocalZ > TightnessZMax - AxialFalloffRange)
				{
					const float Dist = LocalZ - (TightnessZMax - AxialFalloffRange);
					AxialInfluence = CalcFalloff(Dist, AxialFalloffRange, RingSettings.FalloffType);
				}

				const float CombinedInfluence = RadialInfluence * AxialInfluence;

				if (CombinedInfluence > KINDA_SMALL_NUMBER)
				{
					FAffectedVertex AffectedVert;
					AffectedVert.VertexIndex = static_cast<uint32>(VertexIdx);
					AffectedVert.Influence = CombinedInfluence;
					RingData.Vertices.Add(AffectedVert);
				}
			}
			*/
		}
		else
		{
			// ===== Manual 모드: 원통형 거리 기반 Spatial Hash 쿼리 =====
			const FQuat BoneRotation = BoneTransform.GetRotation();
			const FVector WorldRingOffset = BoneRotation.RotateVector(RingSettings.RingOffset);
			const FVector RingCenter = BoneTransform.GetLocation() + WorldRingOffset;
			const FQuat WorldRingRotation = BoneRotation * RingSettings.RingRotation;
			const FVector RingAxis = WorldRingRotation.RotateVector(FVector::ZAxisVector);

			const float MaxDistance = RingSettings.RingRadius + RingSettings.RingThickness;
			const float HalfWidth = RingSettings.RingHeight / 2.0f;

			// Spatial Hash로 원통을 포함하는 OBB 내 후보만 추출 - O(1)
			TArray<int32> CandidateIndices;
			if (DebugSpatialHash.IsBuilt())
			{
				// Ring 회전을 반영한 OBB 쿼리
				FTransform RingLocalToComponent;
				RingLocalToComponent.SetLocation(RingCenter);
				RingLocalToComponent.SetRotation(WorldRingRotation);
				RingLocalToComponent.SetScale3D(FVector::OneVector);

				const FVector LocalMin(-MaxDistance, -MaxDistance, -HalfWidth);
				const FVector LocalMax(MaxDistance, MaxDistance, HalfWidth);
				DebugSpatialHash.QueryOBB(RingLocalToComponent, LocalMin, LocalMax, CandidateIndices);
			}
			else
			{
				// 폴백: 전체 순회
				CandidateIndices.Reserve(DebugBindPoseVertices.Num());
				for (int32 i = 0; i < DebugBindPoseVertices.Num(); ++i)
				{
					CandidateIndices.Add(i);
				}
			}

			for (int32 VertexIdx : CandidateIndices)
			{
				const FVector VertexPos = FVector(DebugBindPoseVertices[VertexIdx]);
				const FVector ToVertex = VertexPos - RingCenter;
				const float AxisDistance = FVector::DotProduct(ToVertex, RingAxis);
				const FVector RadialVec = ToVertex - RingAxis * AxisDistance;
				const float RadialDistance = RadialVec.Size();

				if (RadialDistance <= MaxDistance && FMath::Abs(AxisDistance) <= HalfWidth)
				{
					const float DistFromRingSurface = FMath::Abs(RadialDistance - RingSettings.RingRadius);
					const float RadialInfluence = CalcFalloff(DistFromRingSurface, RingSettings.RingThickness, RingSettings.FalloffType);
					const float AxialInfluence = CalcFalloff(FMath::Abs(AxisDistance), HalfWidth, RingSettings.FalloffType);
					const float CombinedInfluence = RadialInfluence * AxialInfluence;

					if (CombinedInfluence > KINDA_SMALL_NUMBER)
					{
						FAffectedVertex AffectedVert;
						AffectedVert.VertexIndex = static_cast<uint32>(VertexIdx);
						AffectedVert.Influence = CombinedInfluence;
						RingData.Vertices.Add(AffectedVert);
					}
				}
			}
		}

		UE_LOG(LogFleshRingComponent, Verbose, TEXT("CacheAffectedVerticesForDebug: Ring[%d] '%s' - %d affected vertices, Mode=%s"),
			RingIdx, *RingSettings.BoneName.ToString(), RingData.Vertices.Num(),
			bUseSDFForThisRing ? TEXT("SDF") : TEXT("Manual"));
	}

	bDebugAffectedVerticesCached = true;

	UE_LOG(LogFleshRingComponent, Verbose, TEXT("CacheAffectedVerticesForDebug: Cached %d rings, %d total vertices"),
		DebugAffectedData.Num(), DebugBindPoseVertices.Num());
}

void UFleshRingComponent::DrawProceduralBandWireframe(int32 RingIndex)
{
	UWorld* World = GetWorld();
	if (!World || !FleshRingAsset)
	{
		return;
	}

	// Ring 유효성 검사
	if (!FleshRingAsset->Rings.IsValidIndex(RingIndex))
	{
		return;
	}

	const FFleshRingSettings& Ring = FleshRingAsset->Rings[RingIndex];

	// ProceduralBand 모드가 아니면 스킵
	if (Ring.InfluenceMode != EFleshRingInfluenceMode::ProceduralBand)
	{
		return;
	}

	// 와이어프레임 라인 생성
	TArray<TPair<FVector, FVector>> WireframeLines;
	FleshRingProceduralMesh::GenerateWireframeLines(Ring.ProceduralBand, WireframeLines, 32);

	if (WireframeLines.Num() == 0)
	{
		return;
	}

	// 트랜스폼 계산: Local → Component → World
	// Virtual Band 전용 BandOffset/BandRotation 사용
	const FProceduralBandSettings& BandSettings = Ring.ProceduralBand;
	FTransform BandTransform;
	BandTransform.SetLocation(BandSettings.BandOffset);
	BandTransform.SetRotation(BandSettings.BandRotation);
	BandTransform.SetScale3D(FVector::OneVector);

	FTransform BoneTransform = GetBoneBindPoseTransform(ResolvedTargetMesh.Get(), Ring.BoneName);
	FTransform LocalToComponent = BandTransform * BoneTransform;

	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();
	FTransform LocalToWorld = LocalToComponent;
	if (SkelMesh)
	{
		LocalToWorld = LocalToComponent * SkelMesh->GetComponentTransform();
	}

	// 와이어프레임 색상 (마젠타 - 프로시저럴 밴드 전용)
	FColor WireColor = FColor::Magenta;
	float LineThickness = 0.0f;  // 가장 얇은 선

	// 각 라인 그리기
	for (const TPair<FVector, FVector>& Line : WireframeLines)
	{
		FVector WorldStart = LocalToWorld.TransformPosition(Line.Key);
		FVector WorldEnd = LocalToWorld.TransformPosition(Line.Value);

		DrawDebugLine(World, WorldStart, WorldEnd, WireColor, false, -1.0f, SDPG_Foreground, LineThickness);
	}

	// 화면에 정보 표시
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Magenta,
			FString::Printf(TEXT("Ring[%d] VirtualBand: MidU=%.1f MidL=%.1f H=%.1f"),
				RingIndex, Ring.ProceduralBand.MidUpperRadius, Ring.ProceduralBand.MidLowerRadius, Ring.ProceduralBand.BandHeight));
	}
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

	// DebugBulgeData 배열 크기 확인 (Ring 수가 변경되었을 때만 초기화)
	if (DebugBulgeData.Num() != FleshRingAsset->Rings.Num())
	{
		DebugBulgeData.Reset();
		DebugBulgeData.SetNum(FleshRingAsset->Rings.Num());
	}

	for (int32 RingIdx = 0; RingIdx < FleshRingAsset->Rings.Num(); ++RingIdx)
	{
		const FFleshRingSettings& RingSettings = FleshRingAsset->Rings[RingIdx];
		FRingAffectedData& BulgeData = DebugBulgeData[RingIdx];

		// ★ 이미 캐싱된 Ring은 스킵 (Ring별 무효화 지원)
		if (BulgeData.Vertices.Num() > 0)
		{
			continue;
		}

		BulgeData.BoneName = RingSettings.BoneName;

		// Bulge 비활성화면 스킵
		if (!RingSettings.bEnableBulge)
		{
			continue;
		}

		// ===== Ring 정보 계산: SDF 모드 vs Manual 모드 분기 =====
		FTransform LocalToComponent = FTransform::Identity;
		FVector3f RingCenter;
		FVector3f RingAxis;
		float RingHeight;
		float RingRadius;
		int32 DetectedDirection = 0;
		bool bUseLocalSpace = false;  // Manual 모드는 Component Space 직접 사용
		FQuat ManualRingRotation = FQuat::Identity;  // Manual 모드 OBB 쿼리용

		// ★ InfluenceMode 기반 분기: Auto 모드일 때만 SDFCache 접근
		const FRingSDFCache* SDFCache = nullptr;
		bool bHasValidSDF = false;
		if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::Auto)
		{
			SDFCache = GetRingSDFCache(RingIdx);
			bHasValidSDF = (SDFCache && SDFCache->IsValid());
		}

		if (bHasValidSDF)
		{
			// ===== Auto 모드: SDF 캐시에서 Ring 정보 가져오기 =====
			bUseLocalSpace = true;
			LocalToComponent = SDFCache->LocalToComponent;
			FVector3f BoundsMin = SDFCache->BoundsMin;
			FVector3f BoundsMax = SDFCache->BoundsMax;
			FVector3f BoundsSize = BoundsMax - BoundsMin;
			RingCenter = (BoundsMin + BoundsMax) * 0.5f;

			// Ring 축 감지 (가장 짧은 축)
			if (BoundsSize.X <= BoundsSize.Y && BoundsSize.X <= BoundsSize.Z)
				RingAxis = FVector3f(1, 0, 0);
			else if (BoundsSize.Y <= BoundsSize.X && BoundsSize.Y <= BoundsSize.Z)
				RingAxis = FVector3f(0, 1, 0);
			else
				RingAxis = FVector3f(0, 0, 1);

			// Ring 크기 계산 (FleshRingBulgeProviders.cpp와 동일)
			RingHeight = FMath::Min3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z);
			RingRadius = FMath::Max3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z) * 0.5f;
			DetectedDirection = SDFCache->DetectedBulgeDirection;
		}
		else if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::Manual)
		{
			// ===== Manual 모드: Ring 파라미터에서 직접 가져오기 (Component Space) =====
			bUseLocalSpace = false;

			// Bone Transform 가져오기
			FTransform BoneTransform = FTransform::Identity;
			if (SkelMesh)
			{
				int32 BoneIndex = SkelMesh->GetBoneIndex(RingSettings.BoneName);
				if (BoneIndex != INDEX_NONE)
				{
					BoneTransform = SkelMesh->GetBoneTransform(BoneIndex, FTransform::Identity);
				}
			}

			// RingCenter = Bone Position + RingOffset (Bone 회전 적용)
			const FQuat BoneRotation = BoneTransform.GetRotation();
			const FVector WorldRingOffset = BoneRotation.RotateVector(RingSettings.RingOffset);
			RingCenter = FVector3f(BoneTransform.GetLocation() + WorldRingOffset);

			// RingAxis = Bone Rotation * RingRotation의 Z축
			const FQuat WorldRingRotation = BoneRotation * RingSettings.RingRotation;
			RingAxis = FVector3f(WorldRingRotation.RotateVector(FVector::ZAxisVector));
			ManualRingRotation = WorldRingRotation;  // OBB 쿼리용 저장

			// Ring 크기는 직접 사용
			RingHeight = RingSettings.RingHeight;
			RingRadius = RingSettings.RingRadius;
			DetectedDirection = 0;  // Manual 모드는 자동 감지 불가, 양방향
		}
		else
		{
			// SDF 없고 Manual도 아니면 스킵
			continue;
		}

		// Bulge 시작 거리 (Ring 경계)
		const float BulgeStartDist = RingHeight * 0.5f;

		// 직교 범위 제한 (각 축 독립적으로 제어)
		// AxialLimit = 시작점 + 확장량 (AxialRange=1이면 RingHeight*0.5 만큼 확장)
		const float AxialLimit = BulgeStartDist + RingHeight * 0.5f * RingSettings.BulgeAxialRange;
		const float RadialLimit = RingRadius * RingSettings.BulgeRadialRange;

		// 방향 결정 (0 = 양방향) - DetectedDirection은 위에서 이미 계산됨
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

		// Spatial Hash로 후보 버텍스만 추출 - O(1)
		TArray<int32> CandidateIndices;
		if (DebugSpatialHash.IsBuilt())
		{
			if (bUseLocalSpace)
			{
				// SDF 모드: OBB 쿼리 (Bulge 영역 확장 고려)
				const FVector3f& BoundsMin = SDFCache->BoundsMin;
				const FVector3f& BoundsMax = SDFCache->BoundsMax;
				// Bulge 영역 확장: Axial(Z) + Radial(X/Y) 모두 고려
				const float AxialExtend = AxialLimit - BulgeStartDist;
				const float RadialExtend = FMath::Max(0.0f, RadialLimit - RingRadius);
				FVector ExpandedMin = FVector(BoundsMin) - FVector(RadialExtend, RadialExtend, AxialExtend);
				FVector ExpandedMax = FVector(BoundsMax) + FVector(RadialExtend, RadialExtend, AxialExtend);
				DebugSpatialHash.QueryOBB(LocalToComponent, ExpandedMin, ExpandedMax, CandidateIndices);
			}
			else
			{
				// Manual 모드: OBB 쿼리 (Ring 회전 반영, Bulge 영역 포함)
				FTransform RingLocalToComponent;
				RingLocalToComponent.SetLocation(FVector(RingCenter));
				RingLocalToComponent.SetRotation(ManualRingRotation);
				RingLocalToComponent.SetScale3D(FVector::OneVector);

				const float MaxExtent = FMath::Max(RadialLimit * 1.5f, AxialLimit);
				const FVector LocalMin(-MaxExtent, -MaxExtent, -AxialLimit);
				const FVector LocalMax(MaxExtent, MaxExtent, AxialLimit);
				DebugSpatialHash.QueryOBB(RingLocalToComponent, LocalMin, LocalMax, CandidateIndices);
			}
		}
		else
		{
			// 폴백: 전체 순회
			CandidateIndices.Reserve(DebugBindPoseVertices.Num());
			for (int32 i = 0; i < DebugBindPoseVertices.Num(); ++i)
			{
				CandidateIndices.Add(i);
			}
		}

		// 후보 버텍스만 순회
		for (int32 VertIdx : CandidateIndices)
		{
			FVector CompSpacePos = FVector(DebugBindPoseVertices[VertIdx]);
			FVector3f VertexPos;

			if (bUseLocalSpace)
			{
				// SDF 모드: Component Space → Ring Local Space 변환
				// InverseTransformPosition: (V - Trans) * Rot^-1 / Scale (올바른 순서)
				FVector LocalSpacePos = LocalToComponent.InverseTransformPosition(CompSpacePos);
				VertexPos = FVector3f(LocalSpacePos);
			}
			else
			{
				// Manual 모드: Component Space 직접 사용 (RingCenter, RingAxis가 이미 Component Space)
				VertexPos = FVector3f(CompSpacePos);
			}

			// Ring 중심으로부터의 벡터
			FVector3f ToVertex = VertexPos - RingCenter;

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

			// 4. 축 방향 거리 기반 Falloff 감쇠
			// Ring 경계에서 1.0, AxialLimit에서 0으로 부드럽게 감쇠
			const float AxialFalloffRange = AxialLimit - BulgeStartDist;
			float NormalizedDist = (AxialDist - BulgeStartDist) / FMath::Max(AxialFalloffRange, 0.001f);
			float ClampedDist = FMath::Clamp(NormalizedDist, 0.0f, 1.0f);

			// 실제 계산과 시각화가 동일 함수 사용
			float BulgeInfluence = FFleshRingFalloff::Evaluate(ClampedDist, RingSettings.BulgeFalloff);

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

	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();

	// ★ InfluenceMode 기반 분기: Auto 모드일 때만 SDFCache 접근
	const FRingSDFCache* SDFCache = nullptr;
	bool bHasValidSDF = false;
	if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::Auto)
	{
		SDFCache = GetRingSDFCache(RingIndex);
		bHasValidSDF = (SDFCache && SDFCache->IsValid());
	}

	// ===== Ring 정보 계산: SDF 모드 vs Manual 모드 분기 =====
	FVector WorldCenter;
	FVector WorldZAxis;
	float ArrowLength;
	int32 DetectedDirection = 0;

	if (bHasValidSDF)
	{
		// ===== Auto 모드: SDF 캐시에서 정보 가져오기 =====
		DetectedDirection = SDFCache->DetectedBulgeDirection;

		// OBB 중심 위치 (로컬 공간)
		FVector LocalCenter = FVector(SDFCache->BoundsMin + SDFCache->BoundsMax) * 0.5f;

		// Component → World 트랜스폼
		FTransform LocalToWorld = SDFCache->LocalToComponent;
		if (SkelMesh)
		{
			LocalToWorld = LocalToWorld * SkelMesh->GetComponentTransform();
		}

		WorldCenter = LocalToWorld.TransformPosition(LocalCenter);
		FQuat WorldRotation = LocalToWorld.GetRotation();

		// 로컬 Z축을 월드 공간으로 변환
		FVector LocalZAxis = FVector(0.0f, 0.0f, 1.0f);
		WorldZAxis = WorldRotation.RotateVector(LocalZAxis);

		// 화살표 크기 (SDF 볼륨 크기에 비례)
		ArrowLength = FVector(SDFCache->BoundsMax - SDFCache->BoundsMin).Size() * 0.05f;
	}
	else if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::Manual)
	{
		// ===== Manual 모드: Ring 파라미터에서 직접 가져오기 =====
		DetectedDirection = 0;  // Manual 모드는 자동 감지 불가

		// Bone Transform 가져오기
		FTransform BoneTransform = FTransform::Identity;
		if (SkelMesh)
		{
			int32 BoneIndex = SkelMesh->GetBoneIndex(RingSettings.BoneName);
			if (BoneIndex != INDEX_NONE)
			{
				BoneTransform = SkelMesh->GetBoneTransform(BoneIndex);
			}
		}

		// RingCenter (World Space)
		const FQuat BoneRotation = BoneTransform.GetRotation();
		const FVector WorldRingOffset = BoneRotation.RotateVector(RingSettings.RingOffset);
		WorldCenter = BoneTransform.GetLocation() + WorldRingOffset;

		// RingAxis (World Space)
		const FQuat WorldRingRotation = BoneRotation * RingSettings.RingRotation;
		WorldZAxis = WorldRingRotation.RotateVector(FVector::ZAxisVector);

		// 화살표 크기 (Ring 반경에 비례)
		ArrowLength = RingSettings.RingRadius * 0.1f;
	}
	else
	{
		// SDF 없고 Manual도 아니면 스킵
		return;
	}

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

void UFleshRingComponent::DrawBulgeRange(int32 RingIndex)
{
	UWorld* World = GetWorld();
	if (!World)
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

	USkeletalMeshComponent* SkelMesh = ResolvedTargetMesh.Get();

	// 색상 (주황색)
	const FColor CylinderColor(255, 180, 50, 200);
	const float LineThickness = 0.15f;
	const int32 CircleSegments = 32;

	// ★ Falloff 타입별 보정 계수 (Evaluate(q) = KINDA_SMALL_NUMBER 기준)
	// 실제 Bulge 선택: BulgeInfluence > 0.0001 이면 포함
	auto GetFalloffCorrection = [](EFleshRingFalloffType FalloffType) -> float
	{
		switch (FalloffType)
		{
		case EFleshRingFalloffType::Linear:
			return 1.0f;    // 1-q = 0.0001 → q ≈ 1.0
		case EFleshRingFalloffType::Quadratic:
			return 0.99f;   // (1-q)² = 0.0001 → q = 0.99
		case EFleshRingFalloffType::Hermite:
			return 0.99f;   // t²(3-2t) = 0.0001 → q ≈ 0.99
		case EFleshRingFalloffType::WendlandC2:
			return 0.93f;   // (1-q)⁴(4q+1) = 0.0001 → q ≈ 0.93
		case EFleshRingFalloffType::Smootherstep:
			return 0.98f;   // t³(t(6t-15)+10) = 0.0001 → q ≈ 0.98
		default:
			return 1.0f;
		}
	};
	const float FalloffCorrection = GetFalloffCorrection(RingSettings.BulgeFalloff);

	// ===== ProceduralBand 모드: 가변 반경 형상 =====
	if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
	{
		const FProceduralBandSettings& Band = RingSettings.ProceduralBand;

		// Bone Transform 가져오기
		FTransform BoneTransform = FTransform::Identity;
		if (SkelMesh)
		{
			int32 BoneIndex = SkelMesh->GetBoneIndex(RingSettings.BoneName);
			if (BoneIndex != INDEX_NONE)
			{
				BoneTransform = SkelMesh->GetBoneTransform(BoneIndex);
			}
		}

		// Band Center/Axis (World Space)
		const FQuat BoneRotation = BoneTransform.GetRotation();
		const FVector WorldBandOffset = BoneRotation.RotateVector(Band.BandOffset);
		const FVector WorldCenter = BoneTransform.GetLocation() + WorldBandOffset;
		const FQuat WorldBandRotation = BoneRotation * Band.BandRotation;
		const FVector WorldZAxis = WorldBandRotation.RotateVector(FVector::ZAxisVector);

		// 축에 수직인 두 벡터 계산
		FVector Tangent, Binormal;
		WorldZAxis.FindBestAxisVectors(Tangent, Binormal);

		const float BandHalfHeight = Band.BandHeight * 0.5f;
		const float RadialRange = RingSettings.BulgeRadialRange;
		// Falloff 타입별 보정 적용
		const float AxialRange = RingSettings.BulgeAxialRange * FalloffCorrection;

		// 상단 Bulge 영역 (UpperBulgeStrength > 0)
		// Band 상단(+BandHalfHeight)에서 Upper Section 끝(+BandHalfHeight + Upper.Height)까지
		// + AxialRange 확장
		if (RingSettings.UpperBulgeStrength > 0.01f && Band.Upper.Height > 0.01f)
		{
			const float UpperStart = BandHalfHeight;
			const float UpperEnd = BandHalfHeight + Band.Upper.Height * AxialRange;
			const int32 NumSlices = 4;

			// 여러 높이에서 원 그리기 (가변 반경 표현)
			TArray<FVector> SlicePositions;
			TArray<float> SliceRadii;

			for (int32 i = 0; i <= NumSlices; ++i)
			{
				float T = static_cast<float>(i) / static_cast<float>(NumSlices);
				float LocalZ = FMath::Lerp(UpperStart, UpperEnd, T);
				float BaseRadius = Band.GetRadiusAtHeight(LocalZ);
				float BulgeRadius = BaseRadius * RadialRange;

				FVector SlicePos = WorldCenter + WorldZAxis * LocalZ;
				SlicePositions.Add(SlicePos);
				SliceRadii.Add(BulgeRadius);

				// 원 그리기
				DrawDebugCircle(World, SlicePos, BulgeRadius, CircleSegments, CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness, Tangent, Binormal, false);
			}

			// 세로 라인 4개 (슬라이스 연결)
			for (int32 LineIdx = 0; LineIdx < 4; ++LineIdx)
			{
				float Angle = static_cast<float>(LineIdx) / 4.0f * 2.0f * PI;
				FVector Dir = Tangent * FMath::Cos(Angle) + Binormal * FMath::Sin(Angle);

				for (int32 i = 0; i < SlicePositions.Num() - 1; ++i)
				{
					FVector Start = SlicePositions[i] + Dir * SliceRadii[i];
					FVector End = SlicePositions[i + 1] + Dir * SliceRadii[i + 1];
					DrawDebugLine(World, Start, End, CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness);
				}
			}
		}

		// 하단 Bulge 영역 (LowerBulgeStrength > 0)
		// Band 하단(-BandHalfHeight)에서 Lower Section 끝(-BandHalfHeight - Lower.Height)까지
		// + AxialRange 확장
		if (RingSettings.LowerBulgeStrength > 0.01f && Band.Lower.Height > 0.01f)
		{
			const float LowerStart = -BandHalfHeight;
			const float LowerEnd = -BandHalfHeight - Band.Lower.Height * AxialRange;
			const int32 NumSlices = 4;

			// 여러 높이에서 원 그리기 (가변 반경 표현)
			TArray<FVector> SlicePositions;
			TArray<float> SliceRadii;

			for (int32 i = 0; i <= NumSlices; ++i)
			{
				float T = static_cast<float>(i) / static_cast<float>(NumSlices);
				float LocalZ = FMath::Lerp(LowerStart, LowerEnd, T);
				float BaseRadius = Band.GetRadiusAtHeight(LocalZ);
				float BulgeRadius = BaseRadius * RadialRange;

				FVector SlicePos = WorldCenter + WorldZAxis * LocalZ;
				SlicePositions.Add(SlicePos);
				SliceRadii.Add(BulgeRadius);

				// 원 그리기
				DrawDebugCircle(World, SlicePos, BulgeRadius, CircleSegments, CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness, Tangent, Binormal, false);
			}

			// 세로 라인 4개 (슬라이스 연결)
			for (int32 LineIdx = 0; LineIdx < 4; ++LineIdx)
			{
				float Angle = static_cast<float>(LineIdx) / 4.0f * 2.0f * PI;
				FVector Dir = Tangent * FMath::Cos(Angle) + Binormal * FMath::Sin(Angle);

				for (int32 i = 0; i < SlicePositions.Num() - 1; ++i)
				{
					FVector Start = SlicePositions[i] + Dir * SliceRadii[i];
					FVector End = SlicePositions[i + 1] + Dir * SliceRadii[i + 1];
					DrawDebugLine(World, Start, End, CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness);
				}
			}
		}

		return;
	}

	// ===== Auto/Manual 모드: 원뿔 형태 =====
	const FRingSDFCache* SDFCache = GetRingSDFCache(RingIndex);
	const bool bHasValidSDF = SDFCache && SDFCache->IsValid();

	// ★ SDF 모드: 로컬 스페이스에서 모든 점을 계산한 뒤 월드로 변환
	// LocalToComponent에 스케일이 포함될 수 있으므로, 개별 점을 변환해야 함
	if (bHasValidSDF)
	{
		// SDF 바운드에서 기하 정보 계산 (로컬 스페이스)
		const FVector3f BoundsSize = SDFCache->BoundsMax - SDFCache->BoundsMin;
		const FVector LocalCenter = FVector(SDFCache->BoundsMin + SDFCache->BoundsMax) * 0.5f;
		const float RingHeight = FMath::Min3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z);
		const float RingRadius = FMath::Max3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z) * 0.5f;

		// Ring 축 = 가장 짧은 SDF 바운드 축 (실제 Bulge 계산과 동일)
		FVector LocalRingAxis;
		if (BoundsSize.X <= BoundsSize.Y && BoundsSize.X <= BoundsSize.Z)
		{
			LocalRingAxis = FVector(1.0f, 0.0f, 0.0f);
		}
		else if (BoundsSize.Y <= BoundsSize.X && BoundsSize.Y <= BoundsSize.Z)
		{
			LocalRingAxis = FVector(0.0f, 1.0f, 0.0f);
		}
		else
		{
			LocalRingAxis = FVector(0.0f, 0.0f, 1.0f);
		}

		// 축에 수직인 두 벡터 (로컬 스페이스)
		FVector LocalTangent, LocalBinormal;
		LocalRingAxis.FindBestAxisVectors(LocalTangent, LocalBinormal);

		// 로컬 → 월드 트랜스폼
		FTransform LocalToWorld = SDFCache->LocalToComponent;
		if (SkelMesh)
		{
			LocalToWorld = LocalToWorld * SkelMesh->GetComponentTransform();
		}

		// Bulge 범위 (로컬 스페이스) - Falloff 타입별 보정 적용
		const float BulgeRadialExtent = RingRadius * RingSettings.BulgeRadialRange;
		const float AxialExtent = RingHeight * 0.5f * RingSettings.BulgeAxialRange * FalloffCorrection;
		const float RingHalfHeight = RingHeight * 0.5f;

		const int32 NumSlices = 4;

		// 람다: 로컬 스페이스 점을 월드로 변환
		auto TransformToWorld = [&LocalToWorld](const FVector& LocalPos) -> FVector
		{
			return LocalToWorld.TransformPosition(LocalPos);
		};

		// 상단 원뿔 (UpperBulgeStrength > 0)
		if (RingSettings.UpperBulgeStrength > 0.01f)
		{
			// 각 슬라이스의 원 점들을 저장 (월드 스페이스)
			TArray<TArray<FVector>> SliceCirclePoints;
			SliceCirclePoints.SetNum(NumSlices + 1);

			for (int32 i = 0; i <= NumSlices; ++i)
			{
				float T = static_cast<float>(i) / static_cast<float>(NumSlices);
				float LocalZ = RingHalfHeight + AxialExtent * T;
				float DynamicRadius = BulgeRadialExtent * (1.0f + T * 0.5f);

				// 로컬 스페이스에서 원의 중심
				FVector LocalSliceCenter = LocalCenter + LocalRingAxis * LocalZ;

				// 원의 점들을 로컬에서 계산 후 월드로 변환
				TArray<FVector>& CirclePoints = SliceCirclePoints[i];
				CirclePoints.SetNum(CircleSegments + 1);

				for (int32 j = 0; j <= CircleSegments; ++j)
				{
					float Angle = static_cast<float>(j) / static_cast<float>(CircleSegments) * 2.0f * PI;
					FVector LocalPoint = LocalSliceCenter + LocalTangent * (FMath::Cos(Angle) * DynamicRadius) + LocalBinormal * (FMath::Sin(Angle) * DynamicRadius);
					CirclePoints[j] = TransformToWorld(LocalPoint);
				}

				// 원 그리기
				for (int32 j = 0; j < CircleSegments; ++j)
				{
					DrawDebugLine(World, CirclePoints[j], CirclePoints[j + 1], CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness);
				}
			}

			// 세로 라인 4개 (슬라이스 연결)
			for (int32 LineIdx = 0; LineIdx < 4; ++LineIdx)
			{
				int32 PointIdx = (CircleSegments * LineIdx) / 4;
				for (int32 i = 0; i < NumSlices; ++i)
				{
					DrawDebugLine(World, SliceCirclePoints[i][PointIdx], SliceCirclePoints[i + 1][PointIdx], CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness);
				}
			}
		}

		// 하단 원뿔 (LowerBulgeStrength > 0)
		if (RingSettings.LowerBulgeStrength > 0.01f)
		{
			TArray<TArray<FVector>> SliceCirclePoints;
			SliceCirclePoints.SetNum(NumSlices + 1);

			for (int32 i = 0; i <= NumSlices; ++i)
			{
				float T = static_cast<float>(i) / static_cast<float>(NumSlices);
				float LocalZ = -RingHalfHeight - AxialExtent * T;
				float DynamicRadius = BulgeRadialExtent * (1.0f + T * 0.5f);

				FVector LocalSliceCenter = LocalCenter + LocalRingAxis * LocalZ;

				TArray<FVector>& CirclePoints = SliceCirclePoints[i];
				CirclePoints.SetNum(CircleSegments + 1);

				for (int32 j = 0; j <= CircleSegments; ++j)
				{
					float Angle = static_cast<float>(j) / static_cast<float>(CircleSegments) * 2.0f * PI;
					FVector LocalPoint = LocalSliceCenter + LocalTangent * (FMath::Cos(Angle) * DynamicRadius) + LocalBinormal * (FMath::Sin(Angle) * DynamicRadius);
					CirclePoints[j] = TransformToWorld(LocalPoint);
				}

				for (int32 j = 0; j < CircleSegments; ++j)
				{
					DrawDebugLine(World, CirclePoints[j], CirclePoints[j + 1], CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness);
				}
			}

			for (int32 LineIdx = 0; LineIdx < 4; ++LineIdx)
			{
				int32 PointIdx = (CircleSegments * LineIdx) / 4;
				for (int32 i = 0; i < NumSlices; ++i)
				{
					DrawDebugLine(World, SliceCirclePoints[i][PointIdx], SliceCirclePoints[i + 1][PointIdx], CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness);
				}
			}
		}

		return;
	}

	// ===== Manual 모드: 기존 방식 =====
	if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::Manual)
	{
		FTransform BoneTransform = FTransform::Identity;
		if (SkelMesh)
		{
			int32 BoneIndex = SkelMesh->GetBoneIndex(RingSettings.BoneName);
			if (BoneIndex != INDEX_NONE)
			{
				BoneTransform = SkelMesh->GetBoneTransform(BoneIndex);
			}
		}

		const FQuat BoneRotation = BoneTransform.GetRotation();
		const FVector WorldRingOffset = BoneRotation.RotateVector(RingSettings.RingOffset);
		const FVector WorldCenter = BoneTransform.GetLocation() + WorldRingOffset;

		const FQuat WorldRingRotation = BoneRotation * RingSettings.RingRotation;
		const FVector WorldZAxis = WorldRingRotation.RotateVector(FVector::ZAxisVector);

		// Bulge 범위 계산 - Falloff 타입별 보정 적용
		const float RingRadius = RingSettings.RingRadius;
		const float RingHeight = RingSettings.RingHeight;
		const float BulgeRadialExtent = RingRadius * RingSettings.BulgeRadialRange;
		const float AxialExtent = RingHeight * 0.5f * RingSettings.BulgeAxialRange * FalloffCorrection;
		const float RingHalfHeight = RingHeight * 0.5f;

		// 축에 수직인 두 벡터
		FVector Tangent, Binormal;
		WorldZAxis.FindBestAxisVectors(Tangent, Binormal);

		const int32 NumSlices = 4;

		// 상단 원뿔 (UpperBulgeStrength > 0)
		if (RingSettings.UpperBulgeStrength > 0.01f)
		{
			TArray<FVector> SlicePositions;
			TArray<float> SliceRadii;

			for (int32 i = 0; i <= NumSlices; ++i)
			{
				float T = static_cast<float>(i) / static_cast<float>(NumSlices);
				float LocalZ = RingHalfHeight + AxialExtent * T;
				float DynamicRadius = BulgeRadialExtent * (1.0f + T * 0.5f);

				FVector SlicePos = WorldCenter + WorldZAxis * LocalZ;
				SlicePositions.Add(SlicePos);
				SliceRadii.Add(DynamicRadius);

				DrawDebugCircle(World, SlicePos, DynamicRadius, CircleSegments, CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness, Tangent, Binormal, false);
			}

			for (int32 LineIdx = 0; LineIdx < 4; ++LineIdx)
			{
				float Angle = static_cast<float>(LineIdx) / 4.0f * 2.0f * PI;
				FVector Dir = Tangent * FMath::Cos(Angle) + Binormal * FMath::Sin(Angle);

				for (int32 i = 0; i < SlicePositions.Num() - 1; ++i)
				{
					FVector Start = SlicePositions[i] + Dir * SliceRadii[i];
					FVector End = SlicePositions[i + 1] + Dir * SliceRadii[i + 1];
					DrawDebugLine(World, Start, End, CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness);
				}
			}
		}

		// 하단 원뿔 (LowerBulgeStrength > 0)
		if (RingSettings.LowerBulgeStrength > 0.01f)
		{
			TArray<FVector> SlicePositions;
			TArray<float> SliceRadii;

			for (int32 i = 0; i <= NumSlices; ++i)
			{
				float T = static_cast<float>(i) / static_cast<float>(NumSlices);
				float LocalZ = -RingHalfHeight - AxialExtent * T;
				float DynamicRadius = BulgeRadialExtent * (1.0f + T * 0.5f);

				FVector SlicePos = WorldCenter + WorldZAxis * LocalZ;
				SlicePositions.Add(SlicePos);
				SliceRadii.Add(DynamicRadius);

				DrawDebugCircle(World, SlicePos, DynamicRadius, CircleSegments, CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness, Tangent, Binormal, false);
			}

			for (int32 LineIdx = 0; LineIdx < 4; ++LineIdx)
			{
				float Angle = static_cast<float>(LineIdx) / 4.0f * 2.0f * PI;
				FVector Dir = Tangent * FMath::Cos(Angle) + Binormal * FMath::Sin(Angle);

				for (int32 i = 0; i < SlicePositions.Num() - 1; ++i)
				{
					FVector Start = SlicePositions[i] + Dir * SliceRadii[i];
					FVector End = SlicePositions[i + 1] + Dir * SliceRadii[i + 1];
					DrawDebugLine(World, Start, End, CylinderColor, false, -1.0f, SDPG_Foreground, LineThickness);
				}
			}
		}
	}
}

// ============================================================================
// GPU 디버그 렌더링 함수
// ============================================================================

void UFleshRingComponent::InitializeDebugViewExtension()
{
	// 이미 초기화되어 있으면 스킵
	if (DebugViewExtension.IsValid())
	{
		return;
	}

	// SceneViewExtension 생성 및 등록
	// FSceneViewExtensions::NewExtension을 통해 자동 등록됨
	// FWorldSceneViewExtension을 상속하므로 특정 World에서만 활성화됨
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	DebugViewExtension = FSceneViewExtensions::NewExtension<FFleshRingDebugViewExtension>(World);

	UE_LOG(LogFleshRingComponent, Log, TEXT("FleshRingComponent: GPU 디버그 렌더링 ViewExtension 초기화 완료 (World: %s)"), *World->GetName());
}

void UFleshRingComponent::UpdateDebugPointBuffer()
{
	// ViewExtension이 없으면 초기화
	if (!DebugViewExtension.IsValid())
	{
		InitializeDebugViewExtension();
	}

	if (!DebugViewExtension.IsValid())
	{
		return;
	}

	// bShowAffectedVertices가 비활성화되면 렌더링 비활성화
	if (!bShowAffectedVertices || !bShowDebugVisualization)
	{
		DebugViewExtension->ClearDebugPointBuffer();
		return;
	}

	// DeformerInstance에서 캐싱된 DebugPointBuffer 가져오기
	if (!InternalDeformer)
	{
		return;
	}

	UFleshRingDeformerInstance* DeformerInstance = InternalDeformer->GetActiveInstance();
	if (!DeformerInstance)
	{
		return;
	}

	// CachedDebugPointBufferSharedPtr 가져오기
	// PointCount는 ViewExtension에서 버퍼의 NumElements로 직접 읽음 (스레드 안전)
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> DebugPointBufferSharedPtr = DeformerInstance->GetCachedDebugPointBufferSharedPtr();
	if (!DebugPointBufferSharedPtr.IsValid())
	{
		DebugViewExtension->ClearDebugPointBuffer();
		return;
	}

	// ViewExtension에 SharedPtr 전달
	DebugViewExtension->SetDebugPointBufferShared(DebugPointBufferSharedPtr);
}

void UFleshRingComponent::UpdateDebugBulgePointBuffer()
{
	// ViewExtension이 없으면 초기화
	if (!DebugViewExtension.IsValid())
	{
		InitializeDebugViewExtension();
	}

	if (!DebugViewExtension.IsValid())
	{
		return;
	}

	// bShowBulgeHeatmap가 비활성화되면 렌더링 비활성화
	if (!bShowBulgeHeatmap || !bShowDebugVisualization)
	{
		DebugViewExtension->ClearDebugBulgePointBuffer();
		return;
	}

	// DeformerInstance에서 캐싱된 DebugBulgePointBuffer 가져오기
	if (!InternalDeformer)
	{
		return;
	}

	UFleshRingDeformerInstance* DeformerInstance = InternalDeformer->GetActiveInstance();
	if (!DeformerInstance)
	{
		return;
	}

	// CachedDebugBulgePointBufferSharedPtr 가져오기
	// PointCount는 ViewExtension에서 버퍼의 NumElements로 직접 읽음 (스레드 안전)
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> DebugBulgePointBufferSharedPtr = DeformerInstance->GetCachedDebugBulgePointBufferSharedPtr();
	if (!DebugBulgePointBufferSharedPtr.IsValid())
	{
		DebugViewExtension->ClearDebugBulgePointBuffer();
		return;
	}

	// ViewExtension에 SharedPtr 전달
	DebugViewExtension->SetDebugBulgePointBufferShared(DebugBulgePointBufferSharedPtr);
}

#endif // WITH_EDITOR
