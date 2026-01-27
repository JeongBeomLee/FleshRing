// Copyright 2026 LgThx. All Rights Reserved.

// FleshRingSDFVisualizer.h
// SDF visualization utility - general purpose for editor/runtime
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/TextureRenderTarget2D.h"
#include "FleshRingSDFVisualizer.generated.h"

// Struct containing SDF visualization result
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FSDFVisualizationResult
{
    GENERATED_BODY()

    // Spawned plane actor (for visualization display)
    UPROPERTY(BlueprintReadOnly, Category = "SDF Visualization")
    AActor* PlaneActor = nullptr;

    // Generated render target (slice image)
    UPROPERTY(BlueprintReadOnly, Category = "SDF Visualization")
    UTextureRenderTarget2D* SliceTexture = nullptr;

    // SDF bounding box
    UPROPERTY(BlueprintReadOnly, Category = "SDF Visualization")
    FVector BoundsMin = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "SDF Visualization")
    FVector BoundsMax = FVector::ZeroVector;

    // Current slice Z index
    UPROPERTY(BlueprintReadOnly, Category = "SDF Visualization")
    int32 CurrentSliceZ = 0;

    // SDF resolution
    UPROPERTY(BlueprintReadOnly, Category = "SDF Visualization")
    int32 Resolution = 64;

    bool IsValid() const { return PlaneActor != nullptr && SliceTexture != nullptr; }
};

UCLASS()
class FLESHRINGRUNTIME_API UFleshRingSDFVisualizer : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // SDF slice visualization - generate SDF from mesh and display on plane
    // @param WorldContext - World context (for actor spawning)
    // @param Mesh - Static mesh to generate SDF from
    // @param WorldLocation - World location to place the plane
    // @param SliceZ - Z slice index to display (0 ~ Resolution-1)
    // @param Resolution - SDF resolution (default 64)
    // @param PlaneSize - Plane size (default 100)
    // @return Visualization result (plane actor, texture, etc.)
    UFUNCTION(BlueprintCallable, Category = "FleshRing|Visualization", meta = (WorldContext = "WorldContextObject"))
    static FSDFVisualizationResult VisualizeSDFSlice(
        UObject* WorldContextObject,
        UStaticMesh* Mesh,
        FVector WorldLocation,
        int32 SliceZ = 32,
        int32 Resolution = 64
    );

    // Update existing visualization (change slice Z only)
    // @param Result - Previous VisualizeSDFSlice result
    // @param NewSliceZ - New Z slice index
    UFUNCTION(BlueprintCallable, Category = "FleshRing|Visualization")
    static void UpdateSliceZ(UPARAM(ref) FSDFVisualizationResult& Result, int32 NewSliceZ);

    // Cleanup visualization (remove plane actor)
    UFUNCTION(BlueprintCallable, Category = "FleshRing|Visualization")
    static void CleanupVisualization(UPARAM(ref) FSDFVisualizationResult& Result);

    // Visualize all slices at once (single SDF generation)
    // @param WorldContext - World context (for actor spawning)
    // @param Mesh - Static mesh to generate SDF from
    // @param WorldLocation - Base world location to place the planes
    // @param Resolution - SDF resolution (default 64, same as slice count)
    // @return Array of visualization results (Resolution count)
    UFUNCTION(BlueprintCallable, Category = "FleshRing|Visualization", meta = (WorldContext = "WorldContextObject"))
    static TArray<FSDFVisualizationResult> VisualizeAllSDFSlices(
        UObject* WorldContextObject,
        UStaticMesh* Mesh,
        FVector WorldLocation,
        int32 Resolution = 64
    );

};
