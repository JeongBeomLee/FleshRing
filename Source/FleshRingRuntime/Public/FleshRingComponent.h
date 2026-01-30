// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "FleshRingTypes.h"
#include "FleshRingDeformer.h"
#include "FleshRingAffectedVertices.h"
#include "FleshRingDebugPointComponent.h"
#include "FleshRingModularTypes.h"
#include "RenderGraphResources.h"
#include "FleshRingComponent.generated.h"

class UStaticMesh;
class UVolumeTexture;
class UFleshRingAsset;
class UFleshRingMeshComponent;
struct IPooledRenderTarget;

// =====================================
// SDF Cache Struct (Persistent per-Ring storage)
// =====================================

/**
 * Per-Ring SDF texture cache
 * Converts RDG texture to Pooled texture for persistent storage
 * Used by Deformer via RegisterExternalTexture() every frame
 */
struct FRingSDFCache
{
	/**
	 * Pooled render target (persistent on GPU)
	 *
	 * What is IPooledRenderTarget?
	 * - Interface for persistent render target storage outside RDG (Render Dependency Graph)
	 * - RDG textures are destroyed after GraphBuilder.Execute(),
	 *   but ConvertToExternalTexture() converts to Pooled texture for cross-frame persistence
	 * - In subsequent frames, RegisterExternalTexture() re-registers to RDG for use
	 * - TRefCountPtr manages reference count (release via SafeRelease())
	 */
	TRefCountPtr<IPooledRenderTarget> PooledTexture;

	/** SDF volume minimum bounds (Ring local space) */
	FVector3f BoundsMin = FVector3f::ZeroVector;

	/** SDF volume maximum bounds (Ring local space) */
	FVector3f BoundsMax = FVector3f::ZeroVector;

	/** SDF resolution */
	FIntVector Resolution = FIntVector(64, 64, 64);

	/**
	 * Ring local -> Component space transform (for OBB)
	 * SDF is generated in local space, inverse transform used for sampling
	 */
	FTransform LocalToComponent = FTransform::Identity;

	/**
	 * Auto-detected Bulge direction
	 * +1 = Upward (boundary vertex average Z > SDF center Z)
	 * -1 = Downward (boundary vertex average Z < SDF center Z)
	 *  0 = Detection failed (closed mesh or no vertices)
	 */
	int32 DetectedBulgeDirection = 0;

	/** Caching complete flag */
	bool bCached = false;

	/** Reset cache */
	void Reset()
	{
		PooledTexture.SafeRelease();
		BoundsMin = FVector3f::ZeroVector;
		BoundsMax = FVector3f::ZeroVector;
		Resolution = FIntVector(64, 64, 64);
		LocalToComponent = FTransform::Identity;
		DetectedBulgeDirection = 0;
		bCached = false;
	}

	/** Validity check */
	bool IsValid() const
	{
		return bCached && PooledTexture.IsValid();
	}
};

// =====================================
// Component Class
// =====================================

class UFleshRingDeformerInstance;

/**
 * FleshRing mesh deformation component
 * Handles flesh representation of skeletal mesh using SDF
 */
UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent), DisplayName="Flesh Ring")
class FLESHRINGRUNTIME_API UFleshRingComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFleshRingComponent();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void BeginDestroy() override;
	virtual void OnRegister() override;
	virtual void OnUnregister() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	/** Asset change delegate handle */
	FDelegateHandle AssetChangedDelegateHandle;

	/** Subscribe/unsubscribe to asset change delegate */
	void BindToAssetDelegate();
	void UnbindFromAssetDelegate();

	/** Asset change callback */
	void OnFleshRingAssetChanged(UFleshRingAsset* ChangedAsset);
#endif

public:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// =====================================
	// FleshRing Asset (Primary Data Source)
	// =====================================

	/** FleshRing data asset (contains Ring settings) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "FleshRing Asset")
	TObjectPtr<UFleshRingAsset> FleshRingAsset;

	/** Called when asset is changed (internal use) */
	void ApplyAsset();

	/**
	 * Swap FleshRingAsset at runtime
	 * Instant swap between baked assets without animation interruption
	 */
	/** Runtime ring asset swap (legacy API, maintained for backward compatibility) */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	void SwapFleshRingAsset(UFleshRingAsset* NewAsset);

	// ========================== Modular Internal ==========================
	// Use UFleshRingModularLibrary public API instead of calling directly.

	/**
	 * Internal: Swaps ring asset at runtime for modular characters.
	 * @see UFleshRingModularLibrary::SwapModularRingAsset
	 */
	FFleshRingModularResult Internal_SwapModularRingAsset(UFleshRingAsset* NewAsset, bool bPreserveLeaderPose = true);

	/**
	 * Internal: Detaches ring asset and removes ring meshes.
	 * @see UFleshRingModularLibrary::SwapModularPartMesh
	 */
	void Internal_DetachModularRingAsset(bool bPreserveLeaderPose = true);

	/**
	 * Internal: Marks this component as created for Skeletal Merging system.
	 * @see UFleshRingModularLibrary::RebuildMergedMesh
	 */
	void Internal_SetCreatedForMergedMesh(bool bValue) { bCreatedForMergedMesh = bValue; }

	// ======================================================================

	/** True when using BakedMesh at runtime (Deformer disabled) */
	UFUNCTION(BlueprintPure, Category = "FleshRing")
	bool IsUsingBakedMesh() const { return bUsingBakedMesh; }

	// =====================================
	// Target Settings
	// =====================================

	/**
	 * Manual target mesh setting (for special cases like preview, merged mesh)
	 * If not set, auto-searches Owner for component matching FleshRingAsset->TargetSkeletalMesh
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	void SetTargetMesh(USkeletalMeshComponent* InTargetMesh);

	// =====================================
	// General (Runtime Settings)
	// =====================================

	/** Enable all features */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General")
	bool bEnableFleshRing = true;

	/** Show Ring mesh (SDF source mesh) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General")
	bool bShowRingMesh = true;

	/** Bounds scale multiplier (adjust for Deformer deformation to enable VSM caching) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "General", meta = (ClampMin = "1.0", ClampMax = "3.0"))
	float BoundsScale = 2.0f;

	// =====================================
	// Debug / Visualization (Editor Only)
	// =====================================

#if WITH_EDITORONLY_DATA
	/** Enable debug visualization (master switch) */
	UPROPERTY(EditAnywhere, Category = "Debug")
	bool bShowDebugVisualization = false;

	/** Show SDF volume bounding box */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowDebugVisualization"))
	bool bShowSdfVolume = false;

	/** Show affected vertices (color = influence strength) */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowDebugVisualization"))
	bool bShowAffectedVertices = false;

	/** Show SDF slice plane */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowDebugVisualization"))
	bool bShowSDFSlice = false;

	/** SDF slice Z index to display (0 ~ Resolution-1) */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowDebugVisualization && bShowSDFSlice", ClampMin = "0", ClampMax = "63", UIMin = "0", UIMax = "63"))
	int32 DebugSliceZ = 32;

	// TODO: Implement Bulge heatmap visualization
	/** Show Bulge heatmap */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowDebugVisualization"))
	bool bShowBulgeHeatmap = false;

	/** Show Bulge direction arrows */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowDebugVisualization && bShowBulgeHeatmap"))
	bool bShowBulgeArrows = true;

	/** Show Bulge influence range cylinder */
	UPROPERTY(EditAnywhere, Category = "Debug", meta = (EditCondition = "bShowDebugVisualization"))
	bool bShowBulgeRange = false;
#endif

	// =====================================
	// Blueprint Callable Functions
	// =====================================

	/** Manual SDF update */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	void UpdateSDF();

	/** Get resolved SkeletalMeshComponent to apply deformation */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	USkeletalMeshComponent* GetResolvedTargetMesh() const { return ResolvedTargetMesh.Get(); }

	/** Get internal Deformer */
	UFUNCTION(BlueprintCallable, Category = "FleshRing")
	UFleshRingDeformer* GetDeformer() const { return InternalDeformer; }

	/**
	 * Reinitialize Deformer (reallocate GPU buffers when mesh changes)
	 * Call when mesh is changed (e.g., baking) to reallocate Deformer's GPU buffers
	 * to match the new mesh size.
	 */
