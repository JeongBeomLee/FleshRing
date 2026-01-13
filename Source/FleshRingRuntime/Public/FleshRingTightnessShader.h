// ============================================================================
// FleshRing Tightness Shader
// FleshRing 조이기(Tightness) 셰이더
// ============================================================================
// Purpose: Creating tight flesh appearance with optional GPU skinning
// 목적: 살이 조여지는 효과 생성 (GPU 스키닝 옵션 포함)

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "FleshRingAffectedVertices.h"
#include "FleshRingDebugTypes.h"

// ============================================================================
// FFleshRingTightnessCS - Tightness Compute Shader
// 조이기 효과 컴퓨트 셰이더
// ============================================================================
// Processes only AffectedVertices (not all mesh vertices) for performance
// Pulls vertices inward toward Ring center axis
// Supports optional GPU skinning for animated meshes
// 성능 최적화를 위해 영향받는 버텍스만 처리 (전체 메시 버텍스 X)
// 버텍스를 링 중심축 방향으로 안쪽으로 당김
// 애니메이션된 메시를 위한 GPU 스키닝 옵션 지원
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

        // Input: Original vertex positions (bind pose component space)
        // 입력: 원본 버텍스 위치 (바인드 포즈 컴포넌트 스페이스)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, SourcePositions)

        // Input: Indices of affected vertices to process
        // 입력: 처리할 영향받는 버텍스 인덱스
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, AffectedIndices)

        // Input: Per-vertex influence weights (0-1)
        // 입력: 버텍스별 영향도 (0~1)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, Influences)

        // Input: Representative vertex indices for UV seam welding
        // 입력: UV seam 용접을 위한 대표 버텍스 인덱스
        // RepresentativeIndices[ThreadIndex] = 해당 위치 그룹의 대표 버텍스 인덱스
        // 셰이더에서: 대표 위치 읽기 → 변형 계산 → 자기 인덱스에 기록
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RepresentativeIndices)

        // ===== Output Buffer (UAV - Read/Write) =====
        // ===== 출력 버퍼 (UAV - 읽기/쓰기) =====

        // Output: Deformed vertex positions
        // 출력: 변형된 버텍스 위치
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, OutputPositions)

        // Output: Volume accumulation buffer for Bulge pass (Atomic operation)
        // 출력: Bulge 패스용 부피 누적 버퍼 (Atomic 연산)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, VolumeAccumBuffer)

        // Output: Debug Influence values for visualization (ThreadIndex당 1 float)
        // 출력: 시각화용 디버그 Influence 값
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, DebugInfluences)

        // Flag to enable debug influence output (0 = disabled, 1 = enabled)
        // 디버그 Influence 출력 활성화 플래그
        SHADER_PARAMETER(uint32, bOutputDebugInfluences)

        // Output: Debug point buffer for GPU rendering
        // 출력: GPU 렌더링용 디버그 포인트 버퍼
        // WorldPosition + Influence per point (16 bytes each)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FFleshRingDebugPoint>, DebugPointBuffer)

        // Flag to enable debug point output (0 = disabled, 1 = enabled)
        // 디버그 포인트 출력 활성화 플래그
        SHADER_PARAMETER(uint32, bOutputDebugPoints)

        // Base offset for debug point buffer (multi-ring support)
        // 디버그 포인트 버퍼 기본 오프셋 (다중 링 지원)
        SHADER_PARAMETER(uint32, DebugPointBaseOffset)

        // LocalToWorld matrix for DebugPointBuffer world space conversion
        // DebugPointBuffer 월드 공간 변환용 LocalToWorld 행렬
        SHADER_PARAMETER(FMatrix44f, LocalToWorld)

        // ===== Skinning Buffers (SRV - Read Only) =====
        // ===== 스키닝 버퍼 (SRV - 읽기 전용) =====

        // Bone matrices (3 float4 per bone = 3x4 matrix)
        // RefToLocal 행렬: [Bind Pose Component Space] → [Animated Component Space]
        // 본 행렬 (본당 3개의 float4 = 3x4 행렬)
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, BoneMatrices)

        // Packed bone indices + weights
        // 패킹된 본 인덱스 + 웨이트
        // 버텍스마다 [본인덱스0, 본인덱스1, ...] [웨이트0, 웨이트1, ...]
        // 4본 스키닝이라면 100번째 버텍스에서
        // [5,6,7,0] [0.5,0.3,0.2,0.0]
        SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InputWeightStream)

        // ===== Skinning Parameters =====
        // ===== 스키닝 파라미터 =====

        SHADER_PARAMETER(uint32, InputWeightStride)    // 웨이트 스트림 스트라이드 (바이트)
        SHADER_PARAMETER(uint32, InputWeightIndexSize) // 인덱스/웨이트 바이트 크기 (패킹됨)
        SHADER_PARAMETER(uint32, NumBoneInfluences)    // 버텍스당 본 영향 수 (4 or 8)
        SHADER_PARAMETER(uint32, bEnableSkinning)      // 스키닝 활성화 플래그 (0 or 1)

        // ===== Ring Parameters (Constant Buffer) =====
        // ===== 링 파라미터 (상수 버퍼) =====

        SHADER_PARAMETER(FVector3f, RingCenter)       // 링 중심 위치 (컴포넌트 스페이스)
        SHADER_PARAMETER(FVector3f, RingAxis)         // 링 축 방향 (정규화됨)
        SHADER_PARAMETER(float, TightnessStrength)    // 조이기 강도
        SHADER_PARAMETER(float, RingRadius)           // 링 내부 반지름
        SHADER_PARAMETER(float, RingHeight)            // 링 높이 (축 방향)
        SHADER_PARAMETER(float, RingThickness)        // 링 두께 (Radial falloff 범위) - Manual 모드 GPU Influence 계산용
        SHADER_PARAMETER(uint32, FalloffType)         // Falloff 타입 (0=Linear, 1=Quadratic, 2=Hermite) - Manual 모드 GPU Influence 계산용

        // ===== Counts =====
        // ===== 버텍스 수 =====

        SHADER_PARAMETER(uint32, NumAffectedVertices) // 영향받는 버텍스 수
        SHADER_PARAMETER(uint32, NumTotalVertices)    // 전체 버텍스 수 (범위 체크용)

        // ===== Volume Accumulation Parameters (for Bulge pass) =====
        // ===== 부피 누적 파라미터 (Bulge 패스용) =====

        SHADER_PARAMETER(uint32, bAccumulateVolume)   // 부피 누적 활성화 (0 = 비활성, 1 = 활성)
        SHADER_PARAMETER(float, FixedPointScale)      // Fixed-point 스케일 (예: 1000.0)
        SHADER_PARAMETER(uint32, RingIndex)           // Ring 인덱스 (VolumeAccumBuffer 슬롯 지정)

        // ===== SDF Parameters (OBB Design) =====
        // ===== SDF 파라미터 (OBB 설계) =====
        //
        // OBB 방식: SDF는 Ring 로컬 스페이스에서 생성
        // 셰이더에서 버텍스(컴포넌트 스페이스)를 로컬로 역변환 후 SDF 샘플링
        // ComponentToSDFLocal = LocalToComponent.Inverse()

        // SDF 3D 텍스처 (Ring 로컬 스페이스)
        // SDF 값: negative = 내부, positive = 외부
        SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, SDFTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SDFSampler)

        // SDF 볼륨 바운드 (Ring 로컬 스페이스)
        // UV 변환: (LocalPos - BoundsMin) / (BoundsMax - BoundsMin)
        SHADER_PARAMETER(FVector3f, SDFBoundsMin)
        SHADER_PARAMETER(FVector3f, SDFBoundsMax)

        // SDF 영향 모드 (0 = Manual, 1 = Auto/SDF-based)
        SHADER_PARAMETER(uint32, bUseSDFInfluence)

        // 컴포넌트 스페이스 → SDF 로컬 스페이스 변환 행렬 (OBB 지원)
        // BindPos를 로컬로 역변환 후 SDF 샘플링에 사용
        SHADER_PARAMETER(FMatrix44f, ComponentToSDFLocal)

        // SDF 로컬 스페이스 → 컴포넌트 스페이스 변환 행렬
        // Local 변위를 Component로 변환할 때 사용 (스케일 포함 정확한 역변환)
        SHADER_PARAMETER(FMatrix44f, SDFLocalToComponent)

        // SDF 모드 falloff 거리 (이 거리에서 Influence가 0이 됨)
        // SDF mode falloff distance (Influence becomes 0 at this distance)
        SHADER_PARAMETER(float, SDFInfluenceFalloffDistance)

        // Ring Center/Axis (SDF Local Space)
        // 원본 Ring 바운드 기준으로 계산 (확장 전)
        // SDF 바운드가 확장되어도 Ring의 실제 위치/축을 정확히 전달
        SHADER_PARAMETER(FVector3f, SDFLocalRingCenter)
        SHADER_PARAMETER(FVector3f, SDFLocalRingAxis)

        // Z축 상단 확장 거리 (절대값, cm 단위)
        // 링 위쪽으로 얼마나 확장할지 설정 (0 = 확장 없음)
        SHADER_PARAMETER(float, BoundsZTop)

        // Z축 하단 확장 거리 (절대값, cm 단위)
        // 링 아래쪽으로 얼마나 확장할지 설정 (0 = 확장 없음)
        SHADER_PARAMETER(float, BoundsZBottom)
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

