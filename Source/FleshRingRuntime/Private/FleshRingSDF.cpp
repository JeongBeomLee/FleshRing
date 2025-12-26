// FleshRingSDF.cpp
#include "FleshRingSDF.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"

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

// 2D Slice Flood Fill 셰이더 등록
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

void Apply2DSliceFloodFill(
    FRDGBuilder& GraphBuilder,
    FRDGTextureRef InputSDF,
    FRDGTextureRef OutputSDF,
    FIntVector Resolution)
{
    // 스레드 그룹 계산 (8x8x8 per group)
    FIntVector GroupCount(
        FMath::DivideAndRoundUp(Resolution.X, 8),
        FMath::DivideAndRoundUp(Resolution.Y, 8),
        FMath::DivideAndRoundUp(Resolution.Z, 8)
    );

    // Flood 마스크 텍스처 2개 생성 (핑퐁 버퍼)
    FRDGTextureDesc MaskDesc = FRDGTextureDesc::Create3D(
        Resolution,
        PF_R32_UINT,
        FClearValueBinding::Black,
        TexCreate_ShaderResource | TexCreate_UAV
    );
    FRDGTextureRef FloodMaskA = GraphBuilder.CreateTexture(MaskDesc, TEXT("2DFloodMaskA"));
    FRDGTextureRef FloodMaskB = GraphBuilder.CreateTexture(MaskDesc, TEXT("2DFloodMaskB"));

    // Pass 1: 초기화 - XY 경계를 외부 시드로 마킹
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

    // Pass 2-N: 2D Flood 전파 (최대 해상도만큼 반복)
    // 2D에서는 대각선 없이 4방향만 전파하므로, max(X,Y) 번 반복
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

        // 핑퐁 스왑
        Swap(CurrentInput, CurrentOutput);
    }

    // 마지막 결과는 CurrentInput에 있음 (스왑 후)
    FRDGTextureRef FinalMask = CurrentInput;

    // Pass Final: 도넛홀 부호 반전
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

    UE_LOG(LogTemp, Log, TEXT("Apply2DSliceFloodFill: Completed %d iterations for Resolution %dx%dx%d"),
        MaxIterations, Resolution.X, Resolution.Y, Resolution.Z);
}
