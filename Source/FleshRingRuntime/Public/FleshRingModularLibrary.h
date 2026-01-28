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
	 * Requires BakedMesh to be present on the new asset.
	 *
	 * @param InFleshRingComponent Target FleshRingComponent
	 * @param InNewAsset New FleshRingAsset (nullptr = remove ring effect + restore original mesh)
	 * @return Operation result with error details on failure
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Modular|Leader Pose",
		meta = (DisplayName = "Swap Modular Ring Asset",
			ToolTip = "Swaps the Ring Asset on a Leader Pose modular part at runtime.\nRequires the new asset to have a BakedMesh.\nLeader Pose setup is preserved automatically.\n\nPass nullptr as NewAsset to remove the ring effect and restore the original mesh.",
			Keywords = "swap change replace ring modular leader pose"))
	static FFleshRingModularResult SwapModularRingAsset(
		UFleshRingComponent* InFleshRingComponent,
		UFleshRingAsset* InNewAsset);

	/**
	 * Swaps skeletal mesh on a modular part with ring cleanup.
	 * Detaches the ring asset effect from the target mesh before applying new mesh.
	 * Validates skeleton compatibility with Leader when Leader Pose is configured.
	 * Preserves Leader Pose setup automatically.
	 *
	 * @param InSkeletalMeshComponent SkeletalMeshComponent to swap mesh on
	 * @param InNewMesh New modular part mesh (must use same skeleton as Leader)
	 * @return Operation result with error details on failure
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Modular|Leader Pose",
		meta = (DisplayName = "Swap Modular Part Mesh",
			ToolTip = "Detaches the ring asset effect from the target mesh, then applies the new skeletal mesh.\nValidates skeleton compatibility when Leader Pose is configured.\n\nLeader Pose setup is preserved automatically.",
			Keywords = "swap change replace part mesh modular leader pose skeletal"))
	static FFleshRingModularResult SwapModularPartMesh(
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
