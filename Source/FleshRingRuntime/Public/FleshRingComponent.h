// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FleshRingComponent.generated.h"

// =====================================
// 열거형 정의
// =====================================

/** Ring 영향 범위 결정 방식 */
UENUM(BlueprintType)
enum class EFleshRingInfluenceMode : uint8
{
	/** SDF 기반 자동 계산 */
	Auto	UMETA(DisplayName = "Auto (SDF-based)"),

	/** 수동 Radius 지정 */
	Manual	UMETA(DisplayName = "Manual")
};

/** SDF 업데이트 모드 */
UENUM(BlueprintType)
enum class EFleshRingSdfUpdateMode : uint8
{
	/** 매 틱마다 업데이트 */
	OnTick		UMETA(DisplayName = "On Tick"),

	/** 값 변경 시에만 업데이트 */
	OnChange	UMETA(DisplayName = "On Change"),

	/** 수동 업데이트 */
	Manual		UMETA(DisplayName = "Manual")
};

// =====================================
// 구조체 정의
// =====================================

/** 개별 Ring 설정 */
USTRUCT(BlueprintType)
struct FFleshRingSettings
{
	GENERATED_BODY()

	/** 타겟 본 이름 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	FName BoneName;

	/** Ring 메쉬 (시각적 표현용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	TSoftObjectPtr<UStaticMesh> RingMesh;

	/** 영향 범위 결정 방식 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	EFleshRingInfluenceMode InfluenceMode = EFleshRingInfluenceMode::Auto;

	/** Ring 반지름 (Manual 모드에서만 사용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::Manual", ClampMin = "0.1", ClampMax = "100.0"))
	float RingRadius = 5.0f;

	/** Ring 두께 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.1", ClampMax = "50.0"))
	float RingWidth = 2.0f;

	/** 감쇠 곡선 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.0", ClampMax = "5.0"))
	float Falloff = 1.0f;

	/** 볼록 효과 강도 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float BulgeIntensity = 0.5f;

	FFleshRingSettings()
		: BoneName(NAME_None)
		, InfluenceMode(EFleshRingInfluenceMode::Auto)
		, RingRadius(5.0f)
		, RingWidth(2.0f)
		, Falloff(1.0f)
		, BulgeIntensity(0.5f)
	{
	}
};

/** SDF 관련 설정 */
USTRUCT(BlueprintType)
struct FFleshRingSdfSettings
{
	GENERATED_BODY()

	/** SDF 볼륨 해상도 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SDF", meta = (ClampMin = "16", ClampMax = "128"))
	int32 Resolution = 64;

	/** JFA 반복 횟수 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SDF", meta = (ClampMin = "1", ClampMax = "16"))
	int32 JfaIterations = 8;

	/** 업데이트 모드 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SDF")
	EFleshRingSdfUpdateMode UpdateMode = EFleshRingSdfUpdateMode::OnTick;
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

public:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// =====================================
	// General
	// =====================================

	/** 변형 대상 스켈레탈 메쉬 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General")
	TObjectPtr<USkeletalMeshComponent> TargetSkeletalMesh;

	/** 전체 기능 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General")
	bool bEnableFleshRing = true;

	// =====================================
	// Ring Settings
	// =====================================

	/** Ring 설정 배열 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring Settings")
	TArray<FFleshRingSettings> Rings;

	// =====================================
	// SDF Settings
	// =====================================

	/** SDF 설정 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SDF Settings")
	FFleshRingSdfSettings SdfSettings;

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
};
