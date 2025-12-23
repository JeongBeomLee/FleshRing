// FleshRing Affected Vertices System - Implementation
// Purpose: Track and manage vertices affected by each Ring
// Role B: Deformation Algorithm (Week 2)

#include "FleshRingAffectedVertices.h"

#include "Components/SkeletalMeshComponent.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"

DEFINE_LOG_CATEGORY_STATIC(LogFleshRingVertices, Log, All);

// Distance-Based Vertex Selector Implementation
void FDistanceBasedVertexSelector::SelectVertices(
    const FFleshRingSettings& Ring,
    const FTransform& BoneTransform,
    const TArray<FVector3f>& AllVertices,
    TArray<FAffectedVertex>& OutAffected)
{
    OutAffected.Reset();

    // Get Ring center and axis from bone transform
    const FVector RingCenter = BoneTransform.GetLocation();
    const FVector RingAxis = BoneTransform.GetRotation().GetUpVector(); // Bone's up vector

    // Calculate maximum influence distance
    const float MaxDistance = Ring.RingRadius + Ring.RingWidth;

    // Reserve estimated capacity
    OutAffected.Reserve(AllVertices.Num() / 4); // Assume ~25% vertices affected

    for (int32 VertexIdx = 0; VertexIdx < AllVertices.Num(); ++VertexIdx)
    {
        // Convert vertex to world space (for now, assume local = world)
        // TODO: Apply mesh transform if needed
        const FVector VertexPos = FVector(AllVertices[VertexIdx]);

        // Calculate vector from Ring center to vertex
        const FVector ToVertex = VertexPos - RingCenter;

        // Project onto Ring axis to find axial distance
        const float AxisDistance = FVector::DotProduct(ToVertex, RingAxis);

        // Calculate radial distance (perpendicular to axis)
        const FVector RadialVec = ToVertex - RingAxis * AxisDistance;
        const float RadialDistance = RadialVec.Size();

        // Check if within influence range (cylindrical model)
        // 1. Radial distance check (perpendicular to axis)
        // 2. Axial distance check (along axis) - for proper edge handling
        const float HalfWidth = Ring.RingWidth / 2.0f;

        if (RadialDistance <= MaxDistance && FMath::Abs(AxisDistance) <= HalfWidth)
        {
            // Calculate radial influence (distance from axis)
            const float RadialInfluence = CalculateFalloff(RadialDistance, MaxDistance, Ring.Falloff);

            // Calculate axial influence (distance from ring center along axis)
            // This ensures smooth falloff at ring edges (like stocking/band edges)
            const float AxialInfluence = CalculateFalloff(FMath::Abs(AxisDistance), HalfWidth, Ring.Falloff);

            // Combine both influences for final effect
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

float FDistanceBasedVertexSelector::CalculateFalloff(
    float Distance,
    float MaxDistance,
    float FalloffParam) const
{
    // Normalize distance to 0-1 range
    const float NormalizedDist = FMath::Clamp(Distance / MaxDistance, 0.0f, 1.0f);
    const float T = 1.0f - NormalizedDist; // Inverted: closer = higher influence

    // [FLEXIBLE] Apply falloff curve based on type
    switch (FalloffType)
    {
    case EFalloffType::Quadratic:
        // Smoother falloff near center
        return T * T;

    case EFalloffType::Smooth:
        // Hermite S-curve (smooth in, smooth out)
        return T * T * (3.0f - 2.0f * T);

    case EFalloffType::Linear:
    default:
        // Simple linear falloff
        return T;
    }
}

// Affected Vertices Manager Implementation

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

bool FFleshRingAffectedVerticesManager::RegisterAffectedVertices(
    const UFleshRingComponent* Component,
    const USkeletalMeshComponent* SkeletalMesh)
{
    if (!Component || !SkeletalMesh || !VertexSelector)
    {
        UE_LOG(LogFleshRingVertices, Warning,
            TEXT("RegisterAffectedVertices: Invalid parameters"));
        return false;
    }

    // Clear previous data
    ClearAll();

    // Extract mesh vertices
    TArray<FVector3f> MeshVertices;
    if (!ExtractMeshVertices(SkeletalMesh, MeshVertices))
    {
        UE_LOG(LogFleshRingVertices, Error,
            TEXT("RegisterAffectedVertices: Failed to extract mesh vertices"));
        return false;
    }

    UE_LOG(LogFleshRingVertices, Log,
        TEXT("RegisterAffectedVertices: Processing %d vertices for %d Rings"),
        MeshVertices.Num(), Component->Rings.Num());

    // Process each Ring
    RingDataArray.Reserve(Component->Rings.Num());

    for (int32 RingIdx = 0; RingIdx < Component->Rings.Num(); ++RingIdx)
    {
        const FFleshRingSettings& RingSettings = Component->Rings[RingIdx];

        // Skip Rings without valid bone
        if (RingSettings.BoneName == NAME_None)
        {
            UE_LOG(LogFleshRingVertices, Warning,
                TEXT("Ring[%d]: Skipping - no bone assigned"), RingIdx);
            continue;
        }

        // Get bone transform
        const int32 BoneIndex = SkeletalMesh->GetBoneIndex(RingSettings.BoneName);
        if (BoneIndex == INDEX_NONE)
        {
            UE_LOG(LogFleshRingVertices, Warning,
                TEXT("Ring[%d]: Bone '%s' not found"), RingIdx, *RingSettings.BoneName.ToString());
            continue;
        }

        const FTransform BoneTransform = SkeletalMesh->GetBoneTransform(BoneIndex);

        // Create Ring data
        FRingAffectedData RingData;
        RingData.BoneName = RingSettings.BoneName;
        RingData.RingCenter = BoneTransform.GetLocation();
        RingData.RingAxis = BoneTransform.GetRotation().GetUpVector();
        RingData.RingRadius = RingSettings.RingRadius;
        RingData.RingWidth = RingSettings.RingWidth;

        // Select affected vertices using current strategy
        VertexSelector->SelectVertices(
            RingSettings,
            BoneTransform,
            MeshVertices,
            RingData.Vertices
        );

        // Pack for GPU
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

bool FFleshRingAffectedVerticesManager::ExtractMeshVertices(
    const USkeletalMeshComponent* SkeletalMesh,
    TArray<FVector3f>& OutVertices)
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

    // Use LOD 0 for vertex data
    const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];
    const uint32 NumVertices = LODData.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices();

    if (NumVertices == 0)
    {
        return false;
    }

    // Extract vertex positions
    OutVertices.Reset(NumVertices);

    for (uint32 VertexIdx = 0; VertexIdx < NumVertices; ++VertexIdx)
    {
        const FVector3f& Position = LODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(VertexIdx);
        OutVertices.Add(Position);
    }

    return true;
}

// ============================================================================
// Console Command for Testing AffectedVertices Registration
// ============================================================================
static FAutoConsoleCommand GFleshRingAffectedVerticesTestCommand(
    TEXT("FleshRing.AffectedVerticesTest"),
    TEXT("Test affected vertices registration with synthetic data"),
    FConsoleCommandDelegate::CreateLambda([]()
    {
        UE_LOG(LogTemp, Log, TEXT("========================================="));
        UE_LOG(LogTemp, Log, TEXT("FleshRing.AffectedVerticesTest: Starting test"));
        UE_LOG(LogTemp, Log, TEXT("========================================="));

        // Create manager
        FFleshRingAffectedVerticesManager Manager;

        // Test distance-based selector directly
        FDistanceBasedVertexSelector Selector;

        // Create synthetic ring settings
        FFleshRingSettings RingSettings;
        RingSettings.BoneName = TEXT("TestBone");
        RingSettings.RingRadius = 3.0f;
        RingSettings.RingWidth = 2.0f;
        RingSettings.Falloff = 1.0f;

        // Create bone transform
        FTransform BoneTransform;
        BoneTransform.SetLocation(FVector(0.0f, 0.0f, 5.0f));
        BoneTransform.SetRotation(FQuat::Identity);

        // Generate synthetic vertices (cylinder)
        TArray<FVector3f> SyntheticVertices;
        const int32 NumVertices = 256;
        SyntheticVertices.Reserve(NumVertices);

        for (int32 i = 0; i < NumVertices; ++i)
        {
            float Angle = (float(i) / float(NumVertices)) * 2.0f * PI;
            float Height = (float(i % 16) / 16.0f) * 10.0f;
            float Radius = 4.0f;

            SyntheticVertices.Add(FVector3f(
                FMath::Cos(Angle) * Radius,
                FMath::Sin(Angle) * Radius,
                Height
            ));
        }

        // Select affected vertices
        TArray<FAffectedVertex> AffectedVertices;
        Selector.SelectVertices(RingSettings, BoneTransform, SyntheticVertices, AffectedVertices);

        UE_LOG(LogTemp, Log, TEXT("Total vertices: %d"), NumVertices);
        UE_LOG(LogTemp, Log, TEXT("Affected vertices: %d"), AffectedVertices.Num());

        if (AffectedVertices.Num() > 0)
        {
            // Log sample vertices
            int32 SamplesToLog = FMath::Min(5, AffectedVertices.Num());
            UE_LOG(LogTemp, Log, TEXT("Sample affected vertices:"));

            for (int32 i = 0; i < SamplesToLog; ++i)
            {
                const FAffectedVertex& V = AffectedVertices[i];
                const FVector3f& Pos = SyntheticVertices[V.VertexIndex];

                UE_LOG(LogTemp, Log,
                    TEXT("  [%d] Index=%d, Distance=%.2f, Influence=%.2f, Pos=(%.1f, %.1f, %.1f)"),
                    i, V.VertexIndex, V.DistanceToRing, V.Influence,
                    Pos.X, Pos.Y, Pos.Z);
            }

            UE_LOG(LogTemp, Log, TEXT("FleshRing.AffectedVerticesTest: ===== PASSED ====="));
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("FleshRing.AffectedVerticesTest: No vertices selected"));
            UE_LOG(LogTemp, Log, TEXT("This may be expected if Ring is outside vertex range"));
        }

        // Test falloff types
        UE_LOG(LogTemp, Log, TEXT("Testing falloff types:"));

        FDistanceBasedVertexSelector LinearSelector;
        LinearSelector.FalloffType = FDistanceBasedVertexSelector::EFalloffType::Linear;

        FDistanceBasedVertexSelector QuadSelector;
        QuadSelector.FalloffType = FDistanceBasedVertexSelector::EFalloffType::Quadratic;

        FDistanceBasedVertexSelector SmoothSelector;
        SmoothSelector.FalloffType = FDistanceBasedVertexSelector::EFalloffType::Smooth;

        TArray<FAffectedVertex> LinearResult, QuadResult, SmoothResult;

        LinearSelector.SelectVertices(RingSettings, BoneTransform, SyntheticVertices, LinearResult);
        QuadSelector.SelectVertices(RingSettings, BoneTransform, SyntheticVertices, QuadResult);
        SmoothSelector.SelectVertices(RingSettings, BoneTransform, SyntheticVertices, SmoothResult);

        UE_LOG(LogTemp, Log, TEXT("  Linear: %d vertices"), LinearResult.Num());
        UE_LOG(LogTemp, Log, TEXT("  Quadratic: %d vertices"), QuadResult.Num());
        UE_LOG(LogTemp, Log, TEXT("  Smooth: %d vertices"), SmoothResult.Num());

        UE_LOG(LogTemp, Log, TEXT("========================================="));
        UE_LOG(LogTemp, Log, TEXT("FleshRing.AffectedVerticesTest: Complete"));
        UE_LOG(LogTemp, Log, TEXT("========================================="));
    })
);