// ============================================================================
// FTightnessDispatchParams - Dispatch Parameters
// 디스패치 파라미터 구조체
// ============================================================================

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
     * - bEnableSkinning=false: bind pose component space
     * - bEnableSkinning=true: animated component space (current frame)
     * 링 중심 위치 (컴포넌트 스페이스)
     * - 스키닝 비활성화: 바인드 포즈 컴포넌트 스페이스
     * - 스키닝 활성화: 애니메이션된 컴포넌트 스페이스 (현재 프레임)
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
     */
    float RingHeight;

    /**
     * Ring thickness (radial falloff range) - Manual mode GPU Influence calculation
     * 링 두께 (Radial falloff 범위) - Manual 모드 GPU Influence 계산용
     */
    float RingThickness;

    /**
     * Falloff type (0=Linear, 1=Quadratic, 2=Hermite) - Manual mode GPU Influence calculation
     * Falloff 타입 - Manual 모드 GPU Influence 계산용
     */
    uint32 FalloffType;

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

    // =========== Skinning Parameters ===========

    /**
     * Enable GPU skinning (0=bind pose, 1=skinned)
     * GPU 스키닝 활성화 (0=바인드 포즈, 1=스키닝)
     */
    uint32 bEnableSkinning;

    /**
     * Weight stream stride in bytes
     * 웨이트 스트림 스트라이드 (바이트)
     * 다음 버텍스 데이터로 가려면 몇 바이트 건너뛰어야 하는지
     * 4본 스키닝: 12, 8본 스키닝: 24
     */
    uint32 InputWeightStride;

    /**
     * Packed: BoneIndexByteSize | (BoneWeightByteSize << 8)
     * 패킹: 본인덱스바이트크기 | (본웨이트바이트크기 << 8)
     * 하위 8비트: 본 인덱스 크기(바이트), 상위 8비트: 본 웨이트 크기(바이트)
     * 일반적인 값:
     * 본 인덱스: 1바이트 (0~255 범위, 본 256개까지)
     * 본 웨이트: 2바이트 (0~65535 범위, 정밀도 ↑)
     */
    uint32 InputWeightIndexSize;

    /**
     * Number of bone influences per vertex (4 or 8)
     * 버텍스당 본 영향 수 (4 또는 8)
     */
    uint32 NumBoneInfluences;

    // =========== SDF Parameters (OBB Design) ===========
    // =========== SDF 파라미터 (OBB 설계) ===========

    /**
     * SDF volume minimum bounds (Ring 로컬 스페이스)
     * SDF 볼륨 최소 바운드
     */
    FVector3f SDFBoundsMin;

    /**
     * SDF volume maximum bounds (Ring 로컬 스페이스)
     * SDF 볼륨 최대 바운드
     */
    FVector3f SDFBoundsMax;

    /**
     * Use SDF-based influence calculation (0=Manual, 1=SDF Auto)
     * SDF 기반 영향도 계산 사용 (0=수동, 1=SDF 자동)
     */
    uint32 bUseSDFInfluence;

    /**
     * Component space → SDF local space transform matrix (OBB support)
     * 컴포넌트 스페이스 → SDF 로컬 스페이스 변환 행렬 (OBB 지원)
     * = LocalToComponent.Inverse()
     * BindPos를 로컬로 역변환 후 SDF 샘플링에 사용
     */
    FMatrix44f ComponentToSDFLocal;

    /**
     * SDF local space → Component space transform matrix
     * SDF 로컬 스페이스 → 컴포넌트 스페이스 변환 행렬
     * Local 변위를 Component로 변환할 때 사용 (스케일 포함 정확한 역변환)
     */
    FMatrix44f SDFLocalToComponent;

    /**
     * SDF falloff distance - Influence goes from 1.0 to 0.0 over this distance
     * SDF falloff 거리 - 이 거리에 걸쳐 Influence가 1.0에서 0.0으로 감소
     * Default: 5.0 (Ring 근처에서 부드러운 전환)
     */
    float SDFInfluenceFalloffDistance;

    /**
     * Z축 상단 확장 거리 (절대값, cm 단위)
     * 링 위쪽으로 얼마나 확장할지 (0 = 확장 없음)
     */
    float BoundsZTop;

    /**
     * Z축 하단 확장 거리 (절대값, cm 단위)
     * 링 아래쪽으로 얼마나 확장할지 (0 = 확장 없음)
     */
    float BoundsZBottom;

    // =========== SDF Local Ring Geometry ===========
    // =========== SDF 로컬 링 지오메트리 ===========

    /**
     * Ring Center (SDF Local Space)
     * 원본 Ring 바운드 기준으로 계산 (확장 전)
     * SDF 바운드가 확장되어도 Ring의 실제 위치를 정확히 전달
     */
    FVector3f SDFLocalRingCenter;

    /**
     * Ring Axis (SDF Local Space)
     * 원본 Ring 바운드 기준으로 계산 (확장 전)
     * SDF 바운드가 확장되어도 Ring의 실제 축을 정확히 전달
     */
    FVector3f SDFLocalRingAxis;

    // =========== Volume Accumulation Parameters (for Bulge pass) ===========
    // =========== 부피 누적 파라미터 (Bulge 패스용) ===========

    /**
     * Enable volume accumulation for Bulge pass
     * Bulge 패스를 위한 부피 누적 활성화 (0 = 비활성, 1 = 활성)
     */
    uint32 bAccumulateVolume;

    /**
     * Fixed-point scale for Atomic operations
     * Atomic 연산을 위한 Fixed-point 스케일 (예: 1000.0)
     * float × Scale → uint로 변환하여 Atomic 연산
     */
    float FixedPointScale;

    /**
     * Ring index for per-ring VolumeAccumBuffer slot
     * Ring별 VolumeAccumBuffer 슬롯 지정용 인덱스
     */
    uint32 RingIndex;

    // =========== Debug Parameters ===========
    // =========== 디버그 파라미터 ===========

    /**
     * Enable debug influence output for visualization
     * 시각화를 위한 디버그 Influence 출력 활성화 (0 = 비활성, 1 = 활성)
     */
    uint32 bOutputDebugInfluences;

    /**
     * Enable debug point output for GPU rendering
     * GPU 렌더링을 위한 디버그 포인트 출력 활성화 (0 = 비활성, 1 = 활성)
     */
    uint32 bOutputDebugPoints;

    /**
     * Base offset for debug point buffer (multi-ring support)
     * 디버그 포인트 버퍼 기본 오프셋 (다중 링 지원)
     * Ring 0: offset 0, Ring 1: offset = Ring0.NumAffectedVertices, etc.
     */
    uint32 DebugPointBaseOffset;

    /**
     * LocalToWorld matrix for converting deformed positions to world space
     * 변형된 위치를 월드 스페이스로 변환하기 위한 LocalToWorld 행렬
     * Used by DebugPointBuffer output
     */
    FMatrix44f LocalToWorld;

    FTightnessDispatchParams()
        : RingCenter(FVector3f::ZeroVector)
        , RingAxis(FVector3f::UpVector)
        , RingRadius(5.0f)
        , RingHeight(2.0f)
        , RingThickness(2.0f)
        , FalloffType(0)
        , TightnessStrength(1.0f)
        , NumAffectedVertices(0)
        , NumTotalVertices(0)
        , bEnableSkinning(0)
        , InputWeightStride(0)
        , InputWeightIndexSize(0)
        , NumBoneInfluences(0)
        , SDFBoundsMin(FVector3f::ZeroVector)
        , SDFBoundsMax(FVector3f::ZeroVector)
        , bUseSDFInfluence(0)
        , ComponentToSDFLocal(FMatrix44f::Identity)
        , SDFLocalToComponent(FMatrix44f::Identity)
        , SDFInfluenceFalloffDistance(5.0f)
        , BoundsZTop(5.0f)
        , BoundsZBottom(0.0f)
        , SDFLocalRingCenter(FVector3f::ZeroVector)
        , SDFLocalRingAxis(FVector3f(0.0f, 0.0f, 1.0f))
        , bAccumulateVolume(0)
        , FixedPointScale(1000.0f)
        , RingIndex(0)
        , bOutputDebugInfluences(0)
        , bOutputDebugPoints(0)
        , DebugPointBaseOffset(0)
        , LocalToWorld(FMatrix44f::Identity)
    {
    }
};

