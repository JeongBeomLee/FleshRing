// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Normal Recompute Shader
// ============================================================================
// Purpose: Recompute vertex normals using surface rotation method
//
// This shader runs AFTER TightnessCS and BulgeCS.
// It calculates the rotation from original to deformed face normals
// and applies this rotation to the original vertex normals.
// This preserves smooth shading while accounting for surface deformation.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"

// ============================================================================
// FFleshRingNormalRecomputeCS - Normal Recompute Compute Shader
// ============================================================================
class FFleshRingNormalRecomputeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFleshRingNormalRecomputeCS);
	SHADER_USE_PARAMETER_STRUCT(FFleshRingNormalRecomputeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// ===== Input Buffers (SRV - Read Only) =====

		// Input: Deformed vertex positions (from TightnessCS/BulgeCS)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, DeformedPositions)

		// Input: Original vertex positions (bind pose) - for calculating original face normals
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, OriginalPositions)

		// Input: Affected vertex indices to process
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AffectedVertexIndices)

		// Input: Adjacency offsets for each affected vertex
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AdjacencyOffsets)

		// Input: Flattened list of adjacent triangle indices
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AdjacencyTriangles)

		// Input: Mesh index buffer
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, IndexBuffer)

		// Input: Original tangents buffer (contains normals) - packed SNORM8x4 format
		// Format: TangentX (Index 0), TangentZ=Normal+BinormalSign (Index 1) per vertex
		SHADER_PARAMETER_SRV(Buffer<float4>, OriginalTangents)

		// ===== Output Buffer (UAV - Read/Write) =====

		// Output: Recomputed normals
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputNormals)

		// ===== Hop-based Blending (HopBased mode only) =====

		// Input: Hop distances for each affected vertex (optional, for blending)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, HopDistances)

		// ===== UV Seam Welding (optional) =====

		// Input: Representative vertex indices for UV seam welding
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RepresentativeIndices)

		// ===== Parameters =====

		SHADER_PARAMETER(uint32, NumAffectedVertices)
		SHADER_PARAMETER(uint32, NumTotalVertices)
		SHADER_PARAMETER(uint32, NormalRecomputeMode)  // 0 = Geometric, 1 = SurfaceRotation
		SHADER_PARAMETER(uint32, bEnableHopBlending)   // 0 = off, 1 = on (blend with original at boundary)
		SHADER_PARAMETER(uint32, MaxHops)              // Maximum hop distance (for blend factor calculation)
		SHADER_PARAMETER(uint32, FalloffType)          // 0 = Linear, 1 = Quadratic, 2 = Hermite
		SHADER_PARAMETER(uint32, bEnableUVSeamWelding) // 0 = off, 1 = on (use RepresentativeIndices for UV seam)
		SHADER_PARAMETER(uint32, bEnableDisplacementBlending) // 0 = off, 1 = on (blend based on vertex displacement)
		SHADER_PARAMETER(float, MaxDisplacement)       // Maximum displacement for blend (cm) - displacement >= this uses 100% recomputed normal
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
// FNormalRecomputeDispatchParams - Dispatch Parameters
// ============================================================================
struct FNormalRecomputeDispatchParams
{
	// Number of affected vertices to process
	uint32 NumAffectedVertices = 0;

	// Total mesh vertex count (for bounds checking)
	uint32 NumTotalVertices = 0;

	// Normal recompute mode (matches ENormalRecomputeMethod)
	// 0 = Geometric, 1 = SurfaceRotation
	uint32 NormalRecomputeMode = 1;  // Default: SurfaceRotation

	// ===== Hop-based Blending Parameters =====

	// Enable hop-based blending (blend with original normal at boundary)
	bool bEnableHopBlending = false;

	// Maximum hop distance (for blend factor calculation)
	uint32 MaxHops = 0;

	// Falloff type for blending (0=Linear, 1=Quadratic, 2=Hermite)
	uint32 FalloffType = 2;  // Default: Hermite

	// ===== UV Seam Welding Parameters =====

	// Enable UV seam welding (use RepresentativeIndices for UV seam)
	bool bEnableUVSeamWelding = false;

	// ===== Displacement-based Blending Parameters =====

	// Enable displacement-based blending (blend based on actual vertex movement)
	bool bEnableDisplacementBlending = false;

	// Maximum displacement for blend (cm) - vertices beyond this use 100% recomputed normal
	float MaxDisplacement = 1.0f;

	FNormalRecomputeDispatchParams() = default;

	FNormalRecomputeDispatchParams(uint32 InNumAffectedVertices, uint32 InNumTotalVertices, uint32 InNormalRecomputeMode = 2)
		: NumAffectedVertices(InNumAffectedVertices)
		, NumTotalVertices(InNumTotalVertices)
		, NormalRecomputeMode(InNormalRecomputeMode)
		, bEnableHopBlending(false)
		, MaxHops(0)
		, FalloffType(2)
		, bEnableUVSeamWelding(false)
		, bEnableDisplacementBlending(false)
		, MaxDisplacement(1.0f)
	{
	}
};

// ============================================================================
// Dispatch Function Declaration
// ============================================================================

/**
 * Dispatch NormalRecomputeCS to recalculate normals for affected vertices
 * using the surface rotation method.
 *
 * The shader calculates the rotation from original to deformed face normals
 * and applies this rotation to the original vertex normals, preserving smooth shading.
 *
 * @param GraphBuilder - RDG builder for resource management
 * @param Params - Dispatch parameters
 * @param DeformedPositionsBuffer - Deformed vertex positions (TightnessCS/BulgeCS output)
 * @param OriginalPositionsBuffer - Original bind pose vertex positions
 * @param AffectedVertexIndicesBuffer - Indices of affected vertices
 * @param AdjacencyOffsetsBuffer - Adjacency offsets for each affected vertex
 * @param AdjacencyTrianglesBuffer - Flattened list of adjacent triangle indices
 * @param IndexBuffer - Mesh index buffer (triangle indices)
 * @param SourceTangentsSRV - Original tangents buffer SRV (contains normals in TangentZ)
 * @param OutputNormalsBuffer - Output buffer for recomputed normals
 * @param HopDistancesBuffer - (Optional) Hop distances for blend factor calculation (nullptr if not using hop blending)
 * @param RepresentativeIndicesBuffer - (Optional) Representative indices for UV seam welding (nullptr if not using)
 */
void DispatchFleshRingNormalRecomputeCS(
	FRDGBuilder& GraphBuilder,
	const FNormalRecomputeDispatchParams& Params,
	FRDGBufferRef DeformedPositionsBuffer,
	FRDGBufferRef OriginalPositionsBuffer,
	FRDGBufferRef AffectedVertexIndicesBuffer,
	FRDGBufferRef AdjacencyOffsetsBuffer,
	FRDGBufferRef AdjacencyTrianglesBuffer,
	FRDGBufferRef IndexBuffer,
	FRHIShaderResourceView* SourceTangentsSRV,
	FRDGBufferRef OutputNormalsBuffer,
	FRDGBufferRef HopDistancesBuffer = nullptr,
	FRDGBufferRef RepresentativeIndicesBuffer = nullptr);
