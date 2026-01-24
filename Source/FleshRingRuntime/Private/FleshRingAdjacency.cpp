// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Mesh Adjacency Builder - Implementation
// ============================================================================

#include "FleshRingAdjacency.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingAdjacency, Log, All);

// ============================================================================
// BuildFromTriangles
// ============================================================================

bool FMeshAdjacencyBuilder::BuildFromTriangles(int32 NumVertices, const TArray<uint32>& TriangleIndices)
{
    Clear();

    if (NumVertices <= 0)
    {
        UE_LOG(LogFleshRingAdjacency, Warning, TEXT("BuildFromTriangles: Invalid vertex count %d"), NumVertices);
        return false;
    }

    if (TriangleIndices.Num() < 3 || TriangleIndices.Num() % 3 != 0)
    {
        UE_LOG(LogFleshRingAdjacency, Warning, TEXT("BuildFromTriangles: Invalid triangle count %d"), TriangleIndices.Num());
        return false;
    }

    // Initialize neighbor arrays
    VertexNeighbors.SetNum(NumVertices);
    for (TArray<uint32>& Neighbors : VertexNeighbors)
    {
        Neighbors.Reserve(FLESHRING_MAX_NEIGHBORS);
    }

    // Build adjacency from triangles
    // For each triangle, each vertex is a neighbor of the other two
    const int32 NumTriangles = TriangleIndices.Num() / 3;

    for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
    {
        const int32 BaseIdx = TriIdx * 3;
        const uint32 V0 = TriangleIndices[BaseIdx + 0];
        const uint32 V1 = TriangleIndices[BaseIdx + 1];
        const uint32 V2 = TriangleIndices[BaseIdx + 2];

        // Validate indices
        if (V0 >= (uint32)NumVertices || V1 >= (uint32)NumVertices || V2 >= (uint32)NumVertices)
        {
            UE_LOG(LogFleshRingAdjacency, Warning,
                TEXT("BuildFromTriangles: Invalid vertex index in triangle %d (%d, %d, %d)"),
                TriIdx, V0, V1, V2);
            continue;
        }

        // Add neighbors (avoid duplicates using AddUnique)
        // V0 <-> V1, V0 <-> V2
        VertexNeighbors[V0].AddUnique(V1);
        VertexNeighbors[V0].AddUnique(V2);

        // V1 <-> V0, V1 <-> V2
        VertexNeighbors[V1].AddUnique(V0);
        VertexNeighbors[V1].AddUnique(V2);

        // V2 <-> V0, V2 <-> V1
        VertexNeighbors[V2].AddUnique(V0);
        VertexNeighbors[V2].AddUnique(V1);
    }

    // Trim neighbor arrays to max size (shouldn't happen in normal meshes)
    int32 TrimmedCount = 0;
    for (TArray<uint32>& Neighbors : VertexNeighbors)
    {
        if (Neighbors.Num() > FLESHRING_MAX_NEIGHBORS)
        {
            Neighbors.SetNum(FLESHRING_MAX_NEIGHBORS);
            TrimmedCount++;
        }
    }

    if (TrimmedCount > 0)
    {
        UE_LOG(LogFleshRingAdjacency, Warning,
            TEXT("BuildFromTriangles: %d vertices had more than %d neighbors (trimmed)"),
            TrimmedCount, FLESHRING_MAX_NEIGHBORS);
    }

    UE_LOG(LogFleshRingAdjacency, Log,
        TEXT("BuildFromTriangles: Built adjacency for %d vertices from %d triangles"),
        NumVertices, NumTriangles);

    return true;
}

// ============================================================================
// BuildFromSkeletalMesh
// ============================================================================

bool FMeshAdjacencyBuilder::BuildFromSkeletalMesh(const USkeletalMeshComponent* SkeletalMesh, int32 LODIndex)
{
    if (!SkeletalMesh)
    {
        UE_LOG(LogFleshRingAdjacency, Warning, TEXT("BuildFromSkeletalMesh: SkeletalMesh is null"));
        return false;
    }

    USkeletalMesh* MeshAsset = SkeletalMesh->GetSkeletalMeshAsset();
    if (!MeshAsset)
    {
        UE_LOG(LogFleshRingAdjacency, Warning, TEXT("BuildFromSkeletalMesh: Mesh asset is null"));
        return false;
    }

    const FSkeletalMeshRenderData* RenderData = MeshAsset->GetResourceForRendering();
    if (!RenderData || RenderData->LODRenderData.Num() == 0)
    {
        UE_LOG(LogFleshRingAdjacency, Warning, TEXT("BuildFromSkeletalMesh: No render data"));
        return false;
    }

    // Validate LOD index
    if (LODIndex < 0 || LODIndex >= RenderData->LODRenderData.Num())
    {
        UE_LOG(LogFleshRingAdjacency, Warning,
            TEXT("BuildFromSkeletalMesh: Invalid LOD %d, using LOD 0"), LODIndex);
        LODIndex = 0;
    }

    const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];
    const int32 NumVertices = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

    // Extract triangle indices from all sections
    TArray<uint32> TriangleIndices;

    // Get multi-size index buffer
    const FRawStaticIndexBuffer16or32Interface* IndexBuffer = LODData.MultiSizeIndexContainer.GetIndexBuffer();
    if (!IndexBuffer)
    {
        UE_LOG(LogFleshRingAdjacency, Warning, TEXT("BuildFromSkeletalMesh: No index buffer"));
        return false;
    }

    // Copy all indices
    const int32 NumIndices = IndexBuffer->Num();
    TriangleIndices.Reserve(NumIndices);

    for (int32 i = 0; i < NumIndices; ++i)
    {
        TriangleIndices.Add(IndexBuffer->Get(i));
    }

    UE_LOG(LogFleshRingAdjacency, Log,
        TEXT("BuildFromSkeletalMesh: Extracted %d vertices, %d indices from LOD %d"),
        NumVertices, NumIndices, LODIndex);

    return BuildFromTriangles(NumVertices, TriangleIndices);
}

