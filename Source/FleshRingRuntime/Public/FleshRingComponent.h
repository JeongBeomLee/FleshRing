// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "FleshRingTypes.h"
#include "FleshRingDeformer.h"
#include "FleshRingAffectedVertices.h"
#include "FleshRingDebugViewExtension.h"
#include "RenderGraphResources.h"
#include "FleshRingComponent.generated.h"

class UStaticMesh;
class UVolumeTexture;
class UFleshRingAsset;
class UFleshRingMeshComponent;
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

	/**
	 * 자동 감지된 Bulge 방향
	 * +1 = 위쪽 (경계 버텍스 평균 Z > SDF 중심 Z)
	 * -1 = 아래쪽 (경계 버텍스 평균 Z < SDF 중심 Z)
	 *  0 = 감지 실패 (폐쇄 메시 또는 버텍스 없음)
	 */
	int32 DetectedBulgeDirection = 0;

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
		DetectedBulgeDirection = 0;
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

class UFleshRingDeformerInstance;

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
	virtual void BeginDestroy() override;
	virtual void OnRegister() override;
	virtual void OnUnregister() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	/** 에셋 변경 델리게이트 핸들 */
	FDelegateHandle AssetChangedDelegateHandle;

	/** 에셋 변경 델리게이트 구독/해제 */
	void BindToAssetDelegate();
	void UnbindFromAssetDelegate();

	/** 에셋 변경 콜백 */
	void OnFleshRingAssetChanged(UFleshRingAsset* ChangedAsset);
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

	/**
	 * 런타임에서 FleshRingAsset 교체
	 * 베이크된 에셋 간 전환 시 애니메이션 끊김 없이 즉시 교체
	 */
	/** 런타임 링 에셋 교체 (기존 API, 하위 호환성 유지) */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	void SwapFleshRingAsset(UFleshRingAsset* NewAsset);

	/**
	 * 모듈러 캐릭터용 런타임 링 에셋 교체
	 * Leader Pose 설정 보존 옵션 제공
	 *
	 * @param NewAsset - 새로 적용할 FleshRingAsset (nullptr이면 링 효과 제거 + 원본 메시 복원)
	 * @param bPreserveLeaderPose - LeaderPoseComponent 설정 보존 여부
	 * @return 성공 여부
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Modular")
	bool SwapRingAssetForModular(UFleshRingAsset* NewAsset, bool bPreserveLeaderPose = true);

	/** 베이크 모드로 동작 중인지 여부 (Deformer 없이 BakedMesh 사용) */
	UFUNCTION(BlueprintPure, Category = "FleshRing")
	bool IsUsingBakedMesh() const { return bUsingBakedMesh; }

	// =====================================
	// Target Settings (런타임 오버라이드)
	// =====================================

	/** 수동으로 대상 SkeletalMeshComponent 지정 (false면 Owner에서 자동 검색) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target Settings", AdvancedDisplay)
	bool bUseCustomTarget = false;

	/** 수동 지정 대상 (bUseCustomTarget이 true일 때만 사용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Target Settings", AdvancedDisplay, meta = (EditCondition = "bUseCustomTarget", UseComponentPicker))
	TObjectPtr<USkeletalMeshComponent> CustomTargetMesh;

	// =====================================
	// General (런타임 설정)
	// =====================================

	/** 전체 기능 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General")
	bool bEnableFleshRing = true;

	/** Ring 메시 표시 (SDF 소스 메시) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General")
	bool bShowRingMesh = true;

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

	/** Bulge 방향 화살표 표시 */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowDebugVisualization && bShowBulgeHeatmap"))
	bool bShowBulgeArrows = true;

	/** VirtualBand 와이어프레임 표시 */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowDebugVisualization"))
	bool bShowProceduralBandWireframe = true;

	/** Bulge 영향 범위 원기둥 표시 */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowDebugVisualization"))
	bool bShowBulgeRange = false;
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
	 * Deformer 재초기화 (메시 교체 시 GPU 버퍼 재할당)
	 * 베이킹 등 메시가 변경될 때 호출하여 Deformer의 GPU 버퍼를
	 * 새 메시 크기에 맞게 재할당합니다.
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Editor")
	void ReinitializeDeformer();

	/**
	 * 에디터 프리뷰 환경에서 Deformer를 초기화합니다.
	 * BeginPlay()가 호출되지 않는 에디터 환경에서 사용합니다.
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Editor")
	void InitializeForEditorPreview();

	/**
	 * 에디터 프리뷰 강제 재초기화 (이미 초기화된 상태여도 다시 수행)
	 * 서브디비전 OFF 상태에서 베이크 시 Deformer 설정을 위해 사용
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Editor")
	void ForceInitializeForEditorPreview();

	/**
	 * Ring 트랜스폼만 업데이트 (Deformer 유지, SDF 텍스처 유지)
	 * 기즈모 드래그나 프로퍼티 변경 시 깜빡임 없이 실시간 갱신용
	 * @param DirtyRingIndex - 특정 Ring만 업데이트 (-1이면 전체 업데이트)
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Editor")
	void UpdateRingTransforms(int32 DirtyRingIndex = -1);

	/**
	 * Ring 메시 컴포넌트 재생성 (RingMesh 변경 시 호출)
	 * 에디터에서 RingMesh 프로퍼티 변경 시 사용
	 */
	void RefreshRingMeshes();

	/** 디버그 슬라이스 평면 숨기기/보이기 */
	void SetDebugSlicePlanesVisible(bool bVisible);

	/** Ring 메시 컴포넌트 배열 반환 (에디터 피킹용) */
	const TArray<TObjectPtr<UFleshRingMeshComponent>>& GetRingMeshComponents() const { return RingMeshComponents; }

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

	/** 하나 이상의 유효한 SDF 캐시가 있는지 확인 (부분 동작 허용) */
	bool HasAnyValidSDFCaches() const
	{
		for (const FRingSDFCache& Cache : RingSDFCaches)
		{
			if (Cache.IsValid())
			{
				return true;
			}
		}
		return false;
	}

	/** SDF 없이 동작하는 Ring이 있는지 확인 (Manual/ProceduralBand - 거리 기반 로직) */
	bool HasAnyNonSDFRings() const;

	/** SDF 재생성 (에디터에서 VirtualBand 실시간 갱신용) */
	void RefreshSDF() { GenerateSDF(); }

