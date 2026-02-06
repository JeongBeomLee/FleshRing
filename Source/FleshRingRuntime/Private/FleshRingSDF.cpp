// Copyright 2026 LgThx. All Rights Reserved.

// FleshRingSDF.cpp
#include "FleshRingSDF.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "RHIStaticStates.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingSDF, Log, All);

// Register mesh SDF generation shader
IMPLEMENT_GLOBAL_SHADER(
    FMeshSDFGenerateCS,
    "/Plugin/FleshRingPlugin/FleshRingSDFGenerate.usf",
    "MainCS",
    SF_Compute
);

// Register SDF slice visualization shader
IMPLEMENT_GLOBAL_SHADER(
    FSDFSliceVisualizeCS,
    "/Plugin/FleshRingPlugin/SDFSliceVisualize.usf",
    "MainCS",
    SF_Compute
);

// Register 2D Slice Flood Fill shader
IMPLEMENT_GLOBAL_SHADER(
    F2DFloodInitializeCS,
    "/Plugin/FleshRingPlugin/FleshRing2DSliceFlood.usf",
    "Initialize2DFloodCS",
    SF_Compute
);

IMPLEMENT_GLOBAL_SHADER(
    F2DFloodPassCS,
    "/Plugin/FleshRingPlugin/FleshRing2DSliceFlood.usf",
    "Flood2DPassCS",
    SF_Compute
);

IMPLEMENT_GLOBAL_SHADER(
    FZAxisVoteCS,
    "/Plugin/FleshRingPlugin/FleshRing2DSliceFlood.usf",
    "ZAxisVoteCS",
    SF_Compute
);

IMPLEMENT_GLOBAL_SHADER(
    F2DFloodFinalizeCS,
    "/Plugin/FleshRingPlugin/FleshRing2DSliceFlood.usf",
    "Finalize2DFloodCS",
    SF_Compute
);

void GenerateMeshSDF(
    FRDGBuilder& GraphBuilder,
    FRDGTextureRef OutputTexture,
    const TArray<FVector3f>& Vertices,
    const TArray<uint32>& Indices,
    FVector3f BoundsMin,
    FVector3f BoundsMax,
    FIntVector Resolution)
{
    const int32 VertexCount = Vertices.Num();
    const int32 TriangleCount = Indices.Num() / 3;

    if (VertexCount == 0 || TriangleCount == 0)
    {
        UE_LOG(LogFleshRingSDF, Error, TEXT("GenerateMeshSDF: Empty mesh data"));
        return;
    }

    // 1. Create and upload vertex buffer
    FRDGBufferDesc VertexBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), VertexCount);
    FRDGBufferRef VertexBuffer = GraphBuilder.CreateBuffer(VertexBufferDesc, TEXT("MeshSDFVertices"));
    GraphBuilder.QueueBufferUpload(VertexBuffer, Vertices.GetData(), VertexCount * sizeof(FVector3f));

    // 2. Create and upload index buffer (uint3 = 3 * uint32 per triangle)
    TArray<FIntVector> PackedIndices;
    PackedIndices.SetNum(TriangleCount);
    for (int32 i = 0; i < TriangleCount; i++)
    {
        PackedIndices[i] = FIntVector(
            Indices[i * 3 + 0],
            Indices[i * 3 + 1],
            Indices[i * 3 + 2]
        );
    }

    FRDGBufferDesc IndexBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FIntVector), TriangleCount);
    FRDGBufferRef IndexBuffer = GraphBuilder.CreateBuffer(IndexBufferDesc, TEXT("MeshSDFIndices"));
    GraphBuilder.QueueBufferUpload(IndexBuffer, PackedIndices.GetData(), TriangleCount * sizeof(FIntVector));

    // 3. Get shader
    TShaderMapRef<FMeshSDFGenerateCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // 4. Set parameters
    FMeshSDFGenerateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FMeshSDFGenerateCS::FParameters>();
    Parameters->MeshVertices = GraphBuilder.CreateSRV(VertexBuffer);
    Parameters->MeshIndices = GraphBuilder.CreateSRV(IndexBuffer);
    Parameters->TriangleCount = TriangleCount;
    Parameters->SDFBoundsMin = BoundsMin;
    Parameters->SDFBoundsMax = BoundsMax;
    Parameters->SDFResolution = Resolution;
    Parameters->OutputSDF = GraphBuilder.CreateUAV(OutputTexture);

    // 6. Calculate thread groups (8x8x8 per group)
    FIntVector GroupCount(
        FMath::DivideAndRoundUp(Resolution.X, 8),
        FMath::DivideAndRoundUp(Resolution.Y, 8),
        FMath::DivideAndRoundUp(Resolution.Z, 8)
    );

    // 7. Dispatch Compute Shader
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("MeshSDFGenerate (Triangles=%d, Resolution=%dx%dx%d)", TriangleCount, Resolution.X, Resolution.Y, Resolution.Z),
        ComputeShader,
        Parameters,
        GroupCount
    );

}

void GenerateSDFSlice(
    FRDGBuilder& GraphBuilder,
    FRDGTextureRef SDFTexture,
    FRDGTextureRef OutputSlice,
    FIntVector SDFResolution,
    int32 SliceZ,
    float MaxDisplayDist)
{
    // Get shader
    TShaderMapRef<FSDFSliceVisualizeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // Set parameters
    FSDFSliceVisualizeCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSDFSliceVisualizeCS::FParameters>();
    Parameters->SDFTexture = GraphBuilder.CreateSRV(SDFTexture);
    Parameters->SDFSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
    Parameters->OutputSlice = GraphBuilder.CreateUAV(OutputSlice);
    Parameters->SDFResolution = SDFResolution;
    Parameters->SliceZ = SliceZ;
    Parameters->MaxDisplayDist = MaxDisplayDist;

    // Calculate thread groups (8x8 per group, Z=1)
    FIntVector GroupCount(
        FMath::DivideAndRoundUp(SDFResolution.X, 8),
        FMath::DivideAndRoundUp(SDFResolution.Y, 8),
        1
    );

    // Dispatch Compute Shader
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("SDFSliceVisualize (Z=%d)", SliceZ),
        ComputeShader,
        Parameters,
        GroupCount
    );
}

