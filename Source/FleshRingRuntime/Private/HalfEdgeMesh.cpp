// Copyright 2026 LgThx. All Rights Reserved.

// HalfEdgeMesh.cpp
// Implementation of Half-Edge mesh and Red-Green adaptive subdivision

#include "HalfEdgeMesh.h"

//=============================================================================
// FHalfEdgeMesh Implementation
//=============================================================================

bool FHalfEdgeMesh::BuildFromTriangles(
	const TArray<FVector>& InVertices,
	const TArray<int32>& InTriangles,
	const TArray<FVector2D>& InUVs,
	const TArray<int32>& InMaterialIndices,
	const TArray<TPair<int32, int32>>* InParentIndices)
{
	Clear();

	if (InTriangles.Num() < 3 || InTriangles.Num() % 3 != 0)
	{
		UE_LOG(LogTemp, Error, TEXT("HalfEdgeMesh: Invalid triangle count %d"), InTriangles.Num());
		return false;
	}

	// Copy vertices with optional parent info
	Vertices.Reserve(InVertices.Num());
	for (int32 i = 0; i < InVertices.Num(); i++)
	{
		FHalfEdgeVertex Vert;
		Vert.Position = InVertices[i];
		Vert.UV = InUVs.IsValidIndex(i) ? InUVs[i] : FVector2D::ZeroVector;
		Vert.HalfEdgeIndex = -1;

		// 부모 정보 설정 (있으면)
		if (InParentIndices && InParentIndices->IsValidIndex(i))
		{
			Vert.ParentIndex0 = (*InParentIndices)[i].Key;
			Vert.ParentIndex1 = (*InParentIndices)[i].Value;
		}

		Vertices.Add(Vert);
	}

	int32 NumFaces = InTriangles.Num() / 3;
	Faces.Reserve(NumFaces);
	HalfEdges.Reserve(NumFaces * 3);
	EdgeToHalfEdge.Reserve(NumFaces * 3);

	// Create faces and half-edges
	for (int32 FaceIdx = 0; FaceIdx < NumFaces; FaceIdx++)
	{
		int32 BaseIdx = FaceIdx * 3;
		int32 V0 = InTriangles[BaseIdx];
		int32 V1 = InTriangles[BaseIdx + 1];
		int32 V2 = InTriangles[BaseIdx + 2];

		if (V0 < 0 || V0 >= Vertices.Num() ||
			V1 < 0 || V1 >= Vertices.Num() ||
			V2 < 0 || V2 >= Vertices.Num())
		{
			UE_LOG(LogTemp, Error, TEXT("HalfEdgeMesh: Invalid vertex index in face %d"), FaceIdx);
			continue;
		}

		FHalfEdgeFace Face;
		Face.HalfEdgeIndex = HalfEdges.Num();
		Face.SubdivisionLevel = 0;
		Face.MaterialIndex = InMaterialIndices.IsValidIndex(FaceIdx) ? InMaterialIndices[FaceIdx] : 0;
		int32 NewFaceIdx = Faces.Add(Face);

		int32 HE0 = HalfEdges.Num();
		int32 HE1 = HE0 + 1;
		int32 HE2 = HE0 + 2;

		FHalfEdge Edge0, Edge1, Edge2;

		Edge0.VertexIndex = V1;
		Edge0.NextIndex = HE1;
		Edge0.PrevIndex = HE2;
		Edge0.FaceIndex = NewFaceIdx;
		Edge0.TwinIndex = -1;

		Edge1.VertexIndex = V2;
		Edge1.NextIndex = HE2;
		Edge1.PrevIndex = HE0;
		Edge1.FaceIndex = NewFaceIdx;
		Edge1.TwinIndex = -1;

		Edge2.VertexIndex = V0;
		Edge2.NextIndex = HE0;
		Edge2.PrevIndex = HE1;
		Edge2.FaceIndex = NewFaceIdx;
		Edge2.TwinIndex = -1;

		HalfEdges.Add(Edge0);
		HalfEdges.Add(Edge1);
		HalfEdges.Add(Edge2);

		if (Vertices[V0].HalfEdgeIndex == -1) Vertices[V0].HalfEdgeIndex = HE0;
		if (Vertices[V1].HalfEdgeIndex == -1) Vertices[V1].HalfEdgeIndex = HE1;
		if (Vertices[V2].HalfEdgeIndex == -1) Vertices[V2].HalfEdgeIndex = HE2;

		auto MakeEdgeKey = [](int32 A, int32 B) -> TPair<int32, int32>
		{
			return A < B ? TPair<int32, int32>(A, B) : TPair<int32, int32>(B, A);
		};

		TPair<int32, int32> Key01 = MakeEdgeKey(V0, V1);
		if (int32* ExistingHE = EdgeToHalfEdge.Find(Key01))
		{
			HalfEdges[HE0].TwinIndex = *ExistingHE;
			HalfEdges[*ExistingHE].TwinIndex = HE0;
		}
		else
		{
			EdgeToHalfEdge.Add(Key01, HE0);
		}

		TPair<int32, int32> Key12 = MakeEdgeKey(V1, V2);
		if (int32* ExistingHE = EdgeToHalfEdge.Find(Key12))
		{
			HalfEdges[HE1].TwinIndex = *ExistingHE;
			HalfEdges[*ExistingHE].TwinIndex = HE1;
		}
		else
		{
			EdgeToHalfEdge.Add(Key12, HE1);
		}

		TPair<int32, int32> Key20 = MakeEdgeKey(V2, V0);
		if (int32* ExistingHE = EdgeToHalfEdge.Find(Key20))
		{
			HalfEdges[HE2].TwinIndex = *ExistingHE;
			HalfEdges[*ExistingHE].TwinIndex = HE2;
		}
		else
		{
			EdgeToHalfEdge.Add(Key20, HE2);
		}
	}

	return true;
}

void FHalfEdgeMesh::ExportToTriangles(TArray<FVector>& OutVertices, TArray<int32>& OutTriangles, TArray<FVector2D>& OutUVs, TArray<FVector>& OutNormals, TArray<int32>& OutMaterialIndices) const
{
	OutVertices.Empty();
	OutTriangles.Empty();
	OutUVs.Empty();
	OutNormals.Empty();
	OutMaterialIndices.Empty();

	OutVertices.Reserve(Vertices.Num());
	OutUVs.Reserve(Vertices.Num());
	for (const FHalfEdgeVertex& Vert : Vertices)
	{
		OutVertices.Add(Vert.Position);
		OutUVs.Add(Vert.UV);
	}

	OutTriangles.Reserve(Faces.Num() * 3);
	OutMaterialIndices.Reserve(Faces.Num());
	for (int32 FaceIdx = 0; FaceIdx < Faces.Num(); FaceIdx++)
	{
		int32 V0, V1, V2;
		GetFaceVertices(FaceIdx, V0, V1, V2);

		if (V0 >= 0 && V1 >= 0 && V2 >= 0)
		{
			OutTriangles.Add(V0);
			OutTriangles.Add(V1);
			OutTriangles.Add(V2);
			OutMaterialIndices.Add(Faces[FaceIdx].MaterialIndex);
		}
	}

	OutNormals.SetNum(OutVertices.Num());
	for (FVector& Normal : OutNormals)
	{
		Normal = FVector::ZeroVector;
	}

	int32 NumTris = OutTriangles.Num() / 3;
	for (int32 i = 0; i < NumTris; i++)
	{
		int32 V0 = OutTriangles[i * 3];
		int32 V1 = OutTriangles[i * 3 + 1];
		int32 V2 = OutTriangles[i * 3 + 2];

		if (V0 < 0 || V1 < 0 || V2 < 0 ||
			V0 >= OutVertices.Num() || V1 >= OutVertices.Num() || V2 >= OutVertices.Num())
			continue;

		FVector Edge1 = OutVertices[V1] - OutVertices[V0];
		FVector Edge2 = OutVertices[V2] - OutVertices[V0];
		FVector FaceNormal = FVector::CrossProduct(Edge1, Edge2).GetSafeNormal();

		OutNormals[V0] += FaceNormal;
		OutNormals[V1] += FaceNormal;
		OutNormals[V2] += FaceNormal;
	}

	for (FVector& Normal : OutNormals)
	{
		Normal = Normal.GetSafeNormal();
		if (Normal.IsNearlyZero())
		{
			Normal = FVector::UpVector;
		}
	}
}

void FHalfEdgeMesh::GetFaceVertices(int32 FaceIndex, int32& OutV0, int32& OutV1, int32& OutV2) const
{
	OutV0 = OutV1 = OutV2 = -1;

	if (!Faces.IsValidIndex(FaceIndex)) return;

	int32 HE0 = Faces[FaceIndex].HalfEdgeIndex;
	if (HE0 < 0 || !HalfEdges.IsValidIndex(HE0)) return;

	int32 HE1 = HalfEdges[HE0].NextIndex;
	if (!HalfEdges.IsValidIndex(HE1)) return;

	int32 HE2 = HalfEdges[HE1].NextIndex;
	if (!HalfEdges.IsValidIndex(HE2)) return;

	OutV0 = HalfEdges[HE2].VertexIndex;
	OutV1 = HalfEdges[HE0].VertexIndex;
	OutV2 = HalfEdges[HE1].VertexIndex;
}

void FHalfEdgeMesh::GetFaceHalfEdges(int32 FaceIndex, int32& OutHE0, int32& OutHE1, int32& OutHE2) const
{
	OutHE0 = OutHE1 = OutHE2 = -1;

	if (!Faces.IsValidIndex(FaceIndex)) return;

	OutHE0 = Faces[FaceIndex].HalfEdgeIndex;
	if (OutHE0 < 0 || !HalfEdges.IsValidIndex(OutHE0)) return;

	OutHE1 = HalfEdges[OutHE0].NextIndex;
	if (!HalfEdges.IsValidIndex(OutHE1)) return;

	OutHE2 = HalfEdges[OutHE1].NextIndex;
}

int32 FHalfEdgeMesh::GetLongestEdge(int32 FaceIndex) const
{
	int32 HE0, HE1, HE2;
	GetFaceHalfEdges(FaceIndex, HE0, HE1, HE2);

	if (HE0 < 0) return -1;

	float Len0 = GetEdgeLength(HE0);
	float Len1 = GetEdgeLength(HE1);
	float Len2 = GetEdgeLength(HE2);

	if (Len0 >= Len1 && Len0 >= Len2) return HE0;
	if (Len1 >= Len0 && Len1 >= Len2) return HE1;
	return HE2;
}

