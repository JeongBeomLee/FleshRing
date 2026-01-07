// ============================================================================
// FleshRing Tangent Space Relaxation Shader
// ============================================================================
// Purpose: Redistribute vertices along tangent directions while preserving
//          normal-direction displacement (SDF deformation depth)
//          접선 방향으로 버텍스 재배치 (노멀 방향 변형은 유지)
//
// Use Case: After TightnessCS + PBDEdgeCS, vertices may cluster unevenly.
//           TangentRelaxCS improves vertex distribution uniformity.
//           TightnessCS + PBDEdgeCS 후 버텍스가 불균등하게 몰릴 수 있음.
//           TangentRelaxCS로 버텍스 분포 균일성 개선.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"

// Maximum neighbors per vertex (must match shader)
#ifndef FLESHRING_MAX_NEIGHBORS
#define FLESHRING_MAX_NEIGHBORS 12
#endif

// ============================================================================
// FFleshRingTangentRelaxCS - Tangent Space Relaxation Compute Shader
// ============================================================================

class FFleshRingTangentRelaxCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFleshRingTangentRelaxCS);
	SHADER_USE_PARAMETER_STRUCT(FFleshRingTangentRelaxCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input positions (read from previous pass)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, InputPositions)

		// Output positions (write for this pass)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPositions)

		// Affected vertex indices
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AffectedIndices)

		// Per-vertex deform amounts (for strength modulation)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, DeformAmounts)

		// Adjacency data (neighbor indices only, no rest lengths)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AdjacencyData)

		// Counts
		SHADER_PARAMETER(uint32, NumAffectedVertices)
		SHADER_PARAMETER(uint32, NumTotalVertices)

		// Relaxation parameters
		SHADER_PARAMETER(float, RelaxStrength)
		SHADER_PARAMETER(float, DeformAmountInfluence)

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
		OutEnvironment.SetDefine(TEXT("MAX_NEIGHBORS"), FLESHRING_MAX_NEIGHBORS);
	}
};

// ============================================================================
// FTangentRelaxDispatchParams - Dispatch Parameters
// ============================================================================

struct FTangentRelaxDispatchParams
{
	/** Number of affected vertices to process */
	uint32 NumAffectedVertices;

	/** Total mesh vertex count */
	uint32 NumTotalVertices;

	/** Relaxation strength (0~1) - higher = more smoothing
	 *  완화 강도 (0~1) - 높을수록 더 많이 스무딩 */
	float RelaxStrength;

	/** How much deform amount affects relaxation strength
	 *  0 = uniform relaxation, 1 = less relaxation on deformed areas
	 *  변형량이 relaxation에 미치는 영향
	 *  0 = 균일 적용, 1 = 변형된 곳은 적게 적용 */
	float DeformAmountInfluence;

	/** Number of relaxation iterations */
	int32 NumIterations;

	/** Bounds scale for this pass (Z-direction only) */
	float BoundsScale;

	FTangentRelaxDispatchParams()
		: NumAffectedVertices(0)
		, NumTotalVertices(0)
		, RelaxStrength(0.5f)
		, DeformAmountInfluence(0.8f)
		, NumIterations(2)
		, BoundsScale(1.5f)
	{
	}
};

// ============================================================================
// Dispatch Functions
// ============================================================================

/**
 * Dispatch single pass of tangent space relaxation
 *
 * @param GraphBuilder - RDG builder
 * @param Params - Dispatch parameters
 * @param InputPositionsBuffer - Source positions (read)
 * @param OutputPositionsBuffer - Destination positions (write)
 * @param AffectedIndicesBuffer - Affected vertex indices
 * @param DeformAmountsBuffer - Per-vertex deform amounts
 * @param AdjacencyDataBuffer - Packed adjacency data (neighbor indices only)
 */
void DispatchFleshRingTangentRelaxCS(
	FRDGBuilder& GraphBuilder,
	const FTangentRelaxDispatchParams& Params,
	FRDGBufferRef InputPositionsBuffer,
	FRDGBufferRef OutputPositionsBuffer,
	FRDGBufferRef AffectedIndicesBuffer,
	FRDGBufferRef DeformAmountsBuffer,
	FRDGBufferRef AdjacencyDataBuffer);

/**
 * Dispatch multiple iterations of tangent space relaxation
 * Uses ping-pong buffers internally
 *
 * @param GraphBuilder - RDG builder
 * @param Params - Dispatch parameters (NumIterations used)
 * @param PositionsBuffer - Position buffer (in-place, uses ping-pong internally)
 * @param AffectedIndicesBuffer - Affected vertex indices
 * @param DeformAmountsBuffer - Per-vertex deform amounts
 * @param AdjacencyDataBuffer - Packed adjacency data (neighbor indices only)
 */
void DispatchFleshRingTangentRelaxCS_MultiPass(
	FRDGBuilder& GraphBuilder,
	const FTangentRelaxDispatchParams& Params,
	FRDGBufferRef PositionsBuffer,
	FRDGBufferRef AffectedIndicesBuffer,
	FRDGBufferRef DeformAmountsBuffer,
	FRDGBufferRef AdjacencyDataBuffer);
