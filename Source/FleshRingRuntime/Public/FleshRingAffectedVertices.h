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

    FAffectedVertex()
        : VertexIndex(0)
        , RadialDistance(0.0f)
        , Influence(0.0f)
    {
    }

    FAffectedVertex(uint32 InIndex, float InRadialDist, float InInfluence)
        : VertexIndex(InIndex)
        , RadialDistance(InRadialDist)
        , Influence(InInfluence)
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
     * Ring height along axis direction (total height, ±RingWidth/2 from center)
     * 링 높이 - 축 방향 전체 높이 (중심에서 위아래 RingWidth/2)
     */
    float RingWidth;

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

    FRingAffectedData()
        : BoneName(NAME_None)
        , RingCenter(FVector::ZeroVector)
        , RingAxis(FVector::UpVector)
        , RingRadius(5.0f)
        , RingThickness(1.0f)
        , RingWidth(2.0f)
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

        for (const FAffectedVertex& Vert : Vertices)
        {
            PackedIndices.Add(Vert.VertexIndex);
            PackedInfluences.Add(Vert.Influence);
        }
    }
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

    // ===== Future Extensions =====
    // const TArray<FBoneWeight>* BoneWeights;

    FVertexSelectionContext(
        const FFleshRingSettings& InRingSettings,
        int32 InRingIndex,
        const FTransform& InBoneTransform,
        const TArray<FVector3f>& InAllVertices,
        const FRingSDFCache* InSDFCache = nullptr)
        : RingSettings(InRingSettings)
        , RingIndex(InRingIndex)
        , BoneTransform(InBoneTransform)
        , AllVertices(InAllVertices)
        , SDFCache(InSDFCache)
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
     * Clear all registered data
     * 모든 등록된 데이터 삭제
     */
    void ClearAll();

    /**
     * Get total affected vertex count across all Rings
     * 모든 링의 총 영향받는 버텍스 수 반환
     */
    int32 GetTotalAffectedCount() const;

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
     * Extract vertices from skeletal mesh at specific LOD
     * 스켈레탈 메시의 특정 LOD에서 버텍스 추출 (바인드 포즈 컴포넌트 스페이스)
     */
    bool ExtractMeshVertices(
        const USkeletalMeshComponent* SkeletalMesh,
        TArray<FVector3f>& OutVertices,
        int32 LODIndex = 0);
};
