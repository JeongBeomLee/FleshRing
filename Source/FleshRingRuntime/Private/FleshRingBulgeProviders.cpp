// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingBulgeProviders.h"
#include "FleshRingAffectedVertices.h"

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
	const FVertexSpatialHash* SpatialHash,
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
	const float RingHeight = FMath::Min3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z);  // 축 방향 크기
	const float RingRadius = FMath::Max3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z) * 0.5f;  // 반경 방향 크기

	// Bulge 시작 거리 (Ring 경계)
	const float BulgeStartDist = RingHeight * 0.5f;

	// 직교 범위 제한 (각 축 독립적으로 제어)
	// AxialLimit = 시작점 + 확장량 (AxialRange=1이면 RingHeight*0.5 만큼 확장)
	const float AxialLimit = BulgeStartDist + RingHeight * 0.5f * AxialRange;
	const float RadialLimit = RingRadius * RadialRange;

	// ================================================================
	// Spatial Hash 최적화: 전체 버텍스 대신 후보만 쿼리
	// ================================================================
	TArray<int32> CandidateIndices;
	const int32 TotalVertexCount = AllVertexPositions.Num();

	if (SpatialHash && SpatialHash->IsBuilt())
	{
		// Spatial Hash 쿼리로 후보만 추출 - O(1)
		FVector ExpandedLocalMin, ExpandedLocalMax;
		CalculateExpandedBulgeAABB(ExpandedLocalMin, ExpandedLocalMax);
		SpatialHash->QueryOBB(LocalToComponent, ExpandedLocalMin, ExpandedLocalMax, CandidateIndices);

		UE_LOG(LogFleshRingBulge, Verbose, TEXT("Bulge SpatialHash: %d 후보 (전체 %d 중, %.1f%%)"),
			CandidateIndices.Num(), TotalVertexCount,
			TotalVertexCount > 0 ? (100.0f * CandidateIndices.Num() / TotalVertexCount) : 0.0f);
	}
	else
	{
		// 폴백: 전체 버텍스 (기존 브루트포스 동작)
		CandidateIndices.Reserve(TotalVertexCount);
		for (int32 i = 0; i < TotalVertexCount; ++i)
		{
			CandidateIndices.Add(i);
		}
		UE_LOG(LogFleshRingBulge, Verbose, TEXT("Bulge 브루트포스: SpatialHash 없음, 전체 %d 버텍스 순회"), TotalVertexCount);
	}

	// 비균등 스케일 + 회전 조합에서 InverseTransformPosition 사용 필수!
	// Inverse().TransformPosition()은 스케일과 회전 순서가 잘못됨
	OutBulgeVertexIndices.Reserve(CandidateIndices.Num() / 5);
	OutBulgeInfluences.Reserve(CandidateIndices.Num() / 5);

	int32 AxialPassCount = 0;
	int32 RadialPassCount = 0;

	// 후보에 대해서만 정밀 필터링
	for (int32 VertexIdx : CandidateIndices)
	{
		const FVector3f& VertexPosComponent = AllVertexPositions[VertexIdx];
		// Component Space → Local Space 변환
		// InverseTransformPosition: (V - Trans) * Rot^-1 / Scale (올바른 순서)
		const FVector VertexPosLocalD = LocalToComponent.InverseTransformPosition(FVector(VertexPosComponent));
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

		// Axial 거리에 따라 RadialLimit 동적 조절 (RadialTaper: 음수=수축, 0=원통, 양수=확장)
		const float AxialRatio = (AxialDist - BulgeStartDist) / FMath::Max(AxialLimit - BulgeStartDist, 0.001f);
		const float DynamicRadialLimit = RadialLimit * (1.0f + AxialRatio * RadialTaper);

		// 반경 방향 범위 초과 체크 (다른 허벅지 영향 방지)
		if (RadialDist > DynamicRadialLimit)
		{
			continue;
		}
		RadialPassCount++;

		// 3. 축 방향 거리 기반 Falloff 감쇠
		// Ring 경계에서 1.0, AxialLimit에서 0으로 부드럽게 감쇠
		const float AxialFalloffRange = AxialLimit - BulgeStartDist;
		const float NormalizedAxialDist = (AxialDist - BulgeStartDist) / FMath::Max(AxialFalloffRange, 0.001f);
		const float ClampedAxialDist = FMath::Clamp(NormalizedAxialDist, 0.0f, 1.0f);

		// 에디터에서 선택한 FalloffType에 따라 다른 커브 적용
		const float BulgeInfluence = FFleshRingFalloff::Evaluate(ClampedAxialDist, FalloffType);

		if (BulgeInfluence > KINDA_SMALL_NUMBER)
		{
			OutBulgeVertexIndices.Add(static_cast<uint32>(VertexIdx));
			OutBulgeInfluences.Add(BulgeInfluence);
		}
	}

	UE_LOG(LogFleshRingBulge, Verbose, TEXT("Bulge 필터링: 후보=%d, Axial통과=%d, Radial통과=%d, 최종=%d (%.1f%%)"),
		CandidateIndices.Num(),
		AxialPassCount,
		RadialPassCount,
		OutBulgeVertexIndices.Num(),
		CandidateIndices.Num() > 0 ? (100.0f * OutBulgeVertexIndices.Num() / CandidateIndices.Num()) : 0.0f);
}

