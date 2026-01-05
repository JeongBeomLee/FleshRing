// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FleshRingTypes.generated.h"

class UStaticMesh;

// =====================================
// 열거형 정의
// =====================================

/** Ring 선택 타입 (에디터용) */
UENUM()
enum class EFleshRingSelectionType : uint8
{
	None,		// 선택 없음
	Gizmo,		// Ring 기즈모 선택 (이동 + Scale로 반경 조절)
	Mesh		// Ring 메시 선택 (메시 이동/회전)
};

/** Ring 영향 범위 결정 방식 */
UENUM(BlueprintType)
enum class EFleshRingInfluenceMode : uint8
{
	/** SDF 기반 자동 계산 */
	Auto	UMETA(DisplayName = "Auto (SDF-based)"),

	/** 수동 Radius 지정 */
	Manual	UMETA(DisplayName = "Manual")
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

/** Bulge 방향 모드 */
UENUM(BlueprintType)
enum class EBulgeDirectionMode : uint8
{
	/** SDF 경계 버텍스로 자동 감지 (폐쇄 메시는 양방향) */
	Auto		UMETA(DisplayName = "Auto (Boundary Detection)"),

	/** 양방향 Bulge (도넛형 Ring, 폐쇄 메시) */
	Bidirectional	UMETA(DisplayName = "Bidirectional (Both)"),

	/** +Z 방향 (위쪽) 강제 */
	Positive	UMETA(DisplayName = "Positive (+Z)"),

	/** -Z 방향 (아래쪽) 강제 */
	Negative	UMETA(DisplayName = "Negative (-Z)")
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

	/** Ring 커스텀 이름 (비어있으면 "FleshRing_인덱스" 형식 사용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	FString RingName;

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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::Manual", ClampMin = "0.1", ClampMax = "50.0"))
	float RingWidth = 2.0f;

	/** Bone 기준 Ring 위치 오프셋 (변형 영역) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::Manual"))
	FVector RingOffset = FVector::ZeroVector;

	/** Ring 회전 Euler 각도 (UI 편집용, 제한 없음) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::Manual", DisplayName = "Ring Rotation"))
	FRotator RingEulerRotation = FRotator(-90.0f, 0.0f, 0.0f);

	/** Bulge 효과 활성화 (부피 보존) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	bool bEnableBulge = true;

	/** Bulge 방향 모드 (Auto: SDF 경계 감지, Positive: +Z, Negative: -Z) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge"))
	EBulgeDirectionMode BulgeDirection = EBulgeDirectionMode::Auto;

	/** 볼록 효과 강도 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "0.0"))
	float BulgeIntensity = 0.5f;

	/** Bulge 축 방향 범위 (Ring 높이 대비 배수, 위아래로 얼마나 퍼지는지) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "1.0", ClampMax = "8.0"))
	float BulgeAxialRange = 3.0f;

	/** Bulge 반경 방향 범위 (Ring 반지름 대비 배수, 옆으로 얼마나 퍼지는지) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "1.0", ClampMax = "3.0"))
	float BulgeRadialRange = 1.5f;

	/** 조이기 강도 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.0", ClampMax = "3.0"))
	float TightnessStrength = 1.0f;

	/** 감쇠 곡선 타입 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	EFalloffType FalloffType = EFalloffType::Linear;

	/** Ring 회전 (실제 적용되는 쿼터니언, 런타임에서 사용) */
	UPROPERTY()
	FQuat RingRotation = FRotator(-90.0f, 0.0f, 0.0f).Quaternion();

	/** Bone 기준 메시 위치 오프셋 (시각적 + SDF) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	FVector MeshOffset = FVector::ZeroVector;

	/** 메시 회전 (실제 적용되는 쿼터니언, 런타임에서 사용) */
	UPROPERTY()
	FQuat MeshRotation = FRotator(-90.0f, 0.0f, 0.0f).Quaternion();

	/** 메시 회전 Euler 각도 (UI 편집용, 제한 없음) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform", meta = (DisplayName = "Mesh Rotation"))
	FRotator MeshEulerRotation = FRotator(-90.0f, 0.0f, 0.0f);

	/** 메시 스케일 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform", meta = (ClampMin = "0.01"))
	FVector MeshScale = FVector::OneVector;

	FFleshRingSettings()
		: BoneName(NAME_None)
		, InfluenceMode(EFleshRingInfluenceMode::Auto)
		, RingRadius(5.0f)
		, RingThickness(1.0f)
		, RingWidth(2.0f)
		, bEnableBulge(true)
		, BulgeIntensity(0.5f)
		, BulgeAxialRange(3.0f)
		, BulgeRadialRange(1.5f)
		, TightnessStrength(1.0f)
		, FalloffType(EFalloffType::Linear)
	{
	}

	/**
	 * Ring 메시의 월드 트랜스폼 계산
	 * @param BoneTransform 본의 컴포넌트 스페이스 트랜스폼
	 * @return Ring 메시의 월드 트랜스폼 (Location, Rotation, Scale)
	 */
	FTransform CalculateWorldTransform(const FTransform& BoneTransform) const
	{
		const FQuat BoneRotation = BoneTransform.GetRotation();
		const FVector WorldLocation = BoneTransform.GetLocation() + BoneRotation.RotateVector(MeshOffset);
		const FQuat WorldRotation = BoneRotation * MeshRotation;

		return FTransform(WorldRotation, WorldLocation, MeshScale);
	}

	/**
	 * 표시용 Ring 이름 반환
	 * @param Index 배열 인덱스 (커스텀 이름이 없을 때 fallback용)
	 * @return 커스텀 이름 또는 "FleshRing_인덱스" 형식
	 */
	FString GetDisplayName(int32 Index) const
	{
		return RingName.IsEmpty() ? FString::Printf(TEXT("FleshRing_%d"), Index) : RingName;
	}
};
