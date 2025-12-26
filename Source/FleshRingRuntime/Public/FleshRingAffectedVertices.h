// Purpose: Track and manage vertices affected by each Ring for optimized processing
// [FLEXIBLE] This module uses Strategy Pattern for vertex selection
// The selection algorithm can be swapped by implementing IVertexSelector

#pragma once

#include "CoreMinimal.h"
#include "FleshRingTypes.h"

class UFleshRingComponent;

// Forward declarations
class USkeletalMeshComponent;

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

// [FLEXIBLE] Vertex Selector Interface
// 버텍스 선택 인터페이스 (Strategy Pattern)
// 
// Strategy pattern for vertex selection algorithm
// Can be replaced with different implementations:
// - Distance-based (default, Week 2)
// - SDF-based (future, with Role A)
// - Bone weight based (future)
//
// 버텍스 선택 알고리즘을 위한 전략 패턴
// 다양한 구현체로 교체 가능:
// - 거리 기반 (기본, Week 2)
// - SDF 기반 (미래, Role A와 협업)
// - 본 웨이트 기반 (미래)

/**
 * Interface for vertex selection strategies
 * Implement this to create custom vertex selection algorithms
 * 버텍스 선택 전략 인터페이스
 * 커스텀 버텍스 선택 알고리즘 구현 시 이 인터페이스 상속
 */
class IVertexSelector
{
public:
    virtual ~IVertexSelector() = default;

    /**
     * Select vertices affected by a Ring
     * 링에 영향받는 버텍스 선택
     *
     * @param Ring - Ring settings from FleshRingComponent
     *               링 설정 (FleshRingComponent에서 가져옴)
     * @param BoneTransform - Bone transform in bind pose component space
     *                        본 트랜스폼 (바인드 포즈 컴포넌트 스페이스)
     * @param AllVertices - All mesh vertices in bind pose component space
     *                      메시의 모든 버텍스 (바인드 포즈 컴포넌트 스페이스)
     * @param OutAffected - Output: affected vertices with influence
     *                      출력: 영향받는 버텍스 목록 (영향도 포함)
     */
    virtual void SelectVertices(
        const FFleshRingSettings& Ring,
        const FTransform& BoneTransform,
        const TArray<FVector3f>& AllVertices,
        TArray<FAffectedVertex>& OutAffected) = 0;

    /**
     * Get the name of this selection strategy (for debugging)
     * 선택 전략 이름 반환 (디버깅용)
     */
    virtual FString GetStrategyName() const = 0;
};

// [DEFAULT] Distance-Based Vertex Selector
// 거리 기반 버텍스 선택기 (기본 구현)
// 
// Default implementation using cylindrical distance from Ring axis
// This is the Week 2 basic implementation
// 링 축으로부터의 원통형 거리를 사용하는 기본 구현
// Week 2 기본 구현체
class FDistanceBasedVertexSelector : public IVertexSelector
{
public:
    virtual void SelectVertices(
        const FFleshRingSettings& Ring,
        const FTransform& BoneTransform,
        const TArray<FVector3f>& AllVertices,
        TArray<FAffectedVertex>& OutAffected) override;

    virtual FString GetStrategyName() const override
    {
        return TEXT("DistanceBased");
    }

protected:
    /**
     * Calculate falloff influence based on distance
     * 거리에 따른 감쇠 영향도 계산
     *
     * @param Distance - Distance from Ring axis
     *                   링 축으로부터의 거리
     * @param MaxDistance - Maximum influence distance (RingRadius + RingThickness)
     *                      최대 영향 거리 (내부 반지름 + 링 두께)
     * @param InFalloffType - Falloff curve type (from Ring settings)
     *                        감쇠 곡선 타입 (링 설정에서 전달)
     * @return Influence value (0-1)
     *         영향도 값 (0~1)
     */
    virtual float CalculateFalloff(float Distance, float MaxDistance, EFalloffType InFalloffType) const;
};

// ============================================================================
// [FUTURE] SDF-Based Vertex Selector (Placeholder)
// SDF 기반 버텍스 선택기 (미래 구현 예정)
// ============================================================================
// Will be implemented after Role A completes SDF generation
// Uses SDF texture to determine vertex influence
// Role A의 SDF 생성 완료 후 구현 예정
// SDF 텍스처를 사용하여 버텍스 영향도 결정

// class FSdfBasedVertexSelector : public IVertexSelector
// {
// public:
//     void SetSdfTexture(UVolumeTexture* InSdfTexture);
//
//     virtual void SelectVertices(
//         const FFleshRingSettings& Ring,
//         const FTransform& BoneTransform,
//         const TArray<FVector3f>& AllVertices,
//         TArray<FAffectedVertex>& OutAffected) override;
//
//     virtual FString GetStrategyName() const override
//     {
//         return TEXT("SdfBased");
//     }
//
// private:
//     UVolumeTexture* SdfTexture = nullptr;
// };

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
     * @return true if registration succeeded
     *         등록 성공 시 true
     */
    bool RegisterAffectedVertices(
        const UFleshRingComponent* Component,
        const USkeletalMeshComponent* SkeletalMesh);

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
     * Extract vertices from skeletal mesh
     * 스켈레탈 메시에서 버텍스 추출 (바인드 포즈 컴포넌트 스페이스)
     */
    bool ExtractMeshVertices(
        const USkeletalMeshComponent* SkeletalMesh,
        TArray<FVector3f>& OutVertices);
};
