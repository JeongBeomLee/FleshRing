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
	/** Part mesh skeleton does not match the first part's skeleton */
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

	/** Parts excluded from merge due to IsValid() == false (null BaseMesh) */
	UPROPERTY(BlueprintReadOnly, Category = "FleshRing")
	TArray<int32> InvalidPartIndices;

	/** Parts included using BaseMesh because RingAsset has no BakedMesh */
	UPROPERTY(BlueprintReadOnly, Category = "FleshRing")
	TArray<int32> UnbakedRingPartIndices;

	/** Returns true if merge succeeded */
	bool Succeeded() const { return Result == EFleshRingMergeResult::Success; }

	/** Returns true if any parts were excluded due to invalid BaseMesh */
	bool HasInvalidParts() const { return InvalidPartIndices.Num() > 0; }

	/** Returns true if any parts used BaseMesh instead of BakedMesh */
	bool HasUnbakedRingParts() const { return UnbakedRingPartIndices.Num() > 0; }
};

/**
 * Result of modular swap operation (Leader Pose / Copy Pose)
 */
UENUM(BlueprintType)
enum class EFleshRingModularResult : uint8
{
	/** Operation completed successfully */
	Success,
	/** FleshRingComponent argument was null */
	InvalidComponent,
	/** SkeletalMeshComponent argument was null */
	InvalidMeshComponent,
	/** NewAsset has no BakedMesh (baking required before runtime swap) */
	NoBakedMesh,
	/** FleshRingComponent could not resolve its target SkeletalMeshComponent */
	TargetMeshNotResolved,
	/** BakedMesh skeleton does not match current mesh skeleton */
	SkeletonMismatch,
	/** SkeletalMeshComponent has no owning Actor */
	NoOwner,
	/** Target mesh does not match FleshRingAsset's TargetSkeletalMesh */
	MeshMismatch,
	/** Target SkeletalMeshComponent has no SkeletalMesh assigned */
	NoMeshOnTarget,
};

/**
 * Output of modular swap operations
 * @see UFleshRingModularLibrary::SwapModularRingAsset
 * @see UFleshRingModularLibrary::SwapModularPartMesh
 */
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FFleshRingModularResult
{
	GENERATED_BODY()

	/** Operation result */
	UPROPERTY(BlueprintReadOnly, Category = "FleshRing")
	EFleshRingModularResult Result = EFleshRingModularResult::Success;

	/** Error message (populated on failure) */
	UPROPERTY(BlueprintReadOnly, Category = "FleshRing")
	FString ErrorMessage;

	/** Returns true if operation succeeded */
	bool Succeeded() const { return Result == EFleshRingModularResult::Success; }
};
