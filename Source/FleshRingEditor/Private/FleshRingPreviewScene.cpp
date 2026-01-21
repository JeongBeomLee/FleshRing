// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingPreviewScene.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "FleshRingUtils.h"
#include "FleshRingMeshComponent.h"
#include "FleshRingDeformerInstance.h"
#include "FleshRingSubdivisionProcessor.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/MeshDeformerInstance.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Editor.h"
#include "RenderingThread.h"         // FlushRenderingCommands용
#include "UObject/UObjectGlobals.h"  // CollectGarbage용
#include "MeshDescription.h"
#include "SkeletalMeshAttributes.h"

FFleshRingPreviewScene::FFleshRingPreviewScene(const ConstructionValues& CVS)
	: FAdvancedPreviewScene(CVS)
{
	// 프리뷰 액터 생성
	CreatePreviewActor();
}

FFleshRingPreviewScene::~FFleshRingPreviewScene()
{
	// 델리게이트 구독 해제
	UnbindFromAssetDelegate();

	// 원본 메시 복원 (PreviewSubdividedMesh가 적용되어 있던 경우)
	if (SkeletalMeshComponent && CachedOriginalMesh.IsValid())
	{
		USkeletalMesh* CurrentMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
		USkeletalMesh* OriginalMesh = CachedOriginalMesh.Get();
		if (CurrentMesh != OriginalMesh)
		{
			// ★ Undo 비활성화
			ITransaction* OldGUndo = GUndo;
			GUndo = nullptr;

			SkeletalMeshComponent->SetSkeletalMesh(OriginalMesh);

			GUndo = OldGUndo;

			UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Restored original mesh '%s' on destruction"),
				OriginalMesh ? *OriginalMesh->GetName() : TEXT("null"));
		}
	}
	CachedOriginalMesh.Reset();

	// PreviewSubdividedMesh 정리
	ClearPreviewMesh();

	// Ring 메시 컴포넌트 정리
	for (UStaticMeshComponent* RingComp : RingMeshComponents)
	{
		if (RingComp)
		{
			RemoveComponent(RingComp);
		}
	}
	RingMeshComponents.Empty();

	// 프리뷰 액터 정리
	if (PreviewActor)
	{
		PreviewActor->Destroy();
		PreviewActor = nullptr;
	}

	SkeletalMeshComponent = nullptr;
	FleshRingComponent = nullptr;
}

void FFleshRingPreviewScene::CreatePreviewActor()
{
	// 프리뷰 월드에 액터 생성
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Name = FName(TEXT("FleshRingPreviewActor"));
	SpawnParams.ObjectFlags = RF_Transient;

	PreviewActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
	if (!PreviewActor)
	{
		return;
	}

	// 스켈레탈 메시 컴포넌트 생성 (DebugSkelMesh 사용 - Persona 스타일 고정 본 색상)
	SkeletalMeshComponent = NewObject<UDebugSkelMeshComponent>(PreviewActor, TEXT("SkeletalMeshComponent"));
	SkeletalMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SkeletalMeshComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	SkeletalMeshComponent->bCastDynamicShadow = true;
	SkeletalMeshComponent->CastShadow = true;
	SkeletalMeshComponent->SetVisibility(true);
	SkeletalMeshComponent->SkeletonDrawMode = ESkeletonDrawMode::Default;  // 본 표시 및 선택 가능
	SkeletalMeshComponent->RegisterComponent();
	PreviewActor->AddInstanceComponent(SkeletalMeshComponent);

	// FleshRing 컴포넌트 생성 (에디터에서도 Deformer 활성화)
	FleshRingComponent = NewObject<UFleshRingComponent>(PreviewActor, TEXT("FleshRingComponent"));
	FleshRingComponent->bUseCustomTarget = true;
	FleshRingComponent->CustomTargetMesh = SkeletalMeshComponent;
	FleshRingComponent->bEnableFleshRing = true;  // 에디터 프리뷰에서도 Deformer 활성화
	FleshRingComponent->RegisterComponent();
	PreviewActor->AddInstanceComponent(FleshRingComponent);
}

