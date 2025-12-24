// Purpose: Creating tight flesh appearance
#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "FleshRingAffectedVertices.h"

// Processes only AffectedVertices (not all mesh vertices) for performance
// Pulls vertices inward toward Ring center axis
class FFleshRingTightnessCS : public FGlobalShader
{
public:
    // Register this class to UE shader system
    DECLARE_GLOBAL_SHADER(FFleshRingTightnessCS);

    // Declare using parameter struct 
    SHADER_USE_PARAMETER_STRUCT(FFleshRingTightnessCS, FGlobalShader);

    // Shader Parameters - Must match FleshRingTightnessCS.usf
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // SRV is Read Only
        // Input: Original vertex positions (all mesh vertices)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, SourcePositions)

        // Input: Indices of affected vertices to process
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AffectedIndices)

        // Input: Per-vertex influence weights (0-1)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, Influences)

        // UAV is Read and Write
        // Output: Deformed vertex positions
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPositions)

        // Ring parameters (to constant buffer)
        SHADER_PARAMETER(FVector3f, RingCenter)
        SHADER_PARAMETER(FVector3f, RingAxis)
        SHADER_PARAMETER(float, TightnessStrength)
        SHADER_PARAMETER(float, RingRadius)
        SHADER_PARAMETER(float, RingWidth)

        // Counts
        SHADER_PARAMETER(uint32, NumAffectedVertices)
        SHADER_PARAMETER(uint32, NumTotalVertices)
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
        OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), 64);

        // [삭제됨] FALLOFF_TYPE define
        // Influence 값은 CPU에서 FalloffType에 따라 계산되어 GPU로 전달됨
    }
};

// Structure that Encapsulates the parameters to be passed to the Dispatch function
// Makes it easier to modify parameters without changing function signatures
struct FTightnessDispatchParams
{
    // Ring transform
    FVector3f RingCenter;
    FVector3f RingAxis;

    // Ring geometry
    float RingRadius;
    float RingWidth;

    // Deformation strength
    float TightnessStrength;

    // Vertex counts
    uint32 NumAffectedVertices;
    uint32 NumTotalVertices;

    FTightnessDispatchParams()
        : RingCenter(FVector3f::ZeroVector)
        , RingAxis(FVector3f::UpVector)
        , RingRadius(5.0f)
        , RingWidth(2.0f)
        , TightnessStrength(1.0f)
        , NumAffectedVertices(0)
        , NumTotalVertices(0)
    {
    }
};

// ============================================================================
// [추가] FRingAffectedData에서 FTightnessDispatchParams로 변환하는 헬퍼 함수
// FFleshRingSettings → FRingAffectedData → FTightnessDispatchParams 데이터 흐름 완성
// ============================================================================

/**
 * FRingAffectedData에서 FTightnessDispatchParams 생성
 *
 * @param AffectedData - 영향받는 버텍스 데이터 (Ring 파라미터 포함)
 * @param TotalVertexCount - 메시 전체 버텍스 수
 * @return GPU Dispatch용 파라미터 구조체
 */
inline FTightnessDispatchParams CreateTightnessParams(
    const FRingAffectedData& AffectedData,
    uint32 TotalVertexCount)
{
    FTightnessDispatchParams Params;

    // [변환] Ring 트랜스폼 정보
    Params.RingCenter = FVector3f(AffectedData.RingCenter);
    Params.RingAxis = FVector3f(AffectedData.RingAxis);

    // [변환] Ring 지오메트리 정보
    Params.RingRadius = AffectedData.RingRadius;
    Params.RingWidth = AffectedData.RingWidth;

    // [변환] 변형 강도 (FFleshRingSettings에서 복사된 값)
    Params.TightnessStrength = AffectedData.TightnessStrength;

    // [변환] 버텍스 카운트
    Params.NumAffectedVertices = static_cast<uint32>(AffectedData.Vertices.Num());
    Params.NumTotalVertices = TotalVertexCount;

    return Params;
}

// Dispatch Function Declarations
/**
 * Dispatch TightnessCS to process affected vertices
 *
 * @param GraphBuilder - RDG builder for resource management
 * @param Params - Dispatch parameters (Ring settings, counts ...)
 * @param SourcePositionsBuffer - RDG buffer containing source vertex positions
 * @param AffectedIndicesBuffer - Buffer containing vertex indices to process
 * @param InfluencesBuffer - Buffer containing per-vertex influence weights
 * @param OutputPositionsBuffer - UAV buffer for deformed positions
 */
void DispatchFleshRingTightnessCS(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef OutputPositionsBuffer);

/**
 * Dispatch TightnessCS with readback for validation/testing
 *
 * @param GraphBuilder - RDG builder for resource management
 * @param Params - Dispatch parameters
 * @param SourcePositionsBuffer - RDG buffer containing source vertex positions
 * @param AffectedIndicesBuffer - Buffer containing vertex indices to process
 * @param InfluencesBuffer - Buffer containing per-vertex influence weights
 * @param OutputPositionsBuffer - UAV buffer for deformed positions
 * @param Readback - Readback object for GPU->CPU transfer
 */
void DispatchFleshRingTightnessCS_WithReadback(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRHIGPUBufferReadback* Readback);
