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

	// Ring 기하 정보 계산
	const FVector3f RingCenter = (SDFBoundsMin + SDFBoundsMax) * 0.5f;
	const FVector3f RingAxis = DetectRingAxis();

	// EffectWidth: Ring 축 방향 크기의 40% (GPU BulgeCS와 동일)
	const float MinBoundsSize = FMath::Min3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z);
	const float EffectWidth = MinBoundsSize * 0.4f;

	// 확장된 바운드 계산
	const FVector3f HalfExtent = BoundsSize * 0.5f;
	const FVector3f ExpandedHalfExtent = HalfExtent * BoundsExpansionRatio;
	const FVector3f ExpandedMin = RingCenter - ExpandedHalfExtent;
	const FVector3f ExpandedMax = RingCenter + ExpandedHalfExtent;

	const FTransform ComponentToLocal = LocalToComponent.Inverse();
	const int32 NumVertices = AllVertexPositions.Num();
	OutBulgeVertexIndices.Reserve(NumVertices / 10);  // Mexican Hat 필터링 후 ~10% 예상
	OutBulgeInfluences.Reserve(NumVertices / 10);

	int32 BoundsPassCount = 0;  // 디버그용

	for (int32 i = 0; i < NumVertices; ++i)
	{
		const FVector3f& VertexPosComponent = AllVertexPositions[i];
		const FVector VertexPosLocalD = ComponentToLocal.TransformPosition(FVector(VertexPosComponent));
		const FVector3f VertexPosLocal = FVector3f(VertexPosLocalD);

		// 1단계: 확장 바운드 체크 (빠른 필터링)
		if (!IsWithinExpandedBounds(VertexPosLocal, ExpandedMin, ExpandedMax))
		{
			continue;
		}
		BoundsPassCount++;

		// 2단계: Mexican Hat 필터링 (GPU와 동일 로직)
		const FVector3f ToVertex = VertexPosLocal - RingCenter;
		const float AxialComponent = FVector3f::DotProduct(ToVertex, RingAxis);
		const float AxialDist = FMath::Abs(AxialComponent);
		const float t = AxialDist / FMath::Max(EffectWidth, 0.001f);

		// Mexican Hat: f(t) = (1 - t²) × exp(-t²/2)
		// 음수 영역(t > 1)만 Bulge 대상
		const float MexicanHatValue = MexicanHat(t);
		if (MexicanHatValue >= 0.0f)
		{
			continue;  // 양수 영역 = Bulge 대상 아님
		}

		// BulgeMask = -MexicanHat (양수로 변환)
		const float BulgeMask = -MexicanHatValue;

		// 3단계: 거리 기반 Influence 계산
		const float DistInfluence = CalculateApproximateInfluence(
			VertexPosLocal, ExpandedMin, ExpandedMax);

		// 최종 Influence = BulgeMask × DistInfluence
		const float FinalInfluence = BulgeMask * DistInfluence;

		if (FinalInfluence > KINDA_SMALL_NUMBER)
		{
			OutBulgeVertexIndices.Add(static_cast<uint32>(i));
			OutBulgeInfluences.Add(FinalInfluence);
		}
	}

	UE_LOG(LogFleshRingBulge, Log, TEXT("Bulge 필터링: 바운드 통과=%d, Mexican Hat 통과=%d/%d (%.1f%%)"),
		BoundsPassCount,
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

FVector3f FSDFBulgeProvider::DetectRingAxis() const
{
	// Ring 축 = 가장 짧은 SDF 바운드 축 (GPU BulgeCS와 동일)
	const FVector3f BoundsSize = SDFBoundsMax - SDFBoundsMin;

	if (BoundsSize.X <= BoundsSize.Y && BoundsSize.X <= BoundsSize.Z)
	{
		return FVector3f(1.0f, 0.0f, 0.0f);
	}
	else if (BoundsSize.Y <= BoundsSize.X && BoundsSize.Y <= BoundsSize.Z)
	{
		return FVector3f(0.0f, 1.0f, 0.0f);
	}
	else
	{
		return FVector3f(0.0f, 0.0f, 1.0f);
	}
}

float FSDFBulgeProvider::MexicanHat(float t)
{
	// Ricker Wavelet (Mexican Hat)
	// f(t) = (1 - t²) × exp(-t²/2)
	// t=0: 1 (최대), t=1: 0 (영점), t>1: 음수 (Bulge 영역)
	const float t2 = t * t;
	return (1.0f - t2) * FMath::Exp(-t2 * 0.5f);
}
