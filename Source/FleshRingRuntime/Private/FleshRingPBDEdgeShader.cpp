// Copyright 2026 LgThx. All Rights Reserved.

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
// Single Pass Dispatch (Tolerance-based)
// ============================================================================

void DispatchFleshRingPBDEdgeCS(
	FRDGBuilder& GraphBuilder,
	const FPBDEdgeDispatchParams& Params,
	FRDGBufferRef InputPositionsBuffer,
	FRDGBufferRef OutputPositionsBuffer,
	FRDGBufferRef AffectedIndicesBuffer,
	FRDGBufferRef RepresentativeIndicesBuffer,
	FRDGBufferRef IsAnchorFlagsBuffer,
	FRDGBufferRef FullVertexAnchorFlagsBuffer,
	FRDGBufferRef AdjacencyWithRestLengthsBuffer)
{
	// Early out if no vertices to process
	if (Params.NumAffectedVertices == 0)
	{
		return;
	}

	// Validate required buffers
	if (!IsAnchorFlagsBuffer || !FullVertexAnchorFlagsBuffer)
	{
		return;
	}

	// Allocate shader parameters
	FFleshRingPBDEdgeCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFleshRingPBDEdgeCS::FParameters>();

	// Bind position buffers
	PassParameters->InputPositions = GraphBuilder.CreateSRV(InputPositionsBuffer, PF_R32_FLOAT);
	PassParameters->OutputPositions = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);

	// Bind affected indices
	PassParameters->AffectedIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer);

	// UV Seam Welding: Bind RepresentativeIndices
	// If nullptr, use AffectedIndices as fallback
	if (RepresentativeIndicesBuffer)
	{
		PassParameters->RepresentativeIndices = GraphBuilder.CreateSRV(RepresentativeIndicesBuffer);
	}
	else
	{
		PassParameters->RepresentativeIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer);
	}

	// Bind IsAnchor buffers (Tolerance-based weighting)
	PassParameters->IsAnchorFlags = GraphBuilder.CreateSRV(IsAnchorFlagsBuffer);
	PassParameters->FullVertexAnchorFlags = GraphBuilder.CreateSRV(FullVertexAnchorFlagsBuffer);

	// Bind adjacency data
	PassParameters->AdjacencyWithRestLengths = GraphBuilder.CreateSRV(AdjacencyWithRestLengthsBuffer);

	// Set parameters
	PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
	PassParameters->NumTotalVertices = Params.NumTotalVertices;
	PassParameters->Stiffness = Params.Stiffness;
	PassParameters->Tolerance = Params.Tolerance;

	// Get shader
	TShaderMapRef<FFleshRingPBDEdgeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Calculate dispatch groups
	const uint32 ThreadGroupSize = 64;
	const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumAffectedVertices, ThreadGroupSize);

	// Add compute pass
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FleshRingPBDEdgeCS_Tolerance"),
		ComputeShader,
		PassParameters,
		FIntVector(static_cast<int32>(NumGroups), 1, 1)
	);
}

// ============================================================================
// Multi-Pass Dispatch (Ping-Pong, Tolerance-based)
// ============================================================================

void DispatchFleshRingPBDEdgeCS_MultiPass(
	FRDGBuilder& GraphBuilder,
	const FPBDEdgeDispatchParams& Params,
	FRDGBufferRef PositionsBuffer,
	FRDGBufferRef AffectedIndicesBuffer,
	FRDGBufferRef RepresentativeIndicesBuffer,
	FRDGBufferRef IsAnchorFlagsBuffer,
	FRDGBufferRef FullVertexAnchorFlagsBuffer,
	FRDGBufferRef AdjacencyWithRestLengthsBuffer)
{
	if (Params.NumAffectedVertices == 0 || Params.NumIterations <= 0)
	{
		return;
	}

	// Validate required buffers
	if (!IsAnchorFlagsBuffer || !FullVertexAnchorFlagsBuffer)
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
			IsAnchorFlagsBuffer,
			FullVertexAnchorFlagsBuffer,
			AdjacencyWithRestLengthsBuffer
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
			IsAnchorFlagsBuffer,
			FullVertexAnchorFlagsBuffer,
			AdjacencyWithRestLengthsBuffer
		);
	}

	// Copy final result back to original buffer
	bool bFinalInPong = (Params.NumIterations % 2 == 1);
	FRDGBufferRef FinalBuffer = bFinalInPong ? PongBuffer : PingBuffer;
	AddCopyBufferPass(GraphBuilder, PositionsBuffer, FinalBuffer);
}
