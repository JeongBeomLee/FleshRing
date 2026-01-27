// Copyright 2026 LgThx. All Rights Reserved.

// FleshRingSubdivisionProcessor.h
// CPU-side subdivision topology processor using Red-Green Refinement / LEB
// GPU handles only final vertex interpolation

#pragma once

#include "CoreMinimal.h"
#include "HalfEdgeMesh.h"

// Forward declarations
class USkeletalMesh;
struct FReferenceSkeleton;

/**
 * Per-vertex bone influence info
 * Data extracted from SkinWeightVertexBuffer
 */
struct FVertexBoneInfluence
{
	static constexpr int32 MAX_INFLUENCES = 8;

	uint16 BoneIndices[MAX_INFLUENCES] = {0};
	uint8 BoneWeights[MAX_INFLUENCES] = {0};  // 0-255 normalized

	/** Check if significantly affected by a specific bone set */
	bool IsAffectedByBones(const TSet<int32>& TargetBones, uint8 WeightThreshold = 25) const  // 25 ≈ 10%
	{
		for (int32 i = 0; i < MAX_INFLUENCES; ++i)
		{
			if (BoneWeights[i] >= WeightThreshold && TargetBones.Contains(BoneIndices[i]))
			{
				return true;
			}
		}
		return false;
	}
};

/**
 * Bone region-based Subdivision parameters
 * For editor preview - only subdivide neighbor bone region of ring-attached bone
 */
struct FBoneRegionSubdivisionParams
{
	/** Target bone indices (ring-attached bone + neighbor bones) */
	TSet<int32> TargetBoneIndices;

	/** Bone weight threshold (0-255, default 25 ≈ 10%) */
	uint8 BoneWeightThreshold = 25;

	/** Neighbor bone search depth (1 = parent+child, 2 = includes grandparent+grandchild) */
	int32 NeighborHopCount = 1;

	/** Maximum subdivision level */
	int32 MaxSubdivisionLevel = 2;

	/** Parameter hash (for cache invalidation) */
	uint32 GetHash() const
	{
		uint32 Hash = 0;
		for (int32 BoneIdx : TargetBoneIndices)
		{
			Hash = HashCombine(Hash, GetTypeHash(BoneIdx));
		}
		Hash = HashCombine(Hash, GetTypeHash(BoneWeightThreshold));
		Hash = HashCombine(Hash, GetTypeHash(NeighborHopCount));
		Hash = HashCombine(Hash, GetTypeHash(MaxSubdivisionLevel));
		return Hash;
	}
};

/**
 * New vertex creation info (passed to GPU)
 * Contains all info needed for barycentric interpolation
 */
struct FSubdivisionVertexData
{
	// Parent vertex indices (based on original mesh)
	// Edge midpoint: uses only ParentV0, ParentV1 (ParentV2 == ParentV0)
	// Face interior: uses all 3
	uint32 ParentV0 = 0;
	uint32 ParentV1 = 0;
	uint32 ParentV2 = 0;

	// Barycentric coordinates (u + v + w = 1)
	// Edge midpoint: (0.5, 0.5, 0)
	// Face center: (0.333, 0.333, 0.333)
	FVector3f BarycentricCoords = FVector3f(1.0f, 0.0f, 0.0f);

	// Check if copying original vertex as-is
	bool IsOriginalVertex() const
	{
		return BarycentricCoords.X >= 0.999f && ParentV0 == ParentV1 && ParentV1 == ParentV2;
	}

	// Check if edge midpoint
	bool IsEdgeMidpoint() const
	{
		return FMath::IsNearlyEqual(BarycentricCoords.X, 0.5f) &&
			   FMath::IsNearlyEqual(BarycentricCoords.Y, 0.5f) &&
			   FMath::IsNearlyEqual(BarycentricCoords.Z, 0.0f);
	}

