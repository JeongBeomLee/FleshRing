// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Mesh Adjacency Builder
// ============================================================================
// Purpose: Build vertex adjacency data from mesh topology for Laplacian smoothing
// Creates neighbor lists for each vertex from triangle index buffer

#pragma once

#include "CoreMinimal.h"

// Maximum neighbors per vertex (must match shader FLESHRING_MAX_NEIGHBORS)
#define FLESHRING_MAX_NEIGHBORS 12

/**
 * Mesh adjacency data builder for Laplacian smoothing
 * Builds neighbor information from triangle indices
 */
class FLESHRINGRUNTIME_API FMeshAdjacencyBuilder
{
public:
    FMeshAdjacencyBuilder() = default;

    /**
     * Build adjacency data from triangle indices
     *
     * @param NumVertices - Total number of vertices in the mesh
     * @param TriangleIndices - Triangle index buffer (3 indices per triangle)
     * @return true if successful
     */
    bool BuildFromTriangles(int32 NumVertices, const TArray<uint32>& TriangleIndices);

    /**
     * Build adjacency data from skeletal mesh render data
     *
     * @param SkeletalMesh - Source skeletal mesh component
     * @param LODIndex - LOD level to use
     * @return true if successful
     */
    bool BuildFromSkeletalMesh(const class USkeletalMeshComponent* SkeletalMesh, int32 LODIndex = 0);

    /**
     * Get packed adjacency data for a subset of vertices (AffectedVertices only)
     *
     * Layout per affected vertex: [NeighborCount, N0, N1, ..., N11] (13 uints)
     * Total size: NumAffectedVertices * 13 uints
     *
     * Important: The adjacency is indexed by ThreadIndex (0 to NumAffected-1),
     * but the neighbor indices are actual mesh VertexIndices.
     *
     * @param AffectedIndices - Indices of affected vertices (from AffectedVerticesManager)
     * @param OutPackedData - Output: Packed adjacency data for GPU upload
     */
    void GetPackedDataForAffectedVertices(
        const TArray<uint32>& AffectedIndices,
        TArray<uint32>& OutPackedData) const;

    /**
     * Get packed adjacency data WITH rest lengths for PBD edge constraints
     *
     * Layout per affected vertex: [Count, N0, RestLen0, N1, RestLen1, ..., N11, RestLen11] (25 uints)
     * RestLength is stored as reinterpreted float bits (asfloat in shader)
     * Total size: NumAffectedVertices * 25 uints
     *
     * @param AffectedIndices - Indices of affected vertices
     * @param BindPosePositions - Bind pose vertex positions (for rest length calculation)
     * @param OutPackedData - Output: Packed adjacency + rest length data for GPU
     */
    void GetPackedDataWithRestLengths(
        const TArray<uint32>& AffectedIndices,
        const TArray<FVector3f>& BindPosePositions,
        TArray<uint32>& OutPackedData) const;

    /**
     * Get packed adjacency data for ALL mesh vertices
     * Useful for debugging or full-mesh smoothing
     *
     * @param OutPackedData - Output: Packed adjacency data (NumVertices * 13 uints)
     */
    void GetPackedDataForAllVertices(TArray<uint32>& OutPackedData) const;

    /** Get neighbor count for a specific vertex */
    int32 GetNeighborCount(int32 VertexIndex) const;

    /** Get neighbor list for a specific vertex (nullptr if invalid) */
    const TArray<uint32>* GetNeighbors(int32 VertexIndex) const;

    /** Get total vertex count */
    int32 GetNumVertices() const { return VertexNeighbors.Num(); }

    /** Check if adjacency data is built */
    bool IsBuilt() const { return VertexNeighbors.Num() > 0; }

    /** Clear all adjacency data */
    void Clear();

    /** Debug: Print adjacency stats */
    void PrintStats() const;

private:
    /**
     * Per-vertex neighbor lists
     * Index: VertexIndex (0 to NumVertices-1)
     * Value: Array of neighbor vertex indices
     */
    TArray<TArray<uint32>> VertexNeighbors;
};
