// Copyright 2026 LgThx. All Rights Reserved.

// Purpose: Track and manage vertices affected by each Ring for optimized processing
// [FLEXIBLE] This module uses Strategy Pattern for vertex selection
// The selection algorithm can be swapped by implementing IVertexSelector

#pragma once

#include "CoreMinimal.h"
#include "FleshRingTypes.h"

class UFleshRingComponent;
class USkeletalMeshComponent;
struct FRingSDFCache;

/**
 * Single 'affected vertex' data
 * Contains index and influence weight for deformation
 */
struct FAffectedVertex
{
    /**
     * Original mesh vertex index
     */
    uint32 VertexIndex;

    /**
     * Radial distance from Ring axis (perpendicular distance)
     */
    float RadialDistance;

    /**
     * Influence weight (0-1) based on falloff calculation
     */
    float Influence;

    /**
     * Layer type for penetration resolution (Skin, Stocking, etc.)
     * Auto-detected from material name or uses default
     */
    EFleshRingLayerType LayerType;

    FAffectedVertex()
        : VertexIndex(0)
        , RadialDistance(0.0f)
        , Influence(0.0f)
        , LayerType(EFleshRingLayerType::Other)
    {
    }

    FAffectedVertex(uint32 InIndex, float InRadialDist, float InInfluence, EFleshRingLayerType InLayerType = EFleshRingLayerType::Other)
        : VertexIndex(InIndex)
        , RadialDistance(InRadialDist)
        , Influence(InInfluence)
        , LayerType(InLayerType)
    {
    }
};

/**
 * Per-Ring affected vertex collection
 * Contains all vertices affected by a single Ring
 */
struct FRingAffectedData
{
    // =========== Ring Information (Get from Bone) ===========

    /**
     * Bone name this Ring is attached to
     */
    FName BoneName;

    /**
     * Ring center in component space (bind pose)
     */
    FVector RingCenter;

    /**
     * Ring orientation axis (normalized, bone's up vector)
     */
    FVector RingAxis;

    // =========== Ring Geometry (Copy from FFleshRingSettings) ===========

    /**
     * Inner radius from bone axis to ring inner surface
     */
    float RingRadius;

    /**
     * Ring wall thickness in radial direction (inner -> outer)
     */
    float RingThickness;

    /**
     * Ring height along axis direction (total height, +/-RingHeight/2 from center)
     */
    float RingHeight;

    // =========== Deformation Parameters (Copy from FFleshRingSettings) ===========

    /**
     * Tightness deformation strength
     */
    float TightnessStrength;

    /**
     * Falloff curve type for influence calculation
     */
    EFalloffType FalloffType;

    // =========== Affected Vertices Data ===========

    /**
     * List of affected vertices with influence weights
     */
    TArray<FAffectedVertex> Vertices;

    // Why separate flat arrays from Vertices struct array:
    // GPU cannot read struct arrays, only flat arrays
    /**
     * GPU buffer: vertex indices (packed for CS dispatch)
     */
    TArray<uint32> PackedIndices;

    /**
     * GPU buffer: influence weights (packed for CS dispatch)
     */
    TArray<float> PackedInfluences;

    /**
     * GPU buffer: layer types (packed for CS dispatch, used in penetration resolution)
     * 0=Skin, 1=Stocking, 2=Underwear, 3=Outerwear, 4=Unknown
     */
    TArray<uint32> PackedLayerTypes;

    // =========== UV Seam Welding Data ===========
    //
    // [Design]
    // Ensures vertices split at UV seams (same position, different indices) deform identically
    // Used in all deformation passes (Tightness, Bulge, BoneRatio, Laplacian)
    //
    // RepresentativeIndices[ThreadIndex] = representative vertex index for that position group
    // In shader: read representative position -> compute deformation -> write to own index

    /**
     * GPU buffer: Representative vertex index for each affected vertex
     * All UV duplicate vertices at the same position share the same representative
     * Shader reads position from representative, computes deformation, writes to own index
     */
    TArray<uint32> RepresentativeIndices;

    /**
     * Whether this region has UV duplicates (vertices with different index but same position)
     * If false, UV Sync pass can be skipped for optimization
     */
    bool bHasUVDuplicates = false;

    // =========== Unified Smoothing Region Data ===========
    //
    // [Design]
    // - Unified variables from PostProcessing~ (BoundsExpand) and Extended~ (HopBased)
    // - Filled appropriately at build time based on SmoothingExpandMode
    // - ComputeWorker uses same variables without branching
    //
    // BoundsExpand: Z-axis bounds expansion (SmoothingBoundsZTop/Bottom)
    // HopBased: Topology-based hop propagation (from Seeds to N hops)

    /**
     * Currently active smoothing expansion mode
     * Set at build time, used by ComputeWorker without branching
     */
    ESmoothingVolumeMode SmoothingExpandMode = ESmoothingVolumeMode::BoundsExpand;

    /**
     * GPU buffer: Smoothing region vertex indices (absolute mesh indices)
     * BoundsExpand: Original AABB + BoundsZTop/Bottom range
     * HopBased: Seeds (Affected Vertices) + N-hop reachable vertices
     */
    TArray<uint32> SmoothingRegionIndices;

