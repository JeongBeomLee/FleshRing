// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HitProxies.h"

/**
 * Ring 클릭 감지용 HitProxy
 * 뷰포트에서 Ring을 클릭했을 때 어떤 Ring인지 식별
 */
struct HFleshRingHitProxy : public HHitProxy
{
	DECLARE_HIT_PROXY();

	/** Ring 인덱스 */
	int32 RingIndex;

	HFleshRingHitProxy(int32 InRingIndex)
		: HHitProxy(HPP_UI)
		, RingIndex(InRingIndex)
	{
	}

	virtual EMouseCursor::Type GetMouseCursor() override
	{
		return EMouseCursor::Crosshairs;
	}
};
