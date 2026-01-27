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
	OutBulgeDirections.Reset();  // Calculated on GPU

	const FVector3f BoundsSize = SDFBoundsMax - SDFBoundsMin;
	if (BoundsSize.X <= KINDA_SMALL_NUMBER ||
		BoundsSize.Y <= KINDA_SMALL_NUMBER ||
		BoundsSize.Z <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogFleshRingBulge, Warning, TEXT("SDF bounds are invalid"));
		return;
	}

	// Calculate Ring geometry information
	const FVector3f RingCenter = (SDFBoundsMin + SDFBoundsMax) * 0.5f;
	const FVector3f RingAxis = DetectRingAxis();

	// Calculate Ring size (axial direction = Width, radial direction = Radius)
	const float RingHeight = FMath::Min3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z);  // Axial size
	const float RingRadius = FMath::Max3(BoundsSize.X, BoundsSize.Y, BoundsSize.Z) * 0.5f;  // Radial size

	// Bulge start distance (Ring boundary)
	const float BulgeStartDist = RingHeight * 0.5f;

	// Orthogonal range limit (each axis controlled independently)
	// AxialLimit = start point + expansion amount (if AxialRange=1, expands by RingHeight*0.5)
	const float AxialLimit = BulgeStartDist + RingHeight * 0.5f * AxialRange;
	const float RadialLimit = RingRadius * RadialRange;

	// ================================================================
	// Spatial Hash optimization: Query only candidates instead of all vertices
	// ================================================================
	TArray<int32> CandidateIndices;
	const int32 TotalVertexCount = AllVertexPositions.Num();

	if (SpatialHash && SpatialHash->IsBuilt())
	{
		// Extract only candidates via Spatial Hash query - O(1)
		FVector ExpandedLocalMin, ExpandedLocalMax;
		CalculateExpandedBulgeAABB(ExpandedLocalMin, ExpandedLocalMax);
		SpatialHash->QueryOBB(LocalToComponent, ExpandedLocalMin, ExpandedLocalMax, CandidateIndices);

		UE_LOG(LogFleshRingBulge, Verbose, TEXT("Bulge SpatialHash: %d candidates (out of %d total, %.1f%%)"),
			CandidateIndices.Num(), TotalVertexCount,
			TotalVertexCount > 0 ? (100.0f * CandidateIndices.Num() / TotalVertexCount) : 0.0f);
	}
	else
	{
		// Fallback: all vertices (existing brute force behavior)
		CandidateIndices.Reserve(TotalVertexCount);
		for (int32 i = 0; i < TotalVertexCount; ++i)
		{
			CandidateIndices.Add(i);
		}
		UE_LOG(LogFleshRingBulge, Verbose, TEXT("Bulge brute force: no SpatialHash, iterating all %d vertices"), TotalVertexCount);
	}

	// Must use InverseTransformPosition with non-uniform scale + rotation combination!
	// Inverse().TransformPosition() has wrong scale and rotation order
	OutBulgeVertexIndices.Reserve(CandidateIndices.Num() / 5);
	OutBulgeInfluences.Reserve(CandidateIndices.Num() / 5);

	int32 AxialPassCount = 0;
	int32 RadialPassCount = 0;

	// Precise filtering on candidates only
	for (int32 VertexIdx : CandidateIndices)
	{
		const FVector3f& VertexPosComponent = AllVertexPositions[VertexIdx];
		// Component Space -> Local Space conversion
		// InverseTransformPosition: (V - Trans) * Rot^-1 / Scale (correct order)
		const FVector VertexPosLocalD = LocalToComponent.InverseTransformPosition(FVector(VertexPosComponent));
		const FVector3f VertexPosLocal = FVector3f(VertexPosLocalD);

		// Vector from Ring center
		const FVector3f ToVertex = VertexPosLocal - RingCenter;

		// 1. Axial distance (up/down)
		const float AxialComponent = FVector3f::DotProduct(ToVertex, RingAxis);
		const float AxialDist = FMath::Abs(AxialComponent);

		// Exclude points before Bulge start (Ring boundary) - Tightness region
		if (AxialDist < BulgeStartDist)
		{
			continue;
		}

		// Check if axial range exceeded
		if (AxialDist > AxialLimit)
		{
			continue;
		}
		AxialPassCount++;

		// 2. Radial distance (side)
		const FVector3f RadialVec = ToVertex - RingAxis * AxialComponent;
		const float RadialDist = RadialVec.Size();

		// Dynamically adjust RadialLimit based on Axial distance (RadialTaper: negative=shrink, 0=cylinder, positive=expand)
		const float AxialRatio = (AxialDist - BulgeStartDist) / FMath::Max(AxialLimit - BulgeStartDist, 0.001f);
		const float DynamicRadialLimit = RadialLimit * (1.0f + AxialRatio * RadialTaper);

		// Check if radial range exceeded (prevent affecting other thigh)
		if (RadialDist > DynamicRadialLimit)
		{
			continue;
		}
		RadialPassCount++;

		// 3. Falloff attenuation based on axial distance
		// Smooth attenuation from 1.0 at Ring boundary to 0 at AxialLimit
		const float AxialFalloffRange = AxialLimit - BulgeStartDist;
		const float NormalizedAxialDist = (AxialDist - BulgeStartDist) / FMath::Max(AxialFalloffRange, 0.001f);
		const float ClampedAxialDist = FMath::Clamp(NormalizedAxialDist, 0.0f, 1.0f);

		// Apply different curve based on FalloffType selected in editor
		const float BulgeInfluence = FFleshRingFalloff::Evaluate(ClampedAxialDist, FalloffType);

		if (BulgeInfluence > KINDA_SMALL_NUMBER)
		{
			OutBulgeVertexIndices.Add(static_cast<uint32>(VertexIdx));
			OutBulgeInfluences.Add(BulgeInfluence);
		}
	}

	UE_LOG(LogFleshRingBulge, Verbose, TEXT("Bulge filtering: candidates=%d, Axial passed=%d, Radial passed=%d, final=%d (%.1f%%)"),
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

	// Calculate Bulge region expansion amount
	const float AxialExpansion = RingHeight * 0.5f * AxialRange;
	// Consider dynamic RadialLimit expansion (max multiplier based on RadialTaper)
	const float MaxTaperFactor = 1.0f + FMath::Max(RadialTaper, 0.0f);
	const float RadialExpansion = RingRadius * RadialRange * MaxTaperFactor;

	// Expanded AABB in local space
	// Expand on all axes (includes both Radial + Axial)
	const float MaxExpansion = FMath::Max(AxialExpansion, RadialExpansion);
	OutMin = FVector(SDFBoundsMin) - FVector(MaxExpansion);
	OutMax = FVector(SDFBoundsMax) + FVector(MaxExpansion);
}

