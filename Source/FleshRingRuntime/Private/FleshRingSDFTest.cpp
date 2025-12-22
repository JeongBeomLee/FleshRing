// FleshRingSDFTest.cpp
#include "FleshRingSDFTest.h"
#include "FleshRingSDF.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderingThread.h"

void UFleshRingSDFTest::TestSphereSDF()
{
    // 테스트 파라미터
    const FIntVector Resolution(64, 64, 64);  // 64³ 해상도
    const FVector3f Center(0.5f, 0.5f, 0.5f); // 중앙
    const float Radius = 0.25f;               // 반지름 0.25

    UE_LOG(LogTemp, Warning, TEXT("=== FleshRing SDF Test Start ==="));
    UE_LOG(LogTemp, Warning, TEXT("Resolution: %d x %d x %d"), Resolution.X, Resolution.Y, Resolution.Z);
    UE_LOG(LogTemp, Warning, TEXT("Sphere Center: (%.2f, %.2f, %.2f)"), Center.X, Center.Y, Center.Z);
    UE_LOG(LogTemp, Warning, TEXT("Sphere Radius: %.2f"), Radius);

    // 렌더 스레드에서 실행
    ENQUEUE_RENDER_COMMAND(TestSphereSDF)(
        [Resolution, Center, Radius](FRHICommandListImmediate& RHICmdList)
        {
            // RDG 빌더 생성
            FRDGBuilder GraphBuilder(RHICmdList);

            // 3D 텍스처 생성
            FRDGTextureDesc Desc = FRDGTextureDesc::Create3D(
                Resolution,
                PF_R32_FLOAT,  // 32비트 float (SDF 값 저장용)
                FClearValueBinding::Black,
                TexCreate_ShaderResource | TexCreate_UAV
            );

            FRDGTextureRef SDFTexture = GraphBuilder.CreateTexture(Desc, TEXT("TestSDFTexture"));

            // CS Dispatch!
            GenerateSphereSDF(
                GraphBuilder,
                SDFTexture,
                Center,
                Radius,
                Resolution
            );

            // 그래프 실행
            GraphBuilder.Execute();

            UE_LOG(LogTemp, Warning, TEXT("=== CS Dispatch Completed! ==="));
            UE_LOG(LogTemp, Warning, TEXT("3D Texture Created and SDF Generated Successfully"));
        }
    );

    UE_LOG(LogTemp, Warning, TEXT("CS Dispatch Command Enqueued (will execute on render thread)"));
}
