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
	FRDGBufferRef DeformedPositionsBuffer,
	FRDGBufferRef OriginalPositionsBuffer,
	FRDGBufferRef AdjacencyOffsetsBuffer,
	FRDGBufferRef AdjacencyTrianglesBuffer,
	FRDGBufferRef IndexBuffer,
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

	// [DEPRECATED] Polar 모드 체크 - 더 이상 사용되지 않음
	// Polar 모드가 선택되어도 쉐이더에서 GramSchmidt로 fallback됨
	// 아래 코드는 호환성을 위해 유지
	const bool bIsPolarMode = (Params.TangentRecomputeMode == 1);
	if (bIsPolarMode)
	{
		// [DEPRECATED] Polar 모드 경고 - 향후 버전에서 제거 예정
		UE_LOG(LogTemp, Warning, TEXT("TangentRecomputeCS: Polar mode is DEPRECATED, falling back to GramSchmidt"));
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

	// ===== [DEPRECATED] Polar Decomposition용 추가 버퍼 바인딩 =====
	// Polar 모드가 deprecated되어 이 버퍼들은 실제로 사용되지 않음
	// 쉐이더 파라미터 검증 통과를 위해 더미 버퍼 바인딩
	// 향후 버전에서 제거 예정
	FRDGBufferRef ActualDeformedPositionsBuffer = DeformedPositionsBuffer;
	FRDGBufferRef ActualOriginalPositionsBuffer = OriginalPositionsBuffer;
	FRDGBufferRef ActualAdjacencyOffsetsBuffer = AdjacencyOffsetsBuffer;
	FRDGBufferRef ActualAdjacencyTrianglesBuffer = AdjacencyTrianglesBuffer;
	FRDGBufferRef ActualIndexBuffer = IndexBuffer;

	// 더미 데이터 (static으로 한 번만 생성)
	static const float DummyFloatData[3] = { 0.0f, 0.0f, 0.0f };
	static const uint32 DummyUintData[3] = { 0, 0, 0 };
	static const uint32 DummyAdjOffsets[2] = { 0, 0 };

	// GramSchmidt 모드 또는 버퍼 누락 시 더미 버퍼 생성 + 업로드
	if (!ActualDeformedPositionsBuffer)
	{
		ActualDeformedPositionsBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), 3),
			TEXT("FleshRing_DummyDeformedPositions")
		);
		GraphBuilder.QueueBufferUpload(ActualDeformedPositionsBuffer, DummyFloatData, sizeof(DummyFloatData), ERDGInitialDataFlags::None);
	}
	if (!ActualOriginalPositionsBuffer)
	{
		ActualOriginalPositionsBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), 3),
			TEXT("FleshRing_DummyOriginalPositions")
		);
		GraphBuilder.QueueBufferUpload(ActualOriginalPositionsBuffer, DummyFloatData, sizeof(DummyFloatData), ERDGInitialDataFlags::None);
	}
	if (!ActualAdjacencyOffsetsBuffer)
	{
		ActualAdjacencyOffsetsBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 2),
			TEXT("FleshRing_DummyAdjacencyOffsets")
		);
		GraphBuilder.QueueBufferUpload(ActualAdjacencyOffsetsBuffer, DummyAdjOffsets, sizeof(DummyAdjOffsets), ERDGInitialDataFlags::None);
	}
	if (!ActualAdjacencyTrianglesBuffer)
	{
		ActualAdjacencyTrianglesBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
			TEXT("FleshRing_DummyAdjacencyTriangles")
		);
		GraphBuilder.QueueBufferUpload(ActualAdjacencyTrianglesBuffer, DummyUintData, sizeof(uint32), ERDGInitialDataFlags::None);
	}
	if (!ActualIndexBuffer)
	{
		ActualIndexBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 3),
			TEXT("FleshRing_DummyIndexBuffer")
		);
		GraphBuilder.QueueBufferUpload(ActualIndexBuffer, DummyUintData, sizeof(DummyUintData), ERDGInitialDataFlags::None);
	}

	PassParameters->DeformedPositions = GraphBuilder.CreateSRV(ActualDeformedPositionsBuffer, PF_R32_FLOAT);
	PassParameters->OriginalPositions = GraphBuilder.CreateSRV(ActualOriginalPositionsBuffer, PF_R32_FLOAT);
	PassParameters->AdjacencyOffsets = GraphBuilder.CreateSRV(ActualAdjacencyOffsetsBuffer);
	PassParameters->AdjacencyTriangles = GraphBuilder.CreateSRV(ActualAdjacencyTrianglesBuffer);
	PassParameters->IndexBuffer = GraphBuilder.CreateSRV(ActualIndexBuffer, PF_R32_UINT);

	// ===== Bind output buffer (UAV) =====
	// ===== 출력 버퍼 바인딩 (UAV) =====
	PassParameters->OutputTangents = GraphBuilder.CreateUAV(OutputTangentsBuffer, PF_R32_FLOAT);

	// ===== Parameters =====
	// ===== 파라미터 =====
	PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
	PassParameters->NumTotalVertices = Params.NumTotalVertices;
	PassParameters->TangentRecomputeMode = Params.TangentRecomputeMode;

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
		RDG_EVENT_NAME("FleshRingTangentRecomputeCS (%d verts, mode=%d)", Params.NumAffectedVertices, Params.TangentRecomputeMode),
		ComputeShader,
		PassParameters,
		FIntVector(static_cast<int32>(NumGroups), 1, 1)
	);
}
