// Copyright Epic Games, Inc. All Rights Reserved.

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
 * - Leader Pose / Copy Pose: SwapModularPartWithRingCleanup()
 * - Skeletal Merging: RebuildMergedMesh()
 *
 * Future extensions:
 * - UFleshRingMergedCharacterComponent (Stateful approach)
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
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Modular")
	static FFleshRingMergeOutput RebuildMergedMesh(
		USkeletalMeshComponent* TargetComponent,
		const TArray<FFleshRingModularPart>& Parts,
		USkeleton* Skeleton);

	//==========================================================================
	// Leader Pose / Copy Pose API
	//==========================================================================

	/**
	 * Swaps modular part with ring cleanup, preserving Leader Pose setup.
	 * Detaches ring assets before applying new mesh.
	 *
	 * @param InSkeletalMeshComponent SkeletalMeshComponent to swap mesh on
	 * @param InNewMesh New modular part mesh (without ring effect)
	 * @return true on success
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Modular")
	static bool SwapModularPartWithRingCleanup(
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
