// FleshRingSDF.cpp
#include "FleshRingSDF.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"

// 셰이더를 엔진에 등록
IMPLEMENT_GLOBAL_SHADER(
    FSimpleSphereSDF,
    "/Plugin/FleshRingPlugin/SimpleSphereSDF.usf",
    "MainCS",
    SF_Compute
);

void GenerateSphereSDF(
    FRDGBuilder& GraphBuilder,
    FRDGTextureRef OutputTexture,
    FVector3f SphereCenter,
    float SphereRadius,
    FIntVector GridResolution)
{
    // 셰이더 가져오기
    TShaderMapRef<FSimpleSphereSDF> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // 파라미터 설정
    FSimpleSphereSDF::FParameters* Parameters = GraphBuilder.AllocParameters<FSimpleSphereSDF::FParameters>();
    Parameters->SphereCenter = SphereCenter;
    Parameters->SphereRadius = SphereRadius;
    Parameters->GridResolution = GridResolution;
    Parameters->OutputSDF = GraphBuilder.CreateUAV(OutputTexture);

    // 스레드 그룹 계산
    FIntVector GroupCount(
        FMath::DivideAndRoundUp(GridResolution.X, 8),
        FMath::DivideAndRoundUp(GridResolution.Y, 8),
        FMath::DivideAndRoundUp(GridResolution.Z, 8)
    );

    // Compute Shader 디스패치 등록
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("SimpleSphereSDF"),
        ComputeShader,
        Parameters,
        GroupCount
    );
}
