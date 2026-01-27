// Copyright 2026 LgThx. All Rights Reserved.

// FleshRingSDFVisualizer.cpp
#include "FleshRingSDFVisualizer.h"
#include "FleshRingSDF.h"
#include "FleshRingMeshExtractor.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "RenderingThread.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/Material.h"

FSDFVisualizationResult UFleshRingSDFVisualizer::VisualizeSDFSlice(
    UObject* WorldContextObject,
    UStaticMesh* Mesh,
    FVector WorldLocation,
    int32 SliceZ /*= 32*/,
    int32 Resolution /*= 64 */)
{
    FSDFVisualizationResult Result;

    if (!WorldContextObject || !Mesh)
    {
        UE_LOG(LogTemp, Error, TEXT("VisualizeSDFSlice: Invalid parameters"));
        return Result;
    }

    UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
    if (!World)
    {
        UE_LOG(LogTemp, Error, TEXT("VisualizeSDFSlice: Could not get world"));
        return Result;
    }

    // 1. Extract mesh data
    FFleshRingMeshData MeshData;
    if (!UFleshRingMeshExtractor::ExtractMeshData(Mesh, MeshData))
    {
        UE_LOG(LogTemp, Error, TEXT("VisualizeSDFSlice: Failed to extract mesh data"));
        return Result;
    }

    // 2. Calculate bounding box (with margin)
    FVector3f BoundsSize = MeshData.Bounds.Max - MeshData.Bounds.Min;
    FVector3f Margin = BoundsSize * 0.1f;
    FVector3f BoundsMin = MeshData.Bounds.Min - Margin;
    FVector3f BoundsMax = MeshData.Bounds.Max + Margin;

    Result.BoundsMin = FVector(BoundsMin);
    Result.BoundsMax = FVector(BoundsMax);
    Result.CurrentSliceZ = FMath::Clamp(SliceZ, 0, Resolution - 1);
    Result.Resolution = Resolution;

    // 3. Create render target
    Result.SliceTexture = NewObject<UTextureRenderTarget2D>(WorldContextObject);
    Result.SliceTexture->InitCustomFormat(Resolution, Resolution, PF_B8G8R8A8, false);
    Result.SliceTexture->UpdateResourceImmediate(true);

    // 4. Spawn plane actor
    FActorSpawnParameters SpawnParams;
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    AActor* PlaneActor = World->SpawnActor<AActor>(AActor::StaticClass(), WorldLocation, FRotator::ZeroRotator, SpawnParams);
    if (!PlaneActor)
    {
        UE_LOG(LogTemp, Error, TEXT("VisualizeSDFSlice: Failed to spawn plane actor"));
        return Result;
    }

    // Set up root component
    USceneComponent* RootComp = NewObject<USceneComponent>(PlaneActor, TEXT("Root"));
    PlaneActor->SetRootComponent(RootComp);
    RootComp->RegisterComponent();

    // Add plane mesh component
    UStaticMeshComponent* PlaneMeshComp = NewObject<UStaticMeshComponent>(PlaneActor, TEXT("PlaneMesh"));
    PlaneMeshComp->SetupAttachment(RootComp);

    // Use engine default plane mesh
    UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
    if (PlaneMesh)
    {
        PlaneMeshComp->SetStaticMesh(PlaneMesh);
    }

    // Adjust plane scale - fit to mesh bounds size
    FVector3f BoundsSizeWithMargin = BoundsMax - BoundsMin;
    float PlaneScaleX = BoundsSizeWithMargin.X / 100.0f;  // Default plane is 100x100 units
    float PlaneScaleY = BoundsSizeWithMargin.Y / 100.0f;
    PlaneMeshComp->SetWorldScale3D(FVector(PlaneScaleX, PlaneScaleY, 1.0f));

    // Calculate Z slice position (local bounds + world offset)
    float LocalSliceZ = FMath::Lerp(
        BoundsMin.Z,
        BoundsMax.Z,
        (float)Result.CurrentSliceZ / (float)(Resolution - 1)
    );
    // World position = mesh local bounds center + world offset
    FVector PlaneCenter(
        WorldLocation.X + (BoundsMin.X + BoundsMax.X) * 0.5f,
        WorldLocation.Y + (BoundsMin.Y + BoundsMax.Y) * 0.5f,
        WorldLocation.Z + LocalSliceZ
    );
    PlaneMeshComp->SetWorldLocation(PlaneCenter);

    // 5. Create and apply material
    // Use Widget3DPassThrough material (supports texture parameter)
    UMaterialInterface* BaseMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/Widget3DPassThrough.Widget3DPassThrough"));
    if (!BaseMaterial)
    {
        // Fallback: default material
        BaseMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
    }
    UMaterialInstanceDynamic* DynMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, PlaneActor);

    // Set render target as texture parameter
    if (DynMaterial && Result.SliceTexture)
    {
        DynMaterial->SetTextureParameterValue(TEXT("SlateUI"), Result.SliceTexture);
    }

    PlaneMeshComp->SetMaterial(0, DynMaterial);
    PlaneMeshComp->RegisterComponent();

    // 6. Add back plane (for double-sided rendering)
    UStaticMeshComponent* BackPlaneMeshComp = NewObject<UStaticMeshComponent>(PlaneActor, TEXT("BackPlaneMesh"));
    BackPlaneMeshComp->SetupAttachment(RootComp);
    if (PlaneMesh)
    {
        BackPlaneMeshComp->SetStaticMesh(PlaneMesh);
    }
    BackPlaneMeshComp->SetWorldScale3D(FVector(PlaneScaleX, PlaneScaleY, 1.0f));
    BackPlaneMeshComp->SetWorldLocation(PlaneCenter);
    BackPlaneMeshComp->SetWorldRotation(FRotator(180.0f, 0.0f, 0.0f));  // Rotate 180 degrees around X axis (flip)
    BackPlaneMeshComp->SetMaterial(0, DynMaterial);
    BackPlaneMeshComp->RegisterComponent();

    Result.PlaneActor = PlaneActor;
    
    UE_LOG(LogTemp, Warning, TEXT("Plane spawned at: %s, Scale: (%.2f, %.2f)"),
        *PlaneCenter.ToString(), PlaneScaleX, PlaneScaleY);

    // 6. Generate SDF on GPU + slice visualization
    TArray<FVector3f> Vertices = MeshData.Vertices;
    TArray<uint32> Indices = MeshData.Indices;
    FIntVector SDFResolution(Resolution, Resolution, Resolution);
    int32 CapturedSliceZ = Result.CurrentSliceZ;
    float MaxDisplayDist = BoundsSize.GetMax() * 0.5f;
    UTextureRenderTarget2D* RenderTarget = Result.SliceTexture;

    ENQUEUE_RENDER_COMMAND(GenerateSDFAndSlice)(
        [Vertices, Indices, BoundsMin, BoundsMax, SDFResolution, CapturedSliceZ, MaxDisplayDist, RenderTarget](FRHICommandListImmediate& RHICmdList)
        {
            if (!RenderTarget)
            {
                UE_LOG(LogTemp, Error, TEXT("RenderTarget is null"));
                return;
            }

            FTextureRenderTargetResource* RTResource = RenderTarget->GetRenderTargetResource();
            if (!RTResource)
            {
                UE_LOG(LogTemp, Error, TEXT("RenderTargetResource is null"));
                return;
            }

            FRDGBuilder GraphBuilder(RHICmdList);

            // Create SDF 3D texture (sign determined by Ray Casting)
            FRDGTextureDesc SDFDesc = FRDGTextureDesc::Create3D(
                SDFResolution,
                PF_R32_FLOAT,
                FClearValueBinding::Black,
                TexCreate_ShaderResource | TexCreate_UAV
            );
            FRDGTextureRef SDFTexture = GraphBuilder.CreateTexture(SDFDesc, TEXT("SDFTexture"));

            // Generate SDF (sign determined by Multiple Ray Casting)
            GenerateMeshSDF(
                GraphBuilder,
                SDFTexture,
                Vertices,
                Indices,
                BoundsMin,
                BoundsMax,
                SDFResolution
            );

            // 2D Slice Flood Fill for donut hole correction
            FRDGTextureDesc CorrectedSDFDesc = FRDGTextureDesc::Create3D(
                SDFResolution,
                PF_R32_FLOAT,
                FClearValueBinding::Black,
                TexCreate_ShaderResource | TexCreate_UAV
            );
            FRDGTextureRef CorrectedSDF = GraphBuilder.CreateTexture(CorrectedSDFDesc, TEXT("CorrectedSDFTexture"));

            Apply2DSliceFloodFill(
                GraphBuilder,
                SDFTexture,
                CorrectedSDF,
                SDFResolution
            );

            // Create slice 2D texture
            FRDGTextureDesc SliceDesc = FRDGTextureDesc::Create2D(
                FIntPoint(SDFResolution.X, SDFResolution.Y),
                PF_B8G8R8A8,
                FClearValueBinding::Black,
                TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable
            );
            FRDGTextureRef SliceTexture = GraphBuilder.CreateTexture(SliceDesc, TEXT("SDFSliceTexture"));

            // Slice visualization (using corrected SDF)
            GenerateSDFSlice(
                GraphBuilder,
                CorrectedSDF,
                SliceTexture,
                SDFResolution,
                CapturedSliceZ,
                MaxDisplayDist
            );

            // Register external render target and copy
            FRHITexture* DestRHI = RTResource->GetRenderTargetTexture();
            if (DestRHI)
            {
                FRDGTextureRef DestTexture = GraphBuilder.RegisterExternalTexture(
                    CreateRenderTarget(DestRHI, TEXT("DestRenderTarget"))
                );
                AddCopyTexturePass(GraphBuilder, SliceTexture, DestTexture);
            }

            GraphBuilder.Execute();

            UE_LOG(LogTemp, Warning, TEXT("SDF Slice Visualization Generated: Z=%d"), CapturedSliceZ);
        }
    );

    UE_LOG(LogTemp, Warning, TEXT("VisualizeSDFSlice: Created visualization at Z=%d"), Result.CurrentSliceZ);
    return Result;
}

