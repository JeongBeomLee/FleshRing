// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingEditorCommands.h"

#define LOCTEXT_NAMESPACE "FleshRingEditorCommands"

void FFleshRingEditorCommands::RegisterCommands()
{
	UI_COMMAND(SetWidgetModeNone, "Select Mode", "Disable transform gizmo (Q)", EUserInterfaceActionType::Button, FInputChord(EKeys::Q));
	UI_COMMAND(SetWidgetModeTranslate, "Translate Mode", "Switch to translate gizmo (W)", EUserInterfaceActionType::Button, FInputChord(EKeys::W));
	UI_COMMAND(SetWidgetModeRotate, "Rotate Mode", "Switch to rotate gizmo (E)", EUserInterfaceActionType::Button, FInputChord(EKeys::E));
	UI_COMMAND(SetWidgetModeScale, "Scale Mode", "Switch to scale gizmo (R)", EUserInterfaceActionType::Button, FInputChord(EKeys::R));
	UI_COMMAND(ToggleCoordSystem, "Toggle Coordinate System", "Toggle between Local and World coordinate system (Ctrl+`)", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Control, EKeys::Tilde));

	// Debug Visualization (number keys only)
	UI_COMMAND(ToggleDebugVisualization, "Toggle Debug Visualization", "Toggle debug visualization (1)", EUserInterfaceActionType::Button, FInputChord(EKeys::One));
	UI_COMMAND(ToggleSdfVolume, "Toggle SDF Volume", "Toggle SDF volume display (2)", EUserInterfaceActionType::Button, FInputChord(EKeys::Two));
	UI_COMMAND(ToggleAffectedVertices, "Toggle Affected Vertices", "Toggle affected vertices display (3)", EUserInterfaceActionType::Button, FInputChord(EKeys::Three));
	UI_COMMAND(ToggleBulgeHeatmap, "Toggle Bulge Heatmap", "Toggle bulge heatmap display (4)", EUserInterfaceActionType::Button, FInputChord(EKeys::Four));

	// Show toggles (Shift+number)
	UI_COMMAND(ToggleSkeletalMesh, "Toggle Skeletal Mesh", "Toggle skeletal mesh visibility (Shift+1)", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Shift, EKeys::One));
	UI_COMMAND(ToggleRingGizmos, "Toggle Ring Gizmos", "Toggle ring gizmos visibility (Shift+2)", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Shift, EKeys::Two));
	UI_COMMAND(ToggleRingMeshes, "Toggle Ring Meshes", "Toggle ring meshes visibility (Shift+3)", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Shift, EKeys::Three));
	UI_COMMAND(ToggleBulgeRange, "Toggle Bulge Range", "Toggle bulge range display (Shift+4)", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Shift, EKeys::Four));

	// Debug options (Ctrl+number)
	UI_COMMAND(ToggleSDFSlice, "Toggle SDF Slice", "Toggle SDF slice display (Ctrl+2)", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Control, EKeys::Two));
	UI_COMMAND(ToggleBulgeArrows, "Toggle Bulge Arrows", "Toggle bulge direction arrows (Ctrl+4)", EUserInterfaceActionType::Button, FInputChord(EModifierKey::Control, EKeys::Four));
}

#undef LOCTEXT_NAMESPACE
