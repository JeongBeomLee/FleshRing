// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * GPU debug point data structure
 * Output from TightnessCS/BulgeCS -> Billboard rendering in VS/PS shader
 * GPU alignment required: 24 bytes (multiple of 8)
 */
struct FFleshRingDebugPoint
{
	FVector3f WorldPosition;  // 12 bytes - World space position
	float Influence;          // 4 bytes - Influence value (0~1)
	uint32 RingIndex;         // 4 bytes - Ring index (for visibility filtering)
	uint32 Padding;           // 4 bytes - Alignment padding
};
static_assert(sizeof(FFleshRingDebugPoint) == 24, "FFleshRingDebugPoint must be 24 bytes for GPU alignment");
