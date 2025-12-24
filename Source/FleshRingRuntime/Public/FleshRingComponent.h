// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "FleshRingTypes.h"
#include "FleshRingDeformer.h"
#include "FleshRingComponent.generated.h"

class UStaticMesh;
class UVolumeTexture;
class UFleshRingAsset;

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

public:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// =====================================
	// FleshRing Asset (Primary Data Source)
	// =====================================

	/** FleshRing 데이터 에셋 (Ring 설정, SDF 설정, SDFSourceMesh 등 포함) */
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
	/** SDF 볼륨 표시 */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowSdfVolume = false;

	/** 영향받는 버텍스 표시 */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowAffectedVertices = false;

	/** Ring 기즈모 표시 */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowRingGizmos = true;

	/** Bulge 히트맵 표시 */
	UPROPERTY(EditAnywhere, Category = "Debug")
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

private:
	/** 자동/수동 검색된 실제 대상 */
	UPROPERTY(Transient)
	TWeakObjectPtr<USkeletalMeshComponent> ResolvedTargetMesh;

	/** 내부에 생성된 Deformer */
	UPROPERTY(Transient)
	TObjectPtr<UFleshRingDeformer> InternalDeformer;

	/** SDF 3D 볼륨 텍스처 */
	UPROPERTY(Transient)
	TObjectPtr<UVolumeTexture> SDFVolumeTexture;

	/** 대상 SkeletalMeshComponent 검색 및 설정 */
	void ResolveTargetMesh();

	/** Deformer 생성 및 등록 */
	void SetupDeformer();

	/** Deformer 제거 */
	void CleanupDeformer();

	/** SDF 생성 (Asset의 SDFSourceMesh 기반) */
	void GenerateSDF();
};
