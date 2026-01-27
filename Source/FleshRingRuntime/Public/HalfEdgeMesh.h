// Copyright 2026 LgThx. All Rights Reserved.

// HalfEdgeMesh.h
// Half-Edge data structure for topology-aware mesh operations
// Supports Red-Green Refinement for crack-free adaptive subdivision

#pragma once

#include "CoreMinimal.h"

/**
 * Half-Edge structure for mesh topology traversal
 * Each edge in a mesh is represented by two half-edges (twins)
 */
struct FHalfEdge
{
	int32 VertexIndex = -1;      // Vertex this half-edge points TO
	int32 TwinIndex = -1;        // Opposite half-edge (in adjacent face), -1 if boundary
	int32 NextIndex = -1;        // Next half-edge in the same face (CCW)
	int32 PrevIndex = -1;        // Previous half-edge in the same face
	int32 FaceIndex = -1;        // Face this half-edge belongs to

	bool IsBoundary() const { return TwinIndex == -1; }
};

/**
 * Face (Triangle) in Half-Edge mesh
 */
struct FHalfEdgeFace
{
	int32 HalfEdgeIndex = -1;    // One of the half-edges of this face
	int32 SubdivisionLevel = 0;  // How many times this face has been subdivided
	int32 MaterialIndex = 0;     // Material slot index for this face (inherited during subdivision)
	bool bMarkedForSubdivision = false;
};

/**
 * Vertex in Half-Edge mesh
 */
struct FHalfEdgeVertex
{
	FVector Position;
	FVector2D UV;
	int32 HalfEdgeIndex = -1;    // One of the outgoing half-edges from this vertex

	// ========================================
	// Parent vertex information (recorded during Subdivision)
	// Original vertex: ParentIndex0 == ParentIndex1 == INDEX_NONE
	// Edge midpoint: parent vertex indices from both ends
	// ========================================
	int32 ParentIndex0 = INDEX_NONE;
	int32 ParentIndex1 = INDEX_NONE;

	FHalfEdgeVertex() : Position(FVector::ZeroVector), UV(FVector2D::ZeroVector) {}
	FHalfEdgeVertex(const FVector& InPos) : Position(InPos), UV(FVector2D::ZeroVector) {}
	FHalfEdgeVertex(const FVector& InPos, const FVector2D& InUV) : Position(InPos), UV(InUV) {}
	FHalfEdgeVertex(const FVector& InPos, const FVector2D& InUV, int32 InParent0, int32 InParent1)
		: Position(InPos), UV(InUV), ParentIndex0(InParent0), ParentIndex1(InParent1) {}

	/** Check if this is an original vertex */
	bool IsOriginalVertex() const { return ParentIndex0 == INDEX_NONE && ParentIndex1 == INDEX_NONE; }
};

/**
 * Half-Edge Mesh structure
 * Provides O(1) adjacency queries for mesh operations
 */
class FLESHRINGRUNTIME_API FHalfEdgeMesh
{
public:
	TArray<FHalfEdgeVertex> Vertices;
	TArray<FHalfEdge> HalfEdges;
	TArray<FHalfEdgeFace> Faces;

	// Build from triangle mesh data (MaterialIndices: per-triangle material index, optional)
	// ParentIndices: per-vertex parent info (optional, pairs of int32 for each vertex)
	bool BuildFromTriangles(
		const TArray<FVector>& InVertices,
		const TArray<int32>& InTriangles,
		const TArray<FVector2D>& InUVs,
		const TArray<int32>& InMaterialIndices = TArray<int32>(),
		const TArray<TPair<int32, int32>>* InParentIndices = nullptr);

	// Export to triangle mesh data (OutMaterialIndices: per-triangle material index)
	void ExportToTriangles(TArray<FVector>& OutVertices, TArray<int32>& OutTriangles, TArray<FVector2D>& OutUVs, TArray<FVector>& OutNormals, TArray<int32>& OutMaterialIndices) const;

	// Get the three vertex indices of a face
	void GetFaceVertices(int32 FaceIndex, int32& OutV0, int32& OutV1, int32& OutV2) const;

	// Get the three half-edge indices of a face
	void GetFaceHalfEdges(int32 FaceIndex, int32& OutHE0, int32& OutHE1, int32& OutHE2) const;

	// Get the longest edge of a face (returns half-edge index)
	int32 GetLongestEdge(int32 FaceIndex) const;

	// Get edge length
	float GetEdgeLength(int32 HalfEdgeIndex) const;

	// Get edge midpoint
	FVector GetEdgeMidpoint(int32 HalfEdgeIndex) const;

	// Get the opposite vertex of an edge in a face
	int32 GetOppositeVertex(int32 HalfEdgeIndex) const;

	// Check if a face intersects with a sphere/torus region
	bool FaceIntersectsRegion(int32 FaceIndex, const FVector& RegionCenter, float RegionRadius) const;

	// Validate mesh integrity (for debugging)
	bool Validate() const;

	// Clear all data
	void Clear();

