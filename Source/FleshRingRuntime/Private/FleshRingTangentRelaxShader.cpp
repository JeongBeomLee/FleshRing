// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Tangent Space Relaxation Shader - Implementation
// ============================================================================

#include "FleshRingTangentRelaxShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"

// ============================================================================
// Shader Implementation Registration
// ============================================================================

IMPLEMENT_GLOBAL_SHADER(
	FFleshRingTangentRelaxCS,
	"/Plugin/FleshRingPlugin/FleshRingTangentRelaxCS.usf",
	"MainCS",
	SF_Compute
);

// ============================================================================
// Single Pass Dispatch
// ============================================================================

void DispatchFleshRingTangentRelaxCS(
	FRDGBuilder& GraphBuilder,
	const FTangentRelaxDispatchParams& Params,
	FRDGBufferRef InputPositionsBuffer,
	FRDGBufferRef OutputPositionsBuffer,
	FRDGBufferRef AffectedIndicesBuffer,
	FRDGBufferRef DeformAmountsBuffer,
	FRDGBufferRef AdjacencyDataBuffer)
{
	// Early out if no vertices to process
	if (Params.NumAffectedVertices == 0)
	{
		return;
	}

	// Allocate shader parameters
	FFleshRingTangentRelaxCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFleshRingTangentRelaxCS::FParameters>();

	// Bind buffers
	PassParameters->InputPositions = GraphBuilder.CreateSRV(InputPositionsBuffer, PF_R32_FLOAT);
	PassParameters->OutputPositions = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);
	PassParameters->AffectedIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer);
	PassParameters->DeformAmounts = GraphBuilder.CreateSRV(DeformAmountsBuffer);
	PassParameters->AdjacencyData = GraphBuilder.CreateSRV(AdjacencyDataBuffer);

	// Set parameters
	PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
	PassParameters->NumTotalVertices = Params.NumTotalVertices;
	PassParameters->RelaxStrength = Params.RelaxStrength;
	PassParameters->DeformAmountInfluence = Params.DeformAmountInfluence;

	// Get shader
	TShaderMapRef<FFleshRingTangentRelaxCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Calculate dispatch groups
	const uint32 ThreadGroupSize = 64;
	const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumAffectedVertices, ThreadGroupSize);

	// Add compute pass
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FleshRingTangentRelaxCS"),
		ComputeShader,
		PassParameters,
		FIntVector(static_cast<int32>(NumGroups), 1, 1)
	);
}

// ============================================================================
// Multi-Pass Dispatch (Ping-Pong)
// ============================================================================

void DispatchFleshRingTangentRelaxCS_MultiPass(
	FRDGBuilder& GraphBuilder,
	const FTangentRelaxDispatchParams& Params,
	FRDGBufferRef PositionsBuffer,
	FRDGBufferRef AffectedIndicesBuffer,
	FRDGBufferRef DeformAmountsBuffer,
	FRDGBufferRef AdjacencyDataBuffer)
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
			TEXT("FleshRingTangentRelax_Temp")
		);

		// Copy input to temp
		AddCopyBufferPass(GraphBuilder, TempBuffer, PositionsBuffer);

		// Dispatch: Temp -> Positions
		DispatchFleshRingTangentRelaxCS(
			GraphBuilder,
			Params,
			TempBuffer,
			PositionsBuffer,
			AffectedIndicesBuffer,
			DeformAmountsBuffer,
			AdjacencyDataBuffer
		);
		return;
	}

	// Multi-pass: use ping-pong buffers
	FRDGBufferRef PingBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), Params.NumTotalVertices * 3),
		TEXT("FleshRingTangentRelax_Ping")
	);

	FRDGBufferRef PongBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), Params.NumTotalVertices * 3),
		TEXT("FleshRingTangentRelax_Pong")
	);

	// Initialize BOTH buffers with input data
	AddCopyBufferPass(GraphBuilder, PingBuffer, PositionsBuffer);
	AddCopyBufferPass(GraphBuilder, PongBuffer, PositionsBuffer);

	// Ping-pong iterations
	for (int32 Iteration = 0; Iteration < Params.NumIterations; ++Iteration)
	{
		bool bEvenIteration = (Iteration % 2 == 0);
		FRDGBufferRef ReadBuffer = bEvenIteration ? PingBuffer : PongBuffer;
		FRDGBufferRef WriteBuffer = bEvenIteration ? PongBuffer : PingBuffer;

		DispatchFleshRingTangentRelaxCS(
			GraphBuilder,
			Params,
			ReadBuffer,
			WriteBuffer,
			AffectedIndicesBuffer,
			DeformAmountsBuffer,
			AdjacencyDataBuffer
		);
	}

	// Copy final result back to original buffer
	bool bFinalInPong = (Params.NumIterations % 2 == 1);
	FRDGBufferRef FinalBuffer = bFinalInPong ? PongBuffer : PingBuffer;
	AddCopyBufferPass(GraphBuilder, PositionsBuffer, FinalBuffer);
}
