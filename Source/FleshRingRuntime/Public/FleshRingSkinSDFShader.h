// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphBuilder.h"

// ============================================================================
// Skin SDF Layer Separation Shader Parameters
// 스킨 SDF 기반 레이어 분리 셰이더 파라미터
// ============================================================================
// 스킨 버텍스들의 위치/노멀로 implicit surface를 정의하고,
// 스타킹 버텍스가 스킨 안쪽에 있으면 바깥으로 밀어냄
//
// 핵심 알고리즘:
// 1. 가장 가까운 스킨 버텍스 찾기
// 2. SignedDist = dot(stocking_pos - skin_pos, skin_normal)
// 3. SignedDist < MinSeparation이면 바깥으로 밀어냄

struct FSkinSDFDispatchParams
{
	// 처리할 스타킹 버텍스 수
	uint32 NumStockingVertices = 0;

	// 스킨 버텍스 수
	uint32 NumSkinVertices = 0;

	// 전체 메시 버텍스 수
	uint32 NumTotalVertices = 0;

	// 최소 분리 거리 (cm) - 이 아래로 침투 시 밀어냄
	float MinSeparation = 0.01f;  // 0.1mm

	// 목표 분리 거리 (cm) - 접촉 유지를 위한 목표 거리
	float TargetSeparation = 0.02f;  // 0.2mm (시각적 접촉)

	// 최대 밀어내기 거리 (iteration당, cm)
	float MaxPushDistance = 1.0f;  // 1cm

	// 최대 당기기 거리 (iteration당, cm) - 부유 방지
	float MaxPullDistance = 0.0f;  // 비활성화

	// 최대 반복 횟수 (침투 해결 시 조기 종료)
	uint32 MaxIterations = 20;

	// Ring 축 (노멀 방향 폴백용)
	FVector3f RingAxis = FVector3f(0, 0, 1);

	// Ring 중심
	FVector3f RingCenter = FVector3f::ZeroVector;
};

// ============================================================================
// Skin SDF Layer Separation Compute Shader
// ============================================================================

class FSkinSDFLayerSeparationCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSkinSDFLayerSeparationCS);
	SHADER_USE_PARAMETER_STRUCT(FSkinSDFLayerSeparationCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// 버텍스 위치 (읽기/쓰기)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, PositionsRW)

		// 스킨 버텍스 인덱스
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SkinVertexIndices)

		// 스킨 버텍스 노멀 (변형 후)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, SkinNormals)

		// 스타킹 버텍스 인덱스
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, StockingVertexIndices)

		// 파라미터
		SHADER_PARAMETER(uint32, NumStockingVertices)
		SHADER_PARAMETER(uint32, NumSkinVertices)
		SHADER_PARAMETER(uint32, NumTotalVertices)
		SHADER_PARAMETER(float, MinSeparation)
		SHADER_PARAMETER(float, TargetSeparation)
		SHADER_PARAMETER(float, MaxPushDistance)
		SHADER_PARAMETER(float, MaxPullDistance)
		SHADER_PARAMETER(uint32, MaxIterations)
		SHADER_PARAMETER(FVector3f, RingAxis)
		SHADER_PARAMETER(FVector3f, RingCenter)
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

// ============================================================================
// Dispatch Function
// ============================================================================

void DispatchFleshRingSkinSDFCS(
	FRDGBuilder& GraphBuilder,
	const FSkinSDFDispatchParams& Params,
	FRDGBufferRef PositionsBuffer,
	FRDGBufferRef SkinVertexIndicesBuffer,
	FRDGBufferRef SkinNormalsBuffer,
	FRDGBufferRef StockingVertexIndicesBuffer);

// Multi-pass version (iterative refinement)
void DispatchFleshRingSkinSDFCS_MultiPass(
	FRDGBuilder& GraphBuilder,
	const FSkinSDFDispatchParams& Params,
	FRDGBufferRef PositionsBuffer,
	FRDGBufferRef SkinVertexIndicesBuffer,
	FRDGBufferRef SkinNormalsBuffer,
	FRDGBufferRef StockingVertexIndicesBuffer);
