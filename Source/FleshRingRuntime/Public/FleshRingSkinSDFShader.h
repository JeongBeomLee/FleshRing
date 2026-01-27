// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphBuilder.h"

// ============================================================================
// Skin SDF Layer Separation Shader Parameters
// Skin SDF-based layer separation shader parameters
// ============================================================================
// Define an implicit surface using skin vertex positions/normals,
// and push stocking vertices outward if they are inside the skin
//
// Core algorithm:
// 1. Find the closest skin vertex
// 2. SignedDist = dot(stocking_pos - skin_pos, skin_normal)
// 3. Push outward if SignedDist < MinSeparation

struct FSkinSDFDispatchParams
{
	// Number of stocking vertices to process
	uint32 NumStockingVertices = 0;

	// Number of skin vertices
	uint32 NumSkinVertices = 0;

	// Total mesh vertex count
	uint32 NumTotalVertices = 0;

	// Minimum separation distance (cm) - push out when penetration below this
	float MinSeparation = 0.01f;  // 0.1mm

	// Target separation distance (cm) - target distance to maintain contact
	float TargetSeparation = 0.02f;  // 0.2mm (visual contact)

	// Maximum push distance (per iteration, cm)
	float MaxPushDistance = 1.0f;  // 1cm

	// Maximum pull distance (per iteration, cm) - prevents floating
	float MaxPullDistance = 0.0f;  // disabled

	// Maximum iteration count (early exit when penetration resolved)
	uint32 MaxIterations = 20;

	// Ring axis (fallback for normal direction)
	FVector3f RingAxis = FVector3f(0, 0, 1);

	// Ring center
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
		// Vertex positions (read/write)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, PositionsRW)

		// Skin vertex indices
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SkinVertexIndices)

		// Skin vertex normals (post-deformation)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, SkinNormals)

		// Stocking vertex indices
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, StockingVertexIndices)

		// Parameters
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
