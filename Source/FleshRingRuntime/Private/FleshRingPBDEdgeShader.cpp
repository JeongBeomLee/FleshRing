// ============================================================================
// FleshRing PBD Edge Constraint Shader - Implementation
// ============================================================================

#include "FleshRingPBDEdgeShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"

// ============================================================================
// Shader Implementation Registration
// ============================================================================

IMPLEMENT_GLOBAL_SHADER(
	FFleshRingPBDEdgeCS,
	"/Plugin/FleshRingPlugin/FleshRingPBDEdgeCS.usf",
	"MainCS",
	SF_Compute
);

// ============================================================================
// Single Pass Dispatch
// ============================================================================

void DispatchFleshRingPBDEdgeCS(
	FRDGBuilder& GraphBuilder,
	const FPBDEdgeDispatchParams& Params,
	FRDGBufferRef InputPositionsBuffer,
	FRDGBufferRef OutputPositionsBuffer,
	FRDGBufferRef AffectedIndicesBuffer,
	FRDGBufferRef RepresentativeIndicesBuffer,
	FRDGBufferRef InfluencesBuffer,
	FRDGBufferRef DeformAmountsBuffer,
	FRDGBufferRef AdjacencyWithRestLengthsBuffer,
	FRDGBufferRef FullInfluenceMapBuffer,
	FRDGBufferRef FullDeformAmountMapBuffer)
{
	// Early out if no vertices to process
	if (Params.NumAffectedVertices == 0)
	{
		return;
	}

	// Allocate shader parameters
	FFleshRingPBDEdgeCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFleshRingPBDEdgeCS::FParameters>();

	// Bind buffers
	PassParameters->InputPositions = GraphBuilder.CreateSRV(InputPositionsBuffer, PF_R32_FLOAT);
	PassParameters->OutputPositions = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);
	PassParameters->AffectedIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer);

	// UV Seam Welding: RepresentativeIndices 바인딩
	// nullptr이면 AffectedIndices를 fallback으로 사용
	if (RepresentativeIndicesBuffer)
	{
		PassParameters->RepresentativeIndices = GraphBuilder.CreateSRV(RepresentativeIndicesBuffer);
	}
	else
	{
		PassParameters->RepresentativeIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer);
	}

	PassParameters->Influences = GraphBuilder.CreateSRV(InfluencesBuffer);
	PassParameters->AdjacencyWithRestLengths = GraphBuilder.CreateSRV(AdjacencyWithRestLengthsBuffer);
	PassParameters->FullInfluenceMap = GraphBuilder.CreateSRV(FullInfluenceMapBuffer);

	// ===== DeformAmount 가중치 모드 활성화 조건 =====
	// 버퍼가 실제로 있어야만 DeformAmount 가중치 모드 사용 가능
	// 버퍼 없이 bUseDeformAmountWeight=true이면 버퍼 오버플로우 발생!
	const bool bCanUseDeformAmountWeight =
		Params.bUseDeformAmountWeight &&
		DeformAmountsBuffer != nullptr &&
		FullDeformAmountMapBuffer != nullptr;

	// DeformAmounts 바인딩
	if (bCanUseDeformAmountWeight)
	{
		PassParameters->DeformAmounts = GraphBuilder.CreateSRV(DeformAmountsBuffer);
	}
	else
	{
		// Dummy buffer (셰이더 파라미터 바인딩 필수)
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(float), 1),
			TEXT("FleshRingPBD_DummyDeformAmounts")
		);
		float DummyData = 0.0f;
		GraphBuilder.QueueBufferUpload(DummyBuffer, &DummyData, sizeof(DummyData), ERDGInitialDataFlags::None);
		PassParameters->DeformAmounts = GraphBuilder.CreateSRV(DummyBuffer);
	}

	// FullDeformAmountMap 바인딩
	if (bCanUseDeformAmountWeight)
	{
		PassParameters->FullDeformAmountMap = GraphBuilder.CreateSRV(FullDeformAmountMapBuffer);
	}
	else
	{
		// Dummy buffer
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(float), 1),
			TEXT("FleshRingPBD_DummyFullDeformMap")
		);
		float DummyData = 0.0f;
		GraphBuilder.QueueBufferUpload(DummyBuffer, &DummyData, sizeof(DummyData), ERDGInitialDataFlags::None);
		PassParameters->FullDeformAmountMap = GraphBuilder.CreateSRV(DummyBuffer);
	}

	// Set parameters
	PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
	PassParameters->NumTotalVertices = Params.NumTotalVertices;
	PassParameters->Stiffness = Params.Stiffness;
	PassParameters->BoundsScale = Params.BoundsScale;
	// 버퍼가 없으면 DeformAmount 모드 강제 비활성화 (버퍼 오버플로우 방지)
	PassParameters->bUseDeformAmountWeight = bCanUseDeformAmountWeight ? 1 : 0;

	// Get shader
	TShaderMapRef<FFleshRingPBDEdgeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Calculate dispatch groups
	const uint32 ThreadGroupSize = 64;
	const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumAffectedVertices, ThreadGroupSize);

	// Add compute pass
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FleshRingPBDEdgeCS"),
		ComputeShader,
		PassParameters,
		FIntVector(static_cast<int32>(NumGroups), 1, 1)
	);
}

