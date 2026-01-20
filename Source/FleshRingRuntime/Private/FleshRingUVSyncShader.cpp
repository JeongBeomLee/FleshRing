// ============================================================================
// FleshRing UV Sync Shader - Implementation
// ============================================================================

#include "FleshRingUVSyncShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"

DECLARE_LOG_CATEGORY_EXTERN(LogFleshRingUVSync, Log, All);
DEFINE_LOG_CATEGORY(LogFleshRingUVSync);

// ============================================================================
// Shader Implementation Registration
// ============================================================================

IMPLEMENT_GLOBAL_SHADER(
    FFleshRingUVSyncCS,
    "/Plugin/FleshRingPlugin/FleshRingUVSyncCS.usf",
    "MainCS",
    SF_Compute
);

// ============================================================================
// Dispatch Function
// ============================================================================

void DispatchFleshRingUVSyncCS(
    FRDGBuilder& GraphBuilder,
    const FUVSyncDispatchParams& Params,
    FRDGBufferRef PositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef RepresentativeIndicesBuffer)
{
    // Early out if no vertices to process
    if (Params.NumAffectedVertices == 0)
    {
        return;
    }

    // Validate buffers
    if (!PositionsBuffer || !AffectedIndicesBuffer || !RepresentativeIndicesBuffer)
    {
        UE_LOG(LogFleshRingUVSync, Warning, TEXT("DispatchFleshRingUVSyncCS: Missing required buffer"));
        return;
    }

    // Get shader
    TShaderMapRef<FFleshRingUVSyncCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // Allocate shader parameters
    FFleshRingUVSyncCS::FParameters* PassParameters =
        GraphBuilder.AllocParameters<FFleshRingUVSyncCS::FParameters>();

    // Bind buffers
    PassParameters->Positions = GraphBuilder.CreateUAV(PositionsBuffer, PF_R32_FLOAT);
    PassParameters->AffectedIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer);
    PassParameters->RepresentativeIndices = GraphBuilder.CreateSRV(RepresentativeIndicesBuffer);
    PassParameters->NumAffectedVertices = Params.NumAffectedVertices;

    // Calculate dispatch dimensions
    const uint32 ThreadGroupSize = 64;
    const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumAffectedVertices, ThreadGroupSize);

    // Dispatch
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("FleshRing_UVSync"),
        ComputeShader,
        PassParameters,
        FIntVector(NumGroups, 1, 1)
    );

}
