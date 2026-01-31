// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorViewportClient.h"
#include "ScopedTransaction.h"
#include "FleshRingTypes.h"
#include "AssetViewerSettings.h"

class FFleshRingPreviewScene;
class SFleshRingEditorViewport;
class UFleshRingAsset;

/** Bone draw mode (Persona style) */
namespace EFleshRingBoneDrawMode
{
	enum Type
	{
		None,						// Don't show bones
		Selected,					// Selected bone only
		SelectedAndParents,			// Selected bone + parent bones
		SelectedAndChildren,		// Selected bone + child bones
		SelectedAndParentsAndChildren,	// Selected bone + parents + children
		All							// All bones (entire hierarchy)
	};
}

/** Bone selection cleared delegate */
DECLARE_DELEGATE(FOnBoneSelectionCleared);

/** Ring selected in viewport delegate (RingIndex, SelectionType) */
DECLARE_DELEGATE_TwoParams(FOnRingSelectedInViewport, int32 /*RingIndex*/, EFleshRingSelectionType /*SelectionType*/);

/** Ring deleted in viewport delegate */
DECLARE_DELEGATE(FOnRingDeletedInViewport);

/** Bone selected in viewport delegate */
DECLARE_DELEGATE_OneParam(FOnBoneSelectedInViewport, FName /*BoneName*/);

/** Ring add request in viewport delegate (called from right-click menu) */
DECLARE_DELEGATE_FourParams(FOnAddRingAtPositionRequested, FName /*BoneName*/, const FVector& /*LocalOffset*/, const FRotator& /*LocalRotation*/, UStaticMesh* /*SelectedMesh*/);

/**
 * FleshRing editor viewport client
 * Handles rendering, camera control, input processing
 */
class FFleshRingEditorViewportClient : public FEditorViewportClient
{
public:
	/** Track all active instances (for type checking) */
	static TSet<FFleshRingEditorViewportClient*>& GetAllInstances()
	{
		static TSet<FFleshRingEditorViewportClient*> Instances;
		return Instances;
	}

	/** Check if a specific ViewportClient is FleshRing type */
	static bool IsFleshRingViewportClient(FEditorViewportClient* Client)
	{
		return Client && GetAllInstances().Contains(static_cast<FFleshRingEditorViewportClient*>(Client));
	}

	FFleshRingEditorViewportClient(
		FEditorModeTools* InModeTools,
		FFleshRingPreviewScene* InPreviewScene,
		const TWeakPtr<SFleshRingEditorViewport>& InViewportWidget);

	virtual ~FFleshRingEditorViewportClient();

