// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Math/Vector.h"

// Forward declaration for Spatial Hash
class FVertexSpatialHash;

// ============================================================================
// EBulgeDirection - Bulge 방향 모드
// ============================================================================

/**
 * Bulge 변형 방향 결정 방식
 * Auto: SDF 경계 버텍스 분석으로 자동 감지
 * Positive/Negative: 수동으로 +Z/-Z 방향 지정
 */
enum class EBulgeDirection : uint8
{
	/** SDF 경계 버텍스 평균 Z로 자동 감지 */
	Auto = 0,

	/** +Z 방향 (위쪽) 강제 */
	Positive = 1,

	/** -Z 방향 (아래쪽) 강제 */
	Negative = 2
};

// ============================================================================
// FBulgeDirectionDetector - 경계 버텍스 기반 방향 감지
// ============================================================================

/**
 * Ring 메시의 경계 버텍스(edge use count == 1)를 분석하여
 * Bulge 방향을 자동으로 감지하는 유틸리티 클래스
 *
 * 알고리즘:
 * 1. 모든 엣지의 사용 횟수 계산
 * 2. 사용 횟수 == 1인 버텍스 = 경계 버텍스
 * 3. 경계 버텍스들의 평균 Z 위치 계산
 * 4. 평균 Z가 SDF 중심보다 위면 +1, 아래면 -1
 */
class FBulgeDirectionDetector
{
public:
	/**
	 * Ring 메시 경계 버텍스 분석으로 Bulge 방향 감지
	 *
	 * @param Vertices 메시 버텍스 배열 (로컬 스페이스)
	 * @param Indices 메시 인덱스 배열 (삼각형당 3개)
	 * @param SDFCenter SDF 볼륨 중심점 (로컬 스페이스)
	 * @return +1 (위쪽), -1 (아래쪽), 0 (감지 실패)
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

		// 엣지 사용 횟수 카운트 (엣지 = 정렬된 버텍스 쌍)
		TMap<TPair<uint32, uint32>, int32> EdgeUseCounts;

		// 모든 삼각형의 엣지 순회
		const int32 NumTriangles = Indices.Num() / 3;
		for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
		{
			uint32 Idx0 = Indices[TriIdx * 3 + 0];
			uint32 Idx1 = Indices[TriIdx * 3 + 1];
			uint32 Idx2 = Indices[TriIdx * 3 + 2];

			// 3개의 엣지 추가 (항상 작은 인덱스가 먼저)
			auto AddEdge = [&EdgeUseCounts](uint32 A, uint32 B)
			{
				TPair<uint32, uint32> Edge(FMath::Min(A, B), FMath::Max(A, B));
				EdgeUseCounts.FindOrAdd(Edge)++;
			};

			AddEdge(Idx0, Idx1);
			AddEdge(Idx1, Idx2);
			AddEdge(Idx2, Idx0);
		}

		// 경계 엣지(사용횟수 == 1)에 속한 버텍스 수집
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
			// 경계 없음 (폐쇄 메시) - 양방향 Bulge
			UE_LOG(LogTemp, Log, TEXT("FBulgeDirectionDetector: No boundary edges (closed mesh) - returning 0 for bidirectional"));
			return 0;
		}

		UE_LOG(LogTemp, Log, TEXT("FBulgeDirectionDetector: Found %d boundary vertices"), BoundaryVertexSet.Num());

		// 경계 버텍스들의 평균 Z 계산
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

		// 경계 버텍스가 중심 근처에 있으면 → Torus 이음새 패턴 → 양방향
		const float Tolerance = 0.1f;  // 허용 오차
		if (FMath::Abs(AverageZ - SDFCenter.Z) < Tolerance)
		{
			UE_LOG(LogTemp, Log, TEXT("FBulgeDirectionDetector: Boundary at center (AverageZ=%.2f ≈ SDFCenter.Z=%.2f) - Torus seam pattern, returning 0 for bidirectional"),
				AverageZ, SDFCenter.Z);
			return 0;  // 양방향
		}

		// SDF 중심 Z와 비교
		int32 Result = (AverageZ > SDFCenter.Z) ? 1 : -1;
		UE_LOG(LogTemp, Log, TEXT("FBulgeDirectionDetector: AverageZ=%.2f, SDFCenter.Z=%.2f, Result=%d"),
			AverageZ, SDFCenter.Z, Result);
		return Result;
	}

	/**
	 * EBulgeDirection에 따라 실제 방향값 반환
	 *
	 * @param Mode 방향 모드
	 * @param AutoDetectedDirection Auto 모드일 때 사용할 감지된 방향
	 * @return +1 (위쪽), -1 (아래쪽)
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
// IBulgeRegionProvider - Bulge 영역 계산 인터페이스
// ============================================================================

/**
 * Bulge 영역 계산 인터페이스
 * SDF 기반, Manual 모드 등 다양한 방식 지원
 */
class IBulgeRegionProvider
{
public:
	virtual ~IBulgeRegionProvider() = default;

	/**
	 * Bulge 영역 버텍스 계산
	 * @param AllVertexPositions - 모든 메시 버텍스 위치 (Component Space)
	 * @param SpatialHash - Spatial Hash (O(1) 쿼리, nullptr이면 브루트포스)
	 * @param OutBulgeVertexIndices - 출력: Bulge 영향받는 버텍스 인덱스
	 * @param OutBulgeInfluences - 출력: 각 버텍스의 Bulge 영향도
	 * @param OutBulgeDirections - 출력: Bulge 방향 (GPU에서 계산 시 비어있음)
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
 * CPU에서 계산된 Bulge 데이터
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
