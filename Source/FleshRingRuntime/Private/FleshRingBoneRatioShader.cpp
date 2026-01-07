// ============================================================================
// FleshRing Bone Ratio Preserve Shader - Implementation
// ============================================================================

#include "FleshRingBoneRatioShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"

// ============================================================================
// Shader Implementation Registration
// ============================================================================

IMPLEMENT_GLOBAL_SHADER(
    FFleshRingBoneRatioCS,
    "/Plugin/FleshRingPlugin/FleshRingBoneRatioCS.usf",
    "MainCS",
    SF_Compute
);

// ============================================================================
// Dispatch Function
// ============================================================================

void DispatchFleshRingBoneRatioCS(
    FRDGBuilder& GraphBuilder,
    const FBoneRatioDispatchParams& Params,
    FRDGBufferRef InputPositionsBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef OriginalBoneDistancesBuffer,
    FRDGBufferRef AxisHeightsBuffer,
    FRDGBufferRef SliceDataBuffer)
{
    // Early out if no vertices to process
    if (Params.NumAffectedVertices == 0)
    {
        return;
    }

    // Allocate shader parameters
    FFleshRingBoneRatioCS::FParameters* PassParameters =
        GraphBuilder.AllocParameters<FFleshRingBoneRatioCS::FParameters>();

    // Bind buffers
    PassParameters->InputPositions = GraphBuilder.CreateSRV(InputPositionsBuffer, PF_R32_FLOAT);
    PassParameters->OutputPositions = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);
    PassParameters->AffectedIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer);
    PassParameters->Influences = GraphBuilder.CreateSRV(InfluencesBuffer);
    PassParameters->OriginalBoneDistances = GraphBuilder.CreateSRV(OriginalBoneDistancesBuffer);
    PassParameters->AxisHeights = GraphBuilder.CreateSRV(AxisHeightsBuffer);
    PassParameters->SliceData = GraphBuilder.CreateSRV(SliceDataBuffer);

    // Set parameters
    PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
    PassParameters->NumTotalVertices = Params.NumTotalVertices;
    PassParameters->RingAxis = Params.RingAxis;
    PassParameters->RingCenter = Params.RingCenter;
    PassParameters->BlendStrength = Params.BlendStrength;
    PassParameters->HeightSigma = Params.HeightSigma;
    PassParameters->BoundsScale = Params.BoundsScale;

    // Get shader
    TShaderMapRef<FFleshRingBoneRatioCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // Calculate dispatch groups
    const uint32 ThreadGroupSize = 64;
    const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumAffectedVertices, ThreadGroupSize);

    // Add compute pass
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("FleshRingBoneRatioCS"),
        ComputeShader,
        PassParameters,
        FIntVector(static_cast<int32>(NumGroups), 1, 1)
    );
}
