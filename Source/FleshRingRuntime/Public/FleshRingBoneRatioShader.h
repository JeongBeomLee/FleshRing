// ============================================================================
// FleshRing Bone Ratio Preserve Shader
// ============================================================================
// Purpose: Equalize radial distance for same-slice vertices
// Solves "cracking" from non-uniform SDF sampling
//
// Algorithm:
//   1. Group vertices by height along ring axis (slices)
//   2. Calculate average deformation ratio per slice
//   3. Apply average ratio to all vertices in slice
//   Result: Same-height vertices have uniform radial distance

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"

// Maximum vertices per slice (must match shader)
#define FLESHRING_MAX_SLICE_VERTICES 32

// ============================================================================
// FFleshRingBoneRatioCS - Bone Ratio Preserve Compute Shader
// ============================================================================

class FFleshRingBoneRatioCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FFleshRingBoneRatioCS);
    SHADER_USE_PARAMETER_STRUCT(FFleshRingBoneRatioCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // Input positions (read)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, InputPositions)

        // Output positions (write)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPositions)

        // Affected vertex indices
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AffectedIndices)

        // Per-vertex influences
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, Influences)

        // Original bone distances (bind pose)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, OriginalBoneDistances)

        // Axis heights for Gaussian weighting
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, AxisHeights)

        // Slice data (packed)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SliceData)

        // Counts
        SHADER_PARAMETER(uint32, NumAffectedVertices)
        SHADER_PARAMETER(uint32, NumTotalVertices)

        // Ring geometry
        SHADER_PARAMETER(FVector3f, RingAxis)
        SHADER_PARAMETER(FVector3f, RingCenter)

        // Blend strength
        SHADER_PARAMETER(float, BlendStrength)

        // Height sigma for Gaussian weighting (bucket size)
        SHADER_PARAMETER(float, HeightSigma)

        // Bounds Scale (Z-direction only, for future Z falloff if needed)
        SHADER_PARAMETER(float, BoundsScale)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);
        OutEnvironment.SetDefine(TEXT("MAX_SLICE_VERTICES"), FLESHRING_MAX_SLICE_VERTICES);
    }
};

// ============================================================================
// FBoneRatioDispatchParams - Dispatch Parameters
// ============================================================================

struct FBoneRatioDispatchParams
{
    /** Number of affected vertices to process */
    uint32 NumAffectedVertices;

    /** Total mesh vertex count (for bounds checking) */
    uint32 NumTotalVertices;

    /** Ring axis direction (normalized) */
    FVector3f RingAxis;

    /** Ring center position */
    FVector3f RingCenter;

    /** Blend strength (0-1, default: 1.0 for full equalization) */
    float BlendStrength;

    /** Height sigma for Gaussian weighting (bucket size, default: 1.0cm) */
    float HeightSigma;

    /** Bounds scale for this pass (Z-direction only) */
    float BoundsScale;

    FBoneRatioDispatchParams()
        : NumAffectedVertices(0)
        , NumTotalVertices(0)
        , RingAxis(FVector3f::UpVector)
        , RingCenter(FVector3f::ZeroVector)
        , BlendStrength(1.0f)
        , HeightSigma(1.0f)
        , BoundsScale(1.5f)
    {
    }
};

// ============================================================================
// Dispatch Function
// ============================================================================

/**
 * Dispatch bone ratio preserve compute shader
 *
 * @param GraphBuilder - RDG builder
 * @param Params - Dispatch parameters
 * @param InputPositionsBuffer - Source positions (from Tightness/Bulge output)
 * @param OutputPositionsBuffer - Destination positions
 * @param AffectedIndicesBuffer - Affected vertex indices
 * @param InfluencesBuffer - Per-vertex influence weights
 * @param OriginalBoneDistancesBuffer - Original bone distances (bind pose)
 * @param AxisHeightsBuffer - Axis heights for Gaussian weighting
 * @param SliceDataBuffer - Packed slice data
 */
void DispatchFleshRingBoneRatioCS(
    FRDGBuilder& GraphBuilder,
    const FBoneRatioDispatchParams& Params,
    FRDGBufferRef InputPositionsBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef OriginalBoneDistancesBuffer,
    FRDGBufferRef AxisHeightsBuffer,
    FRDGBufferRef SliceDataBuffer);
