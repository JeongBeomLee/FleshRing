// Copyright 2026 LgThx. All Rights Reserved.

// ============================================================================
// FleshRing Affected Vertices System - Implementation
// ============================================================================
// Purpose: Track and manage vertices affected by each Ring
// Role B: Deformation Algorithm (Week 2)

#include "FleshRingAffectedVertices.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Materials/MaterialInterface.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingVertices, Log, All);

// ============================================================================
// Layer Type Detection from Material Name
// ============================================================================

namespace FleshRingLayerUtils
{
    /**
     * Detect layer type from material name using keyword matching
     *
     * Supported keywords (case-insensitive):
     * - Skin: "skin", "body", "flesh", "face", "hand", "leg", "arm"
     * - Stocking: "stocking", "tight", "pantyhose", "hosiery", "nylon"
     * - Underwear: "underwear", "bra", "panty", "lingerie", "bikini"
     * - Outerwear: "cloth", "dress", "shirt", "skirt", "jacket", "coat"
     *
     * @param MaterialName - Material name to analyze
     * @return Detected layer type (Unknown if no keyword matched)
     */
    EFleshRingLayerType DetectLayerTypeFromMaterialName(const FString& MaterialName)
    {
        // Convert to lowercase for case-insensitive matching
        FString LowerName = MaterialName.ToLower();

        // Skin keywords (highest priority for base layer)
        static const TArray<FString> SkinKeywords = {
            TEXT("skin"), TEXT("body"), TEXT("flesh"), TEXT("face"),
            TEXT("hand"), TEXT("leg"), TEXT("arm"), TEXT("foot"), TEXT("head")
        };

        // Stocking keywords
        static const TArray<FString> StockingKeywords = {
            TEXT("stocking"), TEXT("tight"), TEXT("pantyhose"),
            TEXT("hosiery"), TEXT("nylon"), TEXT("sock"), TEXT("legging")
        };

        // Underwear keywords
        static const TArray<FString> UnderwearKeywords = {
            TEXT("underwear"), TEXT("bra"), TEXT("panty"), TEXT("panties"),
            TEXT("lingerie"), TEXT("bikini"), TEXT("brief"), TEXT("thong")
        };

        // Outerwear keywords
        static const TArray<FString> OuterwearKeywords = {
            TEXT("cloth"), TEXT("dress"), TEXT("shirt"), TEXT("skirt"),
            TEXT("jacket"), TEXT("coat"), TEXT("pants"), TEXT("jeans"),
            TEXT("top"), TEXT("blouse"), TEXT("suit")
        };

        // Check in order of specificity (more specific layers first)

        for (const FString& Keyword : StockingKeywords)
        {
            if (LowerName.Contains(Keyword))
            {
                return EFleshRingLayerType::Stocking;
            }
        }

        for (const FString& Keyword : UnderwearKeywords)
        {
            if (LowerName.Contains(Keyword))
            {
                return EFleshRingLayerType::Underwear;
            }
        }

        for (const FString& Keyword : OuterwearKeywords)
        {
            if (LowerName.Contains(Keyword))
            {
                return EFleshRingLayerType::Outerwear;
            }
        }

        for (const FString& Keyword : SkinKeywords)
        {
            if (LowerName.Contains(Keyword))
            {
                return EFleshRingLayerType::Skin;
            }
        }

        // No keyword matched
        return EFleshRingLayerType::Other;
    }

    /**
     * Build per-vertex layer type array from skeletal mesh sections
     *
     * @param SkeletalMesh - Source skeletal mesh component
     * @param LODIndex - LOD index to use
     * @param OutVertexLayerTypes - Output array (vertex index → layer type)
     * @return true if successful
     */
    bool BuildVertexLayerTypes(
        const USkeletalMeshComponent* SkeletalMesh,
        int32 LODIndex,
        TArray<EFleshRingLayerType>& OutVertexLayerTypes)
    {
        if (!SkeletalMesh)
        {
            return false;
        }

        USkeletalMesh* Mesh = SkeletalMesh->GetSkeletalMeshAsset();
        if (!Mesh)
        {
            return false;
        }

        const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
        if (!RenderData || RenderData->LODRenderData.Num() == 0)
        {
            return false;
        }

        if (LODIndex < 0 || LODIndex >= RenderData->LODRenderData.Num())
        {
            LODIndex = 0;
        }

        const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];
        const int32 NumVertices = static_cast<int32>(LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices());

        // Initialize all vertices as Unknown
        OutVertexLayerTypes.Reset();
        OutVertexLayerTypes.SetNum(NumVertices);
        for (int32 i = 0; i < NumVertices; ++i)
        {
            OutVertexLayerTypes[i] = EFleshRingLayerType::Other;
        }

        // Get materials from the skeletal mesh component
        const TArray<UMaterialInterface*>& Materials = SkeletalMesh->GetMaterials();

        // Iterate through render sections and assign layer types
        for (int32 SectionIdx = 0; SectionIdx < LODData.RenderSections.Num(); ++SectionIdx)
        {
            const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIdx];
            const uint16 MaterialIndex = Section.MaterialIndex;

            // Get material name
            FString MaterialName = TEXT("Unknown");
            if (Materials.IsValidIndex(MaterialIndex) && Materials[MaterialIndex])
            {
                MaterialName = Materials[MaterialIndex]->GetName();
            }

            // Detect layer type from material name
            EFleshRingLayerType LayerType = DetectLayerTypeFromMaterialName(MaterialName);

            // Assign layer type to all vertices in this section
            const int32 BaseVertexIndex = static_cast<int32>(Section.BaseVertexIndex);
            const int32 NumSectionVertices = static_cast<int32>(Section.NumVertices);

            for (int32 i = 0; i < NumSectionVertices; ++i)
            {
                const int32 VertexIndex = BaseVertexIndex + i;
                if (VertexIndex < NumVertices)
                {
                    OutVertexLayerTypes[VertexIndex] = LayerType;
                }
            }

            UE_LOG(LogFleshRingVertices, Log,
                TEXT("Section[%d]: Material '%s' → Layer %d (%d vertices)"),
                SectionIdx, *MaterialName, static_cast<int32>(LayerType), NumSectionVertices);
        }

        return true;
    }
} // namespace FleshRingLayerUtils

// ============================================================================
// FVertexSpatialHash Implementation (O(n) → O(1) query optimization)
// ============================================================================

void FVertexSpatialHash::Build(const TArray<FVector3f>& Vertices, float InCellSize)
{
    Clear();

    if (Vertices.Num() == 0 || InCellSize <= 0.0f)
    {
        return;
    }

    CellSize = InCellSize;
    InvCellSize = 1.0f / CellSize;
    CachedVertices = Vertices;

    // Insert all vertices into hash grid
    for (int32 i = 0; i < Vertices.Num(); ++i)
    {
        FIntVector CellKey = GetCellKey(FVector(Vertices[i]));
        uint64 Hash = HashCellKey(CellKey);
        CellMap.FindOrAdd(Hash).Add(i);
    }

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("SpatialHash: Built with %d vertices, %d cells (CellSize=%.1f)"),
        Vertices.Num(), CellMap.Num(), CellSize);
}

void FVertexSpatialHash::QueryAABB(const FVector& Min, const FVector& Max, TArray<int32>& OutIndices) const
{
    OutIndices.Reset();

    if (!IsBuilt())
    {
        return;
    }

    // Get cell range
    FIntVector MinCell = GetCellKey(Min);
    FIntVector MaxCell = GetCellKey(Max);

    // Reserve approximate capacity
    OutIndices.Reserve((MaxCell.X - MinCell.X + 1) * (MaxCell.Y - MinCell.Y + 1) * (MaxCell.Z - MinCell.Z + 1) * 10);

    // Iterate through all cells in range
    for (int32 X = MinCell.X; X <= MaxCell.X; ++X)
    {
        for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y)
        {
            for (int32 Z = MinCell.Z; Z <= MaxCell.Z; ++Z)
            {
                uint64 Hash = HashCellKey(FIntVector(X, Y, Z));
                if (const TArray<int32>* CellVertices = CellMap.Find(Hash))
                {
                    // Add all vertices in this cell (they're within AABB)
                    OutIndices.Append(*CellVertices);
                }
            }
        }
    }
}

void FVertexSpatialHash::QueryOBB(const FTransform& LocalToWorld, const FVector& LocalMin, const FVector& LocalMax, TArray<int32>& OutIndices) const
{
    OutIndices.Reset();

    if (!IsBuilt())
    {
        return;
    }

    // Step 1: Convert OBB to world AABB (conservative bounds)
    FBox WorldAABB(ForceInit);
    for (int32 i = 0; i < 8; ++i)
    {
        FVector Corner(
            (i & 1) ? LocalMax.X : LocalMin.X,
            (i & 2) ? LocalMax.Y : LocalMin.Y,
            (i & 4) ? LocalMax.Z : LocalMin.Z
        );
        WorldAABB += LocalToWorld.TransformPosition(Corner);
    }

    // Step 2: Query AABB to get candidates
    TArray<int32> Candidates;
    QueryAABB(WorldAABB.Min, WorldAABB.Max, Candidates);

    // Step 3: Precise OBB check for each candidate
    OutIndices.Reserve(Candidates.Num());
    for (int32 VertexIdx : Candidates)
    {
        FVector LocalPos = LocalToWorld.InverseTransformPosition(FVector(CachedVertices[VertexIdx]));

        if (LocalPos.X >= LocalMin.X && LocalPos.X <= LocalMax.X &&
            LocalPos.Y >= LocalMin.Y && LocalPos.Y <= LocalMax.Y &&
            LocalPos.Z >= LocalMin.Z && LocalPos.Z <= LocalMax.Z)
        {
            OutIndices.Add(VertexIdx);
        }
    }
}

// ============================================================================
// Distance-Based Vertex Selector Implementation
// ============================================================================

void FDistanceBasedVertexSelector::SelectVertices(
    const FVertexSelectionContext& Context,
    TArray<FAffectedVertex>& OutAffected)
{
    OutAffected.Reset();

    // Extract required data from Context
    const FFleshRingSettings& Ring = Context.RingSettings;
    const FTransform& BoneTransform = Context.BoneTransform;
    const TArray<FVector3f>& AllVertices = Context.AllVertices;

    // InfluenceMode-based branching: Check SDFCache validity only in Auto mode
    bool bUseOBB = false;
    if (Ring.InfluenceMode == EFleshRingInfluenceMode::Auto)
    {
        bUseOBB = (Context.SDFCache && Context.SDFCache->bCached);
    }

    // Reserve estimated capacity (assume ~25% vertices affected)
    OutAffected.Reserve(AllVertices.Num() / 4);

    if (bUseOBB)
    {
        // ===== OBB-based vertex selection (exactly matches GPU SDF) =====
        // Must use InverseTransformPosition for non-uniform scale + rotation combination!
        // Inverse().TransformPosition() has incorrect scale and rotation order
        const FTransform& LocalToComponent = Context.SDFCache->LocalToComponent;
        const FVector BoundsMin = FVector(Context.SDFCache->BoundsMin);
        const FVector BoundsMax = FVector(Context.SDFCache->BoundsMax);

        // [Debug] OBB transform info log (for scale verification)
        UE_LOG(LogFleshRingVertices, Log,
            TEXT("OBB SelectVertices: Ring[%d] LocalToComponent Scale=%s, Rot=%s, Trans=%s"),
            Context.RingIndex,
            *LocalToComponent.GetScale3D().ToString(),
            *LocalToComponent.GetRotation().Rotator().ToString(),
            *LocalToComponent.GetLocation().ToString());
        UE_LOG(LogFleshRingVertices, Log,
            TEXT("OBB SelectVertices: Ring[%d] LocalBounds Min=%s, Max=%s, Size=%s"),
            Context.RingIndex,
            *BoundsMin.ToString(),
            *BoundsMax.ToString(),
            *(BoundsMax - BoundsMin).ToString());

        // Parameters for Influence calculation (local space, no scale applied)
        const float RingRadius = Ring.RingRadius;
        const float RingThickness = Ring.RingThickness;
        const float HalfWidth = Ring.RingHeight / 2.0f;

        // ===== O(1) query with Spatial Hash, brute-force O(n) without =====
        TArray<int32> CandidateIndices;
        if (Context.SpatialHash && Context.SpatialHash->IsBuilt())
        {
            // Extract OBB candidates via Spatial Hash (O(1))
            Context.SpatialHash->QueryOBB(LocalToComponent, BoundsMin, BoundsMax, CandidateIndices);
            UE_LOG(LogFleshRingVertices, Verbose,
                TEXT("Ring[%d]: SpatialHash query returned %d candidates (from %d total)"),
                Context.RingIndex, CandidateIndices.Num(), AllVertices.Num());
        }
        else
        {
            // Brute-force fallback: iterate all vertices
            CandidateIndices.Reserve(AllVertices.Num());
            for (int32 i = 0; i < AllVertices.Num(); ++i)
            {
                CandidateIndices.Add(i);
            }
        }

        for (int32 VertexIdx : CandidateIndices)
        {
            // === Layer Type Filtering ===
            if (Context.VertexLayerTypes && Context.VertexLayerTypes->IsValidIndex(VertexIdx))
            {
                const EFleshRingLayerType LayerType = (*Context.VertexLayerTypes)[VertexIdx];
                if (!Context.RingSettings.IsLayerAffected(LayerType))
                {
                    continue; // Skip if not in specified layer
                }
            }

            const FVector VertexPos = FVector(AllVertices[VertexIdx]);

            // Component Space → Local Space transform
            // InverseTransformPosition: (Rot^-1 * (V - Trans)) / Scale (correct order)
            const FVector LocalPos = LocalToComponent.InverseTransformPosition(VertexPos);

            // OBB bounds check (only needed when SpatialHash not used, QueryOBB already checks)
            if (!Context.SpatialHash || !Context.SpatialHash->IsBuilt())
            {
                if (LocalPos.X < BoundsMin.X || LocalPos.X > BoundsMax.X ||
                    LocalPos.Y < BoundsMin.Y || LocalPos.Y > BoundsMax.Y ||
                    LocalPos.Z < BoundsMin.Z || LocalPos.Z > BoundsMax.Z)
                {
                    continue; // Outside OBB - skip
                }
            }

            // Calculate distance to Ring geometry in local space
            // Ring axis = Z-axis (local space), Ring center = origin
            const float AxisDistance = LocalPos.Z;
            const FVector2D RadialVec(LocalPos.X, LocalPos.Y);
            const float RadialDistance = RadialVec.Size();

            // Influence calculation (based on distance from Ring surface)
            const float DistFromRingSurface = FMath::Abs(RadialDistance - RingRadius);
            const float RadialInfluence = CalculateFalloff(DistFromRingSurface, RingThickness, Ring.FalloffType);
            const float AxialInfluence = CalculateFalloff(FMath::Abs(AxisDistance), HalfWidth, Ring.FalloffType);
            const float CombinedInfluence = RadialInfluence * AxialInfluence;

            if (CombinedInfluence > KINDA_SMALL_NUMBER)
            {
                OutAffected.Add(FAffectedVertex(
                    static_cast<uint32>(VertexIdx),
                    RadialDistance,
                    CombinedInfluence
                ));
            }
        }
    }
    else
    {
        // ===== Fallback: Cylindrical model (VirtualRing mode, when SDFCache unavailable) =====
        // Use RingOffset/RingRotation for VirtualRing mode (not MeshOffset/MeshRotation!)
        const FQuat BoneRotation = BoneTransform.GetRotation();
        const FVector WorldRingOffset = BoneRotation.RotateVector(Ring.RingOffset);
        const FVector RingCenter = BoneTransform.GetLocation() + WorldRingOffset;
        const FQuat WorldRingRotation = BoneRotation * Ring.RingRotation;
        const FVector RingAxis = WorldRingRotation.RotateVector(FVector::ZAxisVector);

        // VirtualRing mode has no scale (RingRadius/RingThickness/RingHeight are direct units)
        const float MaxDistance = Ring.RingRadius + Ring.RingThickness;
        const float HalfWidth = Ring.RingHeight / 2.0f;

        // ===== Reduce candidates via Spatial Hash OBB query (O(N) → O(K)) =====
        TArray<int32> CandidateIndices;
        if (Context.SpatialHash && Context.SpatialHash->IsBuilt())
        {
            // OBB transform reflecting Ring rotation
            FTransform RingLocalToComponent;
            RingLocalToComponent.SetLocation(RingCenter);
            RingLocalToComponent.SetRotation(WorldRingRotation);
            RingLocalToComponent.SetScale3D(FVector::OneVector);

            const FVector LocalMin(-MaxDistance, -MaxDistance, -HalfWidth);
            const FVector LocalMax(MaxDistance, MaxDistance, HalfWidth);
            Context.SpatialHash->QueryOBB(RingLocalToComponent, LocalMin, LocalMax, CandidateIndices);

            UE_LOG(LogFleshRingVertices, Log,
                TEXT("VirtualRing Ring[%d]: SpatialHash OBB query returned %d candidates (from %d total)"),
                Context.RingIndex, CandidateIndices.Num(), AllVertices.Num());
        }
        else
        {
            // Fallback: iterate all
            CandidateIndices.Reserve(AllVertices.Num());
            for (int32 i = 0; i < AllVertices.Num(); ++i)
            {
                CandidateIndices.Add(i);
            }
        }

        // Iterate only candidates (previously: all vertices)
        for (int32 VertexIdx : CandidateIndices)
        {
            // === Layer Type Filtering ===
            if (Context.VertexLayerTypes && Context.VertexLayerTypes->IsValidIndex(VertexIdx))
            {
                const EFleshRingLayerType LayerType = (*Context.VertexLayerTypes)[VertexIdx];
                if (!Context.RingSettings.IsLayerAffected(LayerType))
                {
                    continue; // Skip if not in specified layer
                }
            }

            const FVector VertexPos = FVector(AllVertices[VertexIdx]);
            const FVector ToVertex = VertexPos - RingCenter;
            const float AxisDistance = FVector::DotProduct(ToVertex, RingAxis);
            const FVector RadialVec = ToVertex - RingAxis * AxisDistance;
            const float RadialDistance = RadialVec.Size();

            if (RadialDistance <= MaxDistance && FMath::Abs(AxisDistance) <= HalfWidth)
            {
                // CPU uses 1.0 as placeholder since GPU recalculates Influence
                // GPU: CalculateVirtualRingInfluence() in FleshRingTightnessCS.usf
                OutAffected.Add(FAffectedVertex(
                    static_cast<uint32>(VertexIdx),
                    RadialDistance,
                    1.0f  // GPU recalculates
                ));
            }
        }
    }

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("DistanceBasedSelector: Selected %d vertices for Ring[%d] '%s' (Total: %d, OBB: %s)"),
        OutAffected.Num(), Context.RingIndex, *Ring.BoneName.ToString(), AllVertices.Num(),
        bUseOBB ? TEXT("Yes") : TEXT("No"));
}

// ============================================================================
// CalculateFalloff - Falloff curve calculation
// ============================================================================
float FDistanceBasedVertexSelector::CalculateFalloff(
    float Distance,
    float MaxDistance,
    EFalloffType InFalloffType) const
{
    // Normalize distance to 0-1 range
    const float NormalizedDist = FMath::Clamp(Distance / MaxDistance, 0.0f, 1.0f);

    // Inverted: closer = higher influence
    const float T = 1.0f - NormalizedDist;

    switch (InFalloffType)
    {
    case EFalloffType::Quadratic:
        // Smoother falloff near center
        return T * T;

    case EFalloffType::Hermite:
        // Hermite S-curve (smooth in, smooth out)
        return T * T * (3.0f - 2.0f * T);

    case EFalloffType::Linear:
    default:
        // Simple linear falloff
        return T;
    }
}

