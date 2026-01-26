// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Self-Collision Detection & Resolution Shader - Implementation
// ============================================================================

#include "FleshRingCollisionShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"

// ============================================================================
// Shader Implementation Registration
// ============================================================================

IMPLEMENT_GLOBAL_SHADER(
    FFleshRingCollisionDetectCS,
    "/Plugin/FleshRingPlugin/FleshRingCollisionCS.usf",
    "DetectCS",
    SF_Compute
);

IMPLEMENT_GLOBAL_SHADER(
    FFleshRingCollisionResolveCS,
    "/Plugin/FleshRingPlugin/FleshRingCollisionCS.usf",
    "ResolveCS",
    SF_Compute
);

// ============================================================================
// Dispatch Function
// ============================================================================

void DispatchFleshRingCollisionCS(
    FRDGBuilder& GraphBuilder,
    const FCollisionDispatchParams& Params,
    FRDGBufferRef PositionsBuffer,
    FRDGBufferRef TriangleIndicesBuffer)
{
    // Early out if no triangles
    if (Params.NumTriangles < 2)
    {
        return;
    }

    // Calculate total triangle pairs
    const uint32 TotalPairs = Params.NumTriangles * (Params.NumTriangles - 1) / 2;

    // Skip if too many pairs (performance safeguard)
    const uint32 MaxPairsToProcess = 100000;  // ~450 triangles
    if (TotalPairs > MaxPairsToProcess)
    {
        UE_LOG(LogTemp, Warning, TEXT("FleshRingCollision: Too many triangle pairs (%d), skipping collision detection"), TotalPairs);
        return;
    }

    // Create collision output buffers
    FRDGBufferRef CollisionPairsBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Params.MaxCollisionPairs * 3),
        TEXT("FleshRing_CollisionPairs")
    );

    FRDGBufferRef CollisionCountBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
        TEXT("FleshRing_CollisionCount")
    );

    // Clear collision count
    AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(CollisionCountBuffer, PF_R32_UINT), 0u);

    // ========== Detection Pass ==========
    {
        FFleshRingCollisionDetectCS::FParameters* DetectParams =
            GraphBuilder.AllocParameters<FFleshRingCollisionDetectCS::FParameters>();

        DetectParams->Positions = GraphBuilder.CreateSRV(PositionsBuffer, PF_R32_FLOAT);
        DetectParams->TriangleIndices = GraphBuilder.CreateSRV(TriangleIndicesBuffer, PF_R32_UINT);
        DetectParams->CollisionPairs = GraphBuilder.CreateUAV(CollisionPairsBuffer);
        DetectParams->CollisionCount = GraphBuilder.CreateUAV(CollisionCountBuffer, PF_R32_UINT);
        DetectParams->NumTriangles = Params.NumTriangles;
        DetectParams->MaxCollisionPairs = Params.MaxCollisionPairs;

        TShaderMapRef<FFleshRingCollisionDetectCS> DetectShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

        const uint32 ThreadGroupSize = 64;
        const uint32 NumGroups = FMath::DivideAndRoundUp(TotalPairs, ThreadGroupSize);

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("FleshRingCollisionDetect"),
            DetectShader,
            DetectParams,
            FIntVector(static_cast<int32>(NumGroups), 1, 1)
        );
    }

    // ========== Resolution Pass (may iterate multiple times) ==========
    for (int32 Iteration = 0; Iteration < Params.NumIterations; ++Iteration)
    {
        FFleshRingCollisionResolveCS::FParameters* ResolveParams =
            GraphBuilder.AllocParameters<FFleshRingCollisionResolveCS::FParameters>();

        ResolveParams->PositionsRW = GraphBuilder.CreateUAV(PositionsBuffer, PF_R32_FLOAT);
        ResolveParams->TriangleIndices = GraphBuilder.CreateSRV(TriangleIndicesBuffer, PF_R32_UINT);
        ResolveParams->CollisionPairsRead = GraphBuilder.CreateSRV(CollisionPairsBuffer);
        ResolveParams->CollisionCountRead = GraphBuilder.CreateSRV(CollisionCountBuffer, PF_R32_UINT);
        ResolveParams->NumTotalVertices = Params.NumTotalVertices;
        ResolveParams->ResolutionStrength = Params.ResolutionStrength;

        TShaderMapRef<FFleshRingCollisionResolveCS> ResolveShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

        // Dispatch for max possible collisions (shader will bounds check)
        const uint32 ThreadGroupSize = 64;
        const uint32 NumGroups = FMath::DivideAndRoundUp(Params.MaxCollisionPairs, ThreadGroupSize);

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("FleshRingCollisionResolve_Iter%d", Iteration),
            ResolveShader,
            ResolveParams,
            FIntVector(static_cast<int32>(NumGroups), 1, 1)
        );
    }
}
