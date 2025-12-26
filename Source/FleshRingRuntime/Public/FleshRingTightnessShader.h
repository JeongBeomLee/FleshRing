// ============================================================================
// FleshRing Tightness Shader
// FleshRing 조이기(Tightness) 셰이더
// ============================================================================
// Purpose: Creating tight flesh appearance
// 목적: 살이 조여지는 효과 생성

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "FleshRingAffectedVertices.h"

// ============================================================================
// FFleshRingTightnessCS - Tightness Compute Shader
// 조이기 효과 컴퓨트 셰이더
// ============================================================================
// Processes only AffectedVertices (not all mesh vertices) for performance
// Pulls vertices inward toward Ring center axis
// 성능 최적화를 위해 영향받는 버텍스만 처리 (전체 메시 버텍스 X)
// 버텍스를 링 중심축 방향으로 안쪽으로 당김
class FFleshRingTightnessCS : public FGlobalShader
{
public:
    // Register this class to UE shader system
    // UE 셰이더 시스템에 이 클래스 등록
    DECLARE_GLOBAL_SHADER(FFleshRingTightnessCS);

    // Declare using parameter struct
    // 파라미터 구조체 사용 선언
    SHADER_USE_PARAMETER_STRUCT(FFleshRingTightnessCS, FGlobalShader);

    // Shader Parameters - Must match FleshRingTightnessCS.usf
    // 셰이더 파라미터 - FleshRingTightnessCS.usf와 일치해야 함
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // ===== Input Buffers (SRV - Read Only) =====
        // ===== 입력 버퍼 (SRV - 읽기 전용) =====

        // Input: Original vertex positions (all mesh vertices)
        // 입력: 원본 버텍스 위치 (메시 전체 버텍스)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, SourcePositions)

        // Input: Indices of affected vertices to process
        // 입력: 처리할 영향받는 버텍스 인덱스
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AffectedIndices)

        // Input: Per-vertex influence weights (0-1)
        // 입력: 버텍스별 영향도 (0~1)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, Influences)

        // ===== Output Buffer (UAV - Read/Write) =====
        // ===== 출력 버퍼 (UAV - 읽기/쓰기) =====

        // Output: Deformed vertex positions
        // 출력: 변형된 버텍스 위치
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPositions)

        // ===== Ring Parameters (Constant Buffer) =====
        // ===== 링 파라미터 (상수 버퍼) =====

        SHADER_PARAMETER(FVector3f, RingCenter)       // 링 중심 위치
        SHADER_PARAMETER(FVector3f, RingAxis)         // 링 축 방향
        SHADER_PARAMETER(float, TightnessStrength)    // 조이기 강도
        SHADER_PARAMETER(float, RingRadius)           // 링 내부 반지름
        SHADER_PARAMETER(float, RingWidth)            // 링 높이 (축 방향)

        // ===== Counts =====
        // ===== 버텍스 수 =====

        SHADER_PARAMETER(uint32, NumAffectedVertices) // 영향받는 버텍스 수
        SHADER_PARAMETER(uint32, NumTotalVertices)    // 전체 버텍스 수 (범위 체크용)
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

        // Note: Influence values are pre-calculated on CPU with FalloffType
        // 참고: Influence 값은 CPU에서 FalloffType에 따라 미리 계산되어 GPU로 전달됨
    }
};

/**
 * Structure that encapsulates the parameters to be passed to the Dispatch function
 * GPU Dispatch에 전달할 파라미터를 캡슐화하는 구조체
 * Makes it easier to modify parameters without changing function signatures
 */
struct FTightnessDispatchParams
{
    // =========== Ring Transform ===========

    /**
     * Ring center position (component space)
     * 링 중심 위치 (컴포넌트 스페이스)
     */
    FVector3f RingCenter;

    /**
     * Ring orientation axis (normalized)
     * 링 축 방향 (정규화됨)
     */
    FVector3f RingAxis;

    // =========== Ring Geometry ===========

    /**
     * Inner radius from bone axis to ring inner surface
     * 본 축에서 링 안쪽 면까지의 거리 (내부 반지름)
     */
    float RingRadius;

    /**
     * Ring height along axis direction (for GPU reference only)
     * 링 높이 - 축 방향 (GPU 참조용, 실제 변형에는 사용 안함)
     * Note: RingThickness는 CPU에서 버텍스 선택 시에만 사용됨
     */
    float RingWidth;

    // =========== Deformation Parameters ===========

    /**
     * Tightness deformation strength
     * 조이기(Tightness) 변형 강도
     */
    float TightnessStrength;

    // =========== Vertex Counts ===========

    /**
     * Number of affected vertices to process
     * 처리할 영향받는 버텍스 수
     */
    uint32 NumAffectedVertices;

    /**
     * Total mesh vertex count (for bounds checking)
     * 전체 메시 버텍스 수 (범위 체크용)
     */
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
// FRingAffectedData에서 FTightnessDispatchParams로 변환하는 헬퍼 함수
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

// ============================================================================
// Dispatch Function Declarations
// Dispatch 함수 선언
// ============================================================================

/**
 * Dispatch TightnessCS to process affected vertices
 * TightnessCS를 디스패치하여 영향받는 버텍스 처리
 *
 * @param GraphBuilder - RDG builder for resource management
 *                       RDG 빌더 (리소스 관리용)
 * @param Params - Dispatch parameters (Ring settings, counts ...)
 *                 디스패치 파라미터 (링 설정, 버텍스 수 등)
 * @param SourcePositionsBuffer - RDG buffer containing source vertex positions
 *                                원본 버텍스 위치 버퍼
 * @param AffectedIndicesBuffer - Buffer containing vertex indices to process
 *                                처리할 버텍스 인덱스 버퍼
 * @param InfluencesBuffer - Buffer containing per-vertex influence weights
 *                           버텍스별 영향도 버퍼
 * @param OutputPositionsBuffer - UAV buffer for deformed positions
 *                                변형된 위치 출력 버퍼 (UAV)
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
 * TightnessCS 디스패치 + GPU→CPU 리드백 (검증/테스트용)
 *
 * @param GraphBuilder - RDG builder for resource management
 *                       RDG 빌더 (리소스 관리용)
 * @param Params - Dispatch parameters
 *                 디스패치 파라미터
 * @param SourcePositionsBuffer - RDG buffer containing source vertex positions
 *                                원본 버텍스 위치 버퍼
 * @param AffectedIndicesBuffer - Buffer containing vertex indices to process
 *                                처리할 버텍스 인덱스 버퍼
 * @param InfluencesBuffer - Buffer containing per-vertex influence weights
 *                           버텍스별 영향도 버퍼
 * @param OutputPositionsBuffer - UAV buffer for deformed positions
 *                                변형된 위치 출력 버퍼 (UAV)
 * @param Readback - Readback object for GPU->CPU transfer
 *                   GPU→CPU 데이터 전송용 리드백 객체
 */
void DispatchFleshRingTightnessCS_WithReadback(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRHIGPUBufferReadback* Readback);
