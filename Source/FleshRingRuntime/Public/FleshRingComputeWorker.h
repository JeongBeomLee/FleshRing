// Copyright 2026 LgThx. All Rights Reserved.

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
#include "FleshRingLayerPenetrationShader.h"
#include "FleshRingPBDEdgeShader.h"
#include "FleshRingSkinSDFShader.h"
#include "FleshRingHeatPropagationShader.h"

class FSkeletalMeshObject;
class UFleshRingDeformerInstance;
struct IPooledRenderTarget;

// ============================================================================
// FFleshRingWorkItem - Queued work item
// ============================================================================
struct FFleshRingWorkItem
{
	// Work identification
	TWeakObjectPtr<UFleshRingDeformerInstance> DeformerInstance;
	FSkeletalMeshObject* MeshObject = nullptr;
	int32 LODIndex = 0;

	// Vertex data
	uint32 TotalVertexCount = 0;
	TSharedPtr<TArray<float>> SourceDataPtr;

	// Ring data (for Tightness + Bulge)
	struct FRingDispatchData
	{
		// Original Ring index (index in FleshRingAsset->Rings array)
		// Ensures correct index lookup even when Rings without vertices are skipped
		int32 OriginalRingIndex = INDEX_NONE;

		FTightnessDispatchParams Params;
		TArray<uint32> Indices;
		TArray<float> Influences;

		// ===== Representative vertex indices for UV Seam Welding =====
		// Ensures vertices split at UV seams (same position, different indices) move identically
		// In shader: reads position from RepresentativeIndices[ThreadIndex] for deformation calculation
		// Vertices in the same Position Group share the same Representative
		TArray<uint32> RepresentativeIndices;
		bool bHasUVDuplicates = false;  // Whether UV duplicates exist (UVSync can be skipped if false)

		// ===== Cached GPU Buffers (static data, created once on first frame and reused) =====
		// No need to upload every frame unless topology changes
		mutable TRefCountPtr<FRDGPooledBuffer> CachedRepresentativeIndicesBuffer;

		// SDF cache data (safely passed to render thread)
		TRefCountPtr<IPooledRenderTarget> SDFPooledTexture;
		FVector3f SDFBoundsMin = FVector3f::ZeroVector;
		FVector3f SDFBoundsMax = FVector3f::ZeroVector;
		bool bHasValidSDF = false;

		/**
		 * SDF Local -> Component Space transform (OBB support)
		 * Since SDF is generated in local space, shader must inverse transform vertices
		 * from Component -> Local for correct SDF sampling
		 */
		FTransform SDFLocalToComponent = FTransform::Identity;

		/**
		 * Ring Center/Axis (SDF Local Space)
		 * Calculated based on original Ring mesh bounds (before expansion)
		 * Accurately conveys Ring's actual position/axis even when SDF bounds are expanded
		 * Calculated on CPU and passed to GPU shader (replaces bounds-based inference)
		 */
		FVector3f SDFLocalRingCenter = FVector3f::ZeroVector;
		FVector3f SDFLocalRingAxis = FVector3f(0.0f, 0.0f, 1.0f);

		// ===== Per-Ring Bulge data =====
		bool bEnableBulge = false;
		TArray<uint32> BulgeIndices;
		TArray<float> BulgeInfluences;
		float BulgeStrength = 1.0f;
		float MaxBulgeDistance = 10.0f;
		float BulgeRadialRatio = 0.7f;	// Radial vs Axial direction ratio (0.0~1.0)

		// ===== Asymmetric Bulge (for stocking/tights effect) =====
		float UpperBulgeStrength = 1.0f;	// Upper (positive axis) Bulge strength multiplier
		float LowerBulgeStrength = 1.0f;	// Lower (negative axis) Bulge strength multiplier

		// ===== Bulge direction data =====
		/**
		 * Bulge direction (-1, 0, +1)
		 * Determined by EBulgeDirectionMode:
		 * - Auto mode: uses DetectedBulgeDirection
		 * - Positive: +1
		 * - Negative: -1
		 */
		int32 BulgeAxisDirection = 0;

		/** Auto-detected direction (calculated in GenerateSDF) */
		int32 DetectedBulgeDirection = 0;

		// ===== Adjacency data for Normal Recomputation =====
		// AdjacencyOffsets[i] = start index of adjacent triangles for AffectedVertex i
		// AdjacencyOffsets[NumAffected] = total adjacent triangle count (sentinel)
		TArray<uint32> AdjacencyOffsets;
		// Flattened list of adjacent triangle indices
		TArray<uint32> AdjacencyTriangles;

