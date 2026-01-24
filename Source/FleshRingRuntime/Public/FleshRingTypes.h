// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FleshRingFalloff.h"
#include "FleshRingTypes.generated.h"

class UStaticMesh;
class USkeletalMesh;

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

/** Virtual Band 섹션 타입 (개별 편집용) */
enum class EBandSection : uint8
{
	None,		// 섹션 미선택 (전체 밴드)
	Upper,		// 상단 캡 (Upper.Radius, Upper.Height)
	MidUpper,	// 밴드 상단 경계 (MidUpperRadius)
	MidLower,	// 밴드 하단 경계 (MidLowerRadius)
	Lower		// 하단 캡 (Lower.Radius, Lower.Height)
};

/** Ring 영향 범위 결정 방식 */
UENUM(BlueprintType)
enum class EFleshRingInfluenceMode : uint8
{
	/** 메시 기반 영향 범위 계산 (SDF) */
	MeshBased	UMETA(DisplayName = "Mesh Based"),

	/** 수동 Radius 지정 (가상 Ring) */
	VirtualRing	UMETA(DisplayName = "Virtual Ring"),

	/** 가상 밴드 (스타킹/타이즈용 가상 틀) */
	VirtualBand	UMETA(DisplayName = "Virtual Band"),

	/** [DEPRECATED] Auto → MeshBased 리네이밍됨, 기존 에셋 호환성용 */
	Auto = MeshBased	UMETA(Hidden)
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

/** 스무딩 볼륨 선택 모드 */
UENUM(BlueprintType)
enum class ESmoothingVolumeMode : uint8
{
	/** Z축 바운드 확장 (SmoothingBoundsZTop/Bottom 사용) */
	BoundsExpand	UMETA(DisplayName = "Bounds Expand (Z)"),

	/** 토폴로지 기반 홉 전파 (Seed에서 N홉까지) */
	HopBased		UMETA(DisplayName = "Hop-Based (Topology)")
};

/** Laplacian 스무딩 알고리즘 타입 */
UENUM(BlueprintType)
enum class ELaplacianSmoothingType : uint8
{
	/** 일반 Laplacian (반복 시 수축 발생) */
	Laplacian	UMETA(DisplayName = "Laplacian"),

	/** Taubin λ-μ 스무딩 (수축 방지) */
	Taubin		UMETA(DisplayName = "Taubin (No Shrink)")
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
 *
 * NOTE [마이그레이션]: UE enum 직렬화는 이름 기반!
 *       - 값 순서 변경 시 기존 에셋 깨짐
 *       - 이름 변경 시에도 기존 에셋 깨짐 (이름으로 저장됨)
 *       - 새 타입 추가는 항상 맨 뒤에 할 것
 *       - 이름 변경 시 기존 이름을 Hidden 별칭으로 유지할 것
 *       (Unknown → Other 리네이밍, Unknown = Other로 별칭 유지)
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

	/** 기타/미분류 (자동 감지 실패 시 기본값, AffectedLayerMask로 포함/제외 제어) */
	Other		UMETA(DisplayName = "Other"),

	/**
	 * 제외 (Tightness 효과 적용 안 함)
	 * AffectedLayerMask와 무관하게 항상 제외됨
	 * 눈동자, 머리카락, 악세서리 등 Tightness가 필요 없는 머티리얼에 사용
	 */
	Exclude		UMETA(DisplayName = "Exclude (Never Affected)"),

