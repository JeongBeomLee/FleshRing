// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "RenderGraphResources.h"
#include "FleshRingDebugTypes.h"
#include "RendererInterface.h"

class UFleshRingDebugPointComponent;

/**
 * FFleshRingDebugPointSceneProxy - Scene Proxy for GPU debug point rendering
 *
 * Draws debug points from GPU buffers during Scene rendering stage,
 * rendering before editor gizmos (PDI).
 *
 * Rendering method:
 * - Uses IRendererModule::RegisterPostOpaqueRenderDelegate
 * - Custom rendering after Opaque, before Translucency
 * - Drawn before editor gizmos (PDI)
 *
 * Rendering order: Scene Geometry < Debug Points < Editor Gizmos
 */
class FLESHRINGRUNTIME_API FFleshRingDebugPointSceneProxy : public FPrimitiveSceneProxy
{
public:
	FFleshRingDebugPointSceneProxy(const UFleshRingDebugPointComponent* InComponent);
	virtual ~FFleshRingDebugPointSceneProxy();

	// ========================================
	// FPrimitiveSceneProxy Interface
	// ========================================

	virtual SIZE_T GetTypeHash() const override;
	virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

	/** Whether this proxy should be rendered */
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;

	/** Collect dynamic mesh elements - empty implementation (uses custom render delegate) */
	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override;

	/** Called when registered to Scene */
	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;

	/** Called when removed from Scene */
	virtual void DestroyRenderThreadResources() override;

	// ========================================
	// Buffer Update (Render Thread)
	// ========================================

	/**
	 * Update Tightness buffer (called from render thread)
	 * @param InBuffer - Pooled RDG buffer reference
	 * @param InVisibilityMaskArray - Visible Ring bitmask array (unlimited Ring support)
	 */
	void UpdateTightnessBuffer_RenderThread(
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBuffer,
		const TArray<uint32>& InVisibilityMaskArray);

	/**
	 * Update Bulge buffer (called from render thread)
	 * @param InBuffer - Pooled RDG buffer reference
	 * @param InVisibilityMaskArray - Visible Ring bitmask array (unlimited Ring support)
	 */
	void UpdateBulgeBuffer_RenderThread(
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBuffer,
		const TArray<uint32>& InVisibilityMaskArray);

	/** Clear Tightness buffer */
	void ClearTightnessBuffer_RenderThread();

	/** Clear Bulge buffer */
	void ClearBulgeBuffer_RenderThread();

	/** Clear all buffers */
	void ClearBuffer_RenderThread();

	// ========================================
	// Rendering Parameters
	// ========================================

	/** Base point size (pixels) */
	float PointSizeBase = 8.0f;

	/** Additional point size based on Influence */
	float PointSizeInfluence = 4.0f;

	/** Outline opacity (0.0 = no outline, 1.0 = full outline) */
	float DebugPointOutlineOpacity = 1.0f;

private:
	/** Tightness GPU debug point buffer */
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> TightnessBufferShared;

	/** Bulge GPU debug point buffer */
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> BulgeBufferShared;

	/** Visible Ring bitmask array (unlimited Ring support) */
	TArray<uint32> VisibilityMaskArray;

	/** Buffer access synchronization */
	mutable FCriticalSection BufferLock;

	/** Post-Opaque render delegate handle */
	FDelegateHandle PostOpaqueRenderDelegateHandle;

	/** Post-Opaque rendering callback */
	void RenderPostOpaque_RenderThread(FPostOpaqueRenderParameters& Parameters);
};
