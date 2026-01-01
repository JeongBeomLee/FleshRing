// ============================================================================
// FleshRing PBD Edge Constraint Shader
// ============================================================================
// Purpose: Maintain edge lengths after deformation (prevent stretching/shrinking)
// Uses influence-weighted PBD to propagate deformation outward
//
// Key Concept ("Inverse PBD"):
//   - High influence vertices (deformed by SDF): FIXED (anchors)
//   - Low influence vertices (boundary/outside): FREE to move
//   - Edge constraint pulls free vertices to maintain rest lengths
//
// Algorithm (per vertex, per neighbor):
//   Weight = 1.0 - Influence  (high influence = fixed)
//   Error = CurrentEdgeLength - RestLength
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

		// Per-vertex influences (for weight calculation)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, Influences)

		// Adjacency data with rest lengths
		// Format per vertex: [Count, Neighbor0, RestLen0, Neighbor1, RestLen1, ...]
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AdjacencyWithRestLengths)

		// Full mesh influence map (for neighbor weight lookup)
		// Indexed by absolute vertex index, not thread index
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, FullInfluenceMap)

		// Counts
		SHADER_PARAMETER(uint32, NumAffectedVertices)
		SHADER_PARAMETER(uint32, NumTotalVertices)

		// PBD parameters
		SHADER_PARAMETER(float, Stiffness)
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

	FPBDEdgeDispatchParams()
		: NumAffectedVertices(0)
		, NumTotalVertices(0)
		, Stiffness(0.8f)
		, NumIterations(3)
	{
	}
};

// ============================================================================
// Dispatch Functions
// ============================================================================

/**
 * Dispatch single pass of PBD edge constraint shader
 *
 * @param GraphBuilder - RDG builder
 * @param Params - Dispatch parameters
 * @param InputPositionsBuffer - Source positions (read)
 * @param OutputPositionsBuffer - Destination positions (write)
 * @param AffectedIndicesBuffer - Affected vertex indices
 * @param InfluencesBuffer - Per-vertex influence weights
 * @param AdjacencyWithRestLengthsBuffer - Packed adjacency with rest lengths
 * @param FullInfluenceMapBuffer - Full mesh influence map for neighbor lookup
 */
void DispatchFleshRingPBDEdgeCS(
	FRDGBuilder& GraphBuilder,
	const FPBDEdgeDispatchParams& Params,
	FRDGBufferRef InputPositionsBuffer,
	FRDGBufferRef OutputPositionsBuffer,
	FRDGBufferRef AffectedIndicesBuffer,
	FRDGBufferRef InfluencesBuffer,
	FRDGBufferRef AdjacencyWithRestLengthsBuffer,
	FRDGBufferRef FullInfluenceMapBuffer);

/**
 * Dispatch multiple iterations of PBD edge constraints
 * Uses ping-pong buffers internally
 *
 * @param GraphBuilder - RDG builder
 * @param Params - Dispatch parameters (NumIterations used)
 * @param PositionsBuffer - Position buffer (in-place, uses ping-pong internally)
 * @param AffectedIndicesBuffer - Affected vertex indices
 * @param InfluencesBuffer - Per-vertex influence weights
 * @param AdjacencyWithRestLengthsBuffer - Packed adjacency with rest lengths
 * @param FullInfluenceMapBuffer - Full mesh influence map for neighbor lookup
 */
void DispatchFleshRingPBDEdgeCS_MultiPass(
	FRDGBuilder& GraphBuilder,
	const FPBDEdgeDispatchParams& Params,
	FRDGBufferRef PositionsBuffer,
	FRDGBufferRef AffectedIndicesBuffer,
	FRDGBufferRef InfluencesBuffer,
	FRDGBufferRef AdjacencyWithRestLengthsBuffer,
	FRDGBufferRef FullInfluenceMapBuffer);