		// ===== Adjacency data for Laplacian Smoothing =====
		// Packed format: [NeighborCount, N0, N1, ..., N11] per affected vertex (13 uints each)
		TArray<uint32> LaplacianAdjacencyData;

		// ===== DeformAmounts for Laplacian Smoothing =====
		// Per-vertex deform amount: negative=tightness(inward), positive=bulge(outward)
		// Used to reduce smoothing on bulge areas to preserve bulge effect
		TArray<float> DeformAmounts;

		// ===== Laplacian/Taubin Smoothing parameters =====
		bool bEnableLaplacianSmoothing = true;
		bool bUseTaubinSmoothing = true;      // Taubin: shrink-free smoothing
		float SmoothingLambda = 0.5f;         // λ (shrink strength)
		float TaubinMu = -0.53f;              // μ (inflate strength, negative)
		int32 SmoothingIterations = 2;

		// ===== Anchor Mode (Laplacian) =====
		// true: original Affected Vertices are anchored, only extended region is smoothed
		bool bAnchorDeformedVertices = false;

		// ===== Unified smoothing region data =====
		// Consolidates former Refinement~ (BoundsExpand) and Extended~ (HopBased) variables
		// Filled with appropriate data from DeformerInstance based on SmoothingExpandMode
		ESmoothingVolumeMode SmoothingExpandMode = ESmoothingVolumeMode::BoundsExpand;
		TArray<uint32> SmoothingRegionIndices;           // Smoothing region vertex indices
		TArray<float> SmoothingRegionInfluences;         // Smoothing region influence (with falloff)
		TArray<uint32> SmoothingRegionIsAnchor;          // Anchor flags (1=Seed/Core, 0=extended)
		TArray<uint32> SmoothingRegionRepresentativeIndices;  // UV seam representative vertex indices
		bool bSmoothingRegionHasUVDuplicates = false;    // Whether UV duplicates exist
		mutable TRefCountPtr<FRDGPooledBuffer> CachedSmoothingRegionRepresentativeIndicesBuffer;  // Cached GPU buffer
		TArray<uint32> SmoothingRegionLaplacianAdjacency;  // Laplacian adjacency data
		TArray<uint32> SmoothingRegionPBDAdjacency;      // PBD adjacency data
		TArray<uint32> SmoothingRegionAdjacencyOffsets;  // Adjacency offsets for normal recomputation
		TArray<uint32> SmoothingRegionAdjacencyTriangles;  // Adjacency triangles for normal recomputation
		TArray<int32> SmoothingRegionHopDistances;       // Hop distances (HopBased only)
		int32 MaxSmoothingHops = 0;                      // Max hop distance (for blend coefficient calculation)
		uint32 NormalBlendFalloffType = 2;               // Normal blending falloff (0=Linear, 1=Quadratic, 2=Hermite)

		// Hop distance based influence (for Affected region)
		TArray<float> HopBasedInfluences;

		// ===== Heat Propagation (deformation propagation) =====
		// Propagates delta from Seeds to SmoothingRegion area
		// Runs after BoneRatioCS, before LaplacianCS
		bool bEnableHeatPropagation = false;
		int32 HeatPropagationIterations = 10;
		float HeatPropagationLambda = 0.5f;
		bool bIncludeBulgeVerticesAsSeeds = true;  // Include Bulge vertices as Seeds

		// ===== Slice data for Bone Ratio Preserve =====
		// Enable radial uniformization smoothing
		bool bEnableRadialSmoothing = true;
		// Radial uniformization strength (0.0 = no effect, 1.0 = full uniformization)
		float RadialBlendStrength = 1.0f;
		// Radial uniformization slice height (cm)
		float RadialSliceHeight = 1.0f;
		// Original bone distances (bind pose)
		TArray<float> OriginalBoneDistances;
		// Axis heights (for Gaussian weighting)
		TArray<float> AxisHeights;
		// Packed format: [SliceCount, V0, V1, ..., V31] per affected vertex (33 uints each)
		TArray<uint32> SlicePackedData;

		// ===== Layer types for Layer Penetration Resolution =====
		// Per-affected-vertex layer types (0=Skin, 1=Stocking, etc.)
		// Auto-detected from material names
		TArray<uint32> LayerTypes;

