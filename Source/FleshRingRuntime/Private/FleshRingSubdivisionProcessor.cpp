// Copyright 2026 LgThx. All Rights Reserved.

// FleshRingSubdivisionProcessor.cpp
// CPU-side subdivision topology processor implementation

#include "FleshRingSubdivisionProcessor.h"
#include "HalfEdgeMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingSubdivisionProcessor, Log, All);

FFleshRingSubdivisionProcessor::FFleshRingSubdivisionProcessor()
{
}

FFleshRingSubdivisionProcessor::~FFleshRingSubdivisionProcessor()
{
}

bool FFleshRingSubdivisionProcessor::SetSourceMesh(
	const TArray<FVector>& InPositions,
	const TArray<uint32>& InIndices,
	const TArray<FVector2D>& InUVs,
	const TArray<int32>& InMaterialIndices)
{
	if (InPositions.Num() == 0 || InIndices.Num() == 0 || InIndices.Num() % 3 != 0)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("Invalid source mesh data"));
		return false;
	}

	SourcePositions = InPositions;
	SourceIndices = InIndices;
	SourceUVs = InUVs;
	SourceMaterialIndices = InMaterialIndices;

	// Initialize with empty array if UV is not available
	if (SourceUVs.Num() != SourcePositions.Num())
	{
		SourceUVs.SetNumZeroed(SourcePositions.Num());
	}

	// Initialize all to 0 if MaterialIndices is not available
	const int32 NumTriangles = SourceIndices.Num() / 3;
	if (SourceMaterialIndices.Num() != NumTriangles)
	{
		SourceMaterialIndices.SetNumZeroed(NumTriangles);
	}

	InvalidateCache();

	return true;
}

bool FFleshRingSubdivisionProcessor::SetSourceMeshFromSkeletalMesh(
	USkeletalMesh* SkeletalMesh,
	int32 LODIndex)
{
	if (!SkeletalMesh)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("SkeletalMesh is null"));
		return false;
	}

	FSkeletalMeshRenderData* RenderData = SkeletalMesh->GetResourceForRendering();
	if (!RenderData || LODIndex >= RenderData->LODRenderData.Num())
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("Invalid LOD index: %d"), LODIndex);
		return false;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];

	// Extract positions
	const uint32 NumVertices = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();
	TArray<FVector> Positions;
	Positions.SetNum(NumVertices);

	for (uint32 i = 0; i < NumVertices; ++i)
	{
		Positions[i] = FVector(LODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i));
	}

	// Extract indices
	TArray<uint32> Indices;
	const FRawStaticIndexBuffer16or32Interface* IndexBuffer = LODData.MultiSizeIndexContainer.GetIndexBuffer();
	if (IndexBuffer)
	{
		const int32 NumIndices = IndexBuffer->Num();
		Indices.SetNum(NumIndices);
		for (int32 i = 0; i < NumIndices; ++i)
		{
			Indices[i] = IndexBuffer->Get(i);
		}
	}

	// Extract UVs
	TArray<FVector2D> UVs;
	if (LODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetNumVertices() > 0)
	{
		UVs.SetNum(NumVertices);
		for (uint32 i = 0; i < NumVertices; ++i)
		{
			UVs[i] = FVector2D(LODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(i, 0));
		}
	}

	return SetSourceMesh(Positions, Indices, UVs);
}

void FFleshRingSubdivisionProcessor::SetRingParamsArray(const TArray<FSubdivisionRingParams>& InRingParamsArray)
{
	// Invalidate cache if array has changed
	if (InRingParamsArray.Num() != RingParamsArray.Num())
	{
		InvalidateCache();
	}
	else
	{
		for (int32 i = 0; i < InRingParamsArray.Num(); ++i)
		{
			if (NeedsRecomputation(InRingParamsArray[i]))
			{
				InvalidateCache();
				break;
			}
		}
	}

	RingParamsArray = InRingParamsArray;
}

void FFleshRingSubdivisionProcessor::AddRingParams(const FSubdivisionRingParams& RingParams)
{
	InvalidateCache();
	RingParamsArray.Add(RingParams);
}

void FFleshRingSubdivisionProcessor::ClearRingParams()
{
	if (RingParamsArray.Num() > 0)
	{
		InvalidateCache();
		RingParamsArray.Empty();
	}
}

void FFleshRingSubdivisionProcessor::SetTargetVertexIndices(const TSet<uint32>& InTargetVertexIndices)
{
	InvalidateCache();
	TargetVertexIndices = InTargetVertexIndices;
	bUseVertexBasedMode = TargetVertexIndices.Num() > 0;

	UE_LOG(LogFleshRingSubdivisionProcessor, Log,
		TEXT("SetTargetVertexIndices: %d vertices, VertexBasedMode=%s"),
		TargetVertexIndices.Num(),
		bUseVertexBasedMode ? TEXT("true") : TEXT("false"));
}

void FFleshRingSubdivisionProcessor::ClearTargetVertexIndices()
{
	if (bUseVertexBasedMode)
	{
		InvalidateCache();
		TargetVertexIndices.Empty();
		bUseVertexBasedMode = false;
	}
}

