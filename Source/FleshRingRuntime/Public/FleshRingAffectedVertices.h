// Purpose: Track and manage vertices affected by each Ring for optimized processing
// [FLEXIBLE] This module uses Strategy Pattern for vertex selection
// The selection algorithm can be swapped by implementing IVertexSelector

#pragma once

#include "CoreMinimal.h"
#include "FleshRingTypes.h"

class UFleshRingComponent;
class USkeletalMeshComponent;
struct FRingSDFCache;

/**
 * Single 'affected vertex' data
 * Contains index and influence weight for deformation
 * 영향받는 버텍스 하나
 */
struct FAffectedVertex
{
    /**
     * Original mesh vertex index
     * 메시에서 몇 번째 버텍스인지
     */
    uint32 VertexIndex;

    /**
     * Radial distance from Ring axis (perpendicular distance)
     * 링 축에서 버텍스까지의 수직 거리 (반경 방향)
     */
    float RadialDistance;

    /**
     * Influence weight (0-1) based on falloff calculation
     * 영향도 (0~1), FalloffType으로 계산됨
     */
    float Influence;

    /**
     * Layer type for penetration resolution
     * 침투 해결용 레이어 타입 (Skin, Stocking 등)
     * Material 이름에서 자동 감지되거나 기본값 사용
     */
    EFleshRingLayerType LayerType;

    FAffectedVertex()
        : VertexIndex(0)
        , RadialDistance(0.0f)
        , Influence(0.0f)
        , LayerType(EFleshRingLayerType::Other)
    {
    }

    FAffectedVertex(uint32 InIndex, float InRadialDist, float InInfluence, EFleshRingLayerType InLayerType = EFleshRingLayerType::Other)
        : VertexIndex(InIndex)
        , RadialDistance(InRadialDist)
        , Influence(InInfluence)
        , LayerType(InLayerType)
    {
    }
};

/**
 * Per-Ring affected vertex collection
 * Contains all vertices affected by a single Ring
 * Ring 하나가 영향을 주는 버텍스들의 데이터
 */
struct FRingAffectedData
{
    // =========== Ring Information (Get from Bone) ===========

    /**
     * Bone name this Ring is attached to
     * 링이 부착된 본 이름
     */
    FName BoneName;

    /**
     * Ring center in component space (bind pose)
     * 링 중심 위치 (바인드 포즈 컴포넌트 스페이스)
     */
    FVector RingCenter;

    /**
     * Ring orientation axis (normalized, bone's up vector)
     * 링 축 방향 (본의 Up 벡터, 정규화됨)
     */
    FVector RingAxis;

    // =========== Ring Geometry (Copy from FFleshRingSettings) ===========

    /**
     * Inner radius from bone axis to ring inner surface
     * 본 축에서 링 안쪽 면까지의 거리 (내부 반지름)
     */
    float RingRadius;

    /**
     * Ring wall thickness in radial direction (inner → outer)
     * 링 벽 두께 - 반경 방향 (안쪽→바깥쪽)
     */
    float RingThickness;

    /**
     * Ring height along axis direction (total height, ±RingHeight/2 from center)
     * 링 높이 - 축 방향 전체 높이 (중심에서 위아래 RingHeight/2)
     */
    float RingHeight;

    // =========== Deformation Parameters (Copy from FFleshRingSettings) ===========

    /**
     * Tightness deformation strength
     * 조이기(Tightness) 변형 강도
     */
    float TightnessStrength;

    /**
     * Falloff curve type for influence calculation
     * 영향도 계산용 감쇠 곡선 타입
     */
    EFalloffType FalloffType;

    // =========== Affected Vertices Data ===========

    /**
     * List of affected vertices with influence weights
     * 영향받는 버텍스 목록 (인덱스 + 영향도)
     */
    TArray<FAffectedVertex> Vertices;

    // Vertices에 있지만 별도 평면 배열로 가지고 있는 이유
    // GPU는 구조체 배열을 못읽고, 평면 배열만 읽을 수 있기 때문
    /**
     * GPU buffer: vertex indices (packed for CS dispatch)
     * GPU 버퍼: 버텍스 인덱스 배열 (CS Dispatch용)
     */
    TArray<uint32> PackedIndices;

    /**
     * GPU buffer: influence weights (packed for CS dispatch)
     * GPU 버퍼: 영향도 배열 (CS Dispatch용)
     */
    TArray<float> PackedInfluences;

    /**
     * GPU buffer: layer types (packed for CS dispatch)
     * GPU 버퍼: 레이어 타입 배열 (CS Dispatch용, 침투 해결에서 사용)
     * 0=Skin, 1=Stocking, 2=Underwear, 3=Outerwear, 4=Unknown
     */
    TArray<uint32> PackedLayerTypes;

    // =========== UV Seam Welding Data ===========
    // =========== UV Seam 용접 데이터 ===========
    //
    // [설계]
    // UV seam에서 분리된 버텍스들(같은 위치, 다른 인덱스)이 동일하게 변형되도록 보장
    // 모든 변형 패스(Tightness, Bulge, BoneRatio, Laplacian)에서 사용
    //
    // RepresentativeIndices[ThreadIndex] = 해당 위치 그룹의 대표 버텍스 인덱스
    // 셰이더에서: 대표 버텍스 위치 읽기 → 변형 계산 → 자기 인덱스에 기록

    /**
     * GPU buffer: Representative vertex index for each affected vertex
     * All UV duplicate vertices at the same position share the same representative
     * Shader reads position from representative, computes deformation, writes to own index
     * GPU 버퍼: 각 영향받는 버텍스의 대표 버텍스 인덱스
     * 같은 위치의 모든 UV 중복 버텍스가 동일한 대표를 공유
     * 셰이더: 대표 위치 읽기 → 변형 계산 → 자기 인덱스에 기록
     */
    TArray<uint32> RepresentativeIndices;

    /**
     * GPU buffer: Representative vertex index for PostProcessing vertices
     * Same concept as RepresentativeIndices but for Z-extended region
     * GPU 버퍼: 후처리 버텍스의 대표 버텍스 인덱스
     * RepresentativeIndices와 동일 개념, Z 확장 영역용
     */
    TArray<uint32> PostProcessingRepresentativeIndices;

    // =========== Z-Extended Post-Processing Vertices ===========
    // =========== Z 확장 후처리 버텍스 ===========
    //
    // [설계]
    // - Affected Vertices (Vertices/PackedIndices) = 원본 SDF AABB → Tightness 변형 대상
    // - Post-Processing Vertices = 원본 AABB + BoundsZTop/Bottom → 스무딩/침투해결 등
    //
    // 경계에서 날카로운 크랙 방지를 위해 후처리 패스는 확장된 범위에서 수행

    /**
     * GPU buffer: Post-processing vertex indices (Z-extended range)
     * Includes all vertices in (original AABB + BoundsZTop/Bottom)
     * Used for: Laplacian smoothing, Layer penetration, PBD, etc. (NOT Tightness)
     * GPU 버퍼: 후처리용 버텍스 인덱스 (Z 확장 범위)
     * 원본 AABB + BoundsZTop/Bottom 범위의 모든 버텍스 포함
     * 용도: 라플라시안 스무딩, 레이어 침투 해결, PBD 등 (Tightness 제외)
     */
    TArray<uint32> PostProcessingIndices;

