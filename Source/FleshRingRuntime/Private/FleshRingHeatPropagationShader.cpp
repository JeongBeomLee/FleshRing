// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Heat Propagation Shader - Implementation
// ============================================================================

#include "FleshRingHeatPropagationShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"

DECLARE_LOG_CATEGORY_EXTERN(LogFleshRingHeatProp, Log, All);
DEFINE_LOG_CATEGORY(LogFleshRingHeatProp);

// ============================================================================
// Shader Implementation Registration
// ============================================================================

IMPLEMENT_GLOBAL_SHADER(
    FFleshRingHeatPropagationCS,
    "/Plugin/FleshRingPlugin/FleshRingHeatPropagationCS.usf",
    "MainCS",
    SF_Compute
);

// ============================================================================
// Dispatch Function
// ============================================================================

void DispatchFleshRingHeatPropagationCS(
    FRDGBuilder& GraphBuilder,
    const FHeatPropagationDispatchParams& Params,
    FRDGBufferRef OriginalPositionsBuffer,
    FRDGBufferRef CurrentPositionsBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRDGBufferRef ExtendedIndicesBuffer,
    FRDGBufferRef IsSeedFlagsBuffer,
    FRDGBufferRef IsBoundarySeedFlagsBuffer,
    FRDGBufferRef IsBarrierFlagsBuffer,
    FRDGBufferRef AdjacencyDataBuffer,
    FRDGBufferRef RepresentativeIndicesBuffer)
{
    // Early out
    if (Params.NumExtendedVertices == 0 || Params.NumIterations <= 0)
    {
        return;
    }

    // Debug log (once)
    static bool bLoggedOnce = false;
    if (!bLoggedOnce)
    {
        UE_LOG(LogFleshRingHeatProp, Log,
            TEXT("[HeatPropagation] NumExtended=%d, NumTotal=%d, Lambda=%.2f, Iterations=%d"),
            Params.NumExtendedVertices, Params.NumTotalVertices, Params.HeatLambda, Params.NumIterations);
        bLoggedOnce = true;
    }

    const uint32 ThreadGroupSize = 64;
    const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumExtendedVertices, ThreadGroupSize);
    // Delta buffers use full mesh size (accessed by mesh vertex index)
    const uint32 DeltaBufferSize = Params.NumTotalVertices * 3;

    // Get shader
    TShaderMapRef<FFleshRingHeatPropagationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // ========================================
    // Create Delta buffers and initialize to zero
    // This solves the RDG "never written to" dependency issue
    // ========================================
    FRDGBufferRef DeltaBufferA = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateBufferDesc(sizeof(float), DeltaBufferSize),
        TEXT("FleshRing_HeatProp_DeltaA")
    );

    FRDGBufferRef DeltaBufferB = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateBufferDesc(sizeof(float), DeltaBufferSize),
        TEXT("FleshRing_HeatProp_DeltaB")
    );

    // Initialize Delta buffers to zero (solves RDG dependency)
    AddClearUAVFloatPass(GraphBuilder, GraphBuilder.CreateUAV(DeltaBufferA, PF_R32_FLOAT), 0.0f);
    AddClearUAVFloatPass(GraphBuilder, GraphBuilder.CreateUAV(DeltaBufferB, PF_R32_FLOAT), 0.0f);

    // Create dummy buffer for unused SRV bindings
    FRDGBufferRef DummyFloatBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateBufferDesc(sizeof(float), 4),
        TEXT("FleshRing_HeatProp_DummyFloat")
    );
    AddClearUAVFloatPass(GraphBuilder, GraphBuilder.CreateUAV(DummyFloatBuffer, PF_R32_FLOAT), 0.0f);

    // UV Seam Welding: RepresentativeIndices binding
    // If nullptr, use ExtendedIndices as fallback
    FRDGBufferSRVRef RepresentativeIndicesSRV = RepresentativeIndicesBuffer
        ? GraphBuilder.CreateSRV(RepresentativeIndicesBuffer)
        : GraphBuilder.CreateSRV(ExtendedIndicesBuffer);

    // Barrier buffer: If nullptr, use dummy buffer with all flags set to 0
    FRDGBufferRef BarrierBuffer = IsBarrierFlagsBuffer;
    if (!BarrierBuffer)
    {
        BarrierBuffer = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Params.NumExtendedVertices),
            TEXT("FleshRing_HeatProp_DummyBarrier")
        );
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(BarrierBuffer), 0u);
    }
    FRDGBufferSRVRef IsBarrierFlagsSRV = GraphBuilder.CreateSRV(BarrierBuffer);

    // BoundarySeed buffer: If nullptr, treat all Seeds as boundary (legacy behavior)
    FRDGBufferRef BoundarySeedBuffer = IsBoundarySeedFlagsBuffer;
    if (!BoundarySeedBuffer)
    {
        // If nullptr, use IsSeedFlags directly (all Seeds are boundary)
        BoundarySeedBuffer = IsSeedFlagsBuffer;
    }
    FRDGBufferSRVRef IsBoundarySeedFlagsSRV = GraphBuilder.CreateSRV(BoundarySeedBuffer);

    // ========================================
    // Pass 0: Init
    // Seed.delta = CurrentPos - OriginalPos
    // Non-Seed.delta = 0
    // ========================================
    {
        FFleshRingHeatPropagationCS::FParameters* InitParams =
            GraphBuilder.AllocParameters<FFleshRingHeatPropagationCS::FParameters>();

        InitParams->PassType = 0;  // Init
        InitParams->OriginalPositions = GraphBuilder.CreateSRV(OriginalPositionsBuffer, PF_R32_FLOAT);
        InitParams->CurrentPositions = GraphBuilder.CreateSRV(CurrentPositionsBuffer, PF_R32_FLOAT);
        InitParams->OutputPositions = GraphBuilder.CreateUAV(DummyFloatBuffer, PF_R32_FLOAT);  // Not used in Init
        InitParams->DeltaIn = GraphBuilder.CreateSRV(DummyFloatBuffer, PF_R32_FLOAT);  // Not used in Init
        InitParams->DeltaOut = GraphBuilder.CreateUAV(DeltaBufferA, PF_R32_FLOAT);
        InitParams->ExtendedIndices = GraphBuilder.CreateSRV(ExtendedIndicesBuffer);
        InitParams->IsSeedFlags = GraphBuilder.CreateSRV(IsSeedFlagsBuffer);
        InitParams->IsBoundarySeedFlags = IsBoundarySeedFlagsSRV;
        InitParams->IsBarrierFlags = IsBarrierFlagsSRV;
        InitParams->AdjacencyData = GraphBuilder.CreateSRV(AdjacencyDataBuffer);
        InitParams->RepresentativeIndices = RepresentativeIndicesSRV;
        InitParams->NumExtendedVertices = Params.NumExtendedVertices;
        InitParams->HeatLambda = Params.HeatLambda;

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("HeatPropagation_Init"),
            ComputeShader,
            InitParams,
            FIntVector(NumGroups, 1, 1)
        );
    }

    // ========================================
    // Pass 1: Diffuse x N (ping-pong)
    // ========================================
    FRDGBufferRef ReadBuffer = DeltaBufferA;
    FRDGBufferRef WriteBuffer = DeltaBufferB;

    for (int32 Iter = 0; Iter < Params.NumIterations; ++Iter)
    {
        FFleshRingHeatPropagationCS::FParameters* DiffuseParams =
            GraphBuilder.AllocParameters<FFleshRingHeatPropagationCS::FParameters>();

        DiffuseParams->PassType = 1;  // Diffuse
        DiffuseParams->OriginalPositions = GraphBuilder.CreateSRV(DummyFloatBuffer, PF_R32_FLOAT);  // Not used
        DiffuseParams->CurrentPositions = GraphBuilder.CreateSRV(DummyFloatBuffer, PF_R32_FLOAT);  // Not used
        DiffuseParams->OutputPositions = GraphBuilder.CreateUAV(DummyFloatBuffer, PF_R32_FLOAT);  // Not used
        DiffuseParams->DeltaIn = GraphBuilder.CreateSRV(ReadBuffer, PF_R32_FLOAT);
        DiffuseParams->DeltaOut = GraphBuilder.CreateUAV(WriteBuffer, PF_R32_FLOAT);
        DiffuseParams->ExtendedIndices = GraphBuilder.CreateSRV(ExtendedIndicesBuffer);
        DiffuseParams->IsSeedFlags = GraphBuilder.CreateSRV(IsSeedFlagsBuffer);
        DiffuseParams->IsBoundarySeedFlags = IsBoundarySeedFlagsSRV;
        DiffuseParams->IsBarrierFlags = IsBarrierFlagsSRV;
        DiffuseParams->AdjacencyData = GraphBuilder.CreateSRV(AdjacencyDataBuffer);
        DiffuseParams->RepresentativeIndices = RepresentativeIndicesSRV;
        DiffuseParams->NumExtendedVertices = Params.NumExtendedVertices;
        DiffuseParams->HeatLambda = Params.HeatLambda;

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("HeatPropagation_Diffuse_%d", Iter),
            ComputeShader,
            DiffuseParams,
            FIntVector(NumGroups, 1, 1)
        );

        // Swap buffers for next iteration
        Swap(ReadBuffer, WriteBuffer);
    }

    // After iterations, ReadBuffer contains the final delta

    // ========================================
    // Pass 2: Apply
    // Non-Seed: FinalPos = OriginalPos + delta
    // Seed: FinalPos = CurrentPos
    // ========================================
    {
        FFleshRingHeatPropagationCS::FParameters* ApplyParams =
            GraphBuilder.AllocParameters<FFleshRingHeatPropagationCS::FParameters>();

        ApplyParams->PassType = 2;  // Apply
        ApplyParams->OriginalPositions = GraphBuilder.CreateSRV(OriginalPositionsBuffer, PF_R32_FLOAT);
        ApplyParams->CurrentPositions = GraphBuilder.CreateSRV(CurrentPositionsBuffer, PF_R32_FLOAT);
        ApplyParams->OutputPositions = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);
        ApplyParams->DeltaIn = GraphBuilder.CreateSRV(ReadBuffer, PF_R32_FLOAT);  // Final delta
        ApplyParams->DeltaOut = GraphBuilder.CreateUAV(DummyFloatBuffer, PF_R32_FLOAT);  // Not used
        ApplyParams->ExtendedIndices = GraphBuilder.CreateSRV(ExtendedIndicesBuffer);
        ApplyParams->IsSeedFlags = GraphBuilder.CreateSRV(IsSeedFlagsBuffer);
        ApplyParams->IsBoundarySeedFlags = IsBoundarySeedFlagsSRV;
        ApplyParams->IsBarrierFlags = IsBarrierFlagsSRV;
        ApplyParams->AdjacencyData = GraphBuilder.CreateSRV(AdjacencyDataBuffer);
        ApplyParams->RepresentativeIndices = RepresentativeIndicesSRV;
        ApplyParams->NumExtendedVertices = Params.NumExtendedVertices;
        ApplyParams->HeatLambda = Params.HeatLambda;

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("HeatPropagation_Apply"),
            ComputeShader,
            ApplyParams,
            FIntVector(NumGroups, 1, 1)
        );
    }
}
