// ============================================================================
// FleshRing Tightness Shader - Implementation
// FleshRing 조이기(Tightness) 셰이더 - 구현부
// ============================================================================
// Purpose: Pull vertices toward Ring center axis (Tightness effect)
// 목적: 버텍스를 링 중심축 방향으로 안쪽으로 당김 (조이기 효과)

#include "FleshRingTightnessShader.h"

#include "CoreMinimal.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"

// Includes for asset-based testing
// 에셋 기반 테스트를 위한 include
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"

// ============================================================================
// Shader Implementation Registration
// 셰이더 구현 등록
// ============================================================================
IMPLEMENT_GLOBAL_SHADER(
    FFleshRingTightnessCS,
    "/Plugin/FleshRingPlugin/FleshRingTightnessCS.usf",
    "MainCS",
    SF_Compute
);

// ============================================================================
// Dispatch Function Implementation
// Dispatch 함수 구현
// ============================================================================

void DispatchFleshRingTightnessCS(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef RepresentativeIndicesBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRDGTextureRef SDFTexture,
    FRDGBufferRef VolumeAccumBuffer)
{
    // Early out if no vertices to process
    // 처리할 버텍스가 없으면 조기 반환
    if (Params.NumAffectedVertices == 0)
    {
        return;
    }

    // Allocate shader parameters
    // 셰이더 파라미터 할당
    FFleshRingTightnessCS::FParameters* PassParameters =
        GraphBuilder.AllocParameters<FFleshRingTightnessCS::FParameters>();

    // ===== Bind input buffers (SRV) =====
    // ===== 입력 버퍼 바인딩 (SRV) =====
    PassParameters->SourcePositions = GraphBuilder.CreateSRV(SourcePositionsBuffer, PF_R32_FLOAT);
    PassParameters->AffectedIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer);
    PassParameters->Influences = GraphBuilder.CreateSRV(InfluencesBuffer);

    // ===== UV Seam Welding: RepresentativeIndices 바인딩 =====
    // RepresentativeIndices가 nullptr이면 AffectedIndices를 대신 사용 (fallback)
    // 셰이더에서: 대표 위치 읽기 → 변형 계산 → 자기 인덱스에 기록
    if (RepresentativeIndicesBuffer)
    {
        PassParameters->RepresentativeIndices = GraphBuilder.CreateSRV(RepresentativeIndicesBuffer);
    }
    else
    {
        // Fallback: AffectedIndices 사용 (각 버텍스가 자기 자신이 대표)
        PassParameters->RepresentativeIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer);
    }

    // ===== Bind output buffer (UAV) =====
    // ===== 출력 버퍼 바인딩 (UAV) =====
    PassParameters->OutputPositions = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);

    // ===== Skinning disabled (bind pose mode) =====
    // ===== 스키닝 비활성화 (바인드 포즈 모드) =====
    // Note: RDG requires valid SRV bindings with uploaded data even for unused resources
    // RDG는 사용하지 않는 리소스도 데이터가 업로드된 유효한 SRV 바인딩이 필요함
    static const float DummyBoneMatrixData[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    static const uint32 DummyWeightData = 0;

    FRDGBufferRef DummyBoneMatricesBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateBufferDesc(sizeof(float) * 4, 1),
        TEXT("FleshRingTightness_DummyBoneMatrices")
    );
    GraphBuilder.QueueBufferUpload(DummyBoneMatricesBuffer, DummyBoneMatrixData, sizeof(DummyBoneMatrixData), ERDGInitialDataFlags::None);

    FRDGBufferRef DummyWeightStreamBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
        TEXT("FleshRingTightness_DummyWeightStream")
    );
    GraphBuilder.QueueBufferUpload(DummyWeightStreamBuffer, &DummyWeightData, sizeof(DummyWeightData), ERDGInitialDataFlags::None);

    PassParameters->BoneMatrices = GraphBuilder.CreateSRV(DummyBoneMatricesBuffer, PF_A32B32G32R32F);
    PassParameters->InputWeightStream = GraphBuilder.CreateSRV(DummyWeightStreamBuffer, PF_R32_UINT);
    PassParameters->InputWeightStride = 0;
    PassParameters->InputWeightIndexSize = 0;
    PassParameters->NumBoneInfluences = 0;
    PassParameters->bEnableSkinning = 0;

    // ===== Ring parameters =====
    // ===== 링 파라미터 =====
    PassParameters->RingCenter = Params.RingCenter;
    PassParameters->RingAxis = Params.RingAxis;
    PassParameters->TightnessStrength = Params.TightnessStrength;
    PassParameters->RingRadius = Params.RingRadius;
    PassParameters->RingWidth = Params.RingWidth;

    // ===== Counts =====
    // ===== 버텍스 수 =====
    PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
    PassParameters->NumTotalVertices = Params.NumTotalVertices;

    // ===== SDF Parameters (OBB Design) =====
    // ===== SDF 파라미터 (OBB 설계) =====
    // SDFTexture가 유효하면 SDF Auto 모드, nullptr이면 Manual 모드
    if (SDFTexture)
    {
        PassParameters->SDFTexture = GraphBuilder.CreateSRV(SDFTexture);
        PassParameters->SDFSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
        PassParameters->SDFBoundsMin = Params.SDFBoundsMin;
        PassParameters->SDFBoundsMax = Params.SDFBoundsMax;
        PassParameters->bUseSDFInfluence = 1;
        // OBB 지원: 컴포넌트 → 로컬 역변환 행렬
        PassParameters->ComponentToSDFLocal = Params.ComponentToSDFLocal;
        PassParameters->SDFLocalToComponent = Params.SDFLocalToComponent;
        // SDF falloff 거리
        PassParameters->SDFInfluenceFalloffDistance = Params.SDFInfluenceFalloffDistance;
        // Ring Center/Axis (SDF Local Space) - 바운드 확장 시에도 정확한 위치 전달
        PassParameters->SDFLocalRingCenter = Params.SDFLocalRingCenter;
        PassParameters->SDFLocalRingAxis = Params.SDFLocalRingAxis;
    }
    else
    {
        // Manual 모드: Dummy SDF 텍스처 바인딩 (RDG 요구사항 - 모든 파라미터 바인딩 필수)
        FRDGTextureDesc DummySDFDesc = FRDGTextureDesc::Create3D(
            FIntVector(1, 1, 1),
            PF_R32_FLOAT,
            FClearValueBinding::Black,
            TexCreate_ShaderResource | TexCreate_UAV);  // UAV 추가 (Clear용)
        FRDGTextureRef DummySDFTexture = GraphBuilder.CreateTexture(DummySDFDesc, TEXT("FleshRingTightness_DummySDF"));

        // RDG 검증 통과: 텍스처에 쓰기 패스 추가 (Producer 필요)
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummySDFTexture), 0.0f);

        PassParameters->SDFTexture = GraphBuilder.CreateSRV(DummySDFTexture);
        PassParameters->SDFSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
        PassParameters->SDFBoundsMin = FVector3f::ZeroVector;
        PassParameters->SDFBoundsMax = FVector3f::OneVector;
        PassParameters->bUseSDFInfluence = 0;
        // Manual 모드: Identity 행렬 (사용 안함)
        PassParameters->ComponentToSDFLocal = FMatrix44f::Identity;
        PassParameters->SDFLocalToComponent = FMatrix44f::Identity;
        // Manual 모드에서는 사용 안 하지만 바인딩 필요
        PassParameters->SDFInfluenceFalloffDistance = 5.0f;
        // Manual 모드: 기본값 바인딩 (사용 안함)
        PassParameters->SDFLocalRingCenter = FVector3f::ZeroVector;
        PassParameters->SDFLocalRingAxis = FVector3f(0.0f, 0.0f, 1.0f);
    }

    // ===== Smoothing Bounds Z Extension Parameters =====
    // ===== 스무딩 영역 Z 확장 파라미터 =====
    PassParameters->BoundsZTop = Params.BoundsZTop;
    PassParameters->BoundsZBottom = Params.BoundsZBottom;

    // ===== Volume Accumulation Parameters (for Bulge pass) =====
    // ===== 부피 누적 파라미터 (Bulge 패스용) =====
    PassParameters->bAccumulateVolume = Params.bAccumulateVolume;
    PassParameters->FixedPointScale = Params.FixedPointScale;
    PassParameters->RingIndex = Params.RingIndex;

    if (VolumeAccumBuffer)
    {
        // VolumeAccumBuffer가 제공되면 바인딩
        PassParameters->VolumeAccumBuffer = GraphBuilder.CreateUAV(VolumeAccumBuffer, PF_R32_UINT);
    }
    else
    {
        // VolumeAccumBuffer가 없으면 Dummy 생성 (RDG 요구사항 - 모든 파라미터 바인딩 필수)
        FRDGBufferRef DummyVolumeBuffer = GraphBuilder.CreateBuffer(
            FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
            TEXT("FleshRingTightness_DummyVolumeAccum")
        );
        // Dummy 버퍼 초기화 (RDG Producer 필요)
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyVolumeBuffer, PF_R32_UINT), 0u);
        PassParameters->VolumeAccumBuffer = GraphBuilder.CreateUAV(DummyVolumeBuffer, PF_R32_UINT);
        // Dummy일 때는 부피 누적 비활성화 강제
        PassParameters->bAccumulateVolume = 0;
    }

    // Get shader reference
    // 셰이더 참조 가져오기
    TShaderMapRef<FFleshRingTightnessCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // Calculate dispatch groups
    // 디스패치 그룹 수 계산
    const uint32 ThreadGroupSize = 64; // .usf의 [numthreads(64,1,1)]와 일치
    const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumAffectedVertices, ThreadGroupSize);

    // Add compute pass to RDG
    // RDG에 컴퓨트 패스 추가
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("FleshRingTightnessCS"),
        ComputeShader,
        PassParameters,
        FIntVector(static_cast<int32>(NumGroups), 1, 1)
    );
}