    /**
     * GPU buffer: Post-processing vertex influences
     * 1.0 for vertices in original AABB (core), falloff for Z-extended vertices
     * GPU 버퍼: 후처리용 버텍스 영향도
     * 원본 AABB 내 버텍스는 1.0 (코어), Z 확장 버텍스는 falloff 적용
     */
    TArray<float> PostProcessingInfluences;

    /**
     * GPU buffer: Anchor flags for post-processing vertices
     * 1 = original affected vertex (anchor, skip smoothing)
     * 0 = extended region vertex (apply smoothing)
     * GPU 버퍼: 후처리 버텍스의 앵커 플래그
     * 1 = 원본 영향 버텍스 (앵커, 스무딩 건너뜀)
     * 0 = 확장 영역 버텍스 (스무딩 적용)
     */
    TArray<uint32> PostProcessingIsAnchor;

    // Note: PostProcessingLayerTypes는 CachedVertexLayerTypes/FullMeshLayerTypes로 대체됨 (deprecated/removed)
    // 전체 메시 크기의 lookup 테이블을 직접 사용하여 축소→확대 변환 제거

    /**
     * GPU buffer: Laplacian adjacency data for post-processing vertices
     * Format: [NeighborCount, N0, N1, ..., N11] per vertex (13 uints each)
     * Neighbor indices are raw vertex indices (not thread indices)
     * 후처리 버텍스용 라플라시안 인접 데이터
     * 포맷: 버텍스당 [이웃수, N0, N1, ..., N11] (각 13 uint)
     * 이웃 인덱스는 전역 버텍스 인덱스 (thread index가 아님)
     */
    TArray<uint32> PostProcessingLaplacianAdjacencyData;

    /**
     * PBD adjacency data for PostProcessing vertices
     * 후처리 버텍스용 PBD 인접 데이터
     * Format: [Count, N0, RL0, N1, RL1, ...] per vertex (1 + MAX_NEIGHBORS*2 uints)
     */
    TArray<uint32> PostProcessingPBDAdjacencyWithRestLengths;

    /**
     * Normal adjacency offsets for PostProcessing vertices
     * 후처리 버텍스용 노멀 인접 오프셋
     * Size: NumPostProcessing + 1 (sentinel)
     */
    TArray<uint32> PostProcessingAdjacencyOffsets;

    /**
     * Normal adjacency triangles for PostProcessing vertices
     * 후처리 버텍스용 노멀 인접 삼각형
     */
    TArray<uint32> PostProcessingAdjacencyTriangles;

    // =========== Skin SDF Layer Separation Data ===========
    // =========== 스킨 SDF 레이어 분리용 데이터 ===========

    /**
     * Skin vertex indices (PostProcessing range, LayerType=Skin)
     * 스킨 버텍스 인덱스 (후처리 범위 내)
     */
    TArray<uint32> SkinVertexIndices;

    /**
     * Skin vertex normals (radial direction from ring axis)
     * 스킨 버텍스 노멀 (Ring 축 기준 방사 방향)
     * Packed as: [N0.x, N0.y, N0.z, N1.x, N1.y, N1.z, ...]
     */
    TArray<float> SkinVertexNormals;

    /**
     * Stocking vertex indices (PostProcessing range, LayerType=Stocking)
     * 스타킹 버텍스 인덱스 (후처리 범위 내)
     */
    TArray<uint32> StockingVertexIndices;

    // =========== Adjacency Data for Normal Recomputation ===========
    // =========== 노멀 재계산용 인접 데이터 ===========

    /**
     * GPU buffer: Adjacency offsets for each affected vertex
     * AdjacencyOffsets[i] = start index in AdjacencyTriangles for affected vertex i
     * AdjacencyOffsets[NumAffectedVertices] = total size of AdjacencyTriangles (sentinel)
     * GPU 버퍼: 각 영향받는 버텍스의 인접 삼각형 시작 인덱스
     * AdjacencyOffsets[i]에서 AdjacencyOffsets[i+1] 사이가 해당 버텍스의 인접 삼각형 범위
     */
    TArray<uint32> AdjacencyOffsets;

    /**
     * GPU buffer: Flattened list of adjacent triangle indices
     * GPU 버퍼: 인접 삼각형 인덱스의 평탄화된 리스트
     */
    TArray<uint32> AdjacencyTriangles;

    // =========== Laplacian Smoothing Adjacency Data ===========
    // =========== 라플라시안 스무딩용 인접 데이터 ===========

    /**
     * GPU buffer: Packed adjacency data for Laplacian smoothing
     * Format: [NeighborCount, N0, N1, ..., N11] per affected vertex (13 uints each)
     * GPU 버퍼: 라플라시안 스무딩용 패킹된 인접 데이터
     * 포맷: 영향받는 버텍스당 [이웃수, N0, N1, ..., N11] (각 13 uint)
     */
    TArray<uint32> LaplacianAdjacencyData;

    // =========== Bone Ratio Preserve Data ===========
    // =========== 본 거리 비율 보존용 데이터 ===========

    /** Maximum vertices per slice (for GPU buffer sizing) */
    static constexpr int32 MAX_SLICE_VERTICES = 32;
    /** Packed size per vertex: [Count, V0, V1, ..., V31] = 33 uints */
    static constexpr int32 SLICE_PACKED_SIZE = 1 + MAX_SLICE_VERTICES;

    /**
     * Original bone distance for each affected vertex (bind pose)
     * 각 영향받는 버텍스의 원본 본-버텍스 거리 (바인드 포즈)
     */
    TArray<float> OriginalBoneDistances;

    /**
     * GPU buffer: Packed slice data for bone ratio preservation
     * Format: [SliceVertexCount, V0, V1, ..., V31] per affected vertex (33 uints each)
     * V0~V31 are ThreadIndices (not VertexIndices) of same-slice vertices
     * GPU 버퍼: 본 거리 비율 보존용 슬라이스 데이터
     * 포맷: 영향받는 버텍스당 [슬라이스버텍스수, V0, V1, ..., V31] (각 33 uint)
     * V0~V31은 같은 슬라이스 버텍스들의 ThreadIndex (VertexIndex 아님)
     */
    TArray<uint32> SlicePackedData;

    /**
     * GPU buffer: Axis height for each affected vertex
     * Used for Gaussian weighted averaging (smooth transitions)
     * GPU 버퍼: 영향받는 버텍스의 축 높이
     * 가우시안 가중치 평균 계산에 사용 (부드러운 전환)
     */
    TArray<float> AxisHeights;

    // =========== PBD Edge Constraint Data ===========
    // =========== PBD 에지 제약 데이터 (변형 전파용) ===========