void FFleshRingSubdivisionProcessor::SetTargetTriangleIndices(const TSet<int32>& InTargetTriangleIndices)
{
	InvalidateCache();
	TargetTriangleIndices = InTargetTriangleIndices;
	bUseTriangleBasedMode = TargetTriangleIndices.Num() > 0;

	// Disable vertex-based mode when using triangle-based mode
	if (bUseTriangleBasedMode)
	{
		bUseVertexBasedMode = false;
	}

	UE_LOG(LogFleshRingSubdivisionProcessor, Log,
		TEXT("SetTargetTriangleIndices: %d triangles, TriangleBasedMode=%s"),
		TargetTriangleIndices.Num(),
		bUseTriangleBasedMode ? TEXT("true") : TEXT("false"));
}

void FFleshRingSubdivisionProcessor::ClearTargetTriangleIndices()
{
	if (bUseTriangleBasedMode)
	{
		InvalidateCache();
		TargetTriangleIndices.Empty();
		bUseTriangleBasedMode = false;
	}
}

void FFleshRingSubdivisionProcessor::SetRingParams(const FSubdivisionRingParams& RingParams)
{
	// Backward compatibility: Clear existing parameters and add a single Ring
	ClearRingParams();
	AddRingParams(RingParams);
}

void FFleshRingSubdivisionProcessor::SetSettings(const FSubdivisionProcessorSettings& Settings)
{
	if (CurrentSettings.MaxSubdivisionLevel != Settings.MaxSubdivisionLevel ||
		CurrentSettings.MinEdgeLength != Settings.MinEdgeLength)
	{
		InvalidateCache();
	}

	CurrentSettings = Settings;
}

void FFleshRingSubdivisionProcessor::InvalidateCache()
{
	bCacheValid = false;

	// Prevent memory leak: Also clear HalfEdgeMesh data
	// Will be rebuilt with new data during recomputation
	HalfEdgeMesh.Clear();

	// Also clear intermediate computation results
	OriginalToNewVertexMap.Empty();
	EdgeMidpointCache.Empty();
}

bool FFleshRingSubdivisionProcessor::Process(FSubdivisionTopologyResult& OutResult)
{
	// Return cached result if cache is valid
	if (bCacheValid)
	{
		OutResult = CachedResult;
		return true;
	}

	// Validate source data
	if (SourcePositions.Num() == 0 || SourceIndices.Num() == 0)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("No source mesh data"));
		return false;
	}

	OutResult.Reset();
	OutResult.OriginalVertexCount = SourcePositions.Num();
	OutResult.OriginalTriangleCount = SourceIndices.Num() / 3;

	// 1. Build Half-Edge mesh
	// Convert to int32 array (FHalfEdgeMesh uses int32)
	TArray<int32> IndicesInt32;
	IndicesInt32.SetNum(SourceIndices.Num());
	for (int32 i = 0; i < SourceIndices.Num(); ++i)
	{
		IndicesInt32[i] = static_cast<int32>(SourceIndices[i]);
	}

	if (!HalfEdgeMesh.BuildFromTriangles(SourcePositions, IndicesInt32, SourceUVs, SourceMaterialIndices))
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("Failed to build Half-Edge mesh"));
		return false;
	}

	// 2. Perform LEB/Red-Green Refinement
	int32 TotalFacesAdded = 0;

	if (bUseTriangleBasedMode && TargetTriangleIndices.Num() > 0)
	{
		// ========================================
		// Triangle-based mode: Directly use triangle set extracted from DI
		// ========================================
		UE_LOG(LogFleshRingSubdivisionProcessor, Log,
			TEXT("Process: Using triangle-based mode with %d target triangles"),
			TargetTriangleIndices.Num());

		const int32 NumTriangles = SourceIndices.Num() / 3;
		UE_LOG(LogFleshRingSubdivisionProcessor, Log,
			TEXT("Process: %d/%d triangles selected for subdivision (%.1f%%)"),
			TargetTriangleIndices.Num(), NumTriangles,
			100.0f * (float)TargetTriangleIndices.Num() / NumTriangles);

		// Subdivide only target triangles (passed directly)
		TotalFacesAdded = FLEBSubdivision::SubdivideSelectedFaces(
			HalfEdgeMesh,
			TargetTriangleIndices,
			CurrentSettings.MaxSubdivisionLevel,
			CurrentSettings.MinEdgeLength
		);
	}
	else if (bUseVertexBasedMode && TargetVertexIndices.Num() > 0)
	{
		// ========================================
		// Vertex-based mode: Subdivide triangles containing specified vertices
		// ========================================
		UE_LOG(LogFleshRingSubdivisionProcessor, Log,
			TEXT("Process: Using vertex-based mode with %d target vertices"),
			TargetVertexIndices.Num());

		// Collect triangles containing target vertices
		TSet<int32> TargetTrianglesLocal;
		const int32 NumTriangles = SourceIndices.Num() / 3;

		for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
		{
			uint32 V0 = SourceIndices[TriIdx * 3 + 0];
			uint32 V1 = SourceIndices[TriIdx * 3 + 1];
			uint32 V2 = SourceIndices[TriIdx * 3 + 2];

			// Mark for subdivision if any vertex of the triangle is in the target set
			if (TargetVertexIndices.Contains(V0) ||
				TargetVertexIndices.Contains(V1) ||
				TargetVertexIndices.Contains(V2))
			{
				TargetTrianglesLocal.Add(TriIdx);
			}
		}

		UE_LOG(LogFleshRingSubdivisionProcessor, Log,
			TEXT("Process: %d/%d triangles selected for subdivision (%.1f%%)"),
			TargetTrianglesLocal.Num(), NumTriangles,
			100.0f * (float)TargetTrianglesLocal.Num() / NumTriangles);

		// Subdivide only target triangles
		TotalFacesAdded = FLEBSubdivision::SubdivideSelectedFaces(
			HalfEdgeMesh,
			TargetTrianglesLocal,
			CurrentSettings.MaxSubdivisionLevel,
			CurrentSettings.MinEdgeLength
		);
	}
	else
	{
		// ========================================
		// Ring parameter-based mode (legacy approach)
		// ========================================
		for (int32 RingIdx = 0; RingIdx < RingParamsArray.Num(); ++RingIdx)
		{
			const FSubdivisionRingParams& CurrentRingParams = RingParamsArray[RingIdx];
			int32 FacesAdded = 0;

			if (CurrentRingParams.bUseSDFBounds)
			{
				// SDF mode: OBB-based region checking (accurate method)
				FSubdivisionOBB OBB = FSubdivisionOBB::CreateFromSDFBounds(
					CurrentRingParams.SDFBoundsMin,
					CurrentRingParams.SDFBoundsMax,
					CurrentRingParams.SDFLocalToComponent,
					CurrentRingParams.SDFInfluenceMultiplier
				);

				FacesAdded = FLEBSubdivision::SubdivideRegion(
					HalfEdgeMesh,
					OBB,
					CurrentSettings.MaxSubdivisionLevel,
					CurrentSettings.MinEdgeLength
				);
			}
			else
			{
				// VirtualRing mode: Torus approach
				FTorusParams TorusParams;
				TorusParams.Center = CurrentRingParams.Center;
				TorusParams.Axis = CurrentRingParams.Axis.GetSafeNormal();
				TorusParams.MajorRadius = CurrentRingParams.Radius;
				TorusParams.MinorRadius = CurrentRingParams.Width * 0.5f;
				TorusParams.InfluenceMargin = CurrentRingParams.GetInfluenceRadius();

				FacesAdded = FLEBSubdivision::SubdivideRegion(
					HalfEdgeMesh,
					TorusParams,
					CurrentSettings.MaxSubdivisionLevel,
					CurrentSettings.MinEdgeLength
				);
			}

			TotalFacesAdded += FacesAdded;
		}
	}

	// 3. Extract topology result
	if (!ExtractTopologyResult(OutResult))
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("Failed to extract topology result"));
		return false;
	}

	// Save to cache
	CachedResult = OutResult;
	CachedRingParamsArray = RingParamsArray;
	bCacheValid = true;

	UE_LOG(LogFleshRingSubdivisionProcessor, Log,
		TEXT("Subdivision complete: %d -> %d vertices, %d -> %d triangles"),
		OutResult.OriginalVertexCount, OutResult.SubdividedVertexCount,
		OutResult.OriginalTriangleCount, OutResult.SubdividedTriangleCount);

	return true;
}

bool FFleshRingSubdivisionProcessor::ExtractTopologyResult(FSubdivisionTopologyResult& OutResult)
{
	OriginalToNewVertexMap.Empty();
	EdgeMidpointCache.Empty();

	const int32 HEVertexCount = HalfEdgeMesh.GetVertexCount();
	const int32 HEFaceCount = HalfEdgeMesh.GetFaceCount();

	if (HEVertexCount == 0 || HEFaceCount == 0)
	{
		return false;
	}

	const int32 OriginalVertexCount = SourcePositions.Num();
	OutResult.VertexData.Reserve(HEVertexCount);

	// ========================================================================
	// Simplified parent vertex extraction: Direct read from HalfEdgeMesh - O(N)
	// (Already recorded at subdivision time)
	// ========================================================================

	// Step 1: Read parent info directly from HalfEdgeMesh
	TArray<TPair<int32, int32>> ImmediateParents;
	ImmediateParents.SetNum(HEVertexCount);

	for (int32 i = 0; i < HEVertexCount; ++i)
	{
		const FHalfEdgeVertex& Vert = HalfEdgeMesh.Vertices[i];

		if (Vert.IsOriginalVertex())
		{
			// Original vertex: Self is parent
			ImmediateParents[i] = TPair<int32, int32>(i, i);
		}
		else
		{
			// Subdivision vertex: Use stored parent info
			ImmediateParents[i] = TPair<int32, int32>(Vert.ParentIndex0, Vert.ParentIndex1);
		}
	}

	UE_LOG(LogFleshRingSubdivisionProcessor, Log,
		TEXT("Parent extraction: %d original, %d subdivided vertices (direct read from HalfEdgeMesh)"),
		OriginalVertexCount, HEVertexCount - OriginalVertexCount);

	// ========================================================================
	// Validation: Check if parent indices are valid
	// ========================================================================
	int32 InvalidParentCount = 0;
	int32 OutOfOrderCount = 0;
	for (int32 i = OriginalVertexCount; i < HEVertexCount; ++i)
	{
		const TPair<int32, int32>& Parents = ImmediateParents[i];
		int32 P0 = Parents.Key;
		int32 P1 = Parents.Value;

		// Check if parent indices are within valid range
		if (P0 < 0 || P0 >= HEVertexCount || P1 < 0 || P1 >= HEVertexCount)
		{
			InvalidParentCount++;
			if (InvalidParentCount <= 5)
			{
				UE_LOG(LogFleshRingSubdivisionProcessor, Error,
					TEXT("BUG: Vertex %d has INVALID parent indices P0=%d, P1=%d (valid range: 0-%d)"),
					i, P0, P1, HEVertexCount - 1);
			}
		}
		// Check if parent was created after self (topology order violation)
		else if (P0 >= i || P1 >= i)
		{
			OutOfOrderCount++;
			if (OutOfOrderCount <= 5)
			{
				UE_LOG(LogFleshRingSubdivisionProcessor, Error,
					TEXT("BUG: Vertex %d has OUT-OF-ORDER parent P0=%d, P1=%d (must be < %d)"),
					i, P0, P1, i);
			}
		}
	}

	if (InvalidParentCount > 0 || OutOfOrderCount > 0)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Error,
			TEXT("CRITICAL: %d invalid parents, %d out-of-order parents detected!"),
			InvalidParentCount, OutOfOrderCount);
	}

	// Step 2: Recursively trace to original vertices to calculate final contributions
	TArray<TMap<uint32, float>> OriginalContributions;
	OriginalContributions.SetNum(HEVertexCount);

	// Initialize original vertices
	for (int32 i = 0; i < OriginalVertexCount; ++i)
	{
		OriginalContributions[i].Add(i, 1.0f);
	}

	// Recursively calculate contributions for subdivision vertices
	int32 FallbackCount = 0;
	for (int32 i = OriginalVertexCount; i < HEVertexCount; ++i)
	{
		const TPair<int32, int32>& Parents = ImmediateParents[i];
		int32 P0 = Parents.Key;
		int32 P1 = Parents.Value;

		// Safety check
		if (P0 < 0 || P0 >= i || P1 < 0 || P1 >= i)
		{
			// WARNING: If this fallback occurs, bone weights will be set to vertex 0's values!
			FallbackCount++;
			if (FallbackCount <= 10)
			{
				UE_LOG(LogFleshRingSubdivisionProcessor, Error,
					TEXT("BONE WEIGHT FALLBACK! Vertex %d: P0=%d, P1=%d (must be 0 <= P < %d)"),
					i, P0, P1, i);
			}
			OriginalContributions[i].Add(0, 1.0f);
			continue;
		}

		// Inherit 0.5 contribution from each parent
		for (const auto& Contrib : OriginalContributions[P0])
		{
			OriginalContributions[i].FindOrAdd(Contrib.Key) += Contrib.Value * 0.5f;
		}
		for (const auto& Contrib : OriginalContributions[P1])
		{
			OriginalContributions[i].FindOrAdd(Contrib.Key) += Contrib.Value * 0.5f;
		}
	}

	// Output fallback count (should be 0 for normal operation)
	if (FallbackCount > 0)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Error,
			TEXT("CRITICAL: %d vertices fell back to vertex 0's bone weights! Animation will break!"),
			FallbackCount);
	}

	// Validation: Check if contribution total equals 1.0
	int32 EmptyContribCount = 0;
	int32 InvalidTotalCount = 0;
	for (int32 i = OriginalVertexCount; i < HEVertexCount; ++i)
	{
		const TMap<uint32, float>& Contribs = OriginalContributions[i];

		if (Contribs.Num() == 0)
		{
			EmptyContribCount++;
			if (EmptyContribCount <= 5)
			{
				UE_LOG(LogFleshRingSubdivisionProcessor, Error,
					TEXT("BUG: Vertex %d has EMPTY contributions! Parents=(%d,%d)"),
					i, ImmediateParents[i].Key, ImmediateParents[i].Value);
			}
		}
		else
		{
			float Total = 0.0f;
			for (const auto& C : Contribs)
			{
				Total += C.Value;
				// Check if contribution key (original vertex index) is within valid range
				if (C.Key >= (uint32)OriginalVertexCount)
				{
					UE_LOG(LogFleshRingSubdivisionProcessor, Error,
						TEXT("BUG: Vertex %d has contrib key %u >= OriginalCount %d"),
						i, C.Key, OriginalVertexCount);
				}
			}
			if (FMath::Abs(Total - 1.0f) > 0.01f)
			{
				InvalidTotalCount++;
				if (InvalidTotalCount <= 5)
				{
					UE_LOG(LogFleshRingSubdivisionProcessor, Warning,
						TEXT("Vertex %d: Total contribution = %.4f (expected 1.0)"),
						i, Total);
				}
			}
		}
	}

	if (EmptyContribCount > 0 || InvalidTotalCount > 0)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Error,
			TEXT("CONTRIBUTION ERRORS: %d empty, %d invalid totals"),
			EmptyContribCount, InvalidTotalCount);
	}

	// Step 3: Convert contributions to FSubdivisionVertexData (max 3 original vertices)
	for (int32 i = 0; i < HEVertexCount; ++i)
	{
		if (i < OriginalVertexCount)
		{
			OutResult.VertexData.Add(FSubdivisionVertexData::CreateOriginal(i));
		}
		else
		{
			const TMap<uint32, float>& Contribs = OriginalContributions[i];

			// Sort by contribution
			TArray<TPair<uint32, float>> SortedContribs;
			for (const auto& C : Contribs)
			{
				SortedContribs.Add(TPair<uint32, float>(C.Key, C.Value));
			}
			SortedContribs.Sort([](const TPair<uint32, float>& A, const TPair<uint32, float>& B)
			{
				return A.Value > B.Value;
			});

			// Use top 3 (warn if 4 or more contributors)
			if (SortedContribs.Num() > 3)
			{
				// Calculate total dropped contribution weight
				float DroppedWeight = 0.0f;
				for (int32 j = 3; j < SortedContribs.Num(); ++j)
				{
					DroppedWeight += SortedContribs[j].Value;
				}

				static int32 TruncationWarningCount = 0;
				if (TruncationWarningCount < 10)
				{
					UE_LOG(LogFleshRingSubdivisionProcessor, Warning,
						TEXT("Vertex %d has %d contributors (truncating to 3). Dropped weight: %.4f"),
						i, SortedContribs.Num(), DroppedWeight);
					TruncationWarningCount++;
				}
			}

			uint32 P0 = 0, P1 = 0, P2 = 0;
			float W0 = 0, W1 = 0, W2 = 0;

			if (SortedContribs.Num() >= 1) { P0 = SortedContribs[0].Key; W0 = SortedContribs[0].Value; }
			if (SortedContribs.Num() >= 2) { P1 = SortedContribs[1].Key; W1 = SortedContribs[1].Value; }
			if (SortedContribs.Num() >= 3) { P2 = SortedContribs[2].Key; W2 = SortedContribs[2].Value; }

			// Normalize
			float TotalWeight = W0 + W1 + W2;
			if (TotalWeight > 0.0f)
			{
				W0 /= TotalWeight;
				W1 /= TotalWeight;
				W2 /= TotalWeight;
			}
			else
			{
				W0 = 1.0f;
			}

			FSubdivisionVertexData Data = FSubdivisionVertexData::CreateBarycentric(
				P0, P1, P2, FVector3f(W0, W1, W2));
			OutResult.VertexData.Add(Data);
		}
	}

	// Extract triangle indices and material indices
	OutResult.Indices.Reserve(HEFaceCount * 3);
	OutResult.TriangleMaterialIndices.Reserve(HEFaceCount);

	for (int32 FaceIdx = 0; FaceIdx < HEFaceCount; ++FaceIdx)
	{
		int32 V0, V1, V2;
		HalfEdgeMesh.GetFaceVertices(FaceIdx, V0, V1, V2);

		OutResult.Indices.Add(static_cast<uint32>(V0));
		OutResult.Indices.Add(static_cast<uint32>(V1));
		OutResult.Indices.Add(static_cast<uint32>(V2));

		// Material index (inherited during subdivision process)
		OutResult.TriangleMaterialIndices.Add(HalfEdgeMesh.Faces[FaceIdx].MaterialIndex);
	}

	OutResult.SubdividedVertexCount = OutResult.VertexData.Num();
	OutResult.SubdividedTriangleCount = OutResult.Indices.Num() / 3;

	return true;
}

bool FFleshRingSubdivisionProcessor::NeedsRecomputation(const FSubdivisionRingParams& NewRingParams, float Threshold) const
{
	if (!bCacheValid)
	{
		return true;
	}

	// Find and compare matching parameters from cached array
	// (Used for index-order comparison in SetRingParamsArray)
	for (const FSubdivisionRingParams& CachedRingParams : CachedRingParamsArray)
	{
		// Skip if mode is different
		if (CachedRingParams.bUseSDFBounds != NewRingParams.bUseSDFBounds)
		{
			continue;
		}

		bool bMatches = false;

		if (NewRingParams.bUseSDFBounds)
		{
			// SDF mode: Check bounds changes
			float BoundsMinDist = FVector::Dist(CachedRingParams.SDFBoundsMin, NewRingParams.SDFBoundsMin);
			float BoundsMaxDist = FVector::Dist(CachedRingParams.SDFBoundsMax, NewRingParams.SDFBoundsMax);
			FVector CachedPos = CachedRingParams.SDFLocalToComponent.GetLocation();
			FVector NewPos = NewRingParams.SDFLocalToComponent.GetLocation();

			if (BoundsMinDist <= Threshold && BoundsMaxDist <= Threshold &&
				FVector::Dist(CachedPos, NewPos) <= Threshold)
			{
				bMatches = true;
			}
		}
		else
		{
			// VirtualRing mode: Legacy approach
			float CenterDist = FVector::Dist(CachedRingParams.Center, NewRingParams.Center);
			float AxisDot = FVector::DotProduct(CachedRingParams.Axis.GetSafeNormal(), NewRingParams.Axis.GetSafeNormal());

			if (CenterDist <= Threshold &&
				FMath::Abs(CachedRingParams.Radius - NewRingParams.Radius) <= Threshold * 0.1f &&
				AxisDot >= 0.99f)
			{
				bMatches = true;
			}
		}

		if (bMatches)
		{
			return false; // Matching parameters found in cache - no recomputation needed
		}
	}

	return true; // No matching parameters in cache - recomputation needed
}

