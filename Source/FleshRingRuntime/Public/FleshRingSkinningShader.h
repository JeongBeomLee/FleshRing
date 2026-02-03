// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Skinning Shader
// ============================================================================
// Purpose: Apply GPU skinning to cached TightenedBindPose
//
// This shader is used after TightenedBindPose is cached.
// Runs every frame to apply current animation pose.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"

// ============================================================================
// FFleshRingSkinningCS - Skinning Compute Shader
// ============================================================================
// Processes ALL mesh vertices with skinning only (no tightness)
class FFleshRingSkinningCS : public FGlobalShader
{
public:
    // Register this class to UE shader system
    DECLARE_GLOBAL_SHADER(FFleshRingSkinningCS);

    // Declare using parameter struct
    SHADER_USE_PARAMETER_STRUCT(FFleshRingSkinningCS, FGlobalShader);

    // Shader Parameters - Must match FleshRingSkinningCS.usf
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // ===== Input Buffers (SRV - Read Only) =====

        // Input: TightenedBindPose (cached positions)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, SourcePositions)

        // Input: Original tangents from bind pose (TangentX=Normal, TangentZ=Tangent)
        // Format: SNORM float4 (hardware auto-converts)
        SHADER_PARAMETER_SRV(Buffer<float4>, SourceTangents)

        // Input: Recomputed normals from NormalRecomputeCS (optional)
        // Format: 3 floats per vertex, (0,0,0) = use SourceTangents
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, RecomputedNormals)

        // Input: Recomputed tangents from TangentRecomputeCS (optional, Gram-Schmidt orthonormalized)
        // Format: 8 floats per vertex (TangentX.xyzw + TangentZ.xyzw), (0,0,0,...) = use SourceTangents
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, RecomputedTangents)

        // ===== Output Buffers (UAV - Read/Write) =====

        // Output: Skinned vertex positions (current frame)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPositions)

        // Output: Previous frame skinned positions for TAA/TSR velocity
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPreviousPositions)

        // Output: Skinned tangents (TangentX=Normal, TangentZ=Tangent)
        // Optimus approach: PF_R16G16B16A16_SNORM (16-bit per channel)
        // In HLSL: TANGENT_RWBUFFER_FORMAT (GpuSkinCommon.ush)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, OutputTangents)

        // ===== Skinning Buffers (SRV - Read Only) =====

        // Bone matrices (3 float4 per bone = 3x4 matrix)
        // RefToLocal: [Bind Pose Component Space] -> [Animated Component Space]
        // Using RHI SRV directly (not RDG) because bone matrices are managed externally
        SHADER_PARAMETER_SRV(Buffer<float4>, BoneMatrices)

        // Previous frame bone matrices for TAA/TSR velocity calculation
        SHADER_PARAMETER_SRV(Buffer<float4>, PreviousBoneMatrices)

        // Packed bone indices + weights
        // Using RHI SRV directly (not RDG) because weight stream is managed externally
        SHADER_PARAMETER_SRV(Buffer<uint>, InputWeightStream)

        // ===== Skinning Parameters =====

        SHADER_PARAMETER(uint32, InputWeightStride)    // Weight stream stride (bytes)
        SHADER_PARAMETER(uint32, InputWeightIndexSize) // Index/weight byte size
        SHADER_PARAMETER(uint32, NumBoneInfluences)    // Number of bone influences per vertex

        // ===== Section Parameters (like WaveCS) =====

        SHADER_PARAMETER(uint32, BaseVertexIndex)      // Section's base vertex index in LOD
        SHADER_PARAMETER(uint32, NumVertices)          // Section's vertex count

        // ===== Debug/Feature Flags =====

        SHADER_PARAMETER(uint32, bProcessTangents)     // Whether to process tangents (0 = Position only, 1 = Position + Tangent)
        SHADER_PARAMETER(uint32, bProcessPreviousPosition) // Whether to process Previous Position (0 = current only, 1 = current + Previous)
        SHADER_PARAMETER(uint32, bUseRecomputedNormals) // Whether to use recomputed normals (0 = SourceTangents only, 1 = RecomputedNormals preferred)
        SHADER_PARAMETER(uint32, bUseRecomputedTangents) // Whether to use recomputed tangents (0 = SourceTangents only, 1 = RecomputedTangents preferred)
        SHADER_PARAMETER(uint32, bPassthroughSkinning)  // Skip bone skinning, copy positions/tangents directly (1 = T-pose optimization)
    END_SHADER_PARAMETER_STRUCT()

    // Shader Compilation Settings
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        // SM5 = Shader Model 5 (~=DX11)
        // Require SM5 for compute shader support
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

        // Thread group size = 64 (must match .usf)
        OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);

        // GPUSKIN_* defines removed - using GpuSkinCommon.ush defaults like WaveCS
        // Defaults: GPUSKIN_BONE_INDEX_UINT16=0 (8-bit), GPUSKIN_USE_EXTRA_INFLUENCES=0 (4 bones)
        // Must match the mesh's actual settings
    }
};

