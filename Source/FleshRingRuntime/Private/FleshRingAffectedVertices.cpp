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
    const FFleshRingSettings& Ring,
    const FTransform& BoneTransform,
    const TArray<FVector3f>& AllVertices,
    TArray<FAffectedVertex>& OutAffected)
{
    OutAffected.Reset();

    // Get Ring center and axis from bone transform
    // 본 트랜스폼에서 링 중심과 축 방향 추출
    const FVector RingCenter = BoneTransform.GetLocation();
    const FVector RingAxis = BoneTransform.GetRotation().GetUpVector();

    // Calculate maximum influence distance (from axis to outer ring surface)
    // 최대 영향 거리 = 내부 반지름 + 링 벽 두께 (축에서 링 바깥면까지)
    const float MaxDistance = Ring.RingRadius + Ring.RingThickness;

    // Reserve estimated capacity (assume ~25% vertices affected)
    // 예상 용량 확보 (약 25% 버텍스가 영향받는다고 가정)
    OutAffected.Reserve(AllVertices.Num() / 4);

    for (int32 VertexIdx = 0; VertexIdx < AllVertices.Num(); ++VertexIdx)
    {
        // Get vertex position (bind pose local space)
        // 버텍스 위치 (바인드 포즈 로컬 스페이스)
        const FVector VertexPos = FVector(AllVertices[VertexIdx]);

        // Calculate vector from Ring center to vertex
        // 링 중심에서 버텍스까지의 벡터 계산
        const FVector ToVertex = VertexPos - RingCenter;

        // Project onto Ring axis to find axial distance
        // 링 축에 투영하여 축 방향 거리 계산
        const float AxisDistance = FVector::DotProduct(ToVertex, RingAxis);

        // Calculate radial distance (perpendicular to axis)
        // 반경 방향 거리 계산 (축에 수직)
        const FVector RadialVec = ToVertex - RingAxis * AxisDistance;
        const float RadialDistance = RadialVec.Size();

        // Check if within influence range (cylindrical model)
        // 영향 범위 내에 있는지 확인 (원통형 모델)
        // 1. Radial distance check (perpendicular to axis) - 반경 거리 체크
        // 2. Axial distance check (along axis) - 축 방향 거리 체크
        const float HalfWidth = Ring.RingWidth / 2.0f;

        if (RadialDistance <= MaxDistance && FMath::Abs(AxisDistance) <= HalfWidth)
        {
            // Calculate radial influence (distance from axis)
            // 반경 방향 영향도 계산 (축으로부터의 거리)
            const float RadialInfluence = CalculateFalloff(RadialDistance, MaxDistance, Ring.FalloffType);

            // Calculate axial influence (distance from ring center along axis)
            // This ensures smooth falloff at ring edges (like stocking/band edges)
            // 축 방향 영향도 계산 (링 중심으로부터 축 방향 거리)
            // 링 가장자리에서 부드러운 감쇠 보장 (스타킹/밴드 가장자리처럼)
            const float AxialInfluence = CalculateFalloff(FMath::Abs(AxisDistance), HalfWidth, Ring.FalloffType);

            // Combine both influences for final effect
            // 두 영향도를 곱하여 최종 효과 계산
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

    UE_LOG(LogFleshRingVertices, Verbose,
        TEXT("DistanceBasedSelector: Selected %d vertices for Ring '%s' (Total: %d)"),
        OutAffected.Num(), *Ring.BoneName.ToString(), AllVertices.Num());
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
    const USkeletalMeshComponent* SkeletalMesh)
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

    // Extract mesh vertices from skeletal mesh
    // 스켈레탈 메시에서 버텍스 추출 (바인드 포즈 컴포넌트 스페이스)
    TArray<FVector3f> MeshVertices;
    if (!ExtractMeshVertices(SkeletalMesh, MeshVertices))
    {
        UE_LOG(LogFleshRingVertices, Error,
            TEXT("RegisterAffectedVertices: Failed to extract mesh vertices"));
        return false;
    }

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("RegisterAffectedVertices: Processing %d vertices for %d Rings"),
        MeshVertices.Num(), Rings.Num());

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

        // [TODO] 링 회전 오프셋 지원 시 아래 코드로 교체 (Role D가 RingRotationOffset 추가 후)
        // FQuat BoneRotation = BoneTransform.GetRotation();
        // FQuat FinalRotation = BoneRotation * RingSettings.RingRotationOffset.Quaternion();
        // RingData.RingAxis = FinalRotation.GetUpVector();
        RingData.RingAxis = BoneTransform.GetRotation().GetUpVector();

        // Ring Geometry (copy from asset)
        // 링 지오메트리 (에셋에서 복사)
        RingData.RingRadius = RingSettings.RingRadius;
        RingData.RingThickness = RingSettings.RingThickness;
        RingData.RingWidth = RingSettings.RingWidth;

        // Deformation Parameters (copy from asset)
        // 변형 파라미터 (에셋에서 복사)
        RingData.TightnessStrength = RingSettings.TightnessStrength;
        RingData.FalloffType = RingSettings.FalloffType;

        // Select affected vertices using current strategy
        // 현재 전략을 사용하여 영향받는 버텍스 선택
        VertexSelector->SelectVertices(
            RingSettings,
            BoneTransform,
            MeshVertices,
            RingData.Vertices // (인덱스 + 영향도)
        );

        // Pack for GPU (convert to flat arrays)
        // GPU용 패킹 (평면 배열로 변환)
        RingData.PackForGPU();

        UE_LOG(LogFleshRingVertices, Log,
            TEXT("Ring[%d] '%s': %d affected vertices"),
            RingIdx, *RingSettings.BoneName.ToString(), RingData.Vertices.Num());

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
    TArray<FVector3f>& OutVertices)
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

    // Use LOD 0 for vertex data
    // LOD 0에서 버텍스 데이터 사용
    const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];
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
