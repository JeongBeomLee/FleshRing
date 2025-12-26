// FleshRingMeshExtractor.h
// UStaticMesh에서 버텍스/인덱스/노말 데이터를 추출하는 유틸리티
#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "FleshRingMeshExtractor.generated.h"

// GPU로 전달할 삼각형 데이터 구조체
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FFleshRingMeshData
{
    GENERATED_BODY()

    // 버텍스 위치 배열
    TArray<FVector3f> Vertices;

    // 삼각형 인덱스 (3개씩 = 1개 삼각형)
    TArray<uint32> Indices;

    // 삼각형별 노말 (Indices.Num() / 3 개)
    TArray<FVector3f> TriangleNormals;

    // 메시 바운딩 박스
    FBox3f Bounds;

    // 삼각형 개수
    int32 GetTriangleCount() const { return Indices.Num() / 3; }

    // 버텍스 개수
    int32 GetVertexCount() const { return Vertices.Num(); }

    // 유효성 검사
    bool IsValid() const
    {
        return Vertices.Num() > 0 &&
               Indices.Num() > 0 &&
               Indices.Num() % 3 == 0 &&
               TriangleNormals.Num() == Indices.Num() / 3;
    }

    // 데이터 클리어
    void Reset()
    {
        Vertices.Empty();
        Indices.Empty();
        TriangleNormals.Empty();
        Bounds = FBox3f(ForceInit);
    }
};

// 메시 추출 유틸리티 클래스
UCLASS()
class FLESHRINGRUNTIME_API UFleshRingMeshExtractor : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // UStaticMesh에서 데이터 추출 (LOD 0 사용)
    // @param Mesh - 추출할 스태틱 메시
    // @param OutMeshData - 추출된 데이터가 저장될 구조체
    // @return 성공 여부
    UFUNCTION(BlueprintCallable, Category = "FleshRing|Mesh")
    static bool ExtractMeshData(UStaticMesh* Mesh, FFleshRingMeshData& OutMeshData);

    // 특정 LOD에서 데이터 추출
    // @param Mesh - 추출할 스태틱 메시
    // @param LODIndex - 사용할 LOD 인덱스
    // @param OutMeshData - 추출된 데이터가 저장될 구조체
    // @return 성공 여부
    UFUNCTION(BlueprintCallable, Category = "FleshRing|Mesh")
    static bool ExtractMeshDataFromLOD(UStaticMesh* Mesh, int32 LODIndex, FFleshRingMeshData& OutMeshData);

    // 추출된 데이터 디버그 출력
    UFUNCTION(BlueprintCallable, Category = "FleshRing|Mesh")
    static void DebugPrintMeshData(const FFleshRingMeshData& MeshData);
};