	// Constructor for original vertex
	static FSubdivisionVertexData CreateOriginal(uint32 OriginalIndex)
	{
		FSubdivisionVertexData Data;
		Data.ParentV0 = OriginalIndex;
		Data.ParentV1 = OriginalIndex;
		Data.ParentV2 = OriginalIndex;
		Data.BarycentricCoords = FVector3f(1.0f, 0.0f, 0.0f);
		return Data;
	}

	// Constructor for edge midpoint
	static FSubdivisionVertexData CreateEdgeMidpoint(uint32 V0, uint32 V1)
	{
		FSubdivisionVertexData Data;
		Data.ParentV0 = V0;
		Data.ParentV1 = V1;
		Data.ParentV2 = V0; // unused but set for consistency
		Data.BarycentricCoords = FVector3f(0.5f, 0.5f, 0.0f);
		return Data;
	}

	// Constructor for face center
	static FSubdivisionVertexData CreateFaceCenter(uint32 V0, uint32 V1, uint32 V2)
	{
		FSubdivisionVertexData Data;
		Data.ParentV0 = V0;
		Data.ParentV1 = V1;
		Data.ParentV2 = V2;
		Data.BarycentricCoords = FVector3f(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f);
		return Data;
	}

	// Constructor for arbitrary barycentric coordinates
	static FSubdivisionVertexData CreateBarycentric(uint32 V0, uint32 V1, uint32 V2, const FVector3f& Bary)
	{
		FSubdivisionVertexData Data;
		Data.ParentV0 = V0;
		Data.ParentV1 = V1;
		Data.ParentV2 = V2;
		Data.BarycentricCoords = Bary;
		return Data;
	}
};

/**
 * Subdivision result (for CPU -> GPU transfer)
 */
struct FSubdivisionTopologyResult
{
	// New vertex creation info array
	TArray<FSubdivisionVertexData> VertexData;

	// Final triangle indices (based on new vertex indices)
	TArray<uint32> Indices;

	// Per-triangle material index (for section tracking)
	TArray<int32> TriangleMaterialIndices;

	// Statistics
	uint32 OriginalVertexCount = 0;
	uint32 OriginalTriangleCount = 0;
	uint32 SubdividedVertexCount = 0;
	uint32 SubdividedTriangleCount = 0;

	void Reset()
	{
		VertexData.Empty();
		Indices.Empty();
		TriangleMaterialIndices.Empty();
		OriginalVertexCount = 0;
		OriginalTriangleCount = 0;
		SubdividedVertexCount = 0;
		SubdividedTriangleCount = 0;
	}

	bool IsValid() const
	{
		return VertexData.Num() > 0 && Indices.Num() > 0;
	}
};

/**
 * Ring influence parameters
 */
struct FSubdivisionRingParams
{
	/** SDF-based mode (true) vs manual geometry mode (false) */
	bool bUseSDFBounds = false;

	// =====================================
	// VirtualRing mode parameters
	// =====================================
	FVector Center = FVector::ZeroVector;
	FVector Axis = FVector::UpVector;
	float Radius = 10.0f;
	float Width = 5.0f;
	float InfluenceMultiplier = 2.0f;  // Influence range multiplier based on Width

	// =====================================
	// SDF mode parameters (OBB bounds)
	// =====================================
	/** SDF volume minimum bounds (Ring local space) */
	FVector SDFBoundsMin = FVector::ZeroVector;

	/** SDF volume maximum bounds (Ring local space) */
	FVector SDFBoundsMax = FVector::ZeroVector;

	/** Ring local -> Component space transform (OBB) */
	FTransform SDFLocalToComponent = FTransform::Identity;

	/** SDF influence range expansion multiplier */
	float SDFInfluenceMultiplier = 1.5f;

	// Get influence radius (VirtualRing mode)
	float GetInfluenceRadius() const
	{
		return Width * InfluenceMultiplier;
	}