// ============================================================================
// Helper Functions - Parameter Creation
// 헬퍼 함수 - 파라미터 생성
// ============================================================================

/**
 * Create FTightnessDispatchParams from FRingAffectedData (bind pose mode)
 * FRingAffectedData에서 FTightnessDispatchParams 생성 (바인드 포즈 모드)
 *
 * @param AffectedData - 영향받는 버텍스 데이터 (Ring 파라미터 포함)
 * @param TotalVertexCount - 메시 전체 버텍스 수
 * @return GPU Dispatch용 파라미터 구조체 (스키닝 비활성화)
 */
inline FTightnessDispatchParams CreateTightnessParams(
    const FRingAffectedData& AffectedData,
    uint32 TotalVertexCount)
{
    FTightnessDispatchParams Params;

    // Ring 트랜스폼 정보 (바인드 포즈)
    Params.RingCenter = FVector3f(AffectedData.RingCenter);
    Params.RingAxis = FVector3f(AffectedData.RingAxis);

    // Ring 지오메트리 정보
    Params.RingRadius = AffectedData.RingRadius;
    Params.RingHeight = AffectedData.RingHeight;
    Params.RingThickness = AffectedData.RingThickness;
    Params.FalloffType = static_cast<uint32>(AffectedData.FalloffType);

    // 변형 강도 (FFleshRingSettings에서 복사된 값)
    Params.TightnessStrength = AffectedData.TightnessStrength;

    // 버텍스 카운트
    Params.NumAffectedVertices = static_cast<uint32>(AffectedData.Vertices.Num());
    Params.NumTotalVertices = TotalVertexCount;

    // 스키닝 비활성화
    Params.bEnableSkinning = 0;
    Params.InputWeightStride = 0;
    Params.InputWeightIndexSize = 0;
    Params.NumBoneInfluences = 0;

    return Params;
}