    /** Maximum neighbors per vertex for PBD (must match shader) */
    static constexpr int32 PBD_MAX_NEIGHBORS = 12;
    /** Packed size per vertex: [Count, (Neighbor, RestLen)*12] = 1 + 24 = 25 uints */
    static constexpr int32 PBD_ADJACENCY_PACKED_SIZE = 1 + PBD_MAX_NEIGHBORS * 2;

    /**
     * GPU buffer: Packed adjacency data with rest lengths for PBD
     * Format: [NeighborCount, Neighbor0, RestLen0, Neighbor1, RestLen1, ...] per affected vertex
     * RestLength is stored as bit-cast uint (use asfloat in shader)
     * GPU 버퍼: PBD용 rest length 포함 인접 데이터
     * 포맷: 영향받는 버텍스당 [이웃수, 이웃0, RestLen0, 이웃1, RestLen1, ...]
     * RestLength는 uint로 bit-cast되어 저장 (셰이더에서 asfloat 사용)
     */
    TArray<uint32> PBDAdjacencyWithRestLengths;

    /**
     * GPU buffer: Full mesh influence map (for neighbor weight lookup)
     * Index: absolute vertex index, Value: influence
     * Vertices not in affected set have 0 influence
     * GPU 버퍼: 전체 메시 influence 맵 (이웃 가중치 조회용)
     * 인덱스: 전체 버텍스 인덱스, 값: influence
     * 영향 영역 외의 버텍스는 0
     */
    TArray<float> FullInfluenceMap;

    /**
     * GPU buffer: Full mesh deform amount map (for neighbor weight lookup)
     * Index: absolute vertex index, Value: deform amount
     * Used when bPBDUseDeformAmountWeight is true
     * GPU 버퍼: 전체 메시 deform amount 맵 (이웃 가중치 조회용)
     * 인덱스: 전체 버텍스 인덱스, 값: deform amount
     * bPBDUseDeformAmountWeight가 true일 때 사용
     */
    TArray<float> FullDeformAmountMap;

    // =========== Hop-Based Smoothing Data ===========
    // =========== 홉 기반 스무딩 데이터 ===========

    /**
     * GPU buffer: Hop distance from nearest seed for each affected vertex
     * -1 = not reached (outside smoothing range)
     * 0 = seed vertex (inside SDF, will be deformed)
     * 1+ = hop distance from nearest seed
     * GPU 버퍼: 각 영향받는 버텍스의 Seed로부터 홉 거리
     * -1 = 도달 안 함 (스무딩 범위 밖)
     * 0 = Seed 버텍스 (SDF 내부, 변형될 버텍스)
     * 1+ = 가장 가까운 Seed로부터의 홉 거리
     */
    TArray<int32> HopDistances;

    /**
     * Hop-based influence (calculated from HopDistances)
     * Replaces PackedInfluences when SmoothingVolumeMode == HopBased
     * 홉 기반 influence (HopDistances에서 계산됨)
     * SmoothingVolumeMode가 HopBased일 때 PackedInfluences 대신 사용
     */
    TArray<float> HopBasedInfluences;

    /**
     * Indices of seed vertices (vertices inside SDF that will be deformed)
     * Used as starting points for BFS hop propagation
     * Seed 버텍스들의 ThreadIndex (SDF 내부, 변형될 버텍스)
     * BFS 홉 전파의 시작점으로 사용
     */
    TArray<int32> SeedThreadIndices;

    // =========== Extended Smoothing Region (Hop-Based) ===========
    // =========== 확장된 스무딩 영역 (홉 기반) ===========
    // Seeds(Affected Vertices)에서 N-hop 거리 내의 모든 버텍스
    // LaplacianCS가 이 확장된 영역에 대해 스무딩 적용

    /**
     * Extended smoothing region vertex indices (absolute mesh indices)
     * Includes: Seeds (Affected Vertices) + N-hop reachable vertices
     * 확장된 스무딩 영역 버텍스 인덱스 (전체 메시 기준)
     * 포함: Seeds (Affected Vertices) + N-hop 도달 가능 버텍스
     */
    TArray<uint32> ExtendedSmoothingIndices;

    /**
     * Hop distance for each vertex in ExtendedSmoothingIndices
     * 0 = Seed (original affected vertex)
     * 1+ = hop distance from nearest seed
     * ExtendedSmoothingIndices 각 버텍스의 홉 거리
     * 0 = Seed (원본 affected vertex)
     * 1+ = 가장 가까운 Seed로부터의 홉 거리
     */
    TArray<int32> ExtendedHopDistances;

    /**
     * Influence for each vertex in ExtendedSmoothingIndices
     * Calculated from hop distance with falloff
     * ExtendedSmoothingIndices 각 버텍스의 influence
     * 홉 거리에서 falloff로 계산됨
     */
    TArray<float> ExtendedInfluences;

    /**
     * Anchor flags for each vertex in ExtendedSmoothingIndices
     * 1 = Seed vertex (hop distance 0, original affected) → anchor, skip smoothing
     * 0 = Extended vertex (hop distance > 0) → apply smoothing
     * ExtendedSmoothingIndices 각 버텍스의 앵커 플래그
     * 1 = Seed 버텍스 (홉 거리 0, 원본 affected) → 앵커, 스무딩 건너뜀
     * 0 = 확장 버텍스 (홉 거리 > 0) → 스무딩 적용
     */
    TArray<uint32> ExtendedIsAnchor;

    /**
     * Laplacian adjacency data for extended smoothing region
     * Format: [NeighborCount, N0, N1, ..., N11] per vertex (13 uints each)
     * Neighbor indices are relative to ExtendedSmoothingIndices (thread index)
     * 확장된 스무딩 영역용 라플라시안 인접 데이터
     * 포맷: 버텍스당 [이웃수, N0, N1, ..., N11] (각 13 uint)
     * 이웃 인덱스는 ExtendedSmoothingIndices 기준 (thread index)
     */
    TArray<uint32> ExtendedLaplacianAdjacency;

    /**
     * Representative vertex indices for UV seam welding in extended region
     * ExtendedRepresentativeIndices[ThreadIndex] = representative vertex index for that vertex
     * All UV duplicates at same position share the same representative
     * Used by HeatPropagationCS to ensure UV seam vertices move identically
     * 확장 영역 UV seam 용접용 대표 버텍스 인덱스
     * ExtendedRepresentativeIndices[ThreadIndex] = 해당 버텍스의 대표 버텍스 인덱스
     * 같은 위치의 모든 UV duplicate가 동일한 대표를 공유
     * HeatPropagationCS에서 UV seam 버텍스가 동일하게 이동하도록 보장
     */
    TArray<uint32> ExtendedRepresentativeIndices;

    /**
     * Triangle adjacency data for extended smoothing region (NormalRecomputeCS용)
     * ExtendedAdjacencyOffsets[i] = start index in ExtendedAdjacencyTriangles for vertex i
     * ExtendedAdjacencyOffsets has NumExtended+1 elements (prefix sum format)
     * ExtendedAdjacencyTriangles = flattened list of adjacent triangle indices
     * 확장 스무딩 영역용 삼각형 인접 데이터 (NormalRecomputeCS용)
     * ExtendedAdjacencyOffsets[i] = 버텍스 i의 인접 삼각형 시작 인덱스
     * ExtendedAdjacencyOffsets는 NumExtended+1 개 원소 (누적합 형식)
     * ExtendedAdjacencyTriangles = 인접 삼각형 인덱스의 평탄화된 리스트
     */
    TArray<uint32> ExtendedAdjacencyOffsets;
    TArray<uint32> ExtendedAdjacencyTriangles;

