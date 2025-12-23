// FleshRingSDF.cpp
#include "FleshRingSDF.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"

// ============================================
// 셰이더 등록
// ============================================

// 메시 SDF 생성 셰이더 등록
IMPLEMENT_GLOBAL_SHADER(
    FMeshSDFGenerateCS,
    "/Plugin/FleshRingPlugin/FleshRingSDFGenerate.usf",
    "MainCS",
    SF_Compute
);

// SDF 슬라이스 시각화 셰이더 등록
IMPLEMENT_GLOBAL_SHADER(
    FSDFSliceVisualizeCS,
    "/Plugin/FleshRingPlugin/SDFSliceVisualize.usf",
    "MainCS",
    SF_Compute
);

// ============================================
// 함수 구현
// ============================================

void GenerateMeshSDF(
    FRDGBuilder& GraphBuilder,
    FRDGTextureRef OutputTexture,
    const TArray<FVector3f>& Vertices,
    const TArray<uint32>& Indices,
    const TArray<FVector3f>& TriangleNormals,
    FVector3f BoundsMin,
    FVector3f BoundsMax,
    FIntVector Resolution)
{
    const int32 VertexCount = Vertices.Num();
    const int32 TriangleCount = Indices.Num() / 3;

    if (VertexCount == 0 || TriangleCount == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("GenerateMeshSDF: Empty mesh data"));
        return;
    }

    // 1. 버텍스 버퍼 생성 및 업로드
    FRDGBufferDesc VertexBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), VertexCount);
    FRDGBufferRef VertexBuffer = GraphBuilder.CreateBuffer(VertexBufferDesc, TEXT("MeshSDFVertices"));
    GraphBuilder.QueueBufferUpload(VertexBuffer, Vertices.GetData(), VertexCount * sizeof(FVector3f));

    // 2. 인덱스 버퍼 생성 및 업로드 (uint3 = 3 * uint32 per triangle)
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

    // 3. 노말 버퍼 생성 및 업로드
    FRDGBufferDesc NormalBufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), TriangleCount);
    FRDGBufferRef NormalBuffer = GraphBuilder.CreateBuffer(NormalBufferDesc, TEXT("MeshSDFNormals"));
    GraphBuilder.QueueBufferUpload(NormalBuffer, TriangleNormals.GetData(), TriangleCount * sizeof(FVector3f));

    // 4. 셰이더 가져오기
    TShaderMapRef<FMeshSDFGenerateCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // 5. 파라미터 설정
    FMeshSDFGenerateCS::FParameters* Parameters = GraphBuilder.AllocParameters<FMeshSDFGenerateCS::FParameters>();
    Parameters->MeshVertices = GraphBuilder.CreateSRV(VertexBuffer);
    Parameters->MeshIndices = GraphBuilder.CreateSRV(IndexBuffer);
    Parameters->TriangleNormals = GraphBuilder.CreateSRV(NormalBuffer);
    Parameters->TriangleCount = TriangleCount;
    Parameters->SDFBoundsMin = BoundsMin;
    Parameters->SDFBoundsMax = BoundsMax;
    Parameters->SDFResolution = Resolution;
    Parameters->OutputSDF = GraphBuilder.CreateUAV(OutputTexture);

    // 6. 스레드 그룹 계산 (8x8x8 per group)
    FIntVector GroupCount(
        FMath::DivideAndRoundUp(Resolution.X, 8),
        FMath::DivideAndRoundUp(Resolution.Y, 8),
        FMath::DivideAndRoundUp(Resolution.Z, 8)
    );

    // 7. Compute Shader 디스패치
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("MeshSDFGenerate (Triangles=%d, Resolution=%dx%dx%d)", TriangleCount, Resolution.X, Resolution.Y, Resolution.Z),
        ComputeShader,
        Parameters,
        GroupCount
    );

    UE_LOG(LogTemp, Log, TEXT("GenerateMeshSDF: Dispatched CS for %d triangles, Resolution %dx%dx%d"),
        TriangleCount, Resolution.X, Resolution.Y, Resolution.Z);
}

void GenerateSDFSlice(
    FRDGBuilder& GraphBuilder,
    FRDGTextureRef SDFTexture,
    FRDGTextureRef OutputSlice,
    FIntVector SDFResolution,
    int32 SliceZ,
    float MaxDisplayDist)
{
    // 셰이더 가져오기
    TShaderMapRef<FSDFSliceVisualizeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

    // 파라미터 설정
    FSDFSliceVisualizeCS::FParameters* Parameters = GraphBuilder.AllocParameters<FSDFSliceVisualizeCS::FParameters>();
    Parameters->SDFTexture = GraphBuilder.CreateSRV(SDFTexture);
    Parameters->SDFSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
    Parameters->OutputSlice = GraphBuilder.CreateUAV(OutputSlice);
    Parameters->SDFResolution = SDFResolution;
    Parameters->SliceZ = SliceZ;
    Parameters->MaxDisplayDist = MaxDisplayDist;

    // 스레드 그룹 계산 (8x8 per group, Z=1)
    FIntVector GroupCount(
        FMath::DivideAndRoundUp(SDFResolution.X, 8),
        FMath::DivideAndRoundUp(SDFResolution.Y, 8),
        1
    );

    // Compute Shader 디스패치
    FComputeShaderUtils::AddPass(
        GraphBuilder,
        RDG_EVENT_NAME("SDFSliceVisualize (Z=%d)", SliceZ),
        ComputeShader,
        Parameters,
        GroupCount
    );
}