    /**
     * GPU buffer: Smoothing region vertex influences (falloff applied)
     * BoundsExpand: 1.0 inside original AABB, falloff in Z-extended region
     * HopBased: Calculated from hop distance with falloff
     */
    TArray<float> SmoothingRegionInfluences;

    /**
     * GPU buffer: Anchor flags for smoothing region vertices
     * 1 = Seed/Core vertex (anchor, skip smoothing)
     * 0 = Extended vertex (apply smoothing)
     */
    TArray<uint32> SmoothingRegionIsAnchor;

    /**
     * GPU buffer: Representative vertex index for UV seam welding
     * All UV duplicates at same position share the same representative
     */
    TArray<uint32> SmoothingRegionRepresentativeIndices;

    /** Whether UV duplicates exist (smoothing region) */
    bool bSmoothingRegionHasUVDuplicates = false;

    /**
     * GPU buffer: Laplacian adjacency data for smoothing region
     * Format: [NeighborCount, N0, N1, ..., N11] per vertex (13 uints each)
     */
    TArray<uint32> SmoothingRegionLaplacianAdjacency;

    /**
     * GPU buffer: PBD adjacency data for smoothing region
     * Format: [Count, N0, RL0, N1, RL1, ...] per vertex (1 + MAX_NEIGHBORS*2 uints)
     */
    TArray<uint32> SmoothingRegionPBDAdjacency;

    /**
     * GPU buffer: Normal adjacency offsets for smoothing region
     * Size: NumSmoothingRegion + 1 (sentinel)
     */
    TArray<uint32> SmoothingRegionAdjacencyOffsets;

    /**
     * GPU buffer: Normal adjacency triangles for smoothing region
     */
    TArray<uint32> SmoothingRegionAdjacencyTriangles;

    // =========== Hop Distance Data (HopBased Mode) ===========
    /**
     * Hop distance for each vertex in smoothing region (HopBased only)
     * Used for normal blending at boundaries
     * 0 = Seed (original affected vertex), 1+ = hop distance from nearest seed
     * Empty in BoundsExpand mode
     */
    TArray<int32> SmoothingRegionHopDistances;

    /**
     * Maximum hop distance (for blend factor calculation)
     */
    int32 MaxSmoothingHops = 0;

    // =========== Skin SDF Layer Separation Data ===========

    /**
     * Skin vertex indices (within smoothing region, LayerType=Skin)
     */
    TArray<uint32> SkinVertexIndices;

    /**
     * Skin vertex normals (radial direction from ring axis)
     * Packed as: [N0.x, N0.y, N0.z, N1.x, N1.y, N1.z, ...]
     */
    TArray<float> SkinVertexNormals;

    /**
     * Stocking vertex indices (within smoothing region, LayerType=Stocking)
     */
    TArray<uint32> StockingVertexIndices;

    // =========== Adjacency Data for Normal Recomputation ===========

    /**
     * GPU buffer: Adjacency offsets for each affected vertex
     * AdjacencyOffsets[i] = start index in AdjacencyTriangles for affected vertex i
     * AdjacencyOffsets[NumAffectedVertices] = total size of AdjacencyTriangles (sentinel)
     * Range from AdjacencyOffsets[i] to AdjacencyOffsets[i+1] is the adjacent triangles for that vertex
     */
    TArray<uint32> AdjacencyOffsets;

    /**
     * GPU buffer: Flattened list of adjacent triangle indices
     */
    TArray<uint32> AdjacencyTriangles;

    // =========== Laplacian Smoothing Adjacency Data ===========

    /**
     * GPU buffer: Packed adjacency data for Laplacian smoothing
     * Format: [NeighborCount, N0, N1, ..., N11] per affected vertex (13 uints each)
     */
    TArray<uint32> LaplacianAdjacencyData;

    // =========== Bone Ratio Preserve Data ===========

    /** Maximum vertices per slice (for GPU buffer sizing) */
    static constexpr int32 MAX_SLICE_VERTICES = 32;
    /** Packed size per vertex: [Count, V0, V1, ..., V31] = 33 uints */
    static constexpr int32 SLICE_PACKED_SIZE = 1 + MAX_SLICE_VERTICES;

    /**
     * Original bone distance for each affected vertex (bind pose)
     */
    TArray<float> OriginalBoneDistances;

    /**
     * GPU buffer: Packed slice data for bone ratio preservation
     * Format: [SliceVertexCount, V0, V1, ..., V31] per affected vertex (33 uints each)
     * V0~V31 are ThreadIndices (not VertexIndices) of same-slice vertices
     */
    TArray<uint32> SlicePackedData;

    /**
     * GPU buffer: Axis height for each affected vertex
     * Used for Gaussian weighted averaging (smooth transitions)
     */
    TArray<float> AxisHeights;

    // =========== PBD Edge Constraint Data (for deformation propagation) ===========

