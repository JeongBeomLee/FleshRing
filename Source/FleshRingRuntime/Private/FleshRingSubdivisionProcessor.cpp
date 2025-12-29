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
	const TArray<FVector2D>& InUVs)
{
	if (InPositions.Num() == 0 || InIndices.Num() == 0 || InIndices.Num() % 3 != 0)
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("Invalid source mesh data"));
		return false;
	}

	SourcePositions = InPositions;
	SourceIndices = InIndices;
	SourceUVs = InUVs;

	// UV가 없으면 빈 배열로 초기화
	if (SourceUVs.Num() != SourcePositions.Num())
	{
		SourceUVs.SetNumZeroed(SourcePositions.Num());
	}

	InvalidateCache();

	UE_LOG(LogFleshRingSubdivisionProcessor, Log, TEXT("Source mesh set: %d vertices, %d triangles"),
		SourcePositions.Num(), SourceIndices.Num() / 3);

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

void FFleshRingSubdivisionProcessor::SetRingParams(const FSubdivisionRingParams& RingParams)
{
	// 파라미터가 크게 변경되었으면 캐시 무효화
	if (NeedsRecomputation(RingParams))
	{
		InvalidateCache();
	}

	CurrentRingParams = RingParams;
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
	UE_LOG(LogFleshRingSubdivisionProcessor, Log, TEXT("Building Half-Edge mesh..."));

	// int32 배열로 변환 (FHalfEdgeMesh가 int32 사용)
	TArray<int32> IndicesInt32;
	IndicesInt32.SetNum(SourceIndices.Num());
	for (int32 i = 0; i < SourceIndices.Num(); ++i)
	{
		IndicesInt32[i] = static_cast<int32>(SourceIndices[i]);
	}

	if (!HalfEdgeMesh.BuildFromTriangles(SourcePositions, IndicesInt32, SourceUVs))
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("Failed to build Half-Edge mesh"));
		return false;
	}

	UE_LOG(LogFleshRingSubdivisionProcessor, Log, TEXT("Half-Edge mesh built: %d vertices, %d faces, %d half-edges"),
		HalfEdgeMesh.GetVertexCount(), HalfEdgeMesh.GetFaceCount(), HalfEdgeMesh.GetHalfEdgeCount());

	// 2. LEB/Red-Green Refinement 수행
	UE_LOG(LogFleshRingSubdivisionProcessor, Log, TEXT("Performing LEB subdivision..."));

	FTorusParams TorusParams;
	TorusParams.Center = CurrentRingParams.Center;
	TorusParams.Axis = CurrentRingParams.Axis.GetSafeNormal();
	TorusParams.MajorRadius = CurrentRingParams.Radius;
	TorusParams.MinorRadius = CurrentRingParams.Width * 0.5f;
	TorusParams.InfluenceMargin = CurrentRingParams.GetInfluenceRadius();

	int32 FacesAdded = FLEBSubdivision::SubdivideRegion(
		HalfEdgeMesh,
		TorusParams,
		CurrentSettings.MaxSubdivisionLevel,
		CurrentSettings.MinEdgeLength
	);

	UE_LOG(LogFleshRingSubdivisionProcessor, Log, TEXT("LEB subdivision complete: %d faces added, total %d faces"),
		FacesAdded, HalfEdgeMesh.GetFaceCount());

	// 3. 토폴로지 결과 추출
	if (!ExtractTopologyResult(OutResult))
	{
		UE_LOG(LogFleshRingSubdivisionProcessor, Warning, TEXT("Failed to extract topology result"));
		return false;
	}

	// 캐시 저장
	CachedResult = OutResult;
	CachedRingParams = CurrentRingParams;
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

	// Half-Edge 메시의 버텍스를 FSubdivisionVertexData로 변환
	// 원본 버텍스는 그대로, 새 버텍스는 부모 정보 필요

	// 원본 버텍스 수만큼은 그대로 복사
	const int32 OriginalVertexCount = SourcePositions.Num();

	OutResult.VertexData.Reserve(HEVertexCount);

	// 모든 Half-Edge 버텍스에 대해 처리
	for (int32 i = 0; i < HEVertexCount; ++i)
	{
		if (i < OriginalVertexCount)
		{
			// 원본 버텍스
			OutResult.VertexData.Add(FSubdivisionVertexData::CreateOriginal(i));
		}
		else
		{
			// 새로 생성된 버텍스 - 부모 정보 필요
			// Half-Edge 메시에서 이 버텍스가 어떤 엣지의 중점인지 추적해야 함
			// 현재 FHalfEdgeMesh 구조에서는 이 정보가 직접 저장되어 있지 않으므로
			// 위치 기반으로 가장 가까운 엣지의 중점으로 판단

			const FVector& NewVertexPos = HalfEdgeMesh.Vertices[i].Position;

			// 원본 메시의 모든 엣지를 검사하여 가장 가까운 중점 찾기
			float MinDist = FLT_MAX;
			uint32 BestV0 = 0, BestV1 = 0;

			for (int32 TriIdx = 0; TriIdx < SourceIndices.Num(); TriIdx += 3)
			{
				uint32 V0 = SourceIndices[TriIdx + 0];
				uint32 V1 = SourceIndices[TriIdx + 1];
				uint32 V2 = SourceIndices[TriIdx + 2];

				// 3개의 엣지 검사
				TPair<uint32, uint32> Edges[3] = {
					{V0, V1}, {V1, V2}, {V2, V0}
				};

				for (const auto& Edge : Edges)
				{
					FVector Midpoint = (SourcePositions[Edge.Key] + SourcePositions[Edge.Value]) * 0.5f;
					float Dist = FVector::DistSquared(NewVertexPos, Midpoint);

					if (Dist < MinDist)
					{
						MinDist = Dist;
						BestV0 = Edge.Key;
						BestV1 = Edge.Value;
					}
				}
			}

			// Edge midpoint로 간주
			OutResult.VertexData.Add(FSubdivisionVertexData::CreateEdgeMidpoint(BestV0, BestV1));
		}
	}

	// 삼각형 인덱스 추출
	OutResult.Indices.Reserve(HEFaceCount * 3);

	for (int32 FaceIdx = 0; FaceIdx < HEFaceCount; ++FaceIdx)
	{
		int32 V0, V1, V2;
		HalfEdgeMesh.GetFaceVertices(FaceIdx, V0, V1, V2);

		OutResult.Indices.Add(static_cast<uint32>(V0));
		OutResult.Indices.Add(static_cast<uint32>(V1));
		OutResult.Indices.Add(static_cast<uint32>(V2));
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

	// 위치 변화 검사
	float CenterDist = FVector::Dist(CachedRingParams.Center, NewRingParams.Center);
	if (CenterDist > Threshold)
	{
		return true;
	}

	// 반지름 변화 검사
	if (FMath::Abs(CachedRingParams.Radius - NewRingParams.Radius) > Threshold * 0.1f)
	{
		return true;
	}

	// 축 방향 변화 검사
	float AxisDot = FVector::DotProduct(CachedRingParams.Axis.GetSafeNormal(), NewRingParams.Axis.GetSafeNormal());
	if (AxisDot < 0.99f)
	{
		return true;
	}

	return false;
}
