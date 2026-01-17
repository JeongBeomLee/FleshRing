// FleshRingSubdivisionProcessor.h
// CPU-side subdivision topology processor using Red-Green Refinement / LEB
// GPU는 최종 버텍스 보간만 담당

#pragma once

#include "CoreMinimal.h"
#include "HalfEdgeMesh.h"

// Forward declarations
class USkeletalMesh;
struct FReferenceSkeleton;

/**
 * 버텍스별 본 영향 정보
 * SkinWeightVertexBuffer에서 추출한 데이터
 */
struct FVertexBoneInfluence
{
	static constexpr int32 MAX_INFLUENCES = 8;

	uint16 BoneIndices[MAX_INFLUENCES] = {0};
	uint8 BoneWeights[MAX_INFLUENCES] = {0};  // 0-255 normalized

	/** 특정 본 집합에 유의미한 영향을 받는지 확인 */
	bool IsAffectedByBones(const TSet<int32>& TargetBones, uint8 WeightThreshold = 25) const  // 25 ≈ 10%
	{
		for (int32 i = 0; i < MAX_INFLUENCES; ++i)
		{
			if (BoneWeights[i] >= WeightThreshold && TargetBones.Contains(BoneIndices[i]))
			{
				return true;
			}
		}
		return false;
	}
};

/**
 * 본 영역 기반 Subdivision 파라미터
 * 에디터 프리뷰용 - 링 부착 본의 이웃 본 영역만 subdivision
 */
struct FBoneRegionSubdivisionParams
{
	/** 대상 본 인덱스들 (링 부착 본 + 이웃 본) */
	TSet<int32> TargetBoneIndices;

	/** 본 가중치 임계값 (0-255, 기본 25 ≈ 10%) */
	uint8 BoneWeightThreshold = 25;

	/** 이웃 본 탐색 깊이 (1 = 부모+자식, 2 = 조부모+손자 포함) */
	int32 NeighborHopCount = 1;

	/** subdivision 최대 레벨 */
	int32 MaxSubdivisionLevel = 2;

	/** 파라미터 해시 (캐시 무효화 판단용) */
	uint32 GetHash() const
	{
		uint32 Hash = 0;
		for (int32 BoneIdx : TargetBoneIndices)
		{
			Hash = HashCombine(Hash, GetTypeHash(BoneIdx));
		}
		Hash = HashCombine(Hash, GetTypeHash(BoneWeightThreshold));
		Hash = HashCombine(Hash, GetTypeHash(NeighborHopCount));
		Hash = HashCombine(Hash, GetTypeHash(MaxSubdivisionLevel));
		return Hash;
	}
};

/**
 * 새 버텍스 생성 정보 (GPU로 전달)
 * Barycentric 보간에 필요한 모든 정보 포함
 */
struct FSubdivisionVertexData
{
	// 부모 버텍스 인덱스 (원본 메시 기준)
	// Edge midpoint: ParentV0, ParentV1만 사용 (ParentV2 == ParentV0)
	// Face interior: 3개 모두 사용
	uint32 ParentV0 = 0;
	uint32 ParentV1 = 0;
	uint32 ParentV2 = 0;

	// Barycentric 좌표 (u + v + w = 1)
	// Edge midpoint: (0.5, 0.5, 0)
	// Face center: (0.333, 0.333, 0.333)
	FVector3f BarycentricCoords = FVector3f(1.0f, 0.0f, 0.0f);

	// 원본 버텍스 그대로 복사하는 경우
	bool IsOriginalVertex() const
	{
		return BarycentricCoords.X >= 0.999f && ParentV0 == ParentV1 && ParentV1 == ParentV2;
	}

	// Edge midpoint인 경우
	bool IsEdgeMidpoint() const
	{
		return FMath::IsNearlyEqual(BarycentricCoords.X, 0.5f) &&
			   FMath::IsNearlyEqual(BarycentricCoords.Y, 0.5f) &&
			   FMath::IsNearlyEqual(BarycentricCoords.Z, 0.0f);
	}

	// 원본 버텍스용 생성자
	static FSubdivisionVertexData CreateOriginal(uint32 OriginalIndex)
	{
		FSubdivisionVertexData Data;
		Data.ParentV0 = OriginalIndex;
		Data.ParentV1 = OriginalIndex;
		Data.ParentV2 = OriginalIndex;
		Data.BarycentricCoords = FVector3f(1.0f, 0.0f, 0.0f);
		return Data;
	}

