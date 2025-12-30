// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "FleshRingTypes.h"
#include "FleshRingDeformer.h"
#include "FleshRingAffectedVertices.h"
#include "RenderGraphResources.h"
#include "FleshRingComponent.generated.h"

class UStaticMesh;
class UVolumeTexture;
class UFleshRingAsset;
struct IPooledRenderTarget;

// =====================================
// SDF 캐시 구조체 (Ring별 영구 저장)
// =====================================

/**
 * Ring별 SDF 텍스처 캐시
 * RDG 텍스처를 Pooled 텍스처로 변환하여 영구 저장
 * Deformer에서 매 프레임 RegisterExternalTexture()로 사용
 */
struct FRingSDFCache
{
	/**
	 * Pooled 렌더 타겟 (GPU에 영구 저장)
	 *
	 * IPooledRenderTarget이란?
	 * - RDG(Render Dependency Graph) 외부에서 렌더 타겟을 영구 보관하기 위한 인터페이스
	 * - RDG 텍스처는 GraphBuilder.Execute() 후 소멸되지만,
	 *   ConvertToExternalTexture()로 Pooled 텍스처로 변환하면 프레임 간 유지됨
	 * - 이후 프레임에서 RegisterExternalTexture()로 RDG에 다시 등록하여 사용
	 * - TRefCountPtr로 참조 카운트 관리 (SafeRelease()로 해제)
	 */
	TRefCountPtr<IPooledRenderTarget> PooledTexture;

	/** SDF 볼륨 최소 바운드 (Ring 로컬 스페이스) */
	FVector3f BoundsMin = FVector3f::ZeroVector;

	/** SDF 볼륨 최대 바운드 (Ring 로컬 스페이스) */
	FVector3f BoundsMax = FVector3f::ZeroVector;

	/** SDF 해상도 */
	FIntVector Resolution = FIntVector(64, 64, 64);

	/**
	 * Ring 로컬 → 컴포넌트 스페이스 트랜스폼 (OBB용)
	 * SDF는 로컬 스페이스에서 생성, 샘플링 시 역변환 사용
	 */
	FTransform LocalToComponent = FTransform::Identity;

	/** 캐싱 완료 여부 */
	bool bCached = false;

	/** 캐시 초기화 */
	void Reset()
	{
		PooledTexture.SafeRelease();
		BoundsMin = FVector3f::ZeroVector;
		BoundsMax = FVector3f::ZeroVector;
		Resolution = FIntVector(64, 64, 64);
		LocalToComponent = FTransform::Identity;
		bCached = false;
	}

	/** 유효성 검사 */
	bool IsValid() const
	{
		return bCached && PooledTexture.IsValid();
	}
};

// =====================================
// 컴포넌트 클래스
// =====================================

/**
 * FleshRing 메쉬 변형 컴포넌트
 * SDF 기반으로 스켈레탈 메쉬의 살(Flesh) 표현을 처리
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent), DisplayName="Flesh Ring")
class FLESHRINGRUNTIME_API UFleshRingComponent : public UActorComponent
{
	GENERATED_BODY()
	
public:
	UFleshRingComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnRegister() override;
	virtual void OnUnregister() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

public:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// =====================================
	// FleshRing Asset (Primary Data Source)
	// =====================================

	/** FleshRing 데이터 에셋 (Ring 설정 포함) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FleshRing Asset")
	TObjectPtr<UFleshRingAsset> FleshRingAsset;

	/** Asset 변경 시 호출 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	void ApplyAsset();

	// =====================================
	// Target Settings (런타임 오버라이드)
	// =====================================

	/** 수동으로 대상 SkeletalMeshComponent 지정 (false면 Owner에서 자동 검색) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target Settings", AdvancedDisplay)
	bool bUseCustomTarget = false;

	/** 수동 지정 대상 (bUseCustomTarget이 true일 때만 사용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target Settings", AdvancedDisplay, meta = (EditCondition = "bUseCustomTarget"))
	TObjectPtr<USkeletalMeshComponent> CustomTargetMesh;

	// =====================================
	// General (런타임 설정)
	// =====================================

	/** 전체 기능 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General")
	bool bEnableFleshRing = true;

	/** Bounds 확장 배율 (VSM 캐싱을 위해 Deformer 변형량에 맞게 조정) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General", meta = (ClampMin = "1.0", ClampMax = "3.0"))
	float BoundsScale = 2.0f;

	// =====================================
	// Debug / Visualization (에디터 전용)
	// =====================================

#if WITH_EDITORONLY_DATA
	/** 디버그 시각화 전체 활성화 (마스터 스위치) */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowDebugVisualization = false;

	/** SDF 볼륨 바운드 박스 표시 */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowDebugVisualization"))
	bool bShowSdfVolume = false;

	/** 영향받는 버텍스 표시 (색상 = Influence 강도) */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowDebugVisualization"))
	bool bShowAffectedVertices = false;

	/** SDF 슬라이스 평면 표시 */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowDebugVisualization"))
	bool bShowSDFSlice = false;

	/** 표시할 SDF 슬라이스 Z 인덱스 (0 ~ Resolution-1) */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowDebugVisualization && bShowSDFSlice", ClampMin = "0", ClampMax = "63", UIMin = "0", UIMax = "63"))
	int32 DebugSliceZ = 32;

	// TODO: Bulge 히트맵 시각화 구현
	/** Bulge 히트맵 표시 */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowDebugVisualization"))
	bool bShowBulgeHeatmap = false;