	// Get counts
	int32 GetVertexCount() const { return Vertices.Num(); }
	int32 GetFaceCount() const { return Faces.Num(); }
	int32 GetHalfEdgeCount() const { return HalfEdges.Num(); }

private:
	// Helper to find twin half-edge during construction
	TMap<TPair<int32, int32>, int32> EdgeToHalfEdge;
};

/**
 * Torus parameters for subdivision influence region (for VirtualRing mode)
 */
struct FTorusParams
{
	FVector Center = FVector::ZeroVector;
	FVector Axis = FVector(0, 1, 0);  // Ring axis direction
	float MajorRadius = 22.0f;        // Distance from center to tube center
	float MinorRadius = 5.0f;         // Tube thickness
	float InfluenceMargin = 10.0f;    // Extra margin around torus for subdivision
};

/**
 * OBB (Oriented Bounding Box) for subdivision influence region
 * OBB check using exactly the same method as DrawSdfVolume
 *
 * DrawSdfVolume method:
 *   Center = LocalToComponent.TransformPosition(LocalCenter)
 *   Rotation = LocalToComponent.GetRotation()
 *   HalfExtents = LocalHalfExtents * LocalToComponent.GetScale3D()
 *   DrawDebugBox(Center, HalfExtents, Rotation)
 */
struct FSubdivisionOBB
{
	/** OBB center (Component Space) */
	FVector Center = FVector::ZeroVector;

	/** OBB axes (Component Space, normalized) - same as DrawSdfVolume's WorldRotation */
	FVector AxisX = FVector(1, 0, 0);
	FVector AxisY = FVector(0, 1, 0);
	FVector AxisZ = FVector(0, 0, 1);

	/** OBB half extents (in each axis direction) - same as DrawSdfVolume's ScaledExtent */
	FVector HalfExtents = FVector(10.0f);

	/** Additional influence range margin */
	float InfluenceMargin = 5.0f;

	/** For debugging: local bounds */
	FVector LocalBoundsMin = FVector(-10.0f);
	FVector LocalBoundsMax = FVector(10.0f);

	/** Default constructor */
	FSubdivisionOBB() = default;

	/**
	 * Create OBB from SDF cache information
	 * Uses exactly the same calculation method as DrawSdfVolume
	 *
	 * @param BoundsMin - Local space AABB minimum point
	 * @param BoundsMax - Local space AABB maximum point
	 * @param LocalToComponent - Local to Component space transform
	 * @param InInfluenceMultiplier - Influence range expansion multiplier
	 */
	static FSubdivisionOBB CreateFromSDFBounds(
		const FVector& BoundsMin,
		const FVector& BoundsMax,
		const FTransform& LocalToComponent,
		float InInfluenceMultiplier = 1.5f)
	{
		FSubdivisionOBB OBB;

		// Save local bounds for debugging
		OBB.LocalBoundsMin = BoundsMin;
		OBB.LocalBoundsMax = BoundsMax;

		// ========================================
		// Same calculation as DrawSdfVolume
		// ========================================

		// Local space center/half extents
		FVector LocalCenter = (BoundsMin + BoundsMax) * 0.5f;
		FVector LocalHalfExtents = (BoundsMax - BoundsMin) * 0.5f;

		// Transform OBB center to Component Space
		OBB.Center = LocalToComponent.TransformPosition(LocalCenter);

		// Transform OBB axes to Component Space (rotation only)
		FQuat Rotation = LocalToComponent.GetRotation();
		OBB.AxisX = Rotation.RotateVector(FVector(1, 0, 0));
		OBB.AxisY = Rotation.RotateVector(FVector(0, 1, 0));
		OBB.AxisZ = Rotation.RotateVector(FVector(0, 0, 1));

		// Half extents (with scale applied) - same as DrawSdfVolume's ScaledExtent
		FVector Scale = LocalToComponent.GetScale3D();
		OBB.HalfExtents = LocalHalfExtents * Scale;

		// Influence margin
		float MinDimension = (BoundsMax - BoundsMin).GetMin();
		OBB.InfluenceMargin = MinDimension * (InInfluenceMultiplier - 1.0f);

		return OBB;
	}

	/**
	 * Check if a point is within the OBB influence range
	 * Exactly the same region as the box drawn by DrawSdfVolume
	 *
	 * @param Point - Point to check (Component Space)
	 * @return true if within influence range
	 */
	bool IsPointInInfluence(const FVector& Point) const
	{
		// Vector from point to OBB center
		FVector D = Point - Center;

		// Project onto each OBB axis and check range
		float ProjX = FMath::Abs(FVector::DotProduct(D, AxisX));
		float ProjY = FMath::Abs(FVector::DotProduct(D, AxisY));
		float ProjZ = FMath::Abs(FVector::DotProduct(D, AxisZ));

		// Range check including margin
		return ProjX <= HalfExtents.X + InfluenceMargin &&
		       ProjY <= HalfExtents.Y + InfluenceMargin &&
		       ProjZ <= HalfExtents.Z + InfluenceMargin;
	}