void FFleshRingPreviewScene::SetFleshRingAsset(UFleshRingAsset* InAsset)
{
	// 기존 에셋에서 델리게이트 해제
	UnbindFromAssetDelegate();

	CurrentAsset = InAsset;

	// ★ nullptr 및 GC된 객체 체크 (Timer 콜백에서 호출 시 유효하지 않을 수 있음)
	if (!InAsset || !IsValid(InAsset))
	{
		return;
	}

	// 새 에셋에 델리게이트 바인딩
	BindToAssetDelegate();

	// ============================================
	// 1단계: 먼저 원본 메시로 설정 (FleshRingComponent 초기화용)
	// ============================================
	// ★ Soft Reference 유효성 체크 (오래된 에셋의 stale reference 방지)
	USkeletalMesh* OriginalMesh = nullptr;
	if (!InAsset->TargetSkeletalMesh.IsNull())
	{
		OriginalMesh = InAsset->TargetSkeletalMesh.LoadSynchronous();
		// LoadSynchronous 후 추가 검증 (corrupt 객체 방지)
		if (OriginalMesh && !IsValid(OriginalMesh))
		{
			UE_LOG(LogTemp, Warning, TEXT("FleshRingPreviewScene: TargetSkeletalMesh reference is invalid (stale asset?)"));
			OriginalMesh = nullptr;
		}
	}

	// 메시 변경 여부 확인 (TargetSkeletalMesh 기준)
	const bool bOriginalMeshChanged = (CachedOriginalMesh.Get() != OriginalMesh);

	// 현재 표시 중인 메시
	USkeletalMesh* CurrentDisplayedMesh = SkeletalMeshComponent ? SkeletalMeshComponent->GetSkeletalMeshAsset() : nullptr;

	// 표시해야 할 메시 결정 + Subdivision 재생성 필요 여부
	USkeletalMesh* TargetDisplayMesh = OriginalMesh;
	bool bNeedsPreviewMeshGeneration = false;

#if WITH_EDITOR
	if (InAsset->SubdivisionSettings.bEnableSubdivision)
	{
		if (HasValidPreviewMesh() && !NeedsPreviewMeshRegeneration())
		{
			// 유효한 PreviewMesh 존재 - 그것을 표시해야 함
			TargetDisplayMesh = PreviewSubdividedMesh;
		}
		else
		{
			// PreviewMesh 재생성 필요 - full refresh 경로 필수
			bNeedsPreviewMeshGeneration = true;
		}
	}
#endif

	// 표시 메시 변경 필요 여부
	const bool bDisplayMeshChanged = (CurrentDisplayedMesh != TargetDisplayMesh);

	// 조건: 원본 동일 + 표시 메시 동일 + 재생성 필요 없음 + DeformerInstance 존재
	// 이 모든 조건 충족 시에만 early return (Ring 파라미터 갱신만 수행)
	if (!bOriginalMeshChanged && !bDisplayMeshChanged && !bNeedsPreviewMeshGeneration &&
		OriginalMesh && SkeletalMeshComponent && SkeletalMeshComponent->GetMeshDeformerInstance())
	{
		UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Mesh unchanged, skipping full refresh (preserving DeformerInstance caches)"));

		// Ring 메시만 갱신 (Tightness 등 파라미터 변경 반영)
		if (FleshRingComponent)
		{
			FleshRingComponent->FleshRingAsset = InAsset;
			// ApplyAsset() 대신 가벼운 갱신만 수행
			FleshRingComponent->UpdateRingTransforms();
			// RingMesh 변경 시에도 반영되도록 Ring 메시 재생성 + SDF 재생성
			FleshRingComponent->RefreshRingMeshes();
			FleshRingComponent->RefreshSDF();

			// DeformerInstance의 Tightness 캐시 무효화 (파라미터 변경 반영)
			if (UFleshRingDeformerInstance* DeformerInstance = Cast<UFleshRingDeformerInstance>(SkeletalMeshComponent->GetMeshDeformerInstance()))
			{
				DeformerInstance->InvalidateTightnessCache();
			}
		}

		// Ring 메시 갱신 (FleshRingComponent가 비활성화된 경우만)
		// bEnableFleshRing=true면 FleshRingComponent가 Ring Mesh 관리, PreviewScene은 정리만
		if (!FleshRingComponent || !FleshRingComponent->bEnableFleshRing)
		{
			RefreshRings(InAsset->Rings);
		}
		else
		{
			// FleshRingComponent가 Ring Mesh를 관리하므로 PreviewScene의 Ring Mesh 정리
			RefreshRings(TArray<FFleshRingSettings>());
		}
		return;
	}

	// ★ CL 320 복원: 원본 메시가 변경된 경우에만 DeformerInstance 파괴
	// (Subdivision 토글 시에는 Deformer 유지 - ApplyAsset이 먼저 실행되어 Deformer가 설정된 후 메시 교체)
	if (bOriginalMeshChanged && SkeletalMeshComponent)
	{
		UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Mesh changed, destroying DeformerInstance"));
		if (UMeshDeformerInstance* OldInstance = SkeletalMeshComponent->GetMeshDeformerInstance())
		{
			FlushRenderingCommands();
			OldInstance->MarkAsGarbage();
			OldInstance->ConditionalBeginDestroy();
		}
		// Deformer도 해제하여 SetSkeletalMesh()가 새 Instance를 생성하지 않도록 함
		SkeletalMeshComponent->SetMeshDeformer(nullptr);
	}

	// TargetSkeletalMesh가 null이면 씬 정리 후 리턴
	if (!OriginalMesh)
	{
		SetSkeletalMesh(nullptr);
		CachedOriginalMesh.Reset();  // 캐시 초기화 (다시 복원 안 되게)
		if (FleshRingComponent)
		{
			FleshRingComponent->FleshRingAsset = InAsset;
			FleshRingComponent->ApplyAsset();
		}
		RefreshRings(TArray<FFleshRingSettings>());  // Ring도 정리
		return;
	}

	SetSkeletalMesh(OriginalMesh);

	// 원본 메시 캐싱 (복원용) - 메시 변경 시에도 갱신
	if (CachedOriginalMesh.IsValid() && CachedOriginalMesh.Get() != OriginalMesh)
	{
		// 메시가 변경되었으면 캐시 갱신
		CachedOriginalMesh = OriginalMesh;
		UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Updated cached mesh to '%s' (mesh changed)"),
			OriginalMesh ? *OriginalMesh->GetName() : TEXT("null"));
	}
	else if (!CachedOriginalMesh.IsValid() && OriginalMesh)
	{
		// 최초 설정
		CachedOriginalMesh = OriginalMesh;
		UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Cached original mesh '%s' for restoration"),
			*OriginalMesh->GetName());
	}

	// ============================================
	// 2단계: FleshRing 컴포넌트 초기화 (Subdivision 처리 전에!)
	// ★ CL 320 순서 복원: ApplyAsset()을 먼저 호출하여 Deformer가 설정된 후 메시 교체
	// ============================================
	if (FleshRingComponent)
	{
		FleshRingComponent->FleshRingAsset = InAsset;
		FleshRingComponent->ApplyAsset();

		// ApplyAsset() 후 즉시 Ring 메시 가시성 적용 (깜빡임 방지)
		const auto& ComponentRingMeshes = FleshRingComponent->GetRingMeshComponents();
		for (UStaticMeshComponent* RingComp : ComponentRingMeshes)
		{
			if (RingComp)
			{
				RingComp->SetVisibility(bRingMeshesVisible);
			}
		}
	}

	// ============================================
	// 3단계: Subdivision 처리 (ApplyAsset 후에!)
	// ★ Deformer가 이미 설정된 후 메시를 교체하면 Deformer가 유지됨
	// ============================================
#if WITH_EDITOR
	if (InAsset->SubdivisionSettings.bEnableSubdivision)
	{
		// 프리뷰 메시가 없거나 재생성 필요 시 생성
		if (!HasValidPreviewMesh() || NeedsPreviewMeshRegeneration())
		{
			GeneratePreviewMesh();
		}

		// 프리뷰 메시 사용 (있으면)
		if (HasValidPreviewMesh())
		{
			SetSkeletalMesh(PreviewSubdividedMesh);

			// ★ 렌더 리소스 동기화 (IndexBuffer 초기화 대기)
			if (SkeletalMeshComponent)
			{
				SkeletalMeshComponent->MarkRenderStateDirty();
				FlushRenderingCommands();
			}

			// ★ GC 방지: 로깅 전 유효성 체크 (Timer 콜백에서 호출 시 객체가 destroyed될 수 있음)
			if (IsValid(InAsset) && IsValid(PreviewSubdividedMesh))
			{
				FSkeletalMeshRenderData* RenderData = PreviewSubdividedMesh->GetResourceForRendering();
				UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Using PreviewSubdividedMesh (Level %d, %d vertices)"),
					InAsset->SubdivisionSettings.PreviewSubdivisionLevel,
					RenderData ? RenderData->LODRenderData[0].StaticVertexBuffers.PositionVertexBuffer.GetNumVertices() : 0);
			}
		}
	}
	else
	{
		// Subdivision 비활성화 시 프리뷰 메시 제거 및 원본 복원
		ClearPreviewMesh();

		// 원본 메시로 복원
		if (CachedOriginalMesh.IsValid() && SkeletalMeshComponent)
		{
			USkeletalMesh* CurrentMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
			USkeletalMesh* OrigMesh = CachedOriginalMesh.Get();
			if (CurrentMesh != OrigMesh)
			{
				SetSkeletalMesh(OrigMesh);
				UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Restored original mesh '%s' (subdivision disabled)"),
					OrigMesh ? *OrigMesh->GetName() : TEXT("null"));
			}
		}
	}