void FSDFBulgeProvider::CalculateExpandedBulgeAABB(FVector& OutMin, FVector& OutMax) const
{
	const FVector3f BoundsSize = SDFBoundsMax - SDFBoundsMin;
	const float RingHeight = FMath::Min3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z);
	const float RingRadius = FMath::Max3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z) * 0.5f;

	// Bulge 영역 확장량 계산
	const float AxialExpansion = RingHeight * 0.5f * AxialRange;
	// 동적 RadialLimit 확장 고려 (RadialTaper에 따른 최대 배율)
	const float MaxTaperFactor = 1.0f + FMath::Max(RadialTaper, 0.0f);
	const float RadialExpansion = RingRadius * RadialRange * MaxTaperFactor;

	// 로컬 스페이스에서 확장된 AABB
	// 모든 축에 대해 확장 (Radial + Axial 모두 포함)
	const float MaxExpansion = FMath::Max(AxialExpansion, RadialExpansion);
	OutMin = FVector(SDFBoundsMin) - FVector(MaxExpansion);
	OutMax = FVector(SDFBoundsMax) + FVector(MaxExpansion);
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

// ============================================================================
// FVirtualRingBulgeProvider - VirtualRing 모드용 Bulge 영역 계산
// ============================================================================

void FVirtualRingBulgeProvider::InitFromRingParams(
	const FVector3f& InRingCenter,
	const FVector3f& InRingAxis,
	float InRingRadius,
	float InRingHeight,
	float InAxialRange,
	float InRadialRange)
{
	RingCenter = InRingCenter;
	RingAxis = InRingAxis.GetSafeNormal();
	RingRadius = InRingRadius;
	RingHeight = InRingHeight;
	AxialRange = InAxialRange;
	RadialRange = InRadialRange;
}

