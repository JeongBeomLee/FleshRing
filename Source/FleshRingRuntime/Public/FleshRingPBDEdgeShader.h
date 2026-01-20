// ============================================================================
// FleshRing PBD Edge Constraint Shader
// ============================================================================
// Purpose: Maintain edge lengths after deformation (prevent extreme stretching/compression)
// Uses tolerance-based PBD to preserve intentional deformation while preventing artifacts
//
// Key Concept (Tolerance-based PBD):
//   - Affected Vertices (Tightness region): FIXED (anchors) - no movement
//   - Non-Affected Vertices (extended region): FREE to move within tolerance
//   - Edge constraint only applies when length is outside tolerance range
//
// Algorithm (per vertex, per neighbor):
//   Tolerance range = [RestLength * (1-Tolerance), RestLength * (1+Tolerance)]
//   If CurrentLength within range: Error = 0 (preserve deformation)
//   If outside range: Error = distance to nearest boundary
//   Weight: Anchor = 0 (fixed), Non-Anchor = 1 (free)
//   Correction = Direction * Error * (MyWeight / TotalWeight)
//   NewPos = CurrentPos + Correction * Stiffness

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"

// Maximum neighbors per vertex (must match shader and FleshRingLaplacianShader.h)
#ifndef FLESHRING_MAX_NEIGHBORS
#define FLESHRING_MAX_NEIGHBORS 12
#endif

// ============================================================================
// FFleshRingPBDEdgeCS - PBD Edge Constraint Compute Shader
// ============================================================================

class FFleshRingPBDEdgeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFleshRingPBDEdgeCS);
	SHADER_USE_PARAMETER_STRUCT(FFleshRingPBDEdgeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input positions (read from previous iteration)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, InputPositions)

		// Output positions (write for this iteration)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPositions)

		// Affected vertex indices (smoothing region)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AffectedIndices)

		// UV Seam Welding: Representative vertex indices
		// 대표 버텍스 인덱스 (UV seam 용접용)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RepresentativeIndices)

		// Per-vertex anchor flags (1 = Affected/Anchor, 0 = Non-Affected/Free)
		// Affected Vertices (Tightness 영역)는 고정, 나머지는 자유롭게 이동
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, IsAnchorFlags)

		// Full mesh anchor map (indexed by absolute vertex index)
		// For neighbor anchor lookup (neighbors might not be in current region)
		// 이웃의 앵커 여부 조회용 전체 메시 크기 맵
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FullVertexAnchorFlags)

		// Adjacency data with rest lengths
		// Format per vertex: [Count, Neighbor0, RestLen0, Neighbor1, RestLen1, ...]
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AdjacencyWithRestLengths)

		// Counts
		SHADER_PARAMETER(uint32, NumAffectedVertices)
		SHADER_PARAMETER(uint32, NumTotalVertices)

		// PBD parameters
		SHADER_PARAMETER(float, Stiffness)

		// Tolerance ratio (0.0 ~ 0.5)
		// Allowed range: [RestLength * (1-Tolerance), RestLength * (1+Tolerance)]
		// 예: Tolerance=0.2 → 원래 길이의 80%~120% 범위 허용
		SHADER_PARAMETER(float, Tolerance)
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
// FPBDEdgeDispatchParams - Dispatch Parameters
// ============================================================================

struct FPBDEdgeDispatchParams
{
	/** Number of affected vertices to process */
	uint32 NumAffectedVertices;

	/** Total mesh vertex count */
	uint32 NumTotalVertices;

	/** Constraint stiffness (0-1, higher = stronger constraint) */
	float Stiffness;

	/** Number of solver iterations */
	int32 NumIterations;

	/** Tolerance ratio (0.0 ~ 0.5)
	 *  Allowed range: [RestLength * (1-Tolerance), RestLength * (1+Tolerance)]
	 *  예: Tolerance=0.2 → 원래 길이의 80%~120% 범위 허용
	 */
	float Tolerance;

	FPBDEdgeDispatchParams()
		: NumAffectedVertices(0)
		, NumTotalVertices(0)
		, Stiffness(0.8f)
		, NumIterations(3)
		, Tolerance(0.2f)
	{
	}
};

// ============================================================================
// Dispatch Functions
// ============================================================================

/**
 * Dispatch single pass of PBD edge constraint shader (Tolerance-based)
 *
 * @param GraphBuilder - RDG builder
 * @param Params - Dispatch parameters
 * @param InputPositionsBuffer - Source positions (read)
 * @param OutputPositionsBuffer - Destination positions (write)
 * @param AffectedIndicesBuffer - Affected vertex indices
 * @param RepresentativeIndicesBuffer - Representative vertex indices for UV seam welding (nullptr = use AffectedIndices)
 * @param IsAnchorFlagsBuffer - Per-vertex anchor flags (1=anchor, 0=free)
 * @param FullVertexAnchorFlagsBuffer - Full mesh anchor map for neighbor lookup
 * @param AdjacencyWithRestLengthsBuffer - Packed adjacency with rest lengths
 */
void DispatchFleshRingPBDEdgeCS(
	FRDGBuilder& GraphBuilder,
	const FPBDEdgeDispatchParams& Params,
	FRDGBufferRef InputPositionsBuffer,
	FRDGBufferRef OutputPositionsBuffer,
	FRDGBufferRef AffectedIndicesBuffer,
	FRDGBufferRef RepresentativeIndicesBuffer,
	FRDGBufferRef IsAnchorFlagsBuffer,
	FRDGBufferRef FullVertexAnchorFlagsBuffer,
	FRDGBufferRef AdjacencyWithRestLengthsBuffer);

/**
 * Dispatch multiple iterations of PBD edge constraints (Tolerance-based)
 * Uses ping-pong buffers internally
 *
 * @param GraphBuilder - RDG builder
 * @param Params - Dispatch parameters (NumIterations used)
 * @param PositionsBuffer - Position buffer (in-place, uses ping-pong internally)
 * @param AffectedIndicesBuffer - Affected vertex indices
 * @param RepresentativeIndicesBuffer - Representative vertex indices for UV seam welding (nullptr = use AffectedIndices)
 * @param IsAnchorFlagsBuffer - Per-vertex anchor flags (1=anchor, 0=free)
 * @param FullVertexAnchorFlagsBuffer - Full mesh anchor map for neighbor lookup
 * @param AdjacencyWithRestLengthsBuffer - Packed adjacency with rest lengths
 */
void DispatchFleshRingPBDEdgeCS_MultiPass(
	FRDGBuilder& GraphBuilder,
	const FPBDEdgeDispatchParams& Params,
	FRDGBufferRef PositionsBuffer,
	FRDGBufferRef AffectedIndicesBuffer,
	FRDGBufferRef RepresentativeIndicesBuffer,
	FRDGBufferRef IsAnchorFlagsBuffer,
	FRDGBufferRef FullVertexAnchorFlagsBuffer,
	FRDGBufferRef AdjacencyWithRestLengthsBuffer);
