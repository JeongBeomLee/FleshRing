// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FleshRingBulgeTypes.h"
#include "FleshRingFalloff.h"

/**
 * SDF 바운드 기반 Bulge 영역 계산
 * CPU: 버텍스 필터링 + 영향도 계산
 * GPU: 실제 Bulge 방향/변형 적용
 */
class FLESHRINGRUNTIME_API FSDFBulgeProvider : public IBulgeRegionProvider
{
public:
	FVector3f SDFBoundsMin = FVector3f::ZeroVector;	// SDF Local
	FVector3f SDFBoundsMax = FVector3f::ZeroVector;	// SDF Local
	FTransform LocalToComponent = FTransform::Identity;

	float AxialRange = 3.0f;	// 축 방향(위아래) 범위 배율
	float RadialRange = 1.5f;	// 반경 방향(옆) 범위 배율
	EFleshRingFalloffType FalloffType = EFleshRingFalloffType::WendlandC2;	// Falloff 커브 타입, FleshRingDeformerInstance가 주입

public:
	FSDFBulgeProvider() = default;

	void InitFromSDFCache(
		const FVector3f& InBoundsMin,
		const FVector3f& InBoundsMax,
		const FTransform& InLocalToComponent,
		float InAxialRange = 3.0f,
		float InRadialRange = 1.5f);

	// IBulgeRegionProvider
	virtual void CalculateBulgeRegion(
		const TArrayView<const FVector3f>& AllVertexPositions,
		const FVertexSpatialHash* SpatialHash,
		TArray<uint32>& OutBulgeVertexIndices,
		TArray<float>& OutBulgeInfluences,
		TArray<FVector3f>& OutBulgeDirections
	) const override;

private:
	/** Ring 축 감지 (가장 짧은 SDF 바운드 축) */
	FVector3f DetectRingAxis() const;

	/** Bulge 영역의 확장된 AABB 계산 (Spatial Hash 쿼리용) */
	void CalculateExpandedBulgeAABB(FVector& OutMin, FVector& OutMax) const;
};

/**
 * VirtualRing 모드용 Bulge 영역 계산
 * Ring 파라미터(RingCenter, RingAxis, RingRadius, RingHeight)를 직접 받아서
 * SDF 없이 Bulge 영역을 계산
 *
 * FSDFBulgeProvider와의 차이점:
 * - 좌표계: Component Space 직접 사용 (LocalToComponent 변환 없음)
 * - Ring 정보: 직접 입력받음 (SDF 바운드에서 추론 안 함)
 * - Ring 축: 직접 입력받음 (자동 감지 안 함)
 */
class FLESHRINGRUNTIME_API FVirtualRingBulgeProvider : public IBulgeRegionProvider
{
public:
	// Ring 기하 정보 (Component Space)
	FVector3f RingCenter = FVector3f::ZeroVector;
	FVector3f RingAxis = FVector3f(0.0f, 0.0f, 1.0f);
	float RingRadius = 5.0f;
	float RingHeight = 2.0f;

	// Bulge 범위 파라미터
	float AxialRange = 3.0f;	// 축 방향(위아래) 범위 배율
	float RadialRange = 1.5f;	// 반경 방향(옆) 범위 배율
	EFleshRingFalloffType FalloffType = EFleshRingFalloffType::WendlandC2;

public:
	FVirtualRingBulgeProvider() = default;

	/**
	 * Ring 파라미터로 초기화
	 * @param InRingCenter - Ring 중심 (Component Space)
	 * @param InRingAxis - Ring 축 방향 (정규화된 벡터)
	 * @param InRingRadius - Ring 반경
	 * @param InRingHeight - Ring 폭 (축 방향 두께)
	 * @param InAxialRange - 축 방향 Bulge 범위 배율
	 * @param InRadialRange - 반경 방향 Bulge 범위 배율
	 */
	void InitFromRingParams(
		const FVector3f& InRingCenter,
		const FVector3f& InRingAxis,
		float InRingRadius,
		float InRingHeight,
		float InAxialRange = 3.0f,
		float InRadialRange = 1.5f);

