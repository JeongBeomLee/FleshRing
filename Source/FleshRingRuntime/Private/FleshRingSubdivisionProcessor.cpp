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

	// UV가 없으면 빈 배열로 초기화
	if (SourceUVs.Num() != SourcePositions.Num())
	{
		SourceUVs.SetNumZeroed(SourcePositions.Num());
	}

	// MaterialIndices가 없으면 모두 0으로 초기화
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

	// Position 추출
	const uint32 NumVertices = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();
	TArray<FVector> Positions;
	Positions.SetNum(NumVertices);

	for (uint32 i = 0; i < NumVertices; ++i)
	{
		Positions[i] = FVector(LODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i));
	}

	// Index 추출
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

	// UV 추출
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
	// 배열이 변경되었으면 캐시 무효화
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

	// 삼각형 기반 모드 사용 시 버텍스 기반 모드 비활성화
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
	// 하위 호환: 기존 파라미터 초기화 후 단일 Ring 추가
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

	// ★ 메모리 누수 방지: HalfEdgeMesh 데이터도 클리어
	// 재계산 시 새로운 데이터로 다시 빌드됨
	HalfEdgeMesh.Clear();

	// 중간 계산 결과도 클리어
	OriginalToNewVertexMap.Empty();
	EdgeMidpointCache.Empty();
}

