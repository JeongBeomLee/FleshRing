// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Tangent Recompute Shader - Implementation
// ============================================================================

#include "FleshRingTangentRecomputeShader.h"

#include "CoreMinimal.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingTangentRecompute, Log, All);

// ============================================================================
// Shader Implementation Registration
// ============================================================================
IMPLEMENT_GLOBAL_SHADER(
	FFleshRingTangentRecomputeCS,
	"/Plugin/FleshRingPlugin/FleshRingTangentRecomputeCS.usf",
	"MainCS",
	SF_Compute
);

// ============================================================================
// Dispatch Function Implementation
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
	if (Params.NumAffectedVertices == 0)
	{
		return;
	}

	// Validate required inputs
	if (!RecomputedNormalsBuffer || !OriginalTangentsSRV || !AffectedVertexIndicesBuffer || !OutputTangentsBuffer)
	{
		UE_LOG(LogFleshRingTangentRecompute, Warning, TEXT("TangentRecomputeCS: Missing required buffer"));
		return;
	}

	// Allocate shader parameters
	FFleshRingTangentRecomputeCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFleshRingTangentRecomputeCS::FParameters>();

	// ===== Bind input buffers =====
	PassParameters->RecomputedNormals = GraphBuilder.CreateSRV(RecomputedNormalsBuffer, PF_R32_FLOAT);
	PassParameters->OriginalTangents = OriginalTangentsSRV;
	PassParameters->AffectedVertexIndices = GraphBuilder.CreateSRV(AffectedVertexIndicesBuffer);

	// ===== Bind output buffer (UAV) =====
	PassParameters->OutputTangents = GraphBuilder.CreateUAV(OutputTangentsBuffer, PF_R32_FLOAT);

	// ===== Parameters =====
	PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
	PassParameters->NumTotalVertices = Params.NumTotalVertices;

	// Get shader reference
	TShaderMapRef<FFleshRingTangentRecomputeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	// Calculate dispatch groups
	const uint32 ThreadGroupSize = 64;
	const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumAffectedVertices, ThreadGroupSize);

	// Add compute pass to RDG
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FleshRingTangentRecomputeCS (%d verts)", Params.NumAffectedVertices),
		ComputeShader,
		PassParameters,
		FIntVector(static_cast<int32>(NumGroups), 1, 1)
	);
}