#endif

	// =====================================
	// Blueprint Callable Functions
	// =====================================

	/** SDF 수동 업데이트 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	void UpdateSDF();

	/** 실제 적용될 SkeletalMeshComponent 반환 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	USkeletalMeshComponent* GetResolvedTargetMesh() const { return ResolvedTargetMesh.Get(); }

	/** 내부 Deformer 반환 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	UFleshRingDeformer* GetDeformer() const { return InternalDeformer; }

	/**
	 * 에디터 프리뷰 환경에서 Deformer를 초기화합니다.
	 * BeginPlay()가 호출되지 않는 에디터 환경에서 사용합니다.
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Editor")
	void InitializeForEditorPreview();

	/**
	 * Ring 트랜스폼만 업데이트 (Deformer 유지, SDF 텍스처 유지)
	 * 기즈모 드래그나 프로퍼티 변경 시 깜빡임 없이 실시간 갱신용
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Editor")
	void UpdateRingTransforms();

	/** 디버그 슬라이스 평면 숨기기/보이기 */
	void SetDebugSlicePlanesVisible(bool bVisible);

	/** Ring 메시 컴포넌트 배열 반환 (에디터 피킹용) */
	const TArray<TObjectPtr<UStaticMeshComponent>>& GetRingMeshComponents() const { return RingMeshComponents; }

	// =====================================
	// SDF 캐시 접근 (Deformer에서 사용)
	// =====================================

	/** Ring 개수 반환 */
	int32 GetNumRingSDFCaches() const { return RingSDFCaches.Num(); }

	/** 특정 Ring의 SDF 캐시 반환 (읽기 전용) */
	const FRingSDFCache* GetRingSDFCache(int32 RingIndex) const
	{
		if (RingSDFCaches.IsValidIndex(RingIndex))
		{
			return &RingSDFCaches[RingIndex];
		}
		return nullptr;
	}

	/** 모든 Ring의 SDF 캐시가 유효한지 확인 */
	bool AreAllSDFCachesValid() const
	{
		for (const FRingSDFCache& Cache : RingSDFCaches)
		{
			if (!Cache.IsValid())
			{
				return false;
			}
		}
		return RingSDFCaches.Num() > 0;
	}

private:
	/** 에디터 프리뷰 초기화 완료 여부 */
	bool bEditorPreviewInitialized = false;

	/** 자동/수동 검색된 실제 대상 */
	UPROPERTY(Transient)
	TWeakObjectPtr<USkeletalMeshComponent> ResolvedTargetMesh;

	/** 내부에 생성된 Deformer */
	UPROPERTY(Transient)
	TObjectPtr<UFleshRingDeformer> InternalDeformer;

	/**
	 * Ring별 SDF 캐시 배열
	 * - GenerateSDF()에서 Pooled 텍스처로 변환하여 저장
	 * - Deformer에서 GetRingSDFCache()로 접근
	 * - UPROPERTY 불가 (IPooledRenderTarget은 UObject가 아님)
	 * - CleanupDeformer()에서 수동 해제 필요
	 */
	TArray<FRingSDFCache> RingSDFCaches;

	/**
	 * Ring별 렌더링용 StaticMeshComponent 배열
	 * - SetupRingMeshes()에서 생성하여 본에 부착
	 * - CleanupRingMeshes()에서 제거
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> RingMeshComponents;

	/** 대상 SkeletalMeshComponent 검색 및 설정 */
	void ResolveTargetMesh();

	/** Deformer 생성 및 등록 */
	void SetupDeformer();

	/** Deformer 제거 */
	void CleanupDeformer();

	/** SDF 생성 (각 Ring의 RingMesh 기반) */
	void GenerateSDF();

	/** Ring 메시 컴포넌트 생성 및 본에 부착 */
	void SetupRingMeshes();

	/** Ring 메시 컴포넌트 제거 */
	void CleanupRingMeshes();

	// =====================================
	// Debug Drawing (에디터 전용)
	// =====================================

#if WITH_EDITORONLY_DATA
	/** SDF 슬라이스 시각화용 평면 액터 (Ring별) */
	UPROPERTY(Transient)
	TArray<TObjectPtr<AActor>> DebugSlicePlaneActors;

	/** SDF 슬라이스 시각화용 렌더 타겟 (Ring별) */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextureRenderTarget2D>> DebugSliceRenderTargets;

	/**
	 * 디버그 시각화용 영향받는 버텍스 데이터 (Ring별)
	 * GenerateSDF() 완료 후 CacheAffectedVerticesForDebug()에서 계산
	 */
	TArray<FRingAffectedData> DebugAffectedData;

	/** 바인드 포즈 버텍스 위치 (컴포넌트 스페이스) */
	TArray<FVector3f> DebugBindPoseVertices;

	/** 디버그 데이터 캐싱 완료 여부 */
	bool bDebugAffectedVerticesCached = false;
#endif

#if WITH_EDITOR
	/** 디버그 시각화 메인 함수 (TickComponent에서 호출) */
	void DrawDebugVisualization();

	/** SDF 볼륨 바운드 박스 그리기 */
	void DrawSdfVolume(int32 RingIndex);

	/** 영향받는 버텍스 그리기 */
	void DrawAffectedVertices(int32 RingIndex);

	/** SDF 슬라이스 평면 그리기 */
	void DrawSDFSlice(int32 RingIndex);

	/** 슬라이스 평면 액터 생성 */
	AActor* CreateDebugSlicePlane(int32 RingIndex);

	/** 슬라이스 텍스처 업데이트 */
	void UpdateSliceTexture(int32 RingIndex, int32 SliceZ);

	/** 디버그 리소스 정리 */
	void CleanupDebugResources();

	/** 디버그용 영향받는 버텍스 데이터 캐싱 */
	void CacheAffectedVerticesForDebug();
#endif
};
