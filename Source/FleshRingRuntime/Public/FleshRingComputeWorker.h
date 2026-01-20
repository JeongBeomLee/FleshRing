// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include <atomic>
#include "ComputeWorkerInterface.h"
#include "ComputeSystemInterface.h"
#include "RenderGraphResources.h"
#include "RendererInterface.h"
#include "FleshRingTightnessShader.h"
#include "FleshRingDebugTypes.h"
#include "FleshRingBulgeShader.h"
#include "FleshRingNormalRecomputeShader.h"
#include "FleshRingTangentRecomputeShader.h"
#include "FleshRingLaplacianShader.h"
#include "FleshRingBoneRatioShader.h"
#include "FleshRingCollisionShader.h"
#include "FleshRingLayerPenetrationShader.h"
#include "FleshRingPBDEdgeShader.h"
#include "FleshRingSkinSDFShader.h"
#include "FleshRingHeatPropagationShader.h"

class FSkeletalMeshObject;
class UFleshRingDeformerInstance;
class FFleshRingDebugViewExtension;
struct IPooledRenderTarget;

// ============================================================================
// FFleshRingWorkItem - 큐잉된 작업 항목
// ============================================================================
struct FFleshRingWorkItem
{
	// 작업 식별
	TWeakObjectPtr<UFleshRingDeformerInstance> DeformerInstance;
	FSkeletalMeshObject* MeshObject = nullptr;
	int32 LODIndex = 0;

	// 버텍스 데이터
	uint32 TotalVertexCount = 0;
	TSharedPtr<TArray<float>> SourceDataPtr;

	// Ring 데이터 (Tightness + Bulge용)
	struct FRingDispatchData
	{
		// 원본 Ring 인덱스 (FleshRingAsset->Rings 배열의 인덱스)
		// 버텍스가 없는 Ring이 스킵되어도 설정 조회 시 올바른 인덱스 사용
		int32 OriginalRingIndex = INDEX_NONE;

		FTightnessDispatchParams Params;
		TArray<uint32> Indices;
		TArray<float> Influences;

		// ===== UV Seam Welding용 대표 버텍스 인덱스 =====
		// UV seam에서 분리된 버텍스들(같은 위치, 다른 인덱스)이 동일하게 움직이도록 보장
		// 셰이더에서: RepresentativeIndices[ThreadIndex]의 위치를 읽어서 변형 계산
		// 같은 Position Group의 버텍스들은 동일한 Representative를 공유
		TArray<uint32> RepresentativeIndices;
		bool bHasUVDuplicates = false;  // UV duplicate 존재 여부 (없으면 UVSync 스킵 가능)

		// SDF 캐시 데이터 (렌더 스레드로 안전하게 전달)
		TRefCountPtr<IPooledRenderTarget> SDFPooledTexture;
		FVector3f SDFBoundsMin = FVector3f::ZeroVector;
		FVector3f SDFBoundsMax = FVector3f::ZeroVector;
		bool bHasValidSDF = false;

		/**
		 * SDF 로컬 → 컴포넌트 스페이스 변환 (OBB 지원)
		 * SDF는 로컬 스페이스에서 생성되므로, 셰이더에서 버텍스를
		 * 컴포넌트 → 로컬로 역변환해야 올바른 SDF 샘플링 가능
		 */
		FTransform SDFLocalToComponent = FTransform::Identity;

		/**
		 * Ring Center/Axis (SDF Local Space)
		 * 원본 Ring 메시 바운드 기준으로 계산 (확장 전 바운드)
		 * SDF 바운드가 확장되어도 Ring의 실제 위치/축을 정확히 전달
		 * CPU에서 계산하여 GPU 셰이더로 전달 (바운드 기반 추론 대체)
		 */
		FVector3f SDFLocalRingCenter = FVector3f::ZeroVector;
		FVector3f SDFLocalRingAxis = FVector3f(0.0f, 0.0f, 1.0f);

		// ===== Ring별 Bulge 데이터 =====
		bool bEnableBulge = false;
		TArray<uint32> BulgeIndices;
		TArray<float> BulgeInfluences;
		float BulgeStrength = 1.0f;
		float MaxBulgeDistance = 10.0f;
		float BulgeRadialRatio = 0.7f;	// Radial vs Axial 방향 비율 (0.0~1.0)

