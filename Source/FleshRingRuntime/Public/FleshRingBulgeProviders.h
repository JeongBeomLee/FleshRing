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
 * Manual 모드용 Bulge 영역 계산
 * Ring 파라미터(RingCenter, RingAxis, RingRadius, RingWidth)를 직접 받아서
 * SDF 없이 Bulge 영역을 계산
 *
 * FSDFBulgeProvider와의 차이점:
 * - 좌표계: Component Space 직접 사용 (LocalToComponent 변환 없음)
 * - Ring 정보: 직접 입력받음 (SDF 바운드에서 추론 안 함)
 * - Ring 축: 직접 입력받음 (자동 감지 안 함)
 */
class FLESHRINGRUNTIME_API FManualBulgeProvider : public IBulgeRegionProvider
{
public:
	// Ring 기하 정보 (Component Space)
	FVector3f RingCenter = FVector3f::ZeroVector;
	FVector3f RingAxis = FVector3f(0.0f, 0.0f, 1.0f);
	float RingRadius = 5.0f;
	float RingWidth = 2.0f;

	// Bulge 범위 파라미터
	float AxialRange = 3.0f;	// 축 방향(위아래) 범위 배율
	float RadialRange = 1.5f;	// 반경 방향(옆) 범위 배율
	EFleshRingFalloffType FalloffType = EFleshRingFalloffType::WendlandC2;

public:
	FManualBulgeProvider() = default;

	/**
	 * Ring 파라미터로 초기화
	 * @param InRingCenter - Ring 중심 (Component Space)
	 * @param InRingAxis - Ring 축 방향 (정규화된 벡터)
	 * @param InRingRadius - Ring 반경
	 * @param InRingWidth - Ring 폭 (축 방향 두께)
	 * @param InAxialRange - 축 방향 Bulge 범위 배율
	 * @param InRadialRange - 반경 방향 Bulge 범위 배율
	 */
	void InitFromRingParams(
		const FVector3f& InRingCenter,
		const FVector3f& InRingAxis,
		float InRingRadius,
		float InRingWidth,
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