#endif

	// ============================================
	// 4단계: Deformer 초기화 예약
	// ★ CL 320 복원: bPendingDeformerInit만 설정
	// ============================================
	if (FleshRingComponent && FleshRingComponent->bEnableFleshRing)
	{
		bPendingDeformerInit = true;
		UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Deformer init pending (waiting for mesh to be rendered)"));
	}

	// Deformer 비활성화 시에만 Ring 시각화 (활성화 시 FleshRingComponent가 관리)
	if (!FleshRingComponent || !FleshRingComponent->bEnableFleshRing)
	{
		RefreshRings(InAsset->Rings);
	}
	else
	{
		// FleshRingComponent가 Ring Mesh를 관리하므로 PreviewScene의 Ring Mesh 정리
		RefreshRings(TArray<FFleshRingSettings>());
	}

	// ============================================
	// 5단계: 사용하지 않는 PreviewMesh 정리 (CL 325 메시 정리 코드)
	// ★ 메모리 누수 방지: Subdivision 토글 또는 Refresh 시 이전 PreviewMesh GC
	// ============================================
	if (bDisplayMeshChanged || bNeedsPreviewMeshGeneration)
	{
		FlushRenderingCommands();
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: GC executed to clean up unused PreviewMesh"));
	}
}

void FFleshRingPreviewScene::SetSkeletalMesh(USkeletalMesh* InMesh)
{
	if (SkeletalMeshComponent)
	{
		// 메시 유효성 검사 (Undo/Redo 크래시 방지 + 렌더 리소스 초기화 검증)
		if (InMesh && !FleshRingUtils::IsSkeletalMeshValid(InMesh, /*bLogWarnings=*/ true))
		{
			UE_LOG(LogTemp, Warning,
				TEXT("FleshRingPreviewScene::SetSkeletalMesh: Mesh '%s' is invalid, skipping"),
				*InMesh->GetName());
			return;
		}

		// ★ Undo 비활성화하여 메시 교체 시 트랜잭션에 캡처되지 않도록 함
		// (이전 메시가 TransBuffer에 캡처되면 GC 불가)
		ITransaction* OldGUndo = GUndo;
		GUndo = nullptr;

		SkeletalMeshComponent->SetSkeletalMesh(InMesh);

		GUndo = OldGUndo;

		if (InMesh)
		{
			SkeletalMeshComponent->InitAnim(true);
			SkeletalMeshComponent->SetVisibility(true);
			SkeletalMeshComponent->UpdateBounds();
			SkeletalMeshComponent->MarkRenderStateDirty();
		}
		else
		{
			// 메시가 nullptr이면 컴포넌트 숨기기
			SkeletalMeshComponent->SetVisibility(false);
		}
	}
}

void FFleshRingPreviewScene::RefreshPreview()
{
	if (CurrentAsset)
	{
		SetFleshRingAsset(CurrentAsset);
	}
}

void FFleshRingPreviewScene::RefreshRings(const TArray<FFleshRingSettings>& Rings)
{
	// 기존 Ring 컴포넌트 제거
	for (UStaticMeshComponent* RingComp : RingMeshComponents)
	{
		if (RingComp)
		{
			RemoveComponent(RingComp);
		}
	}
	RingMeshComponents.Empty();

	// 새 Ring 컴포넌트 생성
	for (int32 i = 0; i < Rings.Num(); ++i)
	{
		const FFleshRingSettings& RingSetting = Rings[i];

		UFleshRingMeshComponent* RingComp = NewObject<UFleshRingMeshComponent>(PreviewActor);
		RingComp->SetRingIndex(i);  // HitProxy에서 사용할 Ring 인덱스 설정
		RingComp->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		RingComp->SetCollisionResponseToAllChannels(ECR_Ignore);
		RingComp->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
		RingComp->bSelectable = true;

		// Ring 메시 설정
		UStaticMesh* RingMesh = RingSetting.RingMesh.LoadSynchronous();
		if (RingMesh)
		{
			RingComp->SetStaticMesh(RingMesh);
		}

		// 본 위치에 배치 (MeshOffset, MeshRotation 적용)
		if (SkeletalMeshComponent && SkeletalMeshComponent->GetSkeletalMeshAsset())
		{
			int32 BoneIndex = SkeletalMeshComponent->GetBoneIndex(RingSetting.BoneName);
			if (BoneIndex != INDEX_NONE)
			{
				FTransform BoneTransform = SkeletalMeshComponent->GetBoneTransform(BoneIndex);
				FQuat BoneRotation = BoneTransform.GetRotation();

				// MeshOffset 적용 (본 로컬 좌표계)
				FVector MeshLocation = BoneTransform.GetLocation() + BoneRotation.RotateVector(RingSetting.MeshOffset);

				// 본 회전 * 메시 회전 = 월드 회전 (기본값으로 본의 X축과 메시의 Z축이 일치)
				FQuat MeshWorldRotation = BoneRotation * RingSetting.MeshRotation;

				RingComp->SetWorldLocationAndRotation(MeshLocation, MeshWorldRotation);
				RingComp->SetWorldScale3D(RingSetting.MeshScale);
			}
		}

		// 현재 Show Flag에 맞게 가시성 설정 (AddComponent 전에 설정)
		RingComp->SetVisibility(bRingMeshesVisible);

		AddComponent(RingComp, RingComp->GetComponentTransform());
		RingMeshComponents.Add(RingComp);
	}
}

void FFleshRingPreviewScene::UpdateRingTransform(int32 Index, const FTransform& Transform)
{
	if (RingMeshComponents.IsValidIndex(Index) && RingMeshComponents[Index])
	{
		RingMeshComponents[Index]->SetWorldTransform(Transform);
	}
}