bool FFleshRingSubdivisionProcessor::ProcessUniform(FSubdivisionTopologyResult& OutResult, int32 MaxLevel)
{
	// Validate source data
	if (SourcePositions.Num() == 0 || SourceIndices.Num() == 0)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("ProcessUniform: No source mesh data"));
		return false;
	}

	OutResult.Reset();
	OutResult.OriginalVertexCount = SourcePositions.Num();
	OutResult.OriginalTriangleCount = SourceIndices.Num() / 3;

	// 1. Build Half-Edge mesh
	TArray<int32> IndicesInt32;
	IndicesInt32.SetNum(SourceIndices.Num());
	for (int32 i = 0; i < SourceIndices.Num(); ++i)
	{
		IndicesInt32[i] = static_cast<int32>(SourceIndices[i]);
	}

	if (!HalfEdgeMesh.BuildFromTriangles(SourcePositions, IndicesInt32, SourceUVs, SourceMaterialIndices))
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("ProcessUniform: Failed to build Half-Edge mesh"));
		return false;
	}

	// 2. Perform uniform subdivision
	FLEBSubdivision::SubdivideUniform(
		HalfEdgeMesh,
		MaxLevel,
		CurrentSettings.MinEdgeLength
	);

	// 3. Extract topology result
	if (!ExtractTopologyResult(OutResult))
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("ProcessUniform: Failed to extract topology result"));
		return false;
	}

	UE_LOG(LogFleshRingSubdivisionProcessor, Log,
		TEXT("ProcessUniform: Complete - %d -> %d vertices, %d -> %d triangles"),
		OutResult.OriginalVertexCount, OutResult.SubdividedVertexCount,
		OutResult.OriginalTriangleCount, OutResult.SubdividedTriangleCount);

	return true;
}

// =====================================
// Bone region-based subdivision (optimized for editor preview)
// =====================================

