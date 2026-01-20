// ============================================================================
// FleshRing Heat Propagation Shader
// ============================================================================
// Purpose: Propagate deformation delta from Seeds to Extended region
//
// Algorithm (Delta-based):
//   1. Init:    Seed.delta = CurrentPos - OriginalPos, Non-Seed.delta = 0
//   2. Diffuse: Non-Seed.delta = lerp(delta, neighborAvgDelta, lambda) x N
//   3. Apply:   Non-Seed.FinalPos = OriginalPos + delta

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"

// ============================================================================
// FFleshRingHeatPropagationCS - Heat Propagation Compute Shader
// ============================================================================

class FFleshRingHeatPropagationCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FFleshRingHeatPropagationCS);
    SHADER_USE_PARAMETER_STRUCT(FFleshRingHeatPropagationCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // Pass type: 0=Init, 1=Diffuse, 2=Apply
        SHADER_PARAMETER(uint32, PassType)

        // Position buffers (Init/Apply)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, OriginalPositions)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, CurrentPositions)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPositions)

        // Delta buffers (Init/Diffuse)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, DeltaIn)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, DeltaOut)

        // Extended region data
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ExtendedIndices)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, IsSeedFlags)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, IsBoundarySeedFlags)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, IsBarrierFlags)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AdjacencyData)

        // UV Seam Welding: Representative vertex indices
        // RepresentativeIndices[ThreadIndex] = representative vertex index for UV seam welding
        // All UV duplicates at same position share the same representative
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RepresentativeIndices)

        // Parameters
        SHADER_PARAMETER(uint32, NumExtendedVertices)
        SHADER_PARAMETER(float, HeatLambda)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);
        OutEnvironment.SetDefine(TEXT("MAX_NEIGHBORS"), 12);
    }
};

// ============================================================================
// FHeatPropagationDispatchParams - Dispatch Parameters
// ============================================================================

struct FHeatPropagationDispatchParams
{
    /** Number of extended region vertices */
    uint32 NumExtendedVertices;

    /** Total mesh vertices (for output buffer size) */
    uint32 NumTotalVertices;

    /** Heat propagation lambda (diffusion coefficient, 0.1~0.9) */
    float HeatLambda;

    /** Number of diffusion iterations (more = wider propagation) */
    int32 NumIterations;

    FHeatPropagationDispatchParams()
        : NumExtendedVertices(0)
        , NumTotalVertices(0)
        , HeatLambda(0.5f)
        , NumIterations(10)
    {
    }
};

// ============================================================================
// Dispatch Function
// ============================================================================

/**
 * Dispatch Heat Propagation compute shader (Init → Diffuse × N → Apply)
 *
 * @param GraphBuilder - RDG builder
 * @param Params - Dispatch parameters
 * @param OriginalPositionsBuffer - Original bind pose positions (SourceData)
 * @param CurrentPositionsBuffer - Current tightened positions (input, read-only)
 * @param OutputPositionsBuffer - Output positions (modified in-place for Non-Seed)
 * @param ExtendedIndicesBuffer - Extended region vertex indices
 * @param IsSeedFlagsBuffer - Seed flags (1=Seed, 0=Non-Seed)
 * @param IsBoundarySeedFlagsBuffer - Boundary Seed flags (1=Boundary Seed, 0=Internal Seed or Non-Seed)
 * @param IsBarrierFlagsBuffer - Barrier flags (1=Barrier/Tightness, 0=Non-Barrier) - blocks heat propagation
 * @param AdjacencyDataBuffer - Laplacian adjacency for Extended region
 * @param RepresentativeIndicesBuffer - Representative vertex indices for UV seam welding (nullptr = use ExtendedIndices)
 */
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
    FRDGBufferRef RepresentativeIndicesBuffer);