	// FEditorViewportClient interface
	virtual void Tick(float DeltaSeconds) override;
	virtual void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override;
	virtual void DrawCanvas(FViewport& InViewport, FSceneView& View, FCanvas& Canvas) override;
	virtual bool InputKey(const FInputKeyEventArgs& EventArgs) override;
	virtual bool InputAxis(const FInputKeyEventArgs& EventArgs) override;
	virtual void ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY) override;

	// Widget related overrides
	virtual FVector GetWidgetLocation() const override;
	virtual FMatrix GetWidgetCoordSystem() const override;
	virtual ECoordSystem GetWidgetCoordSystemSpace() const override;
	virtual void SetWidgetCoordSystemSpace(ECoordSystem NewCoordSystem) override;
	virtual UE::Widget::EWidgetMode GetWidgetMode() const override;
	virtual bool InputWidgetDelta(FViewport* InViewport, EAxisList::Type CurrentAxis, FVector& Drag, FRotator& Rot, FVector& Scale) override;
	virtual void TrackingStarted(const FInputEventState& InInputState, bool bIsDraggingWidget, bool bNudge) override;
	virtual void TrackingStopped() override;

	/** Set the Asset being edited */
	void SetAsset(UFleshRingAsset* InAsset);

	/** Get preview scene */
	FFleshRingPreviewScene* GetPreviewScene() const { return PreviewScene; }

	/** Get the Asset being edited */
	UFleshRingAsset* GetEditingAsset() const { return EditingAsset.Get(); }

	/** Focus camera on mesh */
	void FocusOnMesh();

	/** Get current selection type */
	EFleshRingSelectionType GetSelectionType() const { return SelectionType; }

	/** Set selection type (for Undo restoration) */
	void SetSelectionType(EFleshRingSelectionType InType) { SelectionType = InType; }

	/** Set skip selection validation (used during Undo/Redo) */
	void SetSkipSelectionValidation(bool bSkip) { bSkipSelectionValidation = bSkip; }

	/** Get AlignRotation of currently selected Ring (for gizmo coordinate system) */
	FMatrix GetSelectedRingAlignMatrix() const;

	/** Clear selection */
	void ClearSelection();

	/** Select Ring (called from tree, also highlights attached bone) */
	void SelectRing(int32 RingIndex, FName AttachedBoneName = NAME_None);

	// Show flag toggles
	void ToggleShowSkeletalMesh();
	void ToggleShowRingGizmos() { bShowRingGizmos = !bShowRingGizmos; Invalidate(); }
	void SetRingGizmoThickness(float InThickness) { RingGizmoThickness = FMath::Clamp(InThickness, 0.05f, 1.0f); Invalidate(); }
	float GetRingGizmoThickness() const { return RingGizmoThickness; }
	void ToggleShowRingMeshes();
	void ToggleShowBones() { bShowBones = !bShowBones; Invalidate(); }
	void ToggleShowGrid();

	// Show flag state
	bool ShouldShowSkeletalMesh() const { return bShowSkeletalMesh; }
	bool ShouldShowRingGizmos() const { return bShowRingGizmos; }
	bool ShouldShowRingMeshes() const { return bShowRingMeshes; }
	bool ShouldShowBones() const { return bShowBones; }
	bool ShouldShowGrid() const;

	/** Apply current Show Flags to preview scene (call after refresh) */
	void ApplyShowFlagsToScene();

	// Bone draw option toggles/settings
	void ToggleShowBoneNames() { bShowBoneNames = !bShowBoneNames; Invalidate(); }
	void ToggleShowMultiColorBones() { bShowMultiColorBones = !bShowMultiColorBones; Invalidate(); }
	void SetBoneDrawSize(float InSize) { BoneDrawSize = FMath::Clamp(InSize, 0.1f, 5.0f); Invalidate(); }
	void SetBoneDrawMode(EFleshRingBoneDrawMode::Type InMode);

	/** Update bones to draw bit array (Persona style) */
	void UpdateBonesToDraw();

	// Bone draw option state
	bool ShouldShowBoneNames() const { return bShowBoneNames; }
	bool ShouldShowMultiColorBones() const { return bShowMultiColorBones; }
	float GetBoneDrawSize() const { return BoneDrawSize; }
	EFleshRingBoneDrawMode::Type GetBoneDrawMode() const { return BoneDrawMode; }
	bool IsBoneDrawModeSet(EFleshRingBoneDrawMode::Type InMode) const { return BoneDrawMode == InMode; }

	// Debug visualization toggles
	void ToggleShowDebugVisualization();
	void ToggleShowSdfVolume();
	void ToggleShowAffectedVertices();
	void ToggleShowSDFSlice();
	void ToggleShowBulgeHeatmap();
	void ToggleShowBulgeArrows();
	void ToggleShowBulgeRange();
	void ToggleShowRingSkinSamplingRadius() { bShowRingSkinSamplingRadius = !bShowRingSkinSamplingRadius; Invalidate(); }

	// Debug visualization state
	bool ShouldShowDebugVisualization() const;
	bool ShouldShowSdfVolume() const;
	bool ShouldShowAffectedVertices() const;
	bool ShouldShowSDFSlice() const;
	bool ShouldShowBulgeHeatmap() const;
	bool ShouldShowBulgeArrows() const;
	bool ShouldShowBulgeRange() const;
	bool ShouldShowRingSkinSamplingRadius() const { return bShowRingSkinSamplingRadius; }

	// Debug Slice Z
	int32 GetDebugSliceZ() const;
	void SetDebugSliceZ(int32 NewValue);

	/** Save settings (camera, show flags) */
	void SaveSettings();

	/** Load settings (camera, show flags) */
	void LoadSettings();

	// Local/World coordinate system (custom managed - linked with toolbar button)
	void ToggleLocalCoordSystem();
	void SetLocalCoordSystem(bool bLocal);
	bool IsUsingLocalCoordSystem() const;

	// Bone selection (linked with skeleton tree)
	void SetSelectedBone(FName BoneName);
	void ClearSelectedBone();
	FName GetSelectedBoneName() const { return SelectedBoneName; }

	/** Set bone selection cleared delegate */
	void SetOnBoneSelectionCleared(FOnBoneSelectionCleared InDelegate) { OnBoneSelectionCleared = InDelegate; }

	/** Set Ring selected delegate (called when Ring is picked in viewport) */
	void SetOnRingSelectedInViewport(FOnRingSelectedInViewport InDelegate) { OnRingSelectedInViewport = InDelegate; }

	/** Set Ring deleted delegate (called when Ring is deleted in viewport) */
	void SetOnRingDeletedInViewport(FOnRingDeletedInViewport InDelegate) { OnRingDeletedInViewport = InDelegate; }

	/** Set bone selected delegate (called when bone is picked in viewport) */
	void SetOnBoneSelectedInViewport(FOnBoneSelectedInViewport InDelegate) { OnBoneSelectedInViewport = InDelegate; }

	/** Set Ring add request delegate (called when Ring is added from right-click menu) */
	void SetOnAddRingAtPositionRequested(FOnAddRingAtPositionRequested InDelegate) { OnAddRingAtPositionRequested = InDelegate; }

	/** Show bone right-click context menu */
	void ShowBoneContextMenu(FName BoneName, const FVector2D& ScreenPos);

	/** Callback when Ring add is selected from context menu */
	void OnContextMenu_AddRing();

	/** Calculate bone local offset from screen coordinates (project to bone line) */
	FVector CalculateBoneLocalOffsetFromScreenPos(const FVector2D& ScreenPos, FName BoneName, FRotator* OutLocalRotation = nullptr);

	/** Calculate default Ring rotation for bone (based on weighted children) */
	FRotator CalculateDefaultRingRotationForBone(FName BoneName);

	/** Delete selected Ring */
	void DeleteSelectedRing();

	/** Check if selected Ring can be deleted */
	bool CanDeleteSelectedRing() const;

