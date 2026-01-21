// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "RHIResources.h"
#include "RenderGraphResources.h"

/**
 * FFleshRingDebugViewExtension - Scene View Extension for Debug Point Rendering
 * GPU 원형 디버그 포인트 렌더링용 SceneViewExtension
 *
 * Renders debug points directly on GPU without CPU readback.
 * Uses PostRenderViewFamily_RenderThread for rendering after scene.
 * CPU Readback 없이 GPU에서 디버그 포인트를 직접 렌더링합니다.
 * 특정 World에서만 활성화됩니다.
 */
class FLESHRINGRUNTIME_API FFleshRingDebugViewExtension : public FSceneViewExtensionBase
{
public:
    FFleshRingDebugViewExtension(const FAutoRegister& AutoRegister, UWorld* InWorld);
    virtual ~FFleshRingDebugViewExtension();

    // ========================================
    // ISceneViewExtension Interface
    // ========================================

    virtual void SetupViewFamily(FSceneViewFamily& InViewFamily) override {}
    virtual void SetupView(FSceneViewFamily& InViewFamily, FSceneView& InView) override {}
    virtual void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override {}

    /** Called after view family rendering - render debug points here */
    /** 뷰 패밀리 렌더링 후 호출 - 여기서 디버그 포인트 렌더링 */
    virtual void PostRenderViewFamily_RenderThread(
        FRDGBuilder& GraphBuilder,
        FSceneViewFamily& InViewFamily) override;

    /** Filter to only render in the bound World's viewports */
    /** 바인딩된 World의 뷰포트에서만 렌더링하도록 필터링 */
    virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

    // ========================================
    // Debug Point Buffer Management
    // ========================================

    /**
     * Set the debug point buffer (call from game thread)
     * 디버그 포인트 버퍼 설정 (게임 스레드에서 호출)
     * PointCount는 렌더 스레드에서 버퍼의 NumElements로 직접 읽음
     *
     * @param InBufferPtr - SharedPtr to pooled buffer (매 프레임 최신 버퍼 참조)
     */
    void SetDebugPointBufferShared(TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBufferPtr);

    /** Clear the debug point buffer (disable rendering) */
    /** 디버그 포인트 버퍼 클리어 (렌더링 비활성화) */
    void ClearDebugPointBuffer();

    // ========================================
    // Bulge Debug Point Buffer Management
    // ========================================

    /**
     * Set the Bulge debug point buffer (call from game thread)
     * Bulge 디버그 포인트 버퍼 설정 (게임 스레드에서 호출)
     * PointCount는 렌더 스레드에서 버퍼의 NumElements로 직접 읽음
     */
    void SetDebugBulgePointBufferShared(TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBufferPtr);

    /** Clear the Bulge debug point buffer (disable Bulge rendering) */
    /** Bulge 디버그 포인트 버퍼 클리어 (Bulge 렌더링 비활성화) */
    void ClearDebugBulgePointBuffer();

    /** Check if debug rendering is enabled */
    /** 디버그 렌더링이 활성화되어 있는지 확인 */
    bool IsEnabled() const { return bEnabled; }

    /** Check if Bulge debug rendering is enabled */
    /** Bulge 디버그 렌더링이 활성화되어 있는지 확인 */
    bool IsBulgeEnabled() const { return bBulgeEnabled; }

    // ========================================
    // Rendering Parameters
    // ========================================

    /** Base point size in pixels (default: 8.0) */
    /** 기본 포인트 크기 (픽셀, 기본값: 8.0) */
    float PointSizeBase = 8.0f;

    /** Additional point size based on influence (default: 4.0) */
    /** Influence에 따른 추가 포인트 크기 (기본값: 4.0) */
    float PointSizeInfluence = 4.0f;

    // ========================================
    // Ring Visibility Filtering (64-bit)
    // ========================================

    /**
     * Set the visible ring mask for debug point filtering
     * 디버그 포인트 필터링용 가시 Ring 마스크 설정
     * @param InMask - Bitmask where bit N = Ring N is visible (64-bit, supports up to 64 rings)
     */
    void SetVisibleRingMask(uint64 InMask);

    /** Get current visible ring mask */
    /** 현재 가시 Ring 마스크 반환 (64-bit) */
    uint64 GetVisibleRingMask() const { return VisibleRingMask; }

private:
    /** The World this extension is bound to */
    /** 이 확장이 바인딩된 World */
    TWeakObjectPtr<UWorld> BoundWorld;

    /** Thread-safe access to buffer data */
    /** 버퍼 데이터에 대한 스레드 안전 접근 */
    mutable FCriticalSection BufferLock;

    /** SharedPtr to pooled buffer (매 프레임 최신 버퍼 참조) */
    TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> DebugPointBufferSharedPtr;

    /** Flag to enable/disable rendering */
    /** 렌더링 활성화/비활성화 플래그 */
    bool bEnabled = false;

    // ========================================
    // Bulge Debug Point Data
    // ========================================

    /** SharedPtr to Bulge pooled buffer */
    TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> DebugBulgePointBufferSharedPtr;

    /** Flag to enable/disable Bulge rendering */
    bool bBulgeEnabled = false;

    // ========================================
    // Ring Visibility Filtering Data (64-bit)
    // ========================================

    /** Bitmask for ring visibility (bit N = Ring N visible, default: all visible, supports up to 64 rings) */
    /** Ring 가시성 비트마스크 (비트 N = Ring N 가시, 기본값: 모두 가시, 최대 64개 Ring 지원) */
    uint64 VisibleRingMask = 0xFFFFFFFFFFFFFFFFull;
};
