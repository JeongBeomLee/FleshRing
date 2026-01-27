// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "RenderGraphResources.h"
#include "FleshRingDebugPointComponent.generated.h"

class FFleshRingDebugPointSceneProxy;

/**
 * UFleshRingDebugPointComponent - Component for GPU debug point rendering
 *
 * Draws debug points from GPU buffers during Scene rendering stage,
 * rendering before editor gizmos (PDI).
 *
 * Handles both Tightness and Bulge buffers, using a shared depth buffer
 * to ensure depth ordering between the two point types.
 *
 * Usage:
 * 1. Create as child component of FleshRingComponent
 * 2. Set GPU buffers via SetTightnessBuffer() / SetBulgeBuffer()
 * 3. Scene Proxy performs rendering automatically
 */
UCLASS(ClassGroup=(FleshRing))
class FLESHRINGRUNTIME_API UFleshRingDebugPointComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UFleshRingDebugPointComponent(const FObjectInitializer& ObjectInitializer);

	// ========================================
	// UPrimitiveComponent Interface
	// ========================================

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual bool ShouldRecreateProxyOnUpdateTransform() const override { return false; }

	// ========================================
	// Buffer Management - Tightness
	// ========================================

	/**
	 * Set Tightness debug point buffer (called from game thread)
	 * @param InBuffer - Pooled RDG buffer reference
	 * @param InVisibilityMaskArray - Visible Ring bitmask array (unlimited Ring support)
	 */
	void SetTightnessBuffer(
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBuffer,
		const TArray<uint32>& InVisibilityMaskArray);

	/** Clear Tightness buffer */
	void ClearTightnessBuffer();

	// ========================================
	// Buffer Management - Bulge
	// ========================================

	/**
	 * Set Bulge debug point buffer (called from game thread)
	 * @param InBuffer - Pooled RDG buffer reference
	 * @param InVisibilityMaskArray - Visible Ring bitmask array (unlimited Ring support)
	 */
	void SetBulgeBuffer(
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBuffer,
		const TArray<uint32>& InVisibilityMaskArray);

	/** Clear Bulge buffer */
	void ClearBulgeBuffer();

	/** Check if buffer is set */
	bool HasValidBuffer() const;

	// ========================================
	// Rendering Parameters
	// ========================================

	/** Base point size (pixels) */
	UPROPERTY(EditAnywhere, Category = "Debug Rendering")
	float PointSizeBase = 8.0f;

	/** Additional point size based on Influence */
	UPROPERTY(EditAnywhere, Category = "Debug Rendering")
	float PointSizeInfluence = 4.0f;

protected:
	virtual void SendRenderDynamicData_Concurrent() override;

private:
	/** Tightness buffer (set from game thread, passed to render thread) */
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> PendingTightnessBuffer;

	/** Bulge buffer (set from game thread, passed to render thread) */
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> PendingBulgeBuffer;

	/** Pending visibility mask array (unlimited Ring support) */
	TArray<uint32> PendingVisibilityMaskArray;

	/** Buffer changed flag */
	bool bBufferDirty = false;

	/** Synchronization */
	mutable FCriticalSection BufferLock;
};
