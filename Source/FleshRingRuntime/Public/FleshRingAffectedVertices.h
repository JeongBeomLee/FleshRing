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
 */
struct FAffectedVertex
{
    /** Original mesh vertex index */
    uint32 VertexIndex;

    /** Distance from Ring center axis */
    float DistanceToRing;

    /** Influence weight (0-1) based on falloff calculation */
    float Influence;

    FAffectedVertex()
        : VertexIndex(0)
        , DistanceToRing(0.0f)
        , Influence(0.0f)
    {
    }

    FAffectedVertex(uint32 InIndex, float InDistance, float InInfluence)
        : VertexIndex(InIndex)
        , DistanceToRing(InDistance)
        , Influence(InInfluence)
    {
    }
};

/**
 * Per-Ring affected vertex collection
 * Contains all vertices affected by a single Ring
 */
struct FRingAffectedData
{
    /** Bone name this Ring is attached to */
    FName BoneName;

    /** Ring center in world space */
    FVector RingCenter;

    /** Ring orientation axis (normalized) */
    FVector RingAxis;

    /** Ring radius from component settings */
    float RingRadius;

    /** Ring width from component settings */
    float RingWidth;

    /** [추가] 조이기 강도 - FFleshRingSettings에서 복사 */
    float TightnessStrength;

    /** [추가] 감쇠 곡선 타입 - FFleshRingSettings에서 복사 */
    EFalloffType FalloffType;

    /** List of affected vertices */
    TArray<FAffectedVertex> Vertices;

    /** GPU buffer indices (for CS dispatch) */
    TArray<uint32> PackedIndices;

    /** GPU buffer influences (for CS dispatch) */
    TArray<float> PackedInfluences;

    FRingAffectedData()
        : BoneName(NAME_None)
        , RingCenter(FVector::ZeroVector)
        , RingAxis(FVector::UpVector)
        , RingRadius(5.0f)
        , RingWidth(2.0f)
        , TightnessStrength(1.0f)
        , FalloffType(EFalloffType::Linear)
    {
    }

    /** Pack vertex data into GPU-friendly buffers */
    // GPU likes just flat array
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
// Strategy pattern for vertex selection algorithm
// Can be replaced with different implementations:
// - Distance-based (default, Week 2)
// - SDF-based (future, with Role A)
// - Bone weight based (future)

/**
 * Interface for vertex selection strategies
 * Implement this to create custom vertex selection algorithms
 */
class IVertexSelector
{
public:
    virtual ~IVertexSelector() = default;

    /**
     * Select vertices affected by a Ring
     *
     * @param Ring - Ring settings from FleshRingComponent
     * @param BoneTransform - World transform of the bone
     * @param AllVertices - All mesh vertices in local space
     * @param OutAffected - Output: affected vertices with influence
     */
    virtual void SelectVertices(
        const FFleshRingSettings& Ring,
        const FTransform& BoneTransform,
        const TArray<FVector3f>& AllVertices,
        TArray<FAffectedVertex>& OutAffected) = 0;

    /**
     * Get the name of this selection strategy (for debugging)
     */
    virtual FString GetStrategyName() const = 0;
};

// [DEFAULT] 'Distance-Based' Vertex Selector
// Default implementation using cylindrical distance from Ring axis
// This is the Week 2 basic implementation
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

    // [수정] 로컬 EFalloffType enum 삭제 - FleshRingTypes.h의 전역 enum 사용
    // [수정] FalloffType 멤버 삭제 - Ring.FalloffType에서 가져옴

protected:
    /**
     * Calculate falloff influence based on distance
     * [수정] FalloffType 파라미터 추가 - Ring 설정에서 전달받음
     *
     * @param Distance - Distance from Ring axis
     * @param MaxDistance - Maximum influence distance (RingRadius + RingWidth)
     * @param InFalloffType - 감쇠 곡선 타입 (Ring 설정에서 전달)
     * @return Influence value (0-1)
     */
    virtual float CalculateFalloff(float Distance, float MaxDistance, EFalloffType InFalloffType) const;
};

// [FUTURE] SDF-Based Vertex Selector (Placeholder)
// Will be implemented after Role A completes SDF generation
// Uses SDF texture to determine vertex influence

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
// Central manager for affected vertex registration and updates
class FLESHRINGRUNTIME_API FFleshRingAffectedVerticesManager
{
public:
    FFleshRingAffectedVerticesManager();
    ~FFleshRingAffectedVerticesManager();

    /**
     * Set the vertex selector strategy
     * [FLEXIBLE] Can swap selection algorithm at runtime
     *
     * @param InSelector - New vertex selector to use
     */
    void SetVertexSelector(TSharedPtr<IVertexSelector> InSelector);

    /**
     * Get current vertex selector
     */
    TSharedPtr<IVertexSelector> GetVertexSelector() const { return VertexSelector; }

    /**
     * Register affected vertices for all Rings in a component
     *
     * @param Component - FleshRingComponent with Ring settings
     * @param SkeletalMesh - Target skeletal mesh
     * @return true if registration succeeded
     */
    bool RegisterAffectedVertices(
        const UFleshRingComponent* Component,
        const USkeletalMeshComponent* SkeletalMesh);

    /**
     * Get affected data for a specific Ring by index
     */
    const FRingAffectedData* GetRingData(int32 RingIndex) const;

    /**
     * Get all Ring affected data
     */
    const TArray<FRingAffectedData>& GetAllRingData() const { return RingDataArray; }

    /**
     * Clear all registered data
     */
    void ClearAll();

    /**
     * Get total affected vertex count across all Rings
     */
    int32 GetTotalAffectedCount() const;

private:
    /** Current vertex selector strategy */
    TSharedPtr<IVertexSelector> VertexSelector;

    /** Per-Ring affected data */
    TArray<FRingAffectedData> RingDataArray;

    /** Extract vertices from skeletal mesh */
    bool ExtractMeshVertices(
        const USkeletalMeshComponent* SkeletalMesh,
        TArray<FVector3f>& OutVertices);
};