float FHalfEdgeMesh::GetEdgeLength(int32 HalfEdgeIndex) const
{
	if (!HalfEdges.IsValidIndex(HalfEdgeIndex)) return 0.0f;

	const FHalfEdge& HE = HalfEdges[HalfEdgeIndex];
	if (!HalfEdges.IsValidIndex(HE.PrevIndex)) return 0.0f;

	int32 StartVert = HalfEdges[HE.PrevIndex].VertexIndex;
	int32 EndVert = HE.VertexIndex;

	if (!Vertices.IsValidIndex(StartVert) || !Vertices.IsValidIndex(EndVert)) return 0.0f;

	return FVector::Dist(Vertices[StartVert].Position, Vertices[EndVert].Position);
}

FVector FHalfEdgeMesh::GetEdgeMidpoint(int32 HalfEdgeIndex) const
{
	if (!HalfEdges.IsValidIndex(HalfEdgeIndex)) return FVector::ZeroVector;

	const FHalfEdge& HE = HalfEdges[HalfEdgeIndex];
	if (!HalfEdges.IsValidIndex(HE.PrevIndex)) return FVector::ZeroVector;

	int32 StartVert = HalfEdges[HE.PrevIndex].VertexIndex;
	int32 EndVert = HE.VertexIndex;

	if (!Vertices.IsValidIndex(StartVert) || !Vertices.IsValidIndex(EndVert)) return FVector::ZeroVector;

	return (Vertices[StartVert].Position + Vertices[EndVert].Position) * 0.5f;
}

int32 FHalfEdgeMesh::GetOppositeVertex(int32 HalfEdgeIndex) const
{
	if (!HalfEdges.IsValidIndex(HalfEdgeIndex)) return -1;

	int32 NextHE = HalfEdges[HalfEdgeIndex].NextIndex;
	if (!HalfEdges.IsValidIndex(NextHE)) return -1;

	return HalfEdges[NextHE].VertexIndex;
}

bool FHalfEdgeMesh::FaceIntersectsRegion(int32 FaceIndex, const FVector& RegionCenter, float RegionRadius) const
{
	int32 V0, V1, V2;
	GetFaceVertices(FaceIndex, V0, V1, V2);

	if (V0 < 0 || V1 < 0 || V2 < 0) return false;

	const FVector& P0 = Vertices[V0].Position;
	const FVector& P1 = Vertices[V1].Position;
	const FVector& P2 = Vertices[V2].Position;

	if (FVector::Dist(P0, RegionCenter) <= RegionRadius) return true;
	if (FVector::Dist(P1, RegionCenter) <= RegionRadius) return true;
	if (FVector::Dist(P2, RegionCenter) <= RegionRadius) return true;

	FVector Center = (P0 + P1 + P2) / 3.0f;
	if (FVector::Dist(Center, RegionCenter) <= RegionRadius) return true;

	return false;
}

bool FHalfEdgeMesh::Validate() const
{
	bool bValid = true;

	for (int32 i = 0; i < HalfEdges.Num(); i++)
	{
		const FHalfEdge& HE = HalfEdges[i];

		if (HE.TwinIndex != -1)
		{
			if (!HalfEdges.IsValidIndex(HE.TwinIndex))
			{
				bValid = false;
			}
			else if (HalfEdges[HE.TwinIndex].TwinIndex != i)
			{
				bValid = false;
			}
		}
	}

	return bValid;
}

void FHalfEdgeMesh::Clear()
{
	Vertices.Empty();
	HalfEdges.Empty();
	Faces.Empty();
	EdgeToHalfEdge.Empty();
}

//=============================================================================
// FLEBSubdivision - Red-Green Refinement Implementation
//=============================================================================

