// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FleshRingModularTypes.h"
#include "FleshRingModularLibrary.generated.h"

class USkeletalMesh;
class USkeletalMeshComponent;
class UFleshRingComponent;
class UFleshRingAsset;
class USkeleton;

/**
 * Unified FleshRing library for modular characters
 *
 * Supported systems:
 * - Skeletal Merging: RebuildMergedMesh()
 * - Leader Pose / Copy Pose: SwapModularRingAsset(), SwapModularPartMesh()
 */
UCLASS()
class FLESHRINGRUNTIME_API UFleshRingModularLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	//==========================================================================
	// Skeletal Merging API
	//==========================================================================

	/**
	 * Merges modular parts into a single skeletal mesh and applies to target.
	 * Ring visuals are automatically set up.
	 *
	 * @param TargetComponent Target SkeletalMeshComponent to apply result (nullptr = only create mesh)
	 * @param Parts Array of modular parts to merge
	 * @param Skeleton Shared skeleton for all parts
	 * @return Merge result (contains MergedMesh on success)
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Modular|Skeletal Merging")
	static FFleshRingMergeOutput RebuildMergedMesh(
		USkeletalMeshComponent* TargetComponent,
		const TArray<FFleshRingModularPart>& Parts,
		USkeleton* Skeleton);

	//==========================================================================
	// Leader Pose / Copy Pose API
	//==========================================================================

	/**
	 * Swaps ring asset on a modular part at runtime.
	 * Preserves Leader Pose setup automatically.
	 *
	 * @param InFleshRingComponent Target FleshRingComponent
	 * @param InNewAsset New FleshRingAsset (nullptr = remove ring effect + restore original mesh)
	 * @param bPreserveLeaderPose Whether to preserve LeaderPoseComponent setting
	 * @return true on success
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Modular|Leader Pose")
	static bool SwapModularRingAsset(
		UFleshRingComponent* InFleshRingComponent,
		UFleshRingAsset* InNewAsset,
		bool bPreserveLeaderPose = true);

	/**
	 * Swaps skeletal mesh on a modular part with ring cleanup.
	 * Detaches any attached ring assets before applying new mesh.
	 * Preserves Leader Pose setup automatically.
	 *
	 * @param InSkeletalMeshComponent SkeletalMeshComponent to swap mesh on
	 * @param InNewMesh New modular part mesh (without ring effect)
	 * @return true on success
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Modular|Leader Pose")
	static bool SwapModularPartMesh(
		USkeletalMeshComponent* InSkeletalMeshComponent,
		USkeletalMesh* InNewMesh);

private:
	//==========================================================================
	// Internal Helpers
	//==========================================================================

	/**
	 * Creates FleshRingComponents and attaches them to target mesh.
	 * BeginPlay auto-detects merged mesh mode.
	 */
	static TArray<UFleshRingComponent*> AttachRingVisuals(
		USkeletalMeshComponent* InMergedMeshComponent,
		const TArray<UFleshRingAsset*>& InRingAssets);

	/**
	 * Removes all FleshRingComponents attached to target mesh.
	 */
	static int32 DetachAllRingVisuals(
		USkeletalMeshComponent* InMergedMeshComponent);
};