// ============================================================================
// FSkinningDispatchParams - Dispatch Parameters
// ============================================================================

/**
 * Structure that encapsulates the parameters to be passed to the Dispatch function
 */
struct FSkinningDispatchParams
{
    // =========== Section Parameters (like WaveCS) ===========

    /**
     * Section's base vertex index in LOD (for GlobalVertexIndex calculation)
     */
    uint32 BaseVertexIndex = 0;

    /**
     * Section's vertex count (dispatch size)
     */
    uint32 NumVertices = 0;

    // =========== Skinning Parameters ===========

    /**
     * Weight stream stride in bytes
     */
    uint32 InputWeightStride = 0;

    /**
     * Packed: BoneIndexByteSize | (BoneWeightByteSize << 8)
     */
    uint32 InputWeightIndexSize = 0;

    /**
     * Number of bone influences per vertex (4 or 8)
     */
    uint32 NumBoneInfluences = 0;

    /**
     * Skip bone skinning and copy positions/tangents directly (T-pose optimization).
     * When true, avoids GPU FP non-determinism from identity bone transforms.
     *
     * Currently always set to true (editor T-pose only, where RefToLocal = Identity).
     * Must be set to false when animation preview is added (bone matrices != Identity).
     */
    bool bPassthroughSkinning = false;
};

// ============================================================================
// Dispatch Function Declaration
// ============================================================================

/**
 * Dispatch SkinningCS to apply GPU skinning to all vertices
 *
 * @param GraphBuilder - RDG builder for resource management
 * @param Params - Skinning dispatch parameters
 * @param SourcePositionsBuffer - TightenedBindPose buffer (cached, RDG)
 * @param SourceTangentsSRV - Original bind pose tangents SRV (RHI)
 * @param OutputPositionsBuffer - Output skinned positions (UAV, RDG)
 * @param OutputPreviousPositionsBuffer - Output previous positions for TAA/TSR velocity (UAV, RDG), can be nullptr
 * @param OutputTangentsBuffer - Output skinned tangents (UAV, RDG)
 * @param BoneMatricesSRV - Current frame bone matrices SRV (RHI, 3 float4 per bone)
 * @param PreviousBoneMatricesSRV - Previous frame bone matrices SRV for velocity (RHI), can be nullptr
 * @param InputWeightStreamSRV - Packed bone indices + weights SRV (RHI)
 * @param RecomputedNormalsBuffer - Recomputed normals from NormalRecomputeCS (RDG, optional, can be nullptr)
 * @param RecomputedTangentsBuffer - Recomputed tangents from TangentRecomputeCS (RDG, optional, can be nullptr)
 */
void DispatchFleshRingSkinningCS(
    FRDGBuilder& GraphBuilder,
    const FSkinningDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRHIShaderResourceView* SourceTangentsSRV,
    FRDGBufferRef OutputPositionsBuffer,
    FRDGBufferRef OutputPreviousPositionsBuffer,
    FRDGBufferRef OutputTangentsBuffer,
    FRHIShaderResourceView* BoneMatricesSRV,
    FRHIShaderResourceView* PreviousBoneMatricesSRV,
    FRHIShaderResourceView* InputWeightStreamSRV,
    FRDGBufferRef RecomputedNormalsBuffer = nullptr,
    FRDGBufferRef RecomputedTangentsBuffer = nullptr);
