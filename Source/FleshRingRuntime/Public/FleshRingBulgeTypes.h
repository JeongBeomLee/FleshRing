// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Math/Vector.h"

// Forward declaration for Spatial Hash
class FVertexSpatialHash;

// ============================================================================
// EBulgeDirection - Bulge Direction Mode
// ============================================================================

/**
 * Bulge deformation direction determination method
 * Auto: Auto-detect via SDF boundary vertex analysis
 * Positive/Negative: Manually specify +Z/-Z direction
 */
enum class EBulgeDirection : uint8
{
	/** Auto-detect via average Z of SDF boundary vertices */
	Auto = 0,

	/** Force +Z direction (upward) */
	Positive = 1,

	/** Force -Z direction (downward) */
	Negative = 2
};

// ============================================================================
// FBulgeDirectionDetector - Boundary Vertex Based Direction Detection
// ============================================================================

/**
 * Utility class that analyzes ring mesh boundary vertices (edge use count == 1)
 * to automatically detect Bulge direction
 *
 * Algorithm:
 * 1. Count usage of all edges
 * 2. Vertices with use count == 1 = boundary vertices
 * 3. Calculate average Z position of boundary vertices
 * 4. +1 if average Z is above SDF center, -1 if below
 */
class FBulgeDirectionDetector
{
public:
	/**
	 * Detect Bulge direction by analyzing ring mesh boundary vertices
	 *
	 * @param Vertices Mesh vertex array (local space)
	 * @param Indices Mesh index array (3 per triangle)
	 * @param SDFCenter SDF volume center point (local space)
	 * @return +1 (upward), -1 (downward), 0 (detection failed)
	 */
	static int32 DetectFromBoundaryVertices(
		const TArray<FVector3f>& Vertices,
		const TArray<uint32>& Indices,
		const FVector3f& SDFCenter)
	{
		if (Vertices.Num() == 0 || Indices.Num() < 3)
		{
			return 0;
		}

		// Count edge usage (edge = sorted vertex pair)
		TMap<TPair<uint32, uint32>, int32> EdgeUseCounts;

		// Iterate all triangle edges
		const int32 NumTriangles = Indices.Num() / 3;
		for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
		{
			uint32 Idx0 = Indices[TriIdx * 3 + 0];
			uint32 Idx1 = Indices[TriIdx * 3 + 1];
			uint32 Idx2 = Indices[TriIdx * 3 + 2];

			// Add 3 edges (smaller index always first)
			auto AddEdge = [&EdgeUseCounts](uint32 A, uint32 B)
			{
				TPair<uint32, uint32> Edge(FMath::Min(A, B), FMath::Max(A, B));
				EdgeUseCounts.FindOrAdd(Edge)++;
			};

			AddEdge(Idx0, Idx1);
			AddEdge(Idx1, Idx2);
			AddEdge(Idx2, Idx0);
		}

		// Collect vertices belonging to boundary edges (use count == 1)
		TSet<uint32> BoundaryVertexSet;
		for (const auto& Pair : EdgeUseCounts)
		{
			if (Pair.Value == 1)
			{
				BoundaryVertexSet.Add(Pair.Key.Key);
				BoundaryVertexSet.Add(Pair.Key.Value);
			}
		}

		if (BoundaryVertexSet.Num() == 0)
		{
			// No boundary (closed mesh) - bidirectional Bulge
			UE_LOG(LogTemp, Log, TEXT("FBulgeDirectionDetector: No boundary edges (closed mesh) - returning 0 for bidirectional"));
			return 0;
		}

		UE_LOG(LogTemp, Log, TEXT("FBulgeDirectionDetector: Found %d boundary vertices"), BoundaryVertexSet.Num());

		// Calculate average Z of boundary vertices
		float SumZ = 0.0f;
		int32 Count = 0;
		for (uint32 VertIdx : BoundaryVertexSet)
		{
			if (Vertices.IsValidIndex(VertIdx))
			{
				SumZ += Vertices[VertIdx].Z;
				Count++;
			}
		}

		if (Count == 0)
		{
			return 0;
		}

		const float AverageZ = SumZ / static_cast<float>(Count);

		// If boundary vertices are near center → Torus seam pattern → bidirectional
		const float Tolerance = 0.1f;  // Tolerance
		if (FMath::Abs(AverageZ - SDFCenter.Z) < Tolerance)
		{
			UE_LOG(LogTemp, Log, TEXT("FBulgeDirectionDetector: Boundary at center (AverageZ=%.2f ≈ SDFCenter.Z=%.2f) - Torus seam pattern, returning 0 for bidirectional"),
				AverageZ, SDFCenter.Z);
			return 0;  // Bidirectional
		}

		// Compare with SDF center Z
		int32 Result = (AverageZ > SDFCenter.Z) ? 1 : -1;
		UE_LOG(LogTemp, Log, TEXT("FBulgeDirectionDetector: AverageZ=%.2f, SDFCenter.Z=%.2f, Result=%d"),
			AverageZ, SDFCenter.Z, Result);
		return Result;
	}

	/**
	 * Return actual direction value based on EBulgeDirection
	 *
	 * @param Mode Direction mode
	 * @param AutoDetectedDirection Detected direction to use in Auto mode
	 * @return +1 (upward), -1 (downward)
	 */
	static int32 ResolveDirection(EBulgeDirection Mode, int32 AutoDetectedDirection)
	{
		switch (Mode)
		{
		case EBulgeDirection::Auto:
			return (AutoDetectedDirection != 0) ? AutoDetectedDirection : 1;
		case EBulgeDirection::Positive:
			return 1;
		case EBulgeDirection::Negative:
			return -1;
		default:
			return 1;
		}
	}
};

// ============================================================================
// IBulgeRegionProvider - Bulge Region Calculation Interface
// ============================================================================

/**
 * Bulge region calculation interface
 * Supports various methods including SDF-based, VirtualRing mode, etc.
 */
class IBulgeRegionProvider
{
public:
	virtual ~IBulgeRegionProvider() = default;

	/**
	 * Calculate Bulge region vertices
	 * @param AllVertexPositions - All mesh vertex positions (Component Space)
	 * @param SpatialHash - Spatial Hash (O(1) query, brute force if nullptr)
	 * @param OutBulgeVertexIndices - Output: Bulge-affected vertex indices
	 * @param OutBulgeInfluences - Output: Bulge influence for each vertex
	 * @param OutBulgeDirections - Output: Bulge directions (empty when calculated on GPU)
	 */
	virtual void CalculateBulgeRegion(
		const TArrayView<const FVector3f>& AllVertexPositions,
		const FVertexSpatialHash* SpatialHash,
		TArray<uint32>& OutBulgeVertexIndices,
		TArray<float>& OutBulgeInfluences,
		TArray<FVector3f>& OutBulgeDirections
	) const = 0;
};

/**
 * Bulge data calculated on CPU
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
