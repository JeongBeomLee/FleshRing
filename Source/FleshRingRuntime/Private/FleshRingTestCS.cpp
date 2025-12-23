// FleshRing Test Compute Shader - C++ 구현
// 목적: GPU Compute Shader �이�라�작 �인

// �리코딩 규칙: �신�더�반드�번째�include
#include "FleshRingTestCS.h"

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderCompilerCore.h"
#include "RenderingThread.h"  // ENQUEUE_RENDER_COMMAND �용�해
#include "RHIGPUReadback.h"   // GPU -> CPU readback�해

// ============================================================================
// Compute Shader �래�의
// ============================================================================
// FGlobalShader: 머티리얼�무�게 �립�으롬용 가�한 �이// Compute Shader보통 FGlobalShader륁속받음
class FFleshRingTestCS : public FGlobalShader
{
public:
    // �래�� Global Shader�을 �리�에 �록
    DECLARE_GLOBAL_SHADER(FFleshRingTestCS);

    // BEGIN_SHADER_PARAMETER_STRUCT롕의�라미터 구조체� �용
    SHADER_USE_PARAMETER_STRUCT(FFleshRingTestCS, FGlobalShader);

    // ========================================================================
    // �이�라미터 구조�    // ========================================================================
    // .usf �일변�들�1:1 매칭�어    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // RWStructuredBuffer<float> TestBuffer; � 매칭
        // UAV = Unordered Access View (�기+�기 가
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, TestBuffer)

        // uint BufferSize; � 매칭
        SHADER_PARAMETER(uint32, BufferSize)
    END_SHADER_PARAMETER_STRUCT()

    // ========================================================================
    // �이컴파조건 (�택
    // ========================================================================
    // �떤 �랫�에�이�� 컴파�할지 결정
    // Windows/PS5/XSX �� �랫�� 모두 Compute Shader 지    // 모바�겄요IsFeatureLevelSupported() 조건 추� 가    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return true;
    }
};

// ============================================================================
// �이구현 �록
// ============================================================================
// IMPLEMENT_GLOBAL_SHADER(�래�명, .usf경로, 진입�함 �이��
//
// 경로 �명:
// - "/Plugin/FleshRingPlugin/..." � 가경로
// - UE5가 �동�로 Plugins/FleshRingPlugin/Shaders/ -> /Plugin/FleshRingPlugin/ 매핑
// - �제 �일: Plugins/FleshRingPlugin/Shaders/FleshRingTestCS.usf
//
// SF_Compute: �이�� Compute Shader�을 지IMPLEMENT_GLOBAL_SHADER(
    FFleshRingTestCS,                           // C++ �래�명
    "/FleshRingPlugin/FleshRingTestCS.usf",     // .usf �일 가경로
    "/Plugin/FleshRingPlugin/FleshRingTestCS.usf",      // .usf �일 가경로
    "/Plugin/FleshRingPlugin/FleshRingTestCS.usf",     // .usf �일 가경로
    "MainCS",                                   // .usf 진입�수�    SF_Compute                                  // �이�);

