// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Layer Penetration Resolution Shader
// ============================================================================
// Purpose: Ensure outer layers (stocking) never penetrate inner layers (skin)
// Algorithm:
//   1. For each outer-layer vertex, find nearest inner-layer triangle
//   2. If vertex is below triangle surface, push it outward
//   3. Uses material-based layer hierarchy (Skin < Stocking < Underwear < Outerwear)
//
// Performance Note:
//   - Only processes vertices with different layer types
//   - Uses spatial acceleration for nearest triangle search
//   - Typical cost: O(n*m) where n=outer vertices, m=inner triangles

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "FleshRingTypes.h"

// ============================================================================
// Layer Type Constants (must match EFleshRingLayerType enum)
// ============================================================================
#define LAYER_TYPE_SKIN       0
#define LAYER_TYPE_STOCKING   1
#define LAYER_TYPE_UNDERWEAR  2
#define LAYER_TYPE_OUTERWEAR  3
#define LAYER_TYPE_UNKNOWN    4

// ============================================================================
// FFleshRingLayerPenetrationCS - Layer Penetration Resolution
// ============================================================================
// Main compute shader that ensures outer layers stay outside inner layers

class FFleshRingLayerPenetrationCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FFleshRingLayerPenetrationCS);
    SHADER_USE_PARAMETER_STRUCT(FFleshRingLayerPenetrationCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // Vertex positions (read/write)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, PositionsRW)

        // Vertex normals (for push direction)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, Normals)

        // Per-vertex layer types (0=Skin, 1=Stocking, etc.)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, VertexLayerTypes)

        // Affected vertex indices
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, AffectedIndices)

        // Triangle indices (3 indices per triangle)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TriangleIndices)

        // Per-triangle layer types (derived from dominant vertex layer)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TriangleLayerTypes)

        // Number of affected vertices
        SHADER_PARAMETER(uint32, NumAffectedVertices)

        // Number of triangles
        SHADER_PARAMETER(uint32, NumTriangles)

        // Minimum separation distance (to prevent z-fighting)
        SHADER_PARAMETER(float, MinSeparation)

        // Maximum push distance (to prevent extreme corrections)
        SHADER_PARAMETER(float, MaxPushDistance)

        // Ring center (for radial direction calculation)
        SHADER_PARAMETER(FVector3f, RingCenter)

        // Ring axis (for axial filtering)
        SHADER_PARAMETER(FVector3f, RingAxis)

        // Tightness strength (for dynamic separation calculation)
        SHADER_PARAMETER(float, TightnessStrength)

        // Push ratio for outer layer (0.0~1.0, e.g., 0.7 = 70% outward)
        SHADER_PARAMETER(float, OuterLayerPushRatio)

        // Push ratio for inner layer (0.0~1.0, e.g., 0.3 = 30% inward)
        SHADER_PARAMETER(float, InnerLayerPushRatio)
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
// FFleshRingBuildTriangleLayerCS - Build Per-Triangle Layer Types
// ============================================================================
// Pre-pass that determines each triangle's layer type from its vertices

class FFleshRingBuildTriangleLayerCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FFleshRingBuildTriangleLayerCS);
    SHADER_USE_PARAMETER_STRUCT(FFleshRingBuildTriangleLayerCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // Per-vertex layer types
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, VertexLayerTypes)

        // Triangle indices
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TriangleIndices)

        // Output: Per-triangle layer types
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, TriangleLayerTypesRW)

        // Number of triangles
        SHADER_PARAMETER(uint32, NumTriangles)
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
// FLayerPenetrationDispatchParams
// ============================================================================

struct FLayerPenetrationDispatchParams
{
    /** Number of affected vertices */
    uint32 NumAffectedVertices;

    /** Number of triangles in the region */
    uint32 NumTriangles;

    /** Minimum separation distance between layers (cm) */
    float MinSeparation;

    /** Maximum push distance to prevent extreme corrections (cm) */
    float MaxPushDistance;

    /** Ring center for radial direction */
    FVector3f RingCenter;

    /** Ring axis for filtering */
    FVector3f RingAxis;

    /** Number of iterations for convergence */
    int32 NumIterations;

    /** Current tightness strength (for dynamic separation) */
    float TightnessStrength;

    /** Push ratio for outer layer (0.0~1.0) */
    float OuterLayerPushRatio;

    /** Push ratio for inner layer (0.0~1.0) */
    float InnerLayerPushRatio;

    FLayerPenetrationDispatchParams()
        : NumAffectedVertices(0)
        , NumTriangles(0)
        , MinSeparation(0.05f)      // 0.5mm default separation
        , MaxPushDistance(2.0f)      // 2cm max push
        , RingCenter(FVector3f::ZeroVector)
        , RingAxis(FVector3f::UpVector)
        , NumIterations(3)
        , TightnessStrength(0.5f)   // Default mid-strength
        , OuterLayerPushRatio(0.7f) // 70% outward (stocking moves out)
        , InnerLayerPushRatio(0.3f) // 30% inward (skin gives way)
    {
    }
};

// ============================================================================
// Dispatch Functions
// ============================================================================

/**
 * Dispatch layer penetration resolution
 *
 * @param GraphBuilder - RDG builder
 * @param Params - Dispatch parameters
 * @param PositionsBuffer - Vertex positions (will be modified)
 * @param NormalsBuffer - Vertex normals (read-only)
 * @param VertexLayerTypesBuffer - Per-vertex layer types
 * @param AffectedIndicesBuffer - Affected vertex indices
 * @param TriangleIndicesBuffer - Triangle indices
 */
void DispatchFleshRingLayerPenetrationCS(
    FRDGBuilder& GraphBuilder,
    const FLayerPenetrationDispatchParams& Params,
    FRDGBufferRef PositionsBuffer,
    FRDGBufferRef NormalsBuffer,
    FRDGBufferRef VertexLayerTypesBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef TriangleIndicesBuffer);
