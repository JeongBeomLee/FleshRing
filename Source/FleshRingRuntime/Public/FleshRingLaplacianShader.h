// ============================================================================
// FleshRing Laplacian Smoothing Shader
// ============================================================================
// Purpose: Smooth jagged boundaries from tightness deformation
// Applies Laplacian smoothing as post-process after TightnessCS
//
// Problem Solved:
//   TightnessCS moves vertices independently based on SDF
//   Vertices outside SDF don't move, creating jagged boundaries
//   Laplacian smoothing propagates movement to neighbors
//
// Algorithm:
//   NewPos = CurrentPos + Lambda * (NeighborAverage - CurrentPos)
//   Repeated for multiple iterations if needed

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"

// Maximum neighbors per vertex (must match shader)
#define FLESHRING_MAX_NEIGHBORS 12

// ============================================================================
// Adjacency Data Structure
// ============================================================================

/**
 * Per-vertex adjacency data for GPU
 * Packed format: [NeighborCount, Neighbor0, ..., Neighbor11]
 */
struct FVertexAdjacency
{
    /** Number of valid neighbors (0-12) */
    uint32 NeighborCount;

    /** Neighbor vertex indices (unused slots = 0) */
    uint32 NeighborIndices[FLESHRING_MAX_NEIGHBORS];

    FVertexAdjacency()
        : NeighborCount(0)
    {
        FMemory::Memzero(NeighborIndices, sizeof(NeighborIndices));
    }

    /** Get packed size in uint32s */
    static constexpr uint32 GetPackedSize()
    {
        return 1 + FLESHRING_MAX_NEIGHBORS; // Count + 12 indices = 13
    }

    /** Pack into flat array for GPU upload */
    void PackInto(TArray<uint32>& OutData) const
    {
        OutData.Add(NeighborCount);
        for (int32 i = 0; i < FLESHRING_MAX_NEIGHBORS; ++i)
        {
            OutData.Add(NeighborIndices[i]);
        }
    }
};

// ============================================================================
// FFleshRingLaplacianCS - Laplacian Smoothing Compute Shader
// ============================================================================

class FFleshRingLaplacianCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FFleshRingLaplacianCS);
    SHADER_USE_PARAMETER_STRUCT(FFleshRingLaplacianCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // Input positions (read)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, InputPositions)

        // Output positions (write)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPositions)

        // Affected vertex indices
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AffectedIndices)

        // Per-vertex influences
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, Influences)

        // Adjacency data (packed)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AdjacencyData)

        // Counts
        SHADER_PARAMETER(uint32, NumAffectedVertices)
        SHADER_PARAMETER(uint32, NumTotalVertices)

        // Smoothing parameters
        SHADER_PARAMETER(float, SmoothingLambda)
        SHADER_PARAMETER(float, VolumePreservation)
    END_SHADER_PARAMETER_STRUCT()

    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
        OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);
        OutEnvironment.SetDefine(TEXT("MAX_NEIGHBORS"), FLESHRING_MAX_NEIGHBORS);
    }
};

// ============================================================================
// FLaplacianDispatchParams - Dispatch Parameters
// ============================================================================

struct FLaplacianDispatchParams
{
    /** Number of affected vertices to process */
    uint32 NumAffectedVertices;

    /** Total mesh vertex count (for bounds checking) */
    uint32 NumTotalVertices;

    /** Smoothing strength (0-1, typical: 0.3-0.7) */
    float SmoothingLambda;

    /** Volume preservation factor (0-1) */
    float VolumePreservation;

    /** Number of smoothing iterations */
    int32 NumIterations;

    FLaplacianDispatchParams()
        : NumAffectedVertices(0)
        , NumTotalVertices(0)
        , SmoothingLambda(0.5f)
        , VolumePreservation(0.3f)
        , NumIterations(2)
    {
    }
};

// ============================================================================
// Dispatch Function
// ============================================================================

/**
 * Dispatch Laplacian smoothing compute shader
 *
 * @param GraphBuilder - RDG builder
 * @param Params - Dispatch parameters
 * @param InputPositionsBuffer - Source positions (from TightnessCS output)
 * @param OutputPositionsBuffer - Destination positions
 * @param AffectedIndicesBuffer - Affected vertex indices
 * @param InfluencesBuffer - Per-vertex influence weights
 * @param AdjacencyDataBuffer - Packed adjacency data
 */
void DispatchFleshRingLaplacianCS(
    FRDGBuilder& GraphBuilder,
    const FLaplacianDispatchParams& Params,
    FRDGBufferRef InputPositionsBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef AdjacencyDataBuffer);

/**
 * Dispatch multiple iterations of Laplacian smoothing
 * Uses ping-pong buffers internally
 *
 * @param GraphBuilder - RDG builder
 * @param Params - Dispatch parameters (NumIterations used)
 * @param PositionsBuffer - Position buffer (in-place smoothing)
 * @param AffectedIndicesBuffer - Affected vertex indices
 * @param InfluencesBuffer - Per-vertex influence weights
 * @param AdjacencyDataBuffer - Packed adjacency data
 */
void DispatchFleshRingLaplacianCS_MultiPass(
    FRDGBuilder& GraphBuilder,
    const FLaplacianDispatchParams& Params,
    FRDGBufferRef PositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef AdjacencyDataBuffer);
