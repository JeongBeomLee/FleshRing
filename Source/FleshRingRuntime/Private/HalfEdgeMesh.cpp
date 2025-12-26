// HalfEdgeMesh.cpp
// Implementation of Half-Edge mesh and Red-Green adaptive subdivision

#include "HalfEdgeMesh.h"

//=============================================================================
// FHalfEdgeMesh Implementation
//=============================================================================

bool FHalfEdgeMesh::BuildFromTriangles(const TArray<FVector>& InVertices, const TArray<int32>& InTriangles, const TArray<FVector2D>& InUVs)
{
	Clear();

	if (InTriangles.Num() < 3 || InTriangles.Num() % 3 != 0)
	{
		UE_LOG(LogTemp, Error, TEXT("HalfEdgeMesh: Invalid triangle count %d"), InTriangles.Num());
		return false;
	}

	// Copy vertices
	Vertices.Reserve(InVertices.Num());
	for (int32 i = 0; i < InVertices.Num(); i++)
	{
		FHalfEdgeVertex Vert;
		Vert.Position = InVertices[i];
		Vert.UV = InUVs.IsValidIndex(i) ? InUVs[i] : FVector2D::ZeroVector;
		Vert.HalfEdgeIndex = -1;
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

void FHalfEdgeMesh::ExportToTriangles(TArray<FVector>& OutVertices, TArray<int32>& OutTriangles, TArray<FVector2D>& OutUVs, TArray<FVector>& OutNormals) const
{
	OutVertices.Empty();
	OutTriangles.Empty();
	OutUVs.Empty();
	OutNormals.Empty();

	OutVertices.Reserve(Vertices.Num());
	OutUVs.Reserve(Vertices.Num());
	for (const FHalfEdgeVertex& Vert : Vertices)
	{
		OutVertices.Add(Vert.Position);
		OutUVs.Add(Vert.UV);
	}

	OutTriangles.Reserve(Faces.Num() * 3);
	for (int32 FaceIdx = 0; FaceIdx < Faces.Num(); FaceIdx++)
	{
		int32 V0, V1, V2;
		GetFaceVertices(FaceIdx, V0, V1, V2);

		if (V0 >= 0 && V1 >= 0 && V2 >= 0)
		{
			OutTriangles.Add(V0);
			OutTriangles.Add(V1);
			OutTriangles.Add(V2);
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

	Positions.Reserve(Mesh.Vertices.Num());
	UVs.Reserve(Mesh.Vertices.Num());
	for (const FHalfEdgeVertex& V : Mesh.Vertices)
	{
		Positions.Add(V.Position);
		UVs.Add(V.UV);
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
		}
	}

	int32 InitialTriCount = Triangles.Num() / 3;

	// Midpoint map: edge (VA, VB) -> midpoint vertex index
	TMap<TPair<int32, int32>, int32> MidpointMap;

	auto MakeEdgeKey = [](int32 A, int32 B) -> TPair<int32, int32>
	{
		return A < B ? TPair<int32, int32>(A, B) : TPair<int32, int32>(B, A);
	};

	auto GetOrCreateMidpoint = [&](int32 VA, int32 VB) -> int32
	{
		TPair<int32, int32> Key = MakeEdgeKey(VA, VB);
		if (int32* Existing = MidpointMap.Find(Key))
		{
			return *Existing;
		}

		int32 NewIdx = Positions.Num();
		Positions.Add((Positions[VA] + Positions[VB]) * 0.5f);
		UVs.Add((UVs[VA] + UVs[VB]) * 0.5f);
		MidpointMap.Add(Key, NewIdx);
		return NewIdx;
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
		NewTriangles.Reserve(Triangles.Num() * 4);

		int32 NumTris = Triangles.Num() / 3;
		TArray<bool> TriNeedsRedSplit;
		TriNeedsRedSplit.SetNumZeroed(NumTris);

		// Phase 1: Mark triangles that need RED split (4-way, in region)
		for (int32 i = 0; i < NumTris; i++)
		{
			int32 V0 = Triangles[i * 3];
			int32 V1 = Triangles[i * 3 + 1];
			int32 V2 = Triangles[i * 3 + 2];

			const FVector& P0 = Positions[V0];
			const FVector& P1 = Positions[V1];
			const FVector& P2 = Positions[V2];

			// Check if any vertex or center is near torus surface
			bool bInRegion = false;
			if (IsInInfluenceRegion(P0)) bInRegion = true;
			else if (IsInInfluenceRegion(P1)) bInRegion = true;
			else if (IsInInfluenceRegion(P2)) bInRegion = true;
			else
			{
				FVector Center = (P0 + P1 + P2) / 3.0f;
				if (IsInInfluenceRegion(Center)) bInRegion = true;
			}

			if (!bInRegion) continue;

			// Check minimum edge length
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

			if (TriNeedsRedSplit[i])
			{
				// RED: Split into 4 triangles
				int32 M01 = GetOrCreateMidpoint(V0, V1);
				int32 M12 = GetOrCreateMidpoint(V1, V2);
				int32 M20 = GetOrCreateMidpoint(V2, V0);

				// Triangle 1: V0, M01, M20
				NewTriangles.Add(V0);
				NewTriangles.Add(M01);
				NewTriangles.Add(M20);

				// Triangle 2: M01, V1, M12
				NewTriangles.Add(M01);
				NewTriangles.Add(V1);
				NewTriangles.Add(M12);

				// Triangle 3: M20, M12, V2
				NewTriangles.Add(M20);
				NewTriangles.Add(M12);
				NewTriangles.Add(V2);

				// Triangle 4: M01, M12, M20 (center)
				NewTriangles.Add(M01);
				NewTriangles.Add(M12);
				NewTriangles.Add(M20);
			}
			else
			{
				// Keep original triangle for now (may be GREEN split later)
				NewTriangles.Add(V0);
				NewTriangles.Add(V1);
				NewTriangles.Add(V2);
			}
		}

		// Phase 3: GREEN splits - fix T-junctions
		// Check each non-red triangle for dangling midpoints
		TArray<int32> FinalTriangles;
		FinalTriangles.Reserve(NewTriangles.Num() * 2);

		NumTris = NewTriangles.Num() / 3;
		for (int32 i = 0; i < NumTris; i++)
		{
			int32 V0 = NewTriangles[i * 3];
			int32 V1 = NewTriangles[i * 3 + 1];
			int32 V2 = NewTriangles[i * 3 + 2];

			// Check if any edge has a midpoint
			int32* M01 = MidpointMap.Find(MakeEdgeKey(V0, V1));
			int32* M12 = MidpointMap.Find(MakeEdgeKey(V1, V2));
			int32* M20 = MidpointMap.Find(MakeEdgeKey(V2, V0));

			int32 NumMidpoints = (M01 ? 1 : 0) + (M12 ? 1 : 0) + (M20 ? 1 : 0);

			if (NumMidpoints == 0)
			{
				// No midpoints on edges, keep as is
				FinalTriangles.Add(V0);
				FinalTriangles.Add(V1);
				FinalTriangles.Add(V2);
			}
			else if (NumMidpoints == 3)
			{
				// All edges have midpoints - this was a RED triangle, already split
				FinalTriangles.Add(V0);
				FinalTriangles.Add(V1);
				FinalTriangles.Add(V2);
			}
			else
			{
				// GREEN: Some edges have midpoints, need to split
				// Split into 2, 3, or 4 triangles depending on configuration

				if (NumMidpoints == 1)
				{
					// One edge has midpoint - split into 2
					if (M01)
					{
						// Edge V0-V1 has midpoint
						FinalTriangles.Add(V0);
						FinalTriangles.Add(*M01);
						FinalTriangles.Add(V2);

						FinalTriangles.Add(*M01);
						FinalTriangles.Add(V1);
						FinalTriangles.Add(V2);
					}
					else if (M12)
					{
						// Edge V1-V2 has midpoint
						FinalTriangles.Add(V0);
						FinalTriangles.Add(V1);
						FinalTriangles.Add(*M12);

						FinalTriangles.Add(V0);
						FinalTriangles.Add(*M12);
						FinalTriangles.Add(V2);
					}
					else // M20
					{
						// Edge V2-V0 has midpoint
						FinalTriangles.Add(V0);
						FinalTriangles.Add(V1);
						FinalTriangles.Add(*M20);

						FinalTriangles.Add(*M20);
						FinalTriangles.Add(V1);
						FinalTriangles.Add(V2);
					}
				}
				else if (NumMidpoints == 2)
				{
					// Two edges have midpoints - split into 3
					if (M01 && M12)
					{
						FinalTriangles.Add(V0);
						FinalTriangles.Add(*M01);
						FinalTriangles.Add(V2);

						FinalTriangles.Add(*M01);
						FinalTriangles.Add(V1);
						FinalTriangles.Add(*M12);

						FinalTriangles.Add(*M01);
						FinalTriangles.Add(*M12);
						FinalTriangles.Add(V2);
					}
					else if (M12 && M20)
					{
						FinalTriangles.Add(V0);
						FinalTriangles.Add(V1);
						FinalTriangles.Add(*M12);

						FinalTriangles.Add(V0);
						FinalTriangles.Add(*M12);
						FinalTriangles.Add(*M20);

						FinalTriangles.Add(*M20);
						FinalTriangles.Add(*M12);
						FinalTriangles.Add(V2);
					}
					else // M01 && M20
					{
						FinalTriangles.Add(V0);
						FinalTriangles.Add(*M01);
						FinalTriangles.Add(*M20);

						FinalTriangles.Add(*M01);
						FinalTriangles.Add(V1);
						FinalTriangles.Add(*M20);

						FinalTriangles.Add(*M20);
						FinalTriangles.Add(V1);
						FinalTriangles.Add(V2);
					}
				}
			}
		}

		Triangles = MoveTemp(FinalTriangles);

		int32 CurrentTriCount = Triangles.Num() / 3;

		// If no change, stop
		if (CurrentTriCount == NumTris)
		{
			break;
		}
	}

	// Rebuild half-edge mesh from result
	TArray<FVector> OutNormals;
	Mesh.Clear();
	Mesh.BuildFromTriangles(Positions, Triangles, UVs);

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
