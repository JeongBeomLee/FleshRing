// FleshRing Test Compute Shader - C++ 구현
// 목적: GPU Compute Shader 파이프라인 동작 확인

// 언리얼 코딩 규칙: 자신의 헤더를 반드시 첫 번째로 include
#include "FleshRingTestCS.h"

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderCompilerCore.h"
#include "RenderingThread.h"  // ENQUEUE_RENDER_COMMAND 사용을 위해
#include "RHIGPUReadback.h"   // GPU -> CPU readback을 위해

// ============================================================================
// Compute Shader 클래스 정의
// ============================================================================
// FGlobalShader: 머티리얼과 무관하게 독립적으로 사용 가능한 셰이더
// Compute Shader는 보통 FGlobalShader를 상속받음
class FFleshRingTestCS : public FGlobalShader
{
public:
    // 이 클래스가 Global Shader임을 언리얼에 등록
    DECLARE_GLOBAL_SHADER(FFleshRingTestCS);

    // BEGIN_SHADER_PARAMETER_STRUCT로 정의한 파라미터 구조체를 사용
    SHADER_USE_PARAMETER_STRUCT(FFleshRingTestCS, FGlobalShader);

    // ========================================================================
    // 셰이더 파라미터 구조체
    // ========================================================================
    // .usf 파일의 변수들과 1:1 매칭되어야 함
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // RWStructuredBuffer<float> TestBuffer; 와 매칭
        // UAV = Unordered Access View (읽기+쓰기 가능)
        SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, TestBuffer)

        // uint BufferSize; 와 매칭
        SHADER_PARAMETER(uint32, BufferSize)
    END_SHADER_PARAMETER_STRUCT()

    // ========================================================================
    // 셰이더 컴파일 조건 (선택적)
    // ========================================================================
    // 어떤 플랫폼에서 이 셰이더를 컴파일할지 결정
    // Windows/PS5/XSX 등 현대 플랫폼은 모두 Compute Shader 지원
    // 모바일 타겟 필요시 IsFeatureLevelSupported() 조건 추가 가능
    static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
    {
        return true;
    }
};

// ============================================================================
// 셰이더 구현 등록
// ============================================================================
// IMPLEMENT_GLOBAL_SHADER(클래스명, .usf경로, 진입점함수, 셰이더타입)
//
// 경로 설명:
// - "/FleshRingPlugin/..." 은 가상 경로
// - FleshRingRuntime.cpp의 StartupModule()에서 실제 경로와 매핑됨
// - 실제 파일: Plugins/FleshRingPlugin/Shaders/FleshRingTestCS.usf
//
// SF_Compute: 이 셰이더가 Compute Shader임을 지정
IMPLEMENT_GLOBAL_SHADER(
    FFleshRingTestCS,                           // C++ 클래스명
    "/FleshRingPlugin/FleshRingTestCS.usf",     // .usf 파일 가상 경로
    "MainCS",                                   // .usf 내 진입점 함수명
    SF_Compute                                  // 셰이더 타입
);

// ============================================================================
// Dispatch 함수 (외부에서 호출용)
// ============================================================================
// FRDGBuilder: Render Dependency Graph 빌더
// - GPU 리소스 생성, Pass 추가, 의존성 관리를 담당
//
// Count: 처리할 데이터 개수
void DispatchFleshRingTestCS(FRDGBuilder& GraphBuilder, uint32 Count)
{
    // 1. 파라미터 구조체 할당
    // GraphBuilder가 메모리 관리 (수동 해제 불필요)
    FFleshRingTestCS::FParameters* Parameters =
        GraphBuilder.AllocParameters<FFleshRingTestCS::FParameters>();

    // 2. GPU 버퍼 생성
    // CreateStructuredDesc: 구조화 버퍼 설명자 생성
    // - sizeof(float): 각 요소의 크기 (4바이트)
    // - Count: 요소 개수
    FRDGBufferRef TestBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Count),
        TEXT("FleshRingTestBuffer")  // 디버깅용 이름 (RenderDoc에서 보임)
    );

    // 3. UAV (쓰기 가능 뷰) 생성 및 파라미터에 바인딩
    Parameters->TestBuffer = GraphBuilder.CreateUAV(TestBuffer);
    Parameters->BufferSize = Count;

    // 4. 셰이더 가져오기
    // TShaderMapRef: 컴파일된 셰이더에 대한 참조
    // GetGlobalShaderMap: 전역 셰이더 맵에서 가져옴
    // GMaxRHIFeatureLevel: 현재 RHI의 최대 기능 레벨
    TShaderMapRef<FFleshRingTestCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // 5. Compute Shader Pass 추가
    // FComputeShaderUtils::AddPass: RDG에 CS 실행 Pass 추가
    //
    // 파라미터:
    // - GraphBuilder: RDG 빌더
    // - RDG_EVENT_NAME: 디버깅용 이벤트 이름 (RenderDoc에서 보임)
    // - ComputeShader: 실행할 셰이더
    // - Parameters: 셰이더에 전달할 파라미터
    // - FIntVector: Dispatch 그룹 수 (X, Y, Z)
    //
    // 그룹 수 계산:
    // - 전체 데이터: Count개
    // - 그룹당 스레드: 64개 ([numthreads(64,1,1)])
    // - 필요한 그룹: ceil(Count / 64)
    // - DivideAndRoundUp: 올림 나눗셈
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("FleshRingTestCS"),
        ComputeShader,
        Parameters,
        FIntVector(
            FMath::DivideAndRoundUp(Count, 64u),  // X 방향 그룹 수
            1,                                     // Y 방향 그룹 수
            1                                      // Z 방향 그룹 수
        )
    );

    // 참고: 실제 GPU 실행은 GraphBuilder.Execute() 호출 시 일어남
    // 이 함수는 Pass를 "예약"만 하는 것
}

