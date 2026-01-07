// FleshRingSubdivisionComponent.h
// FleshRing Subdivision Component using Hybrid CPU+GPU Architecture
//
// CPU: Red-Green Refinement / LEB 기반 토폴로지 결정
// GPU: Barycentric 보간으로 버텍스 데이터 생성

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
	// Ring이 Bone에 고정됨 - 가장 효율적
	// Bind Pose에서 1회 subdivision, 이후 캐시 사용
	BindPoseFixed UMETA(DisplayName = "Bind Pose Fixed (Recommended)"),

	// Ring 위치 변경 시 자동 재계산 (비동기)
	// 약간의 지연 허용되는 경우
	DynamicAsync UMETA(DisplayName = "Dynamic Async"),

	// 넓은 영역 미리 subdivision
	// 메모리 더 쓰지만 런타임 자유도 높음
	PreSubdivideRegion UMETA(DisplayName = "Pre-Subdivide Region")
};

// =====================================
// Subdivision Result Cache
// =====================================

struct FSubdivisionResultCache
{
	// GPU 버퍼 (Pooled)
	TRefCountPtr<FRDGPooledBuffer> PositionsBuffer;
	TRefCountPtr<FRDGPooledBuffer> NormalsBuffer;
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
 * FleshRing Subdivision 컴포넌트
 *
 * Low-Poly SkeletalMesh에 대해 Ring 영향권 내 삼각형을 adaptive subdivision
 * Red-Green Refinement / LEB 알고리즘으로 T-Junction 없는 crack-free subdivision 보장
 *
 * 아키텍처:
 * - CPU: FHalfEdgeMesh + FLEBSubdivision으로 토폴로지 결정
 * - GPU: Barycentric 보간으로 Position, Normal, UV, BoneWeight 생성
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent), DisplayName="FleshRing Subdivision")
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

	/** Subdivision 활성화 (Low-Poly 전용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision")
	bool bEnableSubdivision = true;

	// =====================================
	// Subdivision Settings
	// =====================================

	/** Subdivision 모드 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision|Settings")
	EFleshRingSubdivisionMode SubdivisionMode = EFleshRingSubdivisionMode::BindPoseFixed;

	/** LEB 최대 레벨 (높을수록 더 세밀, 더 느림) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision|Settings", meta = (ClampMin = "1", ClampMax = "6"))
	int32 MaxSubdivisionLevel = 4;

	/** 최소 엣지 길이 (이보다 작으면 subdivision 중단) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision|Settings", meta = (ClampMin = "0.1"))
	float MinEdgeLength = 1.0f;

	/** Ring 영향 범위 배율 (RingWidth 기준) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision|Settings", meta = (ClampMin = "1.0", ClampMax = "5.0"))
	float InfluenceRadiusMultiplier = 2.0f;

	// =====================================
	// PreSubdivideRegion Mode Settings
	// =====================================

	/** 미리 subdivision할 추가 반경 (PreSubdivideRegion 모드) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision|PreSubdivide",
		meta = (EditCondition = "SubdivisionMode == EFleshRingSubdivisionMode::PreSubdivideRegion", ClampMin = "10.0"))
	float PreSubdivideMargin = 50.0f;

	// =====================================
	// LOD Settings
	// =====================================

	/** 거리 기반 Subdivision 감쇠 활성화 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision|LOD")
	bool bEnableDistanceFalloff = true;

	/** Subdivision 완전 비활성화 거리 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision|LOD",
		meta = (ClampMin = "100.0", EditCondition = "bEnableDistanceFalloff"))
	float SubdivisionFadeDistance = 2000.0f;

	/** Subdivision 최대 유지 거리 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Subdivision|LOD",
		meta = (ClampMin = "50.0", EditCondition = "bEnableDistanceFalloff"))
	float SubdivisionFullDistance = 500.0f;

	// =====================================
	// Bake Settings (Editor Only)
	// =====================================

#if WITH_EDITORONLY_DATA
	/** Bake된 SkeletalMesh 저장 경로 (패키지 경로) */
	UPROPERTY(EditAnywhere, Category = "Subdivision|Bake")
	FString BakedMeshSavePath = TEXT("/Game/BakedMeshes/");

	/** Bake된 메시 이름 접미사 */
	UPROPERTY(EditAnywhere, Category = "Subdivision|Bake")
	FString BakedMeshSuffix = TEXT("_Subdivided");
#endif

	// =====================================
	// Debug
	// =====================================

#if WITH_EDITORONLY_DATA
	/** Subdivision 통계 로그 출력 */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bLogSubdivisionStats = false;

	/** Subdivision으로 추가된 버텍스 시각화 (흰색 점) */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowSubdividedVertices = false;

	/** 시각화 점 크기 */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowSubdividedVertices", ClampMin = "1.0", ClampMax = "20.0"))
	float DebugPointSize = 5.0f;

	/** Subdivision으로 변경된 와이어프레임 표시 (빨간색) */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowSubdividedWireframe = false;

	/** Subdivided 버텍스 디버그 시각화 */
	void DrawSubdividedVerticesDebug();

	/** Subdivided 와이어프레임 디버그 시각화 */
	void DrawSubdividedWireframeDebug();
#endif

#if WITH_EDITOR
	/** Subdivided SkeletalMesh를 에셋으로 Bake */
	UFUNCTION(CallInEditor, Category = "Subdivision|Bake")
	void BakeSubdividedMesh();
#endif

	// =====================================
	// Blueprint Callable Functions
	// =====================================

	/** Subdivision 강제 재계산 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Subdivision")
	void ForceRecompute();

	/** 캐시 무효화 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Subdivision")
	void InvalidateCache();

	/** Subdivision 활성화 여부 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Subdivision")
	bool IsSubdivisionEnabled() const { return bEnableSubdivision && bIsInitialized; }

	/** 원본 버텍스 수 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Subdivision")
	int32 GetOriginalVertexCount() const;

	/** Subdivided 버텍스 수 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Subdivision")
	int32 GetSubdividedVertexCount() const;

	/** Subdivided 삼각형 수 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Subdivision")
	int32 GetSubdividedTriangleCount() const;

	// =====================================
	// Access (Deformer 연동용)
	// =====================================

	/** 캐시된 결과 반환 */
	const FSubdivisionResultCache& GetResultCache() const { return ResultCache; }

	/** 캐시 유효성 */
	bool IsResultCacheValid() const { return ResultCache.IsValid(); }

	/** CPU Processor 접근 */
	const FFleshRingSubdivisionProcessor* GetProcessor() const { return Processor.Get(); }

private:
	/** 연결된 FleshRingComponent */
	UPROPERTY(Transient)
	TWeakObjectPtr<UFleshRingComponent> FleshRingComp;

	/** 대상 SkeletalMeshComponent */
	UPROPERTY(Transient)
	TWeakObjectPtr<USkeletalMeshComponent> TargetMeshComp;

	/** CPU Subdivision Processor */
	TUniquePtr<FFleshRingSubdivisionProcessor> Processor;

	/** GPU 결과 캐시 */
	FSubdivisionResultCache ResultCache;

	/** 초기화 완료 */
	bool bIsInitialized = false;

	/** 재계산 필요 */
	bool bNeedsRecompute = true;

	/** 현재 거리 스케일 */
	float CurrentDistanceScale = 1.0f;

	// Internal functions
	void FindDependencies();
	void Initialize();
	void Cleanup();
	void UpdateDistanceScale();
	void ComputeSubdivision();
	void ExecuteGPUInterpolation();
};