    /** Maximum neighbors per vertex for PBD (must match shader) */
    static constexpr int32 PBD_MAX_NEIGHBORS = 12;
    /** Packed size per vertex: [Count, (Neighbor, RestLen)*12] = 1 + 24 = 25 uints */
    static constexpr int32 PBD_ADJACENCY_PACKED_SIZE = 1 + PBD_MAX_NEIGHBORS * 2;

    /**
     * GPU buffer: Packed adjacency data with rest lengths for PBD
     * Format: [NeighborCount, Neighbor0, RestLen0, Neighbor1, RestLen1, ...] per affected vertex
     * RestLength is stored as bit-cast uint (use asfloat in shader)
     */
    TArray<uint32> PBDAdjacencyWithRestLengths;

    /**
     * GPU buffer: Full mesh influence map (for neighbor weight lookup)
     * Index: absolute vertex index, Value: influence
     * Vertices not in affected set have 0 influence
     */
    TArray<float> FullInfluenceMap;

    /**
     * GPU buffer: Full mesh deform amount map (for neighbor weight lookup)
     * Index: absolute vertex index, Value: deform amount
     * Used when bPBDUseDeformAmountWeight is true
     */
    TArray<float> FullDeformAmountMap;

    /**
     * GPU buffer: Full mesh IsAnchor map (for PBD neighbor weight lookup)
     * Index: absolute vertex index, Value: 1 = Affected/Anchor, 0 = Non-Affected/Free
     * Used by Tolerance-based PBD to determine neighbor's anchor status for weight distribution
     */
    TArray<uint32> FullVertexAnchorFlags;

    // =========== Hop-Based Smoothing Data ===========

    /**
     * GPU buffer: Hop distance from nearest seed for each affected vertex
     * -1 = not reached (outside smoothing range)
     * 0 = seed vertex (inside SDF, will be deformed)
     * 1+ = hop distance from nearest seed
     */
    TArray<int32> HopDistances;

    /**
     * Hop-based influence (calculated from HopDistances)
     * Replaces PackedInfluences when SmoothingVolumeMode == HopBased
     */
    TArray<float> HopBasedInfluences;

    /**
     * ThreadIndices of seed vertices (vertices inside SDF that will be deformed)
     * Used as starting points for BFS hop propagation
     */
    TArray<int32> SeedThreadIndices;

    // Note: Extended~ variables are unified into SmoothingRegion~ (see above)

    FRingAffectedData()
        : BoneName(NAME_None)
        , RingCenter(FVector::ZeroVector)
        , RingAxis(FVector::UpVector)
        , RingRadius(5.0f)
        , RingThickness(1.0f)
        , RingHeight(2.0f)
        , TightnessStrength(1.0f)
        , FalloffType(EFalloffType::Linear)
    {
    }

    /** Pack vertex data into GPU-friendly buffers (flat arrays) */
    void PackForGPU()
    {
        PackedIndices.Reset(Vertices.Num());
        PackedInfluences.Reset(Vertices.Num());
        PackedLayerTypes.Reset(Vertices.Num());

        for (const FAffectedVertex& Vert : Vertices)
        {
            PackedIndices.Add(Vert.VertexIndex);
            PackedInfluences.Add(Vert.Influence);
            PackedLayerTypes.Add(static_cast<uint32>(Vert.LayerType));
        }
    }
};

// ============================================================================
// FVertexSpatialHash - Vertex Spatial Hash (O(1) query)
// ============================================================================
// Stores vertices in 3D grid cells to optimize AABB queries to O(1)
// 10~100x performance improvement vs brute force O(n)

class FLESHRINGRUNTIME_API FVertexSpatialHash
{
public:
    FVertexSpatialHash() : CellSize(5.0f), InvCellSize(0.2f) {}

    /**
     * Build spatial hash from vertex array
     * @param Vertices - Bind pose vertices in component space
     * @param InCellSize - Grid cell size (default 5.0 cm)
     */
    void Build(const TArray<FVector3f>& Vertices, float InCellSize = 5.0f);

    /**
     * Query vertices within AABB
     * @param Min - AABB minimum corner
     * @param Max - AABB maximum corner
     * @param OutIndices - Output vertex indices
     */
    void QueryAABB(const FVector& Min, const FVector& Max, TArray<int32>& OutIndices) const;

    /**
     * Query vertices within OBB (converts to AABB internally, then precise check)
     * @param LocalToWorld - OBB local to world transform
     * @param LocalMin - OBB local minimum corner
     * @param LocalMax - OBB local maximum corner
     * @param OutIndices - Output vertex indices (only those inside OBB)
     */
    void QueryOBB(const FTransform& LocalToWorld, const FVector& LocalMin, const FVector& LocalMax, TArray<int32>& OutIndices) const;

    /** Check if hash is built */
    bool IsBuilt() const { return CellMap.Num() > 0; }

    /** Clear all data */
    void Clear() { CellMap.Empty(); CachedVertices.Empty(); }

private:
    /** Convert world position to cell key */
    FIntVector GetCellKey(const FVector& Position) const
    {
        return FIntVector(
            FMath::FloorToInt(Position.X * InvCellSize),
            FMath::FloorToInt(Position.Y * InvCellSize),
            FMath::FloorToInt(Position.Z * InvCellSize)
        );
    }

