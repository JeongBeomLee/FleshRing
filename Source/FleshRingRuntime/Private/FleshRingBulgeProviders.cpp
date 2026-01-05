// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingBulgeProviders.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingBulge, Log, All);

void FSDFBulgeProvider::InitFromSDFCache(
	const FVector3f& InBoundsMin,
	const FVector3f& InBoundsMax,
	const FTransform& InLocalToComponent,
	float InAxialRange,
	float InRadialRange)
{
	SDFBoundsMin = InBoundsMin;
	SDFBoundsMax = InBoundsMax;
	LocalToComponent = InLocalToComponent;
	AxialRange = InAxialRange;
	RadialRange = InRadialRange;
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

	// Ring 크기 계산 (축 방향 = Width, 반경 방향 = Radius)
	const float RingWidth = FMath::Min3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z);  // 축 방향 크기
	const float RingRadius = FMath::Max3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z) * 0.5f;  // 반경 방향 크기

	// Bulge 시작 거리 (Ring 경계)
	const float BulgeStartDist = RingWidth * 0.5f;

	// 직교 범위 제한 (각 축 독립적으로 제어)
	const float AxialLimit = RingWidth * 0.5f * AxialRange;   // 위아래 최대 범위
	const float RadialLimit = RingRadius * RadialRange;       // 옆 최대 범위

	const FTransform ComponentToLocal = LocalToComponent.Inverse();
	const int32 NumVertices = AllVertexPositions.Num();
	OutBulgeVertexIndices.Reserve(NumVertices / 5);
	OutBulgeInfluences.Reserve(NumVertices / 5);

	int32 AxialPassCount = 0;
	int32 RadialPassCount = 0;

	for (int32 i = 0; i < NumVertices; ++i)
	{
		const FVector3f& VertexPosComponent = AllVertexPositions[i];
		const FVector VertexPosLocalD = ComponentToLocal.TransformPosition(FVector(VertexPosComponent));
		const FVector3f VertexPosLocal = FVector3f(VertexPosLocalD);

		// Ring 중심으로부터의 벡터
		const FVector3f ToVertex = VertexPosLocal - RingCenter;

		// 1. 축 방향 거리 (위아래)
		const float AxialComponent = FVector3f::DotProduct(ToVertex, RingAxis);
		const float AxialDist = FMath::Abs(AxialComponent);

		// Bulge 시작점(Ring 경계) 이전은 제외 - Tightness 영역
		if (AxialDist < BulgeStartDist)
		{
			continue;
		}

		// 축 방향 범위 초과 체크
		if (AxialDist > AxialLimit)
		{
			continue;
		}
		AxialPassCount++;

		// 2. 반경 방향 거리 (옆)
		const FVector3f RadialVec = ToVertex - RingAxis * AxialComponent;
		const float RadialDist = RadialVec.Size();

		// Axial 거리에 따라 RadialLimit 동적 확장 (몸이 위아래로 넓어지는 것 보정)
		const float AxialRatio = (AxialDist - BulgeStartDist) / FMath::Max(AxialLimit - BulgeStartDist, 0.001f);
		const float DynamicRadialLimit = RadialLimit * (1.0f + AxialRatio * 0.5f);

		// 반경 방향 범위 초과 체크 (다른 허벅지 영향 방지)
		if (RadialDist > DynamicRadialLimit)
		{
			continue;
		}
		RadialPassCount++;

		// 3. 축 방향 거리 기반 Smoothstep 감쇠
		// Ring 경계에서 1.0, AxialLimit에서 0으로 부드럽게 감쇠
		const float AxialFalloffRange = AxialLimit - BulgeStartDist;
		const float NormalizedAxialDist = (AxialDist - BulgeStartDist) / FMath::Max(AxialFalloffRange, 0.001f);
		const float ClampedAxialDist = FMath::Clamp(NormalizedAxialDist, 0.0f, 1.0f);

		// Smoothstep: 1 → 0 (가까울수록 강함)
		const float t = 1.0f - ClampedAxialDist;
		const float BulgeInfluence = t * t * (3.0f - 2.0f * t);  // Hermite smoothstep

		if (BulgeInfluence > KINDA_SMALL_NUMBER)
		{
			OutBulgeVertexIndices.Add(static_cast<uint32>(i));
			OutBulgeInfluences.Add(BulgeInfluence);
		}
	}

	UE_LOG(LogFleshRingBulge, Verbose, TEXT("Bulge 필터링: Axial통과=%d, Radial통과=%d, 최종=%d/%d (%.1f%%)"),
		AxialPassCount,
		RadialPassCount,
		OutBulgeVertexIndices.Num(),
		NumVertices,
		NumVertices > 0 ? (100.0f * OutBulgeVertexIndices.Num() / NumVertices) : 0.0f);
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
