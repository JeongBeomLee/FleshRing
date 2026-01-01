// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Math/Vector.h"

/**
 * Bulge 영역 계산 인터페이스
 * SDF 기반, Manual 모드 등 다양한 방식 지원
 */
class IBulgeRegionProvider
{
public:
	virtual ~IBulgeRegionProvider() = default;

	virtual void CalculateBulgeRegion(
		const TArrayView<const FVector3f>& AllVertexPositions,
		TArray<uint32>& OutBulgeVertexIndices,
		TArray<float>& OutBulgeInfluences,
		TArray<FVector3f>& OutBulgeDirections
	) const = 0;
};

/**
 * CPU에서 계산된 Bulge 데이터
 */
struct FBulgeRegionData
{
	TArray<uint32> VertexIndices;
	TArray<float> Influences;
	TArray<FVector3f> Directions;
	float BulgeStrength = 1.0f;
	float MaxBulgeDistance = 10.0f;

	bool IsValid() const
	{
		return VertexIndices.Num() > 0 &&
			   VertexIndices.Num() == Influences.Num() &&
			   VertexIndices.Num() == Directions.Num();
	}

	int32 Num() const { return VertexIndices.Num(); }

	void Reset()
	{
		VertexIndices.Reset();
		Influences.Reset();
		Directions.Reset();
	}
};