// ============================================================================
// [DEPRECATED] Dispatch with GPU Skinning (animated mode)
// [DEPRECATED] GPU 스키닝 포함 디스패치 (애니메이션 모드)
// NOTE: 스키닝이 FleshRingSkinningCS로 분리되어 더 이상 사용되지 않음
// ============================================================================

void DispatchFleshRingTightnessCS_WithSkinning_Deprecated(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRDGBufferRef BoneMatricesBuffer,
    FRDGBufferRef InputWeightStreamBuffer,
    FRDGTextureRef SDFTexture)
{
    // Early out if no vertices to process
    // 처리할 버텍스가 없으면 조기 반환
    if (Params.NumAffectedVertices == 0)
    {
        return;
    }

    // Allocate shader parameters
    // 셰이더 파라미터 할당
    FFleshRingTightnessCS::FParameters* PassParameters =
        GraphBuilder.AllocParameters<FFleshRingTightnessCS::FParameters>();

    // ===== Bind input buffers (SRV) =====
    // ===== 입력 버퍼 바인딩 (SRV) =====
    PassParameters->SourcePositions = GraphBuilder.CreateSRV(SourcePositionsBuffer, PF_R32_FLOAT);
    PassParameters->AffectedIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer);
    PassParameters->Influences = GraphBuilder.CreateSRV(InfluencesBuffer);

    // ===== Bind output buffer (UAV) =====
    // ===== 출력 버퍼 바인딩 (UAV) =====
    PassParameters->OutputPositions = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);

    // ===== Bind skinning buffers (SRV) =====
    // ===== 스키닝 버퍼 바인딩 (SRV) =====
    // BoneMatrices: RefToLocal 행렬 (본당 3개 float4)
    // [Bind Pose Component Space] → [Animated Component Space]
    PassParameters->BoneMatrices = GraphBuilder.CreateSRV(BoneMatricesBuffer, PF_A32B32G32R32F);
    PassParameters->InputWeightStream = GraphBuilder.CreateSRV(InputWeightStreamBuffer, PF_R32_UINT);

    // ===== Skinning parameters =====
    // ===== 스키닝 파라미터 =====
    PassParameters->InputWeightStride = Params.InputWeightStride;
    PassParameters->InputWeightIndexSize = Params.InputWeightIndexSize;
    PassParameters->NumBoneInfluences = Params.NumBoneInfluences;
    PassParameters->bEnableSkinning = Params.bEnableSkinning;

    // ===== Ring parameters (animated component space) =====
    // ===== 링 파라미터 (애니메이션된 컴포넌트 스페이스) =====
    PassParameters->RingCenter = Params.RingCenter;
    PassParameters->RingAxis = Params.RingAxis;
    PassParameters->TightnessStrength = Params.TightnessStrength;
    PassParameters->RingRadius = Params.RingRadius;
    PassParameters->RingWidth = Params.RingWidth;

    // ===== Counts =====
    // ===== 버텍스 수 =====
    PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
    PassParameters->NumTotalVertices = Params.NumTotalVertices;

    // ===== SDF Parameters =====
    // ===== SDF 파라미터 =====
    // SDFTexture가 유효하면 SDF Auto 모드, nullptr이면 Manual 모드
    if (SDFTexture)
    {
        PassParameters->SDFTexture = GraphBuilder.CreateSRV(SDFTexture);
        PassParameters->SDFSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
        PassParameters->SDFBoundsMin = Params.SDFBoundsMin;
        PassParameters->SDFBoundsMax = Params.SDFBoundsMax;
        PassParameters->bUseSDFInfluence = 1;
        PassParameters->ComponentToSDFLocal = Params.ComponentToSDFLocal;
        PassParameters->SDFInfluenceFalloffDistance = Params.SDFInfluenceFalloffDistance;
        // Ring Center/Axis (SDF Local Space) - 바운드 확장 시에도 정확한 위치 전달
        PassParameters->SDFLocalRingCenter = Params.SDFLocalRingCenter;
        PassParameters->SDFLocalRingAxis = Params.SDFLocalRingAxis;
    }
    else
    {
        // Manual 모드: Dummy SDF 텍스처 바인딩 (RDG 요구사항)
        FRDGTextureDesc DummySDFDesc = FRDGTextureDesc::Create3D(
            FIntVector(1, 1, 1),
            PF_R32_FLOAT,
            FClearValueBinding::Black,
            TexCreate_ShaderResource | TexCreate_UAV);  // UAV 추가 (Clear용)
        FRDGTextureRef DummySDFTexture = GraphBuilder.CreateTexture(DummySDFDesc, TEXT("FleshRingTightness_DummySDF_Skinned"));

        // RDG 검증 통과: 텍스처에 쓰기 패스 추가 (Producer 필요)
        AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummySDFTexture), 0.0f);

        PassParameters->SDFTexture = GraphBuilder.CreateSRV(DummySDFTexture);
        PassParameters->SDFSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
        PassParameters->SDFBoundsMin = FVector3f::ZeroVector;
        PassParameters->SDFBoundsMax = FVector3f::OneVector;
        PassParameters->bUseSDFInfluence = 0;
        PassParameters->ComponentToSDFLocal = FMatrix44f::Identity;
        PassParameters->SDFInfluenceFalloffDistance = 5.0f;
        // Manual 모드: 기본값 바인딩 (사용 안함)
        PassParameters->SDFLocalRingCenter = FVector3f::ZeroVector;
        PassParameters->SDFLocalRingAxis = FVector3f(0.0f, 0.0f, 1.0f);
    }

    // ===== Smoothing Bounds Z Extension Parameters =====
    // ===== 스무딩 영역 Z 확장 파라미터 =====
    PassParameters->BoundsZTop = Params.BoundsZTop;
    PassParameters->BoundsZBottom = Params.BoundsZBottom;

    // Get shader reference
    // 셰이더 참조 가져오기
    TShaderMapRef<FFleshRingTightnessCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // Calculate dispatch groups
    // 디스패치 그룹 수 계산
    const uint32 ThreadGroupSize = 64;
    const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumAffectedVertices, ThreadGroupSize);

    // Add compute pass to RDG
    // RDG에 컴퓨트 패스 추가
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("FleshRingTightnessCS_Skinned_Deprecated"),
        ComputeShader,
        PassParameters,
        FIntVector(static_cast<int32>(NumGroups), 1, 1)
    );
}

