// ============================================================================
// FleshRing Skinning Shader
// FleshRing 스키닝 셰이더
// ============================================================================
// Purpose: Apply GPU skinning to cached TightenedBindPose
// 목적: 캐싱된 TightenedBindPose에 GPU 스키닝 적용
//
// This shader is used after TightenedBindPose is cached.
// Runs every frame to apply current animation pose.
// TightenedBindPose가 캐싱된 이후 사용됩니다.
// 매 프레임 현재 애니메이션 포즈를 적용합니다.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"

// ============================================================================
// FFleshRingSkinningCS - Skinning Compute Shader
// 스키닝 컴퓨트 셰이더
// ============================================================================
// Processes ALL mesh vertices with skinning only (no tightness)
// 모든 메시 버텍스에 스키닝만 적용 (tightness 없음)
class FFleshRingSkinningCS : public FGlobalShader
{
public:
    // Register this class to UE shader system
    // UE 셰이더 시스템에 이 클래스 등록
    DECLARE_GLOBAL_SHADER(FFleshRingSkinningCS);

    // Declare using parameter struct
    // 파라미터 구조체 사용 선언
    SHADER_USE_PARAMETER_STRUCT(FFleshRingSkinningCS, FGlobalShader);

    // Shader Parameters - Must match FleshRingSkinningCS.usf
    // 셰이더 파라미터 - FleshRingSkinningCS.usf와 일치해야 함
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // ===== Input Buffers (SRV - Read Only) =====
        // ===== 입력 버퍼 (SRV - 읽기 전용) =====

        // Input: TightenedBindPose (cached positions)
        // 입력: TightenedBindPose (캐싱된 위치)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, SourcePositions)

        // Input: Original tangents from bind pose (TangentX=Normal, TangentZ=Tangent)
        // 입력: 바인드 포즈의 원본 탄젠트 (TangentX=노멀, TangentZ=탄젠트)
        // Format: SNORM float4 (hardware auto-converts)
        SHADER_PARAMETER_SRV(Buffer<float4>, SourceTangents)

        // Input: Recomputed normals from NormalRecomputeCS (optional)
        // 입력: NormalRecomputeCS에서 재계산된 노멀 (선택적)
        // Format: 3 floats per vertex, (0,0,0) = use SourceTangents
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, RecomputedNormals)

        // ===== Output Buffers (UAV - Read/Write) =====
        // ===== 출력 버퍼 (UAV - 읽기/쓰기) =====

        // Output: Skinned vertex positions (current frame)
        // 출력: 스키닝된 버텍스 위치 (현재 프레임)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPositions)

        // Output: Previous frame skinned positions for TAA/TSR velocity
        // 출력: TAA/TSR velocity용 이전 프레임 스키닝된 위치
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPreviousPositions)

        // Output: Skinned tangents (TangentX=Normal, TangentZ=Tangent)
        // 출력: 스키닝된 탄젠트 (TangentX=노멀, TangentZ=탄젠트)
        // Optimus 방식: PF_R16G16B16A16_SNORM (16-bit per channel)
        // HLSL에서는 TANGENT_RWBUFFER_FORMAT (GpuSkinCommon.ush)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, OutputTangents)

        // ===== Skinning Buffers (SRV - Read Only) =====
        // ===== 스키닝 버퍼 (SRV - 읽기 전용) =====

        // Bone matrices (3 float4 per bone = 3x4 matrix)
        // RefToLocal: [Bind Pose Component Space] -> [Animated Component Space]
        // Using RHI SRV directly (not RDG) because bone matrices are managed externally
        // 본 행렬 (본당 3개의 float4 = 3x4 행렬)
        // RHI SRV 직접 사용 (RDG 아님) - 본 행렬은 외부에서 관리됨
        SHADER_PARAMETER_SRV(Buffer<float4>, BoneMatrices)

        // Previous frame bone matrices for TAA/TSR velocity calculation
        // TAA/TSR velocity 계산용 이전 프레임 본 행렬
        SHADER_PARAMETER_SRV(Buffer<float4>, PreviousBoneMatrices)

        // Packed bone indices + weights
        // Using RHI SRV directly (not RDG) because weight stream is managed externally
        // 패킹된 본 인덱스 + 웨이트
        // RHI SRV 직접 사용 (RDG 아님) - 웨이트 스트림은 외부에서 관리됨
        SHADER_PARAMETER_SRV(Buffer<uint>, InputWeightStream)

        // ===== Skinning Parameters =====
        // ===== 스키닝 파라미터 =====

        SHADER_PARAMETER(uint32, InputWeightStride)    // 웨이트 스트림 스트라이드 (바이트)
        SHADER_PARAMETER(uint32, InputWeightIndexSize) // 인덱스/웨이트 바이트 크기
        SHADER_PARAMETER(uint32, NumBoneInfluences)    // 버텍스당 본 영향 수

        // ===== Section Parameters (like WaveCS) =====
        // ===== Section 파라미터 (WaveCS와 동일) =====

        SHADER_PARAMETER(uint32, BaseVertexIndex)      // Section's base vertex index in LOD
        SHADER_PARAMETER(uint32, NumVertices)          // Section's vertex count

        // ===== Debug/Feature Flags =====
        // ===== 디버그/기능 플래그 =====

        SHADER_PARAMETER(uint32, bProcessTangents)     // 탄젠트 처리 여부 (0 = Position만, 1 = Position + Tangent)
        SHADER_PARAMETER(uint32, bProcessPreviousPosition) // Previous Position 처리 여부 (0 = 현재만, 1 = 현재 + Previous)
        SHADER_PARAMETER(uint32, bUseRecomputedNormals) // 재계산된 노멀 사용 여부 (0 = SourceTangents만, 1 = RecomputedNormals 우선)
    END_SHADER_PARAMETER_STRUCT()

    // Shader Compilation Settings
    // 셰이더 컴파일 설정
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        // SM5 = Shader Model 5 (~=DX11)
        // Require SM5 for compute shader support
        // SM5 이상에서만 컴퓨트 셰이더 지원
        return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
    }

    static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
    {
        FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

        // Thread group size = 64 (must match .usf)
        // 스레드 그룹 크기 = 64 (.usf와 일치해야 함)
        OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);

        // GPUSKIN_* 디파인 제거 - WaveCS와 동일하게 GpuSkinCommon.ush 기본값 사용
        // 기본값: GPUSKIN_BONE_INDEX_UINT16=0 (8비트), GPUSKIN_USE_EXTRA_INFLUENCES=0 (4본)
        // 메시의 실제 설정에 맞춰야 함
    }
};