// ============================================================================
// SelectSmoothingRegionVertices - Post-processing vertex selection for VirtualRing mode
// ============================================================================
void FDistanceBasedVertexSelector::SelectSmoothingRegionVertices(
    const FVertexSelectionContext& Context,
    const TArray<FAffectedVertex>& AffectedVertices,
    FRingAffectedData& OutRingData)
{
    OutRingData.SmoothingRegionIndices.Reset();
    OutRingData.SmoothingRegionInfluences.Reset();
    OutRingData.SmoothingRegionIsAnchor.Reset();

    // Set for fast lookup of original Affected Vertices (for anchor determination)
    TSet<uint32> AffectedSet;
    AffectedSet.Reserve(AffectedVertices.Num());
    for (const FAffectedVertex& V : AffectedVertices)
    {
        AffectedSet.Add(V.VertexIndex);
    }

    // Copy only original Affected Vertices if all Smoothing is disabled
    const bool bAnySmoothingEnabled =
        Context.RingSettings.bEnableRadialSmoothing ||
        Context.RingSettings.bEnableLaplacianSmoothing ||
        Context.RingSettings.bEnablePBDEdgeConstraint;

    if (!bAnySmoothingEnabled)
    {
        OutRingData.SmoothingRegionIndices.Reserve(AffectedVertices.Num());
        OutRingData.SmoothingRegionInfluences.Reserve(AffectedVertices.Num());
        OutRingData.SmoothingRegionIsAnchor.Reserve(AffectedVertices.Num());

        for (const FAffectedVertex& V : AffectedVertices)
        {
            OutRingData.SmoothingRegionIndices.Add(V.VertexIndex);
            OutRingData.SmoothingRegionInfluences.Add(1.0f);
            OutRingData.SmoothingRegionIsAnchor.Add(1);  // Original Affected = anchor
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("PostProcessing (VirtualRing): Smoothing disabled, using %d affected vertices"),
            OutRingData.SmoothingRegionIndices.Num());
        return;
    }

    const float BoundsZTop = Context.RingSettings.SmoothingBoundsZTop;
    const float BoundsZBottom = Context.RingSettings.SmoothingBoundsZBottom;
    const FFleshRingSettings& Ring = Context.RingSettings;
    const TArray<FVector3f>& AllVertices = Context.AllVertices;

    // Use original Affected Vertices as-is if no Z extension
    if (BoundsZTop < 0.01f && BoundsZBottom < 0.01f)
    {
        OutRingData.SmoothingRegionIndices.Reserve(AffectedVertices.Num());
        OutRingData.SmoothingRegionInfluences.Reserve(AffectedVertices.Num());
        OutRingData.SmoothingRegionIsAnchor.Reserve(AffectedVertices.Num());

        for (const FAffectedVertex& V : AffectedVertices)
        {
            OutRingData.SmoothingRegionIndices.Add(V.VertexIndex);
            OutRingData.SmoothingRegionInfluences.Add(1.0f);
            OutRingData.SmoothingRegionIsAnchor.Add(1);  // Original Affected = anchor
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("PostProcessing (VirtualRing): No Z extension, using %d affected vertices"),
            OutRingData.SmoothingRegionIndices.Num());
        return;
    }

    // VirtualRing mode: Calculate directly from Ring parameters (Component Space)
    const FQuat BoneRotation = Context.BoneTransform.GetRotation();
    const FVector WorldRingOffset = BoneRotation.RotateVector(Ring.RingOffset);
    const FVector RingCenter = Context.BoneTransform.GetLocation() + WorldRingOffset;
    const FQuat WorldRingRotation = BoneRotation * Ring.RingRotation;
    const FVector RingAxis = WorldRingRotation.RotateVector(FVector::ZAxisVector);

    const float HalfWidth = Ring.RingHeight / 2.0f;
    const float MaxRadialDistance = Ring.RingRadius + Ring.RingThickness;

    // Z-extended axial range
    const float OriginalZMin = -HalfWidth;
    const float OriginalZMax = HalfWidth;
    const float ExtendedZMin = OriginalZMin - BoundsZBottom;
    const float ExtendedZMax = OriginalZMax + BoundsZTop;

    OutRingData.SmoothingRegionIndices.Reserve(AllVertices.Num() / 4);
    OutRingData.SmoothingRegionInfluences.Reserve(AllVertices.Num() / 4);
    OutRingData.SmoothingRegionIsAnchor.Reserve(AllVertices.Num() / 4);

    int32 CoreCount = 0;
    int32 ExtendedCount = 0;
    int32 AnchorCount = 0;

    for (int32 VertexIdx = 0; VertexIdx < AllVertices.Num(); ++VertexIdx)
    {
        const FVector VertexPos = FVector(AllVertices[VertexIdx]);
        const FVector ToVertex = VertexPos - RingCenter;
        const float AxisDistance = FVector::DotProduct(ToVertex, RingAxis);
        const FVector RadialVec = ToVertex - RingAxis * AxisDistance;
        const float RadialDistance = RadialVec.Size();

        // Radial is original range, axial is extended range
        if (RadialDistance <= MaxRadialDistance &&
            AxisDistance >= ExtendedZMin && AxisDistance <= ExtendedZMax)
        {
            // Influence calculation: within original Z range = 1.0, extended region = falloff
            float Influence = 1.0f;

            if (AxisDistance < OriginalZMin)
            {
                // Bottom extended region: falloff by distance
                float Dist = OriginalZMin - AxisDistance;
                Influence = 1.0f - FMath::Clamp(Dist / BoundsZBottom, 0.0f, 1.0f);
                Influence = FMath::InterpEaseInOut(0.0f, 1.0f, Influence, 2.0f);
                ExtendedCount++;
            }
            else if (AxisDistance > OriginalZMax)
            {
                // Top extended region: falloff by distance
                float Dist = AxisDistance - OriginalZMax;
                Influence = 1.0f - FMath::Clamp(Dist / BoundsZTop, 0.0f, 1.0f);
                Influence = FMath::InterpEaseInOut(0.0f, 1.0f, Influence, 2.0f);
                ExtendedCount++;
            }
            else
            {
                CoreCount++;
            }

            // Anchor determination: only vertices in original AffectedVertices are anchors
            const bool bIsAnchor = AffectedSet.Contains(static_cast<uint32>(VertexIdx));
            if (bIsAnchor) AnchorCount++;

            OutRingData.SmoothingRegionIndices.Add(static_cast<uint32>(VertexIdx));
            OutRingData.SmoothingRegionInfluences.Add(Influence);
            OutRingData.SmoothingRegionIsAnchor.Add(bIsAnchor ? 1 : 0);
        }
    }

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("PostProcessing (VirtualRing): Selected %d vertices (Core=%d, ZExtended=%d, Anchors=%d) for Ring[%d], ZExtend=[%.1f, %.1f]"),
        OutRingData.SmoothingRegionIndices.Num(), CoreCount, ExtendedCount, AnchorCount,
        Context.RingIndex, BoundsZBottom, BoundsZTop);
}

// ============================================================================
// SDF Bounds-Based Vertex Selector Implementation
// ============================================================================

void FSDFBoundsBasedVertexSelector::SelectVertices(
    const FVertexSelectionContext& Context,
    TArray<FAffectedVertex>& OutAffected)
{
    OutAffected.Reset();

    // Check SDF cache from Context
    // Skip selection if SDFCache is nullptr or invalid
    if (!Context.SDFCache || !Context.SDFCache->IsValid())
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("SDFBoundsBasedSelector: No valid SDF cache for Ring[%d] '%s', skipping"),
            Context.RingIndex, *Context.RingSettings.BoneName.ToString());
        return;
    }

    // OBB transform: Component Space → Local Space
    // Must use InverseTransformPosition for non-uniform scale + rotation combination!
    // Inverse().TransformPosition() has incorrect scale and rotation order
    const FTransform& LocalToComponent = Context.SDFCache->LocalToComponent;

    // Vertex filtering bounds (with SDFBoundsExpandX/Y applied)
    // NOTE: SDF texture bounds stay original size, only vertex filtering is expanded
    const float ExpandX = Context.RingSettings.SDFBoundsExpandX;
    const float ExpandY = Context.RingSettings.SDFBoundsExpandY;

    FVector BoundsMin = FVector(Context.SDFCache->BoundsMin);
    FVector BoundsMax = FVector(Context.SDFCache->BoundsMax);

    // Expand in X, Y directions in Ring local space (Z is Ring axis, keep unchanged)
    BoundsMin.X -= ExpandX;
    BoundsMin.Y -= ExpandY;
    BoundsMax.X += ExpandX;
    BoundsMax.Y += ExpandY;

    const TArray<FVector3f>& AllVertices = Context.AllVertices;

    // [Debug] LocalToComponent transform info log (for scale verification)
    UE_LOG(LogFleshRingVertices, Log,
        TEXT("SDFBoundsSelector: Ring[%d] LocalToComponent Scale=%s, Rot=%s, Trans=%s"),
        Context.RingIndex,
        *LocalToComponent.GetScale3D().ToString(),
        *LocalToComponent.GetRotation().Rotator().ToString(),
        *LocalToComponent.GetLocation().ToString());

    // ================================================================
    // UV Seam Welding: Position Group based selection
    // ================================================================
    // Purpose: Ensure all vertices split at UV seams are selected together
    // Method: Group vertices by position → if any in group is selected, select all
    // ================================================================

    constexpr float WeldPrecision = 0.001f;  // Position quantization precision

    // Step 1: Use cached map or fallback to local map build
    // Use cached map if available, otherwise fallback to local build (slow)
    TMap<FIntVector, TArray<uint32>> LocalPositionToVertices;
    const TMap<FIntVector, TArray<uint32>>* PositionToVerticesPtr = Context.CachedPositionToVertices;

    if (!PositionToVerticesPtr || PositionToVerticesPtr->Num() == 0)
    {
        // Fallback: no cache, local map build (O(N) - slow!)
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("SDFBoundsBasedSelector Ring[%d]: CachedPositionToVertices not available, falling back to O(N) local build"),
            Context.RingIndex);

        for (int32 i = 0; i < AllVertices.Num(); ++i)
        {
            const FVector3f& Pos = AllVertices[i];
            FIntVector PosKey(
                FMath::RoundToInt(Pos.X / WeldPrecision),
                FMath::RoundToInt(Pos.Y / WeldPrecision),
                FMath::RoundToInt(Pos.Z / WeldPrecision)
            );
            LocalPositionToVertices.FindOrAdd(PosKey).Add(static_cast<uint32>(i));
        }
        PositionToVerticesPtr = &LocalPositionToVertices;
    }

    const TMap<FIntVector, TArray<uint32>>& PositionToVertices = *PositionToVerticesPtr;

    // ===== O(1) query with Spatial Hash, brute-force O(n) without =====
    TArray<int32> CandidateIndices;
    if (Context.SpatialHash && Context.SpatialHash->IsBuilt())
    {
        // Extract OBB candidates via Spatial Hash (O(1))
        Context.SpatialHash->QueryOBB(LocalToComponent, BoundsMin, BoundsMax, CandidateIndices);
        UE_LOG(LogFleshRingVertices, Verbose,
            TEXT("SDFBoundsSelector Ring[%d]: SpatialHash query returned %d candidates (from %d total)"),
            Context.RingIndex, CandidateIndices.Num(), AllVertices.Num());
    }
    else
    {
        // Brute-force fallback: iterate all vertices
        CandidateIndices.Reserve(AllVertices.Num());
        for (int32 i = 0; i < AllVertices.Num(); ++i)
        {
            CandidateIndices.Add(i);
        }
    }

    // Step 2: Collect selected positions (by Position Group)
    // If any vertex is selected, all vertices at that position are selected
    TSet<FIntVector> SelectedPositions;

    for (int32 VertexIdx : CandidateIndices)
    {
        // === Layer Type Filtering ===
        if (Context.VertexLayerTypes && Context.VertexLayerTypes->IsValidIndex(VertexIdx))
        {
            const EFleshRingLayerType LayerType = (*Context.VertexLayerTypes)[VertexIdx];
            if (!Context.RingSettings.IsLayerAffected(LayerType))
            {
                continue; // Skip if not in specified layer
            }
        }

        const FVector VertexPos = FVector(AllVertices[VertexIdx]);

        // Component Space → Local Space transform
        const FVector LocalPos = LocalToComponent.InverseTransformPosition(VertexPos);

        // AABB containment test in Local Space (only needed when SpatialHash not used)
        if (!Context.SpatialHash || !Context.SpatialHash->IsBuilt())
        {
            if (LocalPos.X < BoundsMin.X || LocalPos.X > BoundsMax.X ||
                LocalPos.Y < BoundsMin.Y || LocalPos.Y > BoundsMax.Y ||
                LocalPos.Z < BoundsMin.Z || LocalPos.Z > BoundsMax.Z)
            {
                continue; // Outside OBB - skip
            }
        }

        // Add this vertex's position key (entire group gets selected)
        const FVector3f& Pos = AllVertices[VertexIdx];
        FIntVector PosKey(
            FMath::RoundToInt(Pos.X / WeldPrecision),
            FMath::RoundToInt(Pos.Y / WeldPrecision),
            FMath::RoundToInt(Pos.Z / WeldPrecision)
        );
        SelectedPositions.Add(PosKey);
    }

    // Step 3: Add all vertices at selected positions (including UV duplicates)
    OutAffected.Reserve(SelectedPositions.Num() * 2);  // Assume average 2 UV duplicates

    int32 UVDuplicatesAdded = 0;
    for (const FIntVector& PosKey : SelectedPositions)
    {
        const TArray<uint32>* VerticesAtPos = PositionToVertices.Find(PosKey);
        if (VerticesAtPos)
        {
            for (uint32 VertIdx : *VerticesAtPos)
            {
                OutAffected.Add(FAffectedVertex(
                    VertIdx,
                    0.0f,  // RadialDistance: unused in SDF mode
                    1.0f   // Influence: max value, GPU shader refines via CalculateInfluenceFromSDF()
                ));
            }
            if (VerticesAtPos->Num() > 1)
            {
                UVDuplicatesAdded += VerticesAtPos->Num() - 1;
            }
        }
    }

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("SDFBoundsBasedSelector: Selected %d vertices (%d positions, %d UV duplicates) for Ring[%d] '%s'"),
        OutAffected.Num(), SelectedPositions.Num(), UVDuplicatesAdded,
        Context.RingIndex, *Context.RingSettings.BoneName.ToString());
}

void FSDFBoundsBasedVertexSelector::SelectSmoothingRegionVertices(
    const FVertexSelectionContext& Context,
    const TArray<FAffectedVertex>& AffectedVertices,
    FRingAffectedData& OutRingData)
{
    OutRingData.SmoothingRegionIndices.Reset();
    OutRingData.SmoothingRegionInfluences.Reset();
    OutRingData.SmoothingRegionIsAnchor.Reset();

    if (!Context.SDFCache || !Context.SDFCache->IsValid())
    {
        return;
    }

    // Set for fast lookup of original Affected Vertices (for anchor determination)
    TSet<uint32> AffectedSet;
    AffectedSet.Reserve(AffectedVertices.Num());
    for (const FAffectedVertex& V : AffectedVertices)
    {
        AffectedSet.Add(V.VertexIndex);
    }

    // Copy only original Affected Vertices without Z extension if all Smoothing is disabled
    // ANY Smoothing ON → Z extension / Hop-based extension possible
    // ALL Smoothing OFF → no extended region needed (use basic SDF volume, Tightness/Bulge only)
    const bool bAnySmoothingEnabled =
        Context.RingSettings.bEnableRadialSmoothing ||
        Context.RingSettings.bEnableLaplacianSmoothing ||
        Context.RingSettings.bEnablePBDEdgeConstraint;

    if (!bAnySmoothingEnabled)
    {
        OutRingData.SmoothingRegionIndices.Reserve(AffectedVertices.Num());
        OutRingData.SmoothingRegionInfluences.Reserve(AffectedVertices.Num());
        OutRingData.SmoothingRegionIsAnchor.Reserve(AffectedVertices.Num());

        for (const FAffectedVertex& V : AffectedVertices)
        {
            OutRingData.SmoothingRegionIndices.Add(V.VertexIndex);
            OutRingData.SmoothingRegionInfluences.Add(1.0f);
            OutRingData.SmoothingRegionIsAnchor.Add(1);  // Original Affected = anchor
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("PostProcessing: Smoothing disabled, using %d affected vertices (no Z extension)"),
            OutRingData.SmoothingRegionIndices.Num());
        return;
    }

    const float BoundsZTop = Context.RingSettings.SmoothingBoundsZTop;
    const float BoundsZBottom = Context.RingSettings.SmoothingBoundsZBottom;

    // Use original Affected Vertices as-is if no Z extension
    if (BoundsZTop < 0.01f && BoundsZBottom < 0.01f)
    {
        // Copy original
        OutRingData.SmoothingRegionIndices.Reserve(AffectedVertices.Num());
        OutRingData.SmoothingRegionInfluences.Reserve(AffectedVertices.Num());
        OutRingData.SmoothingRegionIsAnchor.Reserve(AffectedVertices.Num());

        for (const FAffectedVertex& V : AffectedVertices)
        {
            OutRingData.SmoothingRegionIndices.Add(V.VertexIndex);
            OutRingData.SmoothingRegionInfluences.Add(1.0f);  // Core vertices = 1.0
            OutRingData.SmoothingRegionIsAnchor.Add(1);  // Original Affected = anchor
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("PostProcessing: No Z extension, using %d affected vertices"),
            OutRingData.SmoothingRegionIndices.Num());
        return;
    }

    // Calculate Z extension range
    const FTransform& LocalToComponent = Context.SDFCache->LocalToComponent;
    const FTransform ComponentToLocal = LocalToComponent.Inverse();

    const FVector OriginalBoundsMin = FVector(Context.SDFCache->BoundsMin);
    const FVector OriginalBoundsMax = FVector(Context.SDFCache->BoundsMax);
    const TArray<FVector3f>& AllVertices = Context.AllVertices;

    // Keep XY original, extend Z only (SmoothingBoundsZTop/Bottom)
    FVector ExtendedBoundsMin = OriginalBoundsMin;
    FVector ExtendedBoundsMax = OriginalBoundsMax;
    ExtendedBoundsMin.Z -= BoundsZBottom;
    ExtendedBoundsMax.Z += BoundsZTop;

    const float OriginalZSize = OriginalBoundsMax.Z - OriginalBoundsMin.Z;

    OutRingData.SmoothingRegionIndices.Reserve(AllVertices.Num() / 4);
    OutRingData.SmoothingRegionInfluences.Reserve(AllVertices.Num() / 4);
    OutRingData.SmoothingRegionIsAnchor.Reserve(AllVertices.Num() / 4);
    // Note: PostProcessingLayerTypes removed - using FullMeshLayerTypes for GPU direct lookup

    int32 CoreCount = 0;
    int32 ExtendedCount = 0;
    int32 AnchorCount = 0;

    for (int32 VertexIdx = 0; VertexIdx < AllVertices.Num(); ++VertexIdx)
    {
        const FVector VertexPos = FVector(AllVertices[VertexIdx]);
        const FVector LocalPos = ComponentToLocal.TransformPosition(VertexPos);

        // Check if within extended Z range (XY uses original range)
        if (LocalPos.X >= OriginalBoundsMin.X && LocalPos.X <= OriginalBoundsMax.X &&
            LocalPos.Y >= OriginalBoundsMin.Y && LocalPos.Y <= OriginalBoundsMax.Y &&
            LocalPos.Z >= ExtendedBoundsMin.Z && LocalPos.Z <= ExtendedBoundsMax.Z)
        {
            // Influence calculation: core(original range) = 1.0, Z extended region = falloff
            float Influence = 1.0f;

            if (LocalPos.Z < OriginalBoundsMin.Z)
            {
                // Bottom extended region: falloff by distance
                float Dist = OriginalBoundsMin.Z - LocalPos.Z;
                Influence = 1.0f - FMath::Clamp(Dist / BoundsZBottom, 0.0f, 1.0f);
                Influence = FMath::InterpEaseInOut(0.0f, 1.0f, Influence, 2.0f);  // Smooth falloff
                ExtendedCount++;
            }
            else if (LocalPos.Z > OriginalBoundsMax.Z)
            {
                // Top extended region: falloff by distance
                float Dist = LocalPos.Z - OriginalBoundsMax.Z;
                Influence = 1.0f - FMath::Clamp(Dist / BoundsZTop, 0.0f, 1.0f);
                Influence = FMath::InterpEaseInOut(0.0f, 1.0f, Influence, 2.0f);  // Smooth falloff
                ExtendedCount++;
            }
            else
            {
                CoreCount++;
            }

            // Anchor determination: only vertices in original AffectedVertices are anchors
            const bool bIsAnchor = AffectedSet.Contains(static_cast<uint32>(VertexIdx));
            if (bIsAnchor) AnchorCount++;

            OutRingData.SmoothingRegionIndices.Add(static_cast<uint32>(VertexIdx));
            OutRingData.SmoothingRegionInfluences.Add(Influence);
            OutRingData.SmoothingRegionIsAnchor.Add(bIsAnchor ? 1 : 0);
            // Note: LayerTypes is looked up directly by GPU from FullMeshLayerTypes
        }
    }

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("PostProcessing: Selected %d vertices (Core=%d, ZExtended=%d, Anchors=%d) for Ring[%d], ZExtend=[%.1f, %.1f]"),
        OutRingData.SmoothingRegionIndices.Num(), CoreCount, ExtendedCount, AnchorCount,
        Context.RingIndex, BoundsZBottom, BoundsZTop);
}

// ============================================================================
// FVirtualBandVertexSelector Implementation
// Vertex selection implementation for Virtual Band mode
// ============================================================================

float FVirtualBandVertexSelector::GetRadiusAtHeight(float LocalZ, const FVirtualBandSettings& BandSettings) const
{
    return BandSettings.GetRadiusAtHeight(LocalZ);
}