	// SDF bounds-based influence check (whether vertex is within influence range)
	bool IsVertexInSDFInfluence(const FVector& VertexPosition) const
	{
		if (!bUseSDFBounds)
		{
			return false;
		}

		// Transform from component space to local space
		FVector LocalPos = SDFLocalToComponent.InverseTransformPosition(VertexPosition);

		// Calculate expanded bounds
		FVector ExpandedMin = SDFBoundsMin * SDFInfluenceMultiplier;
		FVector ExpandedMax = SDFBoundsMax * SDFInfluenceMultiplier;

		// Check if within bounds
		return LocalPos.X >= ExpandedMin.X && LocalPos.X <= ExpandedMax.X &&
			   LocalPos.Y >= ExpandedMin.Y && LocalPos.Y <= ExpandedMax.Y &&
			   LocalPos.Z >= ExpandedMin.Z && LocalPos.Z <= ExpandedMax.Z;
	}
};

/**
 * Subdivision processor settings
 */
struct FSubdivisionProcessorSettings
{
	// LEB maximum level
	int32 MaxSubdivisionLevel = 4;

	// Minimum edge length (stop subdivision if smaller than this)
	float MinEdgeLength = 1.0f;

	// Subdivision mode
	enum class EMode : uint8
	{
		// Calculate once at Bind Pose, cache
		BindPoseFixed,

		// Async recalculation on Ring change
		DynamicAsync,

		// Pre-subdivide wide region
		PreSubdivideRegion
	};
	EMode Mode = EMode::BindPoseFixed;

	// PreSubdivideRegion mode: additional radius to pre-subdivide
	float PreSubdivideMargin = 50.0f;
};

/**
 * CPU-based Subdivision topology processor
 *
 * Uses existing FHalfEdgeMesh and FLEBSubdivision to perform
 * Red-Green Refinement based crack-free adaptive subdivision
 *
 * GPU handles only final vertex interpolation
 */
class FLESHRINGRUNTIME_API FFleshRingSubdivisionProcessor
{
public:
	FFleshRingSubdivisionProcessor();
	~FFleshRingSubdivisionProcessor();

	/**
	 * Set source mesh data
	 *
	 * @param InPositions - Vertex position array
	 * @param InIndices - Triangle index array
	 * @param InUVs - UV coordinate array (optional)
	 * @param InMaterialIndices - Per-triangle material index (optional)
	 * @return Success status
	 */
	bool SetSourceMesh(
		const TArray<FVector>& InPositions,
		const TArray<uint32>& InIndices,
		const TArray<FVector2D>& InUVs = TArray<FVector2D>(),
		const TArray<int32>& InMaterialIndices = TArray<int32>());

	/**
	 * Extract source mesh from SkeletalMesh LOD
	 *
	 * @param SkeletalMesh - Source skeletal mesh
	 * @param LODIndex - LOD index
	 * @return Success status
	 */
	bool SetSourceMeshFromSkeletalMesh(
		class USkeletalMesh* SkeletalMesh,
		int32 LODIndex = 0);

	/**
	 * Set Ring parameters array (replaces existing parameters)
	 *
	 * @param InRingParamsArray - Ring influence parameters array
	 */
	void SetRingParamsArray(const TArray<FSubdivisionRingParams>& InRingParamsArray);

	/**
	 * Add Ring parameters
	 *
	 * @param RingParams - Ring influence parameters to add
	 */
	void AddRingParams(const FSubdivisionRingParams& RingParams);

	/**
	 * Clear Ring parameters
	 */
	void ClearRingParams();

	/**
	 * Set target vertex indices for subdivision (vertex-based mode)
	 *
	 * When this function is called, performs subdivision based on vertex set instead of Ring parameters
	 * Triangles containing these vertices become subdivision targets
	 *
	 * @param InTargetVertexIndices - Vertex index set for subdivision targets
	 */
	void SetTargetVertexIndices(const TSet<uint32>& InTargetVertexIndices);

