// FleshRingDebugPointOutputShader.cpp
// Purpose: Output debug points at final transformed positions (after all deformation passes)

#include "FleshRingDebugPointOutputShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"

IMPLEMENT_GLOBAL_SHADER(
	FFleshRingDebugPointOutputCS,
	"/Plugin/FleshRingPlugin/FleshRingDebugPointOutputCS.usf",
	"MainCS",
	SF_Compute
);

void DispatchFleshRingDebugPointOutputCS(
	FRDGBuilder& GraphBuilder,
	const FDebugPointOutputDispatchParams& Params,
	FRDGBufferRef FinalPositionsBuffer,
	FRDGBufferRef VertexIndicesBuffer,
	FRDGBufferRef InfluencesBuffer,
	FRDGBufferRef DebugPointBuffer)
{
	if (Params.NumVertices == 0 || !DebugPointBuffer || !InfluencesBuffer)
	{
		return;
	}

	FFleshRingDebugPointOutputCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFleshRingDebugPointOutputCS::FParameters>();

	// Input (SRV)
	PassParameters->FinalPositions = GraphBuilder.CreateSRV(FinalPositionsBuffer, PF_R32_FLOAT);
	PassParameters->VertexIndices = GraphBuilder.CreateSRV(VertexIndicesBuffer);
	PassParameters->Influences = GraphBuilder.CreateSRV(InfluencesBuffer, PF_R32_FLOAT);  // GPU에서 계산된 Influence

	// Output (UAV)
	PassParameters->DebugPointBuffer = GraphBuilder.CreateUAV(DebugPointBuffer);

	// Parameters
	PassParameters->NumVertices = Params.NumVertices;
	PassParameters->NumTotalVertices = Params.NumTotalVertices;
	PassParameters->RingIndex = Params.RingIndex;
	PassParameters->BaseOffset = Params.BaseOffset;
	PassParameters->InfluenceBaseOffset = Params.InfluenceBaseOffset;  // 다중 Ring 지원용 오프셋
	PassParameters->LocalToWorld = Params.LocalToWorld;

	TShaderMapRef<FFleshRingDebugPointOutputCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	const uint32 ThreadGroupSize = 64;
	const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumVertices, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FleshRingDebugPointOutputCS (Ring %d, %d verts)", Params.RingIndex, Params.NumVertices),
		ComputeShader,
		PassParameters,
		FIntVector(static_cast<int32>(NumGroups), 1, 1)
	);
}
