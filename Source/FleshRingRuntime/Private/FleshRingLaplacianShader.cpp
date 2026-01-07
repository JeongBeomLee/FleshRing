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
    FRDGBufferRef DeformAmountsBuffer,
    FRDGBufferRef RepresentativeIndicesBuffer,
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
    PassParameters->DeformAmounts = GraphBuilder.CreateSRV(DeformAmountsBuffer);

    // UV Seam Welding: RepresentativeIndices 바인딩
    // nullptr이면 AffectedIndices를 fallback으로 사용
    if (RepresentativeIndicesBuffer)
    {
        PassParameters->RepresentativeIndices = GraphBuilder.CreateSRV(RepresentativeIndicesBuffer);
    }
    else
    {
        PassParameters->RepresentativeIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer);
    }

    PassParameters->AdjacencyData = GraphBuilder.CreateSRV(AdjacencyDataBuffer);

    // Bind layer types buffer (for excluding stocking from smoothing)
    if (VertexLayerTypesBuffer && Params.bExcludeStockingFromSmoothing)
    {
        PassParameters->VertexLayerTypes = GraphBuilder.CreateSRV(VertexLayerTypesBuffer, PF_R32_UINT);
        PassParameters->bExcludeStockingFromSmoothing = 1;
    }
    else
    {
        // Create dummy buffer (shader parameter binding required)
        uint32 DummyLayerData = 4;  // LAYER_UNKNOWN
        FRDGBufferRef DummyLayerBuffer = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
            TEXT("FleshRingLaplacian_DummyLayerTypes")
        );
        GraphBuilder.QueueBufferUpload(DummyLayerBuffer, &DummyLayerData, sizeof(DummyLayerData), ERDGInitialDataFlags::None);
        PassParameters->VertexLayerTypes = GraphBuilder.CreateSRV(DummyLayerBuffer, PF_R32_UINT);
        PassParameters->bExcludeStockingFromSmoothing = 0;
    }

    // Set parameters
    PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
    PassParameters->NumTotalVertices = Params.NumTotalVertices;
    // Use clamped lambda for positive values (standard smoothing), pass through negative (Taubin mu)
    PassParameters->SmoothingLambda = (Params.SmoothingLambda >= 0.0f)
        ? Params.GetEffectiveLambda()
        : Params.SmoothingLambda;  // Taubin mu pass (negative) - don't clamp
    PassParameters->VolumePreservation = Params.VolumePreservation;
    PassParameters->BulgeSmoothingFactor = Params.BulgeSmoothingFactor;
    PassParameters->BoundsScale = Params.BoundsScale;

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
// Multi-Pass Dispatch (Ping-Pong) - Standard Laplacian
// ============================================================================

static void DispatchStandardLaplacianMultiPass(
    FRDGBuilder& GraphBuilder,
    const FLaplacianDispatchParams& Params,
    FRDGBufferRef PositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef DeformAmountsBuffer,
    FRDGBufferRef RepresentativeIndicesBuffer,
    FRDGBufferRef AdjacencyDataBuffer,
    FRDGBufferRef VertexLayerTypesBuffer)
{
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
            DeformAmountsBuffer,
            RepresentativeIndicesBuffer,
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
    AddCopyBufferPass(GraphBuilder, PingBuffer, PositionsBuffer);
    AddCopyBufferPass(GraphBuilder, PongBuffer, PositionsBuffer);

    // Ping-pong iterations
    for (int32 Iteration = 0; Iteration < Params.NumIterations; ++Iteration)
    {
        bool bEvenIteration = (Iteration % 2 == 0);
        FRDGBufferRef ReadBuffer = bEvenIteration ? PingBuffer : PongBuffer;
        FRDGBufferRef WriteBuffer = bEvenIteration ? PongBuffer : PingBuffer;

        FLaplacianDispatchParams IterParams = Params;

        DispatchFleshRingLaplacianCS(
            GraphBuilder,
            IterParams,
            ReadBuffer,
            WriteBuffer,
            AffectedIndicesBuffer,
            InfluencesBuffer,
            DeformAmountsBuffer,
            RepresentativeIndicesBuffer,
            AdjacencyDataBuffer,
            VertexLayerTypesBuffer
        );
    }

    // Copy final result back to original buffer
    bool bFinalInPong = (Params.NumIterations % 2 == 1);
    FRDGBufferRef FinalBuffer = bFinalInPong ? PongBuffer : PingBuffer;
    AddCopyBufferPass(GraphBuilder, PositionsBuffer, FinalBuffer);
}

// ============================================================================
// Multi-Pass Dispatch (Ping-Pong) - Taubin Smoothing
// ============================================================================
// Taubin smoothing alternates between shrink (lambda) and expand (mu) passes
// Each "iteration" = 2 passes (lambda then mu)
//
// Mathematical basis: Acts as a band-pass filter
//   f(k) = (1 - lambda*k)(1 - mu*k)
//   - Low frequencies (k small): f(k) ~ 1 -> preserved (no shrinkage)
//   - High frequencies (k large): f(k) < 1 -> attenuated (smoothing)
//
// Condition: mu < -lambda (|mu| > lambda)
// Typical values: lambda = 0.5, mu = -0.53