    FRingAffectedData()
        : BoneName(NAME_None)
        , RingCenter(FVector::ZeroVector)
        , RingAxis(FVector::UpVector)
        , RingRadius(5.0f)
        , RingThickness(1.0f)
        , RingHeight(2.0f)
        , TightnessStrength(1.0f)
        , FalloffType(EFalloffType::Linear)
    {
    }

    /** Pack vertex data into GPU-friendly buffers(flat array)
     *  GPU 버퍼가 읽기 편하도록 평면 배열에 정보를 옮기는 함수
     */
    void PackForGPU()
    {
        PackedIndices.Reset(Vertices.Num());
        PackedInfluences.Reset(Vertices.Num());
        PackedLayerTypes.Reset(Vertices.Num());

        for (const FAffectedVertex& Vert : Vertices)
        {
            PackedIndices.Add(Vert.VertexIndex);
            PackedInfluences.Add(Vert.Influence);
            PackedLayerTypes.Add(static_cast<uint32>(Vert.LayerType));
        }
    }
};

// ============================================================================
// FVertexSpatialHash - 버텍스 공간 해시 (O(1) 쿼리)
// ============================================================================
// 버텍스를 3D 그리드 셀에 저장하여 AABB 쿼리를 O(1)로 최적화
// 브루트포스 O(n) 대비 10~100배 성능 향상

class FLESHRINGRUNTIME_API FVertexSpatialHash
{
public:
    FVertexSpatialHash() : CellSize(5.0f), InvCellSize(0.2f) {}

    /**
     * Build spatial hash from vertex array
     * 버텍스 배열로 공간 해시 빌드
     * @param Vertices - Bind pose vertices in component space
     * @param InCellSize - Grid cell size (default 5.0 cm)
     */
    void Build(const TArray<FVector3f>& Vertices, float InCellSize = 5.0f);

    /**
     * Query vertices within AABB
     * AABB 내 버텍스 인덱스 반환
     * @param Min - AABB minimum corner
     * @param Max - AABB maximum corner
     * @param OutIndices - Output vertex indices
     */
    void QueryAABB(const FVector& Min, const FVector& Max, TArray<int32>& OutIndices) const;

    /**
     * Query vertices within OBB (converts to AABB internally, then precise check)
     * OBB 내 버텍스 인덱스 반환 (내부적으로 AABB 변환 후 정밀 체크)
     * @param LocalToWorld - OBB local to world transform
     * @param LocalMin - OBB local minimum corner
     * @param LocalMax - OBB local maximum corner
     * @param OutIndices - Output vertex indices (only those inside OBB)
     */
    void QueryOBB(const FTransform& LocalToWorld, const FVector& LocalMin, const FVector& LocalMax, TArray<int32>& OutIndices) const;

    /** Check if hash is built */
    bool IsBuilt() const { return CellMap.Num() > 0; }

    /** Clear all data */
    void Clear() { CellMap.Empty(); CachedVertices.Empty(); }

private:
    /** Convert world position to cell key */
    FIntVector GetCellKey(const FVector& Position) const
    {
        return FIntVector(
            FMath::FloorToInt(Position.X * InvCellSize),
            FMath::FloorToInt(Position.Y * InvCellSize),
            FMath::FloorToInt(Position.Z * InvCellSize)
        );
    }

    /** Convert FIntVector to hash key */
    uint64 HashCellKey(const FIntVector& Key) const
    {
        // Combine X, Y, Z into a single hash (21 bits each for reasonable range)
        return (static_cast<uint64>(Key.X & 0x1FFFFF)) |
               (static_cast<uint64>(Key.Y & 0x1FFFFF) << 21) |
               (static_cast<uint64>(Key.Z & 0x1FFFFF) << 42);
    }

    float CellSize;
    float InvCellSize;
    TMap<uint64, TArray<int32>> CellMap;  // Cell hash -> vertex indices
    TArray<FVector3f> CachedVertices;     // Cached vertex positions
};

// ============================================================================
// FVertexSelectionContext - 버텍스 선택 컨텍스트
// ============================================================================
// 모든 선택 전략에 필요한 데이터를 담는 컨텍스트
// 각 Selector는 필요한 것만 사용. 확장 시 필드 추가만 하면 됨.

struct FVertexSelectionContext
{
    // ===== Core (Always Present) =====

    /** Ring settings from asset */
    const FFleshRingSettings& RingSettings;

    /** Ring index in the asset's Rings array */
    int32 RingIndex;

    /** Bone transform in bind pose component space */
    const FTransform& BoneTransform;

    /** All mesh vertices in bind pose component space */
    const TArray<FVector3f>& AllVertices;

    // ===== SDF Data (Optional - nullptr if not available) =====

    /** SDF cache for this Ring */
    const FRingSDFCache* SDFCache;

    // ===== Spatial Hash (Optional - nullptr for brute force fallback) =====

    /** Spatial hash for O(1) vertex query */
    const FVertexSpatialHash* SpatialHash;

    // ===== Position-to-Vertices Cache (Optional - nullptr for fallback to local build) =====

    /**
     * Cached position-to-vertices map for UV seam welding
     * UV seam 용접용 캐시된 위치-버텍스 맵
     * nullptr이면 SelectVertices에서 로컬 맵 빌드 (폴백, 느림)
     */
    const TMap<FIntVector, TArray<uint32>>* CachedPositionToVertices;

    // ===== Layer Data (Optional - nullptr if layer filtering disabled) =====

    /**
     * Per-vertex layer types for filtering (full mesh size array)
     * 전체 메시 버텍스의 레이어 타입 배열
     * nullptr이면 레이어 필터링 비활성화 (모든 버텍스 선택)
     */
    const TArray<EFleshRingLayerType>* VertexLayerTypes;

    FVertexSelectionContext(
        const FFleshRingSettings& InRingSettings,
        int32 InRingIndex,
        const FTransform& InBoneTransform,
        const TArray<FVector3f>& InAllVertices,
        const FRingSDFCache* InSDFCache = nullptr,
        const FVertexSpatialHash* InSpatialHash = nullptr,
        const TMap<FIntVector, TArray<uint32>>* InCachedPositionToVertices = nullptr,
        const TArray<EFleshRingLayerType>* InVertexLayerTypes = nullptr)
        : RingSettings(InRingSettings)
        , RingIndex(InRingIndex)
        , BoneTransform(InBoneTransform)
        , AllVertices(InAllVertices)
        , SDFCache(InSDFCache)
        , SpatialHash(InSpatialHash)
        , CachedPositionToVertices(InCachedPositionToVertices)
        , VertexLayerTypes(InVertexLayerTypes)
    {
    }
};

