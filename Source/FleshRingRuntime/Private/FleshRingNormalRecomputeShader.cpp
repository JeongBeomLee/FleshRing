// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Normal Recompute Shader - Implementation
// ============================================================================

#include "FleshRingNormalRecomputeShader.h"

#include "CoreMinimal.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"

// ============================================================================
// Shader Implementation Registration
// ============================================================================
IMPLEMENT_GLOBAL_SHADER(
	FFleshRingNormalRecomputeCS,
	"/Plugin/FleshRingPlugin/FleshRingNormalRecomputeCS.usf",
	"MainCS",
	SF_Compute
);

// ============================================================================
// Cached Dummy Buffers (created once, reused every frame)
// ============================================================================
static TRefCountPtr<FRDGPooledBuffer> GDummyHopDistancesBuffer;
static TRefCountPtr<FRDGPooledBuffer> GDummyRepresentativeIndicesBuffer;

// Helper: Get or create dummy HopDistances buffer
static FRDGBufferRef GetOrCreateDummyHopDistancesBuffer(FRDGBuilder& GraphBuilder)
{
	if (!GDummyHopDistancesBuffer.IsValid())
	{
		// First frame: create and upload
		FRDGBufferRef TempBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), 1),
			TEXT("FleshRing_DummyHopDistances")
		);
		static const int32 DummyData = 0;
		GraphBuilder.QueueBufferUpload(TempBuffer, &DummyData, sizeof(int32), ERDGInitialDataFlags::None);

		// Extract to pooled buffer for reuse
		GDummyHopDistancesBuffer = GraphBuilder.ConvertToExternalBuffer(TempBuffer);

		return TempBuffer;
	}

	// Subsequent frames: reuse existing buffer
	return GraphBuilder.RegisterExternalBuffer(GDummyHopDistancesBuffer, TEXT("FleshRing_DummyHopDistances"));
}

// Helper: Get or create dummy RepresentativeIndices buffer
static FRDGBufferRef GetOrCreateDummyRepresentativeIndicesBuffer(FRDGBuilder& GraphBuilder)
{
	if (!GDummyRepresentativeIndicesBuffer.IsValid())
	{
		// First frame: create and upload
		FRDGBufferRef TempBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1),
			TEXT("FleshRing_DummyRepresentativeIndices")
		);
		static const uint32 DummyData = 0;
		GraphBuilder.QueueBufferUpload(TempBuffer, &DummyData, sizeof(uint32), ERDGInitialDataFlags::None);

		// Extract to pooled buffer for reuse
		GDummyRepresentativeIndicesBuffer = GraphBuilder.ConvertToExternalBuffer(TempBuffer);

		return TempBuffer;
	}

	// Subsequent frames: reuse existing buffer
	return GraphBuilder.RegisterExternalBuffer(GDummyRepresentativeIndicesBuffer, TEXT("FleshRing_DummyRepresentativeIndices"));
}

// ============================================================================
// Dispatch Function Implementation
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
	if (Params.NumAffectedVertices == 0 || !SourceTangentsSRV)
	{
		return;
	}

	// Allocate shader parameters
	FFleshRingNormalRecomputeCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFleshRingNormalRecomputeCS::FParameters>();

	// ===== Bind input buffers (SRV) =====
	PassParameters->DeformedPositions = GraphBuilder.CreateSRV(DeformedPositionsBuffer, PF_R32_FLOAT);
	PassParameters->OriginalPositions = GraphBuilder.CreateSRV(OriginalPositionsBuffer, PF_R32_FLOAT);
	PassParameters->AffectedVertexIndices = GraphBuilder.CreateSRV(AffectedVertexIndicesBuffer);
	PassParameters->AdjacencyOffsets = GraphBuilder.CreateSRV(AdjacencyOffsetsBuffer);
	PassParameters->AdjacencyTriangles = GraphBuilder.CreateSRV(AdjacencyTrianglesBuffer);
	PassParameters->IndexBuffer = GraphBuilder.CreateSRV(IndexBuffer, PF_R32_UINT);
	PassParameters->OriginalTangents = SourceTangentsSRV;

	// ===== Bind output buffer (UAV) =====
	PassParameters->OutputNormals = GraphBuilder.CreateUAV(OutputNormalsBuffer, PF_R32_FLOAT);

	// ===== Hop-based Blending =====
	// Shader parameters must always be bound - use cached dummy buffer when not in use
	if (HopDistancesBuffer && Params.bEnableHopBlending)
	{
		PassParameters->HopDistances = GraphBuilder.CreateSRV(HopDistancesBuffer);
	}
	else
	{
		// Use cached dummy buffer (created only on first frame, reused afterwards)
		FRDGBufferRef DummyHopBuffer = GetOrCreateDummyHopDistancesBuffer(GraphBuilder);
		PassParameters->HopDistances = GraphBuilder.CreateSRV(DummyHopBuffer);
	}

	// ===== UV Seam Welding =====
	// Shader parameters must always be bound - use cached dummy buffer when not in use
	if (RepresentativeIndicesBuffer && Params.bEnableUVSeamWelding)
	{
		PassParameters->RepresentativeIndices = GraphBuilder.CreateSRV(RepresentativeIndicesBuffer);
	}
	else
	{
		// Use cached dummy buffer (created only on first frame, reused afterwards)
		FRDGBufferRef DummyRepBuffer = GetOrCreateDummyRepresentativeIndicesBuffer(GraphBuilder);
		PassParameters->RepresentativeIndices = GraphBuilder.CreateSRV(DummyRepBuffer);
	}

	// ===== Parameters =====
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
	TShaderMapRef<FFleshRingNormalRecomputeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Calculate dispatch groups
	const uint32 ThreadGroupSize = 64;
	const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumAffectedVertices, ThreadGroupSize);

	// Add compute pass to RDG
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FleshRingNormalRecomputeCS (%d verts)", Params.NumAffectedVertices),
		ComputeShader,
		PassParameters,
		FIntVector(static_cast<int32>(NumGroups), 1, 1)
	);
}