	/**
	 * Check if vertex-based mode is enabled
	 */
	bool IsVertexBasedMode() const { return bUseVertexBasedMode; }

	/**
	 * Disable vertex-based mode (return to Ring parameters mode)
	 */
	void ClearTargetVertexIndices();

	/**
	 * Set target triangle indices for subdivision (triangle-based mode)
	 *
	 * When this function is called, performs subdivision based on triangle set instead of Ring parameters or vertex set
	 * Used after converting AffectedVertices positions extracted from DI to triangles
	 *
	 * @param InTargetTriangleIndices - Triangle index set for subdivision targets
	 */
	void SetTargetTriangleIndices(const TSet<int32>& InTargetTriangleIndices);

	/**
	 * Check if triangle-based mode is enabled
	 */
	bool IsTriangleBasedMode() const { return bUseTriangleBasedMode; }

	/**
	 * Disable triangle-based mode
	 */
	void ClearTargetTriangleIndices();

	/**
	 * Set single Ring parameters (backward compatibility - clears existing and adds)
	 *
	 * @param RingParams - Ring influence parameters
	 */
	void SetRingParams(const FSubdivisionRingParams& RingParams);

	/**
	 * Set processor settings
	 *
	 * @param Settings - Processor settings
	 */
	void SetSettings(const FSubdivisionProcessorSettings& Settings);

	/**
	 * Execute Subdivision (synchronous)
	 *
	 * Build Half-Edge -> Apply LEB/Red-Green -> Generate topology result
	 * Ring region-based partial subdivision (for runtime)
	 *
	 * @param OutResult - Output topology result
	 * @return Success status
	 */
	bool Process(FSubdivisionTopologyResult& OutResult);

	/**
	 * Execute uniform Subdivision (for editor preview)
	 *
	 * Uniformly subdivide entire mesh without Ring region check
	 * Used for real-time preview when editing rings in editor
	 * (ProcessBoneRegion recommended for performance)
	 *
	 * @param OutResult - Output topology result
	 * @param MaxLevel - Maximum subdivision level (default 2)
	 * @return Success status
	 */
	bool ProcessUniform(FSubdivisionTopologyResult& OutResult, int32 MaxLevel = 2);

	/**
	 * Execute bone region-based Subdivision (for editor preview - optimized)
	 *
	 * Only subdivide vertex region affected by neighbor bones of ring-attached bone
	 * 70-85% vertex count reduction compared to entire mesh
	 *
	 * @param OutResult - Output topology result
	 * @param Params - Bone region parameters
	 * @return Success status
	 */
	bool ProcessBoneRegion(FSubdivisionTopologyResult& OutResult, const FBoneRegionSubdivisionParams& Params);

	// =====================================
	// Bone info related (for editor preview)
	// =====================================

	/**
	 * Extract source mesh with bone info from SkeletalMesh
	 *
	 * @param SkeletalMesh - Source skeletal mesh
	 * @param LODIndex - LOD index
	 * @return Success status
	 */
	bool SetSourceMeshWithBoneInfo(USkeletalMesh* SkeletalMesh, int32 LODIndex = 0);

	/**
	 * Gather neighbor bone set of ring-attached bones
	 *
	 * @param RefSkeleton - Skeleton reference
	 * @param RingBoneIndices - Ring-attached bone index array
	 * @param HopCount - Search depth (1 = parent+child)
	 * @return Neighbor bone index set
	 */
	static TSet<int32> GatherNeighborBones(
		const FReferenceSkeleton& RefSkeleton,
		const TArray<int32>& RingBoneIndices,
		int32 HopCount = 1);

	/**
	 * Check bone region cache validity
	 */
	bool IsBoneRegionCacheValid() const { return bBoneRegionCacheValid; }

	/**
	 * Invalidate bone region cache
	 */
	void InvalidateBoneRegionCache() { bBoneRegionCacheValid = false; }

