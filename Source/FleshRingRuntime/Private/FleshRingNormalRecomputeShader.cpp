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
	FRDGBufferRef OriginalPositionsBuffer,
	FRDGBufferRef AffectedVertexIndicesBuffer,
	FRDGBufferRef AdjacencyOffsetsBuffer,
	FRDGBufferRef AdjacencyTrianglesBuffer,
	FRDGBufferRef IndexBuffer,
	FRHIShaderResourceView* SourceTangentsSRV,
	FRDGBufferRef OutputNormalsBuffer,
	FRDGBufferRef HopDistancesBuffer)
{
	// Early out if no vertices to process or missing SRV
	// 처리할 버텍스가 없거나 SRV가 없으면 조기 반환
	if (Params.NumAffectedVertices == 0 || !SourceTangentsSRV)
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
	PassParameters->OriginalPositions = GraphBuilder.CreateSRV(OriginalPositionsBuffer, PF_R32_FLOAT);
	PassParameters->AffectedVertexIndices = GraphBuilder.CreateSRV(AffectedVertexIndicesBuffer);
	PassParameters->AdjacencyOffsets = GraphBuilder.CreateSRV(AdjacencyOffsetsBuffer);
	PassParameters->AdjacencyTriangles = GraphBuilder.CreateSRV(AdjacencyTrianglesBuffer);
	PassParameters->IndexBuffer = GraphBuilder.CreateSRV(IndexBuffer, PF_R32_UINT);
	PassParameters->OriginalTangents = SourceTangentsSRV;

	// ===== Bind output buffer (UAV) =====
	// ===== 출력 버퍼 바인딩 (UAV) =====
	PassParameters->OutputNormals = GraphBuilder.CreateUAV(OutputNormalsBuffer, PF_R32_FLOAT);

	// ===== Hop-based Blending =====
	// ===== 홉 기반 블렌딩 =====
	// 셰이더 파라미터는 항상 바인딩되어야 함 - 사용하지 않을 때는 더미 버퍼 사용
	if (HopDistancesBuffer && Params.bEnableHopBlending)
	{
		PassParameters->HopDistances = GraphBuilder.CreateSRV(HopDistancesBuffer);
	}
	else
	{
		// 더미 버퍼 생성 및 데이터 업로드 (1 element, 셰이더에서 사용되지 않음)
		// RDG는 읽기 전에 버퍼가 써져야 함
		static const int32 DummyData = 0;
		FRDGBufferRef DummyHopBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), 1),
			TEXT("FleshRing_DummyHopDistances")
		);
		GraphBuilder.QueueBufferUpload(DummyHopBuffer, &DummyData, sizeof(int32), ERDGInitialDataFlags::None);
		PassParameters->HopDistances = GraphBuilder.CreateSRV(DummyHopBuffer);
	}

	// ===== Parameters =====
	// ===== 파라미터 =====
	PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
	PassParameters->NumTotalVertices = Params.NumTotalVertices;
	PassParameters->NormalRecomputeMode = Params.NormalRecomputeMode;
	PassParameters->bEnableHopBlending = Params.bEnableHopBlending ? 1 : 0;
	PassParameters->MaxHops = Params.MaxHops;
	PassParameters->FalloffType = Params.FalloffType;

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