		// ===== Asymmetric Bulge (스타킹/타이즈 효과용) =====
		float UpperBulgeStrength = 1.0f;	// 상단(축 양수) Bulge 강도 배수
		float LowerBulgeStrength = 1.0f;	// 하단(축 음수) Bulge 강도 배수

		// ===== Bulge 방향 데이터 =====
		/**
		 * Bulge 방향 (-1, 0, +1)
		 * EBulgeDirectionMode에서 결정됨:
		 * - Auto 모드: DetectedBulgeDirection 사용
		 * - Positive: +1
		 * - Negative: -1
		 */
		int32 BulgeAxisDirection = 0;

		/** Auto 감지된 방향 (GenerateSDF에서 계산됨) */
		int32 DetectedBulgeDirection = 0;

		// ===== Normal Recomputation용 인접 데이터 =====
		// AdjacencyOffsets[i] = AffectedVertex i의 인접 삼각형 시작 인덱스
		// AdjacencyOffsets[NumAffected] = 총 인접 삼각형 수 (sentinel)
		TArray<uint32> AdjacencyOffsets;
		// 인접 삼각형 인덱스의 평탄화된 리스트
		TArray<uint32> AdjacencyTriangles;

		// ===== Laplacian Smoothing용 인접 데이터 =====
		// Packed format: [NeighborCount, N0, N1, ..., N11] per affected vertex (13 uints each)
		// 패킹 포맷: 영향받는 버텍스당 [이웃수, N0, N1, ..., N11] (각 13 uint)
		TArray<uint32> LaplacianAdjacencyData;

		// ===== Laplacian Smoothing용 DeformAmounts =====
		// Per-vertex deform amount: negative=tightness(inward), positive=bulge(outward)
		// Used to reduce smoothing on bulge areas to preserve bulge effect
		TArray<float> DeformAmounts;

		// ===== Laplacian/Taubin Smoothing 파라미터 =====
		bool bEnableLaplacianSmoothing = true;
		bool bUseTaubinSmoothing = true;      // Taubin: 수축 없는 스무딩
		float SmoothingLambda = 0.5f;         // λ (수축 강도)
		float TaubinMu = -0.53f;              // μ (팽창 강도, 음수)
		int32 SmoothingIterations = 2;

		// ===== Anchor Mode (Laplacian) =====
		// true: 원본 Affected Vertices는 앵커로 고정, 확장 영역만 스무딩
		bool bAnchorDeformedVertices = false;

		// ===== 스무딩 볼륨 모드 파라미터 =====
		bool bUseHopBasedSmoothing = false;   // true = HopBased, false = BoundsExpand
		TArray<float> HopBasedInfluences;     // (legacy) 홉 거리 기반 influence

		// ===== 확장된 스무딩 영역 (홉 기반) =====
		// Seeds(Affected) + N-hop 도달 버텍스로 구성
		// SmoothingVolumeMode == HopBased일 때 LaplacianCS가 이 영역 사용
		TArray<uint32> ExtendedSmoothingIndices;     // 확장 영역 버텍스 인덱스
		TArray<float> ExtendedInfluences;            // 확장 영역 influence (홉 falloff)
		TArray<uint32> ExtendedIsAnchor;             // 확장 영역 앵커 플래그 (1=Seed, 0=확장)
		TArray<uint32> ExtendedLaplacianAdjacency;   // 확장 영역 인접 데이터
		TArray<uint32> ExtendedRepresentativeIndices;  // 확장 영역 UV seam 대표 버텍스 인덱스
		bool bExtendedHasUVDuplicates = false;  // 확장 영역 UV duplicate 존재 여부
		TArray<uint32> ExtendedAdjacencyOffsets;    // 확장 영역 노멀 재계산용 인접 오프셋
		TArray<uint32> ExtendedAdjacencyTriangles;  // 확장 영역 노멀 재계산용 인접 삼각형
		TArray<uint32> ExtendedPBDAdjacencyWithRestLengths;  // 확장 영역 PBD 인접 데이터 (HopBased 모드)

		// ===== Heat Propagation (변형 전파) =====
		// Seed의 delta를 Extended 영역으로 확산
		// BoneRatioCS 이후, LaplacianCS 이전에 실행
		bool bEnableHeatPropagation = false;
		int32 HeatPropagationIterations = 10;
		float HeatPropagationLambda = 0.5f;
		bool bIncludeBulgeVerticesAsSeeds = true;  // Bulge 버텍스도 Seed로 포함

		// ===== Bone Ratio Preserve용 슬라이스 데이터 =====
		// 반경 균일화 스무딩 활성화 여부
		bool bEnableRadialSmoothing = true;
		// 반경 균일화 강도 (0.0 = 효과 없음, 1.0 = 완전 균일화)
		float RadialBlendStrength = 1.0f;
		// 반경 균일화 슬라이스 높이 (cm)
		float RadialSliceHeight = 1.0f;
		// 원본 본 거리 (바인드 포즈)
		TArray<float> OriginalBoneDistances;
		// 축 높이 (가우시안 가중치용)
		TArray<float> AxisHeights;
		// Packed format: [SliceCount, V0, V1, ..., V31] per affected vertex (33 uints each)
		// 패킹 포맷: 영향받는 버텍스당 [슬라이스버텍스수, V0, V1, ..., V31] (각 33 uint)
		TArray<uint32> SlicePackedData;

		// ===== Self-Collision Detection용 삼각형 데이터 =====
		// SDF 영역 내의 삼각형 인덱스 (3 uints per triangle)
		// 스타킹-살 관통 방지용
		TArray<uint32> CollisionTriangleIndices;

		// ===== Layer Penetration Resolution용 레이어 타입 =====
		// Per-affected-vertex layer types (0=Skin, 1=Stocking, etc.)
		// 머티리얼 이름에서 자동 감지됨
		TArray<uint32> LayerTypes;

		// ===== 전체 메시 레이어 타입 (GPU 직접 업로드용) =====
		// Full mesh vertex layer types - index by VertexIndex directly
		// 전체 메시 버텍스 레이어 타입 - VertexIndex로 직접 조회 가능
		// 축소(PostProcessingLayerTypes) → 확대(FullVertexLayerTypes) 변환 불필요
		TArray<uint32> FullMeshLayerTypes;

		// ===== Z 확장 후처리 버텍스 데이터 =====
		// [설계]
		// - Indices/Influences = 원본 SDF AABB → Tightness 변형 대상
		// - PostProcessing* = 원본 AABB + BoundsZTop/Bottom → 스무딩/침투해결 등
		// 경계에서 날카로운 크랙 방지를 위해 후처리 패스는 확장된 범위에서 수행
		// Note: PostProcessingLayerTypes는 FullMeshLayerTypes로 대체됨 (deprecated/removed)
		TArray<uint32> PostProcessingIndices;
		TArray<float> PostProcessingInfluences;
		TArray<uint32> PostProcessingIsAnchor;  // 앵커 플래그 (1=원본Affected, 0=확장영역)
		TArray<uint32> PostProcessingRepresentativeIndices;  // 후처리 버텍스용 UV seam 대표 인덱스
		bool bPostProcessingHasUVDuplicates = false;  // 후처리 영역 UV duplicate 존재 여부
		TArray<uint32> PostProcessingLaplacianAdjacencyData;  // 후처리 버텍스용 라플라시안 인접 데이터
		TArray<uint32> PostProcessingPBDAdjacencyWithRestLengths;  // 후처리 버텍스용 PBD 인접 데이터
		TArray<uint32> PostProcessingAdjacencyOffsets;    // 후처리 버텍스용 노멀 인접 오프셋
		TArray<uint32> PostProcessingAdjacencyTriangles;  // 후처리 버텍스용 노멀 인접 삼각형

		// ===== Skin SDF 기반 레이어 분리용 데이터 =====
		// 스킨 버텍스 인덱스 (PostProcessing 범위 내, LayerType=Skin)
		TArray<uint32> SkinVertexIndices;
		// 스킨 버텍스 노멀 (방사 방향으로 계산)
		TArray<float> SkinVertexNormals;
		// 스타킹 버텍스 인덱스 (PostProcessing 범위 내, LayerType=Stocking)
		TArray<uint32> StockingVertexIndices;

		// ===== PBD Edge Constraint용 데이터 (Tolerance 기반 변형 전파) =====
		bool bEnablePBDEdgeConstraint = false;
		float PBDStiffness = 0.8f;
		int32 PBDIterations = 5;
		float PBDTolerance = 0.2f;  // 허용 오차 비율 (0.2 = 80%~120% 허용)
		bool bPBDAnchorAffectedVertices = true;  // true: Affected Vertices 고정, false: 모든 버텍스 자유