float FVirtualBandVertexSelector::CalculateFalloff(float Distance, float MaxDistance, EFalloffType InFalloffType) const
{
    if (MaxDistance < KINDA_SMALL_NUMBER)
    {
        return 1.0f;
    }

    const float NormalizedDist = FMath::Clamp(Distance / MaxDistance, 0.0f, 1.0f);

    switch (InFalloffType)
    {
    case EFalloffType::Linear:
        return 1.0f - NormalizedDist;
    case EFalloffType::Quadratic:
        return 1.0f - NormalizedDist * NormalizedDist;
    case EFalloffType::Hermite:
        // Hermite S-curve: 3t² - 2t³
        return 1.0f - (3.0f * NormalizedDist * NormalizedDist - 2.0f * NormalizedDist * NormalizedDist * NormalizedDist);
    default:
        return 1.0f - NormalizedDist;
    }
}

void FVirtualBandVertexSelector::SelectVertices(
    const FVertexSelectionContext& Context,
    TArray<FAffectedVertex>& OutAffected)
{
    OutAffected.Reset();

    const FFleshRingSettings& Ring = Context.RingSettings;
    const TArray<FVector3f>& AllVertices = Context.AllVertices;

    // Get VirtualBand settings
    const FVirtualBandSettings& BandSettings = Ring.VirtualBand;

    // Calculate band transform (use Virtual Band specific BandOffset/BandRotation)
    const FTransform& BoneTransform = Context.BoneTransform;
    const FQuat BoneRotation = BoneTransform.GetRotation();
    const FVector WorldBandOffset = BoneRotation.RotateVector(BandSettings.BandOffset);
    const FVector BandCenter = BoneTransform.GetLocation() + WorldBandOffset;
    const FQuat WorldBandRotation = BoneRotation * BandSettings.BandRotation;
    const FVector BandAxis = WorldBandRotation.RotateVector(FVector::ZAxisVector);

    // Height parameters
    const float LowerHeight = BandSettings.Lower.Height;
    const float BandHeight = BandSettings.BandHeight;
    const float UpperHeight = BandSettings.Upper.Height;
    const float TotalHeight = LowerHeight + BandHeight + UpperHeight;

    // Tightness region: Band Section only (-BandHeight/2 ~ +BandHeight/2)
    // New coordinate system: Z=0 is Mid Band center
    const float TightnessZMin = -BandHeight * 0.5f;
    const float TightnessZMax = BandHeight * 0.5f;

    // Tightness Falloff range: distance band pushes while tightening
    // Upper/Lower radius difference = bulge amount = distance to tighten
    const float UpperBulge = BandSettings.Upper.Radius - BandSettings.MidUpperRadius;
    const float LowerBulge = BandSettings.Lower.Radius - BandSettings.MidLowerRadius;
    const float TightnessFalloffRange = FMath::Max(FMath::Max(UpperBulge, LowerBulge), 1.0f);  // Ensure minimum 1.0

    OutAffected.Reserve(AllVertices.Num() / 4);

    for (int32 VertexIdx = 0; VertexIdx < AllVertices.Num(); ++VertexIdx)
    {
        // === Layer Type Filtering ===
        if (Context.VertexLayerTypes && Context.VertexLayerTypes->IsValidIndex(VertexIdx))
        {
            const EFleshRingLayerType LayerType = (*Context.VertexLayerTypes)[VertexIdx];
            if (!Context.RingSettings.IsLayerAffected(LayerType))
            {
                continue; // Skip if not in specified layer
            }
        }

        const FVector VertexPos = FVector(AllVertices[VertexIdx]);
        const FVector ToVertex = VertexPos - BandCenter;

        // Axial distance
        const float AxisDistance = FVector::DotProduct(ToVertex, BandAxis);
        const float LocalZ = AxisDistance;

        // Band Section range check (Tightness region)
        if (LocalZ < TightnessZMin || LocalZ > TightnessZMax)
        {
            continue;
        }

        // Radial distance
        const FVector RadialVec = ToVertex - BandAxis * AxisDistance;
        const float RadialDistance = RadialVec.Size();

        // Band radius at this height (variable radius)
        const float BandRadius = GetRadiusAtHeight(LocalZ, BandSettings);

        // Must be outside band surface for Tightness effect
        if (RadialDistance <= BandRadius)
        {
            continue;
        }

        // Distance from band surface
        const float DistanceOutside = RadialDistance - BandRadius;

        // Falloff range check
        if (DistanceOutside > TightnessFalloffRange)
        {
            continue;
        }

        // Radial Influence (higher when closer to surface)
        const float RadialInfluence = CalculateFalloff(DistanceOutside, TightnessFalloffRange, Ring.FalloffType);

        // Axial Influence (falloff by distance from Band boundary)
        float AxialInfluence = 1.0f;
        const float AxialFalloffRange = BandHeight * 0.2f;
        if (LocalZ < TightnessZMin + AxialFalloffRange)
        {
            const float Dist = TightnessZMin + AxialFalloffRange - LocalZ;
            AxialInfluence = CalculateFalloff(Dist, AxialFalloffRange, Ring.FalloffType);
        }
        else if (LocalZ > TightnessZMax - AxialFalloffRange)
        {
            const float Dist = LocalZ - (TightnessZMax - AxialFalloffRange);
            AxialInfluence = CalculateFalloff(Dist, AxialFalloffRange, Ring.FalloffType);
        }

        const float CombinedInfluence = RadialInfluence * AxialInfluence;

        if (CombinedInfluence > KINDA_SMALL_NUMBER)
        {
            OutAffected.Add(FAffectedVertex(
                static_cast<uint32>(VertexIdx),
                RadialDistance,
                CombinedInfluence
            ));
        }
    }

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("VirtualBandSelector: Ring[%d] '%s' - Selected %d vertices"),
        Context.RingIndex, *Ring.BoneName.ToString(), OutAffected.Num());
}

void FVirtualBandVertexSelector::SelectSmoothingRegionVertices(
    const FVertexSelectionContext& Context,
    const TArray<FAffectedVertex>& AffectedVertices,
    FRingAffectedData& OutRingData)
{
    OutRingData.SmoothingRegionIndices.Reset();
    OutRingData.SmoothingRegionInfluences.Reset();
    OutRingData.SmoothingRegionIsAnchor.Reset();

    // Set for fast lookup of original Affected Vertices (for anchor determination)
    TSet<uint32> AffectedSet;
    AffectedSet.Reserve(AffectedVertices.Num());
    for (const FAffectedVertex& V : AffectedVertices)
    {
        AffectedSet.Add(V.VertexIndex);
    }

    // Copy only original Affected Vertices if Smoothing is disabled
    const bool bAnySmoothingEnabled =
        Context.RingSettings.bEnableRadialSmoothing ||
        Context.RingSettings.bEnableLaplacianSmoothing ||
        Context.RingSettings.bEnablePBDEdgeConstraint;

    if (!bAnySmoothingEnabled)
    {
        OutRingData.SmoothingRegionIndices.Reserve(AffectedVertices.Num());
        OutRingData.SmoothingRegionInfluences.Reserve(AffectedVertices.Num());
        OutRingData.SmoothingRegionIsAnchor.Reserve(AffectedVertices.Num());

        for (const FAffectedVertex& V : AffectedVertices)
        {
            OutRingData.SmoothingRegionIndices.Add(V.VertexIndex);
            OutRingData.SmoothingRegionInfluences.Add(1.0f);
            OutRingData.SmoothingRegionIsAnchor.Add(1);  // Original Affected = anchor
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("PostProcessing (VirtualBand): Smoothing disabled, using %d affected vertices"),
            OutRingData.SmoothingRegionIndices.Num());
        return;
    }

    const float BoundsZTop = Context.RingSettings.SmoothingBoundsZTop;
    const float BoundsZBottom = Context.RingSettings.SmoothingBoundsZBottom;
    const FFleshRingSettings& Ring = Context.RingSettings;
    const TArray<FVector3f>& AllVertices = Context.AllVertices;
    const FVirtualBandSettings& BandSettings = Ring.VirtualBand;

    // Use original Affected Vertices as-is if no Z extension
    if (BoundsZTop < 0.01f && BoundsZBottom < 0.01f)
    {
        OutRingData.SmoothingRegionIndices.Reserve(AffectedVertices.Num());
        OutRingData.SmoothingRegionInfluences.Reserve(AffectedVertices.Num());
        OutRingData.SmoothingRegionIsAnchor.Reserve(AffectedVertices.Num());

        for (const FAffectedVertex& V : AffectedVertices)
        {
            OutRingData.SmoothingRegionIndices.Add(V.VertexIndex);
            OutRingData.SmoothingRegionInfluences.Add(1.0f);
            OutRingData.SmoothingRegionIsAnchor.Add(1);  // Original Affected = anchor
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("PostProcessing (VirtualBand): No Z extension, using %d affected vertices"),
            OutRingData.SmoothingRegionIndices.Num());
        return;
    }

    // Calculate band transform (use Virtual Band specific BandOffset/BandRotation)
    const FQuat BoneRotation = Context.BoneTransform.GetRotation();
    const FVector WorldBandOffset = BoneRotation.RotateVector(BandSettings.BandOffset);
    const FVector BandCenter = Context.BoneTransform.GetLocation() + WorldBandOffset;
    const FQuat WorldBandRotation = BoneRotation * BandSettings.BandRotation;
    const FVector BandAxis = WorldBandRotation.RotateVector(FVector::ZAxisVector);

    // Virtual Band total height
    const float LowerHeight = BandSettings.Lower.Height;
    const float BandHeight = BandSettings.BandHeight;
    const float UpperHeight = BandSettings.Upper.Height;
    const float TotalHeight = LowerHeight + BandHeight + UpperHeight;

    // New coordinate system: Z=0 is Mid Band center
    const float MidOffset = LowerHeight + BandHeight * 0.5f;
    const float ZMin = -MidOffset;
    const float ZMax = TotalHeight - MidOffset;

    // Extended Z range (entire Virtual Band + Z extension)
    const float ExtendedZMin = ZMin - BoundsZBottom;
    const float ExtendedZMax = ZMax + BoundsZTop;

    // Calculate max radius (for AABB query)
    const float MaxRadius = FMath::Max(
        FMath::Max(BandSettings.Lower.Radius, BandSettings.Upper.Radius),
        FMath::Max(BandSettings.MidLowerRadius, BandSettings.MidUpperRadius)
    ) + Ring.RingThickness;

    OutRingData.SmoothingRegionIndices.Reserve(AllVertices.Num() / 4);
    OutRingData.SmoothingRegionInfluences.Reserve(AllVertices.Num() / 4);
    OutRingData.SmoothingRegionIsAnchor.Reserve(AllVertices.Num() / 4);

    int32 CoreCount = 0;
    int32 ExtendedCount = 0;

    for (int32 VertexIdx = 0; VertexIdx < AllVertices.Num(); ++VertexIdx)
    {
        const FVector VertexPos = FVector(AllVertices[VertexIdx]);
        const FVector ToVertex = VertexPos - BandCenter;

        // Axial distance
        const float AxisDistance = FVector::DotProduct(ToVertex, BandAxis);
        const float LocalZ = AxisDistance;

        // Extended Z range check
        if (LocalZ < ExtendedZMin || LocalZ > ExtendedZMax)
        {
            continue;
        }

        // Radial distance
        const FVector RadialVec = ToVertex - BandAxis * AxisDistance;
        const float RadialDistance = RadialVec.Size();

        // Band radius at this height (variable radius, Z range clamped)
        const float ClampedZ = FMath::Clamp(LocalZ, ZMin, ZMax);
        const float BandRadius = GetRadiusAtHeight(ClampedZ, BandSettings);

        // Radial range check (near band)
        if (RadialDistance > BandRadius + Ring.RingThickness)
        {
            continue;
        }

        OutRingData.SmoothingRegionIndices.Add(static_cast<uint32>(VertexIdx));

        // Anchor determination: anchor if included in original Affected Vertices
        const bool bIsAnchor = AffectedSet.Contains(static_cast<uint32>(VertexIdx));

        // Influence calculation: core(within Band Section) = 1.0, Z extended region = falloff
        float Influence = 1.0f;

        // Core region: Band Section (-BandHeight/2 ~ +BandHeight/2)
        // New coordinate system: Z=0 is Mid Band center
        const float CoreZMin = -BandHeight * 0.5f;
        const float CoreZMax = BandHeight * 0.5f;

        if (LocalZ < CoreZMin)
        {
            // Bottom extended region (Lower Section + Z extension)
            const float DistFromCore = CoreZMin - LocalZ;
            const float MaxExtension = LowerHeight + BoundsZBottom;
            Influence = 1.0f - FMath::Clamp(DistFromCore / FMath::Max(MaxExtension, 0.01f), 0.0f, 1.0f);
            Influence = FMath::InterpEaseInOut(0.0f, 1.0f, Influence, 2.0f);
            ExtendedCount++;
        }
        else if (LocalZ > CoreZMax)
        {
            // Top extended region (Upper Section + Z extension)
            const float DistFromCore = LocalZ - CoreZMax;
            const float MaxExtension = UpperHeight + BoundsZTop;
            Influence = 1.0f - FMath::Clamp(DistFromCore / FMath::Max(MaxExtension, 0.01f), 0.0f, 1.0f);
            Influence = FMath::InterpEaseInOut(0.0f, 1.0f, Influence, 2.0f);
            ExtendedCount++;
        }
        else
        {
            CoreCount++;
        }

        OutRingData.SmoothingRegionInfluences.Add(Influence);
        OutRingData.SmoothingRegionIsAnchor.Add(bIsAnchor ? 1 : 0);
    }

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("PostProcessing (VirtualBand): Selected %d vertices (Core=%d, Extended=%d) for Ring[%d]"),
        OutRingData.SmoothingRegionIndices.Num(), CoreCount, ExtendedCount, Context.RingIndex);
}

// ============================================================================
// Affected Vertices Manager Implementation
// ============================================================================

FFleshRingAffectedVerticesManager::FFleshRingAffectedVerticesManager()
{
    // Default to distance-based selector
    VertexSelector = MakeShared<FDistanceBasedVertexSelector>();
}

FFleshRingAffectedVerticesManager::~FFleshRingAffectedVerticesManager()
{
    ClearAll();
}

void FFleshRingAffectedVerticesManager::SetVertexSelector(TSharedPtr<IVertexSelector> InSelector)
{
    if (InSelector)
    {
        VertexSelector = InSelector;
        UE_LOG(LogFleshRingVertices, Log,
            TEXT("VertexSelector changed to: %s"),
            *VertexSelector->GetStrategyName());
    }
}