void FFleshRingPreviewScene::UpdateAllRingTransforms()
{
	if (!CurrentAsset || !SkeletalMeshComponent || !SkeletalMeshComponent->GetSkeletalMeshAsset())
	{
		return;
	}

	const TArray<FFleshRingSettings>& Rings = CurrentAsset->Rings;

	for (int32 i = 0; i < Rings.Num() && i < RingMeshComponents.Num(); ++i)
	{
		UStaticMeshComponent* RingComp = RingMeshComponents[i];
		if (!RingComp)
		{
			continue;
		}

		const FFleshRingSettings& RingSetting = Rings[i];
		int32 BoneIndex = SkeletalMeshComponent->GetBoneIndex(RingSetting.BoneName);
		if (BoneIndex != INDEX_NONE)
		{
			FTransform BoneTransform = SkeletalMeshComponent->GetBoneTransform(BoneIndex);
			FQuat BoneRotation = BoneTransform.GetRotation();

			// MeshOffset 적용 (본 로컬 좌표계)
			FVector MeshLocation = BoneTransform.GetLocation() + BoneRotation.RotateVector(RingSetting.MeshOffset);

			// 본 회전 * 메시 회전 = 월드 회전
			FQuat MeshWorldRotation = BoneRotation * RingSetting.MeshRotation;

			RingComp->SetWorldLocationAndRotation(MeshLocation, MeshWorldRotation);
			RingComp->SetWorldScale3D(RingSetting.MeshScale);
		}
	}
}

void FFleshRingPreviewScene::SetSelectedRingIndex(int32 Index)
{
	SelectedRingIndex = Index;
}

void FFleshRingPreviewScene::SetRingMeshesVisible(bool bVisible)
{
	bRingMeshesVisible = bVisible;

	// FleshRingComponent의 bShowRingMesh도 동기화 (SetupRingMeshes 시 적용되도록)
	if (FleshRingComponent)
	{
		FleshRingComponent->bShowRingMesh = bVisible;
	}

	// 1. PreviewScene의 RingMeshComponents (Deformer 비활성화 시)
	for (UStaticMeshComponent* RingComp : RingMeshComponents)
	{
		if (RingComp)
		{
			RingComp->SetVisibility(bVisible);
		}
	}

	// 2. FleshRingComponent의 RingMeshComponents (Deformer 활성화 시)
	if (FleshRingComponent)
	{
		const auto& ComponentRingMeshes = FleshRingComponent->GetRingMeshComponents();
		for (UStaticMeshComponent* RingComp : ComponentRingMeshes)
		{
			if (RingComp)
			{
				RingComp->SetVisibility(bVisible);
			}
		}
	}
}

void FFleshRingPreviewScene::BindToAssetDelegate()
{
	if (CurrentAsset)
	{
		if (!AssetChangedDelegateHandle.IsValid())
		{
			AssetChangedDelegateHandle = CurrentAsset->OnAssetChanged.AddRaw(
				this, &FFleshRingPreviewScene::OnAssetChanged);
		}
	}
}

void FFleshRingPreviewScene::UnbindFromAssetDelegate()
{
	if (CurrentAsset)
	{
		if (AssetChangedDelegateHandle.IsValid())
		{
			CurrentAsset->OnAssetChanged.Remove(AssetChangedDelegateHandle);
			AssetChangedDelegateHandle.Reset();
		}
	}
}

void FFleshRingPreviewScene::OnAssetChanged(UFleshRingAsset* ChangedAsset)
{
	// 동일한 에셋인지 확인
	if (ChangedAsset == CurrentAsset)
	{
		// 트랜잭션 완료 후 다음 틱에서 안전하게 갱신
		// (PostEditChangeProperty에서 호출될 때 트랜잭션 내부일 수 있음 - 메시 생성 시 Undo 크래시 방지)
		if (GEditor)
		{
			TWeakObjectPtr<UFleshRingAsset> WeakAsset = ChangedAsset;
			FFleshRingPreviewScene* Scene = this;

			GEditor->GetTimerManager()->SetTimerForNextTick(
				[Scene, WeakAsset]()
				{
					if (WeakAsset.IsValid() && Scene && Scene->CurrentAsset == WeakAsset.Get())
					{
						UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Asset changed, refreshing preview (deferred)..."));
						Scene->RefreshPreview();
					}
				});
		}
	}
}

bool FFleshRingPreviewScene::IsPendingDeformerInit() const
{
	if (!bPendingDeformerInit)
	{
		return false;
	}

	// 스켈레탈 메시가 렌더링되었는지 확인
	// WasRecentlyRendered()는 마지막 렌더링 시간을 체크하여 최근 렌더링 여부 반환
	if (SkeletalMeshComponent && SkeletalMeshComponent->WasRecentlyRendered(0.1f))
	{
		return true;
	}

	return false;
}

void FFleshRingPreviewScene::ExecutePendingDeformerInit()
{
	if (!bPendingDeformerInit)
	{
		return;
	}

	bPendingDeformerInit = false;

	if (!FleshRingComponent || !FleshRingComponent->bEnableFleshRing)
	{
		return;
	}

	UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Mesh rendered, executing deferred Deformer init"));

	// Deformer 초기화
	FleshRingComponent->InitializeForEditorPreview();

	// FleshRingComponent가 생성한 Ring 메시에 Show Flag 적용
	const auto& RingMeshes = FleshRingComponent->GetRingMeshComponents();
	for (UStaticMeshComponent* RingComp : RingMeshes)
	{
		if (RingComp)
		{
			RingComp->SetVisibility(bRingMeshesVisible);
		}
	}

	// PreviewMesh 재적용 (InitializeForEditorPreview가 메시를 덮어썼을 수 있음)
	if (CurrentAsset)
	{
		bool bUsePreviewMesh = CurrentAsset->SubdivisionSettings.bEnableSubdivision
			&& HasValidPreviewMesh();
		if (bUsePreviewMesh && SkeletalMeshComponent)
		{
			// ★ Undo 비활성화하여 메시 교체 시 트랜잭션에 캡처되지 않도록 함
			ITransaction* OldGUndo = GUndo;
			GUndo = nullptr;

			SkeletalMeshComponent->SetSkeletalMesh(PreviewSubdividedMesh);

			GUndo = OldGUndo;

			SkeletalMeshComponent->MarkRenderStateDirty();
			FlushRenderingCommands();
		}
	}
}