int32 FLEBSubdivision::SubdivideRegion(
	FHalfEdgeMesh& Mesh,
	const FTorusParams& Torus,
	int32 MaxLevel,
	float MinEdgeLength)
{
	// Extract torus parameters
	const FVector& TorusCenter = Torus.Center;
	FVector TorusAxis = Torus.Axis.GetSafeNormal();
	if (TorusAxis.IsNearlyZero()) TorusAxis = FVector(0, 1, 0);

	float TorusMajorRadius = Torus.MajorRadius;
	float TorusMinorRadius = Torus.MinorRadius;
	float InfluenceMargin = Torus.InfluenceMargin;

	// Step 1: Export current mesh to simple triangle format
	TArray<FVector> Positions;
	TArray<FVector2D> UVs;
	TArray<int32> Triangles;
	TArray<int32> MaterialIndices;  // Per-triangle material index
	TArray<TPair<int32, int32>> ParentIndices;  // Per-vertex parent info

	Positions.Reserve(Mesh.Vertices.Num());
	UVs.Reserve(Mesh.Vertices.Num());
	ParentIndices.Reserve(Mesh.Vertices.Num());
	for (const FHalfEdgeVertex& V : Mesh.Vertices)
	{
		Positions.Add(V.Position);
		UVs.Add(V.UV);
		// 부모 정보 유지 (멀티레벨 Subdivision용)
		// 원본 버텍스: INDEX_NONE, 이전 레벨 생성 버텍스: 부모 인덱스 보존
		ParentIndices.Add(TPair<int32, int32>(V.ParentIndex0, V.ParentIndex1));
	}

	for (int32 FaceIdx = 0; FaceIdx < Mesh.Faces.Num(); FaceIdx++)
	{
		int32 V0, V1, V2;
		Mesh.GetFaceVertices(FaceIdx, V0, V1, V2);
		if (V0 >= 0 && V1 >= 0 && V2 >= 0)
		{
			Triangles.Add(V0);
			Triangles.Add(V1);
			Triangles.Add(V2);
			MaterialIndices.Add(Mesh.Faces[FaceIdx].MaterialIndex);
		}
	}

	int32 InitialTriCount = Triangles.Num() / 3;

	// ============================================================================
	// Dual Midpoint Map System - Torus Version
	// ============================================================================
	constexpr float MidpointWeldPrecision = 0.1f;

	auto PositionToKey = [MidpointWeldPrecision](const FVector& Pos) -> FIntVector
	{
		return FIntVector(
			FMath::RoundToInt(Pos.X / MidpointWeldPrecision),
			FMath::RoundToInt(Pos.Y / MidpointWeldPrecision),
			FMath::RoundToInt(Pos.Z / MidpointWeldPrecision)
		);
	};

	auto IntVectorLess = [](const FIntVector& A, const FIntVector& B) -> bool
	{
		if (A.X != B.X) return A.X < B.X;
		if (A.Y != B.Y) return A.Y < B.Y;
		return A.Z < B.Z;
	};

	// 1. Position-based: GREEN split 감지용
	TSet<TPair<FIntVector, FIntVector>> PositionMidpointSet;

	auto MakePositionKey = [&](int32 VA, int32 VB) -> TPair<FIntVector, FIntVector>
	{
		FIntVector KeyA = PositionToKey(Positions[VA]);
		FIntVector KeyB = PositionToKey(Positions[VB]);
		if (IntVectorLess(KeyA, KeyB))
			return TPair<FIntVector, FIntVector>(KeyA, KeyB);
		else
			return TPair<FIntVector, FIntVector>(KeyB, KeyA);
	};

	// 2. Index-based: 버텍스 재사용용 (UV 보존)
	TMap<TPair<int32, int32>, int32> IndexMidpointMap;

	auto MakeIndexKey = [](int32 A, int32 B) -> TPair<int32, int32>
	{
		return A < B ? TPair<int32, int32>(A, B) : TPair<int32, int32>(B, A);
	};

	auto GetOrCreateMidpoint = [&](int32 VA, int32 VB) -> int32
	{
		TPair<int32, int32> IndexKey = MakeIndexKey(VA, VB);
		if (int32* Existing = IndexMidpointMap.Find(IndexKey))
		{
			return *Existing;
		}

		int32 NewIdx = Positions.Num();
		Positions.Add((Positions[VA] + Positions[VB]) * 0.5f);
		UVs.Add((UVs[VA] + UVs[VB]) * 0.5f);
		ParentIndices.Add(TPair<int32, int32>(VA, VB));

		IndexMidpointMap.Add(IndexKey, NewIdx);
		PositionMidpointSet.Add(MakePositionKey(VA, VB));
		return NewIdx;
	};

	auto HasMidpointAtEdge = [&](int32 VA, int32 VB) -> bool
	{
		return PositionMidpointSet.Contains(MakePositionKey(VA, VB));
	};

	// Torus SDF helper - returns signed distance to torus surface
	// Uses arbitrary axis direction
	auto TorusSDF = [&](const FVector& P) -> float
	{
		FVector ToP = P - TorusCenter;
		// Project onto axis to get axial distance
		float AxisDist = FVector::DotProduct(ToP, TorusAxis);
		// Get radial vector (perpendicular to axis)
		FVector RadialVec = ToP - (AxisDist * TorusAxis);
		float RadialDist = RadialVec.Size();

		// 2D distance to tube center line
		FVector2D Q(RadialDist - TorusMajorRadius, AxisDist);
		return Q.Size() - TorusMinorRadius;
	};

	// Check if point is in influence region (near torus surface)
	auto IsInInfluenceRegion = [&](const FVector& P) -> bool
	{
		float SDF = TorusSDF(P);
		return SDF <= InfluenceMargin;
	};

	// Perform multiple levels of subdivision
	for (int32 Level = 0; Level < MaxLevel; Level++)
	{
		TArray<int32> NewTriangles;
		TArray<int32> NewMaterialIndices;
		NewTriangles.Reserve(Triangles.Num() * 4);
		NewMaterialIndices.Reserve(MaterialIndices.Num() * 4);

		int32 NumTris = Triangles.Num() / 3;
		TArray<bool> TriNeedsRedSplit;
		TriNeedsRedSplit.SetNumZeroed(NumTris);

		// ========================================================================
		// 최적화: 버텍스별 영역 판정 캐싱 (Torus)
		// 같은 버텍스가 여러 삼각형에 공유되어도 1번만 검사
		// ========================================================================
		const int32 NumPositions = Positions.Num();
		TArray<int8> VertexInRegionCache;  // -1: 미검사, 0: 밖, 1: 안
		VertexInRegionCache.SetNumUninitialized(NumPositions);
		FMemory::Memset(VertexInRegionCache.GetData(), -1, NumPositions);

		// 캐싱된 버텍스 영역 검사 람다
		auto IsVertexInRegionCached = [&](int32 VertexIndex) -> bool
		{
			if (VertexInRegionCache[VertexIndex] == -1)
			{
				VertexInRegionCache[VertexIndex] = IsInInfluenceRegion(Positions[VertexIndex]) ? 1 : 0;
			}
			return VertexInRegionCache[VertexIndex] == 1;
		};

		// Phase 1: Mark triangles that need RED split (4-way, in region)
		for (int32 i = 0; i < NumTris; i++)
		{
			int32 V0 = Triangles[i * 3];
			int32 V1 = Triangles[i * 3 + 1];
			int32 V2 = Triangles[i * 3 + 2];

			// 최적화: 캐싱된 버텍스 검사 사용
			bool bInRegion = false;
			if (IsVertexInRegionCached(V0)) bInRegion = true;
			else if (IsVertexInRegionCached(V1)) bInRegion = true;
			else if (IsVertexInRegionCached(V2)) bInRegion = true;
			else
			{
				// Edge midpoints 검사 (캐싱 불가 - 매번 새로운 위치)
				const FVector& P0 = Positions[V0];
				const FVector& P1 = Positions[V1];
				const FVector& P2 = Positions[V2];

				FVector Mid01 = (P0 + P1) * 0.5f;
				FVector Mid12 = (P1 + P2) * 0.5f;
				FVector Mid20 = (P2 + P0) * 0.5f;

				if (IsInInfluenceRegion(Mid01)) bInRegion = true;
				else if (IsInInfluenceRegion(Mid12)) bInRegion = true;
				else if (IsInInfluenceRegion(Mid20)) bInRegion = true;
				else
				{
					// 중심점 검사
					FVector Center = (P0 + P1 + P2) / 3.0f;
					if (IsInInfluenceRegion(Center)) bInRegion = true;
				}
			}

			if (!bInRegion) continue;

			// Check minimum edge length
			const FVector& P0 = Positions[V0];
			const FVector& P1 = Positions[V1];
			const FVector& P2 = Positions[V2];

			float MaxEdgeLen = FMath::Max3(
				FVector::Dist(P0, P1),
				FVector::Dist(P1, P2),
				FVector::Dist(P2, P0)
			);

			if (MaxEdgeLen >= MinEdgeLength)
			{
				TriNeedsRedSplit[i] = true;
			}
		}

		// Phase 2: Do RED splits (creates midpoints)
		for (int32 i = 0; i < NumTris; i++)
		{
			int32 V0 = Triangles[i * 3];
			int32 V1 = Triangles[i * 3 + 1];
			int32 V2 = Triangles[i * 3 + 2];
			int32 MatIdx = MaterialIndices.IsValidIndex(i) ? MaterialIndices[i] : 0;

			if (TriNeedsRedSplit[i])
			{
				// RED: Split into 4 triangles (all inherit parent's material)
				int32 M01 = GetOrCreateMidpoint(V0, V1);
				int32 M12 = GetOrCreateMidpoint(V1, V2);
				int32 M20 = GetOrCreateMidpoint(V2, V0);

				// Triangle 1: V0, M01, M20
				NewTriangles.Add(V0); NewTriangles.Add(M01); NewTriangles.Add(M20);
				NewMaterialIndices.Add(MatIdx);

				// Triangle 2: M01, V1, M12
				NewTriangles.Add(M01); NewTriangles.Add(V1); NewTriangles.Add(M12);
				NewMaterialIndices.Add(MatIdx);

				// Triangle 3: M20, M12, V2
				NewTriangles.Add(M20); NewTriangles.Add(M12); NewTriangles.Add(V2);
				NewMaterialIndices.Add(MatIdx);

				// Triangle 4: M01, M12, M20 (center)
				NewTriangles.Add(M01); NewTriangles.Add(M12); NewTriangles.Add(M20);
				NewMaterialIndices.Add(MatIdx);
			}
			else
			{
				// Keep original triangle for now (may be GREEN split later)
				NewTriangles.Add(V0); NewTriangles.Add(V1); NewTriangles.Add(V2);
				NewMaterialIndices.Add(MatIdx);
			}
		}

		// Phase 3: GREEN splits - fix T-junctions (Dual Map System)
		TArray<int32> FinalTriangles;
		TArray<int32> FinalMaterialIndices;
		FinalTriangles.Reserve(NewTriangles.Num() * 2);
		FinalMaterialIndices.Reserve(NewMaterialIndices.Num() * 2);

		NumTris = NewTriangles.Num() / 3;
		for (int32 i = 0; i < NumTris; i++)
		{
			int32 V0 = NewTriangles[i * 3];
			int32 V1 = NewTriangles[i * 3 + 1];
			int32 V2 = NewTriangles[i * 3 + 2];
			int32 MatIdx = NewMaterialIndices.IsValidIndex(i) ? NewMaterialIndices[i] : 0;

			// 위치 기반으로 중점 존재 여부 감지
			bool bHas01 = HasMidpointAtEdge(V0, V1);
			bool bHas12 = HasMidpointAtEdge(V1, V2);
			bool bHas20 = HasMidpointAtEdge(V2, V0);

			int32 NumMidpoints = (bHas01 ? 1 : 0) + (bHas12 ? 1 : 0) + (bHas20 ? 1 : 0);

			if (NumMidpoints == 0)
			{
				FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
				FinalMaterialIndices.Add(MatIdx);
			}
			else if (NumMidpoints == 3)
			{
				// GREEN-3: 이웃들이 전부 RED split됨 → 강제 4분할
				int32 M01 = GetOrCreateMidpoint(V0, V1);
				int32 M12 = GetOrCreateMidpoint(V1, V2);
				int32 M20 = GetOrCreateMidpoint(V2, V0);

				FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(M20);
				FinalMaterialIndices.Add(MatIdx);
				FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(M12);
				FinalMaterialIndices.Add(MatIdx);
				FinalTriangles.Add(M20); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
				FinalMaterialIndices.Add(MatIdx);
				FinalTriangles.Add(M01); FinalTriangles.Add(M12); FinalTriangles.Add(M20);
				FinalMaterialIndices.Add(MatIdx);
			}
			else if (NumMidpoints == 1)
			{
				if (bHas01)
				{
					int32 M01 = GetOrCreateMidpoint(V0, V1);
					FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
					FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
				}
				else if (bHas12)
				{
					int32 M12 = GetOrCreateMidpoint(V1, V2);
					FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(M12);
					FinalMaterialIndices.Add(MatIdx);
					FinalTriangles.Add(V0); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
				}
				else // bHas20
				{
					int32 M20 = GetOrCreateMidpoint(V2, V0);
					FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(M20);
					FinalMaterialIndices.Add(MatIdx);
					FinalTriangles.Add(M20); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
				}
			}
			else // NumMidpoints == 2
			{
				if (bHas01 && bHas12)
				{
					int32 M01 = GetOrCreateMidpoint(V0, V1);
					int32 M12 = GetOrCreateMidpoint(V1, V2);

					FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(M12);
					FinalMaterialIndices.Add(MatIdx);

					float DiagA = FVector::DistSquared(Positions[M01], Positions[V2]);
					float DiagB = FVector::DistSquared(Positions[V0], Positions[M12]);
					if (DiagA <= DiagB)
					{
						FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(M01); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
					}
					else
					{
						FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(M12);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(V0); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
					}
				}
				else if (bHas12 && bHas20)
				{
					int32 M12 = GetOrCreateMidpoint(V1, V2);
					int32 M20 = GetOrCreateMidpoint(V2, V0);

					FinalTriangles.Add(M20); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);

					float DiagA = FVector::DistSquared(Positions[V0], Positions[M12]);
					float DiagB = FVector::DistSquared(Positions[V1], Positions[M20]);
					if (DiagA <= DiagB)
					{
						FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(M12);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(V0); FinalTriangles.Add(M12); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
					}
					else
					{
						FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(V1); FinalTriangles.Add(M12); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
					}
				}
				else // bHas01 && bHas20
				{
					int32 M01 = GetOrCreateMidpoint(V0, V1);
					int32 M20 = GetOrCreateMidpoint(V2, V0);

					FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(M20);
					FinalMaterialIndices.Add(MatIdx);

					float DiagA = FVector::DistSquared(Positions[V1], Positions[M20]);
					float DiagB = FVector::DistSquared(Positions[M01], Positions[V2]);
					if (DiagA <= DiagB)
					{
						FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(M20); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
					}
					else
					{
						FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(M01); FinalTriangles.Add(V2); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
					}
				}
			}
		}

		Triangles = MoveTemp(FinalTriangles);
		MaterialIndices = MoveTemp(FinalMaterialIndices);

		int32 CurrentTriCount = Triangles.Num() / 3;

		// If no change, stop
		if (CurrentTriCount == NumTris)
		{
			break;
		}
	}

	// Rebuild half-edge mesh from result with parent info
	Mesh.Clear();
	Mesh.BuildFromTriangles(Positions, Triangles, UVs, MaterialIndices, &ParentIndices);

	int32 FinalTriCount = Triangles.Num() / 3;
	return FinalTriCount - InitialTriCount;
}

