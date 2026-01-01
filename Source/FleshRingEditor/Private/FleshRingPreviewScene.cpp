// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingPreviewScene.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "Animation/DebugSkelMeshComponent.h"
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
	SetSkeletalMesh(OriginalMesh);

	// 원본 메시 캐싱 (복원용) - 최초 설정 시에만
	if (!CachedOriginalMesh.IsValid() && OriginalMesh)
	{
		CachedOriginalMesh = OriginalMesh;
		UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Cached original mesh '%s' for restoration"),
			*OriginalMesh->GetName());
	}

	// FleshRing 컴포넌트에 Asset 설정 및 초기화
	if (FleshRingComponent)
	{
		FleshRingComponent->FleshRingAsset = InAsset;
		FleshRingComponent->ApplyAsset();
	}

	// ============================================
	// 2단계: Subdivision 활성화 시 PreviewMesh로 교체
	// ApplyAsset() 이후에 설정해야 덮어쓰이지 않음
	// ============================================
#if WITH_EDITOR
	if (InAsset->bEnableSubdivision)
	{
		// 프리뷰 메시가 없거나 재생성 필요 시 생성
		if (!InAsset->HasValidPreviewMesh() || InAsset->NeedsPreviewMeshRegeneration())
		{
			InAsset->GeneratePreviewMesh();
		}

		// 프리뷰 메시 사용 (있으면)
		if (InAsset->HasValidPreviewMesh())
		{
			SetSkeletalMesh(InAsset->PreviewSubdividedMesh);
			UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Using PreviewSubdividedMesh (Level %d, %d vertices)"),
				InAsset->PreviewSubdivisionLevel,
				InAsset->PreviewSubdividedMesh->GetResourceForRendering() ?
					InAsset->PreviewSubdividedMesh->GetResourceForRendering()->LODRenderData[0].StaticVertexBuffers.PositionVertexBuffer.GetNumVertices() : 0);
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
	// 3단계: Deformer 초기화 (딜레이)
	// InitializeForEditorPreview() 내부의 ResolveTargetMesh()가 메시를 덮어쓸 수 있으므로
	// 초기화 완료 후 PreviewMesh를 다시 적용
	// ============================================
	if (FleshRingComponent && FleshRingComponent->bEnableFleshRing)
	{
		UWorld* World = GetWorld();
		if (World)
		{
			FTimerHandle TimerHandle;
			TWeakObjectPtr<UFleshRingComponent> WeakComponent = FleshRingComponent;
			TWeakObjectPtr<UFleshRingAsset> WeakAsset = InAsset;
			TWeakObjectPtr<UDebugSkelMeshComponent> WeakSkelMesh = SkeletalMeshComponent;
			bool bUsePreviewMesh = InAsset->bEnableSubdivision && InAsset->HasValidPreviewMesh();

			World->GetTimerManager().SetTimer(
				TimerHandle,
				[WeakComponent, WeakAsset, WeakSkelMesh, bUsePreviewMesh]()
				{
					if (WeakComponent.IsValid())
					{
						WeakComponent->InitializeForEditorPreview();
					}

					// InitializeForEditorPreview()의 ResolveTargetMesh()가 메시를 덮어썼을 수 있으므로
					// PreviewMesh를 다시 적용
					if (bUsePreviewMesh && WeakAsset.IsValid() && WeakAsset->HasValidPreviewMesh() && WeakSkelMesh.IsValid())
					{
						WeakSkelMesh->SetSkeletalMesh(WeakAsset->PreviewSubdividedMesh);
						WeakSkelMesh->MarkRenderStateDirty();
						WeakSkelMesh->MarkRenderDynamicDataDirty();
						UE_LOG(LogTemp, Log, TEXT("FleshRingPreviewScene: Re-applied PreviewSubdividedMesh after Deformer init"));
					}
				},
				0.1f,  // 0.1초 딜레이
				false  // 반복 안 함
			);
		}
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
		SkeletalMeshComponent->SetSkeletalMesh(InMesh);

		if (InMesh)
		{
			SkeletalMeshComponent->InitAnim(true);
			SkeletalMeshComponent->SetVisibility(true);
			SkeletalMeshComponent->UpdateBounds();
			SkeletalMeshComponent->MarkRenderStateDirty();
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

		UStaticMeshComponent* RingComp = NewObject<UStaticMeshComponent>(PreviewActor);
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

void FFleshRingPreviewScene::SetSelectedRingIndex(int32 Index)
{
	SelectedRingIndex = Index;
}

void FFleshRingPreviewScene::BindToAssetDelegate()
{
	if (CurrentAsset && !AssetChangedDelegateHandle.IsValid())
	{
		AssetChangedDelegateHandle = CurrentAsset->OnAssetChanged.AddRaw(
			this, &FFleshRingPreviewScene::OnAssetChanged);
	}
}

void FFleshRingPreviewScene::UnbindFromAssetDelegate()
{
	if (CurrentAsset && AssetChangedDelegateHandle.IsValid())
	{
		CurrentAsset->OnAssetChanged.Remove(AssetChangedDelegateHandle);
		AssetChangedDelegateHandle.Reset();
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