/**
 * [DEPRECATED] Create FTightnessDispatchParams with skinning enabled (animated mode)
 * [DEPRECATED] 스키닝 활성화된 FTightnessDispatchParams 생성 (애니메이션 모드)
 *
 * NOTE: 스키닝이 FleshRingSkinningCS로 분리되어 더 이상 사용되지 않음
 *       TightnessCS는 바인드 포즈에서만 동작하고, 스키닝은 별도 패스로 처리
 *
 * @param AffectedData - 영향받는 버텍스 데이터 (Ring 파라미터 포함, 바인드 포즈 기준)
 * @param TotalVertexCount - 메시 전체 버텍스 수
 * @param AnimatedRingCenter - 현재 프레임 본 트랜스폼에서 가져온 Ring 중심
 * @param AnimatedRingAxis - 현재 프레임 본 트랜스폼에서 가져온 Ring 축
 * @param InInputWeightStride - 웨이트 스트림 스트라이드
 * @param InInputWeightIndexSize - 인덱스/웨이트 바이트 크기
 * @param InNumBoneInfluences - 버텍스당 본 영향 수
 * @return GPU Dispatch용 파라미터 구조체 (스키닝 활성화)
 */
UE_DEPRECATED(5.7, "Use CreateTightnessParams + FleshRingSkinningCS instead")
inline FTightnessDispatchParams CreateTightnessParamsWithSkinning_Deprecated(
    const FRingAffectedData& AffectedData,
    uint32 TotalVertexCount,
    const FVector3f& AnimatedRingCenter,
    const FVector3f& AnimatedRingAxis,
    uint32 InInputWeightStride,
    uint32 InInputWeightIndexSize,
    uint32 InNumBoneInfluences)
{
    FTightnessDispatchParams Params;

    // Ring 트랜스폼 정보 (애니메이션된 현재 프레임)
    Params.RingCenter = AnimatedRingCenter;
    Params.RingAxis = AnimatedRingAxis;

    // Ring 지오메트리 정보
    Params.RingRadius = AffectedData.RingRadius;
    Params.RingHeight = AffectedData.RingHeight;

    // 변형 강도
    Params.TightnessStrength = AffectedData.TightnessStrength;

    // 버텍스 카운트
    Params.NumAffectedVertices = static_cast<uint32>(AffectedData.Vertices.Num());
    Params.NumTotalVertices = TotalVertexCount;

    // 스키닝 활성화
    Params.bEnableSkinning = 1;
    Params.InputWeightStride = InInputWeightStride;
    Params.InputWeightIndexSize = InInputWeightIndexSize;
    Params.NumBoneInfluences = InNumBoneInfluences;

    return Params;
}