// ============================================================================
// GetPackedDataForAffectedVertices
// ============================================================================

void FMeshAdjacencyBuilder::GetPackedDataForAffectedVertices(
    const TArray<uint32>& AffectedIndices,
    TArray<uint32>& OutPackedData) const
{
    OutPackedData.Reset();

    if (!IsBuilt())
    {
        UE_LOG(LogFleshRingAdjacency, Warning,
            TEXT("GetPackedDataForAffectedVertices: Adjacency not built"));
        return;
    }

    // Each affected vertex: 1 count + 12 neighbor indices = 13 uints
    const int32 PackedSizePerVertex = 1 + FLESHRING_MAX_NEIGHBORS;
    OutPackedData.Reserve(AffectedIndices.Num() * PackedSizePerVertex);

    for (int32 ThreadIdx = 0; ThreadIdx < AffectedIndices.Num(); ++ThreadIdx)
    {
        const uint32 VertexIndex = AffectedIndices[ThreadIdx];

        if (VertexIndex < (uint32)VertexNeighbors.Num())
        {
            const TArray<uint32>& Neighbors = VertexNeighbors[VertexIndex];
            const uint32 NeighborCount = FMath::Min(Neighbors.Num(), FLESHRING_MAX_NEIGHBORS);

            // Pack: [Count, N0, N1, ..., N11]
            OutPackedData.Add(NeighborCount);

            for (int32 i = 0; i < FLESHRING_MAX_NEIGHBORS; ++i)
            {
                if (i < (int32)NeighborCount)
                {
                    OutPackedData.Add(Neighbors[i]);
                }
                else
                {
                    OutPackedData.Add(0); // Padding
                }
            }
        }
        else
        {
            // Invalid vertex index - add empty adjacency
            OutPackedData.Add(0); // Count = 0
            for (int32 i = 0; i < FLESHRING_MAX_NEIGHBORS; ++i)
            {
                OutPackedData.Add(0);
            }
        }
    }

    UE_LOG(LogFleshRingAdjacency, Verbose,
        TEXT("GetPackedDataForAffectedVertices: Packed %d vertices (%d uints)"),
        AffectedIndices.Num(), OutPackedData.Num());
}

// ============================================================================
// GetPackedDataWithRestLengths (for PBD Edge Constraint)
// ============================================================================