// ============================================================================
// Multi-Pass Dispatch (Ping-Pong)
// ============================================================================

void DispatchFleshRingPBDEdgeCS_MultiPass(
	FRDGBuilder& GraphBuilder,
	const FPBDEdgeDispatchParams& Params,
	FRDGBufferRef PositionsBuffer,
	FRDGBufferRef AffectedIndicesBuffer,
	FRDGBufferRef RepresentativeIndicesBuffer,
	FRDGBufferRef InfluencesBuffer,
	FRDGBufferRef DeformAmountsBuffer,
	FRDGBufferRef AdjacencyWithRestLengthsBuffer,
	FRDGBufferRef FullInfluenceMapBuffer,
	FRDGBufferRef FullDeformAmountMapBuffer)
{
	if (Params.NumAffectedVertices == 0 || Params.NumIterations <= 0)
	{
		return;
	}

	// For single iteration, use simpler path
	if (Params.NumIterations == 1)
	{
		// Create temp buffer for output
		FRDGBufferRef TempBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(float), Params.NumTotalVertices * 3),
			TEXT("FleshRingPBDEdge_Temp")
		);

		// Copy input to temp
		AddCopyBufferPass(GraphBuilder, TempBuffer, PositionsBuffer);

		// Dispatch: Temp -> Positions
		DispatchFleshRingPBDEdgeCS(
			GraphBuilder,
			Params,
			TempBuffer,
			PositionsBuffer,
			AffectedIndicesBuffer,
			RepresentativeIndicesBuffer,
			InfluencesBuffer,
			DeformAmountsBuffer,
			AdjacencyWithRestLengthsBuffer,
			FullInfluenceMapBuffer,
			FullDeformAmountMapBuffer
		);
		return;
	}

	// Multi-pass: use ping-pong buffers
	FRDGBufferRef PingBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), Params.NumTotalVertices * 3),
		TEXT("FleshRingPBDEdge_Ping")
	);

	FRDGBufferRef PongBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), Params.NumTotalVertices * 3),
		TEXT("FleshRingPBDEdge_Pong")
	);

	// Initialize BOTH buffers with input data
	// Critical: Both must be initialized because PBD only writes affected vertices
	AddCopyBufferPass(GraphBuilder, PingBuffer, PositionsBuffer);
	AddCopyBufferPass(GraphBuilder, PongBuffer, PositionsBuffer);

	// Ping-pong iterations
	for (int32 Iteration = 0; Iteration < Params.NumIterations; ++Iteration)
	{
		bool bEvenIteration = (Iteration % 2 == 0);
		FRDGBufferRef ReadBuffer = bEvenIteration ? PingBuffer : PongBuffer;
		FRDGBufferRef WriteBuffer = bEvenIteration ? PongBuffer : PingBuffer;

		DispatchFleshRingPBDEdgeCS(
			GraphBuilder,
			Params,
			ReadBuffer,
			WriteBuffer,
			AffectedIndicesBuffer,
			RepresentativeIndicesBuffer,
			InfluencesBuffer,
			DeformAmountsBuffer,
			AdjacencyWithRestLengthsBuffer,
			FullInfluenceMapBuffer,
			FullDeformAmountMapBuffer
		);
	}

	// Copy final result back to original buffer
	bool bFinalInPong = (Params.NumIterations % 2 == 1);
	FRDGBufferRef FinalBuffer = bFinalInPong ? PongBuffer : PingBuffer;
	AddCopyBufferPass(GraphBuilder, PositionsBuffer, FinalBuffer);
}
