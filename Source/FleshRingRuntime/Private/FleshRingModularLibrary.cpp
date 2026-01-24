// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingModularLibrary.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "SkeletalMeshMerge.h"  // FSkeletalMeshMerge (Engine module)

//==========================================================================
// Skeletal Merging API
//==========================================================================

FFleshRingMergeOutput UFleshRingModularLibrary::RebuildMergedMesh(
	USkeletalMeshComponent* TargetComponent,
	const TArray<FFleshRingModularPart>& Parts,
	USkeleton* Skeleton)
{
	FFleshRingMergeOutput Output;

	// 1. Validation
	if (Parts.Num() == 0)
	{
		Output.Result = EFleshRingMergeResult::NoValidParts;
		Output.ErrorMessage = TEXT("No parts provided");
		return Output;
	}

	if (!Skeleton)
	{
		Output.Result = EFleshRingMergeResult::SkeletonMismatch;
		Output.ErrorMessage = TEXT("Skeleton is null");
		return Output;
	}

	// 2. Build mesh array and ring asset array
	TArray<USkeletalMesh*> MeshesToMerge;
	TArray<UFleshRingAsset*> RingAssets;
	MeshesToMerge.Reserve(Parts.Num());

	for (int32 i = 0; i < Parts.Num(); ++i)
	{
		const FFleshRingModularPart& Part = Parts[i];

		if (!Part.IsValid())
		{
			continue;
		}

		if (Part.RingAsset && Part.RingAsset->HasBakedMesh())
		{
			// Use BakedMesh (with ring deformation baked in)
			USkeletalMesh* BakedMesh = Part.RingAsset->SubdivisionSettings.BakedMesh;

			if (BakedMesh->GetSkeleton() != Skeleton)
			{
				Output.Result = EFleshRingMergeResult::SkeletonMismatch;
				Output.ErrorMessage = FString::Printf(
					TEXT("Part[%d] BakedMesh skeleton mismatch"), i);
				Output.FailedPartIndex = i;
				return Output;
			}

			MeshesToMerge.Add(BakedMesh);
			RingAssets.Add(Part.RingAsset);
		}
		else
		{
			// Use BaseMesh (no ring effect)
			if (Part.BaseMesh->GetSkeleton() != Skeleton)
			{
				Output.Result = EFleshRingMergeResult::SkeletonMismatch;
				Output.ErrorMessage = FString::Printf(
					TEXT("Part[%d] BaseMesh skeleton mismatch"), i);
				Output.FailedPartIndex = i;
				return Output;
			}

			MeshesToMerge.Add(Part.BaseMesh);
		}
	}

	if (MeshesToMerge.Num() == 0)
	{
		Output.Result = EFleshRingMergeResult::NoValidParts;
		Output.ErrorMessage = TEXT("No valid meshes to merge");
		return Output;
	}

	// 3. Merge meshes using FSkeletalMeshMerge
	USkeletalMesh* MergedMesh = NewObject<USkeletalMesh>();
	MergedMesh->SetSkeleton(Skeleton);

	TArray<FSkelMeshMergeSectionMapping> SectionMappings;
	FSkeletalMeshMerge Merger(MergedMesh, MeshesToMerge, SectionMappings, 0);

	if (!Merger.DoMerge())
	{
		Output.Result = EFleshRingMergeResult::MergeFailed;
		Output.ErrorMessage = TEXT("FSkeletalMeshMerge::DoMerge failed");
		return Output;
	}

	Output.MergedMesh = MergedMesh;

	// 4. Apply to TargetComponent + setup ring visuals
	if (TargetComponent)
	{
		// Remove existing ring visuals
		DetachAllRingVisuals(TargetComponent);

		// Apply merged mesh
		TargetComponent->SetSkeletalMeshAsset(MergedMesh);

		// Create ring visuals (BeginPlay auto-detects merged mesh mode)
		AttachRingVisuals(TargetComponent, RingAssets);
	}

	Output.Result = EFleshRingMergeResult::Success;
	return Output;
}

//==========================================================================
// Leader Pose / Copy Pose API
//==========================================================================

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

//==========================================================================
// Internal Helpers
//==========================================================================

TArray<UFleshRingComponent*> UFleshRingModularLibrary::AttachRingVisuals(
	USkeletalMeshComponent* InMergedMeshComponent,
	const TArray<UFleshRingAsset*>& InRingAssets)
{
	TArray<UFleshRingComponent*> CreatedComponents;

	if (!InMergedMeshComponent)
	{
		return CreatedComponents;
	}

	AActor* Owner = InMergedMeshComponent->GetOwner();
	if (!Owner)
	{
		return CreatedComponents;
	}

	for (UFleshRingAsset* Asset : InRingAssets)
	{
		if (!Asset)
		{
			continue;
		}

		// Create FleshRingComponent
		UFleshRingComponent* RingComp = NewObject<UFleshRingComponent>(Owner);
		if (!RingComp)
		{
			continue;
		}

		// Set FleshRingAsset
		RingComp->FleshRingAsset = Asset;

		// Set target mesh (merged SKM as target)
		RingComp->bUseCustomTarget = true;
		RingComp->CustomTargetMesh = InMergedMeshComponent;
		RingComp->SetCreatedForMergedMesh(true);  // Explicit flag for merged mesh mode

		// Register component (OnRegister -> FindTargetMeshOnly + SetupRingMeshes)
		// BeginPlay auto-detects merged mesh mode
		RingComp->RegisterComponent();

		// Add to Actor's instance component list (visible in editor during PIE)
		Owner->AddInstanceComponent(RingComp);

		CreatedComponents.Add(RingComp);
	}

	return CreatedComponents;
}

int32 UFleshRingModularLibrary::DetachAllRingVisuals(
	USkeletalMeshComponent* InMergedMeshComponent)
{
	if (!InMergedMeshComponent)
	{
		return 0;
	}

	AActor* Owner = InMergedMeshComponent->GetOwner();
	if (!Owner)
	{
		return 0;
	}

	int32 RemovedCount = 0;

	// Collect all FleshRingComponents from Owner
	TArray<UFleshRingComponent*> RingComponents;
	Owner->GetComponents<UFleshRingComponent>(RingComponents);

	// Remove only components targeting this merged mesh
	for (UFleshRingComponent* RingComp : RingComponents)
	{
		if (RingComp && RingComp->bUseCustomTarget && RingComp->CustomTargetMesh == InMergedMeshComponent)
		{
			RingComp->DestroyComponent();
			RemovedCount++;
		}
	}

	return RemovedCount;
}
