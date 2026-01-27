// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Tightness Shader - Implementation
// ============================================================================
// Purpose: Pull vertices toward Ring center axis (Tightness effect)

#include "FleshRingTightnessShader.h"
#include "FleshRingDebugTypes.h"

#include "CoreMinimal.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "RHIStaticStates.h"

// Includes for asset-based testing
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"

// ============================================================================
// Shader Implementation Registration
// ============================================================================
IMPLEMENT_GLOBAL_SHADER(
    FFleshRingTightnessCS,
    "/Plugin/FleshRingPlugin/FleshRingTightnessCS.usf",
    "MainCS",
    SF_Compute
);

// ============================================================================
// Dispatch Function Implementation
// ============================================================================

void DispatchFleshRingTightnessCS(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    // Influence is calculated directly on GPU
    FRDGBufferRef RepresentativeIndicesBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRDGTextureRef SDFTexture,
    FRDGBufferRef VolumeAccumBuffer,
    FRDGBufferRef DebugInfluencesBuffer)
    // DebugPointBuffer is handled by DebugPointOutputCS
{
    // Early out if no vertices to process
    if (Params.NumAffectedVertices == 0)
    {
        UE_LOG(LogTemp, Warning, TEXT("[TightnessShader] Early return: NumAffectedVertices=0"));
        return;
    }

    // Allocate shader parameters
    FFleshRingTightnessCS::FParameters* PassParameters =
        GraphBuilder.AllocParameters<FFleshRingTightnessCS::FParameters>();

    // ===== Bind input buffers (SRV) =====
    PassParameters->SourcePositions = GraphBuilder.CreateSRV(SourcePositionsBuffer, PF_R32_FLOAT);
    PassParameters->AffectedIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer);
    // Influence is calculated directly on GPU (CalculateVirtualRingInfluence, CalculateVirtualBandInfluence)

    // ===== UV Seam Welding: RepresentativeIndices binding =====
    // If RepresentativeIndices is nullptr, use AffectedIndices as fallback
    // In shader: read representative position -> calculate deformation -> write to own index
    if (RepresentativeIndicesBuffer)
    {
        PassParameters->RepresentativeIndices = GraphBuilder.CreateSRV(RepresentativeIndicesBuffer);
    }
    else
    {
        // Fallback: use AffectedIndices (each vertex is its own representative)
        PassParameters->RepresentativeIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer);
    }

    // ===== Bind output buffer (UAV) =====
    PassParameters->OutputPositions = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);

    // ===== Skinning disabled (bind pose mode) =====
    // Note: RDG requires valid SRV bindings with uploaded data even for unused resources
    static const float DummyBoneMatrixData[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    static const uint32 DummyWeightData = 0;

    FRDGBufferRef DummyBoneMatricesBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateBufferDesc(sizeof(float) * 4, 1),
        TEXT("FleshRingTightness_DummyBoneMatrices")
    );
    GraphBuilder.QueueBufferUpload(DummyBoneMatricesBuffer, DummyBoneMatrixData, sizeof(DummyBoneMatrixData), ERDGInitialDataFlags::None);

    FRDGBufferRef DummyWeightStreamBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
        TEXT("FleshRingTightness_DummyWeightStream")
    );
    GraphBuilder.QueueBufferUpload(DummyWeightStreamBuffer, &DummyWeightData, sizeof(DummyWeightData), ERDGInitialDataFlags::None);

    PassParameters->BoneMatrices = GraphBuilder.CreateSRV(DummyBoneMatricesBuffer, PF_A32B32G32R32F);
    PassParameters->InputWeightStream = GraphBuilder.CreateSRV(DummyWeightStreamBuffer, PF_R32_UINT);
    PassParameters->InputWeightStride = 0;
    PassParameters->InputWeightIndexSize = 0;
    PassParameters->NumBoneInfluences = 0;
    PassParameters->bEnableSkinning = 0;

    // ===== Ring parameters =====
    PassParameters->RingCenter = Params.RingCenter;
    PassParameters->RingAxis = Params.RingAxis;
    PassParameters->TightnessStrength = Params.TightnessStrength;
    PassParameters->RingRadius = Params.RingRadius;
    PassParameters->RingHeight = Params.RingHeight;
    PassParameters->RingThickness = Params.RingThickness;
    PassParameters->FalloffType = Params.FalloffType;
    PassParameters->InfluenceMode = Params.InfluenceMode;

    // ===== VirtualBand (Virtual Band) Parameters =====
    PassParameters->LowerRadius = Params.LowerRadius;
    PassParameters->MidLowerRadius = Params.MidLowerRadius;
    PassParameters->MidUpperRadius = Params.MidUpperRadius;
    PassParameters->UpperRadius = Params.UpperRadius;
    PassParameters->LowerHeight = Params.LowerHeight;
    PassParameters->BandSectionHeight = Params.BandSectionHeight;
    PassParameters->UpperHeight = Params.UpperHeight;

    // ===== Counts =====
    PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
    PassParameters->NumTotalVertices = Params.NumTotalVertices;

    // ===== SDF Parameters (OBB Design) =====
    // If SDFTexture is valid, use SDF Auto mode; if nullptr, use VirtualRing mode
    if (SDFTexture)
    {
        PassParameters->SDFTexture = GraphBuilder.CreateSRV(SDFTexture);
        PassParameters->SDFSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
        PassParameters->SDFBoundsMin = Params.SDFBoundsMin;
        PassParameters->SDFBoundsMax = Params.SDFBoundsMax;
        PassParameters->bUseSDFInfluence = 1;
        // OBB support: Component to Local inverse transform matrix
        PassParameters->ComponentToSDFLocal = Params.ComponentToSDFLocal;
        PassParameters->SDFLocalToComponent = Params.SDFLocalToComponent;
        // SDF falloff distance
        PassParameters->SDFInfluenceFalloffDistance = Params.SDFInfluenceFalloffDistance;
        // Ring Center/Axis (SDF Local Space) - pass accurate position even when bounds are extended
        PassParameters->SDFLocalRingCenter = Params.SDFLocalRingCenter;
        PassParameters->SDFLocalRingAxis = Params.SDFLocalRingAxis;
    }
    else
    {
        // VirtualRing mode: Bind dummy SDF texture (RDG requirement - all parameters must be bound)
        FRDGTextureDesc DummySDFDesc = FRDGTextureDesc::Create3D(
            FIntVector(1, 1, 1),
            PF_R32_FLOAT,
            FClearValueBinding::Black,
            TexCreate_ShaderResource | TexCreate_UAV);  // Add UAV (for Clear)
        FRDGTextureRef DummySDFTexture = GraphBuilder.CreateTexture(DummySDFDesc, TEXT("FleshRingTightness_DummySDF"));

        // RDG validation pass: add write pass to texture (Producer required)
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummySDFTexture), 0.0f);

        PassParameters->SDFTexture = GraphBuilder.CreateSRV(DummySDFTexture);
        PassParameters->SDFSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
        PassParameters->SDFBoundsMin = FVector3f::ZeroVector;
        PassParameters->SDFBoundsMax = FVector3f::OneVector;
        PassParameters->bUseSDFInfluence = 0;
        // VirtualRing mode: Identity matrix (not used)
        PassParameters->ComponentToSDFLocal = FMatrix44f::Identity;
        PassParameters->SDFLocalToComponent = FMatrix44f::Identity;
        // Not used in VirtualRing mode but binding required
        PassParameters->SDFInfluenceFalloffDistance = 5.0f;
        // VirtualRing mode: default value binding (not used)
        PassParameters->SDFLocalRingCenter = FVector3f::ZeroVector;
        PassParameters->SDFLocalRingAxis = FVector3f(0.0f, 0.0f, 1.0f);
    }

    // ===== Smoothing Bounds Z Extension Parameters =====
    PassParameters->BoundsZTop = Params.BoundsZTop;
    PassParameters->BoundsZBottom = Params.BoundsZBottom;

    // ===== Volume Accumulation Parameters (for Bulge pass) =====
    PassParameters->bAccumulateVolume = Params.bAccumulateVolume;
    PassParameters->FixedPointScale = Params.FixedPointScale;
    PassParameters->RingIndex = Params.RingIndex;

    if (VolumeAccumBuffer)
    {
        // Bind VolumeAccumBuffer if provided
        PassParameters->VolumeAccumBuffer = GraphBuilder.CreateUAV(VolumeAccumBuffer, PF_R32_UINT);
    }
    else
    {
        // Create dummy if VolumeAccumBuffer is not provided (RDG requirement - all parameters must be bound)
        FRDGBufferRef DummyVolumeBuffer = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
            TEXT("FleshRingTightness_DummyVolumeAccum")
        );
        // Initialize dummy buffer (RDG Producer required)
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyVolumeBuffer, PF_R32_UINT), 0u);
        PassParameters->VolumeAccumBuffer = GraphBuilder.CreateUAV(DummyVolumeBuffer, PF_R32_UINT);
        // Force disable volume accumulation when using dummy
        PassParameters->bAccumulateVolume = 0;
    }

    // ===== Debug Influence Output Parameters =====
    PassParameters->bOutputDebugInfluences = Params.bOutputDebugInfluences;
    // DebugInfluenceBaseOffset and DebugPointBaseOffset use the same offset

    if (DebugInfluencesBuffer && Params.bOutputDebugInfluences)
    {
        // Bind if DebugInfluencesBuffer is provided and output is enabled
        PassParameters->DebugInfluences = GraphBuilder.CreateUAV(DebugInfluencesBuffer, PF_R32_FLOAT);
    }
    else
    {
        // Create dummy if DebugInfluences is not provided or disabled (RDG requirement)
        FRDGBufferRef DummyDebugInfluencesBuffer = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateBufferDesc(sizeof(float), 1),
            TEXT("FleshRingTightness_DummyDebugInfluences")
        );
        // Initialize dummy buffer (RDG Producer required)
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyDebugInfluencesBuffer, PF_R32_FLOAT), 0.0f);
        PassParameters->DebugInfluences = GraphBuilder.CreateUAV(DummyDebugInfluencesBuffer, PF_R32_FLOAT);
        // Force disable output when using dummy
        PassParameters->bOutputDebugInfluences = 0;
    }

    // DebugPointBaseOffset is also used for DebugInfluences
    PassParameters->DebugPointBaseOffset = Params.DebugPointBaseOffset;
    // DebugPointBuffer is handled by DebugPointOutputCS based on final positions

    // Get shader reference
    TShaderMapRef<FFleshRingTightnessCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // Calculate dispatch groups
    const uint32 ThreadGroupSize = 64; // Matches [numthreads(64,1,1)] in .usf
    const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumAffectedVertices, ThreadGroupSize);

    // Add compute pass to RDG
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("FleshRingTightnessCS"),
        ComputeShader,
        PassParameters,
        FIntVector(static_cast<int32>(NumGroups), 1, 1)
    );
}

