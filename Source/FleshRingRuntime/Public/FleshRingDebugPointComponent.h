// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "RenderGraphResources.h"
#include "FleshRingDebugPointComponent.generated.h"

class FFleshRingDebugPointSceneProxy;

/**
 * UFleshRingDebugPointComponent - GPU 디버그 포인트 렌더링용 컴포넌트
 *
 * GPU 버퍼의 디버그 포인트를 Scene 렌더링 단계에서 그려서
 * 에디터 기즈모(PDI)보다 먼저 렌더링되도록 합니다.
 *
 * Tightness와 Bulge 버퍼를 모두 처리하며, 공유 depth buffer를 사용하여
 * 두 타입의 포인트 간 depth ordering을 보장합니다.
 *
 * 사용법:
 * 1. FleshRingComponent의 자식 컴포넌트로 생성
 * 2. SetTightnessBuffer() / SetBulgeBuffer()로 GPU 버퍼 설정
 * 3. Scene Proxy가 자동으로 렌더링 수행
 */
UCLASS(ClassGroup=(FleshRing), meta=(BlueprintSpawnableComponent))
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
	 * Tightness 디버그 포인트 버퍼 설정 (게임 스레드에서 호출)
	 * @param InBuffer - 풀링된 RDG 버퍼 참조
	 * @param InVisibleRingMask - 가시 Ring 비트마스크 (64-bit)
	 */
	void SetTightnessBuffer(
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBuffer,
		uint64 InVisibleRingMask);

	/** Tightness 버퍼 클리어 */
	void ClearTightnessBuffer();

	// ========================================
	// Buffer Management - Bulge
	// ========================================

	/**
	 * Bulge 디버그 포인트 버퍼 설정 (게임 스레드에서 호출)
	 * @param InBuffer - 풀링된 RDG 버퍼 참조
	 * @param InVisibleRingMask - 가시 Ring 비트마스크 (64-bit)
	 */
	void SetBulgeBuffer(
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBuffer,
		uint64 InVisibleRingMask);

	/** Bulge 버퍼 클리어 */
	void ClearBulgeBuffer();

	/** 버퍼가 설정되어 있는지 확인 */
	bool HasValidBuffer() const;

	// ========================================
	// Rendering Parameters
	// ========================================

	/** 기본 포인트 크기 (픽셀) */
	UPROPERTY(EditAnywhere, Category = "Debug Rendering")
	float PointSizeBase = 8.0f;

	/** Influence 기반 추가 포인트 크기 */
	UPROPERTY(EditAnywhere, Category = "Debug Rendering")
	float PointSizeInfluence = 4.0f;

protected:
	virtual void SendRenderDynamicData_Concurrent() override;

private:
	/** Tightness 버퍼 (게임 스레드에서 설정, 렌더 스레드로 전달) */
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> PendingTightnessBuffer;

	/** Bulge 버퍼 (게임 스레드에서 설정, 렌더 스레드로 전달) */
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> PendingBulgeBuffer;

	/** 대기 중인 가시성 마스크 */
	uint64 PendingVisibleRingMask = 0xFFFFFFFFFFFFFFFFull;

	/** 버퍼 변경 플래그 */
	bool bBufferDirty = false;

	/** 동기화 */
	mutable FCriticalSection BufferLock;
};