// ============================================================================
// Dispatch Function Declarations
// Dispatch 함수 선언
// ============================================================================

/**
 * Dispatch TightnessCS to process affected vertices (bind pose mode)
 * TightnessCS를 디스패치하여 영향받는 버텍스 처리 (바인드 포즈 모드)
 *
 * @param GraphBuilder - RDG builder for resource management
 *                       RDG 빌더 (리소스 관리용)
 * @param Params - Dispatch parameters (Ring settings, counts, volume accumulation ...)
 *                 디스패치 파라미터 (링 설정, 버텍스 수, 부피 누적 등)
 * @param SourcePositionsBuffer - RDG buffer containing source vertex positions
 *                                원본 버텍스 위치 버퍼
 * @param AffectedIndicesBuffer - Buffer containing vertex indices to process
 *                                처리할 버텍스 인덱스 버퍼
 * @param InfluencesBuffer - Buffer containing per-vertex influence weights
 *                           버텍스별 영향도 버퍼
 * @param RepresentativeIndicesBuffer - Buffer containing representative vertex indices for UV seam welding
 *                                      UV seam 용접용 대표 버텍스 인덱스 버퍼 (nullptr이면 AffectedIndices 사용)
 * @param OutputPositionsBuffer - UAV buffer for deformed positions
 *                                변형된 위치 출력 버퍼 (UAV)
 * @param SDFTexture - (Optional) SDF 3D texture for Auto influence mode
 *                     (옵션) SDF 자동 영향 모드용 3D 텍스처
 *                     nullptr이면 Manual 모드 (Influences 버퍼 사용)
 * @param VolumeAccumBuffer - (Optional) Volume accumulation buffer for Bulge pass
 *                            (옵션) Bulge 패스용 부피 누적 버퍼
 *                            nullptr이면 부피 누적 비활성화
 * @param DebugInfluencesBuffer - (Optional) Debug influence output buffer
 *                                (옵션) 디버그 Influence 출력 버퍼
 *                                Params.bOutputDebugInfluences=1일 때 사용
 * @param DebugPointBuffer - (Optional) Debug point buffer for GPU rendering
 *                           (옵션) GPU 렌더링용 디버그 포인트 버퍼
 *                           Params.bOutputDebugPoints=1일 때 사용
 */