void FVirtualRingBulgeProvider::CalculateBulgeRegion(
	const TArrayView<const FVector3f>& AllVertexPositions,
	const FVertexSpatialHash* SpatialHash,
	TArray<uint32>& OutBulgeVertexIndices,
	TArray<float>& OutBulgeInfluences,
	TArray<FVector3f>& OutBulgeDirections) const
{
	OutBulgeVertexIndices.Reset();
	OutBulgeInfluences.Reset();
	OutBulgeDirections.Reset();  // GPU에서 계산

	// 유효성 검사
	if (RingRadius <= KINDA_SMALL_NUMBER || RingHeight <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogFleshRingBulge, Warning, TEXT("VirtualRing Bulge: Ring 파라미터가 유효하지 않음 (Radius=%.2f, Width=%.2f)"),
			RingRadius, RingHeight);
		return;
	}

	// Bulge 시작 거리 (Ring 경계)
	const float BulgeStartDist = RingHeight * 0.5f;

	// 직교 범위 제한
	const float AxialLimit = BulgeStartDist + RingHeight * 0.5f * AxialRange;
	const float RadialLimit = RingRadius * RadialRange;

	// ================================================================
	// Spatial Hash 최적화: 전체 버텍스 대신 후보만 쿼리
	// ================================================================
	TArray<int32> CandidateIndices;
	const int32 TotalVertexCount = AllVertexPositions.Num();

	if (SpatialHash && SpatialHash->IsBuilt())
	{
		// Spatial Hash 쿼리용 확장된 AABB 계산
		FVector ExpandedMin, ExpandedMax;
		CalculateExpandedBulgeAABB(ExpandedMin, ExpandedMax);

		// Component Space에서 직접 AABB 쿼리 (Identity Transform 사용)
		SpatialHash->QueryOBB(FTransform::Identity, ExpandedMin, ExpandedMax, CandidateIndices);

		UE_LOG(LogFleshRingBulge, Verbose, TEXT("VirtualRing Bulge SpatialHash: %d 후보 (전체 %d 중, %.1f%%)"),
			CandidateIndices.Num(), TotalVertexCount,
			TotalVertexCount > 0 ? (100.0f * CandidateIndices.Num() / TotalVertexCount) : 0.0f);
	}
	else
	{
		// 폴백: 전체 버텍스
		CandidateIndices.Reserve(TotalVertexCount);
		for (int32 i = 0; i < TotalVertexCount; ++i)
		{
			CandidateIndices.Add(i);
		}
		UE_LOG(LogFleshRingBulge, Verbose, TEXT("VirtualRing Bulge 브루트포스: SpatialHash 없음, 전체 %d 버텍스 순회"), TotalVertexCount);
	}

	OutBulgeVertexIndices.Reserve(CandidateIndices.Num() / 5);
	OutBulgeInfluences.Reserve(CandidateIndices.Num() / 5);

	int32 AxialPassCount = 0;
	int32 RadialPassCount = 0;

	// 후보에 대해서만 정밀 필터링
	// Component Space에서 직접 계산 (LocalToComponent 변환 없음)
	for (int32 VertexIdx : CandidateIndices)
	{
		const FVector3f& VertexPos = AllVertexPositions[VertexIdx];

		// Ring 중심으로부터의 벡터
		const FVector3f ToVertex = VertexPos - RingCenter;

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

		// Axial 거리에 따라 RadialLimit 동적 조절 (RadialTaper: 음수=수축, 0=원통, 양수=확장)
		const float AxialRatio = (AxialDist - BulgeStartDist) / FMath::Max(AxialLimit - BulgeStartDist, 0.001f);
		const float DynamicRadialLimit = RadialLimit * (1.0f + AxialRatio * RadialTaper);

		// 반경 방향 범위 초과 체크
		if (RadialDist > DynamicRadialLimit)
		{
			continue;
		}
		RadialPassCount++;

		// 3. 축 방향 거리 기반 Falloff 감쇠
		const float AxialFalloffRange = AxialLimit - BulgeStartDist;
		const float NormalizedAxialDist = (AxialDist - BulgeStartDist) / FMath::Max(AxialFalloffRange, 0.001f);
		const float ClampedAxialDist = FMath::Clamp(NormalizedAxialDist, 0.0f, 1.0f);

		// FalloffType에 따라 감쇠 곡선 적용
		const float BulgeInfluence = FFleshRingFalloff::Evaluate(ClampedAxialDist, FalloffType);

		if (BulgeInfluence > KINDA_SMALL_NUMBER)
		{
			OutBulgeVertexIndices.Add(static_cast<uint32>(VertexIdx));
			OutBulgeInfluences.Add(BulgeInfluence);
		}
	}

	UE_LOG(LogFleshRingBulge, Verbose, TEXT("VirtualRing Bulge 필터링: 후보=%d, Axial통과=%d, Radial통과=%d, 최종=%d (%.1f%%)"),
		CandidateIndices.Num(),
		AxialPassCount,
		RadialPassCount,
		OutBulgeVertexIndices.Num(),
		CandidateIndices.Num() > 0 ? (100.0f * OutBulgeVertexIndices.Num() / CandidateIndices.Num()) : 0.0f);
}

void FVirtualRingBulgeProvider::CalculateExpandedBulgeAABB(FVector& OutMin, FVector& OutMax) const
{
	// Bulge 영역 확장량 계산
	const float AxialExpansion = RingHeight * 0.5f * AxialRange;
	// 동적 RadialLimit 확장 고려 (RadialTaper에 따른 최대 배율)
	const float MaxTaperFactor = 1.0f + FMath::Max(RadialTaper, 0.0f);
	const float RadialExpansion = RingRadius * RadialRange * MaxTaperFactor;

	// Component Space에서 확장된 AABB
	// 모든 축에 대해 확장 (Radial + Axial 모두 포함)
	const float MaxExpansion = FMath::Max(AxialExpansion, RadialExpansion);
	OutMin = FVector(RingCenter) - FVector(MaxExpansion);
	OutMax = FVector(RingCenter) + FVector(MaxExpansion);
}

// ============================================================================
// FVirtualBandInfluenceProvider - Virtual Band(VirtualBand) 모드용 Bulge 영역 계산
// ============================================================================

