// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "FleshRingDebugTypes.h"

/**
 * Bulge Compute Shader
 * TightnessCS에서 누적된 부피를 주변 버텍스로 분배
 */
class FFleshRingBulgeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFleshRingBulgeCS);
	SHADER_USE_PARAMETER_STRUCT(FFleshRingBulgeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, InputPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, BulgeVertexIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, BulgeInfluences)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, VolumeAccumBuffer)

		// Output
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPositions)

		// SDF
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, SDFTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, SDFSampler)
		SHADER_PARAMETER(FVector3f, SDFBoundsMin)
		SHADER_PARAMETER(FVector3f, SDFBoundsMax)
		SHADER_PARAMETER(FMatrix44f, ComponentToSDFLocal)

		// Params
		SHADER_PARAMETER(uint32, NumBulgeVertices)
		SHADER_PARAMETER(uint32, NumTotalVertices)
		SHADER_PARAMETER(float, BulgeStrength)
		SHADER_PARAMETER(float, MaxBulgeDistance)
		SHADER_PARAMETER(float, FixedPointScale)
		SHADER_PARAMETER(int32, BulgeAxisDirection)
		SHADER_PARAMETER(uint32, RingIndex)           // Ring 인덱스 (VolumeAccumBuffer 슬롯 지정)
		SHADER_PARAMETER(float, BulgeRadialRatio)     // Radial vs Axial 방향 비율 (0.0~1.0, 기본 0.7)
		SHADER_PARAMETER(float, UpperBulgeStrength)   // 상단(축 양수) Bulge 강도 배수
		SHADER_PARAMETER(float, LowerBulgeStrength)   // 하단(축 음수) Bulge 강도 배수
		SHADER_PARAMETER(uint32, bUseSDFInfluence)    // 0 = Manual (Component Space), 1 = SDF 모드

		// Ring Center/Axis (SDF Local Space) - SDF 모드용
		SHADER_PARAMETER(FVector3f, SDFLocalRingCenter)
		SHADER_PARAMETER(FVector3f, SDFLocalRingAxis)

		// Ring Center/Axis (Component Space) - Manual 모드용
		SHADER_PARAMETER(FVector3f, RingCenter)
		SHADER_PARAMETER(FVector3f, RingAxis)
		SHADER_PARAMETER(float, RingHeight)

		// Debug Point Output - GPU 디버그 렌더링용
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FFleshRingDebugPoint>, DebugBulgePointBuffer)
		SHADER_PARAMETER(uint32, bOutputDebugBulgePoints)
		SHADER_PARAMETER(uint32, DebugBulgePointBaseOffset)
		SHADER_PARAMETER(FMatrix44f, BulgeLocalToWorld)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);
	}
};

struct FBulgeDispatchParams
{
	uint32 NumBulgeVertices = 0;	// Bulge 대상 버텍스 수
	uint32 NumTotalVertices = 0;	// 전체 메시 버텍스 수(범위 체크용)
	float BulgeStrength = 1.0f;		// Bulge 강도
	float MaxBulgeDistance = 10.0f;	// 최대 Bulge 거리
	float FixedPointScale = 0.001f;	// TightnessCS와 동일해야 함.
	int32 BulgeAxisDirection = 0;	// Bulge 방향 (-1: 음의 축, +1: 양의 축, 0: 양방향)
	uint32 RingIndex = 0;			// Ring 인덱스 (VolumeAccumBuffer 슬롯 지정)
	float BulgeRadialRatio = 0.7f;	// Radial vs Axial 방향 비율 (0.0~1.0)
	float UpperBulgeStrength = 1.0f;	// 상단(축 양수) Bulge 강도 배수
	float LowerBulgeStrength = 1.0f;	// 하단(축 음수) Bulge 강도 배수
	uint32 bUseSDFInfluence = 1;	// 0 = Manual (Component Space), 1 = SDF 모드

	// SDF 모드용 파라미터
	FVector3f SDFBoundsMin = FVector3f::ZeroVector;			// in Ring Local Space
	FVector3f SDFBoundsMax = FVector3f::ZeroVector;			// in Ring Local Space
	FMatrix44f ComponentToSDFLocal = FMatrix44f::Identity;	// Component -> SDF Local
	FVector3f SDFLocalRingCenter = FVector3f::ZeroVector;
	FVector3f SDFLocalRingAxis = FVector3f(0.0f, 0.0f, 1.0f);

	// Manual 모드용 파라미터 (Component Space)
	FVector3f RingCenter = FVector3f::ZeroVector;
	FVector3f RingAxis = FVector3f(0.0f, 0.0f, 1.0f);
	float RingHeight = 2.0f;

	// Debug Point Output 파라미터 - GPU 디버그 렌더링용
	bool bOutputDebugBulgePoints = false;
	uint32 DebugBulgePointBaseOffset = 0;
	FMatrix44f BulgeLocalToWorld = FMatrix44f::Identity;
};

void DispatchFleshRingBulgeCS(
	FRDGBuilder& GraphBuilder,
	const FBulgeDispatchParams& Params,
	FRDGBufferRef InputPositionsBuffer,
	FRDGBufferRef BulgeVertexIndicesBuffer,
	FRDGBufferRef BulgeInfluencesBuffer,
	FRDGBufferRef VolumeAccumBuffer,
	FRDGBufferRef OutputPositionsBuffer,
	FRDGTextureRef SDFTexture,
	FRDGBufferRef DebugBulgePointBuffer = nullptr);

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
	FRDGBufferRef DebugBulgePointBuffer = nullptr);