	// Edge midpoint용 생성자
	static FSubdivisionVertexData CreateEdgeMidpoint(uint32 V0, uint32 V1)
	{
		FSubdivisionVertexData Data;
		Data.ParentV0 = V0;
		Data.ParentV1 = V1;
		Data.ParentV2 = V0; // unused but set for consistency
		Data.BarycentricCoords = FVector3f(0.5f, 0.5f, 0.0f);
		return Data;
	}

	// Face center용 생성자
	static FSubdivisionVertexData CreateFaceCenter(uint32 V0, uint32 V1, uint32 V2)
	{
		FSubdivisionVertexData Data;
		Data.ParentV0 = V0;
		Data.ParentV1 = V1;
		Data.ParentV2 = V2;
		Data.BarycentricCoords = FVector3f(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f);
		return Data;
	}

	// 임의의 Barycentric 좌표
	static FSubdivisionVertexData CreateBarycentric(uint32 V0, uint32 V1, uint32 V2, const FVector3f& Bary)
	{
		FSubdivisionVertexData Data;
		Data.ParentV0 = V0;
		Data.ParentV1 = V1;
		Data.ParentV2 = V2;
		Data.BarycentricCoords = Bary;
		return Data;
	}
};

/**
 * Subdivision 결과 (CPU → GPU 전달용)
 */
struct FSubdivisionTopologyResult
{
	// 새 버텍스 생성 정보 배열
	TArray<FSubdivisionVertexData> VertexData;

	// 최종 삼각형 인덱스 (새 버텍스 인덱스 기준)
	TArray<uint32> Indices;

	// 삼각형별 머티리얼 인덱스 (섹션 추적용)
	TArray<int32> TriangleMaterialIndices;

	// 통계
	uint32 OriginalVertexCount = 0;
	uint32 OriginalTriangleCount = 0;
	uint32 SubdividedVertexCount = 0;
	uint32 SubdividedTriangleCount = 0;

	void Reset()
	{
		VertexData.Empty();
		Indices.Empty();
		TriangleMaterialIndices.Empty();
		OriginalVertexCount = 0;
		OriginalTriangleCount = 0;
		SubdividedVertexCount = 0;
		SubdividedTriangleCount = 0;
	}

	bool IsValid() const
	{
		return VertexData.Num() > 0 && Indices.Num() > 0;
	}
};

/**
 * Ring 영향 파라미터
 */
struct FSubdivisionRingParams
{
	/** SDF 기반 모드 (true) vs 수동 기하학 모드 (false) */
	bool bUseSDFBounds = false;

	// =====================================
	// Manual 모드용 파라미터
	// =====================================
	FVector Center = FVector::ZeroVector;
	FVector Axis = FVector::UpVector;
	float Radius = 10.0f;
	float Width = 5.0f;
	float InfluenceMultiplier = 2.0f;  // Width 기준 영향 범위 배율

	// =====================================
	// SDF 모드용 파라미터 (OBB 바운드)
	// =====================================
	/** SDF 볼륨 최소 바운드 (Ring 로컬 스페이스) */
	FVector SDFBoundsMin = FVector::ZeroVector;

	/** SDF 볼륨 최대 바운드 (Ring 로컬 스페이스) */
	FVector SDFBoundsMax = FVector::ZeroVector;

	/** Ring 로컬 → 컴포넌트 스페이스 트랜스폼 (OBB) */
	FTransform SDFLocalToComponent = FTransform::Identity;

	/** SDF 영향 범위 확장 배율 */
	float SDFInfluenceMultiplier = 1.5f;

	// 영향 범위 반환 (Manual 모드)
	float GetInfluenceRadius() const
	{
		return Width * InfluenceMultiplier;
	}

