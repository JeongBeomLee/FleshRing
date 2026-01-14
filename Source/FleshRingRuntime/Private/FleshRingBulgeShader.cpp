// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingBulgeShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"
#include "RHIGPUReadback.h"

IMPLEMENT_GLOBAL_SHADER(
	FFleshRingBulgeCS,
	"/Plugin/FleshRingPlugin/FleshRingBulgeCS.usf",
	"MainCS",
	SF_Compute
);

void DispatchFleshRingBulgeCS(
	FRDGBuilder& GraphBuilder,
	const FBulgeDispatchParams& Params,
	FRDGBufferRef InputPositionsBuffer,
	FRDGBufferRef BulgeVertexIndicesBuffer,
	FRDGBufferRef BulgeInfluencesBuffer,
	FRDGBufferRef VolumeAccumBuffer,
	FRDGBufferRef OutputPositionsBuffer,
	FRDGTextureRef SDFTexture,
	FRDGBufferRef DebugBulgePointBuffer)
{
	if (Params.NumBulgeVertices == 0)
	{
		return;
	}

	FFleshRingBulgeCS::FParameters* PassParameters =
		GraphBuilder.AllocParameters<FFleshRingBulgeCS::FParameters>();

	// Input (SRV)
	PassParameters->InputPositions = GraphBuilder.CreateSRV(InputPositionsBuffer, PF_R32_FLOAT);
	PassParameters->BulgeVertexIndices = GraphBuilder.CreateSRV(BulgeVertexIndicesBuffer);
	PassParameters->BulgeInfluences = GraphBuilder.CreateSRV(BulgeInfluencesBuffer);
	PassParameters->VolumeAccumBuffer = GraphBuilder.CreateSRV(VolumeAccumBuffer, PF_R32_UINT);

	// Output (UAV)
	PassParameters->OutputPositions = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);

	// SDF
	if (SDFTexture)
	{
		PassParameters->SDFTexture = GraphBuilder.CreateSRV(SDFTexture);
		PassParameters->SDFSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->SDFBoundsMin = Params.SDFBoundsMin;
		PassParameters->SDFBoundsMax = Params.SDFBoundsMax;
		PassParameters->ComponentToSDFLocal = Params.ComponentToSDFLocal;

		// Ring Center/Axis (SDF Local Space)
		PassParameters->SDFLocalRingCenter = Params.SDFLocalRingCenter;
		PassParameters->SDFLocalRingAxis = Params.SDFLocalRingAxis;
	}
	else
	{
		// Dummy SDF (SDF 텍스처가 없는 경우 더미 텍스처를 RDG가 필요로 함)
		FRDGTextureDesc DummySDFDesc = FRDGTextureDesc::Create3D(
			FIntVector(1, 1, 1),
			PF_R32_FLOAT,
			FClearValueBinding::Black,
			TexCreate_ShaderResource | TexCreate_UAV);
		FRDGTextureRef DummySDFTexture = GraphBuilder.CreateTexture(DummySDFDesc, TEXT("FleshRingBulge_DummySDF"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummySDFTexture), 0.0f);

		PassParameters->SDFTexture = GraphBuilder.CreateSRV(DummySDFTexture);
		PassParameters->SDFSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		PassParameters->SDFBoundsMin = FVector3f::ZeroVector;
		PassParameters->SDFBoundsMax = FVector3f::OneVector;
		PassParameters->ComponentToSDFLocal = FMatrix44f::Identity;

		// Ring Center/Axis (default values for non-SDF mode)
		PassParameters->SDFLocalRingCenter = FVector3f::ZeroVector;
		PassParameters->SDFLocalRingAxis = FVector3f(0.0f, 0.0f, 1.0f);
	}

	// Params
	PassParameters->NumBulgeVertices = Params.NumBulgeVertices;
	PassParameters->NumTotalVertices = Params.NumTotalVertices;
	PassParameters->BulgeStrength = Params.BulgeStrength;
	PassParameters->MaxBulgeDistance = Params.MaxBulgeDistance;
	PassParameters->FixedPointScale = Params.FixedPointScale;
	PassParameters->BulgeAxisDirection = Params.BulgeAxisDirection;
	PassParameters->RingIndex = Params.RingIndex;
	PassParameters->BulgeRadialRatio = Params.BulgeRadialRatio;
	PassParameters->UpperBulgeStrength = Params.UpperBulgeStrength;
	PassParameters->LowerBulgeStrength = Params.LowerBulgeStrength;
	PassParameters->bUseSDFInfluence = Params.bUseSDFInfluence;

	// Manual 모드용 파라미터 (Component Space)
	PassParameters->RingCenter = Params.RingCenter;
	PassParameters->RingAxis = Params.RingAxis;
	PassParameters->RingHeight = Params.RingHeight;

	// Debug Point Output 파라미터
	PassParameters->bOutputDebugBulgePoints = Params.bOutputDebugBulgePoints ? 1 : 0;
	PassParameters->DebugBulgePointBaseOffset = Params.DebugBulgePointBaseOffset;
	PassParameters->BulgeLocalToWorld = Params.BulgeLocalToWorld;

	if (DebugBulgePointBuffer)
	{
		PassParameters->DebugBulgePointBuffer = GraphBuilder.CreateUAV(DebugBulgePointBuffer);
	}
	else
	{
		// Dummy 버퍼 생성 (RDG는 모든 선언된 UAV 파라미터가 바인딩되어야 함)
		FRDGBufferDesc DummyDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FFleshRingDebugPoint), 1);
		FRDGBufferRef DummyBuffer = GraphBuilder.CreateBuffer(DummyDesc, TEXT("FleshRingBulge_DummyDebugPoints"));
		PassParameters->DebugBulgePointBuffer = GraphBuilder.CreateUAV(DummyBuffer);
	}

	TShaderMapRef<FFleshRingBulgeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

	const uint32 ThreadGroupSize = 64;
	const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumBulgeVertices, ThreadGroupSize);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("FleshRingBulgeCS"),
		ComputeShader,
		PassParameters,
		FIntVector(static_cast<int32>(NumGroups), 1, 1)
	);
}

void DispatchFleshRingBulgeCS_WithReadback(
	FRDGBuilder& GraphBuilder,
	const FBulgeDispatchParams& Params,
	FRDGBufferRef InputPositionsBuffer,
	FRDGBufferRef BulgeVertexIndicesBuffer,
	FRDGBufferRef BulgeInfluencesBuffer,
	FRDGBufferRef VolumeAccumBuffer,
	FRDGBufferRef OutputPositionsBuffer,
	FRDGTextureRef SDFTexture,
	FRHIGPUBufferReadback* Readback,
	FRDGBufferRef DebugBulgePointBuffer)
{
	DispatchFleshRingBulgeCS(
		GraphBuilder,
		Params,
		InputPositionsBuffer,
		BulgeVertexIndicesBuffer,
		BulgeInfluencesBuffer,
		VolumeAccumBuffer,
		OutputPositionsBuffer,
		SDFTexture,
		DebugBulgePointBuffer
	);

	AddEnqueueCopyPass(GraphBuilder, Readback, OutputPositionsBuffer, 0);
}