    /** Convert FIntVector to hash key */
    uint64 HashCellKey(const FIntVector& Key) const
    {
        // Combine X, Y, Z into a single hash (21 bits each for reasonable range)
        return (static_cast<uint64>(Key.X & 0x1FFFFF)) |
               (static_cast<uint64>(Key.Y & 0x1FFFFF) << 21) |
               (static_cast<uint64>(Key.Z & 0x1FFFFF) << 42);
    }

    float CellSize;
    float InvCellSize;
    TMap<uint64, TArray<int32>> CellMap;  // Cell hash -> vertex indices
    TArray<FVector3f> CachedVertices;     // Cached vertex positions
};

// ============================================================================
// FVertexSelectionContext - Vertex Selection Context
// ============================================================================
// Context containing data needed by all selection strategies
// Each Selector uses only what it needs. Add fields for extensions.

struct FVertexSelectionContext
{
    // ===== Core (Always Present) =====

    /** Ring settings from asset */
    const FFleshRingSettings& RingSettings;

    /** Ring index in the asset's Rings array */
    int32 RingIndex;

    /** Bone transform in bind pose component space */
    const FTransform& BoneTransform;

    /** All mesh vertices in bind pose component space */
    const TArray<FVector3f>& AllVertices;

    // ===== SDF Data (Optional - nullptr if not available) =====

    /** SDF cache for this Ring */
    const FRingSDFCache* SDFCache;

    // ===== Spatial Hash (Optional - nullptr for brute force fallback) =====

    /** Spatial hash for O(1) vertex query */
    const FVertexSpatialHash* SpatialHash;

    // ===== Position-to-Vertices Cache (Optional - nullptr for fallback to local build) =====

    /**
     * Cached position-to-vertices map for UV seam welding
     * If nullptr, builds local map in SelectVertices (fallback, slower)
     */
    const TMap<FIntVector, TArray<uint32>>* CachedPositionToVertices;

    // ===== Layer Data (Optional - nullptr if layer filtering disabled) =====

    /**
     * Per-vertex layer types for filtering (full mesh size array)
     * If nullptr, layer filtering disabled (selects all vertices)
     */
    const TArray<EFleshRingLayerType>* VertexLayerTypes;

    FVertexSelectionContext(
        const FFleshRingSettings& InRingSettings,
        int32 InRingIndex,
        const FTransform& InBoneTransform,
        const TArray<FVector3f>& InAllVertices,
        const FRingSDFCache* InSDFCache = nullptr,
        const FVertexSpatialHash* InSpatialHash = nullptr,
        const TMap<FIntVector, TArray<uint32>>* InCachedPositionToVertices = nullptr,
        const TArray<EFleshRingLayerType>* InVertexLayerTypes = nullptr)
        : RingSettings(InRingSettings)
        , RingIndex(InRingIndex)
        , BoneTransform(InBoneTransform)
        , AllVertices(InAllVertices)
        , SDFCache(InSDFCache)
        , SpatialHash(InSpatialHash)
        , CachedPositionToVertices(InCachedPositionToVertices)
        , VertexLayerTypes(InVertexLayerTypes)
    {
    }
};

// ============================================================================
// IVertexSelector - Vertex Selection Strategy Interface (Strategy Pattern)
// ============================================================================

/**
 * Interface for vertex selection strategies
 */
class IVertexSelector
{
public:
    virtual ~IVertexSelector() = default;

    /**
     * Select vertices affected by a Ring
     * Context provides all data - each selector uses what it needs
     *
     * @param Context - All selection-related data (Ring, Bone, Vertices, SDF, etc.)
     * @param OutAffected - Output: selected vertices with influence
     */
    virtual void SelectVertices(
        const FVertexSelectionContext& Context,
        TArray<FAffectedVertex>& OutAffected) = 0;

    /** Strategy name for debugging */
    virtual FString GetStrategyName() const = 0;
};

// ============================================================================
// FDistanceBasedVertexSelector - Distance-based selection (default)
// ============================================================================
// Uses: Context.RingSettings, Context.BoneTransform, Context.AllVertices
// Ignores: Context.SDFCache

class FDistanceBasedVertexSelector : public IVertexSelector
{
public:
    virtual void SelectVertices(
        const FVertexSelectionContext& Context,
        TArray<FAffectedVertex>& OutAffected) override;

    virtual FString GetStrategyName() const override
    {
        return TEXT("DistanceBased");
    }

    /**
     * Select post-processing vertices for VirtualRing mode (Z-extended range)
     *
     * Selects post-processing vertices based on Ring parameters (RingCenter, RingAxis, RingHeight).
     * Calculates directly in Component Space without SDF.
     *
     * @param Context - Vertex selection context
     * @param AffectedVertices - Already selected affected vertices
     * @param OutRingData - Output: fills SmoothingRegionIndices, SmoothingRegionInfluences
     */
    void SelectSmoothingRegionVertices(
        const FVertexSelectionContext& Context,
        const TArray<FAffectedVertex>& AffectedVertices,
        FRingAffectedData& OutRingData);

protected:
    float CalculateFalloff(float Distance, float MaxDistance, EFalloffType InFalloffType) const;
};

