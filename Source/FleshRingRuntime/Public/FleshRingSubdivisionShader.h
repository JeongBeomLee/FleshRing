// FleshRingSubdivisionShader.h
// GPU Barycentric Interpolation Shader for Subdivision
//
// CPU가 Red-Green Refinement / LEB로 토폴로지를 결정하고,
// GPU는 실제 버텍스 데이터 보간만 수행

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "FleshRingSubdivisionProcessor.h"

// ============================================================================
// FFleshRingBarycentricInterpolationCS - Barycentric Interpolation Shader
// ============================================================================
class FFleshRingBarycentricInterpolationCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FFleshRingBarycentricInterpolationCS);
	SHADER_USE_PARAMETER_STRUCT(FFleshRingBarycentricInterpolationCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// ===== Source Mesh Data (SRV) =====
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, SourcePositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, SourceNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, SourceUVs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, SourceBoneWeights)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SourceBoneIndices)

		// ===== Subdivision Topology from CPU (SRV) =====
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, VertexParentIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, VertexBarycentrics)

		// ===== Output Buffers (UAV) =====
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputUVs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputBoneWeights)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, OutputBoneIndices)

		// ===== Parameters =====
		SHADER_PARAMETER(uint32, NumOutputVertices)
		SHADER_PARAMETER(uint32, NumBoneInfluences)
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
// FSubdivisionInterpolationParams - Dispatch Parameters
// ============================================================================
struct FSubdivisionInterpolationParams
{
	uint32 NumOutputVertices = 0;
	uint32 NumSourceVertices = 0;
	uint32 NumBoneInfluences = 4;
};

// ============================================================================
// FSubdivisionGPUBuffers - GPU Buffer Container
// ============================================================================
struct FSubdivisionGPUBuffers
{
	// Source mesh data (from SkeletalMesh)
	FRDGBufferRef SourcePositions = nullptr;
	FRDGBufferRef SourceNormals = nullptr;
	FRDGBufferRef SourceUVs = nullptr;
	FRDGBufferRef SourceBoneWeights = nullptr;
	FRDGBufferRef SourceBoneIndices = nullptr;

	// Subdivision topology (from CPU FSubdivisionTopologyResult)
	FRDGBufferRef VertexParentIndices = nullptr;
	FRDGBufferRef VertexBarycentrics = nullptr;

	// Output subdivided mesh
	FRDGBufferRef OutputPositions = nullptr;
	FRDGBufferRef OutputNormals = nullptr;
	FRDGBufferRef OutputUVs = nullptr;
	FRDGBufferRef OutputBoneWeights = nullptr;
	FRDGBufferRef OutputBoneIndices = nullptr;

	// Output triangle indices (직접 복사, 보간 불필요)
	FRDGBufferRef OutputIndices = nullptr;
};

// ============================================================================
// Dispatch Functions
// ============================================================================

/**
 * Dispatch Barycentric Interpolation shader
 *
 * @param GraphBuilder - RDG builder
 * @param Params - Interpolation parameters
 * @param Buffers - GPU buffers
 */
void DispatchFleshRingBarycentricInterpolationCS(
	FRDGBuilder& GraphBuilder,
	const FSubdivisionInterpolationParams& Params,
	const FSubdivisionGPUBuffers& Buffers);

/**
 * Create GPU buffers from CPU topology result
 *
 * @param GraphBuilder - RDG builder
 * @param TopologyResult - CPU-computed subdivision topology
 * @param Params - Output interpolation parameters
 * @param OutBuffers - Output GPU buffers
 */
void CreateSubdivisionGPUBuffersFromTopology(
	FRDGBuilder& GraphBuilder,
	const FSubdivisionTopologyResult& TopologyResult,
	FSubdivisionInterpolationParams& OutParams,
	FSubdivisionGPUBuffers& OutBuffers);

/**
 * Upload source mesh data to GPU
 *
 * @param GraphBuilder - RDG builder
 * @param SourcePositions - CPU position array
 * @param SourceNormals - CPU normal array
 * @param SourceUVs - CPU UV array
 * @param SourceBoneWeights - CPU bone weight array
 * @param SourceBoneIndices - CPU bone index array
 * @param NumBoneInfluences - Bone influences per vertex
 * @param OutBuffers - Output GPU buffers
 */
void UploadSourceMeshToGPU(
	FRDGBuilder& GraphBuilder,
	const TArray<FVector>& SourcePositions,
	const TArray<FVector>& SourceNormals,
	const TArray<FVector2D>& SourceUVs,
	const TArray<float>& SourceBoneWeights,
	const TArray<uint32>& SourceBoneIndices,
	uint32 NumBoneInfluences,
	FSubdivisionGPUBuffers& OutBuffers);

/**
 * Execute full subdivision pipeline:
 * 1. Upload source mesh to GPU
 * 2. Upload topology result to GPU
 * 3. Dispatch interpolation shader
 *
 * @param GraphBuilder - RDG builder
 * @param Processor - CPU subdivision processor (must have valid cached result)
 * @param SourceNormals - Source mesh normals
 * @param SourceBoneWeights - Source bone weights (flat array)
 * @param SourceBoneIndices - Source bone indices (flat array)
 * @param NumBoneInfluences - Bone influences per vertex
 * @param OutBuffers - Output subdivided mesh buffers
 * @return Success
 */
bool ExecuteSubdivisionInterpolation(
	FRDGBuilder& GraphBuilder,
	const FFleshRingSubdivisionProcessor& Processor,
	const TArray<FVector>& SourceNormals,
	const TArray<float>& SourceBoneWeights,
	const TArray<uint32>& SourceBoneIndices,
	uint32 NumBoneInfluences,
	FSubdivisionGPUBuffers& OutBuffers);
