// ============================================================================
// FleshRing Self-Collision Detection & Resolution Shader
// ============================================================================
// Purpose: Detect and resolve triangle-triangle intersections within SDF region
// Prevents mesh self-penetration (e.g., stocking penetrating through skin)
//
// Algorithm:
//   1. Check all triangle pairs in SDF region for intersection
//   2. For intersecting pairs, calculate separation vector
//   3. Move vertices apart to resolve penetration
//
// Performance Note:
//   - Only processes triangles within SDF bounds (typically 100-500)
//   - O(n²) but manageable for small n
//   - Can be optimized with spatial hashing if needed

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"

// ============================================================================
// FFleshRingCollisionDetectCS - Triangle Intersection Detection
// ============================================================================

class FFleshRingCollisionDetectCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FFleshRingCollisionDetectCS);
    SHADER_USE_PARAMETER_STRUCT(FFleshRingCollisionDetectCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // Vertex positions (after deformation)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, Positions)

        // Triangle indices (3 indices per triangle)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TriangleIndices)

        // Output: Collision pairs (triangleA, triangleB, penetrationDepth)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, CollisionPairs)

        // Output: Collision count (atomic counter)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CollisionCount)

        // Number of triangles to check
        SHADER_PARAMETER(uint32, NumTriangles)

        // Maximum collision pairs to record
        SHADER_PARAMETER(uint32, MaxCollisionPairs)
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
// FFleshRingCollisionResolveCS - Collision Resolution
// ============================================================================

class FFleshRingCollisionResolveCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FFleshRingCollisionResolveCS);
    SHADER_USE_PARAMETER_STRUCT(FFleshRingCollisionResolveCS, FGlobalShader);

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // Vertex positions (read/write) - matches USF: PositionsRW
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, PositionsRW)

        // Triangle indices
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TriangleIndices)

        // Collision pairs from detection pass - matches USF: CollisionPairsRead
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CollisionPairsRead)

        // Number of collisions detected - matches USF: CollisionCountRead
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CollisionCountRead)

        // Total vertex count (for bounds checking)
        SHADER_PARAMETER(uint32, NumTotalVertices)

        // Resolution strength (0-1)
        SHADER_PARAMETER(float, ResolutionStrength)
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
// FCollisionDispatchParams
// ============================================================================

struct FCollisionDispatchParams
{
    /** Number of triangles in SDF region */
    uint32 NumTriangles;

    /** Total mesh vertex count */
    uint32 NumTotalVertices;

    /** Maximum collision pairs to detect */
    uint32 MaxCollisionPairs;

    /** Resolution strength (0-1) */
    float ResolutionStrength;

    /** Number of resolution iterations */
    int32 NumIterations;

    FCollisionDispatchParams()
        : NumTriangles(0)
        , NumTotalVertices(0)
        , MaxCollisionPairs(1024)
        , ResolutionStrength(1.0f)
        , NumIterations(1)
    {
    }
};

// ============================================================================
// Dispatch Functions
// ============================================================================

/**
 * Dispatch collision detection and resolution
 *
 * @param GraphBuilder - RDG builder
 * @param Params - Dispatch parameters
 * @param PositionsBuffer - Vertex positions (will be modified)
 * @param TriangleIndicesBuffer - Triangle indices in SDF region
 */
void DispatchFleshRingCollisionCS(
    FRDGBuilder& GraphBuilder,
    const FCollisionDispatchParams& Params,
    FRDGBufferRef PositionsBuffer,
    FRDGBufferRef TriangleIndicesBuffer);
