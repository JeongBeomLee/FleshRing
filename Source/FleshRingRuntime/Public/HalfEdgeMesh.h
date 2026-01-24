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
	// 부모 버텍스 정보 (Subdivision 시 기록)
	// 원본 버텍스: ParentIndex0 == ParentIndex1 == INDEX_NONE
	// Edge midpoint: 양쪽 부모 버텍스 인덱스
	// ========================================
	int32 ParentIndex0 = INDEX_NONE;
	int32 ParentIndex1 = INDEX_NONE;

	FHalfEdgeVertex() : Position(FVector::ZeroVector), UV(FVector2D::ZeroVector) {}
	FHalfEdgeVertex(const FVector& InPos) : Position(InPos), UV(FVector2D::ZeroVector) {}
	FHalfEdgeVertex(const FVector& InPos, const FVector2D& InUV) : Position(InPos), UV(InUV) {}
	FHalfEdgeVertex(const FVector& InPos, const FVector2D& InUV, int32 InParent0, int32 InParent1)
		: Position(InPos), UV(InUV), ParentIndex0(InParent0), ParentIndex1(InParent1) {}

	/** 원본 버텍스인지 확인 */
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
 * Torus parameters for subdivision influence region (Legacy)
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
 * DrawSdfVolume과 정확히 동일한 방식으로 OBB 체크
 *
 * DrawSdfVolume 방식:
 *   Center = LocalToComponent.TransformPosition(LocalCenter)
 *   Rotation = LocalToComponent.GetRotation()
 *   HalfExtents = LocalHalfExtents * LocalToComponent.GetScale3D()
 *   DrawDebugBox(Center, HalfExtents, Rotation)
 */
struct FSubdivisionOBB
{
	/** OBB 중심 (Component Space) */
	FVector Center = FVector::ZeroVector;

	/** OBB 축들 (Component Space, 정규화됨) - DrawSdfVolume의 WorldRotation과 동일 */
	FVector AxisX = FVector(1, 0, 0);
	FVector AxisY = FVector(0, 1, 0);
	FVector AxisZ = FVector(0, 0, 1);

	/** OBB 반크기 (각 축 방향) - DrawSdfVolume의 ScaledExtent와 동일 */
	FVector HalfExtents = FVector(10.0f);

	/** 추가 영향 범위 마진 */
	float InfluenceMargin = 5.0f;

	/** 디버그용: 로컬 바운드 */
	FVector LocalBoundsMin = FVector(-10.0f);
	FVector LocalBoundsMax = FVector(10.0f);

	/** 기본 생성자 */
	FSubdivisionOBB() = default;

	/**
	 * SDF 캐시 정보로부터 OBB 생성
	 * DrawSdfVolume과 완전히 동일한 계산 방식
	 *
	 * @param BoundsMin - 로컬 스페이스 AABB 최소점
	 * @param BoundsMax - 로컬 스페이스 AABB 최대점
	 * @param LocalToComponent - 로컬 → 컴포넌트 스페이스 변환
	 * @param InInfluenceMultiplier - 영향 범위 확장 배율
	 */
	static FSubdivisionOBB CreateFromSDFBounds(
		const FVector& BoundsMin,
		const FVector& BoundsMax,
		const FTransform& LocalToComponent,
		float InInfluenceMultiplier = 1.5f)
	{
		FSubdivisionOBB OBB;

		// 디버그용 로컬 바운드 저장
		OBB.LocalBoundsMin = BoundsMin;
		OBB.LocalBoundsMax = BoundsMax;

		// ========================================
		// DrawSdfVolume과 동일한 계산
		// ========================================

		// 로컬 스페이스 중심/반크기
		FVector LocalCenter = (BoundsMin + BoundsMax) * 0.5f;
		FVector LocalHalfExtents = (BoundsMax - BoundsMin) * 0.5f;

		// OBB 중심을 Component Space로 변환
		OBB.Center = LocalToComponent.TransformPosition(LocalCenter);

		// OBB 축들을 Component Space로 변환 (회전만 적용)
		FQuat Rotation = LocalToComponent.GetRotation();
		OBB.AxisX = Rotation.RotateVector(FVector(1, 0, 0));
		OBB.AxisY = Rotation.RotateVector(FVector(0, 1, 0));
		OBB.AxisZ = Rotation.RotateVector(FVector(0, 0, 1));

		// 반크기 (스케일 적용) - DrawSdfVolume의 ScaledExtent와 동일
		FVector Scale = LocalToComponent.GetScale3D();
		OBB.HalfExtents = LocalHalfExtents * Scale;

		// 영향 마진
		float MinDimension = (BoundsMax - BoundsMin).GetMin();
		OBB.InfluenceMargin = MinDimension * (InInfluenceMultiplier - 1.0f);

		return OBB;
	}

	/**
	 * 점이 OBB 영향 범위 내에 있는지 검사
	 * DrawSdfVolume에서 그리는 박스와 정확히 동일한 영역
	 *
	 * @param Point - 검사할 점 (컴포넌트 스페이스)
	 * @return 영향 범위 내이면 true
	 */
	bool IsPointInInfluence(const FVector& Point) const
	{
		// 점에서 OBB 중심까지의 벡터
		FVector D = Point - Center;

		// 각 OBB 축에 투영하여 범위 체크
		float ProjX = FMath::Abs(FVector::DotProduct(D, AxisX));
		float ProjY = FMath::Abs(FVector::DotProduct(D, AxisY));
		float ProjZ = FMath::Abs(FVector::DotProduct(D, AxisZ));

		// 마진 포함 범위 체크
		return ProjX <= HalfExtents.X + InfluenceMargin &&
		       ProjY <= HalfExtents.Y + InfluenceMargin &&
		       ProjZ <= HalfExtents.Z + InfluenceMargin;
	}

	/**
	 * OBB까지의 서명된 거리 (근사값)
	 * 양수: 외부, 음수: 내부
	 */
	float SignedDistance(const FVector& Point) const
	{
		// 점에서 OBB 중심까지의 벡터
		FVector D = Point - Center;

		// 각 축에 투영
		FVector LocalD(
			FVector::DotProduct(D, AxisX),
			FVector::DotProduct(D, AxisY),
			FVector::DotProduct(D, AxisZ)
		);

		// 각 축에 대한 초과 거리 계산
		FVector Q;
		for (int32 i = 0; i < 3; i++)
		{
			Q[i] = FMath::Max(0.0f, FMath::Abs(LocalD[i]) - HalfExtents[i]);
		}

		// 외부 거리
		float OutsideDist = Q.Size();

		// 내부 거리
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
	 * Subdivide faces that intersect with the torus influence region (Legacy)
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
	 * 전체 메시를 균일하게 Subdivision (에디터 프리뷰용)
	 * 영역 체크 없이 모든 삼각형을 분할
	 * Red-Green refinement로 T-junction 방지
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
	 * 선택된 삼각형들만 Subdivision (에디터 프리뷰용 - 본 기반 최적화)
	 * 지정된 삼각형 인덱스 집합만 분할
	 * Red-Green refinement로 T-junction 방지
	 *
	 * @param Mesh - Half-edge mesh to subdivide (modified in place)
	 * @param TargetFaces - 대상 삼각형 인덱스 집합
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
