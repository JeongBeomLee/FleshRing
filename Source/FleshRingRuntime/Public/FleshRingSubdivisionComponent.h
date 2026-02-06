// Copyright 2026 LgThx. All Rights Reserved.

// FleshRingSubdivisionComponent.h
// FleshRing Subdivision Component using Hybrid CPU+GPU Architecture
//
// CPU: Red-Green Refinement / LEB-based topology determination
// GPU: Vertex data generation via Barycentric interpolation

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FleshRingSubdivisionProcessor.h"
#include "FleshRingSubdivisionShader.h"
#include "RenderGraphResources.h"
#include "FleshRingSubdivisionComponent.generated.h"

class UFleshRingComponent;
class USkeletalMeshComponent;

// =====================================
// Subdivision Mode
// =====================================

UENUM(BlueprintType)
enum class EFleshRingSubdivisionMode : uint8
{
	// Ring is fixed to Bone - most efficient
	// Single subdivision at Bind Pose, then use cache
	BindPoseFixed UMETA(DisplayName = "Bind Pose Fixed (Recommended)"),

	// Auto recompute when Ring position changes (async)
	// When slight delay is acceptable
	DynamicAsync UMETA(DisplayName = "Dynamic Async"),

	// Pre-subdivide a wide region
	// Uses more memory but higher runtime flexibility
	PreSubdivideRegion UMETA(DisplayName = "Pre-Subdivide Region")
};

// =====================================
// Subdivision Result Cache
// =====================================

struct FSubdivisionResultCache
{
	// GPU Buffer (Pooled)
	TRefCountPtr<FRDGPooledBuffer> PositionsBuffer;
	TRefCountPtr<FRDGPooledBuffer> NormalsBuffer;
	TRefCountPtr<FRDGPooledBuffer> TangentsBuffer;
	TRefCountPtr<FRDGPooledBuffer> UVsBuffer;
	TRefCountPtr<FRDGPooledBuffer> IndicesBuffer;
	TRefCountPtr<FRDGPooledBuffer> BoneWeightsBuffer;
	TRefCountPtr<FRDGPooledBuffer> BoneIndicesBuffer;

	uint32 NumVertices = 0;
	uint32 NumIndices = 0;
	bool bCached = false;

	void Reset()
	{
		PositionsBuffer.SafeRelease();
		NormalsBuffer.SafeRelease();
		TangentsBuffer.SafeRelease();
		UVsBuffer.SafeRelease();
		IndicesBuffer.SafeRelease();
		BoneWeightsBuffer.SafeRelease();
		BoneIndicesBuffer.SafeRelease();
		NumVertices = 0;
		NumIndices = 0;
		bCached = false;
	}

	bool IsValid() const
	{
		return bCached && PositionsBuffer.IsValid() && IndicesBuffer.IsValid();
	}
};

// =====================================
// Subdivision Component
// =====================================

/**
 * FleshRing Subdivision Component
 *
 * Performs adaptive subdivision on triangles within Ring influence area for Low-Poly SkeletalMesh
 * Guarantees T-Junction free crack-free subdivision using Red-Green Refinement / LEB algorithm
 *
 * Architecture:
 * - CPU: Topology determination via FHalfEdgeMesh + FLEBSubdivision
 * - GPU: Position, Normal, UV, BoneWeight generation via Barycentric interpolation
 */
UCLASS(ClassGroup=(Custom), DisplayName="FleshRing Subdivision")
class FLESHRINGRUNTIME_API UFleshRingSubdivisionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFleshRingSubdivisionComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnRegister() override;
	virtual void OnUnregister() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

public:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// =====================================
	// Enable/Disable
	// =====================================

	/** Enable Subdivision (Low-Poly only) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision")
	bool bEnableSubdivision = true;

	// =====================================
	// Subdivision Settings
	// =====================================

	/** Subdivision Mode */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision|Settings")
	EFleshRingSubdivisionMode SubdivisionMode = EFleshRingSubdivisionMode::BindPoseFixed;

	/** LEB Max Level (higher = finer detail, slower) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision|Settings", meta = (ClampMin = "1", ClampMax = "6"))
	int32 MaxSubdivisionLevel = 4;

	/** Minimum edge length (subdivision stops below this threshold) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision|Settings", meta = (ClampMin = "0.1"))
	float MinEdgeLength = 1.0f;

	// =====================================
	// PreSubdivideRegion Mode Settings
	// =====================================

	/** Additional radius for pre-subdivision (PreSubdivideRegion mode) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision|PreSubdivide",
		meta = (EditCondition = "SubdivisionMode == EFleshRingSubdivisionMode::PreSubdivideRegion", ClampMin = "10.0"))
	float PreSubdivideMargin = 50.0f;

	// =====================================
	// LOD Settings
	// =====================================

	/** Enable distance-based Subdivision falloff */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision|LOD")
	bool bEnableDistanceFalloff = true;

	/** Distance at which Subdivision is completely disabled */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision|LOD",
		meta = (ClampMin = "100.0", EditCondition = "bEnableDistanceFalloff"))
	float SubdivisionFadeDistance = 2000.0f;

	/** Distance at which full Subdivision is maintained */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision|LOD",
		meta = (ClampMin = "50.0", EditCondition = "bEnableDistanceFalloff"))
	float SubdivisionFullDistance = 500.0f;

	// =====================================
	// Bake Settings (Editor Only)
	// =====================================

#if WITH_EDITORONLY_DATA
	/** Baked SkeletalMesh save path (package path) */
	UPROPERTY(EditAnywhere, Category = "Subdivision|Bake")
	FString BakedMeshSavePath = TEXT("/Game/BakedMeshes/");

	/** Baked mesh name suffix */
	UPROPERTY(EditAnywhere, Category = "Subdivision|Bake")
	FString BakedMeshSuffix = TEXT("_Subdivided");
#endif

	// =====================================
	// Debug
	// =====================================

#if WITH_EDITORONLY_DATA
	/** Output Subdivision statistics to log */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bLogSubdivisionStats = false;

	/** Visualize vertices added by Subdivision (white dots) */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowSubdividedVertices = false;

	/** Visualization point size */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowSubdividedVertices", ClampMin = "1.0", ClampMax = "20.0"))
	float DebugPointSize = 5.0f;

	/** Show wireframe modified by Subdivision (red) */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowSubdividedWireframe = false;

	/** Subdivided vertex debug visualization */
	void DrawSubdividedVerticesDebug();

	/** Subdivided wireframe debug visualization */
	void DrawSubdividedWireframeDebug();
#endif

#if WITH_EDITOR
	/** Bake Subdivided SkeletalMesh as asset */
	UFUNCTION(CallInEditor, Category = "Subdivision|Bake")
	void BakeSubdividedMesh();
#endif

	// =====================================
	// Blueprint Callable Functions
	// =====================================

	/** Force Subdivision recomputation */
	void ForceRecompute();

	/** Invalidate cache */
	void InvalidateCache();

	/** Whether Subdivision is enabled */
	bool IsSubdivisionEnabled() const { return bEnableSubdivision && bIsInitialized; }

	/** Original vertex count */
	int32 GetOriginalVertexCount() const;

	/** Subdivided vertex count */
	int32 GetSubdividedVertexCount() const;

	/** Subdivided triangle count */
	int32 GetSubdividedTriangleCount() const;

	// =====================================
	// Access (for Deformer integration)
	// =====================================

	/** Return cached result */
	const FSubdivisionResultCache& GetResultCache() const { return ResultCache; }

	/** Cache validity */
	bool IsResultCacheValid() const { return ResultCache.IsValid(); }

	/** CPU Processor access */
	const FFleshRingSubdivisionProcessor* GetProcessor() const { return Processor.Get(); }

private:
	/** Connected FleshRingComponent */
	UPROPERTY(Transient)
	TWeakObjectPtr<UFleshRingComponent> FleshRingComp;

	/** Target SkeletalMeshComponent */
	UPROPERTY(Transient)
	TWeakObjectPtr<USkeletalMeshComponent> TargetMeshComp;

	/** CPU Subdivision Processor */
	TUniquePtr<FFleshRingSubdivisionProcessor> Processor;

	/** GPU result cache */
	FSubdivisionResultCache ResultCache;

	/** Initialization complete */
	bool bIsInitialized = false;

	/** Recomputation needed */
	bool bNeedsRecompute = true;

	/** Current distance scale */
	float CurrentDistanceScale = 1.0f;

	// Internal functions
	void FindDependencies();
	void Initialize();
	void Cleanup();
	void UpdateDistanceScale();
	void ComputeSubdivision();
	void ExecuteGPUInterpolation();
};
