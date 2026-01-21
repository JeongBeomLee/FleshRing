// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * GPU 디버그 포인트 데이터 구조체
 * TightnessCS/BulgeCS에서 출력 → VS/PS 셰이더에서 빌보드 렌더링
 * GPU 정렬 필수: 24바이트 (8의 배수)
 */
struct FFleshRingDebugPoint
{
	FVector3f WorldPosition;  // 12 bytes - 월드 공간 위치
	float Influence;          // 4 bytes - Influence 값 (0~1)
	uint32 RingIndex;         // 4 bytes - Ring 인덱스 (가시성 필터링용)
	uint32 Padding;           // 4 bytes - 정렬 패딩
};
static_assert(sizeof(FFleshRingDebugPoint) == 24, "FFleshRingDebugPoint must be 24 bytes for GPU alignment");
