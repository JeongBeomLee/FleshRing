// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingModularLibrary.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "SkeletalMeshMerge.h"  // FSkeletalMeshMerge (Engine module)

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingModular, Log, All);

//==========================================================================
// Skeletal Merging API
//==========================================================================

FFleshRingMergeOutput UFleshRingModularLibrary::RebuildMergedMesh(
	USkeletalMeshComponent* TargetComponent,
	const TArray<FFleshRingModularPart>& Parts){
	FFleshRingMergeOutput Output;

	// 1. Validation
	if (Parts.Num() == 0)
	{
		Output.Result = EFleshRingMergeResult::NoValidParts;
		Output.ErrorMessage = TEXT("No parts provided");
		return Output;
	}

	// 2. Build mesh array and ring asset array, extract skeleton from first valid part
	TArray<USkeletalMesh*> MeshesToMerge;
	TArray<UFleshRingAsset*> RingAssets;
	MeshesToMerge.Reserve(Parts.Num());

	USkeleton* Skeleton = nullptr;  // Extracted from first valid part

	for (int32 i = 0; i < Parts.Num(); ++i)
	{
		const FFleshRingModularPart& Part = Parts[i];

		if (!Part.IsValid())
		{
			Output.InvalidPartIndices.Add(i);
			UE_LOG(LogFleshRingModular, Warning,
				TEXT("RebuildMergedMesh: Part[%d] is invalid (BaseMesh is null), skipping"),
				i);
			continue;
		}

		if (Part.RingAsset && Part.RingAsset->HasBakedMesh())
		{
			// Use BakedMesh (with ring deformation baked in)
			USkeletalMesh* BakedMesh = Part.RingAsset->SubdivisionSettings.BakedMesh;
			USkeleton* PartSkeleton = BakedMesh->GetSkeleton();

			if (!Skeleton)
			{
				Skeleton = PartSkeleton;
			}
			else if (PartSkeleton != Skeleton)
			{
				Output.Result = EFleshRingMergeResult::SkeletonMismatch;
				Output.ErrorMessage = FString::Printf(
					TEXT("Part[%d] BakedMesh skeleton '%s' does not match first part skeleton '%s'"),
					i, *PartSkeleton->GetName(), *Skeleton->GetName());
				Output.FailedPartIndex = i;
				return Output;
			}

			MeshesToMerge.Add(BakedMesh);
			RingAssets.Add(Part.RingAsset);
		}
		else
		{
			// Use BaseMesh (no ring effect)
			USkeleton* PartSkeleton = Part.BaseMesh->GetSkeleton();

			if (!Skeleton)
			{
				Skeleton = PartSkeleton;
			}
			else if (PartSkeleton != Skeleton)
			{
				Output.Result = EFleshRingMergeResult::SkeletonMismatch;
				Output.ErrorMessage = FString::Printf(
					TEXT("Part[%d] BaseMesh skeleton '%s' does not match first part skeleton '%s'"),
					i, *PartSkeleton->GetName(), *Skeleton->GetName());
				Output.FailedPartIndex = i;
				return Output;
			}

			// Track parts with RingAsset but no BakedMesh (fallback case)
			if (Part.RingAsset)
			{
				Output.UnbakedRingPartIndices.Add(i);
				UE_LOG(LogFleshRingModular, Warning,
					TEXT("RebuildMergedMesh: Part[%d] has RingAsset '%s' but no BakedMesh, using BaseMesh instead"),
					i, *Part.RingAsset->GetName());
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

FFleshRingModularResult UFleshRingModularLibrary::SwapModularRingAsset(
	UFleshRingComponent* FleshRingComponent,
	UFleshRingAsset* NewAsset)
{
	if (!FleshRingComponent)
	{
		FFleshRingModularResult Output;
		Output.Result = EFleshRingModularResult::InvalidComponent;
		Output.ErrorMessage = TEXT("FleshRingComponent is null");
		return Output;
	}

	return FleshRingComponent->Internal_SwapModularRingAsset(NewAsset, /*bPreserveLeaderPose=*/true);
}

FFleshRingModularResult UFleshRingModularLibrary::SwapModularPartMesh(
	USkeletalMeshComponent* SkeletalMeshComponent,
	USkeletalMesh* NewMesh)
{
	if (!SkeletalMeshComponent)
	{
		FFleshRingModularResult Output;
		Output.Result = EFleshRingModularResult::InvalidMeshComponent;
		Output.ErrorMessage = TEXT("SkeletalMeshComponent is null");
		return Output;
	}

	AActor* Owner = SkeletalMeshComponent->GetOwner();
	if (!Owner)
	{
		FFleshRingModularResult Output;
		Output.Result = EFleshRingModularResult::NoOwner;
		Output.ErrorMessage = TEXT("SkeletalMeshComponent has no owning Actor");
		return Output;
	}

	// Skeleton compatibility check (only when Leader Pose is configured)
	if (USkinnedMeshComponent* Leader = SkeletalMeshComponent->LeaderPoseComponent.Get())
	{
		USkeletalMesh* LeaderMesh = Cast<USkeletalMesh>(Leader->GetSkinnedAsset());
		USkeleton* LeaderSkeleton = LeaderMesh ? LeaderMesh->GetSkeleton() : nullptr;
		USkeleton* NewSkeleton = NewMesh ? NewMesh->GetSkeleton() : nullptr;

		if (LeaderSkeleton && NewSkeleton && LeaderSkeleton != NewSkeleton)
		{
			FFleshRingModularResult Output;
			Output.Result = EFleshRingModularResult::SkeletonMismatch;
			Output.ErrorMessage = FString::Printf(
				TEXT("Skeleton mismatch - Leader: '%s', NewMesh: '%s'"),
				*LeaderSkeleton->GetName(),
				*NewSkeleton->GetName());
			return Output;
		}
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

		const bool bIsTarget = (RingComp->GetResolvedTargetSkeletalMeshComponent() == SkeletalMeshComponent);

		if (bIsTarget)
		{
			// 2. Detach ring asset (SkeletalMesh unchanged)
			RingComp->Internal_DetachModularRingAsset(/*bPreserveLeaderPose=*/true);
		}
	}

	// 3. Apply new modular part
	SkeletalMeshComponent->SetSkeletalMeshAsset(NewMesh);

	FFleshRingModularResult Output;
	Output.Result = EFleshRingModularResult::Success;
	return Output;
}

//==========================================================================
// Internal Helpers
//==========================================================================

TArray<UFleshRingComponent*> UFleshRingModularLibrary::AttachRingVisuals(
	USkeletalMeshComponent* MergedMeshComponent,
	const TArray<UFleshRingAsset*>& RingAssets)
{
	TArray<UFleshRingComponent*> CreatedComponents;

	if (!MergedMeshComponent)
	{
		return CreatedComponents;
	}

	AActor* Owner = MergedMeshComponent->GetOwner();
	if (!Owner)
	{
		return CreatedComponents;
	}

	for (UFleshRingAsset* Asset : RingAssets)
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
		RingComp->SetTargetSkeletalMeshComponent(MergedMeshComponent);
		RingComp->Internal_SetCreatedForMergedMesh(true);  // Explicit flag for merged mesh mode

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
	USkeletalMeshComponent* MergedMeshComponent)
{
	if (!MergedMeshComponent)
	{
		return 0;
	}

	AActor* Owner = MergedMeshComponent->GetOwner();
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
		if (RingComp && RingComp->GetResolvedTargetSkeletalMeshComponent() == MergedMeshComponent)
		{
			RingComp->DestroyComponent();
			RemovedCount++;
		}
	}

	return RemovedCount;
}