		// ===== Full mesh layer types (for direct GPU upload) =====
		// Full mesh vertex layer types - index by VertexIndex directly
		// No need to expand from reduced (RefinementLayerTypes) to full (FullVertexLayerTypes)
		TArray<uint32> FullMeshLayerTypes;

		// Note: Refinement~ variables are consolidated into SmoothingRegion~ (see above)

		// ===== Data for Skin SDF based layer separation =====
		// Skin vertex indices (within SmoothingRegion, LayerType=Skin)
		TArray<uint32> SkinVertexIndices;
		// Skin vertex normals (calculated as radial direction)
		TArray<float> SkinVertexNormals;
		// Stocking vertex indices (within SmoothingRegion, LayerType=Stocking)
		TArray<uint32> StockingVertexIndices;

		// ===== Data for PBD Edge Constraint (Tolerance-based deformation propagation) =====
		bool bEnablePBDEdgeConstraint = false;
		float PBDStiffness = 0.8f;
		int32 PBDIterations = 5;
		float PBDTolerance = 0.2f;  // Tolerance ratio (0.2 = allow 80%~120%)
		bool bPBDAnchorAffectedVertices = true;  // true: Affected Vertices fixed, false: all vertices free

		// PBD adjacency data (includes rest length)
		// Packed format: [NeighborCount, Neighbor0, RestLen0(as uint), Neighbor1, RestLen1, ...] per affected vertex
		// RestLength is stored as float bit-cast to uint
		TArray<uint32> PBDAdjacencyWithRestLengths;

		// Influence map for all vertices (for neighbor weight lookup)
		// Index: full vertex index, Value: influence
		TArray<float> FullInfluenceMap;

		// DeformAmount map for all vertices (for neighbor weight lookup)
		// Index: full vertex index, Value: deform amount
		TArray<float> FullDeformAmountMap;

		// IsAnchor map for all vertices (for Tolerance-based PBD)
		// Index: full vertex index, Value: 1=Affected/anchor, 0=Non-Affected/free
		// Used to query neighbor's anchor status for PBD weight distribution
		TArray<uint32> FullVertexAnchorFlags;

		// ===== Cached Zero arrays (used when bPBDAnchorAffectedVertices=false) =====
		// Pre-created Zero-filled arrays to avoid per-tick allocation
		TArray<uint32> CachedZeroIsAnchorFlags;   // Size of PBD target vertex count
		TArray<uint32> CachedZeroFullVertexAnchorFlags;   // Size of total vertex count
	};
	TSharedPtr<TArray<FRingDispatchData>> RingDispatchDataPtr;

	// ===== Bulge global flag =====
	// Whether Bulge is enabled on one or more Rings
	// (Used to determine whether to create VolumeAccumBuffer)
	bool bAnyRingHasBulge = false;

	// ===== Layer Penetration Resolution flag =====
	// Whether layer penetration resolution is enabled (set from FleshRingAsset)
	bool bEnableLayerPenetrationResolution = true;

	// ===== Normal/Tangent Recompute flags =====
	// Whether normal recomputation is enabled (set from FleshRingAsset)
	bool bEnableNormalRecompute = true;
	// Normal recomputation mode (matches ENormalRecomputeMethod)
	// 0 = Geometric, 1 = SurfaceRotation
	uint32 NormalRecomputeMode = 1;  // Default: SurfaceRotation
	// Whether hop-based blending is enabled (blends with original normals at boundary)
	bool bEnableNormalHopBlending = true;
	// Whether displacement-based blending is enabled (blends based on vertex movement)
	bool bEnableDisplacementBlending = false;
	// Max displacement distance for blending (cm) - uses 100% recomputed normal beyond this distance
	float MaxDisplacementForBlend = 1.0f;
	// Whether tangent recomputation is enabled (set from FleshRingAsset, requires normal recompute on)
	bool bEnableTangentRecompute = true;

	// ===== Mesh index buffer for Normal Recomputation =====
	// Mesh index buffer shared by all Rings (3 indices per triangle)
	TSharedPtr<TArray<uint32>> MeshIndicesPtr;

	// Caching state
	bool bNeedTightnessCaching = false;
	bool bInvalidatePreviousPosition = false;

	// Cache buffer (accessed from render thread)
	// Wrapped in TSharedPtr for safe access after DeformerInstance destruction
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedBufferSharedPtr;

	// Recomputed normals cache buffer (cached together with TightenedBindPose)
	// Caches NormalRecomputeCS results to use correct normals on cached frames
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedNormalsBufferSharedPtr;

	// Recomputed tangents cache buffer (cached together with TightenedBindPose)
	// Caches TangentRecomputeCS results to use correct tangents on cached frames
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedTangentsBufferSharedPtr;

	// ===== Debug Influence cache buffer =====
	// Caches Influence values output from TightnessCS
	// For visualizing GPU-computed Influence in DrawDebugPoint
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedDebugInfluencesBufferSharedPtr;

	// Debug Influence output enable flag
	bool bOutputDebugInfluences = false;

	// ===== Debug point buffer (GPU rendering) =====
	// Debug points output from TightnessCS (WorldPosition + Influence)
	// Direct GPU rendering in Scene Proxy (no CPU Readback needed)
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedDebugPointBufferSharedPtr;

	// Debug point output enable flag
	bool bOutputDebugPoints = false;

	// LocalToWorld matrix (for debug point world transform)
	FMatrix44f LocalToWorldMatrix = FMatrix44f::Identity;

	// ===== Bulge debug point buffer (GPU rendering) =====
	// Debug points output from BulgeCS (WorldPosition + Influence)
	// Direct GPU rendering in Scene Proxy (different color from Tightness)
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> CachedDebugBulgePointBufferSharedPtr;

	// Bulge debug point output enable flag
	bool bOutputDebugBulgePoints = false;

	// Bulge debug point count
	uint32 DebugBulgePointCount = 0;

	// ===== GPU Readback related =====
	// Array to store Readback results (accessed from game thread)
	TSharedPtr<TArray<float>> DebugInfluenceReadbackResultPtr;

	// Readback completion flag (thread safe)
	TSharedPtr<std::atomic<bool>> bDebugInfluenceReadbackComplete;

	// Number of vertices to Readback
	uint32 DebugInfluenceCount = 0;

	// Fallback delegate
	FSimpleDelegate FallbackDelegate;

	// ===== Passthrough Mode =====
	// Runs SkinningCS with original data once when AffectedVertices becomes 0
	// Skips TightnessCS and outputs original tangents to remove previous deformation residue
	bool bPassthroughMode = false;
};

// ============================================================================
// FFleshRingComputeWorker - IComputeTaskWorker implementation
// ============================================================================
// Called by renderer at appropriate timing to execute FleshRing work
class FLESHRINGRUNTIME_API FFleshRingComputeWorker : public IComputeTaskWorker
{
public:
	FFleshRingComputeWorker(FSceneInterface const* InScene);
	virtual ~FFleshRingComputeWorker();

	// IComputeTaskWorker interface
	virtual bool HasWork(FName InExecutionGroupName) const override;
	virtual void SubmitWork(FComputeContext& Context) override;

	// Queue work (called from render thread)
	void EnqueueWork(FFleshRingWorkItem&& InWorkItem);

	// Cancel work (remove work for specific DeformerInstance)
	void AbortWork(UFleshRingDeformerInstance* InDeformerInstance);

private:
	// Execute actual work
	void ExecuteWorkItem(FRDGBuilder& GraphBuilder, FFleshRingWorkItem& WorkItem);

	FSceneInterface const* Scene;

	// Pending work list (render thread only)
	TArray<FFleshRingWorkItem> PendingWorkItems;

	// Lock for thread safety
	mutable FCriticalSection WorkItemsLock;
};

// ============================================================================
// FFleshRingComputeSystem - IComputeSystem implementation
// ============================================================================
// Creates/manages FleshRingComputeWorker per Scene
class FLESHRINGRUNTIME_API FFleshRingComputeSystem : public IComputeSystem
{
public:
	static FFleshRingComputeSystem& Get();

	// IComputeSystem interface
	virtual void CreateWorkers(FSceneInterface const* InScene, TArray<IComputeTaskWorker*>& OutWorkers) override;
	virtual void DestroyWorkers(FSceneInterface const* InScene, TArray<IComputeTaskWorker*>& InOutWorkers) override;

	// Get Worker for Scene
	FFleshRingComputeWorker* GetWorker(FSceneInterface const* InScene) const;

	// System registration/unregistration
	static void Register();
	static void Unregister();

private:
	FFleshRingComputeSystem() = default;

	// Per-Scene Worker mapping
	TMap<FSceneInterface const*, FFleshRingComputeWorker*> SceneWorkers;
	mutable FCriticalSection WorkersLock;

	static FFleshRingComputeSystem* Instance;
	static bool bIsRegistered;
};
