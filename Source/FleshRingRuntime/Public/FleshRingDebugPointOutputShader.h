// FleshRingDebugPointOutputShader.h
// Purpose: Output debug points at final transformed positions (after all deformation passes)

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "FleshRingDebugTypes.h"

/**
 * Debug Point Output Compute Shader
 * Outputs debug points at final transformed positions (after all CS passes)
 */
class FFleshRingDebugPointOutputCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFleshRingDebugPointOutputCS);
	SHADER_USE_PARAMETER_STRUCT(FFleshRingDebugPointOutputCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// Input Buffers
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, FinalPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, VertexIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, Influences)  // GPU에서 계산된 Influence (RWBuffer<float>에서 읽기)

		// Output Buffer
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FFleshRingDebugPoint>, DebugPointBuffer)

		// Parameters
		SHADER_PARAMETER(uint32, NumVertices)
		SHADER_PARAMETER(uint32, NumTotalVertices)
		SHADER_PARAMETER(uint32, RingIndex)
		SHADER_PARAMETER(uint32, BaseOffset)           // 출력 버퍼 오프셋
		SHADER_PARAMETER(uint32, InfluenceBaseOffset)  // Influence 버퍼 오프셋 (다중 Ring 지원)
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
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

/**
 * Dispatch parameters for debug point output
 */
struct FDebugPointOutputDispatchParams
{
	uint32 NumVertices = 0;           // Number of vertices to process
	uint32 NumTotalVertices = 0;      // Total mesh vertex count
	uint32 RingIndex = 0;             // Ring index
	uint32 BaseOffset = 0;            // Base offset in output buffer
	uint32 InfluenceBaseOffset = 0;   // Base offset in influence buffer (multi-ring support)
	FMatrix44f LocalToWorld = FMatrix44f::Identity;  // Local to world transform
};

/**
 * Dispatch debug point output compute shader
 *
 * @param GraphBuilder - RDG builder
 * @param Params - Dispatch parameters
 * @param FinalPositionsBuffer - Final transformed positions (after all CS passes)
 * @param VertexIndicesBuffer - Vertex indices to output
 * @param InfluencesBuffer - GPU-computed influence values (from TightnessCS DebugInfluences)
 * @param DebugPointBuffer - Output debug point buffer
 */
void DispatchFleshRingDebugPointOutputCS(
	FRDGBuilder& GraphBuilder,
	const FDebugPointOutputDispatchParams& Params,
	FRDGBufferRef FinalPositionsBuffer,
	FRDGBufferRef VertexIndicesBuffer,
	FRDGBufferRef InfluencesBuffer,
	FRDGBufferRef DebugPointBuffer);
