// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingBulgeProviders.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingBulge, Log, All);

void FSDFBulgeProvider::InitFromSDFCache(
	const FVector3f& InBoundsMin,
	const FVector3f& InBoundsMax,
	const FTransform& InLocalToComponent,
	float InBoundsExpansionRatio,
	EFalloffType InFalloffType)
{
	SDFBoundsMin = InBoundsMin;
	SDFBoundsMax = InBoundsMax;
	LocalToComponent = InLocalToComponent;
	BoundsExpansionRatio = InBoundsExpansionRatio;
	FalloffType = InFalloffType;
}

void FSDFBulgeProvider::CalculateBulgeRegion(
	const TArrayView<const FVector3f>& AllVertexPositions,
	TArray<uint32>& OutBulgeVertexIndices,
	TArray<float>& OutBulgeInfluences,
	TArray<FVector3f>& OutBulgeDirections) const
{
	OutBulgeVertexIndices.Reset();
	OutBulgeInfluences.Reset();
	OutBulgeDirections.Reset();  // GPU에서 계산
	const FVector3f BoundsSize = SDFBoundsMax - SDFBoundsMin;
	if (BoundsSize.X <= KINDA_SMALL_NUMBER ||
		BoundsSize.Y <= KINDA_SMALL_NUMBER ||
		BoundsSize.Z <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogFleshRingBulge, Warning, TEXT("SDF 바운드가 유효하지 않음"));
		return;
	}

	// 확장된 바운드 계산
	const FVector3f BoundsCenter = (SDFBoundsMin + SDFBoundsMax) * 0.5f;
	const FVector3f HalfExtent = BoundsSize * 0.5f;
	const FVector3f ExpandedHalfExtent = HalfExtent * BoundsExpansionRatio;
	const FVector3f ExpandedMin = BoundsCenter - ExpandedHalfExtent;
	const FVector3f ExpandedMax = BoundsCenter + ExpandedHalfExtent;

	const FTransform ComponentToLocal = LocalToComponent.Inverse();
	const int32 NumVertices = AllVertexPositions.Num();
	OutBulgeVertexIndices.Reserve(NumVertices / 4);  // 대략 25% 예상
	OutBulgeInfluences.Reserve(NumVertices / 4);

	for (int32 i = 0; i < NumVertices; ++i)
	{
		const FVector3f& VertexPosComponent = AllVertexPositions[i];
		const FVector VertexPosLocalD = ComponentToLocal.TransformPosition(FVector(VertexPosComponent));
		const FVector3f VertexPosLocal = FVector3f(VertexPosLocalD);

		if (!IsWithinExpandedBounds(VertexPosLocal, ExpandedMin, ExpandedMax))
		{
			continue;
		}

		const float Influence = CalculateApproximateInfluence(
			VertexPosLocal, ExpandedMin, ExpandedMax);

		if (Influence > KINDA_SMALL_NUMBER)
		{
			OutBulgeVertexIndices.Add(static_cast<uint32>(i));
			OutBulgeInfluences.Add(Influence);
		}
	}

	UE_LOG(LogFleshRingBulge, Log, TEXT("Bulge 후보: %d/%d (%.1f%%)"),
		OutBulgeVertexIndices.Num(),
		NumVertices,
		NumVertices > 0 ? (100.0f * OutBulgeVertexIndices.Num() / NumVertices) : 0.0f);
}

bool FSDFBulgeProvider::IsWithinExpandedBounds(
	const FVector3f& VertexPosLocal,
	const FVector3f& ExpandedMin,
	const FVector3f& ExpandedMax) const
{
	return VertexPosLocal.X >= ExpandedMin.X && VertexPosLocal.X <= ExpandedMax.X &&
		   VertexPosLocal.Y >= ExpandedMin.Y && VertexPosLocal.Y <= ExpandedMax.Y &&
		   VertexPosLocal.Z >= ExpandedMin.Z && VertexPosLocal.Z <= ExpandedMax.Z;
}

float FSDFBulgeProvider::CalculateApproximateInfluence(
	const FVector3f& VertexPosLocal,
	const FVector3f& ExpandedMin,
	const FVector3f& ExpandedMax) const
{
	const FVector3f ExpandedCenter = (ExpandedMin + ExpandedMax) * 0.5f;
	const FVector3f ExpandedHalfExtent = (ExpandedMax - ExpandedMin) * 0.5f;
	const FVector3f ToVertex = VertexPosLocal - ExpandedCenter;

	float MaxNormalizedDist = 0.0f;
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		if (ExpandedHalfExtent[Axis] > KINDA_SMALL_NUMBER)
		{
			const float NormalizedDist = FMath::Abs(ToVertex[Axis]) / ExpandedHalfExtent[Axis];
			MaxNormalizedDist = FMath::Max(MaxNormalizedDist, NormalizedDist);
		}
	}

	const float RawInfluence = FMath::Clamp(1.0f - MaxNormalizedDist, 0.0f, 1.0f);
	return ApplyFalloff(RawInfluence);
}

float FSDFBulgeProvider::ApplyFalloff(float NormalizedDistance) const
{
	const float t = FMath::Clamp(NormalizedDistance, 0.0f, 1.0f);

	switch (FalloffType)
	{
	case EFalloffType::Linear:
		return t;

	case EFalloffType::Quadratic:
		return t * t;

	case EFalloffType::Hermite:
		// Smoothstep: 3t^2 - 2t^3
		return t * t * (3.0f - 2.0f * t);

	default:
		return t;
	}
}
