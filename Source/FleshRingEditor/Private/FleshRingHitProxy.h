// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HitProxies.h"
#include "FleshRingMeshHitProxy.h"  // HFleshRingMeshHitProxy from Runtime module
#include "FleshRingTypes.h"         // EBandSection

/** Transform Gizmo axis type */
enum class EFleshRingGizmoAxis : uint8
{
	None,
	X,
	Y,
	Z
};

/**
 * HitProxy for Ring gizmo (circular line) click detection
 * Identifies which Ring when the Ring gizmo is clicked in viewport
 */
struct HFleshRingGizmoHitProxy : public HHitProxy
{
	DECLARE_HIT_PROXY();

	/** Ring index */
	int32 RingIndex;

	HFleshRingGizmoHitProxy(int32 InRingIndex)
		: HHitProxy(HPP_World)  // Lower priority than SRT Widget (HPP_Foreground)
		, RingIndex(InRingIndex)
	{
	}

	virtual EMouseCursor::Type GetMouseCursor() override
	{
		return EMouseCursor::Crosshairs;
	}
};

/**
 * HitProxy for Transform Gizmo axis dragging
 * Detects dragging of translation handles (arrows)
 */
struct HFleshRingAxisHitProxy : public HHitProxy
{
	DECLARE_HIT_PROXY();

	/** Ring index */
	int32 RingIndex;

	/** Drag axis */
	EFleshRingGizmoAxis Axis;

	HFleshRingAxisHitProxy(int32 InRingIndex, EFleshRingGizmoAxis InAxis)
		: HHitProxy(HPP_World)  // Lower priority than SRT Widget (HPP_Foreground)
		, RingIndex(InRingIndex)
		, Axis(InAxis)
	{
	}

	virtual EMouseCursor::Type GetMouseCursor() override
	{
		return EMouseCursor::CardinalCross;
	}
};

// HFleshRingMeshHitProxy is defined in FleshRingMeshHitProxy.h (Runtime module)

/**
 * HitProxy for skeletal bone click detection
 * Identifies which bone when a bone is clicked in viewport
 */
struct HFleshRingBoneHitProxy : public HHitProxy
{
	DECLARE_HIT_PROXY();

	/** Bone index */
	int32 BoneIndex;

	/** Bone name */
	FName BoneName;

	HFleshRingBoneHitProxy(int32 InBoneIndex, FName InBoneName)
		: HHitProxy(HPP_World)  // Lower priority than Ring mesh (HPP_Foreground)
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
 * HitProxy for Virtual Band section click detection
 * Identifies when individual sections (Upper/MidUpper/MidLower/Lower) are clicked
 */
struct HFleshRingBandSectionHitProxy : public HHitProxy
{
	DECLARE_HIT_PROXY();

	/** Ring index */
	int32 RingIndex;

	/** Selected section */
	EBandSection Section;

	HFleshRingBandSectionHitProxy(int32 InRingIndex, EBandSection InSection)
		: HHitProxy(HPP_World)  // Lower priority than SRT Widget (HPP_Foreground)
		, RingIndex(InRingIndex)
		, Section(InSection)
	{
	}

	virtual EMouseCursor::Type GetMouseCursor() override
	{
		return EMouseCursor::Crosshairs;
	}
};

