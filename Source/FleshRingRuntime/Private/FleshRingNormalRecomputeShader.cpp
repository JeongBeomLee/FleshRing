// ============================================================================
// FleshRing Normal Recompute Shader - Implementation
// FleshRing 노멀 재계산 셰이더 - 구현부
// ============================================================================

#include "FleshRingNormalRecomputeShader.h"

#include "CoreMinimal.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"

// ============================================================================
// Shader Implementation Registration
// 셰이더 구현 등록
// ============================================================================
IMPLEMENT_GLOBAL_SHADER(
	FFleshRingNormalRecomputeCS,
	"/Plugin/FleshRingPlugin/FleshRingNormalRecomputeCS.usf",
	"MainCS",
	SF_Compute
);

// ============================================================================
// Dispatch Function Implementation
// Dispatch 함수 구현
// ============================================================================

void DispatchFleshRingNormalRecomputeCS(
	FRDGBuilder& GraphBuilder,
	const FNormalRecomputeDispatchParams& Params,
	FRDGBufferRef DeformedPositionsBuffer,
	FRDGBufferRef AffectedVertexIndicesBuffer,
	FRDGBufferRef AdjacencyOffsetsBuffer,
	FRDGBufferRef AdjacencyTrianglesBuffer,
	FRDGBufferRef IndexBuffer,
	FRDGBufferRef OriginalNormalsBuffer,
	FRDGBufferRef OutputNormalsBuffer)
{
	// Early out if no vertices to process
	// 처리할 버텍스가 없으면 조기 반환
	if (Params.NumAffectedVertices == 0)
	{
		return;
	}

	// Allocate shader parameters
	// 셰이더 파라미터 할당
	FFleshRingNormalRecomputeCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFleshRingNormalRecomputeCS::FParameters>();

	// ===== Bind input buffers (SRV) =====
	// ===== 입력 버퍼 바인딩 (SRV) =====
	PassParameters->DeformedPositions = GraphBuilder.CreateSRV(DeformedPositionsBuffer, PF_R32_FLOAT);
	PassParameters->AffectedVertexIndices = GraphBuilder.CreateSRV(AffectedVertexIndicesBuffer);
	PassParameters->AdjacencyOffsets = GraphBuilder.CreateSRV(AdjacencyOffsetsBuffer);
	PassParameters->AdjacencyTriangles = GraphBuilder.CreateSRV(AdjacencyTrianglesBuffer);
	PassParameters->IndexBuffer = GraphBuilder.CreateSRV(IndexBuffer, PF_R32_UINT);
	PassParameters->OriginalNormals = GraphBuilder.CreateSRV(OriginalNormalsBuffer, PF_R32_FLOAT);

	// ===== Bind output buffer (UAV) =====
	// ===== 출력 버퍼 바인딩 (UAV) =====
	PassParameters->OutputNormals = GraphBuilder.CreateUAV(OutputNormalsBuffer, PF_R32_FLOAT);

	// ===== Parameters =====
	// ===== 파라미터 =====
	PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
	PassParameters->NumTotalVertices = Params.NumTotalVertices;

	// Get shader reference
	// 셰이더 참조 가져오기
	TShaderMapRef<FFleshRingNormalRecomputeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Calculate dispatch groups
	// 디스패치 그룹 수 계산
	const uint32 ThreadGroupSize = 64;
	const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumAffectedVertices, ThreadGroupSize);

	// Add compute pass to RDG
	// RDG에 컴퓨트 패스 추가
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FleshRingNormalRecomputeCS (%d verts)", Params.NumAffectedVertices),
		ComputeShader,
		PassParameters,
		FIntVector(static_cast<int32>(NumGroups), 1, 1)
	);
}
