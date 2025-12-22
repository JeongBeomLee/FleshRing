// FleshRingSDF.h
#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphUtils.h"

// 간단한 구체 SDF 생성 Compute Shader
class FSimpleSphereSDF : public FGlobalShader
{
public:
    // 필수 매크로: 셰이더 타입 선언
    DECLARE_GLOBAL_SHADER(FSimpleSphereSDF)

    // 필수 매크로: 파라미터 구조체 사용
    SHADER_USE_PARAMETER_STRUCT(FSimpleSphereSDF, FGlobalShader)

    // 셰이더 파라미터 정의 (CPU → GPU로 전달할 데이터)
    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER(FVector3f, SphereCenter)
        SHADER_PARAMETER(float, SphereRadius)
        SHADER_PARAMETER(FIntVector, GridResolution)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, OutputSDF)
    END_SHADER_PARAMETER_STRUCT()
};

// 구체 SDF 생성 함수 선언
void GenerateSphereSDF(
    FRDGBuilder& GraphBuilder,
    FRDGTextureRef OutputTexture,
    FVector3f SphereCenter,
    float SphereRadius,
    FIntVector GridResolution);