// ============================================================================
// IVertexSelector - 버텍스 선택 전략 인터페이스 (Strategy Pattern)
// ============================================================================

/**
 * Interface for vertex selection strategies
 * 버텍스 선택 전략 인터페이스
 */
class IVertexSelector
{
public:
    virtual ~IVertexSelector() = default;

    /**
     * Select vertices affected by a Ring
     * Context provides all data - each selector uses what it needs
     *
     * @param Context - All selection-related data (Ring, Bone, Vertices, SDF, etc.)
     * @param OutAffected - Output: selected vertices with influence
     */
    virtual void SelectVertices(
        const FVertexSelectionContext& Context,
        TArray<FAffectedVertex>& OutAffected) = 0;

    /** Strategy name for debugging */
    virtual FString GetStrategyName() const = 0;
};

// ============================================================================
// FDistanceBasedVertexSelector - 거리 기반 선택 (기본)
// ============================================================================
// Uses: Context.RingSettings, Context.BoneTransform, Context.AllVertices
// Ignores: Context.SDFCache

class FDistanceBasedVertexSelector : public IVertexSelector
{
public:
    virtual void SelectVertices(
        const FVertexSelectionContext& Context,
        TArray<FAffectedVertex>& OutAffected) override;

    virtual FString GetStrategyName() const override
    {
        return TEXT("DistanceBased");
    }

    /**
     * Select post-processing vertices for Manual mode (Z-extended range)
     * Manual 모드용 후처리 버텍스 선택 (Z 확장 범위)
     *
     * Ring 파라미터(RingCenter, RingAxis, RingHeight) 기반으로 후처리 버텍스 선택.
     * SDF 없이 Component Space에서 직접 계산.
     *
     * @param Context - Vertex selection context
     * @param AffectedVertices - Already selected affected vertices
     * @param OutRingData - Output: fills PostProcessingIndices, PostProcessingInfluences
     */
    void SelectPostProcessingVertices(
        const FVertexSelectionContext& Context,
        const TArray<FAffectedVertex>& AffectedVertices,
        FRingAffectedData& OutRingData);

protected:
    float CalculateFalloff(float Distance, float MaxDistance, EFalloffType InFalloffType) const;
};

// ============================================================================
// FSDFBoundsBasedVertexSelector - SDF 바운드 기반 선택
// ============================================================================
// Uses: Context.SDFCache (BoundsMin, BoundsMax), Context.AllVertices
// Ignores: Context.RingSettings geometry, Context.BoneTransform
//
// Design: Select all vertices within SDF bounding box.
// GPU shader determines actual influence via SDF sampling.
// If SDFCache is nullptr or invalid, selects nothing.

class FSDFBoundsBasedVertexSelector : public IVertexSelector
{
public:
    virtual void SelectVertices(
        const FVertexSelectionContext& Context,
        TArray<FAffectedVertex>& OutAffected) override;

    virtual FString GetStrategyName() const override
    {
        return TEXT("SDFBoundsBased");
    }

    /**
     * Select post-processing vertices (Z-extended range)
     * 후처리용 버텍스 선택 (Z 확장 범위)
     *
     * Selects vertices in the Z-extended range for post-processing passes.
     * Core vertices (inside original AABB) get influence 1.0.
     * Extended vertices get falloff-based influence.
     * Z 확장 범위의 버텍스를 후처리 패스용으로 선택.
     * 코어 버텍스(원본 AABB 내)는 influence 1.0.
     * 확장 버텍스는 falloff 기반 influence.
     *
     * @param Context - Vertex selection context with SDF cache
     * @param AffectedVertices - Already selected affected vertices (for quick lookup)
     * @param OutRingData - Output: fills PostProcessingIndices, PostProcessingInfluences
     */
    void SelectPostProcessingVertices(
        const FVertexSelectionContext& Context,
        const TArray<FAffectedVertex>& AffectedVertices,
        FRingAffectedData& OutRingData);
};

// ============================================================================
// FVirtualBandVertexSelector - Virtual Band(ProceduralBand) 모드용 선택
// ============================================================================
// Uses: Context.RingSettings.ProceduralBand (4반경, 3높이)
//
// Design: SDF 없이 4-반경 가변 형상으로 Tightness 버텍스 선택
// Manual Mode와의 차이점:
// - 고정 반경 대신 GetRadiusAtHeight()로 가변 반경 사용
// - Band Section(중간)만 Tightness 적용, Upper/Lower는 Bulge용

class FVirtualBandVertexSelector : public IVertexSelector
{
public:
    virtual void SelectVertices(
        const FVertexSelectionContext& Context,
        TArray<FAffectedVertex>& OutAffected) override;

    virtual FString GetStrategyName() const override
    {
        return TEXT("VirtualBandBased");
    }

    /**
     * Select post-processing vertices for Virtual Band mode (Z-extended range)
     * Virtual Band 모드용 후처리 버텍스 선택 (Z 확장 범위)
     *
     * 전체 Virtual Band 높이 + BoundsZTop/Bottom 확장 범위에서 버텍스 선택.
     * Manual Mode 패턴과 동일한 로직 사용.
     *
     * @param Context - Vertex selection context
     * @param AffectedVertices - Already selected affected vertices
     * @param OutRingData - Output: fills PostProcessingIndices, PostProcessingInfluences
     */
    void SelectPostProcessingVertices(
        const FVertexSelectionContext& Context,
        const TArray<FAffectedVertex>& AffectedVertices,
        FRingAffectedData& OutRingData);

private:
    /**
     * Calculate band radius at given height (variable radius)
     * 주어진 높이에서의 밴드 반경 계산 (가변 반경)
     *
     * Lower Section: LowerRadius → MidLowerRadius
     * Band Section: MidLowerRadius → MidUpperRadius
     * Upper Section: MidUpperRadius → UpperRadius
     */
    float GetRadiusAtHeight(float LocalZ, const struct FProceduralBandSettings& BandSettings) const;

    /**
     * Calculate falloff influence based on distance
     * 거리에 따른 Falloff 영향도 계산
     */
    float CalculateFalloff(float Distance, float MaxDistance, EFalloffType InFalloffType) const;
};

// Affected Vertices Manager
// 영향받는 버텍스 관리자
// Central manager for affected vertex registration and updates
// 버텍스 등록 및 업데이트를 담당하는 중앙 관리자
class FLESHRINGRUNTIME_API FFleshRingAffectedVerticesManager
{
public:
    FFleshRingAffectedVerticesManager();
    ~FFleshRingAffectedVerticesManager();

    /**
     * Set the vertex selector strategy
     * [FLEXIBLE] Can swap selection algorithm at runtime
     * 버텍스 선택 전략 설정
     * 런타임에 선택 알고리즘 교체 가능
     *
     * @param InSelector - New vertex selector to use
     *                     사용할 새 버텍스 선택기
     */
    void SetVertexSelector(TSharedPtr<IVertexSelector> InSelector);

    /**
     * Get current vertex selector
     * 현재 버텍스 선택기 반환
     */
    TSharedPtr<IVertexSelector> GetVertexSelector() const { return VertexSelector; }