void FVirtualBandInfluenceProvider::InitFromBandSettings(
	float InLowerRadius, float InMidLowerRadius,
	float InMidUpperRadius, float InUpperRadius,
	float InLowerHeight, float InBandHeight, float InUpperHeight,
	const FVector3f& InCenter, const FVector3f& InAxis,
	float InAxialRange, float InRadialRange)
{
	LowerRadius = InLowerRadius;
	MidLowerRadius = InMidLowerRadius;
	MidUpperRadius = InMidUpperRadius;
	UpperRadius = InUpperRadius;
	LowerHeight = InLowerHeight;
	BandHeight = InBandHeight;
	UpperHeight = InUpperHeight;
	BandCenter = InCenter;
	BandAxis = InAxis.GetSafeNormal();
	AxialRange = InAxialRange;
	RadialRange = InRadialRange;
}

float FVirtualBandInfluenceProvider::GetRadiusAtHeight(float LocalZ) const
{
	FVirtualBandSettings TempSettings;
	TempSettings.Lower.Radius = LowerRadius;
	TempSettings.Lower.Height = LowerHeight;
	TempSettings.MidLowerRadius = MidLowerRadius;
	TempSettings.MidUpperRadius = MidUpperRadius;
	TempSettings.BandHeight = BandHeight;
	TempSettings.Upper.Radius = UpperRadius;
	TempSettings.Upper.Height = UpperHeight;

	return TempSettings.GetRadiusAtHeight(LocalZ);
}

float FVirtualBandInfluenceProvider::CalculateFalloff(float Distance, float MaxDistance) const
{
	if (MaxDistance <= KINDA_SMALL_NUMBER)
	{
		return 1.0f;
	}

	const float NormalizedDist = FMath::Clamp(Distance / MaxDistance, 0.0f, 1.0f);
	return FFleshRingFalloff::Evaluate(NormalizedDist, FalloffType);
}