// ============================================================================
// RegisterAffectedVertices - Register affected vertices
// ============================================================================
bool FFleshRingAffectedVerticesManager::RegisterAffectedVertices(
    const UFleshRingComponent* Component,
    const USkeletalMeshComponent* SkeletalMesh,
    int32 LODIndex)
{
    // Validate input parameters
    if (!Component || !SkeletalMesh || !VertexSelector)
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("RegisterAffectedVertices: Invalid parameters"));
        return false;
    }

    // RingDataArray resizing handled in SetNum() logic below
    // (Dirty Flag system preserves cached data for clean Rings)

    // FleshRingAsset null check
    if (!Component->FleshRingAsset)
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("RegisterAffectedVertices: FleshRingAsset is null"));
        return false;
    }

    const TArray<FFleshRingSettings>& Rings = Component->FleshRingAsset->Rings;

    // ================================================================
    // Mesh data caching: bind pose is immutable, extract only once
    // ================================================================
    if (!bMeshDataCached)
    {
        // Extract mesh vertices from skeletal mesh at specified LOD (bind pose component space)
        if (!ExtractMeshVertices(SkeletalMesh, CachedMeshVertices, LODIndex))
        {
            UE_LOG(LogFleshRingVertices, Error,
                TEXT("RegisterAffectedVertices: Failed to extract mesh vertices"));
            return false;
        }

        // Build Spatial Hash for O(1) vertex queries
        VertexSpatialHash.Build(CachedMeshVertices);

        // Extract mesh indices for adjacency data (for Normal recomputation)
        CachedMeshIndices.Reset();
        if (!ExtractMeshIndices(SkeletalMesh, CachedMeshIndices, LODIndex))
        {
            UE_LOG(LogFleshRingVertices, Warning,
                TEXT("RegisterAffectedVertices: Failed to extract mesh indices, Normal recomputation will be disabled"));
        }

        // Build topology cache (along with mesh data caching)
        // Always build regardless of Laplacian smoothing to leverage cache in all functions
        // Previously only built inside BuildLaplacianAdjacencyData(), so
        // BuildRepresentativeIndices(), BuildAdjacencyData() etc. couldn't use the cache
        if (CachedMeshIndices.Num() > 0 && !bTopologyCacheBuilt)
        {
            BuildTopologyCache(CachedMeshVertices, CachedMeshIndices);
        }

        bMeshDataCached = true;
        UE_LOG(LogFleshRingVertices, Log,
            TEXT("RegisterAffectedVertices: Cached mesh data (%d vertices, %d indices, SpatialHash built, TopologyCache=%s)"),
            CachedMeshVertices.Num(), CachedMeshIndices.Num(), bTopologyCacheBuilt ? TEXT("Yes") : TEXT("No"));
    }

    // ================================================================
    // Rebuild layer types every time to reflect MaterialLayerMappings changes
    // ================================================================
    RebuildVertexLayerTypes(Component, SkeletalMesh, LODIndex);

    // Local references (for compatibility with subsequent code)
    const TArray<FVector3f>& MeshVertices = CachedMeshVertices;
    const TArray<EFleshRingLayerType>& VertexLayerTypes = CachedVertexLayerTypes;

    // ================================================================
    // Initialize dirty flag system
    // ================================================================
    const int32 NumRings = Rings.Num();

    // Resize RingDataArray
    if (RingDataArray.Num() != NumRings)
    {
        RingDataArray.SetNum(NumRings);
    }

    // Resize RingDirtyFlags (preserve existing dirty state, only mark new elements as dirty)
    if (RingDirtyFlags.Num() != NumRings)
    {
        const int32 OldNum = RingDirtyFlags.Num();
        RingDirtyFlags.SetNum(NumRings);
        // Only mark newly added Rings as dirty (preserve existing state)
        for (int32 i = OldNum; i < NumRings; ++i)
        {
            RingDirtyFlags[i] = true;
        }
        UE_LOG(LogFleshRingVertices, Log, TEXT("RegisterAffectedVertices: Resized dirty flags %d -> %d rings (new rings marked dirty)"), OldNum, NumRings);
    }

    // Process each Ring
    for (int32 RingIdx = 0; RingIdx < NumRings; ++RingIdx)
    {
        const FFleshRingSettings& RingSettings = Rings[RingIdx];

        // ===== Dirty Flag check: Skip clean Rings =====
        if (!RingDirtyFlags[RingIdx])
        {
            // Already has valid data and not changed - skip
            UE_LOG(LogFleshRingVertices, Log, TEXT("Ring[%d]: SKIPPED (not dirty)"), RingIdx);
            continue;
        }
        UE_LOG(LogFleshRingVertices, Log, TEXT("Ring[%d]: PROCESSING (dirty)"), RingIdx);

        // Skip Rings without valid bone
        if (RingSettings.BoneName == NAME_None)
        {
            UE_LOG(LogFleshRingVertices, Warning,
                TEXT("Ring[%d]: Skipping - no bone assigned"), RingIdx);
            RingDirtyFlags[RingIdx] = false;  // Mark as processed
            continue;
        }

        // Get bone index from skeletal mesh
        const int32 BoneIndex = SkeletalMesh->GetBoneIndex(RingSettings.BoneName);
        if (BoneIndex == INDEX_NONE)
        {
            UE_LOG(LogFleshRingVertices, Warning,
                TEXT("Ring[%d]: Bone '%s' not found"), RingIdx, *RingSettings.BoneName.ToString());
            RingDirtyFlags[RingIdx] = false;  // Error but mark as processed
            continue;
        }

        // Get skeletal mesh asset for reference skeleton
        USkeletalMesh* SkelMeshAsset = SkeletalMesh->GetSkeletalMeshAsset();
        if (!SkelMeshAsset)
        {
            UE_LOG(LogFleshRingVertices, Warning,
                TEXT("Ring[%d]: SkeletalMesh asset is null"), RingIdx);
            RingDirtyFlags[RingIdx] = false;  // Error but mark as processed
            continue;
        }

        // Calculate bind pose component space transform
        // (MeshVertices are in bind pose local coordinates, so need same coordinate system)
        const FReferenceSkeleton& RefSkeleton = SkelMeshAsset->GetRefSkeleton();
        const TArray<FTransform>& RefBonePose = RefSkeleton.GetRefBonePose();

        // Accumulate transforms through parent chain
        FTransform BoneTransform = FTransform::Identity;
        int32 CurrentBoneIdx = BoneIndex;
        while (CurrentBoneIdx != INDEX_NONE)
        {
            BoneTransform = BoneTransform * RefBonePose[CurrentBoneIdx];
            CurrentBoneIdx = RefSkeleton.GetParentIndex(CurrentBoneIdx);
        }

        UE_LOG(LogFleshRingVertices, Verbose,
            TEXT("Ring[%d] '%s': RefPose Center=(%.2f, %.2f, %.2f)"),
            RingIdx, *RingSettings.BoneName.ToString(),
            BoneTransform.GetLocation().X, BoneTransform.GetLocation().Y, BoneTransform.GetLocation().Z);

        // ================================================================
        // Create Ring data (FFleshRingSettings → FRingAffectedData)
        // ================================================================
        FRingAffectedData RingData;

        // Ring Information (from bone transform)
        RingData.BoneName = RingSettings.BoneName;

        const FQuat BoneRotation = BoneTransform.GetRotation();

        // Branch RingCenter/RingAxis/Geometry calculation based on InfluenceMode
        if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::VirtualRing)
        {
            // ===== VirtualRing mode: Use RingOffset/RingRotation =====
            const FVector WorldRingOffset = BoneRotation.RotateVector(RingSettings.RingOffset);
            RingData.RingCenter = BoneTransform.GetLocation() + WorldRingOffset;

            const FQuat WorldRingRotation = BoneRotation * RingSettings.RingRotation;
            RingData.RingAxis = WorldRingRotation.RotateVector(FVector::ZAxisVector);

            // VirtualRing mode uses values directly without scale
            RingData.RingRadius = RingSettings.RingRadius;
            RingData.RingThickness = RingSettings.RingThickness;
            RingData.RingHeight = RingSettings.RingHeight;
        }
        else if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
        {
            // ===== Virtual Band mode: Use dedicated BandOffset/BandRotation =====
            const FVirtualBandSettings& BandSettings = RingSettings.VirtualBand;
            const FVector WorldBandOffset = BoneRotation.RotateVector(BandSettings.BandOffset);
            RingData.RingCenter = BoneTransform.GetLocation() + WorldBandOffset;

            const FQuat WorldBandRotation = BoneRotation * BandSettings.BandRotation;
            RingData.RingAxis = WorldBandRotation.RotateVector(FVector::ZAxisVector);

            // VirtualBand uses radius from band settings
            RingData.RingRadius = BandSettings.MidUpperRadius;
            RingData.RingThickness = BandSettings.BandThickness;
            RingData.RingHeight = BandSettings.BandHeight;
        }
        else
        {
            // ===== Auto mode: Apply MeshOffset/MeshRotation + MeshScale (SDF-based) =====
            const FVector WorldMeshOffset = BoneRotation.RotateVector(RingSettings.MeshOffset);
            RingData.RingCenter = BoneTransform.GetLocation() + WorldMeshOffset;

            const FQuat WorldMeshRotation = BoneRotation * RingSettings.MeshRotation;
            RingData.RingAxis = WorldMeshRotation.RotateVector(FVector::ZAxisVector);

            // Apply MeshScale: separate radial (X, Y average) and axial (Z) directions
            const float RadialScale = (RingSettings.MeshScale.X + RingSettings.MeshScale.Y) * 0.5f;
            const float AxialScale = RingSettings.MeshScale.Z;

            RingData.RingRadius = RingSettings.RingRadius * RadialScale;
            RingData.RingThickness = RingSettings.RingThickness * RadialScale;
            RingData.RingHeight = RingSettings.RingHeight * AxialScale;
        }

        // Deformation Parameters (copy from asset)
        RingData.TightnessStrength = RingSettings.TightnessStrength;
        RingData.FalloffType = RingSettings.FalloffType;

        // ================================================================
        // Build Context and select affected vertices
        // ================================================================
        const FRingSDFCache* SDFCache = Component->GetRingSDFCache(RingIdx);

        FVertexSelectionContext Context(
            RingSettings,
            RingIdx,
            BoneTransform,
            MeshVertices,
            SDFCache,  // nullptr means SDF not used (Distance-based Selector ignores)
            &VertexSpatialHash,  // Spatial Hash for O(1) vertex queries
            bTopologyCacheBuilt ? &CachedPositionToVertices : nullptr,  // UV seam welding cache (only if built)
            &CachedVertexLayerTypes  // For layer-based vertex filtering
        );

        // Determine Selector based on per-Ring InfluenceMode
        // Auto mode + SDF valid → SDFBoundsBasedSelector
        // VirtualRing/VirtualBand mode or SDF invalid → DistanceBasedSelector/VirtualBandVertexSelector
        TSharedPtr<IVertexSelector> RingSelector;
        const bool bUseSDFForThisRing =
            (RingSettings.InfluenceMode == EFleshRingInfluenceMode::Auto) &&
            (SDFCache && SDFCache->IsValid());

        if (bUseSDFForThisRing)
        {
            RingSelector = MakeShared<FSDFBoundsBasedVertexSelector>();
        }
        else if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
        {
            // VirtualBand mode + SDF invalid → VirtualBandVertexSelector (distance-based variable radius)
            RingSelector = MakeShared<FVirtualBandVertexSelector>();
        }
        else
        {
            RingSelector = MakeShared<FDistanceBasedVertexSelector>();
        }

        // Determine InfluenceMode name
        const TCHAR* InfluenceModeStr = TEXT("VirtualRing");
        if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::Auto)
        {
            InfluenceModeStr = TEXT("Auto");
        }
        else if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
        {
            InfluenceModeStr = TEXT("VirtualBand");
        }

        // Determine Selector name
        const TCHAR* SelectorStr = TEXT("DistanceBasedSelector");
        if (bUseSDFForThisRing)
        {
            SelectorStr = TEXT("SDFBoundsBasedSelector");
        }
        else if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
        {
            SelectorStr = TEXT("VirtualBandVertexSelector");
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("Ring[%d] '%s': Using %s (InfluenceMode=%s, SDFValid=%s)"),
            RingIdx, *RingSettings.BoneName.ToString(),
            SelectorStr,
            InfluenceModeStr,
            (SDFCache && SDFCache->IsValid()) ? TEXT("Yes") : TEXT("No"));

        // Select affected vertices using per-Ring Selector
        RingSelector->SelectVertices(Context, RingData.Vertices);

        // ================================================================
        // Select post-processing vertices (Z-extended range)
        // ================================================================
        // Design:
        // - Affected Vertices (PackedIndices) = original AABB → Tightness deformation target
        // - Post-Processing Vertices = original AABB + SmoothingBoundsZTop/Bottom → smoothing/penetration resolution etc.
        if (bUseSDFForThisRing)
        {
            // SDF mode: Z extension based on SDF bounds
            FSDFBoundsBasedVertexSelector* SDFSelector = static_cast<FSDFBoundsBasedVertexSelector*>(RingSelector.Get());
            SDFSelector->SelectSmoothingRegionVertices(Context, RingData.Vertices, RingData);
            // Note: LayerTypes are queried directly by GPU from FullMeshLayerTypes
        }
        else if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
        {
            // VirtualBand mode (SDF invalid): Z extension based on VirtualBand
            FVirtualBandVertexSelector* VBSelector = static_cast<FVirtualBandVertexSelector*>(RingSelector.Get());
            VBSelector->SelectSmoothingRegionVertices(Context, RingData.Vertices, RingData);
            // Note: LayerTypes are queried directly by GPU from FullMeshLayerTypes
        }
        else
        {
            // VirtualRing mode: Z extension based on Ring parameters
            FDistanceBasedVertexSelector* DistSelector = static_cast<FDistanceBasedVertexSelector*>(RingSelector.Get());
            DistSelector->SelectSmoothingRegionVertices(Context, RingData.Vertices, RingData);
            // Note: LayerTypes are queried directly by GPU from FullMeshLayerTypes
        }

        // Pack for GPU (convert to flat arrays)
        RingData.PackForGPU();

        // Build representative indices for UV seam welding
        // This data is used in all deformation passes to ensure UV duplicates move identically
        BuildRepresentativeIndices(RingData, MeshVertices);

        // Build adjacency data for Normal recomputation
        if (CachedMeshIndices.Num() > 0)
        {
            BuildAdjacencyData(RingData, CachedMeshIndices);

            // Also build normal adjacency data for post-processing vertices (Z-extended range)
            if (RingData.SmoothingRegionIndices.Num() > 0)
            {
                BuildSmoothingRegionNormalAdjacency(RingData, CachedMeshIndices);
            }

            // Build Laplacian adjacency data for smoothing (conditional: only when smoothing is enabled)
            // Improvement: includes only neighbors of the same layer to prevent layer boundary mixing
            if (RingSettings.bEnableLaplacianSmoothing)
            {
                BuildLaplacianAdjacencyData(RingData, CachedMeshIndices, MeshVertices, VertexLayerTypes);

                // Also build Laplacian adjacency data for post-processing vertices (Z-extended range)
                if (RingData.SmoothingRegionIndices.Num() > 0)
                {
                    BuildSmoothingRegionLaplacianAdjacency(RingData, CachedMeshIndices, MeshVertices, VertexLayerTypes);
                }
            }

            // Build PBD adjacency data (conditional: only when PBD is enabled)
            if (RingSettings.bEnablePBDEdgeConstraint)
            {
                BuildPBDAdjacencyData(RingData, CachedMeshIndices, MeshVertices, MeshVertices.Num());

                // Also build PBD adjacency data for post-processing vertices (Z-extended range)
                if (RingData.SmoothingRegionIndices.Num() > 0)
                {
                    BuildSmoothingRegionPBDAdjacency(RingData, CachedMeshIndices, MeshVertices, MeshVertices.Num());
                }
            }

            // Build slice data for bone ratio preservation (for Radial Smoothing)
            // GPU dispatch checks bEnableRadialSmoothing so always build here
            BuildSliceData(RingData, MeshVertices, RingSettings.RadialSliceHeight);

            // Build hop distance data for topology-based smoothing (HopBased mode only)
            // Important: only called in HopBased mode - BoundsExpand mode preserves SelectSmoothingRegionVertices data
            const bool bUseHopBased = (RingSettings.SmoothingVolumeMode == ESmoothingVolumeMode::HopBased);
            const bool bAnySmoothingEnabled =
                RingSettings.bEnableRadialSmoothing ||
                RingSettings.bEnableLaplacianSmoothing ||
                RingSettings.bEnablePBDEdgeConstraint ||
                RingSettings.bEnableHeatPropagation;  // Heat Propagation also needs Extended data

            if (bUseHopBased && bAnySmoothingEnabled)
            {
                // HopBased mode: build expanded region via BFS (overwrites SmoothingRegion*)
                BuildHopDistanceData(
                    RingData,
                    CachedMeshIndices,
                    MeshVertices,
                    RingSettings.MaxSmoothingHops,
                    RingSettings.HopFalloffType
                );
            }
            // BoundsExpand mode: preserve data set by SelectSmoothingRegionVertices
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("Ring[%d] '%s': %d affected, %d slices, %d extended smoothing"),
            RingIdx, *RingSettings.BoneName.ToString(),
            RingData.Vertices.Num(), RingData.SlicePackedData.Num() / 33, RingData.SmoothingRegionIndices.Num());

        // Index-based assignment (instead of Add) + clear dirty flag
        RingDataArray[RingIdx] = MoveTemp(RingData);
        RingDirtyFlags[RingIdx] = false;
    }

    // Calculate actual processed ring count
    int32 ProcessedCount = 0;
    for (int32 i = 0; i < NumRings; ++i)
    {
        if (!RingDirtyFlags[i])
        {
            ProcessedCount++;
        }
    }

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("RegisterAffectedVertices: Complete. Total affected: %d, Processed rings: %d/%d"),
        GetTotalAffectedCount(), ProcessedCount, NumRings);

    return true;
}

const FRingAffectedData* FFleshRingAffectedVerticesManager::GetRingData(int32 RingIndex) const
{
    if (RingDataArray.IsValidIndex(RingIndex))
    {
        return &RingDataArray[RingIndex];
    }
    return nullptr;
}

void FFleshRingAffectedVerticesManager::ClearAll()
{
    // Empty() releases memory completely (Reset() keeps memory)
    RingDataArray.Empty();

    // Invalidate topology cache
    InvalidateTopologyCache();

    // Also release cached mesh data (prevent memory leak)
    CachedMeshIndices.Empty();
    CachedMeshVertices.Empty();
    CachedVertexLayerTypes.Empty();
    bMeshDataCached = false;

    // Release Spatial Hash
    VertexSpatialHash.Clear();

    // Release Dirty Flags
    RingDirtyFlags.Empty();
}

int32 FFleshRingAffectedVerticesManager::GetTotalAffectedCount() const
{
    int32 Total = 0;
    for (const FRingAffectedData& RingData : RingDataArray)
    {
        Total += RingData.Vertices.Num();
    }
    return Total;
}

// ============================================================================
// Per-Ring Dirty Flag System - manages per-Ring rebuild requirements
// ============================================================================

void FFleshRingAffectedVerticesManager::MarkRingDirty(int32 RingIndex)
{
    if (RingDirtyFlags.IsValidIndex(RingIndex))
    {
        RingDirtyFlags[RingIndex] = true;
    }
}

void FFleshRingAffectedVerticesManager::MarkAllRingsDirty()
{
    for (int32 i = 0; i < RingDirtyFlags.Num(); ++i)
    {
        RingDirtyFlags[i] = true;
    }
}

bool FFleshRingAffectedVerticesManager::IsRingDirty(int32 RingIndex) const
{
    if (RingDirtyFlags.IsValidIndex(RingIndex))
    {
        return RingDirtyFlags[RingIndex];
    }
    // Consider dirty if flag doesn't exist (first build)
    return true;
}

// ============================================================================
// BuildTopologyCache - build topology cache (once per mesh)
// ============================================================================
// Mesh topology data is determined at bind pose and does not change at runtime.
// - Position-based vertex groups (for UV seam welding)
// - Vertex neighbor map (direct mesh connectivity)
// - Welded neighbor position map (UV seam aware)
// - Full mesh adjacency map (for BFS/hop calculation)
// After calling this function once, all subsequent Ring updates can use O(1) lookups.

void FFleshRingAffectedVerticesManager::BuildTopologyCache(
    const TArray<FVector3f>& AllVertices,
    const TArray<uint32>& MeshIndices)
{
    // Skip if cache is already built
    if (bTopologyCacheBuilt)
    {
        return;
    }

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("BuildTopologyCache: Building topology cache for %d vertices, %d indices"),
        AllVertices.Num(), MeshIndices.Num());

    const double StartTime = FPlatformTime::Seconds();

    // ================================================================
    // Step 1: Build position-based vertex groups for UV seam welding
    // ================================================================
    constexpr float WeldPrecision = 0.001f;  // 0.001 units tolerance

    CachedPositionToVertices.Reset();
    CachedVertexToPosition.Reset();

    for (int32 i = 0; i < AllVertices.Num(); ++i)
    {
        const FVector3f& Pos = AllVertices[i];
        FIntVector PosKey(
            FMath::RoundToInt(Pos.X / WeldPrecision),
            FMath::RoundToInt(Pos.Y / WeldPrecision),
            FMath::RoundToInt(Pos.Z / WeldPrecision)
        );
        CachedPositionToVertices.FindOrAdd(PosKey).Add(static_cast<uint32>(i));
        CachedVertexToPosition.Add(static_cast<uint32>(i), PosKey);
    }

    const int32 WeldedPositionCount = CachedPositionToVertices.Num();
    const int32 DuplicateVertexCount = AllVertices.Num() - WeldedPositionCount;

    // ================================================================
    // Step 1.5: Build representative vertex map for UV seam welding
    // ================================================================
    // Representative vertex for each position = minimum index among vertices at that position
    // Removed O(A) map building from BuildRepresentativeIndices() for O(1) lookup optimization
    CachedPositionToRepresentative.Reset();
    CachedPositionToRepresentative.Reserve(CachedPositionToVertices.Num());

    for (const auto& PosEntry : CachedPositionToVertices)
    {
        uint32 MinIdx = MAX_uint32;
        for (uint32 VertIdx : PosEntry.Value)
        {
            MinIdx = FMath::Min(MinIdx, VertIdx);
        }
        CachedPositionToRepresentative.Add(PosEntry.Key, MinIdx);
    }

    // ================================================================
    // Step 2: Build global vertex neighbor map from mesh triangles
    // ================================================================
    CachedVertexNeighbors.Reset();

    const int32 NumTriangles = MeshIndices.Num() / 3;
    for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
    {
        const uint32 I0 = MeshIndices[TriIdx * 3 + 0];
        const uint32 I1 = MeshIndices[TriIdx * 3 + 1];
        const uint32 I2 = MeshIndices[TriIdx * 3 + 2];

        CachedVertexNeighbors.FindOrAdd(I0).Add(I1);
        CachedVertexNeighbors.FindOrAdd(I0).Add(I2);
        CachedVertexNeighbors.FindOrAdd(I1).Add(I0);
        CachedVertexNeighbors.FindOrAdd(I1).Add(I2);
        CachedVertexNeighbors.FindOrAdd(I2).Add(I0);
        CachedVertexNeighbors.FindOrAdd(I2).Add(I1);
    }

    // ================================================================
    // Step 3: Build welded neighbor map (merge neighbors across UV duplicates)
    // ================================================================
    CachedWeldedNeighborPositions.Reset();

    for (const auto& PosEntry : CachedPositionToVertices)
    {
        const FIntVector& PosKey = PosEntry.Key;
        const TArray<uint32>& VerticesAtPos = PosEntry.Value;

        // Merge all neighbors from all vertices at this position
        TSet<FIntVector> MergedNeighborPositions;

        for (uint32 VertIdx : VerticesAtPos)
        {
            const TSet<uint32>* Neighbors = CachedVertexNeighbors.Find(VertIdx);
            if (Neighbors)
            {
                for (uint32 NeighborIdx : *Neighbors)
                {
                    const FIntVector* NeighborPosKey = CachedVertexToPosition.Find(NeighborIdx);
                    if (NeighborPosKey)
                    {
                        // Exclude self position (UV duplicates are conceptually the same vertex)
                        if (*NeighborPosKey != PosKey)
                        {
                            MergedNeighborPositions.Add(*NeighborPosKey);
                        }
                    }
                }
            }
        }

        CachedWeldedNeighborPositions.Add(PosKey, MoveTemp(MergedNeighborPositions));
    }

    // ================================================================
    // Step 4: Build full mesh adjacency map for BFS/hop distance
    // ================================================================
    CachedFullAdjacencyMap.Reset();
    CachedFullAdjacencyMap.Reserve(AllVertices.Num());

    for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
    {
        const uint32 I0 = MeshIndices[TriIdx * 3 + 0];
        const uint32 I1 = MeshIndices[TriIdx * 3 + 1];
        const uint32 I2 = MeshIndices[TriIdx * 3 + 2];

        // Add bidirectional adjacency for each edge
        auto AddEdge = [this](uint32 A, uint32 B)
        {
            TArray<uint32>& NeighborsA = CachedFullAdjacencyMap.FindOrAdd(A);
            if (!NeighborsA.Contains(B))
            {
                NeighborsA.Add(B);
            }

            TArray<uint32>& NeighborsB = CachedFullAdjacencyMap.FindOrAdd(B);
            if (!NeighborsB.Contains(A))
            {
                NeighborsB.Add(A);
            }
        };

        AddEdge(I0, I1);
        AddEdge(I1, I2);
        AddEdge(I2, I0);
    }

    // ================================================================
    // Step 4.5: Add UV duplicate connections to adjacency map
    // ================================================================
    // Problem: BFS following mesh topology only cannot cross UV seam
    // Solution: Connect UV duplicates at the same position
    //           By directly connecting A and A' at seam like A --+-- A'
    //           BFS can explore the other side of seam via A → A' → E, F
    int32 NumUVDuplicateEdges = 0;
    for (const auto& PosEntry : CachedPositionToVertices)
    {
        const TArray<uint32>& VerticesAtPos = PosEntry.Value;
        const int32 NumAtPos = VerticesAtPos.Num();

        // Only process if there are 2 or more UV duplicates
        if (NumAtPos >= 2)
        {
            // Connect all UV duplicate pairs
            for (int32 i = 0; i < NumAtPos; ++i)
            {
                for (int32 j = i + 1; j < NumAtPos; ++j)
                {
                    const uint32 V1 = VerticesAtPos[i];
                    const uint32 V2 = VerticesAtPos[j];

                    TArray<uint32>& Neighbors1 = CachedFullAdjacencyMap.FindOrAdd(V1);
                    if (!Neighbors1.Contains(V2))
                    {
                        Neighbors1.Add(V2);
                        NumUVDuplicateEdges++;
                    }

                    TArray<uint32>& Neighbors2 = CachedFullAdjacencyMap.FindOrAdd(V2);
                    if (!Neighbors2.Contains(V1))
                    {
                        Neighbors2.Add(V1);
                        NumUVDuplicateEdges++;
                    }
                }
            }
        }
    }

    if (NumUVDuplicateEdges > 0)
    {
        UE_LOG(LogFleshRingVertices, Verbose,
            TEXT("BuildTopologyCache: Added %d UV duplicate edges for BFS seam crossing"),
            NumUVDuplicateEdges);
    }

    // ================================================================
    // Step 5: Build per-vertex triangle list for adjacency lookup
    // ================================================================
    // Used for O(T) → O(avg_triangles_per_vertex) optimization in BuildAdjacencyData()
    CachedVertexTriangles.Reset();
    CachedVertexTriangles.Reserve(AllVertices.Num());

    for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
    {
        const uint32 I0 = MeshIndices[TriIdx * 3 + 0];
        const uint32 I1 = MeshIndices[TriIdx * 3 + 1];
        const uint32 I2 = MeshIndices[TriIdx * 3 + 2];

        CachedVertexTriangles.FindOrAdd(I0).Add(TriIdx);
        CachedVertexTriangles.FindOrAdd(I1).Add(TriIdx);
        CachedVertexTriangles.FindOrAdd(I2).Add(TriIdx);
    }

    // Mark cache build complete
    bTopologyCacheBuilt = true;

    const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("BuildTopologyCache: Completed in %.2f ms - %d vertices -> %d welded positions (%d duplicates), %d adjacency entries"),
        ElapsedMs, AllVertices.Num(), WeldedPositionCount, DuplicateVertexCount, CachedFullAdjacencyMap.Num());
}

