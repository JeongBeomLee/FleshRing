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

/** 감쇠 곡선 타입 */
UENUM(BlueprintType)
enum class EFalloffType : uint8
{
	/** 선형 감쇠 */
	Linear		UMETA(DisplayName = "Linear"),

	/** 2차 곡선 감쇠 (부드러움) */
	Quadratic	UMETA(DisplayName = "Quadratic"),

	/** Hermite S-커브 감쇠 (가장 부드러움) */
	Hermite		UMETA(DisplayName = "Hermite (S-Curve)")
};

// =====================================
// 구조체 정의
// =====================================

/** SDF 관련 설정 (Ring별) */
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
	EFleshRingSdfUpdateMode UpdateMode = EFleshRingSdfUpdateMode::OnChange;
};

/** 개별 Ring 설정 */
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FFleshRingSettings
{
	GENERATED_BODY()

	/** 타겟 본 이름 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	FName BoneName;

	/** Ring 메쉬 (시각적 표현 + SDF 소스) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	TSoftObjectPtr<UStaticMesh> RingMesh;

	/** 영향 범위 결정 방식 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	EFleshRingInfluenceMode InfluenceMode = EFleshRingInfluenceMode::Auto;

	/** Ring 반지름 (Manual 모드에서만 사용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::Manual", ClampMin = "0.1", ClampMax = "100.0"))
	float RingRadius = 5.0f;

	/** 링 두께 - 반경 방향 벽 두께 (안쪽→바깥쪽) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::Manual", ClampMin = "0.1", ClampMax = "20.0"))
	float RingThickness = 1.0f;

	/** 링 높이 - 축 방향 전체 높이 (위아래 각각 RingWidth/2) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.1", ClampMax = "50.0"))
	float RingWidth = 2.0f;

	/** 볼록 효과 강도 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float BulgeIntensity = 0.5f;

	/** 조이기 강도 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.0", ClampMax = "3.0"))
	float TightnessStrength = 1.0f;

	/** 감쇠 곡선 타입 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	EFalloffType FalloffType = EFalloffType::Linear;

	/** 이 Ring의 SDF 설정 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SDF")
	FFleshRingSdfSettings SdfSettings;

	/** Bone 기준 Ring 위치 오프셋 (변형 영역) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	FVector RingOffset = FVector::ZeroVector;

	/** Ring 회전 오프셋 (본 forward 기준 추가 회전) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	FRotator RingRotation = FRotator::ZeroRotator;

	/** Bone 기준 메시 위치 오프셋 (시각적 + SDF) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	FVector MeshOffset = FVector::ZeroVector;

	/** 메시 회전 오프셋 (본 forward 기준 추가 회전) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	FRotator MeshRotation = FRotator::ZeroRotator;

	/** 메시 스케일 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform", meta = (ClampMin = "0.01", ClampMax = "10.0"))
	FVector MeshScale = FVector::OneVector;

	FFleshRingSettings()
		: BoneName(NAME_None)
		, InfluenceMode(EFleshRingInfluenceMode::Auto)
		, RingRadius(5.0f)
		, RingThickness(1.0f)
		, RingWidth(2.0f)
		, BulgeIntensity(0.5f)
		, TightnessStrength(1.0f)
		, FalloffType(EFalloffType::Linear)
	{
	}
};