// ============================================================================
// FSkinningDispatchParams - Dispatch Parameters
// 디스패치 파라미터 구조체
// ============================================================================

/**
 * Structure that encapsulates the parameters to be passed to the Dispatch function
 * GPU Dispatch에 전달할 파라미터를 캡슐화하는 구조체
 */
struct FSkinningDispatchParams
{
    // =========== Section Parameters (like WaveCS) ===========
    // =========== Section 파라미터 (WaveCS와 동일) ===========

    /**
     * Section's base vertex index in LOD (for GlobalVertexIndex calculation)
     * LOD 내 Section의 시작 버텍스 인덱스 (GlobalVertexIndex 계산용)
     */
    uint32 BaseVertexIndex = 0;

    /**
     * Section's vertex count (dispatch size)
     * Section의 버텍스 수 (디스패치 크기)
     */
    uint32 NumVertices = 0;

    // =========== Skinning Parameters ===========

    /**
     * Weight stream stride in bytes
     * 웨이트 스트림 스트라이드 (바이트)
     */
    uint32 InputWeightStride = 0;

    /**
     * Packed: BoneIndexByteSize | (BoneWeightByteSize << 8)
     * 패킹: 본인덱스바이트크기 | (본웨이트바이트크기 << 8)
     */
    uint32 InputWeightIndexSize = 0;

    /**
     * Number of bone influences per vertex (4 or 8)
     * 버텍스당 본 영향 수 (4 또는 8)
     */
    uint32 NumBoneInfluences = 0;
};

// ============================================================================
// Dispatch Function Declaration
// Dispatch 함수 선언
// ============================================================================

/**
 * Dispatch SkinningCS to apply GPU skinning to all vertices
 * SkinningCS를 디스패치하여 전체 버텍스에 GPU 스키닝 적용
 *
 * @param GraphBuilder - RDG builder for resource management
 *                       RDG 빌더 (리소스 관리용)
 * @param Params - Skinning dispatch parameters
 *                 스키닝 디스패치 파라미터
 * @param SourcePositionsBuffer - TightenedBindPose buffer (cached, RDG)
 *                                TightenedBindPose 버퍼 (캐싱됨, RDG)
 * @param SourceTangentsSRV - Original bind pose tangents SRV (RHI)
 *                            원본 바인드 포즈 탄젠트 SRV (RHI)
 * @param OutputPositionsBuffer - Output skinned positions (UAV, RDG)
 *                                스키닝된 위치 출력 버퍼 (UAV, RDG)
 * @param OutputPreviousPositionsBuffer - Output previous positions for TAA/TSR velocity (UAV, RDG), can be nullptr
 *                                        TAA/TSR velocity용 이전 위치 출력 버퍼 (UAV, RDG), nullptr 가능
 * @param OutputTangentsBuffer - Output skinned tangents (UAV, RDG)
 *                               스키닝된 탄젠트 출력 버퍼 (UAV, RDG)
 * @param BoneMatricesSRV - Current frame bone matrices SRV (RHI, 3 float4 per bone)
 *                          현재 프레임 본 행렬 SRV (RHI, 본당 3개 float4)
 * @param PreviousBoneMatricesSRV - Previous frame bone matrices SRV for velocity (RHI), can be nullptr
 *                                  velocity용 이전 프레임 본 행렬 SRV (RHI), nullptr 가능
 * @param InputWeightStreamSRV - Packed bone indices + weights SRV (RHI)
 *                               패킹된 본 인덱스 + 웨이트 SRV (RHI)
 * @param RecomputedNormalsBuffer - Recomputed normals from NormalRecomputeCS (RDG, optional, can be nullptr)
 *                                  NormalRecomputeCS에서 재계산된 노멀 (RDG, 선택적, nullptr 가능)
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
    FRDGBufferRef RecomputedNormalsBuffer = nullptr);
