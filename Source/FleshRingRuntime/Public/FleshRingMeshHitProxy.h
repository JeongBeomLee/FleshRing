// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_EDITOR
#include "HitProxies.h"

/**
 * HitProxy for Ring mesh click detection
 * Higher priority than bones so Ring mesh is selected first
 */
struct FLESHRINGRUNTIME_API HFleshRingMeshHitProxy : public HHitProxy
{
	DECLARE_HIT_PROXY();

	/** Ring index */
	int32 RingIndex;

	HFleshRingMeshHitProxy(int32 InRingIndex)
		: HHitProxy(HPP_Foreground)  // Higher priority than bones (HPP_World)
		, RingIndex(InRingIndex)
	{
	}

	virtual EMouseCursor::Type GetMouseCursor() override
	{
		return EMouseCursor::Crosshairs;
	}
};
#endif // WITH_EDITOR