		// PBD용 인접 데이터 (rest length 포함)
		// Packed format: [NeighborCount, Neighbor0, RestLen0(as uint), Neighbor1, RestLen1, ...] per affected vertex
		// RestLength는 float를 uint로 bit-cast하여 저장
		TArray<uint32> PBDAdjacencyWithRestLengths;

		// 전체 버텍스에 대한 Influence 맵 (이웃 가중치 조회용)
		// 인덱스: 전체 버텍스 인덱스, 값: influence
		TArray<float> FullInfluenceMap;

		// 전체 버텍스에 대한 DeformAmount 맵 (이웃 가중치 조회용)
		// 인덱스: 전체 버텍스 인덱스, 값: deform amount
		TArray<float> FullDeformAmountMap;

		// 전체 버텍스에 대한 IsAnchor 맵 (Tolerance 기반 PBD용)
		// 인덱스: 전체 버텍스 인덱스, 값: 1=Affected/앵커, 0=Non-Affected/자유
		// 이웃의 앵커 여부를 조회하여 PBD 가중치 분배 결정
		TArray<uint32> FullIsAnchorMap;
	};
	TSharedPtr<TArray<FRingDispatchData>> RingDispatchDataPtr;

	// ===== Bulge 전역 플래그 =====
	// 하나 이상의 Ring에서 Bulge가 활성화되어 있는지 여부
	// (VolumeAccumBuffer 생성 여부 결정용)
	bool bAnyRingHasBulge = false;

	// ===== Layer Penetration Resolution 플래그 =====
	// 레이어 침투 해결 활성화 여부 (FleshRingAsset에서 설정)
	bool bEnableLayerPenetrationResolution = true;

	// ===== Normal/Tangent Recompute 플래그 =====
	// 노멀 재계산 활성화 여부 (FleshRingAsset에서 설정)
	bool bEnableNormalRecompute = true;
	// 노멀 재계산 모드 (ENormalRecomputeMethod와 일치)
	// 0 = Geometric, 1 = SurfaceRotation, 2 = PolarDecomposition (DEPRECATED)
	uint32 NormalRecomputeMode = 1;  // Default: SurfaceRotation
	// 탄젠트 재계산 활성화 여부 (FleshRingAsset에서 설정, 노멀 재계산이 켜져있어야 동작)
	bool bEnableTangentRecompute = true;
	// 탄젠트 재계산 모드 (ETangentRecomputeMethod와 일치)
	// 0 = GramSchmidt, 1 = PolarDecomposition (DEPRECATED)
	uint32 TangentRecomputeMode = 0;  // Default: GramSchmidt

	// ===== Normal Recomputation용 메시 인덱스 버퍼 =====
	// 모든 Ring이 공유하는 메시 인덱스 버퍼 (3 indices per triangle)
	TSharedPtr<TArray<uint32>> MeshIndicesPtr;

	// 캐싱 상태
	bool bNeedTightnessCaching = false;
	bool bInvalidatePreviousPosition = false;

	// 캐시 버퍼 (렌더 스레드에서 접근)
	// TSharedPtr로 래핑하여 DeformerInstance 파괴 후에도 안전하게 접근 가능
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedBufferSharedPtr;

	// 재계산된 노멀 캐시 버퍼 (TightenedBindPose와 함께 캐싱)
	// NormalRecomputeCS 결과를 캐싱하여 캐싱된 프레임에서도 올바른 노멀 사용
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedNormalsBufferSharedPtr;

	// 재계산된 탄젠트 캐시 버퍼 (TightenedBindPose와 함께 캐싱)
	// TangentRecomputeCS 결과를 캐싱하여 캐싱된 프레임에서도 올바른 탄젠트 사용
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedTangentsBufferSharedPtr;

	// ===== 디버그 Influence 캐시 버퍼 =====
	// TightnessCS에서 출력된 Influence 값 캐싱
	// DrawDebugPoint에서 GPU 계산된 Influence 시각화용
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedDebugInfluencesBufferSharedPtr;

	// 디버그 Influence 출력 활성화 플래그
	bool bOutputDebugInfluences = false;

