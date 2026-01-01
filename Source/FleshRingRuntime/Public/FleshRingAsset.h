// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "FleshRingTypes.h"
#include "FleshRingAsset.generated.h"

/** 에셋 변경 시 브로드캐스트되는 델리게이트 (구조적 변경 시 전체 리프레시) */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnFleshRingAssetChanged, UFleshRingAsset*);

/**
 * FleshRing 설정을 저장하는 에셋
 * Content Browser에서 생성하여 여러 캐릭터에 재사용 가능
 */
UCLASS(BlueprintType)
class FLESHRINGRUNTIME_API UFleshRingAsset : public UObject
{
	GENERATED_BODY()

public:
	UFleshRingAsset();

	// =====================================
	// Target Mesh
	// =====================================

	/** 이 에셋이 타겟으로 하는 스켈레탈 메시 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target")
	TSoftObjectPtr<USkeletalMesh> TargetSkeletalMesh;

	// =====================================
	// Ring Settings
	// =====================================

	/** Ring 설정 배열 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring Settings")
	TArray<FFleshRingSettings> Rings;

	// =====================================
	// Subdivision Settings
	// =====================================

	/** Subdivision 활성화 (Low-Poly 메시용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision")
	bool bEnableSubdivision = false;

	/** 최대 Subdivision 레벨 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision", meta = (ClampMin = "1", ClampMax = "6", EditCondition = "bEnableSubdivision"))
	int32 MaxSubdivisionLevel = 4;

	/** 최소 엣지 길이 (이보다 작으면 subdivision 중단) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision", meta = (ClampMin = "0.1", EditCondition = "bEnableSubdivision"))
	float MinEdgeLength = 1.0f;

	/** Ring 영향 범위 배율 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision", meta = (ClampMin = "1.0", ClampMax = "5.0", EditCondition = "bEnableSubdivision"))
	float InfluenceRadiusMultiplier = 2.0f;

	// =====================================
	// Subdivided Mesh (Embedded Asset)
	// =====================================

	/**
	 * Subdivision된 SkeletalMesh (이 에셋 안에 내장됨)
	 * GenerateSubdividedMesh()로 생성됨 - 런타임용 (Ring 영역만 subdivision)
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Subdivision|Generated")
	TObjectPtr<USkeletalMesh> SubdividedMesh;

	/** Subdivision 생성 시점의 파라미터 해시 (재생성 필요 여부 판단용) */
	UPROPERTY()
	uint32 SubdivisionParamsHash = 0;

	// =====================================
	// Preview Mesh (Editor Only, Transient)
	// =====================================

	/**
	 * 에디터 프리뷰용 Subdivision 메시 (Transient - 저장 안 함)
	 * 전체 메시를 균일하게 subdivision하여 링 편집 시 실시간 프리뷰 제공
	 * GeneratePreviewMesh()로 생성됨
	 */
	UPROPERTY(Transient)
	TObjectPtr<USkeletalMesh> PreviewSubdividedMesh;

	/** 에디터 프리뷰용 Subdivision 레벨 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision|Preview", meta = (ClampMin = "1", ClampMax = "4", EditCondition = "bEnableSubdivision"))
	int32 PreviewSubdivisionLevel = 2;

	// =====================================
	// Editor Selection State (Transient - Undo 가능, 저장 안 함)
	// =====================================

	/** 에디터에서 선택된 Ring 인덱스 (-1 = 선택 없음) */
	UPROPERTY(Transient)
	int32 EditorSelectedRingIndex = -1;

	/** 에디터에서 선택 타입 (Gizmo/Mesh) */
	UPROPERTY(Transient)
	EFleshRingSelectionType EditorSelectionType = EFleshRingSelectionType::None;

	// =====================================
	// Utility Functions
	// =====================================

	/** Ring 추가 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	int32 AddRing(const FFleshRingSettings& NewRing);

	/** Ring 제거 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	bool RemoveRing(int32 Index);

	/** Ring 개수 반환 */
	UFUNCTION(BlueprintPure, Category = "FleshRing")
	int32 GetNumRings() const { return Rings.Num(); }

	/** 유효성 검사 */
	UFUNCTION(BlueprintPure, Category = "FleshRing")
	bool IsValid() const;

	/** Subdivided 메시가 생성되어 있는지 */
	UFUNCTION(BlueprintPure, Category = "FleshRing|Subdivision")
	bool HasSubdividedMesh() const { return SubdividedMesh != nullptr; }

	/** Subdivision 파라미터 변경으로 재생성 필요한지 */
	UFUNCTION(BlueprintPure, Category = "FleshRing|Subdivision")
	bool NeedsSubdivisionRegeneration() const;

	/** 현재 파라미터 해시 계산 */
	uint32 CalculateSubdivisionParamsHash() const;

#if WITH_EDITOR
	/**
	 * Subdivided SkeletalMesh 생성 (에디터 전용)
	 * Ring 영향 영역의 삼각형을 subdivision하고 SkinWeight를 barycentric 보간
	 * 런타임용 - Ring 영역만 부분 subdivision
	 */
	UFUNCTION(CallInEditor, Category = "Subdivision")
	void GenerateSubdividedMesh();

	/** Subdivided 메시 제거 */
	UFUNCTION(CallInEditor, Category = "Subdivision")
	void ClearSubdividedMesh();

	/**
	 * 프리뷰용 메시 생성 (에디터 전용, Transient)
	 * 전체 메시를 균일하게 subdivision - 링 편집 시 실시간 프리뷰용
	 * 에셋 에디터 로드 시 자동 호출됨
	 */
	void GeneratePreviewMesh();

	/** 프리뷰 메시 제거 */
	void ClearPreviewMesh();

	/** 프리뷰 메시 유효 여부 */
	bool HasValidPreviewMesh() const { return PreviewSubdividedMesh != nullptr; }

	/** 프리뷰 메시 재생성 필요 여부 (레벨 변경 시 등) */
	bool NeedsPreviewMeshRegeneration() const;
#endif

#if WITH_EDITOR
	/** 에디터에서 프로퍼티 변경 시 호출 */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	/** 에셋 변경 델리게이트 - 구조적 변경 시 전체 리프레시 */
	FOnFleshRingAssetChanged OnAssetChanged;
#endif
};