void UFleshRingSDFVisualizer::UpdateSliceZ(FSDFVisualizationResult& Result, int32 NewSliceZ)
{
    if (!Result.IsValid())
    {
        UE_LOG(LogTemp, Warning, TEXT("UpdateSliceZ: Invalid result"));
        return;
    }

    Result.CurrentSliceZ = FMath::Clamp(NewSliceZ, 0, Result.Resolution - 1);

    // TODO: Cache SDF and update slice only
    // Currently requires full regeneration
    UE_LOG(LogTemp, Warning, TEXT("UpdateSliceZ: Updated to Z=%d (requires re-visualization for now)"), Result.CurrentSliceZ);
}

void UFleshRingSDFVisualizer::CleanupVisualization(FSDFVisualizationResult& Result)
{
    if (Result.PlaneActor)
    {
        Result.PlaneActor->Destroy();
        Result.PlaneActor = nullptr;
    }

    if (Result.SliceTexture)
    {
        Result.SliceTexture = nullptr;
    }

    UE_LOG(LogTemp, Log, TEXT("CleanupVisualization: Cleaned up"));
}

TArray<FSDFVisualizationResult> UFleshRingSDFVisualizer::VisualizeAllSDFSlices(
    UObject* WorldContextObject,
    UStaticMesh* Mesh,
    FVector WorldLocation,
    int32 Resolution)
{
    TArray<FSDFVisualizationResult> Results;

    if (!WorldContextObject || !Mesh)
    {
        UE_LOG(LogTemp, Error, TEXT("VisualizeAllSDFSlices: Invalid parameters"));
        return Results;
    }

    UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
    if (!World)
    {
        UE_LOG(LogTemp, Error, TEXT("VisualizeAllSDFSlices: Could not get world"));
        return Results;
    }

    // 1. Extract mesh data (only once)
    FFleshRingMeshData MeshData;
    if (!UFleshRingMeshExtractor::ExtractMeshData(Mesh, MeshData))
    {
        UE_LOG(LogTemp, Error, TEXT("VisualizeAllSDFSlices: Failed to extract mesh data"));
        return Results;
    }

    // 2. Calculate bounding box
    FVector3f BoundsSize = MeshData.Bounds.Max - MeshData.Bounds.Min;
    FVector3f Margin = BoundsSize * 0.1f;
    FVector3f BoundsMin = MeshData.Bounds.Min - Margin;
    FVector3f BoundsMax = MeshData.Bounds.Max + Margin;
    FVector3f BoundsSizeWithMargin = BoundsMax - BoundsMin;

    float PlaneScaleX = BoundsSizeWithMargin.X / 100.0f;
    float PlaneScaleY = BoundsSizeWithMargin.Y / 100.0f;

    UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
    UMaterialInterface* BaseMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/Widget3DPassThrough.Widget3DPassThrough"));
    if (!BaseMaterial)
    {
        BaseMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
    }

    // 3. Create render targets + plane actors for all slices
    Results.SetNum(Resolution);
    TArray<UTextureRenderTarget2D*> RenderTargets;
    RenderTargets.SetNum(Resolution);

    for (int32 SliceZ = 0; SliceZ < Resolution; SliceZ++)
    {
        FSDFVisualizationResult& Result = Results[SliceZ];
        Result.BoundsMin = FVector(BoundsMin);
        Result.BoundsMax = FVector(BoundsMax);
        Result.CurrentSliceZ = SliceZ;
        Result.Resolution = Resolution;

        // Create render target
        Result.SliceTexture = NewObject<UTextureRenderTarget2D>(WorldContextObject);
        Result.SliceTexture->InitCustomFormat(Resolution, Resolution, PF_B8G8R8A8, false);
        Result.SliceTexture->UpdateResourceImmediate(true);
        RenderTargets[SliceZ] = Result.SliceTexture;

        // Spawn plane actor
        FActorSpawnParameters SpawnParams;
        SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

        AActor* PlaneActor = World->SpawnActor<AActor>(AActor::StaticClass(), WorldLocation, FRotator::ZeroRotator, SpawnParams);
        if (!PlaneActor) continue;

        USceneComponent* RootComp = NewObject<USceneComponent>(PlaneActor, TEXT("Root"));
        PlaneActor->SetRootComponent(RootComp);
        RootComp->RegisterComponent();

        // Calculate Z slice position
        float LocalSliceZ = FMath::Lerp(BoundsMin.Z, BoundsMax.Z, (float)SliceZ / (float)(Resolution - 1));
        FVector PlaneCenter(
            WorldLocation.X + (BoundsMin.X + BoundsMax.X) * 0.5f,
            WorldLocation.Y + (BoundsMin.Y + BoundsMax.Y) * 0.5f,
            WorldLocation.Z + LocalSliceZ
        );

        // Front plane
        UStaticMeshComponent* FrontPlane = NewObject<UStaticMeshComponent>(PlaneActor, TEXT("FrontPlane"));
        FrontPlane->SetupAttachment(RootComp);
        if (PlaneMesh) FrontPlane->SetStaticMesh(PlaneMesh);
        FrontPlane->SetWorldScale3D(FVector(PlaneScaleX, PlaneScaleY, 1.0f));
        FrontPlane->SetWorldLocation(PlaneCenter);

        UMaterialInstanceDynamic* DynMaterial = UMaterialInstanceDynamic::Create(BaseMaterial, PlaneActor);
        if (DynMaterial && Result.SliceTexture)
        {
            DynMaterial->SetTextureParameterValue(TEXT("SlateUI"), Result.SliceTexture);
        }
        FrontPlane->SetMaterial(0, DynMaterial);
        FrontPlane->RegisterComponent();

        // Back plane
        UStaticMeshComponent* BackPlane = NewObject<UStaticMeshComponent>(PlaneActor, TEXT("BackPlane"));
        BackPlane->SetupAttachment(RootComp);
        if (PlaneMesh) BackPlane->SetStaticMesh(PlaneMesh);
        BackPlane->SetWorldScale3D(FVector(PlaneScaleX, PlaneScaleY, 1.0f));
        BackPlane->SetWorldLocation(PlaneCenter);
        BackPlane->SetWorldRotation(FRotator(180.0f, 0.0f, 0.0f));
        BackPlane->SetMaterial(0, DynMaterial);
        BackPlane->RegisterComponent();

        Result.PlaneActor = PlaneActor;
    }

    // 4. GPU work: Generate SDF once + visualize all slices
    TArray<FVector3f> Vertices = MeshData.Vertices;
    TArray<uint32> Indices = MeshData.Indices;
    FIntVector SDFResolution(Resolution, Resolution, Resolution);
    float MaxDisplayDist = BoundsSize.GetMax() * 0.5f;

    ENQUEUE_RENDER_COMMAND(GenerateSDFAndAllSlices)(
        [Vertices, Indices, BoundsMin, BoundsMax, SDFResolution, MaxDisplayDist, RenderTargets](FRHICommandListImmediate& RHICmdList)
        {
            FRDGBuilder GraphBuilder(RHICmdList);

            // Create SDF 3D texture (only once!)
            FRDGTextureDesc SDFDesc = FRDGTextureDesc::Create3D(
                SDFResolution,
                PF_R32_FLOAT,
                FClearValueBinding::Black,
                TexCreate_ShaderResource | TexCreate_UAV
            );
            FRDGTextureRef SDFTexture = GraphBuilder.CreateTexture(SDFDesc, TEXT("SDFTexture"));

            // Generate SDF (only once!)
            GenerateMeshSDF(
                GraphBuilder,
                SDFTexture,
                Vertices,
                Indices,
                BoundsMin,
                BoundsMax,
                SDFResolution
            );

            // Flood Fill correction (only once!)
            FRDGTextureDesc CorrectedSDFDesc = FRDGTextureDesc::Create3D(
                SDFResolution,
                PF_R32_FLOAT,
                FClearValueBinding::Black,
                TexCreate_ShaderResource | TexCreate_UAV
            );
            FRDGTextureRef CorrectedSDF = GraphBuilder.CreateTexture(CorrectedSDFDesc, TEXT("CorrectedSDFTexture"));

            Apply2DSliceFloodFill(
                GraphBuilder,
                SDFTexture,
                CorrectedSDF,
                SDFResolution
            );

            // Visualize all slices
            for (int32 SliceZ = 0; SliceZ < SDFResolution.Z; SliceZ++)
            {
                UTextureRenderTarget2D* RenderTarget = RenderTargets[SliceZ];
                if (!RenderTarget) continue;

                FTextureRenderTargetResource* RTResource = RenderTarget->GetRenderTargetResource();
                if (!RTResource) continue;

                // Create slice 2D texture
                FRDGTextureDesc SliceDesc = FRDGTextureDesc::Create2D(
                    FIntPoint(SDFResolution.X, SDFResolution.Y),
                    PF_B8G8R8A8,
                    FClearValueBinding::Black,
                    TexCreate_ShaderResource | TexCreate_UAV | TexCreate_RenderTargetable
                );
                FRDGTextureRef SliceTexture = GraphBuilder.CreateTexture(SliceDesc, *FString::Printf(TEXT("SDFSlice_%d"), SliceZ));

                // Slice visualization
                GenerateSDFSlice(
                    GraphBuilder,
                    CorrectedSDF,
                    SliceTexture,
                    SDFResolution,
                    SliceZ,
                    MaxDisplayDist
                );

                // Copy to render target
                FRHITexture* DestRHI = RTResource->GetRenderTargetTexture();
                if (DestRHI)
                {
                    FRDGTextureRef DestTexture = GraphBuilder.RegisterExternalTexture(
                        CreateRenderTarget(DestRHI, TEXT("DestRenderTarget"))
                    );
                    AddCopyTexturePass(GraphBuilder, SliceTexture, DestTexture);
                }
            }

            GraphBuilder.Execute();

            UE_LOG(LogTemp, Warning, TEXT("VisualizeAllSDFSlices: Generated SDF once, visualized %d slices"), SDFResolution.Z);
        }
    );

    UE_LOG(LogTemp, Warning, TEXT("VisualizeAllSDFSlices: Created %d slice visualizations"), Resolution);
    return Results;
}

