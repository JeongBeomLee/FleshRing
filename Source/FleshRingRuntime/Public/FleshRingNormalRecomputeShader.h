// ============================================================================
// FleshRing Normal Recompute Shader
// FleshRing 노멀 재계산 셰이더
// ============================================================================
// Purpose: Recompute vertex normals for deformed vertices using face normal averaging
// 목적: Face Normal 평균을 사용하여 변형된 버텍스의 노멀 재계산
//
// This shader runs AFTER TightnessCS and BulgeCS.
// Only processes affected vertices for efficiency.
// TightnessCS와 BulgeCS 이후에 실행됩니다.
// 효율성을 위해 영향받은 버텍스만 처리합니다.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"

// ============================================================================
// FFleshRingNormalRecomputeCS - Normal Recompute Compute Shader
// 노멀 재계산 컴퓨트 셰이더
// ============================================================================
class FFleshRingNormalRecomputeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFleshRingNormalRecomputeCS);
	SHADER_USE_PARAMETER_STRUCT(FFleshRingNormalRecomputeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// ===== Input Buffers (SRV - Read Only) =====
		// ===== 입력 버퍼 (SRV - 읽기 전용) =====

		// Input: Deformed vertex positions (from TightnessCS/BulgeCS)
		// 입력: 변형된 버텍스 위치
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, DeformedPositions)

		// Input: Affected vertex indices to process
		// 입력: 처리할 영향받는 버텍스 인덱스
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AffectedVertexIndices)

		// Input: Adjacency offsets for each affected vertex
		// 입력: 각 영향받는 버텍스의 인접 오프셋
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AdjacencyOffsets)

		// Input: Flattened list of adjacent triangle indices
		// 입력: 인접 삼각형 인덱스의 평탄화된 리스트
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AdjacencyTriangles)

		// Input: Mesh index buffer
		// 입력: 메시 인덱스 버퍼
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, IndexBuffer)

		// Input: Original normals (bind pose) - fallback
		// 입력: 원본 노멀 (바인드 포즈) - 폴백용
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, OriginalNormals)

		// ===== Output Buffer (UAV - Read/Write) =====
		// ===== 출력 버퍼 (UAV - 읽기/쓰기) =====

		// Output: Recomputed normals
		// 출력: 재계산된 노멀
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputNormals)

		// ===== Parameters =====
		// ===== 파라미터 =====

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
// FNormalRecomputeDispatchParams - Dispatch Parameters
// 디스패치 파라미터 구조체
// ============================================================================
struct FNormalRecomputeDispatchParams
{
	// Number of affected vertices to process
	// 처리할 영향받는 버텍스 수
	uint32 NumAffectedVertices = 0;

	// Total mesh vertex count (for bounds checking)
	// 전체 메시 버텍스 수 (범위 체크용)
	uint32 NumTotalVertices = 0;

	FNormalRecomputeDispatchParams() = default;

	FNormalRecomputeDispatchParams(uint32 InNumAffectedVertices, uint32 InNumTotalVertices)
		: NumAffectedVertices(InNumAffectedVertices)
		, NumTotalVertices(InNumTotalVertices)
	{
	}
};

// ============================================================================
// Dispatch Function Declaration
// Dispatch 함수 선언
// ============================================================================

/**
 * Dispatch NormalRecomputeCS to recalculate normals for affected vertices
 * NormalRecomputeCS를 디스패치하여 영향받는 버텍스의 노멀 재계산
 *
 * @param GraphBuilder - RDG builder for resource management
 * @param Params - Dispatch parameters
 * @param DeformedPositionsBuffer - Deformed vertex positions (TightnessCS/BulgeCS output)
 * @param AffectedVertexIndicesBuffer - Indices of affected vertices
 * @param AdjacencyOffsetsBuffer - Adjacency offsets for each affected vertex
 * @param AdjacencyTrianglesBuffer - Flattened list of adjacent triangle indices
 * @param IndexBuffer - Mesh index buffer (triangle indices)
 * @param OriginalNormalsBuffer - Original bind pose normals (fallback)
 * @param OutputNormalsBuffer - Output buffer for recomputed normals
 */
void DispatchFleshRingNormalRecomputeCS(
	FRDGBuilder& GraphBuilder,
	const FNormalRecomputeDispatchParams& Params,
	FRDGBufferRef DeformedPositionsBuffer,
	FRDGBufferRef AffectedVertexIndicesBuffer,
	FRDGBufferRef AdjacencyOffsetsBuffer,
	FRDGBufferRef AdjacencyTrianglesBuffer,
	FRDGBufferRef IndexBuffer,
	FRDGBufferRef OriginalNormalsBuffer,
	FRDGBufferRef OutputNormalsBuffer);
