// ============================================================================
// FleshRing Affected Vertices System - Implementation
// FleshRing 영향받는 버텍스 시스템 - 구현부
// ============================================================================
// Purpose: Track and manage vertices affected by each Ring
// 목적: 각 링에 영향받는 버텍스 추적 및 관리
// Role B: Deformation Algorithm (Week 2)
// 역할 B: 변형 알고리즘 (Week 2)

#include "FleshRingAffectedVertices.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"

#include "Components/SkeletalMeshComponent.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Materials/MaterialInterface.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingVertices, Log, All);

// ============================================================================
// Layer Type Detection from Material Name
// 머티리얼 이름에서 레이어 타입 감지
// ============================================================================

namespace FleshRingLayerUtils
{
    /**
     * Detect layer type from material name using keyword matching
     * 키워드 매칭으로 머티리얼 이름에서 레이어 타입 감지
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
        // 특정성 순서로 체크 (더 구체적인 레이어 먼저)

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
     * 스켈레탈 메시 섹션에서 버텍스별 레이어 타입 배열 빌드
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
// FVertexSpatialHash Implementation
// 버텍스 공간 해시 구현 (O(n) → O(1) 쿼리 최적화)
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
// 거리 기반 버텍스 선택기 구현
// ============================================================================

void FDistanceBasedVertexSelector::SelectVertices(
    const FVertexSelectionContext& Context,
    TArray<FAffectedVertex>& OutAffected)
{
    OutAffected.Reset();

    // Context에서 필요한 데이터 추출
    const FFleshRingSettings& Ring = Context.RingSettings;
    const FTransform& BoneTransform = Context.BoneTransform;
    const TArray<FVector3f>& AllVertices = Context.AllVertices;

    // OBB 지원: SDFCache가 유효하면 BoundsMin/Max와 LocalToComponent 사용
    const bool bUseOBB = Context.SDFCache && Context.SDFCache->bCached;

    // Reserve estimated capacity (assume ~25% vertices affected)
    OutAffected.Reserve(AllVertices.Num() / 4);

    if (bUseOBB)
    {
        // ===== OBB 기반 버텍스 선택 (GPU SDF와 정확히 일치) =====
        // 비균등 스케일 + 회전 조합에서 InverseTransformPosition 사용 필수!
        // Inverse().TransformPosition()은 스케일과 회전 순서가 잘못됨
        const FTransform& LocalToComponent = Context.SDFCache->LocalToComponent;
        const FVector BoundsMin = FVector(Context.SDFCache->BoundsMin);
        const FVector BoundsMax = FVector(Context.SDFCache->BoundsMax);

        // [디버그] OBB 변환 정보 로그 (스케일 확인용)
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

        // Influence 계산용 파라미터 (로컬 스페이스 기준, 스케일 미적용)
        const float RingRadius = Ring.RingRadius;
        const float RingThickness = Ring.RingThickness;
        const float HalfWidth = Ring.RingHeight / 2.0f;

        // ===== Spatial Hash 사용 시 O(1) 쿼리, 없으면 브루트포스 O(n) =====
        TArray<int32> CandidateIndices;
        if (Context.SpatialHash && Context.SpatialHash->IsBuilt())
        {
            // Spatial Hash로 OBB 내 후보 추출 (O(1))
            Context.SpatialHash->QueryOBB(LocalToComponent, BoundsMin, BoundsMax, CandidateIndices);
            UE_LOG(LogFleshRingVertices, Verbose,
                TEXT("Ring[%d]: SpatialHash query returned %d candidates (from %d total)"),
                Context.RingIndex, CandidateIndices.Num(), AllVertices.Num());
        }
        else
        {
            // 브루트포스 폴백: 모든 버텍스 순회
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
                    continue; // 지정된 레이어가 아니면 스킵
                }
            }

            const FVector VertexPos = FVector(AllVertices[VertexIdx]);

            // Component Space → Local Space 변환
            // InverseTransformPosition: (Rot^-1 * (V - Trans)) / Scale (올바른 순서)
            const FVector LocalPos = LocalToComponent.InverseTransformPosition(VertexPos);

            // OBB 경계 체크 (SpatialHash 미사용 시에만 필요, QueryOBB는 이미 체크함)
            if (!Context.SpatialHash || !Context.SpatialHash->IsBuilt())
            {
                if (LocalPos.X < BoundsMin.X || LocalPos.X > BoundsMax.X ||
                    LocalPos.Y < BoundsMin.Y || LocalPos.Y > BoundsMax.Y ||
                    LocalPos.Z < BoundsMin.Z || LocalPos.Z > BoundsMax.Z)
                {
                    continue; // OBB 밖 - 스킵
                }
            }

            // 로컬 스페이스에서 Ring 기하에 대한 거리 계산
            // 링 축 = Z축 (로컬 스페이스), 링 중심 = 원점
            const float AxisDistance = LocalPos.Z;
            const FVector2D RadialVec(LocalPos.X, LocalPos.Y);
            const float RadialDistance = RadialVec.Size();

            // Influence 계산 (Ring 표면으로부터의 거리 기반)
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
        // ===== Fallback: 원통형 모델 (Manual 모드, SDFCache 없을 때) =====
        // Manual 모드 전용 RingOffset/RingRotation 사용 (MeshOffset/MeshRotation 아님!)
        const FQuat BoneRotation = BoneTransform.GetRotation();
        const FVector WorldRingOffset = BoneRotation.RotateVector(Ring.RingOffset);
        const FVector RingCenter = BoneTransform.GetLocation() + WorldRingOffset;
        const FQuat WorldRingRotation = BoneRotation * Ring.RingRotation;
        const FVector RingAxis = WorldRingRotation.RotateVector(FVector::ZAxisVector);

        // Manual 모드는 스케일 없음 (RingRadius/RingThickness/RingHeight가 직접 단위)
        const float MaxDistance = Ring.RingRadius + Ring.RingThickness;
        const float HalfWidth = Ring.RingHeight / 2.0f;

        // ===== Spatial Hash OBB 쿼리로 후보 축소 (O(N) → O(K)) =====
        TArray<int32> CandidateIndices;
        if (Context.SpatialHash && Context.SpatialHash->IsBuilt())
        {
            // Ring 회전을 반영한 OBB 트랜스폼
            FTransform RingLocalToComponent;
            RingLocalToComponent.SetLocation(RingCenter);
            RingLocalToComponent.SetRotation(WorldRingRotation);
            RingLocalToComponent.SetScale3D(FVector::OneVector);

            const FVector LocalMin(-MaxDistance, -MaxDistance, -HalfWidth);
            const FVector LocalMax(MaxDistance, MaxDistance, HalfWidth);
            Context.SpatialHash->QueryOBB(RingLocalToComponent, LocalMin, LocalMax, CandidateIndices);

            UE_LOG(LogFleshRingVertices, Log,
                TEXT("Manual Ring[%d]: SpatialHash OBB query returned %d candidates (from %d total)"),
                Context.RingIndex, CandidateIndices.Num(), AllVertices.Num());
        }
        else
        {
            // 폴백: 전체 순회
            CandidateIndices.Reserve(AllVertices.Num());
            for (int32 i = 0; i < AllVertices.Num(); ++i)
            {
                CandidateIndices.Add(i);
            }
        }

        // 후보만 순회 (기존: 전체 순회)
        for (int32 VertexIdx : CandidateIndices)
        {
            // === Layer Type Filtering ===
            if (Context.VertexLayerTypes && Context.VertexLayerTypes->IsValidIndex(VertexIdx))
            {
                const EFleshRingLayerType LayerType = (*Context.VertexLayerTypes)[VertexIdx];
                if (!Context.RingSettings.IsLayerAffected(LayerType))
                {
                    continue; // 지정된 레이어가 아니면 스킵
                }
            }

            const FVector VertexPos = FVector(AllVertices[VertexIdx]);
            const FVector ToVertex = VertexPos - RingCenter;
            const float AxisDistance = FVector::DotProduct(ToVertex, RingAxis);
            const FVector RadialVec = ToVertex - RingAxis * AxisDistance;
            const float RadialDistance = RadialVec.Size();

            if (RadialDistance <= MaxDistance && FMath::Abs(AxisDistance) <= HalfWidth)
            {
                // GPU가 Influence를 재계산하므로 CPU에서는 1.0으로 고정 (placeholder)
                // GPU: CalculateManualInfluence() in FleshRingTightnessCS.usf
                OutAffected.Add(FAffectedVertex(
                    static_cast<uint32>(VertexIdx),
                    RadialDistance,
                    1.0f  // GPU가 재계산
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
// CalculateFalloff - 감쇠 곡선 계산
// ============================================================================
float FDistanceBasedVertexSelector::CalculateFalloff(
    float Distance,
    float MaxDistance,
    EFalloffType InFalloffType) const
{
    // Normalize distance to 0-1 range
    // 거리를 0~1 범위로 정규화
    const float NormalizedDist = FMath::Clamp(Distance / MaxDistance, 0.0f, 1.0f);

    // Inverted: closer = higher influence
    // 반전: 가까울수록 영향도 높음
    const float T = 1.0f - NormalizedDist;

    switch (InFalloffType)
    {
    case EFalloffType::Quadratic:
        // Smoother falloff near center
        // 중심 근처에서 더 부드러운 감쇠
        return T * T;

    case EFalloffType::Hermite:
        // Hermite S-curve (smooth in, smooth out)
        // Hermite S-커브 (시작과 끝 모두 부드러움)
        return T * T * (3.0f - 2.0f * T);

    case EFalloffType::Linear:
    default:
        // Simple linear falloff
        // 단순 선형 감쇠
        return T;
    }
}

// ============================================================================
// SelectPostProcessingVertices - Manual 모드용 후처리 버텍스 선택
// ============================================================================
void FDistanceBasedVertexSelector::SelectPostProcessingVertices(
    const FVertexSelectionContext& Context,
    const TArray<FAffectedVertex>& AffectedVertices,
    FRingAffectedData& OutRingData)
{
    OutRingData.PostProcessingIndices.Reset();
    OutRingData.PostProcessingInfluences.Reset();
    OutRingData.PostProcessingIsAnchor.Reset();
    // Note: PostProcessingLayerTypes는 FullMeshLayerTypes로 대체됨 (deprecated/removed)

    // 원본 Affected Vertices를 빠르게 조회하기 위한 Set (앵커 판정용)
    TSet<uint32> AffectedSet;
    AffectedSet.Reserve(AffectedVertices.Num());
    for (const FAffectedVertex& V : AffectedVertices)
    {
        AffectedSet.Add(V.VertexIndex);
    }

    // ★ 모든 Smoothing이 꺼져있으면 원본 Affected Vertices만 복사
    const bool bAnySmoothingEnabled =
        Context.RingSettings.bEnableRadialSmoothing ||
        Context.RingSettings.bEnableLaplacianSmoothing ||
        Context.RingSettings.bEnablePBDEdgeConstraint;

    if (!bAnySmoothingEnabled)
    {
        OutRingData.PostProcessingIndices.Reserve(AffectedVertices.Num());
        OutRingData.PostProcessingInfluences.Reserve(AffectedVertices.Num());
        OutRingData.PostProcessingIsAnchor.Reserve(AffectedVertices.Num());

        for (const FAffectedVertex& V : AffectedVertices)
        {
            OutRingData.PostProcessingIndices.Add(V.VertexIndex);
            OutRingData.PostProcessingInfluences.Add(1.0f);
            OutRingData.PostProcessingIsAnchor.Add(1);  // 원본 Affected = 앵커
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("PostProcessing (Manual): Smoothing disabled, using %d affected vertices"),
            OutRingData.PostProcessingIndices.Num());
        return;
    }

    const float BoundsZTop = Context.RingSettings.SmoothingBoundsZTop;
    const float BoundsZBottom = Context.RingSettings.SmoothingBoundsZBottom;
    const FFleshRingSettings& Ring = Context.RingSettings;
    const TArray<FVector3f>& AllVertices = Context.AllVertices;

    // Z 확장이 없으면 원본 Affected Vertices를 그대로 사용
    if (BoundsZTop < 0.01f && BoundsZBottom < 0.01f)
    {
        OutRingData.PostProcessingIndices.Reserve(AffectedVertices.Num());
        OutRingData.PostProcessingInfluences.Reserve(AffectedVertices.Num());
        OutRingData.PostProcessingIsAnchor.Reserve(AffectedVertices.Num());

        for (const FAffectedVertex& V : AffectedVertices)
        {
            OutRingData.PostProcessingIndices.Add(V.VertexIndex);
            OutRingData.PostProcessingInfluences.Add(1.0f);
            OutRingData.PostProcessingIsAnchor.Add(1);  // 원본 Affected = 앵커
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("PostProcessing (Manual): No Z extension, using %d affected vertices"),
            OutRingData.PostProcessingIndices.Num());
        return;
    }

    // Manual 모드: Ring 파라미터로 직접 계산 (Component Space)
    const FQuat BoneRotation = Context.BoneTransform.GetRotation();
    const FVector WorldRingOffset = BoneRotation.RotateVector(Ring.RingOffset);
    const FVector RingCenter = Context.BoneTransform.GetLocation() + WorldRingOffset;
    const FQuat WorldRingRotation = BoneRotation * Ring.RingRotation;
    const FVector RingAxis = WorldRingRotation.RotateVector(FVector::ZAxisVector);

    const float HalfWidth = Ring.RingHeight / 2.0f;
    const float MaxRadialDistance = Ring.RingRadius + Ring.RingThickness;

    // Z 확장된 축 방향 범위
    const float OriginalZMin = -HalfWidth;
    const float OriginalZMax = HalfWidth;
    const float ExtendedZMin = OriginalZMin - BoundsZBottom;
    const float ExtendedZMax = OriginalZMax + BoundsZTop;

    OutRingData.PostProcessingIndices.Reserve(AllVertices.Num() / 4);
    OutRingData.PostProcessingInfluences.Reserve(AllVertices.Num() / 4);
    OutRingData.PostProcessingIsAnchor.Reserve(AllVertices.Num() / 4);

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

        // 반경 방향은 원본 범위, 축 방향은 확장 범위
        if (RadialDistance <= MaxRadialDistance &&
            AxisDistance >= ExtendedZMin && AxisDistance <= ExtendedZMax)
        {
            // Influence 계산: 원본 Z 범위 내 = 1.0, 확장 영역 = falloff
            float Influence = 1.0f;

            if (AxisDistance < OriginalZMin)
            {
                // 하단 확장 영역: 거리에 따라 falloff
                float Dist = OriginalZMin - AxisDistance;
                Influence = 1.0f - FMath::Clamp(Dist / BoundsZBottom, 0.0f, 1.0f);
                Influence = FMath::InterpEaseInOut(0.0f, 1.0f, Influence, 2.0f);
                ExtendedCount++;
            }
            else if (AxisDistance > OriginalZMax)
            {
                // 상단 확장 영역: 거리에 따라 falloff
                float Dist = AxisDistance - OriginalZMax;
                Influence = 1.0f - FMath::Clamp(Dist / BoundsZTop, 0.0f, 1.0f);
                Influence = FMath::InterpEaseInOut(0.0f, 1.0f, Influence, 2.0f);
                ExtendedCount++;
            }
            else
            {
                CoreCount++;
            }

            // 앵커 판정: 원본 AffectedVertices에 포함된 버텍스만 앵커
            const bool bIsAnchor = AffectedSet.Contains(static_cast<uint32>(VertexIdx));
            if (bIsAnchor) AnchorCount++;

            OutRingData.PostProcessingIndices.Add(static_cast<uint32>(VertexIdx));
            OutRingData.PostProcessingInfluences.Add(Influence);
            OutRingData.PostProcessingIsAnchor.Add(bIsAnchor ? 1 : 0);
        }
    }

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("PostProcessing (Manual): Selected %d vertices (Core=%d, ZExtended=%d, Anchors=%d) for Ring[%d], ZExtend=[%.1f, %.1f]"),
        OutRingData.PostProcessingIndices.Num(), CoreCount, ExtendedCount, AnchorCount,
        Context.RingIndex, BoundsZBottom, BoundsZTop);
}

// ============================================================================
// SDF Bounds-Based Vertex Selector Implementation
// SDF 바운드 기반 버텍스 선택기 구현
// ============================================================================

void FSDFBoundsBasedVertexSelector::SelectVertices(
    const FVertexSelectionContext& Context,
    TArray<FAffectedVertex>& OutAffected)
{
    OutAffected.Reset();

    // Context에서 SDF 캐시 확인
    // SDFCache가 nullptr이거나 유효하지 않으면 선택 안 함
    if (!Context.SDFCache || !Context.SDFCache->IsValid())
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("SDFBoundsBasedSelector: No valid SDF cache for Ring[%d] '%s', skipping"),
            Context.RingIndex, *Context.RingSettings.BoneName.ToString());
        return;
    }

    // OBB 변환: Component Space → Local Space
    // 비균등 스케일 + 회전 조합에서 InverseTransformPosition 사용 필수!
    // Inverse().TransformPosition()은 스케일과 회전 순서가 잘못됨
    const FTransform& LocalToComponent = Context.SDFCache->LocalToComponent;

    // 버텍스 필터링 바운드 (SDFBoundsExpandX/Y 적용)
    // NOTE: SDF 텍스처 바운드는 원래 크기 유지, 버텍스 필터링만 확장
    const float ExpandX = Context.RingSettings.SDFBoundsExpandX;
    const float ExpandY = Context.RingSettings.SDFBoundsExpandY;

    FVector BoundsMin = FVector(Context.SDFCache->BoundsMin);
    FVector BoundsMax = FVector(Context.SDFCache->BoundsMax);

    // Ring 로컬 스페이스에서 X, Y 방향 확장 (Z는 Ring 축이므로 유지)
    BoundsMin.X -= ExpandX;
    BoundsMin.Y -= ExpandY;
    BoundsMax.X += ExpandX;
    BoundsMax.Y += ExpandY;

    const TArray<FVector3f>& AllVertices = Context.AllVertices;

    // [디버그] LocalToComponent 변환 정보 로그 (스케일 확인용)
    UE_LOG(LogFleshRingVertices, Log,
        TEXT("SDFBoundsSelector: Ring[%d] LocalToComponent Scale=%s, Rot=%s, Trans=%s"),
        Context.RingIndex,
        *LocalToComponent.GetScale3D().ToString(),
        *LocalToComponent.GetRotation().Rotator().ToString(),
        *LocalToComponent.GetLocation().ToString());

    // ================================================================
    // UV Seam Welding: Position Group 기반 선택
    // ================================================================
    // 목적: UV seam에서 분리된 버텍스들이 모두 함께 선택되도록 보장
    // 방법: 위치 기반으로 버텍스 그룹화 → 그룹 내 하나라도 선택되면 전체 선택
    // ================================================================

    constexpr float WeldPrecision = 0.001f;  // 위치 양자화 정밀도

    // Step 1: 캐시된 맵 사용 또는 폴백으로 로컬 맵 빌드
    // Use cached map if available, otherwise fallback to local build (slow)
    TMap<FIntVector, TArray<uint32>> LocalPositionToVertices;
    const TMap<FIntVector, TArray<uint32>>* PositionToVerticesPtr = Context.CachedPositionToVertices;

    if (!PositionToVerticesPtr || PositionToVerticesPtr->Num() == 0)
    {
        // 폴백: 캐시 없음, 로컬 맵 빌드 (O(N) - 느림!)
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

    // ===== Spatial Hash 사용 시 O(1) 쿼리, 없으면 브루트포스 O(n) =====
    TArray<int32> CandidateIndices;
    if (Context.SpatialHash && Context.SpatialHash->IsBuilt())
    {
        // Spatial Hash로 OBB 내 후보 추출 (O(1))
        Context.SpatialHash->QueryOBB(LocalToComponent, BoundsMin, BoundsMax, CandidateIndices);
        UE_LOG(LogFleshRingVertices, Verbose,
            TEXT("SDFBoundsSelector Ring[%d]: SpatialHash query returned %d candidates (from %d total)"),
            Context.RingIndex, CandidateIndices.Num(), AllVertices.Num());
    }
    else
    {
        // 브루트포스 폴백: 모든 버텍스 순회
        CandidateIndices.Reserve(AllVertices.Num());
        for (int32 i = 0; i < AllVertices.Num(); ++i)
        {
            CandidateIndices.Add(i);
        }
    }

    // Step 2: 선택된 위치들 수집 (Position Group 단위)
    // 어느 버텍스라도 선택되면 해당 위치의 모든 버텍스가 선택됨
    TSet<FIntVector> SelectedPositions;

    for (int32 VertexIdx : CandidateIndices)
    {
        // === Layer Type Filtering ===
        if (Context.VertexLayerTypes && Context.VertexLayerTypes->IsValidIndex(VertexIdx))
        {
            const EFleshRingLayerType LayerType = (*Context.VertexLayerTypes)[VertexIdx];
            if (!Context.RingSettings.IsLayerAffected(LayerType))
            {
                continue; // 지정된 레이어가 아니면 스킵
            }
        }

        const FVector VertexPos = FVector(AllVertices[VertexIdx]);

        // Component Space → Local Space 변환
        const FVector LocalPos = LocalToComponent.InverseTransformPosition(VertexPos);

        // Local Space에서 AABB 포함 테스트 (SpatialHash 미사용 시에만 필요)
        if (!Context.SpatialHash || !Context.SpatialHash->IsBuilt())
        {
            if (LocalPos.X < BoundsMin.X || LocalPos.X > BoundsMax.X ||
                LocalPos.Y < BoundsMin.Y || LocalPos.Y > BoundsMax.Y ||
                LocalPos.Z < BoundsMin.Z || LocalPos.Z > BoundsMax.Z)
            {
                continue; // OBB 밖 - 스킵
            }
        }

        // 이 버텍스의 위치 키 추가 (그룹 전체가 선택됨)
        const FVector3f& Pos = AllVertices[VertexIdx];
        FIntVector PosKey(
            FMath::RoundToInt(Pos.X / WeldPrecision),
            FMath::RoundToInt(Pos.Y / WeldPrecision),
            FMath::RoundToInt(Pos.Z / WeldPrecision)
        );
        SelectedPositions.Add(PosKey);
    }

    // Step 3: 선택된 위치의 모든 버텍스 추가 (UV 중복 포함)
    OutAffected.Reserve(SelectedPositions.Num() * 2);  // 평균 2개의 UV 중복 가정

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
                    0.0f,  // RadialDistance: SDF 모드에서는 미사용
                    1.0f   // Influence: 최대값, GPU 셰이더가 CalculateInfluenceFromSDF()로 정제
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

void FSDFBoundsBasedVertexSelector::SelectPostProcessingVertices(
    const FVertexSelectionContext& Context,
    const TArray<FAffectedVertex>& AffectedVertices,
    FRingAffectedData& OutRingData)
{
    OutRingData.PostProcessingIndices.Reset();
    OutRingData.PostProcessingInfluences.Reset();
    OutRingData.PostProcessingIsAnchor.Reset();
    // Note: PostProcessingLayerTypes는 FullMeshLayerTypes로 대체됨 (deprecated/removed)

    if (!Context.SDFCache || !Context.SDFCache->IsValid())
    {
        return;
    }

    // 원본 Affected Vertices를 빠르게 조회하기 위한 Set (앵커 판정용)
    TSet<uint32> AffectedSet;
    AffectedSet.Reserve(AffectedVertices.Num());
    for (const FAffectedVertex& V : AffectedVertices)
    {
        AffectedSet.Add(V.VertexIndex);
    }

    // ★ 모든 Smoothing이 꺼져있으면 Z 확장 없이 원본 Affected Vertices만 복사
    // ANY Smoothing ON → Z 확장 / Hop 기반 확장 가능
    // ALL Smoothing OFF → 확장 영역 불필요 (기본 SDF 볼륨만 사용, Tightness/Bulge만 동작)
    const bool bAnySmoothingEnabled =
        Context.RingSettings.bEnableRadialSmoothing ||
        Context.RingSettings.bEnableLaplacianSmoothing ||
        Context.RingSettings.bEnablePBDEdgeConstraint;

    if (!bAnySmoothingEnabled)
    {
        OutRingData.PostProcessingIndices.Reserve(AffectedVertices.Num());
        OutRingData.PostProcessingInfluences.Reserve(AffectedVertices.Num());
        OutRingData.PostProcessingIsAnchor.Reserve(AffectedVertices.Num());

        for (const FAffectedVertex& V : AffectedVertices)
        {
            OutRingData.PostProcessingIndices.Add(V.VertexIndex);
            OutRingData.PostProcessingInfluences.Add(1.0f);
            OutRingData.PostProcessingIsAnchor.Add(1);  // 원본 Affected = 앵커
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("PostProcessing: Smoothing disabled, using %d affected vertices (no Z extension)"),
            OutRingData.PostProcessingIndices.Num());
        return;
    }

    const float BoundsZTop = Context.RingSettings.SmoothingBoundsZTop;
    const float BoundsZBottom = Context.RingSettings.SmoothingBoundsZBottom;

    // Z 확장이 없으면 원본 Affected Vertices를 그대로 사용
    if (BoundsZTop < 0.01f && BoundsZBottom < 0.01f)
    {
        // 원본 복사
        OutRingData.PostProcessingIndices.Reserve(AffectedVertices.Num());
        OutRingData.PostProcessingInfluences.Reserve(AffectedVertices.Num());
        OutRingData.PostProcessingIsAnchor.Reserve(AffectedVertices.Num());

        for (const FAffectedVertex& V : AffectedVertices)
        {
            OutRingData.PostProcessingIndices.Add(V.VertexIndex);
            OutRingData.PostProcessingInfluences.Add(1.0f);  // 코어 버텍스는 1.0
            OutRingData.PostProcessingIsAnchor.Add(1);  // 원본 Affected = 앵커
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("PostProcessing: No Z extension, using %d affected vertices"),
            OutRingData.PostProcessingIndices.Num());
        return;
    }

    // Z 확장 범위 계산
    const FTransform& LocalToComponent = Context.SDFCache->LocalToComponent;
    const FTransform ComponentToLocal = LocalToComponent.Inverse();

    const FVector OriginalBoundsMin = FVector(Context.SDFCache->BoundsMin);
    const FVector OriginalBoundsMax = FVector(Context.SDFCache->BoundsMax);
    const TArray<FVector3f>& AllVertices = Context.AllVertices;

    // XY는 원본 유지, Z만 확장 (SmoothingBoundsZTop/Bottom)
    FVector ExtendedBoundsMin = OriginalBoundsMin;
    FVector ExtendedBoundsMax = OriginalBoundsMax;
    ExtendedBoundsMin.Z -= BoundsZBottom;
    ExtendedBoundsMax.Z += BoundsZTop;

    const float OriginalZSize = OriginalBoundsMax.Z - OriginalBoundsMin.Z;

    OutRingData.PostProcessingIndices.Reserve(AllVertices.Num() / 4);
    OutRingData.PostProcessingInfluences.Reserve(AllVertices.Num() / 4);
    OutRingData.PostProcessingIsAnchor.Reserve(AllVertices.Num() / 4);
    // Note: PostProcessingLayerTypes removed - using FullMeshLayerTypes for GPU direct lookup

    int32 CoreCount = 0;
    int32 ExtendedCount = 0;
    int32 AnchorCount = 0;

    for (int32 VertexIdx = 0; VertexIdx < AllVertices.Num(); ++VertexIdx)
    {
        const FVector VertexPos = FVector(AllVertices[VertexIdx]);
        const FVector LocalPos = ComponentToLocal.TransformPosition(VertexPos);

        // 확장된 Z 범위 내에 있는지 체크 (XY는 원본 범위)
        if (LocalPos.X >= OriginalBoundsMin.X && LocalPos.X <= OriginalBoundsMax.X &&
            LocalPos.Y >= OriginalBoundsMin.Y && LocalPos.Y <= OriginalBoundsMax.Y &&
            LocalPos.Z >= ExtendedBoundsMin.Z && LocalPos.Z <= ExtendedBoundsMax.Z)
        {
            // Influence 계산: 코어(원본 범위) = 1.0, Z 확장 영역 = falloff
            float Influence = 1.0f;

            if (LocalPos.Z < OriginalBoundsMin.Z)
            {
                // 하단 확장 영역: 거리에 따라 falloff
                float Dist = OriginalBoundsMin.Z - LocalPos.Z;
                Influence = 1.0f - FMath::Clamp(Dist / BoundsZBottom, 0.0f, 1.0f);
                Influence = FMath::InterpEaseInOut(0.0f, 1.0f, Influence, 2.0f);  // Smooth falloff
                ExtendedCount++;
            }
            else if (LocalPos.Z > OriginalBoundsMax.Z)
            {
                // 상단 확장 영역: 거리에 따라 falloff
                float Dist = LocalPos.Z - OriginalBoundsMax.Z;
                Influence = 1.0f - FMath::Clamp(Dist / BoundsZTop, 0.0f, 1.0f);
                Influence = FMath::InterpEaseInOut(0.0f, 1.0f, Influence, 2.0f);  // Smooth falloff
                ExtendedCount++;
            }
            else
            {
                CoreCount++;
            }

            // 앵커 판정: 원본 AffectedVertices에 포함된 버텍스만 앵커
            const bool bIsAnchor = AffectedSet.Contains(static_cast<uint32>(VertexIdx));
            if (bIsAnchor) AnchorCount++;

            OutRingData.PostProcessingIndices.Add(static_cast<uint32>(VertexIdx));
            OutRingData.PostProcessingInfluences.Add(Influence);
            OutRingData.PostProcessingIsAnchor.Add(bIsAnchor ? 1 : 0);
            // Note: LayerTypes는 FullMeshLayerTypes에서 GPU가 직접 조회
        }
    }

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("PostProcessing: Selected %d vertices (Core=%d, ZExtended=%d, Anchors=%d) for Ring[%d], ZExtend=[%.1f, %.1f]"),
        OutRingData.PostProcessingIndices.Num(), CoreCount, ExtendedCount, AnchorCount,
        Context.RingIndex, BoundsZBottom, BoundsZTop);
}

// ============================================================================
// FVirtualBandVertexSelector Implementation
// Virtual Band(ProceduralBand) 모드용 버텍스 선택 구현
// ============================================================================

float FVirtualBandVertexSelector::GetRadiusAtHeight(float LocalZ, const FProceduralBandSettings& BandSettings) const
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

    // ProceduralBand 설정 가져오기
    const FProceduralBandSettings& BandSettings = Ring.ProceduralBand;

    // 밴드 트랜스폼 계산 (Virtual Band 전용 BandOffset/BandRotation 사용)
    const FTransform& BoneTransform = Context.BoneTransform;
    const FQuat BoneRotation = BoneTransform.GetRotation();
    const FVector WorldBandOffset = BoneRotation.RotateVector(BandSettings.BandOffset);
    const FVector BandCenter = BoneTransform.GetLocation() + WorldBandOffset;
    const FQuat WorldBandRotation = BoneRotation * BandSettings.BandRotation;
    const FVector BandAxis = WorldBandRotation.RotateVector(FVector::ZAxisVector);

    // 높이 파라미터
    const float LowerHeight = BandSettings.Lower.Height;
    const float BandHeight = BandSettings.BandHeight;
    const float UpperHeight = BandSettings.Upper.Height;
    const float TotalHeight = LowerHeight + BandHeight + UpperHeight;

    // Tightness 영역: Band Section만 (-BandHeight/2 ~ +BandHeight/2)
    // 새 좌표계: Z=0이 Mid Band 중심
    const float TightnessZMin = -BandHeight * 0.5f;
    const float TightnessZMax = BandHeight * 0.5f;

    // Tightness Falloff 범위: 밴드가 조이면서 밀어내는 거리
    // Upper/Lower 반경 차이 = 불룩한 정도 = 조여야 할 거리
    const float UpperBulge = BandSettings.Upper.Radius - BandSettings.MidUpperRadius;
    const float LowerBulge = BandSettings.Lower.Radius - BandSettings.MidLowerRadius;
    const float TightnessFalloffRange = FMath::Max(FMath::Max(UpperBulge, LowerBulge), 1.0f);  // 최소 1.0 보장

    OutAffected.Reserve(AllVertices.Num() / 4);

    for (int32 VertexIdx = 0; VertexIdx < AllVertices.Num(); ++VertexIdx)
    {
        // === Layer Type Filtering ===
        if (Context.VertexLayerTypes && Context.VertexLayerTypes->IsValidIndex(VertexIdx))
        {
            const EFleshRingLayerType LayerType = (*Context.VertexLayerTypes)[VertexIdx];
            if (!Context.RingSettings.IsLayerAffected(LayerType))
            {
                continue; // 지정된 레이어가 아니면 스킵
            }
        }

        const FVector VertexPos = FVector(AllVertices[VertexIdx]);
        const FVector ToVertex = VertexPos - BandCenter;

        // 축 방향 거리
        const float AxisDistance = FVector::DotProduct(ToVertex, BandAxis);
        const float LocalZ = AxisDistance;

        // Band Section 범위 체크 (Tightness 영역)
        if (LocalZ < TightnessZMin || LocalZ > TightnessZMax)
        {
            continue;
        }

        // 반경 방향 거리
        const FVector RadialVec = ToVertex - BandAxis * AxisDistance;
        const float RadialDistance = RadialVec.Size();

        // 해당 높이에서의 밴드 반경 (가변 반경)
        const float BandRadius = GetRadiusAtHeight(LocalZ, BandSettings);

        // 밴드 표면보다 바깥에 있어야 Tightness 영향
        if (RadialDistance <= BandRadius)
        {
            continue;
        }

        // 밴드 표면과의 거리
        const float DistanceOutside = RadialDistance - BandRadius;

        // Falloff 범위 체크
        if (DistanceOutside > TightnessFalloffRange)
        {
            continue;
        }

        // Radial Influence (표면에 가까울수록 높음)
        const float RadialInfluence = CalculateFalloff(DistanceOutside, TightnessFalloffRange, Ring.FalloffType);

        // Axial Influence (Band 경계에서 거리에 따른 falloff)
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

void FVirtualBandVertexSelector::SelectPostProcessingVertices(
    const FVertexSelectionContext& Context,
    const TArray<FAffectedVertex>& AffectedVertices,
    FRingAffectedData& OutRingData)
{
    OutRingData.PostProcessingIndices.Reset();
    OutRingData.PostProcessingInfluences.Reset();

    // Smoothing이 꺼져있으면 원본 Affected Vertices만 복사
    const bool bAnySmoothingEnabled =
        Context.RingSettings.bEnableRadialSmoothing ||
        Context.RingSettings.bEnableLaplacianSmoothing ||
        Context.RingSettings.bEnablePBDEdgeConstraint;

    if (!bAnySmoothingEnabled)
    {
        OutRingData.PostProcessingIndices.Reserve(AffectedVertices.Num());
        OutRingData.PostProcessingInfluences.Reserve(AffectedVertices.Num());

        for (const FAffectedVertex& V : AffectedVertices)
        {
            OutRingData.PostProcessingIndices.Add(V.VertexIndex);
            OutRingData.PostProcessingInfluences.Add(1.0f);
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("PostProcessing (VirtualBand): Smoothing disabled, using %d affected vertices"),
            OutRingData.PostProcessingIndices.Num());
        return;
    }

    const float BoundsZTop = Context.RingSettings.SmoothingBoundsZTop;
    const float BoundsZBottom = Context.RingSettings.SmoothingBoundsZBottom;
    const FFleshRingSettings& Ring = Context.RingSettings;
    const TArray<FVector3f>& AllVertices = Context.AllVertices;
    const FProceduralBandSettings& BandSettings = Ring.ProceduralBand;

    // Z 확장이 없으면 원본 Affected Vertices를 그대로 사용
    if (BoundsZTop < 0.01f && BoundsZBottom < 0.01f)
    {
        OutRingData.PostProcessingIndices.Reserve(AffectedVertices.Num());
        OutRingData.PostProcessingInfluences.Reserve(AffectedVertices.Num());

        for (const FAffectedVertex& V : AffectedVertices)
        {
            OutRingData.PostProcessingIndices.Add(V.VertexIndex);
            OutRingData.PostProcessingInfluences.Add(1.0f);
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("PostProcessing (VirtualBand): No Z extension, using %d affected vertices"),
            OutRingData.PostProcessingIndices.Num());
        return;
    }

    // 밴드 트랜스폼 계산 (Virtual Band 전용 BandOffset/BandRotation 사용)
    const FQuat BoneRotation = Context.BoneTransform.GetRotation();
    const FVector WorldBandOffset = BoneRotation.RotateVector(BandSettings.BandOffset);
    const FVector BandCenter = Context.BoneTransform.GetLocation() + WorldBandOffset;
    const FQuat WorldBandRotation = BoneRotation * BandSettings.BandRotation;
    const FVector BandAxis = WorldBandRotation.RotateVector(FVector::ZAxisVector);

    // Virtual Band 전체 높이
    const float LowerHeight = BandSettings.Lower.Height;
    const float BandHeight = BandSettings.BandHeight;
    const float UpperHeight = BandSettings.Upper.Height;
    const float TotalHeight = LowerHeight + BandHeight + UpperHeight;

    // 새 좌표계: Z=0이 Mid Band 중심
    const float MidOffset = LowerHeight + BandHeight * 0.5f;
    const float ZMin = -MidOffset;
    const float ZMax = TotalHeight - MidOffset;

    // 확장된 Z 범위 (전체 Virtual Band + Z 확장)
    const float ExtendedZMin = ZMin - BoundsZBottom;
    const float ExtendedZMax = ZMax + BoundsZTop;

    // 최대 반경 계산 (AABB 쿼리용)
    const float MaxRadius = FMath::Max(
        FMath::Max(BandSettings.Lower.Radius, BandSettings.Upper.Radius),
        FMath::Max(BandSettings.MidLowerRadius, BandSettings.MidUpperRadius)
    ) + Ring.RingThickness;

    OutRingData.PostProcessingIndices.Reserve(AllVertices.Num() / 4);
    OutRingData.PostProcessingInfluences.Reserve(AllVertices.Num() / 4);

    int32 CoreCount = 0;
    int32 ExtendedCount = 0;

    for (int32 VertexIdx = 0; VertexIdx < AllVertices.Num(); ++VertexIdx)
    {
        const FVector VertexPos = FVector(AllVertices[VertexIdx]);
        const FVector ToVertex = VertexPos - BandCenter;

        // 축 방향 거리
        const float AxisDistance = FVector::DotProduct(ToVertex, BandAxis);
        const float LocalZ = AxisDistance;

        // 확장된 Z 범위 체크
        if (LocalZ < ExtendedZMin || LocalZ > ExtendedZMax)
        {
            continue;
        }

        // 반경 방향 거리
        const FVector RadialVec = ToVertex - BandAxis * AxisDistance;
        const float RadialDistance = RadialVec.Size();

        // 해당 높이에서의 밴드 반경 (가변 반경, Z 범위 클램프)
        const float ClampedZ = FMath::Clamp(LocalZ, ZMin, ZMax);
        const float BandRadius = GetRadiusAtHeight(ClampedZ, BandSettings);

        // 반경 범위 체크 (밴드 근처)
        if (RadialDistance > BandRadius + Ring.RingThickness)
        {
            continue;
        }

        OutRingData.PostProcessingIndices.Add(static_cast<uint32>(VertexIdx));

        // Influence 계산: 코어(Band Section 내) = 1.0, Z 확장 영역 = falloff
        float Influence = 1.0f;

        // 코어 영역: Band Section (-BandHeight/2 ~ +BandHeight/2)
        // 새 좌표계: Z=0이 Mid Band 중심
        const float CoreZMin = -BandHeight * 0.5f;
        const float CoreZMax = BandHeight * 0.5f;

        if (LocalZ < CoreZMin)
        {
            // 하단 확장 영역 (Lower Section + Z 확장)
            const float DistFromCore = CoreZMin - LocalZ;
            const float MaxExtension = LowerHeight + BoundsZBottom;
            Influence = 1.0f - FMath::Clamp(DistFromCore / FMath::Max(MaxExtension, 0.01f), 0.0f, 1.0f);
            Influence = FMath::InterpEaseInOut(0.0f, 1.0f, Influence, 2.0f);
            ExtendedCount++;
        }
        else if (LocalZ > CoreZMax)
        {
            // 상단 확장 영역 (Upper Section + Z 확장)
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

        OutRingData.PostProcessingInfluences.Add(Influence);
    }

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("PostProcessing (VirtualBand): Selected %d vertices (Core=%d, Extended=%d) for Ring[%d]"),
        OutRingData.PostProcessingIndices.Num(), CoreCount, ExtendedCount, Context.RingIndex);
}

// ============================================================================
// Affected Vertices Manager Implementation
// 영향받는 버텍스 관리자 구현
// ============================================================================

FFleshRingAffectedVerticesManager::FFleshRingAffectedVerticesManager()
{
    // Default to distance-based selector
    // 기본값: 거리 기반 선택기
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
// RegisterAffectedVertices - 영향받는 버텍스 등록
// ============================================================================
bool FFleshRingAffectedVerticesManager::RegisterAffectedVertices(
    const UFleshRingComponent* Component,
    const USkeletalMeshComponent* SkeletalMesh,
    int32 LODIndex)
{
    // Validate input parameters
    // 입력 파라미터 유효성 검사
    if (!Component || !SkeletalMesh || !VertexSelector)
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("RegisterAffectedVertices: Invalid parameters"));
        return false;
    }

    // [버그 수정] RingDataArray.Reset() 제거
    // Dirty Flag 시스템 도입 후, 여기서 Reset()하면 clean한 Ring의 캐시된 데이터가 삭제됨
    // 배열 크기 조정은 아래 SetNum() 로직에서 처리됨 (라인 1440-1443)

    // FleshRingAsset null check
    // FleshRingAsset null 체크
    if (!Component->FleshRingAsset)
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("RegisterAffectedVertices: FleshRingAsset is null"));
        return false;
    }

    const TArray<FFleshRingSettings>& Rings = Component->FleshRingAsset->Rings;

    // ================================================================
    // 메시 데이터 캐싱: 바인드 포즈는 불변이므로 한 번만 추출
    // Mesh data caching: bind pose is immutable, extract only once
    // ================================================================
    if (!bMeshDataCached)
    {
        // Extract mesh vertices from skeletal mesh at specified LOD
        // 스켈레탈 메시의 지정된 LOD에서 버텍스 추출 (바인드 포즈 컴포넌트 스페이스)
        if (!ExtractMeshVertices(SkeletalMesh, CachedMeshVertices, LODIndex))
        {
            UE_LOG(LogFleshRingVertices, Error,
                TEXT("RegisterAffectedVertices: Failed to extract mesh vertices"));
            return false;
        }

        // Build Spatial Hash for O(1) vertex queries
        // O(1) 버텍스 쿼리를 위한 Spatial Hash 빌드
        VertexSpatialHash.Build(CachedMeshVertices);

        // Extract mesh indices for adjacency data (Normal recomputation)
        // 인접 데이터용 메시 인덱스 추출 (노멀 재계산용)
        CachedMeshIndices.Reset();
        if (!ExtractMeshIndices(SkeletalMesh, CachedMeshIndices, LODIndex))
        {
            UE_LOG(LogFleshRingVertices, Warning,
                TEXT("RegisterAffectedVertices: Failed to extract mesh indices, Normal recomputation will be disabled"));
        }

        // 토폴로지 캐시 빌드 (메시 데이터 캐싱과 함께)
        // 라플라시안 스무딩 여부와 무관하게 항상 빌드하여 모든 함수에서 캐시 활용
        // 이전에는 BuildLaplacianAdjacencyData() 내부에서만 빌드되어
        // BuildRepresentativeIndices(), BuildAdjacencyData() 등이 캐시를 활용하지 못했음
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
    // Layer Types 재빌드 (매번 호출 - MaterialLayerMappings 변경 즉시 반영)
    // Rebuild layer types every time to reflect MaterialLayerMappings changes
    // ================================================================
    RebuildVertexLayerTypes(Component, SkeletalMesh, LODIndex);

    // 로컬 참조용 (이후 코드와 호환)
    const TArray<FVector3f>& MeshVertices = CachedMeshVertices;
    const TArray<EFleshRingLayerType>& VertexLayerTypes = CachedVertexLayerTypes;

    // ================================================================
    // Dirty Flag 시스템 초기화
    // Initialize dirty flag system
    // ================================================================
    const int32 NumRings = Rings.Num();

    // RingDataArray 크기 조정
    if (RingDataArray.Num() != NumRings)
    {
        RingDataArray.SetNum(NumRings);
    }

    // RingDirtyFlags 크기 조정 (기존 dirty 상태 유지, 새 요소만 dirty로 설정)
    if (RingDirtyFlags.Num() != NumRings)
    {
        const int32 OldNum = RingDirtyFlags.Num();
        RingDirtyFlags.SetNum(NumRings);
        // 새로 추가된 Ring만 dirty로 설정 (기존 상태는 유지)
        for (int32 i = OldNum; i < NumRings; ++i)
        {
            RingDirtyFlags[i] = true;
        }
        UE_LOG(LogFleshRingVertices, Log, TEXT("RegisterAffectedVertices: Resized dirty flags %d -> %d rings (new rings marked dirty)"), OldNum, NumRings);
    }

    // Process each Ring
    // 각 링 처리
    for (int32 RingIdx = 0; RingIdx < NumRings; ++RingIdx)
    {
        const FFleshRingSettings& RingSettings = Rings[RingIdx];

        // ===== Dirty Flag 체크: Clean한 링은 스킵 =====
        if (!RingDirtyFlags[RingIdx])
        {
            // 이미 유효한 데이터가 있고 변경되지 않음 - 스킵
            UE_LOG(LogFleshRingVertices, Log, TEXT("Ring[%d]: SKIPPED (not dirty)"), RingIdx);
            continue;
        }
        UE_LOG(LogFleshRingVertices, Log, TEXT("Ring[%d]: PROCESSING (dirty)"), RingIdx);

        // Skip Rings without valid bone
        // 유효한 본이 없는 링은 건너뜀
        if (RingSettings.BoneName == NAME_None)
        {
            UE_LOG(LogFleshRingVertices, Warning,
                TEXT("Ring[%d]: Skipping - no bone assigned"), RingIdx);
            RingDirtyFlags[RingIdx] = false;  // 처리 완료로 표시
            continue;
        }

        // Get bone index from skeletal mesh
        // 스켈레탈 메시에서 본 인덱스 가져오기
        const int32 BoneIndex = SkeletalMesh->GetBoneIndex(RingSettings.BoneName);
        if (BoneIndex == INDEX_NONE)
        {
            UE_LOG(LogFleshRingVertices, Warning,
                TEXT("Ring[%d]: Bone '%s' not found"), RingIdx, *RingSettings.BoneName.ToString());
            RingDirtyFlags[RingIdx] = false;  // 에러지만 처리 완료로 표시
            continue;
        }

        // Get skeletal mesh asset for reference skeleton
        // 레퍼런스 스켈레톤을 위한 스켈레탈 메시 에셋 가져오기
        USkeletalMesh* SkelMeshAsset = SkeletalMesh->GetSkeletalMeshAsset();
        if (!SkelMeshAsset)
        {
            UE_LOG(LogFleshRingVertices, Warning,
                TEXT("Ring[%d]: SkeletalMesh asset is null"), RingIdx);
            RingDirtyFlags[RingIdx] = false;  // 에러지만 처리 완료로 표시
            continue;
        }

        // Calculate bind pose component space transform
        // 바인드 포즈 컴포넌트 스페이스 트랜스폼 계산
        // (MeshVertices가 바인드 포즈 로컬 좌표이므로 동일한 좌표계 사용 필요)
        const FReferenceSkeleton& RefSkeleton = SkelMeshAsset->GetRefSkeleton();
        const TArray<FTransform>& RefBonePose = RefSkeleton.GetRefBonePose();

        // Accumulate transforms through parent chain
        // 부모 체인을 따라가며 트랜스폼 누적
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
        // 링 데이터 생성 (FFleshRingSettings → FRingAffectedData 복사)
        // ================================================================
        FRingAffectedData RingData;

        // Ring Information (from bone transform)
        // 링 정보 (본 트랜스폼에서 계산)
        RingData.BoneName = RingSettings.BoneName;

        const FQuat BoneRotation = BoneTransform.GetRotation();

        // InfluenceMode에 따라 RingCenter/RingAxis/Geometry 계산 분기
        if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::Manual)
        {
            // ===== Manual 모드: RingOffset/RingRotation 사용 =====
            const FVector WorldRingOffset = BoneRotation.RotateVector(RingSettings.RingOffset);
            RingData.RingCenter = BoneTransform.GetLocation() + WorldRingOffset;

            const FQuat WorldRingRotation = BoneRotation * RingSettings.RingRotation;
            RingData.RingAxis = WorldRingRotation.RotateVector(FVector::ZAxisVector);

            // Manual 모드는 스케일 없이 직접 값 사용
            RingData.RingRadius = RingSettings.RingRadius;
            RingData.RingThickness = RingSettings.RingThickness;
            RingData.RingHeight = RingSettings.RingHeight;
        }
        else if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
        {
            // ===== Virtual Band 모드: 전용 BandOffset/BandRotation 사용 =====
            const FProceduralBandSettings& BandSettings = RingSettings.ProceduralBand;
            const FVector WorldBandOffset = BoneRotation.RotateVector(BandSettings.BandOffset);
            RingData.RingCenter = BoneTransform.GetLocation() + WorldBandOffset;

            const FQuat WorldBandRotation = BoneRotation * BandSettings.BandRotation;
            RingData.RingAxis = WorldBandRotation.RotateVector(FVector::ZAxisVector);

            // ProceduralBand는 밴드 설정에서 반경 사용
            RingData.RingRadius = BandSettings.MidUpperRadius;
            RingData.RingThickness = BandSettings.BandThickness;
            RingData.RingHeight = BandSettings.BandHeight;
        }
        else
        {
            // ===== Auto 모드: MeshOffset/MeshRotation + MeshScale 적용 (SDF 기반) =====
            const FVector WorldMeshOffset = BoneRotation.RotateVector(RingSettings.MeshOffset);
            RingData.RingCenter = BoneTransform.GetLocation() + WorldMeshOffset;

            const FQuat WorldMeshRotation = BoneRotation * RingSettings.MeshRotation;
            RingData.RingAxis = WorldMeshRotation.RotateVector(FVector::ZAxisVector);

            // MeshScale 반영: 반경 방향 (X, Y 평균) 과 축 방향 (Z) 분리
            const float RadialScale = (RingSettings.MeshScale.X + RingSettings.MeshScale.Y) * 0.5f;
            const float AxialScale = RingSettings.MeshScale.Z;

            RingData.RingRadius = RingSettings.RingRadius * RadialScale;
            RingData.RingThickness = RingSettings.RingThickness * RadialScale;
            RingData.RingHeight = RingSettings.RingHeight * AxialScale;
        }

        // Deformation Parameters (copy from asset)
        // 변형 파라미터 (에셋에서 복사)
        RingData.TightnessStrength = RingSettings.TightnessStrength;
        RingData.FalloffType = RingSettings.FalloffType;

        // ================================================================
        // Context 생성 및 버텍스 선택
        // Build Context and select affected vertices
        // ================================================================
        const FRingSDFCache* SDFCache = Component->GetRingSDFCache(RingIdx);

        FVertexSelectionContext Context(
            RingSettings,
            RingIdx,
            BoneTransform,
            MeshVertices,
            SDFCache,  // nullptr이면 SDF 미사용 (Distance 기반 Selector는 무시)
            &VertexSpatialHash,  // O(1) 버텍스 쿼리용 Spatial Hash
            bTopologyCacheBuilt ? &CachedPositionToVertices : nullptr,  // UV seam 용접용 캐시 (빌드됨 시에만)
            &CachedVertexLayerTypes  // 레이어 기반 버텍스 필터링용
        );

        // Ring별 InfluenceMode에 따라 Selector 결정
        // Auto 또는 ProceduralBand 모드 + SDF 유효 → SDFBoundsBasedSelector
        // Manual 모드 또는 SDF 무효 → DistanceBasedSelector
        TSharedPtr<IVertexSelector> RingSelector;
        const bool bUseSDFForThisRing =
            (RingSettings.InfluenceMode == EFleshRingInfluenceMode::Auto ||
             RingSettings.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand) &&
            (SDFCache && SDFCache->IsValid());

        if (bUseSDFForThisRing)
        {
            RingSelector = MakeShared<FSDFBoundsBasedVertexSelector>();
        }
        else if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
        {
            // ProceduralBand 모드 + SDF 무효 → VirtualBandVertexSelector (거리 기반 가변 반경)
            RingSelector = MakeShared<FVirtualBandVertexSelector>();
        }
        else
        {
            RingSelector = MakeShared<FDistanceBasedVertexSelector>();
        }

        // InfluenceMode 이름 결정
        const TCHAR* InfluenceModeStr = TEXT("Manual");
        if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::Auto)
        {
            InfluenceModeStr = TEXT("Auto");
        }
        else if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
        {
            InfluenceModeStr = TEXT("ProceduralBand");
        }

        // Selector 이름 결정
        const TCHAR* SelectorStr = TEXT("DistanceBasedSelector");
        if (bUseSDFForThisRing)
        {
            SelectorStr = TEXT("SDFBoundsBasedSelector");
        }
        else if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
        {
            SelectorStr = TEXT("VirtualBandVertexSelector");
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("Ring[%d] '%s': Using %s (InfluenceMode=%s, SDFValid=%s)"),
            RingIdx, *RingSettings.BoneName.ToString(),
            SelectorStr,
            InfluenceModeStr,
            (SDFCache && SDFCache->IsValid()) ? TEXT("Yes") : TEXT("No"));

        // Ring별 Selector로 영향받는 버텍스 선택
        RingSelector->SelectVertices(Context, RingData.Vertices);

        // ================================================================
        // 후처리용 버텍스 선택 (Z 확장 범위)
        // Select post-processing vertices (Z-extended range)
        // ================================================================
        // 설계:
        // - Affected Vertices (PackedIndices) = 원본 AABB → Tightness 변형 대상
        // - Post-Processing Vertices = 원본 AABB + SmoothingBoundsZTop/Bottom → 스무딩/침투해결 등
        if (bUseSDFForThisRing)
        {
            // SDF 모드: SDF 바운드 기반 Z 확장
            FSDFBoundsBasedVertexSelector* SDFSelector = static_cast<FSDFBoundsBasedVertexSelector*>(RingSelector.Get());
            SDFSelector->SelectPostProcessingVertices(Context, RingData.Vertices, RingData);
            // Note: LayerTypes는 FullMeshLayerTypes에서 GPU가 직접 조회
        }
        else if (RingSettings.InfluenceMode == EFleshRingInfluenceMode::ProceduralBand)
        {
            // ProceduralBand 모드 (SDF 무효): VirtualBand 기반 Z 확장
            FVirtualBandVertexSelector* VBSelector = static_cast<FVirtualBandVertexSelector*>(RingSelector.Get());
            VBSelector->SelectPostProcessingVertices(Context, RingData.Vertices, RingData);
            // Note: LayerTypes는 FullMeshLayerTypes에서 GPU가 직접 조회
        }
        else
        {
            // Manual 모드: Ring 파라미터 기반 Z 확장
            FDistanceBasedVertexSelector* DistSelector = static_cast<FDistanceBasedVertexSelector*>(RingSelector.Get());
            DistSelector->SelectPostProcessingVertices(Context, RingData.Vertices, RingData);
            // Note: LayerTypes는 FullMeshLayerTypes에서 GPU가 직접 조회
        }

        // Pack for GPU (convert to flat arrays)
        // GPU용 패킹 (평면 배열로 변환)
        RingData.PackForGPU();

        // Build representative indices for UV seam welding
        // UV seam 용접을 위한 대표 버텍스 인덱스 빌드
        // 이 데이터는 모든 변형 패스에서 사용되어 UV 중복이 동일하게 움직이도록 보장
        BuildRepresentativeIndices(RingData, MeshVertices);

        // Build adjacency data for Normal recomputation
        // 노멀 재계산용 인접 데이터 빌드
        if (CachedMeshIndices.Num() > 0)
        {
            BuildAdjacencyData(RingData, CachedMeshIndices);

            // PostProcessing 버텍스용 노멀 인접 데이터도 빌드 (Z 확장 범위)
            if (RingData.PostProcessingIndices.Num() > 0)
            {
                BuildPostProcessingAdjacencyData(RingData, CachedMeshIndices);
            }

            // Build Laplacian adjacency data for smoothing (조건부: 스무딩 활성화 시에만)
            // 스무딩용 라플라시안 인접 데이터 빌드
            // 개선: 같은 레이어의 이웃만 포함하여 레이어 경계 혼합 방지
            if (RingSettings.bEnableLaplacianSmoothing)
            {
                BuildLaplacianAdjacencyData(RingData, CachedMeshIndices, MeshVertices, VertexLayerTypes);

                // PostProcessing 버텍스용 라플라시안 인접 데이터도 빌드 (Z 확장 범위)
                if (RingData.PostProcessingIndices.Num() > 0)
                {
                    BuildPostProcessingLaplacianAdjacencyData(RingData, CachedMeshIndices, MeshVertices, VertexLayerTypes);
                }
            }

            // Build PBD adjacency data (조건부: PBD 활성화 시에만)
            // PBD 에지 제약용 인접 데이터 빌드
            if (RingSettings.bEnablePBDEdgeConstraint)
            {
                BuildPBDAdjacencyData(RingData, CachedMeshIndices, MeshVertices, MeshVertices.Num());

                // PostProcessing 버텍스용 PBD 인접 데이터도 빌드 (Z 확장 범위)
                if (RingData.PostProcessingIndices.Num() > 0)
                {
                    BuildPostProcessingPBDAdjacencyData(RingData, CachedMeshIndices, MeshVertices, MeshVertices.Num());
                }
            }

            // Build slice data for bone ratio preservation (Radial Smoothing용)
            // 본 거리 비율 보존용 슬라이스 데이터 빌드
            // GPU 디스패치에서 bEnableRadialSmoothing 체크하므로 여기선 무조건 빌드
            BuildSliceData(RingData, MeshVertices, RingSettings.RadialSliceHeight);

            // Build hop distance data for topology-based smoothing
            // 홉 기반 스무딩용 확장 영역 데이터 빌드
            // ANY smoothing 또는 HeatPropagation이 켜져있으면 hop 데이터 빌드
            const bool bAnySmoothingEnabled =
                RingSettings.bEnableRadialSmoothing ||
                RingSettings.bEnableLaplacianSmoothing ||
                RingSettings.bEnablePBDEdgeConstraint ||
                RingSettings.bEnableHeatPropagation;  // Heat Propagation도 Extended 데이터 필요

            if (bAnySmoothingEnabled)
            {
                BuildHopDistanceData(
                    RingData,
                    CachedMeshIndices,
                    MeshVertices,
                    RingSettings.MaxSmoothingHops,
                    RingSettings.HopFalloffType
                );
            }
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("Ring[%d] '%s': %d affected, %d slices, %d extended smoothing"),
            RingIdx, *RingSettings.BoneName.ToString(),
            RingData.Vertices.Num(), RingData.SlicePackedData.Num() / 33, RingData.ExtendedSmoothingIndices.Num());

        // 인덱스 기반 할당 (Add 대신) + dirty flag 클리어
        RingDataArray[RingIdx] = MoveTemp(RingData);
        RingDirtyFlags[RingIdx] = false;
    }

    // 실제 처리된 ring 수 계산
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
    // Empty()로 메모리 완전 해제 (Reset()은 메모리 유지)
    RingDataArray.Empty();

    // 토폴로지 캐시 무효화
    InvalidateTopologyCache();

    // ★ 캐시된 메시 데이터도 해제 (메모리 누수 방지)
    CachedMeshIndices.Empty();
    CachedMeshVertices.Empty();
    CachedVertexLayerTypes.Empty();
    bMeshDataCached = false;

    // ★ Spatial Hash 해제
    VertexSpatialHash.Clear();

    // ★ Dirty Flags 해제
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
// Per-Ring Dirty Flag System - Ring별 재빌드 필요 여부 관리
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
    // 플래그가 없으면 dirty로 간주 (첫 빌드)
    return true;
}

// ============================================================================
// BuildTopologyCache - 토폴로지 캐시 빌드 (메시당 한 번만)
// ============================================================================
// 메시의 토폴로지 데이터는 바인드 포즈에서 결정되며 런타임에 변하지 않음.
// - 위치 기반 버텍스 그룹 (UV seam 용접용)
// - 버텍스 이웃 맵 (직접 메시 연결)
// - 용접된 이웃 위치 맵 (UV seam 인식)
// - 전체 메시 인접 맵 (BFS/홉 계산용)
// 이 함수를 한 번 호출하면 이후 모든 Ring 업데이트에서 O(1) 조회 가능.

void FFleshRingAffectedVerticesManager::BuildTopologyCache(
    const TArray<FVector3f>& AllVertices,
    const TArray<uint32>& MeshIndices)
{
    // 이미 캐시가 빌드되어 있으면 스킵
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
    // 1단계: UV seam 용접을 위한 위치 기반 버텍스 그룹 구축
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
    // 1.5단계: UV seam 용접용 대표 버텍스 맵 구축
    // ================================================================
    // 각 위치의 대표 버텍스 = 해당 위치 버텍스들 중 최소 인덱스
    // BuildRepresentativeIndices()에서 O(A) 맵 빌드 제거하여 O(1) 조회로 최적화
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
    // 2단계: 메시 삼각형에서 전역 버텍스 이웃 맵 구축
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
    // 3단계: 용접된 이웃 맵 구축 (UV 중복 버텍스들의 이웃 병합)
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
    // 4단계: BFS/홉 거리 계산용 전체 메시 인접 맵 구축
    // ================================================================
    CachedFullAdjacencyMap.Reset();
    CachedFullAdjacencyMap.Reserve(AllVertices.Num());

    for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
    {
        const uint32 I0 = MeshIndices[TriIdx * 3 + 0];
        const uint32 I1 = MeshIndices[TriIdx * 3 + 1];
        const uint32 I2 = MeshIndices[TriIdx * 3 + 2];

        // 각 에지에 대해 양방향 인접 추가
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
    // Step 5: Build per-vertex triangle list for adjacency lookup
    // 5단계: 인접 조회용 버텍스별 삼각형 리스트 구축
    // ================================================================
    // BuildAdjacencyData()에서 O(T) → O(avg_triangles_per_vertex) 최적화에 사용
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

    // 캐시 빌드 완료 표시
    bTopologyCacheBuilt = true;

    const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("BuildTopologyCache: Completed in %.2f ms - %d vertices -> %d welded positions (%d duplicates), %d adjacency entries"),
        ElapsedMs, AllVertices.Num(), WeldedPositionCount, DuplicateVertexCount, CachedFullAdjacencyMap.Num());
}

// ============================================================================
// InvalidateTopologyCache - 토폴로지 캐시 무효화
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

    // 에셋에서 매핑 가져오기
    if (Component->FleshRingAsset->MaterialLayerMappings.Num() > 0)
    {
        const UFleshRingAsset* Asset = Component->FleshRingAsset;

        // 섹션별로 레이어 타입 할당
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

                // 각 섹션의 머티리얼 슬롯에서 레이어 타입 가져오기
                for (int32 SectionIdx = 0; SectionIdx < LODData.RenderSections.Num(); ++SectionIdx)
                {
                    const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIdx];
                    const int32 MaterialSlotIndex = static_cast<int32>(Section.MaterialIndex);

                    // 에셋에서 이 머티리얼 슬롯의 레이어 타입 조회
                    EFleshRingLayerType LayerType = Asset->GetLayerTypeForMaterialSlot(MaterialSlotIndex);

                    // 섹션의 모든 버텍스에 할당
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

    // 에셋 매핑이 없으면 키워드 기반 자동 감지 폴백
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
// ExtractMeshVertices - 메시에서 버텍스 추출 (바인드 포즈 컴포넌트 스페이스)
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
    // 스켈레탈 메시 에셋 가져오기
    USkeletalMesh* Mesh = SkeletalMesh->GetSkeletalMeshAsset();
    if (!Mesh)
    {
        return false;
    }

    // Get render data
    // 렌더 데이터 가져오기
    const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
    if (!RenderData || RenderData->LODRenderData.Num() == 0)
    {
        return false;
    }

    // Validate LOD index
    // LOD 인덱스 유효성 검사
    if (LODIndex < 0 || LODIndex >= RenderData->LODRenderData.Num())
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("ExtractMeshVertices: Invalid LOD index %d (max: %d), falling back to LOD 0"),
            LODIndex, RenderData->LODRenderData.Num() - 1);
        LODIndex = 0;
    }

    // Use specified LOD for vertex data
    // 지정된 LOD에서 버텍스 데이터 사용
    const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];
    const uint32 NumVertices = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

    if (NumVertices == 0)
    {
        return false;
    }

    // Extract vertex positions (bind pose local space)
    // 버텍스 위치 추출 (바인드 포즈 컴포넌트 스페이스)
    OutVertices.Reset(NumVertices);

    for (uint32 VertexIdx = 0; VertexIdx < NumVertices; ++VertexIdx)
    {
        const FVector3f& Position = LODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(VertexIdx);
        OutVertices.Add(Position);
    }

    return true;
}

// ============================================================================
// ExtractMeshIndices - 메시에서 인덱스 버퍼 추출
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
// BuildAdjacencyData - 인접 삼각형 데이터 빌드 (캐시 사용 최적화)
// ============================================================================
// 최적화: CachedVertexTriangles 사용으로 O(T) → O(A × avg_triangles_per_vertex)
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

    // 캐시가 없으면 폴백 (발생하면 안 되지만 안전 장치)
    if (!bTopologyCacheBuilt || CachedVertexTriangles.Num() == 0)
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("BuildAdjacencyData: Topology cache not built, falling back to brute force"));

        // 폴백: 전체 삼각형 순회 (기존 방식)
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
    // 캐시 사용 최적화 경로: O(A × avg_triangles_per_vertex)
    // ================================================================

    // Step 1: 각 영향받는 버텍스의 삼각형 수 계산 (캐시에서 O(1) 조회)
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

    // Step 2: 오프셋 배열 빌드 (누적합)
    RingData.AdjacencyOffsets.SetNum(NumAffected + 1);
    RingData.AdjacencyOffsets[0] = 0;

    for (int32 i = 0; i < NumAffected; ++i)
    {
        RingData.AdjacencyOffsets[i + 1] = RingData.AdjacencyOffsets[i] + AdjCounts[i];
    }

    const uint32 TotalAdjacencies = RingData.AdjacencyOffsets[NumAffected];

    // Step 3: 삼각형 배열 채우기 (캐시에서 직접 복사)
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
// BuildLaplacianAdjacencyData - 라플라시안 스무딩용 이웃 데이터 빌드
// ============================================================================
// 개선: 같은 레이어의 이웃만 포함 (스타킹-살 경계에서 섞이지 않음)
// 개선: UV seam 용접 - 동일 위치의 분리된 버텍스들이 같은 이웃을 공유하여 크랙 방지
// 최적화: 토폴로지 캐시 사용 - 매 프레임 O(V*T) 재빌드 제거
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
    // 1단계: Affected 버텍스 조회용 Set 빌드
    // ================================================================
    // 참고: 토폴로지 캐시는 RegisterAffectedVertices() 초기 단계에서 이미 빌드됨
    // 이웃 선택 시 Affected 버텍스를 우선하기 위해 필요.
    // Affected가 아닌 버텍스는 스무딩되지 않으므로 위치가 변하지 않음.
    // -> 일관성을 위해 Affected 버텍스를 이웃으로 선택해야 함.
    TSet<uint32> AffectedVertexSet;
    AffectedVertexSet.Reserve(NumAffected);
    for (int32 i = 0; i < NumAffected; ++i)
    {
        AffectedVertexSet.Add(RingData.Vertices[i].VertexIndex);
    }

    // ================================================================
    // Step 2: Pack adjacency data for affected vertices using cached topology
    // 2단계: 캐시된 토폴로지를 사용하여 영향받는 버텍스의 인접 데이터 패킹
    //
    // 핵심: 같은 위치의 모든 버텍스가 동일한 이웃 위치 집합을 사용
    //       -> 동일한 Laplacian 계산 -> 동일한 이동 -> 크랙 없음!
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
                    // 핵심 수정: Representative 버텍스 우선 선택 (UV Seam Welding)
                    // ============================================================
                    // UV seam에서 동일 위치의 모든 duplicate vertex가 같은 이웃 인덱스를 참조해야
                    // Laplacian 계산 결과가 동일해지고 크랙이 방지됨.
                    //
                    // 선택 우선순위:
                    // 1. Representative 인덱스 (같은 위치의 대표 버텍스)
                    // 2. Affected 버텍스 중 하나 (스무딩 대상)
                    // 3. 해당 위치의 첫 번째 버텍스 (fallback)
                    uint32 NeighborIdx = UINT32_MAX;

                    // 1순위: Representative 인덱스 확인
                    const uint32* RepresentativeIdx = CachedPositionToRepresentative.Find(NeighborPosKey);
                    if (RepresentativeIdx && AffectedVertexSet.Contains(*RepresentativeIdx))
                    {
                        NeighborIdx = *RepresentativeIdx;
                    }
                    else
                    {
                        // 2순위: Affected 버텍스 중 가장 작은 인덱스 선택 (일관성)
                        // 중요: "첫 번째 발견"이 아닌 "최소 인덱스" 사용
                        // 그래야 같은 위치의 모든 UV duplicate가 동일한 이웃 인덱스를 참조
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

                    // 3순위: Fallback - 가장 작은 인덱스 (일관성)
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
// BuildPostProcessingLaplacianAdjacencyData - 후처리 버텍스용 라플라시안 인접 데이터 빌드
// ============================================================================
// PostProcessingIndices 기반으로 라플라시안 인접 데이터를 구축합니다.
// Z 확장 범위의 버텍스들이 스무딩될 수 있도록 인접 정보 제공.
// 최적화: 토폴로지 캐시 사용 - 매 프레임 O(V*T) 재빌드 제거

void FFleshRingAffectedVerticesManager::BuildPostProcessingLaplacianAdjacencyData(
    FRingAffectedData& RingData,
    const TArray<uint32>& MeshIndices,
    const TArray<FVector3f>& AllVertices,
    const TArray<EFleshRingLayerType>& VertexLayerTypes)
{
    constexpr int32 MAX_NEIGHBORS = 12;
    constexpr int32 PACKED_SIZE = 1 + MAX_NEIGHBORS;  // Count + 12 indices = 13

    const int32 NumPostProcessing = RingData.PostProcessingIndices.Num();
    if (NumPostProcessing == 0 || MeshIndices.Num() == 0)
    {
        RingData.PostProcessingLaplacianAdjacencyData.Reset();
        return;
    }

    // ================================================================
    // Step 1: Build PostProcessing vertex lookup set
    // 1단계: PostProcessing 버텍스 조회용 Set 빌드
    // ================================================================
    // 참고: 토폴로지 캐시는 RegisterAffectedVertices() 초기 단계에서 이미 빌드됨
    TSet<uint32> PostProcessingVertexSet;
    PostProcessingVertexSet.Reserve(NumPostProcessing);
    for (int32 i = 0; i < NumPostProcessing; ++i)
    {
        PostProcessingVertexSet.Add(RingData.PostProcessingIndices[i]);
    }

    // ================================================================
    // Step 2: Build adjacency for each post-processing vertex using cached topology
    // 2단계: 캐시된 토폴로지를 사용하여 후처리 버텍스의 인접 데이터 빌드
    // ================================================================
    RingData.PostProcessingLaplacianAdjacencyData.Reset(NumPostProcessing * PACKED_SIZE);
    RingData.PostProcessingLaplacianAdjacencyData.AddZeroed(NumPostProcessing * PACKED_SIZE);

    int32 CrossLayerSkipped = 0;

    for (int32 PPIdx = 0; PPIdx < NumPostProcessing; ++PPIdx)
    {
        const uint32 VertIdx = RingData.PostProcessingIndices[PPIdx];
        const int32 BaseOffset = PPIdx * PACKED_SIZE;

        // Get my layer type (전역 캐시에서 직접 조회 - Extended와 동일)
        // [최적화] PostProcessingLayerTypes 대신 전역 캐시 사용
        EFleshRingLayerType MyLayerType = EFleshRingLayerType::Other;
        if (VertexLayerTypes.IsValidIndex(static_cast<int32>(VertIdx)))
        {
            MyLayerType = VertexLayerTypes[static_cast<int32>(VertIdx)];
        }

        // Get my position key from cache
        const FIntVector* MyPosKey = CachedVertexToPosition.Find(VertIdx);
        if (!MyPosKey)
        {
            RingData.PostProcessingLaplacianAdjacencyData[BaseOffset] = 0;
            continue;
        }

        // Get welded neighbors from cache
        const TSet<FIntVector>* WeldedNeighborPositions = CachedWeldedNeighborPositions.Find(*MyPosKey);
        if (!WeldedNeighborPositions)
        {
            RingData.PostProcessingLaplacianAdjacencyData[BaseOffset] = 0;
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
            // [UV Seam Welding] 이웃 인덱스도 Representative 우선 사용
            // ================================================================
            // 같은 위치의 모든 버텍스가 동일한 이웃 인덱스를 참조하도록
            // Representative 인덱스를 우선 사용하여 Laplacian 계산 일관성 보장
            // ================================================================
            uint32 NeighborIdx = UINT32_MAX;

            // 1순위: Representative 인덱스 사용 (PostProcessing 내부면 더 좋음)
            const uint32* RepresentativeIdx = CachedPositionToRepresentative.Find(NeighborPosKey);
            if (RepresentativeIdx)
            {
                NeighborIdx = *RepresentativeIdx;
            }

            // 2순위: Representative가 없거나 PostProcessing에 없으면, PostProcessing 내 가장 작은 인덱스 선택
            // 중요: 일관성을 위해 "첫 번째 발견"이 아닌 "최소 인덱스" 사용
            // 그래야 같은 위치의 모든 UV duplicate가 동일한 이웃 인덱스를 참조
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

            // 3순위: 여전히 없으면 가장 작은 인덱스 사용 (일관성)
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
        RingData.PostProcessingLaplacianAdjacencyData[BaseOffset] = NeighborCount;
        for (int32 i = 0; i < MAX_NEIGHBORS; ++i)
        {
            RingData.PostProcessingLaplacianAdjacencyData[BaseOffset + 1 + i] = NeighborIndices[i];
        }
    }

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("BuildPostProcessingLaplacianAdjacencyData (Cached): %d vertices, %d packed uints, %d cross-layer skipped"),
        NumPostProcessing, RingData.PostProcessingLaplacianAdjacencyData.Num(), CrossLayerSkipped);
}

// ============================================================================
// BuildPostProcessingPBDAdjacencyData - 후처리 버텍스용 PBD 인접 데이터 빌드
// ============================================================================
// PostProcessingIndices 기반으로 PBD 인접 데이터를 구축합니다.
// BuildPBDAdjacencyData와 동일하지만 확장된 범위 사용.

void FFleshRingAffectedVerticesManager::BuildPostProcessingPBDAdjacencyData(
    FRingAffectedData& RingData,
    const TArray<uint32>& MeshIndices,
    const TArray<FVector3f>& AllVertices,
    int32 TotalVertexCount)
{
    const int32 NumPostProcessing = RingData.PostProcessingIndices.Num();
    if (NumPostProcessing == 0 || MeshIndices.Num() < 3)
    {
        RingData.PostProcessingPBDAdjacencyWithRestLengths.Reset();
        return;
    }

    // Step 1: Build VertexIndex → ThreadIndex lookup
    TMap<uint32, int32> VertexToThreadIndex;
    for (int32 ThreadIdx = 0; ThreadIdx < NumPostProcessing; ++ThreadIdx)
    {
        VertexToThreadIndex.Add(RingData.PostProcessingIndices[ThreadIdx], ThreadIdx);
    }

    // Step 2: Build per-vertex neighbor set with rest lengths
    // 캐시 사용 최적화: CachedVertexNeighbors에서 이웃 조회, rest length는 on-demand 계산
    TArray<TMap<uint32, float>> VertexNeighborsWithRestLen;
    VertexNeighborsWithRestLen.SetNum(NumPostProcessing);

    if (bTopologyCacheBuilt && CachedVertexNeighbors.Num() > 0)
    {
        // 캐시 사용 경로: O(PP × avg_neighbors_per_vertex)
        for (int32 ThreadIdx = 0; ThreadIdx < NumPostProcessing; ++ThreadIdx)
        {
            const uint32 VertexIndex = RingData.PostProcessingIndices[ThreadIdx];
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
        // 폴백: 전체 삼각형 순회 (기존 방식)
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("BuildPostProcessingPBDAdjacencyData: Topology cache not built, falling back to brute force"));

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
    RingData.PostProcessingPBDAdjacencyWithRestLengths.Reset(NumPostProcessing * PackedSizePerVertex);
    RingData.PostProcessingPBDAdjacencyWithRestLengths.AddZeroed(NumPostProcessing * PackedSizePerVertex);

    for (int32 ThreadIdx = 0; ThreadIdx < NumPostProcessing; ++ThreadIdx)
    {
        const TMap<uint32, float>& NeighborsMap = VertexNeighborsWithRestLen[ThreadIdx];
        const int32 NeighborCount = FMath::Min(NeighborsMap.Num(), FRingAffectedData::PBD_MAX_NEIGHBORS);
        const int32 BaseOffset = ThreadIdx * PackedSizePerVertex;

        RingData.PostProcessingPBDAdjacencyWithRestLengths[BaseOffset] = static_cast<uint32>(NeighborCount);

        int32 SlotIdx = 0;
        for (const auto& Pair : NeighborsMap)
        {
            if (SlotIdx >= FRingAffectedData::PBD_MAX_NEIGHBORS)
            {
                break;
            }

            const uint32 NeighborIdx = Pair.Key;
            const float RestLength = Pair.Value;

            RingData.PostProcessingPBDAdjacencyWithRestLengths[BaseOffset + 1 + SlotIdx * 2] = NeighborIdx;

            uint32 RestLengthAsUint;
            FMemory::Memcpy(&RestLengthAsUint, &RestLength, sizeof(float));
            RingData.PostProcessingPBDAdjacencyWithRestLengths[BaseOffset + 1 + SlotIdx * 2 + 1] = RestLengthAsUint;

            ++SlotIdx;
        }
    }

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("BuildPostProcessingPBDAdjacencyData: %d vertices, %d packed uints"),
        NumPostProcessing, RingData.PostProcessingPBDAdjacencyWithRestLengths.Num());
}

// ============================================================================
// BuildPostProcessingAdjacencyData - 후처리 버텍스용 노멀 인접 데이터 빌드
// ============================================================================
// PostProcessingIndices 기반으로 노멀 재계산용 인접 데이터를 구축합니다.
// BuildAdjacencyData와 동일하지만 확장된 범위 사용.

void FFleshRingAffectedVerticesManager::BuildPostProcessingAdjacencyData(
    FRingAffectedData& RingData,
    const TArray<uint32>& MeshIndices)
{
    const int32 NumPostProcessing = RingData.PostProcessingIndices.Num();
    if (NumPostProcessing == 0 || MeshIndices.Num() == 0)
    {
        RingData.PostProcessingAdjacencyOffsets.Reset();
        RingData.PostProcessingAdjacencyTriangles.Reset();
        return;
    }

    // 캐시가 없으면 폴백 (발생하면 안 되지만 안전 장치)
    if (!bTopologyCacheBuilt || CachedVertexTriangles.Num() == 0)
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("BuildPostProcessingAdjacencyData: Topology cache not built, falling back to brute force"));

        // 폴백: 전체 삼각형 순회 (기존 방식)
        TMap<uint32, int32> VertexToIndex;
        VertexToIndex.Reserve(NumPostProcessing);
        for (int32 PPIdx = 0; PPIdx < NumPostProcessing; ++PPIdx)
        {
            VertexToIndex.Add(RingData.PostProcessingIndices[PPIdx], PPIdx);
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

        RingData.PostProcessingAdjacencyOffsets.SetNum(NumPostProcessing + 1);
        RingData.PostProcessingAdjacencyOffsets[0] = 0;
        for (int32 i = 0; i < NumPostProcessing; ++i)
        {
            RingData.PostProcessingAdjacencyOffsets[i + 1] = RingData.PostProcessingAdjacencyOffsets[i] + AdjCounts[i];
        }

        const uint32 TotalAdjacencies = RingData.PostProcessingAdjacencyOffsets[NumPostProcessing];
        RingData.PostProcessingAdjacencyTriangles.SetNum(TotalAdjacencies);

        TArray<uint32> WritePos;
        WritePos.SetNum(NumPostProcessing);
        for (int32 i = 0; i < NumPostProcessing; ++i)
        {
            WritePos[i] = RingData.PostProcessingAdjacencyOffsets[i];
        }

        for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
        {
            const uint32 I0 = MeshIndices[TriIdx * 3 + 0];
            const uint32 I1 = MeshIndices[TriIdx * 3 + 1];
            const uint32 I2 = MeshIndices[TriIdx * 3 + 2];

            if (const int32* Idx = VertexToIndex.Find(I0)) { RingData.PostProcessingAdjacencyTriangles[WritePos[*Idx]++] = TriIdx; }
            if (const int32* Idx = VertexToIndex.Find(I1)) { RingData.PostProcessingAdjacencyTriangles[WritePos[*Idx]++] = TriIdx; }
            if (const int32* Idx = VertexToIndex.Find(I2)) { RingData.PostProcessingAdjacencyTriangles[WritePos[*Idx]++] = TriIdx; }
        }
        return;
    }

    // ================================================================
    // 캐시 사용 최적화 경로: O(PP × avg_triangles_per_vertex)
    // ================================================================

    // Step 1: 각 후처리 버텍스의 삼각형 수 계산 (캐시에서 O(1) 조회)
    TArray<int32> AdjCounts;
    AdjCounts.SetNumZeroed(NumPostProcessing);

    for (int32 PPIdx = 0; PPIdx < NumPostProcessing; ++PPIdx)
    {
        const uint32 VertexIndex = RingData.PostProcessingIndices[PPIdx];
        const TArray<uint32>* TrianglesPtr = CachedVertexTriangles.Find(VertexIndex);
        if (TrianglesPtr)
        {
            AdjCounts[PPIdx] = TrianglesPtr->Num();
        }
    }

    // Step 2: 오프셋 배열 빌드 (누적합)
    RingData.PostProcessingAdjacencyOffsets.SetNum(NumPostProcessing + 1);
    RingData.PostProcessingAdjacencyOffsets[0] = 0;

    for (int32 i = 0; i < NumPostProcessing; ++i)
    {
        RingData.PostProcessingAdjacencyOffsets[i + 1] = RingData.PostProcessingAdjacencyOffsets[i] + AdjCounts[i];
    }

    const uint32 TotalAdjacencies = RingData.PostProcessingAdjacencyOffsets[NumPostProcessing];

    // Step 3: 삼각형 배열 채우기 (캐시에서 직접 복사)
    RingData.PostProcessingAdjacencyTriangles.SetNum(TotalAdjacencies);

    for (int32 PPIdx = 0; PPIdx < NumPostProcessing; ++PPIdx)
    {
        const uint32 VertexIndex = RingData.PostProcessingIndices[PPIdx];
        const TArray<uint32>* TrianglesPtr = CachedVertexTriangles.Find(VertexIndex);

        if (TrianglesPtr && TrianglesPtr->Num() > 0)
        {
            const uint32 Offset = RingData.PostProcessingAdjacencyOffsets[PPIdx];
            FMemory::Memcpy(
                &RingData.PostProcessingAdjacencyTriangles[Offset],
                TrianglesPtr->GetData(),
                TrianglesPtr->Num() * sizeof(uint32)
            );
        }
    }

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("BuildPostProcessingAdjacencyData (Cached): %d vertices, %d offsets, %d triangles"),
        NumPostProcessing, RingData.PostProcessingAdjacencyOffsets.Num(), TotalAdjacencies);
}

// ============================================================================
// BuildSliceData - 슬라이스 기반 본 거리 비율 보존 데이터 빌드
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
    // 1단계: 각 버텍스의 축 거리(높이)와 본 거리 계산
    // ================================================================

    // Ring 축과 중심 (바인드 포즈 기준)
    const FVector3f Axis = FVector3f(RingData.RingAxis.GetSafeNormal());
    const FVector3f Center = FVector3f(RingData.RingCenter);

    // 높이 데이터 저장 (GPU 전송용)
    RingData.AxisHeights.Reset();
    RingData.AxisHeights.SetNum(NumAffected);

    RingData.OriginalBoneDistances.Reset();
    RingData.OriginalBoneDistances.SetNum(NumAffected);

    for (int32 AffIdx = 0; AffIdx < NumAffected; ++AffIdx)
    {
        const uint32 VertexIndex = RingData.Vertices[AffIdx].VertexIndex;
        const FVector3f& VertexPos = AllVertices[VertexIndex];

        // 중심에서 버텍스까지의 벡터
        const FVector3f ToVertex = VertexPos - Center;

        // 축 방향 높이 (dot product)
        const float Height = FVector3f::DotProduct(ToVertex, Axis);
        RingData.AxisHeights[AffIdx] = Height;

        // 축에 수직인 거리 (본 거리 = 반경)
        const FVector3f AxisComponent = Axis * Height;
        const FVector3f RadialComponent = ToVertex - AxisComponent;
        const float BoneDistance = RadialComponent.Size();

        RingData.OriginalBoneDistances[AffIdx] = BoneDistance;
    }

    // ================================================================
    // Step 2: Group vertices by height bucket (slice)
    // 2단계: 높이 버킷으로 버텍스 그룹핑 (슬라이스)
    // ================================================================

    // 버킷 인덱스 → 해당 버킷의 영향받는 버텍스 인덱스 리스트
    TMap<int32, TArray<int32>> BucketToVertices;

    for (int32 AffIdx = 0; AffIdx < NumAffected; ++AffIdx)
    {
        const int32 BucketIdx = FMath::FloorToInt(RingData.AxisHeights[AffIdx] / BucketSize);
        BucketToVertices.FindOrAdd(BucketIdx).Add(AffIdx);
    }

    // ================================================================
    // Step 3: Pack slice data for GPU (with adjacent buckets)
    // 3단계: GPU용 슬라이스 데이터 패킹 (인접 버킷 포함)
    // ================================================================
    // Format: [SliceVertexCount, V0, V1, ..., V31] per affected vertex
    // 개선: 현재 버킷 + 인접 버킷(±1)을 포함하여 부드러운 전환

    RingData.SlicePackedData.Reset();
    RingData.SlicePackedData.Reserve(NumAffected * FRingAffectedData::SLICE_PACKED_SIZE);

    for (int32 AffIdx = 0; AffIdx < NumAffected; ++AffIdx)
    {
        // 이 버텍스가 속한 버킷 찾기
        const int32 BucketIdx = FMath::FloorToInt(RingData.AxisHeights[AffIdx] / BucketSize);

        // 인접 버킷(±1) 포함하여 버텍스 수집
        // 순서: 현재 버킷(0) 우선 → 오버플로우 시에도 자신의 슬라이스 버텍스 보장
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

        // 나머지 슬롯은 0으로 채움
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
// BuildPBDAdjacencyData - PBD 에지 제약용 인접 데이터 빌드 (캐시 사용 최적화)
// ============================================================================
// 최적화: CachedVertexNeighbors 사용으로 O(T) → O(A × avg_neighbors_per_vertex)

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
    // 버텍스 인덱스 → 스레드 인덱스 룩업 빌드
    TMap<uint32, int32> VertexToThreadIndex;
    for (int32 ThreadIdx = 0; ThreadIdx < NumAffected; ++ThreadIdx)
    {
        VertexToThreadIndex.Add(RingData.Vertices[ThreadIdx].VertexIndex, ThreadIdx);
    }

    // Step 2: Build per-vertex neighbor set with rest lengths
    // 캐시 사용 최적화: CachedVertexNeighbors에서 이웃 조회, rest length는 on-demand 계산
    TArray<TMap<uint32, float>> VertexNeighborsWithRestLen;
    VertexNeighborsWithRestLen.SetNum(NumAffected);

    if (bTopologyCacheBuilt && CachedVertexNeighbors.Num() > 0)
    {
        // 캐시 사용 경로: O(A × avg_neighbors_per_vertex)
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
        // 폴백: 전체 삼각형 순회 (기존 방식)
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
    // 인접 데이터 패킹 (rest length 포함)
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

    // Step 4: Build full influence map (전체 버텍스에 대한 influence)
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
    // Note: DeformAmount는 FleshRingDeformerInstance에서 계산되므로
    // 여기서는 AxisHeight 기반으로 대략적인 값 설정
    // 실제 값은 DispatchData.DeformAmounts에서 사용
    RingData.FullDeformAmountMap.Reset(TotalVertexCount);
    RingData.FullDeformAmountMap.AddZeroed(TotalVertexCount);

    // Ring 높이의 절반을 threshold로 사용
    const float RingHalfWidth = RingData.RingHeight * 0.5f;

    for (int32 ThreadIdx = 0; ThreadIdx < NumAffected; ++ThreadIdx)
    {
        const FAffectedVertex& Vert = RingData.Vertices[ThreadIdx];
        if (Vert.VertexIndex < static_cast<uint32>(TotalVertexCount))
        {
            // AxisHeight 기반 DeformAmount 계산
            const float AxisHeight = RingData.AxisHeights.IsValidIndex(ThreadIdx)
                ? RingData.AxisHeights[ThreadIdx] : 0.0f;
            const float EdgeRatio = FMath::Clamp(
                FMath::Abs(AxisHeight) / FMath::Max(RingHalfWidth, 0.01f), 0.0f, 2.0f);

            // EdgeRatio > 1: Bulge 영역 (양수), EdgeRatio < 1: Tightness 영역 (음수)
            RingData.FullDeformAmountMap[Vert.VertexIndex] = (EdgeRatio - 1.0f) * Vert.Influence;
        }
    }

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("BuildPBDAdjacencyData: %d affected vertices, %d packed uints, %d total vertices in map"),
        NumAffected, RingData.PBDAdjacencyWithRestLengths.Num(), TotalVertexCount);
}

// ============================================================================
// BuildFullMeshAdjacency - 전체 메시 인접 맵 구축
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

        // 각 에지에 대해 양방향 인접 추가
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
// BuildExtendedLaplacianAdjacency - 확장된 스무딩 영역용 인접 데이터 구축
// ============================================================================
// [수정] BuildPostProcessingLaplacianAdjacencyData와 동일한 welding 로직 사용
// 기존: CachedFullAdjacencyMap 사용 (UV welding 안됨)
// 수정: CachedWeldedNeighborPositions 사용 (UV welding 적용)
//
// 핵심: 같은 위치의 모든 버텍스가 동일한 이웃 위치 집합을 사용
//       -> 동일한 Laplacian 계산 -> 동일한 이동 -> UV seam crack 방지

void FFleshRingAffectedVerticesManager::BuildExtendedLaplacianAdjacency(
    FRingAffectedData& RingData,
    const TArray<EFleshRingLayerType>& VertexLayerTypes)
{
    constexpr int32 MAX_NEIGHBORS = 12;
    constexpr int32 PACKED_SIZE = 1 + MAX_NEIGHBORS;  // Count + 12 indices = 13

    const int32 NumExtended = RingData.ExtendedSmoothingIndices.Num();
    if (NumExtended == 0)
    {
        RingData.ExtendedLaplacianAdjacency.Reset();
        return;
    }

    // ================================================================
    // Step 1: Build Extended vertex lookup set
    // 1단계: Extended 버텍스 조회용 Set 빌드
    // ================================================================
    TSet<uint32> ExtendedVertexSet;
    ExtendedVertexSet.Reserve(NumExtended);
    for (int32 i = 0; i < NumExtended; ++i)
    {
        ExtendedVertexSet.Add(RingData.ExtendedSmoothingIndices[i]);
    }

    // ================================================================
    // Step 2: Build adjacency for each extended vertex using cached topology
    // 2단계: 캐시된 토폴로지를 사용하여 확장 버텍스의 인접 데이터 빌드
    //
    // 핵심: CachedWeldedNeighborPositions 사용 (UV duplicate 이웃 병합)
    // ================================================================
    RingData.ExtendedLaplacianAdjacency.Reset(NumExtended * PACKED_SIZE);
    RingData.ExtendedLaplacianAdjacency.AddZeroed(NumExtended * PACKED_SIZE);

    int32 CrossLayerSkipped = 0;

    for (int32 ExtIdx = 0; ExtIdx < NumExtended; ++ExtIdx)
    {
        const uint32 VertIdx = RingData.ExtendedSmoothingIndices[ExtIdx];
        const int32 BaseOffset = ExtIdx * PACKED_SIZE;

        // Get my layer type (Extended는 별도 LayerTypes 배열이 없으므로 전역 사용)
        EFleshRingLayerType MyLayerType = EFleshRingLayerType::Other;
        if (VertexLayerTypes.IsValidIndex(static_cast<int32>(VertIdx)))
        {
            MyLayerType = VertexLayerTypes[static_cast<int32>(VertIdx)];
        }

        // Get my position key from cache
        const FIntVector* MyPosKey = CachedVertexToPosition.Find(VertIdx);
        if (!MyPosKey)
        {
            RingData.ExtendedLaplacianAdjacency[BaseOffset] = 0;
            continue;
        }

        // Get welded neighbors from cache (UV duplicate들의 이웃 병합됨!)
        const TSet<FIntVector>* WeldedNeighborPositions = CachedWeldedNeighborPositions.Find(*MyPosKey);
        if (!WeldedNeighborPositions)
        {
            RingData.ExtendedLaplacianAdjacency[BaseOffset] = 0;
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
            // 항상 전역 Representative 인덱스 사용
            // ================================================================
            // 문제: 기존 로직은 Representative가 Extended 밖에 있으면
            //       Extended 내 다른 인덱스를 선택했음
            //       → InitPass에서 delta는 Representative에 저장
            //       → DiffusePass에서 Adjacency의 다른 인덱스로 읽음
            //       → 불일치로 delta=0 읽음!
            //
            // 해결: 이웃 위치에 Extended 버텍스가 있으면
            //       항상 전역 Representative 인덱스 저장
            //       delta 버퍼는 전체 메시 크기이므로 Extended 밖도 접근 가능
            // ================================================================
            uint32 NeighborIdx = UINT32_MAX;  // Invalid sentinel

            // 먼저 이 위치에 Extended 버텍스가 있는지 확인
            bool bHasExtendedNeighbor = false;
            for (uint32 CandidateIdx : *VerticesAtNeighborPos)
            {
                if (ExtendedVertexSet.Contains(CandidateIdx))
                {
                    bHasExtendedNeighbor = true;
                    break;
                }
            }

            // Extended 이웃이 있으면, 전역 Representative 인덱스 사용
            if (bHasExtendedNeighbor)
            {
                const uint32* RepresentativeIdx = CachedPositionToRepresentative.Find(NeighborPosKey);
                if (RepresentativeIdx)
                {
                    NeighborIdx = *RepresentativeIdx;
                }
                else
                {
                    // Representative 캐시 미스 - 해당 위치의 첫 번째 버텍스 사용
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
        RingData.ExtendedLaplacianAdjacency[BaseOffset] = NeighborCount;
        for (int32 i = 0; i < MAX_NEIGHBORS; ++i)
        {
            RingData.ExtendedLaplacianAdjacency[BaseOffset + 1 + i] = NeighborIndices[i];
        }
    }

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("BuildExtendedLaplacianAdjacency (Welded): %d vertices, %d packed uints, %d cross-layer skipped"),
        NumExtended, RingData.ExtendedLaplacianAdjacency.Num(), CrossLayerSkipped);
}

// ============================================================================
// BuildRepresentativeIndices - UV seam 용접을 위한 대표 버텍스 인덱스 빌드
// ============================================================================
// [설계]
// UV seam에서 분리된 버텍스들(같은 위치, 다른 인덱스)이 동일한 변형을 받도록 보장.
// 모든 변형 패스(TightnessCS, BulgeCS, LaplacianCS 등)에서:
//   1. 대표 버텍스 위치 읽기
//   2. 변형 계산
//   3. 자기 인덱스에 기록
// 이로써 UV 중복이 항상 동일하게 움직여 크랙 방지.
//
// [최적화 - 2024.01]
// 토폴로지 캐시 활용: CachedPositionToRepresentative, CachedVertexToPosition
// 기존: 매 프레임 O(A) TMap 빌드 + O(A) PosKey 재계산 = ~30-50ms
// 최적화: O(A) 캐시 조회 = ~1-2ms

void FFleshRingAffectedVerticesManager::BuildRepresentativeIndices(
    FRingAffectedData& RingData,
    const TArray<FVector3f>& AllVertices)
{
    // ================================================================
    // 캐시가 있으면 O(1) 조회로 최적화된 경로 사용
    // ================================================================
    if (bTopologyCacheBuilt && CachedPositionToRepresentative.Num() > 0)
    {
        // ===== Affected Vertices용 RepresentativeIndices (캐시 사용) =====
        const int32 NumAffected = RingData.Vertices.Num();
        RingData.RepresentativeIndices.Reset(NumAffected);
        RingData.RepresentativeIndices.AddUninitialized(NumAffected);

        int32 NumWelded = 0;
        for (int32 i = 0; i < NumAffected; ++i)
        {
            const uint32 VertIdx = RingData.Vertices[i].VertexIndex;

            // O(1) 조회 - CachedVertexToPosition에서 PosKey 가져오기
            const FIntVector* PosKey = CachedVertexToPosition.Find(VertIdx);
            if (PosKey)
            {
                // O(1) 조회 - CachedPositionToRepresentative에서 대표 가져오기
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
                // 캐시 미스 - 자기 자신을 대표로
                RingData.RepresentativeIndices[i] = VertIdx;
            }
        }

        // ===== PostProcessing Vertices용 RepresentativeIndices (캐시 사용) =====
        const int32 NumPostProcessing = RingData.PostProcessingIndices.Num();
        if (NumPostProcessing > 0)
        {
            RingData.PostProcessingRepresentativeIndices.Reset(NumPostProcessing);
            RingData.PostProcessingRepresentativeIndices.AddUninitialized(NumPostProcessing);

            int32 PPNumWelded = 0;
            for (int32 i = 0; i < NumPostProcessing; ++i)
            {
                const uint32 VertIdx = RingData.PostProcessingIndices[i];

                // O(1) 조회 - 동일 패턴
                const FIntVector* PosKey = CachedVertexToPosition.Find(VertIdx);
                if (PosKey)
                {
                    const uint32* Representative = CachedPositionToRepresentative.Find(*PosKey);
                    const uint32 RepIdx = Representative ? *Representative : VertIdx;
                    RingData.PostProcessingRepresentativeIndices[i] = RepIdx;

                    if (RepIdx != VertIdx)
                    {
                        PPNumWelded++;
                    }
                }
                else
                {
                    RingData.PostProcessingRepresentativeIndices[i] = VertIdx;
                }
            }

            UE_LOG(LogFleshRingVertices, Verbose,
                TEXT("BuildRepresentativeIndices (cached): Affected=%d (welded=%d), PostProcessing=%d (welded=%d)"),
                NumAffected, NumWelded, NumPostProcessing, PPNumWelded);
        }
        else
        {
            UE_LOG(LogFleshRingVertices, Verbose,
                TEXT("BuildRepresentativeIndices (cached): Affected=%d (welded=%d), PostProcessing=0"),
                NumAffected, NumWelded);
        }

        return;  // 최적화된 경로 완료
    }

    // ================================================================
    // 폴백: 캐시가 없으면 기존 로직 사용 (첫 호출 시)
    // ================================================================
    constexpr float WeldPrecision = 0.001f;  // SelectVertices와 동일한 정밀도

    // Step 1: 위치 기반 그룹화 및 대표 선택
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

    // Step 2: Affected Vertices용 RepresentativeIndices 빌드
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

    // Step 3: PostProcessing Vertices용 RepresentativeIndices 빌드
    const int32 NumPostProcessing = RingData.PostProcessingIndices.Num();
    if (NumPostProcessing > 0)
    {
        TMap<FIntVector, uint32> PPPositionToRepresentative;

        for (uint32 VertIdx : RingData.PostProcessingIndices)
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

        RingData.PostProcessingRepresentativeIndices.Reset(NumPostProcessing);
        RingData.PostProcessingRepresentativeIndices.AddUninitialized(NumPostProcessing);

        int32 PPNumWelded = 0;
        for (int32 i = 0; i < NumPostProcessing; ++i)
        {
            const uint32 VertIdx = RingData.PostProcessingIndices[i];
            const FVector3f& Pos = AllVertices[VertIdx];

            FIntVector PosKey(
                FMath::RoundToInt(Pos.X / WeldPrecision),
                FMath::RoundToInt(Pos.Y / WeldPrecision),
                FMath::RoundToInt(Pos.Z / WeldPrecision)
            );

            const uint32 Representative = PPPositionToRepresentative[PosKey];
            RingData.PostProcessingRepresentativeIndices[i] = Representative;

            if (Representative != VertIdx)
            {
                PPNumWelded++;
            }
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("BuildRepresentativeIndices (fallback): Affected=%d (welded=%d), PostProcessing=%d (welded=%d)"),
            NumAffected, NumWelded, NumPostProcessing, PPNumWelded);
    }
    else
    {
        UE_LOG(LogFleshRingVertices, Log,
            TEXT("BuildRepresentativeIndices (fallback): Affected=%d (welded=%d), PostProcessing=0"),
            NumAffected, NumWelded);
    }
}

// ============================================================================
// BuildHopDistanceData - 홉 기반 스무딩용 확장 영역 구축 (전체 메시 BFS)
// ============================================================================
// 최적화: 토폴로지 캐시 사용 - 매 프레임 O(T) 인접 맵 재빌드 제거

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

    // ===== Step 1: Seeds = 모든 Affected Vertices =====
    // 참고: 토폴로지 캐시는 RegisterAffectedVertices() 초기 단계에서 이미 빌드됨
    // Seeds는 전체 메시 버텍스 인덱스
    TSet<uint32> SeedSet;
    SeedSet.Reserve(NumAffected);
    for (const FAffectedVertex& AffVert : RingData.Vertices)
    {
        SeedSet.Add(AffVert.VertexIndex);
    }

    // ===== Step 2: 전체 메시에서 BFS (N-hop 도달 버텍스 수집) =====
    // 캐시된 인접 맵 사용 - 매 프레임 재빌드 제거
    // HopDistanceMap: 전체 버텍스 인덱스 → 홉 거리
    TMap<uint32, int32> HopDistanceMap;
    HopDistanceMap.Reserve(NumAffected * (MaxHops + 1));

    // Seeds 초기화 (Hop 0)
    TQueue<uint32> BfsQueue;
    for (uint32 SeedVertIdx : SeedSet)
    {
        HopDistanceMap.Add(SeedVertIdx, 0);
        BfsQueue.Enqueue(SeedVertIdx);
    }

    // BFS 전파 (캐시된 인접 맵 사용)
    while (!BfsQueue.IsEmpty())
    {
        uint32 CurrentVertIdx;
        BfsQueue.Dequeue(CurrentVertIdx);

        const int32 CurrentHop = HopDistanceMap[CurrentVertIdx];

        // MaxHops에 도달하면 더 이상 전파 안 함
        if (CurrentHop >= MaxHops)
        {
            continue;
        }

        // 이웃들 확인 (캐시에서 조회)
        const TArray<uint32>* NeighborsPtr = CachedFullAdjacencyMap.Find(CurrentVertIdx);
        if (!NeighborsPtr)
        {
            continue;
        }

        for (uint32 NeighborVertIdx : *NeighborsPtr)
        {
            // 아직 방문 안 한 이웃에게 전파
            if (!HopDistanceMap.Contains(NeighborVertIdx))
            {
                HopDistanceMap.Add(NeighborVertIdx, CurrentHop + 1);
                BfsQueue.Enqueue(NeighborVertIdx);
            }
        }
    }

    // ===== Step 2.5: UV Duplicate 확장 (UV Seam Welding) =====
    // HopDistanceMap에 포함된 모든 버텍스의 UV duplicate들도 포함시킴
    // 이렇게 해야 UV seam의 모든 duplicate가 함께 스무딩되어 크랙 방지
    {
        // 현재 HopDistanceMap의 복사본을 만들어 순회 (순회 중 수정 방지)
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

            // 이 버텍스의 position key 찾기
            const FIntVector* PosKey = CachedVertexToPosition.Find(VertIdx);
            if (!PosKey)
            {
                continue;
            }

            // 같은 position의 모든 버텍스 찾기
            const TArray<uint32>* VerticesAtPos = CachedPositionToVertices.Find(*PosKey);
            if (!VerticesAtPos)
            {
                continue;
            }

            // UV duplicate들을 같은 hop distance로 추가
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

    // ===== Step 3: ExtendedSmoothing* 배열 구축 =====
    const int32 NumExtended = HopDistanceMap.Num();
    RingData.ExtendedSmoothingIndices.Reset(NumExtended);
    RingData.ExtendedHopDistances.Reset(NumExtended);
    RingData.ExtendedInfluences.Reset(NumExtended);
    RingData.ExtendedIsAnchor.Reset(NumExtended);

    const float MaxHopsFloat = static_cast<float>(MaxHops);

    // Seeds 먼저 추가 (Hop 0)
    for (const FAffectedVertex& AffVert : RingData.Vertices)
    {
        RingData.ExtendedSmoothingIndices.Add(AffVert.VertexIndex);
        RingData.ExtendedHopDistances.Add(0);
        RingData.ExtendedInfluences.Add(1.0f);  // Seeds는 influence 1.0
        RingData.ExtendedIsAnchor.Add(1);       // Seeds는 앵커 (스무딩 건너뜀)
    }

    // Seeds가 아닌 도달 버텍스 추가 (Hop 1+)
    for (const auto& Pair : HopDistanceMap)
    {
        const uint32 VertIdx = Pair.Key;
        const int32 Hop = Pair.Value;

        // Seeds는 이미 추가됨
        if (Hop == 0)
        {
            continue;
        }

        RingData.ExtendedSmoothingIndices.Add(VertIdx);
        RingData.ExtendedHopDistances.Add(Hop);
        RingData.ExtendedIsAnchor.Add(0);  // Extended 버텍스는 스무딩 적용

        // t = 정규화된 홉 거리 (0 = seed, 1 = maxHops)
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

        RingData.ExtendedInfluences.Add(FMath::Clamp(Influence, 0.0f, 1.0f));
    }

    // ===== Step 4: 확장된 영역의 Laplacian 인접 데이터 구축 (캐시 사용) =====
    // [수정] CachedFullAdjacencyMap 대신 CachedVertexLayerTypes 전달
    // BuildExtendedLaplacianAdjacency가 내부적으로 CachedWeldedNeighborPositions 사용
    BuildExtendedLaplacianAdjacency(RingData, CachedVertexLayerTypes);

    // ===== Step 5: 확장된 영역의 RepresentativeIndices 구축 (UV seam welding) =====
    // Heat Propagation에서 UV seam vertex들이 동일한 delta를 받도록 보장
    {
        RingData.ExtendedRepresentativeIndices.Reset(NumExtended);
        RingData.ExtendedRepresentativeIndices.AddUninitialized(NumExtended);

        int32 NumWelded = 0;
        for (int32 i = 0; i < NumExtended; ++i)
        {
            const uint32 VertIdx = RingData.ExtendedSmoothingIndices[i];

            // 캐시에서 O(1) 조회
            const FIntVector* PosKey = CachedVertexToPosition.Find(VertIdx);
            if (PosKey)
            {
                const uint32* Representative = CachedPositionToRepresentative.Find(*PosKey);
                const uint32 RepIdx = Representative ? *Representative : VertIdx;
                RingData.ExtendedRepresentativeIndices[i] = RepIdx;

                if (RepIdx != VertIdx)
                {
                    NumWelded++;
                }
            }
            else
            {
                // 캐시 미스 - 자기 자신을 대표로
                RingData.ExtendedRepresentativeIndices[i] = VertIdx;
            }
        }

        UE_LOG(LogFleshRingVertices, Verbose,
            TEXT("BuildHopDistanceData: ExtendedRepresentativeIndices built, %d vertices (welded=%d)"),
            NumExtended, NumWelded);
    }

    // ===== Step 6: 기존 HopBasedInfluences도 업데이트 (기존 Affected 영역용) =====
    // 이건 기존 코드와의 호환성을 위해 유지
    RingData.HopBasedInfluences.Reset(NumAffected);
    RingData.HopBasedInfluences.AddUninitialized(NumAffected);
    for (int32 i = 0; i < NumAffected; ++i)
    {
        RingData.HopBasedInfluences[i] = 1.0f;  // Seeds는 모두 1.0
    }

    // ===== Step 7: Extended 영역 삼각형 인접 데이터 구축 (NormalRecomputeCS용) =====
    // ExtendedSmoothingIndices에 대한 삼각형 인접 정보 구축
    // 로드리게스 기반 노말 재계산에서 사용
    {
        RingData.ExtendedAdjacencyOffsets.Reset();
        RingData.ExtendedAdjacencyTriangles.Reset();

        if (!bTopologyCacheBuilt || CachedVertexTriangles.Num() == 0)
        {
            UE_LOG(LogFleshRingVertices, Warning,
                TEXT("BuildHopDistanceData: Topology cache not built for Extended adjacency"));
        }
        else
        {
            // Step 7-1: 각 Extended 버텍스의 삼각형 수 계산
            TArray<int32> AdjCounts;
            AdjCounts.SetNumZeroed(NumExtended);

            for (int32 ExtIdx = 0; ExtIdx < NumExtended; ++ExtIdx)
            {
                const uint32 VertexIndex = RingData.ExtendedSmoothingIndices[ExtIdx];
                const TArray<uint32>* TrianglesPtr = CachedVertexTriangles.Find(VertexIndex);
                if (TrianglesPtr)
                {
                    AdjCounts[ExtIdx] = TrianglesPtr->Num();
                }
            }

            // Step 7-2: 오프셋 배열 빌드 (누적합)
            RingData.ExtendedAdjacencyOffsets.SetNum(NumExtended + 1);
            RingData.ExtendedAdjacencyOffsets[0] = 0;

            for (int32 i = 0; i < NumExtended; ++i)
            {
                RingData.ExtendedAdjacencyOffsets[i + 1] = RingData.ExtendedAdjacencyOffsets[i] + AdjCounts[i];
            }

            const uint32 TotalAdjacencies = RingData.ExtendedAdjacencyOffsets[NumExtended];

            // Step 7-3: 삼각형 배열 채우기 (캐시에서 직접 복사)
            RingData.ExtendedAdjacencyTriangles.SetNum(TotalAdjacencies);

            for (int32 ExtIdx = 0; ExtIdx < NumExtended; ++ExtIdx)
            {
                const uint32 VertexIndex = RingData.ExtendedSmoothingIndices[ExtIdx];
                const TArray<uint32>* TrianglesPtr = CachedVertexTriangles.Find(VertexIndex);

                if (TrianglesPtr && TrianglesPtr->Num() > 0)
                {
                    const uint32 Offset = RingData.ExtendedAdjacencyOffsets[ExtIdx];
                    FMemory::Memcpy(
                        &RingData.ExtendedAdjacencyTriangles[Offset],
                        TrianglesPtr->GetData(),
                        TrianglesPtr->Num() * sizeof(uint32)
                    );
                }
            }

            UE_LOG(LogFleshRingVertices, Verbose,
                TEXT("BuildHopDistanceData: ExtendedAdjacencyData built, %d vertices, %d triangles (avg %.1f tri/vert)"),
                NumExtended, TotalAdjacencies,
                NumExtended > 0 ? static_cast<float>(TotalAdjacencies) / NumExtended : 0.0f);
        }
    }

    // 통계 로그
    const int32 NumNewVertices = NumExtended - NumAffected;
    UE_LOG(LogFleshRingVertices, Log,
        TEXT("BuildHopDistanceData: %d seeds → %d extended (%d new vertices from %d-hop BFS)"),
        NumAffected, NumExtended, NumNewVertices, MaxHops);
}