	// SDF 바운드 기반 영향 검사 (버텍스가 영향 범위 내인지)
	bool IsVertexInSDFInfluence(const FVector& VertexPosition) const
	{
		if (!bUseSDFBounds)
		{
			return false;
		}

		// 컴포넌트 스페이스 → 로컬 스페이스로 변환
		FVector LocalPos = SDFLocalToComponent.InverseTransformPosition(VertexPosition);

		// 확장된 바운드 계산
		FVector ExpandedMin = SDFBoundsMin * SDFInfluenceMultiplier;
		FVector ExpandedMax = SDFBoundsMax * SDFInfluenceMultiplier;

		// 바운드 내인지 확인
		return LocalPos.X >= ExpandedMin.X && LocalPos.X <= ExpandedMax.X &&
			   LocalPos.Y >= ExpandedMin.Y && LocalPos.Y <= ExpandedMax.Y &&
			   LocalPos.Z >= ExpandedMin.Z && LocalPos.Z <= ExpandedMax.Z;
	}
};

/**
 * Subdivision 프로세서 설정
 */
struct FSubdivisionProcessorSettings
{
	// LEB 최대 레벨
	int32 MaxSubdivisionLevel = 4;

	// 최소 엣지 길이 (이보다 작으면 subdivision 중단)
	float MinEdgeLength = 1.0f;

	// Subdivision 모드
	enum class EMode : uint8
	{
		// Bind Pose에서 1회 계산, 캐싱
		BindPoseFixed,

		// Ring 변경 시 비동기 재계산
		DynamicAsync,

		// 넓은 영역 미리 subdivision
		PreSubdivideRegion
	};
	EMode Mode = EMode::BindPoseFixed;

	// PreSubdivideRegion 모드용: 미리 subdivision할 추가 반경
	float PreSubdivideMargin = 50.0f;
};

/**
 * CPU 기반 Subdivision 토폴로지 프로세서
 *
 * 기존 FHalfEdgeMesh와 FLEBSubdivision을 활용하여
 * Red-Green Refinement 기반 crack-free adaptive subdivision 수행
 *
 * GPU는 최종 버텍스 보간만 담당
 */
class FLESHRINGRUNTIME_API FFleshRingSubdivisionProcessor
{
public:
	FFleshRingSubdivisionProcessor();
	~FFleshRingSubdivisionProcessor();

	/**
	 * 소스 메시 데이터 설정
	 *
	 * @param InPositions - 버텍스 위치 배열
	 * @param InIndices - 삼각형 인덱스 배열
	 * @param InUVs - UV 좌표 배열 (optional)
	 * @param InMaterialIndices - 삼각형별 머티리얼 인덱스 (optional)
	 * @return 성공 여부
	 */
	bool SetSourceMesh(
		const TArray<FVector>& InPositions,
		const TArray<uint32>& InIndices,
		const TArray<FVector2D>& InUVs = TArray<FVector2D>(),
		const TArray<int32>& InMaterialIndices = TArray<int32>());

	/**
	 * SkeletalMesh LOD에서 소스 메시 추출
	 *
	 * @param SkeletalMesh - 소스 스켈레탈 메시
	 * @param LODIndex - LOD 인덱스
	 * @return 성공 여부
	 */
	bool SetSourceMeshFromSkeletalMesh(
		class USkeletalMesh* SkeletalMesh,
		int32 LODIndex = 0);

	/**
	 * Ring 파라미터 배열 설정 (기존 파라미터 교체)
	 *
	 * @param InRingParamsArray - Ring 영향 파라미터 배열
	 */
	void SetRingParamsArray(const TArray<FSubdivisionRingParams>& InRingParamsArray);

	/**
	 * Ring 파라미터 추가
	 *
	 * @param RingParams - 추가할 Ring 영향 파라미터
	 */
	void AddRingParams(const FSubdivisionRingParams& RingParams);

	/**
	 * Ring 파라미터 초기화
	 */
	void ClearRingParams();

	/**
	 * Subdivision 대상 버텍스 인덱스 설정 (버텍스 기반 모드)
	 *
	 * 이 함수가 호출되면 Ring 파라미터 대신 버텍스 집합을 기반으로 subdivision 수행
	 * 해당 버텍스를 포함하는 삼각형들이 subdivision 대상이 됨
	 *
	 * @param InTargetVertexIndices - subdivision 대상 버텍스 인덱스 집합
	 */
	void SetTargetVertexIndices(const TSet<uint32>& InTargetVertexIndices);

	/**
	 * 버텍스 기반 모드 활성화 여부
	 */
	bool IsVertexBasedMode() const { return bUseVertexBasedMode; }

	/**
	 * 버텍스 기반 모드 해제 (Ring 파라미터 모드로 복귀)
	 */
	void ClearTargetVertexIndices();