FVector3f FSDFBulgeProvider::DetectRingAxis() const
{
	// Ring axis = shortest SDF bounds axis (same as GPU BulgeCS)
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
// FVirtualRingBulgeProvider - Bulge region calculation for VirtualRing mode
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
	OutBulgeDirections.Reset();  // Calculated on GPU

	// Validation check
	if (RingRadius <= KINDA_SMALL_NUMBER || RingHeight <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogFleshRingBulge, Warning, TEXT("VirtualRing Bulge: Ring parameters are invalid (Radius=%.2f, Width=%.2f)"),
			RingRadius, RingHeight);
		return;
	}

	// Bulge start distance (Ring boundary)
	const float BulgeStartDist = RingHeight * 0.5f;

	// Orthogonal range limit
	const float AxialLimit = BulgeStartDist + RingHeight * 0.5f * AxialRange;
	const float RadialLimit = RingRadius * RadialRange;

	// ================================================================
	// Spatial Hash optimization: Query only candidates instead of all vertices
	// ================================================================
	TArray<int32> CandidateIndices;
	const int32 TotalVertexCount = AllVertexPositions.Num();

	if (SpatialHash && SpatialHash->IsBuilt())
	{
		// Calculate expanded AABB for Spatial Hash query
		FVector ExpandedMin, ExpandedMax;
		CalculateExpandedBulgeAABB(ExpandedMin, ExpandedMax);

		// Query AABB directly in Component Space (using Identity Transform)
		SpatialHash->QueryOBB(FTransform::Identity, ExpandedMin, ExpandedMax, CandidateIndices);

		UE_LOG(LogFleshRingBulge, Verbose, TEXT("VirtualRing Bulge SpatialHash: %d candidates (out of %d total, %.1f%%)"),
			CandidateIndices.Num(), TotalVertexCount,
			TotalVertexCount > 0 ? (100.0f * CandidateIndices.Num() / TotalVertexCount) : 0.0f);
	}
	else
	{
		// Fallback: all vertices
		CandidateIndices.Reserve(TotalVertexCount);
		for (int32 i = 0; i < TotalVertexCount; ++i)
		{
			CandidateIndices.Add(i);
		}
		UE_LOG(LogFleshRingBulge, Verbose, TEXT("VirtualRing Bulge brute force: no SpatialHash, iterating all %d vertices"), TotalVertexCount);
	}

	OutBulgeVertexIndices.Reserve(CandidateIndices.Num() / 5);
	OutBulgeInfluences.Reserve(CandidateIndices.Num() / 5);

	int32 AxialPassCount = 0;
	int32 RadialPassCount = 0;

	// Precise filtering on candidates only
	// Calculate directly in Component Space (no LocalToComponent conversion)
	for (int32 VertexIdx : CandidateIndices)
	{
		const FVector3f& VertexPos = AllVertexPositions[VertexIdx];

		// Vector from Ring center
		const FVector3f ToVertex = VertexPos - RingCenter;

		// 1. Axial distance (up/down)
		const float AxialComponent = FVector3f::DotProduct(ToVertex, RingAxis);
		const float AxialDist = FMath::Abs(AxialComponent);

		// Exclude points before Bulge start (Ring boundary) - Tightness region
		if (AxialDist < BulgeStartDist)
		{
			continue;
		}

		// Check if axial range exceeded
		if (AxialDist > AxialLimit)
		{
			continue;
		}
		AxialPassCount++;

		// 2. Radial distance (side)
		const FVector3f RadialVec = ToVertex - RingAxis * AxialComponent;
		const float RadialDist = RadialVec.Size();

		// Dynamically adjust RadialLimit based on Axial distance (RadialTaper: negative=shrink, 0=cylinder, positive=expand)
		const float AxialRatio = (AxialDist - BulgeStartDist) / FMath::Max(AxialLimit - BulgeStartDist, 0.001f);
		const float DynamicRadialLimit = RadialLimit * (1.0f + AxialRatio * RadialTaper);

		// Check if radial range exceeded
		if (RadialDist > DynamicRadialLimit)
		{
			continue;
		}
		RadialPassCount++;

		// 3. Falloff attenuation based on axial distance
		const float AxialFalloffRange = AxialLimit - BulgeStartDist;
		const float NormalizedAxialDist = (AxialDist - BulgeStartDist) / FMath::Max(AxialFalloffRange, 0.001f);
		const float ClampedAxialDist = FMath::Clamp(NormalizedAxialDist, 0.0f, 1.0f);

		// Apply attenuation curve based on FalloffType
		const float BulgeInfluence = FFleshRingFalloff::Evaluate(ClampedAxialDist, FalloffType);

		if (BulgeInfluence > KINDA_SMALL_NUMBER)
		{
			OutBulgeVertexIndices.Add(static_cast<uint32>(VertexIdx));
			OutBulgeInfluences.Add(BulgeInfluence);
		}
	}

	UE_LOG(LogFleshRingBulge, Verbose, TEXT("VirtualRing Bulge filtering: candidates=%d, Axial passed=%d, Radial passed=%d, final=%d (%.1f%%)"),
		CandidateIndices.Num(),
		AxialPassCount,
		RadialPassCount,
		OutBulgeVertexIndices.Num(),
		CandidateIndices.Num() > 0 ? (100.0f * OutBulgeVertexIndices.Num() / CandidateIndices.Num()) : 0.0f);
}

