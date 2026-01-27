// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Tangent Recompute Shader
// ============================================================================
// Purpose: Recompute tangents using Gram-Schmidt orthonormalization
//
// This shader runs AFTER NormalRecomputeCS.
// Uses recomputed normals to orthonormalize tangents.
//
// Algorithm (Gram-Schmidt):
// 1. Read recomputed normal N
// 2. Read original tangent T
// 3. T' = T - (T·N)N  (project out N component)
// 4. T' = normalize(T')
// 5. Preserve original binormal sign
// ============================================================================

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"

// ============================================================================
// FFleshRingTangentRecomputeCS - Tangent Recompute Compute Shader
// ============================================================================
class FFleshRingTangentRecomputeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFleshRingTangentRecomputeCS);
	SHADER_USE_PARAMETER_STRUCT(FFleshRingTangentRecomputeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// ===== Input Buffers (SRV - Read Only) =====

		// Input: Recomputed normals from NormalRecomputeCS
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, RecomputedNormals)

		// Input: Original tangent buffer (TangentX=Normal, TangentZ=Tangent)
		// Format: 2 x SNORM8x4 per vertex (from StaticMeshVertexBuffer)
		SHADER_PARAMETER_SRV(Buffer<float4>, OriginalTangents)

		// Input: Affected vertex indices to process
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AffectedVertexIndices)

		// ===== Output Buffer (UAV - Read/Write) =====

		// Output: Recomputed tangents (float4 x 2 per vertex: TangentX, TangentZ)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputTangents)

		// ===== Parameters =====

		SHADER_PARAMETER(uint32, NumAffectedVertices)
		SHADER_PARAMETER(uint32, NumTotalVertices)
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
// FTangentRecomputeDispatchParams - Dispatch Parameters
// ============================================================================
struct FTangentRecomputeDispatchParams
{
	// Number of affected vertices to process
	uint32 NumAffectedVertices = 0;

	// Total mesh vertex count (for bounds checking)
	uint32 NumTotalVertices = 0;

	FTangentRecomputeDispatchParams() = default;

	FTangentRecomputeDispatchParams(uint32 InNumAffectedVertices, uint32 InNumTotalVertices)
		: NumAffectedVertices(InNumAffectedVertices)
		, NumTotalVertices(InNumTotalVertices)
	{
	}
};

// ============================================================================
// Dispatch Function Declaration
// ============================================================================

/**
 * Dispatch TangentRecomputeCS to orthonormalize tangents for affected vertices (Gram-Schmidt)
 *
 * @param GraphBuilder - RDG builder for resource management
 * @param Params - Dispatch parameters
 * @param RecomputedNormalsBuffer - Recomputed normals from NormalRecomputeCS (RDG)
 * @param OriginalTangentsSRV - Original tangent buffer SRV (RHI, from StaticMeshVertexBuffer)
 * @param AffectedVertexIndicesBuffer - Indices of affected vertices (RDG)
 * @param OutputTangentsBuffer - Output buffer for recomputed tangents (RDG)
 */
void DispatchFleshRingTangentRecomputeCS(
	FRDGBuilder& GraphBuilder,
	const FTangentRecomputeDispatchParams& Params,
	FRDGBufferRef RecomputedNormalsBuffer,
	FRHIShaderResourceView* OriginalTangentsSRV,
	FRDGBufferRef AffectedVertexIndicesBuffer,
	FRDGBufferRef OutputTangentsBuffer);