	/**
	 * Subdivision 대상 삼각형 인덱스 설정 (삼각형 기반 모드)
	 *
	 * 이 함수가 호출되면 Ring 파라미터나 버텍스 집합 대신 삼각형 집합을 기반으로 subdivision 수행
	 * DI에서 추출한 AffectedVertices 위치를 삼각형으로 변환한 후 사용
	 *
	 * @param InTargetTriangleIndices - subdivision 대상 삼각형 인덱스 집합
	 */
	void SetTargetTriangleIndices(const TSet<int32>& InTargetTriangleIndices);

	/**
	 * 삼각형 기반 모드 활성화 여부
	 */
	bool IsTriangleBasedMode() const { return bUseTriangleBasedMode; }

	/**
	 * 삼각형 기반 모드 해제
	 */
	void ClearTargetTriangleIndices();

	/**
	 * 단일 Ring 파라미터 설정 (하위 호환용 - 기존 파라미터 초기화 후 추가)
	 *
	 * @param RingParams - Ring 영향 파라미터
	 */
	void SetRingParams(const FSubdivisionRingParams& RingParams);

	/**
	 * 프로세서 설정
	 *
	 * @param Settings - 프로세서 설정
	 */
	void SetSettings(const FSubdivisionProcessorSettings& Settings);

	/**
	 * Subdivision 실행 (동기)
	 *
	 * Half-Edge 구축 → LEB/Red-Green 적용 → 토폴로지 결과 생성
	 * Ring 영역 기반 부분 subdivision (런타임용)
	 *
	 * @param OutResult - 출력 토폴로지 결과
	 * @return 성공 여부
	 */
	bool Process(FSubdivisionTopologyResult& OutResult);

	/**
	 * 균일 Subdivision 실행 (에디터 프리뷰용 - 레거시)
	 *
	 * Ring 영역 검사 없이 전체 메시를 균일하게 subdivision
	 * 에디터에서 링 편집 시 실시간 프리뷰용으로 사용
	 *
	 * @deprecated ProcessBoneRegion 사용 권장 (성능 개선)
	 *
	 * @param OutResult - 출력 토폴로지 결과
	 * @param MaxLevel - 최대 subdivision 레벨 (기본 2)
	 * @return 성공 여부
	 */
	bool ProcessUniform(FSubdivisionTopologyResult& OutResult, int32 MaxLevel = 2);

	/**
	 * 본 영역 기반 Subdivision 실행 (에디터 프리뷰용 - 최적화)
	 *
	 * 링 부착 본의 이웃 본에 영향받는 버텍스 영역만 subdivision
	 * 전체 메시 대비 70-85% 버텍스 수 감소
	 *
	 * @param OutResult - 출력 토폴로지 결과
	 * @param Params - 본 영역 파라미터
	 * @return 성공 여부
	 */
	bool ProcessBoneRegion(FSubdivisionTopologyResult& OutResult, const FBoneRegionSubdivisionParams& Params);

	// =====================================
	// 본 정보 관련 (에디터 프리뷰용)
	// =====================================

	/**
	 * SkeletalMesh에서 본 정보 포함하여 소스 메시 추출
	 *
	 * @param SkeletalMesh - 소스 스켈레탈 메시
	 * @param LODIndex - LOD 인덱스
	 * @return 성공 여부
	 */
	bool SetSourceMeshWithBoneInfo(USkeletalMesh* SkeletalMesh, int32 LODIndex = 0);

	/**
	 * 링 부착 본들의 이웃 본 집합 수집
	 *
	 * @param RefSkeleton - 스켈레톤 레퍼런스
	 * @param RingBoneIndices - 링 부착 본 인덱스 배열
	 * @param HopCount - 탐색 깊이 (1 = 부모+자식)
	 * @return 이웃 본 인덱스 집합
	 */
	static TSet<int32> GatherNeighborBones(
		const FReferenceSkeleton& RefSkeleton,
		const TArray<int32>& RingBoneIndices,
		int32 HopCount = 1);

	/**
	 * 본 영역 캐시 유효성 확인
	 */
	bool IsBoneRegionCacheValid() const { return bBoneRegionCacheValid; }

	/**
	 * 본 영역 캐시 무효화
	 */
	void InvalidateBoneRegionCache() { bBoneRegionCacheValid = false; }