	/**
	 * Signed distance to OBB (approximation)
	 * Positive: outside, Negative: inside
	 */
	float SignedDistance(const FVector& Point) const
	{
		// Vector from point to OBB center
		FVector D = Point - Center;

		// Project onto each axis
		FVector LocalD(
			FVector::DotProduct(D, AxisX),
			FVector::DotProduct(D, AxisY),
			FVector::DotProduct(D, AxisZ)
		);

		// Calculate excess distance for each axis
		FVector Q;
		for (int32 i = 0; i < 3; i++)
		{
			Q[i] = FMath::Max(0.0f, FMath::Abs(LocalD[i]) - HalfExtents[i]);
		}

		// Outside distance
		float OutsideDist = Q.Size();

		// Inside distance
		float InsideDist = 0.0f;
		if (OutsideDist == 0.0f)
		{
			float MinAxisDist = FLT_MAX;
			for (int32 i = 0; i < 3; i++)
			{
				float DistToFace = HalfExtents[i] - FMath::Abs(LocalD[i]);
				MinAxisDist = FMath::Min(MinAxisDist, DistToFace);
			}
			InsideDist = -MinAxisDist;
		}

		return OutsideDist > 0.0f ? OutsideDist : InsideDist;
	}
};

/**
 * Red-Green Refinement subdivision algorithm
 * Provides crack-free adaptive subdivision
 */
class FLESHRINGRUNTIME_API FLEBSubdivision
{
public:
	/**
	 * Subdivide faces that intersect with the torus influence region (for VirtualRing mode)
	 * Uses Red-Green refinement to ensure no T-junctions
	 *
	 * @param Mesh - Half-edge mesh to subdivide (modified in place)
	 * @param TorusParams - Torus shape parameters defining influence region
	 * @param MaxLevel - Maximum subdivision depth
	 * @param MinEdgeLength - Stop subdividing when edges are smaller than this
	 * @return Number of faces added
	 */
	static int32 SubdivideRegion(
		FHalfEdgeMesh& Mesh,
		const FTorusParams& TorusParams,
		int32 MaxLevel = 4,
		float MinEdgeLength = 1.0f
	);

	/**
	 * Subdivide faces that intersect with the OBB influence region
	 * Uses Red-Green refinement to ensure no T-junctions
	 *
	 * @param Mesh - Half-edge mesh to subdivide (modified in place)
	 * @param OBB - Oriented Bounding Box defining influence region
	 * @param MaxLevel - Maximum subdivision depth
	 * @param MinEdgeLength - Stop subdividing when edges are smaller than this
	 * @return Number of faces added
	 */
	static int32 SubdivideRegion(
		FHalfEdgeMesh& Mesh,
		const FSubdivisionOBB& OBB,
		int32 MaxLevel = 4,
		float MinEdgeLength = 1.0f
	);

	/**
	 * Uniformly subdivide the entire mesh (for editor preview)
	 * Subdivides all triangles without region check
	 * Prevents T-junctions using Red-Green refinement
	 *
	 * @param Mesh - Half-edge mesh to subdivide (modified in place)
	 * @param MaxLevel - Maximum subdivision depth
	 * @param MinEdgeLength - Stop subdividing when edges are smaller than this
	 * @return Number of faces added
	 */
	static int32 SubdivideUniform(
		FHalfEdgeMesh& Mesh,
		int32 MaxLevel = 2,
		float MinEdgeLength = 1.0f
	);

	/**
	 * Subdivide only selected triangles (for editor preview - bone-based optimization)
	 * Subdivides only the specified set of triangle indices
	 * Prevents T-junctions using Red-Green refinement
	 *
	 * @param Mesh - Half-edge mesh to subdivide (modified in place)
	 * @param TargetFaces - Set of target triangle indices
	 * @param MaxLevel - Maximum subdivision depth
	 * @param MinEdgeLength - Stop subdividing when edges are smaller than this
	 * @return Number of faces added
	 */
	static int32 SubdivideSelectedFaces(
		FHalfEdgeMesh& Mesh,
		const TSet<int32>& TargetFaces,
		int32 MaxLevel = 2,
		float MinEdgeLength = 1.0f
	);

	/**
	 * Subdivide a single edge using LEB
	 * Automatically propagates to maintain mesh consistency
	 *
	 * @param Mesh - Half-edge mesh
	 * @param HalfEdgeIndex - The edge to split
	 * @return Index of the new vertex at the midpoint
	 */
	static int32 SplitEdge(FHalfEdgeMesh& Mesh, int32 HalfEdgeIndex);

	/**
	 * Subdivide a single face into 4 triangles (1-to-4 split)
	 * Creates midpoints on each edge and splits into 4 sub-triangles
	 * Simpler and more robust than edge-based splitting
	 */
	static void SubdivideFace4(FHalfEdgeMesh& Mesh, int32 FaceIndex);

private:
	/**
	 * Recursively ensure the edge being split is the longest in its face
	 * This is key to LEB - we may need to split other edges first
	 */
	static void EnsureLongestEdge(FHalfEdgeMesh& Mesh, int32 HalfEdgeIndex, TSet<int32>& ProcessedFaces);

	/**
	 * Split a face by its longest edge
	 * Creates two new faces from one
	 */
	static void SplitFaceByEdge(FHalfEdgeMesh& Mesh, int32 FaceIndex, int32 MidpointVertex);
};
