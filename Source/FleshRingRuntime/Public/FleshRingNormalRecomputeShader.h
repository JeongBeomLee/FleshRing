// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Normal Recompute Shader
// FleshRing 노멀 재계산 셰이더
// ============================================================================
// Purpose: Recompute vertex normals using surface rotation method
// 목적: 표면 회전 방식을 사용하여 버텍스 노멀 재계산
//
// This shader runs AFTER TightnessCS and BulgeCS.
// It calculates the rotation from original to deformed face normals
// and applies this rotation to the original vertex normals.
// This preserves smooth shading while accounting for surface deformation.
//
// TightnessCS와 BulgeCS 이후에 실행됩니다.
// 원본에서 변형된 Face Normal로의 회전을 계산하고
// 이 회전을 원본 버텍스 노멀에 적용합니다.
// 이 방식은 표면 변형을 반영하면서 스무스 셰이딩을 보존합니다.

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

		// Input: Original vertex positions (bind pose) - for calculating original face normals
		// 입력: 원본 버텍스 위치 (바인드 포즈) - 원본 Face Normal 계산용
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, OriginalPositions)

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

		// Input: Original tangents buffer (contains normals) - packed SNORM8x4 format
		// 입력: 원본 탄젠트 버퍼 (노멀 포함) - 패킹된 SNORM8x4 포맷
		// Format: TangentX (Index 0), TangentZ=Normal+BinormalSign (Index 1) per vertex
		SHADER_PARAMETER_SRV(Buffer<float4>, OriginalTangents)

		// ===== Output Buffer (UAV - Read/Write) =====
		// ===== 출력 버퍼 (UAV - 읽기/쓰기) =====

		// Output: Recomputed normals
		// 출력: 재계산된 노멀
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputNormals)

		// ===== Hop-based Blending (HopBased mode only) =====
		// ===== 홉 기반 블렌딩 (HopBased 모드 전용) =====

		// Input: Hop distances for each affected vertex (optional, for blending)
		// 입력: 각 영향받는 버텍스의 홉 거리 (블렌딩용, 선택적)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, HopDistances)

		// ===== UV Seam Welding (optional) =====
		// ===== UV Seam Welding (선택적) =====

		// Input: Representative vertex indices for UV seam welding
		// 입력: UV seam welding용 대표 버텍스 인덱스
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RepresentativeIndices)

		// ===== Parameters =====
		// ===== 파라미터 =====

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

	// Normal recompute mode (matches ENormalRecomputeMethod)
	// 노멀 재계산 모드 (ENormalRecomputeMethod와 일치)
	// 0 = Geometric, 1 = SurfaceRotation
	uint32 NormalRecomputeMode = 1;  // Default: SurfaceRotation

	// ===== Hop-based Blending Parameters =====
	// ===== 홉 기반 블렌딩 파라미터 =====

	// Enable hop-based blending (blend with original normal at boundary)
	// 홉 기반 블렌딩 활성화 (경계에서 원본 노멀과 블렌드)
	bool bEnableHopBlending = false;

	// Maximum hop distance (for blend factor calculation)
	// 최대 홉 거리 (블렌드 계수 계산용)
	uint32 MaxHops = 0;

	// Falloff type for blending (0=Linear, 1=Quadratic, 2=Hermite)
	// 블렌딩 falloff 타입 (0=선형, 1=2차곡선, 2=Hermite)
	uint32 FalloffType = 2;  // Default: Hermite

	// ===== UV Seam Welding Parameters =====
	// ===== UV Seam Welding 파라미터 =====

	// Enable UV seam welding (use RepresentativeIndices for UV seam)
	// UV seam welding 활성화 (대표 버텍스의 노멀을 사용)
	bool bEnableUVSeamWelding = false;

	// ===== Displacement-based Blending Parameters =====
	// ===== 변위 기반 블렌딩 파라미터 =====

	// Enable displacement-based blending (blend based on actual vertex movement)
	// 변위 기반 블렌딩 활성화 (실제 버텍스 이동량에 따라 블렌드)
	bool bEnableDisplacementBlending = false;

	// Maximum displacement for blend (cm)
	// 블렌드 최대 변위 (cm) - 이 거리 이상 이동한 버텍스는 100% 재계산된 노멀 사용
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
// Dispatch 함수 선언
// ============================================================================

/**
 * Dispatch NormalRecomputeCS to recalculate normals for affected vertices
 * using the surface rotation method.
 * 표면 회전 방식을 사용하여 영향받는 버텍스의 노멀 재계산
 *
 * The shader calculates the rotation from original to deformed face normals
 * and applies this rotation to the original vertex normals, preserving smooth shading.
 * 셰이더는 원본에서 변형된 Face Normal로의 회전을 계산하고
 * 이 회전을 원본 버텍스 노멀에 적용하여 스무스 셰이딩을 보존합니다.
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