// ============================================================================
// InvalidateTopologyCache - invalidate topology cache
// ============================================================================

void FFleshRingAffectedVerticesManager::InvalidateTopologyCache()
{
    bTopologyCacheBuilt = false;
    CachedPositionToVertices.Empty();
    CachedVertexToPosition.Empty();
    CachedPositionToRepresentative.Empty();
    CachedVertexNeighbors.Empty();
    CachedWeldedNeighborPositions.Empty();
    CachedFullAdjacencyMap.Empty();
    CachedVertexTriangles.Empty();

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("InvalidateTopologyCache: Topology cache cleared"));
}

void FFleshRingAffectedVerticesManager::RebuildVertexLayerTypes(const UFleshRingComponent* Component, const USkeletalMeshComponent* SkeletalMesh, int32 LODIndex)
{
    if (!Component || !Component->FleshRingAsset || !SkeletalMesh)
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("RebuildVertexLayerTypes: Missing Component, Asset, or SkeletalMesh"));
        return;
    }

    bool bUsedAssetMapping = false;

    // Get mapping from asset
    if (Component->FleshRingAsset->MaterialLayerMappings.Num() > 0)
    {
        const UFleshRingAsset* Asset = Component->FleshRingAsset;

        // Assign layer type per section
        USkeletalMesh* Mesh = SkeletalMesh->GetSkeletalMeshAsset();
        if (Mesh)
        {
            const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
            if (RenderData && LODIndex < RenderData->LODRenderData.Num())
            {
                const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];
                const int32 NumVertices = static_cast<int32>(LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices());

                CachedVertexLayerTypes.SetNum(NumVertices);
                for (int32 i = 0; i < NumVertices; ++i)
                {
                    CachedVertexLayerTypes[i] = EFleshRingLayerType::Other;
                }

                // Get layer type from each section's material slot
                for (int32 SectionIdx = 0; SectionIdx < LODData.RenderSections.Num(); ++SectionIdx)
                {
                    const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIdx];
                    const int32 MaterialSlotIndex = static_cast<int32>(Section.MaterialIndex);

                    // Look up layer type for this material slot from asset
                    EFleshRingLayerType LayerType = Asset->GetLayerTypeForMaterialSlot(MaterialSlotIndex);

                    // Assign to all vertices in section
                    const int32 BaseVertexIndex = static_cast<int32>(Section.BaseVertexIndex);
                    const int32 NumSectionVertices = static_cast<int32>(Section.NumVertices);

                    for (int32 i = 0; i < NumSectionVertices; ++i)
                    {
                        const int32 VertexIndex = BaseVertexIndex + i;
                        if (VertexIndex < NumVertices)
                        {
                            CachedVertexLayerTypes[VertexIndex] = LayerType;
                        }
                    }
                }

                bUsedAssetMapping = true;
                UE_LOG(LogFleshRingVertices, Log,
                    TEXT("RebuildVertexLayerTypes: Rebuilt from MaterialLayerMappings (%d vertices, %d sections)"),
                    NumVertices, LODData.RenderSections.Num());
            }
        }
    }

    // Fallback to keyword-based auto-detection if no asset mapping
    if (!bUsedAssetMapping)
    {
        if (!FleshRingLayerUtils::BuildVertexLayerTypes(SkeletalMesh, LODIndex, CachedVertexLayerTypes))
        {
            CachedVertexLayerTypes.SetNum(CachedMeshVertices.Num());
            for (int32 i = 0; i < CachedMeshVertices.Num(); ++i)
            {
                CachedVertexLayerTypes[i] = EFleshRingLayerType::Other;
            }
        }
        UE_LOG(LogFleshRingVertices, Log,
            TEXT("RebuildVertexLayerTypes: Used keyword-based auto-detection fallback"));
    }
}

// ============================================================================
// ExtractMeshVertices - extract vertices from mesh (bind pose component space)
// ============================================================================
bool FFleshRingAffectedVerticesManager::ExtractMeshVertices(
    const USkeletalMeshComponent* SkeletalMesh,
    TArray<FVector3f>& OutVertices,
    int32 LODIndex)
{
    if (!SkeletalMesh)
    {
        return false;
    }

    // Get skeletal mesh asset
    USkeletalMesh* Mesh = SkeletalMesh->GetSkeletalMeshAsset();
    if (!Mesh)
    {
        return false;
    }

    // Get render data
    const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
    if (!RenderData || RenderData->LODRenderData.Num() == 0)
    {
        return false;
    }

    // Validate LOD index
    if (LODIndex < 0 || LODIndex >= RenderData->LODRenderData.Num())
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("ExtractMeshVertices: Invalid LOD index %d (max: %d), falling back to LOD 0"),
            LODIndex, RenderData->LODRenderData.Num() - 1);
        LODIndex = 0;
    }

    // Use specified LOD for vertex data
    const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];
    const uint32 NumVertices = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

    if (NumVertices == 0)
    {
        return false;
    }

    // Extract vertex positions (bind pose component space)
    OutVertices.Reset(NumVertices);

    for (uint32 VertexIdx = 0; VertexIdx < NumVertices; ++VertexIdx)
    {
        const FVector3f& Position = LODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(VertexIdx);
        OutVertices.Add(Position);
    }

    return true;
}

// ============================================================================
// ExtractMeshIndices - extract index buffer from mesh
// ============================================================================
bool FFleshRingAffectedVerticesManager::ExtractMeshIndices(
    const USkeletalMeshComponent* SkeletalMesh,
    TArray<uint32>& OutIndices,
    int32 LODIndex)
{
    if (!SkeletalMesh)
    {
        return false;
    }

    USkeletalMesh* Mesh = SkeletalMesh->GetSkeletalMeshAsset();
    if (!Mesh)
    {
        return false;
    }

    const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
    if (!RenderData || RenderData->LODRenderData.Num() == 0)
    {
        return false;
    }

    if (LODIndex < 0 || LODIndex >= RenderData->LODRenderData.Num())
    {
        LODIndex = 0;
    }

    const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];
    const FRawStaticIndexBuffer16or32Interface* IndexBuffer = LODData.MultiSizeIndexContainer.GetIndexBuffer();

    if (!IndexBuffer)
    {
        return false;
    }

    const int32 NumIndices = IndexBuffer->Num();
    OutIndices.Reset(NumIndices);

    for (int32 i = 0; i < NumIndices; ++i)
    {
        OutIndices.Add(IndexBuffer->Get(i));
    }

    return true;
}

// ============================================================================
// BuildAdjacencyData - build adjacent triangle data (optimized with cache)
// ============================================================================
// Optimization: O(T) → O(A × avg_triangles_per_vertex) using CachedVertexTriangles
void FFleshRingAffectedVerticesManager::BuildAdjacencyData(
    FRingAffectedData& RingData,
    const TArray<uint32>& MeshIndices)
{
    const int32 NumAffected = RingData.Vertices.Num();
    if (NumAffected == 0 || MeshIndices.Num() == 0)
    {
        RingData.AdjacencyOffsets.Reset();
        RingData.AdjacencyTriangles.Reset();
        return;
    }

    // Fallback if no cache (shouldn't happen but safety measure)
    if (!bTopologyCacheBuilt || CachedVertexTriangles.Num() == 0)
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("BuildAdjacencyData: Topology cache not built, falling back to brute force"));

        // Fallback: iterate all triangles (legacy method)
        TMap<uint32, int32> VertexToAffectedIndex;
        VertexToAffectedIndex.Reserve(NumAffected);
        for (int32 AffIdx = 0; AffIdx < NumAffected; ++AffIdx)
        {
            VertexToAffectedIndex.Add(RingData.Vertices[AffIdx].VertexIndex, AffIdx);
        }

        TArray<int32> AdjCounts;
        AdjCounts.SetNumZeroed(NumAffected);

        const int32 NumTriangles = MeshIndices.Num() / 3;
        for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
        {
            const uint32 I0 = MeshIndices[TriIdx * 3 + 0];
            const uint32 I1 = MeshIndices[TriIdx * 3 + 1];
            const uint32 I2 = MeshIndices[TriIdx * 3 + 2];

            if (const int32* AffIdx = VertexToAffectedIndex.Find(I0)) { AdjCounts[*AffIdx]++; }
            if (const int32* AffIdx = VertexToAffectedIndex.Find(I1)) { AdjCounts[*AffIdx]++; }
            if (const int32* AffIdx = VertexToAffectedIndex.Find(I2)) { AdjCounts[*AffIdx]++; }
        }

        RingData.AdjacencyOffsets.SetNum(NumAffected + 1);
        RingData.AdjacencyOffsets[0] = 0;
        for (int32 i = 0; i < NumAffected; ++i)
        {
            RingData.AdjacencyOffsets[i + 1] = RingData.AdjacencyOffsets[i] + AdjCounts[i];
        }

        const uint32 TotalAdjacencies = RingData.AdjacencyOffsets[NumAffected];
        RingData.AdjacencyTriangles.SetNum(TotalAdjacencies);

        TArray<uint32> WritePos;
        WritePos.SetNum(NumAffected);
        for (int32 i = 0; i < NumAffected; ++i)
        {
            WritePos[i] = RingData.AdjacencyOffsets[i];
        }

        for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
        {
            const uint32 I0 = MeshIndices[TriIdx * 3 + 0];
            const uint32 I1 = MeshIndices[TriIdx * 3 + 1];
            const uint32 I2 = MeshIndices[TriIdx * 3 + 2];

            if (const int32* AffIdx = VertexToAffectedIndex.Find(I0)) { RingData.AdjacencyTriangles[WritePos[*AffIdx]++] = TriIdx; }
            if (const int32* AffIdx = VertexToAffectedIndex.Find(I1)) { RingData.AdjacencyTriangles[WritePos[*AffIdx]++] = TriIdx; }
            if (const int32* AffIdx = VertexToAffectedIndex.Find(I2)) { RingData.AdjacencyTriangles[WritePos[*AffIdx]++] = TriIdx; }
        }
        return;
    }

    // ================================================================
    // Optimized path using cache: O(A × avg_triangles_per_vertex)
    // ================================================================

    // Step 1: Count triangles for each affected vertex (O(1) lookup from cache)
    TArray<int32> AdjCounts;
    AdjCounts.SetNumZeroed(NumAffected);

    for (int32 AffIdx = 0; AffIdx < NumAffected; ++AffIdx)
    {
        const uint32 VertexIndex = RingData.Vertices[AffIdx].VertexIndex;
        const TArray<uint32>* TrianglesPtr = CachedVertexTriangles.Find(VertexIndex);
        if (TrianglesPtr)
        {
            AdjCounts[AffIdx] = TrianglesPtr->Num();
        }
    }

    // Step 2: Build offset array (cumulative sum)
    RingData.AdjacencyOffsets.SetNum(NumAffected + 1);
    RingData.AdjacencyOffsets[0] = 0;

    for (int32 i = 0; i < NumAffected; ++i)
    {
        RingData.AdjacencyOffsets[i + 1] = RingData.AdjacencyOffsets[i] + AdjCounts[i];
    }

    const uint32 TotalAdjacencies = RingData.AdjacencyOffsets[NumAffected];

    // Step 3: Fill triangle array (direct copy from cache)
    RingData.AdjacencyTriangles.SetNum(TotalAdjacencies);

    for (int32 AffIdx = 0; AffIdx < NumAffected; ++AffIdx)
    {
        const uint32 VertexIndex = RingData.Vertices[AffIdx].VertexIndex;
        const TArray<uint32>* TrianglesPtr = CachedVertexTriangles.Find(VertexIndex);

        if (TrianglesPtr && TrianglesPtr->Num() > 0)
        {
            const uint32 Offset = RingData.AdjacencyOffsets[AffIdx];
            FMemory::Memcpy(
                &RingData.AdjacencyTriangles[Offset],
                TrianglesPtr->GetData(),
                TrianglesPtr->Num() * sizeof(uint32)
            );
        }
    }

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("BuildAdjacencyData (Cached): %d affected vertices, %d total adjacencies (avg %.1f triangles/vertex)"),
        NumAffected, TotalAdjacencies,
        NumAffected > 0 ? static_cast<float>(TotalAdjacencies) / NumAffected : 0.0f);
}

// ============================================================================
// BuildLaplacianAdjacencyData - build neighbor data for Laplacian smoothing
// ============================================================================
// Improvement: includes only neighbors of same layer (no mixing at stocking-skin boundary)
// Improvement: UV seam welding - separated vertices at same position share same neighbors to prevent cracks
// Optimization: uses topology cache - eliminates per-frame O(V*T) rebuild
void FFleshRingAffectedVerticesManager::BuildLaplacianAdjacencyData(
    FRingAffectedData& RingData,
    const TArray<uint32>& MeshIndices,
    const TArray<FVector3f>& AllVertices,
    const TArray<EFleshRingLayerType>& VertexLayerTypes)
{
    // Maximum neighbors per vertex (must match shader's MAX_NEIGHBORS)
    constexpr int32 MAX_NEIGHBORS = 12;
    constexpr int32 PACKED_SIZE = 1 + MAX_NEIGHBORS;  // Count + 12 indices = 13

    const int32 NumAffected = RingData.Vertices.Num();
    if (NumAffected == 0 || MeshIndices.Num() == 0)
    {
        RingData.LaplacianAdjacencyData.Reset();
        return;
    }

    // ================================================================
    // Step 1: Build affected vertex lookup set
    // ================================================================
    // Note: topology cache is already built in RegisterAffectedVertices() early stage.
    // Needed to prioritize affected vertices when selecting neighbors.
    // Non-affected vertices don't get smoothed so their positions don't change.
    // -> Must select affected vertices as neighbors for consistency.
    TSet<uint32> AffectedVertexSet;
    AffectedVertexSet.Reserve(NumAffected);
    for (int32 i = 0; i < NumAffected; ++i)
    {
        AffectedVertexSet.Add(RingData.Vertices[i].VertexIndex);
    }

    // ================================================================
    // Step 2: Pack adjacency data for affected vertices using cached topology
    //
    // Key: All vertices at same position use identical neighbor position set
    //      -> Same Laplacian calculation -> Same movement -> No cracks!
    // ================================================================
    RingData.LaplacianAdjacencyData.Reset();
    RingData.LaplacianAdjacencyData.Reserve(NumAffected * PACKED_SIZE);

    int32 CrossLayerSkipped = 0;

    for (int32 AffIdx = 0; AffIdx < NumAffected; ++AffIdx)
    {
        const uint32 VertexIndex = RingData.Vertices[AffIdx].VertexIndex;
        const EFleshRingLayerType MyLayerType = RingData.Vertices[AffIdx].LayerType;

        uint32 NeighborCount = 0;
        uint32 NeighborIndices[MAX_NEIGHBORS] = { 0 };

        // Get this vertex's position key from cache
        const FIntVector* MyPosKey = CachedVertexToPosition.Find(VertexIndex);
        if (MyPosKey)
        {
            // Get the welded neighbor positions from cache
            const TSet<FIntVector>* WeldedNeighborPosSet = CachedWeldedNeighborPositions.Find(*MyPosKey);

            if (WeldedNeighborPosSet)
            {
                for (const FIntVector& NeighborPosKey : *WeldedNeighborPosSet)
                {
                    // Get vertices at that position from cache
                    const TArray<uint32>* VerticesAtNeighborPos = CachedPositionToVertices.Find(NeighborPosKey);
                    if (!VerticesAtNeighborPos || VerticesAtNeighborPos->Num() == 0)
                    {
                        continue;
                    }

                    // ============================================================
                    // Key fix: Prioritize representative vertex selection (UV Seam Welding)
                    // ============================================================
                    // All duplicate vertices at same position in UV seam must reference same neighbor index
                    // so Laplacian calculation results are identical and cracks are prevented.
                    //
                    // Selection priority:
                    // 1. Representative index (representative vertex at same position)
                    // 2. One of the affected vertices (smoothing target)
                    // 3. First vertex at that position (fallback)
                    uint32 NeighborIdx = UINT32_MAX;

                    // Priority 1: Check representative index
                    const uint32* RepresentativeIdx = CachedPositionToRepresentative.Find(NeighborPosKey);
                    if (RepresentativeIdx && AffectedVertexSet.Contains(*RepresentativeIdx))
                    {
                        NeighborIdx = *RepresentativeIdx;
                    }
                    else
                    {
                        // Priority 2: Select minimum index among affected vertices (for consistency)
                        // Important: use "minimum index" not "first found"
                        // So all UV duplicates at same position reference identical neighbor index
                        uint32 MinAffectedIdx = UINT32_MAX;
                        for (uint32 CandidateIdx : *VerticesAtNeighborPos)
                        {
                            if (AffectedVertexSet.Contains(CandidateIdx))
                            {
                                MinAffectedIdx = FMath::Min(MinAffectedIdx, CandidateIdx);
                            }
                        }
                        if (MinAffectedIdx != UINT32_MAX)
                        {
                            NeighborIdx = MinAffectedIdx;
                        }
                    }

                    // Priority 3: Fallback - minimum index (for consistency)
                    if (NeighborIdx == UINT32_MAX)
                    {
                        uint32 MinIdx = UINT32_MAX;
                        for (uint32 CandidateIdx : *VerticesAtNeighborPos)
                        {
                            MinIdx = FMath::Min(MinIdx, CandidateIdx);
                        }
                        NeighborIdx = MinIdx;
                    }

                    // Layer type filtering: only include neighbors of same layer
                    EFleshRingLayerType NeighborLayerType = EFleshRingLayerType::Other;
                    if (VertexLayerTypes.IsValidIndex(static_cast<int32>(NeighborIdx)))
                    {
                        NeighborLayerType = VertexLayerTypes[static_cast<int32>(NeighborIdx)];
                    }

                    const bool bSameLayer = (MyLayerType == NeighborLayerType);
                    const bool bBothOther = (MyLayerType == EFleshRingLayerType::Other &&
                                              NeighborLayerType == EFleshRingLayerType::Other);

                    if (bSameLayer || bBothOther)
                    {
                        if (NeighborCount < MAX_NEIGHBORS)
                        {
                            NeighborIndices[NeighborCount++] = NeighborIdx;
                        }
                    }
                    else
                    {
                        CrossLayerSkipped++;
                    }
                }
            }
        }

        // Pack: [NeighborCount, N0, N1, ..., N11]
        RingData.LaplacianAdjacencyData.Add(NeighborCount);
        for (int32 i = 0; i < MAX_NEIGHBORS; ++i)
        {
            RingData.LaplacianAdjacencyData.Add(NeighborIndices[i]);
        }
    }

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("BuildLaplacianAdjacencyData (Cached): %d affected, %d packed uints, %d cross-layer skipped"),
        NumAffected, RingData.LaplacianAdjacencyData.Num(), CrossLayerSkipped);
}

// ============================================================================
// BuildSmoothingRegionLaplacianAdjacency - build Laplacian adjacency data for post-processing vertices
// ============================================================================
// Builds Laplacian adjacency data based on SmoothingRegionIndices.
// Provides adjacency info so vertices in Z-extended range can be smoothed.
// Optimization: uses topology cache - eliminates per-frame O(V*T) rebuild

