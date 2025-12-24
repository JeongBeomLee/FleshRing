// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FleshRingTypes.generated.h"

class UStaticMesh;

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
struct FLESHRINGRUNTIME_API FFleshRingSettings
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
struct FLESHRINGRUNTIME_API FFleshRingSdfSettings
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