void FVirtualRingBulgeProvider::CalculateExpandedBulgeAABB(FVector& OutMin, FVector& OutMax) const
{
	// Calculate Bulge region expansion amount
	const float AxialExpansion = RingHeight * 0.5f * AxialRange;
	// Consider dynamic RadialLimit expansion (max multiplier based on RadialTaper)
	const float MaxTaperFactor = 1.0f + FMath::Max(RadialTaper, 0.0f);
	const float RadialExpansion = RingRadius * RadialRange * MaxTaperFactor;

	// Expanded AABB in Component Space
	// Expand on all axes (includes both Radial + Axial)
	const float MaxExpansion = FMath::Max(AxialExpansion, RadialExpansion);
	OutMin = FVector(RingCenter) - FVector(MaxExpansion);
	OutMax = FVector(RingCenter) + FVector(MaxExpansion);
}

// ============================================================================
// FVirtualBandInfluenceProvider - Bulge region calculation for Virtual Band (VirtualBand) mode
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
	OutBulgeDirections.Reset();  // Calculated on GPU

	const float TotalHeight = GetTotalHeight();

	// Validation check
	if (TotalHeight <= KINDA_SMALL_NUMBER)
	{
		UE_LOG(LogFleshRingBulge, Warning, TEXT("VirtualBand Bulge: Height is invalid (TotalHeight=%.2f)"),
			TotalHeight);
		return;
	}

	// New coordinate system: Z=0 is Mid Band center
	const float MidOffset = LowerHeight + BandHeight * 0.5f;
	const float ZMin = -MidOffset;
	const float ZMax = TotalHeight - MidOffset;

	// Band Section boundary (-BandHeight/2 ~ +BandHeight/2)
	const float BandZMin = -BandHeight * 0.5f;
	const float BandZMax = BandHeight * 0.5f;

	// Bulge range (axial direction)
	// Lower Section: from ZMin to BandZMin, expansion downward
	// Upper Section: from BandZMax to ZMax, expansion upward
	const float BulgeAxialLimit = FMath::Max(LowerHeight, UpperHeight) * AxialRange;

	// Bulge range (radial direction) - based on maximum radius
	const float MaxRadius = FMath::Max(FMath::Max(LowerRadius, MidLowerRadius), FMath::Max(MidUpperRadius, UpperRadius));
	const float BulgeRadialLimit = MaxRadius * RadialRange;

	// ================================================================
	// Spatial Hash optimization
	// ================================================================
	TArray<int32> CandidateIndices;
	const int32 TotalVertexCount = AllVertexPositions.Num();

	if (SpatialHash && SpatialHash->IsBuilt())
	{
		FVector ExpandedMin, ExpandedMax;
		CalculateExpandedBulgeAABB(ExpandedMin, ExpandedMax);
		SpatialHash->QueryOBB(FTransform::Identity, ExpandedMin, ExpandedMax, CandidateIndices);

		UE_LOG(LogFleshRingBulge, Verbose, TEXT("VirtualBand Bulge SpatialHash: %d candidates (out of %d total, %.1f%%)"),
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
		UE_LOG(LogFleshRingBulge, Verbose, TEXT("VirtualBand Bulge brute force: no SpatialHash, iterating all %d vertices"), TotalVertexCount);
	}

	OutBulgeVertexIndices.Reserve(CandidateIndices.Num() / 5);
	OutBulgeInfluences.Reserve(CandidateIndices.Num() / 5);

	int32 LowerSectionCount = 0;
	int32 UpperSectionCount = 0;

	for (int32 VertexIdx : CandidateIndices)
	{
		const FVector3f& VertexPos = AllVertexPositions[VertexIdx];

		// Vector from Band center
		const FVector3f ToVertex = VertexPos - BandCenter;

		// Axial component (Local Z)
		const float LocalZ = FVector3f::DotProduct(ToVertex, BandAxis);

		// Exclude Bulge inside Band Section (apply Tightness only)
		if (LocalZ >= BandZMin && LocalZ <= BandZMax)
		{
			continue;
		}

		// Radial vector and distance
		const FVector3f RadialVec = ToVertex - BandAxis * LocalZ;
		const float RadialDist = RadialVec.Size();

		// Band radius at this height (using clamped Z)
		const float ClampedZ = FMath::Clamp(LocalZ, ZMin, ZMax);
		const float BandRadiusAtZ = GetRadiusAtHeight(ClampedZ);

		// Radial range limit: apply only near band radius
		const float RadialMargin = BandRadiusAtZ * (RadialRange - 1.0f);
		if (RadialDist > BandRadiusAtZ + RadialMargin || RadialDist < BandRadiusAtZ * 0.3f)
		{
			continue;
		}

		// Calculate Bulge influence
		float BulgeInfluence = 0.0f;

		if (LocalZ < BandZMin)
		{
			// Lower Section: Decreases further from Band lower boundary
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
			// Upper Section: Decreases further from Band upper boundary
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

	UE_LOG(LogFleshRingBulge, Verbose, TEXT("VirtualBand Bulge filtering: candidates=%d, Lower=%d, Upper=%d, final=%d"),
		CandidateIndices.Num(),
		LowerSectionCount,
		UpperSectionCount,
		OutBulgeVertexIndices.Num());
}

void FVirtualBandInfluenceProvider::CalculateExpandedBulgeAABB(FVector& OutMin, FVector& OutMax) const
{
	const float TotalHeight = GetTotalHeight();

	// New coordinate system: Z=0 is Mid Band center
	const float MidOffset = LowerHeight + BandHeight * 0.5f;
	const float ZMin = -MidOffset;
	const float ZMax = TotalHeight - MidOffset;

	// Maximum radius
	const float MaxRadius = FMath::Max(FMath::Max(LowerRadius, MidLowerRadius), FMath::Max(MidUpperRadius, UpperRadius));

	// Expansion amount
	const float AxialExpansion = FMath::Max(LowerHeight, UpperHeight) * AxialRange;
	const float RadialExpansion = MaxRadius * RadialRange;
	const float MaxExpansion = FMath::Max(AxialExpansion, RadialExpansion);

	// AABB based on Band center (new coordinate system)
	// Z direction: ZMin ~ ZMax + expansion
	// XY direction: maximum radius + expansion
	OutMin = FVector(BandCenter) + FVector(-MaxExpansion, -MaxExpansion, ZMin - MaxExpansion);
	OutMax = FVector(BandCenter) + FVector(MaxExpansion, MaxExpansion, ZMax + MaxExpansion);
}