void Apply2DSliceFloodFill(
    FRDGBuilder& GraphBuilder,
    FRDGTextureRef InputSDF,
    FRDGTextureRef OutputSDF,
    FIntVector Resolution)
{
    // Calculate thread groups (8x8x8 per group)
    FIntVector GroupCount(
        FMath::DivideAndRoundUp(Resolution.X, 8),
        FMath::DivideAndRoundUp(Resolution.Y, 8),
        FMath::DivideAndRoundUp(Resolution.Z, 8)
    );

    // Create 2 flood mask textures (ping-pong buffers)
    FRDGTextureDesc MaskDesc = FRDGTextureDesc::Create3D(
        Resolution,
        PF_R32_UINT,
        FClearValueBinding::Black,
        TexCreate_ShaderResource | TexCreate_UAV
    );
    FRDGTextureRef FloodMaskA = GraphBuilder.CreateTexture(MaskDesc, TEXT("2DFloodMaskA"));
    FRDGTextureRef FloodMaskB = GraphBuilder.CreateTexture(MaskDesc, TEXT("2DFloodMaskB"));

    // Pass 1: Initialize - mark XY boundaries as exterior seeds
    {
        TShaderMapRef<F2DFloodInitializeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        F2DFloodInitializeCS::FParameters* Parameters = GraphBuilder.AllocParameters<F2DFloodInitializeCS::FParameters>();
        Parameters->InputSDF = GraphBuilder.CreateSRV(InputSDF);
        Parameters->FloodMask = GraphBuilder.CreateUAV(FloodMaskA);
        Parameters->GridResolution = Resolution;

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("2DFlood Initialize"),
            ComputeShader,
            Parameters,
            GroupCount
        );
    }

    // Pass 2-N: 2D Flood propagation (iterate up to max resolution times)
    // In 2D, propagate only 4 directions without diagonals, so iterate max(X,Y) times
    TShaderMapRef<F2DFloodPassCS> FloodPassShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    int32 MaxIterations = FMath::Max(Resolution.X, Resolution.Y);
    FRDGTextureRef CurrentInput = FloodMaskA;
    FRDGTextureRef CurrentOutput = FloodMaskB;

    for (int32 Iter = 0; Iter < MaxIterations; Iter++)
    {
        F2DFloodPassCS::FParameters* Parameters = GraphBuilder.AllocParameters<F2DFloodPassCS::FParameters>();
        Parameters->FloodMaskInput = GraphBuilder.CreateSRV(CurrentInput);
        Parameters->FloodMaskOutput = GraphBuilder.CreateUAV(CurrentOutput);
        Parameters->SDFForFlood = GraphBuilder.CreateSRV(InputSDF);
        Parameters->GridResolution = Resolution;

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("2DFlood Pass %d", Iter),
            FloodPassShader,
            Parameters,
            GroupCount
        );

        // Ping-pong swap
        Swap(CurrentInput, CurrentOutput);
    }

    // Final result is in CurrentInput (after swap)
    FRDGTextureRef FloodResult = CurrentInput;
    FRDGTextureRef VoteOutput = CurrentOutput;  // Reuse ping-pong buffer

    // Pass Z-Vote: Propagate donut hole via Z-axis voting
    // If majority at each XY coordinate is "interior", set all Z to "interior"
    {
        TShaderMapRef<FZAxisVoteCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        FZAxisVoteCS::FParameters* Parameters = GraphBuilder.AllocParameters<FZAxisVoteCS::FParameters>();
        Parameters->VoteMaskInput = GraphBuilder.CreateSRV(FloodResult);
        Parameters->VoteMaskOutput = GraphBuilder.CreateUAV(VoteOutput);
        Parameters->SDFForVote = GraphBuilder.CreateSRV(InputSDF);
        Parameters->GridResolution = Resolution;

        // Dispatch XY only (Z is iterated inside shader)
        FIntVector VoteGroupCount(
            FMath::DivideAndRoundUp(Resolution.X, 8),
            FMath::DivideAndRoundUp(Resolution.Y, 8),
            1  // Z is 1
        );

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("ZAxisVote"),
            ComputeShader,
            Parameters,
            VoteGroupCount
        );
    }

    // Use Z-axis vote result as final mask
    FRDGTextureRef FinalMask = VoteOutput;

    // Pass Final: Invert donut hole sign
    {
        TShaderMapRef<F2DFloodFinalizeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
        F2DFloodFinalizeCS::FParameters* Parameters = GraphBuilder.AllocParameters<F2DFloodFinalizeCS::FParameters>();
        Parameters->FinalFloodMask = GraphBuilder.CreateSRV(FinalMask);
        Parameters->OriginalSDF = GraphBuilder.CreateSRV(InputSDF);
        Parameters->OutputSDF = GraphBuilder.CreateUAV(OutputSDF);
        Parameters->GridResolution = Resolution;

        FComputeShaderUtils::AddPass(
            GraphBuilder,
            RDG_EVENT_NAME("2DFlood Finalize"),
            ComputeShader,
            Parameters,
            GroupCount
        );
    }

}
