// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"

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

	FVector3f SDFBoundsMin = FVector3f::ZeroVector;			// in Ring Local Space
	FVector3f SDFBoundsMax = FVector3f::ZeroVector;			// in Ring Local Space
	FMatrix44f ComponentToSDFLocal = FMatrix44f::Identity;	// Component -> SDF Local
};

void DispatchFleshRingBulgeCS(
	FRDGBuilder& GraphBuilder,
	const FBulgeDispatchParams& Params,
	FRDGBufferRef InputPositionsBuffer,
	FRDGBufferRef BulgeVertexIndicesBuffer,
	FRDGBufferRef BulgeInfluencesBuffer,
	FRDGBufferRef VolumeAccumBuffer,
	FRDGBufferRef OutputPositionsBuffer,
	FRDGTextureRef SDFTexture);

void DispatchFleshRingBulgeCS_WithReadback(
	FRDGBuilder& GraphBuilder,
	const FBulgeDispatchParams& Params,
	FRDGBufferRef InputPositionsBuffer,
	FRDGBufferRef BulgeVertexIndicesBuffer,
	FRDGBufferRef BulgeInfluencesBuffer,
	FRDGBufferRef VolumeAccumBuffer,
	FRDGBufferRef OutputPositionsBuffer,
	FRDGTextureRef SDFTexture,
	FRHIGPUBufferReadback* Readback);
