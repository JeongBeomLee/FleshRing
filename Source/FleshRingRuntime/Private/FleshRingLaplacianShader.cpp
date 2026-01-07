// ============================================================================
// FleshRing Laplacian Smoothing Shader - Implementation
// ============================================================================

#include "FleshRingLaplacianShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"

// ============================================================================
// Shader Implementation Registration
// ============================================================================

IMPLEMENT_GLOBAL_SHADER(
    FFleshRingLaplacianCS,
    "/Plugin/FleshRingPlugin/FleshRingLaplacianCS.usf",
    "MainCS",
    SF_Compute
);

// ============================================================================
// Single Pass Dispatch
// ============================================================================

void DispatchFleshRingLaplacianCS(
    FRDGBuilder& GraphBuilder,
    const FLaplacianDispatchParams& Params,
    FRDGBufferRef InputPositionsBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef AdjacencyDataBuffer,
    FRDGBufferRef VertexLayerTypesBuffer)
{
    // Early out if no vertices to process
    if (Params.NumAffectedVertices == 0)
    {
        return;
    }

    // Allocate shader parameters
    FFleshRingLaplacianCS::FParameters* PassParameters =
        GraphBuilder.AllocParameters<FFleshRingLaplacianCS::FParameters>();

    // Bind buffers
    PassParameters->InputPositions = GraphBuilder.CreateSRV(InputPositionsBuffer, PF_R32_FLOAT);
    PassParameters->OutputPositions = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);
    PassParameters->AffectedIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer);
    PassParameters->Influences = GraphBuilder.CreateSRV(InfluencesBuffer);
    PassParameters->AdjacencyData = GraphBuilder.CreateSRV(AdjacencyDataBuffer);

    // Set parameters
    PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
    PassParameters->NumTotalVertices = Params.NumTotalVertices;
    PassParameters->SmoothingLambda = Params.SmoothingLambda;
    PassParameters->VolumePreservation = Params.VolumePreservation;
    PassParameters->BulgeSmoothingFactor = Params.BulgeSmoothingFactor;
    PassParameters->BoundsScale = Params.BoundsScale;

    // Layer types for stocking exclusion
    if (VertexLayerTypesBuffer && Params.bExcludeStockingFromSmoothing)
    {
        PassParameters->VertexLayerTypes = GraphBuilder.CreateSRV(VertexLayerTypesBuffer, PF_R32_UINT);
        PassParameters->bExcludeStockingFromSmoothing = 1;
    }
    else
    {
        PassParameters->bExcludeStockingFromSmoothing = 0;
    }

    // Get shader
    TShaderMapRef<FFleshRingLaplacianCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // Calculate dispatch groups
    const uint32 ThreadGroupSize = 64;
    const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumAffectedVertices, ThreadGroupSize);

    // Add compute pass
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("FleshRingLaplacianCS"),
        ComputeShader,
        PassParameters,
        FIntVector(static_cast<int32>(NumGroups), 1, 1)
    );
}

// ============================================================================
// Multi-Pass Dispatch (Ping-Pong)
// ============================================================================

void DispatchFleshRingLaplacianCS_MultiPass(
    FRDGBuilder& GraphBuilder,
    const FLaplacianDispatchParams& Params,
    FRDGBufferRef PositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef AdjacencyDataBuffer,
    FRDGBufferRef VertexLayerTypesBuffer)
{
    if (Params.NumAffectedVertices == 0 || Params.NumIterations <= 0)
    {
        return;
    }

    // For single iteration, dispatch directly
    if (Params.NumIterations == 1)
    {
        // Create temp buffer for output
        FRDGBufferRef TempBuffer = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateBufferDesc(sizeof(float), Params.NumTotalVertices * 3),
            TEXT("FleshRingLaplacian_Temp")
        );

        // Copy input to temp (for reading neighbors that aren't being smoothed)
        AddCopyBufferPass(GraphBuilder, TempBuffer, PositionsBuffer);

        // Dispatch: Temp -> Positions
        DispatchFleshRingLaplacianCS(
            GraphBuilder,
            Params,
            TempBuffer,
            PositionsBuffer,
            AffectedIndicesBuffer,
            InfluencesBuffer,
            AdjacencyDataBuffer,
            VertexLayerTypesBuffer
        );
        return;
    }

    // Multi-pass: use ping-pong buffers
    FRDGBufferRef PingBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateBufferDesc(sizeof(float), Params.NumTotalVertices * 3),
        TEXT("FleshRingLaplacian_Ping")
    );

    FRDGBufferRef PongBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateBufferDesc(sizeof(float), Params.NumTotalVertices * 3),
        TEXT("FleshRingLaplacian_Pong")
    );

    // Initialize BOTH buffers with input data
    // Critical: PongBuffer must also be initialized because Laplacian shader only writes
    // AffectedVertices, leaving non-affected vertices uninitialized. When the second
    // iteration reads neighbor positions from PongBuffer, uninitialized values cause
    // vertices to explode to infinity.
    AddCopyBufferPass(GraphBuilder, PingBuffer, PositionsBuffer);
    AddCopyBufferPass(GraphBuilder, PongBuffer, PositionsBuffer);

    // Ping-pong iterations
    for (int32 Iteration = 0; Iteration < Params.NumIterations; ++Iteration)
    {
        bool bEvenIteration = (Iteration % 2 == 0);
        FRDGBufferRef ReadBuffer = bEvenIteration ? PingBuffer : PongBuffer;
        FRDGBufferRef WriteBuffer = bEvenIteration ? PongBuffer : PingBuffer;

        // Adjust lambda for iteration (optional: decrease over iterations)
        FLaplacianDispatchParams IterParams = Params;
        // Could reduce lambda per iteration for better convergence:
        // IterParams.SmoothingLambda = Params.SmoothingLambda * (1.0f - (float)Iteration / Params.NumIterations * 0.5f);

        DispatchFleshRingLaplacianCS(
            GraphBuilder,
            IterParams,
            ReadBuffer,
            WriteBuffer,
            AffectedIndicesBuffer,
            InfluencesBuffer,
            AdjacencyDataBuffer,
            VertexLayerTypesBuffer
        );
    }

    // Copy final result back to original buffer
    bool bFinalInPong = (Params.NumIterations % 2 == 1);
    FRDGBufferRef FinalBuffer = bFinalInPong ? PongBuffer : PingBuffer;
    AddCopyBufferPass(GraphBuilder, PositionsBuffer, FinalBuffer);
}
