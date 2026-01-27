// Copyright 2026 LgThx. All Rights Reserved.

// FleshRingMeshExtractor.h
// Utility to extract vertex/index/normal data from UStaticMesh
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/StaticMesh.h"
#include "FleshRingMeshExtractor.generated.h"

// Triangle data structure to be passed to GPU
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FFleshRingMeshData
{
    GENERATED_BODY()

    // Vertex position array
    TArray<FVector3f> Vertices;

    // Triangle indices (3 per triangle)
    TArray<uint32> Indices;

    // Mesh bounding box
    FBox3f Bounds;

    // Triangle count
    int32 GetTriangleCount() const { return Indices.Num() / 3; }

    // Vertex count
    int32 GetVertexCount() const { return Vertices.Num(); }

    // Validity check
    bool IsValid() const
    {
        return Vertices.Num() > 0 &&
               Indices.Num() > 0 &&
               Indices.Num() % 3 == 0;
    }

    // Clear data
    void Reset()
    {
        Vertices.Empty();
        Indices.Empty();
        Bounds = FBox3f(ForceInit);
    }
};

// Mesh extraction utility class
UCLASS()
class FLESHRINGRUNTIME_API UFleshRingMeshExtractor : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // Extract data from UStaticMesh (uses LOD 0)
    // @param Mesh - Static mesh to extract from
    // @param OutMeshData - Struct where extracted data will be stored
    // @return Success or failure
    UFUNCTION(BlueprintCallable, Category = "FleshRing|Mesh")
    static bool ExtractMeshData(UStaticMesh* Mesh, FFleshRingMeshData& OutMeshData);

    // Extract data from specific LOD
    // @param Mesh - Static mesh to extract from
    // @param LODIndex - LOD index to use
    // @param OutMeshData - Struct where extracted data will be stored
    // @return Success or failure
    UFUNCTION(BlueprintCallable, Category = "FleshRing|Mesh")
    static bool ExtractMeshDataFromLOD(UStaticMesh* Mesh, int32 LODIndex, FFleshRingMeshData& OutMeshData);

    // Debug print extracted data
    UFUNCTION(BlueprintCallable, Category = "FleshRing|Mesh")
    static void DebugPrintMeshData(const FFleshRingMeshData& MeshData);
};