// ============================================================================
// Dispatch with Readback (for testing/validation)
// ============================================================================

void DispatchFleshRingTightnessCS_WithReadback(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    // Influence is calculated directly on GPU
    FRDGBufferRef RepresentativeIndicesBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRHIGPUBufferReadback* Readback,
    FRDGTextureRef SDFTexture,
    FRDGBufferRef VolumeAccumBuffer,
    FRDGBufferRef DebugInfluencesBuffer)
{
    // Dispatch the compute shader
    DispatchFleshRingTightnessCS(
        GraphBuilder,
        Params,
        SourcePositionsBuffer,
        AffectedIndicesBuffer,
        RepresentativeIndicesBuffer,
        OutputPositionsBuffer,
        SDFTexture,
        VolumeAccumBuffer,
        DebugInfluencesBuffer
    );

    // Add readback pass (GPU to CPU data transfer)
    AddEnqueueCopyPass(GraphBuilder, Readback, OutputPositionsBuffer, 0);
}

// ============================================================================
// Asset-based Test - FleshRing.TightnessTest Console Command
// Finds FleshRingComponent in world and tests TightnessCS with actual asset data
// ============================================================================

// GPU result validation function (currently unused - replaced by per-Ring inline validation)
// Kept for potential reuse

// ============================================================================
// FleshRing.TightnessTest - Asset-based TightnessCS Test Console Command
//
// Usage: Enter FleshRing.TightnessTest in console during PIE mode
// Requirements: Actor with FleshRingComponent in world + FleshRingAsset assigned
// ============================================================================
static FAutoConsoleCommand GFleshRingTightnessTestCommand(
    TEXT("FleshRing.TightnessTest"),
    TEXT("Tests TightnessCS GPU computation using FleshRingAsset"),
    FConsoleCommandDelegate::CreateLambda([]()
    {
        UE_LOG(LogTemp, Log, TEXT(""));
        UE_LOG(LogTemp, Log, TEXT("========================================="));
        UE_LOG(LogTemp, Log, TEXT("  FleshRing TightnessCS Test"));
        UE_LOG(LogTemp, Log, TEXT("  (Asset-based GPU Computation Validation)"));
        UE_LOG(LogTemp, Log, TEXT("========================================="));

        // ============================================================
        // Step 1: Search for FleshRingComponent in World
        // ============================================================
        UE_LOG(LogTemp, Log, TEXT(""));
        UE_LOG(LogTemp, Log, TEXT("----------------------------------------"));
        UE_LOG(LogTemp, Log, TEXT("[ Step 1: FleshRingComponent Search ]"));
        UE_LOG(LogTemp, Log, TEXT("----------------------------------------"));

        UFleshRingComponent* FoundComponent = nullptr;
        USkeletalMeshComponent* TargetSkelMesh = nullptr;

        for (TObjectIterator<UFleshRingComponent> It; It; ++It)
        {
            UFleshRingComponent* Comp = *It;
            if (Comp && Comp->GetWorld() && !Comp->GetWorld()->IsPreviewWorld())
            {
                if (Comp->FleshRingAsset && Comp->GetResolvedTargetMesh())
                {
                    FoundComponent = Comp;
                    TargetSkelMesh = Comp->GetResolvedTargetMesh();
                    break;
                }
            }
        }

        if (!FoundComponent)
        {
            UE_LOG(LogTemp, Error, TEXT("  X FleshRingComponent not found"));
            UE_LOG(LogTemp, Error, TEXT(""));
            UE_LOG(LogTemp, Error, TEXT("  Solutions:"));
            UE_LOG(LogTemp, Error, TEXT("    1. Place an actor with FleshRingComponent in the world"));
            UE_LOG(LogTemp, Error, TEXT("    2. Assign FleshRingAsset to the component"));
            UE_LOG(LogTemp, Error, TEXT("    3. Test in PIE mode (Play)"));
            return;
        }

        if (!TargetSkelMesh)
        {
            UE_LOG(LogTemp, Error, TEXT("  X TargetSkeletalMesh not found"));
            return;
        }

        UFleshRingAsset* Asset = FoundComponent->FleshRingAsset;
        if (Asset->Rings.Num() == 0)
        {
            UE_LOG(LogTemp, Error, TEXT("  X FleshRingAsset has no Rings"));
            return;
        }

        UE_LOG(LogTemp, Log, TEXT("  O FleshRingComponent found"));
        UE_LOG(LogTemp, Log, TEXT("    - Actor: %s"), *FoundComponent->GetOwner()->GetName());
        UE_LOG(LogTemp, Log, TEXT("    - FleshRingAsset: %s"), *Asset->GetName());
        UE_LOG(LogTemp, Log, TEXT("    - Ring count: %d"), Asset->Rings.Num());
        UE_LOG(LogTemp, Log, TEXT("    - TargetMesh: %s"), *TargetSkelMesh->GetName());

        // ============================================================
        // Step 2: Register AffectedVertices (Affected Vertex Selection)
        // ============================================================
        UE_LOG(LogTemp, Log, TEXT(""));
        UE_LOG(LogTemp, Log, TEXT("----------------------------------------"));
        UE_LOG(LogTemp, Log, TEXT("[ Step 2: Affected Vertex Selection ]"));
        UE_LOG(LogTemp, Log, TEXT("----------------------------------------"));

        FFleshRingAffectedVerticesManager AffectedManager;
        if (!AffectedManager.RegisterAffectedVertices(FoundComponent, TargetSkelMesh))
        {
            UE_LOG(LogTemp, Error, TEXT("  X AffectedVertices registration failed"));
            return;
        }

        const TArray<FRingAffectedData>& AllRingData = AffectedManager.GetAllRingData();
        if (AllRingData.Num() == 0)
        {
            UE_LOG(LogTemp, Error, TEXT("  X No Ring data registered"));
            return;
        }

        UE_LOG(LogTemp, Log, TEXT("  O Affected vertex selection complete"));
        UE_LOG(LogTemp, Log, TEXT("    - Rings processed: %d"), AllRingData.Num());
        UE_LOG(LogTemp, Log, TEXT("    - Total affected vertices: %d"), AffectedManager.GetTotalAffectedCount());

        // Per-Ring affected vertex summary
        for (int32 i = 0; i < AllRingData.Num(); ++i)
        {
            UE_LOG(LogTemp, Log, TEXT("    - Ring[%d] '%s': %d vertices"),
                i, *AllRingData[i].BoneName.ToString(), AllRingData[i].Vertices.Num());
        }

        // ============================================================
        // Step 3: Extract Vertex Data from Mesh
        // ============================================================
        USkeletalMesh* SkelMesh = TargetSkelMesh->GetSkeletalMeshAsset();
        if (!SkelMesh)
        {
            UE_LOG(LogTemp, Error, TEXT("  X SkeletalMesh asset not found"));
            return;
        }

        const FSkeletalMeshRenderData* RenderData = SkelMesh->GetResourceForRendering();
        if (!RenderData || RenderData->LODRenderData.Num() == 0)
        {
            UE_LOG(LogTemp, Error, TEXT("  X RenderData not found"));
            return;
        }

        const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];
        const uint32 TotalVertexCount = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

        UE_LOG(LogTemp, Log, TEXT(""));
        UE_LOG(LogTemp, Log, TEXT("----------------------------------------"));
        UE_LOG(LogTemp, Log, TEXT("[ Step 3: Mesh Vertex Data Extraction ]"));
        UE_LOG(LogTemp, Log, TEXT("----------------------------------------"));
        UE_LOG(LogTemp, Log, TEXT("  Total mesh vertex count: %d"), TotalVertexCount);

        // Extract vertex position data (shared across all Rings)
        TArray<float> SourcePositions;
        SourcePositions.SetNum(TotalVertexCount * 3);

        for (uint32 i = 0; i < TotalVertexCount; ++i)
        {
            const FVector3f& Pos = LODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i);
            SourcePositions[i * 3 + 0] = Pos.X;
            SourcePositions[i * 3 + 1] = Pos.Y;
            SourcePositions[i * 3 + 2] = Pos.Z;
        }
        UE_LOG(LogTemp, Log, TEXT("  Vertex position buffer extraction complete"));

        // Shared data pointer
        TSharedPtr<TArray<float>> SourceDataPtr = MakeShared<TArray<float>>(SourcePositions);

        // ============================================================
        // Step 4: Run GPU Test for Each Ring
        // ============================================================
        UE_LOG(LogTemp, Log, TEXT(""));
        UE_LOG(LogTemp, Log, TEXT("----------------------------------------"));
        UE_LOG(LogTemp, Log, TEXT("[ Step 4: Per-Ring GPU TightnessCS Test ]"));
        UE_LOG(LogTemp, Log, TEXT("----------------------------------------"));

        int32 TestedRingCount = 0;

        for (int32 RingIdx = 0; RingIdx < AllRingData.Num(); ++RingIdx)
        {
            const FRingAffectedData& RingData = AllRingData[RingIdx];

            UE_LOG(LogTemp, Log, TEXT(""));
            UE_LOG(LogTemp, Log, TEXT("--------------------------------------"));
            UE_LOG(LogTemp, Log, TEXT("> Ring[%d] '%s' Test"), RingIdx, *RingData.BoneName.ToString());
            UE_LOG(LogTemp, Log, TEXT("--------------------------------------"));

            // Convert FalloffType to string
            FString FalloffTypeStr;
            switch (RingData.FalloffType)
            {
            case EFalloffType::Linear:    FalloffTypeStr = TEXT("Linear"); break;
            case EFalloffType::Quadratic: FalloffTypeStr = TEXT("Quadratic"); break;
            case EFalloffType::Hermite:   FalloffTypeStr = TEXT("Hermite (S-curve)"); break;
            default:                      FalloffTypeStr = TEXT("Unknown"); break;
            }

            UE_LOG(LogTemp, Log, TEXT("  [Ring Settings]"));
            UE_LOG(LogTemp, Log, TEXT("    - Bone position (bind pose): (%.2f, %.2f, %.2f)"),
                RingData.RingCenter.X, RingData.RingCenter.Y, RingData.RingCenter.Z);
            UE_LOG(LogTemp, Log, TEXT("    - Bone axis direction: (%.2f, %.2f, %.2f)"),
                RingData.RingAxis.X, RingData.RingAxis.Y, RingData.RingAxis.Z);
            UE_LOG(LogTemp, Log, TEXT("    - Ring radius: %.2f"), RingData.RingRadius);
            UE_LOG(LogTemp, Log, TEXT("    - Ring width: %.2f"), RingData.RingHeight);
            UE_LOG(LogTemp, Log, TEXT("    - Influence range (Radius+Width): %.2f"), RingData.RingRadius + RingData.RingHeight);
            UE_LOG(LogTemp, Log, TEXT("    - Tightness strength: %.2f"), RingData.TightnessStrength);
            UE_LOG(LogTemp, Log, TEXT("    - Falloff type: %s"), *FalloffTypeStr);

            UE_LOG(LogTemp, Log, TEXT(""));
            UE_LOG(LogTemp, Log, TEXT("  [Affected Vertices]"));
            UE_LOG(LogTemp, Log, TEXT("    - Selected vertex count: %d"), RingData.Vertices.Num());

            if (RingData.Vertices.Num() == 0)
            {
                UE_LOG(LogTemp, Warning, TEXT("    ! No affected vertices - skipping this Ring test"));
                UE_LOG(LogTemp, Warning, TEXT("    -> Check Ring position/size or increase Radius/Width values"));
                continue;
            }

            // Output sample vertex info
            UE_LOG(LogTemp, Log, TEXT("    - Sample vertices (up to 5):"));
            int32 SampleCount = FMath::Min(5, RingData.Vertices.Num());
            for (int32 i = 0; i < SampleCount; ++i)
            {
                const FAffectedVertex& V = RingData.Vertices[i];
                uint32 BaseIdx = V.VertexIndex * 3;
                UE_LOG(LogTemp, Log, TEXT("      [%d] Vertex#%d: RadialDist=%.2f, Influence=%.3f, Position=(%.2f, %.2f, %.2f)"),
                    i, V.VertexIndex, V.RadialDistance, V.Influence,
                    SourcePositions[BaseIdx], SourcePositions[BaseIdx + 1], SourcePositions[BaseIdx + 2]);
            }

            // GPU Dispatch preparation
            TSharedPtr<TArray<uint32>> IndicesPtr = MakeShared<TArray<uint32>>(RingData.PackedIndices);
            TSharedPtr<TArray<float>> InfluencesPtr = MakeShared<TArray<float>>(RingData.PackedInfluences);
            TSharedPtr<FRHIGPUBufferReadback> Readback = MakeShared<FRHIGPUBufferReadback>(
                *FString::Printf(TEXT("TightnessTestReadback_Ring%d"), RingIdx));

            FTightnessDispatchParams Params = CreateTightnessParams(RingData, TotalVertexCount);
            FName BoneName = RingData.BoneName;

            UE_LOG(LogTemp, Log, TEXT(""));
            UE_LOG(LogTemp, Log, TEXT("  [GPU Dispatch]"));
            UE_LOG(LogTemp, Log, TEXT("    - Creating buffers..."));

            // ================================================================
            // RDG (Render Dependency Graph) Dispatch on render thread
            // ================================================================
            // RDG uses "deferred execution" approach:
            //   1. CreateBuffer / QueueBufferUpload / CreateSRV etc. only "schedule" operations
            //   2. Actual execution happens in dependency order when GraphBuilder.Execute() is called
            //   3. Therefore, when passing buffers to Dispatch function, data is already "scheduled"
            // ================================================================
            ENQUEUE_RENDER_COMMAND(FleshRingTightnessTest_Dispatch)(
                [SourceDataPtr, IndicesPtr, InfluencesPtr, Params, Readback, TotalVertexCount, RingIdx, BoneName]
                (FRHICommandListImmediate& RHICmdList)
                {
                    FRDGBuilder GraphBuilder(RHICmdList);

                    // ========================================
                    // [Step 1] Buffer creation + data upload "scheduling"
                    // ========================================

                    // Source positions buffer (input: original vertex positions)
                    FRDGBufferRef SourceBuffer = GraphBuilder.CreateBuffer(
                        FRDGBufferDesc::CreateBufferDesc(sizeof(float), TotalVertexCount * 3),
                        TEXT("TightnessTest_SourcePositions")
                    );
                    GraphBuilder.QueueBufferUpload(  // "Schedule" data upload
                        SourceBuffer,
                        SourceDataPtr->GetData(),
                        SourceDataPtr->Num() * sizeof(float),
                        ERDGInitialDataFlags::None
                    );

                    // Affected indices buffer (input: affected vertex indices)
                    FRDGBufferRef IndicesBuffer = GraphBuilder.CreateBuffer(
                        FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Params.NumAffectedVertices),
                        TEXT("TightnessTest_AffectedIndices")
                    );
                    GraphBuilder.QueueBufferUpload(  // "Schedule" data upload
                        IndicesBuffer,
                        IndicesPtr->GetData(),
                        IndicesPtr->Num() * sizeof(uint32),
                        ERDGInitialDataFlags::None
                    );

                    // Influence is calculated directly on GPU

                    // Output buffer (output: deformed vertex positions)
                    FRDGBufferRef OutputBuffer = GraphBuilder.CreateBuffer(
                        FRDGBufferDesc::CreateBufferDesc(sizeof(float), TotalVertexCount * 3),
                        TEXT("TightnessTest_OutputPositions")
                    );

                    // Copy source to output (preserve unaffected vertices)
                    AddCopyBufferPass(GraphBuilder, OutputBuffer, SourceBuffer);

                    // ========================================
                    // [Step 2] "Schedule" Dispatch call
                    // ========================================
                    // At this point:
                    //   - Buffers are not yet actually created in GPU memory
                    //   - Data is not yet uploaded
                    //   - Inside DispatchFleshRingTightnessCS:
                    //     1. CreateSRV/CreateUAV = "Schedule" View creation
                    //     2. AddPass = "Schedule" shader execution
                    //   - Everything is only "scheduled" in GraphBuilder
                    // ========================================
                    DispatchFleshRingTightnessCS_WithReadback(
                        GraphBuilder,
                        Params,
                        SourceBuffer,
                        IndicesBuffer,
                        // Influence is calculated directly on GPU
                        nullptr,  // RepresentativeIndicesBuffer - not used in test
                        OutputBuffer,
                        Readback.Get()
                    );

                    // ========================================
                    // [Step 3] Execute() = actual execution
                    // ========================================
                    // What happens when Execute() is called:
                    //   1. RDG analyzes all resource dependencies
                    //   2. Determines optimal execution order
                    //   3. Actually creates GPU buffers
                    //   4. Actually uploads data scheduled by QueueBufferUpload
                    //   5. Actually executes shaders scheduled by AddPass
                    //   6. Executes Readback pass (GPU to CPU copy)
                    // ========================================
                    GraphBuilder.Execute();
                    UE_LOG(LogTemp, Log, TEXT("    - Ring[%d] '%s' GPU Dispatch complete"), RingIdx, *BoneName.ToString());
                });

            // Result validation
            ENQUEUE_RENDER_COMMAND(FleshRingTightnessTest_Validate)(
                [SourceDataPtr, IndicesPtr, InfluencesPtr, Readback, Params, TotalVertexCount, RingIdx, BoneName]
                (FRHICommandListImmediate& RHICmdList)
                {
                    if (!Readback->IsReady())
                    {
                        RHICmdList.BlockUntilGPUIdle();
                    }

                    UE_LOG(LogTemp, Log, TEXT(""));
                    UE_LOG(LogTemp, Log, TEXT("  [Ring[%d] '%s' Validation Result]"), RingIdx, *BoneName.ToString());

                    if (Readback->IsReady())
                    {
                        const float* OutputData = static_cast<const float*>(
                            Readback->Lock(TotalVertexCount * 3 * sizeof(float)));

                        if (OutputData)
                        {
                            // Validate GPU results on CPU
                            uint32 PassCount = 0;
                            uint32 FailCount = 0;

                            for (uint32 i = 0; i < Params.NumAffectedVertices; ++i)
                            {
                                uint32 VertexIndex = (*IndicesPtr)[i];
                                float Influence = (*InfluencesPtr)[i];

                                uint32 BaseIndex = VertexIndex * 3;
                                FVector3f SourcePos(
                                    (*SourceDataPtr)[BaseIndex + 0],
                                    (*SourceDataPtr)[BaseIndex + 1],
                                    (*SourceDataPtr)[BaseIndex + 2]
                                );
                                FVector3f OutputPos(
                                    OutputData[BaseIndex + 0],
                                    OutputData[BaseIndex + 1],
                                    OutputData[BaseIndex + 2]
                                );

                                // Calculate expected result (same logic as shader)
                                FVector3f ToVertex = SourcePos - Params.RingCenter;
                                float AxisDist = FVector3f::DotProduct(ToVertex, Params.RingAxis);
                                FVector3f RadialVec = ToVertex - Params.RingAxis * AxisDist;
                                float RadialDist = RadialVec.Size();

                                FVector3f ExpectedPos = SourcePos;
                                if (RadialDist > 0.001f)
                                {
                                    FVector3f InwardDir = -RadialVec / RadialDist;
                                    float Displacement = Params.TightnessStrength * Influence;
                                    ExpectedPos = SourcePos + InwardDir * Displacement;
                                }

                                bool bMatch = FVector3f::Distance(OutputPos, ExpectedPos) < 0.01f;
                                if (bMatch) PassCount++;
                                else FailCount++;
                            }

                            if (FailCount == 0)
                            {
                                UE_LOG(LogTemp, Log, TEXT("    O Validation passed: all %d vertices deformed correctly"), PassCount);
                            }
                            else
                            {
                                UE_LOG(LogTemp, Error, TEXT("    X Validation failed: passed=%d, failed=%d"), PassCount, FailCount);
                            }

                            Readback->Unlock();
                        }
                        else
                        {
                            UE_LOG(LogTemp, Error, TEXT("    X Readback Lock failed"));
                        }
                    }
                    else
                    {
                        UE_LOG(LogTemp, Error, TEXT("    X Readback not ready"));
                    }
                });

            TestedRingCount++;
        }

        UE_LOG(LogTemp, Log, TEXT(""));
        UE_LOG(LogTemp, Log, TEXT("----------------------------------------"));
        UE_LOG(LogTemp, Log, TEXT("[ Test Complete ]"));
        UE_LOG(LogTemp, Log, TEXT("----------------------------------------"));
        UE_LOG(LogTemp, Log, TEXT("  Total Ring count: %d"), AllRingData.Num());
        UE_LOG(LogTemp, Log, TEXT("  Tested Ring count: %d"), TestedRingCount);
        UE_LOG(LogTemp, Log, TEXT("  (Only Rings with affected vertices are tested)"));
        UE_LOG(LogTemp, Log, TEXT(""));
        UE_LOG(LogTemp, Log, TEXT("  * Validation results are output from render thread"));
        UE_LOG(LogTemp, Log, TEXT("========================================="));
    })
);
