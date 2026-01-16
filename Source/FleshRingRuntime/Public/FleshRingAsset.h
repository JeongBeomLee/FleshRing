// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "FleshRingTypes.h"
#include "FleshRingAsset.generated.h"

class UFleshRingComponent;

/** 에셋 변경 시 브로드캐스트되는 델리게이트 (구조적 변경 시 전체 리프레시) */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnFleshRingAssetChanged, UFleshRingAsset*);

/** Ring 선택 변경 시 브로드캐스트되는 델리게이트 (디테일 패널 → 뷰포트/트리 동기화) */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnRingSelectionChanged, int32 /*RingIndex*/);

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
	// Material Layer Settings (침투 해결용)
	// =====================================

	/**
	 * 머티리얼-레이어 매핑 배열
	 * 각 머티리얼 슬롯이 어느 레이어(Skin, Stocking 등)에 속하는지 정의
	 * 스타킹 레이어가 항상 스킨 레이어 바깥에 위치하도록 보장
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Material Layer Settings")
	TArray<FMaterialLayerMapping> MaterialLayerMappings;

	/**
	 * 레이어 침투 해결 활성화
	 * 비활성화하면 레이어 순서 보정 없이 순수 변형만 적용
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Material Layer Settings")
	bool bEnableLayerPenetrationResolution = true;

	// =====================================
	// Normal/Tangent Recompute Settings
	// =====================================

	/**
	 * 노멀 재계산 활성화
	 * 변형된 메시의 Face Normal 평균으로 버텍스 노멀 재계산
	 * 비활성화하면 원본 노멀 사용 (라이팅 부정확할 수 있음)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normal/Tangent Recompute", meta = (DisplayName = "Enable Normal Recompute"))
	bool bEnableNormalRecompute = true;

	/**
	 * 노멀 재계산 방식
	 * - Geometric: Face Normal 평균 (TBN 정확, Laplacian 2회 충분)
	 * - SurfaceRotation: 원본 노멀 회전 (기존 방식, Laplacian 10회+ 필요)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normal/Tangent Recompute", meta = (DisplayName = "Normal Recompute Method", EditCondition = "bEnableNormalRecompute"))
	ENormalRecomputeMethod NormalRecomputeMethod = ENormalRecomputeMethod::Geometric;

	/**
	 * 탄젠트 재계산 활성화 (Gram-Schmidt 정규직교화)
	 * 재계산된 노멀에 맞춰 탄젠트를 정규직교화하여 TBN 매트릭스 일관성 유지
	 * 비활성화하면 원본 탄젠트 사용 (노멀맵 렌더링 부정확할 수 있음)
	 * Note: 노멀 재계산이 꺼져있으면 탄젠트 재계산도 무시됨
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Normal/Tangent Recompute", meta = (DisplayName = "Enable Tangent Recompute", EditCondition = "bEnableNormalRecompute"))
	bool bEnableTangentRecompute = true;

	// =====================================
	// Subdivision Settings
	// =====================================

	/** Subdivision 설정 (에디터 프리뷰 + 런타임) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision Settings")
	FSubdivisionSettings SubdivisionSettings;

	// =====================================
	// Editor Selection State (Undo 가능, 디스크 저장 시 초기화)
	// =====================================

	/** 에디터에서 선택된 Ring 인덱스 (-1 = 선택 없음) */
	UPROPERTY()
	int32 EditorSelectedRingIndex = -1;

	/** 에디터에서 선택 타입 (Gizmo/Mesh) */
	UPROPERTY()
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

	/** Ring 이름이 고유한지 확인 (특정 인덱스 제외) */
	bool IsRingNameUnique(FName Name, int32 ExcludeIndex = INDEX_NONE) const;

	/** 고유한 Ring 이름 생성 (중복 시 suffix 추가) */
	FName MakeUniqueRingName(FName BaseName, int32 ExcludeIndex = INDEX_NONE) const;

	/** 유효성 검사 */
	UFUNCTION(BlueprintPure, Category = "FleshRing")
	bool IsValid() const;

	// =====================================
	// Material Layer Utilities
	// =====================================

	/**
	 * 타겟 메시의 머티리얼 슬롯에서 레이어 매핑 자동 생성
	 * 머티리얼 이름 키워드 기반으로 초기 레이어 타입 추측
	 * 기존 매핑은 유지하고 새 슬롯만 추가
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Material Layer Settings")
	void AutoPopulateMaterialLayers();

	/**
	 * 머티리얼 슬롯 인덱스로 레이어 타입 조회
	 * @param MaterialSlotIndex - 조회할 머티리얼 슬롯 인덱스
	 * @return 해당 슬롯의 레이어 타입 (매핑 없으면 Unknown)
	 */
	UFUNCTION(BlueprintPure, Category = "Material Layer Settings")
	EFleshRingLayerType GetLayerTypeForMaterialSlot(int32 MaterialSlotIndex) const;

	/**
	 * 모든 머티리얼 레이어 매핑 초기화
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Material Layer Settings")
	void ClearMaterialLayerMappings();

	/** Subdivided 메시가 생성되어 있는지 */
	UFUNCTION(BlueprintPure, Category = "FleshRing|Subdivision")
	bool HasSubdividedMesh() const { return SubdivisionSettings.SubdividedMesh != nullptr; }

	/** 베이크된 메시가 생성되어 있는지 */
	UFUNCTION(BlueprintPure, Category = "FleshRing|Baked")
	bool HasBakedMesh() const { return SubdivisionSettings.BakedMesh != nullptr; }

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
	 * DetailCustomization에서 버튼으로 호출됨
	 *
	 * @param SourceComponent - AffectedVertices 데이터 제공용 Component (에디터 프리뷰)
	 *                          SmoothingVolumeMode에 따라 Extended/PostProcessing 영역 포함
	 *                          nullptr이면 기존 OBB 기반 영역 사용 (폴백)
	 */
	void GenerateSubdividedMesh(UFleshRingComponent* SourceComponent = nullptr);

	/** Subdivided 메시 제거 (DetailCustomization에서 버튼으로 호출됨) */
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
	bool HasValidPreviewMesh() const { return SubdivisionSettings.PreviewSubdividedMesh != nullptr; }

	/** 프리뷰 메시 재생성 필요 여부 (레벨 변경 시 등) */
	bool NeedsPreviewMeshRegeneration() const;

	/**
	 * 프리뷰 메시 캐시 무효화
	 * 링 본 변경, 링 추가/삭제 시 호출됨
	 */
	void InvalidatePreviewMeshCache();

	/**
	 * 현재 본 구성의 해시 계산
	 * 링 부착 본 목록 + subdivision 파라미터 기반
	 */
	uint32 CalculatePreviewBoneConfigHash() const;

	/**
	 * 프리뷰 메시 캐시가 유효한지 확인
	 */
	bool IsPreviewMeshCacheValid() const;

	// =====================================
	// Baked Mesh (변형 적용 완료된 런타임용 메시)
	// =====================================

	/**
	 * 베이크된 메시 생성 (에디터 전용)
	 * 변형(Tightness, Bulge, Smoothing)이 적용된 최종 메시 생성
	 * 런타임에서는 이 메시를 사용하여 Deformer 없이 동작
	 *
	 * @param SourceComponent - GPU 변형 결과를 제공하는 FleshRingComponent
	 * @return 성공 여부
	 */
	bool GenerateBakedMesh(UFleshRingComponent* SourceComponent);

	/** 베이크 메시 제거 */
	void ClearBakedMesh();

	/** 베이크 파라미터 변경으로 재생성 필요한지 */
	bool NeedsBakeRegeneration() const;

	/** 베이크 파라미터 해시 계산 (Ring 설정 + 변형 파라미터 포함) */
	uint32 CalculateBakeParamsHash() const;

	/**
	 * 에셋 내 누적된 고아 메시 정리
	 * 이전 버전에서 BakedMesh_1, BakedMesh_2... 등이 누적된 경우 호출
	 * 현재 사용 중인 SubdividedMesh, BakedMesh, PreviewSubdividedMesh 외의 SkeletalMesh 제거
	 * @return 제거된 고아 메시 개수
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "FleshRing|Maintenance")
	int32 CleanupOrphanedMeshes();
#endif

	/** 에셋 로드 후 호출 - 에디터 선택 상태 초기화 */
	virtual void PostLoad() override;

#if WITH_EDITOR
	/** 에셋 저장 전 호출 - 자동 베이크 수행 */
	virtual void PreSave(FObjectPreSaveContext SaveContext) override;
#endif

#if WITH_EDITOR
	/** 에디터에서 프로퍼티 변경 시 호출 */
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	/** Undo/Redo 트랜잭션 완료 후 호출 - 손상된 메시 복구 */
	virtual void PostTransacted(const FTransactionObjectEvent& TransactionEvent) override;

	/** 에셋 변경 델리게이트 - 구조적 변경 시 전체 리프레시 */
	FOnFleshRingAssetChanged OnAssetChanged;

	/** Ring 선택 변경 델리게이트 - 디테일 패널 → 뷰포트/트리 동기화 */
	FOnRingSelectionChanged OnRingSelectionChanged;

	/**
	 * Ring 선택 설정 (델리게이트 호출 포함)
	 * 디테일 패널에서 Ring 클릭 시 뷰포트/트리 동기화를 위해 사용
	 */
	void SetEditorSelectedRingIndex(int32 RingIndex, EFleshRingSelectionType SelectionType);
#endif
};
