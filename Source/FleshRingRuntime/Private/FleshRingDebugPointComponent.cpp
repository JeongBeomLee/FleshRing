// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingDebugPointComponent.h"
#include "FleshRingDebugPointSceneProxy.h"

UFleshRingDebugPointComponent::UFleshRingDebugPointComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 기본 설정
	PrimaryComponentTick.bCanEverTick = false;

	// 렌더링 설정
	bCastDynamicShadow = false;
	bCastStaticShadow = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);

	// 에디터에서만 렌더링
	bIsEditorOnly = true;

	// 항상 Dirty 상태로 시작
	bBufferDirty = false;
}

FPrimitiveSceneProxy* UFleshRingDebugPointComponent::CreateSceneProxy()
{
	// 버퍼가 없으면 프록시 생성하지 않음
	if (!HasValidBuffer())
	{
		return nullptr;
	}

	FFleshRingDebugPointSceneProxy* Proxy = new FFleshRingDebugPointSceneProxy(this);

	// 렌더링 파라미터 전달
	Proxy->PointSizeBase = PointSizeBase;
	Proxy->PointSizeInfluence = PointSizeInfluence;

	// 초기 버퍼 설정
	{
		FScopeLock Lock(&BufferLock);
		if (PendingTightnessBuffer.IsValid())
		{
			ENQUEUE_RENDER_COMMAND(InitTightnessBuffer)(
				[Proxy, Buffer = PendingTightnessBuffer, Mask = PendingVisibleRingMask](FRHICommandListImmediate& RHICmdList)
				{
					Proxy->UpdateTightnessBuffer_RenderThread(Buffer, Mask);
				}
			);
		}
		if (PendingBulgeBuffer.IsValid())
		{
			ENQUEUE_RENDER_COMMAND(InitBulgeBuffer)(
				[Proxy, Buffer = PendingBulgeBuffer, Mask = PendingVisibleRingMask](FRHICommandListImmediate& RHICmdList)
				{
					Proxy->UpdateBulgeBuffer_RenderThread(Buffer, Mask);
				}
			);
		}
	}

	return Proxy;
}

FBoxSphereBounds UFleshRingDebugPointComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// 매우 큰 바운딩 박스를 사용하여 항상 렌더링되도록 함
	// 디버그 포인트는 월드 전체에 걸쳐 있을 수 있음
	return FBoxSphereBounds(FBox(FVector(-HALF_WORLD_MAX), FVector(HALF_WORLD_MAX)));
}

void UFleshRingDebugPointComponent::SetTightnessBuffer(
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBuffer,
	uint64 InVisibleRingMask)
{
	FScopeLock Lock(&BufferLock);

	PendingTightnessBuffer = InBuffer;
	PendingVisibleRingMask = InVisibleRingMask;
	bBufferDirty = true;

	// 프록시가 없으면 생성
	if (!SceneProxy && InBuffer.IsValid())
	{
		MarkRenderStateDirty();
	}
	else
	{
		// 동적 데이터 업데이트 요청
		MarkRenderDynamicDataDirty();
	}
}

void UFleshRingDebugPointComponent::ClearTightnessBuffer()
{
	FScopeLock Lock(&BufferLock);

	PendingTightnessBuffer = nullptr;
	bBufferDirty = true;

	// 둘 다 없으면 프록시 제거, 아니면 동적 데이터 업데이트
	if (!PendingBulgeBuffer.IsValid())
	{
		MarkRenderStateDirty();
	}
	else
	{
		MarkRenderDynamicDataDirty();
	}
}

void UFleshRingDebugPointComponent::SetBulgeBuffer(
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBuffer,
	uint64 InVisibleRingMask)
{
	FScopeLock Lock(&BufferLock);

	PendingBulgeBuffer = InBuffer;
	PendingVisibleRingMask = InVisibleRingMask;
	bBufferDirty = true;

	// 프록시가 없으면 생성
	if (!SceneProxy && InBuffer.IsValid())
	{
		MarkRenderStateDirty();
	}
	else
	{
		// 동적 데이터 업데이트 요청
		MarkRenderDynamicDataDirty();
	}
}

void UFleshRingDebugPointComponent::ClearBulgeBuffer()
{
	FScopeLock Lock(&BufferLock);

	PendingBulgeBuffer = nullptr;
	bBufferDirty = true;

	// 둘 다 없으면 프록시 제거, 아니면 동적 데이터 업데이트
	if (!PendingTightnessBuffer.IsValid())
	{
		MarkRenderStateDirty();
	}
	else
	{
		MarkRenderDynamicDataDirty();
	}
}

bool UFleshRingDebugPointComponent::HasValidBuffer() const
{
	FScopeLock Lock(&BufferLock);
	bool bHasTightness = PendingTightnessBuffer.IsValid() && PendingTightnessBuffer->IsValid();
	bool bHasBulge = PendingBulgeBuffer.IsValid() && PendingBulgeBuffer->IsValid();
	return bHasTightness || bHasBulge;
}

void UFleshRingDebugPointComponent::SendRenderDynamicData_Concurrent()
{
	Super::SendRenderDynamicData_Concurrent();

	if (!bBufferDirty)
	{
		return;
	}

	FScopeLock Lock(&BufferLock);
	bBufferDirty = false;

	if (SceneProxy)
	{
		FFleshRingDebugPointSceneProxy* Proxy = static_cast<FFleshRingDebugPointSceneProxy*>(SceneProxy);
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> TightnessToSend = PendingTightnessBuffer;
		TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> BulgeToSend = PendingBulgeBuffer;
		uint64 MaskToSend = PendingVisibleRingMask;

		ENQUEUE_RENDER_COMMAND(UpdateDebugPointBuffers)(
			[Proxy, TightnessToSend, BulgeToSend, MaskToSend](FRHICommandListImmediate& RHICmdList)
			{
				if (TightnessToSend.IsValid())
				{
					Proxy->UpdateTightnessBuffer_RenderThread(TightnessToSend, MaskToSend);
				}
				else
				{
					Proxy->ClearTightnessBuffer_RenderThread();
				}

				if (BulgeToSend.IsValid())
				{
					Proxy->UpdateBulgeBuffer_RenderThread(BulgeToSend, MaskToSend);
				}
				else
				{
					Proxy->ClearBulgeBuffer_RenderThread();
				}
			}
		);
	}
}
