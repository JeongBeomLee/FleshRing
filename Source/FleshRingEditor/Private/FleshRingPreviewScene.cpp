// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingPreviewScene.h"
#include "FleshRingComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"

FFleshRingPreviewScene::FFleshRingPreviewScene(const ConstructionValues& CVS)
	: FAdvancedPreviewScene(CVS)
{
	// 스켈레탈 메시 컴포넌트 생성
	SkeletalMeshComponent = NewObject<USkeletalMeshComponent>(GetTransientPackage());
	SkeletalMeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SkeletalMeshComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	SkeletalMeshComponent->bCastDynamicShadow = true;
	SkeletalMeshComponent->CastShadow = true;
	SkeletalMeshComponent->SetVisibility(true);
	SkeletalMeshComponent->MarkRenderStateDirty();
	AddComponent(SkeletalMeshComponent, FTransform::Identity);
}

FFleshRingPreviewScene::~FFleshRingPreviewScene()
{
	// 컴포넌트 정리
	if (SkeletalMeshComponent)
	{
		RemoveComponent(SkeletalMeshComponent);
	}

	for (UStaticMeshComponent* RingComp : RingMeshComponents)
	{
		if (RingComp)
		{
			RemoveComponent(RingComp);
		}
	}
	RingMeshComponents.Empty();
}

void FFleshRingPreviewScene::SetSkeletalMesh(USkeletalMesh* InMesh)
{
	if (SkeletalMeshComponent)
	{
		SkeletalMeshComponent->SetSkeletalMesh(InMesh);

		if (InMesh)
		{
			// 컴포넌트 초기화 및 바운드 업데이트
			SkeletalMeshComponent->InitAnim(true);
			SkeletalMeshComponent->SetVisibility(true);
			SkeletalMeshComponent->UpdateBounds();
			SkeletalMeshComponent->MarkRenderStateDirty();
		}
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
			FTransform BoneTransform = SkeletalMeshComponent->GetBoneTransform(RingSetting.BoneName);
			RingComp->SetWorldTransform(BoneTransform);
		}

		// 선택 상태에 따른 색상
		if (i == SelectedRingIndex)
		{
			RingComp->SetCustomPrimitiveDataFloat(0, 1.0f); // 선택됨 표시용
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