// ============================================================================
// 검증용 Dispatch 함수 (Readback 포함)
// ============================================================================
// GPU에서 계산된 결과를 CPU로 읽어와서 검증
// Readback 객체는 호출자가 소유권을 가짐
void DispatchFleshRingTestCS_WithReadback(
    FRDGBuilder& GraphBuilder,
    uint32 Count,
    FRHIGPUBufferReadback* Readback)
{
    // 1. 파라미터 구조체 할당
    FFleshRingTestCS::FParameters* Parameters =
        GraphBuilder.AllocParameters<FFleshRingTestCS::FParameters>();

    // 2. GPU 버퍼 생성
    FRDGBufferRef TestBuffer = GraphBuilder.CreateBuffer(
        FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Count),
        TEXT("FleshRingTestBuffer")
    );

    // 3. UAV 생성 및 파라미터 바인딩
    Parameters->TestBuffer = GraphBuilder.CreateUAV(TestBuffer);
    Parameters->BufferSize = Count;

    // 4. 셰이더 가져오기
    TShaderMapRef<FFleshRingTestCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // 5. Compute Shader Pass 추가
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("FleshRingTestCS"),
        ComputeShader,
        Parameters,
        FIntVector(FMath::DivideAndRoundUp(Count, 64u), 1, 1)
    );

    // 6. Readback Pass 추가
    // GPU 버퍼 -> CPU 메모리로 복사 예약
    // AddEnqueueCopyPass: RDG 버퍼를 Readback 객체로 복사
    AddEnqueueCopyPass(GraphBuilder, Readback, TestBuffer, 0);
}

// ============================================================================
// 결과 검증 함수
// ============================================================================
// GPU에서 계산된 값이 예상값(ThreadId * 2.0f)과 일치하는지 확인
void ValidateTestCSResults(const float* Data, uint32 Count)
{
    uint32 PassCount = 0;
    uint32 FailCount = 0;
    constexpr uint32 MaxErrorsToLog = 10;  // 너무 많은 에러 로그 방지

    for (uint32 i = 0; i < Count; ++i)
    {
        // 예상값: ThreadId * 2.0f (FleshRingTestCS.usf 참조)
        const float Expected = static_cast<float>(i) * 2.0f;
        const float Actual = Data[i];

        // 부동소수점 비교 (오차 허용)
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

    // 결과 요약 로그
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
// 콘솔 명령어 등록
// ============================================================================
// FAutoConsoleCommand: 정적 객체로 생성하면 모듈 로드 시 자동으로 콘솔 명령어 등록
//
// 사용법: 에디터 콘솔(~)에서 "FleshRing.TestCS" 입력
static FAutoConsoleCommand GFleshRingTestCSCommand(
    TEXT("FleshRing.TestCS"),
    TEXT("FleshRing Compute Shader 테스트 실행 및 결과 검증"),
    FConsoleCommandDelegate::CreateLambda([]()
    {
        const uint32 TestCount = 1024;

        UE_LOG(LogTemp, Log, TEXT("========================================="));
        UE_LOG(LogTemp, Log, TEXT("FleshRing.TestCS: Starting compute shader test"));
        UE_LOG(LogTemp, Log, TEXT("FleshRing.TestCS: Element count: %d"), TestCount);
        UE_LOG(LogTemp, Log, TEXT("========================================="));

        // Readback 객체 생성 (shared_ptr로 람다 간 공유)
        // GPU -> CPU 데이터 전송을 위한 객체
        TSharedPtr<FRHIGPUBufferReadback> Readback =
            MakeShared<FRHIGPUBufferReadback>(TEXT("FleshRingTestReadback"));

        // 1단계: 렌더링 스레드에서 CS 실행 및 Readback 예약
        ENQUEUE_RENDER_COMMAND(FleshRingTestCS_Dispatch)(
            [TestCount, Readback](FRHICommandListImmediate& RHICmdList)
            {
                FRDGBuilder GraphBuilder(RHICmdList);
                DispatchFleshRingTestCS_WithReadback(GraphBuilder, TestCount, Readback.Get());
                GraphBuilder.Execute();

                UE_LOG(LogTemp, Log, TEXT("FleshRing.TestCS: Dispatch and readback enqueued"));
            });

        // 2단계: 결과 검증 (Readback 완료 대기 후)
        // 별도의 렌더 커맨드로 분리하여 이전 커맨드 완료 보장
        ENQUEUE_RENDER_COMMAND(FleshRingTestCS_Validate)(
            [TestCount, Readback](FRHICommandListImmediate& RHICmdList)
            {
                // Readback 완료 대기
                // IsReady()가 true가 될 때까지 대기
                // 일반적으로 이전 커맨드 실행 후이므로 바로 ready 상태
                if (!Readback->IsReady())
                {
                    // 만약 아직 ready가 아니면, RHI flush로 강제 동기화
                    RHICmdList.BlockUntilGPUIdle();
                }

                if (Readback->IsReady())
                {
                    // GPU 메모리 -> CPU 메모리 매핑
                    const float* ResultData = static_cast<const float*>(
                        Readback->Lock(TestCount * sizeof(float)));

                    if (ResultData)
                    {
                        // 결과 검증
                        ValidateTestCSResults(ResultData, TestCount);

                        // 매핑 해제
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