void DispatchFleshRingTightnessCS(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef RepresentativeIndicesBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRDGTextureRef SDFTexture = nullptr,
    FRDGBufferRef VolumeAccumBuffer = nullptr,
    FRDGBufferRef DebugInfluencesBuffer = nullptr,
    FRDGBufferRef DebugPointBuffer = nullptr);

/**
 * [DEPRECATED] Dispatch TightnessCS with GPU skinning (animated mode)
 * [DEPRECATED] GPU 스키닝이 포함된 TightnessCS 디스패치 (애니메이션 모드)
 *
 * NOTE: 스키닝이 FleshRingSkinningCS로 분리되어 더 이상 사용되지 않음
 *       DispatchFleshRingTightnessCS + DispatchFleshRingSkinningCS로 대체
 *
 * @param GraphBuilder - RDG builder for resource management
 *                       RDG 빌더 (리소스 관리용)
 * @param Params - Dispatch parameters (must have bEnableSkinning=1)
 *                 디스패치 파라미터 (bEnableSkinning=1 필수)
 * @param SourcePositionsBuffer - Bind pose vertex positions
 *                                바인드 포즈 버텍스 위치 버퍼
 * @param AffectedIndicesBuffer - Vertex indices to process
 *                                처리할 버텍스 인덱스 버퍼
 * @param InfluencesBuffer - Per-vertex influence weights
 *                           버텍스별 영향도 버퍼
 * @param OutputPositionsBuffer - Output deformed positions (UAV)
 *                                변형된 위치 출력 버퍼 (UAV)
 * @param BoneMatricesBuffer - Bone matrices (3 float4 per bone)
 *                             RefToLocal 행렬 버퍼 (본당 3개 float4)
 * @param InputWeightStreamBuffer - Packed bone indices + weights
 *                                  패킹된 본 인덱스 + 웨이트 버퍼
 * @param SDFTexture - (Optional) SDF 3D texture for Auto influence mode
 *                     (옵션) SDF 자동 영향 모드용 3D 텍스처
 */
