// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FleshRingFalloff.h"
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
	Manual	UMETA(DisplayName = "Manual"),

	/** 가상 밴드 (스타킹/타이즈용 가상 틀) */
	ProceduralBand	UMETA(DisplayName = "Virtual Band")
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

/** Seed 블렌딩 가중치 타입 (K-Nearest Seeds) */
UENUM(BlueprintType)
enum class ESeedBlendWeightType : uint8
{
	/** 1/(d+1) - 선형 역수, 균일한 블렌딩 */
	InverseLinear	UMETA(DisplayName = "1/(d+1) - Inverse Linear"),

	/** 1/(d+1)² - 제곱 역수, 가까운 seed 강조 */
	InverseSquare	UMETA(DisplayName = "1/(d+1)² - Inverse Square"),

	/** exp(-d/σ) - 가우시안, 부드러운 감쇠 */
	Gaussian		UMETA(DisplayName = "exp(-d/σ) - Gaussian")
};

/** 변형 전파 모드 (Hop-based vs Heat Diffusion) */
UENUM(BlueprintType)
enum class EDeformPropagationMode : uint8
{
	/**
	 * Hop 기반 전파 (기존 방식)
	 * - K-Nearest Seeds 블렌딩
	 * - 1패스, 빠름
	 * - Seed 경계에서 약간의 불연속 가능
	 */
	HopBased		UMETA(DisplayName = "Hop-based (K-Nearest)"),

	/**
	 * Heat Diffusion (열 확산)
	 * - 변형량을 열처럼 확산
	 * - 여러 iteration, 연속적이고 부드러움
	 * - 물리적으로 자연스러운 결과
	 */
	HeatDiffusion	UMETA(DisplayName = "Heat Diffusion")
};

/** 스무딩 볼륨 선택 모드 */
UENUM(BlueprintType)
enum class ESmoothingVolumeMode : uint8
{
	/** Z축 바운드 확장 (SmoothingBoundsZTop/Bottom 사용) */
	BoundsExpand	UMETA(DisplayName = "Bounds Expand (Z)"),

	/** 토폴로지 기반 홉 전파 (Seed에서 N홉까지) */
	HopBased		UMETA(DisplayName = "Hop-Based (Topology)")
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

/**
 * 메시 레이어 타입 (의류 계층 구조)
 * Material 이름에서 자동 감지되거나 수동 지정 가능
 * GPU에서 레이어 침투 해결 시 사용
 */
UENUM(BlueprintType)
enum class EFleshRingLayerType : uint8
{
	/** 피부/살 레이어 (가장 안쪽, 다른 레이어가 침투하면 밀어냄) */
	Skin		UMETA(DisplayName = "Skin (Base Layer)"),

	/** 스타킹/타이즈 레이어 (살 바로 위, 항상 살 바깥에 위치) */
	Stocking	UMETA(DisplayName = "Stocking"),

	/** 속옷 레이어 (스타킹 위) */
	Underwear	UMETA(DisplayName = "Underwear"),

	/** 외투/겉옷 레이어 (가장 바깥쪽) */
	Outerwear	UMETA(DisplayName = "Outerwear"),