// ============================================================================
// FSDFBoundsBasedVertexSelector - SDF Bounds-based selection
// ============================================================================
// Uses: Context.SDFCache (BoundsMin, BoundsMax), Context.AllVertices
// Ignores: Context.RingSettings geometry, Context.BoneTransform
//
// Design: Select all vertices within SDF bounding box.
// GPU shader determines actual influence via SDF sampling.
// If SDFCache is nullptr or invalid, selects nothing.

class FSDFBoundsBasedVertexSelector : public IVertexSelector
{
public:
    virtual void SelectVertices(
        const FVertexSelectionContext& Context,
        TArray<FAffectedVertex>& OutAffected) override;

    virtual FString GetStrategyName() const override
    {
        return TEXT("SDFBoundsBased");
    }

    /**
     * Select post-processing vertices (Z-extended range)
     *
     * Selects vertices in the Z-extended range for post-processing passes.
     * Core vertices (inside original AABB) get influence 1.0.
     * Extended vertices get falloff-based influence.
     *
     * @param Context - Vertex selection context with SDF cache
     * @param AffectedVertices - Already selected affected vertices (for quick lookup)
     * @param OutRingData - Output: fills SmoothingRegionIndices, SmoothingRegionInfluences
     */
    void SelectSmoothingRegionVertices(
        const FVertexSelectionContext& Context,
        const TArray<FAffectedVertex>& AffectedVertices,
        FRingAffectedData& OutRingData);
};

// ============================================================================
// FVirtualBandVertexSelector - Virtual Band mode selection
// ============================================================================
// Uses: Context.RingSettings.VirtualBand (4 radii, 3 heights)
//
// Design: Select Tightness vertices with variable 4-radius shape without SDF
// Differences from VirtualRing Mode:
// - Uses GetRadiusAtHeight() for variable radius instead of fixed radius
// - Only Band Section (middle) applies Tightness, Upper/Lower are for Bulge

class FVirtualBandVertexSelector : public IVertexSelector
{
public:
    virtual void SelectVertices(
        const FVertexSelectionContext& Context,
        TArray<FAffectedVertex>& OutAffected) override;

    virtual FString GetStrategyName() const override
    {
        return TEXT("VirtualBandBased");
    }

    /**
     * Select post-processing vertices for Virtual Band mode (Z-extended range)
     *
     * Selects vertices in full Virtual Band height + BoundsZTop/Bottom extended range.
     * Uses same logic pattern as VirtualRing Mode.
     *
     * @param Context - Vertex selection context
     * @param AffectedVertices - Already selected affected vertices
     * @param OutRingData - Output: fills SmoothingRegionIndices, SmoothingRegionInfluences
     */
    void SelectSmoothingRegionVertices(
        const FVertexSelectionContext& Context,
        const TArray<FAffectedVertex>& AffectedVertices,
        FRingAffectedData& OutRingData);

private:
    /**
     * Calculate band radius at given height (variable radius)
     *
     * Lower Section: LowerRadius -> MidLowerRadius
     * Band Section: MidLowerRadius -> MidUpperRadius
     * Upper Section: MidUpperRadius -> UpperRadius
     */
    float GetRadiusAtHeight(float LocalZ, const struct FVirtualBandSettings& BandSettings) const;

    /**
     * Calculate falloff influence based on distance
     */
    float CalculateFalloff(float Distance, float MaxDistance, EFalloffType InFalloffType) const;
};

// Affected Vertices Manager
// Central manager for affected vertex registration and updates
class FLESHRINGRUNTIME_API FFleshRingAffectedVerticesManager
{
public:
    FFleshRingAffectedVerticesManager();
    ~FFleshRingAffectedVerticesManager();

    /**
     * Set the vertex selector strategy
     * [FLEXIBLE] Can swap selection algorithm at runtime
     *
     * @param InSelector - New vertex selector to use
     */
    void SetVertexSelector(TSharedPtr<IVertexSelector> InSelector);

    /**
     * Get current vertex selector
     */
    TSharedPtr<IVertexSelector> GetVertexSelector() const { return VertexSelector; }

    /**
     * Register affected vertices for all Rings in a component
     *
     * @param Component - FleshRingComponent with Ring settings
     * @param SkeletalMesh - Target skeletal mesh
     * @param LODIndex - LOD index to use for vertex extraction (default: 0)
     * @return true if registration succeeded
     */
    bool RegisterAffectedVertices(
        const UFleshRingComponent* Component,
        const USkeletalMeshComponent* SkeletalMesh,
        int32 LODIndex = 0);

    /**
     * Get affected data for a specific Ring by index
     */
    const FRingAffectedData* GetRingData(int32 RingIndex) const;

    /**
     * Get all Ring affected data
     */
    const TArray<FRingAffectedData>& GetAllRingData() const { return RingDataArray; }

    /**
     * Get the Spatial Hash for O(1) vertex queries
     * Used by Bulge calculation for performance optimization
     */
    const FVertexSpatialHash& GetSpatialHash() const { return VertexSpatialHash; }