private:
	/** 에디터 프리뷰 초기화 완료 여부 */
	bool bEditorPreviewInitialized = false;

	/** 베이크 모드로 동작 중인지 여부 (런타임에서 BakedMesh 사용 시 true) */
	bool bUsingBakedMesh = false;

	/** 자동/수동 검색된 실제 대상 */
	UPROPERTY(Transient)
	TWeakObjectPtr<USkeletalMeshComponent> ResolvedTargetMesh;

	/** 컴포넌트 제거 시 복원할 원본 SkeletalMesh (SubdividedMesh 적용 전 저장) */
	UPROPERTY(Transient)
	TWeakObjectPtr<USkeletalMesh> CachedOriginalMesh;

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
	TArray<TObjectPtr<UFleshRingMeshComponent>> RingMeshComponents;

	/** 대상 SkeletalMeshComponent 검색만 수행 (메시 변경 없음) */
	void FindTargetMeshOnly();

	/** 대상 SkeletalMeshComponent 검색 및 메시 설정 (SubdividedMesh 적용 등) */
	void ResolveTargetMesh();

	/** Deformer 생성 및 등록 */
	void SetupDeformer();

	/** Deformer 제거 */
	void CleanupDeformer();

	/**
	 * Deformer를 유지한 채 SDF와 Ring 메시만 갱신
	 * Undo/Redo 시 GPU 메모리 누수 방지를 위해 Deformer 재생성 대신 이 함수 사용
	 * @return true if refresh succeeded, false if full recreation needed
	 */
	bool RefreshWithDeformerReuse();

	/** SDF 생성 (각 Ring의 RingMesh 기반) */
	void GenerateSDF();

	/** Ring 메시 컴포넌트 생성 및 본에 부착 */
	void SetupRingMeshes();

	/** Ring 메시 컴포넌트 제거 */
	void CleanupRingMeshes();

	/** Ring 메시 가시성 업데이트 */
	void UpdateRingMeshVisibility();

	/** 베이크된 메시 적용 (BakedMesh + BakedRingTransforms) */
	void ApplyBakedMesh();

	/** 베이크된 Ring 트랜스폼 적용 (Ring 메시 위치 복원) */
	void ApplyBakedRingTransforms();

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

	/** 디버그용 Spatial Hash (O(1) 버텍스 쿼리용) */
	FVertexSpatialHash DebugSpatialHash;

	/** 디버그 데이터 캐싱 완료 여부 */
	bool bDebugAffectedVerticesCached = false;

	/**
	 * 디버그 시각화용 Bulge 버텍스 데이터 (Ring별)
	 * Smoothstep 거리 기반 필터링 + 방향 필터링 적용된 결과
	 */
	TArray<FRingAffectedData> DebugBulgeData;

	/** Bulge 디버그 데이터 캐싱 완료 여부 */
	bool bDebugBulgeVerticesCached = false;

	// ===== GPU Influence Readback 캐시 =====
	// TightnessCS에서 계산된 Influence 값을 CPU로 Readback하여 시각화용 캐싱
	// DrawAffectedVertices에서 사용

	/** GPU에서 계산된 Influence 캐시 (Ring별) */
	TArray<TArray<float>> CachedGPUInfluences;

	/** GPU Influence Readback 준비 완료 플래그 (Ring별) */
	TArray<bool> bGPUInfluenceReady;

	/** GPU Influence Readback 객체 (Ring별) */
	TArray<TSharedPtr<class FRHIGPUBufferReadback>> GPUInfluenceReadbacks;

	// ===== GPU 디버그 렌더링 =====

	/** GPU 디버그 포인트 렌더링용 SceneViewExtension */
	TSharedPtr<FFleshRingDebugViewExtension> DebugViewExtension;

	/** GPU 디버그 렌더링 활성화 (DrawDebugPoint 대체) */
	bool bUseGPUDebugRendering = true;