#if WITH_EDITOR
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Editor")
	void ReinitializeDeformer();
#endif

	/**
	 * Initialize Deformer for editor preview environment.
	 * Used in editor environment where BeginPlay() is not called.
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Editor")
	void InitializeForEditorPreview();

	/**
	 * Force reinitialize editor preview (even if already initialized)
	 * Used for Deformer setup when baking with subdivision OFF
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Editor")
	void ForceInitializeForEditorPreview();

	/**
	 * Update Ring transforms only (keep Deformer, keep SDF textures)
	 * For real-time update without flickering during gizmo drag or property changes
	 * @param DirtyRingIndex - Update specific Ring only (-1 for all)
	 */
	UFUNCTION(BlueprintCallable, Category = "FleshRing|Editor")
	void UpdateRingTransforms(int32 DirtyRingIndex = -1);

	/**
	 * Recreate Ring mesh components (call when RingMesh changes)
	 * Used when RingMesh property is changed in editor
	 */
	void RefreshRingMeshes();

	/** Show/hide debug slice planes */
	void SetDebugSlicePlanesVisible(bool bVisible);

	/** Get Ring mesh component array (for editor picking) */
	const TArray<TObjectPtr<UFleshRingMeshComponent>>& GetRingMeshComponents() const { return RingMeshComponents; }

	// =====================================
	// SDF Cache Access (Used by Deformer)
	// =====================================

	/** Get Ring count */
	int32 GetNumRingSDFCaches() const { return RingSDFCaches.Num(); }

	/** Get SDF cache for specific Ring (read-only) */
	const FRingSDFCache* GetRingSDFCache(int32 RingIndex) const
	{
		if (RingSDFCaches.IsValidIndex(RingIndex))
		{
			return &RingSDFCaches[RingIndex];
		}
		return nullptr;
	}

	/** Check if all Ring SDF caches are valid */
	bool AreAllSDFCachesValid() const
	{
		for (const FRingSDFCache& Cache : RingSDFCaches)
		{
			if (!Cache.IsValid())
			{
				return false;
			}
		}
		return RingSDFCaches.Num() > 0;
	}

	/** Check if at least one valid SDF cache exists (allows partial operation) */
	bool HasAnyValidSDFCaches() const
	{
		for (const FRingSDFCache& Cache : RingSDFCaches)
		{
			if (Cache.IsValid())
			{
				return true;
			}
		}
		return false;
	}

	/** Check if any Rings operate without SDF (VirtualRing/VirtualBand - distance-based logic) */
	bool HasAnyNonSDFRings() const;

	/** Regenerate SDF (for real-time VirtualBand updates in editor) */
	void RefreshSDF() { GenerateSDF(); }

