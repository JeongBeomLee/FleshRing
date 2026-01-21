// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "FleshRingModularLibrary.generated.h"

class USkeletalMesh;
class USkeletalMeshComponent;
class UFleshRingComponent;

/**
 * Utility library for managing FleshRing effects in modular character systems
 * Supports both Leader Pose/Copy Pose and Skeletal Merging approaches
 */
UCLASS()
class FLESHRINGRUNTIME_API UFleshRingModularLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	// =============================================
	// Leader Pose / Copy Pose
	// =============================================

	/**
	 * Swap modular part and detach ring asset
	 * Use when replacing a modular part while removing ring
	 * e.g., Soft thigh (with ring) -> Muscular thigh (no ring)
	 *
	 * Internal flow:
	 * 1. Find FleshRingComponent targeting the given SkeletalMeshComponent
	 * 2. Call DetachRingAsset() (removes ring mesh, keeps SkeletalMesh)
	 * 3. Apply new modular part via SetSkeletalMeshAsset()
	 *
	 * @param InSkeletalMeshComponent - SkeletalMeshComponent to swap mesh on
	 * @param InNewMesh - New modular part to apply (original mesh without ring effect)
	 * @return Success or failure
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Modular")
	static bool SwapModularPartWithRingCleanup(
		USkeletalMeshComponent* InSkeletalMeshComponent,
		USkeletalMesh* InNewMesh);
};
