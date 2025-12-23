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
void GenerateMeshSDF(
    FRDGBuilder& GraphBuilder,
    FRDGTextureRef OutputTexture,
    const TArray<FVector3f>& Vertices,
    const TArray<uint32>& Indices,
    const TArray<FVector3f>& TriangleNormals,
    FVector3f BoundsMin,
    FVector3f BoundsMax,
    FIntVector Resolution);

// SDF 슬라이스 시각화 함수
void GenerateSDFSlice(
    FRDGBuilder& GraphBuilder,
    FRDGTextureRef SDFTexture,
    FRDGTextureRef OutputSlice,
    FIntVector SDFResolution,
    int32 SliceZ,
    float MaxDisplayDist);
