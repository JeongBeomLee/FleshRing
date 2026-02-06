// Copyright 2026 LgThx. All Rights Reserved.

// FleshRingMeshExtractor.cpp
#include "FleshRingMeshExtractor.h"
#include "StaticMeshResources.h"
#include "Rendering/PositionVertexBuffer.h"
#include "RawIndexBuffer.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingMeshExtractor, Log, All);

bool UFleshRingMeshExtractor::ExtractMeshData(UStaticMesh* Mesh, FFleshRingMeshData& OutMeshData)
{
    return ExtractMeshDataFromLOD(Mesh, 0, OutMeshData);
}

bool UFleshRingMeshExtractor::ExtractMeshDataFromLOD(UStaticMesh* Mesh, int32 LODIndex, FFleshRingMeshData& OutMeshData)
{
    OutMeshData.Reset();

    // 1. Validation check
    if (!Mesh)
    {
        UE_LOG(LogFleshRingMeshExtractor, Error, TEXT("ExtractMeshData: Mesh is null"));
        return false;
    }

    // Check render data
    FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
    if (!RenderData)
    {
        UE_LOG(LogFleshRingMeshExtractor, Error, TEXT("ExtractMeshData: RenderData is null"));
        return false;
    }

    // LOD validation check
    if (LODIndex < 0 || LODIndex >= RenderData->LODResources.Num())
    {
        UE_LOG(LogFleshRingMeshExtractor, Error, TEXT("ExtractMeshData: Invalid LOD index %d (max: %d)"),
            LODIndex, RenderData->LODResources.Num() - 1);
        return false;
    }

    // 2. Get LOD resources
    const FStaticMeshLODResources& LODResource = RenderData->LODResources[LODIndex];

    // 3. Extract vertices
    const FPositionVertexBuffer& PositionBuffer = LODResource.VertexBuffers.PositionVertexBuffer;
    const int32 VertexCount = PositionBuffer.GetNumVertices();

    if (VertexCount == 0)
    {
        UE_LOG(LogFleshRingMeshExtractor, Error, TEXT("ExtractMeshData: No vertices found"));
        return false;
    }

    OutMeshData.Vertices.SetNum(VertexCount);

    // Initialize bounding box
    FVector3f MinBounds(FLT_MAX, FLT_MAX, FLT_MAX);
    FVector3f MaxBounds(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    for (int32 i = 0; i < VertexCount; i++)
    {
        // In UE5, VertexPosition returns FVector3f
        const FVector3f& Position = PositionBuffer.VertexPosition(i);
        OutMeshData.Vertices[i] = Position;

        // Update bounding box
        MinBounds = FVector3f::Min(MinBounds, Position);
        MaxBounds = FVector3f::Max(MaxBounds, Position);
    }

    OutMeshData.Bounds = FBox3f(MinBounds, MaxBounds);

    // 4. Extract indices
    const FRawStaticIndexBuffer& IndexBuffer = LODResource.IndexBuffer;

    // Access index buffer (16-bit or 32-bit)
    TArray<uint32> AllIndices;

    if (IndexBuffer.Is32Bit())
    {
        // 32-bit indices
        const int32 NumIndices = IndexBuffer.GetNumIndices();
        AllIndices.SetNum(NumIndices);

        // Copy directly from index buffer
        // Create CPU-accessible copy using GetCopy
        IndexBuffer.GetCopy(AllIndices);
    }
    else
    {
        // Convert 16-bit indices to 32-bit
        const int32 NumIndices = IndexBuffer.GetNumIndices();
        AllIndices.SetNum(NumIndices);

        TArray<uint16> Indices16;
        Indices16.SetNum(NumIndices);

        // Copy as 16-bit version
        TArray<uint32> TempIndices;
        IndexBuffer.GetCopy(TempIndices);

        for (int32 i = 0; i < NumIndices; i++)
        {
            AllIndices[i] = TempIndices[i];
        }
    }

    // Process indices per section (supports multiple material slots)
    // For now, combine triangles from all sections
    OutMeshData.Indices = MoveTemp(AllIndices);

    if (OutMeshData.Indices.Num() == 0 || OutMeshData.Indices.Num() % 3 != 0)
    {
        UE_LOG(LogFleshRingMeshExtractor, Error, TEXT("ExtractMeshData: Invalid index count %d (must be multiple of 3)"),
            OutMeshData.Indices.Num());
        return false;
    }

    return true;
}

void UFleshRingMeshExtractor::DebugPrintMeshData(const FFleshRingMeshData& MeshData)
{
    // Intentionally empty for release builds
}