	/** 알 수 없음 (기본값, 침투 해결에서 제외) */
	Unknown		UMETA(DisplayName = "Unknown")
};

// =====================================
// 구조체 정의
// =====================================

/**
 * 머티리얼-레이어 매핑 (침투 해결용)
 * 각 머티리얼이 어느 레이어에 속하는지 정의
 * 스타킹이 항상 살 위에 렌더링되도록 보장
 */
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FMaterialLayerMapping
{
	GENERATED_BODY()

	/** 대상 머티리얼 (슬롯 인덱스로 참조) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	int32 MaterialSlotIndex = 0;

	/** 머티리얼 슬롯 이름 (표시용, 자동 설정됨) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Layer")
	FName MaterialSlotName;

	/** 레이어 타입 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	EFleshRingLayerType LayerType = EFleshRingLayerType::Unknown;

	FMaterialLayerMapping()
		: MaterialSlotIndex(0)
		, MaterialSlotName(NAME_None)
		, LayerType(EFleshRingLayerType::Unknown)
	{
	}

	FMaterialLayerMapping(int32 InSlotIndex, FName InSlotName, EFleshRingLayerType InLayerType)
		: MaterialSlotIndex(InSlotIndex)
		, MaterialSlotName(InSlotName)
		, LayerType(InLayerType)
	{
	}
};

// =====================================
// 가상 밴드 설정 (스타킹/타이즈용)
// =====================================

/** 가상 밴드의 상단/하단 섹션 설정 */
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FProceduralBandSection
{
	GENERATED_BODY()

	/** 해당 섹션의 끝단 반경 (MidRadius와의 차이로 경사 결정) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.1"))
	float Radius = 10.0f;

	/** 해당 섹션의 높이 (경사 구간 길이) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0"))
	float Height = 2.0f;

	FProceduralBandSection()
		: Radius(10.0f)
		, Height(2.0f)
	{
	}

	FProceduralBandSection(float InRadius, float InHeight)
		: Radius(InRadius)
		, Height(InHeight)
	{
	}
};

/**
 * 가상 밴드 전체 설정 (스타킹/타이즈용 비대칭 원통)
 *
 * 단면도 (4개의 반경으로 형태 결정):
 *
 *       ══════════════      ← Upper.Radius (상단 끝, 살 불룩)
 *        ╲          ╱       ← Upper Section (경사)
 *         ╔══════╗          ← MidUpperRadius (밴드 상단)
 *         ╚══════╝          ← MidLowerRadius (밴드 하단)
 *        ╱          ╲       ← Lower Section (경사)
 *       ══════════════      ← Lower.Radius (하단 끝, 스타킹 영역)
 */
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FProceduralBandSettings
{
	GENERATED_BODY()

	// ===== 밴드 본체 (조임 지점) =====

	/** 밴드 상단 반경 (Upper Section과 만나는 지점) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band", meta = (ClampMin = "0.1"))
	float MidUpperRadius = 8.0f;

	/** 밴드 하단 반경 (Lower Section과 만나는 지점) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band", meta = (ClampMin = "0.1"))
	float MidLowerRadius = 8.0f;

	/** 밴드 본체 높이 (조이는 영역) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band", meta = (ClampMin = "0.1"))
	float BandHeight = 2.0f;

	/** 밴드 두께 (벽 두께, SDF 생성용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band", meta = (ClampMin = "0.1"))
	float BandThickness = 1.0f;

	// ===== 상단 섹션 (살이 불룩해지는 영역) =====

	/** Upper.Radius > MidUpperRadius → 위로 벌어지며 살이 불룩해짐 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Upper Section")
	FProceduralBandSection Upper;

	// ===== 하단 섹션 (스타킹이 덮는 영역) =====

	/** Lower.Radius ≥ MidLowerRadius → 아래로 벌어지며 스타킹이 덮음 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lower Section")
	FProceduralBandSection Lower;

	// ===== 메시 생성 품질 =====

	/** 원형 단면 세그먼트 수 (높을수록 부드러움) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quality", meta = (ClampMin = "8", ClampMax = "64"))
	int32 RadialSegments = 32;

	/** 높이당 세그먼트 수 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quality", meta = (ClampMin = "1", ClampMax = "16"))
	int32 HeightSegments = 4;

	FProceduralBandSettings()
		: MidUpperRadius(8.0f)        // 밴드 상단 반경
		, MidLowerRadius(8.0f)        // 밴드 하단 반경
		, BandHeight(2.0f)
		, BandThickness(1.0f)
		, Upper(11.0f, 2.0f)          // 상단: 불룩한 살 (가장 큰 반경)
		, Lower(9.0f, 1.0f)           // 하단: 스타킹 영역
		, RadialSegments(32)
		, HeightSegments(4)
	{
	}

	/** 전체 높이 계산 (하단 + 밴드 + 상단) */
	float GetTotalHeight() const
	{
		return Lower.Height + BandHeight + Upper.Height;
	}

	/** 최대 반경 계산 (바운딩용) */
	float GetMaxRadius() const
	{
		return FMath::Max(FMath::Max(MidUpperRadius, MidLowerRadius), FMath::Max(Upper.Radius, Lower.Radius));
	}

};

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
	FName RingName;

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

	/** Bulge Falloff 커브 타입 (거리에 따른 영향도 감쇠 방식) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge"))
	EFleshRingFalloffType BulgeFalloff = EFleshRingFalloffType::WendlandC2;

	/** 볼록 효과 강도 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "0.0"))
	float BulgeIntensity = 1.0f;

	/** Bulge 축 방향 범위 (Ring 높이 대비 배수, 위아래로 얼마나 퍼지는지) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "1.0", ClampMax = "8.0"))
	float BulgeAxialRange = 5.0f;

	/** Bulge 반경 방향 범위 (Ring 반지름 대비 배수, 옆으로 얼마나 퍼지는지) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "1.0", ClampMax = "3.0"))
	float BulgeRadialRange = 1.0f;

	/** 상단 Bulge 강도 배수 (1.0 = 기본, 0.0 = 비활성) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "0.0", ClampMax = "2.0"))
	float UpperBulgeStrength = 1.0f;

	/** 하단 Bulge 강도 배수 (1.0 = 기본, 0.0 = 비활성, 스타킹 효과용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "0.0", ClampMax = "2.0"))
	float LowerBulgeStrength = 1.0f;

	/** Bulge 방향 비율: Radial(바깥) vs Axial(위아래) (0.0 = 순수 Axial, 1.0 = 순수 Radial, 0.7 = 기본) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "0.0", ClampMax = "1.0"))
	float BulgeRadialRatio = 0.7f;

	/** 조이기 강도 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.0", ClampMax = "3.0"))
	float TightnessStrength = 1.0f;

	/**
	 * SDF 바운드 X 방향 확장 (cm)
	 * SDF 텍스처 생성 및 버텍스 필터링 바운드를 X 방향으로 확장
	 * 작은 Ring이 더 큰 영역을 커버해야 할 때 사용
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.0", ClampMax = "10.0", DisplayName = "SDF Bounds Expand X (cm)"))
	float SDFBoundsExpandX = 0.0f;

	/**
	 * SDF 바운드 Y 방향 확장 (cm)
	 * SDF 텍스처 생성 및 버텍스 필터링 바운드를 Y 방향으로 확장
	 * 작은 Ring이 더 큰 영역를 커버해야 할 때 사용
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.0", ClampMax = "10.0", DisplayName = "SDF Bounds Expand Y (cm)"))
	float SDFBoundsExpandY = 0.0f;

	/** 감쇠 곡선 타입 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring")
	EFalloffType FalloffType = EFalloffType::Linear;

	/** 가상 밴드 설정 (VirtualBand 모드에서만 사용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Virtual Band", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::ProceduralBand"))
	FProceduralBandSettings ProceduralBand;

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

	// ===== Post Process (후처리 전체 제어) =====

	/** 후처리 활성화 (Smoothing, PBD 등 모든 후처리 제어) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process")
	bool bEnablePostProcess = true;

	// ===== Smoothing Volume (후처리 영역) =====

	/**
	 * 스무딩 영역 선택 모드
	 * - BoundsExpand: Z축 바운드 확장 (SmoothingBoundsZTop/Bottom)
	 * - HopBased: 토폴로지 기반 홉 전파 (Seed에서 N홉까지)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess"))
	ESmoothingVolumeMode SmoothingVolumeMode = ESmoothingVolumeMode::BoundsExpand;

	/**
	 * 최대 전파 홉 수
	 * - Seed(변형된 버텍스)에서 몇 홉까지 스무딩을 적용할지
	 * - 권장: 저해상도 메시 5~10, 고해상도 메시 3~5
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased", EditConditionHides, ClampMin = "1", ClampMax = "100"))
	int32 MaxSmoothingHops = 5;

	/**
	 * 홉 기반 Falloff Plateau 비율 (0.0 ~ 1.0)
	 * - 이 비율까지는 influence = 1.0 (plateau, 감쇠 없음)
	 * - 이후 MaxSmoothingHops까지 제곱 감쇠
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased", EditConditionHides, ClampMin = "0.0", ClampMax = "1.0"))
	float HopFalloffRatio = 0.3f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased", EditConditionHides))
	EFalloffType HopFalloffType = EFalloffType::Hermite;

	/** 변형 전파 모드 (HopBased vs HeatDiffusion) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased", EditConditionHides, DisplayName = "Propagation Mode"))
	EDeformPropagationMode DeformPropagationMode = EDeformPropagationMode::HeatDiffusion;

	/** K-Nearest Seed 블렌딩 개수 (1 = nearest seed만, 4-8 권장) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased", EditConditionHides, ClampMin = "1", ClampMax = "16", DisplayName = "Seed Blend Count (K)"))
	int32 SeedBlendCount = 4;

	/** Seed 블렌딩 가중치 함수 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased && SeedBlendCount > 1", EditConditionHides, DisplayName = "Seed Blend Weight Type"))
	ESeedBlendWeightType SeedBlendWeightType = ESeedBlendWeightType::InverseSquare;

	/** Gaussian 블렌딩 시그마 (Gaussian 타입 전용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased && SeedBlendCount > 1 && SeedBlendWeightType == ESeedBlendWeightType::Gaussian", EditConditionHides, ClampMin = "0.5", ClampMax = "20.0", DisplayName = "Gaussian Sigma"))
	float SeedBlendGaussianSigma = 3.0f;

	/** Heat Diffusion 반복 횟수 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased && DeformPropagationMode == EDeformPropagationMode::HeatDiffusion", EditConditionHides, ClampMin = "1", ClampMax = "50", DisplayName = "Heat Diffusion Iterations"))
	int32 HeatDiffusionIterations = 10;

	/** Heat Diffusion Lambda (확산 속도) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased && DeformPropagationMode == EDeformPropagationMode::HeatDiffusion", EditConditionHides, ClampMin = "0.1", ClampMax = "0.9", DisplayName = "Heat Diffusion Lambda"))
	float HeatDiffusionLambda = 0.5f;

	/** Hop Propagation 후 Local Polish용 Laplacian 반복 횟수 (0 = 비활성화) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased", EditConditionHides, ClampMin = "0", ClampMax = "5", DisplayName = "Post-Hop Laplacian Iterations"))
	int32 PostHopLaplacianIterations = 1;

	/** Hop Propagation 후 Laplacian 강도 (Lambda) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased && PostHopLaplacianIterations > 0", EditConditionHides, ClampMin = "0.1", ClampMax = "0.8", DisplayName = "Post-Hop Laplacian Lambda"))
	float PostHopLaplacianLambda = 0.3f;

	/** 스무딩 영역 상단 확장 거리 (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::BoundsExpand", EditConditionHides, ClampMin = "0.0", ClampMax = "50.0", DisplayName = "Bounds Expand Top (cm)"))
	float SmoothingBoundsZTop = 5.0f;

	/** 스무딩 영역 하단 확장 거리 (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::BoundsExpand", EditConditionHides, ClampMin = "0.0", ClampMax = "50.0", DisplayName = "Bounds Expand Bottom (cm)"))
	float SmoothingBoundsZBottom = 0.0f;

	// ===== Radial Smoothing (반경 균일화) =====

	/** 반경 균일화 스무딩 활성화 (같은 높이의 버텍스들이 동일한 반경을 갖도록) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess"))
	bool bEnableRadialSmoothing = true;

	// ===== Laplacian/Taubin Smoothing 설정 =====

	/** Laplacian 스무딩 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess"))
	bool bEnableLaplacianSmoothing = true;

	/**
	 * Taubin 스무딩 사용 (수축 방지)
	 * - true: Taubin λ-μ 스무딩 (수축 없이 부드럽게, 권장)
	 * - false: 일반 Laplacian 스무딩 (반복 시 수축 발생)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableLaplacianSmoothing"))
	bool bUseTaubinSmoothing = true;

	/**
	 * 스무딩 강도 λ (Taubin: 수축 단계 강도)
	 * 권장: 0.3~0.7, 기본값 0.5
	 * 경고: 0.8 초과 시 수치 불안정 (비늘 현상)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableLaplacianSmoothing", ClampMin = "0.1", ClampMax = "0.8", UIMin = "0.1", UIMax = "0.8"))
	float SmoothingLambda = 0.5f;

	/**
	 * Taubin 팽창 강도 μ (음수값)
	 * 조건: |μ| > λ, 0이면 자동 계산
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableLaplacianSmoothing && bUseTaubinSmoothing", ClampMin = "-1.0", ClampMax = "0.0"))
	float TaubinMu = -0.53f;

	/** 스무딩 반복 횟수 (Taubin: 각 반복 = λ+μ 2패스) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableLaplacianSmoothing", ClampMin = "1", ClampMax = "10"))
	int32 SmoothingIterations = 2;

	/** 볼륨 보존 (일반 Laplacian 전용, Taubin 시 무시) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableLaplacianSmoothing && !bUseTaubinSmoothing", ClampMin = "0.0", ClampMax = "1.0"))
	float VolumePreservation = 0.3f;

	// ===== PBD Edge Constraint 설정 =====

	/**
	 * PBD Edge Constraint 활성화 (변형 전파)
	 * - 조이기로 인한 변형이 스무딩 볼륨 전체로 퍼지도록 함
	 * - "역 PBD": 변형량이 큰 버텍스는 고정, 작은 버텍스는 자유롭게 이동
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PBD Edge Constraint", meta = (EditCondition = "bEnablePostProcess"))
	bool bEnablePBDEdgeConstraint = false;

	/** PBD 제약 강도 (0.0 ~ 1.0), 권장: 0.5 ~ 0.9 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PBD Edge Constraint", meta = (EditCondition = "bEnablePostProcess && bEnablePBDEdgeConstraint", ClampMin = "0.0", ClampMax = "1.0"))
	float PBDStiffness = 0.8f;

	/** PBD 반복 횟수, 권장: 3 ~ 10 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PBD Edge Constraint", meta = (EditCondition = "bEnablePostProcess && bEnablePBDEdgeConstraint", ClampMin = "1", ClampMax = "20"))
	int32 PBDIterations = 5;

	/** 변형량 기반 가중치 사용 (true: DeformAmount 기반, false: Influence 기반) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PBD Edge Constraint", meta = (EditCondition = "bEnablePostProcess && bEnablePBDEdgeConstraint"))
	bool bPBDUseDeformAmountWeight = true;

	FFleshRingSettings()
		: BoneName(NAME_None)
		, InfluenceMode(EFleshRingInfluenceMode::Auto)
		, RingRadius(5.0f)
		, RingThickness(1.0f)
		, RingWidth(2.0f)
		, bEnableBulge(true)
		, BulgeFalloff(EFleshRingFalloffType::WendlandC2)
		, BulgeIntensity(1.0f)
		, BulgeAxialRange(5.0f)
		, BulgeRadialRange(1.0f)
		, UpperBulgeStrength(1.0f)
		, LowerBulgeStrength(1.0f)
		, TightnessStrength(1.0f)
		, FalloffType(EFalloffType::Linear)
		, bEnablePostProcess(true)
		, SmoothingVolumeMode(ESmoothingVolumeMode::BoundsExpand)
		, MaxSmoothingHops(5)
		, HopFalloffRatio(0.3f)
		, HopFalloffType(EFalloffType::Hermite)
		, DeformPropagationMode(EDeformPropagationMode::HeatDiffusion)
		, SeedBlendCount(4)
		, SeedBlendWeightType(ESeedBlendWeightType::InverseSquare)
		, SeedBlendGaussianSigma(3.0f)
		, HeatDiffusionIterations(10)
		, HeatDiffusionLambda(0.5f)
		, PostHopLaplacianIterations(1)
		, PostHopLaplacianLambda(0.3f)
		, SmoothingBoundsZTop(5.0f)
		, SmoothingBoundsZBottom(0.0f)
		, bEnableRadialSmoothing(true)
		, bEnableLaplacianSmoothing(true)
		, bUseTaubinSmoothing(true)
		, SmoothingLambda(0.5f)
		, TaubinMu(-0.53f)
		, SmoothingIterations(2)
		, VolumePreservation(0.3f)
		, bEnablePBDEdgeConstraint(false)
		, PBDStiffness(0.8f)
		, PBDIterations(5)
		, bPBDUseDeformAmountWeight(true)
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
		return RingName.IsNone() ? FString::Printf(TEXT("FleshRing_%d"), Index) : RingName.ToString();
	}
};
