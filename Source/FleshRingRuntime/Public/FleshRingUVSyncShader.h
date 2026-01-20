// ============================================================================
// FleshRing UV Sync Shader
// ============================================================================
// Purpose: Synchronize positions of UV duplicate vertices before Normal Recompute
//
// Problem Solved:
//   UV seam vertices may have slightly different positions after deformation.
//   This pass ensures all UV duplicates have identical positions.
//
// Algorithm:
//   Each vertex copies the position from its representative vertex.
//   Result: All UV duplicates share the exact same position.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"

// ============================================================================
// FFleshRingUVSyncCS - UV Sync Compute Shader
// ============================================================================

class FFleshRingUVSyncCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FFleshRingUVSyncCS);
    SHADER_USE_PARAMETER_STRUCT(FFleshRingUVSyncCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // In-place positions buffer (read/write)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, Positions)

        // Affected vertex indices
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AffectedIndices)

        // Representative vertex indices for UV seam welding
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RepresentativeIndices)

        // Number of affected vertices
        SHADER_PARAMETER(uint32, NumAffectedVertices)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);
    }
};

// ============================================================================
// FUVSyncDispatchParams - Dispatch Parameters
// ============================================================================

struct FUVSyncDispatchParams
{
    /** Number of affected vertices to process */
    uint32 NumAffectedVertices;

    FUVSyncDispatchParams()
        : NumAffectedVertices(0)
    {
    }

    explicit FUVSyncDispatchParams(uint32 InNumAffectedVertices)
        : NumAffectedVertices(InNumAffectedVertices)
    {
    }
};

// ============================================================================
// Dispatch Function
// ============================================================================

/**
 * Dispatch UV Sync compute shader
 *
 * @param GraphBuilder - RDG builder
 * @param Params - Dispatch parameters
 * @param PositionsBuffer - Vertex positions buffer (in-place modification)
 * @param AffectedIndicesBuffer - Indices of affected vertices
 * @param RepresentativeIndicesBuffer - Representative indices for UV welding
 */
void DispatchFleshRingUVSyncCS(
    FRDGBuilder& GraphBuilder,
    const FUVSyncDispatchParams& Params,
    FRDGBufferRef PositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef RepresentativeIndicesBuffer);
