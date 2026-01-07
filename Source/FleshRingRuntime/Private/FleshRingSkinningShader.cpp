// ============================================================================
// FleshRing Skinning Shader - Implementation
// FleshRing 스키닝 셰이더 - 구현부
// ============================================================================
// Purpose: Apply GPU skinning to cached TightenedBindPose
// 목적: 캐싱된 TightenedBindPose에 GPU 스키닝 적용

#include "FleshRingSkinningShader.h"

#include "CoreMinimal.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"
#include "DataDrivenShaderPlatformInfo.h"  // For IsOpenGLPlatform

// ============================================================================
// Shader Implementation Registration
// 셰이더 구현 등록
// ============================================================================
IMPLEMENT_GLOBAL_SHADER(
    FFleshRingSkinningCS,
    "/Plugin/FleshRingPlugin/FleshRingSkinningCS.usf",
    "MainCS",
    SF_Compute
);

// ============================================================================
// Dispatch Function Implementation
// Dispatch 함수 구현
// ============================================================================

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
    FRDGBufferRef RecomputedNormalsBuffer)
{
    // Early out if no vertices to process
    // 처리할 버텍스가 없으면 조기 반환
    if (Params.NumVertices == 0)
    {
        return;
    }

    // Early out if skinning buffers are not available
    // 스키닝 버퍼가 없으면 조기 반환
    if (!BoneMatricesSRV || !InputWeightStreamSRV)
    {
        UE_LOG(LogTemp, Warning, TEXT("FleshRingSkinningCS: Missing skinning buffers"));
        return;
    }

    // Tangent processing is optional - allow position-only skinning
    // 탄젠트 처리는 선택적 - 포지션 전용 스키닝 허용
    const bool bProcessTangents = (OutputTangentsBuffer != nullptr);

    // Previous Position processing for TAA/TSR velocity
    // TAA/TSR velocity용 Previous Position 처리
    const bool bProcessPreviousPosition = (OutputPreviousPositionsBuffer != nullptr) && (PreviousBoneMatricesSRV != nullptr);

    // Recomputed normals processing - use NormalRecomputeCS output for deformed vertices
    // 재계산된 노멀 처리 - 변형된 버텍스에 NormalRecomputeCS 출력 사용
    const bool bUseRecomputedNormals = (RecomputedNormalsBuffer != nullptr);

    // Allocate shader parameters
    // 셰이더 파라미터 할당
    FFleshRingSkinningCS::FParameters* PassParameters =
        GraphBuilder.AllocParameters<FFleshRingSkinningCS::FParameters>();

    // ===== Bind input buffers (SRV) =====
    // ===== 입력 버퍼 바인딩 (SRV) =====
    // TightenedBindPose (cached positions)
    PassParameters->SourcePositions = GraphBuilder.CreateSRV(SourcePositionsBuffer, PF_R32_FLOAT);

    // ===== Bind output buffers (UAV) =====
    // ===== 출력 버퍼 바인딩 (UAV) =====
    PassParameters->OutputPositions = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);

    // Previous Position output buffer for TAA/TSR velocity
    // TAA/TSR velocity용 Previous Position 출력 버퍼
    if (bProcessPreviousPosition)
    {
        PassParameters->OutputPreviousPositions = GraphBuilder.CreateUAV(OutputPreviousPositionsBuffer, PF_R32_FLOAT);
    }
    else
    {
        // Dummy buffer - use Position buffer as placeholder (won't be written)
        // 더미 버퍼 - Position 버퍼를 플레이스홀더로 사용 (실제로 쓰지 않음)
        PassParameters->OutputPreviousPositions = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);
    }

    // Tangent buffers - RDG requires all declared parameters to be bound
    // 탄젠트 버퍼 - RDG는 선언된 모든 파라미터가 바인딩되어야 함
    PassParameters->SourceTangents = SourceTangentsSRV;

    // Recomputed normals buffer (optional, from NormalRecomputeCS)
    // 재계산된 노멀 버퍼 (선택적, NormalRecomputeCS에서)
    if (bUseRecomputedNormals)
    {
        PassParameters->RecomputedNormals = GraphBuilder.CreateSRV(RecomputedNormalsBuffer, PF_R32_FLOAT);
    }
    else
    {
        // Dummy buffer - use Position buffer as placeholder (won't be read if bUseRecomputedNormals=0)
        // 더미 버퍼 - Position 버퍼를 플레이스홀더로 사용 (bUseRecomputedNormals=0이면 읽지 않음)
        PassParameters->RecomputedNormals = GraphBuilder.CreateSRV(SourcePositionsBuffer, PF_R32_FLOAT);
    }

    if (bProcessTangents)
    {
        // Real tangent output buffer - Optimus approach
        // Format: PF_R16G16B16A16_SNORM (non-OpenGL) or PF_R16G16B16A16_SINT (OpenGL)
        // Matches GpuSkinCommon.ush TANGENT_RWBUFFER_FORMAT
        const EPixelFormat TangentsFormat = IsOpenGLPlatform(GMaxRHIShaderPlatform)
            ? PF_R16G16B16A16_SINT
            : PF_R16G16B16A16_SNORM;
        PassParameters->OutputTangents = GraphBuilder.CreateUAV(OutputTangentsBuffer, TangentsFormat);
    }
    else
    {
        // Dummy output tangent buffer - use Position buffer as dummy (won't be written)
        // 더미 출력 탄젠트 버퍼 - Position 버퍼를 더미로 사용 (실제로 쓰지 않음)
        PassParameters->OutputTangents = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);
    }

    // ===== Bind skinning buffers (RHI SRV directly) =====
    // ===== 스키닝 버퍼 바인딩 (RHI SRV 직접) =====
    // BoneMatrices: RefToLocal matrix (3 float4 per bone)
    // [Bind Pose Component Space] -> [Animated Component Space]
    PassParameters->BoneMatrices = BoneMatricesSRV;

    // Previous frame bone matrices for velocity calculation
    // velocity 계산용 이전 프레임 본 행렬
    if (bProcessPreviousPosition)
    {
        PassParameters->PreviousBoneMatrices = PreviousBoneMatricesSRV;
    }
    else
    {
        // Use current frame as fallback (no velocity)
        // 폴백으로 현재 프레임 사용 (velocity 없음)
        PassParameters->PreviousBoneMatrices = BoneMatricesSRV;
    }

    PassParameters->InputWeightStream = InputWeightStreamSRV;

    // ===== Skinning parameters =====
    // ===== 스키닝 파라미터 =====
    PassParameters->InputWeightStride = Params.InputWeightStride;
    PassParameters->InputWeightIndexSize = Params.InputWeightIndexSize;
    PassParameters->NumBoneInfluences = Params.NumBoneInfluences;

    // ===== Section parameters (like WaveCS) =====
    // ===== Section 파라미터 (WaveCS와 동일) =====
    PassParameters->BaseVertexIndex = Params.BaseVertexIndex;
    PassParameters->NumVertices = Params.NumVertices;

    // ===== Debug/Feature flags =====
    // ===== 디버그/기능 플래그 =====
    PassParameters->bProcessTangents = bProcessTangents ? 1 : 0;
    PassParameters->bProcessPreviousPosition = bProcessPreviousPosition ? 1 : 0;
    PassParameters->bUseRecomputedNormals = bUseRecomputedNormals ? 1 : 0;

    // Get shader reference
    // 셰이더 참조 가져오기
    TShaderMapRef<FFleshRingSkinningCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // Calculate dispatch groups based on Section's vertex count
    // Section의 버텍스 수 기준으로 디스패치 그룹 수 계산
    const uint32 ThreadGroupSize = 64; // .usf의 [numthreads(64,1,1)]와 일치
    const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumVertices, ThreadGroupSize);

    // Add compute pass to RDG
    // RDG에 컴퓨트 패스 추가
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("FleshRingSkinningCS (Section base=%d, %d verts, PrevPos=%d)",
            Params.BaseVertexIndex, Params.NumVertices, bProcessPreviousPosition ? 1 : 0),
        ComputeShader,
        PassParameters,
        FIntVector(static_cast<int32>(NumGroups), 1, 1)
    );
}
