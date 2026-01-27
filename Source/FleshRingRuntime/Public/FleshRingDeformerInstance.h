// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include <atomic>
#include "Animation/MeshDeformerInstance.h"
#include "RenderGraphResources.h"
#include "FleshRingAffectedVertices.h"
#if WITH_EDITORONLY_DATA
#include "Animation/MeshDeformerGeometryReadback.h"
#endif
#include "FleshRingDeformerInstance.generated.h"

class UFleshRingDeformer;
class UMeshComponent;
class FMeshDeformerGeometry;
class UFleshRingComponent;

UCLASS()
class FLESHRINGRUNTIME_API UFleshRingDeformerInstance : public UMeshDeformerInstance
{
	GENERATED_BODY()

public:
	UFleshRingDeformerInstance();

	// UObject interface
	virtual void BeginDestroy() override;

	/**
	 * Initialize settings from Deformer
	 * @param InDeformer - Source Deformer
	 * @param InMeshComponent - Target MeshComponent
	 * @param InOwnerFleshRingComponent - FleshRingComponent that owns this Deformer (supports multi-component environments)
	 *                                    If nullptr, uses legacy method (FindComponentByClass)
	 */
	void SetupFromDeformer(
		UFleshRingDeformer* InDeformer,
		UMeshComponent* InMeshComponent,
		UFleshRingComponent* InOwnerFleshRingComponent = nullptr);

	// UMeshDeformerInstance interface
	virtual void AllocateResources() override;
	virtual void ReleaseResources() override;
	virtual void EnqueueWork(FEnqueueWorkDesc const& InDesc) override;
	virtual EMeshDeformerOutputBuffer GetOutputBuffers() const override;
	virtual UMeshDeformerInstance* GetInstanceForSourceDeformer() override { return this; }

	/**
	 * Invalidate TightenedBindPose cache (triggers recalculation on transform change)
	 * @param DirtyRingIndex - Invalidate only specific Ring (INDEX_NONE invalidates all)
	 */
	void InvalidateTightnessCache(int32 DirtyRingIndex = INDEX_NONE);

	/**
	 * Invalidate all caches on mesh change (for baking)
	 * Invalidates both source position cache and TightenedBindPose cache
	 * so that buffers are regenerated based on the new mesh in the next frame
	 */
	void InvalidateForMeshChange();

	/**
	 * For debug: Return per-LOD AffectedVertices data
	 * @param LODIndex - LOD index (0 = highest quality)
	 * @return Array of per-Ring Affected data for the given LOD, nullptr if not available
	 */
	const TArray<FRingAffectedData>* GetAffectedRingDataForDebug(int32 LODIndex = 0) const
	{
		if (LODData.IsValidIndex(LODIndex) && LODData[LODIndex].bAffectedVerticesRegistered)
		{
			return &LODData[LODIndex].AffectedVerticesManager.GetAllRingData();
		}
		return nullptr;
	}

	/**
	 * Check if GPU Influence Readback is complete
	 * @param LODIndex - LOD index
	 * @return true when Readback is complete
	 */
	bool IsDebugInfluenceReadbackComplete(int32 LODIndex = 0) const
	{
		if (LODData.IsValidIndex(LODIndex) &&
			LODData[LODIndex].bDebugInfluenceReadbackComplete.IsValid())
		{
			return LODData[LODIndex].bDebugInfluenceReadbackComplete->load();
		}
		return false;
	}

	/**
	 * Return GPU Influence Readback result
	 * @param LODIndex - LOD index
	 * @return Pointer to Readback Influence array, nullptr if not available
	 */
	const TArray<float>* GetDebugInfluenceReadbackResult(int32 LODIndex = 0) const
	{
		if (LODData.IsValidIndex(LODIndex) &&
			LODData[LODIndex].DebugInfluenceReadbackResult.IsValid() &&
			IsDebugInfluenceReadbackComplete(LODIndex))
		{
			return LODData[LODIndex].DebugInfluenceReadbackResult.Get();
		}
		return nullptr;
	}

	/**
	 * Reset GPU Influence Readback complete flag (for preparing next Readback)
	 * @param LODIndex - LOD index
	 */
	void ResetDebugInfluenceReadback(int32 LODIndex = 0)
	{
		if (LODData.IsValidIndex(LODIndex) &&
			LODData[LODIndex].bDebugInfluenceReadbackComplete.IsValid())
		{
			LODData[LODIndex].bDebugInfluenceReadbackComplete->store(false);
		}
	}

	/**
	 * Get cached point buffer for GPU debug rendering
	 * @param LODIndex - LOD index
	 * @return Cached DebugPointBuffer, empty pointer if not available
	 */
	TRefCountPtr<FRDGPooledBuffer> GetCachedDebugPointBuffer(int32 LODIndex = 0) const
	{
		if (LODData.IsValidIndex(LODIndex) &&
			LODData[LODIndex].CachedDebugPointBufferShared.IsValid() &&
			LODData[LODIndex].CachedDebugPointBufferShared->IsValid())
		{
			return *LODData[LODIndex].CachedDebugPointBufferShared;
		}
		return nullptr;
	}