    /**
     * Clear all registered data
     */
    void ClearAll();

    /**
     * Get total affected vertex count across all Rings
     */
    int32 GetTotalAffectedCount() const;

    // ===== Per-Ring Dirty Flag System (prevents unnecessary rebuilds) =====

    /**
     * Mark a specific ring as dirty (needs rebuild)
     */
    void MarkRingDirty(int32 RingIndex);

    /**
     * Mark all rings as dirty
     */
    void MarkAllRingsDirty();

    /**
     * Check if a ring is dirty
     */
    bool IsRingDirty(int32 RingIndex) const;

    // ===== Topology Cache Public API =====

    /**
     * Build topology cache from mesh data (call once per mesh)
     *
     * Builds position groups, neighbor maps, and welded neighbor data.
     * This is O(V*T) but only runs once per mesh binding.
     *
     * @param AllVertices - All mesh vertices in bind pose
     * @param MeshIndices - Mesh index buffer (3 indices per triangle)
     */
    void BuildTopologyCache(
        const TArray<FVector3f>& AllVertices,
        const TArray<uint32>& MeshIndices);

    /**
     * Invalidate topology cache (call when mesh changes)
     */
    void InvalidateTopologyCache();

    /**
     * Rebuild vertex layer types from MaterialLayerMappings
     * Called every RegisterAffectedVertices to reflect MaterialLayerMappings changes
     * @param Component - FleshRingComponent with asset settings
     * @param SkeletalMesh - Source skeletal mesh component
     * @param LODIndex - LOD index to use
     */
    void RebuildVertexLayerTypes(const UFleshRingComponent* Component, const USkeletalMeshComponent* SkeletalMesh, int32 LODIndex);

    /**
     * Check if topology cache is built
     */
    bool IsTopologyCacheBuilt() const { return bTopologyCacheBuilt; }

    /**
     * Get cached mesh indices for Normal recomputation
     */
    const TArray<uint32>& GetCachedMeshIndices() const { return CachedMeshIndices; }

    /**
     * Get cached position-to-vertices map for UV seam welding
     * Used by SDFBoundsBasedSelector to avoid O(N) rebuild every frame
     */
    const TMap<FIntVector, TArray<uint32>>& GetCachedPositionToVertices() const { return CachedPositionToVertices; }

    /**
     * Get cached vertex layer types for GPU upload
     * Full mesh size array - can be directly uploaded to GPU
     */
    const TArray<EFleshRingLayerType>& GetCachedVertexLayerTypes() const { return CachedVertexLayerTypes; }

private:
    /**
     * Current vertex selector strategy
     */
    TSharedPtr<IVertexSelector> VertexSelector;

    /**
     * Per-Ring affected data array
     */
    TArray<FRingAffectedData> RingDataArray;

    /**
     * Cached mesh indices for Normal recomputation (shared by all Rings)
     */
    TArray<uint32> CachedMeshIndices;

    /**
     * Cached mesh vertices (bind pose, immutable)
     */
    TArray<FVector3f> CachedMeshVertices;

    /**
     * Cached vertex layer types (material-based, immutable)
     */
    TArray<EFleshRingLayerType> CachedVertexLayerTypes;

    /**
     * Flag indicating mesh data is cached (skip re-extraction on update)
     */
    bool bMeshDataCached = false;

    /**
     * Spatial hash for O(1) vertex query (replaces brute force O(n))
     */
    FVertexSpatialHash VertexSpatialHash;

    /**
     * Per-Ring dirty flags (true = needs rebuild)
     */
    TArray<bool> RingDirtyFlags;

    // ===== Topology Cache (Immutable after first build) =====
    // Mesh topology (vertex adjacency, UV seam welding info) is determined at bind pose
    // and doesn't change at runtime. Build once and cache for performance optimization.

    /**
     * Flag indicating topology cache is built
     */
    bool bTopologyCacheBuilt = false;

    /**
     * Position-based vertex grouping for UV seam welding
     * Key: quantized position (FIntVector), Value: vertex indices at that position
     */
    TMap<FIntVector, TArray<uint32>> CachedPositionToVertices;

    /**
     * Reverse lookup: vertex index to quantized position
     */
    TMap<uint32, FIntVector> CachedVertexToPosition;

    /**
     * Per-vertex neighbor map (direct mesh connectivity)
     * Key: vertex index, Value: set of neighbor vertex indices
     */
    TMap<uint32, TSet<uint32>> CachedVertexNeighbors;

    /**
     * Position-based welded neighbor map (UV seam aware)
     * Key: quantized position, Value: set of neighbor positions (welded)
     */
    TMap<FIntVector, TSet<FIntVector>> CachedWeldedNeighborPositions;

    /**
     * Full mesh adjacency map for BFS/hop distance calculation
     * Key: vertex index, Value: neighbor vertex indices
     */
    TMap<uint32, TArray<uint32>> CachedFullAdjacencyMap;

