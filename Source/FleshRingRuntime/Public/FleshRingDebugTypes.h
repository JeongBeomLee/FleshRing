// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * GPU 디버그 포인트 데이터 구조체
 * TightnessCS에서 출력 → VS/PS 셰이더에서 빌보드 렌더링
 * GPU 정렬 필수: 16바이트 경계
 */
struct FFleshRingDebugPoint
{
	FVector3f WorldPosition;  // 12 bytes - 월드 공간 위치
	float Influence;          // 4 bytes - Influence 값 (0~1)
};
static_assert(sizeof(FFleshRingDebugPoint) == 16, "FFleshRingDebugPoint must be 16 bytes for GPU alignment");
