// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingPreviewScene.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"

FFleshRingPreviewScene::FFleshRingPreviewScene(const ConstructionValues& CVS)
	: FAdvancedPreviewScene(CVS)
{
	// 프리뷰 액터 생성
	CreatePreviewActor();
}

FFleshRingPreviewScene::~FFleshRingPreviewScene()
{
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

	// 스켈레탈 메시 컴포넌트 생성
	SkeletalMeshComponent = NewObject<USkeletalMeshComponent>(PreviewActor, TEXT("SkeletalMeshComponent"));
	SkeletalMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SkeletalMeshComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	SkeletalMeshComponent->bCastDynamicShadow = true;
	SkeletalMeshComponent->CastShadow = true;
	SkeletalMeshComponent->SetVisibility(true);
	SkeletalMeshComponent->RegisterComponent();
	PreviewActor->AddInstanceComponent(SkeletalMeshComponent);

	// FleshRing 컴포넌트 생성 (에디터에서는 Deformer 비활성화)
	FleshRingComponent = NewObject<UFleshRingComponent>(PreviewActor, TEXT("FleshRingComponent"));
	FleshRingComponent->bUseCustomTarget = true;
	FleshRingComponent->CustomTargetMesh = SkeletalMeshComponent;
	FleshRingComponent->bEnableFleshRing = false;  // 에디터 프리뷰에서는 Deformer 비활성화
	FleshRingComponent->RegisterComponent();
	PreviewActor->AddInstanceComponent(FleshRingComponent);
}

void FFleshRingPreviewScene::SetFleshRingAsset(UFleshRingAsset* InAsset)
{
	CurrentAsset = InAsset;

	if (!InAsset)
	{
		return;
	}

	// 스켈레탈 메시 설정
	USkeletalMesh* SkelMesh = InAsset->TargetSkeletalMesh.LoadSynchronous();
	SetSkeletalMesh(SkelMesh);

	// FleshRing 컴포넌트에 Asset 설정
	if (FleshRingComponent)
	{
		FleshRingComponent->FleshRingAsset = InAsset;
		FleshRingComponent->ApplyAsset();
	}

	// Ring 시각화 갱신
	RefreshRings(InAsset->Rings);
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

		UStaticMeshComponent* RingComp = NewObject<UStaticMeshComponent>(GetTransientPackage());
		RingComp->SetCollisionEnabled(ECollisionEnabled::NoCollision);

		// Ring 메시 설정
		UStaticMesh* RingMesh = RingSetting.RingMesh.LoadSynchronous();
		if (RingMesh)
		{
			RingComp->SetStaticMesh(RingMesh);
		}

		// 본 위치에 배치
		if (SkeletalMeshComponent && SkeletalMeshComponent->GetSkeletalMeshAsset())
		{
			int32 BoneIndex = SkeletalMeshComponent->GetBoneIndex(RingSetting.BoneName);
			if (BoneIndex != INDEX_NONE)
			{
				FTransform BoneTransform = SkeletalMeshComponent->GetBoneTransform(BoneIndex);
				RingComp->SetWorldTransform(BoneTransform);
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