// ============================================================================
// Dispatch with Readback (for testing/validation)
// 리드백 포함 디스패치 (테스트/검증용)
// ============================================================================

void DispatchFleshRingTightnessCS_WithReadback(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef RepresentativeIndicesBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRHIGPUBufferReadback* Readback,
    FRDGTextureRef SDFTexture,
    FRDGBufferRef VolumeAccumBuffer)
{
    // Dispatch the compute shader
    // 컴퓨트 셰이더 디스패치
    DispatchFleshRingTightnessCS(
        GraphBuilder,
        Params,
        SourcePositionsBuffer,
        AffectedIndicesBuffer,
        InfluencesBuffer,
        RepresentativeIndicesBuffer,
        OutputPositionsBuffer,
        SDFTexture,
        VolumeAccumBuffer
    );

    // Add readback pass (GPU → CPU data transfer)
    // 리드백 패스 추가 (GPU → CPU 데이터 전송)
    AddEnqueueCopyPass(GraphBuilder, Readback, OutputPositionsBuffer, 0);
}

// ============================================================================
// 실제 에셋 기반 테스트 - FleshRing.TightnessTest 콘솔 커맨드
// 월드에서 FleshRingComponent를 찾아 실제 에셋 데이터로 TightnessCS 테스트
// ============================================================================