bool FFleshRingSubdivisionProcessor::SetSourceMeshWithBoneInfo(USkeletalMesh* SkeletalMesh, int32 LODIndex)
{
	if (!SkeletalMesh)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("SetSourceMeshWithBoneInfo: SkeletalMesh is null"));
		return false;
	}

	FSkeletalMeshRenderData* RenderData = SkeletalMesh->GetResourceForRendering();
	if (!RenderData || LODIndex >= RenderData->LODRenderData.Num())
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("SetSourceMeshWithBoneInfo: Invalid LOD index: %d"), LODIndex);
		return false;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];
	const uint32 NumVertices = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

	// Extract positions
	TArray<FVector> Positions;
	Positions.SetNum(NumVertices);
	for (uint32 i = 0; i < NumVertices; ++i)
	{
		Positions[i] = FVector(LODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i));
	}

	// Extract indices
	TArray<uint32> Indices;
	const FRawStaticIndexBuffer16or32Interface* IndexBuffer = LODData.MultiSizeIndexContainer.GetIndexBuffer();
	if (IndexBuffer)
	{
		const int32 NumIndices = IndexBuffer->Num();
		Indices.SetNum(NumIndices);
		for (int32 i = 0; i < NumIndices; ++i)
		{
			Indices[i] = IndexBuffer->Get(i);
		}
	}

	// Extract UVs
	TArray<FVector2D> UVs;
	if (LODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetNumVertices() > 0)
	{
		UVs.SetNum(NumVertices);
		for (uint32 i = 0; i < NumVertices; ++i)
		{
			UVs[i] = FVector2D(LODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(i, 0));
		}
	}

	// Extract material indices per section
	TArray<int32> MaterialIndices;
	{
		const int32 NumTriangles = Indices.Num() / 3;
		MaterialIndices.SetNum(NumTriangles);
		for (int32& MatIdx : MaterialIndices) { MatIdx = 0; }

		for (const FSkelMeshRenderSection& Section : LODData.RenderSections)
		{
			const int32 StartTriangle = Section.BaseIndex / 3;
			const int32 EndTriangle = StartTriangle + Section.NumTriangles;
			for (int32 TriIdx = StartTriangle; TriIdx < EndTriangle && TriIdx < NumTriangles; ++TriIdx)
			{
				MaterialIndices[TriIdx] = Section.MaterialIndex;
			}
		}

		UE_LOG(LogFleshRingSubdivisionProcessor, Log,
			TEXT("SetSourceMeshWithBoneInfo: Extracted material indices for %d triangles (%d sections)"),
			NumTriangles, LODData.RenderSections.Num());
	}

	// Extract bone info (SkinWeightVertexBuffer)
	// Create per-vertex section index map (for local to global bone index conversion)
	TArray<int32> VertexToSectionIndex;
	VertexToSectionIndex.SetNum(NumVertices);
	for (int32& SectionIdx : VertexToSectionIndex) { SectionIdx = INDEX_NONE; }

	for (int32 SectionIdx = 0; SectionIdx < LODData.RenderSections.Num(); ++SectionIdx)
	{
		const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIdx];
		const int32 StartIndex = Section.BaseIndex;
		const int32 EndIndex = StartIndex + Section.NumTriangles * 3;
		for (int32 IdxPos = StartIndex; IdxPos < EndIndex && IdxPos < Indices.Num(); ++IdxPos)
		{
			uint32 VertexIdx = Indices[IdxPos];
			if (VertexIdx < NumVertices && VertexToSectionIndex[VertexIdx] == INDEX_NONE)
			{
				VertexToSectionIndex[VertexIdx] = SectionIdx;
			}
		}
	}

	VertexBoneInfluences.SetNum(NumVertices);
	const FSkinWeightVertexBuffer* SkinWeightBuffer = LODData.GetSkinWeightVertexBuffer();

	if (SkinWeightBuffer && SkinWeightBuffer->GetNumVertices() > 0)
	{
		const int32 MaxInfluences = FMath::Min((int32)SkinWeightBuffer->GetMaxBoneInfluences(), FVertexBoneInfluence::MAX_INFLUENCES);

		for (uint32 VertIdx = 0; VertIdx < NumVertices; ++VertIdx)
		{
			FVertexBoneInfluence& Influence = VertexBoneInfluences[VertIdx];

			// Initialize
			FMemory::Memzero(Influence.BoneIndices, sizeof(Influence.BoneIndices));
			FMemory::Memzero(Influence.BoneWeights, sizeof(Influence.BoneWeights));

			// Get section BoneMap for this vertex
			int32 SectionIdx = VertexToSectionIndex[VertIdx];
			const TArray<FBoneIndexType>* BoneMap = nullptr;
			if (SectionIdx != INDEX_NONE && SectionIdx < LODData.RenderSections.Num())
			{
				BoneMap = &LODData.RenderSections[SectionIdx].BoneMap;
			}

			// Read from SkinWeightBuffer and convert to global bone index
			for (int32 InfluenceIdx = 0; InfluenceIdx < MaxInfluences; ++InfluenceIdx)
			{
				uint16 LocalBoneIdx = SkinWeightBuffer->GetBoneIndex(VertIdx, InfluenceIdx);
				uint8 Weight = SkinWeightBuffer->GetBoneWeight(VertIdx, InfluenceIdx);

				// Local to global bone index conversion
				uint16 GlobalBoneIdx = LocalBoneIdx;
				if (BoneMap && LocalBoneIdx < BoneMap->Num())
				{
					GlobalBoneIdx = (*BoneMap)[LocalBoneIdx];
				}

				Influence.BoneIndices[InfluenceIdx] = GlobalBoneIdx;
				Influence.BoneWeights[InfluenceIdx] = Weight;
			}
		}
	}
	else
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("SetSourceMeshWithBoneInfo: No SkinWeightBuffer available"));
	}

	// Invalidate cache
	InvalidateCache();
	InvalidateBoneRegionCache();

	return SetSourceMesh(Positions, Indices, UVs, MaterialIndices);
}

void FFleshRingSubdivisionProcessor::SetVertexBoneInfluences(const TArray<FVertexBoneInfluence>& InInfluences)
{
	VertexBoneInfluences = InInfluences;
	InvalidateBoneRegionCache();
}

TSet<int32> FFleshRingSubdivisionProcessor::GatherNeighborBones(
	const FReferenceSkeleton& RefSkeleton,
	const TArray<int32>& RingBoneIndices,
	int32 HopCount)
{
	TSet<int32> Result;
	const int32 NumBones = RefSkeleton.GetNum();

	// Add initial bones
	for (int32 BoneIdx : RingBoneIndices)
	{
		if (BoneIdx >= 0 && BoneIdx < NumBones)
		{
			Result.Add(BoneIdx);
		}
	}

	// Expand to neighbor bones via BFS
	for (int32 Hop = 0; Hop < HopCount; ++Hop)
	{
		TSet<int32> NewBones;

		for (int32 BoneIdx : Result)
		{
			// Add parent
			int32 ParentIdx = RefSkeleton.GetParentIndex(BoneIdx);
			if (ParentIdx != INDEX_NONE)
			{
				NewBones.Add(ParentIdx);
			}

			// Add children
			for (int32 i = 0; i < NumBones; ++i)
			{
				if (RefSkeleton.GetParentIndex(i) == BoneIdx)
				{
					NewBones.Add(i);
				}
			}
		}

		Result.Append(NewBones);
	}

	UE_LOG(LogFleshRingSubdivisionProcessor, Log,
		TEXT("GatherNeighborBones: %d ring bones -> %d total bones (HopCount=%d)"),
		RingBoneIndices.Num(), Result.Num(), HopCount);

	return Result;
}