	// ===== 디버그 포인트 버퍼 (GPU 렌더링) =====
	// TightnessCS에서 출력된 디버그 포인트 (WorldPosition + Influence)
	// SceneViewExtension에서 직접 GPU 렌더링 (CPU Readback 불필요)
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedDebugPointBufferSharedPtr;

	// 디버그 포인트 출력 활성화 플래그
	bool bOutputDebugPoints = false;

	// LocalToWorld 행렬 (디버그 포인트 월드 변환용)
	FMatrix44f LocalToWorldMatrix = FMatrix44f::Identity;

	// ViewExtension 참조 (렌더 스레드에서 직접 버퍼 전달용)
	TSharedPtr<FFleshRingDebugViewExtension, ESPMode::ThreadSafe> DebugViewExtension;

	// 디버그 포인트 수 (ViewExtension에 전달)
	uint32 DebugPointCount = 0;

	// ===== Bulge 디버그 포인트 버퍼 (GPU 렌더링) =====
	// BulgeCS에서 출력된 디버그 포인트 (WorldPosition + Influence)
	// SceneViewExtension에서 직접 GPU 렌더링 (Tightness와 다른 색상)
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedDebugBulgePointBufferSharedPtr;

	// Bulge 디버그 포인트 출력 활성화 플래그
	bool bOutputDebugBulgePoints = false;

	// Bulge 디버그 포인트 수 (ViewExtension에 전달)
	uint32 DebugBulgePointCount = 0;

	// ===== GPU Readback 관련 =====
	// Readback 결과를 저장할 배열 (게임 스레드에서 접근)
	TSharedPtr<TArray<float>> DebugInfluenceReadbackResultPtr;

	// Readback 완료 플래그 (스레드 안전)
	TSharedPtr<std::atomic<bool>> bDebugInfluenceReadbackComplete;

	// Readback할 버텍스 수
	uint32 DebugInfluenceCount = 0;

	// Fallback 델리게이트
	FSimpleDelegate FallbackDelegate;
};

// ============================================================================
// FFleshRingComputeWorker - IComputeTaskWorker 구현
// ============================================================================
// 렌더러가 적절한 시점에 호출하여 FleshRing 작업 실행
class FLESHRINGRUNTIME_API FFleshRingComputeWorker : public IComputeTaskWorker
{
public:
	FFleshRingComputeWorker(FSceneInterface const* InScene);
	virtual ~FFleshRingComputeWorker();

	// IComputeTaskWorker interface
	virtual bool HasWork(FName InExecutionGroupName) const override;
	virtual void SubmitWork(FComputeContext& Context) override;

	// 작업 큐잉 (렌더 스레드에서 호출)
	void EnqueueWork(FFleshRingWorkItem&& InWorkItem);

	// 작업 취소 (특정 DeformerInstance의 작업 제거)
	void AbortWork(UFleshRingDeformerInstance* InDeformerInstance);

private:
	// 실제 작업 실행
	void ExecuteWorkItem(FRDGBuilder& GraphBuilder, FFleshRingWorkItem& WorkItem);
	
	FSceneInterface const* Scene;

	// 대기 중인 작업 목록 (렌더 스레드 전용)
	TArray<FFleshRingWorkItem> PendingWorkItems;

	// 스레드 안전성을 위한 락
	mutable FCriticalSection WorkItemsLock;
};

// ============================================================================
// FFleshRingComputeSystem - IComputeSystem 구현
// ============================================================================
// Scene별 FleshRingComputeWorker 생성/관리
class FLESHRINGRUNTIME_API FFleshRingComputeSystem : public IComputeSystem
{
public:
	static FFleshRingComputeSystem& Get();

	// IComputeSystem interface
	virtual void CreateWorkers(FSceneInterface const* InScene, TArray<IComputeTaskWorker*>& OutWorkers) override;
	virtual void DestroyWorkers(FSceneInterface const* InScene, TArray<IComputeTaskWorker*>& InOutWorkers) override;

	// Scene에 대한 Worker 조회
	FFleshRingComputeWorker* GetWorker(FSceneInterface const* InScene) const;

	// 시스템 등록/해제
	static void Register();
	static void Unregister();

private:
	FFleshRingComputeSystem() = default;

	// Scene별 Worker 매핑
	TMap<FSceneInterface const*, FFleshRingComputeWorker*> SceneWorkers;
	mutable FCriticalSection WorkersLock;

	static FFleshRingComputeSystem* Instance;
	static bool bIsRegistered;
};
