// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * FleshRing Asset Editor constant definitions
 */
namespace FleshRingAssetEditorToolkit
{
	// Editor app identifier
	static const FName AppIdentifier(TEXT("FleshRingAssetEditorApp"));

	// Tab IDs
	static const FName SkeletonTreeTabId(TEXT("FleshRingAssetEditor_SkeletonTree"));
	static const FName ViewportTabId(TEXT("FleshRingAssetEditor_Viewport"));
	static const FName DetailsTabId(TEXT("FleshRingAssetEditor_Details"));
	static const FName PreviewSettingsTabId(TEXT("FleshRingAssetEditor_PreviewSettings"));

	// Layout name (v3: Preview Scene Settings tab added)
	static const FName LayoutName(TEXT("FleshRingAssetEditor_Layout_v3"));
}