    /**
     * Per-vertex triangle list for fast adjacency lookup
     * Key: vertex index, Value: list of triangle indices containing this vertex
     * Used for O(T) -> O(avg_triangles_per_vertex) optimization in BuildAdjacencyData()
     */
    TMap<uint32, TArray<uint32>> CachedVertexTriangles;

    /**
     * Position -> Representative vertex (smallest index at that position)
     * For UV seam welding - the vertex with smallest index at same position is representative
     * Optimizes BuildRepresentativeIndices() from O(A) map build to O(1) cache lookup
     */
    TMap<FIntVector, uint32> CachedPositionToRepresentative;

    /**
     * Extract vertices from skeletal mesh at specific LOD (bind pose component space)
     */
    bool ExtractMeshVertices(
        const USkeletalMeshComponent* SkeletalMesh,
        TArray<FVector3f>& OutVertices,
        int32 LODIndex = 0);

    /**
     * Extract index buffer from skeletal mesh at specific LOD
     */
    bool ExtractMeshIndices(
        const USkeletalMeshComponent* SkeletalMesh,
        TArray<uint32>& OutIndices,
        int32 LODIndex = 0);

    /**
     * Build adjacency data for a single Ring
     *
     * @param RingData - Ring data with Vertices already populated
     * @param MeshIndices - Mesh index buffer (3 indices per triangle)
     */
    void BuildAdjacencyData(
        FRingAffectedData& RingData,
        const TArray<uint32>& MeshIndices);

    /**
     * Build Laplacian adjacency data for smoothing with UV seam welding
     *
     * Builds per-vertex neighbor lists from mesh indices.
     * Uses position-based welding to ensure vertices at UV seams get consistent neighbors.
     * This prevents cracks at UV seams during Laplacian smoothing.
     *
     * Output format: [NeighborCount, N0, N1, ..., N11] per affected vertex (13 uints)
     *
     * @param RingData - Ring data with Vertices already populated
     * @param MeshIndices - Mesh index buffer (3 indices per triangle)
     * @param AllVertices - All mesh vertex positions (for position-based welding)
     * @param VertexLayerTypes - Per-vertex layer types (for same-layer filtering)
     */
    void BuildLaplacianAdjacencyData(
        FRingAffectedData& RingData,
        const TArray<uint32>& MeshIndices,
        const TArray<FVector3f>& AllVertices,
        const TArray<EFleshRingLayerType>& VertexLayerTypes);

    /**
     * Build Laplacian adjacency data for post-processing vertices (Z-extended range)
     *
     * Similar to BuildLaplacianAdjacencyData but for SmoothingRegionIndices.
     *
     * @param RingData - Ring data with SmoothingRegionIndices already populated
     * @param MeshIndices - Mesh index buffer (3 indices per triangle)
     * @param AllVertices - All mesh vertex positions (for position-based welding)
     * @param VertexLayerTypes - Per-vertex layer types (for same-layer filtering)
     */
    void BuildSmoothingRegionLaplacianAdjacency(
        FRingAffectedData& RingData,
        const TArray<uint32>& MeshIndices,
        const TArray<FVector3f>& AllVertices,
        const TArray<EFleshRingLayerType>& VertexLayerTypes);

    /**
     * Build slice data for bone ratio preservation
     *
     * Groups vertices by their height along the Ring axis (bucket-based).
     * Same-height vertices should have same bone distance ratio after deformation.
     *
     * Output:
     * - RingData.OriginalBoneDistances: bind pose bone distance per affected vertex
     * - RingData.SlicePackedData: [Count, V0, V1, ..., V31] per affected vertex
     *
     * @param RingData - Ring data with Vertices already populated
     * @param AllVertices - All mesh vertices in bind pose component space
     * @param BucketSize - Height bucket size for slice grouping (default: 1.0 cm)
     */
    void BuildSliceData(
        FRingAffectedData& RingData,
        const TArray<FVector3f>& AllVertices,
        float BucketSize = 1.0f);

    /**
     * Build PBD adjacency data with rest lengths for edge constraint
     *
     * Builds per-vertex neighbor lists with rest lengths from mesh indices.
     * Rest length = bind pose distance between vertices.
     * Also builds full mesh influence/deform maps for neighbor weight lookup.
     *
     * Output:
     * - RingData.PBDAdjacencyWithRestLengths: [Count, N0, RL0, N1, RL1, ...] per vertex
     * - RingData.FullInfluenceMap: influence for all mesh vertices
     * - RingData.FullDeformAmountMap: deform amount for all mesh vertices
     *
     * @param RingData - Ring data with Vertices already populated
     * @param MeshIndices - Mesh index buffer (3 indices per triangle)
     * @param AllVertices - All mesh vertices in bind pose component space
     * @param TotalVertexCount - Total number of vertices in mesh
     */
    void BuildPBDAdjacencyData(
        FRingAffectedData& RingData,
        const TArray<uint32>& MeshIndices,
        const TArray<FVector3f>& AllVertices,
        int32 TotalVertexCount);

