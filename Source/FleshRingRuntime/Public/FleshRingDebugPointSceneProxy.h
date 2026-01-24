// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "RenderGraphResources.h"
#include "FleshRingDebugTypes.h"
#include "RendererInterface.h"

class UFleshRingDebugPointComponent;

/**
 * FFleshRingDebugPointSceneProxy - GPU 디버그 포인트 렌더링용 Scene Proxy
 *
 * GPU 버퍼의 디버그 포인트를 Scene 렌더링 단계에서 그려서
 * 에디터 기즈모(PDI)보다 먼저 렌더링되도록 합니다.
 *
 * 렌더링 방식:
 * - IRendererModule::RegisterPostOpaqueRenderDelegate 사용
 * - Opaque 렌더링 이후, Translucency 이전에 커스텀 렌더링
 * - 에디터 기즈모(PDI)보다 먼저 그려짐
 *
 * 렌더링 순서: Scene Geometry < Debug Points < Editor Gizmos
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

	/** 이 프록시가 렌더링되어야 하는지 여부 */
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;

	/** 동적 메시 엘리먼트 수집 - 빈 구현 (커스텀 렌더 델리게이트 사용) */
	virtual void GetDynamicMeshElements(
		const TArray<const FSceneView*>& Views,
		const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector) const override;

	/** Scene에 등록될 때 호출 */
	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;

	/** Scene에서 제거될 때 호출 */
	virtual void DestroyRenderThreadResources() override;

	// ========================================
	// Buffer Update (Render Thread)
	// ========================================

	/**
	 * Tightness 버퍼 업데이트 (렌더 스레드에서 호출)
	 * @param InBuffer - 풀링된 RDG 버퍼 참조
	 * @param InVisibleRingMask - 가시 Ring 비트마스크 (64-bit)
	 */
	void UpdateTightnessBuffer_RenderThread(
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBuffer,
		uint64 InVisibleRingMask);

	/**
	 * Bulge 버퍼 업데이트 (렌더 스레드에서 호출)
	 * @param InBuffer - 풀링된 RDG 버퍼 참조
	 * @param InVisibleRingMask - 가시 Ring 비트마스크 (64-bit)
	 */
	void UpdateBulgeBuffer_RenderThread(
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBuffer,
		uint64 InVisibleRingMask);

	/** Tightness 버퍼 클리어 */
	void ClearTightnessBuffer_RenderThread();

	/** Bulge 버퍼 클리어 */
	void ClearBulgeBuffer_RenderThread();

	/** 모든 버퍼 클리어 */
	void ClearBuffer_RenderThread();

	// ========================================
	// Rendering Parameters
	// ========================================

	/** 기본 포인트 크기 (픽셀) */
	float PointSizeBase = 8.0f;

	/** Influence 기반 추가 포인트 크기 */
	float PointSizeInfluence = 4.0f;

private:
	/** Tightness GPU 디버그 포인트 버퍼 */
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> TightnessBufferShared;

	/** Bulge GPU 디버그 포인트 버퍼 */
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> BulgeBufferShared;

	/** 가시 Ring 비트마스크 (64-bit) */
	uint64 VisibleRingMask = 0xFFFFFFFFFFFFFFFFull;

	/** 버퍼 접근 동기화 */
	mutable FCriticalSection BufferLock;

	/** Post-Opaque 렌더 델리게이트 핸들 */
	FDelegateHandle PostOpaqueRenderDelegateHandle;

	/** Post-Opaque 렌더링 콜백 */
	void RenderPostOpaque_RenderThread(FPostOpaqueRenderParameters& Parameters);
};