private:
	/** Editor preview initialization complete flag */
	bool bEditorPreviewInitialized = false;

	/** True when using BakedMesh at runtime (Deformer disabled) */
	bool bUsingBakedMesh = false;

	/**
	 * True when using FleshRing with Skeletal Merging system.
	 * In this mode, BakedMesh is already merged into the character mesh,
	 * so only ring visuals are applied without mesh replacement.
	 * Automatically set by UFleshRingModularLibrary::RebuildMergedMesh().
	 */
	bool bCreatedForMergedMesh = false;

	/**
	 * Whether manually set via SetTargetMesh()
	 * If true, skip auto-search in FindTargetMeshOnly() (restore from ManualTargetMesh)
	 * If true, skip mesh change in ResolveTargetMesh()
	 */
	bool bManualTargetSet = false;

	/**
	 * Target mesh set via SetTargetMesh() (for caching)
	 * Can be restored even when ResolvedTargetMesh is reset in CleanupDeformer()
	 */
	TWeakObjectPtr<USkeletalMeshComponent> ManualTargetMesh;

	/** Auto/manually resolved actual target */
	UPROPERTY(Transient)
	TWeakObjectPtr<USkeletalMeshComponent> ResolvedTargetMesh;

	/** Original SkeletalMesh to restore when component is removed (saved before SubdividedMesh application) */
	UPROPERTY(Transient)
	TWeakObjectPtr<USkeletalMesh> CachedOriginalMesh;

	/** Internally created Deformer */
	UPROPERTY(Transient)
	TObjectPtr<UFleshRingDeformer> InternalDeformer;

	/**
	 * Per-Ring SDF cache array
	 * - Converted to Pooled texture and stored in GenerateSDF()
	 * - Accessed by Deformer via GetRingSDFCache()
	 * - Cannot be UPROPERTY (IPooledRenderTarget is not a UObject)
	 * - Must be manually released in CleanupDeformer()
	 */
	TArray<FRingSDFCache> RingSDFCaches;

	/**
	 * Per-Ring rendering StaticMeshComponent array
	 * - Created and attached to bone in SetupRingMeshes()
	 * - Removed in CleanupRingMeshes()
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UFleshRingMeshComponent>> RingMeshComponents;

	/**
	 * Per-Ring skinned SkeletalMeshComponent array (for runtime deformation)
	 * - Created when BakedSkinnedRingMeshes are available
	 * - Uses SetLeaderPoseComponent() to follow main character animation
	 * - Allows ring mesh to deform with twist bones like skin vertices
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<USkeletalMeshComponent>> SkinnedRingMeshComponents;

	/** Search for target SkeletalMeshComponent only (no mesh change) */
	void FindTargetMeshOnly();

	/** Search for target SkeletalMeshComponent and set mesh (SubdividedMesh application, etc.) */
	void ResolveTargetMesh();

	/** Create and register Deformer */
	void SetupDeformer();

	/** Remove Deformer */
	void CleanupDeformer();

	/**
	 * Refresh SDF and Ring meshes while keeping Deformer
	 * Use this instead of Deformer recreation to prevent GPU memory leaks during Undo/Redo
	 * @return true if refresh succeeded, false if full recreation needed
	 */
	bool RefreshWithDeformerReuse();

	/** Generate SDF (based on each Ring's RingMesh) */
	void GenerateSDF();

	/** Create Ring mesh components and attach to bone */
	void SetupRingMeshes();

	/** Remove Ring mesh components */
	void CleanupRingMeshes();

	/** Update Ring mesh visibility */
	void UpdateRingMeshVisibility();

	/** Apply baked mesh (BakedMesh + BakedRingTransforms) */
	void ApplyBakedMesh();

	/** Apply baked Ring transforms (restore Ring mesh positions) */
	void ApplyBakedRingTransforms();

	// =====================================
	// Debug Drawing (Editor Only)
	// =====================================

#if WITH_EDITORONLY_DATA
	/** SDF slice visualization plane actors (per-Ring) */
	UPROPERTY(Transient)
	TArray<TObjectPtr<AActor>> DebugSlicePlaneActors;

	/** SDF slice visualization render targets (per-Ring) */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextureRenderTarget2D>> DebugSliceRenderTargets;

	/**
	 * Affected vertex data for debug visualization (per-Ring)
	 * Calculated in CacheAffectedVerticesForDebug() after GenerateSDF() completes
	 */
	TArray<FRingAffectedData> DebugAffectedData;

	/** Bind pose vertex positions (component space) */
	TArray<FVector3f> DebugBindPoseVertices;

	/** Spatial Hash for debug (O(1) vertex query) */
	FVertexSpatialHash DebugSpatialHash;

	/** Debug data caching complete flag */
	bool bDebugAffectedVerticesCached = false;

	/**
	 * Bulge vertex data for debug visualization (per-Ring)
	 * Result after smoothstep distance-based filtering + direction filtering
	 */
	TArray<FRingAffectedData> DebugBulgeData;

	/** Bulge debug data caching complete flag */
	bool bDebugBulgeVerticesCached = false;

	// ===== GPU Influence Readback Cache =====
	// Readback Influence values calculated in TightnessCS from GPU to CPU for visualization caching
	// Used in DrawAffectedVertices

	/** GPU-computed Influence cache (per-Ring) */
	TArray<TArray<float>> CachedGPUInfluences;

	/** GPU Influence Readback ready flag (per-Ring) */
	TArray<bool> bGPUInfluenceReady;

	/** GPU Influence Readback objects (per-Ring) */
	TArray<TSharedPtr<class FRHIGPUBufferReadback>> GPUInfluenceReadbacks;

	// ===== GPU Debug Rendering =====

	/** Enable GPU debug rendering (replaces DrawDebugPoint) */
	bool bUseGPUDebugRendering = true;

	/** Unified debug point component (uses Scene Proxy, shared depth buffer for Tightness + Bulge) */
	UPROPERTY()
	TObjectPtr<UFleshRingDebugPointComponent> DebugPointComponent;

