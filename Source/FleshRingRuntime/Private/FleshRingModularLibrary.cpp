// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingModularLibrary.h"
#include "FleshRingComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

bool UFleshRingModularLibrary::SwapModularPartWithRingCleanup(
	USkeletalMeshComponent* InSkeletalMeshComponent,
	USkeletalMesh* InNewMesh)
{
	if (!InSkeletalMeshComponent)
	{
		return false;
	}

	AActor* Owner = InSkeletalMeshComponent->GetOwner();
	if (!Owner)
	{
		return false;
	}

	// 1. Find FleshRingComponents targeting this SkeletalMeshComponent
	TArray<UFleshRingComponent*> RingComponents;
	Owner->GetComponents<UFleshRingComponent>(RingComponents);

	for (UFleshRingComponent* RingComp : RingComponents)
	{
		if (!RingComp)
		{
			continue;
		}

		const bool bIsTarget =
			(RingComp->bUseCustomTarget && RingComp->CustomTargetMesh == InSkeletalMeshComponent) ||
			(!RingComp->bUseCustomTarget && RingComp->GetResolvedTargetMesh() == InSkeletalMeshComponent);

		if (bIsTarget)
		{
			// 2. Detach ring asset (SkeletalMesh unchanged)
			RingComp->DetachRingAsset(/*bPreserveLeaderPose=*/true);
		}
	}

	// 3. Apply new modular part
	InSkeletalMeshComponent->SetSkeletalMeshAsset(InNewMesh);

	return true;
}
