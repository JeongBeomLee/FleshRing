// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "Styling/AppStyle.h"

/**
 * FleshRing Asset Editor commands
 * Global shortcuts that work regardless of focus (QWER for gizmo mode)
 */
class FFleshRingEditorCommands : public TCommands<FFleshRingEditorCommands>
{
public:
	FFleshRingEditorCommands()
		: TCommands<FFleshRingEditorCommands>(
			TEXT("FleshRingEditor"),
			NSLOCTEXT("Contexts", "FleshRingEditor", "FleshRing Editor"),
			NAME_None,
			FAppStyle::GetAppStyleSetName())
	{
	}

	// TCommands interface
	virtual void RegisterCommands() override;

public:
	/** Set widget mode to None (Q) */
	TSharedPtr<FUICommandInfo> SetWidgetModeNone;

	/** Set widget mode to Translate (W) */
	TSharedPtr<FUICommandInfo> SetWidgetModeTranslate;

	/** Set widget mode to Rotate (E) */
	TSharedPtr<FUICommandInfo> SetWidgetModeRotate;

	/** Set widget mode to Scale (R) */
	TSharedPtr<FUICommandInfo> SetWidgetModeScale;

	/** Toggle Local/World coordinate system (Ctrl+`) */
	TSharedPtr<FUICommandInfo> ToggleCoordSystem;

	// Debug Visualization (number keys only)
	TSharedPtr<FUICommandInfo> ToggleDebugVisualization;  // 1
	TSharedPtr<FUICommandInfo> ToggleSdfVolume;           // 2
	TSharedPtr<FUICommandInfo> ToggleAffectedVertices;    // 3
	TSharedPtr<FUICommandInfo> ToggleBulgeHeatmap;        // 4
	TSharedPtr<FUICommandInfo> ToggleRingSkinSamplingRadius;  // 5

	// Show toggles (Shift+number)
	TSharedPtr<FUICommandInfo> ToggleSkeletalMesh;        // Shift+1
	TSharedPtr<FUICommandInfo> ToggleRingGizmos;          // Shift+2
	TSharedPtr<FUICommandInfo> ToggleRingMeshes;          // Shift+3
	TSharedPtr<FUICommandInfo> ToggleBulgeRange;          // Shift+4

	// Debug options (Ctrl+number)
	TSharedPtr<FUICommandInfo> ToggleSDFSlice;            // Ctrl+2
	TSharedPtr<FUICommandInfo> ToggleBulgeArrows;         // Ctrl+4
};
