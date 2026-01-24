// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HitProxies.h"
#include "FleshRingMeshHitProxy.h"  // Runtime 모듈의 HFleshRingMeshHitProxy
#include "FleshRingTypes.h"         // EBandSection

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
		: HHitProxy(HPP_World)  // SRT Widget(HPP_Foreground)보다 낮은 우선순위
		, RingIndex(InRingIndex)
	{
	}

	virtual EMouseCursor::Type GetMouseCursor() override
	{
		return EMouseCursor::Crosshairs;
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
		: HHitProxy(HPP_World)  // SRT Widget(HPP_Foreground)보다 낮은 우선순위
		, RingIndex(InRingIndex)
		, Axis(InAxis)
	{
	}

	virtual EMouseCursor::Type GetMouseCursor() override
	{
		return EMouseCursor::CardinalCross;
	}
};

// HFleshRingMeshHitProxy는 FleshRingMeshHitProxy.h (Runtime 모듈)에서 정의됨

/**
 * 스켈레탈 본 클릭 감지용 HitProxy
 * 뷰포트에서 본을 클릭했을 때 어떤 본인지 식별
 */
struct HFleshRingBoneHitProxy : public HHitProxy
{
	DECLARE_HIT_PROXY();

	/** 본 인덱스 */
	int32 BoneIndex;

	/** 본 이름 */
	FName BoneName;

	HFleshRingBoneHitProxy(int32 InBoneIndex, FName InBoneName)
		: HHitProxy(HPP_World)  // Ring 메시(HPP_Foreground)보다 낮은 우선순위
		, BoneIndex(InBoneIndex)
		, BoneName(InBoneName)
	{
	}

	virtual EMouseCursor::Type GetMouseCursor() override
	{
		return EMouseCursor::Crosshairs;
	}
};

/**
 * Virtual Band 섹션 클릭 감지용 HitProxy
 * 개별 섹션(Upper/MidUpper/MidLower/Lower)을 클릭했을 때 식별
 */
struct HFleshRingBandSectionHitProxy : public HHitProxy
{
	DECLARE_HIT_PROXY();

	/** Ring 인덱스 */
	int32 RingIndex;

	/** 선택된 섹션 */
	EBandSection Section;

	HFleshRingBandSectionHitProxy(int32 InRingIndex, EBandSection InSection)
		: HHitProxy(HPP_World)  // SRT Widget(HPP_Foreground)보다 낮은 우선순위
		, RingIndex(InRingIndex)
		, Section(InSection)
	{
	}

	virtual EMouseCursor::Type GetMouseCursor() override
	{
		return EMouseCursor::Crosshairs;
	}
};