private:
	/** Invalidate() + Viewport->Draw() helper (immediate rendering even with dropdown open) */
	void InvalidateAndDraw();

	/** Generate per-asset config section name */
	FString GetConfigSectionName() const;

	/** Bone rendering (Persona style) */
	void DrawMeshBones(FPrimitiveDrawInterface* PDI);

	/** Draw Ring gizmos */
	void DrawRingGizmos(FPrimitiveDrawInterface* PDI);

	/** Draw Ring skin sampling radius spheres */
	void DrawRingSkinSamplingRadius(FPrimitiveDrawInterface* PDI);

	/** Preview scene */
	FFleshRingPreviewScene* PreviewScene;

	/** Viewport widget reference */
	TWeakPtr<SFleshRingEditorViewport> ViewportWidget;

	/** Asset being edited */
	TWeakObjectPtr<UFleshRingAsset> EditingAsset;

	/** Current selection type */
	EFleshRingSelectionType SelectionType = EFleshRingSelectionType::None;

	/** Currently selected Virtual Band section (for section picking) */
	EBandSection SelectedSection = EBandSection::None;

	/** Transaction for Undo */
	TUniquePtr<FScopedTransaction> ScopedTransaction;

	/** Initial rotation at drag start (for gizmo fix) */
	FQuat DragStartWorldRotation = FQuat::Identity;

	/** Accumulated delta rotation during drag (gimbal lock prevention) */
	FQuat AccumulatedDeltaRotation = FQuat::Identity;

	bool bIsDraggingRotation = false;

	/** Skip selection validation flag during Undo/Redo */
	bool bSkipSelectionValidation = false;

	// Show flags
	bool bShowSkeletalMesh = true;
	bool bShowRingGizmos = true;
	bool bShowRingMeshes = true;
	bool bShowBones = true;

	// Ring Gizmo visualization settings
	float RingGizmoThickness = 0.5f;

	// Bone draw options
	bool bShowBoneNames = false;
	bool bShowMultiColorBones = false;
	float BoneDrawSize = 1.0f;
	EFleshRingBoneDrawMode::Type BoneDrawMode = EFleshRingBoneDrawMode::All;

	/** Bones to draw bit array (Persona style) */
	TBitArray<> BonesToDraw;

	/** Cached bone HitProxy array (prevent per-frame recreation) */
	TArray<TRefCountPtr<HHitProxy>> CachedBoneHitProxies;

	/** Skeletal mesh at last caching (for change detection) */
	TWeakObjectPtr<USkeletalMesh> CachedSkeletalMesh;

	/** Weighted bone indices cache */
	TSet<int32> WeightedBoneIndices;

	/** Build weighted bone cache */
	void BuildWeightedBoneCache();

	/** Check if bone has weights */
	bool IsBoneWeighted(int32 BoneIndex) const;

	/** Find weighted child bone (returns INDEX_NONE if not found) */
	int32 FindWeightedChildBone(int32 ParentBoneIndex) const;

	/** Recursively check if there's a weighted bone among descendants (same as skeleton tree) */
	bool HasWeightedDescendant(int32 BoneIndex) const;

	/** Return count of weighted direct child bones */
	int32 CountWeightedChildBones(int32 ParentBoneIndex) const;

	// Debug visualization options (cached - save/load independently of component lifecycle)
	bool bCachedShowDebugVisualization = false;
	bool bCachedShowSdfVolume = false;
	bool bCachedShowAffectedVertices = false;
	bool bCachedShowSDFSlice = false;
	bool bCachedShowBulgeHeatmap = false;
	bool bCachedShowBulgeArrows = true;  // Default on
	bool bCachedShowBulgeRange = false;
	bool bShowRingSkinSamplingRadius = false;  // Show ring vertex sampling radius spheres
	int32 CachedDebugSliceZ = 32;

	// Local/World coordinate system flag (custom managed)
	bool bUseLocalCoordSystem = true;

	// Settings loaded flag (load on first Tick)
	bool bSettingsLoaded = false;

	// Selected bone name (linked with skeleton tree)
	FName SelectedBoneName = NAME_None;

	// Bone selection cleared delegate
	FOnBoneSelectionCleared OnBoneSelectionCleared;

	// Ring selected delegate (when picking in viewport)
	FOnRingSelectedInViewport OnRingSelectedInViewport;

	// Ring deleted delegate (when deleting in viewport)
	FOnRingDeletedInViewport OnRingDeletedInViewport;

	// Bone selected delegate (when picking in viewport)
	FOnBoneSelectedInViewport OnBoneSelectedInViewport;

	// Ring add request delegate (called from right-click menu)
	FOnAddRingAtPositionRequested OnAddRingAtPositionRequested;

	// Temp storage for right-click Ring add
	FName PendingRingAddBoneName = NAME_None;
	FVector2D PendingRingAddScreenPos = FVector2D::ZeroVector;

	// Camera focus interpolation
	bool bIsCameraInterpolating = false;
	FVector CameraTargetLocation = FVector::ZeroVector;
	FRotator CameraTargetRotation = FRotator::ZeroRotator;
	float CameraInterpSpeed = 5.0f;  // Interpolation speed (higher = faster)

	// Preview Scene Settings changed delegate handle
	FDelegateHandle AssetViewerSettingsChangedHandle;

	/** Callback when Preview Scene Settings changes */
	void OnAssetViewerSettingsChanged(const FName& PropertyName);

	/** Apply Preview Scene Settings to EngineShowFlags */
	void ApplyPreviewSceneShowFlags();
};