void FFleshRingAffectedVerticesManager::BuildSmoothingRegionLaplacianAdjacency(
    FRingAffectedData& RingData,
    const TArray<uint32>& MeshIndices,
    const TArray<FVector3f>& AllVertices,
    const TArray<EFleshRingLayerType>& VertexLayerTypes)
{
    constexpr int32 MAX_NEIGHBORS = 12;
    constexpr int32 PACKED_SIZE = 1 + MAX_NEIGHBORS;  // Count + 12 indices = 13

    const int32 NumPostProcessing = RingData.SmoothingRegionIndices.Num();
    if (NumPostProcessing == 0 || MeshIndices.Num() == 0)
    {
        RingData.SmoothingRegionLaplacianAdjacency.Reset();
        return;
    }

    // ================================================================
    // Step 1: Build PostProcessing vertex lookup set
    // ================================================================
    // Note: topology cache is already built in RegisterAffectedVertices() early stage
    TSet<uint32> PostProcessingVertexSet;
    PostProcessingVertexSet.Reserve(NumPostProcessing);
    for (int32 i = 0; i < NumPostProcessing; ++i)
    {
        PostProcessingVertexSet.Add(RingData.SmoothingRegionIndices[i]);
    }

    // ================================================================
    // Step 2: Build adjacency for each post-processing vertex using cached topology
    // ================================================================
    RingData.SmoothingRegionLaplacianAdjacency.Reset(NumPostProcessing * PACKED_SIZE);
    RingData.SmoothingRegionLaplacianAdjacency.AddZeroed(NumPostProcessing * PACKED_SIZE);

    int32 CrossLayerSkipped = 0;

    for (int32 PPIdx = 0; PPIdx < NumPostProcessing; ++PPIdx)
    {
        const uint32 VertIdx = RingData.SmoothingRegionIndices[PPIdx];
        const int32 BaseOffset = PPIdx * PACKED_SIZE;

        // Get my layer type (direct lookup from global cache - same as Extended)
        // [Optimization] Use global cache instead of PostProcessingLayerTypes
        EFleshRingLayerType MyLayerType = EFleshRingLayerType::Other;
        if (VertexLayerTypes.IsValidIndex(static_cast<int32>(VertIdx)))
        {
            MyLayerType = VertexLayerTypes[static_cast<int32>(VertIdx)];
        }

        // Get my position key from cache
        const FIntVector* MyPosKey = CachedVertexToPosition.Find(VertIdx);
        if (!MyPosKey)
        {
            RingData.SmoothingRegionLaplacianAdjacency[BaseOffset] = 0;
            continue;
        }

        // Get welded neighbors from cache
        const TSet<FIntVector>* WeldedNeighborPositions = CachedWeldedNeighborPositions.Find(*MyPosKey);
        if (!WeldedNeighborPositions)
        {
            RingData.SmoothingRegionLaplacianAdjacency[BaseOffset] = 0;
            continue;
        }

        uint32 NeighborCount = 0;
        uint32 NeighborIndices[MAX_NEIGHBORS] = {0};

        for (const FIntVector& NeighborPosKey : *WeldedNeighborPositions)
        {
            if (NeighborCount >= MAX_NEIGHBORS) break;

            // Get vertices at that position from cache
            const TArray<uint32>* VerticesAtNeighborPos = CachedPositionToVertices.Find(NeighborPosKey);
            if (!VerticesAtNeighborPos || VerticesAtNeighborPos->Num() == 0) continue;

            // ================================================================
            // [UV Seam Welding] Prioritize representative index for neighbors too
            // ================================================================
            // Use representative index first so all vertices at same position
            // reference identical neighbor index, ensuring Laplacian calculation consistency
            // ================================================================
            uint32 NeighborIdx = UINT32_MAX;

            // Priority 1: Use representative index (better if inside PostProcessing)
            const uint32* RepresentativeIdx = CachedPositionToRepresentative.Find(NeighborPosKey);
            if (RepresentativeIdx)
            {
                NeighborIdx = *RepresentativeIdx;
            }

            // Priority 2: If no representative or not in PostProcessing, select minimum index within PostProcessing
            // Important: use "minimum index" not "first found" for consistency
            // So all UV duplicates at same position reference identical neighbor index
            if (NeighborIdx == UINT32_MAX || !PostProcessingVertexSet.Contains(NeighborIdx))
            {
                uint32 MinPostProcIdx = UINT32_MAX;
                for (uint32 CandidateIdx : *VerticesAtNeighborPos)
                {
                    if (PostProcessingVertexSet.Contains(CandidateIdx))
                    {
                        MinPostProcIdx = FMath::Min(MinPostProcIdx, CandidateIdx);
                    }
                }
                if (MinPostProcIdx != UINT32_MAX)
                {
                    NeighborIdx = MinPostProcIdx;
                }
            }

            // Priority 3: If still none, use minimum index (for consistency)
            if (NeighborIdx == UINT32_MAX)
            {
                uint32 MinIdx = UINT32_MAX;
                for (uint32 CandidateIdx : *VerticesAtNeighborPos)
                {
                    MinIdx = FMath::Min(MinIdx, CandidateIdx);
                }
                NeighborIdx = MinIdx;
            }

            // Layer type filtering
            EFleshRingLayerType NeighborLayerType = EFleshRingLayerType::Other;
            if (VertexLayerTypes.IsValidIndex(static_cast<int32>(NeighborIdx)))
            {
                NeighborLayerType = VertexLayerTypes[static_cast<int32>(NeighborIdx)];
            }

            const bool bSameLayer = (MyLayerType == NeighborLayerType);
            const bool bBothOther = (MyLayerType == EFleshRingLayerType::Other &&
                                      NeighborLayerType == EFleshRingLayerType::Other);

            if (bSameLayer || bBothOther)
            {
                NeighborIndices[NeighborCount++] = NeighborIdx;
            }
            else
            {
                CrossLayerSkipped++;
            }
        }

        // Pack: [NeighborCount, N0, N1, ..., N11]
        RingData.SmoothingRegionLaplacianAdjacency[BaseOffset] = NeighborCount;
        for (int32 i = 0; i < MAX_NEIGHBORS; ++i)
        {
            RingData.SmoothingRegionLaplacianAdjacency[BaseOffset + 1 + i] = NeighborIndices[i];
        }
    }

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("BuildSmoothingRegionLaplacianAdjacency (Cached): %d vertices, %d packed uints, %d cross-layer skipped"),
        NumPostProcessing, RingData.SmoothingRegionLaplacianAdjacency.Num(), CrossLayerSkipped);
}

// ============================================================================
// BuildSmoothingRegionPBDAdjacency - build PBD adjacency data for post-processing vertices
// ============================================================================
// Builds PBD adjacency data based on SmoothingRegionIndices.
// Same as BuildPBDAdjacencyData but uses extended range.

void FFleshRingAffectedVerticesManager::BuildSmoothingRegionPBDAdjacency(
    FRingAffectedData& RingData,
    const TArray<uint32>& MeshIndices,
    const TArray<FVector3f>& AllVertices,
    int32 TotalVertexCount)
{
    const int32 NumPostProcessing = RingData.SmoothingRegionIndices.Num();
    if (NumPostProcessing == 0 || MeshIndices.Num() < 3)
    {
        RingData.SmoothingRegionPBDAdjacency.Reset();
        return;
    }

    // Step 1: Build VertexIndex → ThreadIndex lookup
    TMap<uint32, int32> VertexToThreadIndex;
    for (int32 ThreadIdx = 0; ThreadIdx < NumPostProcessing; ++ThreadIdx)
    {
        VertexToThreadIndex.Add(RingData.SmoothingRegionIndices[ThreadIdx], ThreadIdx);
    }

    // Step 2: Build per-vertex neighbor set with rest lengths
    // Cache optimization: look up neighbors from CachedVertexNeighbors, calculate rest length on-demand
    TArray<TMap<uint32, float>> VertexNeighborsWithRestLen;
    VertexNeighborsWithRestLen.SetNum(NumPostProcessing);

    if (bTopologyCacheBuilt && CachedVertexNeighbors.Num() > 0)
    {
        // Cached path: O(PP × avg_neighbors_per_vertex)
        for (int32 ThreadIdx = 0; ThreadIdx < NumPostProcessing; ++ThreadIdx)
        {
            const uint32 VertexIndex = RingData.SmoothingRegionIndices[ThreadIdx];
            const TSet<uint32>* NeighborsPtr = CachedVertexNeighbors.Find(VertexIndex);

            if (NeighborsPtr)
            {
                const FVector3f& Pos0 = AllVertices[VertexIndex];

                for (uint32 NeighborIdx : *NeighborsPtr)
                {
                    if (NeighborIdx < static_cast<uint32>(AllVertices.Num()))
                    {
                        const FVector3f& Pos1 = AllVertices[NeighborIdx];
                        const float RestLength = FVector3f::Distance(Pos0, Pos1);
                        VertexNeighborsWithRestLen[ThreadIdx].Add(NeighborIdx, RestLength);
                    }
                }
            }
        }
    }
    else
    {
        // Fallback: iterate all triangles (legacy method)
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("BuildSmoothingRegionPBDAdjacency: Topology cache not built, falling back to brute force"));

        const int32 NumTriangles = MeshIndices.Num() / 3;
        for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
        {
            const uint32 Idx0 = MeshIndices[TriIdx * 3 + 0];
            const uint32 Idx1 = MeshIndices[TriIdx * 3 + 1];
            const uint32 Idx2 = MeshIndices[TriIdx * 3 + 2];

            const uint32 TriIndices[3] = { Idx0, Idx1, Idx2 };

            for (int32 Edge = 0; Edge < 3; ++Edge)
            {
                const uint32 V0 = TriIndices[Edge];
                const uint32 V1 = TriIndices[(Edge + 1) % 3];

                if (int32* ThreadIdxPtr = VertexToThreadIndex.Find(V0))
                {
                    const int32 ThreadIdx = *ThreadIdxPtr;
                    if (V1 < static_cast<uint32>(AllVertices.Num()))
                    {
                        if (!VertexNeighborsWithRestLen[ThreadIdx].Contains(V1))
                        {
                            const FVector3f& Pos0 = AllVertices[V0];
                            const FVector3f& Pos1 = AllVertices[V1];
                            const float RestLength = FVector3f::Distance(Pos0, Pos1);
                            VertexNeighborsWithRestLen[ThreadIdx].Add(V1, RestLength);
                        }
                    }
                }
            }
        }
    }

    // Step 3: Pack adjacency data with rest lengths
    const int32 PackedSizePerVertex = FRingAffectedData::PBD_ADJACENCY_PACKED_SIZE;
    RingData.SmoothingRegionPBDAdjacency.Reset(NumPostProcessing * PackedSizePerVertex);
    RingData.SmoothingRegionPBDAdjacency.AddZeroed(NumPostProcessing * PackedSizePerVertex);

    for (int32 ThreadIdx = 0; ThreadIdx < NumPostProcessing; ++ThreadIdx)
    {
        const TMap<uint32, float>& NeighborsMap = VertexNeighborsWithRestLen[ThreadIdx];
        const int32 NeighborCount = FMath::Min(NeighborsMap.Num(), FRingAffectedData::PBD_MAX_NEIGHBORS);
        const int32 BaseOffset = ThreadIdx * PackedSizePerVertex;

        RingData.SmoothingRegionPBDAdjacency[BaseOffset] = static_cast<uint32>(NeighborCount);

        int32 SlotIdx = 0;
        for (const auto& Pair : NeighborsMap)
        {
            if (SlotIdx >= FRingAffectedData::PBD_MAX_NEIGHBORS)
            {
                break;
            }

            const uint32 NeighborIdx = Pair.Key;
            const float RestLength = Pair.Value;

            RingData.SmoothingRegionPBDAdjacency[BaseOffset + 1 + SlotIdx * 2] = NeighborIdx;

            uint32 RestLengthAsUint;
            FMemory::Memcpy(&RestLengthAsUint, &RestLength, sizeof(float));
            RingData.SmoothingRegionPBDAdjacency[BaseOffset + 1 + SlotIdx * 2 + 1] = RestLengthAsUint;

            ++SlotIdx;
        }
    }

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("BuildSmoothingRegionPBDAdjacency: %d vertices, %d packed uints"),
        NumPostProcessing, RingData.SmoothingRegionPBDAdjacency.Num());
}

// ============================================================================
// BuildSmoothingRegionPBDAdjacency_HopBased - build PBD adjacency data for extended smoothing region
// ============================================================================
// Builds PBD adjacency data based on SmoothingRegionIndices.
// Used in HopBased mode.

void FFleshRingAffectedVerticesManager::BuildSmoothingRegionPBDAdjacency_HopBased(
    FRingAffectedData& RingData,
    const TArray<FVector3f>& AllVertices)
{
    const int32 NumExtended = RingData.SmoothingRegionIndices.Num();
    if (NumExtended == 0)
    {
        RingData.SmoothingRegionPBDAdjacency.Reset();
        return;
    }

    // Verify topology cache
    if (!bTopologyCacheBuilt || CachedVertexNeighbors.Num() == 0)
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("BuildSmoothingRegionPBDAdjacency_HopBased: Topology cache not built, skipping"));
        RingData.SmoothingRegionPBDAdjacency.Reset();
        return;
    }

    // Single-pass: pack directly from CachedVertexNeighbors (remove intermediate TMap)
    const int32 PackedSizePerVertex = FRingAffectedData::PBD_ADJACENCY_PACKED_SIZE;
    RingData.SmoothingRegionPBDAdjacency.Reset(NumExtended * PackedSizePerVertex);
    RingData.SmoothingRegionPBDAdjacency.AddZeroed(NumExtended * PackedSizePerVertex);

    for (int32 ThreadIdx = 0; ThreadIdx < NumExtended; ++ThreadIdx)
    {
        const uint32 VertexIndex = RingData.SmoothingRegionIndices[ThreadIdx];
        const int32 BaseOffset = ThreadIdx * PackedSizePerVertex;

        const TSet<uint32>* NeighborsPtr = CachedVertexNeighbors.Find(VertexIndex);
        if (!NeighborsPtr)
        {
            // No neighbors
            RingData.SmoothingRegionPBDAdjacency[BaseOffset] = 0;
            continue;
        }

        const FVector3f& Pos0 = AllVertices[VertexIndex];
        int32 SlotIdx = 0;

        for (uint32 NeighborIdx : *NeighborsPtr)
        {
            if (SlotIdx >= FRingAffectedData::PBD_MAX_NEIGHBORS)
            {
                break;
            }

            if (NeighborIdx < static_cast<uint32>(AllVertices.Num()))
            {
                const FVector3f& Pos1 = AllVertices[NeighborIdx];
                const float RestLength = FVector3f::Distance(Pos0, Pos1);

                RingData.SmoothingRegionPBDAdjacency[BaseOffset + 1 + SlotIdx * 2] = NeighborIdx;

                uint32 RestLengthAsUint;
                FMemory::Memcpy(&RestLengthAsUint, &RestLength, sizeof(float));
                RingData.SmoothingRegionPBDAdjacency[BaseOffset + 1 + SlotIdx * 2 + 1] = RestLengthAsUint;

                ++SlotIdx;
            }
        }

        // Actual recorded neighbor count
        RingData.SmoothingRegionPBDAdjacency[BaseOffset] = static_cast<uint32>(SlotIdx);
    }

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("BuildSmoothingRegionPBDAdjacency_HopBased: %d vertices, %d packed uints"),
        NumExtended, RingData.SmoothingRegionPBDAdjacency.Num());
}

// ============================================================================
// BuildSmoothingRegionNormalAdjacency - build normal adjacency data for post-processing vertices
// ============================================================================
// Builds adjacency data for normal recomputation based on SmoothingRegionIndices.
// Same as BuildAdjacencyData but uses extended range.

void FFleshRingAffectedVerticesManager::BuildSmoothingRegionNormalAdjacency(
    FRingAffectedData& RingData,
    const TArray<uint32>& MeshIndices)
{
    const int32 NumPostProcessing = RingData.SmoothingRegionIndices.Num();
    if (NumPostProcessing == 0 || MeshIndices.Num() == 0)
    {
        RingData.SmoothingRegionAdjacencyOffsets.Reset();
        RingData.SmoothingRegionAdjacencyTriangles.Reset();
        return;
    }

    // Fallback if no cache (shouldn't happen but safety measure)
    if (!bTopologyCacheBuilt || CachedVertexTriangles.Num() == 0)
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("BuildSmoothingRegionNormalAdjacency: Topology cache not built, falling back to brute force"));

        // Fallback: iterate all triangles (legacy method)
        TMap<uint32, int32> VertexToIndex;
        VertexToIndex.Reserve(NumPostProcessing);
        for (int32 PPIdx = 0; PPIdx < NumPostProcessing; ++PPIdx)
        {
            VertexToIndex.Add(RingData.SmoothingRegionIndices[PPIdx], PPIdx);
        }

        TArray<int32> AdjCounts;
        AdjCounts.SetNumZeroed(NumPostProcessing);

        const int32 NumTriangles = MeshIndices.Num() / 3;
        for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
        {
            const uint32 I0 = MeshIndices[TriIdx * 3 + 0];
            const uint32 I1 = MeshIndices[TriIdx * 3 + 1];
            const uint32 I2 = MeshIndices[TriIdx * 3 + 2];

            if (const int32* Idx = VertexToIndex.Find(I0)) { AdjCounts[*Idx]++; }
            if (const int32* Idx = VertexToIndex.Find(I1)) { AdjCounts[*Idx]++; }
            if (const int32* Idx = VertexToIndex.Find(I2)) { AdjCounts[*Idx]++; }
        }

        RingData.SmoothingRegionAdjacencyOffsets.SetNum(NumPostProcessing + 1);
        RingData.SmoothingRegionAdjacencyOffsets[0] = 0;
        for (int32 i = 0; i < NumPostProcessing; ++i)
        {
            RingData.SmoothingRegionAdjacencyOffsets[i + 1] = RingData.SmoothingRegionAdjacencyOffsets[i] + AdjCounts[i];
        }

        const uint32 TotalAdjacencies = RingData.SmoothingRegionAdjacencyOffsets[NumPostProcessing];
        RingData.SmoothingRegionAdjacencyTriangles.SetNum(TotalAdjacencies);

        TArray<uint32> WritePos;
        WritePos.SetNum(NumPostProcessing);
        for (int32 i = 0; i < NumPostProcessing; ++i)
        {
            WritePos[i] = RingData.SmoothingRegionAdjacencyOffsets[i];
        }

        for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
        {
            const uint32 I0 = MeshIndices[TriIdx * 3 + 0];
            const uint32 I1 = MeshIndices[TriIdx * 3 + 1];
            const uint32 I2 = MeshIndices[TriIdx * 3 + 2];

            if (const int32* Idx = VertexToIndex.Find(I0)) { RingData.SmoothingRegionAdjacencyTriangles[WritePos[*Idx]++] = TriIdx; }
            if (const int32* Idx = VertexToIndex.Find(I1)) { RingData.SmoothingRegionAdjacencyTriangles[WritePos[*Idx]++] = TriIdx; }
            if (const int32* Idx = VertexToIndex.Find(I2)) { RingData.SmoothingRegionAdjacencyTriangles[WritePos[*Idx]++] = TriIdx; }
        }
        return;
    }

    // ================================================================
    // Optimized path using cache: O(PP × avg_triangles_per_vertex)
    // ================================================================

    // Step 1: Count triangles for each post-processing vertex (O(1) lookup from cache)
    TArray<int32> AdjCounts;
    AdjCounts.SetNumZeroed(NumPostProcessing);

    for (int32 PPIdx = 0; PPIdx < NumPostProcessing; ++PPIdx)
    {
        const uint32 VertexIndex = RingData.SmoothingRegionIndices[PPIdx];
        const TArray<uint32>* TrianglesPtr = CachedVertexTriangles.Find(VertexIndex);
        if (TrianglesPtr)
        {
            AdjCounts[PPIdx] = TrianglesPtr->Num();
        }
    }

    // Step 2: Build offset array (cumulative sum)
    RingData.SmoothingRegionAdjacencyOffsets.SetNum(NumPostProcessing + 1);
    RingData.SmoothingRegionAdjacencyOffsets[0] = 0;

    for (int32 i = 0; i < NumPostProcessing; ++i)
    {
        RingData.SmoothingRegionAdjacencyOffsets[i + 1] = RingData.SmoothingRegionAdjacencyOffsets[i] + AdjCounts[i];
    }

    const uint32 TotalAdjacencies = RingData.SmoothingRegionAdjacencyOffsets[NumPostProcessing];

    // Step 3: Fill triangle array (direct copy from cache)
    RingData.SmoothingRegionAdjacencyTriangles.SetNum(TotalAdjacencies);

    for (int32 PPIdx = 0; PPIdx < NumPostProcessing; ++PPIdx)
    {
        const uint32 VertexIndex = RingData.SmoothingRegionIndices[PPIdx];
        const TArray<uint32>* TrianglesPtr = CachedVertexTriangles.Find(VertexIndex);

        if (TrianglesPtr && TrianglesPtr->Num() > 0)
        {
            const uint32 Offset = RingData.SmoothingRegionAdjacencyOffsets[PPIdx];
            FMemory::Memcpy(
                &RingData.SmoothingRegionAdjacencyTriangles[Offset],
                TrianglesPtr->GetData(),
                TrianglesPtr->Num() * sizeof(uint32)
            );
        }
    }

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("BuildSmoothingRegionNormalAdjacency (Cached): %d vertices, %d offsets, %d triangles"),
        NumPostProcessing, RingData.SmoothingRegionAdjacencyOffsets.Num(), TotalAdjacencies);
}

// ============================================================================
// BuildSliceData - build slice-based bone distance ratio preservation data
// ============================================================================