// ============================================================================
// Dispatch �수 (��서 �출
// ============================================================================
// FRDGBuilder: Render Dependency Graph 빌더
// - GPU 리소�성, Pass 추�, �존관리� �당
//
// Count: 처리�이개수
void DispatchFleshRingTestCS(FRDGBuilder& GraphBuilder, uint32 Count)
{
    // 1. �라미터 구조철당
    // GraphBuilder가 메모�관�(�동 �제 불필
    FFleshRingTestCS::FParameters* Parameters =
        GraphBuilder.AllocParameters<FFleshRingTestCS::FParameters>();

    // 2. GPU 버퍼 �성
    // CreateStructuredDesc: 구조버퍼 �명�성
    // - sizeof(float): 갔소�기 (4바이
    // - Count: �소 개수
    FRDGBufferRef TestBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Count),
        TEXT("FleshRingTestBuffer")  // �버깅용 �름 (RenderDoc�서 보임)
    );

    // 3. UAV (�기 가� �성 밌라미터바인    Parameters->TestBuffer = GraphBuilder.CreateUAV(TestBuffer);
    Parameters->BufferSize = Count;

    // 4. �이가�오�    // TShaderMapRef: 컴파�된 �이�에 �참조
    // GetGlobalShaderMap: �역 �이맵에가�옴
    // GMaxRHIFeatureLevel: �재 RHI최� 기능 �벨
    TShaderMapRef<FFleshRingTestCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // 5. Compute Shader Pass 추�
    // FComputeShaderUtils::AddPass: RDGCS �행 Pass 추�
    //
    // �라미터:
    // - GraphBuilder: RDG 빌더
    // - RDG_EVENT_NAME: �버깅용 �벤�름 (RenderDoc�서 보임)
    // - ComputeShader: �행�이    // - Parameters: �이�에 �달�라미터
    // - FIntVector: Dispatch 그룹 (X, Y, Z)
    //
    // 그룹 계산:
    // - �체 �이 Count�    // - 그룹�레 64�([numthreads(64,1,1)])
    // - �요그룹: ceil(Count / 64)
    // - DivideAndRoundUp: �림 �눗    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("FleshRingTestCS"),
        ComputeShader,
        Parameters,
        FIntVector(
            FMath::DivideAndRoundUp(Count, 64u),  // X 방향 그룹             1,                                     // Y 방향 그룹             1                                      // Z 방향 그룹         )
    );

    // 참고: �제 GPU �행� GraphBuilder.Execute() �출 �어    // �수Pass�"�약"맘는 �}

// ============================================================================
// 검증용 Dispatch �수 (Readback �함)
// ============================================================================
// GPU�서 계산결과�CPU롽어�검�// Readback 객체�출�� �유권을 가�void DispatchFleshRingTestCS_WithReadback(
    FRDGBuilder& GraphBuilder,
    uint32 Count,
    FRHIGPUBufferReadback* Readback)
{
    // 1. �라미터 구조철당
    FFleshRingTestCS::FParameters* Parameters =
        GraphBuilder.AllocParameters<FFleshRingTestCS::FParameters>();

    // 2. GPU 버퍼 �성
    FRDGBufferRef TestBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Count),
        TEXT("FleshRingTestBuffer")
    );

    // 3. UAV �성 밌라미터 바인    Parameters->TestBuffer = GraphBuilder.CreateUAV(TestBuffer);
    Parameters->BufferSize = Count;

    // 4. �이가�오�    TShaderMapRef<FFleshRingTestCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // 5. Compute Shader Pass 추�
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("FleshRingTestCS"),
        ComputeShader,
        Parameters,
        FIntVector(FMath::DivideAndRoundUp(Count, 64u), 1, 1)
    );

    // 6. Readback Pass 추�
    // GPU 버퍼 -> CPU 메모리로 복사 �약
    // AddEnqueueCopyPass: RDG 버퍼�Readback 객체�복사
    AddEnqueueCopyPass(GraphBuilder, Readback, TestBuffer, 0);
}

// ============================================================================
// 결과 검즨수
// ============================================================================
// GPU�서 계산값이 �상�ThreadId * 2.0f)과치�는지 �인
void ValidateTestCSResults(const float* Data, uint32 Count)
{
    uint32 PassCount = 0;
    uint32 FailCount = 0;
    constexpr uint32 MaxErrorsToLog = 10;  // �무 많� �러 로그 방�

    for (uint32 i = 0; i < Count; ++i)
    {
        // �상� ThreadId * 2.0f (FleshRingTestCS.usf 참조)
        const float Expected = static_cast<float>(i) * 2.0f;
        const float Actual = Data[i];

        // 부�소�점 비교 (�차 �용)
        if (FMath::IsNearlyEqual(Actual, Expected, 0.001f))
        {
            PassCount++;
        }
        else
        {
            FailCount++;
            if (FailCount <= MaxErrorsToLog)
            {
                UE_LOG(LogTemp, Error,
                    TEXT("FleshRing.TestCS: MISMATCH at [%d] - Expected: %.2f, Actual: %.2f"),
                    i, Expected, Actual);
            }
        }
    }

    // 결과 �약 로그
    if (FailCount == 0)
    {
        UE_LOG(LogTemp, Log,
            TEXT("FleshRing.TestCS: ===== VALIDATION PASSED ====="));
        UE_LOG(LogTemp, Log,
            TEXT("FleshRing.TestCS: All %d elements computed correctly!"), PassCount);
        UE_LOG(LogTemp, Log,
            TEXT("FleshRing.TestCS: Sample values - [0]=%.1f, [1]=%.1f, [2]=%.1f, [100]=%.1f"),
            Data[0], Data[1], Data[2], Data[100]);
    }
    else
    {
        UE_LOG(LogTemp, Error,
            TEXT("FleshRing.TestCS: ===== VALIDATION FAILED ====="));
        UE_LOG(LogTemp, Error,
            TEXT("FleshRing.TestCS: Passed: %d, Failed: %d (Total: %d)"),
            PassCount, FailCount, Count);
    }
}

