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
	FRDGBufferRef InfluencesBuffer,
	FRDGBufferRef AdjacencyWithRestLengthsBuffer,
	FRDGBufferRef FullInfluenceMapBuffer)
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
	PassParameters->Influences = GraphBuilder.CreateSRV(InfluencesBuffer);
	PassParameters->AdjacencyWithRestLengths = GraphBuilder.CreateSRV(AdjacencyWithRestLengthsBuffer);
	PassParameters->FullInfluenceMap = GraphBuilder.CreateSRV(FullInfluenceMapBuffer);

	// Set parameters
	PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
	PassParameters->NumTotalVertices = Params.NumTotalVertices;
	PassParameters->Stiffness = Params.Stiffness;

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
	FRDGBufferRef InfluencesBuffer,
	FRDGBufferRef AdjacencyWithRestLengthsBuffer,
	FRDGBufferRef FullInfluenceMapBuffer)
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
			InfluencesBuffer,
			AdjacencyWithRestLengthsBuffer,
			FullInfluenceMapBuffer
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
			InfluencesBuffer,
			AdjacencyWithRestLengthsBuffer,
			FullInfluenceMapBuffer
		);
	}

	// Copy final result back to original buffer
	bool bFinalInPong = (Params.NumIterations % 2 == 1);
	FRDGBufferRef FinalBuffer = bFinalInPong ? PongBuffer : PingBuffer;
	AddCopyBufferPass(GraphBuilder, PositionsBuffer, FinalBuffer);
}
