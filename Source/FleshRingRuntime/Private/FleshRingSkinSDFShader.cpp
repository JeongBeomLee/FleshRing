// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingSkinSDFShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "GlobalShader.h"

IMPLEMENT_GLOBAL_SHADER(FSkinSDFLayerSeparationCS, "/Plugin/FleshRingPlugin/FleshRingSkinSDFCS.usf", "SkinSDFLayerSeparationCS", SF_Compute);

// ============================================================================
// Single Pass Dispatch
// ============================================================================

void DispatchFleshRingSkinSDFCS(
	FRDGBuilder& GraphBuilder,
	const FSkinSDFDispatchParams& Params,
	FRDGBufferRef PositionsBuffer,
	FRDGBufferRef SkinVertexIndicesBuffer,
	FRDGBufferRef SkinNormalsBuffer,
	FRDGBufferRef StockingVertexIndicesBuffer)
{
	if (Params.NumStockingVertices == 0 || Params.NumSkinVertices == 0)
	{
		return;
	}

	FSkinSDFLayerSeparationCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSkinSDFLayerSeparationCS::FParameters>();

	PassParameters->PositionsRW = GraphBuilder.CreateUAV(PositionsBuffer, PF_R32_FLOAT);
	PassParameters->SkinVertexIndices = GraphBuilder.CreateSRV(SkinVertexIndicesBuffer, PF_R32_UINT);
	PassParameters->SkinNormals = GraphBuilder.CreateSRV(SkinNormalsBuffer, PF_R32_FLOAT);
	PassParameters->StockingVertexIndices = GraphBuilder.CreateSRV(StockingVertexIndicesBuffer, PF_R32_UINT);

	PassParameters->NumStockingVertices = Params.NumStockingVertices;
	PassParameters->NumSkinVertices = Params.NumSkinVertices;
	PassParameters->NumTotalVertices = Params.NumTotalVertices;
	PassParameters->MinSeparation = Params.MinSeparation;
	PassParameters->TargetSeparation = Params.TargetSeparation;
	PassParameters->MaxPushDistance = Params.MaxPushDistance;
	PassParameters->MaxPullDistance = Params.MaxPullDistance;
	PassParameters->MaxIterations = Params.MaxIterations;
	PassParameters->RingAxis = Params.RingAxis;
	PassParameters->RingCenter = Params.RingCenter;

	TShaderMapRef<FSkinSDFLayerSeparationCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	const uint32 ThreadGroupSize = 64;
	const uint32 NumThreadGroups = FMath::DivideAndRoundUp(Params.NumStockingVertices, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FleshRing_SkinSDFLayerSeparation"),
		ComputeShader,
		PassParameters,
		FIntVector(NumThreadGroups, 1, 1)
	);
}

// ============================================================================
// Multi-Pass Dispatch (Iterative Refinement)
// ============================================================================

void DispatchFleshRingSkinSDFCS_MultiPass(
	FRDGBuilder& GraphBuilder,
	const FSkinSDFDispatchParams& Params,
	FRDGBufferRef PositionsBuffer,
	FRDGBufferRef SkinVertexIndicesBuffer,
	FRDGBufferRef SkinNormalsBuffer,
	FRDGBufferRef StockingVertexIndicesBuffer)
{
	// 이제 셰이더 내부에서 루프 처리하므로 단일 디스패치로 위임
	DispatchFleshRingSkinSDFCS(
		GraphBuilder,
		Params,
		PositionsBuffer,
		SkinVertexIndicesBuffer,
		SkinNormalsBuffer,
		StockingVertexIndicesBuffer
	);
}
