// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FleshRing Asset Editor 상수 정의
 */
namespace FleshRingAssetEditorToolkit
{
	// 에디터 앱 식별자
	static const FName AppIdentifier(TEXT("FleshRingAssetEditorApp"));

	// 탭 ID
	static const FName SkeletonTreeTabId(TEXT("FleshRingAssetEditor_SkeletonTree"));
	static const FName ViewportTabId(TEXT("FleshRingAssetEditor_Viewport"));
	static const FName DetailsTabId(TEXT("FleshRingAssetEditor_Details"));
	static const FName PreviewSettingsTabId(TEXT("FleshRingAssetEditor_PreviewSettings"));

	// 레이아웃 이름 (v3: Preview Scene Settings 탭 추가)
	static const FName LayoutName(TEXT("FleshRingAssetEditor_Layout_v3"));
}