	/**
	 * [DEPRECATED] Unknown → Other로 리네이밍됨
	 * 기존 에셋 역직렬화 호환성을 위해 유지 (UE는 이름 기반 직렬화)
	 * 에디터에서 Hidden 처리, 새 에셋에서는 Other 사용 권장
	 */
	Unknown = Other	UMETA(Hidden)
};

/**
 * 레이어 선택 비트마스크 (Tightness 영향 대상 레이어 지정)
 * 여러 레이어를 동시에 선택 가능 (예: Skin | Stocking)
 *
 * NOTE [마이그레이션]: 비트 추가/변경 시 기존 에셋의 AffectedLayerMask 값에 영향!
 *       새 비트 추가 시 PostLoad()에서 마이그레이션 코드 필요.
 *       (Other 비트 추가 + PostLoad 마이그레이션 구현)
 */
UENUM(BlueprintType, Meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class EFleshRingLayerMask : uint8
{
	None       = 0        UMETA(Hidden),
	Skin       = 1 << 0,  // 0x01
	Stocking   = 1 << 1,  // 0x02
	Underwear  = 1 << 2,  // 0x04
	Outerwear  = 1 << 3,  // 0x08
	Other      = 1 << 4,  // 0x10 - 미분류 레이어
	All        = Skin | Stocking | Underwear | Outerwear | Other UMETA(Hidden)
};
ENUM_CLASS_FLAGS(EFleshRingLayerMask);

/**
 * 노멀 재계산 방식
 * TBN 정확성 vs 부드러움 트레이드오프
 */
UENUM(BlueprintType)
enum class ENormalRecomputeMethod : uint8
{
	/**
	 * Geometric Normal (Face Normal 평균)
	 * - 실제 변형된 지오메트리에서 노멀 계산
	 * - TBN이 표면과 정확히 일치 → Normal Map 변환 정확
	 */
	Geometric	UMETA(DisplayName = "Geometric (TBN Accurate)"),

	/**
	 * Surface Rotation (기존 방식)
	 * - 원본 Smooth Normal을 면 회전량만큼 회전
	 * - Smooth Normal의 "캐릭터" 보존
	 * - 변형이 noisy하면 결과도 noisy
	 */
	SurfaceRotation	UMETA(DisplayName = "Surface Rotation"),

	/**
	 * [DEPRECATED] Polar Decomposition
	 * - FleshRing의 작은 symmetric 변형에서는 SurfaceRotation과 차이 없음 (< 0.5도)
	 * - 코드 복잡도 대비 실질적 이득 없어 deprecated 처리
	 * - 향후 버전에서 제거 예정
	 *
	 * (원본 설명)
	 * - Deformation Gradient에서 순수 회전(R) 성분만 추출
	 * - 원본 Smooth Normal에 R 적용
	 * - Scale/Shear 영향 없이 정확한 회전
	 */
	PolarDecomposition	UMETA(DisplayName = "Polar Decomposition (DEPRECATED)", Hidden)
};

/**
 * 탄젠트 재계산 방식
 */
UENUM(BlueprintType)
enum class ETangentRecomputeMethod : uint8
{
	/**
	 * Gram-Schmidt Orthonormalization
	 * - 재계산된 노멀에 원본 탄젠트를 직교화
	 * - T' = T - (T·N)N, normalize(T')
	 * - FleshRing의 symmetric 변형에서 충분히 정확
	 */
	GramSchmidt	UMETA(DisplayName = "Gram-Schmidt"),

	/**
	 * [DEPRECATED] Polar Decomposition
	 * - FleshRing은 symmetric 변형이라 twist가 없음
	 * - GramSchmidt와 차이 없음 (< 0.1도)
	 * - 코드 복잡도 대비 실질적 이득 없어 deprecated 처리
	 * - 향후 버전에서 제거 예정
	 *
	 * (원본 설명)
	 * - Deformation Gradient에서 순수 회전(R) 추출
	 * - 원본 탄젠트에 R 적용 후 Gram-Schmidt로 마무리
	 */
	PolarDecomposition	UMETA(DisplayName = "Polar Decomposition (DEPRECATED)", Hidden)
};

// =====================================
// 구조체 정의
// =====================================

/**
 * Subdivision 설정 (에디터 프리뷰 + 런타임)
 * UFleshRingAsset에서 사용되며, IPropertyTypeCustomization으로 그룹화 가능
 */
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FSubdivisionSettings
{
	GENERATED_BODY()

	// ===== 공통 설정 =====

	/** Subdivision 활성화 (Low-Poly 메시용) */
	// Undo/Redo 지원 (메시 작업은 GUndo=nullptr로 보호되어 GC 문제 없음)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeletal Mesh Detail Settings")
	bool bEnableSubdivision = false;

	/** 최소 엣지 길이 cm (이보다 작으면 subdivision 중단) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeletal Mesh Detail Settings", meta = (ClampMin = "0.1", DisplayName = "Min Edge Length"))
	float MinEdgeLength = 1.0f;

	// ===== 에디터 프리뷰 설정 =====

	/** 에디터 프리뷰용 Subdivision 레벨 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeletal Mesh Detail Settings", meta = (ClampMin = "1", ClampMax = "4"))
	int32 PreviewSubdivisionLevel = 2;

	/**
	 * 이웃 본 탐색 깊이 (0 = 타겟 본만, 1 = 부모+자식, 2 = 조부모+손자 포함)
	 * 0이면 BoneWeightThreshold만으로 영역 판단
	 * 높을수록 subdivision 영역이 넓어지지만 성능 비용 증가
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeletal Mesh Detail Settings", meta = (ClampMin = "0", ClampMax = "3", DisplayName = "Bone Search Depth"))
	int32 PreviewBoneHopCount = 1;

	/**
	 * 본 가중치 임계값 (0.0-1.0)
	 * 이 값 이상의 영향을 받는 버텍스만 subdivision 대상
	 * 높을수록 subdivision 영역이 좁아져 성능 향상
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeletal Mesh Detail Settings", meta = (ClampMin = "0.01", ClampMax = "0.7", DisplayName = "Min Bone Influence"))
	float PreviewBoneWeightThreshold = 0.1f;

	// ===== 런타임 설정 =====

	/** 최대 Subdivision 레벨 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Skeletal Mesh Detail Settings", meta = (ClampMin = "1", ClampMax = "6", EditCondition = "bEnableSubdivision"))
	int32 MaxSubdivisionLevel = 4;

	// ===== 생성된 메시 (런타임) =====

	/**
	 * Subdivision된 SkeletalMesh (에셋 안에 내장됨)
	 * GenerateSubdividedMesh()로 생성됨 - 런타임용 (Ring 영역만 subdivision)
	 * NonTransactional: Undo 시스템에서 제외하여 GC 문제 방지
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Skeletal Mesh Detail Settings", meta = (EditCondition = "bEnableSubdivision"), NonTransactional)
	TObjectPtr<USkeletalMesh> SubdividedMesh;

	/** Subdivision 생성 시점의 파라미터 해시 (재생성 필요 여부 판단용) */
	UPROPERTY()
	uint32 SubdivisionParamsHash = 0;

	// ===== 베이크된 메시 (런타임용, 변형 적용 완료) =====

	/**
	 * 변형이 적용된 베이크 메시 (런타임용)
	 * Tightness + Bulge + Smoothing이 모두 적용된 최종 상태
	 * GenerateBakedMesh()로 생성됨
	 * NonTransactional: Undo 시스템에서 제외하여 GC 문제 방지
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Baked Mesh", NonTransactional)
	TObjectPtr<USkeletalMesh> BakedMesh;

	/**
	 * 베이크된 Ring 트랜스폼 배열 (링 메시 배치용)
	 * 런타임에서 이 위치에 Ring 메시를 배치
	 */
	UPROPERTY()
	TArray<FTransform> BakedRingTransforms;

	/**
	 * 베이크 시점의 파라미터 해시
	 * Ring 설정, Tightness, Bulge 등 모든 파라미터 포함
	 * 재생성 필요 여부 판단용
	 */
	UPROPERTY()
	uint32 BakeParamsHash = 0;

	FSubdivisionSettings()
		: bEnableSubdivision(false)
		, MinEdgeLength(1.0f)
		, PreviewSubdivisionLevel(2)
		, PreviewBoneHopCount(1)
		, PreviewBoneWeightThreshold(0.1f)
		, MaxSubdivisionLevel(4)
		, SubdividedMesh(nullptr)
		, SubdivisionParamsHash(0)
		, BakedMesh(nullptr)
		, BakeParamsHash(0)
	{
	}
};

/**
 * 머티리얼-레이어 매핑 (침투 해결용)
 * 각 머티리얼이 어느 레이어에 속하는지 정의
 * 스타킹이 항상 살 위에 렌더링되도록 보장
 */
USTRUCT(BlueprintType)
struct FLESHRINGRUNTIME_API FMaterialLayerMapping
{
	GENERATED_BODY()

	/** 대상 머티리얼 슬롯 인덱스 (자동 설정됨, 수정 불가) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Layer")
	int32 MaterialSlotIndex = 0;

	/** 머티리얼 슬롯 이름 (표시용, 자동 설정됨) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Layer")
	FName MaterialSlotName;

	/** 레이어 타입 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layer")
	EFleshRingLayerType LayerType = EFleshRingLayerType::Other;

	FMaterialLayerMapping()
		: MaterialSlotIndex(0)
		, MaterialSlotName(NAME_None)
		, LayerType(EFleshRingLayerType::Other)
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
struct FLESHRINGRUNTIME_API FVirtualBandSection
{
	GENERATED_BODY()

	/** 해당 섹션의 끝단 반경 (MidRadius와의 차이로 경사 결정) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Virtual Band", meta = (ClampMin = "0.1"))
	float Radius = 10.0f;

	/** 해당 섹션의 높이 (경사 구간 길이) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Virtual Band", meta = (ClampMin = "0.0"))
	float Height = 2.0f;

	FVirtualBandSection()
		: Radius(10.0f)
		, Height(2.0f)
	{
	}

	FVirtualBandSection(float InRadius, float InHeight)
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
struct FLESHRINGRUNTIME_API FVirtualBandSettings
{
	GENERATED_BODY()

	// ===== 밴드 트랜스폼 =====

	/** Bone 기준 밴드 위치 오프셋 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform")
	FVector BandOffset = FVector::ZeroVector;

	/** 밴드 회전 (Euler, UI 편집용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Transform", meta = (DisplayName = "Band Rotation"))
	FRotator BandEulerRotation = FRotator(-90.0f, 0.0f, 0.0f);

	/** 밴드 회전 (Quaternion, 내부 계산용) */
	UPROPERTY()
	FQuat BandRotation = FQuat(FRotator(-90.0f, 0.0f, 0.0f));

	// ===== 밴드 본체 (조임 지점) =====

	/** 밴드 상단 반경 (Upper Section과 만나는 지점) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band", meta = (ClampMin = "0.1", DisplayName = "Band Top Radius"))
	float MidUpperRadius = 8.0f;

	/** 밴드 하단 반경 (Lower Section과 만나는 지점) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band", meta = (ClampMin = "0.1", DisplayName = "Band Bottom Radius"))
	float MidLowerRadius = 8.0f;

	/** 밴드 본체 높이 (조이는 영역) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band", meta = (ClampMin = "0.1"))
	float BandHeight = 2.0f;

	/** 밴드 두께 (벽 두께, SDF 생성용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band", meta = (ClampMin = "0.1"))
	float BandThickness = 1.0f;

	// ===== 상단 섹션 (살이 불룩해지는 영역) =====

	/** Upper.Radius > MidUpperRadius → 위로 벌어지며 살이 불룩해짐 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Upper Bulge Zone", meta = (DisplayName = "Upper Bulge Zone"))
	FVirtualBandSection Upper;

	// ===== 하단 섹션 (스타킹이 덮는 영역) =====

	/** Lower.Radius ≥ MidLowerRadius → 아래로 벌어지며 스타킹이 덮음 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lower Bulge Zone", meta = (DisplayName = "Lower Bulge Zone"))
	FVirtualBandSection Lower;

	// ===== 메시 생성 품질 =====

	/** 원형 단면 세그먼트 수 (높을수록 부드러움) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quality", meta = (ClampMin = "8", ClampMax = "64"))
	int32 RadialSegments = 32;

	/** 높이당 세그먼트 수 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Quality", meta = (ClampMin = "1", ClampMax = "16"))
	int32 HeightSegments = 4;

	FVirtualBandSettings()
		: BandOffset(FVector::ZeroVector)
		, BandEulerRotation(FRotator(-90.0f, 0.0f, 0.0f))
		, BandRotation(FQuat(FRotator(-90.0f, 0.0f, 0.0f)))
		, MidUpperRadius(8.0f)        // 밴드 상단 반경
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

	/**
	 * Mid Band 중심 기준 Z 오프셋 반환
	 * 새 좌표계: Z=0이 Mid Band 중심
	 * 내부 좌표계: Z=0이 Lower 하단
	 * 변환: InternalZ = LocalZ + GetMidOffset()
	 */
	float GetMidOffset() const
	{
		return Lower.Height + BandHeight * 0.5f;
	}

	/**
	 * Catmull-Rom 스플라인으로 높이별 반경 계산
	 * 4개 제어점(Lower.Radius → MidLowerRadius → MidUpperRadius → Upper.Radius)을
	 * 부드러운 곡선으로 연결
	 *
	 * 좌표계: Z=0이 Mid Band 중심 (조이는 부분의 중심)
	 *   - Z > 0: 상단 방향 (Upper 섹션)
	 *   - Z < 0: 하단 방향 (Lower 섹션)
	 *   - Z = -BandHeight/2: Band 하단 경계 (MidLowerRadius)
	 *   - Z = +BandHeight/2: Band 상단 경계 (MidUpperRadius)
	 *
	 * @param LocalZ - 밴드 로컬 좌표계에서의 높이 (0 = Mid Band 중심)
	 * @return 해당 높이에서의 반경
	 */
	float GetRadiusAtHeight(float LocalZ) const
	{
		const float TotalHeight = GetTotalHeight();
		if (TotalHeight <= KINDA_SMALL_NUMBER)
		{
			return MidLowerRadius;
		}

		// 새 좌표계 → 내부 좌표계 변환
		// 내부: Z=0이 Lower 하단, Z=TotalHeight가 Upper 상단
		const float MidOffset = GetMidOffset();
		const float InternalZ = LocalZ + MidOffset;

		// 4개 제어점 (내부 좌표계 높이, 반경)
		const float H[4] = { 0.0f, Lower.Height, Lower.Height + BandHeight, TotalHeight };
		const float R[4] = { Lower.Radius, MidLowerRadius, MidUpperRadius, Upper.Radius };

		// InternalZ 클램프 (내부 좌표계 범위)
		const float Z = FMath::Clamp(InternalZ, 0.0f, TotalHeight);

		// 어느 구간인지 찾기 (0: H0~H1, 1: H1~H2, 2: H2~H3)
		int32 Segment = 0;
		if (Z >= H[2]) Segment = 2;
		else if (Z >= H[1]) Segment = 1;

		// 구간 내 정규화된 t 계산
		const float SegmentStart = H[Segment];
		const float SegmentEnd = H[Segment + 1];
		const float SegmentLength = SegmentEnd - SegmentStart;
		const float t = (SegmentLength > KINDA_SMALL_NUMBER) ? (Z - SegmentStart) / SegmentLength : 0.0f;

		// Catmull-Rom에 필요한 4개 반경 (P0, P1, P2, P3)
		// P1~P2 구간을 보간, P0과 P3는 이웃 제어점 (끝점은 복제)
		float P0, P1, P2, P3;
		if (Segment == 0)      { P0 = R[0]; P1 = R[0]; P2 = R[1]; P3 = R[2]; }
		else if (Segment == 1) { P0 = R[0]; P1 = R[1]; P2 = R[2]; P3 = R[3]; }
		else                   { P0 = R[1]; P1 = R[2]; P2 = R[3]; P3 = R[3]; }

		// Catmull-Rom 스플라인 계산
		const float t2 = t * t;
		const float t3 = t2 * t;
		const float Result = 0.5f * (
			(2.0f * P1) +
			(-P0 + P2) * t +
			(2.0f * P0 - 5.0f * P1 + 4.0f * P2 - P3) * t2 +
			(-P0 + 3.0f * P1 - 3.0f * P2 + P3) * t3
		);

		// 오버슈트 방지 클램프
		const float MinRadius = FMath::Min(FMath::Min(R[0], R[1]), FMath::Min(R[2], R[3]));
		const float MaxRadius = FMath::Max(FMath::Max(R[0], R[1]), FMath::Max(R[2], R[3]));
		return FMath::Clamp(Result, MinRadius, MaxRadius);
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (DisplayName = "Effect Range Mode"))
	EFleshRingInfluenceMode InfluenceMode = EFleshRingInfluenceMode::MeshBased;

	/** 에디터에서 Ring 가시성 (Mesh, Gizmo, Debug 포함) - 눈 아이콘으로만 제어 */
	UPROPERTY()
	bool bEditorVisible = true;

	/** Ring 반지름 (VirtualRing 모드에서만 사용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::VirtualRing", ClampMin = "0.1", ClampMax = "100.0"))
	float RingRadius = 5.0f;

	/** 링 두께 - 반경 방향 벽 두께 (안쪽→바깥쪽) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::VirtualRing", ClampMin = "0.1", ClampMax = "20.0"))
	float RingThickness = 1.0f;

	/** 링 높이 - 축 방향 전체 높이 (위아래 각각 RingHeight/2) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::VirtualRing", ClampMin = "0.1", ClampMax = "50.0"))
	float RingHeight = 2.0f;

	/** [DEPRECATED] RingWidth → RingHeight 마이그레이션용 */
	UPROPERTY()
	float RingWidth_DEPRECATED = 0.0f;

	/** Bone 기준 Ring 위치 오프셋 (변형 영역) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::VirtualRing"))
	FVector RingOffset = FVector::ZeroVector;

	/** Ring 회전 Euler 각도 (UI 편집용, 제한 없음) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::VirtualRing", DisplayName = "Ring Rotation"))
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

	/** Bulge 수직 확산 범위 (Ring 높이 대비 배수, 위아래로 얼마나 퍼지는지) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "1.0", ClampMax = "8.0", DisplayName = "Bulge Vertical Spread"))
	float BulgeAxialRange = 5.0f;

	/** Bulge 수평 확산 범위 (Ring 반지름 대비 배수, 옆으로 얼마나 퍼지는지) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "1.0", ClampMax = "3.0", DisplayName = "Bulge Horizontal Spread"))
	float BulgeRadialRange = 1.0f;

	/** 상단 Bulge 강도 배수 (1.0 = 기본, 0.0 = 비활성) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "0.0", ClampMax = "2.0"))
	float UpperBulgeStrength = 1.0f;

	/** 하단 Bulge 강도 배수 (1.0 = 기본, 0.0 = 비활성, 스타킹 효과용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "0.0", ClampMax = "2.0"))
	float LowerBulgeStrength = 1.0f;

	/**
	 * Bulge 방향 비율 (0 = 위아래로만, 1 = 바깥으로만, 0.7 = 기본)
	 * 0.0: 위아래(Axial) 방향으로만 팽창
	 * 1.0: 바깥(Radial) 방향으로만 팽창
	 * 0.7: 기본값 - 바깥 방향 70%, 위아래 30%
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (EditCondition = "bEnableBulge", ClampMin = "0.0", ClampMax = "1.0", DisplayName = "Bulge Direction Bias"))
	float BulgeRadialRatio = 0.7f;

	/** 조이기 강도 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.0", ClampMax = "3.0"))
	float TightnessStrength = 1.0f;

	/**
	 * 효과 바운드 X 방향 확장 cm (Mesh Based 모드 전용)
	 * SDF 텍스처 생성 및 버텍스 필터링 바운드를 X 방향으로 확장
	 * 작은 Ring이 더 큰 영역을 커버해야 할 때 사용
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.0", ClampMax = "10.0", DisplayName = "Effect Bounds Expand X"))
	float SDFBoundsExpandX = 0.0f;

	/**
	 * 효과 바운드 Y 방향 확장 cm (Mesh Based 모드 전용)
	 * SDF 텍스처 생성 및 버텍스 필터링 바운드를 Y 방향으로 확장
	 * 작은 Ring이 더 큰 영역를 커버해야 할 때 사용
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (ClampMin = "0.0", ClampMax = "10.0", DisplayName = "Effect Bounds Expand Y"))
	float SDFBoundsExpandY = 0.0f;

	/** 감쇠 곡선 타입 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring", meta = (DisplayName = "Tightness Falloff"))
	EFalloffType FalloffType = EFalloffType::Linear;

	/**
	 * Tightness 효과가 적용될 메시 레이어
	 * 선택되지 않은 레이어의 버텍스는 영향 범위에 있어도 수집되지 않음
	 * 기본값: Skin | Other (미분류 포함하여 "일단 동작하게")
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ring",
		meta = (DisplayName = "Target Material Layers", Bitmask, BitmaskEnum = "/Script/FleshRingRuntime.EFleshRingLayerMask"))
	int32 AffectedLayerMask = static_cast<int32>(EFleshRingLayerMask::Skin) | static_cast<int32>(EFleshRingLayerMask::Other);

	/** 가상 밴드 설정 (VirtualBand 모드에서만 사용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Virtual Band", meta = (EditCondition = "InfluenceMode == EFleshRingInfluenceMode::VirtualBand"))
	FVirtualBandSettings VirtualBand;

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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased", EditConditionHides))
	EFalloffType HopFalloffType = EFalloffType::Hermite;

	/** 스무딩 영역 상단 확장 거리 (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::BoundsExpand", EditConditionHides, ClampMin = "0.0", ClampMax = "50.0", DisplayName = "Bounds Expand Top (cm)"))
	float SmoothingBoundsZTop = 5.0f;

	/** 스무딩 영역 하단 확장 거리 (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Post Process", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::BoundsExpand", EditConditionHides, ClampMin = "0.0", ClampMax = "50.0", DisplayName = "Bounds Expand Bottom (cm)"))
	float SmoothingBoundsZBottom = 0.0f;

	// ===== Heat Propagation (변형 전파) =====

	/**
	 * Heat Propagation 활성화
	 * - Seed(직접 변형된 버텍스)의 delta를 Extended 영역으로 확산
	 * - Tightness 직후, Radial/Laplacian 전에 실행
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Heat Propagation", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased", EditConditionHides))
	bool bEnableHeatPropagation = true;

	/**
	 * Heat Propagation 반복 횟수
	 * - 높을수록 더 넓게 확산 (권장: 5~20)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Heat Propagation", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased && bEnableHeatPropagation", EditConditionHides, ClampMin = "1", ClampMax = "50"))
	int32 HeatPropagationIterations = 10;

	/**
	 * Heat Propagation Lambda (확산 계수)
	 * - 각 반복에서 이웃 평균과 블렌딩하는 비율
	 * - 0.5 권장, 높을수록 빠른 확산
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Heat Propagation", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased && bEnableHeatPropagation", EditConditionHides, ClampMin = "0.1", ClampMax = "0.9"))
	float HeatPropagationLambda = 0.5f;

	/**
	 * Bulge 버텍스도 Heat Propagation Seed로 포함
	 * - true: Tightness + Bulge 변형 모두 전파
	 * - false: Tightness 변형만 전파
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Heat Propagation", meta = (EditCondition = "bEnablePostProcess && SmoothingVolumeMode == ESmoothingVolumeMode::HopBased && bEnableHeatPropagation", EditConditionHides))
	bool bIncludeBulgeVerticesAsSeeds = true;

	// ===== Smoothing (스무딩 전체 제어) =====

	/** 스무딩 활성화 (Radial, Laplacian/Taubin 모든 스무딩 제어) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess"))
	bool bEnableSmoothing = true;

	// ===== Radial Smoothing (반경 균일화) =====

	/** 반경 균일화 스무딩 활성화 (같은 높이의 버텍스들이 동일한 반경을 갖도록) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableSmoothing", EditConditionHides))
	bool bEnableRadialSmoothing = true;

	/**
	 * 반경 균일화 강도
	 * - 0.0: 효과 없음 (원본 유지)
	 * - 1.0: 완전 균일화 (같은 높이의 버텍스들이 동일한 반경)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableSmoothing && bEnableRadialSmoothing", EditConditionHides, ClampMin = "0.0", ClampMax = "1.0"))
	float RadialBlendStrength = 1.0f;

	/**
	 * 반경 균일화 슬라이스 높이 (cm)
	 * - 같은 슬라이스 내 버텍스들이 동일 반경으로 처리됨
	 * - 고밀도 메시: 작은 값 (0.5cm), 저밀도 메시: 큰 값 (2cm)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableSmoothing && bEnableRadialSmoothing", EditConditionHides, ClampMin = "0.1", ClampMax = "10.0"))
	float RadialSliceHeight = 1.0f;

	// ===== Laplacian/Taubin Smoothing 설정 =====

	/** Laplacian 스무딩 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableSmoothing", EditConditionHides))
	bool bEnableLaplacianSmoothing = true;

	/**
	 * Laplacian 스무딩 알고리즘 선택
	 * - Laplacian: 일반 Laplacian (반복 시 수축 발생)
	 * - Taubin: λ-μ 스무딩 (수축 없이 부드럽게, 권장)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableSmoothing && bEnableLaplacianSmoothing", EditConditionHides))
	ELaplacianSmoothingType LaplacianSmoothingType = ELaplacianSmoothingType::Taubin;

	/**
	 * 스무딩 강도 λ (Taubin: 수축 단계 강도)
	 * 권장: 0.3~0.7, 기본값 0.5
	 * 경고: 0.8 초과 시 수치 불안정 (비늘 현상)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableSmoothing && bEnableLaplacianSmoothing", EditConditionHides, ClampMin = "0.1", ClampMax = "0.8", UIMin = "0.1", UIMax = "0.8"))
	float SmoothingLambda = 0.5f;

	/**
	 * Taubin 팽창 강도 μ (음수값)
	 * 조건: |μ| > λ, 0이면 자동 계산
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableSmoothing && bEnableLaplacianSmoothing && LaplacianSmoothingType == ELaplacianSmoothingType::Taubin", EditConditionHides, ClampMin = "-1.0", ClampMax = "0.0"))
	float TaubinMu = -0.53f;

	/** 스무딩 반복 횟수 (Taubin: 각 반복 = λ+μ 2패스) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing", meta = (EditCondition = "bEnablePostProcess && bEnableSmoothing && bEnableLaplacianSmoothing", EditConditionHides, ClampMin = "1", ClampMax = "20"))
	int32 SmoothingIterations = 2;

	/**
	 * 변형된 버텍스 앵커 모드
	 * - true: Tightness로 직접 변형된 버텍스(원본 Affected)는 고정, 확장 영역만 스무딩
	 * - false: 모든 버텍스에 Influence 비례 스무딩 (기존 동작)
	 *
	 * 앵커 판정 기준: 원본 Affected Vertices 멤버십
	 * - Hop-based: Seed 버텍스 (Hop=0) → 앵커
	 * - Z-based: 원본 SDF AABB 내 버텍스 → 앵커
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Smoothing|Laplacian", meta = (EditCondition = "bEnablePostProcess && bEnableSmoothing && bEnableLaplacianSmoothing", EditConditionHides))
	bool bAnchorDeformedVertices = false;

	// ===== PBD Edge Constraint 설정 =====

	/**
	 * PBD Edge Constraint 활성화 (변형 전파)
	 * - 조이기로 인한 변형이 스무딩 볼륨 전체로 퍼지도록 함
	 * - "역 PBD": 변형량이 큰 버텍스는 고정, 작은 버텍스는 자유롭게 이동
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PBD Edge Constraint", meta = (EditCondition = "bEnablePostProcess"))
	bool bEnablePBDEdgeConstraint = false;

	/** PBD 제약 강도 (0.0 ~ 1.0), 권장: 0.5 ~ 0.9 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PBD Edge Constraint", meta = (EditCondition = "bEnablePostProcess && bEnablePBDEdgeConstraint", EditConditionHides, ClampMin = "0.0", ClampMax = "1.0"))
	float PBDStiffness = 0.8f;

	/** PBD 반복 횟수, 권장: 3 ~ 10 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PBD Edge Constraint", meta = (EditCondition = "bEnablePostProcess && bEnablePBDEdgeConstraint", EditConditionHides, ClampMin = "1", ClampMax = "100"))
	int32 PBDIterations = 5;

	/** PBD 허용 오차 비율 (0.0 ~ 0.5)
	 *  이 범위 내의 변형은 유지됨 (데드존)
	 *  예: 0.2 → 원래 길이의 80%~120% 범위는 보정하지 않음
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PBD Edge Constraint", meta = (EditCondition = "bEnablePostProcess && bEnablePBDEdgeConstraint", EditConditionHides, ClampMin = "0.0", ClampMax = "0.5"))
	float PBDTolerance = 0.2f;

	/** Affected Vertices(Tightness 영역)를 앵커로 고정할지 여부
	 *  ON: Affected Vertices는 PBD에서 고정점으로 동작 (기본값)
	 *  OFF: Affected Vertices도 PBD 보정 대상으로 포함 (모든 버텍스가 자유롭게 움직임)
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PBD Edge Constraint", meta = (EditCondition = "bEnablePostProcess && bEnablePBDEdgeConstraint", EditConditionHides))
	bool bPBDAnchorAffectedVertices = true;

	FFleshRingSettings()
		: BoneName(NAME_None)
		, InfluenceMode(EFleshRingInfluenceMode::MeshBased)
		, RingRadius(5.0f)
		, RingThickness(1.0f)
		, RingHeight(2.0f)
		, RingWidth_DEPRECATED(0.0f)
		, bEnableBulge(true)
		, BulgeFalloff(EFleshRingFalloffType::WendlandC2)
		, BulgeIntensity(1.0f)
		, BulgeAxialRange(5.0f)
		, BulgeRadialRange(1.0f)
		, UpperBulgeStrength(1.0f)
		, LowerBulgeStrength(1.0f)
		, TightnessStrength(1.0f)
		, FalloffType(EFalloffType::Linear)
		, AffectedLayerMask(static_cast<int32>(EFleshRingLayerMask::Skin) | static_cast<int32>(EFleshRingLayerMask::Other))
		, bEnablePostProcess(true)
		, SmoothingVolumeMode(ESmoothingVolumeMode::BoundsExpand)
		, MaxSmoothingHops(5)
		, HopFalloffType(EFalloffType::Hermite)
		, SmoothingBoundsZTop(5.0f)
		, SmoothingBoundsZBottom(0.0f)
		, bEnableHeatPropagation(true)
		, HeatPropagationIterations(10)
		, HeatPropagationLambda(0.5f)
		, bIncludeBulgeVerticesAsSeeds(true)
		, bEnableSmoothing(true)
		, bEnableRadialSmoothing(true)
		, bEnableLaplacianSmoothing(true)
		, LaplacianSmoothingType(ELaplacianSmoothingType::Taubin)
		, SmoothingLambda(0.5f)
		, TaubinMu(-0.53f)
		, SmoothingIterations(2)
		, bEnablePBDEdgeConstraint(false)
		, PBDStiffness(0.8f)
		, PBDIterations(5)
		, PBDTolerance(0.2f)
		, bPBDAnchorAffectedVertices(true)
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

	/**
	 * 레이어 타입이 AffectedLayerMask에 포함되는지 확인
	 * @param LayerType 확인할 레이어 타입
	 * @return true면 해당 레이어의 버텍스가 Tightness 영향을 받음
	 */
	bool IsLayerAffected(EFleshRingLayerType LayerType) const
	{
		switch (LayerType)
		{
		case EFleshRingLayerType::Skin:
			return (AffectedLayerMask & static_cast<int32>(EFleshRingLayerMask::Skin)) != 0;
		case EFleshRingLayerType::Stocking:
			return (AffectedLayerMask & static_cast<int32>(EFleshRingLayerMask::Stocking)) != 0;
		case EFleshRingLayerType::Underwear:
			return (AffectedLayerMask & static_cast<int32>(EFleshRingLayerMask::Underwear)) != 0;
		case EFleshRingLayerType::Outerwear:
			return (AffectedLayerMask & static_cast<int32>(EFleshRingLayerMask::Outerwear)) != 0;
		case EFleshRingLayerType::Other:
			return (AffectedLayerMask & static_cast<int32>(EFleshRingLayerMask::Other)) != 0;
		case EFleshRingLayerType::Exclude:
			// Exclude는 마스크와 무관하게 항상 제외
			return false;
		default:
			// NOTE: 새 레이어 타입 추가 시 여기 도달하면 case 추가 필요
			return false;
		}
	}
};
