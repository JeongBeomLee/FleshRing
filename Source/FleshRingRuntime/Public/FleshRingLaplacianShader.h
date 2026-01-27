// Copyright 2026 LgThx. All Rights Reserved.

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

        // Representative vertex indices for UV seam welding
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RepresentativeIndices)

        // Adjacency data (packed)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AdjacencyData)

        // Counts
        SHADER_PARAMETER(uint32, NumAffectedVertices)
        SHADER_PARAMETER(uint32, NumTotalVertices)

        // Smoothing parameters
        SHADER_PARAMETER(float, SmoothingLambda)

        // Per-vertex layer types (for excluding stocking from smoothing)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, VertexLayerTypes)

        // Exclude stocking layer from smoothing (0 = smooth all, 1 = exclude)
        SHADER_PARAMETER(uint32, bExcludeStockingFromSmoothing)

        // Anchor mode: skip smoothing for original affected (directly deformed) vertices
        SHADER_PARAMETER(uint32, bAnchorDeformedVertices)

        // Per-vertex anchor flags (1 = anchor/skip smoothing, 0 = apply smoothing)
        // Used when bAnchorDeformedVertices is enabled
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, IsAnchorFlags)
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

    /** Number of smoothing iterations */
    int32 NumIterations;

    /** Exclude stocking layer from smoothing */
    bool bExcludeStockingFromSmoothing;

    // ========================================
    // Anchor Mode Parameters
    // ========================================
    // Anchor mode makes original affected vertices (directly deformed by tightness)
    // act as fixed anchors, while only extended region vertices get smoothed.
    // Anchor determination: IsAnchorFlags buffer (1=anchor, 0=smooth)
    // - Hop-based: Seed vertices (hop=0) are anchors
    // - Z-based: Original SDF AABB vertices are anchors

    /** Enable anchor mode (original affected vertices skip smoothing) */
    bool bAnchorDeformedVertices;

    // ========================================
    // Taubin Smoothing Parameters
    // ========================================
    // Taubin smoothing prevents shrinkage by alternating λ (shrink) and μ (expand)
    // Mathematical basis: Acts as a band-pass filter
    //   - Low frequencies (overall shape) preserved → No shrinkage
    //   - High frequencies (noise, bumps) attenuated → Smoothing
    //
    // Typical values: λ = 0.5, μ = -0.53
    // Requirement: μ < -λ (|μ| > λ)

    /** Enable Taubin smoothing (alternating λ-μ passes) instead of standard Laplacian */
    bool bUseTaubinSmoothing;

    /**
     * Taubin expansion factor (negative value)
     * Must satisfy: TaubinMu < -SmoothingLambda
     * Typical: -0.53 when Lambda = 0.5
     *
     * If set to 0, will be auto-calculated as: -(Lambda + 0.01)
     */
    float TaubinMu;

    // ========================================
    // Lambda/Mu Safety Limits
    // ========================================
    // λ > 0.8 causes numerical instability:
    //   λ = 1.0 → vertex moves 100% to neighbor average → structure collapse
    //   μ = -1.01 → vertex overshoots 201% → oscillation/scaly mesh
    // Safe range: λ ∈ [0.1, 0.8], typical: 0.5
    static constexpr float MaxSafeLambda = 0.8f;
    static constexpr float MinSafeLambda = 0.1f;

    FLaplacianDispatchParams()
        : NumAffectedVertices(0)
        , NumTotalVertices(0)
        , SmoothingLambda(0.5f)
        , NumIterations(2)
        , bExcludeStockingFromSmoothing(true)  // Default: exclude stocking from smoothing
        , bAnchorDeformedVertices(false)       // Default: smooth all vertices (original behavior)
        , bUseTaubinSmoothing(true)            // Default: use Taubin for shrinkage-free smoothing
        , TaubinMu(-0.53f)                     // Typical value for λ=0.5
    {
    }

    /**
     * Get effective (safe) Lambda value
     * Clamps to [0.1, 0.8] to prevent numerical instability
     */
    float GetEffectiveLambda() const
    {
        return FMath::Clamp(SmoothingLambda, MinSafeLambda, MaxSafeLambda);
    }

    /**
     * Calculate effective Mu value (auto-calculate if not valid)
     * Uses clamped Lambda for calculation
     */
    float GetEffectiveTaubinMu() const
    {
        const float EffectiveLambda = GetEffectiveLambda();

        // Check if current Mu is valid (μ < -λ)
        if (TaubinMu >= 0.0f || TaubinMu > -EffectiveLambda)
        {
            // Auto-calculate: μ = -(λ + small margin)
            // Smaller margin = more stability, less smoothing power
            const float Margin = EffectiveLambda * 0.06f;  // ~3% margin
            return -(EffectiveLambda + Margin);
        }
        return TaubinMu;
    }

    /** Check if Lambda needs clamping (for warning) */
    bool NeedsLambdaClamping() const
    {
        return SmoothingLambda > MaxSafeLambda || SmoothingLambda < MinSafeLambda;
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
 * @param RepresentativeIndicesBuffer - Representative vertex indices for UV seam welding (nullptr = use AffectedIndices)
 * @param AdjacencyDataBuffer - Packed adjacency data
 * @param VertexLayerTypesBuffer - Per-vertex layer types (optional, nullptr if not excluding stocking)
 * @param IsAnchorFlagsBuffer - Per-vertex anchor flags (optional, nullptr disables anchor mode)
 */
void DispatchFleshRingLaplacianCS(
    FRDGBuilder& GraphBuilder,
    const FLaplacianDispatchParams& Params,
    FRDGBufferRef InputPositionsBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef RepresentativeIndicesBuffer,
    FRDGBufferRef AdjacencyDataBuffer,
    FRDGBufferRef VertexLayerTypesBuffer,
    FRDGBufferRef IsAnchorFlagsBuffer);

/**
 * Dispatch multiple iterations of Laplacian smoothing
 * Uses ping-pong buffers internally
 *
 * @param GraphBuilder - RDG builder
 * @param Params - Dispatch parameters (NumIterations used)
 * @param PositionsBuffer - Position buffer (in-place smoothing)
 * @param AffectedIndicesBuffer - Affected vertex indices
 * @param InfluencesBuffer - Per-vertex influence weights
 * @param RepresentativeIndicesBuffer - Representative vertex indices for UV seam welding (nullptr = use AffectedIndices)
 * @param AdjacencyDataBuffer - Packed adjacency data
 * @param VertexLayerTypesBuffer - Per-vertex layer types (optional)
 * @param IsAnchorFlagsBuffer - Per-vertex anchor flags (optional, nullptr disables anchor mode)
 */
void DispatchFleshRingLaplacianCS_MultiPass(
    FRDGBuilder& GraphBuilder,
    const FLaplacianDispatchParams& Params,
    FRDGBufferRef PositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef RepresentativeIndicesBuffer,
    FRDGBufferRef AdjacencyDataBuffer,
    FRDGBufferRef VertexLayerTypesBuffer,
    FRDGBufferRef IsAnchorFlagsBuffer);