	// IBulgeRegionProvider
	virtual void CalculateBulgeRegion(
		const TArrayView<const FVector3f>& AllVertexPositions,
		const FVertexSpatialHash* SpatialHash,
		TArray<uint32>& OutBulgeVertexIndices,
		TArray<float>& OutBulgeInfluences,
		TArray<FVector3f>& OutBulgeDirections
	) const override;

private:
	/** Bulge 영역의 확장된 AABB 계산 (Spatial Hash 쿼리용) */
	void CalculateExpandedBulgeAABB(FVector& OutMin, FVector& OutMax) const;
};

/**
 * Virtual Band(VirtualBand) 모드용 Bulge 영역 계산
 * SDF 없이 4-반경 가변 형상으로 Upper/Lower Section에서 Bulge 적용
 *
 * FVirtualRingBulgeProvider와의 차이점:
 * - 고정 반경 대신 GetRadiusAtHeight()로 가변 반경 사용
 * - Band Section(중간)은 Bulge 제외 (Tightness만 적용)
 * - Upper/Lower Section에서만 Bulge 적용
 */
class FLESHRINGRUNTIME_API FVirtualBandInfluenceProvider : public IBulgeRegionProvider
{
public:
	// 밴드 트랜스폼 (Component Space)
	FVector3f BandCenter = FVector3f::ZeroVector;
	FVector3f BandAxis = FVector3f(0.0f, 0.0f, 1.0f);

	// Virtual Band 형상 (4개 반경 + 3개 높이)
	float LowerRadius = 9.0f;
	float MidLowerRadius = 8.0f;
	float MidUpperRadius = 8.0f;
	float UpperRadius = 11.0f;
	float LowerHeight = 1.0f;
	float BandHeight = 2.0f;
	float UpperHeight = 2.0f;

	// Bulge 범위 파라미터
	float AxialRange = 3.0f;		// 축 방향(위아래) 범위 배율
	float RadialRange = 1.5f;		// 반경 방향(옆) 범위 배율
	EFleshRingFalloffType FalloffType = EFleshRingFalloffType::WendlandC2;

public:
	FVirtualBandInfluenceProvider() = default;

	/**
	 * Virtual Band 파라미터로 초기화
	 * @param InLowerRadius - 하단 끝 반경
	 * @param InMidLowerRadius - 밴드 하단 반경
	 * @param InMidUpperRadius - 밴드 상단 반경
	 * @param InUpperRadius - 상단 끝 반경
	 * @param InLowerHeight - Lower Section 높이
	 * @param InBandHeight - Band Section 높이
	 * @param InUpperHeight - Upper Section 높이
	 * @param InCenter - 밴드 중심 (Component Space)
	 * @param InAxis - 밴드 축 방향 (정규화된 벡터)
	 */
	void InitFromBandSettings(
		float InLowerRadius, float InMidLowerRadius,
		float InMidUpperRadius, float InUpperRadius,
		float InLowerHeight, float InBandHeight, float InUpperHeight,
		const FVector3f& InCenter, const FVector3f& InAxis,
		float InAxialRange = 3.0f, float InRadialRange = 1.5f);

	/** 높이별 가변 반경 계산 */
	float GetRadiusAtHeight(float LocalZ) const;

	/** 전체 높이 */
	float GetTotalHeight() const { return LowerHeight + BandHeight + UpperHeight; }

	// IBulgeRegionProvider
	virtual void CalculateBulgeRegion(
		const TArrayView<const FVector3f>& AllVertexPositions,
		const FVertexSpatialHash* SpatialHash,
		TArray<uint32>& OutBulgeVertexIndices,
		TArray<float>& OutBulgeInfluences,
		TArray<FVector3f>& OutBulgeDirections
	) const override;

private:
	/** Bulge 영역의 확장된 AABB 계산 (Spatial Hash 쿼리용) */
	void CalculateExpandedBulgeAABB(FVector& OutMin, FVector& OutMax) const;

	/** Falloff 계산 */
	float CalculateFalloff(float Distance, float MaxDistance) const;
};
