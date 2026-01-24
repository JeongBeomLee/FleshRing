// Copyright 2026 LgThx. All Rights Reserved.

// FleshRingSDFVisualizer.h
// SDF 시각화 유틸리티 - 에디터/런타임에서 범용으로 사용
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/TextureRenderTarget2D.h"
#include "FleshRingSDFVisualizer.generated.h"

// SDF 시각화 결과를 담는 구조체
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FSDFVisualizationResult
{
    GENERATED_BODY()

    // 스폰된 평면 액터 (시각화 표시용)
    UPROPERTY(BlueprintReadOnly, Category = "SDF Visualization")
    AActor* PlaneActor = nullptr;

    // 생성된 렌더 타겟 (슬라이스 이미지)
    UPROPERTY(BlueprintReadOnly, Category = "SDF Visualization")
    UTextureRenderTarget2D* SliceTexture = nullptr;

    // SDF 바운딩 박스
    UPROPERTY(BlueprintReadOnly, Category = "SDF Visualization")
    FVector BoundsMin = FVector::ZeroVector;

    UPROPERTY(BlueprintReadOnly, Category = "SDF Visualization")
    FVector BoundsMax = FVector::ZeroVector;

    // 현재 슬라이스 Z 인덱스
    UPROPERTY(BlueprintReadOnly, Category = "SDF Visualization")
    int32 CurrentSliceZ = 0;

    // SDF 해상도
    UPROPERTY(BlueprintReadOnly, Category = "SDF Visualization")
    int32 Resolution = 64;

    bool IsValid() const { return PlaneActor != nullptr && SliceTexture != nullptr; }
};

UCLASS()
class FLESHRINGRUNTIME_API UFleshRingSDFVisualizer : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    // SDF 슬라이스 시각화 - 메시에서 SDF 생성 후 평면에 표시
    // @param WorldContext - 월드 컨텍스트 (액터 스폰용)
    // @param Mesh - SDF를 생성할 스태틱 메시
    // @param WorldLocation - 평면을 배치할 월드 위치
    // @param SliceZ - 표시할 Z 슬라이스 인덱스 (0 ~ Resolution-1)
    // @param Resolution - SDF 해상도 (기본 64)
    // @param PlaneSize - 평면 크기 (기본 100)
    // @return 시각화 결과 (평면 액터, 텍스처 등)
    UFUNCTION(BlueprintCallable, Category = "FleshRing|Visualization", meta = (WorldContext = "WorldContextObject"))
    static FSDFVisualizationResult VisualizeSDFSlice(
        UObject* WorldContextObject,
        UStaticMesh* Mesh,
        FVector WorldLocation,
        int32 SliceZ = 32,
        int32 Resolution = 64
    );

    // 기존 시각화 업데이트 (슬라이스 Z만 변경)
    // @param Result - 이전 VisualizeSDFSlice 결과
    // @param NewSliceZ - 새로운 Z 슬라이스 인덱스
    UFUNCTION(BlueprintCallable, Category = "FleshRing|Visualization")
    static void UpdateSliceZ(UPARAM(ref) FSDFVisualizationResult& Result, int32 NewSliceZ);

    // 시각화 정리 (평면 액터 제거)
    UFUNCTION(BlueprintCallable, Category = "FleshRing|Visualization")
    static void CleanupVisualization(UPARAM(ref) FSDFVisualizationResult& Result);

    // 모든 슬라이스 한 번에 시각화 (SDF 1회 생성)
    // @param WorldContext - 월드 컨텍스트 (액터 스폰용)
    // @param Mesh - SDF를 생성할 스태틱 메시
    // @param WorldLocation - 평면들을 배치할 기준 월드 위치
    // @param Resolution - SDF 해상도 (기본 64, 슬라이스 개수와 동일)
    // @return 시각화 결과 배열 (Resolution 개수만큼)
    UFUNCTION(BlueprintCallable, Category = "FleshRing|Visualization", meta = (WorldContext = "WorldContextObject"))
    static TArray<FSDFVisualizationResult> VisualizeAllSDFSlices(
        UObject* WorldContextObject,
        UStaticMesh* Mesh,
        FVector WorldLocation,
        int32 Resolution = 64
    );

    // 유틸리티 함수
    // UTexture2D로 슬라이스 추출 (평면 없이 텍스처만 필요할 때)
    UFUNCTION(BlueprintCallable, Category = "FleshRing|Visualization")
    static UTexture2D* GenerateSDFSliceTexture(
        UStaticMesh* Mesh,
        int32 SliceZ = 32,
        int32 Resolution = 64
    );
};
