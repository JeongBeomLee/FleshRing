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

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingVertices, Log, All);

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

        for (int32 VertexIdx = 0; VertexIdx < AllVertices.Num(); ++VertexIdx)
        {
            const FVector VertexPos = FVector(AllVertices[VertexIdx]);

            // Component Space → Local Space 변환
            // InverseTransformPosition: (Rot^-1 * (V - Trans)) / Scale (올바른 순서)
            const FVector LocalPos = LocalToComponent.InverseTransformPosition(VertexPos);

            // OBB 경계 체크 (SDF 볼륨 내부인지 확인)
            if (LocalPos.X < BoundsMin.X || LocalPos.X > BoundsMax.X ||
                LocalPos.Y < BoundsMin.Y || LocalPos.Y > BoundsMax.Y ||
                LocalPos.Z < BoundsMin.Z || LocalPos.Z > BoundsMax.Z)
            {
                continue; // OBB 밖 - 스킵
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

    // Select all vertices within SDF bounding box (OBB)
    // SDF 바운딩 박스(OBB) 내 모든 버텍스 선택
    for (int32 VertexIdx = 0; VertexIdx < AllVertices.Num(); ++VertexIdx)
    {
        const FVector VertexPos = FVector(AllVertices[VertexIdx]);

        // Component Space → Local Space 변환
        // InverseTransformPosition: (Rot^-1 * (V - Trans)) / Scale (올바른 순서)
        const FVector LocalPos = LocalToComponent.InverseTransformPosition(VertexPos);

        // Local Space에서 AABB 포함 테스트
        if (LocalPos.X >= BoundsMin.X && LocalPos.X <= BoundsMax.X &&
            LocalPos.Y >= BoundsMin.Y && LocalPos.Y <= BoundsMax.Y &&
            LocalPos.Z >= BoundsMin.Z && LocalPos.Z <= BoundsMax.Z)
        {
            // Influence=1.0: GPU shader will determine actual influence via SDF sampling
            // Influence=1.0: GPU 셰이더가 SDF 샘플링으로 실제 영향도 결정
            OutAffected.Add(FAffectedVertex(
                static_cast<uint32>(VertexIdx),
                0.0f,  // RadialDistance: SDF 모드에서는 미사용
                1.0f   // Influence: 최대값, GPU 셰이더가 CalculateInfluenceFromSDF()로 정제
            ));
        }
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
            SDFCache  // nullptr이면 SDF 미사용 (Distance 기반 Selector는 무시)
        );

        // Ring별 InfluenceMode에 따라 Selector 결정
        // Auto 모드 + SDF 유효 → SDFBoundsBasedSelector
        // Manual 모드 또는 SDF 무효 → DistanceBasedSelector
        TSharedPtr<IVertexSelector> RingSelector;
        const bool bUseSDFForThisRing =
            (RingSettings.InfluenceMode == EFleshRingInfluenceMode::Auto) &&
            (SDFCache && SDFCache->IsValid());

        if (bUseSDFForThisRing)
        {
            RingSelector = MakeShared<FSDFBoundsBasedVertexSelector>();
        }
        else
        {
            RingSelector = MakeShared<FDistanceBasedVertexSelector>();
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("Ring[%d] '%s': Using %s (InfluenceMode=%s, SDFValid=%s)"),
            RingIdx, *RingSettings.BoneName.ToString(),
            bUseSDFForThisRing ? TEXT("SDFBoundsBasedSelector") : TEXT("DistanceBasedSelector"),
            RingSettings.InfluenceMode == EFleshRingInfluenceMode::Auto ? TEXT("Auto") : TEXT("Manual"),
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
        }

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("Ring[%d] '%s': %d affected vertices, %d adjacency triangles"),
            RingIdx, *RingSettings.BoneName.ToString(),
            RingData.Vertices.Num(), RingData.AdjacencyTriangles.Num());

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
