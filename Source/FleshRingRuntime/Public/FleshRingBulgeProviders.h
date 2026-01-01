// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FleshRingBulgeTypes.h"
#include "FleshRingTypes.h"

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

	float BoundsExpansionRatio = 1.5f;	// 1.0 = 바운드 그대로, 1.5 = 50% 확장
	EFalloffType FalloffType = EFalloffType::Linear;

public:
	FSDFBulgeProvider() = default;

	void InitFromSDFCache(
		const FVector3f& InBoundsMin,
		const FVector3f& InBoundsMax,
		const FTransform& InLocalToComponent,
		float InBoundsExpansionRatio = 1.5f,
		EFalloffType InFalloffType = EFalloffType::Linear);

	// IBulgeRegionProvider
	virtual void CalculateBulgeRegion(
		const TArrayView<const FVector3f>& AllVertexPositions,
		TArray<uint32>& OutBulgeVertexIndices,
		TArray<float>& OutBulgeInfluences,
		TArray<FVector3f>& OutBulgeDirections
	) const override;

private:
	bool IsWithinExpandedBounds(const FVector3f& VertexPosLocal, const FVector3f& ExpandedMin, const FVector3f& ExpandedMax) const;
	float CalculateApproximateInfluence(const FVector3f& VertexPosLocal, const FVector3f& ExpandedMin, const FVector3f& ExpandedMax) const;
	float ApplyFalloff(float NormalizedDistance) const;
};