    /**
     * Register affected vertices for all Rings in a component
     * 컴포넌트의 모든 링에 대해 영향받는 버텍스 등록
     *
     * @param Component - FleshRingComponent with Ring settings
     *                    링 설정을 가진 FleshRingComponent
     * @param SkeletalMesh - Target skeletal mesh
     *                       대상 스켈레탈 메시
     * @param LODIndex - LOD index to use for vertex extraction (default: 0)
     *                   버텍스 추출에 사용할 LOD 인덱스 (기본값: 0)
     * @return true if registration succeeded
     *         등록 성공 시 true
     */
    bool RegisterAffectedVertices(
        const UFleshRingComponent* Component,
        const USkeletalMeshComponent* SkeletalMesh,
        int32 LODIndex = 0);

    /**
     * Get affected data for a specific Ring by index
     * 인덱스로 특정 링의 영향 데이터 반환
     */
    const FRingAffectedData* GetRingData(int32 RingIndex) const;

    /**
     * Get all Ring affected data
     * 모든 링의 영향 데이터 반환
     */
    const TArray<FRingAffectedData>& GetAllRingData() const { return RingDataArray; }

    /**
     * Get the Spatial Hash for O(1) vertex queries
     * O(1) 버텍스 쿼리용 Spatial Hash 반환
     * Used by Bulge calculation for performance optimization
     * Bulge 계산 성능 최적화에 사용
     */
    const FVertexSpatialHash& GetSpatialHash() const { return VertexSpatialHash; }

    /**
     * Clear all registered data
     * 모든 등록된 데이터 삭제
     */
    void ClearAll();

    /**
     * Get total affected vertex count across all Rings
     * 모든 링의 총 영향받는 버텍스 수 반환
     */
    int32 GetTotalAffectedCount() const;

    // ===== Per-Ring Dirty Flag System =====
    // Ring별 Dirty 플래그 시스템 (불필요한 재빌드 방지)

    /**
     * Mark a specific ring as dirty (needs rebuild)
     * 특정 링을 dirty로 표시 (재빌드 필요)
     */
    void MarkRingDirty(int32 RingIndex);

    /**
     * Mark all rings as dirty
     * 모든 링을 dirty로 표시
     */
    void MarkAllRingsDirty();

    /**
     * Check if a ring is dirty
     * 링이 dirty인지 확인
     */
    bool IsRingDirty(int32 RingIndex) const;

    // ===== Topology Cache Public API =====

    /**
     * Build topology cache from mesh data (call once per mesh)
     * 메시 데이터로 토폴로지 캐시 빌드 (메시당 한 번만 호출)
     *
     * Builds position groups, neighbor maps, and welded neighbor data.
     * This is O(V*T) but only runs once per mesh binding.
     * 위치 그룹, 이웃 맵, 용접된 이웃 데이터 빌드.
     * O(V*T) 복잡도지만 메시 바인딩당 한 번만 실행.
     *
     * @param AllVertices - All mesh vertices in bind pose
     * @param MeshIndices - Mesh index buffer (3 indices per triangle)
     */
    void BuildTopologyCache(
        const TArray<FVector3f>& AllVertices,
        const TArray<uint32>& MeshIndices);

    /**
     * Invalidate topology cache (call when mesh changes)
     * 토폴로지 캐시 무효화 (메시 변경 시 호출)
     */
    void InvalidateTopologyCache();

    /**
     * Rebuild vertex layer types from MaterialLayerMappings
     * MaterialLayerMappings에서 버텍스 레이어 타입 재빌드
     * Called every RegisterAffectedVertices to reflect MaterialLayerMappings changes
     * @param Component - FleshRingComponent with asset settings
     * @param SkeletalMesh - Source skeletal mesh component
     * @param LODIndex - LOD index to use
     */
    void RebuildVertexLayerTypes(const UFleshRingComponent* Component, const USkeletalMeshComponent* SkeletalMesh, int32 LODIndex);

    /**
     * Check if topology cache is built
     * 토폴로지 캐시 빌드 여부 확인
     */
    bool IsTopologyCacheBuilt() const { return bTopologyCacheBuilt; }

    /**
     * Get cached mesh indices for Normal recomputation
     * 노멀 재계산용 캐시된 메시 인덱스 반환
     */
    const TArray<uint32>& GetCachedMeshIndices() const { return CachedMeshIndices; }

    /**
     * Get cached position-to-vertices map for UV seam welding
     * UV seam 용접용 캐시된 위치-버텍스 맵 반환
     * Used by SDFBoundsBasedSelector to avoid O(N) rebuild every frame
     * SDFBoundsBasedSelector에서 매 프레임 O(N) 재빌드 방지에 사용
     */
    const TMap<FIntVector, TArray<uint32>>& GetCachedPositionToVertices() const { return CachedPositionToVertices; }

    /**
     * Get cached vertex layer types for GPU upload
     * GPU 업로드용 캐시된 버텍스 레이어 타입 반환
     * Full mesh size array - can be directly uploaded to GPU
     * 전체 메시 크기 배열 - GPU에 직접 업로드 가능
     */
    const TArray<EFleshRingLayerType>& GetCachedVertexLayerTypes() const { return CachedVertexLayerTypes; }

private:
    /**
     * Current vertex selector strategy
     * 현재 버텍스 선택 전략
     */
    TSharedPtr<IVertexSelector> VertexSelector;

    /**
     * Per-Ring affected data
     * 링별 영향 데이터 배열
     */
    TArray<FRingAffectedData> RingDataArray;

    /**
     * Cached mesh indices for Normal recomputation
     * 노멀 재계산용 캐시된 메시 인덱스 (모든 Ring 공유)
     */
    TArray<uint32> CachedMeshIndices;

    /**
     * Cached mesh vertices (bind pose, immutable)
     * 캐시된 메시 버텍스 (바인드 포즈, 불변)
     */
    TArray<FVector3f> CachedMeshVertices;

    /**
     * Cached vertex layer types (material-based, immutable)
     * 캐시된 버텍스 레이어 타입 (머티리얼 기반, 불변)
     */
    TArray<EFleshRingLayerType> CachedVertexLayerTypes;

    /**
     * Flag indicating mesh data is cached (skip re-extraction on update)
     * 메시 데이터 캐시 여부 (업데이트 시 재추출 스킵)
     */
    bool bMeshDataCached = false;

    /**
     * Spatial hash for O(1) vertex query
     * O(1) 버텍스 쿼리를 위한 공간 해시 (브루트포스 O(n) 대체)
     */
    FVertexSpatialHash VertexSpatialHash;

    /**
     * Per-Ring dirty flags (true = needs rebuild)
     * Ring별 dirty 플래그 (true = 재빌드 필요)
     */
    TArray<bool> RingDirtyFlags;

    // ===== Topology Cache (Immutable after first build) =====
    // ===== 토폴로지 캐시 (첫 빌드 후 불변) =====
    // 메시 토폴로지(버텍스 인접 관계, UV seam 용접 정보)는 바인드 포즈에서 결정되며
    // 런타임에 변하지 않음. 한 번만 빌드하고 캐싱하여 성능 최적화.