int32 FLEBSubdivision::SubdivideRegion(
	FHalfEdgeMesh& Mesh,
	const FSubdivisionOBB& OBB,
	int32 MaxLevel,
	float MinEdgeLength)
{
	// ======================================================================
	// OBB Debug - DrawSdfVolume과 동일한 파라미터 출력
	// ======================================================================

	// 메시 버텍스 범위 계산
	FVector VertexMin(FLT_MAX), VertexMax(-FLT_MAX);
	for (const FHalfEdgeVertex& V : Mesh.Vertices)
	{
		VertexMin = VertexMin.ComponentMin(V.Position);
		VertexMax = VertexMax.ComponentMax(V.Position);
	}

	UE_LOG(LogTemp, Log, TEXT(""));
	UE_LOG(LogTemp, Log, TEXT("======== SubdivideRegion OBB Debug ========"));
	UE_LOG(LogTemp, Log, TEXT("  [OBB Parameters (DrawSdfVolume과 동일)]"));
	UE_LOG(LogTemp, Log, TEXT("    Center: %s"), *OBB.Center.ToString());
	UE_LOG(LogTemp, Log, TEXT("    HalfExtents: %s"), *OBB.HalfExtents.ToString());
	UE_LOG(LogTemp, Log, TEXT("    AxisX: %s"), *OBB.AxisX.ToString());
	UE_LOG(LogTemp, Log, TEXT("    AxisY: %s"), *OBB.AxisY.ToString());
	UE_LOG(LogTemp, Log, TEXT("    AxisZ: %s"), *OBB.AxisZ.ToString());
	UE_LOG(LogTemp, Log, TEXT("    InfluenceMargin: %.2f"), OBB.InfluenceMargin);
	UE_LOG(LogTemp, Log, TEXT("  [Local Bounds (디버그용)]"));
	UE_LOG(LogTemp, Log, TEXT("    LocalBoundsMin: %s"), *OBB.LocalBoundsMin.ToString());
	UE_LOG(LogTemp, Log, TEXT("    LocalBoundsMax: %s"), *OBB.LocalBoundsMax.ToString());
	UE_LOG(LogTemp, Log, TEXT("    LocalSize: %s"), *(OBB.LocalBoundsMax - OBB.LocalBoundsMin).ToString());
	UE_LOG(LogTemp, Log, TEXT("  [Mesh Vertices (Component Space)]"));
	UE_LOG(LogTemp, Log, TEXT("    VertexMin: %s"), *VertexMin.ToString());
	UE_LOG(LogTemp, Log, TEXT("    VertexMax: %s"), *VertexMax.ToString());

	// 샘플 버텍스 테스트 - 실제 투영 값 출력
	UE_LOG(LogTemp, Log, TEXT("  [Sample Vertex Projection Test]"));
	if (Mesh.Vertices.Num() > 0)
	{
		// 중앙 근처 버텍스, 최소 버텍스, 최대 버텍스 테스트
		TArray<int32> SampleIndices = { 0, Mesh.Vertices.Num() / 2, Mesh.Vertices.Num() - 1 };
		for (int32 Idx : SampleIndices)
		{
			if (Idx < Mesh.Vertices.Num())
			{
				const FVector& P = Mesh.Vertices[Idx].Position;
				FVector D = P - OBB.Center;
				float ProjX = FMath::Abs(FVector::DotProduct(D, OBB.AxisX));
				float ProjY = FMath::Abs(FVector::DotProduct(D, OBB.AxisY));
				float ProjZ = FMath::Abs(FVector::DotProduct(D, OBB.AxisZ));
				float LimitX = OBB.HalfExtents.X + OBB.InfluenceMargin;
				float LimitY = OBB.HalfExtents.Y + OBB.InfluenceMargin;
				float LimitZ = OBB.HalfExtents.Z + OBB.InfluenceMargin;
				bool bInX = ProjX <= LimitX;
				bool bInY = ProjY <= LimitY;
				bool bInZ = ProjZ <= LimitZ;
				UE_LOG(LogTemp, Log, TEXT("    V[%d] Pos: %s"), Idx, *P.ToString());
				UE_LOG(LogTemp, Log, TEXT("      ProjX: %.2f vs Limit %.2f -> %s"), ProjX, LimitX, bInX ? TEXT("PASS") : TEXT("FAIL"));
				UE_LOG(LogTemp, Log, TEXT("      ProjY: %.2f vs Limit %.2f -> %s"), ProjY, LimitY, bInY ? TEXT("PASS") : TEXT("FAIL"));
				UE_LOG(LogTemp, Log, TEXT("      ProjZ: %.2f vs Limit %.2f -> %s"), ProjZ, LimitZ, bInZ ? TEXT("PASS") : TEXT("FAIL"));
			}
		}
	}
	UE_LOG(LogTemp, Log, TEXT("============================================"));
	UE_LOG(LogTemp, Log, TEXT(""));

	// Step 1: Export current mesh to simple triangle format
	TArray<FVector> Positions;
	TArray<FVector2D> UVs;
	TArray<int32> Triangles;
	TArray<int32> MaterialIndices;  // Per-triangle material index
	TArray<TPair<int32, int32>> ParentIndices;  // Per-vertex parent info (Subdivision 생성 시점에 기록)

	Positions.Reserve(Mesh.Vertices.Num());
	UVs.Reserve(Mesh.Vertices.Num());
	ParentIndices.Reserve(Mesh.Vertices.Num());
	for (const FHalfEdgeVertex& V : Mesh.Vertices)
	{
		Positions.Add(V.Position);
		UVs.Add(V.UV);
		// 부모 정보 유지 (멀티레벨 Subdivision용)
		// 원본 버텍스: INDEX_NONE, 이전 레벨 생성 버텍스: 부모 인덱스 보존
		ParentIndices.Add(TPair<int32, int32>(V.ParentIndex0, V.ParentIndex1));
	}

	for (int32 FaceIdx = 0; FaceIdx < Mesh.Faces.Num(); FaceIdx++)
	{
		int32 V0, V1, V2;
		Mesh.GetFaceVertices(FaceIdx, V0, V1, V2);
		if (V0 >= 0 && V1 >= 0 && V2 >= 0)
		{
			Triangles.Add(V0);
			Triangles.Add(V1);
			Triangles.Add(V2);
			MaterialIndices.Add(Mesh.Faces[FaceIdx].MaterialIndex);
		}
	}

	int32 InitialTriCount = Triangles.Num() / 3;

	// ============================================================================
	// Dual Midpoint Map System (Position + Index)
	// ============================================================================
	// 문제 1: 머티리얼 섹션/UV seam 경계에서 같은 위치의 버텍스가 다른 인덱스
	//         -> GREEN split이 중점 존재를 감지 못함
	// 문제 2: 위치만으로 버텍스 공유하면 UV가 섞임
	//         -> UV seam에서 렌더링 아티팩트 발생
	//
	// 해결: 두 개의 맵 사용
	//   1. PositionMidpointMap: GREEN split 감지용 (위치 기반) - "이 edge에 중점이 있나?"
	//   2. IndexMidpointMap: 버텍스 재사용용 (인덱스 기반) - "같은 UV면 재사용"
	// ============================================================================

	constexpr float MidpointWeldPrecision = 0.1f;

	// Position -> 양자화된 정수 좌표 변환
	auto PositionToKey = [MidpointWeldPrecision](const FVector& Pos) -> FIntVector
	{
		return FIntVector(
			FMath::RoundToInt(Pos.X / MidpointWeldPrecision),
			FMath::RoundToInt(Pos.Y / MidpointWeldPrecision),
			FMath::RoundToInt(Pos.Z / MidpointWeldPrecision)
		);
	};

	auto IntVectorLess = [](const FIntVector& A, const FIntVector& B) -> bool
	{
		if (A.X != B.X) return A.X < B.X;
		if (A.Y != B.Y) return A.Y < B.Y;
		return A.Z < B.Z;
	};

	// 1. Position-based map: GREEN split 감지용 (이 위치의 edge에 중점이 있는지)
	TSet<TPair<FIntVector, FIntVector>> PositionMidpointSet;

	auto MakePositionKey = [&](int32 VA, int32 VB) -> TPair<FIntVector, FIntVector>
	{
		FIntVector KeyA = PositionToKey(Positions[VA]);
		FIntVector KeyB = PositionToKey(Positions[VB]);
		if (IntVectorLess(KeyA, KeyB))
			return TPair<FIntVector, FIntVector>(KeyA, KeyB);
		else
			return TPair<FIntVector, FIntVector>(KeyB, KeyA);
	};

	// 2. Index-based map: 버텍스 재사용용 (같은 인덱스 = 같은 UV)
	TMap<TPair<int32, int32>, int32> IndexMidpointMap;

	auto MakeIndexKey = [](int32 A, int32 B) -> TPair<int32, int32>
	{
		return A < B ? TPair<int32, int32>(A, B) : TPair<int32, int32>(B, A);
	};

	// 디버그 통계
	int32 DebugMidpointCreated = 0, DebugMidpointReused = 0;

	// GetOrCreateMidpoint: 인덱스 기반으로 재사용, 위치 기반으로 존재 등록
	auto GetOrCreateMidpoint = [&](int32 VA, int32 VB) -> int32
	{
		TPair<int32, int32> IndexKey = MakeIndexKey(VA, VB);

		// 같은 인덱스(= 같은 UV)면 재사용
		if (int32* Existing = IndexMidpointMap.Find(IndexKey))
		{
			DebugMidpointReused++;
			return *Existing;
		}

		// 새 버텍스 생성 (자기 UV로)
		DebugMidpointCreated++;
		int32 NewIdx = Positions.Num();
		Positions.Add((Positions[VA] + Positions[VB]) * 0.5f);
		UVs.Add((UVs[VA] + UVs[VB]) * 0.5f);  // 자기 삼각형의 UV로 보간
		ParentIndices.Add(TPair<int32, int32>(VA, VB));

		// 두 맵 모두에 등록
		IndexMidpointMap.Add(IndexKey, NewIdx);
		PositionMidpointSet.Add(MakePositionKey(VA, VB));  // GREEN split 감지용

		return NewIdx;
	};

	// GREEN split용: 이 edge에 중점이 있는지 확인 (위치 기반)
	auto HasMidpointAtEdge = [&](int32 VA, int32 VB) -> bool
	{
		return PositionMidpointSet.Contains(MakePositionKey(VA, VB));
	};

	// 디버그용 통계
	int32 DebugInCount = 0, DebugOutCount = 0;
	int32 DebugFailX = 0, DebugFailY = 0, DebugFailZ = 0;

	// OBB influence check - DrawSdfVolume과 동일한 방식
	auto IsInInfluenceRegion = [&](const FVector& P) -> bool
	{
		// 점에서 OBB 중심까지의 벡터
		FVector D = P - OBB.Center;

		// 각 OBB 축에 투영
		float ProjX = FMath::Abs(FVector::DotProduct(D, OBB.AxisX));
		float ProjY = FMath::Abs(FVector::DotProduct(D, OBB.AxisY));
		float ProjZ = FMath::Abs(FVector::DotProduct(D, OBB.AxisZ));

		// 마진 포함 범위 체크
		bool bInX = ProjX <= OBB.HalfExtents.X + OBB.InfluenceMargin;
		bool bInY = ProjY <= OBB.HalfExtents.Y + OBB.InfluenceMargin;
		bool bInZ = ProjZ <= OBB.HalfExtents.Z + OBB.InfluenceMargin;

		bool bInRegion = bInX && bInY && bInZ;

		if (bInRegion) DebugInCount++;
		else
		{
			DebugOutCount++;
			if (!bInX) DebugFailX++;
			if (!bInY) DebugFailY++;
			if (!bInZ) DebugFailZ++;
		}
		return bInRegion;
	};

	// Perform multiple levels of subdivision
	for (int32 Level = 0; Level < MaxLevel; Level++)
	{
		TArray<int32> NewTriangles;
		TArray<int32> NewMaterialIndices;
		NewTriangles.Reserve(Triangles.Num() * 4);
		NewMaterialIndices.Reserve(MaterialIndices.Num() * 4);

		int32 NumTris = Triangles.Num() / 3;
		TArray<bool> TriNeedsRedSplit;
		TriNeedsRedSplit.SetNumZeroed(NumTris);

		// ========================================================================
		// 최적화: 버텍스별 영역 판정 캐싱
		// 같은 버텍스가 여러 삼각형에 공유되어도 1번만 검사
		// ========================================================================
		const int32 NumPositions = Positions.Num();
		TArray<int8> VertexInRegionCache;  // -1: 미검사, 0: 밖, 1: 안
		VertexInRegionCache.SetNumUninitialized(NumPositions);
		FMemory::Memset(VertexInRegionCache.GetData(), -1, NumPositions);

		// 캐싱된 버텍스 영역 검사 람다
		auto IsVertexInRegionCached = [&](int32 VertexIndex) -> bool
		{
			if (VertexInRegionCache[VertexIndex] == -1)
			{
				// 처음 검사 - 결과 캐싱
				VertexInRegionCache[VertexIndex] = IsInInfluenceRegion(Positions[VertexIndex]) ? 1 : 0;
			}
			return VertexInRegionCache[VertexIndex] == 1;
		};

		// Phase 1: Mark triangles that need RED split (4-way, in region)
		for (int32 i = 0; i < NumTris; i++)
		{
			int32 V0 = Triangles[i * 3];
			int32 V1 = Triangles[i * 3 + 1];
			int32 V2 = Triangles[i * 3 + 2];

			// 최적화: 캐싱된 버텍스 검사 사용
			bool bInRegion = false;
			if (IsVertexInRegionCached(V0)) bInRegion = true;
			else if (IsVertexInRegionCached(V1)) bInRegion = true;
			else if (IsVertexInRegionCached(V2)) bInRegion = true;
			else
			{
				// Edge midpoints 검사 (캐싱 불가 - 매번 새로운 위치)
				const FVector& P0 = Positions[V0];
				const FVector& P1 = Positions[V1];
				const FVector& P2 = Positions[V2];

				FVector Mid01 = (P0 + P1) * 0.5f;
				FVector Mid12 = (P1 + P2) * 0.5f;
				FVector Mid20 = (P2 + P0) * 0.5f;

				if (IsInInfluenceRegion(Mid01)) bInRegion = true;
				else if (IsInInfluenceRegion(Mid12)) bInRegion = true;
				else if (IsInInfluenceRegion(Mid20)) bInRegion = true;
				else
				{
					// 중심점 검사
					FVector Center = (P0 + P1 + P2) / 3.0f;
					if (IsInInfluenceRegion(Center)) bInRegion = true;
				}
			}

			if (!bInRegion) continue;

			// Check minimum edge length
			const FVector& P0 = Positions[V0];
			const FVector& P1 = Positions[V1];
			const FVector& P2 = Positions[V2];

			float MaxEdgeLen = FMath::Max3(
				FVector::Dist(P0, P1),
				FVector::Dist(P1, P2),
				FVector::Dist(P2, P0)
			);

			if (MaxEdgeLen >= MinEdgeLength)
			{
				TriNeedsRedSplit[i] = true;
			}
		}

		// Phase 2: Do RED splits (creates midpoints)
		for (int32 i = 0; i < NumTris; i++)
		{
			int32 V0 = Triangles[i * 3];
			int32 V1 = Triangles[i * 3 + 1];
			int32 V2 = Triangles[i * 3 + 2];
			int32 MatIdx = MaterialIndices.IsValidIndex(i) ? MaterialIndices[i] : 0;

			if (TriNeedsRedSplit[i])
			{
				// RED: Split into 4 triangles (all inherit parent's material)
				int32 M01 = GetOrCreateMidpoint(V0, V1);
				int32 M12 = GetOrCreateMidpoint(V1, V2);
				int32 M20 = GetOrCreateMidpoint(V2, V0);

				NewTriangles.Add(V0); NewTriangles.Add(M01); NewTriangles.Add(M20);
				NewMaterialIndices.Add(MatIdx);

				NewTriangles.Add(M01); NewTriangles.Add(V1); NewTriangles.Add(M12);
				NewMaterialIndices.Add(MatIdx);

				NewTriangles.Add(M20); NewTriangles.Add(M12); NewTriangles.Add(V2);
				NewMaterialIndices.Add(MatIdx);

				NewTriangles.Add(M01); NewTriangles.Add(M12); NewTriangles.Add(M20);
				NewMaterialIndices.Add(MatIdx);
			}
			else
			{
				NewTriangles.Add(V0); NewTriangles.Add(V1); NewTriangles.Add(V2);
				NewMaterialIndices.Add(MatIdx);
			}
		}

		// Phase 3: GREEN splits - fix T-junctions
		// ============================================================================
		// 핵심 변경: 감지는 위치 기반(HasMidpointAtEdge), 버텍스는 인덱스 기반(GetOrCreateMidpoint)
		// -> UV seam에서 각 삼각형이 자기 UV로 중점 생성, 하지만 GREEN split은 정확히 감지
		// ============================================================================
		TArray<int32> FinalTriangles;
		TArray<int32> FinalMaterialIndices;
		FinalTriangles.Reserve(NewTriangles.Num() * 2);
		FinalMaterialIndices.Reserve(NewMaterialIndices.Num() * 2);

		int32 DebugGreenSplit1 = 0, DebugGreenSplit2 = 0;

		NumTris = NewTriangles.Num() / 3;
		for (int32 i = 0; i < NumTris; i++)
		{
			int32 V0 = NewTriangles[i * 3];
			int32 V1 = NewTriangles[i * 3 + 1];
			int32 V2 = NewTriangles[i * 3 + 2];
			int32 MatIdx = NewMaterialIndices.IsValidIndex(i) ? NewMaterialIndices[i] : 0;

			// 위치 기반으로 중점 존재 여부 감지 (UV seam 경계도 감지)
			bool bHas01 = HasMidpointAtEdge(V0, V1);
			bool bHas12 = HasMidpointAtEdge(V1, V2);
			bool bHas20 = HasMidpointAtEdge(V2, V0);

			int32 NumMidpoints = (bHas01 ? 1 : 0) + (bHas12 ? 1 : 0) + (bHas20 ? 1 : 0);

			if (NumMidpoints == 0)
			{
				FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
				FinalMaterialIndices.Add(MatIdx);
			}
			else if (NumMidpoints == 3)
			{
				// GREEN-3: 이웃들이 전부 RED split됨 → 강제 4분할
				int32 M01 = GetOrCreateMidpoint(V0, V1);
				int32 M12 = GetOrCreateMidpoint(V1, V2);
				int32 M20 = GetOrCreateMidpoint(V2, V0);

				FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(M20);
				FinalMaterialIndices.Add(MatIdx);
				FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(M12);
				FinalMaterialIndices.Add(MatIdx);
				FinalTriangles.Add(M20); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
				FinalMaterialIndices.Add(MatIdx);
				FinalTriangles.Add(M01); FinalTriangles.Add(M12); FinalTriangles.Add(M20);
				FinalMaterialIndices.Add(MatIdx);
			}
			else if (NumMidpoints == 1)
			{
				DebugGreenSplit1++;
				if (bHas01)
				{
					// 자기 UV로 중점 생성/조회
					int32 M01 = GetOrCreateMidpoint(V0, V1);
					FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
					FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
				}
				else if (bHas12)
				{
					int32 M12 = GetOrCreateMidpoint(V1, V2);
					FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(M12);
					FinalMaterialIndices.Add(MatIdx);
					FinalTriangles.Add(V0); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
				}
				else // bHas20
				{
					int32 M20 = GetOrCreateMidpoint(V2, V0);
					FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(M20);
					FinalMaterialIndices.Add(MatIdx);
					FinalTriangles.Add(M20); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
				}
			}
			else // NumMidpoints == 2
			{
				DebugGreenSplit2++;
				// GREEN-2: Corner triangle + quadrilateral split by shorter diagonal
				if (bHas01 && bHas12)
				{
					int32 M01 = GetOrCreateMidpoint(V0, V1);
					int32 M12 = GetOrCreateMidpoint(V1, V2);

					// Corner at V1: (M01, V1, M12)
					FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(M12);
					FinalMaterialIndices.Add(MatIdx);

					// Quad V0-M01-M12-V2: compare diagonals M01-V2 vs V0-M12
					float DiagA = FVector::DistSquared(Positions[M01], Positions[V2]);
					float DiagB = FVector::DistSquared(Positions[V0], Positions[M12]);
					if (DiagA <= DiagB)
					{
						FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(M01); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
					}
					else
					{
						FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(M12);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(V0); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
					}
				}
				else if (bHas12 && bHas20)
				{
					int32 M12 = GetOrCreateMidpoint(V1, V2);
					int32 M20 = GetOrCreateMidpoint(V2, V0);

					// Corner at V2: (M20, M12, V2)
					FinalTriangles.Add(M20); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);

					// Quad V0-V1-M12-M20
					float DiagA = FVector::DistSquared(Positions[V0], Positions[M12]);
					float DiagB = FVector::DistSquared(Positions[V1], Positions[M20]);
					if (DiagA <= DiagB)
					{
						FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(M12);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(V0); FinalTriangles.Add(M12); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
					}
					else
					{
						FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(V1); FinalTriangles.Add(M12); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
					}
				}
				else // bHas01 && bHas20
				{
					int32 M01 = GetOrCreateMidpoint(V0, V1);
					int32 M20 = GetOrCreateMidpoint(V2, V0);

					// Corner at V0: (V0, M01, M20)
					FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(M20);
					FinalMaterialIndices.Add(MatIdx);

					// Quad M01-V1-V2-M20
					float DiagA = FVector::DistSquared(Positions[V1], Positions[M20]);
					float DiagB = FVector::DistSquared(Positions[M01], Positions[V2]);
					if (DiagA <= DiagB)
					{
						FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(M20); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
					}
					else
					{
						FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(M01); FinalTriangles.Add(V2); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
					}
				}
			}
		}

		Triangles = MoveTemp(FinalTriangles);
		MaterialIndices = MoveTemp(FinalMaterialIndices);

		int32 CurrentTriCount = Triangles.Num() / 3;

		UE_LOG(LogTemp, Log, TEXT("  Level %d: GREEN splits - 1-midpoint: %d, 2-midpoint: %d, IndexMidpointMap size: %d"),
			Level, DebugGreenSplit1, DebugGreenSplit2, IndexMidpointMap.Num());

		// If no change, stop
		if (CurrentTriCount == NumTris)
		{
			break;
		}
	}

	// 디버그: 통계 출력
	UE_LOG(LogTemp, Log, TEXT("=== SubdivideRegion Complete ==="));
	UE_LOG(LogTemp, Log, TEXT("  Vertices IN region: %d, OUT of region: %d"), DebugInCount, DebugOutCount);
	UE_LOG(LogTemp, Log, TEXT("  Fail reasons - X: %d, Y: %d, Z: %d"), DebugFailX, DebugFailY, DebugFailZ);
	UE_LOG(LogTemp, Log, TEXT("  Midpoints - Created: %d, Reused: %d (position-based welding)"), DebugMidpointCreated, DebugMidpointReused);

	// Rebuild half-edge mesh from result with parent info
	Mesh.Clear();
	Mesh.BuildFromTriangles(Positions, Triangles, UVs, MaterialIndices, &ParentIndices);

	int32 FinalTriCount = Triangles.Num() / 3;
	return FinalTriCount - InitialTriCount;
}

