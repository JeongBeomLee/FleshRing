// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Skinning Shader - Implementation
// ============================================================================
// Purpose: Apply GPU skinning to cached TightenedBindPose

#include "FleshRingSkinningShader.h"

#include "CoreMinimal.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"
#include "DataDrivenShaderPlatformInfo.h"  // For IsOpenGLPlatform

// ============================================================================
// Shader Implementation Registration
// ============================================================================
IMPLEMENT_GLOBAL_SHADER(
    FFleshRingSkinningCS,
    "/Plugin/FleshRingPlugin/FleshRingSkinningCS.usf",
    "MainCS",
    SF_Compute
);

// ============================================================================
// Dispatch Function Implementation
// ============================================================================

void DispatchFleshRingSkinningCS(
    FRDGBuilder& GraphBuilder,
    const FSkinningDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRHIShaderResourceView* SourceTangentsSRV,
    FRDGBufferRef OutputPositionsBuffer,
    FRDGBufferRef OutputPreviousPositionsBuffer,
    FRDGBufferRef OutputTangentsBuffer,
    FRHIShaderResourceView* BoneMatricesSRV,
    FRHIShaderResourceView* PreviousBoneMatricesSRV,
    FRHIShaderResourceView* InputWeightStreamSRV,
    FRDGBufferRef RecomputedNormalsBuffer,
    FRDGBufferRef RecomputedTangentsBuffer)
{
    // Early out if no vertices to process
    if (Params.NumVertices == 0)
    {
        return;
    }

    // Early out if skinning buffers are not available
    if (!BoneMatricesSRV || !InputWeightStreamSRV)
    {
        UE_LOG(LogTemp, Warning, TEXT("FleshRingSkinningCS: Missing skinning buffers"));
        return;
    }

    // Tangent processing is optional - allow position-only skinning
    const bool bProcessTangents = (OutputTangentsBuffer != nullptr);

    // Previous Position processing for TAA/TSR velocity
    const bool bProcessPreviousPosition = (OutputPreviousPositionsBuffer != nullptr) && (PreviousBoneMatricesSRV != nullptr);

    // Recomputed normals processing - use NormalRecomputeCS output for deformed vertices
    const bool bUseRecomputedNormals = (RecomputedNormalsBuffer != nullptr);

    // Recomputed tangents processing - use TangentRecomputeCS output (Gram-Schmidt orthonormalized)
    const bool bUseRecomputedTangents = (RecomputedTangentsBuffer != nullptr);

    // Allocate shader parameters
    FFleshRingSkinningCS::FParameters* PassParameters =
        GraphBuilder.AllocParameters<FFleshRingSkinningCS::FParameters>();

    // ===== Bind input buffers (SRV) =====
    // TightenedBindPose (cached positions)
    PassParameters->SourcePositions = GraphBuilder.CreateSRV(SourcePositionsBuffer, PF_R32_FLOAT);

    // ===== Bind output buffers (UAV) =====
    PassParameters->OutputPositions = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);

    // Previous Position output buffer for TAA/TSR velocity
    if (bProcessPreviousPosition)
    {
        PassParameters->OutputPreviousPositions = GraphBuilder.CreateUAV(OutputPreviousPositionsBuffer, PF_R32_FLOAT);
    }
    else
    {
        // Dummy buffer - use Position buffer as placeholder (won't be written)
        PassParameters->OutputPreviousPositions = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);
    }

    // Tangent buffers - RDG requires all declared parameters to be bound
    PassParameters->SourceTangents = SourceTangentsSRV;

    // Recomputed normals buffer (optional, from NormalRecomputeCS)
    if (bUseRecomputedNormals)
    {
        PassParameters->RecomputedNormals = GraphBuilder.CreateSRV(RecomputedNormalsBuffer, PF_R32_FLOAT);
    }
    else
    {
        // Dummy buffer - use Position buffer as placeholder (won't be read if bUseRecomputedNormals=0)
        PassParameters->RecomputedNormals = GraphBuilder.CreateSRV(SourcePositionsBuffer, PF_R32_FLOAT);
    }

    // Recomputed tangents buffer (optional, from TangentRecomputeCS)
    if (bUseRecomputedTangents)
    {
        PassParameters->RecomputedTangents = GraphBuilder.CreateSRV(RecomputedTangentsBuffer, PF_R32_FLOAT);
    }
    else
    {
        // Dummy buffer - use Position buffer as placeholder (won't be read if bUseRecomputedTangents=0)
        PassParameters->RecomputedTangents = GraphBuilder.CreateSRV(SourcePositionsBuffer, PF_R32_FLOAT);
    }

    if (bProcessTangents)
    {
        // Real tangent output buffer - Optimus approach
        // Format: PF_R16G16B16A16_SNORM (non-OpenGL) or PF_R16G16B16A16_SINT (OpenGL)
        // Matches GpuSkinCommon.ush TANGENT_RWBUFFER_FORMAT
        const EPixelFormat TangentsFormat = IsOpenGLPlatform(GMaxRHIShaderPlatform)
            ? PF_R16G16B16A16_SINT
            : PF_R16G16B16A16_SNORM;
        PassParameters->OutputTangents = GraphBuilder.CreateUAV(OutputTangentsBuffer, TangentsFormat);
    }
    else
    {
        // Dummy output tangent buffer - use Position buffer as dummy (won't be written)
        PassParameters->OutputTangents = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);
    }

    // ===== Bind skinning buffers (RHI SRV directly) =====
    // BoneMatrices: RefToLocal matrix (3 float4 per bone)
    // [Bind Pose Component Space] -> [Animated Component Space]
    PassParameters->BoneMatrices = BoneMatricesSRV;

    // Previous frame bone matrices for velocity calculation
    if (bProcessPreviousPosition)
    {
        PassParameters->PreviousBoneMatrices = PreviousBoneMatricesSRV;
    }
    else
    {
        // Use current frame as fallback (no velocity)
        PassParameters->PreviousBoneMatrices = BoneMatricesSRV;
    }

    PassParameters->InputWeightStream = InputWeightStreamSRV;

    // ===== Skinning parameters =====
    PassParameters->InputWeightStride = Params.InputWeightStride;
    PassParameters->InputWeightIndexSize = Params.InputWeightIndexSize;
    PassParameters->NumBoneInfluences = Params.NumBoneInfluences;

    // ===== Section parameters (like WaveCS) =====
    PassParameters->BaseVertexIndex = Params.BaseVertexIndex;
    PassParameters->NumVertices = Params.NumVertices;

    // ===== Debug/Feature flags =====
    PassParameters->bProcessTangents = bProcessTangents ? 1 : 0;
    PassParameters->bProcessPreviousPosition = bProcessPreviousPosition ? 1 : 0;
    PassParameters->bUseRecomputedNormals = bUseRecomputedNormals ? 1 : 0;
    PassParameters->bUseRecomputedTangents = bUseRecomputedTangents ? 1 : 0;
    PassParameters->bPassthroughSkinning = Params.bPassthroughSkinning ? 1 : 0;

    // Get shader reference
    TShaderMapRef<FFleshRingSkinningCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // Calculate dispatch groups based on Section's vertex count
    const uint32 ThreadGroupSize = 64; // Matches [numthreads(64,1,1)] in .usf
    const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumVertices, ThreadGroupSize);

    // Add compute pass to RDG
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("FleshRingSkinningCS (Section base=%d, %d verts, PrevPos=%d)",
            Params.BaseVertexIndex, Params.NumVertices, bProcessPreviousPosition ? 1 : 0),
        ComputeShader,
        PassParameters,
        FIntVector(static_cast<int32>(NumGroups), 1, 1)
    );
}