void FFleshRingAffectedVerticesManager::BuildSliceData(
    FRingAffectedData& RingData,
    const TArray<FVector3f>& AllVertices,
    float BucketSize)
{
    const int32 NumAffected = RingData.Vertices.Num();

    if (NumAffected == 0)
    {
        return;
    }

    // ================================================================
    // Step 1: Calculate axis distance (height) and bone distance for each vertex
    // ================================================================

    // Ring axis and center (bind pose basis)
    const FVector3f Axis = FVector3f(RingData.RingAxis.GetSafeNormal());
    const FVector3f Center = FVector3f(RingData.RingCenter);

    // Store height data (for GPU transfer)
    RingData.AxisHeights.Reset();
    RingData.AxisHeights.SetNum(NumAffected);

    RingData.OriginalBoneDistances.Reset();
    RingData.OriginalBoneDistances.SetNum(NumAffected);

    for (int32 AffIdx = 0; AffIdx < NumAffected; ++AffIdx)
    {
        const uint32 VertexIndex = RingData.Vertices[AffIdx].VertexIndex;
        const FVector3f& VertexPos = AllVertices[VertexIndex];

        // Vector from center to vertex
        const FVector3f ToVertex = VertexPos - Center;

        // Height along axis direction (dot product)
        const float Height = FVector3f::DotProduct(ToVertex, Axis);
        RingData.AxisHeights[AffIdx] = Height;

        // Distance perpendicular to axis (bone distance = radius)
        const FVector3f AxisComponent = Axis * Height;
        const FVector3f RadialComponent = ToVertex - AxisComponent;
        const float BoneDistance = RadialComponent.Size();

        RingData.OriginalBoneDistances[AffIdx] = BoneDistance;
    }

    // ================================================================
    // Step 2: Group vertices by height bucket (slice)
    // ================================================================

    // Bucket index → list of affected vertex indices in that bucket
    TMap<int32, TArray<int32>> BucketToVertices;

    for (int32 AffIdx = 0; AffIdx < NumAffected; ++AffIdx)
    {
        const int32 BucketIdx = FMath::FloorToInt(RingData.AxisHeights[AffIdx] / BucketSize);
        BucketToVertices.FindOrAdd(BucketIdx).Add(AffIdx);
    }

    // ================================================================
    // Step 3: Pack slice data for GPU (with adjacent buckets)
    // ================================================================
    // Format: [SliceVertexCount, V0, V1, ..., V31] per affected vertex
    // Improvement: includes current bucket + adjacent buckets (±1) for smooth transitions

    RingData.SlicePackedData.Reset();
    RingData.SlicePackedData.Reserve(NumAffected * FRingAffectedData::SLICE_PACKED_SIZE);

    for (int32 AffIdx = 0; AffIdx < NumAffected; ++AffIdx)
    {
        // Find the bucket this vertex belongs to
        const int32 BucketIdx = FMath::FloorToInt(RingData.AxisHeights[AffIdx] / BucketSize);

        // Collect vertices including adjacent buckets (±1)
        // Order: current bucket (0) first → guarantees own slice vertices even on overflow
        TArray<int32> AdjacentVertices;
        AdjacentVertices.Reserve(FRingAffectedData::MAX_SLICE_VERTICES);

        static const int32 BucketDeltas[] = {0, -1, 1};
        for (int32 Delta : BucketDeltas)
        {
            const int32 NeighborBucket = BucketIdx + Delta;
            if (const TArray<int32>* NeighborVertices = BucketToVertices.Find(NeighborBucket))
            {
                for (int32 NeighborAffIdx : *NeighborVertices)
                {
                    if (AdjacentVertices.Num() < FRingAffectedData::MAX_SLICE_VERTICES)
                    {
                        AdjacentVertices.Add(NeighborAffIdx);
                    }
                }
            }
        }

        // Pack: [Count, V0, V1, ..., V31]
        const int32 SliceCount = AdjacentVertices.Num();
        RingData.SlicePackedData.Add(static_cast<uint32>(SliceCount));

        for (int32 i = 0; i < SliceCount; ++i)
        {
            RingData.SlicePackedData.Add(static_cast<uint32>(AdjacentVertices[i]));
        }

        // Fill remaining slots with 0
        for (int32 i = SliceCount; i < FRingAffectedData::MAX_SLICE_VERTICES; ++i)
        {
            RingData.SlicePackedData.Add(0);
        }
    }

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("BuildSliceData: %d affected vertices, %d buckets, bucket size %.2f (with adjacent buckets)"),
        NumAffected, BucketToVertices.Num(), BucketSize);
}

// ============================================================================
// BuildPBDAdjacencyData - build PBD edge constraint adjacency data (cache optimized)
// ============================================================================
// Optimization: O(T) → O(A × avg_neighbors_per_vertex) using CachedVertexNeighbors

void FFleshRingAffectedVerticesManager::BuildPBDAdjacencyData(
    FRingAffectedData& RingData,
    const TArray<uint32>& MeshIndices,
    const TArray<FVector3f>& AllVertices,
    int32 TotalVertexCount)
{
    const int32 NumAffected = RingData.Vertices.Num();
    if (NumAffected == 0 || MeshIndices.Num() < 3)
    {
        return;
    }

    // Step 1: Build VertexIndex → ThreadIndex lookup
    TMap<uint32, int32> VertexToThreadIndex;
    for (int32 ThreadIdx = 0; ThreadIdx < NumAffected; ++ThreadIdx)
    {
        VertexToThreadIndex.Add(RingData.Vertices[ThreadIdx].VertexIndex, ThreadIdx);
    }

    // Step 2: Build per-vertex neighbor set with rest lengths
    // Cache optimization: look up neighbors from CachedVertexNeighbors, calculate rest length on-demand
    TArray<TMap<uint32, float>> VertexNeighborsWithRestLen;
    VertexNeighborsWithRestLen.SetNum(NumAffected);

    if (bTopologyCacheBuilt && CachedVertexNeighbors.Num() > 0)
    {
        // Cached path: O(A × avg_neighbors_per_vertex)
        for (int32 ThreadIdx = 0; ThreadIdx < NumAffected; ++ThreadIdx)
        {
            const uint32 VertexIndex = RingData.Vertices[ThreadIdx].VertexIndex;
            const TSet<uint32>* NeighborsPtr = CachedVertexNeighbors.Find(VertexIndex);

            if (NeighborsPtr)
            {
                const FVector3f& Pos0 = AllVertices[VertexIndex];

                for (uint32 NeighborIdx : *NeighborsPtr)
                {
                    if (NeighborIdx < static_cast<uint32>(AllVertices.Num()))
                    {
                        const FVector3f& Pos1 = AllVertices[NeighborIdx];
                        const float RestLength = FVector3f::Distance(Pos0, Pos1);
                        VertexNeighborsWithRestLen[ThreadIdx].Add(NeighborIdx, RestLength);
                    }
                }
            }
        }
    }
    else
    {
        // Fallback: iterate all triangles (legacy method)
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("BuildPBDAdjacencyData: Topology cache not built, falling back to brute force"));

        const int32 NumTriangles = MeshIndices.Num() / 3;
        for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
        {
            const uint32 Idx0 = MeshIndices[TriIdx * 3 + 0];
            const uint32 Idx1 = MeshIndices[TriIdx * 3 + 1];
            const uint32 Idx2 = MeshIndices[TriIdx * 3 + 2];

            const uint32 TriIndices[3] = { Idx0, Idx1, Idx2 };

            for (int32 Edge = 0; Edge < 3; ++Edge)
            {
                const uint32 V0 = TriIndices[Edge];
                const uint32 V1 = TriIndices[(Edge + 1) % 3];

                if (int32* ThreadIdxPtr = VertexToThreadIndex.Find(V0))
                {
                    const int32 ThreadIdx = *ThreadIdxPtr;
                    if (V1 < static_cast<uint32>(AllVertices.Num()))
                    {
                        if (!VertexNeighborsWithRestLen[ThreadIdx].Contains(V1))
                        {
                            const FVector3f& Pos0 = AllVertices[V0];
                            const FVector3f& Pos1 = AllVertices[V1];
                            const float RestLength = FVector3f::Distance(Pos0, Pos1);
                            VertexNeighborsWithRestLen[ThreadIdx].Add(V1, RestLength);
                        }
                    }
                }
            }
        }
    }

    // Step 3: Pack adjacency data with rest lengths
    // Format: [Count, N0, RL0, N1, RL1, ...] per vertex (1 + MAX_NEIGHBORS*2 uints)
    const int32 PackedSizePerVertex = FRingAffectedData::PBD_ADJACENCY_PACKED_SIZE;
    RingData.PBDAdjacencyWithRestLengths.Reset(NumAffected * PackedSizePerVertex);
    RingData.PBDAdjacencyWithRestLengths.AddZeroed(NumAffected * PackedSizePerVertex);

    for (int32 ThreadIdx = 0; ThreadIdx < NumAffected; ++ThreadIdx)
    {
        const TMap<uint32, float>& NeighborsMap = VertexNeighborsWithRestLen[ThreadIdx];
        const int32 NeighborCount = FMath::Min(NeighborsMap.Num(), FRingAffectedData::PBD_MAX_NEIGHBORS);
        const int32 BaseOffset = ThreadIdx * PackedSizePerVertex;

        // Count
        RingData.PBDAdjacencyWithRestLengths[BaseOffset] = static_cast<uint32>(NeighborCount);

        // Neighbors with rest lengths
        int32 SlotIdx = 0;
        for (const auto& Pair : NeighborsMap)
        {
            if (SlotIdx >= FRingAffectedData::PBD_MAX_NEIGHBORS)
            {
                break;
            }

            const uint32 NeighborIdx = Pair.Key;
            const float RestLength = Pair.Value;

            // Neighbor index
            RingData.PBDAdjacencyWithRestLengths[BaseOffset + 1 + SlotIdx * 2] = NeighborIdx;

            // Rest length (bit-cast float to uint)
            uint32 RestLengthAsUint;
            FMemory::Memcpy(&RestLengthAsUint, &RestLength, sizeof(float));
            RingData.PBDAdjacencyWithRestLengths[BaseOffset + 1 + SlotIdx * 2 + 1] = RestLengthAsUint;

            ++SlotIdx;
        }
    }

    // Step 4: Build full influence map (influence for all vertices)
    RingData.FullInfluenceMap.Reset(TotalVertexCount);
    RingData.FullInfluenceMap.AddZeroed(TotalVertexCount);

    for (int32 ThreadIdx = 0; ThreadIdx < NumAffected; ++ThreadIdx)
    {
        const FAffectedVertex& Vert = RingData.Vertices[ThreadIdx];
        if (Vert.VertexIndex < static_cast<uint32>(TotalVertexCount))
        {
            RingData.FullInfluenceMap[Vert.VertexIndex] = Vert.Influence;
        }
    }

    // Step 5: Build full deform amount map
    // Note: DeformAmount is calculated in FleshRingDeformerInstance so
    // here we set approximate values based on AxisHeight
    // Actual values are used from DispatchData.DeformAmounts
    RingData.FullDeformAmountMap.Reset(TotalVertexCount);
    RingData.FullDeformAmountMap.AddZeroed(TotalVertexCount);

    // Use half of Ring height as threshold
    const float RingHalfWidth = RingData.RingHeight * 0.5f;

    for (int32 ThreadIdx = 0; ThreadIdx < NumAffected; ++ThreadIdx)
    {
        const FAffectedVertex& Vert = RingData.Vertices[ThreadIdx];
        if (Vert.VertexIndex < static_cast<uint32>(TotalVertexCount))
        {
            // Calculate DeformAmount based on AxisHeight
            const float AxisHeight = RingData.AxisHeights.IsValidIndex(ThreadIdx)
                ? RingData.AxisHeights[ThreadIdx] : 0.0f;
            const float EdgeRatio = FMath::Clamp(
                FMath::Abs(AxisHeight) / FMath::Max(RingHalfWidth, 0.01f), 0.0f, 2.0f);

            // EdgeRatio > 1: Bulge region (positive), EdgeRatio < 1: Tightness region (negative)
            RingData.FullDeformAmountMap[Vert.VertexIndex] = (EdgeRatio - 1.0f) * Vert.Influence;
        }
    }

    // Step 6: Build full IsAnchor map (IsAnchor flags for all vertices)
    // In Tolerance-based PBD, query neighbor's anchor status to determine weight distribution
    // Affected Vertices = Anchor (1), Non-Affected = Free (0)
    RingData.FullVertexAnchorFlags.Reset(TotalVertexCount);
    RingData.FullVertexAnchorFlags.AddZeroed(TotalVertexCount);

    for (int32 ThreadIdx = 0; ThreadIdx < NumAffected; ++ThreadIdx)
    {
        const FAffectedVertex& Vert = RingData.Vertices[ThreadIdx];
        if (Vert.VertexIndex < static_cast<uint32>(TotalVertexCount))
        {
            // Affected Vertex = Anchor (fixed)
            RingData.FullVertexAnchorFlags[Vert.VertexIndex] = 1;
        }
    }

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("BuildPBDAdjacencyData: %d affected vertices, %d packed uints, %d total vertices in map, %d anchor flags"),
        NumAffected, RingData.PBDAdjacencyWithRestLengths.Num(), TotalVertexCount, RingData.FullVertexAnchorFlags.Num());
}

// ============================================================================
// BuildFullMeshAdjacency - build full mesh adjacency map
// ============================================================================

void FFleshRingAffectedVerticesManager::BuildFullMeshAdjacency(
    const TArray<uint32>& MeshIndices,
    int32 NumVertices,
    TMap<uint32, TArray<uint32>>& OutAdjacencyMap)
{
    OutAdjacencyMap.Reset();
    OutAdjacencyMap.Reserve(NumVertices);

    const int32 NumTriangles = MeshIndices.Num() / 3;

    for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
    {
        const uint32 I0 = MeshIndices[TriIdx * 3 + 0];
        const uint32 I1 = MeshIndices[TriIdx * 3 + 1];
        const uint32 I2 = MeshIndices[TriIdx * 3 + 2];

        // Add bidirectional adjacency for each edge
        auto AddEdge = [&OutAdjacencyMap](uint32 A, uint32 B)
        {
            TArray<uint32>& NeighborsA = OutAdjacencyMap.FindOrAdd(A);
            if (!NeighborsA.Contains(B))
            {
                NeighborsA.Add(B);
            }

            TArray<uint32>& NeighborsB = OutAdjacencyMap.FindOrAdd(B);
            if (!NeighborsB.Contains(A))
            {
                NeighborsB.Add(A);
            }
        };

        AddEdge(I0, I1);
        AddEdge(I1, I2);
        AddEdge(I2, I0);
    }
}

// ============================================================================
// BuildSmoothingRegionLaplacianAdjacency_HopBased - build adjacency data for HopBased smoothing region
// ============================================================================
// [Fix] Uses same welding logic as BuildSmoothingRegionLaplacianAdjacency
// Before: Used CachedFullAdjacencyMap (no UV welding)
// After: Uses CachedWeldedNeighborPositions (UV welding applied)
//
// Key: All vertices at same position use identical neighbor position set
//      -> Same Laplacian calculation -> Same movement -> UV seam crack prevention

void FFleshRingAffectedVerticesManager::BuildSmoothingRegionLaplacianAdjacency_HopBased(
    FRingAffectedData& RingData,
    const TArray<EFleshRingLayerType>& VertexLayerTypes)
{
    constexpr int32 MAX_NEIGHBORS = 12;
    constexpr int32 PACKED_SIZE = 1 + MAX_NEIGHBORS;  // Count + 12 indices = 13

    const int32 NumExtended = RingData.SmoothingRegionIndices.Num();
    if (NumExtended == 0)
    {
        RingData.SmoothingRegionLaplacianAdjacency.Reset();
        return;
    }

    // ================================================================
    // Step 1: Build Extended vertex lookup set
    // ================================================================
    TSet<uint32> ExtendedVertexSet;
    ExtendedVertexSet.Reserve(NumExtended);
    for (int32 i = 0; i < NumExtended; ++i)
    {
        ExtendedVertexSet.Add(RingData.SmoothingRegionIndices[i]);
    }

    // ================================================================
    // Step 2: Build adjacency for each extended vertex using cached topology
    //
    // Key: Uses CachedWeldedNeighborPositions (UV duplicate neighbors merged)
    // ================================================================
    RingData.SmoothingRegionLaplacianAdjacency.Reset(NumExtended * PACKED_SIZE);
    RingData.SmoothingRegionLaplacianAdjacency.AddZeroed(NumExtended * PACKED_SIZE);

    int32 CrossLayerSkipped = 0;

    for (int32 ExtIdx = 0; ExtIdx < NumExtended; ++ExtIdx)
    {
        const uint32 VertIdx = RingData.SmoothingRegionIndices[ExtIdx];
        const int32 BaseOffset = ExtIdx * PACKED_SIZE;

        // Get my layer type (Extended has no separate LayerTypes array, use global)
        EFleshRingLayerType MyLayerType = EFleshRingLayerType::Other;
        if (VertexLayerTypes.IsValidIndex(static_cast<int32>(VertIdx)))
        {
            MyLayerType = VertexLayerTypes[static_cast<int32>(VertIdx)];
        }

        // Get my position key from cache
        const FIntVector* MyPosKey = CachedVertexToPosition.Find(VertIdx);
        if (!MyPosKey)
        {
            RingData.SmoothingRegionLaplacianAdjacency[BaseOffset] = 0;
            continue;
        }

        // Get welded neighbors from cache (neighbors of UV duplicates merged!)
        const TSet<FIntVector>* WeldedNeighborPositions = CachedWeldedNeighborPositions.Find(*MyPosKey);
        if (!WeldedNeighborPositions)
        {
            RingData.SmoothingRegionLaplacianAdjacency[BaseOffset] = 0;
            continue;
        }

        uint32 NeighborCount = 0;
        uint32 NeighborIndices[MAX_NEIGHBORS] = {0};

        for (const FIntVector& NeighborPosKey : *WeldedNeighborPositions)
        {
            if (NeighborCount >= MAX_NEIGHBORS) break;

            // Get vertices at that position from cache
            const TArray<uint32>* VerticesAtNeighborPos = CachedPositionToVertices.Find(NeighborPosKey);
            if (!VerticesAtNeighborPos || VerticesAtNeighborPos->Num() == 0) continue;

            // ================================================================
            // [UV Seam Welding + Heat Propagation]
            // Always use global representative index
            // ================================================================
            // Problem: Previous logic selected a different index within Extended
            //          when Representative was outside Extended
            //          → InitPass stores delta at Representative
            //          → DiffusePass reads from different index in Adjacency
            //          → Mismatch causes reading delta=0!
            //
            // Solution: When neighbor position has Extended vertex,
            //           always store global Representative index
            //           delta buffer is full mesh size so can access outside Extended
            // ================================================================
            uint32 NeighborIdx = UINT32_MAX;  // Invalid sentinel

            // First check if there's an Extended vertex at this position
            bool bHasExtendedNeighbor = false;
            for (uint32 CandidateIdx : *VerticesAtNeighborPos)
            {
                if (ExtendedVertexSet.Contains(CandidateIdx))
                {
                    bHasExtendedNeighbor = true;
                    break;
                }
            }

            // If Extended neighbor exists, use global Representative index
            if (bHasExtendedNeighbor)
            {
                const uint32* RepresentativeIdx = CachedPositionToRepresentative.Find(NeighborPosKey);
                if (RepresentativeIdx)
                {
                    NeighborIdx = *RepresentativeIdx;
                }
                else
                {
                    // Representative cache miss - use first vertex at that position
                    NeighborIdx = (*VerticesAtNeighborPos)[0];
                }
            }

            // Skip if no Extended neighbor found at this position
            // (non-Extended neighbors would have delta=0, diluting propagation)
            if (NeighborIdx == UINT32_MAX)
            {
                continue;
            }

            // Layer type filtering
            EFleshRingLayerType NeighborLayerType = EFleshRingLayerType::Other;
            if (VertexLayerTypes.IsValidIndex(static_cast<int32>(NeighborIdx)))
            {
                NeighborLayerType = VertexLayerTypes[static_cast<int32>(NeighborIdx)];
            }

            const bool bSameLayer = (MyLayerType == NeighborLayerType);
            const bool bBothOther = (MyLayerType == EFleshRingLayerType::Other &&
                                      NeighborLayerType == EFleshRingLayerType::Other);

            if (bSameLayer || bBothOther)
            {
                NeighborIndices[NeighborCount++] = NeighborIdx;
            }
            else
            {
                CrossLayerSkipped++;
            }
        }

        // Pack: [NeighborCount, N0, N1, ..., N11]
        RingData.SmoothingRegionLaplacianAdjacency[BaseOffset] = NeighborCount;
        for (int32 i = 0; i < MAX_NEIGHBORS; ++i)
        {
            RingData.SmoothingRegionLaplacianAdjacency[BaseOffset + 1 + i] = NeighborIndices[i];
        }
    }

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("BuildSmoothingRegionLaplacianAdjacency_HopBased: %d vertices, %d packed uints, %d cross-layer skipped"),
        NumExtended, RingData.SmoothingRegionLaplacianAdjacency.Num(), CrossLayerSkipped);
}