public:
	/** GPU 디버그 렌더링 활성화 여부 반환 */
	bool IsGPUDebugRenderingEnabled() const { return bUseGPUDebugRendering; }

	/** GPU 디버그 렌더링용 ViewExtension 반환 (렌더 스레드에서 직접 버퍼 전달용) */
	TSharedPtr<FFleshRingDebugViewExtension, ESPMode::ThreadSafe> GetDebugViewExtension() const
	{
		return StaticCastSharedPtr<FFleshRingDebugViewExtension, FFleshRingDebugViewExtension, ESPMode::ThreadSafe>(DebugViewExtension);
	}

	/** 디버그 포인트 수 반환 (첫 번째 Ring의 AffectedVertices 수) */
	uint32 GetDebugPointCount() const
	{
		if (DebugAffectedData.Num() > 0)
		{
			return DebugAffectedData[0].Vertices.Num();
		}
		return 0;
	}

	/**
	 * CPU 디버그 캐시 무효화 (Ring 이동 시 다른 클래스에서 호출)
	 * @param DirtyRingIndex - 특정 Ring만 무효화 (INDEX_NONE이면 전체 무효화)
	 */
	void InvalidateDebugCaches(int32 DirtyRingIndex = INDEX_NONE)
	{
		if (DirtyRingIndex == INDEX_NONE)
		{
			// 전체 무효화: 모든 데이터 리셋
			bDebugAffectedVerticesCached = false;
			bDebugBulgeVerticesCached = false;
		}
		else
		{
			// 특정 Ring만 무효화: 해당 Ring 데이터만 Reset
			// 캐싱 함수가 다시 호출되도록 플래그도 false로 설정
			// (캐싱 함수 내부에서 이미 데이터 있는 Ring은 스킵)
			if (DebugAffectedData.IsValidIndex(DirtyRingIndex))
			{
				DebugAffectedData[DirtyRingIndex].Vertices.Reset();
				bDebugAffectedVerticesCached = false;
			}
			if (DebugBulgeData.IsValidIndex(DirtyRingIndex))
			{
				DebugBulgeData[DirtyRingIndex].Vertices.Reset();
				bDebugBulgeVerticesCached = false;
			}
		}
	}

private:
	/** GPU 디버그 렌더링용 ViewExtension 초기화 */
	void InitializeDebugViewExtension();

	/** GPU 디버그 렌더링용 포인트 버퍼 업데이트 */
	void UpdateDebugPointBuffer();

	/** GPU 디버그 렌더링용 Bulge 포인트 버퍼 업데이트 */
	void UpdateDebugBulgePointBuffer();
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

	/** Bulge 히트맵 그리기 (Smoothstep 거리 기반 필터링 + 방향 필터링 적용) */
	void DrawBulgeHeatmap(int32 RingIndex);

	/** 디버그용 Bulge 버텍스 데이터 캐싱 */
	void CacheBulgeVerticesForDebug();

	/** 감지된 Bulge 방향 화살표 그리기 */
	void DrawBulgeDirectionArrow(int32 RingIndex);

	/** VirtualBand 와이어프레임 그리기 */
	void DrawProceduralBandWireframe(int32 RingIndex);

	/** Bulge 영향 범위 원기둥 그리기 */
	void DrawBulgeRange(int32 RingIndex);
#endif
};
