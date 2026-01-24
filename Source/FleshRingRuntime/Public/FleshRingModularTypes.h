// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FleshRingModularTypes.generated.h"

class USkeletalMesh;
class UFleshRingAsset;

/**
 * Result of skeletal mesh merge operation
 */
UENUM(BlueprintType)
enum class EFleshRingMergeResult : uint8
{
	Success,
	NoValidParts,
	SkeletonMismatch,
	BakingFailed,
	MergeFailed
};

/**
 * Single modular part unit (mesh + optional ring pair)
 *
 * Usage:
 *   FFleshRingModularPart Part;
 *   Part.BaseMesh = SK_LeftThigh;
 *   Part.RingAsset = DA_ThighRing;  // nullptr = no ring effect
 */
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FFleshRingModularPart
{
	GENERATED_BODY()

	/**
	 * Skeletal mesh for this part.
	 * Used when RingAsset is null or has no BakedMesh.
	 * Ignored if RingAsset has a valid BakedMesh.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FleshRing")
	TObjectPtr<USkeletalMesh> BaseMesh = nullptr;

	/**
	 * Optional ring effect asset.
	 * If set and has BakedMesh, uses BakedMesh instead of BaseMesh.
	 * If null, BaseMesh is used as-is (no ring effect).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FleshRing")
	TObjectPtr<UFleshRingAsset> RingAsset = nullptr;

	/** Returns true if this part has a valid mesh to merge */
	bool IsValid() const { return BaseMesh != nullptr; }
};

/**
 * Output of RebuildMergedMesh operation
 */
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FFleshRingMergeOutput
{
	GENERATED_BODY()

	/** Merge operation result */
	UPROPERTY(BlueprintReadOnly, Category = "FleshRing")
	EFleshRingMergeResult Result = EFleshRingMergeResult::Success;

	/** Generated merged mesh (valid on success) */
	UPROPERTY(BlueprintReadOnly, Category = "FleshRing")
	TObjectPtr<USkeletalMesh> MergedMesh = nullptr;

	/** Error message (populated on failure) */
	UPROPERTY(BlueprintReadOnly, Category = "FleshRing")
	FString ErrorMessage;

	/** Index of the part that caused failure (-1 = general failure or success) */
	UPROPERTY(BlueprintReadOnly, Category = "FleshRing")
	int32 FailedPartIndex = INDEX_NONE;

	/** Returns true if merge succeeded */
	bool Succeeded() const { return Result == EFleshRingMergeResult::Success; }
};