static void DispatchTaubinMultiPass(
    FRDGBuilder& GraphBuilder,
    const FLaplacianDispatchParams& Params,
    FRDGBufferRef PositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef DeformAmountsBuffer,
    FRDGBufferRef RepresentativeIndicesBuffer,
    FRDGBufferRef AdjacencyDataBuffer,
    FRDGBufferRef VertexLayerTypesBuffer)
{
    // Use clamped Lambda for stability (max 0.8)
    const float Lambda = Params.GetEffectiveLambda();
    const float Mu = Params.GetEffectiveTaubinMu();

    // Warn if Lambda was clamped (user set value outside safe range)
    if (Params.NeedsLambdaClamping())
    {
        UE_LOG(LogTemp, Warning,
            TEXT("Taubin Smoothing: Lambda %.2f clamped to [%.1f, %.1f] for stability! Using lambda=%.3f, mu=%.3f"),
            Params.SmoothingLambda, FLaplacianDispatchParams::MinSafeLambda, FLaplacianDispatchParams::MaxSafeLambda,
            Lambda, Mu);
    }
    else
    {
        UE_LOG(LogTemp, Log, TEXT("Taubin Smoothing: lambda=%.3f, mu=%.3f, Iterations=%d (BulgeSmoothingFactor forced to 1.0)"),
            Lambda, Mu, Params.NumIterations);
    }

    // Each Taubin iteration = 2 passes (lambda shrink + mu expand)
    // Total passes = NumIterations * 2
    const int32 TotalPasses = Params.NumIterations * 2;

    // Create ping-pong buffers
    FRDGBufferRef PingBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateBufferDesc(sizeof(float), Params.NumTotalVertices * 3),
        TEXT("FleshRingTaubin_Ping")
    );

    FRDGBufferRef PongBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateBufferDesc(sizeof(float), Params.NumTotalVertices * 3),
        TEXT("FleshRingTaubin_Pong")
    );

    // Initialize BOTH buffers with input data
    // Critical: Both buffers must be initialized because shader only writes AffectedVertices
    AddCopyBufferPass(GraphBuilder, PingBuffer, PositionsBuffer);
    AddCopyBufferPass(GraphBuilder, PongBuffer, PositionsBuffer);

    // Taubin ping-pong: alternate lambda and mu
    for (int32 Pass = 0; Pass < TotalPasses; ++Pass)
    {
        const bool bEvenPass = (Pass % 2 == 0);
        FRDGBufferRef ReadBuffer = bEvenPass ? PingBuffer : PongBuffer;
        FRDGBufferRef WriteBuffer = bEvenPass ? PongBuffer : PingBuffer;

        // Prepare pass parameters
        FLaplacianDispatchParams PassParams = Params;

        // Alternate between lambda (shrink) and mu (expand)
        // Pass 0, 2, 4, ... -> lambda (shrink)
        // Pass 1, 3, 5, ... -> mu (expand)
        const bool bShrinkPass = (Pass % 2 == 0);
        PassParams.SmoothingLambda = bShrinkPass ? Lambda : Mu;

        // Disable volume preservation for Taubin (the lambda-mu alternation handles it)
        PassParams.VolumePreservation = 0.0f;

        // CRITICAL: Force BulgeSmoothingFactor = 1.0 for Taubin
        // Taubin requires symmetric application of shrink/expand to all vertices
        // If BulgeSmoothingFactor < 1.0, bulge vertices get asymmetric smoothing
        // (less shrink, less expand) which breaks the band-pass filter balance
        // This causes spikes at tightness-bulge boundaries!
        PassParams.BulgeSmoothingFactor = 1.0f;

        DispatchFleshRingLaplacianCS(
            GraphBuilder,
            PassParams,
            ReadBuffer,
            WriteBuffer,
            AffectedIndicesBuffer,
            InfluencesBuffer,
            DeformAmountsBuffer,
            RepresentativeIndicesBuffer,
            AdjacencyDataBuffer,
            VertexLayerTypesBuffer
        );
    }

    // Copy final result back to original buffer
    // After TotalPasses, result is in:
    //   - PongBuffer if TotalPasses is odd
    //   - PingBuffer if TotalPasses is even
    const bool bFinalInPong = (TotalPasses % 2 == 1);
    FRDGBufferRef FinalBuffer = bFinalInPong ? PongBuffer : PingBuffer;
    AddCopyBufferPass(GraphBuilder, PositionsBuffer, FinalBuffer);
}

// ============================================================================
// Public Multi-Pass Dispatch Function
// ============================================================================

void DispatchFleshRingLaplacianCS_MultiPass(
    FRDGBuilder& GraphBuilder,
    const FLaplacianDispatchParams& Params,
    FRDGBufferRef PositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef DeformAmountsBuffer,
    FRDGBufferRef RepresentativeIndicesBuffer,
    FRDGBufferRef AdjacencyDataBuffer,
    FRDGBufferRef VertexLayerTypesBuffer)
{
    if (Params.NumAffectedVertices == 0 || Params.NumIterations <= 0)
    {
        return;
    }

    if (Params.bUseTaubinSmoothing)
    {
        // Taubin smoothing: shrinkage-free band-pass filter
        DispatchTaubinMultiPass(
            GraphBuilder,
            Params,
            PositionsBuffer,
            AffectedIndicesBuffer,
            InfluencesBuffer,
            DeformAmountsBuffer,
            RepresentativeIndicesBuffer,
            AdjacencyDataBuffer,
            VertexLayerTypesBuffer
        );
    }
    else
    {
        // Standard Laplacian smoothing (may cause shrinkage)
        DispatchStandardLaplacianMultiPass(
            GraphBuilder,
            Params,
            PositionsBuffer,
            AffectedIndicesBuffer,
            InfluencesBuffer,
            DeformAmountsBuffer,
            RepresentativeIndicesBuffer,
            AdjacencyDataBuffer,
            VertexLayerTypesBuffer
        );
    }
}
