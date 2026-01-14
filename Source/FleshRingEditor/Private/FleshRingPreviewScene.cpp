// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingPreviewScene.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "FleshRingUtils.h"
#include "FleshRingMeshComponent.h"
#include "FleshRingDeformerInstance.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/MeshDeformerInstance.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Editor.h"

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
			SkeletalMeshComponent->SetSkeletalMesh(OriginalMesh);
			UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Restored original mesh '%s' on destruction"),
				OriginalMesh ? *OriginalMesh->GetName() : TEXT("null"));
		}
	}
	CachedOriginalMesh.Reset();

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

	if (!InAsset)
	{
		return;
	}

	// 새 에셋에 델리게이트 바인딩
	BindToAssetDelegate();

	// ============================================
	// 1단계: 먼저 원본 메시로 설정 (FleshRingComponent 초기화용)
	// ============================================
	USkeletalMesh* OriginalMesh = InAsset->TargetSkeletalMesh.LoadSynchronous();

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
		if (InAsset->HasValidPreviewMesh() && !InAsset->NeedsPreviewMeshRegeneration())
		{
			// 유효한 PreviewMesh 존재 - 그것을 표시해야 함
			TargetDisplayMesh = InAsset->SubdivisionSettings.PreviewSubdividedMesh;
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

		// Ring 메시 갱신
		RefreshRings(InAsset->Rings);
		return;
	}

	// 메시가 변경된 경우에만 DeformerInstance 파괴
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

	// FleshRing 컴포넌트에 Asset 설정 및 초기화
	if (FleshRingComponent)
	{
		FleshRingComponent->FleshRingAsset = InAsset;
		FleshRingComponent->ApplyAsset();

		// ApplyAsset() 후 즉시 Ring 메시 가시성 적용 (깜빡임 방지)
		// 주의: SetupRingMeshes()에서 RegisterComponent() 전에 이미 설정됨
		// 여기서는 bRingMeshesVisible이 변경된 경우를 위해 다시 적용
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
	// 2단계: Subdivision 활성화 시 PreviewMesh로 교체
	// ApplyAsset() 이후에 설정해야 덮어쓰이지 않음
	// ============================================
#if WITH_EDITOR
	if (InAsset->SubdivisionSettings.bEnableSubdivision)
	{
		// 프리뷰 메시가 없거나 재생성 필요 시 생성
		if (!InAsset->HasValidPreviewMesh() || InAsset->NeedsPreviewMeshRegeneration())
		{
			InAsset->GeneratePreviewMesh();
		}

		// 프리뷰 메시 사용 (있으면)
		if (InAsset->HasValidPreviewMesh())
		{
			SetSkeletalMesh(InAsset->SubdivisionSettings.PreviewSubdividedMesh);
			UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Using PreviewSubdividedMesh (Level %d, %d vertices)"),
				InAsset->SubdivisionSettings.PreviewSubdivisionLevel,
				InAsset->SubdivisionSettings.PreviewSubdividedMesh->GetResourceForRendering() ?
					InAsset->SubdivisionSettings.PreviewSubdividedMesh->GetResourceForRendering()->LODRenderData[0].StaticVertexBuffers.PositionVertexBuffer.GetNumVertices() : 0);
		}
	}
	else
	{
		// Subdivision 비활성화 시 프리뷰 메시 제거 및 원본 복원
		InAsset->ClearPreviewMesh();

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
	// 3단계: Deformer 초기화 예약
	// 메시가 실제로 렌더링된 후에 초기화해야 GPU 리소스가 준비됨
	// ViewportClient::Tick()에서 WasRecentlyRendered() 체크 후 초기화
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

		SkeletalMeshComponent->SetSkeletalMesh(InMesh);

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

	// InitializeForEditorPreview()의 ResolveTargetMesh()가 메시를 덮어썼을 수 있으므로
	// PreviewMesh를 다시 적용
	if (CurrentAsset)
	{
		bool bUsePreviewMesh = CurrentAsset->SubdivisionSettings.bEnableSubdivision && CurrentAsset->HasValidPreviewMesh();
		if (bUsePreviewMesh && SkeletalMeshComponent)
		{
			SkeletalMeshComponent->SetSkeletalMesh(CurrentAsset->SubdivisionSettings.PreviewSubdividedMesh);
			UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Re-applied PreviewSubdividedMesh after Deformer init"));
		}
	}

	UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Deformer initialization complete"));
}