// =====================================
// Preview Mesh Management (에셋에서 분리하여 트랜잭션 제외)
// =====================================

void FFleshRingPreviewScene::ClearPreviewMesh()
{
	if (PreviewSubdividedMesh)
	{
		USkeletalMesh* OldMesh = PreviewSubdividedMesh;

		UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene::ClearPreviewMesh: Destroying '%s'"),
			*OldMesh->GetName());

		// 1. 포인터 해제
		PreviewSubdividedMesh = nullptr;

		// 2. 렌더 리소스 완전 해제
		OldMesh->ReleaseResources();
		OldMesh->ReleaseResourcesFence.Wait();
		FlushRenderingCommands();

		// 3. Outer를 TransientPackage로 변경
		OldMesh->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);

		// 4. 플래그 설정
		OldMesh->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
		OldMesh->SetFlags(RF_Transient);

		// 5. GC 대상으로 표시
		OldMesh->MarkAsGarbage();

		// 캐시 무효화
		bPreviewMeshCacheValid = false;
		LastPreviewBoneConfigHash = 0;
	}
}

void FFleshRingPreviewScene::InvalidatePreviewMeshCache()
{
	bPreviewMeshCacheValid = false;
	LastPreviewBoneConfigHash = MAX_uint32;
}

bool FFleshRingPreviewScene::IsPreviewMeshCacheValid() const
{
	if (!HasValidPreviewMesh())
	{
		return false;
	}

	// 해시 비교
	return LastPreviewBoneConfigHash == CalculatePreviewBoneConfigHash();
}

bool FFleshRingPreviewScene::NeedsPreviewMeshRegeneration() const
{
	if (!CurrentAsset || !CurrentAsset->SubdivisionSettings.bEnableSubdivision)
	{
		return false;
	}

	// 메시가 없으면 재생성 필요
	if (PreviewSubdividedMesh == nullptr)
	{
		return true;
	}

	// 캐시가 무효화되었으면 재생성 필요
	if (!IsPreviewMeshCacheValid())
	{
		return true;
	}

	return false;
}

uint32 FFleshRingPreviewScene::CalculatePreviewBoneConfigHash() const
{
	if (!CurrentAsset)
	{
		return 0;
	}

	uint32 Hash = 0;

	// TargetSkeletalMesh 포인터 해시 (메시 변경 시 캐시 무효화)
	Hash = HashCombine(Hash, GetTypeHash(CurrentAsset->TargetSkeletalMesh.Get()));

	// 링 부착 본 목록 해시
	for (const FFleshRingSettings& Ring : CurrentAsset->Rings)
	{
		Hash = HashCombine(Hash, GetTypeHash(Ring.BoneName));
	}

	// subdivision 파라미터 해시
	Hash = HashCombine(Hash, GetTypeHash(CurrentAsset->SubdivisionSettings.PreviewSubdivisionLevel));
	Hash = HashCombine(Hash, GetTypeHash(CurrentAsset->SubdivisionSettings.PreviewBoneHopCount));
	Hash = HashCombine(Hash, GetTypeHash(FMath::RoundToInt(CurrentAsset->SubdivisionSettings.PreviewBoneWeightThreshold * 255)));
	Hash = HashCombine(Hash, GetTypeHash(CurrentAsset->SubdivisionSettings.MinEdgeLength));

	return Hash;
}

