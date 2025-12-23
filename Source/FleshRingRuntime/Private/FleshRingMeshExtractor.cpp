// FleshRingMeshExtractor.cpp
#include "FleshRingMeshExtractor.h"
#include "StaticMeshResources.h"
#include "Rendering/PositionVertexBuffer.h"
#include "RawIndexBuffer.h"

bool UFleshRingMeshExtractor::ExtractMeshData(UStaticMesh* Mesh, FFleshRingMeshData& OutMeshData)
{
    return ExtractMeshDataFromLOD(Mesh, 0, OutMeshData);
}

bool UFleshRingMeshExtractor::ExtractMeshDataFromLOD(UStaticMesh* Mesh, int32 LODIndex, FFleshRingMeshData& OutMeshData)
{
    OutMeshData.Reset();

    // 1. 유효성 검사
    if (!Mesh)
    {
        UE_LOG(LogTemp, Error, TEXT("ExtractMeshData: Mesh is null"));
        return false;
    }

    // 렌더 데이터 확인
    FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
    if (!RenderData)
    {
        UE_LOG(LogTemp, Error, TEXT("ExtractMeshData: RenderData is null"));
        return false;
    }

    // LOD 유효성 검사
    if (LODIndex < 0 || LODIndex >= RenderData->LODResources.Num())
    {
        UE_LOG(LogTemp, Error, TEXT("ExtractMeshData: Invalid LOD index %d (max: %d)"),
            LODIndex, RenderData->LODResources.Num() - 1);
        return false;
    }

    // 2. LOD 리소스 가져오기
    const FStaticMeshLODResources& LODResource = RenderData->LODResources[LODIndex];

    // 3. 버텍스 추출
    const FPositionVertexBuffer& PositionBuffer = LODResource.VertexBuffers.PositionVertexBuffer;
    const int32 VertexCount = PositionBuffer.GetNumVertices();

    if (VertexCount == 0)
    {
        UE_LOG(LogTemp, Error, TEXT("ExtractMeshData: No vertices found"));
        return false;
    }

    OutMeshData.Vertices.SetNum(VertexCount);

    // 바운딩 박스 초기화
    FVector3f MinBounds(FLT_MAX, FLT_MAX, FLT_MAX);
    FVector3f MaxBounds(-FLT_MAX, -FLT_MAX, -FLT_MAX);

    for (int32 i = 0; i < VertexCount; i++)
    {
        // UE5에서는 VertexPosition이 FVector3f를 반환
        const FVector3f& Position = PositionBuffer.VertexPosition(i);
        OutMeshData.Vertices[i] = Position;

        // 바운딩 박스 업데이트
        MinBounds = FVector3f::Min(MinBounds, Position);
        MaxBounds = FVector3f::Max(MaxBounds, Position);
    }

    OutMeshData.Bounds = FBox3f(MinBounds, MaxBounds);

    // 4. 인덱스 추출
    const FRawStaticIndexBuffer& IndexBuffer = LODResource.IndexBuffer;

    // 인덱스 버퍼 접근 (16비트 또는 32비트)
    TArray<uint32> AllIndices;

    if (IndexBuffer.Is32Bit())
    {
        // 32비트 인덱스
        const int32 NumIndices = IndexBuffer.GetNumIndices();
        AllIndices.SetNum(NumIndices);

        // 인덱스 버퍼에서 직접 복사
        // GetCopy를 사용하여 CPU 접근 가능한 복사본 생성
        IndexBuffer.GetCopy(AllIndices);
    }
    else
    {
        // 16비트 인덱스 → 32비트로 변환
        const int32 NumIndices = IndexBuffer.GetNumIndices();
        AllIndices.SetNum(NumIndices);

        TArray<uint16> Indices16;
        Indices16.SetNum(NumIndices);

        // 16비트 버전으로 복사
        TArray<uint32> TempIndices;
        IndexBuffer.GetCopy(TempIndices);

        for (int32 i = 0; i < NumIndices; i++)
        {
            AllIndices[i] = TempIndices[i];
        }
    }

    // 섹션별로 인덱스 처리 (여러 머티리얼 슬롯 지원)
    // 일단은 모든 섹션의 삼각형을 합침
    OutMeshData.Indices = MoveTemp(AllIndices);

    if (OutMeshData.Indices.Num() == 0 || OutMeshData.Indices.Num() % 3 != 0)
    {
        UE_LOG(LogTemp, Error, TEXT("ExtractMeshData: Invalid index count %d (must be multiple of 3)"),
            OutMeshData.Indices.Num());
        return false;
    }

    // 5. 삼각형 노말 계산
    const int32 TriangleCount = OutMeshData.Indices.Num() / 3;
    OutMeshData.TriangleNormals.SetNum(TriangleCount);

    for (int32 TriIdx = 0; TriIdx < TriangleCount; TriIdx++)
    {
        // 삼각형의 3개 버텍스 인덱스
        uint32 Idx0 = OutMeshData.Indices[TriIdx * 3 + 0];
        uint32 Idx1 = OutMeshData.Indices[TriIdx * 3 + 1];
        uint32 Idx2 = OutMeshData.Indices[TriIdx * 3 + 2];

        // 버텍스 위치
        const FVector3f& V0 = OutMeshData.Vertices[Idx0];
        const FVector3f& V1 = OutMeshData.Vertices[Idx1];
        const FVector3f& V2 = OutMeshData.Vertices[Idx2];

        // 두 변 벡터
        FVector3f Edge1 = V1 - V0;
        FVector3f Edge2 = V2 - V0;

        // 외적으로 노말 계산 (왼손 좌표계: Cross(Edge1, Edge2))
        FVector3f Normal = FVector3f::CrossProduct(Edge1, Edge2);

        // 정규화 (degenerate 삼각형 처리)
        float Length = Normal.Size();
        if (Length > KINDA_SMALL_NUMBER)
        {
            Normal /= Length;
        }
        else
        {
            // Degenerate 삼각형 - 기본 노말 사용
            Normal = FVector3f(0.0f, 0.0f, 1.0f);
            UE_LOG(LogTemp, Warning, TEXT("ExtractMeshData: Degenerate triangle at index %d"), TriIdx);
        }

        OutMeshData.TriangleNormals[TriIdx] = Normal;
    }

    UE_LOG(LogTemp, Log, TEXT("ExtractMeshData: Success! Vertices=%d, Triangles=%d, Bounds=(%s) to (%s)"),
        OutMeshData.GetVertexCount(),
        OutMeshData.GetTriangleCount(),
        *MinBounds.ToString(),
        *MaxBounds.ToString());

    return true;
}