void FMeshAdjacencyBuilder::GetPackedDataWithRestLengths(
    const TArray<uint32>& AffectedIndices,
    const TArray<FVector3f>& BindPosePositions,
    TArray<uint32>& OutPackedData) const
{
    OutPackedData.Reset();

    if (!IsBuilt())
    {
        UE_LOG(LogFleshRingAdjacency, Warning,
            TEXT("GetPackedDataWithRestLengths: Adjacency not built"));
        return;
    }

    if (BindPosePositions.Num() == 0)
    {
        UE_LOG(LogFleshRingAdjacency, Warning,
            TEXT("GetPackedDataWithRestLengths: BindPosePositions is empty"));
        return;
    }

    // Each affected vertex: 1 count + (neighbor + restLength) * MAX_NEIGHBORS
    // = 1 + 2 * 12 = 25 uints
    const int32 PackedSizePerVertex = 1 + FLESHRING_MAX_NEIGHBORS * 2;
    OutPackedData.Reserve(AffectedIndices.Num() * PackedSizePerVertex);

    for (int32 ThreadIdx = 0; ThreadIdx < AffectedIndices.Num(); ++ThreadIdx)
    {
        const uint32 VertexIndex = AffectedIndices[ThreadIdx];

        if (VertexIndex < (uint32)VertexNeighbors.Num() && VertexIndex < (uint32)BindPosePositions.Num())
        {
            const TArray<uint32>& Neighbors = VertexNeighbors[VertexIndex];
            const uint32 NeighborCount = FMath::Min(Neighbors.Num(), FLESHRING_MAX_NEIGHBORS);

            // Get this vertex's position for rest length calculation
            const FVector3f& MyPos = BindPosePositions[VertexIndex];

            // Pack: [Count, N0, RestLen0, N1, RestLen1, ..., N11, RestLen11]
            OutPackedData.Add(NeighborCount);

            for (int32 i = 0; i < FLESHRING_MAX_NEIGHBORS; ++i)
            {
                if (i < (int32)NeighborCount)
                {
                    const uint32 NeighborIdx = Neighbors[i];
                    OutPackedData.Add(NeighborIdx);

                    // Calculate rest length (distance in bind pose)
                    float RestLength = 0.0f;
                    if (NeighborIdx < (uint32)BindPosePositions.Num())
                    {
                        const FVector3f& NeighborPos = BindPosePositions[NeighborIdx];
                        RestLength = FVector3f::Distance(MyPos, NeighborPos);
                    }

                    // Store float as uint bits (shader will use asfloat())
                    OutPackedData.Add(*reinterpret_cast<const uint32*>(&RestLength));
                }
                else
                {
                    // Padding: invalid neighbor and zero rest length
                    OutPackedData.Add(0);
                    const float ZeroLength = 0.0f;
                    OutPackedData.Add(*reinterpret_cast<const uint32*>(&ZeroLength));
                }
            }
        }
        else
        {
            // Invalid vertex index - add empty adjacency
            OutPackedData.Add(0); // Count = 0
            for (int32 i = 0; i < FLESHRING_MAX_NEIGHBORS; ++i)
            {
                OutPackedData.Add(0); // Neighbor
                OutPackedData.Add(0); // RestLength (0.0f as bits)
            }
        }
    }

    UE_LOG(LogFleshRingAdjacency, Verbose,
        TEXT("GetPackedDataWithRestLengths: Packed %d vertices (%d uints) with rest lengths"),
        AffectedIndices.Num(), OutPackedData.Num());
}

// ============================================================================
// GetPackedDataForAllVertices
// ============================================================================

void FMeshAdjacencyBuilder::GetPackedDataForAllVertices(TArray<uint32>& OutPackedData) const
{
    OutPackedData.Reset();

    if (!IsBuilt())
    {
        return;
    }

    const int32 PackedSizePerVertex = 1 + FLESHRING_MAX_NEIGHBORS;
    OutPackedData.Reserve(VertexNeighbors.Num() * PackedSizePerVertex);

    for (int32 VertexIdx = 0; VertexIdx < VertexNeighbors.Num(); ++VertexIdx)
    {
        const TArray<uint32>& Neighbors = VertexNeighbors[VertexIdx];
        const uint32 NeighborCount = FMath::Min(Neighbors.Num(), FLESHRING_MAX_NEIGHBORS);

        OutPackedData.Add(NeighborCount);

        for (int32 i = 0; i < FLESHRING_MAX_NEIGHBORS; ++i)
        {
            if (i < (int32)NeighborCount)
            {
                OutPackedData.Add(Neighbors[i]);
            }
            else
            {
                OutPackedData.Add(0);
            }
        }
    }
}

// ============================================================================
// Accessors
// ============================================================================

int32 FMeshAdjacencyBuilder::GetNeighborCount(int32 VertexIndex) const
{
    if (VertexIndex >= 0 && VertexIndex < VertexNeighbors.Num())
    {
        return VertexNeighbors[VertexIndex].Num();
    }
    return 0;
}

const TArray<uint32>* FMeshAdjacencyBuilder::GetNeighbors(int32 VertexIndex) const
{
    if (VertexIndex >= 0 && VertexIndex < VertexNeighbors.Num())
    {
        return &VertexNeighbors[VertexIndex];
    }
    return nullptr;
}

void FMeshAdjacencyBuilder::Clear()
{
    VertexNeighbors.Reset();
}

void FMeshAdjacencyBuilder::PrintStats() const
{
    if (!IsBuilt())
    {
        UE_LOG(LogFleshRingAdjacency, Log, TEXT("Adjacency not built"));
        return;
    }

    int32 MinNeighbors = INT_MAX;
    int32 MaxNeighbors = 0;
    int64 TotalNeighbors = 0;
    int32 ZeroNeighborCount = 0;

    for (const TArray<uint32>& Neighbors : VertexNeighbors)
    {
        const int32 Count = Neighbors.Num();
        MinNeighbors = FMath::Min(MinNeighbors, Count);
        MaxNeighbors = FMath::Max(MaxNeighbors, Count);
        TotalNeighbors += Count;

        if (Count == 0)
        {
            ZeroNeighborCount++;
        }
    }

    const float AvgNeighbors = VertexNeighbors.Num() > 0
        ? (float)TotalNeighbors / VertexNeighbors.Num()
        : 0.0f;

    UE_LOG(LogFleshRingAdjacency, Log,
        TEXT("Adjacency Stats: %d vertices, Min=%d, Max=%d, Avg=%.2f, ZeroNeighbor=%d"),
        VertexNeighbors.Num(), MinNeighbors, MaxNeighbors, AvgNeighbors, ZeroNeighborCount);
}