void FFleshRingPreviewScene::GeneratePreviewMesh()
{
	if (!CurrentAsset)
	{
		return;
	}

	// 캐시 체크 - 이미 유효하면 재생성 불필요
	if (IsPreviewMeshCacheValid())
	{
		return;
	}

	// ★ 전체 메시 생성/제거 과정을 Undo 시스템에서 제외
	// 이전 메시 정리 및 새 메시 생성이 트랜잭션에 캡처되면 GC 불가
	ITransaction* OldGUndo = GUndo;
	GUndo = nullptr;

	// 기존 PreviewMesh가 있으면 먼저 제거
	if (PreviewSubdividedMesh)
	{
		ClearPreviewMesh();
	}

	if (!CurrentAsset->SubdivisionSettings.bEnableSubdivision)
	{
		GUndo = OldGUndo;
		return;
	}

	if (CurrentAsset->TargetSkeletalMesh.IsNull())
	{
		UE_LOG(LogTemp, Warning, TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: TargetSkeletalMesh가 설정되지 않음"));
		GUndo = OldGUndo;
		return;
	}

	USkeletalMesh* SourceMesh = CurrentAsset->TargetSkeletalMesh.LoadSynchronous();
	if (!SourceMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: SourceMesh 로드 실패"));
		GUndo = OldGUndo;
		return;
	}

	const double StartTime = FPlatformTime::Seconds();

	// 1. 소스 메시 렌더 데이터 획득
	FSkeletalMeshRenderData* RenderData = SourceMesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: RenderData 없음"));
		GUndo = OldGUndo;
		return;
	}

	const FSkeletalMeshLODRenderData& SourceLODData = RenderData->LODRenderData[0];
	const uint32 SourceVertexCount = SourceLODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

	// 2. 소스 버텍스 데이터 추출
	TArray<FVector> SourcePositions;
	TArray<FVector> SourceNormals;
	TArray<FVector4> SourceTangents;
	TArray<FVector2D> SourceUVs;

	SourcePositions.SetNum(SourceVertexCount);
	SourceNormals.SetNum(SourceVertexCount);
	SourceTangents.SetNum(SourceVertexCount);
	SourceUVs.SetNum(SourceVertexCount);

	for (uint32 i = 0; i < SourceVertexCount; ++i)
	{
		SourcePositions[i] = FVector(SourceLODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i));
		SourceNormals[i] = FVector(SourceLODData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(i));
		FVector4f TangentX = SourceLODData.StaticVertexBuffers.StaticMeshVertexBuffer.VertexTangentX(i);
		SourceTangents[i] = FVector4(TangentX.X, TangentX.Y, TangentX.Z, TangentX.W);
		SourceUVs[i] = FVector2D(SourceLODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(i, 0));
	}

	// 인덱스 추출
	TArray<uint32> SourceIndices;
	const FRawStaticIndexBuffer16or32Interface* IndexBuffer = SourceLODData.MultiSizeIndexContainer.GetIndexBuffer();
	if (IndexBuffer)
	{
		const int32 NumIndices = IndexBuffer->Num();
		SourceIndices.SetNum(NumIndices);
		for (int32 i = 0; i < NumIndices; ++i)
		{
			SourceIndices[i] = IndexBuffer->Get(i);
		}
	}

	// 섹션별 머티리얼 인덱스 추출
	TArray<int32> SourceTriangleMaterialIndices;
	{
		const int32 NumTriangles = SourceIndices.Num() / 3;
		SourceTriangleMaterialIndices.SetNum(NumTriangles);
		for (const FSkelMeshRenderSection& Section : SourceLODData.RenderSections)
		{
			const int32 StartTriangle = Section.BaseIndex / 3;
			const int32 EndTriangle = StartTriangle + Section.NumTriangles;
			for (int32 TriIdx = StartTriangle; TriIdx < EndTriangle && TriIdx < NumTriangles; ++TriIdx)
			{
				SourceTriangleMaterialIndices[TriIdx] = Section.MaterialIndex;
			}
		}
	}

	// 본 웨이트 추출
	const int32 MaxBoneInfluences = SourceLODData.GetVertexBufferMaxBoneInfluences();
	TArray<TArray<uint16>> SourceBoneIndices;
	TArray<TArray<uint8>> SourceBoneWeights;
	SourceBoneIndices.SetNum(SourceVertexCount);
	SourceBoneWeights.SetNum(SourceVertexCount);

	TArray<FVertexBoneInfluence> VertexBoneInfluences;
	VertexBoneInfluences.SetNum(SourceVertexCount);

	// 버텍스별 섹션 인덱스 맵 생성
	TArray<int32> VertexToSectionIndex;
	VertexToSectionIndex.SetNum(SourceVertexCount);
	for (int32& SectionIdx : VertexToSectionIndex) { SectionIdx = INDEX_NONE; }
	for (int32 SectionIdx = 0; SectionIdx < SourceLODData.RenderSections.Num(); ++SectionIdx)
	{
		const FSkelMeshRenderSection& Section = SourceLODData.RenderSections[SectionIdx];
		const int32 StartIndex = Section.BaseIndex;
		const int32 EndIndex = StartIndex + Section.NumTriangles * 3;
		for (int32 IdxPos = StartIndex; IdxPos < EndIndex; ++IdxPos)
		{
			uint32 VertexIdx = SourceIndices[IdxPos];
			if (VertexIdx < SourceVertexCount && VertexToSectionIndex[VertexIdx] == INDEX_NONE)
			{
				VertexToSectionIndex[VertexIdx] = SectionIdx;
			}
		}
	}

	const FSkinWeightVertexBuffer* SkinWeightBuffer = SourceLODData.GetSkinWeightVertexBuffer();
	if (SkinWeightBuffer && SkinWeightBuffer->GetNumVertices() > 0)
	{
		const int32 ClampedInfluences = FMath::Min(MaxBoneInfluences, FVertexBoneInfluence::MAX_INFLUENCES);
		for (uint32 i = 0; i < SourceVertexCount; ++i)
		{
			SourceBoneIndices[i].SetNum(MaxBoneInfluences);
			SourceBoneWeights[i].SetNum(MaxBoneInfluences);

			FVertexBoneInfluence& Influence = VertexBoneInfluences[i];
			FMemory::Memzero(Influence.BoneIndices, sizeof(Influence.BoneIndices));
			FMemory::Memzero(Influence.BoneWeights, sizeof(Influence.BoneWeights));

			int32 SectionIdx = VertexToSectionIndex[i];
			const TArray<FBoneIndexType>* BoneMap = nullptr;
			if (SectionIdx != INDEX_NONE && SectionIdx < SourceLODData.RenderSections.Num())
			{
				BoneMap = &SourceLODData.RenderSections[SectionIdx].BoneMap;
			}
			for (int32 j = 0; j < MaxBoneInfluences; ++j)
			{
				uint16 LocalBoneIdx = SkinWeightBuffer->GetBoneIndex(i, j);
				uint8 Weight = SkinWeightBuffer->GetBoneWeight(i, j);
				uint16 GlobalBoneIdx = LocalBoneIdx;
				if (BoneMap && LocalBoneIdx < BoneMap->Num())
				{
					GlobalBoneIdx = (*BoneMap)[LocalBoneIdx];
				}
				SourceBoneIndices[i][j] = GlobalBoneIdx;
				SourceBoneWeights[i][j] = Weight;

				if (j < ClampedInfluences)
				{
					Influence.BoneIndices[j] = GlobalBoneIdx;
					Influence.BoneWeights[j] = Weight;
				}
			}
		}
	}

	// 3. 본 기반 Subdivision 프로세서 실행
	FFleshRingSubdivisionProcessor Processor;

	if (!Processor.SetSourceMesh(SourcePositions, SourceIndices, SourceUVs, SourceTriangleMaterialIndices))
	{
		UE_LOG(LogTemp, Warning, TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: SetSourceMesh 실패"));
		GUndo = OldGUndo;
		return;
	}
	Processor.SetVertexBoneInfluences(VertexBoneInfluences);

	FSubdivisionProcessorSettings Settings;
	Settings.MinEdgeLength = CurrentAsset->SubdivisionSettings.MinEdgeLength;
	Processor.SetSettings(Settings);

	FSubdivisionTopologyResult TopologyResult;

	// ★ Ring이 없으면 subdivision 스킵 (런타임 동작과 일치)
	if (CurrentAsset->Rings.Num() == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: Ring이 없어 Subdivision을 건너뜀"));
		GUndo = OldGUndo;
		return;
	}

	if (!Processor.HasBoneInfo())
	{
		// Ring 있음 + BoneInfo 없음 -> 스킵 (비정상 상황)
		UE_LOG(LogTemp, Error,
			TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: BoneInfo가 없어 Subdivision을 건너뜀. ")
			TEXT("SkeletalMesh '%s'에 SkinWeightBuffer가 없거나 본 웨이트 추출에 실패했습니다."),
			SourceMesh ? *SourceMesh->GetName() : TEXT("null"));
		GUndo = OldGUndo;
		return;
	}

	// Ring 부착 본 인덱스 수집
	const FReferenceSkeleton& RefSkeleton = SourceMesh->GetRefSkeleton();
	TArray<int32> RingBoneIndices;
	for (const FFleshRingSettings& Ring : CurrentAsset->Rings)
	{
		int32 BoneIdx = RefSkeleton.FindBoneIndex(Ring.BoneName);
		if (BoneIdx != INDEX_NONE)
		{
			RingBoneIndices.Add(BoneIdx);
		}
	}

	// ★ 유효한 BoneName을 가진 Ring이 없으면 스킵
	if (RingBoneIndices.Num() == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: 유효한 BoneName을 가진 Ring이 없어 Subdivision을 건너뜀. ")
			TEXT("Ring에 BoneName을 설정해주세요."));
		GUndo = OldGUndo;
		return;
	}

	TSet<int32> TargetBones = FFleshRingSubdivisionProcessor::GatherNeighborBones(
		RefSkeleton, RingBoneIndices, CurrentAsset->SubdivisionSettings.PreviewBoneHopCount);

	FBoneRegionSubdivisionParams BoneParams;
	BoneParams.TargetBoneIndices = TargetBones;
	BoneParams.BoneWeightThreshold = static_cast<uint8>(CurrentAsset->SubdivisionSettings.PreviewBoneWeightThreshold * 255);
	BoneParams.NeighborHopCount = CurrentAsset->SubdivisionSettings.PreviewBoneHopCount;
	BoneParams.MaxSubdivisionLevel = CurrentAsset->SubdivisionSettings.PreviewSubdivisionLevel;

	if (!Processor.ProcessBoneRegion(TopologyResult, BoneParams))
	{
		UE_LOG(LogTemp, Warning, TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: ProcessBoneRegion 실패"));
		GUndo = OldGUndo;
		return;
	}

	// 4. 새 버텍스 데이터 보간
	const int32 NewVertexCount = TopologyResult.VertexData.Num();
	TArray<FVector> NewPositions;
	TArray<FVector> NewNormals;
	TArray<FVector4> NewTangents;
	TArray<FVector2D> NewUVs;
	TArray<TArray<uint16>> NewBoneIndices;
	TArray<TArray<uint8>> NewBoneWeights;

	NewPositions.SetNum(NewVertexCount);
	NewNormals.SetNum(NewVertexCount);
	NewTangents.SetNum(NewVertexCount);
	NewUVs.SetNum(NewVertexCount);
	NewBoneIndices.SetNum(NewVertexCount);
	NewBoneWeights.SetNum(NewVertexCount);

	TMap<uint16, float> BoneWeightMap;
	TArray<TPair<uint16, float>> SortedWeights;

	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		const FSubdivisionVertexData& VD = TopologyResult.VertexData[i];
		const float U = VD.BarycentricCoords.X;
		const float V = VD.BarycentricCoords.Y;
		const float W = VD.BarycentricCoords.Z;

		const uint32 P0 = FMath::Min(VD.ParentV0, (uint32)(SourceVertexCount - 1));
		const uint32 P1 = FMath::Min(VD.ParentV1, (uint32)(SourceVertexCount - 1));
		const uint32 P2 = FMath::Min(VD.ParentV2, (uint32)(SourceVertexCount - 1));

		NewPositions[i] = SourcePositions[P0] * U + SourcePositions[P1] * V + SourcePositions[P2] * W;
		FVector InterpolatedNormal = SourceNormals[P0] * U + SourceNormals[P1] * V + SourceNormals[P2] * W;
		NewNormals[i] = InterpolatedNormal.GetSafeNormal();
		FVector4 InterpTangent = SourceTangents[P0] * U + SourceTangents[P1] * V + SourceTangents[P2] * W;
		FVector TangentDir = FVector(InterpTangent.X, InterpTangent.Y, InterpTangent.Z).GetSafeNormal();
		NewTangents[i] = FVector4(TangentDir.X, TangentDir.Y, TangentDir.Z, SourceTangents[P0].W);
		NewUVs[i] = SourceUVs[P0] * U + SourceUVs[P1] * V + SourceUVs[P2] * W;

		NewBoneIndices[i].SetNum(MaxBoneInfluences);
		NewBoneWeights[i].SetNum(MaxBoneInfluences);

		BoneWeightMap.Reset();
		SortedWeights.Reset();

		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (SourceBoneWeights[P0][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P0][j]) += (SourceBoneWeights[P0][j] / 255.0f) * U;
			if (SourceBoneWeights[P1][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P1][j]) += (SourceBoneWeights[P1][j] / 255.0f) * V;
			if (SourceBoneWeights[P2][j] > 0)
				BoneWeightMap.FindOrAdd(SourceBoneIndices[P2][j]) += (SourceBoneWeights[P2][j] / 255.0f) * W;
		}
		for (const auto& Pair : BoneWeightMap) { SortedWeights.Add(TPair<uint16, float>(Pair.Key, Pair.Value)); }
		SortedWeights.Sort([](const TPair<uint16, float>& A, const TPair<uint16, float>& B) { return A.Value > B.Value; });
		float TotalWeight = 0.0f;
		for (int32 j = 0; j < FMath::Min(SortedWeights.Num(), MaxBoneInfluences); ++j) { TotalWeight += SortedWeights[j].Value; }
		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (j < SortedWeights.Num() && TotalWeight > 0.0f)
			{
				NewBoneIndices[i][j] = SortedWeights[j].Key;
				NewBoneWeights[i][j] = FMath::Clamp<uint8>(FMath::RoundToInt((SortedWeights[j].Value / TotalWeight) * 255.0f), 0, 255);
			}
			else
			{
				NewBoneIndices[i][j] = 0;
				NewBoneWeights[i][j] = 0;
			}
		}
	}

	// 5. 프리뷰용 USkeletalMesh 생성
	// ★ Outer를 GetTransientPackage()로 설정 - PreviewScene 소멸 시 GC 대상
	FString MeshName = FString::Printf(TEXT("%s_Preview_%s"),
		*SourceMesh->GetName(),
		*FGuid::NewGuid().ToString(EGuidFormats::Short));
	PreviewSubdividedMesh = DuplicateObject<USkeletalMesh>(SourceMesh, GetTransientPackage(), FName(*MeshName));

	if (!PreviewSubdividedMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("FFleshRingPreviewScene::GeneratePreviewMesh: 메시 복제 실패"));
		GUndo = OldGUndo;
		return;
	}

	// 플래그 설정 - 트랜잭션에서 완전히 제외
	PreviewSubdividedMesh->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
	PreviewSubdividedMesh->SetFlags(RF_Transient);

	FlushRenderingCommands();
	PreviewSubdividedMesh->ReleaseResources();
	PreviewSubdividedMesh->ReleaseResourcesFence.Wait();

	if (PreviewSubdividedMesh->HasMeshDescription(0))
	{
		PreviewSubdividedMesh->ClearMeshDescription(0);
	}

	// 6. MeshDescription 생성
	FMeshDescription MeshDescription;
	FSkeletalMeshAttributes MeshAttributes(MeshDescription);
	MeshAttributes.Register();

	MeshDescription.ReserveNewVertices(NewVertexCount);
	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		const FVertexID VertexID = MeshDescription.CreateVertex();
		MeshDescription.GetVertexPositions()[VertexID] = FVector3f(NewPositions[i]);
	}

	MeshDescription.PolygonGroupAttributes().RegisterAttribute<FName>(MeshAttribute::PolygonGroup::ImportedMaterialSlotName);
	const int32 NumMaterials = SourceMesh ? SourceMesh->GetMaterials().Num() : 1;
	const int32 NumFaces = TopologyResult.Indices.Num() / 3;

	TSet<int32> UsedMaterialIndices;
	for (int32 TriIdx = 0; TriIdx < NumFaces; ++TriIdx)
	{
		int32 MatIdx = TopologyResult.TriangleMaterialIndices.IsValidIndex(TriIdx) ? TopologyResult.TriangleMaterialIndices[TriIdx] : 0;
		MatIdx = FMath::Clamp(MatIdx, 0, NumMaterials - 1);
		UsedMaterialIndices.Add(MatIdx);
	}

	TMap<int32, FPolygonGroupID> MaterialIndexToPolygonGroup;
	TArray<int32> SortedMaterialIndices = UsedMaterialIndices.Array();
	SortedMaterialIndices.Sort();
	for (int32 MatIdx : SortedMaterialIndices)
	{
		FPolygonGroupID GroupID = MeshDescription.CreatePolygonGroup();
		MaterialIndexToPolygonGroup.Add(MatIdx, GroupID);
		FName MaterialSlotName = NAME_None;
		if (SourceMesh && SourceMesh->GetMaterials().IsValidIndex(MatIdx))
		{
			MaterialSlotName = SourceMesh->GetMaterials()[MatIdx].ImportedMaterialSlotName;
		}
		if (MaterialSlotName.IsNone()) { MaterialSlotName = *FString::Printf(TEXT("Material_%d"), MatIdx); }
		MeshDescription.PolygonGroupAttributes().SetAttribute(GroupID, MeshAttribute::PolygonGroup::ImportedMaterialSlotName, 0, MaterialSlotName);
	}

	TArray<FVertexInstanceID> VertexInstanceIDs;
	VertexInstanceIDs.Reserve(TopologyResult.Indices.Num());
	for (int32 i = 0; i < TopologyResult.Indices.Num(); ++i)
	{
		const uint32 VertexIndex = TopologyResult.Indices[i];
		const FVertexID VertexID(VertexIndex);
		const FVertexInstanceID VertexInstanceID = MeshDescription.CreateVertexInstance(VertexID);
		VertexInstanceIDs.Add(VertexInstanceID);
		MeshAttributes.GetVertexInstanceUVs().Set(VertexInstanceID, 0, FVector2f(NewUVs[VertexIndex]));
		MeshAttributes.GetVertexInstanceNormals().Set(VertexInstanceID, FVector3f(NewNormals[VertexIndex]));
		MeshAttributes.GetVertexInstanceTangents().Set(VertexInstanceID, FVector3f(NewTangents[VertexIndex].X, NewTangents[VertexIndex].Y, NewTangents[VertexIndex].Z));
		MeshAttributes.GetVertexInstanceBinormalSigns().Set(VertexInstanceID, NewTangents[VertexIndex].W);
	}

	for (int32 i = 0; i < NumFaces; ++i)
	{
		TArray<FVertexInstanceID> TriangleVertexInstances;
		TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 0]);
		TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 1]);
		TriangleVertexInstances.Add(VertexInstanceIDs[i * 3 + 2]);
		int32 MatIdx = TopologyResult.TriangleMaterialIndices.IsValidIndex(i) ? TopologyResult.TriangleMaterialIndices[i] : 0;
		MatIdx = FMath::Clamp(MatIdx, 0, NumMaterials - 1);
		FPolygonGroupID* GroupID = MaterialIndexToPolygonGroup.Find(MatIdx);
		if (GroupID) { MeshDescription.CreatePolygon(*GroupID, TriangleVertexInstances); }
	}

	FSkinWeightsVertexAttributesRef SkinWeights = MeshAttributes.GetVertexSkinWeights();
	for (int32 i = 0; i < NewVertexCount; ++i)
	{
		FVertexID VertexID(i);
		TArray<UE::AnimationCore::FBoneWeight> BoneWeightArray;
		for (int32 j = 0; j < MaxBoneInfluences; ++j)
		{
			if (NewBoneWeights[i][j] > 0)
			{
				UE::AnimationCore::FBoneWeight BW;
				BW.SetBoneIndex(NewBoneIndices[i][j]);
				BW.SetWeight(NewBoneWeights[i][j] / 255.0f);
				BoneWeightArray.Add(BW);
			}
		}
		SkinWeights.Set(VertexID, BoneWeightArray);
	}

	PreviewSubdividedMesh->CreateMeshDescription(0, MoveTemp(MeshDescription));

	USkeletalMesh::FCommitMeshDescriptionParams CommitParams;
	CommitParams.bMarkPackageDirty = false;
	PreviewSubdividedMesh->CommitMeshDescription(0, CommitParams);
	PreviewSubdividedMesh->Build();
	PreviewSubdividedMesh->InitResources();

	FlushRenderingCommands();

	FBox BoundingBox(ForceInit);
	for (int32 i = 0; i < NewVertexCount; ++i) { BoundingBox += NewPositions[i]; }
	PreviewSubdividedMesh->SetImportedBounds(FBoxSphereBounds(BoundingBox));
	PreviewSubdividedMesh->CalculateExtendedBounds();

	// 캐시 해시 업데이트
	LastPreviewBoneConfigHash = CalculatePreviewBoneConfigHash();
	bPreviewMeshCacheValid = true;

	const double EndTime = FPlatformTime::Seconds();
	const double ElapsedMs = (EndTime - StartTime) * 1000.0;

	UE_LOG(LogTemp, Log, TEXT("FFleshRingPreviewScene::GeneratePreviewMesh 완료: %d vertices, %d triangles (%.2fms, CacheHash=%u)"),
		NewVertexCount, TopologyResult.SubdividedTriangleCount, ElapsedMs, LastPreviewBoneConfigHash);

	// ★ Undo 시스템 복원
	GUndo = OldGUndo;
}