	/**
	 * 버텍스별 본 영향 정보 직접 설정 (중복 추출 방지용)
	 *
	 * GeneratePreviewMesh()에서 이미 추출한 본 정보를 재사용
	 * SetSourceMeshWithBoneInfo() 대신 SetSourceMesh() + SetVertexBoneInfluences() 조합 사용
	 *
	 * @param InInfluences - 버텍스별 본 영향 정보 배열
	 */
	void SetVertexBoneInfluences(const TArray<FVertexBoneInfluence>& InInfluences);

	/**
	 * 본 정보가 로드되어 있는지 확인
	 */
	bool HasBoneInfo() const { return VertexBoneInfluences.Num() > 0; }

	/**
	 * 캐싱된 결과 반환
	 */
	const FSubdivisionTopologyResult& GetCachedResult() const { return CachedResult; }

	/**
	 * 캐시 유효성 확인
	 */
	bool IsCacheValid() const { return bCacheValid; }

	/**
	 * 캐시 무효화
	 * HalfEdgeMesh 데이터도 클리어하여 메모리 해제
	 */
	void InvalidateCache();

	/**
	 * 소스 메시 데이터 접근자 (GPU 업로드용)
	 */
	const TArray<FVector>& GetSourcePositions() const { return SourcePositions; }
	const TArray<uint32>& GetSourceIndices() const { return SourceIndices; }
	const TArray<FVector2D>& GetSourceUVs() const { return SourceUVs; }

	/**
	 * Ring 위치가 충분히 변경되었는지 확인
	 *
	 * @param NewRingParams - 새 Ring 파라미터
	 * @param Threshold - 변경 임계값
	 * @return 재계산 필요 여부
	 */
	bool NeedsRecomputation(const FSubdivisionRingParams& NewRingParams, float Threshold = 5.0f) const;

private:
	// Half-Edge 메시 구조
	FHalfEdgeMesh HalfEdgeMesh;

	// 소스 메시 데이터
	TArray<FVector> SourcePositions;
	TArray<uint32> SourceIndices;
	TArray<FVector2D> SourceUVs;
	TArray<int32> SourceMaterialIndices;  // 삼각형별 머티리얼 인덱스

	// Ring 파라미터 배열 (여러 Ring 지원)
	TArray<FSubdivisionRingParams> RingParamsArray;

	// 버텍스 기반 모드용 데이터
	TSet<uint32> TargetVertexIndices;
	bool bUseVertexBasedMode = false;

	// 삼각형 기반 모드용 데이터
	TSet<int32> TargetTriangleIndices;
	bool bUseTriangleBasedMode = false;

	// 설정
	FSubdivisionProcessorSettings CurrentSettings;

	// 캐시 (런타임용 - Process())
	FSubdivisionTopologyResult CachedResult;
	bool bCacheValid = false;
	TArray<FSubdivisionRingParams> CachedRingParamsArray;

	// 본 영역 캐시 (에디터 프리뷰용 - ProcessBoneRegion())
	FSubdivisionTopologyResult BoneRegionCachedResult;
	bool bBoneRegionCacheValid = false;
	uint32 CachedBoneRegionParamsHash = 0;

	// 버텍스별 본 영향 정보 (SetSourceMeshWithBoneInfo에서 추출)
	TArray<FVertexBoneInfluence> VertexBoneInfluences;

	// Half-Edge 메시에서 토폴로지 결과 추출
	bool ExtractTopologyResult(FSubdivisionTopologyResult& OutResult);

	// 삼각형이 대상 본 영역에 포함되는지 확인
	bool IsTriangleInBoneRegion(int32 V0, int32 V1, int32 V2, const TSet<int32>& TargetBones, uint8 WeightThreshold) const;

	// 원본 버텍스 인덱스 → 새 버텍스 인덱스 매핑
	TMap<uint32, uint32> OriginalToNewVertexMap;

	// Edge midpoint 캐시 (Edge를 Key로, 새 버텍스 인덱스를 Value로)
	TMap<TPair<uint32, uint32>, uint32> EdgeMidpointCache;

	// Edge의 정규화된 키 생성 (V0 < V1 보장)
	static TPair<uint32, uint32> MakeEdgeKey(uint32 V0, uint32 V1)
	{
		return V0 < V1 ? TPair<uint32, uint32>(V0, V1) : TPair<uint32, uint32>(V1, V0);
	}
};
