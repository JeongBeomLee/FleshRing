// ============================================================================
// FleshRing Tangent Recompute Shader
// FleshRing 탄젠트 재계산 셰이더
// ============================================================================
// Purpose: Recompute tangents using Gram-Schmidt orthonormalization
// 목적: Gram-Schmidt 정규직교화를 사용하여 탄젠트 재계산
//
// This shader runs AFTER NormalRecomputeCS.
// Uses recomputed normals to orthonormalize tangents.
// NormalRecomputeCS 이후에 실행됩니다.
// 재계산된 노멀을 사용하여 탄젠트를 정규직교화합니다.
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
// 탄젠트 재계산 컴퓨트 셰이더
// ============================================================================
class FFleshRingTangentRecomputeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFleshRingTangentRecomputeCS);
	SHADER_USE_PARAMETER_STRUCT(FFleshRingTangentRecomputeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// ===== Input Buffers (SRV - Read Only) =====
		// ===== 입력 버퍼 (SRV - 읽기 전용) =====

		// Input: Recomputed normals from NormalRecomputeCS
		// 입력: NormalRecomputeCS에서 재계산된 노멀
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, RecomputedNormals)

		// Input: Original tangent buffer (TangentX=Normal, TangentZ=Tangent)
		// 입력: 원본 탄젠트 버퍼 (from StaticMeshVertexBuffer)
		// Format: 2 x SNORM8x4 per vertex
		SHADER_PARAMETER_SRV(Buffer<float4>, OriginalTangents)

		// Input: Affected vertex indices to process
		// 입력: 처리할 영향받는 버텍스 인덱스
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AffectedVertexIndices)

		// ===== [DEPRECATED] Polar Decomposition용 추가 버퍼 (SRV) =====
		// Polar 모드가 deprecated되어 이 버퍼들은 더 이상 사용되지 않음
		// 쉐이더 파라미터 바인딩 호환성을 위해 유지
		// 향후 버전에서 제거 예정

		// Input: Deformed vertex positions (3 floats per vertex) [DEPRECATED]
		// 입력: 변형된 버텍스 위치 (버텍스당 3 float)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, DeformedPositions)

		// Input: Original vertex positions (3 floats per vertex) [DEPRECATED]
		// 입력: 원본 버텍스 위치 (버텍스당 3 float)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, OriginalPositions)

		// Input: Mesh index buffer (3 indices per triangle) [DEPRECATED]
		// 입력: 메시 인덱스 버퍼 (삼각형당 3 인덱스)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, IndexBuffer)

		// Input: Adjacency offsets for each affected vertex [DEPRECATED]
		// 입력: 영향받는 버텍스별 인접 오프셋
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AdjacencyOffsets)

		// Input: Flattened list of adjacent triangle indices [DEPRECATED]
		// 입력: 인접 삼각형 인덱스의 평탄화된 리스트
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AdjacencyTriangles)

		// ===== Output Buffer (UAV - Read/Write) =====
		// ===== 출력 버퍼 (UAV - 읽기/쓰기) =====

		// Output: Recomputed tangents (float4 x 2 per vertex: TangentX, TangentZ)
		// 출력: 재계산된 탄젠트 (버텍스당 float4 x 2: TangentX, TangentZ)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputTangents)

		// ===== Parameters =====
		// ===== 파라미터 =====

		SHADER_PARAMETER(uint32, NumAffectedVertices)
		SHADER_PARAMETER(uint32, NumTotalVertices)
		// 탄젠트 재계산 모드 (ETangentRecomputeMethod와 일치)
		// 0 = GramSchmidt, 1 = PolarDecomposition
		SHADER_PARAMETER(uint32, TangentRecomputeMode)
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
// 디스패치 파라미터 구조체
// ============================================================================
struct FTangentRecomputeDispatchParams
{
	// Number of affected vertices to process
	// 처리할 영향받는 버텍스 수
	uint32 NumAffectedVertices = 0;

	// Total mesh vertex count (for bounds checking)
	// 전체 메시 버텍스 수 (범위 체크용)
	uint32 NumTotalVertices = 0;

	// Tangent recompute mode (ETangentRecomputeMethod 일치)
	// 0 = GramSchmidt, 1 = PolarDecomposition (DEPRECATED)
	uint32 TangentRecomputeMode = 0;  // Default: GramSchmidt

	FTangentRecomputeDispatchParams() = default;

	FTangentRecomputeDispatchParams(uint32 InNumAffectedVertices, uint32 InNumTotalVertices, uint32 InMode = 0)
		: NumAffectedVertices(InNumAffectedVertices)
		, NumTotalVertices(InNumTotalVertices)
		, TangentRecomputeMode(InMode)
	{
	}
};

// ============================================================================
// Dispatch Function Declaration
// Dispatch 함수 선언
// ============================================================================

/**
 * Dispatch TangentRecomputeCS to orthonormalize tangents for affected vertices
 * TangentRecomputeCS를 디스패치하여 영향받는 버텍스의 탄젠트 재계산
 *
 * @param GraphBuilder - RDG builder for resource management
 * @param Params - Dispatch parameters (includes TangentRecomputeMode)
 * @param RecomputedNormalsBuffer - Recomputed normals from NormalRecomputeCS (RDG)
 * @param OriginalTangentsSRV - Original tangent buffer SRV (RHI, from StaticMeshVertexBuffer)
 * @param AffectedVertexIndicesBuffer - Indices of affected vertices (RDG)
 * @param DeformedPositionsBuffer - Deformed vertex positions (Polar 모드에서 사용, optional)
 * @param OriginalPositionsBuffer - Original vertex positions (Polar 모드에서 사용, optional)
 * @param AdjacencyOffsetsBuffer - Adjacency offsets (Polar 모드에서 사용, optional)
 * @param AdjacencyTrianglesBuffer - Adjacent triangle indices (Polar 모드에서 사용, optional)
 * @param IndexBuffer - Mesh index buffer (Polar 모드에서 사용, optional)
 * @param OutputTangentsBuffer - Output buffer for recomputed tangents (RDG)
 */
void DispatchFleshRingTangentRecomputeCS(
	FRDGBuilder& GraphBuilder,
	const FTangentRecomputeDispatchParams& Params,
	FRDGBufferRef RecomputedNormalsBuffer,
	FRHIShaderResourceView* OriginalTangentsSRV,
	FRDGBufferRef AffectedVertexIndicesBuffer,
	FRDGBufferRef DeformedPositionsBuffer,
	FRDGBufferRef OriginalPositionsBuffer,
	FRDGBufferRef AdjacencyOffsetsBuffer,
	FRDGBufferRef AdjacencyTrianglesBuffer,
	FRDGBufferRef IndexBuffer,
	FRDGBufferRef OutputTangentsBuffer);
