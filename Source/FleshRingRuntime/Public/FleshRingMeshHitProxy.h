// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR
#include "HitProxies.h"

/**
 * Ring 메시 클릭 감지용 HitProxy
 * 본보다 높은 우선순위로 Ring 메시가 먼저 선택되도록 함
 */
struct FLESHRINGRUNTIME_API HFleshRingMeshHitProxy : public HHitProxy
{
	DECLARE_HIT_PROXY();

	/** Ring 인덱스 */
	int32 RingIndex;

	HFleshRingMeshHitProxy(int32 InRingIndex)
		: HHitProxy(HPP_Foreground)  // 본(HPP_World)보다 높은 우선순위
		, RingIndex(InRingIndex)
	{
	}

	virtual EMouseCursor::Type GetMouseCursor() override
	{
		return EMouseCursor::Crosshairs;
	}
};
#endif // WITH_EDITOR