void UFleshRingMeshExtractor::DebugPrintMeshData(const FFleshRingMeshData& MeshData)
{
    UE_LOG(LogTemp, Warning, TEXT("========== Mesh Data Debug =========="));
    UE_LOG(LogTemp, Warning, TEXT("Vertices: %d"), MeshData.GetVertexCount());
    UE_LOG(LogTemp, Warning, TEXT("Triangles: %d"), MeshData.GetTriangleCount());
    UE_LOG(LogTemp, Warning, TEXT("Bounds Min: %s"), *MeshData.Bounds.Min.ToString());
    UE_LOG(LogTemp, Warning, TEXT("Bounds Max: %s"), *MeshData.Bounds.Max.ToString());
    UE_LOG(LogTemp, Warning, TEXT("IsValid: %s"), MeshData.IsValid() ? TEXT("Yes") : TEXT("No"));

    // 처음 몇 개 버텍스 출력
    const int32 MaxVertexPrint = FMath::Min(5, MeshData.GetVertexCount());
    for (int32 i = 0; i < MaxVertexPrint; i++)
    {
        const FVector3f& V = MeshData.Vertices[i];
        UE_LOG(LogTemp, Warning, TEXT("  Vertex[%d]: (%.3f, %.3f, %.3f)"), i, V.X, V.Y, V.Z);
    }
    if (MeshData.GetVertexCount() > MaxVertexPrint)
    {
        UE_LOG(LogTemp, Warning, TEXT("  ... and %d more vertices"), MeshData.GetVertexCount() - MaxVertexPrint);
    }

    // 처음 몇 개 삼각형 출력
    const int32 MaxTrianglePrint = FMath::Min(3, MeshData.GetTriangleCount());
    for (int32 i = 0; i < MaxTrianglePrint; i++)
    {
        uint32 Idx0 = MeshData.Indices[i * 3 + 0];
        uint32 Idx1 = MeshData.Indices[i * 3 + 1];
        uint32 Idx2 = MeshData.Indices[i * 3 + 2];
        const FVector3f& N = MeshData.TriangleNormals[i];

        UE_LOG(LogTemp, Warning, TEXT("  Triangle[%d]: Indices(%d, %d, %d), Normal(%.3f, %.3f, %.3f)"),
            i, Idx0, Idx1, Idx2, N.X, N.Y, N.Z);
    }
    if (MeshData.GetTriangleCount() > MaxTrianglePrint)
    {
        UE_LOG(LogTemp, Warning, TEXT("  ... and %d more triangles"), MeshData.GetTriangleCount() - MaxTrianglePrint);
    }

    UE_LOG(LogTemp, Warning, TEXT("======================================"));
}

void UFleshRingMeshExtractor::TestMeshExtraction(UStaticMesh* TestMesh)
{
    UE_LOG(LogTemp, Warning, TEXT("=== Mesh Extraction Test Start ==="));

    if (!TestMesh)
    {
        UE_LOG(LogTemp, Error, TEXT("TestMesh is null! Please provide a valid StaticMesh."));
        return;
    }

    UE_LOG(LogTemp, Warning, TEXT("Testing mesh: %s"), *TestMesh->GetName());

    FFleshRingMeshData MeshData;
    bool bSuccess = ExtractMeshData(TestMesh, MeshData);

    if (bSuccess)
    {
        UE_LOG(LogTemp, Warning, TEXT("Extraction SUCCESS!"));
        DebugPrintMeshData(MeshData);
    }
    else
    {
        UE_LOG(LogTemp, Error, TEXT("Extraction FAILED!"));
    }

    UE_LOG(LogTemp, Warning, TEXT("=== Mesh Extraction Test End ==="));
}
