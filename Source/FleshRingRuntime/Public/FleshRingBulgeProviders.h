// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "FleshRingBulgeTypes.h"
#include "FleshRingFalloff.h"

/**
 * SDF bounds-based Bulge region calculation
 * CPU: vertex filtering + influence calculation
 * GPU: actual Bulge direction/deformation application
 */
class FLESHRINGRUNTIME_API FSDFBulgeProvider : public IBulgeRegionProvider
{
public:
	FVector3f SDFBoundsMin = FVector3f::ZeroVector;	// SDF Local
	FVector3f SDFBoundsMax = FVector3f::ZeroVector;	// SDF Local
	FTransform LocalToComponent = FTransform::Identity;

	float AxialRange = 3.0f;	// Axial (up/down) range multiplier
	float RadialRange = 1.5f;	// Radial (side) range multiplier
	float RadialTaper = 0.5f;	// Axial taper coefficient (negative=shrink, 0=cylinder, positive=expand)
	EFleshRingFalloffType FalloffType = EFleshRingFalloffType::WendlandC2;	// Falloff curve type, injected by FleshRingDeformerInstance

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
	/** Detect Ring axis (shortest SDF bounds axis) */
	FVector3f DetectRingAxis() const;

	/** Calculate expanded AABB for Bulge region (for Spatial Hash query) */
	void CalculateExpandedBulgeAABB(FVector& OutMin, FVector& OutMax) const;
};

/**
 * Bulge region calculation for VirtualRing mode
 * Receives Ring parameters (RingCenter, RingAxis, RingRadius, RingHeight) directly
 * and calculates Bulge region without SDF
 *
 * Differences from FSDFBulgeProvider:
 * - Coordinate system: uses Component Space directly (no LocalToComponent transform)
 * - Ring info: received directly (not inferred from SDF bounds)
 * - Ring axis: received directly (no auto-detection)
 */
class FLESHRINGRUNTIME_API FVirtualRingBulgeProvider : public IBulgeRegionProvider
{
public:
	// Ring geometry info (Component Space)
	FVector3f RingCenter = FVector3f::ZeroVector;
	FVector3f RingAxis = FVector3f(0.0f, 0.0f, 1.0f);
	float RingRadius = 5.0f;
	float RingHeight = 2.0f;

	// Bulge range parameters
	float AxialRange = 3.0f;	// Axial (up/down) range multiplier
	float RadialRange = 1.5f;	// Radial (side) range multiplier
	float RadialTaper = 0.5f;	// Axial taper coefficient (negative=shrink, 0=cylinder, positive=expand)
	EFleshRingFalloffType FalloffType = EFleshRingFalloffType::WendlandC2;

public:
	FVirtualRingBulgeProvider() = default;

	/**
	 * Initialize from Ring parameters
	 * @param InRingCenter - Ring center (Component Space)
	 * @param InRingAxis - Ring axis direction (normalized vector)
	 * @param InRingRadius - Ring radius
	 * @param InRingHeight - Ring width (axial thickness)
	 * @param InAxialRange - Axial Bulge range multiplier
	 * @param InRadialRange - Radial Bulge range multiplier
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
	/** Calculate expanded AABB for Bulge region (for Spatial Hash query) */
	void CalculateExpandedBulgeAABB(FVector& OutMin, FVector& OutMax) const;
};

/**
 * Bulge region calculation for Virtual Band mode
 * Without SDF, applies Bulge at Upper/Lower Sections using 4-radius variable shape
 *
 * Differences from FVirtualRingBulgeProvider:
 * - Uses variable radius via GetRadiusAtHeight() instead of fixed radius
 * - Band Section (middle) excludes Bulge (only Tightness applied)
 * - Bulge only applied at Upper/Lower Sections
 */
class FLESHRINGRUNTIME_API FVirtualBandInfluenceProvider : public IBulgeRegionProvider
{
public:
	// Band transform (Component Space)
	FVector3f BandCenter = FVector3f::ZeroVector;
	FVector3f BandAxis = FVector3f(0.0f, 0.0f, 1.0f);

	// Virtual Band shape (4 radii + 3 heights)
	float LowerRadius = 9.0f;
	float MidLowerRadius = 8.0f;
	float MidUpperRadius = 8.0f;
	float UpperRadius = 11.0f;
	float LowerHeight = 1.0f;
	float BandHeight = 2.0f;
	float UpperHeight = 2.0f;

	// Bulge range parameters
	float AxialRange = 3.0f;		// Axial (up/down) range multiplier
	float RadialRange = 1.5f;		// Radial (side) range multiplier
	EFleshRingFalloffType FalloffType = EFleshRingFalloffType::WendlandC2;

public:
	FVirtualBandInfluenceProvider() = default;

	/**
	 * Initialize from Virtual Band parameters
	 * @param InLowerRadius - Bottom end radius
	 * @param InMidLowerRadius - Band lower radius
	 * @param InMidUpperRadius - Band upper radius
	 * @param InUpperRadius - Top end radius
	 * @param InLowerHeight - Lower Section height
	 * @param InBandHeight - Band Section height
	 * @param InUpperHeight - Upper Section height
	 * @param InCenter - Band center (Component Space)
	 * @param InAxis - Band axis direction (normalized vector)
	 */
	void InitFromBandSettings(
		float InLowerRadius, float InMidLowerRadius,
		float InMidUpperRadius, float InUpperRadius,
		float InLowerHeight, float InBandHeight, float InUpperHeight,
		const FVector3f& InCenter, const FVector3f& InAxis,
		float InAxialRange = 3.0f, float InRadialRange = 1.5f);

	/** Calculate variable radius at height */
	float GetRadiusAtHeight(float LocalZ) const;

	/** Total height */
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
	/** Calculate expanded AABB for Bulge region (for Spatial Hash query) */
	void CalculateExpandedBulgeAABB(FVector& OutMin, FVector& OutMax) const;

	/** Calculate Falloff */
	float CalculateFalloff(float Distance, float MaxDistance) const;
};
