// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HitProxies.h"

/** Transform Gizmo 축 타입 */
enum class EFleshRingGizmoAxis : uint8
{
	None,
	X,
	Y,
	Z
};

/**
 * Ring 기즈모(원형 선) 클릭 감지용 HitProxy
 * 뷰포트에서 Ring 기즈모를 클릭했을 때 어떤 Ring인지 식별
 */
struct HFleshRingGizmoHitProxy : public HHitProxy
{
	DECLARE_HIT_PROXY();

	/** Ring 인덱스 */
	int32 RingIndex;

	HFleshRingGizmoHitProxy(int32 InRingIndex)
		: HHitProxy(HPP_UI)
		, RingIndex(InRingIndex)
	{
	}

	virtual EMouseCursor::Type GetMouseCursor() override
	{
		return EMouseCursor::Crosshairs;
	}
};

/**
 * Ring 메시 클릭 감지용 HitProxy
 * 뷰포트에서 Ring 스태틱 메시를 클릭했을 때 어떤 Ring인지 식별
 */
struct HFleshRingMeshHitProxy : public HHitProxy
{
	DECLARE_HIT_PROXY();

	/** Ring 인덱스 */
	int32 RingIndex;

	HFleshRingMeshHitProxy(int32 InRingIndex)
		: HHitProxy(HPP_UI)
		, RingIndex(InRingIndex)
	{
	}

	virtual EMouseCursor::Type GetMouseCursor() override
	{
		return EMouseCursor::CardinalCross;
	}
};

/**
 * Transform Gizmo 축 드래그용 HitProxy
 * 이동 핸들(화살표) 드래그 감지
 */
struct HFleshRingAxisHitProxy : public HHitProxy
{
	DECLARE_HIT_PROXY();

	/** Ring 인덱스 */
	int32 RingIndex;

	/** 드래그 축 */
	EFleshRingGizmoAxis Axis;

	HFleshRingAxisHitProxy(int32 InRingIndex, EFleshRingGizmoAxis InAxis)
		: HHitProxy(HPP_UI)
		, RingIndex(InRingIndex)
		, Axis(InAxis)
	{
	}

	virtual EMouseCursor::Type GetMouseCursor() override
	{
		return EMouseCursor::CardinalCross;
	}
};