// ============================================================================
// BuildRepresentativeIndices - build representative vertex indices for UV seam welding
// ============================================================================
// [Design]
// Ensures separated vertices at UV seam (same position, different indices) receive identical deformation.
// In all deformation passes (TightnessCS, BulgeCS, LaplacianCS, etc.):
//   1. Read representative vertex position
//   2. Calculate deformation
//   3. Write to own index
// This ensures UV duplicates always move identically, preventing cracks.
//
// [Optimization - 2024.01]
// Utilizes topology cache: CachedPositionToRepresentative, CachedVertexToPosition
// Before: per-frame O(A) TMap build + O(A) PosKey recalculation = ~30-50ms
// After: O(A) cache lookup = ~1-2ms

void FFleshRingAffectedVerticesManager::BuildRepresentativeIndices(
    FRingAffectedData& RingData,
    const TArray<FVector3f>& AllVertices)
{
    // ================================================================
    // Use optimized path with O(1) lookups if cache exists
    // ================================================================
    if (bTopologyCacheBuilt && CachedPositionToRepresentative.Num() > 0)
    {
        // ===== RepresentativeIndices for Affected Vertices (using cache) =====
        const int32 NumAffected = RingData.Vertices.Num();
        RingData.RepresentativeIndices.Reset(NumAffected);
        RingData.RepresentativeIndices.AddUninitialized(NumAffected);

        int32 NumWelded = 0;
        for (int32 i = 0; i < NumAffected; ++i)
        {
            const uint32 VertIdx = RingData.Vertices[i].VertexIndex;

            // O(1) lookup - get PosKey from CachedVertexToPosition
            const FIntVector* PosKey = CachedVertexToPosition.Find(VertIdx);
            if (PosKey)
            {
                // O(1) lookup - get representative from CachedPositionToRepresentative
                const uint32* Representative = CachedPositionToRepresentative.Find(*PosKey);
                const uint32 RepIdx = Representative ? *Representative : VertIdx;
                RingData.RepresentativeIndices[i] = RepIdx;

                if (RepIdx != VertIdx)
                {
                    NumWelded++;
                }
            }
            else
            {
                // Cache miss - use self as representative
                RingData.RepresentativeIndices[i] = VertIdx;
            }
        }

        // Set UV duplicate flag for optimization (skip UV Sync if no duplicates)
        RingData.bHasUVDuplicates = (NumWelded > 0);

        // ===== RepresentativeIndices for PostProcessing Vertices (using cache) =====
        const int32 NumPostProcessing = RingData.SmoothingRegionIndices.Num();
        if (NumPostProcessing > 0)
        {
            RingData.SmoothingRegionRepresentativeIndices.Reset(NumPostProcessing);
            RingData.SmoothingRegionRepresentativeIndices.AddUninitialized(NumPostProcessing);

            int32 PPNumWelded = 0;
            for (int32 i = 0; i < NumPostProcessing; ++i)
            {
                const uint32 VertIdx = RingData.SmoothingRegionIndices[i];

                // O(1) lookup - same pattern
                const FIntVector* PosKey = CachedVertexToPosition.Find(VertIdx);
                if (PosKey)
                {
                    const uint32* Representative = CachedPositionToRepresentative.Find(*PosKey);
                    const uint32 RepIdx = Representative ? *Representative : VertIdx;
                    RingData.SmoothingRegionRepresentativeIndices[i] = RepIdx;

                    if (RepIdx != VertIdx)
                    {
                        PPNumWelded++;
                    }
                }
                else
                {
                    RingData.SmoothingRegionRepresentativeIndices[i] = VertIdx;
                }
            }

            // Set UV duplicate flag for PostProcessing
            RingData.bSmoothingRegionHasUVDuplicates = (PPNumWelded > 0);

            UE_LOG(LogFleshRingVertices, Verbose,
                TEXT("BuildRepresentativeIndices (cached): Affected=%d (welded=%d), PostProcessing=%d (welded=%d)"),
                NumAffected, NumWelded, NumPostProcessing, PPNumWelded);
        }
        else
        {
            RingData.bSmoothingRegionHasUVDuplicates = false;

            UE_LOG(LogFleshRingVertices, Verbose,
                TEXT("BuildRepresentativeIndices (cached): Affected=%d (welded=%d), PostProcessing=0"),
                NumAffected, NumWelded);
        }

        return;  // Optimized path complete
    }

    // ================================================================
    // Fallback: use legacy logic if no cache (first call)
    // ================================================================
    constexpr float WeldPrecision = 0.001f;  // Same precision as SelectVertices

    // Step 1: Position-based grouping and representative selection
    TMap<FIntVector, uint32> PositionToRepresentative;

    for (const FAffectedVertex& Vert : RingData.Vertices)
    {
        const uint32 VertIdx = Vert.VertexIndex;
        const FVector3f& Pos = AllVertices[VertIdx];

        FIntVector PosKey(
            FMath::RoundToInt(Pos.X / WeldPrecision),
            FMath::RoundToInt(Pos.Y / WeldPrecision),
            FMath::RoundToInt(Pos.Z / WeldPrecision)
        );

        uint32* ExistingRep = PositionToRepresentative.Find(PosKey);
        if (ExistingRep)
        {
            *ExistingRep = FMath::Min(*ExistingRep, VertIdx);
        }
        else
        {
            PositionToRepresentative.Add(PosKey, VertIdx);
        }
    }

    // Step 2: Build RepresentativeIndices for Affected Vertices
    const int32 NumAffected = RingData.Vertices.Num();
    RingData.RepresentativeIndices.Reset(NumAffected);
    RingData.RepresentativeIndices.AddUninitialized(NumAffected);

    int32 NumWelded = 0;
    for (int32 i = 0; i < NumAffected; ++i)
    {
        const FAffectedVertex& Vert = RingData.Vertices[i];
        const FVector3f& Pos = AllVertices[Vert.VertexIndex];

        FIntVector PosKey(
            FMath::RoundToInt(Pos.X / WeldPrecision),
            FMath::RoundToInt(Pos.Y / WeldPrecision),
            FMath::RoundToInt(Pos.Z / WeldPrecision)
        );

        const uint32 Representative = PositionToRepresentative[PosKey];
        RingData.RepresentativeIndices[i] = Representative;

        if (Representative != Vert.VertexIndex)
        {
            NumWelded++;
        }
    }

    // Set UV duplicate flag for optimization (skip UV Sync if no duplicates)
    RingData.bHasUVDuplicates = (NumWelded > 0);

    // Step 3: Build RepresentativeIndices for PostProcessing Vertices
    const int32 NumPostProcessing = RingData.SmoothingRegionIndices.Num();
    if (NumPostProcessing > 0)
    {
        TMap<FIntVector, uint32> PPPositionToRepresentative;

        for (uint32 VertIdx : RingData.SmoothingRegionIndices)
        {
            const FVector3f& Pos = AllVertices[VertIdx];

            FIntVector PosKey(
                FMath::RoundToInt(Pos.X / WeldPrecision),
                FMath::RoundToInt(Pos.Y / WeldPrecision),
                FMath::RoundToInt(Pos.Z / WeldPrecision)
            );

            uint32* ExistingRep = PPPositionToRepresentative.Find(PosKey);
            if (ExistingRep)
            {
                *ExistingRep = FMath::Min(*ExistingRep, VertIdx);
            }
            else
            {
                PPPositionToRepresentative.Add(PosKey, VertIdx);
            }
        }

        RingData.SmoothingRegionRepresentativeIndices.Reset(NumPostProcessing);
        RingData.SmoothingRegionRepresentativeIndices.AddUninitialized(NumPostProcessing);

        int32 PPNumWelded = 0;
        for (int32 i = 0; i < NumPostProcessing; ++i)
        {
            const uint32 VertIdx = RingData.SmoothingRegionIndices[i];
            const FVector3f& Pos = AllVertices[VertIdx];

            FIntVector PosKey(
                FMath::RoundToInt(Pos.X / WeldPrecision),
                FMath::RoundToInt(Pos.Y / WeldPrecision),
                FMath::RoundToInt(Pos.Z / WeldPrecision)
            );

            const uint32 Representative = PPPositionToRepresentative[PosKey];
            RingData.SmoothingRegionRepresentativeIndices[i] = Representative;

            if (Representative != VertIdx)
            {
                PPNumWelded++;
            }
        }

        // Set UV duplicate flag for PostProcessing
        RingData.bSmoothingRegionHasUVDuplicates = (PPNumWelded > 0);

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("BuildRepresentativeIndices (fallback): Affected=%d (welded=%d), PostProcessing=%d (welded=%d)"),
            NumAffected, NumWelded, NumPostProcessing, PPNumWelded);
    }
    else
    {
        RingData.bSmoothingRegionHasUVDuplicates = false;

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("BuildRepresentativeIndices (fallback): Affected=%d (welded=%d), PostProcessing=0"),
            NumAffected, NumWelded);
    }
}

// ============================================================================
// BuildHopDistanceData - Build extended region for hop-based smoothing (full mesh BFS)
// ============================================================================
// Optimization: Uses topology cache - eliminates per-frame O(T) adjacency map rebuild

void FFleshRingAffectedVerticesManager::BuildHopDistanceData(
    FRingAffectedData& RingData,
    const TArray<uint32>& MeshIndices,
    const TArray<FVector3f>& AllVertices,
    int32 MaxHops,
    EFalloffType FalloffType)
{
    const int32 NumAffected = RingData.Vertices.Num();
    const int32 NumTotalVertices = AllVertices.Num();

    if (NumAffected == 0 || MeshIndices.Num() == 0)
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("BuildHopDistanceData: No affected vertices or mesh indices"));
        return;
    }

    // ===== Step 1: Seeds = all Affected Vertices =====
    // Note: Topology cache is already built during RegisterAffectedVertices() initial phase
    // Seeds are full mesh vertex indices
    TSet<uint32> SeedSet;
    SeedSet.Reserve(NumAffected);
    for (const FAffectedVertex& AffVert : RingData.Vertices)
    {
        SeedSet.Add(AffVert.VertexIndex);
    }

    // ===== Step 2: BFS on full mesh (collect vertices within N-hops) =====
    // Uses cached adjacency map - eliminates per-frame rebuild
    // HopDistanceMap: full vertex index -> hop distance
    TMap<uint32, int32> HopDistanceMap;
    HopDistanceMap.Reserve(NumAffected * (MaxHops + 1));

    // Initialize seeds (Hop 0)
    TQueue<uint32> BfsQueue;
    for (uint32 SeedVertIdx : SeedSet)
    {
        HopDistanceMap.Add(SeedVertIdx, 0);
        BfsQueue.Enqueue(SeedVertIdx);
    }

    // BFS propagation (uses cached adjacency map)
    while (!BfsQueue.IsEmpty())
    {
        uint32 CurrentVertIdx;
        BfsQueue.Dequeue(CurrentVertIdx);

        const int32 CurrentHop = HopDistanceMap[CurrentVertIdx];

        // Stop propagation when MaxHops is reached
        if (CurrentHop >= MaxHops)
        {
            continue;
        }

        // Check neighbors (lookup from cache)
        const TArray<uint32>* NeighborsPtr = CachedFullAdjacencyMap.Find(CurrentVertIdx);
        if (!NeighborsPtr)
        {
            continue;
        }

        for (uint32 NeighborVertIdx : *NeighborsPtr)
        {
            // Propagate to unvisited neighbors
            if (!HopDistanceMap.Contains(NeighborVertIdx))
            {
                HopDistanceMap.Add(NeighborVertIdx, CurrentHop + 1);
                BfsQueue.Enqueue(NeighborVertIdx);
            }
        }
    }

    // ===== Step 2.5: UV Duplicate expansion (UV Seam Welding) =====
    // Include UV duplicates of all vertices in HopDistanceMap
    // This ensures all duplicates at UV seam are smoothed together to prevent cracks
    {
        // Create copy of current HopDistanceMap for iteration (prevent modification during iteration)
        TArray<TPair<uint32, int32>> CurrentEntries;
        CurrentEntries.Reserve(HopDistanceMap.Num());
        for (const auto& Pair : HopDistanceMap)
        {
            CurrentEntries.Add(TPair<uint32, int32>(Pair.Key, Pair.Value));
        }

        int32 NumDuplicatesAdded = 0;
        for (const auto& Entry : CurrentEntries)
        {
            const uint32 VertIdx = Entry.Key;
            const int32 Hop = Entry.Value;

            // Find position key of this vertex
            const FIntVector* PosKey = CachedVertexToPosition.Find(VertIdx);
            if (!PosKey)
            {
                continue;
            }

            // Find all vertices at the same position
            const TArray<uint32>* VerticesAtPos = CachedPositionToVertices.Find(*PosKey);
            if (!VerticesAtPos)
            {
                continue;
            }

            // Add UV duplicates with the same hop distance
            for (uint32 DuplicateIdx : *VerticesAtPos)
            {
                if (!HopDistanceMap.Contains(DuplicateIdx))
                {
                    HopDistanceMap.Add(DuplicateIdx, Hop);
                    NumDuplicatesAdded++;
                }
            }
        }

        if (NumDuplicatesAdded > 0)
        {
            UE_LOG(LogFleshRingVertices, Verbose,
                TEXT("Extended UV duplicates: %d vertices added for UV seam welding"),
                NumDuplicatesAdded);
        }
    }

    // ===== Step 3: Build ExtendedSmoothing* arrays =====
    const int32 NumExtended = HopDistanceMap.Num();
    RingData.SmoothingRegionIndices.Reset(NumExtended);
    RingData.SmoothingRegionHopDistances.Reset(NumExtended);
    RingData.SmoothingRegionInfluences.Reset(NumExtended);
    RingData.SmoothingRegionIsAnchor.Reset(NumExtended);
    RingData.MaxSmoothingHops = MaxHops;  // For blending coefficient calculation

    const float MaxHopsFloat = static_cast<float>(MaxHops);

    // Add seeds first (Hop 0)
    for (const FAffectedVertex& AffVert : RingData.Vertices)
    {
        RingData.SmoothingRegionIndices.Add(AffVert.VertexIndex);
        RingData.SmoothingRegionHopDistances.Add(0);
        RingData.SmoothingRegionInfluences.Add(1.0f);  // Seeds have influence 1.0
        RingData.SmoothingRegionIsAnchor.Add(1);       // Seeds are anchors (skip smoothing)
    }

    // Add reached vertices that are not seeds (Hop 1+)
    for (const auto& Pair : HopDistanceMap)
    {
        const uint32 VertIdx = Pair.Key;
        const int32 Hop = Pair.Value;

        // Seeds are already added
        if (Hop == 0)
        {
            continue;
        }

        RingData.SmoothingRegionIndices.Add(VertIdx);
        RingData.SmoothingRegionHopDistances.Add(Hop);
        RingData.SmoothingRegionIsAnchor.Add(0);  // Extended vertices receive smoothing

        // t = normalized hop distance (0 = seed, 1 = maxHops)
        const float t = static_cast<float>(Hop) / MaxHopsFloat;

        float Influence = 0.0f;
        switch (FalloffType)
        {
            case EFalloffType::Linear:
                Influence = 1.0f - t;
                break;

            case EFalloffType::Quadratic:
                Influence = (1.0f - t) * (1.0f - t);
                break;

            case EFalloffType::Hermite:
            default:
                {
                    const float OneMinusT = 1.0f - t;
                    Influence = OneMinusT * OneMinusT * (1.0f + 2.0f * t);
                }
                break;
        }

        RingData.SmoothingRegionInfluences.Add(FMath::Clamp(Influence, 0.0f, 1.0f));
    }

    // ===== Step 4: Build Laplacian adjacency data for extended region (using cache) =====
    // [Modified] Pass CachedVertexLayerTypes instead of CachedFullAdjacencyMap
    // BuildSmoothingRegionLaplacianAdjacency internally uses CachedWeldedNeighborPositions
    BuildSmoothingRegionLaplacianAdjacency_HopBased(RingData, CachedVertexLayerTypes);

    // ===== Step 4.5: Build PBD adjacency data for extended region (for Tolerance-based PBD) =====
    // Required when using PBD Edge Constraint in HopBased mode
    BuildSmoothingRegionPBDAdjacency_HopBased(RingData, AllVertices);

    // ===== Step 5: Build RepresentativeIndices for extended region (UV seam welding) =====
    // Ensures UV seam vertices receive identical delta in Heat Propagation
    {
        RingData.SmoothingRegionRepresentativeIndices.Reset(NumExtended);
        RingData.SmoothingRegionRepresentativeIndices.AddUninitialized(NumExtended);

        int32 NumWelded = 0;
        for (int32 i = 0; i < NumExtended; ++i)
        {
            const uint32 VertIdx = RingData.SmoothingRegionIndices[i];

            // O(1) lookup from cache
            const FIntVector* PosKey = CachedVertexToPosition.Find(VertIdx);
            if (PosKey)
            {
                const uint32* Representative = CachedPositionToRepresentative.Find(*PosKey);
                const uint32 RepIdx = Representative ? *Representative : VertIdx;
                RingData.SmoothingRegionRepresentativeIndices[i] = RepIdx;

                if (RepIdx != VertIdx)
                {
                    NumWelded++;
                }
            }
            else
            {
                // Cache miss - use self as representative
                RingData.SmoothingRegionRepresentativeIndices[i] = VertIdx;
            }
        }

        // Set UV duplicate flag for Extended region
        RingData.bSmoothingRegionHasUVDuplicates = (NumWelded > 0);

        UE_LOG(LogFleshRingVertices, Verbose,
            TEXT("BuildHopDistanceData: SmoothingRegionRepresentativeIndices built, %d vertices (welded=%d)"),
            NumExtended, NumWelded);
    }

    // ===== Step 6: Update HopBasedInfluences (for Affected region) =====
    RingData.HopBasedInfluences.Reset(NumAffected);
    RingData.HopBasedInfluences.AddUninitialized(NumAffected);
    for (int32 i = 0; i < NumAffected; ++i)
    {
        RingData.HopBasedInfluences[i] = 1.0f;  // All seeds have 1.0
    }

    // ===== Step 7: Build triangle adjacency data for Extended region (for NormalRecomputeCS) =====
    // Build triangle adjacency info for SmoothingRegionIndices
    // Used in Rodrigues-based normal recomputation
    {
        RingData.SmoothingRegionAdjacencyOffsets.Reset();
        RingData.SmoothingRegionAdjacencyTriangles.Reset();

        if (!bTopologyCacheBuilt || CachedVertexTriangles.Num() == 0)
        {
            UE_LOG(LogFleshRingVertices, Warning,
                TEXT("BuildHopDistanceData: Topology cache not built for Extended adjacency"));
        }
        else
        {
            // Step 7-1: Count triangles for each Extended vertex
            TArray<int32> AdjCounts;
            AdjCounts.SetNumZeroed(NumExtended);

            for (int32 ExtIdx = 0; ExtIdx < NumExtended; ++ExtIdx)
            {
                const uint32 VertexIndex = RingData.SmoothingRegionIndices[ExtIdx];
                const TArray<uint32>* TrianglesPtr = CachedVertexTriangles.Find(VertexIndex);
                if (TrianglesPtr)
                {
                    AdjCounts[ExtIdx] = TrianglesPtr->Num();
                }
            }

            // Step 7-2: Build offset array (prefix sum)
            RingData.SmoothingRegionAdjacencyOffsets.SetNum(NumExtended + 1);
            RingData.SmoothingRegionAdjacencyOffsets[0] = 0;

            for (int32 i = 0; i < NumExtended; ++i)
            {
                RingData.SmoothingRegionAdjacencyOffsets[i + 1] = RingData.SmoothingRegionAdjacencyOffsets[i] + AdjCounts[i];
            }

            const uint32 TotalAdjacencies = RingData.SmoothingRegionAdjacencyOffsets[NumExtended];

            // Step 7-3: Fill triangle array (direct copy from cache)
            RingData.SmoothingRegionAdjacencyTriangles.SetNum(TotalAdjacencies);

            for (int32 ExtIdx = 0; ExtIdx < NumExtended; ++ExtIdx)
            {
                const uint32 VertexIndex = RingData.SmoothingRegionIndices[ExtIdx];
                const TArray<uint32>* TrianglesPtr = CachedVertexTriangles.Find(VertexIndex);

                if (TrianglesPtr && TrianglesPtr->Num() > 0)
                {
                    const uint32 Offset = RingData.SmoothingRegionAdjacencyOffsets[ExtIdx];
                    FMemory::Memcpy(
                        &RingData.SmoothingRegionAdjacencyTriangles[Offset],
                        TrianglesPtr->GetData(),
                        TrianglesPtr->Num() * sizeof(uint32)
                    );
                }
            }

            UE_LOG(LogFleshRingVertices, Verbose,
                TEXT("BuildHopDistanceData: SmoothingRegionAdjacencyData built, %d vertices, %d triangles (avg %.1f tri/vert)"),
                NumExtended, TotalAdjacencies,
                NumExtended > 0 ? static_cast<float>(TotalAdjacencies) / NumExtended : 0.0f);
        }
    }

    // Statistics log
    const int32 NumNewVertices = NumExtended - NumAffected;
    UE_LOG(LogFleshRingVertices, Log,
        TEXT("BuildHopDistanceData: %d seeds → %d smoothing (%d-hop, +%d extended)"),
        NumAffected, NumExtended, MaxHops, NumNewVertices);
}