bool FFleshRingSubdivisionProcessor::Process(FSubdivisionTopologyResult& OutResult)
{
	// 캐시가 유효하면 캐시 반환
	if (bCacheValid)
	{
		OutResult = CachedResult;
		return true;
	}

	// 소스 데이터 검증
	if (SourcePositions.Num() == 0 || SourceIndices.Num() == 0)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("No source mesh data"));
		return false;
	}

	OutResult.Reset();
	OutResult.OriginalVertexCount = SourcePositions.Num();
	OutResult.OriginalTriangleCount = SourceIndices.Num() / 3;

	// 1. Half-Edge 메시 구축
	// int32 배열로 변환 (FHalfEdgeMesh가 int32 사용)
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

	// 2. LEB/Red-Green Refinement 수행
	int32 TotalFacesAdded = 0;

	if (bUseTriangleBasedMode && TargetTriangleIndices.Num() > 0)
	{
		// ========================================
		// 삼각형 기반 모드: DI에서 추출한 삼각형 집합 직접 사용
		// ========================================
		UE_LOG(LogFleshRingSubdivisionProcessor, Log,
			TEXT("Process: Using triangle-based mode with %d target triangles"),
			TargetTriangleIndices.Num());

		const int32 NumTriangles = SourceIndices.Num() / 3;
		UE_LOG(LogFleshRingSubdivisionProcessor, Log,
			TEXT("Process: %d/%d triangles selected for subdivision (%.1f%%)"),
			TargetTriangleIndices.Num(), NumTriangles,
			100.0f * (float)TargetTriangleIndices.Num() / NumTriangles);

		// 대상 삼각형만 subdivision (직접 전달)
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
		// 버텍스 기반 모드: 지정된 버텍스를 포함하는 삼각형 subdivision
		// ========================================
		UE_LOG(LogFleshRingSubdivisionProcessor, Log,
			TEXT("Process: Using vertex-based mode with %d target vertices"),
			TargetVertexIndices.Num());

		// 대상 버텍스를 포함하는 삼각형 수집
		TSet<int32> TargetTrianglesLocal;
		const int32 NumTriangles = SourceIndices.Num() / 3;

		for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
		{
			uint32 V0 = SourceIndices[TriIdx * 3 + 0];
			uint32 V1 = SourceIndices[TriIdx * 3 + 1];
			uint32 V2 = SourceIndices[TriIdx * 3 + 2];

			// 삼각형의 버텍스 중 하나라도 대상 집합에 포함되면 subdivision 대상
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

		// 대상 삼각형만 subdivision
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
		// Ring 파라미터 기반 모드 (기존 방식)
		// ========================================
		for (int32 RingIdx = 0; RingIdx < RingParamsArray.Num(); ++RingIdx)
		{
			const FSubdivisionRingParams& CurrentRingParams = RingParamsArray[RingIdx];
			int32 FacesAdded = 0;

			if (CurrentRingParams.bUseSDFBounds)
			{
				// SDF 모드: OBB 기반 영역 검사 (정확한 방식)
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
				// Manual 모드: 기존 Torus 방식 (Legacy)
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

	// 3. 토폴로지 결과 추출
	if (!ExtractTopologyResult(OutResult))
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("Failed to extract topology result"));
		return false;
	}

	// 캐시 저장
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
	// 단순화된 부모 버텍스 추출: HalfEdgeMesh에서 직접 읽기 - O(N)
	// (Subdivision 시점에 이미 기록됨)
	// ========================================================================

	// 1단계: HalfEdgeMesh에서 직접 부모 정보 읽기
	TArray<TPair<int32, int32>> ImmediateParents;
	ImmediateParents.SetNum(HEVertexCount);

	for (int32 i = 0; i < HEVertexCount; ++i)
	{
		const FHalfEdgeVertex& Vert = HalfEdgeMesh.Vertices[i];

		if (Vert.IsOriginalVertex())
		{
			// 원본 버텍스: 자기 자신이 부모
			ImmediateParents[i] = TPair<int32, int32>(i, i);
		}
		else
		{
			// 서브디비전 버텍스: 저장된 부모 정보 사용
			ImmediateParents[i] = TPair<int32, int32>(Vert.ParentIndex0, Vert.ParentIndex1);
		}
	}

	UE_LOG(LogFleshRingSubdivisionProcessor, Log,
		TEXT("Parent extraction: %d original, %d subdivided vertices (direct read from HalfEdgeMesh)"),
		OriginalVertexCount, HEVertexCount - OriginalVertexCount);

	// ========================================================================
	// 검증: 부모 인덱스가 유효한지 확인
	// ========================================================================
	int32 InvalidParentCount = 0;
	int32 OutOfOrderCount = 0;
	for (int32 i = OriginalVertexCount; i < HEVertexCount; ++i)
	{
		const TPair<int32, int32>& Parents = ImmediateParents[i];
		int32 P0 = Parents.Key;
		int32 P1 = Parents.Value;

		// 부모 인덱스가 유효 범위인지 확인
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
		// 부모가 자신보다 나중에 생성됐는지 확인 (토폴로지 순서 위반)
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

	// 2단계: 재귀적으로 원본 버텍스까지 추적하여 최종 기여도 계산
	TArray<TMap<uint32, float>> OriginalContributions;
	OriginalContributions.SetNum(HEVertexCount);

	// 원본 버텍스 초기화
	for (int32 i = 0; i < OriginalVertexCount; ++i)
	{
		OriginalContributions[i].Add(i, 1.0f);
	}

	// 서브디비전 버텍스의 기여도를 재귀적으로 계산
	int32 FallbackCount = 0;
	for (int32 i = OriginalVertexCount; i < HEVertexCount; ++i)
	{
		const TPair<int32, int32>& Parents = ImmediateParents[i];
		int32 P0 = Parents.Key;
		int32 P1 = Parents.Value;

		// 안전 체크
		if (P0 < 0 || P0 >= i || P1 < 0 || P1 >= i)
		{
			// ★ 경고: 이 fallback이 발생하면 본웨이트가 vertex 0의 것으로 설정됨!
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

		// 각 부모의 기여도를 0.5씩 상속
		for (const auto& Contrib : OriginalContributions[P0])
		{
			OriginalContributions[i].FindOrAdd(Contrib.Key) += Contrib.Value * 0.5f;
		}
		for (const auto& Contrib : OriginalContributions[P1])
		{
			OriginalContributions[i].FindOrAdd(Contrib.Key) += Contrib.Value * 0.5f;
		}
	}

	// ★ Fallback 발생 수 출력 (0이어야 정상)
	if (FallbackCount > 0)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Error,
			TEXT("CRITICAL: %d vertices fell back to vertex 0's bone weights! Animation will break!"),
			FallbackCount);
	}

	// 검증: 기여도 합계가 1.0인지 확인
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
				// 기여도의 키(원본 버텍스 인덱스)가 유효 범위인지 확인
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

	// 3단계: 기여도를 FSubdivisionVertexData로 변환 (최대 3개 원본 버텍스)
	for (int32 i = 0; i < HEVertexCount; ++i)
	{
		if (i < OriginalVertexCount)
		{
			OutResult.VertexData.Add(FSubdivisionVertexData::CreateOriginal(i));
		}
		else
		{
			const TMap<uint32, float>& Contribs = OriginalContributions[i];

			// 기여도 순으로 정렬
			TArray<TPair<uint32, float>> SortedContribs;
			for (const auto& C : Contribs)
			{
				SortedContribs.Add(TPair<uint32, float>(C.Key, C.Value));
			}
			SortedContribs.Sort([](const TPair<uint32, float>& A, const TPair<uint32, float>& B)
			{
				return A.Value > B.Value;
			});

			// 상위 3개 사용
			uint32 P0 = 0, P1 = 0, P2 = 0;
			float W0 = 0, W1 = 0, W2 = 0;

			if (SortedContribs.Num() >= 1) { P0 = SortedContribs[0].Key; W0 = SortedContribs[0].Value; }
			if (SortedContribs.Num() >= 2) { P1 = SortedContribs[1].Key; W1 = SortedContribs[1].Value; }
			if (SortedContribs.Num() >= 3) { P2 = SortedContribs[2].Key; W2 = SortedContribs[2].Value; }

			// 정규화
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

	// 삼각형 인덱스 및 머티리얼 인덱스 추출
	OutResult.Indices.Reserve(HEFaceCount * 3);
	OutResult.TriangleMaterialIndices.Reserve(HEFaceCount);

	for (int32 FaceIdx = 0; FaceIdx < HEFaceCount; ++FaceIdx)
	{
		int32 V0, V1, V2;
		HalfEdgeMesh.GetFaceVertices(FaceIdx, V0, V1, V2);

		OutResult.Indices.Add(static_cast<uint32>(V0));
		OutResult.Indices.Add(static_cast<uint32>(V1));
		OutResult.Indices.Add(static_cast<uint32>(V2));

		// 머티리얼 인덱스 (subdivision 과정에서 상속됨)
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

	// 캐시된 배열에서 동일한 파라미터를 찾아 비교
	// (SetRingParamsArray에서 인덱스 순서대로 비교할 때 사용)
	for (const FSubdivisionRingParams& CachedRingParams : CachedRingParamsArray)
	{
		// 모드가 다르면 건너뛰기
		if (CachedRingParams.bUseSDFBounds != NewRingParams.bUseSDFBounds)
		{
			continue;
		}

		bool bMatches = false;

		if (NewRingParams.bUseSDFBounds)
		{
			// SDF 모드: 바운드 변화 검사
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
			// Manual 모드: 기존 방식
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
			return false; // 캐시에 매칭되는 파라미터 있음 - 재계산 불필요
		}
	}

	return true; // 캐시에 매칭되는 파라미터 없음 - 재계산 필요
}

bool FFleshRingSubdivisionProcessor::ProcessUniform(FSubdivisionTopologyResult& OutResult, int32 MaxLevel)
{
	// 소스 데이터 검증
	if (SourcePositions.Num() == 0 || SourceIndices.Num() == 0)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("ProcessUniform: No source mesh data"));
		return false;
	}

	OutResult.Reset();
	OutResult.OriginalVertexCount = SourcePositions.Num();
	OutResult.OriginalTriangleCount = SourceIndices.Num() / 3;

	// 1. Half-Edge 메시 구축
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

	// 2. 균일 Subdivision 수행
	FLEBSubdivision::SubdivideUniform(
		HalfEdgeMesh,
		MaxLevel,
		CurrentSettings.MinEdgeLength
	);

	// 3. 토폴로지 결과 추출
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
// 본 영역 기반 Subdivision (에디터 프리뷰 최적화)
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

	// Position 추출
	TArray<FVector> Positions;
	Positions.SetNum(NumVertices);
	for (uint32 i = 0; i < NumVertices; ++i)
	{
		Positions[i] = FVector(LODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(i));
	}

	// Index 추출
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

	// UV 추출
	TArray<FVector2D> UVs;
	if (LODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetNumVertices() > 0)
	{
		UVs.SetNum(NumVertices);
		for (uint32 i = 0; i < NumVertices; ++i)
		{
			UVs[i] = FVector2D(LODData.StaticVertexBuffers.StaticMeshVertexBuffer.GetVertexUV(i, 0));
		}
	}

	// ★ 섹션별 머티리얼 인덱스 추출
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

	// ★ 본 정보 추출 (SkinWeightVertexBuffer)
	// 버텍스별 섹션 인덱스 맵 생성 (로컬→글로벌 본 인덱스 변환용)
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

			// 초기화
			FMemory::Memzero(Influence.BoneIndices, sizeof(Influence.BoneIndices));
			FMemory::Memzero(Influence.BoneWeights, sizeof(Influence.BoneWeights));

			// 이 버텍스의 섹션 BoneMap 획득
			int32 SectionIdx = VertexToSectionIndex[VertIdx];
			const TArray<FBoneIndexType>* BoneMap = nullptr;
			if (SectionIdx != INDEX_NONE && SectionIdx < LODData.RenderSections.Num())
			{
				BoneMap = &LODData.RenderSections[SectionIdx].BoneMap;
			}

			// SkinWeightBuffer에서 읽고 글로벌 본 인덱스로 변환
			for (int32 InfluenceIdx = 0; InfluenceIdx < MaxInfluences; ++InfluenceIdx)
			{
				uint16 LocalBoneIdx = SkinWeightBuffer->GetBoneIndex(VertIdx, InfluenceIdx);
				uint8 Weight = SkinWeightBuffer->GetBoneWeight(VertIdx, InfluenceIdx);

				// ★ 로컬 → 글로벌 본 인덱스 변환
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

	// 캐시 무효화
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

	// 초기 본 추가
	for (int32 BoneIdx : RingBoneIndices)
	{
		if (BoneIdx >= 0 && BoneIdx < NumBones)
		{
			Result.Add(BoneIdx);
		}
	}

	// BFS로 이웃 본 확장
	for (int32 Hop = 0; Hop < HopCount; ++Hop)
	{
		TSet<int32> NewBones;

		for (int32 BoneIdx : Result)
		{
			// 부모 추가
			int32 ParentIdx = RefSkeleton.GetParentIndex(BoneIdx);
			if (ParentIdx != INDEX_NONE)
			{
				NewBones.Add(ParentIdx);
			}

			// 자식 추가
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
	// 삼각형의 버텍스 중 하나라도 대상 본에 영향받으면 포함
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
	// ★ 캐시 확인 - 파라미터 해시가 동일하면 캐시된 결과 반환
	const uint32 ParamsHash = Params.GetHash();
	if (bBoneRegionCacheValid && CachedBoneRegionParamsHash == ParamsHash)
	{
		OutResult = BoneRegionCachedResult;
		return true;
	}

	// 소스 데이터 검증
	if (SourcePositions.Num() == 0 || SourceIndices.Num() == 0)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("ProcessBoneRegion: No source mesh data"));
		return false;
	}

	// 본 정보 검증
	if (VertexBoneInfluences.Num() == 0)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning,
			TEXT("ProcessBoneRegion: No bone info - falling back to ProcessUniform"));
		return ProcessUniform(OutResult, Params.MaxSubdivisionLevel);
	}

	// 대상 본이 없으면 전체 subdivision (fallback)
	if (Params.TargetBoneIndices.Num() == 0)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning,
			TEXT("ProcessBoneRegion: No target bones specified - falling back to ProcessUniform"));
		return ProcessUniform(OutResult, Params.MaxSubdivisionLevel);
	}

	OutResult.Reset();
	OutResult.OriginalVertexCount = SourcePositions.Num();
	OutResult.OriginalTriangleCount = SourceIndices.Num() / 3;

	// 1. 대상 영역의 삼각형 인덱스 수집
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

	// 2. Half-Edge 메시 구축
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

	// 3. 대상 영역만 subdivision
	FLEBSubdivision::SubdivideSelectedFaces(
		HalfEdgeMesh,
		TargetTriangles,
		Params.MaxSubdivisionLevel,
		CurrentSettings.MinEdgeLength
	);

	// 4. 토폴로지 결과 추출
	if (!ExtractTopologyResult(OutResult))
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("ProcessBoneRegion: Failed to extract topology result"));
		return false;
	}

	// ★ 캐시 저장
	BoneRegionCachedResult = OutResult;
	CachedBoneRegionParamsHash = ParamsHash;
	bBoneRegionCacheValid = true;

	UE_LOG(LogFleshRingSubdivisionProcessor, Log,
		TEXT("ProcessBoneRegion: Complete - %d -> %d vertices, %d -> %d triangles (cached)"),
		OutResult.OriginalVertexCount, OutResult.SubdividedVertexCount,
		OutResult.OriginalTriangleCount, OutResult.SubdividedTriangleCount);

	return true;
}
