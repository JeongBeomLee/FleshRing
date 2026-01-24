// Copyright 2026 LgThx. All Rights Reserved.

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
// Cached Dummy Buffers (created once, reused every frame)
// 캐싱된 더미 버퍼 (한 번 생성 후 매 프레임 재사용)
// ============================================================================
static TRefCountPtr<FRDGPooledBuffer> GDummyHopDistancesBuffer;
static TRefCountPtr<FRDGPooledBuffer> GDummyRepresentativeIndicesBuffer;

// Helper: Get or create dummy HopDistances buffer
// 헬퍼: 더미 HopDistances 버퍼 가져오기 또는 생성
static FRDGBufferRef GetOrCreateDummyHopDistancesBuffer(FRDGBuilder& GraphBuilder)
{
	if (!GDummyHopDistancesBuffer.IsValid())
	{
		// First frame: create and upload
		// 첫 프레임: 생성 및 업로드
		FRDGBufferRef TempBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), 1),
			TEXT("FleshRing_DummyHopDistances")
		);
		static const int32 DummyData = 0;
		GraphBuilder.QueueBufferUpload(TempBuffer, &DummyData, sizeof(int32), ERDGInitialDataFlags::None);

		// Extract to pooled buffer for reuse
		// 재사용을 위해 풀링 버퍼로 추출
		GDummyHopDistancesBuffer = GraphBuilder.ConvertToExternalBuffer(TempBuffer);

		return TempBuffer;
	}

	// Subsequent frames: reuse existing buffer
	// 이후 프레임: 기존 버퍼 재사용
	return GraphBuilder.RegisterExternalBuffer(GDummyHopDistancesBuffer, TEXT("FleshRing_DummyHopDistances"));
}

// Helper: Get or create dummy RepresentativeIndices buffer
// 헬퍼: 더미 RepresentativeIndices 버퍼 가져오기 또는 생성
static FRDGBufferRef GetOrCreateDummyRepresentativeIndicesBuffer(FRDGBuilder& GraphBuilder)
{
	if (!GDummyRepresentativeIndicesBuffer.IsValid())
	{
		// First frame: create and upload
		// 첫 프레임: 생성 및 업로드
		FRDGBufferRef TempBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
			TEXT("FleshRing_DummyRepresentativeIndices")
		);
		static const uint32 DummyData = 0;
		GraphBuilder.QueueBufferUpload(TempBuffer, &DummyData, sizeof(uint32), ERDGInitialDataFlags::None);

		// Extract to pooled buffer for reuse
		// 재사용을 위해 풀링 버퍼로 추출
		GDummyRepresentativeIndicesBuffer = GraphBuilder.ConvertToExternalBuffer(TempBuffer);

		return TempBuffer;
	}

	// Subsequent frames: reuse existing buffer
	// 이후 프레임: 기존 버퍼 재사용
	return GraphBuilder.RegisterExternalBuffer(GDummyRepresentativeIndicesBuffer, TEXT("FleshRing_DummyRepresentativeIndices"));
}

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
	FRDGBufferRef HopDistancesBuffer,
	FRDGBufferRef RepresentativeIndicesBuffer)
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
	// 셰이더 파라미터는 항상 바인딩되어야 함 - 사용하지 않을 때는 캐싱된 더미 버퍼 사용
	if (HopDistancesBuffer && Params.bEnableHopBlending)
	{
		PassParameters->HopDistances = GraphBuilder.CreateSRV(HopDistancesBuffer);
	}
	else
	{
		// 캐싱된 더미 버퍼 사용 (첫 프레임에만 생성, 이후 재사용)
		FRDGBufferRef DummyHopBuffer = GetOrCreateDummyHopDistancesBuffer(GraphBuilder);
		PassParameters->HopDistances = GraphBuilder.CreateSRV(DummyHopBuffer);
	}

	// ===== UV Seam Welding =====
	// ===== UV Seam Welding =====
	// 셰이더 파라미터는 항상 바인딩되어야 함 - 사용하지 않을 때는 캐싱된 더미 버퍼 사용
	if (RepresentativeIndicesBuffer && Params.bEnableUVSeamWelding)
	{
		PassParameters->RepresentativeIndices = GraphBuilder.CreateSRV(RepresentativeIndicesBuffer);
	}
	else
	{
		// 캐싱된 더미 버퍼 사용 (첫 프레임에만 생성, 이후 재사용)
		FRDGBufferRef DummyRepBuffer = GetOrCreateDummyRepresentativeIndicesBuffer(GraphBuilder);
		PassParameters->RepresentativeIndices = GraphBuilder.CreateSRV(DummyRepBuffer);
	}

	// ===== Parameters =====
	// ===== 파라미터 =====
	PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
	PassParameters->NumTotalVertices = Params.NumTotalVertices;
	PassParameters->NormalRecomputeMode = Params.NormalRecomputeMode;
	PassParameters->bEnableHopBlending = Params.bEnableHopBlending ? 1 : 0;
	PassParameters->MaxHops = Params.MaxHops;
	PassParameters->FalloffType = Params.FalloffType;
	PassParameters->bEnableUVSeamWelding = Params.bEnableUVSeamWelding ? 1 : 0;
	PassParameters->bEnableDisplacementBlending = Params.bEnableDisplacementBlending ? 1 : 0;
	PassParameters->MaxDisplacement = Params.MaxDisplacement;

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