void FLEBSubdivision::SubdivideFace4(FHalfEdgeMesh& Mesh, int32 FaceIndex)
{
	// Not used in Red-Green implementation
}

int32 FLEBSubdivision::SplitEdge(FHalfEdgeMesh& Mesh, int32 HalfEdgeIndex)
{
	// Not used in Red-Green implementation
	return -1;
}

void FLEBSubdivision::EnsureLongestEdge(FHalfEdgeMesh& Mesh, int32 HalfEdgeIndex, TSet<int32>& ProcessedFaces)
{
	// Not used in Red-Green implementation
}

void FLEBSubdivision::SplitFaceByEdge(FHalfEdgeMesh& Mesh, int32 FaceIndex, int32 MidpointVertex)
{
	// Not used in Red-Green implementation
}

int32 FLEBSubdivision::SubdivideUniform(
	FHalfEdgeMesh& Mesh,
	int32 MaxLevel,
	float MinEdgeLength)
{
	UE_LOG(LogTemp, Log, TEXT(""));
	UE_LOG(LogTemp, Log, TEXT("======== SubdivideUniform (Preview Mesh) ========"));
	UE_LOG(LogTemp, Log, TEXT("  MaxLevel: %d, MinEdgeLength: %.2f"), MaxLevel, MinEdgeLength);

	// Step 1: Export current mesh to simple triangle format
	TArray<FVector> Positions;
	TArray<FVector2D> UVs;
	TArray<int32> Triangles;
	TArray<int32> MaterialIndices;
	TArray<TPair<int32, int32>> ParentIndices;

	Positions.Reserve(Mesh.Vertices.Num());
	UVs.Reserve(Mesh.Vertices.Num());
	ParentIndices.Reserve(Mesh.Vertices.Num());
	for (const FHalfEdgeVertex& V : Mesh.Vertices)
	{
		Positions.Add(V.Position);
		UVs.Add(V.UV);
		ParentIndices.Add(TPair<int32, int32>(V.ParentIndex0, V.ParentIndex1));
	}

	for (int32 FaceIdx = 0; FaceIdx < Mesh.Faces.Num(); FaceIdx++)
	{
		int32 V0, V1, V2;
		Mesh.GetFaceVertices(FaceIdx, V0, V1, V2);
		if (V0 >= 0 && V1 >= 0 && V2 >= 0)
		{
			Triangles.Add(V0);
			Triangles.Add(V1);
			Triangles.Add(V2);
			MaterialIndices.Add(Mesh.Faces[FaceIdx].MaterialIndex);
		}
	}

	int32 InitialTriCount = Triangles.Num() / 3;
	int32 InitialVertCount = Positions.Num();

	UE_LOG(LogTemp, Log, TEXT("  Initial: %d vertices, %d triangles"), InitialVertCount, InitialTriCount);

	// ============================================================================
	// Dual Midpoint Map System (Position + Index) - GREEN split 지원
	// ============================================================================
	// 1. PositionMidpointSet: GREEN split 감지용 (위치 기반) - "이 edge에 중점이 있나?"
	// 2. IndexMidpointMap: 버텍스 재사용용 (인덱스 기반) - "같은 UV면 재사용"
	// ============================================================================
	constexpr float MidpointWeldPrecision = 0.1f;

	auto PositionToKey = [MidpointWeldPrecision](const FVector& Pos) -> FIntVector
	{
		return FIntVector(
			FMath::RoundToInt(Pos.X / MidpointWeldPrecision),
			FMath::RoundToInt(Pos.Y / MidpointWeldPrecision),
			FMath::RoundToInt(Pos.Z / MidpointWeldPrecision)
		);
	};

	auto IntVectorLess = [](const FIntVector& A, const FIntVector& B) -> bool
	{
		if (A.X != B.X) return A.X < B.X;
		if (A.Y != B.Y) return A.Y < B.Y;
		return A.Z < B.Z;
	};

	// 1. Position-based set: GREEN split 감지용
	TSet<TPair<FIntVector, FIntVector>> PositionMidpointSet;

	auto MakePositionKey = [&](int32 VA, int32 VB) -> TPair<FIntVector, FIntVector>
	{
		FIntVector KeyA = PositionToKey(Positions[VA]);
		FIntVector KeyB = PositionToKey(Positions[VB]);
		if (IntVectorLess(KeyA, KeyB))
			return TPair<FIntVector, FIntVector>(KeyA, KeyB);
		else
			return TPair<FIntVector, FIntVector>(KeyB, KeyA);
	};

	// 2. Index-based map: 버텍스 재사용용 (같은 인덱스 = 같은 UV)
	TMap<TPair<int32, int32>, int32> IndexMidpointMap;

	auto MakeIndexKey = [](int32 A, int32 B) -> TPair<int32, int32>
	{
		return A < B ? TPair<int32, int32>(A, B) : TPair<int32, int32>(B, A);
	};

	// GetOrCreateMidpoint: 인덱스 기반으로 재사용, 위치 기반으로 존재 등록
	auto GetOrCreateMidpoint = [&](int32 VA, int32 VB) -> int32
	{
		TPair<int32, int32> IndexKey = MakeIndexKey(VA, VB);
		if (int32* Existing = IndexMidpointMap.Find(IndexKey))
		{
			return *Existing;
		}

		int32 NewIdx = Positions.Num();
		Positions.Add((Positions[VA] + Positions[VB]) * 0.5f);
		UVs.Add((UVs[VA] + UVs[VB]) * 0.5f);
		ParentIndices.Add(TPair<int32, int32>(VA, VB));

		IndexMidpointMap.Add(IndexKey, NewIdx);
		PositionMidpointSet.Add(MakePositionKey(VA, VB));  // GREEN split 감지용
		return NewIdx;
	};

	// GREEN split용: 이 edge에 중점이 있는지 확인 (위치 기반)
	auto HasMidpointAtEdge = [&](int32 VA, int32 VB) -> bool
	{
		return PositionMidpointSet.Contains(MakePositionKey(VA, VB));
	};

	// Perform multiple levels of subdivision
	for (int32 Level = 0; Level < MaxLevel; Level++)
	{
		TArray<int32> NewTriangles;
		TArray<int32> NewMaterialIndices;
		NewTriangles.Reserve(Triangles.Num() * 4);
		NewMaterialIndices.Reserve(MaterialIndices.Num() * 4);

		int32 NumTris = Triangles.Num() / 3;
		int32 SplitCount = 0;

		// Phase 1: RED splits - MinEdgeLength 조건으로 4분할
		for (int32 i = 0; i < NumTris; i++)
		{
			int32 V0 = Triangles[i * 3];
			int32 V1 = Triangles[i * 3 + 1];
			int32 V2 = Triangles[i * 3 + 2];
			int32 MatIdx = MaterialIndices.IsValidIndex(i) ? MaterialIndices[i] : 0;

			const FVector& P0 = Positions[V0];
			const FVector& P1 = Positions[V1];
			const FVector& P2 = Positions[V2];

			// MinEdgeLength 조건 확인
			float MaxEdgeLen = FMath::Max3(
				FVector::Dist(P0, P1),
				FVector::Dist(P1, P2),
				FVector::Dist(P2, P0)
			);

			if (MaxEdgeLen >= MinEdgeLength)
			{
				// RED: Split into 4 triangles
				int32 M01 = GetOrCreateMidpoint(V0, V1);
				int32 M12 = GetOrCreateMidpoint(V1, V2);
				int32 M20 = GetOrCreateMidpoint(V2, V0);

				NewTriangles.Add(V0); NewTriangles.Add(M01); NewTriangles.Add(M20);
				NewMaterialIndices.Add(MatIdx);

				NewTriangles.Add(M01); NewTriangles.Add(V1); NewTriangles.Add(M12);
				NewMaterialIndices.Add(MatIdx);

				NewTriangles.Add(M20); NewTriangles.Add(M12); NewTriangles.Add(V2);
				NewMaterialIndices.Add(MatIdx);

				NewTriangles.Add(M01); NewTriangles.Add(M12); NewTriangles.Add(M20);
				NewMaterialIndices.Add(MatIdx);

				SplitCount++;
			}
			else
			{
				// Keep original triangle (may be GREEN split later)
				NewTriangles.Add(V0); NewTriangles.Add(V1); NewTriangles.Add(V2);
				NewMaterialIndices.Add(MatIdx);
			}
		}

		// Phase 2: GREEN splits - T-junction 수정
		TArray<int32> FinalTriangles;
		TArray<int32> FinalMaterialIndices;
		FinalTriangles.Reserve(NewTriangles.Num() * 2);
		FinalMaterialIndices.Reserve(NewMaterialIndices.Num() * 2);

		int32 GreenSplit1 = 0, GreenSplit2 = 0, GreenSplit3 = 0;

		NumTris = NewTriangles.Num() / 3;
		for (int32 i = 0; i < NumTris; i++)
		{
			int32 V0 = NewTriangles[i * 3];
			int32 V1 = NewTriangles[i * 3 + 1];
			int32 V2 = NewTriangles[i * 3 + 2];
			int32 MatIdx = NewMaterialIndices.IsValidIndex(i) ? NewMaterialIndices[i] : 0;

			// 위치 기반으로 중점 존재 여부 감지
			bool bHas01 = HasMidpointAtEdge(V0, V1);
			bool bHas12 = HasMidpointAtEdge(V1, V2);
			bool bHas20 = HasMidpointAtEdge(V2, V0);

			int32 NumMidpoints = (bHas01 ? 1 : 0) + (bHas12 ? 1 : 0) + (bHas20 ? 1 : 0);

			if (NumMidpoints == 0)
			{
				// 분할 불필요
				FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
				FinalMaterialIndices.Add(MatIdx);
			}
			else if (NumMidpoints == 3)
			{
				// GREEN-3: 이웃들이 전부 RED split됨 → 강제 4분할
				// (자기는 RED split 안 됐지만 T-junction 3개 해결 필요)
				GreenSplit3++;
				int32 M01 = GetOrCreateMidpoint(V0, V1);
				int32 M12 = GetOrCreateMidpoint(V1, V2);
				int32 M20 = GetOrCreateMidpoint(V2, V0);

				FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(M20);
				FinalMaterialIndices.Add(MatIdx);

				FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(M12);
				FinalMaterialIndices.Add(MatIdx);

				FinalTriangles.Add(M20); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
				FinalMaterialIndices.Add(MatIdx);

				FinalTriangles.Add(M01); FinalTriangles.Add(M12); FinalTriangles.Add(M20);
				FinalMaterialIndices.Add(MatIdx);
			}
			else if (NumMidpoints == 1)
			{
				// GREEN split: 1개 중점 → 2개 삼각형
				GreenSplit1++;
				if (bHas01)
				{
					int32 M01 = GetOrCreateMidpoint(V0, V1);
					FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
					FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
				}
				else if (bHas12)
				{
					int32 M12 = GetOrCreateMidpoint(V1, V2);
					FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(M12);
					FinalMaterialIndices.Add(MatIdx);
					FinalTriangles.Add(V0); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
				}
				else // bHas20
				{
					int32 M20 = GetOrCreateMidpoint(V2, V0);
					FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(M20);
					FinalMaterialIndices.Add(MatIdx);
					FinalTriangles.Add(M20); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
				}
			}
			else // NumMidpoints == 2
			{
				// GREEN split: 2개 중점 → 3개 삼각형
				GreenSplit2++;
				if (!bHas20) // bHas01 && bHas12
				{
					int32 M01 = GetOrCreateMidpoint(V0, V1);
					int32 M12 = GetOrCreateMidpoint(V1, V2);

					FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(M12);
					FinalMaterialIndices.Add(MatIdx);

					float DiagA = FVector::DistSquared(Positions[M01], Positions[V2]);
					float DiagB = FVector::DistSquared(Positions[V0], Positions[M12]);
					if (DiagA < DiagB)
					{
						FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(M01); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
					}
					else
					{
						FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(M12);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(V0); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
					}
				}
				else if (!bHas01) // bHas12 && bHas20
				{
					int32 M12 = GetOrCreateMidpoint(V1, V2);
					int32 M20 = GetOrCreateMidpoint(V2, V0);

					FinalTriangles.Add(M20); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);

					float DiagA = FVector::DistSquared(Positions[V0], Positions[M12]);
					float DiagB = FVector::DistSquared(Positions[V1], Positions[M20]);
					if (DiagA < DiagB)
					{
						FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(M12);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(V0); FinalTriangles.Add(M12); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
					}
					else
					{
						FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(V1); FinalTriangles.Add(M12); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
					}
				}
				else // bHas01 && bHas20
				{
					int32 M01 = GetOrCreateMidpoint(V0, V1);
					int32 M20 = GetOrCreateMidpoint(V2, V0);

					FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(M20);
					FinalMaterialIndices.Add(MatIdx);

					float DiagA = FVector::DistSquared(Positions[V1], Positions[M20]);
					float DiagB = FVector::DistSquared(Positions[M01], Positions[V2]);
					if (DiagA < DiagB)
					{
						FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(M20); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
					}
					else
					{
						FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
						FinalTriangles.Add(M01); FinalTriangles.Add(V2); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
					}
				}
			}
		}

		Triangles = MoveTemp(FinalTriangles);
		MaterialIndices = MoveTemp(FinalMaterialIndices);

		UE_LOG(LogTemp, Log, TEXT("  Level %d: RED=%d, GREEN(1)=%d, GREEN(2)=%d, GREEN(3)=%d"),
			Level + 1, SplitCount, GreenSplit1, GreenSplit2, GreenSplit3);

		// If no split occurred, stop early
		if (SplitCount == 0 && GreenSplit1 == 0 && GreenSplit2 == 0 && GreenSplit3 == 0)
		{
			UE_LOG(LogTemp, Log, TEXT("  Early stop: No more triangles to split"));
			break;
		}
	}

	// Rebuild half-edge mesh from result with parent info
	Mesh.Clear();
	Mesh.BuildFromTriangles(Positions, Triangles, UVs, MaterialIndices, &ParentIndices);

	int32 FinalTriCount = Triangles.Num() / 3;
	int32 FinalVertCount = Positions.Num();

	UE_LOG(LogTemp, Log, TEXT("  Final: %d vertices, %d triangles"), FinalVertCount, FinalTriCount);
	UE_LOG(LogTemp, Log, TEXT("  Added: %d vertices, %d triangles"), FinalVertCount - InitialVertCount, FinalTriCount - InitialTriCount);
	UE_LOG(LogTemp, Log, TEXT("================================================="));
	UE_LOG(LogTemp, Log, TEXT(""));

	return FinalTriCount - InitialTriCount;
}

