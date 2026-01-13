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
     * Set the debug point buffer and count (call from game thread)
     * 디버그 포인트 버퍼와 개수 설정 (게임 스레드에서 호출)
     *
     * @param InBufferPtr - SharedPtr to pooled buffer (매 프레임 최신 버퍼 참조)
     *                      풀링된 버퍼에 대한 SharedPtr
     * @param InPointCount - Number of debug points in the buffer
     *                       버퍼의 디버그 포인트 개수
     */
    void SetDebugPointBufferShared(TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBufferPtr, uint32 InPointCount);

    /** Legacy: Set debug point buffer (단일 프레임, 복사) */
    void SetDebugPointBuffer(TRefCountPtr<FRDGPooledBuffer> InBuffer, uint32 InPointCount);

    /** Clear the debug point buffer (disable rendering) */
    /** 디버그 포인트 버퍼 클리어 (렌더링 비활성화) */
    void ClearDebugPointBuffer();

    /** Check if debug rendering is enabled */
    /** 디버그 렌더링이 활성화되어 있는지 확인 */
    bool IsEnabled() const { return bEnabled; }

    // ========================================
    // Rendering Parameters
    // ========================================

    /** Base point size in pixels (default: 8.0) */
    /** 기본 포인트 크기 (픽셀, 기본값: 8.0) */
    float PointSizeBase = 8.0f;

    /** Additional point size based on influence (default: 4.0) */
    /** Influence에 따른 추가 포인트 크기 (기본값: 4.0) */
    float PointSizeInfluence = 4.0f;

private:
    /** The World this extension is bound to */
    /** 이 확장이 바인딩된 World */
    TWeakObjectPtr<UWorld> BoundWorld;

    /** Thread-safe access to buffer data */
    /** 버퍼 데이터에 대한 스레드 안전 접근 */
    mutable FCriticalSection BufferLock;

    /** Pooled buffer containing debug points (legacy, 단일 프레임용) */
    /** 디버그 포인트가 포함된 풀링된 버퍼 */
    TRefCountPtr<FRDGPooledBuffer> DebugPointBuffer;

    /** SharedPtr to pooled buffer (매 프레임 최신 버퍼 참조) */
    TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> DebugPointBufferSharedPtr;

    /** Number of debug points in the buffer */
    /** 버퍼의 디버그 포인트 개수 */
    uint32 PointCount = 0;

    /** Flag to enable/disable rendering */
    /** 렌더링 활성화/비활성화 플래그 */
    bool bEnabled = false;
};
