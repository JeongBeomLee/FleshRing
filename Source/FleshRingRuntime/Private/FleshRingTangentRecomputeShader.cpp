// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Tangent Recompute Shader - Implementation
// FleshRing 탄젠트 재계산 셰이더 - 구현부
// ============================================================================

#include "FleshRingTangentRecomputeShader.h"

#include "CoreMinimal.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"

// ============================================================================
// Shader Implementation Registration
// 셰이더 구현 등록
// ============================================================================
IMPLEMENT_GLOBAL_SHADER(
	FFleshRingTangentRecomputeCS,
	"/Plugin/FleshRingPlugin/FleshRingTangentRecomputeCS.usf",
	"MainCS",
	SF_Compute
);

// ============================================================================
// Dispatch Function Implementation
// Dispatch 함수 구현
// ============================================================================

void DispatchFleshRingTangentRecomputeCS(
	FRDGBuilder& GraphBuilder,
	const FTangentRecomputeDispatchParams& Params,
	FRDGBufferRef RecomputedNormalsBuffer,
	FRHIShaderResourceView* OriginalTangentsSRV,
	FRDGBufferRef AffectedVertexIndicesBuffer,
	FRDGBufferRef OutputTangentsBuffer)
{
	// Early out if no vertices to process
	// 처리할 버텍스가 없으면 조기 반환
	if (Params.NumAffectedVertices == 0)
	{
		return;
	}

	// Validate required inputs
	// 필수 입력 검증
	if (!RecomputedNormalsBuffer || !OriginalTangentsSRV || !AffectedVertexIndicesBuffer || !OutputTangentsBuffer)
	{
		UE_LOG(LogTemp, Warning, TEXT("TangentRecomputeCS: Missing required buffer"));
		return;
	}

	// Allocate shader parameters
	// 셰이더 파라미터 할당
	FFleshRingTangentRecomputeCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFleshRingTangentRecomputeCS::FParameters>();

	// ===== Bind input buffers =====
	// ===== 입력 버퍼 바인딩 =====
	PassParameters->RecomputedNormals = GraphBuilder.CreateSRV(RecomputedNormalsBuffer, PF_R32_FLOAT);
	PassParameters->OriginalTangents = OriginalTangentsSRV;
	PassParameters->AffectedVertexIndices = GraphBuilder.CreateSRV(AffectedVertexIndicesBuffer);

	// ===== Bind output buffer (UAV) =====
	// ===== 출력 버퍼 바인딩 (UAV) =====
	PassParameters->OutputTangents = GraphBuilder.CreateUAV(OutputTangentsBuffer, PF_R32_FLOAT);

	// ===== Parameters =====
	// ===== 파라미터 =====
	PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
	PassParameters->NumTotalVertices = Params.NumTotalVertices;

	// Get shader reference
	// 셰이더 참조 가져오기
	TShaderMapRef<FFleshRingTangentRecomputeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Calculate dispatch groups
	// 디스패치 그룹 수 계산
	const uint32 ThreadGroupSize = 64;
	const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumAffectedVertices, ThreadGroupSize);

	// Add compute pass to RDG
	// RDG에 컴퓨트 패스 추가
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FleshRingTangentRecomputeCS (%d verts)", Params.NumAffectedVertices),
		ComputeShader,
		PassParameters,
		FIntVector(static_cast<int32>(NumGroups), 1, 1)
	);
}
