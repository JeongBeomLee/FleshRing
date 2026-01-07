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