	/**
	 * Set per-vertex bone influence info directly (to avoid duplicate extraction)
	 *
	 * Reuses bone info already extracted from GeneratePreviewMesh()
	 * Use SetSourceMesh() + SetVertexBoneInfluences() combination instead of SetSourceMeshWithBoneInfo()
	 *
	 * @param InInfluences - Per-vertex bone influence info array
	 */
	void SetVertexBoneInfluences(const TArray<FVertexBoneInfluence>& InInfluences);

	/**
	 * Check if bone info is loaded
	 */
	bool HasBoneInfo() const { return VertexBoneInfluences.Num() > 0; }

	/**
	 * Get cached result
	 */
	const FSubdivisionTopologyResult& GetCachedResult() const { return CachedResult; }

	/**
	 * Check cache validity
	 */
	bool IsCacheValid() const { return bCacheValid; }

	/**
	 * Invalidate cache
	 * Also clears HalfEdgeMesh data to release memory
	 */
	void InvalidateCache();

	/**
	 * Source mesh data accessors (for GPU upload)
	 */
	const TArray<FVector>& GetSourcePositions() const { return SourcePositions; }
	const TArray<uint32>& GetSourceIndices() const { return SourceIndices; }
	const TArray<FVector2D>& GetSourceUVs() const { return SourceUVs; }

	/**
	 * Check if Ring position has changed sufficiently
	 *
	 * @param NewRingParams - New Ring parameters
	 * @param Threshold - Change threshold
	 * @return Whether recomputation is needed
	 */
	bool NeedsRecomputation(const FSubdivisionRingParams& NewRingParams, float Threshold = 5.0f) const;

private:
	// Half-Edge mesh structure
	FHalfEdgeMesh HalfEdgeMesh;

	// Source mesh data
	TArray<FVector> SourcePositions;
	TArray<uint32> SourceIndices;
	TArray<FVector2D> SourceUVs;
	TArray<int32> SourceMaterialIndices;  // Per-triangle material index

	// Ring parameters array (supports multiple Rings)
	TArray<FSubdivisionRingParams> RingParamsArray;

	// Vertex-based mode data
	TSet<uint32> TargetVertexIndices;
	bool bUseVertexBasedMode = false;

	// Triangle-based mode data
	TSet<int32> TargetTriangleIndices;
	bool bUseTriangleBasedMode = false;

	// Settings
	FSubdivisionProcessorSettings CurrentSettings;

	// Cache (for runtime - Process())
	FSubdivisionTopologyResult CachedResult;
	bool bCacheValid = false;
	TArray<FSubdivisionRingParams> CachedRingParamsArray;

	// Bone region cache (for editor preview - ProcessBoneRegion())
	FSubdivisionTopologyResult BoneRegionCachedResult;
	bool bBoneRegionCacheValid = false;
	uint32 CachedBoneRegionParamsHash = 0;

	// Per-vertex bone influence info (extracted from SetSourceMeshWithBoneInfo)
	TArray<FVertexBoneInfluence> VertexBoneInfluences;

	// Extract topology result from Half-Edge mesh
	bool ExtractTopologyResult(FSubdivisionTopologyResult& OutResult);

	// Check if triangle is within target bone region
	bool IsTriangleInBoneRegion(int32 V0, int32 V1, int32 V2, const TSet<int32>& TargetBones, uint8 WeightThreshold) const;

	// Original vertex index -> New vertex index mapping
	TMap<uint32, uint32> OriginalToNewVertexMap;

	// Edge midpoint cache (Edge as Key, new vertex index as Value)
	TMap<TPair<uint32, uint32>, uint32> EdgeMidpointCache;

	// Generate normalized edge key (ensures V0 < V1)
	static TPair<uint32, uint32> MakeEdgeKey(uint32 V0, uint32 V1)
	{
		return V0 < V1 ? TPair<uint32, uint32>(V0, V1) : TPair<uint32, uint32>(V1, V0);
	}
};
