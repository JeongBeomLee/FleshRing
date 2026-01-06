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
        return EFleshRingLayerType::Unknown;
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
            OutVertexLayerTypes[i] = EFleshRingLayerType::Unknown;
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
        const float HalfWidth = Ring.RingWidth / 2.0f;

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
        // ===== Fallback: 원통형 모델 (SDFCache 없을 때) =====
        // MeshOffset을 본 회전에 맞게 변환 후 적용
        const FQuat BoneRotation = BoneTransform.GetRotation();
        const FVector WorldMeshOffset = BoneRotation.RotateVector(Ring.MeshOffset);
        const FVector RingCenter = BoneTransform.GetLocation() + WorldMeshOffset;
        const FQuat WorldMeshRotation = BoneRotation * FQuat(Ring.MeshRotation);
        const FVector RingAxis = WorldMeshRotation.RotateVector(FVector::ZAxisVector);

        const float RadialScale = (Ring.MeshScale.X + Ring.MeshScale.Y) * 0.5f;
        const float AxialScale = Ring.MeshScale.Z;
        const float MaxDistance = (Ring.RingRadius + Ring.RingThickness) * RadialScale;
        const float HalfWidth = (Ring.RingWidth / 2.0f) * AxialScale;

        for (int32 VertexIdx = 0; VertexIdx < AllVertices.Num(); ++VertexIdx)
        {
            const FVector VertexPos = FVector(AllVertices[VertexIdx]);
            const FVector ToVertex = VertexPos - RingCenter;
            const float AxisDistance = FVector::DotProduct(ToVertex, RingAxis);
            const FVector RadialVec = ToVertex - RingAxis * AxisDistance;
            const float RadialDistance = RadialVec.Size();

            if (RadialDistance <= MaxDistance && FMath::Abs(AxisDistance) <= HalfWidth)
            {
                const float ScaledRingRadius = Ring.RingRadius * RadialScale;
                const float ScaledRingThickness = Ring.RingThickness * RadialScale;
                const float DistFromRingSurface = FMath::Abs(RadialDistance - ScaledRingRadius);
                const float RadialInfluence = CalculateFalloff(DistFromRingSurface, ScaledRingThickness, Ring.FalloffType);
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

    const FVector BoundsMin = FVector(Context.SDFCache->BoundsMin);
    const FVector BoundsMax = FVector(Context.SDFCache->BoundsMax);
    const TArray<FVector3f>& AllVertices = Context.AllVertices;

    // [디버그] LocalToComponent 변환 정보 로그 (스케일 확인용)
    UE_LOG(LogFleshRingVertices, Log,
        TEXT("SDFBoundsSelector: Ring[%d] LocalToComponent Scale=%s, Rot=%s, Trans=%s"),
        Context.RingIndex,
        *LocalToComponent.GetScale3D().ToString(),
        *LocalToComponent.GetRotation().Rotator().ToString(),
        *LocalToComponent.GetLocation().ToString());

    // Reserve estimated capacity
    // 예상 용량 확보
    OutAffected.Reserve(AllVertices.Num() / 4);

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

    // Select all vertices within SDF bounding box (OBB)
    // SDF 바운딩 박스(OBB) 내 모든 버텍스 선택
    for (int32 VertexIdx : CandidateIndices)
    {
        const FVector VertexPos = FVector(AllVertices[VertexIdx]);

        // Component Space → Local Space 변환
        // InverseTransformPosition: (Rot^-1 * (V - Trans)) / Scale (올바른 순서)
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

        // Influence=1.0: GPU shader will determine actual influence via SDF sampling
        // Influence=1.0: GPU 셰이더가 SDF 샘플링으로 실제 영향도 결정
        OutAffected.Add(FAffectedVertex(
            static_cast<uint32>(VertexIdx),
            0.0f,  // RadialDistance: SDF 모드에서는 미사용
            1.0f   // Influence: 최대값, GPU 셰이더가 CalculateInfluenceFromSDF()로 정제
        ));
    }

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("SDFBoundsBasedSelector: Selected %d vertices for Ring[%d] '%s' (LocalBounds: [%.1f,%.1f,%.1f] - [%.1f,%.1f,%.1f])"),
        OutAffected.Num(), Context.RingIndex, *Context.RingSettings.BoneName.ToString(),
        BoundsMin.X, BoundsMin.Y, BoundsMin.Z,
        BoundsMax.X, BoundsMax.Y, BoundsMax.Z);
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

    // Clear previous data
    // 이전 데이터 초기화
    ClearAll();

    // FleshRingAsset null check
    // FleshRingAsset null 체크
    if (!Component->FleshRingAsset)
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("RegisterAffectedVertices: FleshRingAsset is null"));
        return false;
    }

    const TArray<FFleshRingSettings>& Rings = Component->FleshRingAsset->Rings;

    // Extract mesh vertices from skeletal mesh at specified LOD
    // 스켈레탈 메시의 지정된 LOD에서 버텍스 추출 (바인드 포즈 컴포넌트 스페이스)
    TArray<FVector3f> MeshVertices;
    if (!ExtractMeshVertices(SkeletalMesh, MeshVertices, LODIndex))
    {
        UE_LOG(LogFleshRingVertices, Error,
            TEXT("RegisterAffectedVertices: Failed to extract mesh vertices"));
        return false;
    }

    // Build Spatial Hash for O(1) vertex queries
    // O(1) 버텍스 쿼리를 위한 Spatial Hash 빌드
    VertexSpatialHash.Build(MeshVertices);
    UE_LOG(LogFleshRingVertices, Log,
        TEXT("RegisterAffectedVertices: Built SpatialHash for %d vertices"),
        MeshVertices.Num());

    // Extract mesh indices for adjacency data (Normal recomputation)
    // 인접 데이터용 메시 인덱스 추출 (노멀 재계산용)
    CachedMeshIndices.Reset();
    if (!ExtractMeshIndices(SkeletalMesh, CachedMeshIndices, LODIndex))
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("RegisterAffectedVertices: Failed to extract mesh indices, Normal recomputation will be disabled"));
    }

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("RegisterAffectedVertices: Processing %d vertices, %d indices for %d Rings"),
        MeshVertices.Num(), CachedMeshIndices.Num(), Rings.Num());

    // Build per-vertex layer types
    // 버텍스별 레이어 타입 빌드
    // 우선순위: 1. 에셋 매핑 (수동 설정) → 2. 키워드 자동 감지
    TArray<EFleshRingLayerType> VertexLayerTypes;
    bool bUsedAssetMapping = false;

    // 에셋에서 매핑 가져오기
    if (Component && Component->FleshRingAsset && Component->FleshRingAsset->MaterialLayerMappings.Num() > 0)
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

                VertexLayerTypes.SetNum(NumVertices);
                for (int32 i = 0; i < NumVertices; ++i)
                {
                    VertexLayerTypes[i] = EFleshRingLayerType::Unknown;
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
                            VertexLayerTypes[VertexIndex] = LayerType;
                        }
                    }
                }

                bUsedAssetMapping = true;
                UE_LOG(LogFleshRingVertices, Log,
                    TEXT("RegisterAffectedVertices: Using asset material layer mappings (%d mappings)"),
                    Asset->MaterialLayerMappings.Num());
            }
        }
    }

    // 에셋 매핑이 없으면 키워드 기반 자동 감지 폴백
    if (!bUsedAssetMapping)
    {
        if (!FleshRingLayerUtils::BuildVertexLayerTypes(SkeletalMesh, LODIndex, VertexLayerTypes))
        {
            UE_LOG(LogFleshRingVertices, Warning,
                TEXT("RegisterAffectedVertices: Failed to build vertex layer types, using Unknown for all"));
            VertexLayerTypes.SetNum(MeshVertices.Num());
            for (int32 i = 0; i < MeshVertices.Num(); ++i)
            {
                VertexLayerTypes[i] = EFleshRingLayerType::Unknown;
            }
        }
        else
        {
            UE_LOG(LogFleshRingVertices, Log,
                TEXT("RegisterAffectedVertices: Using keyword-based layer detection (no asset mappings)"));
        }
    }

    // Process each Ring
    // 각 링 처리
    RingDataArray.Reserve(Rings.Num());

    for (int32 RingIdx = 0; RingIdx < Rings.Num(); ++RingIdx)
    {
        const FFleshRingSettings& RingSettings = Rings[RingIdx];

        // Skip Rings without valid bone
        // 유효한 본이 없는 링은 건너뜀
        if (RingSettings.BoneName == NAME_None)
        {
            UE_LOG(LogFleshRingVertices, Warning,
                TEXT("Ring[%d]: Skipping - no bone assigned"), RingIdx);
            continue;
        }

        // Get bone index from skeletal mesh
        // 스켈레탈 메시에서 본 인덱스 가져오기
        const int32 BoneIndex = SkeletalMesh->GetBoneIndex(RingSettings.BoneName);
        if (BoneIndex == INDEX_NONE)
        {
            UE_LOG(LogFleshRingVertices, Warning,
                TEXT("Ring[%d]: Bone '%s' not found"), RingIdx, *RingSettings.BoneName.ToString());
            continue;
        }

        // Get skeletal mesh asset for reference skeleton
        // 레퍼런스 스켈레톤을 위한 스켈레탈 메시 에셋 가져오기
        USkeletalMesh* SkelMeshAsset = SkeletalMesh->GetSkeletalMeshAsset();
        if (!SkelMeshAsset)
        {
            UE_LOG(LogFleshRingVertices, Warning,
                TEXT("Ring[%d]: SkeletalMesh asset is null"), RingIdx);
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

        // [TODO] 링 위치 오프셋 지원 시 아래 코드로 교체 (Role D가 RingPositionOffset 추가 후)
        // FVector LocalOffset = RingSettings.RingPositionOffset;
        // FVector WorldOffset = BoneTransform.GetRotation().RotateVector(LocalOffset);
        // RingData.RingCenter = BoneTransform.GetLocation() + WorldOffset;
        RingData.RingCenter = BoneTransform.GetLocation();

        // 링 축 계산: 메시 회전을 반영하여 실제 토러스 구멍 방향 계산
        // BoneRotation * MeshRotation * ZAxis (MeshRotation 기본값으로 Z축이 본 X축과 일치)
        FQuat BoneRotation = BoneTransform.GetRotation();
        FQuat WorldMeshRotation = BoneRotation * FQuat(RingSettings.MeshRotation);
        RingData.RingAxis = WorldMeshRotation.RotateVector(FVector::ZAxisVector);

        // Ring Geometry (copy from asset with MeshScale applied)
        // 링 지오메트리 (에셋에서 복사, MeshScale 반영)
        // 반경 방향 스케일 (X, Y 평균) 과 축 방향 스케일 (Z) 분리
        const float RadialScale = (RingSettings.MeshScale.X + RingSettings.MeshScale.Y) * 0.5f;
        const float AxialScale = RingSettings.MeshScale.Z;

        RingData.RingRadius = RingSettings.RingRadius * RadialScale;
        RingData.RingThickness = RingSettings.RingThickness * RadialScale;
        RingData.RingWidth = RingSettings.RingWidth * AxialScale;

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
            &VertexSpatialHash  // O(1) 버텍스 쿼리용 Spatial Hash
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

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("Ring[%d] '%s': Using %s (InfluenceMode=%s, SDFValid=%s)"),
            RingIdx, *RingSettings.BoneName.ToString(),
            bUseSDFForThisRing ? TEXT("SDFBoundsBasedSelector") : TEXT("DistanceBasedSelector"),
            InfluenceModeStr,
            (SDFCache && SDFCache->IsValid()) ? TEXT("Yes") : TEXT("No"));

        // Ring별 Selector로 영향받는 버텍스 선택
        RingSelector->SelectVertices(Context, RingData.Vertices);

        // Pack for GPU (convert to flat arrays)
        // GPU용 패킹 (평면 배열로 변환)
        RingData.PackForGPU();

        // Build adjacency data for Normal recomputation
        // 노멀 재계산용 인접 데이터 빌드
        if (CachedMeshIndices.Num() > 0)
        {
            BuildAdjacencyData(RingData, CachedMeshIndices);

            // Build Laplacian adjacency data for smoothing
            // 스무딩용 라플라시안 인접 데이터 빌드
            // 개선: 같은 레이어의 이웃만 포함하여 레이어 경계 혼합 방지
            BuildLaplacianAdjacencyData(RingData, CachedMeshIndices, MeshVertices, VertexLayerTypes);
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("Ring[%d] '%s': %d affected vertices, %d adjacency triangles, %d laplacian adjacency uints"),
            RingIdx, *RingSettings.BoneName.ToString(),
            RingData.Vertices.Num(), RingData.AdjacencyTriangles.Num(), RingData.LaplacianAdjacencyData.Num());

        RingDataArray.Add(MoveTemp(RingData));
    }

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("RegisterAffectedVertices: Complete. Total affected: %d"),
        GetTotalAffectedCount());

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
    RingDataArray.Reset();
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
// BuildAdjacencyData - 인접 삼각형 데이터 빌드
// ============================================================================
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

    // ================================================================
    // Step 1: Build vertex-to-affected-index lookup
    // 1단계: 버텍스 인덱스 → 영향받는 버텍스 인덱스 룩업 빌드
    // ================================================================
    // 영향받는 버텍스가 전체 버텍스의 일부이므로, 빠른 룩업을 위해 맵 사용
    TMap<uint32, int32> VertexToAffectedIndex;
    VertexToAffectedIndex.Reserve(NumAffected);

    for (int32 AffIdx = 0; AffIdx < NumAffected; ++AffIdx)
    {
        VertexToAffectedIndex.Add(RingData.Vertices[AffIdx].VertexIndex, AffIdx);
    }

    // ================================================================
    // Step 2: Build per-affected-vertex triangle lists
    // 2단계: 영향받는 버텍스별 삼각형 리스트 빌드
    // ================================================================
    // TArray<TArray<uint32>>는 느리므로 2-pass 방식 사용
    // Pass 1: 각 영향받는 버텍스의 인접 삼각형 수 계산
    TArray<int32> AdjCounts;
    AdjCounts.SetNumZeroed(NumAffected);

    const int32 NumTriangles = MeshIndices.Num() / 3;

    for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
    {
        const uint32 I0 = MeshIndices[TriIdx * 3 + 0];
        const uint32 I1 = MeshIndices[TriIdx * 3 + 1];
        const uint32 I2 = MeshIndices[TriIdx * 3 + 2];

        // 이 삼각형의 각 버텍스가 영향받는 버텍스인지 확인
        if (const int32* AffIdx = VertexToAffectedIndex.Find(I0))
        {
            AdjCounts[*AffIdx]++;
        }
        if (const int32* AffIdx = VertexToAffectedIndex.Find(I1))
        {
            AdjCounts[*AffIdx]++;
        }
        if (const int32* AffIdx = VertexToAffectedIndex.Find(I2))
        {
            AdjCounts[*AffIdx]++;
        }
    }

    // ================================================================
    // Step 3: Build offsets array (prefix sum)
    // 3단계: 오프셋 배열 빌드 (누적합)
    // ================================================================
    RingData.AdjacencyOffsets.SetNum(NumAffected + 1);  // +1 for sentinel
    RingData.AdjacencyOffsets[0] = 0;

    for (int32 i = 0; i < NumAffected; ++i)
    {
        RingData.AdjacencyOffsets[i + 1] = RingData.AdjacencyOffsets[i] + AdjCounts[i];
    }

    const uint32 TotalAdjacencies = RingData.AdjacencyOffsets[NumAffected];

    // ================================================================
    // Step 4: Fill adjacency triangles array
    // 4단계: 인접 삼각형 배열 채우기
    // ================================================================
    RingData.AdjacencyTriangles.SetNum(TotalAdjacencies);

    // 현재 쓰기 위치 추적용 (AdjCounts 재활용)
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

        if (const int32* AffIdx = VertexToAffectedIndex.Find(I0))
        {
            RingData.AdjacencyTriangles[WritePos[*AffIdx]++] = static_cast<uint32>(TriIdx);
        }
        if (const int32* AffIdx = VertexToAffectedIndex.Find(I1))
        {
            RingData.AdjacencyTriangles[WritePos[*AffIdx]++] = static_cast<uint32>(TriIdx);
        }
        if (const int32* AffIdx = VertexToAffectedIndex.Find(I2))
        {
            RingData.AdjacencyTriangles[WritePos[*AffIdx]++] = static_cast<uint32>(TriIdx);
        }
    }

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("BuildAdjacencyData: %d affected vertices, %d total adjacencies (avg %.1f triangles/vertex)"),
        NumAffected, TotalAdjacencies,
        NumAffected > 0 ? static_cast<float>(TotalAdjacencies) / NumAffected : 0.0f);
}