// GPU 결과 검증 함수 (현재는 사용하지 않음 - 각 Ring별 인라인 검증으로 대체)
// 필요시 재사용 가능하도록 유지

// ============================================================================
// FleshRing.TightnessTest - 실제 에셋 기반 TightnessCS 테스트 콘솔 커맨드
//
// 사용법: PIE 모드에서 콘솔에 FleshRing.TightnessTest 입력
// 조건: 월드에 FleshRingComponent가 있는 액터 + FleshRingAsset 할당 필요
// ============================================================================
static FAutoConsoleCommand GFleshRingTightnessTestCommand(
    TEXT("FleshRing.TightnessTest"),
    TEXT("FleshRingAsset을 사용하여 TightnessCS GPU 연산을 테스트합니다"),
    FConsoleCommandDelegate::CreateLambda([]()
    {
        UE_LOG(LogTemp, Log, TEXT(""));
        UE_LOG(LogTemp, Log, TEXT("========================================="));
        UE_LOG(LogTemp, Log, TEXT("  FleshRing TightnessCS 테스트"));
        UE_LOG(LogTemp, Log, TEXT("  (실제 에셋 기반 GPU 연산 검증)"));
        UE_LOG(LogTemp, Log, TEXT("========================================="));

        // ============================================================
        // 1단계: 월드에서 FleshRingComponent 탐색
        // ============================================================
        UE_LOG(LogTemp, Log, TEXT(""));
        UE_LOG(LogTemp, Log, TEXT("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
        UE_LOG(LogTemp, Log, TEXT("【 1단계: FleshRingComponent 탐색 】"));
        UE_LOG(LogTemp, Log, TEXT("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));

        UFleshRingComponent* FoundComponent = nullptr;
        USkeletalMeshComponent* TargetSkelMesh = nullptr;

        for (TObjectIterator<UFleshRingComponent> It; It; ++It)
        {
            UFleshRingComponent* Comp = *It;
            if (Comp && Comp->GetWorld() && !Comp->GetWorld()->IsPreviewWorld())
            {
                if (Comp->FleshRingAsset && Comp->GetResolvedTargetMesh())
                {
                    FoundComponent = Comp;
                    TargetSkelMesh = Comp->GetResolvedTargetMesh();
                    break;
                }
            }
        }

        if (!FoundComponent)
        {
            UE_LOG(LogTemp, Error, TEXT("  ✗ FleshRingComponent를 찾을 수 없습니다"));
            UE_LOG(LogTemp, Error, TEXT(""));
            UE_LOG(LogTemp, Error, TEXT("  해결 방법:"));
            UE_LOG(LogTemp, Error, TEXT("    1. 월드에 FleshRingComponent가 있는 액터를 배치하세요"));
            UE_LOG(LogTemp, Error, TEXT("    2. FleshRingAsset을 컴포넌트에 할당하세요"));
            UE_LOG(LogTemp, Error, TEXT("    3. PIE 모드(플레이)에서 테스트하세요"));
            return;
        }

        if (!TargetSkelMesh)
        {
            UE_LOG(LogTemp, Error, TEXT("  ✗ TargetSkeletalMesh가 없습니다"));
            return;
        }

        UFleshRingAsset* Asset = FoundComponent->FleshRingAsset;
        if (Asset->Rings.Num() == 0)
        {
            UE_LOG(LogTemp, Error, TEXT("  ✗ FleshRingAsset에 Ring이 없습니다"));
            return;
        }

        UE_LOG(LogTemp, Log, TEXT("  ✓ FleshRingComponent 발견"));
        UE_LOG(LogTemp, Log, TEXT("    - 액터: %s"), *FoundComponent->GetOwner()->GetName());
        UE_LOG(LogTemp, Log, TEXT("    - FleshRingAsset: %s"), *Asset->GetName());
        UE_LOG(LogTemp, Log, TEXT("    - Ring 개수: %d개"), Asset->Rings.Num());
        UE_LOG(LogTemp, Log, TEXT("    - TargetMesh: %s"), *TargetSkelMesh->GetName());

        // ============================================================
        // 2단계: AffectedVertices 등록 (영향 버텍스 선택)
        // ============================================================
        UE_LOG(LogTemp, Log, TEXT(""));
        UE_LOG(LogTemp, Log, TEXT("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
        UE_LOG(LogTemp, Log, TEXT("【 2단계: 영향 버텍스 선택 】"));
        UE_LOG(LogTemp, Log, TEXT("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));

        FFleshRingAffectedVerticesManager AffectedManager;
        if (!AffectedManager.RegisterAffectedVertices(FoundComponent, TargetSkelMesh))
        {
            UE_LOG(LogTemp, Error, TEXT("  ✗ AffectedVertices 등록 실패"));
            return;
        }

        const TArray<FRingAffectedData>& AllRingData = AffectedManager.GetAllRingData();
        if (AllRingData.Num() == 0)
        {
            UE_LOG(LogTemp, Error, TEXT("  ✗ 등록된 Ring 데이터 없음"));
            return;
        }

        UE_LOG(LogTemp, Log, TEXT("  ✓ 영향 버텍스 선택 완료"));
        UE_LOG(LogTemp, Log, TEXT("    - 처리된 Ring 수: %d개"), AllRingData.Num());
        UE_LOG(LogTemp, Log, TEXT("    - 총 영향 버텍스: %d개"), AffectedManager.GetTotalAffectedCount());

        // Ring별 영향 버텍스 요약
        for (int32 i = 0; i < AllRingData.Num(); ++i)
        {
            UE_LOG(LogTemp, Log, TEXT("    - Ring[%d] '%s': %d개 버텍스"),
                i, *AllRingData[i].BoneName.ToString(), AllRingData[i].Vertices.Num());
        }

        // ============================================================
        // 3단계: 메시에서 버텍스 데이터 추출
        // ============================================================
        USkeletalMesh* SkelMesh = TargetSkelMesh->GetSkeletalMeshAsset();
        if (!SkelMesh)
        {
            UE_LOG(LogTemp, Error, TEXT("  ✗ SkeletalMesh 에셋 없음"));
            return;
        }

        const FSkeletalMeshRenderData* RenderData = SkelMesh->GetResourceForRendering();
        if (!RenderData || RenderData->LODRenderData.Num() == 0)
        {
            UE_LOG(LogTemp, Error, TEXT("  ✗ RenderData 없음"));
            return;
        }

        const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];
        const uint32 TotalVertexCount = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

        UE_LOG(LogTemp, Log, TEXT(""));
        UE_LOG(LogTemp, Log, TEXT("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
        UE_LOG(LogTemp, Log, TEXT("【 3단계: 메시 버텍스 데이터 추출 】"));
        UE_LOG(LogTemp, Log, TEXT("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
        UE_LOG(LogTemp, Log, TEXT("  메시 전체 버텍스 수: %d개"), TotalVertexCount);

        // 버텍스 위치 데이터 추출 (모든 Ring에서 공유)
        TArray<float> SourcePositions;
        SourcePositions.SetNum(TotalVertexCount * 3);

        for (uint32 i = 0; i < TotalVertexCount; ++i)
        {
            const FVector3f& Pos = LODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i);
            SourcePositions[i * 3 + 0] = Pos.X;
            SourcePositions[i * 3 + 1] = Pos.Y;
            SourcePositions[i * 3 + 2] = Pos.Z;
        }
        UE_LOG(LogTemp, Log, TEXT("  버텍스 위치 버퍼 추출 완료"));

        // 공유 데이터 포인터
        TSharedPtr<TArray<float>> SourceDataPtr = MakeShared<TArray<float>>(SourcePositions);

        // ============================================================
        // 4단계: 각 Ring별로 GPU 테스트 실행
        // ============================================================
        UE_LOG(LogTemp, Log, TEXT(""));
        UE_LOG(LogTemp, Log, TEXT("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
        UE_LOG(LogTemp, Log, TEXT("【 4단계: Ring별 GPU TightnessCS 테스트 】"));
        UE_LOG(LogTemp, Log, TEXT("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));

        int32 TestedRingCount = 0;

        for (int32 RingIdx = 0; RingIdx < AllRingData.Num(); ++RingIdx)
        {
            const FRingAffectedData& RingData = AllRingData[RingIdx];

            UE_LOG(LogTemp, Log, TEXT(""));
            UE_LOG(LogTemp, Log, TEXT("──────────────────────────────────────"));
            UE_LOG(LogTemp, Log, TEXT("▶ Ring[%d] '%s' 테스트"), RingIdx, *RingData.BoneName.ToString());
            UE_LOG(LogTemp, Log, TEXT("──────────────────────────────────────"));

            // FalloffType을 문자열로 변환
            FString FalloffTypeStr;
            switch (RingData.FalloffType)
            {
            case EFalloffType::Linear:    FalloffTypeStr = TEXT("Linear (선형)"); break;
            case EFalloffType::Quadratic: FalloffTypeStr = TEXT("Quadratic (2차)"); break;
            case EFalloffType::Hermite:   FalloffTypeStr = TEXT("Hermite (S커브)"); break;
            default:                      FalloffTypeStr = TEXT("Unknown"); break;
            }

            UE_LOG(LogTemp, Log, TEXT("  [Ring 설정]"));
            UE_LOG(LogTemp, Log, TEXT("    - 본 위치 (바인드포즈): (%.2f, %.2f, %.2f)"),
                RingData.RingCenter.X, RingData.RingCenter.Y, RingData.RingCenter.Z);
            UE_LOG(LogTemp, Log, TEXT("    - 본 축 방향: (%.2f, %.2f, %.2f)"),
                RingData.RingAxis.X, RingData.RingAxis.Y, RingData.RingAxis.Z);
            UE_LOG(LogTemp, Log, TEXT("    - Ring 반지름: %.2f"), RingData.RingRadius);
            UE_LOG(LogTemp, Log, TEXT("    - Ring 너비: %.2f"), RingData.RingWidth);
            UE_LOG(LogTemp, Log, TEXT("    - 영향 범위 (Radius+Width): %.2f"), RingData.RingRadius + RingData.RingWidth);
            UE_LOG(LogTemp, Log, TEXT("    - 조이기 강도: %.2f"), RingData.TightnessStrength);
            UE_LOG(LogTemp, Log, TEXT("    - 감쇠 타입: %s"), *FalloffTypeStr);

            UE_LOG(LogTemp, Log, TEXT(""));
            UE_LOG(LogTemp, Log, TEXT("  [영향 버텍스]"));
            UE_LOG(LogTemp, Log, TEXT("    - 선택된 버텍스 수: %d개"), RingData.Vertices.Num());

            if (RingData.Vertices.Num() == 0)
            {
                UE_LOG(LogTemp, Warning, TEXT("    ⚠ 영향 버텍스 없음 - 이 Ring 테스트 스킵"));
                UE_LOG(LogTemp, Warning, TEXT("    → Ring 위치/크기를 확인하거나 Radius/Width 값을 늘려보세요"));
                continue;
            }

            // 샘플 버텍스 정보 출력
            UE_LOG(LogTemp, Log, TEXT("    - 샘플 버텍스 (최대 5개):"));
            int32 SampleCount = FMath::Min(5, RingData.Vertices.Num());
            for (int32 i = 0; i < SampleCount; ++i)
            {
                const FAffectedVertex& V = RingData.Vertices[i];
                uint32 BaseIdx = V.VertexIndex * 3;
                UE_LOG(LogTemp, Log, TEXT("      [%d] 버텍스#%d: 반경거리=%.2f, 영향도=%.3f, 위치=(%.2f, %.2f, %.2f)"),
                    i, V.VertexIndex, V.RadialDistance, V.Influence,
                    SourcePositions[BaseIdx], SourcePositions[BaseIdx + 1], SourcePositions[BaseIdx + 2]);
            }

            // GPU Dispatch 준비
            TSharedPtr<TArray<uint32>> IndicesPtr = MakeShared<TArray<uint32>>(RingData.PackedIndices);
            TSharedPtr<TArray<float>> InfluencesPtr = MakeShared<TArray<float>>(RingData.PackedInfluences);
            TSharedPtr<FRHIGPUBufferReadback> Readback = MakeShared<FRHIGPUBufferReadback>(
                *FString::Printf(TEXT("TightnessTestReadback_Ring%d"), RingIdx));

            FTightnessDispatchParams Params = CreateTightnessParams(RingData, TotalVertexCount);
            FName BoneName = RingData.BoneName;

            UE_LOG(LogTemp, Log, TEXT(""));
            UE_LOG(LogTemp, Log, TEXT("  [GPU Dispatch]"));
            UE_LOG(LogTemp, Log, TEXT("    - 버퍼 생성 중..."));

            // ================================================================
            // 렌더 스레드에서 RDG(Render Dependency Graph) Dispatch
            // ================================================================
            // RDG는 "지연 실행" 방식:
            //   1. CreateBuffer / QueueBufferUpload / CreateSRV 등은 "예약"만 함
            //   2. GraphBuilder.Execute() 호출 시 의존성 순서대로 실제 실행
            //   3. 따라서 Dispatch 함수에 버퍼를 넘길 때 이미 데이터가 "예약"된 상태
            // ================================================================
            ENQUEUE_RENDER_COMMAND(FleshRingTightnessTest_Dispatch)(
                [SourceDataPtr, IndicesPtr, InfluencesPtr, Params, Readback, TotalVertexCount, RingIdx, BoneName]
                (FRHICommandListImmediate& RHICmdList)
                {
                    FRDGBuilder GraphBuilder(RHICmdList);

                    // ========================================
                    // [1단계] 버퍼 생성 + 데이터 업로드 "예약"
                    // ========================================

                    // Source positions 버퍼 (입력: 원본 버텍스 위치)
                    FRDGBufferRef SourceBuffer = GraphBuilder.CreateBuffer(
                        FRDGBufferDesc::CreateBufferDesc(sizeof(float), TotalVertexCount * 3),
                        TEXT("TightnessTest_SourcePositions")
                    );
                    GraphBuilder.QueueBufferUpload(  // 데이터 업로드 "예약"
                        SourceBuffer,
                        SourceDataPtr->GetData(),
                        SourceDataPtr->Num() * sizeof(float),
                        ERDGInitialDataFlags::None
                    );

                    // Affected indices 버퍼 (입력: 영향받는 버텍스 인덱스)
                    FRDGBufferRef IndicesBuffer = GraphBuilder.CreateBuffer(
                        FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Params.NumAffectedVertices),
                        TEXT("TightnessTest_AffectedIndices")
                    );
                    GraphBuilder.QueueBufferUpload(  // 데이터 업로드 "예약"
                        IndicesBuffer,
                        IndicesPtr->GetData(),
                        IndicesPtr->Num() * sizeof(uint32),
                        ERDGInitialDataFlags::None
                    );

                    // Influences 버퍼 (입력: 버텍스별 영향도)
                    FRDGBufferRef InfluencesBuffer = GraphBuilder.CreateBuffer(
                        FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Params.NumAffectedVertices),
                        TEXT("TightnessTest_Influences")
                    );
                    GraphBuilder.QueueBufferUpload(  // 데이터 업로드 "예약"
                        InfluencesBuffer,
                        InfluencesPtr->GetData(),
                        InfluencesPtr->Num() * sizeof(float),
                        ERDGInitialDataFlags::None
                    );

                    // Output 버퍼 (출력: 변형된 버텍스 위치)
                    FRDGBufferRef OutputBuffer = GraphBuilder.CreateBuffer(
                        FRDGBufferDesc::CreateBufferDesc(sizeof(float), TotalVertexCount * 3),
                        TEXT("TightnessTest_OutputPositions")
                    );

                    // Source를 Output에 복사 (영향 안 받는 버텍스 보존)
                    // Copy source to output (preserve unaffected vertices)
                    AddCopyBufferPass(GraphBuilder, OutputBuffer, SourceBuffer);

                    // ========================================
                    // [2단계] Dispatch 호출 "예약"
                    // ========================================
                    // 이 시점에서:
                    //   - 버퍼들은 아직 GPU 메모리에 실제 생성되지 않음
                    //   - 데이터도 아직 업로드되지 않음
                    //   - DispatchFleshRingTightnessCS 내부에서:
                    //     1. CreateSRV/CreateUAV = View 생성 "예약"
                    //     2. AddPass = 셰이더 실행 "예약"
                    //   - 모든 것은 GraphBuilder에 "예약"만 된 상태
                    // ========================================
                    DispatchFleshRingTightnessCS_WithReadback(
                        GraphBuilder,
                        Params,
                        SourceBuffer,
                        IndicesBuffer,
                        InfluencesBuffer,
                        nullptr,  // RepresentativeIndicesBuffer - 테스트에서는 사용하지 않음
                        OutputBuffer,
                        Readback.Get()
                    );

                    // ========================================
                    // [3단계] Execute() = 실제 실행
                    // ========================================
                    // Execute() 호출 시 발생하는 일:
                    //   1. RDG가 모든 리소스 의존성 분석
                    //   2. 최적의 실행 순서 결정
                    //   3. GPU 버퍼 실제 생성
                    //   4. QueueBufferUpload로 예약된 데이터 실제 업로드
                    //   5. AddPass로 예약된 셰이더 실제 실행
                    //   6. Readback 패스 실행 (GPU→CPU 복사)
                    // ========================================
                    GraphBuilder.Execute();
                    UE_LOG(LogTemp, Log, TEXT("    - Ring[%d] '%s' GPU Dispatch 완료"), RingIdx, *BoneName.ToString());
                });

            // 결과 검증
            ENQUEUE_RENDER_COMMAND(FleshRingTightnessTest_Validate)(
                [SourceDataPtr, IndicesPtr, InfluencesPtr, Readback, Params, TotalVertexCount, RingIdx, BoneName]
                (FRHICommandListImmediate& RHICmdList)
                {
                    if (!Readback->IsReady())
                    {
                        RHICmdList.BlockUntilGPUIdle();
                    }

                    UE_LOG(LogTemp, Log, TEXT(""));
                    UE_LOG(LogTemp, Log, TEXT("  [Ring[%d] '%s' 검증 결과]"), RingIdx, *BoneName.ToString());

                    if (Readback->IsReady())
                    {
                        const float* OutputData = static_cast<const float*>(
                            Readback->Lock(TotalVertexCount * 3 * sizeof(float)));

                        if (OutputData)
                        {
                            // CPU에서 GPU 결과 검증
                            uint32 PassCount = 0;
                            uint32 FailCount = 0;

                            for (uint32 i = 0; i < Params.NumAffectedVertices; ++i)
                            {
                                uint32 VertexIndex = (*IndicesPtr)[i];
                                float Influence = (*InfluencesPtr)[i];

                                uint32 BaseIndex = VertexIndex * 3;
                                FVector3f SourcePos(
                                    (*SourceDataPtr)[BaseIndex + 0],
                                    (*SourceDataPtr)[BaseIndex + 1],
                                    (*SourceDataPtr)[BaseIndex + 2]
                                );
                                FVector3f OutputPos(
                                    OutputData[BaseIndex + 0],
                                    OutputData[BaseIndex + 1],
                                    OutputData[BaseIndex + 2]
                                );

                                // 예상 결과 계산 (셰이더와 동일한 로직)
                                FVector3f ToVertex = SourcePos - Params.RingCenter;
                                float AxisDist = FVector3f::DotProduct(ToVertex, Params.RingAxis);
                                FVector3f RadialVec = ToVertex - Params.RingAxis * AxisDist;
                                float RadialDist = RadialVec.Size();

                                FVector3f ExpectedPos = SourcePos;
                                if (RadialDist > 0.001f)
                                {
                                    FVector3f InwardDir = -RadialVec / RadialDist;
                                    float Displacement = Params.TightnessStrength * Influence;
                                    ExpectedPos = SourcePos + InwardDir * Displacement;
                                }

                                bool bMatch = FVector3f::Distance(OutputPos, ExpectedPos) < 0.01f;
                                if (bMatch) PassCount++;
                                else FailCount++;
                            }

                            if (FailCount == 0)
                            {
                                UE_LOG(LogTemp, Log, TEXT("    ✓ 검증 성공: %d개 버텍스 모두 정상 변형됨"), PassCount);
                            }
                            else
                            {
                                UE_LOG(LogTemp, Error, TEXT("    ✗ 검증 실패: 성공=%d, 실패=%d"), PassCount, FailCount);
                            }

                            Readback->Unlock();
                        }
                        else
                        {
                            UE_LOG(LogTemp, Error, TEXT("    ✗ Readback Lock 실패"));
                        }
                    }
                    else
                    {
                        UE_LOG(LogTemp, Error, TEXT("    ✗ Readback 준비 안됨"));
                    }
                });

            TestedRingCount++;
        }

        UE_LOG(LogTemp, Log, TEXT(""));
        UE_LOG(LogTemp, Log, TEXT("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
        UE_LOG(LogTemp, Log, TEXT("【 테스트 완료 】"));
        UE_LOG(LogTemp, Log, TEXT("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"));
        UE_LOG(LogTemp, Log, TEXT("  총 Ring 수: %d개"), AllRingData.Num());
        UE_LOG(LogTemp, Log, TEXT("  테스트된 Ring 수: %d개"), TestedRingCount);
        UE_LOG(LogTemp, Log, TEXT("  (영향 버텍스가 있는 Ring만 테스트됨)"));
        UE_LOG(LogTemp, Log, TEXT(""));
        UE_LOG(LogTemp, Log, TEXT("  ※ 검증 결과는 렌더 스레드에서 출력됩니다"));
        UE_LOG(LogTemp, Log, TEXT("========================================="));
    })
);