// ============================================================================
// 콘솔 명령�록
// ============================================================================
// FAutoConsoleCommand: �적 객체록성�면 모듈 로드 �동�로 콘솔 명령�록
//
// �용� �디콘솔(~)�서 "FleshRing.TestCS" �력
static FAutoConsoleCommand GFleshRingTestCSCommand(
    TEXT("FleshRing.TestCS"),
    TEXT("FleshRing Compute Shader �스�행 �결과 검�),
    FConsoleCommandDelegate::CreateLambda([]()
    {
        const uint32 TestCount = 1024;

        UE_LOG(LogTemp, Log, TEXT("========================================="));
        UE_LOG(LogTemp, Log, TEXT("FleshRing.TestCS: Starting compute shader test"));
        UE_LOG(LogTemp, Log, TEXT("FleshRing.TestCS: Element count: %d"), TestCount);
        UE_LOG(LogTemp, Log, TEXT("========================================="));

        // Readback 객체 �성 (shared_ptr롌다 �공유)
        // GPU -> CPU �이�송�한 객체
        TSharedPtr<FRHIGPUBufferReadback> Readback =
            MakeShared<FRHIGPUBufferReadback>(TEXT("FleshRingTestReadback"));

        // 1�계: �더매레�에CS �행 �Readback �약
        ENQUEUE_RENDER_COMMAND(FleshRingTestCS_Dispatch)(
            [TestCount, Readback](FRHICommandListImmediate& RHICmdList)
            {
                FRDGBuilder GraphBuilder(RHICmdList);
                DispatchFleshRingTestCS_WithReadback(GraphBuilder, TestCount, Readback.Get());
                GraphBuilder.Execute();

                UE_LOG(LogTemp, Log, TEXT("FleshRing.TestCS: Dispatch and readback enqueued"));
            });

        // 2�계: 결과 검�(Readback �료 ��
        // 별도�더 커맨�로 분리�여 �전 커맨�료 보장
        ENQUEUE_RENDER_COMMAND(FleshRingTestCS_Validate)(
            [TestCount, Readback](FRHICommandListImmediate& RHICmdList)
            {
                // Readback �료 ��                // IsReady()가 true가 �까지 ��                // �반�으롴전 커맨�행 �이므�바로 ready �태
                if (!Readback->IsReady())
                {
                    // 만약 �직 ready가 �니� RHI flush�강제 �기                    RHICmdList.BlockUntilGPUIdle();
                }

                if (Readback->IsReady())
                {
                    // GPU 메모�-> CPU 메모�매핑
                    const float* ResultData = static_cast<const float*>(
                        Readback->Lock(TestCount * sizeof(float)));

                    if (ResultData)
                    {
                        // 결과 검�                        ValidateTestCSResults(ResultData, TestCount);

                        // 매핑 �제
                        Readback->Unlock();
                    }
                    else
                    {
                        UE_LOG(LogTemp, Error,
                            TEXT("FleshRing.TestCS: Failed to lock readback buffer"));
                    }
                }
                else
                {
                    UE_LOG(LogTemp, Error,
                        TEXT("FleshRing.TestCS: Readback not ready after GPU idle"));
                }
            });

        UE_LOG(LogTemp, Log, TEXT("FleshRing.TestCS: Test commands enqueued"));
    })
);