public:
	/** Get GPU debug rendering enabled state */
	bool IsGPUDebugRenderingEnabled() const { return bUseGPUDebugRendering; }

	/**
	 * Get visible Ring bitmask array (supports unlimited Rings)
	 * Each uint32 element is a visibility bitmask for 32 Rings
	 * Element[0] = Ring 0-31, Element[1] = Ring 32-63, ...
	 * N Rings -> ceil(N/32) elements
	 */
	TArray<uint32> GetVisibilityMaskArray() const;

	/** Get debug point count (AffectedVertices count of first Ring) */
	uint32 GetDebugPointCount() const
	{
		if (DebugAffectedData.Num() > 0)
		{
			return DebugAffectedData[0].Vertices.Num();
		}
		return 0;
	}

	/**
	 * Invalidate CPU debug cache (called from other classes when Ring moves)
	 * @param DirtyRingIndex - Invalidate specific Ring only (INDEX_NONE for all)
	 */
	void InvalidateDebugCaches(int32 DirtyRingIndex = INDEX_NONE)
	{
		// Invalidate debug cache when Ring moves
		// Reset both cache flags and actual data to ensure recalculation next frame
		bDebugAffectedVerticesCached = false;
		bDebugBulgeVerticesCached = false;

		if (DirtyRingIndex == INDEX_NONE)
		{
			// Full invalidation: reset all data
			DebugAffectedData.Reset();
			DebugBulgeData.Reset();
		}
		else
		{
			// Specific Ring invalidation: reset only that Ring's data
			if (DebugAffectedData.IsValidIndex(DirtyRingIndex))
			{
				DebugAffectedData[DirtyRingIndex].Vertices.Reset();
			}
			if (DebugBulgeData.IsValidIndex(DirtyRingIndex))
			{
				DebugBulgeData[DirtyRingIndex].Vertices.Reset();
			}
		}
	}

private:
	/** Initialize debug point components */
	void InitializeDebugPointComponents();

	/** Update Tightness point buffer */
	void UpdateTightnessDebugPointComponent();

	/** Update Bulge point buffer */
	void UpdateBulgeDebugPointComponent();
#endif

#if WITH_EDITOR
	/** Debug visualization main function (called from TickComponent) */
	void DrawDebugVisualization();

	/** Draw SDF volume bounding box */
	void DrawSdfVolume(int32 RingIndex);

	/** Draw affected vertices */
	void DrawAffectedVertices(int32 RingIndex);

	/** Draw SDF slice plane */
	void DrawSDFSlice(int32 RingIndex);

	/** Create slice plane actor */
	AActor* CreateDebugSlicePlane(int32 RingIndex);

	/** Update slice texture */
	void UpdateSliceTexture(int32 RingIndex, int32 SliceZ);

	/** Cleanup debug resources */
	void CleanupDebugResources();

	/** Cache affected vertex data for debug */
	void CacheAffectedVerticesForDebug();

	/** Draw Bulge heatmap (with smoothstep distance-based filtering + direction filtering) */
	void DrawBulgeHeatmap(int32 RingIndex);

	/** Cache Bulge vertex data for debug */
	void CacheBulgeVerticesForDebug();

	/** Draw detected Bulge direction arrow */
	void DrawBulgeDirectionArrow(int32 RingIndex);

	/** Draw Bulge influence range cylinder */
	void DrawBulgeRange(int32 RingIndex);
#endif
};
