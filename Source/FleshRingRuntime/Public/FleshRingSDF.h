// Copyright 2026 LgThx. All Rights Reserved.

// FleshRingSDF.h
#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphUtils.h"

// Mesh SDF Generation Compute Shader
// Generates SDF using Point-to-Triangle distance calculation
class FMeshSDFGenerateCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FMeshSDFGenerateCS)
    SHADER_USE_PARAMETER_STRUCT(FMeshSDFGenerateCS, FGlobalShader)

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // Mesh data
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, MeshVertices)
        SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FIntVector>, MeshIndices)
        SHADER_PARAMETER(uint32, TriangleCount)
        // SDF parameters
        SHADER_PARAMETER(FVector3f, SDFBoundsMin)
        SHADER_PARAMETER(FVector3f, SDFBoundsMax)
        SHADER_PARAMETER(FIntVector, SDFResolution)
        // Output
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<float>, OutputSDF)
    END_SHADER_PARAMETER_STRUCT()
};

// SDF Slice Visualization Compute Shader
// Extracts Z slice from 3D SDF + color mapping
class FSDFSliceVisualizeCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FSDFSliceVisualizeCS)
    SHADER_USE_PARAMETER_STRUCT(FSDFSliceVisualizeCS, FGlobalShader)

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        // Input SDF texture
        SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, SDFTexture)
        SHADER_PARAMETER_SAMPLER(SamplerState, SDFSampler)
        // Output 2D texture
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputSlice)
        // Parameters
        SHADER_PARAMETER(FIntVector, SDFResolution)
        SHADER_PARAMETER(int32, SliceZ)
        SHADER_PARAMETER(float, MaxDisplayDist)
    END_SHADER_PARAMETER_STRUCT()
};

// Mesh SDF generation function
// Generates SDF from MeshData triangles
// Donut hole correction is performed separately via Apply2DSliceFloodFill
void GenerateMeshSDF(
    FRDGBuilder& GraphBuilder,
    FRDGTextureRef OutputTexture,
    const TArray<FVector3f>& Vertices,
    const TArray<uint32>& Indices,
    FVector3f BoundsMin,
    FVector3f BoundsMax,
    FIntVector Resolution);

// SDF slice visualization function (for debugging)
void GenerateSDFSlice(
    FRDGBuilder& GraphBuilder,
    FRDGTextureRef SDFTexture,
    FRDGTextureRef OutputSlice,
    FIntVector SDFResolution,
    int32 SliceZ,
    float MaxDisplayDist);

// 2D Slice Flood Fill - Donut hole correction
// Floods from XY boundary in each Z slice to detect donut holes

// 2D Flood initialization shader
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

// 2D Flood propagation pass shader
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

// Z-axis voting shader - Propagates donut hole determination along Z-axis
// If majority at each XY coordinate is "inside", sets all Z values to "inside"
class FZAxisVoteCS : public FGlobalShader
{
public:
    DECLARE_GLOBAL_SHADER(FZAxisVoteCS)
    SHADER_USE_PARAMETER_STRUCT(FZAxisVoteCS, FGlobalShader)

    BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
        SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<uint>, VoteMaskInput)
        SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture3D<uint>, VoteMaskOutput)
        SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture3D<float>, SDFForVote)
        SHADER_PARAMETER(FIntVector, GridResolution)
    END_SHADER_PARAMETER_STRUCT()
};

// 2D Flood finalization shader
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

// 2D Slice Flood Fill application function
// Converts donut holes (exterior regions unreachable from XY boundary) to interior
void Apply2DSliceFloodFill(
    FRDGBuilder& GraphBuilder,
    FRDGTextureRef InputSDF,
    FRDGTextureRef OutputSDF,
    FIntVector Resolution);