void FVirtualBandInfluenceProvider::CalculateBulgeRegion(
	const TArrayView<const FVector3f>& AllVertexPositions,
	const FVertexSpatialHash* SpatialHash,
	TArray<uint32>& OutBulgeVertexIndices,
	TArray<float>& OutBulgeInfluences,
	TArray<FVector3f>& OutBulgeDirections) const
{
	OutBulgeVertexIndices.Reset();
	OutBulgeInfluences.Reset();
	OutBulgeDirections.Reset();  // GPU에서 계산

	const float TotalHeight = GetTotalHeight();

	// 유효성 검사
	if (TotalHeight <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogFleshRingBulge, Warning, TEXT("VirtualBand Bulge: 높이가 유효하지 않음 (TotalHeight=%.2f)"),
			TotalHeight);
		return;
	}

	// 새 좌표계: Z=0이 Mid Band 중심
	const float MidOffset = LowerHeight + BandHeight * 0.5f;
	const float ZMin = -MidOffset;
	const float ZMax = TotalHeight - MidOffset;

	// Band Section 경계 (-BandHeight/2 ~ +BandHeight/2)
	const float BandZMin = -BandHeight * 0.5f;
	const float BandZMax = BandHeight * 0.5f;

	// Bulge 범위 (축 방향)
	// Lower Section: ZMin에서 BandZMin까지, 확장은 아래로
	// Upper Section: BandZMax에서 ZMax까지, 확장은 위로
	const float BulgeAxialLimit = FMath::Max(LowerHeight, UpperHeight) * AxialRange;

	// Bulge 범위 (반경 방향) - 최대 반경 기준
	const float MaxRadius = FMath::Max(FMath::Max(LowerRadius, MidLowerRadius), FMath::Max(MidUpperRadius, UpperRadius));
	const float BulgeRadialLimit = MaxRadius * RadialRange;

	// ================================================================
	// Spatial Hash 최적화
	// ================================================================
	TArray<int32> CandidateIndices;
	const int32 TotalVertexCount = AllVertexPositions.Num();

	if (SpatialHash && SpatialHash->IsBuilt())
	{
		FVector ExpandedMin, ExpandedMax;
		CalculateExpandedBulgeAABB(ExpandedMin, ExpandedMax);
		SpatialHash->QueryOBB(FTransform::Identity, ExpandedMin, ExpandedMax, CandidateIndices);

		UE_LOG(LogFleshRingBulge, Verbose, TEXT("VirtualBand Bulge SpatialHash: %d 후보 (전체 %d 중, %.1f%%)"),
			CandidateIndices.Num(), TotalVertexCount,
			TotalVertexCount > 0 ? (100.0f * CandidateIndices.Num() / TotalVertexCount) : 0.0f);
	}
	else
	{
		CandidateIndices.Reserve(TotalVertexCount);
		for (int32 i = 0; i < TotalVertexCount; ++i)
		{
			CandidateIndices.Add(i);
		}
		UE_LOG(LogFleshRingBulge, Verbose, TEXT("VirtualBand Bulge 브루트포스: SpatialHash 없음, 전체 %d 버텍스 순회"), TotalVertexCount);
	}

	OutBulgeVertexIndices.Reserve(CandidateIndices.Num() / 5);
	OutBulgeInfluences.Reserve(CandidateIndices.Num() / 5);

	int32 LowerSectionCount = 0;
	int32 UpperSectionCount = 0;

	for (int32 VertexIdx : CandidateIndices)
	{
		const FVector3f& VertexPos = AllVertexPositions[VertexIdx];

		// Band 중심으로부터의 벡터
		const FVector3f ToVertex = VertexPos - BandCenter;

		// 축 방향 성분 (Local Z)
		const float LocalZ = FVector3f::DotProduct(ToVertex, BandAxis);

		// Band Section 내부는 Bulge 제외 (Tightness만 적용)
		if (LocalZ >= BandZMin && LocalZ <= BandZMax)
		{
			continue;
		}

		// 반경 방향 벡터와 거리
		const FVector3f RadialVec = ToVertex - BandAxis * LocalZ;
		const float RadialDist = RadialVec.Size();

		// 해당 높이에서의 밴드 반경 (클램핑된 Z 사용)
		const float ClampedZ = FMath::Clamp(LocalZ, ZMin, ZMax);
		const float BandRadiusAtZ = GetRadiusAtHeight(ClampedZ);

		// 반경 범위 제한: 밴드 반경 근처에만 적용
		const float RadialMargin = BandRadiusAtZ * (RadialRange - 1.0f);
		if (RadialDist > BandRadiusAtZ + RadialMargin || RadialDist < BandRadiusAtZ * 0.3f)
		{
			continue;
		}

		// Bulge 영향도 계산
		float BulgeInfluence = 0.0f;

		if (LocalZ < BandZMin)
		{
			// Lower Section: Band 하단 경계에서 멀어질수록 감소
			const float AxialFromBand = BandZMin - LocalZ;
			const float AxialLimit = LowerHeight * AxialRange;

			if (AxialFromBand > AxialLimit)
			{
				continue;
			}

			BulgeInfluence = CalculateFalloff(AxialFromBand, AxialLimit);
			LowerSectionCount++;
		}
		else // LocalZ > BandZMax
		{
			// Upper Section: Band 상단 경계에서 멀어질수록 감소
			const float AxialFromBand = LocalZ - BandZMax;
			const float AxialLimit = UpperHeight * AxialRange;

			if (AxialFromBand > AxialLimit)
			{
				continue;
			}

			BulgeInfluence = CalculateFalloff(AxialFromBand, AxialLimit);
			UpperSectionCount++;
		}

		if (BulgeInfluence > KINDA_SMALL_NUMBER)
		{
			OutBulgeVertexIndices.Add(static_cast<uint32>(VertexIdx));
			OutBulgeInfluences.Add(BulgeInfluence);
		}
	}

	UE_LOG(LogFleshRingBulge, Verbose, TEXT("VirtualBand Bulge 필터링: 후보=%d, Lower=%d, Upper=%d, 최종=%d"),
		CandidateIndices.Num(),
		LowerSectionCount,
		UpperSectionCount,
		OutBulgeVertexIndices.Num());
}

void FVirtualBandInfluenceProvider::CalculateExpandedBulgeAABB(FVector& OutMin, FVector& OutMax) const
{
	const float TotalHeight = GetTotalHeight();

	// 새 좌표계: Z=0이 Mid Band 중심
	const float MidOffset = LowerHeight + BandHeight * 0.5f;
	const float ZMin = -MidOffset;
	const float ZMax = TotalHeight - MidOffset;

	// 최대 반경
	const float MaxRadius = FMath::Max(FMath::Max(LowerRadius, MidLowerRadius), FMath::Max(MidUpperRadius, UpperRadius));

	// 확장량
	const float AxialExpansion = FMath::Max(LowerHeight, UpperHeight) * AxialRange;
	const float RadialExpansion = MaxRadius * RadialRange;
	const float MaxExpansion = FMath::Max(AxialExpansion, RadialExpansion);

	// Band 중심 기준 AABB (새 좌표계)
	// Z 방향: ZMin ~ ZMax + 확장
	// XY 방향: 최대 반경 + 확장
	OutMin = FVector(BandCenter) + FVector(-MaxExpansion, -MaxExpansion, ZMin - MaxExpansion);
	OutMax = FVector(BandCenter) + FVector(MaxExpansion, MaxExpansion, ZMax + MaxExpansion);
}
