// Purpose: Pull vertices toward Ring center axis (Tightness effect)

#include "FleshRingTightnessShader.h"

#include "CoreMinimal.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"

// [추가] 실제 에셋 기반 테스트를 위한 include
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"

// Shader Implementation Registration
IMPLEMENT_GLOBAL_SHADER(
    FFleshRingTightnessCS,
    "/Plugin/FleshRingPlugin/FleshRingTightnessCS.usf",
    "MainCS",
    SF_Compute
);

// Dispatch Function Implementation
void DispatchFleshRingTightnessCS(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef OutputPositionsBuffer)
{
    // Early out if no vertices to process
    if (Params.NumAffectedVertices == 0)
    {
        return;
    }

    // Allocate shader parameters
    FFleshRingTightnessCS::FParameters* PassParameters =
        GraphBuilder.AllocParameters<FFleshRingTightnessCS::FParameters>();

    // ==== Bind buffers ==== 
    // Create SRV (Read Only)
    PassParameters->SourcePositions = GraphBuilder.CreateSRV(SourcePositionsBuffer, PF_R32_FLOAT);
    PassParameters->AffectedIndices = GraphBuilder.CreateSRV(AffectedIndicesBuffer);
    PassParameters->Influences = GraphBuilder.CreateSRV(InfluencesBuffer);

    // Create UAV (Read and Write)
    PassParameters->OutputPositions = GraphBuilder.CreateUAV(OutputPositionsBuffer, PF_R32_FLOAT);

    // Set ring parameters
    PassParameters->RingCenter = Params.RingCenter;
    PassParameters->RingAxis = Params.RingAxis;
    PassParameters->TightnessStrength = Params.TightnessStrength;
    PassParameters->RingRadius = Params.RingRadius;
    PassParameters->RingWidth = Params.RingWidth;

    // Set counts
    PassParameters->NumAffectedVertices = Params.NumAffectedVertices;
    PassParameters->NumTotalVertices = Params.NumTotalVertices;

    // Get shader reference
    TShaderMapRef<FFleshRingTightnessCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // Calculate dispatch groups
    const uint32 ThreadGroupSize = 64; // match .usf [numthreads(64,1,1)]
    const uint32 NumGroups = FMath::DivideAndRoundUp(Params.NumAffectedVertices, ThreadGroupSize); // (NumAffectedVertices + 64 - 1) / 64

    // Add compute pass to RDG
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("FleshRingTightnessCS"),
        ComputeShader,
        PassParameters,
        FIntVector(static_cast<int32>(NumGroups), 1, 1) // Dispatch(NumGroups, 1, 1)
    );
}

// Dispatch with Readback (for testing/validation)
void DispatchFleshRingTightnessCS_WithReadback(
    FRDGBuilder& GraphBuilder,
    const FTightnessDispatchParams& Params,
    FRDGBufferRef SourcePositionsBuffer,
    FRDGBufferRef AffectedIndicesBuffer,
    FRDGBufferRef InfluencesBuffer,
    FRDGBufferRef OutputPositionsBuffer,
    FRHIGPUBufferReadback* Readback)
{
    // Dispatch the compute shader
    DispatchFleshRingTightnessCS(
        GraphBuilder,
        Params,
        SourcePositionsBuffer,
        AffectedIndicesBuffer,
        InfluencesBuffer,
        OutputPositionsBuffer
    );

    // Add readback pass
    AddEnqueueCopyPass(GraphBuilder, Readback, OutputPositionsBuffer, 0);
}

// ============================================================================
// [변경] 실제 에셋 기반 테스트 - FleshRing.TightnessTest 콘솔 커맨드
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
                UE_LOG(LogTemp, Log, TEXT("      [%d] 버텍스#%d: 거리=%.2f, 영향도=%.3f, 위치=(%.2f, %.2f, %.2f)"),
                    i, V.VertexIndex, V.DistanceToRing, V.Influence,
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

            // 렌더 스레드에서 Dispatch
            ENQUEUE_RENDER_COMMAND(FleshRingTightnessTest_Dispatch)(
                [SourceDataPtr, IndicesPtr, InfluencesPtr, Params, Readback, TotalVertexCount, RingIdx, BoneName]
                (FRHICommandListImmediate& RHICmdList)
                {
                    FRDGBuilder GraphBuilder(RHICmdList);

                    // Source positions 버퍼
                    FRDGBufferRef SourceBuffer = GraphBuilder.CreateBuffer(
                        FRDGBufferDesc::CreateBufferDesc(sizeof(float), TotalVertexCount * 3),
                        TEXT("TightnessTest_SourcePositions")
                    );
                    GraphBuilder.QueueBufferUpload(
                        SourceBuffer,
                        SourceDataPtr->GetData(),
                        SourceDataPtr->Num() * sizeof(float),
                        ERDGInitialDataFlags::None
                    );

                    // Affected indices 버퍼
                    FRDGBufferRef IndicesBuffer = GraphBuilder.CreateBuffer(
                        FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Params.NumAffectedVertices),
                        TEXT("TightnessTest_AffectedIndices")
                    );
                    GraphBuilder.QueueBufferUpload(
                        IndicesBuffer,
                        IndicesPtr->GetData(),
                        IndicesPtr->Num() * sizeof(uint32),
                        ERDGInitialDataFlags::None
                    );

                    // Influences 버퍼
                    FRDGBufferRef InfluencesBuffer = GraphBuilder.CreateBuffer(
                        FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Params.NumAffectedVertices),
                        TEXT("TightnessTest_Influences")
                    );
                    GraphBuilder.QueueBufferUpload(
                        InfluencesBuffer,
                        InfluencesPtr->GetData(),
                        InfluencesPtr->Num() * sizeof(float),
                        ERDGInitialDataFlags::None
                    );

                    // Output 버퍼
                    FRDGBufferRef OutputBuffer = GraphBuilder.CreateBuffer(
                        FRDGBufferDesc::CreateBufferDesc(sizeof(float), TotalVertexCount * 3),
                        TEXT("TightnessTest_OutputPositions")
                    );

                    // Source를 Output에 복사 (영향 안 받는 버텍스 보존)
                    AddCopyBufferPass(GraphBuilder, OutputBuffer, SourceBuffer);

                    // TightnessCS Dispatch
                    DispatchFleshRingTightnessCS_WithReadback(
                        GraphBuilder,
                        Params,
                        SourceBuffer,
                        IndicesBuffer,
                        InfluencesBuffer,
                        OutputBuffer,
                        Readback.Get()
                    );

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