    /**
     * Flag indicating topology cache is built
     * 토폴로지 캐시 빌드 완료 플래그
     */
    bool bTopologyCacheBuilt = false;

    /**
     * Position-based vertex grouping for UV seam welding
     * UV seam 용접을 위한 위치 기반 버텍스 그룹핑
     * Key: quantized position (FIntVector), Value: vertex indices at that position
     */
    TMap<FIntVector, TArray<uint32>> CachedPositionToVertices;

    /**
     * Reverse lookup: vertex index to quantized position
     * 역방향 조회: 버텍스 인덱스 → 양자화된 위치
     */
    TMap<uint32, FIntVector> CachedVertexToPosition;

    /**
     * Per-vertex neighbor map (direct mesh connectivity)
     * 버텍스별 이웃 맵 (직접 메시 연결)
     * Key: vertex index, Value: set of neighbor vertex indices
     */
    TMap<uint32, TSet<uint32>> CachedVertexNeighbors;

    /**
     * Position-based welded neighbor map (UV seam aware)
     * 위치 기반 용접된 이웃 맵 (UV seam 인식)
     * Key: quantized position, Value: set of neighbor positions (welded)
     */
    TMap<FIntVector, TSet<FIntVector>> CachedWeldedNeighborPositions;

    /**
     * Full mesh adjacency map for BFS/hop distance calculation
     * BFS/홉 거리 계산용 전체 메시 인접 맵
     * Key: vertex index, Value: neighbor vertex indices
     */
    TMap<uint32, TArray<uint32>> CachedFullAdjacencyMap;

    /**
     * Per-vertex triangle list for fast adjacency lookup
     * 빠른 인접 조회용 버텍스별 삼각형 리스트
     * Key: vertex index, Value: list of triangle indices containing this vertex
     * BuildAdjacencyData()에서 O(T) → O(avg_triangles_per_vertex) 최적화에 사용
     */
    TMap<uint32, TArray<uint32>> CachedVertexTriangles;

    /**
     * Position → Representative vertex (smallest index at that position)
     * UV seam 용접용 대표 버텍스 맵
     * 같은 위치에 있는 버텍스들 중 가장 작은 인덱스가 대표
     * BuildRepresentativeIndices()에서 O(A) 맵 빌드 → O(1) 캐시 조회로 최적화
     */
    TMap<FIntVector, uint32> CachedPositionToRepresentative;

    /**
     * Extract vertices from skeletal mesh at specific LOD
     * 스켈레탈 메시의 특정 LOD에서 버텍스 추출 (바인드 포즈 컴포넌트 스페이스)
     */
    bool ExtractMeshVertices(
        const USkeletalMeshComponent* SkeletalMesh,
        TArray<FVector3f>& OutVertices,
        int32 LODIndex = 0);

    /**
     * Extract index buffer from skeletal mesh at specific LOD
     * 스켈레탈 메시의 특정 LOD에서 인덱스 버퍼 추출
     */
    bool ExtractMeshIndices(
        const USkeletalMeshComponent* SkeletalMesh,
        TArray<uint32>& OutIndices,
        int32 LODIndex = 0);

    /**
     * Build adjacency data for a single Ring
     * 단일 링의 인접 데이터 빌드
     *
     * @param RingData - Ring data with Vertices already populated
     * @param MeshIndices - Mesh index buffer (3 indices per triangle)
     */
    void BuildAdjacencyData(
        FRingAffectedData& RingData,
        const TArray<uint32>& MeshIndices);

    /**
     * Build Laplacian adjacency data for smoothing with UV seam welding
     * UV seam 용접을 포함한 스무딩용 라플라시안 인접 데이터 빌드
     *
     * Builds per-vertex neighbor lists from mesh indices.
     * Uses position-based welding to ensure vertices at UV seams get consistent neighbors.
     * This prevents cracks at UV seams during Laplacian smoothing.
     * 메시 인덱스에서 버텍스별 이웃 리스트 빌드.
     * 위치 기반 용접을 사용하여 UV seam의 버텍스들이 일관된 이웃을 갖도록 함.
     * 이를 통해 라플라시안 스무딩 시 UV seam에서 크랙 방지.
     *
     * Output format: [NeighborCount, N0, N1, ..., N11] per affected vertex (13 uints)
     * 출력 포맷: 영향받는 버텍스당 [이웃수, N0, N1, ..., N11] (13 uint)
     *
     * @param RingData - Ring data with Vertices already populated
     * @param MeshIndices - Mesh index buffer (3 indices per triangle)
     * @param AllVertices - All mesh vertex positions (for position-based welding)
     * @param VertexLayerTypes - Per-vertex layer types (for same-layer filtering)
     */
    void BuildLaplacianAdjacencyData(
        FRingAffectedData& RingData,
        const TArray<uint32>& MeshIndices,
        const TArray<FVector3f>& AllVertices,
        const TArray<EFleshRingLayerType>& VertexLayerTypes);

    /**
     * Build Laplacian adjacency data for post-processing vertices (Z-extended range)
     * 후처리 버텍스(Z 확장 범위)용 라플라시안 인접 데이터 빌드
     *
     * Similar to BuildLaplacianAdjacencyData but for PostProcessingIndices.
     * BuildLaplacianAdjacencyData와 유사하지만 PostProcessingIndices 기반.
     *
     * @param RingData - Ring data with PostProcessingIndices already populated
     * @param MeshIndices - Mesh index buffer (3 indices per triangle)
     * @param AllVertices - All mesh vertex positions (for position-based welding)
     * @param VertexLayerTypes - Per-vertex layer types (for same-layer filtering)
     */
    void BuildPostProcessingLaplacianAdjacencyData(
        FRingAffectedData& RingData,
        const TArray<uint32>& MeshIndices,
        const TArray<FVector3f>& AllVertices,
        const TArray<EFleshRingLayerType>& VertexLayerTypes);

    /**
     * Build slice data for bone ratio preservation
     * 본 거리 비율 보존용 슬라이스 데이터 빌드
     *
     * Groups vertices by their height along the Ring axis (bucket-based).
     * Same-height vertices should have same bone distance ratio after deformation.
     * 링 축 방향 높이로 버텍스를 그룹핑 (버킷 기반).
     * 같은 높이의 버텍스들은 변형 후에도 동일한 본 거리 비율을 가져야 함.
     *
     * Output:
     * - RingData.OriginalBoneDistances: bind pose bone distance per affected vertex
     * - RingData.SlicePackedData: [Count, V0, V1, ..., V31] per affected vertex
     *
     * @param RingData - Ring data with Vertices already populated
     * @param AllVertices - All mesh vertices in bind pose component space
     * @param BucketSize - Height bucket size for slice grouping (default: 1.0 cm)
     */
    void BuildSliceData(
        FRingAffectedData& RingData,
        const TArray<FVector3f>& AllVertices,
        float BucketSize = 1.0f);

