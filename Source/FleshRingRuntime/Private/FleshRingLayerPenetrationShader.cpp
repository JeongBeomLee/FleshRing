// ============================================================================
// FleshRing Layer Penetration Resolution Shader - Implementation
// ============================================================================

#include "FleshRingLayerPenetrationShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"

// ============================================================================
// Shader Implementation Registration
// ============================================================================

IMPLEMENT_GLOBAL_SHADER(
    FFleshRingBuildTriangleLayerCS,
    "/Plugin/FleshRingPlugin/FleshRingLayerPenetrationCS.usf",
    "BuildTriangleLayerCS",
    SF_Compute
);

IMPLEMENT_GLOBAL_SHADER(
    FFleshRingLayerPenetrationCS,
    "/Plugin/FleshRingPlugin/FleshRingLayerPenetrationCS.usf",
    "LayerPenetrationCS",
    SF_Compute
);

// ============================================================================
// Dispatch Function
// ============================================================================

void DispatchFleshRingLayerPenetrationCS(
    FRDGBuilder& GraphBuilder,
    const FLayerPenetrationDispatchParams& Params,
    FRDGBufferRef PositionsBuffer,
    FRDGBufferRef NormalsBuffer,
    FRDGBufferRef VertexLayerTypesBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef TriangleIndicesBuffer)
{
    // Early out if no vertices or triangles
    if (Params.NumAffectedVertices == 0 || Params.NumTriangles == 0)
    {
        return;
    }

    const uint32 ThreadGroupSize = 64;

    // ========== Pass 1: Build Per-Triangle Layer Types ==========
    // Determine each triangle's layer type from its vertices
    FRDGBufferRef TriangleLayerTypesBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), Params.NumTriangles),
        TEXT("FleshRing_TriangleLayerTypes")
    );

    {
        FFleshRingBuildTriangleLayerCS::FParameters* BuildParams =
            GraphBuilder.AllocParameters<FFleshRingBuildTriangleLayerCS::FParameters>();

        BuildParams->VertexLayerTypes = GraphBuilder.CreateSRV(VertexLayerTypesBuffer, PF_R32_UINT);
        BuildParams->TriangleIndices = GraphBuilder.CreateSRV(TriangleIndicesBuffer, PF_R32_UINT);
        BuildParams->TriangleLayerTypesRW = GraphBuilder.CreateUAV(TriangleLayerTypesBuffer, PF_R32_UINT);
        BuildParams->NumTriangles = Params.NumTriangles;

        TShaderMapRef<FFleshRingBuildTriangleLayerCS> BuildShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

        const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumTriangles, ThreadGroupSize);

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("FleshRingBuildTriangleLayer"),
            BuildShader,
            BuildParams,
            FIntVector(static_cast<int32>(NumGroups), 1, 1)
        );
    }

    // ========== Pass 2: Layer Penetration Resolution ==========
    // For each outer-layer vertex, ensure it stays outside inner-layer triangles
    for (int32 Iteration = 0; Iteration < Params.NumIterations; ++Iteration)
    {
        FFleshRingLayerPenetrationCS::FParameters* PenetrationParams =
            GraphBuilder.AllocParameters<FFleshRingLayerPenetrationCS::FParameters>();

        PenetrationParams->PositionsRW = GraphBuilder.CreateUAV(PositionsBuffer, PF_R32_FLOAT);
        PenetrationParams->Normals = GraphBuilder.CreateSRV(NormalsBuffer, PF_R32_FLOAT);
        PenetrationParams->VertexLayerTypes = GraphBuilder.CreateSRV(VertexLayerTypesBuffer, PF_R32_UINT);
        PenetrationParams->AffectedIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer, PF_R32_UINT);
        PenetrationParams->TriangleIndices = GraphBuilder.CreateSRV(TriangleIndicesBuffer, PF_R32_UINT);
        PenetrationParams->TriangleLayerTypes = GraphBuilder.CreateSRV(TriangleLayerTypesBuffer, PF_R32_UINT);
        PenetrationParams->NumAffectedVertices = Params.NumAffectedVertices;
        PenetrationParams->NumTriangles = Params.NumTriangles;
        PenetrationParams->MinSeparation = Params.MinSeparation;
        PenetrationParams->MaxPushDistance = Params.MaxPushDistance;
        PenetrationParams->RingCenter = Params.RingCenter;
        PenetrationParams->RingAxis = Params.RingAxis;
        PenetrationParams->TightnessStrength = Params.TightnessStrength;
        PenetrationParams->OuterLayerPushRatio = Params.OuterLayerPushRatio;
        PenetrationParams->InnerLayerPushRatio = Params.InnerLayerPushRatio;

        TShaderMapRef<FFleshRingLayerPenetrationCS> PenetrationShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

        const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumAffectedVertices, ThreadGroupSize);

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("FleshRingLayerPenetration_Iter%d", Iteration),
            PenetrationShader,
            PenetrationParams,
            FIntVector(static_cast<int32>(NumGroups), 1, 1)
        );
    }
}