UE_DEPRECATED(5.7, "Use DispatchFleshRingTightnessCS + DispatchFleshRingSkinningCS instead")
void DispatchFleshRingTightnessCS_WithSkinning_Deprecated(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRDGBufferRef BoneMatricesBuffer,
    FRDGBufferRef InputWeightStreamBuffer,
    FRDGTextureRef SDFTexture = nullptr);

/**
 * Dispatch TightnessCS with readback for validation/testing (bind pose mode)
 * TightnessCS 디스패치 + GPU→CPU 리드백 (검증/테스트용, 바인드 포즈 모드)
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
 * @param RepresentativeIndicesBuffer - Buffer containing representative vertex indices for UV seam welding
 *                                      UV seam 용접용 대표 버텍스 인덱스 버퍼 (nullptr이면 AffectedIndices 사용)
 * @param OutputPositionsBuffer - UAV buffer for deformed positions
 *                                변형된 위치 출력 버퍼 (UAV)
 * @param Readback - Readback object for GPU->CPU transfer
 *                   GPU→CPU 데이터 전송용 리드백 객체
 * @param SDFTexture - (Optional) SDF 3D texture for Auto influence mode
 *                     (옵션) SDF 자동 영향 모드용 3D 텍스처
 * @param VolumeAccumBuffer - (Optional) Volume accumulation buffer for Bulge pass
 *                            (옵션) Bulge 패스용 부피 누적 버퍼
 * @param DebugInfluencesBuffer - (Optional) Debug influence output buffer
 *                                (옵션) 디버그 Influence 출력 버퍼
 * @param DebugPointBuffer - (Optional) Debug point buffer for GPU rendering
 *                           (옵션) GPU 렌더링용 디버그 포인트 버퍼
 */
void DispatchFleshRingTightnessCS_WithReadback(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef RepresentativeIndicesBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRHIGPUBufferReadback* Readback,
    FRDGTextureRef SDFTexture = nullptr,
    FRDGBufferRef VolumeAccumBuffer = nullptr,
    FRDGBufferRef DebugInfluencesBuffer = nullptr,
    FRDGBufferRef DebugPointBuffer = nullptr);