    /**
     * Build PBD adjacency data with rest lengths for edge constraint
     * PBD 에지 제약용 rest length 포함 인접 데이터 빌드
     *
     * Builds per-vertex neighbor lists with rest lengths from mesh indices.
     * Rest length = bind pose distance between vertices.
     * Also builds full mesh influence/deform maps for neighbor weight lookup.
     * 메시 인덱스에서 rest length 포함 버텍스별 이웃 리스트 빌드.
     * Rest length = 버텍스 간 바인드 포즈 거리.
     * 이웃 가중치 조회용 전체 메시 influence/deform 맵도 빌드.
     *
     * Output:
     * - RingData.PBDAdjacencyWithRestLengths: [Count, N0, RL0, N1, RL1, ...] per vertex
     * - RingData.FullInfluenceMap: influence for all mesh vertices
     * - RingData.FullDeformAmountMap: deform amount for all mesh vertices
     *
     * @param RingData - Ring data with Vertices already populated
     * @param MeshIndices - Mesh index buffer (3 indices per triangle)
     * @param AllVertices - All mesh vertices in bind pose component space
     * @param TotalVertexCount - Total number of vertices in mesh
     */
    void BuildPBDAdjacencyData(
        FRingAffectedData& RingData,
        const TArray<uint32>& MeshIndices,
        const TArray<FVector3f>& AllVertices,
        int32 TotalVertexCount);

    /**
     * Build PBD adjacency data for PostProcessing vertices
     * 후처리 버텍스용 PBD 인접 데이터 빌드
     *
     * Same as BuildPBDAdjacencyData but for PostProcessingIndices range.
     * BuildPBDAdjacencyData와 동일하지만 PostProcessingIndices 범위 사용.
     *
     * Output:
     * - RingData.PostProcessingPBDAdjacencyWithRestLengths: [Count, N0, RL0, N1, RL1, ...] per vertex
     *
     * @param RingData - Ring data with PostProcessingIndices already populated
     * @param MeshIndices - Mesh index buffer (3 indices per triangle)
     * @param AllVertices - All mesh vertices in bind pose component space
     * @param TotalVertexCount - Total number of vertices in mesh
     */
    void BuildPostProcessingPBDAdjacencyData(
        FRingAffectedData& RingData,
        const TArray<uint32>& MeshIndices,
        const TArray<FVector3f>& AllVertices,
        int32 TotalVertexCount);

    /**
     * Build adjacency data for PostProcessing vertices (Normal recomputation)
     * 후처리 버텍스용 인접 데이터 빌드 (노멀 재계산용)
     *
     * Same as BuildAdjacencyData but for PostProcessingIndices range.
     * BuildAdjacencyData와 동일하지만 PostProcessingIndices 범위 사용.
     *
     * Output:
     * - RingData.PostProcessingAdjacencyOffsets: [NumPostProcessing+1] offsets
     * - RingData.PostProcessingAdjacencyTriangles: flattened triangle indices
     *
     * @param RingData - Ring data with PostProcessingIndices already populated
     * @param MeshIndices - Mesh index buffer (3 indices per triangle)
     */
    void BuildPostProcessingAdjacencyData(
        FRingAffectedData& RingData,
        const TArray<uint32>& MeshIndices);

    /**
     * Build hop distance data for topology-based smoothing
     * 토폴로지 기반 스무딩용 홉 거리 데이터 빌드
     *
     * Algorithm:
     * 1. Seeds = All affected vertices (SDF sampled)
     * 2. BFS on full mesh from seeds
     * 3. Collect all vertices within MaxHops
     * 4. Build extended smoothing region with influence falloff
     *
     * 알고리즘:
     * 1. Seeds = 모든 affected vertices (SDF 샘플링됨)
     * 2. 전체 메시에서 Seeds로부터 BFS
     * 3. MaxHops 내의 모든 버텍스 수집
     * 4. Influence falloff를 적용한 확장된 스무딩 영역 구축
     *
     * Output:
     * - RingData.ExtendedSmoothingIndices: all vertices in smoothing region
     * - RingData.ExtendedHopDistances: hop distance (0=seed, 1+=hop)
     * - RingData.ExtendedInfluences: influence based on hop distance
     * - RingData.ExtendedLaplacianAdjacency: adjacency for smoothing region
     *
     * @param RingData - Ring data with Vertices already populated
     * @param MeshIndices - Full mesh index buffer (3 indices per triangle)
     * @param AllVertices - All mesh vertices in bind pose component space
     * @param MaxHops - Maximum hop distance from seeds
     * @param FalloffType - Type of falloff curve for influence calculation
     */
    void BuildHopDistanceData(
        FRingAffectedData& RingData,
        const TArray<uint32>& MeshIndices,
        const TArray<FVector3f>& AllVertices,
        int32 MaxHops,
        EFalloffType FalloffType);

    /**
     * Build full mesh adjacency map (vertex -> neighbors)
     * 전체 메시 인접 맵 구축 (버텍스 -> 이웃들)
     *
     * @param MeshIndices - Full mesh index buffer
     * @param NumVertices - Total vertex count
     * @param OutAdjacencyMap - Output: vertex index -> neighbor vertex indices
     */
    void BuildFullMeshAdjacency(
        const TArray<uint32>& MeshIndices,
        int32 NumVertices,
        TMap<uint32, TArray<uint32>>& OutAdjacencyMap);

    /**
     * Build Laplacian adjacency for extended smoothing region
     * 확장된 스무딩 영역용 라플라시안 인접 데이터 구축
     *
     * [수정] BuildPostProcessingLaplacianAdjacencyData와 동일한 welding 로직 사용
     * 기존: CachedFullAdjacencyMap 사용 (UV welding 안됨)
     * 수정: CachedWeldedNeighborPositions 사용 (UV welding 적용)
     *
     * @param RingData - Ring data with ExtendedSmoothingIndices populated
     * @param VertexLayerTypes - Per-vertex layer types (for same-layer filtering)
     */
    void BuildExtendedLaplacianAdjacency(
        FRingAffectedData& RingData,
        const TArray<EFleshRingLayerType>& VertexLayerTypes);

    /**
     * Build representative indices for UV seam welding
     * UV seam 용접을 위한 대표 버텍스 인덱스 빌드
     *
     * All vertices at the same 3D position (UV duplicates) will share the same representative.
     * Shaders read position from representative, compute deformation, write to own index.
     * This ensures UV duplicates always move identically, preventing cracks at UV seams.
     * 같은 3D 위치의 모든 버텍스(UV 중복)가 동일한 대표를 공유.
     * 셰이더: 대표 위치 읽기 → 변형 계산 → 자기 인덱스에 기록.
     * UV 중복이 항상 동일하게 움직여 UV seam 크랙 방지.
     *
     * Output:
     * - RingData.RepresentativeIndices: [NumAffectedVertices] representative for each affected vertex
     * - RingData.PostProcessingRepresentativeIndices: [NumPostProcessing] representative for each PP vertex
     *
     * @param RingData - Ring data with Vertices and PostProcessingIndices already populated
     * @param AllVertices - All mesh vertex positions (for position-based grouping)
     */
    void BuildRepresentativeIndices(
        FRingAffectedData& RingData,
        const TArray<FVector3f>& AllVertices);
};