// ============================================================================
// BuildLaplacianAdjacencyData - 라플라시안 스무딩용 이웃 데이터 빌드
// ============================================================================
// 개선: 같은 레이어의 이웃만 포함 (스타킹-살 경계에서 섞이지 않음)
// 개선: UV seam 용접 - 동일 위치의 분리된 버텍스들이 같은 이웃을 공유하여 크랙 방지
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
    // Step 0: Build position-based vertex groups for UV seam welding
    // 0단계: UV seam 용접을 위한 위치 기반 버텍스 그룹 구축
    // 동일 위치의 버텍스들(UV seam으로 분리된)을 하나로 취급
    // ================================================================
    constexpr float WeldPrecision = 0.001f;  // 0.001 units tolerance

    // Position key -> All vertex indices at that position
    TMap<FIntVector, TArray<uint32>> PositionToVertices;
    // Vertex index -> Position key
    TMap<uint32, FIntVector> VertexToPosition;

    for (int32 i = 0; i < AllVertices.Num(); ++i)
    {
        const FVector3f& Pos = AllVertices[i];
        FIntVector PosKey(
            FMath::RoundToInt(Pos.X / WeldPrecision),
            FMath::RoundToInt(Pos.Y / WeldPrecision),
            FMath::RoundToInt(Pos.Z / WeldPrecision)
        );
        PositionToVertices.FindOrAdd(PosKey).Add(static_cast<uint32>(i));
        VertexToPosition.Add(static_cast<uint32>(i), PosKey);
    }

    int32 WeldedPositionCount = PositionToVertices.Num();
    int32 DuplicateVertexCount = AllVertices.Num() - WeldedPositionCount;

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("BuildLaplacianAdjacencyData: Welding %d vertices -> %d positions (%d duplicates)"),
        AllVertices.Num(), WeldedPositionCount, DuplicateVertexCount);

    // ================================================================
    // Step 1: Build global vertex neighbor map from mesh triangles
    // 1단계: 메시 삼각형에서 전역 버텍스 이웃 맵 구축
    // ================================================================
    TMap<uint32, TSet<uint32>> VertexNeighbors;

    const int32 NumTriangles = MeshIndices.Num() / 3;
    for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
    {
        const uint32 I0 = MeshIndices[TriIdx * 3 + 0];
        const uint32 I1 = MeshIndices[TriIdx * 3 + 1];
        const uint32 I2 = MeshIndices[TriIdx * 3 + 2];

        VertexNeighbors.FindOrAdd(I0).Add(I1);
        VertexNeighbors.FindOrAdd(I0).Add(I2);
        VertexNeighbors.FindOrAdd(I1).Add(I0);
        VertexNeighbors.FindOrAdd(I1).Add(I2);
        VertexNeighbors.FindOrAdd(I2).Add(I0);
        VertexNeighbors.FindOrAdd(I2).Add(I1);
    }

    // ================================================================
    // Step 2: Build welded neighbor map (merge neighbors across UV duplicates)
    // 2단계: 용접된 이웃 맵 구축 (UV 중복 버텍스들의 이웃 병합)
    //
    // 문제: UV seam에서 분리된 버텍스들(A, B)이 같은 위치에 있지만
    //       서로 다른 이웃 집합을 가짐 -> 스무딩 시 다르게 이동 -> 크랙!
    //
    // 해결: 같은 위치의 모든 버텍스들의 이웃을 병합하여
    //       동일한 스무딩 결과를 보장
    // ================================================================
    TMap<FIntVector, TSet<FIntVector>> PositionToWeldedNeighborPositions;

    for (const auto& PosEntry : PositionToVertices)
    {
        const FIntVector& PosKey = PosEntry.Key;
        const TArray<uint32>& VerticesAtPos = PosEntry.Value;

        // Merge all neighbors from all vertices at this position
        TSet<FIntVector> MergedNeighborPositions;

        for (uint32 VertIdx : VerticesAtPos)
        {
            const TSet<uint32>* Neighbors = VertexNeighbors.Find(VertIdx);
            if (Neighbors)
            {
                for (uint32 NeighborIdx : *Neighbors)
                {
                    const FIntVector* NeighborPosKey = VertexToPosition.Find(NeighborIdx);
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

        PositionToWeldedNeighborPositions.Add(PosKey, MoveTemp(MergedNeighborPositions));
    }

    // ================================================================
    // Step 3: Pack adjacency data for affected vertices
    // 3단계: 영향받는 버텍스에 대한 인접 데이터 패킹
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

        // Get this vertex's position key
        const FIntVector* MyPosKey = VertexToPosition.Find(VertexIndex);
        if (MyPosKey)
        {
            // Get the welded neighbor positions for this position
            const TSet<FIntVector>* WeldedNeighborPosSet = PositionToWeldedNeighborPositions.Find(*MyPosKey);

            if (WeldedNeighborPosSet)
            {
                for (const FIntVector& NeighborPosKey : *WeldedNeighborPosSet)
                {
                    // Get a representative vertex at that position
                    const TArray<uint32>* VerticesAtNeighborPos = PositionToVertices.Find(NeighborPosKey);
                    if (!VerticesAtNeighborPos || VerticesAtNeighborPos->Num() == 0)
                    {
                        continue;
                    }

                    // Use the first vertex at that position as representative
                    const uint32 NeighborIdx = (*VerticesAtNeighborPos)[0];

                    // Layer type filtering: only include neighbors of same layer
                    EFleshRingLayerType NeighborLayerType = EFleshRingLayerType::Unknown;
                    if (VertexLayerTypes.IsValidIndex(static_cast<int32>(NeighborIdx)))
                    {
                        NeighborLayerType = VertexLayerTypes[static_cast<int32>(NeighborIdx)];
                    }

                    const bool bSameLayer = (MyLayerType == NeighborLayerType);
                    const bool bBothUnknown = (MyLayerType == EFleshRingLayerType::Unknown &&
                                              NeighborLayerType == EFleshRingLayerType::Unknown);

                    if (bSameLayer || bBothUnknown)
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
        TEXT("BuildLaplacianAdjacencyData (Welded): %d affected, %d packed uints, %d cross-layer skipped"),
        NumAffected, RingData.LaplacianAdjacencyData.Num(), CrossLayerSkipped);
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
        TArray<int32> AdjacentVertices;
        AdjacentVertices.Reserve(FRingAffectedData::MAX_SLICE_VERTICES);

        for (int32 Delta = -1; Delta <= 1; ++Delta)
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
// BuildPBDAdjacencyData - PBD 에지 제약용 인접 데이터 빌드
// ============================================================================

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
    // 버텍스별 이웃 세트 빌드 (rest length 포함)
    // Key: neighbor vertex index, Value: rest length
    TArray<TMap<uint32, float>> VertexNeighborsWithRestLen;
    VertexNeighborsWithRestLen.SetNum(NumAffected);

    const int32 NumTriangles = MeshIndices.Num() / 3;
    for (int32 TriIdx = 0; TriIdx < NumTriangles; ++TriIdx)
    {
        const uint32 Idx0 = MeshIndices[TriIdx * 3 + 0];
        const uint32 Idx1 = MeshIndices[TriIdx * 3 + 1];
        const uint32 Idx2 = MeshIndices[TriIdx * 3 + 2];

        // 삼각형의 세 에지를 처리
        const uint32 TriIndices[3] = { Idx0, Idx1, Idx2 };

        for (int32 Edge = 0; Edge < 3; ++Edge)
        {
            const uint32 V0 = TriIndices[Edge];
            const uint32 V1 = TriIndices[(Edge + 1) % 3];

            // V0가 영향 영역에 있으면 V1을 이웃으로 추가
            if (int32* ThreadIdxPtr = VertexToThreadIndex.Find(V0))
            {
                const int32 ThreadIdx = *ThreadIdxPtr;

                // V1이 유효한 인덱스인지 확인
                if (V1 < static_cast<uint32>(AllVertices.Num()))
                {
                    // Rest length 계산 (바인드 포즈 거리)
                    const FVector3f& Pos0 = AllVertices[V0];
                    const FVector3f& Pos1 = AllVertices[V1];
                    const float RestLength = FVector3f::Distance(Pos0, Pos1);

                    // 이미 등록된 이웃이면 rest length는 동일해야 하므로 스킵
                    if (!VertexNeighborsWithRestLen[ThreadIdx].Contains(V1))
                    {
                        VertexNeighborsWithRestLen[ThreadIdx].Add(V1, RestLength);
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
    const float RingHalfWidth = RingData.RingWidth * 0.5f;

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

void FFleshRingAffectedVerticesManager::BuildExtendedLaplacianAdjacency(
    FRingAffectedData& RingData,
    const TMap<uint32, TArray<uint32>>& FullAdjacencyMap)
{
    constexpr int32 MAX_NEIGHBORS = 12;
    constexpr int32 PACKED_SIZE = 1 + MAX_NEIGHBORS;

    const int32 NumExtended = RingData.ExtendedSmoothingIndices.Num();
    if (NumExtended == 0)
    {
        RingData.ExtendedLaplacianAdjacency.Reset();
        return;
    }

    // ExtendedSmoothingIndices의 버텍스 인덱스 → 확장 영역 내 ThreadIndex 매핑
    TMap<uint32, int32> VertexToExtendedIdx;
    VertexToExtendedIdx.Reserve(NumExtended);
    for (int32 ExtIdx = 0; ExtIdx < NumExtended; ++ExtIdx)
    {
        VertexToExtendedIdx.Add(RingData.ExtendedSmoothingIndices[ExtIdx], ExtIdx);
    }

    // 패킹된 인접 데이터 구축
    RingData.ExtendedLaplacianAdjacency.Reset(NumExtended * PACKED_SIZE);
    RingData.ExtendedLaplacianAdjacency.AddZeroed(NumExtended * PACKED_SIZE);

    for (int32 ExtIdx = 0; ExtIdx < NumExtended; ++ExtIdx)
    {
        const uint32 VertexIdx = RingData.ExtendedSmoothingIndices[ExtIdx];
        const int32 BaseOffset = ExtIdx * PACKED_SIZE;

        const TArray<uint32>* NeighborsPtr = FullAdjacencyMap.Find(VertexIdx);
        if (!NeighborsPtr)
        {
            RingData.ExtendedLaplacianAdjacency[BaseOffset] = 0;
            continue;
        }

        // 확장 영역 내의 이웃만 포함
        // 중요: 셰이더가 ReadPosition(InputPositions, NeighborIndex)로 사용하므로
        //       raw VertexIndex를 저장해야 함 (ThreadIndex가 아님!)
        int32 ValidNeighborCount = 0;
        for (uint32 NeighborVertIdx : *NeighborsPtr)
        {
            if (ValidNeighborCount >= MAX_NEIGHBORS) break;

            // 확장 영역에 속하는지만 확인 (존재 여부 체크용)
            if (VertexToExtendedIdx.Contains(NeighborVertIdx))
            {
                // raw VertexIndex를 저장 (셰이더가 InputPositions에서 읽을 때 사용)
                RingData.ExtendedLaplacianAdjacency[BaseOffset + 1 + ValidNeighborCount] = NeighborVertIdx;
                ++ValidNeighborCount;
            }
        }

        RingData.ExtendedLaplacianAdjacency[BaseOffset] = static_cast<uint32>(ValidNeighborCount);
    }
}

// ============================================================================
// BuildHopDistanceData - 홉 기반 스무딩용 확장 영역 구축 (전체 메시 BFS)
// ============================================================================

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

    // ===== Step 1: 전체 메시 인접 맵 구축 =====
    TMap<uint32, TArray<uint32>> FullAdjacencyMap;
    BuildFullMeshAdjacency(MeshIndices, NumTotalVertices, FullAdjacencyMap);

    // ===== Step 2: Seeds = 모든 Affected Vertices =====
    // Seeds는 전체 메시 버텍스 인덱스
    TSet<uint32> SeedSet;
    SeedSet.Reserve(NumAffected);
    for (const FAffectedVertex& AffVert : RingData.Vertices)
    {
        SeedSet.Add(AffVert.VertexIndex);
    }

    // ===== Step 3: 전체 메시에서 BFS (N-hop 도달 버텍스 수집) =====
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

    // BFS 전파
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

        // 이웃들 확인
        const TArray<uint32>* NeighborsPtr = FullAdjacencyMap.Find(CurrentVertIdx);
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

    // ===== Step 4: ExtendedSmoothing* 배열 구축 =====
    const int32 NumExtended = HopDistanceMap.Num();
    RingData.ExtendedSmoothingIndices.Reset(NumExtended);
    RingData.ExtendedHopDistances.Reset(NumExtended);
    RingData.ExtendedInfluences.Reset(NumExtended);

    const float MaxHopsFloat = static_cast<float>(MaxHops);

    // Seeds 먼저 추가 (Hop 0)
    for (const FAffectedVertex& AffVert : RingData.Vertices)
    {
        RingData.ExtendedSmoothingIndices.Add(AffVert.VertexIndex);
        RingData.ExtendedHopDistances.Add(0);
        RingData.ExtendedInfluences.Add(1.0f);  // Seeds는 influence 1.0
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

    // ===== Step 5: 확장된 영역의 Laplacian 인접 데이터 구축 =====
    BuildExtendedLaplacianAdjacency(RingData, FullAdjacencyMap);

    // ===== Step 6: 기존 HopBasedInfluences도 업데이트 (기존 Affected 영역용) =====
    // 이건 기존 코드와의 호환성을 위해 유지
    RingData.HopBasedInfluences.Reset(NumAffected);
    RingData.HopBasedInfluences.AddUninitialized(NumAffected);
    for (int32 i = 0; i < NumAffected; ++i)
    {
        RingData.HopBasedInfluences[i] = 1.0f;  // Seeds는 모두 1.0
    }

    // 통계 로그
    const int32 NumNewVertices = NumExtended - NumAffected;
    UE_LOG(LogFleshRingVertices, Log,
        TEXT("BuildHopDistanceData: %d seeds → %d extended (%d new vertices from %d-hop BFS)"),
        NumAffected, NumExtended, NumNewVertices, MaxHops);
}
