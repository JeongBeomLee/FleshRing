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

	FHalfEdgeVertex() : Position(FVector::ZeroVector), UV(FVector2D::ZeroVector) {}
	FHalfEdgeVertex(const FVector& InPos) : Position(InPos), UV(FVector2D::ZeroVector) {}
	FHalfEdgeVertex(const FVector& InPos, const FVector2D& InUV) : Position(InPos), UV(InUV) {}
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

	// Build from triangle mesh data
	bool BuildFromTriangles(const TArray<FVector>& InVertices, const TArray<int32>& InTriangles, const TArray<FVector2D>& InUVs);

	// Export to triangle mesh data
	void ExportToTriangles(TArray<FVector>& OutVertices, TArray<int32>& OutTriangles, TArray<FVector2D>& OutUVs, TArray<FVector>& OutNormals) const;

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
 * Torus parameters for subdivision influence region
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
 * Red-Green Refinement subdivision algorithm
 * Provides crack-free adaptive subdivision
 */
class FLESHRINGRUNTIME_API FLEBSubdivision
{
public:
	/**
	 * Subdivide faces that intersect with the torus influence region
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