int32 FLEBSubdivision::SubdivideSelectedFaces(
	FHalfEdgeMesh& Mesh,
	const TSet<int32>& TargetFaces,
	int32 MaxLevel,
	float MinEdgeLength)
{
	UE_LOG(LogTemp, Log, TEXT(""));
	UE_LOG(LogTemp, Log, TEXT("======== SubdivideSelectedFaces (Bone Region) ========"));
	UE_LOG(LogTemp, Log, TEXT("  TargetFaces: %d, MaxLevel: %d, MinEdgeLength: %.2f"),
		TargetFaces.Num(), MaxLevel, MinEdgeLength);

	// Step 1: Export current mesh to simple triangle format
	TArray<FVector> Positions;
	TArray<FVector2D> UVs;
	TArray<int32> Triangles;
	TArray<int32> MaterialIndices;
	TArray<TPair<int32, int32>> ParentIndices;

	Positions.Reserve(Mesh.Vertices.Num());
	UVs.Reserve(Mesh.Vertices.Num());
	ParentIndices.Reserve(Mesh.Vertices.Num());
	for (const FHalfEdgeVertex& V : Mesh.Vertices)
	{
		Positions.Add(V.Position);
		UVs.Add(V.UV);
		ParentIndices.Add(TPair<int32, int32>(V.ParentIndex0, V.ParentIndex1));
	}

	// 삼각형 인덱스 + 대상 여부 추적
	TArray<bool> IsTargetTriangle;
	for (int32 FaceIdx = 0; FaceIdx < Mesh.Faces.Num(); FaceIdx++)
	{
		int32 V0, V1, V2;
		Mesh.GetFaceVertices(FaceIdx, V0, V1, V2);
		if (V0 >= 0 && V1 >= 0 && V2 >= 0)
		{
			Triangles.Add(V0);
			Triangles.Add(V1);
			Triangles.Add(V2);
			MaterialIndices.Add(Mesh.Faces[FaceIdx].MaterialIndex);
			IsTargetTriangle.Add(TargetFaces.Contains(FaceIdx));
		}
	}

	int32 InitialTriCount = Triangles.Num() / 3;
	int32 InitialVertCount = Positions.Num();
	int32 InitialTargetCount = 0;
	for (bool bTarget : IsTargetTriangle) { if (bTarget) InitialTargetCount++; }

	UE_LOG(LogTemp, Log, TEXT("  Initial: %d vertices, %d triangles (%d in target region)"),
		InitialVertCount, InitialTriCount, InitialTargetCount);

	// ============================================================================
	// Dual Midpoint Map System (Position + Index) - GREEN split 지원
	// ============================================================================
	constexpr float MidpointWeldPrecision = 0.1f;

	auto PositionToKey = [MidpointWeldPrecision](const FVector& Pos) -> FIntVector
	{
		return FIntVector(
			FMath::RoundToInt(Pos.X / MidpointWeldPrecision),
			FMath::RoundToInt(Pos.Y / MidpointWeldPrecision),
			FMath::RoundToInt(Pos.Z / MidpointWeldPrecision)
		);
	};

	auto IntVectorLess = [](const FIntVector& A, const FIntVector& B) -> bool
	{
		if (A.X != B.X) return A.X < B.X;
		if (A.Y != B.Y) return A.Y < B.Y;
		return A.Z < B.Z;
	};

	// 1. Position-based set: GREEN split 감지용
	TSet<TPair<FIntVector, FIntVector>> PositionMidpointSet;

	auto MakePositionKey = [&](int32 VA, int32 VB) -> TPair<FIntVector, FIntVector>
	{
		FIntVector KeyA = PositionToKey(Positions[VA]);
		FIntVector KeyB = PositionToKey(Positions[VB]);
		if (IntVectorLess(KeyA, KeyB))
			return TPair<FIntVector, FIntVector>(KeyA, KeyB);
		else
			return TPair<FIntVector, FIntVector>(KeyB, KeyA);
	};

	// 2. Index-based map: 버텍스 재사용용 (같은 인덱스 = 같은 UV)
	TMap<TPair<int32, int32>, int32> IndexMidpointMap;

	auto MakeIndexKey = [](int32 A, int32 B) -> TPair<int32, int32>
	{
		return A < B ? TPair<int32, int32>(A, B) : TPair<int32, int32>(B, A);
	};

	// GetOrCreateMidpoint: 인덱스 기반으로 재사용, 위치 기반으로 존재 등록
	auto GetOrCreateMidpoint = [&](int32 VA, int32 VB) -> int32
	{
		TPair<int32, int32> IndexKey = MakeIndexKey(VA, VB);
		if (int32* Existing = IndexMidpointMap.Find(IndexKey))
		{
			return *Existing;
		}

		int32 NewIdx = Positions.Num();
		Positions.Add((Positions[VA] + Positions[VB]) * 0.5f);
		UVs.Add((UVs[VA] + UVs[VB]) * 0.5f);
		ParentIndices.Add(TPair<int32, int32>(VA, VB));

		IndexMidpointMap.Add(IndexKey, NewIdx);
		PositionMidpointSet.Add(MakePositionKey(VA, VB));  // GREEN split 감지용
		return NewIdx;
	};

	// GREEN split용: 이 edge에 중점이 있는지 확인 (위치 기반)
	auto HasMidpointAtEdge = [&](int32 VA, int32 VB) -> bool
	{
		return PositionMidpointSet.Contains(MakePositionKey(VA, VB));
	};

	// Perform multiple levels of subdivision
	for (int32 Level = 0; Level < MaxLevel; Level++)
	{
		TArray<int32> NewTriangles;
		TArray<int32> NewMaterialIndices;
		TArray<bool> NewIsTarget;
		NewTriangles.Reserve(Triangles.Num() * 4);
		NewMaterialIndices.Reserve(MaterialIndices.Num() * 4);
		NewIsTarget.Reserve(IsTargetTriangle.Num() * 4);

		int32 NumTris = Triangles.Num() / 3;
		int32 SplitCount = 0;

		// Phase 1: RED splits - 대상 삼각형만 + MinEdgeLength 조건으로 4분할
		for (int32 i = 0; i < NumTris; i++)
		{
			int32 V0 = Triangles[i * 3];
			int32 V1 = Triangles[i * 3 + 1];
			int32 V2 = Triangles[i * 3 + 2];
			int32 MatIdx = MaterialIndices.IsValidIndex(i) ? MaterialIndices[i] : 0;
			bool bIsTarget = IsTargetTriangle.IsValidIndex(i) ? IsTargetTriangle[i] : false;

			// ★ 대상 삼각형인지 확인
			if (!bIsTarget)
			{
				// 대상이 아니면 그대로 유지 (GREEN split은 나중에 할 수 있음)
				NewTriangles.Add(V0); NewTriangles.Add(V1); NewTriangles.Add(V2);
				NewMaterialIndices.Add(MatIdx);
				NewIsTarget.Add(false);
				continue;
			}

			const FVector& P0 = Positions[V0];
			const FVector& P1 = Positions[V1];
			const FVector& P2 = Positions[V2];

			// MinEdgeLength 조건 확인
			float MaxEdgeLen = FMath::Max3(
				FVector::Dist(P0, P1),
				FVector::Dist(P1, P2),
				FVector::Dist(P2, P0)
			);

			if (MaxEdgeLen >= MinEdgeLength)
			{
				// RED: Split into 4 triangles
				int32 M01 = GetOrCreateMidpoint(V0, V1);
				int32 M12 = GetOrCreateMidpoint(V1, V2);
				int32 M20 = GetOrCreateMidpoint(V2, V0);

				NewTriangles.Add(V0); NewTriangles.Add(M01); NewTriangles.Add(M20);
				NewMaterialIndices.Add(MatIdx);
				NewIsTarget.Add(true);

				NewTriangles.Add(M01); NewTriangles.Add(V1); NewTriangles.Add(M12);
				NewMaterialIndices.Add(MatIdx);
				NewIsTarget.Add(true);

				NewTriangles.Add(M20); NewTriangles.Add(M12); NewTriangles.Add(V2);
				NewMaterialIndices.Add(MatIdx);
				NewIsTarget.Add(true);

				NewTriangles.Add(M01); NewTriangles.Add(M12); NewTriangles.Add(M20);
				NewMaterialIndices.Add(MatIdx);
				NewIsTarget.Add(true);

				SplitCount++;
			}
			else
			{
				// 이미 충분히 작음
				NewTriangles.Add(V0); NewTriangles.Add(V1); NewTriangles.Add(V2);
				NewMaterialIndices.Add(MatIdx);
				NewIsTarget.Add(true);
			}
		}

		// Phase 2: GREEN splits - T-junction 수정
		TArray<int32> FinalTriangles;
		TArray<int32> FinalMaterialIndices;
		TArray<bool> FinalIsTarget;
		FinalTriangles.Reserve(NewTriangles.Num() * 2);
		FinalMaterialIndices.Reserve(NewMaterialIndices.Num() * 2);
		FinalIsTarget.Reserve(NewIsTarget.Num() * 2);

		int32 GreenSplit1 = 0, GreenSplit2 = 0, GreenSplit3 = 0;

		NumTris = NewTriangles.Num() / 3;
		for (int32 i = 0; i < NumTris; i++)
		{
			int32 V0 = NewTriangles[i * 3];
			int32 V1 = NewTriangles[i * 3 + 1];
			int32 V2 = NewTriangles[i * 3 + 2];
			int32 MatIdx = NewMaterialIndices.IsValidIndex(i) ? NewMaterialIndices[i] : 0;
			bool bIsTarget = NewIsTarget.IsValidIndex(i) ? NewIsTarget[i] : false;

			// 위치 기반으로 중점 존재 여부 감지
			bool bHas01 = HasMidpointAtEdge(V0, V1);
			bool bHas12 = HasMidpointAtEdge(V1, V2);
			bool bHas20 = HasMidpointAtEdge(V2, V0);

			int32 NumMidpoints = (bHas01 ? 1 : 0) + (bHas12 ? 1 : 0) + (bHas20 ? 1 : 0);

			if (NumMidpoints == 0)
			{
				// 분할 불필요
				FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
				FinalMaterialIndices.Add(MatIdx);
				FinalIsTarget.Add(bIsTarget);
			}
			else if (NumMidpoints == 3)
			{
				// GREEN-3: 이웃들이 전부 RED split됨 → 강제 4분할
				GreenSplit3++;
				int32 M01 = GetOrCreateMidpoint(V0, V1);
				int32 M12 = GetOrCreateMidpoint(V1, V2);
				int32 M20 = GetOrCreateMidpoint(V2, V0);

				FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(M20);
				FinalMaterialIndices.Add(MatIdx);
				FinalIsTarget.Add(bIsTarget);

				FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(M12);
				FinalMaterialIndices.Add(MatIdx);
				FinalIsTarget.Add(bIsTarget);

				FinalTriangles.Add(M20); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
				FinalMaterialIndices.Add(MatIdx);
				FinalIsTarget.Add(bIsTarget);

				FinalTriangles.Add(M01); FinalTriangles.Add(M12); FinalTriangles.Add(M20);
				FinalMaterialIndices.Add(MatIdx);
				FinalIsTarget.Add(bIsTarget);
			}
			else if (NumMidpoints == 1)
			{
				// GREEN split: 1개 중점 → 2개 삼각형
				GreenSplit1++;
				if (bHas01)
				{
					int32 M01 = GetOrCreateMidpoint(V0, V1);
					FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
					FinalIsTarget.Add(bIsTarget);
					FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
					FinalIsTarget.Add(bIsTarget);
				}
				else if (bHas12)
				{
					int32 M12 = GetOrCreateMidpoint(V1, V2);
					FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(M12);
					FinalMaterialIndices.Add(MatIdx);
					FinalIsTarget.Add(bIsTarget);
					FinalTriangles.Add(V0); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
					FinalIsTarget.Add(bIsTarget);
				}
				else // bHas20
				{
					int32 M20 = GetOrCreateMidpoint(V2, V0);
					FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(M20);
					FinalMaterialIndices.Add(MatIdx);
					FinalIsTarget.Add(bIsTarget);
					FinalTriangles.Add(M20); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
					FinalIsTarget.Add(bIsTarget);
				}
			}
			else // NumMidpoints == 2
			{
				// GREEN split: 2개 중점 → 3개 삼각형
				GreenSplit2++;
				if (!bHas20) // bHas01 && bHas12
				{
					int32 M01 = GetOrCreateMidpoint(V0, V1);
					int32 M12 = GetOrCreateMidpoint(V1, V2);

					FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(M12);
					FinalMaterialIndices.Add(MatIdx);
					FinalIsTarget.Add(bIsTarget);

					float DiagA = FVector::DistSquared(Positions[M01], Positions[V2]);
					float DiagB = FVector::DistSquared(Positions[V0], Positions[M12]);
					if (DiagA < DiagB)
					{
						FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
						FinalIsTarget.Add(bIsTarget);
						FinalTriangles.Add(M01); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
						FinalIsTarget.Add(bIsTarget);
					}
					else
					{
						FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(M12);
						FinalMaterialIndices.Add(MatIdx);
						FinalIsTarget.Add(bIsTarget);
						FinalTriangles.Add(V0); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
						FinalIsTarget.Add(bIsTarget);
					}
				}
				else if (!bHas01) // bHas12 && bHas20
				{
					int32 M12 = GetOrCreateMidpoint(V1, V2);
					int32 M20 = GetOrCreateMidpoint(V2, V0);

					FinalTriangles.Add(M20); FinalTriangles.Add(M12); FinalTriangles.Add(V2);
					FinalMaterialIndices.Add(MatIdx);
					FinalIsTarget.Add(bIsTarget);

					float DiagA = FVector::DistSquared(Positions[V0], Positions[M12]);
					float DiagB = FVector::DistSquared(Positions[V1], Positions[M20]);
					if (DiagA < DiagB)
					{
						FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(M12);
						FinalMaterialIndices.Add(MatIdx);
						FinalIsTarget.Add(bIsTarget);
						FinalTriangles.Add(V0); FinalTriangles.Add(M12); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
						FinalIsTarget.Add(bIsTarget);
					}
					else
					{
						FinalTriangles.Add(V0); FinalTriangles.Add(V1); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
						FinalIsTarget.Add(bIsTarget);
						FinalTriangles.Add(V1); FinalTriangles.Add(M12); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
						FinalIsTarget.Add(bIsTarget);
					}
				}
				else // bHas01 && bHas20
				{
					int32 M01 = GetOrCreateMidpoint(V0, V1);
					int32 M20 = GetOrCreateMidpoint(V2, V0);

					FinalTriangles.Add(V0); FinalTriangles.Add(M01); FinalTriangles.Add(M20);
					FinalMaterialIndices.Add(MatIdx);
					FinalIsTarget.Add(bIsTarget);

					float DiagA = FVector::DistSquared(Positions[V1], Positions[M20]);
					float DiagB = FVector::DistSquared(Positions[M01], Positions[V2]);
					if (DiagA < DiagB)
					{
						FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
						FinalIsTarget.Add(bIsTarget);
						FinalTriangles.Add(M20); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
						FinalIsTarget.Add(bIsTarget);
					}
					else
					{
						FinalTriangles.Add(M01); FinalTriangles.Add(V1); FinalTriangles.Add(V2);
						FinalMaterialIndices.Add(MatIdx);
						FinalIsTarget.Add(bIsTarget);
						FinalTriangles.Add(M01); FinalTriangles.Add(V2); FinalTriangles.Add(M20);
						FinalMaterialIndices.Add(MatIdx);
						FinalIsTarget.Add(bIsTarget);
					}
				}
			}
		}

		Triangles = MoveTemp(FinalTriangles);
		MaterialIndices = MoveTemp(FinalMaterialIndices);
		IsTargetTriangle = MoveTemp(FinalIsTarget);

		UE_LOG(LogTemp, Log, TEXT("  Level %d: RED=%d, GREEN(1)=%d, GREEN(2)=%d, GREEN(3)=%d"),
			Level + 1, SplitCount, GreenSplit1, GreenSplit2, GreenSplit3);

		// If no split occurred, stop early
		if (SplitCount == 0 && GreenSplit1 == 0 && GreenSplit2 == 0 && GreenSplit3 == 0)
		{
			UE_LOG(LogTemp, Log, TEXT("  Early stop: No more triangles to split"));
			break;
		}
	}

	// Rebuild half-edge mesh from result with parent info
	Mesh.Clear();
	Mesh.BuildFromTriangles(Positions, Triangles, UVs, MaterialIndices, &ParentIndices);

	int32 FinalTriCount = Triangles.Num() / 3;
	int32 FinalVertCount = Positions.Num();

	UE_LOG(LogTemp, Log, TEXT("  Final: %d vertices, %d triangles"), FinalVertCount, FinalTriCount);
	UE_LOG(LogTemp, Log, TEXT("  Added: %d vertices, %d triangles"), FinalVertCount - InitialVertCount, FinalTriCount - InitialTriCount);
	UE_LOG(LogTemp, Log, TEXT("======================================================="));
	UE_LOG(LogTemp, Log, TEXT(""));

	return FinalTriCount - InitialTriCount;
}