    /**
     * Build PBD adjacency data for Smoothing Region vertices
     *
     * Same as BuildPBDAdjacencyData but for SmoothingRegionIndices range.
     *
     * Output:
     * - RingData.SmoothingRegionPBDAdjacency: [Count, N0, RL0, N1, RL1, ...] per vertex
     *
     * @param RingData - Ring data with SmoothingRegionIndices already populated
     * @param MeshIndices - Mesh index buffer (3 indices per triangle)
     * @param AllVertices - All mesh vertices in bind pose component space
     * @param TotalVertexCount - Total number of vertices in mesh
     */
    void BuildSmoothingRegionPBDAdjacency(
        FRingAffectedData& RingData,
        const TArray<uint32>& MeshIndices,
        const TArray<FVector3f>& AllVertices,
        int32 TotalVertexCount);

    /**
     * Build PBD adjacency data for Extended smoothing region (HopBased mode)
     *
     * Similar to BuildSmoothingRegionPBDAdjacency but for SmoothingRegionIndices.
     *
     * Output:
     * - RingData.SmoothingRegionPBDAdjacency: [Count, N0, RL0, N1, RL1, ...] per vertex
     *
     * @param RingData - Ring data with SmoothingRegionIndices already populated
     * @param AllVertices - All mesh vertices in bind pose component space
     */
    void BuildSmoothingRegionPBDAdjacency_HopBased(
        FRingAffectedData& RingData,
        const TArray<FVector3f>& AllVertices);

    /**
     * Build adjacency data for Smoothing Region vertices (Normal recomputation)
     *
     * Same as BuildAdjacencyData but for SmoothingRegionIndices range.
     *
     * Output:
     * - RingData.SmoothingRegionAdjacencyOffsets: [NumSmoothingRegion+1] offsets
     * - RingData.SmoothingRegionAdjacencyTriangles: flattened triangle indices
     *
     * @param RingData - Ring data with SmoothingRegionIndices already populated
     * @param MeshIndices - Mesh index buffer (3 indices per triangle)
     */
    void BuildSmoothingRegionNormalAdjacency(
        FRingAffectedData& RingData,
        const TArray<uint32>& MeshIndices);

    /**
     * Build hop distance data for topology-based smoothing
     *
     * Algorithm:
     * 1. Seeds = All affected vertices (SDF sampled)
     * 2. BFS on full mesh from seeds
     * 3. Collect all vertices within MaxHops
     * 4. Build extended smoothing region with influence falloff
     *
     * Output:
     * - RingData.SmoothingRegionIndices: all vertices in smoothing region
     * - RingData.SmoothingRegionHopDistances: hop distance (0=seed, 1+=hop)
     * - RingData.SmoothingRegionInfluences: influence based on hop distance
     * - RingData.SmoothingRegionLaplacianAdjacency: adjacency for smoothing region
     *
     * @param RingData - Ring data with Vertices already populated
     * @param MeshIndices - Full mesh index buffer (3 indices per triangle)
     * @param AllVertices - All mesh vertices in bind pose component space
     * @param MaxHops - Maximum hop distance from seeds
     * @param FalloffType - Type of falloff curve for influence calculation
     */
    void BuildHopDistanceData(
        FRingAffectedData& RingData,
        const TArray<uint32>& MeshIndices,
        const TArray<FVector3f>& AllVertices,
        int32 MaxHops,
        EFalloffType FalloffType);

    /**
     * Build full mesh adjacency map (vertex -> neighbors)
     *
     * @param MeshIndices - Full mesh index buffer
     * @param NumVertices - Total vertex count
     * @param OutAdjacencyMap - Output: vertex index -> neighbor vertex indices
     */
    void BuildFullMeshAdjacency(
        const TArray<uint32>& MeshIndices,
        int32 NumVertices,
        TMap<uint32, TArray<uint32>>& OutAdjacencyMap);

    /**
     * Build Laplacian adjacency for extended smoothing region
     *
     * [Modified] Uses same welding logic as BuildSmoothingRegionLaplacianAdjacency
     * Previously: CachedFullAdjacencyMap (no UV welding)
     * Now: CachedWeldedNeighborPositions (UV welding applied)
     *
     * @param RingData - Ring data with SmoothingRegionIndices populated
     * @param VertexLayerTypes - Per-vertex layer types (for same-layer filtering)
     */
    void BuildSmoothingRegionLaplacianAdjacency_HopBased(
        FRingAffectedData& RingData,
        const TArray<EFleshRingLayerType>& VertexLayerTypes);

    /**
     * Build representative indices for UV seam welding
     *
     * All vertices at the same 3D position (UV duplicates) will share the same representative.
     * Shaders read position from representative, compute deformation, write to own index.
     * This ensures UV duplicates always move identically, preventing cracks at UV seams.
     *
     * Output:
     * - RingData.RepresentativeIndices: [NumAffectedVertices] representative for each affected vertex
     * - RingData.SmoothingRegionRepresentativeIndices: [NumSmoothingRegion] representative for each smoothing region vertex
     *
     * @param RingData - Ring data with Vertices and SmoothingRegionIndices already populated
     * @param AllVertices - All mesh vertex positions (for position-based grouping)
     */
    void BuildRepresentativeIndices(
        FRingAffectedData& RingData,
        const TArray<FVector3f>& AllVertices);
};
