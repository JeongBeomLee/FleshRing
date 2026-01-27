// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingDebugPointComponent.h"
#include "FleshRingDebugPointSceneProxy.h"

UFleshRingDebugPointComponent::UFleshRingDebugPointComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Default settings
	PrimaryComponentTick.bCanEverTick = false;

	// Rendering settings
	bCastDynamicShadow = false;
	bCastStaticShadow = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetGenerateOverlapEvents(false);

	// Render only in editor
	bIsEditorOnly = true;

	// Always start in dirty state
	bBufferDirty = false;
}

FPrimitiveSceneProxy* UFleshRingDebugPointComponent::CreateSceneProxy()
{
	// Do not create proxy if buffer does not exist
	if (!HasValidBuffer())
	{
		return nullptr;
	}

	FFleshRingDebugPointSceneProxy* Proxy = new FFleshRingDebugPointSceneProxy(this);

	// Pass rendering parameters
	Proxy->PointSizeBase = PointSizeBase;
	Proxy->PointSizeInfluence = PointSizeInfluence;

	// Initial buffer setup
	{
		FScopeLock Lock(&BufferLock);
		if (PendingTightnessBuffer.IsValid())
		{
			ENQUEUE_RENDER_COMMAND(InitTightnessBuffer)(
				[Proxy, Buffer = PendingTightnessBuffer, MaskArray = PendingVisibilityMaskArray](FRHICommandListImmediate& RHICmdList)
				{
					Proxy->UpdateTightnessBuffer_RenderThread(Buffer, MaskArray);
				}
			);
		}
		if (PendingBulgeBuffer.IsValid())
		{
			ENQUEUE_RENDER_COMMAND(InitBulgeBuffer)(
				[Proxy, Buffer = PendingBulgeBuffer, MaskArray = PendingVisibilityMaskArray](FRHICommandListImmediate& RHICmdList)
				{
					Proxy->UpdateBulgeBuffer_RenderThread(Buffer, MaskArray);
				}
			);
		}
	}

	return Proxy;
}

FBoxSphereBounds UFleshRingDebugPointComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// Use a very large bounding box to ensure it is always rendered
	// Debug points can span across the entire world
	return FBoxSphereBounds(FBox(FVector(-HALF_WORLD_MAX), FVector(HALF_WORLD_MAX)));
}

void UFleshRingDebugPointComponent::SetTightnessBuffer(
	TSharedPtr<TRefCountPtr<FRDGPooledBuffer>> InBuffer,
	const TArray<uint32>& InVisibilityMaskArray)
{
	FScopeLock Lock(&BufferLock);

	PendingTightnessBuffer = InBuffer;
	PendingVisibilityMaskArray = InVisibilityMaskArray;
	bBufferDirty = true;

	// Create proxy if it does not exist
	if (!SceneProxy && InBuffer.IsValid())
	{
		MarkRenderStateDirty();
	}
	else
	{
		// Request dynamic data update
		MarkRenderDynamicDataDirty();
	}
}

void UFleshRingDebugPointComponent::ClearTightnessBuffer()
{
	FScopeLock Lock(&BufferLock);

	PendingTightnessBuffer = nullptr;
	bBufferDirty = true;

	// Remove proxy if both buffers are empty, otherwise update dynamic data
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
	const TArray<uint32>& InVisibilityMaskArray)
{
	FScopeLock Lock(&BufferLock);

	PendingBulgeBuffer = InBuffer;
	PendingVisibilityMaskArray = InVisibilityMaskArray;
	bBufferDirty = true;

	// Create proxy if it does not exist
	if (!SceneProxy && InBuffer.IsValid())
	{
		MarkRenderStateDirty();
	}
	else
	{
		// Request dynamic data update
		MarkRenderDynamicDataDirty();
	}
}

void UFleshRingDebugPointComponent::ClearBulgeBuffer()
{
	FScopeLock Lock(&BufferLock);

	PendingBulgeBuffer = nullptr;
	bBufferDirty = true;

	// Remove proxy if both buffers are empty, otherwise update dynamic data
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
		TArray<uint32> MaskArrayToSend = PendingVisibilityMaskArray;

		ENQUEUE_RENDER_COMMAND(UpdateDebugPointBuffers)(
			[Proxy, TightnessToSend, BulgeToSend, MaskArrayToSend](FRHICommandListImmediate& RHICmdList)
			{
				if (TightnessToSend.IsValid())
				{
					Proxy->UpdateTightnessBuffer_RenderThread(TightnessToSend, MaskArrayToSend);
				}
				else
				{
					Proxy->ClearTightnessBuffer_RenderThread();
				}

				if (BulgeToSend.IsValid())
				{
					Proxy->UpdateBulgeBuffer_RenderThread(BulgeToSend, MaskArrayToSend);
				}
				else
				{
					Proxy->ClearBulgeBuffer_RenderThread();
				}
			}
		);
	}
}