bool FFleshRingSubdivisionProcessor::IsTriangleInBoneRegion(
	int32 V0, int32 V1, int32 V2,
	const TSet<int32>& TargetBones,
	uint8 WeightThreshold) const
{
	// Include if any vertex of the triangle is affected by target bones
	if (V0 < VertexBoneInfluences.Num() && VertexBoneInfluences[V0].IsAffectedByBones(TargetBones, WeightThreshold))
	{
		return true;
	}
	if (V1 < VertexBoneInfluences.Num() && VertexBoneInfluences[V1].IsAffectedByBones(TargetBones, WeightThreshold))
	{
		return true;
	}
	if (V2 < VertexBoneInfluences.Num() && VertexBoneInfluences[V2].IsAffectedByBones(TargetBones, WeightThreshold))
	{
		return true;
	}
	return false;
}

bool FFleshRingSubdivisionProcessor::ProcessBoneRegion(
	FSubdivisionTopologyResult& OutResult,
	const FBoneRegionSubdivisionParams& Params)
{
	// Check cache - Return cached result if parameter hash matches
	const uint32 ParamsHash = Params.GetHash();
	if (bBoneRegionCacheValid && CachedBoneRegionParamsHash == ParamsHash)
	{
		OutResult = BoneRegionCachedResult;
		return true;
	}

	// Validate source data
	if (SourcePositions.Num() == 0 || SourceIndices.Num() == 0)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("ProcessBoneRegion: No source mesh data"));
		return false;
	}

	// Validate bone info
	if (VertexBoneInfluences.Num() == 0)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning,
			TEXT("ProcessBoneRegion: No bone info - falling back to ProcessUniform"));
		return ProcessUniform(OutResult, Params.MaxSubdivisionLevel);
	}

	// Full subdivision if no target bones specified (fallback)
	if (Params.TargetBoneIndices.Num() == 0)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning,
			TEXT("ProcessBoneRegion: No target bones specified - falling back to ProcessUniform"));
		return ProcessUniform(OutResult, Params.MaxSubdivisionLevel);
	}

	OutResult.Reset();
	OutResult.OriginalVertexCount = SourcePositions.Num();
	OutResult.OriginalTriangleCount = SourceIndices.Num() / 3;

	// 1. Collect triangle indices in target region
	TSet<int32> TargetTriangles;
	const int32 NumTriangles = SourceIndices.Num() / 3;

	for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
	{
		int32 V0 = SourceIndices[TriIdx * 3 + 0];
		int32 V1 = SourceIndices[TriIdx * 3 + 1];
		int32 V2 = SourceIndices[TriIdx * 3 + 2];

		if (IsTriangleInBoneRegion(V0, V1, V2, Params.TargetBoneIndices, Params.BoneWeightThreshold))
		{
			TargetTriangles.Add(TriIdx);
		}
	}

	UE_LOG(LogFleshRingSubdivisionProcessor, Log,
		TEXT("ProcessBoneRegion: %d/%d triangles in bone region (%.1f%% reduction)"),
		TargetTriangles.Num(), NumTriangles,
		100.0f * (1.0f - (float)TargetTriangles.Num() / NumTriangles));

	// 2. Build Half-Edge mesh
	TArray<int32> IndicesInt32;
	IndicesInt32.SetNum(SourceIndices.Num());
	for (int32 i = 0; i < SourceIndices.Num(); ++i)
	{
		IndicesInt32[i] = static_cast<int32>(SourceIndices[i]);
	}

	if (!HalfEdgeMesh.BuildFromTriangles(SourcePositions, IndicesInt32, SourceUVs, SourceMaterialIndices))
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("ProcessBoneRegion: Failed to build Half-Edge mesh"));
		return false;
	}

	// 3. Subdivide only target region
	FLEBSubdivision::SubdivideSelectedFaces(
		HalfEdgeMesh,
		TargetTriangles,
		Params.MaxSubdivisionLevel,
		CurrentSettings.MinEdgeLength
	);

	// 4. Extract topology result
	if (!ExtractTopologyResult(OutResult))
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("ProcessBoneRegion: Failed to extract topology result"));
		return false;
	}

	// Save to cache
	BoneRegionCachedResult = OutResult;
	CachedBoneRegionParamsHash = ParamsHash;
	bBoneRegionCacheValid = true;

	UE_LOG(LogFleshRingSubdivisionProcessor, Log,
		TEXT("ProcessBoneRegion: Complete - %d -> %d vertices, %d -> %d triangles (cached)"),
		OutResult.OriginalVertexCount, OutResult.SubdividedVertexCount,
		OutResult.OriginalTriangleCount, OutResult.SubdividedTriangleCount);

	return true;
}
