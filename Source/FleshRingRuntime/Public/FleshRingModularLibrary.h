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
	 * Skeleton is extracted from the first valid part automatically.
	 * Ring visuals are automatically set up.
	 * Invalid parts (null BaseMesh) will be excluded with warning.
	 * Parts with RingAsset but no BakedMesh will use BaseMesh with warning.
	 *
	 * @param TargetComponent Target SkeletalMeshComponent to apply result (nullptr = only create mesh)
	 * @param Parts Array of modular parts to merge (all parts must share the same skeleton)
	 * @return Merge result (contains MergedMesh on success, InvalidPartIndices and UnbakedRingPartIndices for warnings)
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Modular|Skeletal Merging",
		meta = (DisplayName = "Rebuild Merged Mesh",
			ToolTip = "Merges modular parts into a single skeletal mesh.\nSkeleton is extracted from the first valid part automatically.\nRing visuals are automatically set up for parts with BakedMesh.\n\nInvalid parts (null BaseMesh) will be excluded (check InvalidPartIndices).\nParts with RingAsset but no BakedMesh will use BaseMesh (check UnbakedRingPartIndices).",
			Keywords = "merge combine modular skeletal mesh ring"))
	static FFleshRingMergeOutput RebuildMergedMesh(
		USkeletalMeshComponent* TargetComponent,
		const TArray<FFleshRingModularPart>& Parts);

	//==========================================================================
	// Leader Pose / Copy Pose API
	//==========================================================================

	/**
	 * Swaps ring asset on a modular part at runtime.
	 * Preserves Leader Pose setup automatically.
	 * Requires BakedMesh to be present on the new asset.
	 *
	 * @param FleshRingComponent Target FleshRingComponent
	 * @param NewAsset New FleshRingAsset (nullptr = remove ring effect + restore original mesh)
	 * @return Operation result with error details on failure
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Modular|Leader Pose",
		meta = (DisplayName = "Swap Modular Ring Asset",
			ToolTip = "Swaps the Ring Asset on a Leader Pose modular part at runtime.\nRequires the new asset to have a BakedMesh.\nLeader Pose setup is preserved automatically.\n\nPass nullptr as NewAsset to remove the ring effect and restore the original mesh.",
			Keywords = "swap change replace ring modular leader pose"))
	static FFleshRingModularResult SwapModularRingAsset(
		UFleshRingComponent* FleshRingComponent,
		UFleshRingAsset* NewAsset);

	/**
	 * Swaps skeletal mesh on a modular part with ring cleanup.
	 * Detaches the ring asset effect from the target mesh before applying new mesh.
	 * Validates skeleton compatibility with Leader when Leader Pose is configured.
	 * Preserves Leader Pose setup automatically.
	 *
	 * @param SkeletalMeshComponent SkeletalMeshComponent to swap mesh on
	 * @param NewMesh New modular part mesh (must use same skeleton as Leader)
	 * @return Operation result with error details on failure
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Modular|Leader Pose",
		meta = (DisplayName = "Swap Modular Part Mesh",
			ToolTip = "Detaches the ring asset effect from the target mesh, then applies the new skeletal mesh.\nValidates skeleton compatibility when Leader Pose is configured.\n\nLeader Pose setup is preserved automatically.",
			Keywords = "swap change replace part mesh modular leader pose skeletal"))
	static FFleshRingModularResult SwapModularPartMesh(
		USkeletalMeshComponent* SkeletalMeshComponent,
		USkeletalMesh* NewMesh);

private:
	//==========================================================================
	// Internal Helpers
	//==========================================================================

	/**
	 * Creates FleshRingComponents and attaches them to target mesh.
	 * BeginPlay auto-detects merged mesh mode.
	 */
	static TArray<UFleshRingComponent*> AttachRingVisuals(
		USkeletalMeshComponent* MergedMeshComponent,
		const TArray<UFleshRingAsset*>& RingAssets);

	/**
	 * Removes all FleshRingComponents attached to target mesh.
	 */
	static int32 DetachAllRingVisuals(
		USkeletalMeshComponent* MergedMeshComponent);
};