	/**
	 * Get cached point buffer SharedPtr for GPU debug rendering
	 * @param LODIndex - LOD index
	 * @return SharedPtr of CachedDebugPointBufferShared, nullptr if not available
	 */
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> GetCachedDebugPointBufferSharedPtr(int32 LODIndex = 0) const
	{
		if (LODData.IsValidIndex(LODIndex))
		{
			return LODData[LODIndex].CachedDebugPointBufferShared;
		}
		return nullptr;
	}

	/**
	 * Get cached point buffer SharedPtr for Bulge GPU debug rendering
	 * @param LODIndex - LOD index
	 * @return SharedPtr of CachedDebugBulgePointBufferShared, nullptr if not available
	 */
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> GetCachedDebugBulgePointBufferSharedPtr(int32 LODIndex = 0) const
	{
		if (LODData.IsValidIndex(LODIndex))
		{
			return LODData[LODIndex].CachedDebugBulgePointBufferShared;
		}
		return nullptr;
	}

	/**
	 * Get Affected debug point count (value used for actual deformation)
	 * @param LODIndex - LOD index
	 * @return Total Affected vertex count
	 */
	uint32 GetTotalAffectedVertexCount(int32 LODIndex = 0) const
	{
		if (LODData.IsValidIndex(LODIndex) && LODData[LODIndex].bAffectedVerticesRegistered)
		{
			return static_cast<uint32>(LODData[LODIndex].AffectedVerticesManager.GetTotalAffectedCount());
		}
		return 0;
	}

#if WITH_EDITORONLY_DATA
	virtual bool RequestReadbackDeformerGeometry(TUniquePtr<FMeshDeformerGeometryReadbackRequest> InRequest) override { return false; }

	/**
	 * Readback GPU deformation result for baking
	 * Reads TightenedBindPose + Normals + Tangents to CPU
	 *
	 * @param OutPositions - Deformed vertex positions (float3 packed)
	 * @param OutNormals - Recalculated normals (float3 packed)
	 * @param OutTangents - Recalculated tangents (float4 packed)
	 * @param LODIndex - LOD index
	 * @return Success status
	 */
	bool ReadbackDeformedGeometry(
		TArray<FVector3f>& OutPositions,
		TArray<FVector3f>& OutNormals,
		TArray<FVector4f>& OutTangents,
		int32 LODIndex = 0);

	/**
	 * Check if TightenedBindPose is cached
	 * @param LODIndex - LOD index
	 * @return true if cached
	 */
	bool HasCachedDeformedGeometry(int32 LODIndex = 0) const;
#endif

private:
	UPROPERTY()
	TWeakObjectPtr<UFleshRingDeformer> Deformer;

	UPROPERTY()
	TWeakObjectPtr<UMeshComponent> MeshComponent;

	UPROPERTY()
	TWeakObjectPtr<UFleshRingComponent> FleshRingComponent;

	FSceneInterface* Scene = nullptr;

	// Deformed geometry output buffers
	TSharedPtr<FMeshDeformerGeometry> DeformerGeometry;

	// Track last LOD index for invalidating previous position on LOD change
	int32 LastLodIndex = INDEX_NONE;

	// ===== Per-LOD Tightness Deformation Data =====
	// Per-LOD Tightness Deformation Data
	struct FLODDeformationData
	{
		// Affected vertices manager
		FFleshRingAffectedVerticesManager AffectedVerticesManager;

		// Whether vertex registration is complete
		bool bAffectedVerticesRegistered = false;

		// Cached vertex data (for RDG upload)
		TArray<float> CachedSourcePositions;
		bool bSourcePositionsCached = false;

		// TightenedBindPose caching
		// Using TSharedPtr wrapper for thread-safe sharing with render thread
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedTightenedBindPoseShared;
		bool bTightenedBindPoseCached = false;
		uint32 CachedTightnessVertexCount = 0;

		// Recalculated normals caching (NormalRecomputeCS result)
		// Cached along with TightenedBindPose to use correct normals even in cached frames
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedNormalsShared;

		// Recalculated tangents caching (TangentRecomputeCS result)
		// Caches Gram-Schmidt orthonormalized tangents
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedTangentsShared;

		// Debug Influence caching (output from TightnessCS)
		// For visualizing GPU-computed Influence in DrawAffectedVertices
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedDebugInfluencesShared;

		// Debug point buffer caching (WorldPosition + Influence)
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedDebugPointBufferShared;

		// Bulge debug point buffer caching (WorldPosition + Influence)
		// Displayed with Cyan to Magenta color gradient
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedDebugBulgePointBufferShared;

		// ===== GPU Readback Related =====
		// Readback result storage (for thread-safe sharing)
		TSharedPtr<TArray<float>> DebugInfluenceReadbackResult;

		// Readback complete flag (thread-safe)
		TSharedPtr<std::atomic<bool>> bDebugInfluenceReadbackComplete;

		// Number of vertices to Readback
		uint32 DebugInfluenceCount = 0;
	};

	// Per-LOD data array (index = LOD number)
	TArray<FLODDeformationData> LODData;

	// Number of LODs (set during initialization)
	int32 NumLODs = 0;
};
