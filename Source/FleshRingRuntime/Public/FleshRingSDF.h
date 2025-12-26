// FleshRingSDF.h
#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphUtils.h"

// 메시 SDF 생성 Compute Shader
// Point-to-Triangle 거리 계산으로 SDF 생성
class FMeshSDFGenerateCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FMeshSDFGenerateCS)
    SHADER_USE_PARAMETER_STRUCT(FMeshSDFGenerateCS, FGlobalShader)

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // 메시 데이터
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, MeshVertices)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FIntVector>, MeshIndices)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, TriangleNormals)
        SHADER_PARAMETER(uint32, TriangleCount)
        // SDF 파라미터
        SHADER_PARAMETER(FVector3f, SDFBoundsMin)
        SHADER_PARAMETER(FVector3f, SDFBoundsMax)
        SHADER_PARAMETER(FIntVector, SDFResolution)
        // 출력
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, OutputSDF)
    END_SHADER_PARAMETER_STRUCT()
};

// SDF 슬라이스 시각화 Compute Shader
// 3D SDF에서 Z 슬라이스 추출 + 색상 매핑
class FSDFSliceVisualizeCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FSDFSliceVisualizeCS)
    SHADER_USE_PARAMETER_STRUCT(FSDFSliceVisualizeCS, FGlobalShader)

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // 입력 SDF 텍스처
        SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, SDFTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SDFSampler)
        // 출력 2D 텍스처
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputSlice)
        // 파라미터
        SHADER_PARAMETER(FIntVector, SDFResolution)
        SHADER_PARAMETER(int32, SliceZ)
        SHADER_PARAMETER(float, MaxDisplayDist)
    END_SHADER_PARAMETER_STRUCT()
};

// 메시 SDF 생성 함수
// MeshData의 삼각형들로부터 SDF를 생성
// 도넛홀 보정은 Apply2DSliceFloodFill로 별도 수행
void GenerateMeshSDF(
    FRDGBuilder& GraphBuilder,
    FRDGTextureRef OutputTexture,
    const TArray<FVector3f>& Vertices,
    const TArray<uint32>& Indices,
    const TArray<FVector3f>& TriangleNormals,
    FVector3f BoundsMin,
    FVector3f BoundsMax,
    FIntVector Resolution);

// SDF 슬라이스 시각화 함수 (디버그용)
void GenerateSDFSlice(
    FRDGBuilder& GraphBuilder,
    FRDGTextureRef SDFTexture,
    FRDGTextureRef OutputSlice,
    FIntVector SDFResolution,
    int32 SliceZ,
    float MaxDisplayDist);

// 2D Slice Flood Fill - 도넛홀 보정
// 각 Z 슬라이스에서 XY 경계부터 flood하여 도넛홀 감지

// 2D Flood 초기화 셰이더
class F2DFloodInitializeCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(F2DFloodInitializeCS)
    SHADER_USE_PARAMETER_STRUCT(F2DFloodInitializeCS, FGlobalShader)

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, InputSDF)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<uint>, FloodMask)
        SHADER_PARAMETER(FIntVector, GridResolution)
    END_SHADER_PARAMETER_STRUCT()
};

// 2D Flood 전파 패스 셰이더
class F2DFloodPassCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(F2DFloodPassCS)
    SHADER_USE_PARAMETER_STRUCT(F2DFloodPassCS, FGlobalShader)

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<uint>, FloodMaskInput)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<uint>, FloodMaskOutput)
        SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, SDFForFlood)
        SHADER_PARAMETER(FIntVector, GridResolution)
    END_SHADER_PARAMETER_STRUCT()
};

// 2D Flood 최종화 셰이더
class F2DFloodFinalizeCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(F2DFloodFinalizeCS)
    SHADER_USE_PARAMETER_STRUCT(F2DFloodFinalizeCS, FGlobalShader)

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<uint>, FinalFloodMask)
        SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, OriginalSDF)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, OutputSDF)
        SHADER_PARAMETER(FIntVector, GridResolution)
    END_SHADER_PARAMETER_STRUCT()
};

// 2D Slice Flood Fill 적용 함수
// 도넛홀(XY 경계에서 도달 불가능한 외부 영역)을 내부로 변환
void Apply2DSliceFloodFill(
    FRDGBuilder& GraphBuilder,
    FRDGTextureRef InputSDF,
    FRDGTextureRef OutputSDF,
    FIntVector Resolution);