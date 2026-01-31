// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingEditorViewportClient.h"
#include "FleshRingEdMode.h"
#include "FleshRingPreviewScene.h"
#include "SFleshRingEditorViewport.h"
#include "FleshRingAsset.h"
#include "FleshRingComponent.h"
#include "FleshRingHitProxy.h"
#include "FleshRingMeshComponent.h"
#include "FleshRingTypes.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "EngineUtils.h"
#include "SkeletalDebugRendering.h"
#include "Preferences/PersonaOptions.h"
#include "UnrealWidget.h"
#include "Editor.h"
#include "Settings/LevelEditorViewportSettings.h"
#include "Stats/Stats.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Engine/StaticMesh.h"

// Stat group and counter declarations
DECLARE_STATS_GROUP(TEXT("FleshRingEditor"), STATGROUP_FleshRingEditor, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("Tick"), STAT_FleshRingEditor_Tick, STATGROUP_FleshRingEditor);
DECLARE_CYCLE_STAT(TEXT("Draw"), STAT_FleshRingEditor_Draw, STATGROUP_FleshRingEditor);
DECLARE_CYCLE_STAT(TEXT("DrawRingGizmos"), STAT_FleshRingEditor_DrawRingGizmos, STATGROUP_FleshRingEditor);
DECLARE_CYCLE_STAT(TEXT("InputWidgetDelta"), STAT_FleshRingEditor_InputWidgetDelta, STATGROUP_FleshRingEditor);
DECLARE_CYCLE_STAT(TEXT("UpdateRingTransforms"), STAT_FleshRingEditor_UpdateRingTransforms, STATGROUP_FleshRingEditor);
DECLARE_CYCLE_STAT(TEXT("MarkPackageDirty"), STAT_FleshRingEditor_MarkPackageDirty, STATGROUP_FleshRingEditor);
DECLARE_CYCLE_STAT(TEXT("Invalidate"), STAT_FleshRingEditor_Invalidate, STATGROUP_FleshRingEditor);

IMPLEMENT_HIT_PROXY(HFleshRingGizmoHitProxy, HHitProxy);
IMPLEMENT_HIT_PROXY(HFleshRingAxisHitProxy, HHitProxy);
// HFleshRingMeshHitProxy is IMPLEMENT_HIT_PROXY'd in FleshRingMeshComponent.cpp (Runtime module)
IMPLEMENT_HIT_PROXY(HFleshRingBoneHitProxy, HHitProxy);
IMPLEMENT_HIT_PROXY(HFleshRingBandSectionHitProxy, HHitProxy);

// Config section base for per-asset settings storage
static const FString FleshRingViewportConfigSectionBase = TEXT("FleshRingEditorViewport");

FFleshRingEditorViewportClient::FFleshRingEditorViewportClient(
	FEditorModeTools* InModeTools,
	FFleshRingPreviewScene* InPreviewScene,
	const TWeakPtr<SFleshRingEditorViewport>& InViewportWidget)
	: FEditorViewportClient(InModeTools, InPreviewScene, StaticCastSharedPtr<SEditorViewport>(InViewportWidget.Pin()))
	, PreviewScene(InPreviewScene)
	, ViewportWidget(InViewportWidget)
{
	// Connect ModeTools to Widget (required for ShouldDrawWidget calls)
	if (Widget && ModeTools)
	{
		Widget->SetUsesEditorModeTools(ModeTools.Get());
	}

	// Default camera settings
	SetViewLocation(FVector(-300, 200, 150));
	SetViewRotation(FRotator(-15, -30, 0));

	// Viewport settings
	SetRealtime(true);
	DrawHelper.bDrawGrid = true;
	DrawHelper.bDrawPivot = false;
	DrawHelper.AxesLineThickness = 2.0f;
	DrawHelper.PivotSize = 5.0f;

	// Near clip plane default (prevent clipping when zooming into small objects)
	OverrideNearClipPlane(0.001f);

	// Background settings
	EngineShowFlags.SetGrid(true);
	EngineShowFlags.SetBones(false);  // Disable default bone rendering to draw manually

	// Lighting settings
	EngineShowFlags.SetLighting(true);
	EngineShowFlags.SetPostProcessing(true);

	// Enable stats display (FPS, etc.)
	SetShowStats(true);

	// Register with static instance registry (for type-safe checks)
	GetAllInstances().Add(this);

	// Subscribe to Preview Scene Settings change delegate
	if (UAssetViewerSettings* Settings = UAssetViewerSettings::Get())
	{
		AssetViewerSettingsChangedHandle = Settings->OnAssetViewerSettingsChanged().AddRaw(
			this, &FFleshRingEditorViewportClient::OnAssetViewerSettingsChanged);
	}

	// Apply initial ShowFlags
	ApplyPreviewSceneShowFlags();
}

FFleshRingEditorViewportClient::~FFleshRingEditorViewportClient()
{
	// Unsubscribe from Preview Scene Settings change delegate
	if (UAssetViewerSettings* Settings = UAssetViewerSettings::Get())
	{
		Settings->OnAssetViewerSettingsChanged().Remove(AssetViewerSettingsChangedHandle);
	}

	// Save settings
	SaveSettings();

	// Remove from static instance registry
	GetAllInstances().Remove(this);
}

void FFleshRingEditorViewportClient::ToggleLocalCoordSystem()
{
	// Don't toggle in scale mode (always keep local)
	if (GetWidgetMode() == UE::Widget::WM_Scale)
	{
		return;
	}

	bUseLocalCoordSystem = !bUseLocalCoordSystem;
	Invalidate();
}

bool FFleshRingEditorViewportClient::IsUsingLocalCoordSystem() const
{
	// Always local in scale mode
	if (GetWidgetMode() == UE::Widget::WM_Scale)
	{
		return true;
	}

	return bUseLocalCoordSystem;
}

void FFleshRingEditorViewportClient::SetLocalCoordSystem(bool bLocal)
{
	// Don't change in scale mode (always keep local)
	if (GetWidgetMode() == UE::Widget::WM_Scale)
	{
		return;
	}

	bUseLocalCoordSystem = bLocal;
	Invalidate();
}

void FFleshRingEditorViewportClient::Tick(float DeltaSeconds)
{
	SCOPE_CYCLE_COUNTER(STAT_FleshRingEditor_Tick);
	FEditorViewportClient::Tick(DeltaSeconds);

	// Load saved settings on first Tick (viewport not ready in constructor)
	if (!bSettingsLoaded)
	{
		LoadSettings();
		bSettingsLoaded = true;
	}

	// Preview scene tick
	if (PreviewScene)
	{
		PreviewScene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);

		// Check Deformer pending init state - execute init when mesh is rendered
		if (PreviewScene->IsPendingDeformerInit())
		{
			PreviewScene->ExecutePendingDeformerInit();
		}
	}

	// Camera focus interpolation handling
	if (bIsCameraInterpolating)
	{
		const FVector CurrentLocation = GetViewLocation();
		const FVector NewLocation = FMath::VInterpTo(CurrentLocation, CameraTargetLocation, DeltaSeconds, CameraInterpSpeed);

		SetViewLocation(NewLocation);

		// End interpolation when close enough to target (no forced snap)
		const float DistanceToTarget = FVector::Dist(NewLocation, CameraTargetLocation);
		if (DistanceToTarget < 0.01f)
		{
			bIsCameraInterpolating = false;
		}

		Invalidate();
	}

	// Check if selected Ring was deleted and clear selection
	// (Skip during Undo/Redo - restored in RefreshViewport)
	if (!bSkipSelectionValidation && SelectionType != EFleshRingSelectionType::None && PreviewScene)
	{
		int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();
		bool bSelectionValid = false;

		if (EditingAsset.IsValid() && SelectedIndex >= 0)
		{
			bSelectionValid = EditingAsset->Rings.IsValidIndex(SelectedIndex);
		}

		if (!bSelectionValid)
		{
			ClearSelection();
		}
	}
}

void FFleshRingEditorViewportClient::Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI)
{
	SCOPE_CYCLE_COUNTER(STAT_FleshRingEditor_Draw);
	// Update bones to draw array (Persona style - called every Draw)
	UpdateBonesToDraw();

	FEditorViewportClient::Draw(View, PDI);

	// Bone rendering (only when BoneDrawMode is not None)
	if (BoneDrawMode != EFleshRingBoneDrawMode::None)
	{
		DrawMeshBones(PDI);
	}

	// Draw Ring gizmos
	if (bShowRingGizmos)
	{
		DrawRingGizmos(PDI);
	}

	// Draw Ring skin sampling radius (debug visualization - requires master switch)
	if (bCachedShowDebugVisualization && bShowRingSkinSamplingRadius)
	{
		DrawRingSkinSamplingRadius(PDI);
	}
}

void FFleshRingEditorViewportClient::DrawCanvas(FViewport& InViewport, FSceneView& View, FCanvas& Canvas)
{
	FEditorViewportClient::DrawCanvas(InViewport, View, Canvas);

	// Display bone names (Persona style)
	if (BoneDrawMode != EFleshRingBoneDrawMode::None && bShowBoneNames && PreviewScene)
	{
		UDebugSkelMeshComponent* MeshComponent = PreviewScene->GetSkeletalMeshComponent();
		if (MeshComponent && MeshComponent->GetSkeletalMeshAsset() && MeshComponent->IsRegistered())
		{
			const FReferenceSkeleton& RefSkeleton = MeshComponent->GetReferenceSkeleton();
			const int32 NumBones = RefSkeleton.GetNum();
			const TArray<FTransform>& ComponentSpaceTransforms = MeshComponent->GetComponentSpaceTransforms();

			if (ComponentSpaceTransforms.Num() >= NumBones && BonesToDraw.Num() >= NumBones)
			{
				const int32 HalfX = static_cast<int32>(Viewport->GetSizeXY().X / 2 / GetDPIScale());
				const int32 HalfY = static_cast<int32>(Viewport->GetSizeXY().Y / 2 / GetDPIScale());

				for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
				{
					// Determine bones to draw using BonesToDraw array (same as bone rendering)
					if (!BonesToDraw[BoneIdx])
					{
						continue;
					}

					const FVector BonePos = MeshComponent->GetComponentTransform().TransformPosition(
						ComponentSpaceTransforms[BoneIdx].GetLocation());

					// Convert to screen coordinates via View->Project (Persona method)
					const FPlane Proj = View.Project(BonePos);

					// Hide bones behind camera with proj.W > 0.f check
					if (Proj.W > 0.f)
					{
						const int32 XPos = static_cast<int32>(HalfX + (HalfX * Proj.X));
						const int32 YPos = static_cast<int32>(HalfY + (HalfY * (Proj.Y * -1)));

						const FName BoneName = RefSkeleton.GetBoneName(BoneIdx);
						const FString BoneString = FString::Printf(TEXT("%d: %s"), BoneIdx, *BoneName.ToString());

						// Persona style: white text + black shadow
						FCanvasTextItem TextItem(
							FVector2D(XPos, YPos),
							FText::FromString(BoneString),
							GEngine->GetSmallFont(),
							FColor::White);
						TextItem.EnableShadow(FLinearColor::Black);
						Canvas.DrawItem(TextItem);
					}
				}
			}
		}
	}
}

bool FFleshRingEditorViewportClient::InputKey(const FInputKeyEventArgs& EventArgs)
{
	// Stop focus interpolation on camera control key input
	if (bIsCameraInterpolating && EventArgs.Event == IE_Pressed)
	{
		if (EventArgs.Key == EKeys::RightMouseButton ||
			EventArgs.Key == EKeys::MiddleMouseButton ||
			EventArgs.Key == EKeys::MouseScrollUp ||
			EventArgs.Key == EKeys::MouseScrollDown)
		{
			bIsCameraInterpolating = false;
		}
	}

	if (EventArgs.Event == IE_Pressed)
	{
		// Focus on mesh with F key (ignored during camera manipulation)
		if (EventArgs.Key == EKeys::F && !IsTracking())
		{
			FocusOnMesh();
			return true;
		}

		// Delete selected Ring with Delete key
		if (EventArgs.Key == EKeys::Delete)
		{
			if (CanDeleteSelectedRing())
			{
				DeleteSelectedRing();
				return true;
			}
		}

		// All keyboard shortcuts (QWER, Ctrl+`, number keys, Shift+number, Ctrl+number)
		// are now handled by FFleshRingEditorCommands (global shortcuts)
		// This allows them to work even when viewport doesn't have focus
	}

	return FEditorViewportClient::InputKey(EventArgs);
}

bool FFleshRingEditorViewportClient::InputAxis(const FInputKeyEventArgs& Args)
{
	bool bResult = false;

	if (!bDisableInput && PreviewScene)
	{
		// Call FAdvancedPreviewScene::HandleViewportInput (handles Sky Rotation with K key)
		bResult = PreviewScene->HandleViewportInput(
			Args.Viewport,
			Args.InputDevice,
			Args.Key,
			Args.AmountDepressed,
			Args.DeltaTime,
			Args.NumSamples,
			Args.IsGamepad());

		if (bResult)
		{
			Invalidate();
		}
	}

	// Forward to base class if not handled (includes Light Direction handling with L key)
	if (!bResult)
	{
		bResult = FEditorViewportClient::InputAxis(Args);
	}

	return bResult;
}

void FFleshRingEditorViewportClient::ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY)
{
	if (Key == EKeys::LeftMouseButton && Event == IE_Released)
	{
		if (HitProxy)
		{
			// Virtual Band section click (individual section picking)
			if (HitProxy->IsA(HFleshRingBandSectionHitProxy::StaticGetType()))
			{
				HFleshRingBandSectionHitProxy* SectionProxy = static_cast<HFleshRingBandSectionHitProxy*>(HitProxy);
				if (PreviewScene && EditingAsset.IsValid())
				{
					FScopedTransaction Transaction(NSLOCTEXT("FleshRingEditor", "SelectBandSection", "Select Band Section"));
					EditingAsset->Modify();
					EditingAsset->EditorSelectedRingIndex = SectionProxy->RingIndex;
					EditingAsset->EditorSelectionType = EFleshRingSelectionType::Gizmo;

					PreviewScene->SetSelectedRingIndex(SectionProxy->RingIndex);
					SelectionType = EFleshRingSelectionType::Gizmo;
					SelectedSection = SectionProxy->Section;  // Select section
					Invalidate();

					OnRingSelectedInViewport.ExecuteIfBound(SectionProxy->RingIndex, EFleshRingSelectionType::Gizmo);
				}
				return;
			}
			// Ring gizmo click (entire band or Virtual Ring gizmo)
			if (HitProxy->IsA(HFleshRingGizmoHitProxy::StaticGetType()))
			{
				HFleshRingGizmoHitProxy* GizmoProxy = static_cast<HFleshRingGizmoHitProxy*>(HitProxy);
				if (PreviewScene && EditingAsset.IsValid())
				{
					// Create selection transaction (Undo-able)
					FScopedTransaction Transaction(NSLOCTEXT("FleshRingEditor", "SelectRingGizmo", "Select Ring Gizmo"));
					EditingAsset->Modify();
					EditingAsset->EditorSelectedRingIndex = GizmoProxy->RingIndex;
					EditingAsset->EditorSelectionType = EFleshRingSelectionType::Gizmo;

					PreviewScene->SetSelectedRingIndex(GizmoProxy->RingIndex);
					SelectionType = EFleshRingSelectionType::Gizmo;
					SelectedSection = EBandSection::None;  // Deselect section (entire band)
					Invalidate();

					// Call delegate for tree/detail panel synchronization
					OnRingSelectedInViewport.ExecuteIfBound(GizmoProxy->RingIndex, EFleshRingSelectionType::Gizmo);
				}
				return;
			}
			// Ring mesh click (custom HitProxy) - higher priority than bone (HPP_Foreground)
			else if (HitProxy->IsA(HFleshRingMeshHitProxy::StaticGetType()))
			{
				HFleshRingMeshHitProxy* MeshProxy = static_cast<HFleshRingMeshHitProxy*>(HitProxy);
				if (PreviewScene && EditingAsset.IsValid())
				{
					// Create selection transaction (Undo-able)
					FScopedTransaction Transaction(NSLOCTEXT("FleshRingEditor", "SelectRingMesh", "Select Ring Mesh"));
					EditingAsset->Modify();
					EditingAsset->EditorSelectedRingIndex = MeshProxy->RingIndex;
					EditingAsset->EditorSelectionType = EFleshRingSelectionType::Mesh;

					PreviewScene->SetSelectedRingIndex(MeshProxy->RingIndex);
					SelectionType = EFleshRingSelectionType::Mesh;
					Invalidate();

					// Call delegate for tree/detail panel synchronization
					OnRingSelectedInViewport.ExecuteIfBound(MeshProxy->RingIndex, EFleshRingSelectionType::Mesh);
				}
				return;
			}

			// Bone click handling (lower priority than Ring picking - HPP_World)
			if (HitProxy->IsA(HFleshRingBoneHitProxy::StaticGetType()))
			{
				HFleshRingBoneHitProxy* BoneProxy = static_cast<HFleshRingBoneHitProxy*>(HitProxy);
				FName ClickedBoneName = BoneProxy->BoneName;

				// Clear existing Ring selection
				if (PreviewScene)
				{
					PreviewScene->SetSelectedRingIndex(INDEX_NONE);
				}
				SelectionType = EFleshRingSelectionType::None;

				if (EditingAsset.IsValid())
				{
					EditingAsset->EditorSelectedRingIndex = INDEX_NONE;
					EditingAsset->EditorSelectionType = EFleshRingSelectionType::None;
				}

				// Select bone
				SetSelectedBone(ClickedBoneName);

				// Call skeleton tree synchronization delegate
				OnBoneSelectedInViewport.ExecuteIfBound(ClickedBoneName);

				Invalidate();
				return;
			}
		}

		// Empty space click - clear selection (Ring + bone)
		ClearSelection();
		ClearSelectedBone();
	}

	// Right-click handling - show context menu
	if (Key == EKeys::RightMouseButton && Event == IE_Released)
	{
		FName TargetBoneName = NAME_None;

		// 1. Right-click on bone - use that bone
		if (HitProxy && HitProxy->IsA(HFleshRingBoneHitProxy::StaticGetType()))
		{
			HFleshRingBoneHitProxy* BoneProxy = static_cast<HFleshRingBoneHitProxy*>(HitProxy);
			TargetBoneName = BoneProxy->BoneName;
			SetSelectedBone(TargetBoneName);
		}
		// 2. Right-click on empty space with bone selected - use selected bone
		else if (!SelectedBoneName.IsNone())
		{
			TargetBoneName = SelectedBoneName;
		}

		// Show context menu if target bone exists
		if (!TargetBoneName.IsNone())
		{
			PendingRingAddBoneName = TargetBoneName;
			PendingRingAddScreenPos = FVector2D(static_cast<float>(HitX), static_cast<float>(HitY));
			ShowBoneContextMenu(TargetBoneName, FVector2D(static_cast<float>(HitX), static_cast<float>(HitY)));
			return;
		}
	}

	FEditorViewportClient::ProcessClick(View, HitProxy, Key, Event, HitX, HitY);
}

void FFleshRingEditorViewportClient::ClearSelection()
{
	// Skip clearing selection during Undo/Redo (protected wherever called)
	if (bSkipSelectionValidation)
	{
		return;
	}

	// Skip if already deselected (avoid unnecessary transactions)
	if (SelectionType == EFleshRingSelectionType::None)
	{
		return;
	}

	// Create deselection transaction (Undo-able)
	if (EditingAsset.IsValid())
	{
		FScopedTransaction Transaction(NSLOCTEXT("FleshRingEditor", "ClearRingSelection", "Clear Ring Selection"));
		EditingAsset->Modify();
		EditingAsset->EditorSelectedRingIndex = -1;
		EditingAsset->EditorSelectionType = EFleshRingSelectionType::None;
	}

	if (PreviewScene)
	{
		PreviewScene->SetSelectedRingIndex(-1);
	}
	SelectionType = EFleshRingSelectionType::None;
	Invalidate();
}

bool FFleshRingEditorViewportClient::CanDeleteSelectedRing() const
{
	if (!EditingAsset.IsValid() || !PreviewScene)
	{
		return false;
	}

	// Ring must be selected
	if (SelectionType == EFleshRingSelectionType::None)
	{
		return false;
	}

	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();
	return EditingAsset->Rings.IsValidIndex(SelectedIndex);
}

void FFleshRingEditorViewportClient::DeleteSelectedRing()
{
	if (!CanDeleteSelectedRing())
	{
		return;
	}

	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();

	// Undo/Redo support
	// Limit transaction scope so RefreshPreview() is called outside transaction
	// (Prevent Undo crash when PreviewSubdividedMesh is created during transaction)
	{
		FScopedTransaction Transaction(NSLOCTEXT("FleshRingEditor", "DeleteRing", "Delete Ring"));
		EditingAsset->Modify();

		// Delete Ring
		EditingAsset->Rings.RemoveAt(SelectedIndex);

		// Clear selection (Transient removed so properly restored on Undo)
		EditingAsset->EditorSelectedRingIndex = -1;
		EditingAsset->EditorSelectionType = EFleshRingSelectionType::None;
	}

	PreviewScene->SetSelectedRingIndex(-1);
	SelectionType = EFleshRingSelectionType::None;

	// Call delegate (tree refresh)
	// Called after transaction ends so mesh creation isn't included in Undo history
	OnRingDeletedInViewport.ExecuteIfBound();

	Invalidate();
}

void FFleshRingEditorViewportClient::SelectRing(int32 RingIndex, FName AttachedBoneName)
{
	if (RingIndex < 0)
	{
		// Skip if already deselected (avoid duplicate transactions - when called from RefreshTree, etc.)
		if (EditingAsset.IsValid() && EditingAsset->EditorSelectionType == EFleshRingSelectionType::None)
		{
			return;
		}
		// Negative index = clear Ring selection (keep bone selection)
		ClearSelection();
		return;
	}

	// Skip if same Ring already selected with same type (avoid duplicate transactions)
	if (EditingAsset.IsValid() &&
		EditingAsset->EditorSelectedRingIndex == RingIndex &&
		EditingAsset->EditorSelectionType != EFleshRingSelectionType::None)
	{
		// Only update bone highlight
		SelectedBoneName = AttachedBoneName;
		Invalidate();
		return;
	}

	// Determine SelectionType based on RingMesh presence
	// If no RingMesh, select Gizmo (virtual ring/band), otherwise select Mesh
	EFleshRingSelectionType NewSelectionType = EFleshRingSelectionType::Mesh;
	if (EditingAsset.IsValid() && EditingAsset->Rings.IsValidIndex(RingIndex))
	{
		const FFleshRingSettings& Ring = EditingAsset->Rings[RingIndex];
		if (Ring.RingMesh.IsNull())
		{
			NewSelectionType = EFleshRingSelectionType::Gizmo;
		}
	}

	// Create selection transaction (Undo-able)
	if (EditingAsset.IsValid())
	{
		FScopedTransaction Transaction(NSLOCTEXT("FleshRingEditor", "SelectRing", "Select Ring"));
		EditingAsset->Modify();
		EditingAsset->EditorSelectedRingIndex = RingIndex;
		EditingAsset->EditorSelectionType = NewSelectionType;
	}

	// Highlight bone that Ring is attached to (set directly without delegate call)
	SelectedBoneName = AttachedBoneName;

	if (PreviewScene)
	{
		PreviewScene->SetSelectedRingIndex(RingIndex);
	}
	SelectionType = NewSelectionType;
	Invalidate();
}

void FFleshRingEditorViewportClient::SetAsset(UFleshRingAsset* InAsset)
{
	EditingAsset = InAsset;

	if (PreviewScene && InAsset)
	{
		// Set Asset (mesh + component + Ring visualization)
		PreviewScene->SetFleshRingAsset(InAsset);

		// Update bones to draw array
		UpdateBonesToDraw();

		// Camera focus
		FocusOnMesh();
	}
}

void FFleshRingEditorViewportClient::SetSelectedBone(FName BoneName)
{
	SelectedBoneName = BoneName;
	UpdateBonesToDraw();
	Invalidate();
}

void FFleshRingEditorViewportClient::ClearSelectedBone()
{
	SelectedBoneName = NAME_None;
	UpdateBonesToDraw();
	Invalidate();

	// Notify skeleton tree
	OnBoneSelectionCleared.ExecuteIfBound();
}

void FFleshRingEditorViewportClient::FocusOnMesh()
{
	if (!PreviewScene)
	{
		return;
	}

	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
	{
		return;
	}

	FBox FocusBox(ForceInit);
	FName BoneToFocus = SelectedBoneName;  // Bone to focus (local variable)

	// 1. Focus on Ring if a Ring is selected
	int32 SelectedRingIndex = PreviewScene->GetSelectedRingIndex();
	if (SelectedRingIndex >= 0 && EditingAsset.IsValid() && EditingAsset->Rings.IsValidIndex(SelectedRingIndex))
	{
		const FFleshRingSettings& Ring = EditingAsset->Rings[SelectedRingIndex];
		int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
		if (BoneIndex != INDEX_NONE)
		{
			FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);

			// Focus on mesh bounds if Ring mesh exists
			if (Ring.RingMesh)
			{
				// Get mesh bounds
				FBoxSphereBounds MeshBounds = Ring.RingMesh->GetBounds();

				// Apply scale
				FVector ScaledExtent = MeshBounds.BoxExtent * Ring.MeshScale;
				float BoxExtent = ScaledExtent.GetMax();
				BoxExtent = FMath::Max(BoxExtent, 15.0f);

				// Ring mesh position (with MeshOffset applied)
				FVector RingCenter = BoneTransform.GetLocation() + BoneTransform.GetRotation().RotateVector(Ring.MeshOffset);
				FocusBox = FBox(RingCenter - FVector(BoxExtent), RingCenter + FVector(BoxExtent));
			}
			else
			{
				// Focus on bone if no Ring mesh
				BoneToFocus = Ring.BoneName;
			}
		}
	}
	// 2. Focus on bone if a bone is selected
	if (!FocusBox.IsValid && !BoneToFocus.IsNone())
	{
		int32 BoneIndex = SkelMeshComp->GetBoneIndex(BoneToFocus);
		if (BoneIndex != INDEX_NONE)
		{
			FVector BoneLocation = SkelMeshComp->GetBoneTransform(BoneIndex).GetLocation();

			// Estimate bone size (distance to child bones)
			float BoxExtent = 15.0f;

			const FReferenceSkeleton& RefSkel = SkelMeshComp->GetSkeletalMeshAsset()->GetRefSkeleton();
			for (int32 i = 0; i < RefSkel.GetNum(); ++i)
			{
				if (RefSkel.GetParentIndex(i) == BoneIndex)
				{
					FVector ChildLocation = SkelMeshComp->GetBoneTransform(i).GetLocation();
					float DistToChild = FVector::Dist(BoneLocation, ChildLocation);
					BoxExtent = FMath::Max(BoxExtent, DistToChild * 0.5f);
				}
			}

			FocusBox = FBox(BoneLocation - FVector(BoxExtent), BoneLocation + FVector(BoxExtent));
		}
	}

	// Use entire mesh bounds if box is not valid (Persona method)
	if (!FocusBox.IsValid)
	{
		// Persona method: use SkeletalMesh's GetBounds().GetBox()
		USkeletalMesh* SkelMesh = SkelMeshComp->GetSkeletalMeshAsset();
		if (SkelMesh)
		{
			FocusBox = SkelMesh->GetBounds().GetBox();
		}
	}

	// Calculate target position/rotation (reference FocusViewportOnBox logic)
	const FVector BoxCenter = FocusBox.GetCenter();
	const FVector BoxExtent = FocusBox.GetExtent();
	const float BoxRadius = BoxExtent.Size();

	// Keep current camera direction, only adjust distance
	const FRotator CurrentRotation = GetViewRotation();
	const FVector ViewDirection = CurrentRotation.Vector();

	// Calculate appropriate distance (considering FOV)
	const float HalfFOVRadians = FMath::DegreesToRadians(ViewFOV * 0.5f);
	const float DistanceToFit = BoxRadius / FMath::Tan(HalfFOVRadians);

	// Set target position
	CameraTargetLocation = BoxCenter - ViewDirection * DistanceToFit * 1.5f;  // 1.5x margin
	CameraTargetRotation = CurrentRotation;

	// Start interpolation
	bIsCameraInterpolating = true;
}

void FFleshRingEditorViewportClient::DrawMeshBones(FPrimitiveDrawInterface* PDI)
{
	if (!PreviewScene)
	{
		return;
	}

	UDebugSkelMeshComponent* MeshComponent = PreviewScene->GetSkeletalMeshComponent();
	if (!MeshComponent || !MeshComponent->GetSkeletalMeshAsset())
	{
		return;
	}

	// Skip if component is not registered
	if (!MeshComponent->IsRegistered())
	{
		return;
	}

	if (MeshComponent->SkeletonDrawMode == ESkeletonDrawMode::Hidden)
	{
		return;
	}

	const FReferenceSkeleton& RefSkeleton = MeshComponent->GetReferenceSkeleton();
	const int32 NumBones = RefSkeleton.GetNum();

	// Return if no bones
	if (NumBones == 0)
	{
		return;
	}

	// Check if skeletal mesh is fully loaded
	const TArray<FTransform>& ComponentSpaceTransforms = MeshComponent->GetComponentSpaceTransforms();
	if (ComponentSpaceTransforms.Num() < NumBones)
	{
		return;
	}

	// Create world Transform array (directly using ComponentSpaceTransforms)
	TArray<FTransform> WorldTransforms;
	WorldTransforms.SetNum(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		WorldTransforms[i] = ComponentSpaceTransforms[i] * MeshComponent->GetComponentTransform();
	}

	// Bone indices to draw (all bones)
	TArray<FBoneIndexType> AllBoneIndices;
	AllBoneIndices.SetNum(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		AllBoneIndices[i] = static_cast<FBoneIndexType>(i);
	}

	// Create bone color array (auto-generated when using multi-colors)
	TArray<FLinearColor> BoneColors;
	BoneColors.SetNum(NumBones);
	if (bShowMultiColorBones)
	{
		SkeletalDebugRendering::FillWithMultiColors(BoneColors, NumBones);
	}
	else
	{
		for (int32 i = 0; i < NumBones; ++i)
		{
			BoneColors[i] = MeshComponent->GetBoneColor(i);
		}
	}

	// Create selected bone index array
	TArray<int32> SelectedBones;
	if (!SelectedBoneName.IsNone())
	{
		int32 SelectedBoneIndex = RefSkeleton.FindBoneIndex(SelectedBoneName);
		if (SelectedBoneIndex != INDEX_NONE)
		{
			SelectedBones.Add(SelectedBoneIndex);
		}
	}

	// Bone picking HitProxy array (cached - only recreated when skeletal mesh changes)
	USkeletalMesh* CurrentSkelMesh = MeshComponent->GetSkeletalMeshAsset();
	if (CachedSkeletalMesh.Get() != CurrentSkelMesh || CachedBoneHitProxies.Num() != NumBones)
	{
		CachedSkeletalMesh = CurrentSkelMesh;
		CachedBoneHitProxies.SetNum(NumBones);
		for (int32 i = 0; i < NumBones; ++i)
		{
			CachedBoneHitProxies[i] = new HFleshRingBoneHitProxy(i, RefSkeleton.GetBoneName(i));
		}
	}

	// Convert EFleshRingBoneDrawMode to EBoneDrawMode
	EBoneDrawMode::Type EngineBoneDrawMode = EBoneDrawMode::All;
	switch (BoneDrawMode)
	{
	case EFleshRingBoneDrawMode::None:
		EngineBoneDrawMode = EBoneDrawMode::None;
		break;
	case EFleshRingBoneDrawMode::Selected:
		EngineBoneDrawMode = EBoneDrawMode::Selected;
		break;
	case EFleshRingBoneDrawMode::SelectedAndParents:
		EngineBoneDrawMode = EBoneDrawMode::SelectedAndParents;
		break;
	case EFleshRingBoneDrawMode::SelectedAndChildren:
		EngineBoneDrawMode = EBoneDrawMode::SelectedAndChildren;
		break;
	case EFleshRingBoneDrawMode::SelectedAndParentsAndChildren:
		EngineBoneDrawMode = EBoneDrawMode::SelectedAndParentsAndChildren;
		break;
	case EFleshRingBoneDrawMode::All:
	default:
		EngineBoneDrawMode = EBoneDrawMode::All;
		break;
	}

	// DrawConfig setup
	FSkelDebugDrawConfig DrawConfig;
	DrawConfig.BoneDrawMode = EngineBoneDrawMode;
	DrawConfig.BoneDrawSize = BoneDrawSize;
	DrawConfig.bForceDraw = false;
	DrawConfig.bAddHitProxy = true;  // Enable bone picking
	DrawConfig.bUseMultiColorAsDefaultColor = bShowMultiColorBones;
	DrawConfig.DefaultBoneColor = GetDefault<UPersonaOptions>()->DefaultBoneColor;
	DrawConfig.SelectedBoneColor = GetDefault<UPersonaOptions>()->SelectedBoneColor;  // Green
	DrawConfig.AffectedBoneColor = GetDefault<UPersonaOptions>()->AffectedBoneColor;
	DrawConfig.ParentOfSelectedBoneColor = GetDefault<UPersonaOptions>()->ParentOfSelectedBoneColor;  // Yellow

	// Bone rendering (using BonesToDraw member variable - Persona style)
	SkeletalDebugRendering::DrawBones(
		PDI,
		MeshComponent->GetComponentLocation(),
		AllBoneIndices,
		RefSkeleton,
		WorldTransforms,
		SelectedBones,  // Pass selected bones (green + yellow parent connection lines)
		BoneColors,
		CachedBoneHitProxies,  // Cached bone picking HitProxy array
		DrawConfig,
		BonesToDraw
	);
}

void FFleshRingEditorViewportClient::DrawRingGizmos(FPrimitiveDrawInterface* PDI)
{
	SCOPE_CYCLE_COUNTER(STAT_FleshRingEditor_DrawRingGizmos);
	if (!PreviewScene || !EditingAsset.IsValid())
	{
		return;
	}

	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
	{
		return;
	}

	const TArray<FFleshRingSettings>& Rings = EditingAsset->Rings;
	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();

	for (int32 i = 0; i < Rings.Num(); ++i)
	{
		const FFleshRingSettings& Ring = Rings[i];

		// Skip hidden Rings (Gizmo)
		if (!Ring.bEditorVisible)
		{
			continue;
		}

		// Get bone Transform
		int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			continue;
		}

		FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);
		FVector BoneLocation = BoneTransform.GetLocation();
		FQuat BoneRotation = BoneTransform.GetRotation();

		// Ring mesh picking area (applies in all modes, only when mesh exists)
		UStaticMesh* RingMesh = Ring.RingMesh.LoadSynchronous();
		if (RingMesh)
		{
			PDI->SetHitProxy(new HFleshRingMeshHitProxy(i));

			// Calculate mesh position
			FVector MeshLocation = BoneLocation + BoneRotation.RotateVector(Ring.MeshOffset);

			// Picking area size based on mesh bounds
			FBoxSphereBounds MeshBounds = RingMesh->GetBounds();
			float MeshRadius = MeshBounds.SphereRadius * FMath::Max3(Ring.MeshScale.X, Ring.MeshScale.Y, Ring.MeshScale.Z);

			// Set picking area with invisible sphere (SDPG_World to be behind bones)
			DrawWireSphere(PDI, MeshLocation, FLinearColor(0, 0, 0, 0), MeshRadius, 8, SDPG_World);

			PDI->SetHitProxy(nullptr);
		}

		// Only show Ring gizmo in VirtualRing mode (Radius meaningless in SDF mode)
		if (Ring.InfluenceMode != EFleshRingInfluenceMode::VirtualRing)
		{
			// VirtualBand mode: 4-layer band gizmo
			if (Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
			{
				FLinearColor GizmoColor = (i == SelectedIndex)
					? ((SelectionType == EFleshRingSelectionType::Gizmo) ? FLinearColor::Yellow : FLinearColor(1.0f, 0.0f, 1.0f, 1.0f))
					: FLinearColor(0.0f, 1.0f, 1.0f, 0.8f);

				USkeletalMesh* SkelMesh = SkelMeshComp->GetSkeletalMeshAsset();
				const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
				FTransform BindPoseBoneTransform = FTransform::Identity;
				int32 CurrentBoneIdx = BoneIndex;
				while (CurrentBoneIdx != INDEX_NONE)
				{
					BindPoseBoneTransform = BindPoseBoneTransform * RefSkeleton.GetRefBonePose()[CurrentBoneIdx];
					CurrentBoneIdx = RefSkeleton.GetParentIndex(CurrentBoneIdx);
				}

				// Virtual Band gizmo: use dedicated BandOffset/BandRotation
				const FVirtualBandSettings& Band = Ring.VirtualBand;
				FTransform BandTransform;
				BandTransform.SetLocation(Band.BandOffset);
				BandTransform.SetRotation(Band.BandRotation);
				BandTransform.SetScale3D(FVector::OneVector);  // Band gizmo has no scale

				FTransform LocalToWorld = BandTransform * BindPoseBoneTransform * SkelMeshComp->GetComponentTransform();
				constexpr int32 Segments = 32;
				constexpr float HeightEpsilon = 0.0001f;
				const bool bHasLowerSection = (Band.Lower.Height > HeightEpsilon);
				const bool bHasUpperSection = (Band.Upper.Height > HeightEpsilon);

				// New coordinate system: Z=0 is Mid Band center
				const float MidOffset = Band.Lower.Height + Band.BandHeight * 0.5f;
				float LowerZ = -MidOffset;  // Lower bottom
				float BandLowerZ = -Band.BandHeight * 0.5f;  // Band bottom
				float BandUpperZ = Band.BandHeight * 0.5f;   // Band top
				float UpperZ = Band.Upper.Height + Band.BandHeight * 0.5f;  // Upper top

				// Section color determination function
				auto GetSectionColor = [&](EBandSection Section) -> FLinearColor {
					if (i == SelectedIndex && SelectedSection == Section)
					{
						return FLinearColor::Green;  // Selected section: green
					}
					return GizmoColor;  // Default color
				};

				// Section circle drawing function (including HitProxy)
				auto DrawSectionCircle = [&](float R, float Z, float T, EBandSection Section) {
					PDI->SetHitProxy(new HFleshRingBandSectionHitProxy(i, Section));
					FLinearColor Color = GetSectionColor(Section);
					for (int32 s = 0; s < Segments; ++s) {
						float A1 = (float)s / Segments * 2.0f * PI, A2 = (float)(s + 1) / Segments * 2.0f * PI;
						PDI->DrawLine(LocalToWorld.TransformPosition(FVector(FMath::Cos(A1) * R, FMath::Sin(A1) * R, Z)),
							LocalToWorld.TransformPosition(FVector(FMath::Cos(A2) * R, FMath::Sin(A2) * R, Z)), Color, SDPG_Foreground, T);
					}
					PDI->SetHitProxy(nullptr);
				};

				// Skip sections with Height=0 and only use Mid values
				const float SectionLineThickness = RingGizmoThickness;
				if (bHasLowerSection)
				{
					DrawSectionCircle(Band.Lower.Radius, LowerZ, SectionLineThickness, EBandSection::Lower);
				}
				DrawSectionCircle(Band.MidLowerRadius, BandLowerZ, SectionLineThickness, EBandSection::MidLower);
				DrawSectionCircle(Band.MidUpperRadius, BandUpperZ, SectionLineThickness, EBandSection::MidUpper);
				if (bHasUpperSection)
				{
					DrawSectionCircle(Band.Upper.Radius, UpperZ, SectionLineThickness, EBandSection::Upper);
				}

				// Vertical connection lines (using entire gizmo HitProxy)
				PDI->SetHitProxy(new HFleshRingGizmoHitProxy(i));
				for (int32 q = 0; q < 4; ++q) {
					float Angle = (float)q / 4.0f * 2.0f * PI;
					FVector Dir(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
					// Lower → MidLower (only if Lower section exists)
					if (bHasLowerSection)
					{
						PDI->DrawLine(LocalToWorld.TransformPosition(Dir * Band.Lower.Radius + FVector(0, 0, LowerZ)),
							LocalToWorld.TransformPosition(Dir * Band.MidLowerRadius + FVector(0, 0, BandLowerZ)), GizmoColor, SDPG_Foreground, 0.0f);
					}
					// MidLower → MidUpper (always)
					PDI->DrawLine(LocalToWorld.TransformPosition(Dir * Band.MidLowerRadius + FVector(0, 0, BandLowerZ)),
						LocalToWorld.TransformPosition(Dir * Band.MidUpperRadius + FVector(0, 0, BandUpperZ)), GizmoColor, SDPG_Foreground, 0.0f);
					// MidUpper → Upper (only if Upper section exists)
					if (bHasUpperSection)
					{
						PDI->DrawLine(LocalToWorld.TransformPosition(Dir * Band.MidUpperRadius + FVector(0, 0, BandUpperZ)),
							LocalToWorld.TransformPosition(Dir * Band.Upper.Radius + FVector(0, 0, UpperZ)), GizmoColor, SDPG_Foreground, 0.0f);
					}
				}
				DrawWireSphere(PDI, LocalToWorld.TransformPosition(FVector::ZeroVector), GizmoColor, 2.0f, 8, SDPG_Foreground);
				PDI->SetHitProxy(nullptr);
			}
			continue;
		}

		// Bone rotation * Ring rotation = world rotation (by default, bone X-axis aligns with Ring Z-axis)
		FQuat RingWorldRotation = BoneRotation * Ring.RingRotation;

		// Apply RingOffset
		FVector GizmoLocation = BoneLocation + BoneRotation.RotateVector(Ring.RingOffset);

		// Determine Ring color
		FLinearColor GizmoColor;
		if (i == SelectedIndex)
		{
			// Selected Ring: Gizmo=yellow, Mesh=magenta
			GizmoColor = (SelectionType == EFleshRingSelectionType::Gizmo)
				? FLinearColor::Yellow
				: FLinearColor(1.0f, 0.0f, 1.0f, 1.0f); // Magenta (when Mesh selected)
		}
		else
		{
			// Unselected Ring: cyan
			GizmoColor = FLinearColor(0.0f, 1.0f, 1.0f, 0.8f);
		}

		// Set HitProxy (for Ring gizmo)
		PDI->SetHitProxy(new HFleshRingGizmoHitProxy(i));

		// Ring band visualization (rectangular cross-section torus = Hollow Cylinder)
		// RingRadius = inner radius (surface where ring presses flesh)
		// RingThickness = wall thickness (inward→outward one direction)
		float InnerRadius = Ring.RingRadius;
		float OuterRadius = Ring.RingRadius + Ring.RingThickness;
		float HalfHeight = Ring.RingHeight / 2.0f;

		// Fill top/bottom faces (seamlessly with radial lines)
		int32 FillSegments = 360;  // Dense radial lines
		float ZOffsets[2] = { -HalfHeight, HalfHeight };

		for (float ZOffset : ZOffsets)
		{
			for (int32 s = 0; s < FillSegments; ++s)
			{
				float Angle = (float)s / FillSegments * 2.0f * PI;
				FVector Dir(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);

				FVector InnerPt = GizmoLocation + RingWorldRotation.RotateVector(Dir * InnerRadius + FVector(0, 0, ZOffset));
				FVector OuterPt = GizmoLocation + RingWorldRotation.RotateVector(Dir * OuterRadius + FVector(0, 0, ZOffset));
				PDI->DrawLine(InnerPt, OuterPt, GizmoColor, SDPG_Foreground, 0.0f);  // Minimum thickness
			}
		}

		// Inner/outer circle border lines (top/bottom)
		int32 CircleSegments = 64;
		float Radii[2] = { InnerRadius, OuterRadius };
		for (float Radius : Radii)
		{
			for (float ZOffset : ZOffsets)
			{
				for (int32 s = 0; s < CircleSegments; ++s)
				{
					float Angle1 = (float)s / CircleSegments * 2.0f * PI;
					float Angle2 = (float)(s + 1) / CircleSegments * 2.0f * PI;

					FVector P1 = GizmoLocation + RingWorldRotation.RotateVector(
						FVector(FMath::Cos(Angle1) * Radius, FMath::Sin(Angle1) * Radius, ZOffset));
					FVector P2 = GizmoLocation + RingWorldRotation.RotateVector(
						FVector(FMath::Cos(Angle2) * Radius, FMath::Sin(Angle2) * Radius, ZOffset));
					PDI->DrawLine(P1, P2, GizmoColor, SDPG_Foreground, 0.0f);
				}
			}
		}

		// Vertical connection lines (4 directions)
		for (int32 q = 0; q < 4; ++q)
		{
			float Angle = (float)q / 4.0f * 2.0f * PI;
			FVector Dir(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);

			FVector InnerBottom = GizmoLocation + RingWorldRotation.RotateVector(Dir * InnerRadius + FVector(0, 0, -HalfHeight));
			FVector InnerTop = GizmoLocation + RingWorldRotation.RotateVector(Dir * InnerRadius + FVector(0, 0, HalfHeight));
			PDI->DrawLine(InnerBottom, InnerTop, GizmoColor, SDPG_Foreground, 0.0f);

			FVector OuterBottom = GizmoLocation + RingWorldRotation.RotateVector(Dir * OuterRadius + FVector(0, 0, -HalfHeight));
			FVector OuterTop = GizmoLocation + RingWorldRotation.RotateVector(Dir * OuterRadius + FVector(0, 0, HalfHeight));
			PDI->DrawLine(OuterBottom, OuterTop, GizmoColor, SDPG_Foreground, 0.0f);
		}

		// Display small sphere at bone position
		DrawWireSphere(PDI, GizmoLocation, GizmoColor, 2.0f, 8, SDPG_Foreground);

		// Clear HitProxy
		PDI->SetHitProxy(nullptr);
	}
}

void FFleshRingEditorViewportClient::DrawRingSkinSamplingRadius(FPrimitiveDrawInterface* PDI)
{
	if (!PreviewScene || !EditingAsset.IsValid())
	{
		return;
	}

	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
	{
		return;
	}

	const TArray<FFleshRingSettings>& Rings = EditingAsset->Rings;
	const int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();

	// Color for sampling radius visualization
	const FLinearColor SamplingRadiusColor(0.2f, 0.8f, 0.2f, 0.5f);  // Green with alpha
	const FLinearColor SelectedRadiusColor(1.0f, 0.5f, 0.0f, 0.7f);  // Orange for selected

	for (int32 i = 0; i < Rings.Num(); ++i)
	{
		const FFleshRingSettings& Ring = Rings[i];

		// Skip if skinned ring mesh is disabled
		if (!Ring.bGenerateSkinnedRingMesh)
		{
			continue;
		}

		// Skip hidden Rings
		if (!Ring.bEditorVisible)
		{
			continue;
		}

		// Load ring mesh
		UStaticMesh* RingMesh = Ring.RingMesh.LoadSynchronous();
		if (!RingMesh)
		{
			continue;
		}

		// Get bone Transform
		int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			continue;
		}

		FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);

		// Calculate ring mesh transform (same as runtime)
		FTransform MeshTransform;
		MeshTransform.SetLocation(Ring.MeshOffset);
		MeshTransform.SetRotation(Ring.MeshRotation);
		MeshTransform.SetScale3D(Ring.MeshScale);
		FTransform RingWorldTransform = MeshTransform * BoneTransform;

		// Get ring mesh vertex positions
		const FStaticMeshLODResources& LODResource = RingMesh->GetRenderData()->LODResources[0];
		const FPositionVertexBuffer& PositionBuffer = LODResource.VertexBuffers.PositionVertexBuffer;
		const uint32 NumVertices = PositionBuffer.GetNumVertices();

		// Sampling radius from ring settings
		const float Radius = Ring.RingSkinSamplingRadius;
		const FLinearColor& Color = (i == SelectedIndex) ? SelectedRadiusColor : SamplingRadiusColor;

		// Draw wireframe sphere at each vertex position
		for (uint32 VertIdx = 0; VertIdx < NumVertices; ++VertIdx)
		{
			FVector LocalPos = FVector(PositionBuffer.VertexPosition(VertIdx));
			FVector WorldPos = RingWorldTransform.TransformPosition(LocalPos);

			// Draw wireframe sphere
			DrawWireSphere(PDI, WorldPos, Color, Radius, 12, SDPG_World);
		}
	}
}

FVector FFleshRingEditorViewportClient::GetWidgetLocation() const
{
	if (!PreviewScene || !EditingAsset.IsValid())
	{
		return FVector::ZeroVector;
	}

	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();
	if (SelectedIndex < 0 || SelectionType == EFleshRingSelectionType::None)
	{
		return FVector::ZeroVector;
	}

	const TArray<FFleshRingSettings>& Rings = EditingAsset->Rings;
	if (!Rings.IsValidIndex(SelectedIndex))
	{
		return FVector::ZeroVector;
	}

	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
	{
		return FVector::ZeroVector;
	}

	const FFleshRingSettings& Ring = Rings[SelectedIndex];
	int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return FVector::ZeroVector;
	}

	// VirtualBand mode: use bind pose (consistent with SDF/deformation)
	if (Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
	{
		USkeletalMesh* SkelMesh = SkelMeshComp->GetSkeletalMeshAsset();
		const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
		FTransform BindPoseBoneTransform = FTransform::Identity;
		int32 CurrentBoneIdx = BoneIndex;
		while (CurrentBoneIdx != INDEX_NONE)
		{
			BindPoseBoneTransform = BindPoseBoneTransform * RefSkeleton.GetRefBonePose()[CurrentBoneIdx];
			CurrentBoneIdx = RefSkeleton.GetParentIndex(CurrentBoneIdx);
		}

		// Apply different offset based on SelectionType
		// Gizmo selection → use BandOffset (band transform)
		// Mesh selection → use MeshOffset (mesh transform)
		const FVirtualBandSettings& Band = Ring.VirtualBand;
		FTransform LocalTransform;
		if (SelectionType == EFleshRingSelectionType::Gizmo)
		{
			// Calculate section Z offset (new coordinate system: Z=0 is Mid Band center)
			float SectionZ = 0.0f;
			if (SelectedSection == EBandSection::Upper)
			{
				SectionZ = Band.Upper.Height + Band.BandHeight * 0.5f;
			}
			else if (SelectedSection == EBandSection::MidUpper)
			{
				SectionZ = Band.BandHeight * 0.5f;
			}
			else if (SelectedSection == EBandSection::MidLower)
			{
				SectionZ = -Band.BandHeight * 0.5f;
			}
			else if (SelectedSection == EBandSection::Lower)
			{
				SectionZ = -(Band.Lower.Height + Band.BandHeight * 0.5f);
			}
			// else: EBandSection::None → SectionZ = 0 (band center)

			// Convert section Z from band local coordinates to offset
			FVector SectionOffset = Band.BandRotation.RotateVector(FVector(0, 0, SectionZ));
			LocalTransform.SetLocation(Band.BandOffset + SectionOffset);
			LocalTransform.SetRotation(Band.BandRotation);
			LocalTransform.SetScale3D(FVector::OneVector);  // Band has no scale
		}
		else
		{
			LocalTransform.SetLocation(Ring.MeshOffset);
			LocalTransform.SetRotation(Ring.MeshRotation);
			LocalTransform.SetScale3D(Ring.MeshScale);
		}
		FTransform LocalToWorld = LocalTransform * BindPoseBoneTransform * SkelMeshComp->GetComponentTransform();
		return LocalToWorld.GetLocation();
	}

	FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);
	FVector BoneLocation = BoneTransform.GetLocation();

	// Apply different offset based on selection type
	// VirtualRing Gizmo selection → use RingOffset (band transform)
	// Mesh selection or Auto mode → use MeshOffset (mesh transform)
	if (SelectionType == EFleshRingSelectionType::Gizmo &&
		Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualRing)
	{
		return BoneLocation + BoneTransform.GetRotation().RotateVector(Ring.RingOffset);
	}
	else
	{
		// Mesh selection or Auto mode
		return BoneLocation + BoneTransform.GetRotation().RotateVector(Ring.MeshOffset);
	}
}

FMatrix FFleshRingEditorViewportClient::GetWidgetCoordSystem() const
{
	// Return AlignRotation coordinate system (validation performed internally)
	return GetSelectedRingAlignMatrix();
}

FMatrix FFleshRingEditorViewportClient::GetSelectedRingAlignMatrix() const
{
	if (!PreviewScene || !EditingAsset.IsValid())
	{
		return FMatrix::Identity;
	}

	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();
	if (SelectedIndex < 0 || SelectionType == EFleshRingSelectionType::None)
	{
		return FMatrix::Identity;
	}

	const TArray<FFleshRingSettings>& Rings = EditingAsset->Rings;
	if (!Rings.IsValidIndex(SelectedIndex))
	{
		return FMatrix::Identity;
	}

	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
	{
		return FMatrix::Identity;
	}

	const FFleshRingSettings& Ring = Rings[SelectedIndex];
	int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return FMatrix::Identity;
	}

	FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);
	FQuat BoneRotation = BoneTransform.GetRotation();

	// VirtualBand mode: use bind pose
	if (Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
	{
		USkeletalMesh* SkelMesh = SkelMeshComp->GetSkeletalMeshAsset();
		const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
		FTransform BindPoseBoneTransform = FTransform::Identity;
		int32 CurrentBoneIdx = BoneIndex;
		while (CurrentBoneIdx != INDEX_NONE)
		{
			BindPoseBoneTransform = BindPoseBoneTransform * RefSkeleton.GetRefBonePose()[CurrentBoneIdx];
			CurrentBoneIdx = RefSkeleton.GetParentIndex(CurrentBoneIdx);
		}
		BoneRotation = SkelMeshComp->GetComponentTransform().GetRotation() * BindPoseBoneTransform.GetRotation();
	}

	// Check coordinate system mode (World vs Local) - using custom flag
	if (bUseLocalCoordSystem)
	{
		FQuat TargetRotation;
		if (bIsDraggingRotation)
		{
			// While dragging, use rotation from drag start (gizmo fixed)
			TargetRotation = DragStartWorldRotation;
		}
		else
		{
			// Local mode: bone rotation * ring/mesh rotation = current world rotation
			FQuat CurrentRotation;
			if (SelectionType == EFleshRingSelectionType::Gizmo)
			{
				if (Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualRing)
				{
					// VirtualRing Gizmo selection uses RingRotation
					CurrentRotation = Ring.RingRotation;
				}
				else if (Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
				{
					// Virtual Band Gizmo selection uses dedicated BandRotation
					CurrentRotation = Ring.VirtualBand.BandRotation;
				}
				else
				{
					CurrentRotation = Ring.MeshRotation;
				}
			}
			else
			{
				// Mesh selection uses MeshRotation (mesh transform)
				CurrentRotation = Ring.MeshRotation;
			}
			TargetRotation = BoneRotation * CurrentRotation;
		}

		// Use FQuatRotationMatrix (prevents Gimbal lock issue when converting to FRotator)
		return FQuatRotationMatrix(TargetRotation);
	}
	else
	{
		// World mode: pure world axis based
		return FMatrix::Identity;
	}
}

ECoordSystem FFleshRingEditorViewportClient::GetWidgetCoordSystemSpace() const
{
	// Always return COORD_World to disable Widget system's Local Space rotation inversion logic
	// GetWidgetCoordSystem() already returns a rotated coordinate system,
	// so no additional Local Space processing is needed
	return COORD_World;
}

void FFleshRingEditorViewportClient::SetWidgetCoordSystemSpace(ECoordSystem NewCoordSystem)
{
	// Called when default toolbar button clicked - toggle custom flag
	bUseLocalCoordSystem = (NewCoordSystem == COORD_Local);
	Invalidate();
}

UE::Widget::EWidgetMode FFleshRingEditorViewportClient::GetWidgetMode() const
{
	// Use ModeTools widget mode (for toolbar highlight behavior)
	if (ModeTools)
	{
		return ModeTools->GetWidgetMode();
	}
	return UE::Widget::WM_Translate;
}

bool FFleshRingEditorViewportClient::InputWidgetDelta(FViewport* InViewport, EAxisList::Type CurrentAxis, FVector& Drag, FRotator& Rot, FVector& Scale)
{
	SCOPE_CYCLE_COUNTER(STAT_FleshRingEditor_InputWidgetDelta);
	// Don't process if no widget axis is selected
	if (CurrentAxis == EAxisList::None)
	{
		return false;
	}

	if (!PreviewScene || !EditingAsset.IsValid())
	{
		return false;
	}

	int32 SelectedIndex = PreviewScene->GetSelectedRingIndex();
	if (SelectedIndex < 0 || SelectionType == EFleshRingSelectionType::None)
	{
		return false;
	}

	TArray<FFleshRingSettings>& Rings = EditingAsset->Rings;
	if (!Rings.IsValidIndex(SelectedIndex))
	{
		return false;
	}

	// Get bone Transform (for local coordinate conversion)
	USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent();
	if (!SkelMeshComp || !SkelMeshComp->GetSkeletalMeshAsset())
	{
		return false;
	}

	FFleshRingSettings& Ring = Rings[SelectedIndex];
	int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return false;
	}

	FTransform BoneTransform = SkelMeshComp->GetBoneTransform(BoneIndex);
	FQuat BoneRotation = BoneTransform.GetRotation();

	// VirtualBand mode: use bind pose
	if (Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
	{
		USkeletalMesh* SkelMesh = SkelMeshComp->GetSkeletalMeshAsset();
		const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
		FTransform BindPoseBoneTransform = FTransform::Identity;
		int32 CurrentBoneIdx = BoneIndex;
		while (CurrentBoneIdx != INDEX_NONE)
		{
			BindPoseBoneTransform = BindPoseBoneTransform * RefSkeleton.GetRefBonePose()[CurrentBoneIdx];
			CurrentBoneIdx = RefSkeleton.GetParentIndex(CurrentBoneIdx);
		}
		BoneTransform = BindPoseBoneTransform * SkelMeshComp->GetComponentTransform();
		BoneRotation = SkelMeshComp->GetComponentTransform().GetRotation() * BindPoseBoneTransform.GetRotation();
	}

	// Apply snapping
	const ULevelEditorViewportSettings* ViewportSettings = GetDefault<ULevelEditorViewportSettings>();

	// Translation snap - apply based on gizmo axis
	// Widget system passes Drag in world coordinates, so convert to gizmo local before applying snap
	FVector SnappedDrag = Drag;
	if (ViewportSettings->GridEnabled && GEditor && !Drag.IsZero())
	{
		float GridSize = GEditor->GetGridSize();

		// Get gizmo coordinate system matrix
		FMatrix GizmoMatrix = GetSelectedRingAlignMatrix();
		FMatrix GizmoMatrixInverse = GizmoMatrix.Inverse();

		// Convert world Drag to gizmo local coordinates
		FVector LocalDragForSnap = GizmoMatrixInverse.TransformVector(Drag);

		// Apply snap in gizmo local coordinates
		FVector SnappedLocalDrag;
		SnappedLocalDrag.X = FMath::GridSnap(LocalDragForSnap.X, GridSize);
		SnappedLocalDrag.Y = FMath::GridSnap(LocalDragForSnap.Y, GridSize);
		SnappedLocalDrag.Z = FMath::GridSnap(LocalDragForSnap.Z, GridSize);

		// Convert back to world coordinates
		SnappedDrag = GizmoMatrix.TransformVector(SnappedLocalDrag);
	}

	// Rotation snap
	FRotator SnappedRot = Rot;
	if (ViewportSettings->RotGridEnabled && GEditor)
	{
		FRotator RotGridSize = GEditor->GetRotGridSize();
		SnappedRot.Pitch = FMath::GridSnap(Rot.Pitch, RotGridSize.Pitch);
		SnappedRot.Yaw = FMath::GridSnap(Rot.Yaw, RotGridSize.Yaw);
		SnappedRot.Roll = FMath::GridSnap(Rot.Roll, RotGridSize.Roll);
	}

	// Scale snap
	FVector SnappedScale = Scale;
	if (ViewportSettings->SnapScaleEnabled && GEditor)
	{
		float ScaleGridSize = GEditor->GetScaleGridSize();
		SnappedScale.X = FMath::GridSnap(Scale.X, ScaleGridSize);
		SnappedScale.Y = FMath::GridSnap(Scale.Y, ScaleGridSize);
		SnappedScale.Z = FMath::GridSnap(Scale.Z, ScaleGridSize);
	}

	// Scale Z-axis direction correction: Lower/MidLower sign inverted
	// Widget system's Scale.Z is based on gizmo Z-axis
	// Lower/MidLower: "drag down → Height increase" is intuitive, so invert sign
	if (SelectedSection == EBandSection::Lower || SelectedSection == EBandSection::MidLower)
	{
		SnappedScale.Z = -SnappedScale.Z;
	}

	// Convert world drag to bone local coordinates
	// (Widget system always passes Drag in world coordinates regardless of World/Local mode)
	FVector LocalDrag = BoneRotation.UnrotateVector(SnappedDrag);

	// Update different offset based on selection type
	if (SelectionType == EFleshRingSelectionType::Gizmo)
	{
		// Auto mode: even with Gizmo selection, use MeshOffset/MeshRotation (SDF based)
		// (Auto has no gizmo so should not reach here, but handle for safety)
		if (Ring.InfluenceMode == EFleshRingInfluenceMode::Auto)
		{
			// Translation -> update MeshOffset
			Ring.MeshOffset += LocalDrag;

			// Rotation -> update MeshRotation
			if (bIsDraggingRotation)
			{
				FQuat FrameDeltaRotation = Rot.Quaternion();
				if (!FrameDeltaRotation.IsIdentity())
				{
					AccumulatedDeltaRotation = FrameDeltaRotation * AccumulatedDeltaRotation;
					AccumulatedDeltaRotation.Normalize();

					FQuat NewWorldRotation = AccumulatedDeltaRotation * DragStartWorldRotation;
					NewWorldRotation.Normalize();

					FQuat NewLocalRotation = BoneRotation.Inverse() * NewWorldRotation;
					Ring.MeshRotation = NewLocalRotation;
					Ring.MeshEulerRotation = NewLocalRotation.Rotator();
				}
			}

			// Scale handling -> use MeshScale
			if (!SnappedScale.IsZero())
			{
				Ring.MeshScale += SnappedScale;
				Ring.MeshScale.X = FMath::Max(Ring.MeshScale.X, 0.01f);
				Ring.MeshScale.Y = FMath::Max(Ring.MeshScale.Y, 0.01f);
				Ring.MeshScale.Z = FMath::Max(Ring.MeshScale.Z, 0.01f);
			}
		}
		else if (Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualRing)
		{
			// VirtualRing mode: use RingOffset/RingRotation
			Ring.RingOffset += LocalDrag;

			if (bIsDraggingRotation)
			{
				FQuat FrameDeltaRotation = Rot.Quaternion();
				if (!FrameDeltaRotation.IsIdentity())
				{
					AccumulatedDeltaRotation = FrameDeltaRotation * AccumulatedDeltaRotation;
					AccumulatedDeltaRotation.Normalize();

					FQuat NewWorldRotation = AccumulatedDeltaRotation * DragStartWorldRotation;
					NewWorldRotation.Normalize();

					FQuat NewLocalRotation = BoneRotation.Inverse() * NewWorldRotation;
					Ring.RingRotation = NewLocalRotation;
					Ring.RingEulerRotation = NewLocalRotation.Rotator();
				}
			}

			// Scale handling: adjust RingRadius
			if (!SnappedScale.IsZero())
			{
				float ScaleDelta = FMath::Max3(SnappedScale.X, SnappedScale.Y, SnappedScale.Z);
				if (ScaleDelta == 0.0f)
				{
					ScaleDelta = FMath::Min3(SnappedScale.X, SnappedScale.Y, SnappedScale.Z);
				}
				Ring.RingRadius = FMath::Clamp(Ring.RingRadius * (1.0f + ScaleDelta), 0.1f, 100.0f);
			}
		}
		else if (Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
		{
			// Virtual Band mode: use dedicated BandOffset/BandRotation
			FVirtualBandSettings& BandSettings = Ring.VirtualBand;
			BandSettings.BandOffset += LocalDrag;

			if (bIsDraggingRotation)
			{
				FQuat FrameDeltaRotation = Rot.Quaternion();
				if (!FrameDeltaRotation.IsIdentity())
				{
					AccumulatedDeltaRotation = FrameDeltaRotation * AccumulatedDeltaRotation;
					AccumulatedDeltaRotation.Normalize();

					FQuat NewWorldRotation = AccumulatedDeltaRotation * DragStartWorldRotation;
					NewWorldRotation.Normalize();

					FQuat NewLocalRotation = BoneRotation.Inverse() * NewWorldRotation;
					BandSettings.BandRotation = NewLocalRotation;
					BandSettings.BandEulerRotation = NewLocalRotation.Rotator();
				}
			}

			// Scale handling: per-section + per-axis separation
			float RadialScaleDelta = FMath::Max(SnappedScale.X, SnappedScale.Y);
			if (FMath::IsNearlyZero(RadialScaleDelta))
			{
				RadialScaleDelta = FMath::Min(SnappedScale.X, SnappedScale.Y);
			}

			if (SelectedSection == EBandSection::None)
			{
				// Entire band: X/Y → all Radius, Z → all Height
				if (!FMath::IsNearlyZero(RadialScaleDelta))
				{
					float RadialFactor = 1.0f + RadialScaleDelta;
					BandSettings.MidUpperRadius = FMath::Clamp(BandSettings.MidUpperRadius * RadialFactor, 0.1f, 100.0f);
					BandSettings.MidLowerRadius = FMath::Clamp(BandSettings.MidLowerRadius * RadialFactor, 0.1f, 100.0f);
					BandSettings.BandThickness = FMath::Clamp(BandSettings.BandThickness * RadialFactor, 0.1f, 50.0f);
					BandSettings.Upper.Radius = FMath::Clamp(BandSettings.Upper.Radius * RadialFactor, 0.1f, 100.0f);
					BandSettings.Lower.Radius = FMath::Clamp(BandSettings.Lower.Radius * RadialFactor, 0.1f, 100.0f);
				}
				if (!FMath::IsNearlyZero(SnappedScale.Z))
				{
					float HeightFactor = 1.0f + SnappedScale.Z;
					BandSettings.BandHeight = FMath::Clamp(BandSettings.BandHeight * HeightFactor, 0.1f, 100.0f);
					BandSettings.Upper.Height = FMath::Clamp(BandSettings.Upper.Height * HeightFactor, 0.0f, 100.0f);
					BandSettings.Lower.Height = FMath::Clamp(BandSettings.Lower.Height * HeightFactor, 0.0f, 100.0f);
				}
			}
			else if (SelectedSection == EBandSection::Upper)
			{
				// Upper section: X/Y → Upper.Radius, Z → Upper.Height
				if (!FMath::IsNearlyZero(RadialScaleDelta))
				{
					float RadialFactor = 1.0f + RadialScaleDelta;
					BandSettings.Upper.Radius = FMath::Clamp(BandSettings.Upper.Radius * RadialFactor, 0.1f, 100.0f);
				}
				if (!FMath::IsNearlyZero(SnappedScale.Z))
				{
					float HeightFactor = 1.0f + SnappedScale.Z;
					BandSettings.Upper.Height = FMath::Clamp(BandSettings.Upper.Height * HeightFactor, 0.0f, 100.0f);
				}
			}
			else if (SelectedSection == EBandSection::MidUpper)
			{
				// MidUpper section: X/Y → MidUpperRadius, Z → BandHeight
				if (!FMath::IsNearlyZero(RadialScaleDelta))
				{
					float RadialFactor = 1.0f + RadialScaleDelta;
					BandSettings.MidUpperRadius = FMath::Clamp(BandSettings.MidUpperRadius * RadialFactor, 0.1f, 100.0f);
				}
				if (!FMath::IsNearlyZero(SnappedScale.Z))
				{
					float OldBandHeight = BandSettings.BandHeight;
					float HeightFactor = 1.0f + SnappedScale.Z;
					BandSettings.BandHeight = FMath::Clamp(BandSettings.BandHeight * HeightFactor, 0.1f, 100.0f);

					// Keep MidLower/Lower fixed: offset origin in band +Z direction → only MidUpper moves
					float HeightDelta = BandSettings.BandHeight - OldBandHeight;
					BandSettings.BandOffset += BandSettings.BandRotation.RotateVector(FVector(0.0f, 0.0f, HeightDelta * 0.5f));
				}
			}
			else if (SelectedSection == EBandSection::MidLower)
			{
				// MidLower section: X/Y → MidLowerRadius, Z → BandHeight
				if (!FMath::IsNearlyZero(RadialScaleDelta))
				{
					float RadialFactor = 1.0f + RadialScaleDelta;
					BandSettings.MidLowerRadius = FMath::Clamp(BandSettings.MidLowerRadius * RadialFactor, 0.1f, 100.0f);
				}
				if (!FMath::IsNearlyZero(SnappedScale.Z))
				{
					float OldBandHeight = BandSettings.BandHeight;
					float HeightFactor = 1.0f + SnappedScale.Z;
					BandSettings.BandHeight = FMath::Clamp(BandSettings.BandHeight * HeightFactor, 0.1f, 100.0f);

					// Keep MidUpper/Upper fixed: offset origin in band -Z direction → only MidLower moves
					float HeightDelta = BandSettings.BandHeight - OldBandHeight;
					BandSettings.BandOffset += BandSettings.BandRotation.RotateVector(FVector(0.0f, 0.0f, -HeightDelta * 0.5f));
				}
			}
			else if (SelectedSection == EBandSection::Lower)
			{
				// Lower section: X/Y → Lower.Radius, Z → Lower.Height
				if (!FMath::IsNearlyZero(RadialScaleDelta))
				{
					float RadialFactor = 1.0f + RadialScaleDelta;
					BandSettings.Lower.Radius = FMath::Clamp(BandSettings.Lower.Radius * RadialFactor, 0.1f, 100.0f);
				}
				if (!FMath::IsNearlyZero(SnappedScale.Z))
				{
					float HeightFactor = 1.0f + SnappedScale.Z;
					BandSettings.Lower.Height = FMath::Clamp(BandSettings.Lower.Height * HeightFactor, 0.0f, 100.0f);
				}
			}
		}
	}
	else if (SelectionType == EFleshRingSelectionType::Mesh)
	{
		// Ring mesh translation -> update MeshOffset
		Ring.MeshOffset += LocalDrag;

		// Apply rotation as well
		if (bIsDraggingRotation)
		{
			// Rot from Widget is decomposed in world coordinate system
			// Local axis rotation gets distributed across Pitch/Yaw/Roll when converted to world FRotator
			// Therefore must convert entire Rot to quaternion for accurate 360 degree accumulation
			FQuat FrameDeltaRotation = Rot.Quaternion();

			if (!FrameDeltaRotation.IsIdentity())
			{
				// Add to accumulated delta (prevents gimbal lock: accumulate as quaternion instead of re-reading FRotator)
				AccumulatedDeltaRotation = FrameDeltaRotation * AccumulatedDeltaRotation;
				AccumulatedDeltaRotation.Normalize();

				// Apply accumulated delta to drag start rotation
				FQuat NewWorldRotation = AccumulatedDeltaRotation * DragStartWorldRotation;
				NewWorldRotation.Normalize();

				// Convert world rotation to bone local rotation and save
				FQuat NewLocalRotation = BoneRotation.Inverse() * NewWorldRotation;
				Ring.MeshRotation = NewLocalRotation;
				Ring.MeshEulerRotation = NewLocalRotation.Rotator();  // Sync EulerRotation
			}
		}

		// Apply scale
		if (!SnappedScale.IsZero())
		{
			Ring.MeshScale += SnappedScale;
			// Clamp to minimum only
			Ring.MeshScale.X = FMath::Max(Ring.MeshScale.X, 0.01f);
			Ring.MeshScale.Y = FMath::Max(Ring.MeshScale.Y, 0.01f);
			Ring.MeshScale.Z = FMath::Max(Ring.MeshScale.Z, 0.01f);
		}

		// Update StaticMeshComponent Transform
		FVector MeshLocation = BoneTransform.GetLocation() + BoneRotation.RotateVector(Ring.MeshOffset);
		FQuat WorldRotation = BoneRotation * Ring.MeshRotation;

		// 1. Update PreviewScene's RingMeshComponents (when Deformer is disabled)
		const TArray<UFleshRingMeshComponent*>& RingMeshComponents = PreviewScene->GetRingMeshComponents();
		if (RingMeshComponents.IsValidIndex(SelectedIndex) && RingMeshComponents[SelectedIndex])
		{
			RingMeshComponents[SelectedIndex]->SetWorldLocationAndRotation(MeshLocation, WorldRotation);
			RingMeshComponents[SelectedIndex]->SetWorldScale3D(Ring.MeshScale);
		}

		// 2. Update FleshRingComponent's RingMeshComponents (when Deformer is enabled)
		UFleshRingComponent* FleshRingComp = PreviewScene->GetFleshRingComponent();
		if (FleshRingComp)
		{
			const auto& ComponentRingMeshes = FleshRingComp->GetRingMeshComponents();
			if (ComponentRingMeshes.IsValidIndex(SelectedIndex) && ComponentRingMeshes[SelectedIndex])
			{
				ComponentRingMeshes[SelectedIndex]->SetWorldLocationAndRotation(MeshLocation, WorldRotation);
				ComponentRingMeshes[SelectedIndex]->SetWorldScale3D(Ring.MeshScale);
			}
		}
	}

	// Performance optimization: MarkPackageDirty() is called only once in TrackingStopped()
	// 5-10ms overhead per frame when called during drag

	// Update transforms only (keep Deformer, prevent flickering)
	// Pass selected Ring index to process only that Ring (optimization)
	if (PreviewScene)
	{
		UFleshRingComponent* FleshRingComp = PreviewScene->GetFleshRingComponent();
		if (FleshRingComp)
		{
			SCOPE_CYCLE_COUNTER(STAT_FleshRingEditor_UpdateRingTransforms);
			int32 SelectedRingIndex = PreviewScene->GetSelectedRingIndex();
			FleshRingComp->UpdateRingTransforms(SelectedRingIndex);
		}
	}

	// Refresh viewport
	{
		SCOPE_CYCLE_COUNTER(STAT_FleshRingEditor_Invalidate);
		Invalidate();
	}

	return true;
}

void FFleshRingEditorViewportClient::TrackingStarted(const FInputEventState& InInputState, bool bIsDraggingWidget, bool bNudge)
{
	// Start transaction when drag begins
	if (bIsDraggingWidget && EditingAsset.IsValid() && SelectionType != EFleshRingSelectionType::None)
	{
		ScopedTransaction = MakeUnique<FScopedTransaction>(NSLOCTEXT("FleshRingEditor", "ModifyRingTransform", "Modify Ring Transform"));
		EditingAsset->Modify();

		// Only perform rotation-related initialization in rotation mode
		bool bIsRotationMode = ModeTools && ModeTools->GetWidgetMode() == UE::Widget::WM_Rotate;
		if (bIsRotationMode)
		{
			// Save initial rotation at drag start (for gizmo coordinate system fixing)
			USkeletalMeshComponent* SkelMeshComp = PreviewScene ? PreviewScene->GetSkeletalMeshComponent() : nullptr;
			int32 SelectedIndex = PreviewScene ? PreviewScene->GetSelectedRingIndex() : -1;

			if (SkelMeshComp && SkelMeshComp->GetSkeletalMeshAsset() && EditingAsset->Rings.IsValidIndex(SelectedIndex))
			{
				const FFleshRingSettings& Ring = EditingAsset->Rings[SelectedIndex];
				int32 BoneIndex = SkelMeshComp->GetBoneIndex(Ring.BoneName);
				if (BoneIndex != INDEX_NONE)
				{
					FQuat BoneRotation = SkelMeshComp->GetBoneTransform(BoneIndex).GetRotation();

					// VirtualBand mode: use bind pose
					if (Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
					{
						USkeletalMesh* SkelMesh = SkelMeshComp->GetSkeletalMeshAsset();
						const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
						FTransform BindPoseBoneTransform = FTransform::Identity;
						int32 CurrentBoneIdx = BoneIndex;
						while (CurrentBoneIdx != INDEX_NONE)
						{
							BindPoseBoneTransform = BindPoseBoneTransform * RefSkeleton.GetRefBonePose()[CurrentBoneIdx];
							CurrentBoneIdx = RefSkeleton.GetParentIndex(CurrentBoneIdx);
						}
						FTransform ComponentToWorld = SkelMeshComp->GetComponentTransform();
						BoneRotation = ComponentToWorld.GetRotation() * BindPoseBoneTransform.GetRotation();
					}

					// Use mode-specific rotation for Gizmo selection
					FQuat CurrentRotation;
					if (SelectionType == EFleshRingSelectionType::Gizmo)
					{
						if (Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualRing)
						{
							CurrentRotation = Ring.RingRotation;
						}
						else if (Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
						{
							CurrentRotation = Ring.VirtualBand.BandRotation;
						}
						else
						{
							CurrentRotation = Ring.MeshRotation;
						}
					}
					else
					{
						// Mesh selection uses MeshRotation
						CurrentRotation = Ring.MeshRotation;
					}

					DragStartWorldRotation = BoneRotation * CurrentRotation;
					DragStartWorldRotation.Normalize();  // Normalize

					// Initialize accumulated delta rotation
					AccumulatedDeltaRotation = FQuat::Identity;

					bIsDraggingRotation = true;
				}
			}
		}
	}

	FEditorViewportClient::TrackingStarted(InInputState, bIsDraggingWidget, bNudge);
}

void FFleshRingEditorViewportClient::TrackingStopped()
{
	// End transaction when drag finishes
	// Check if transaction existed first (transaction is only created when Ring is modified)
	bool bHadTransaction = ScopedTransaction.IsValid();
	ScopedTransaction.Reset();
	bIsDraggingRotation = false;

	// Mark asset dirty when drag ends (performance optimization: not called during drag)
	// Exclude cases that don't modify asset like camera movement
	if (bHadTransaction && EditingAsset.IsValid())
	{
		SCOPE_CYCLE_COUNTER(STAT_FleshRingEditor_MarkPackageDirty);
		EditingAsset->MarkPackageDirty();
	}

	FEditorViewportClient::TrackingStopped();
}

void FFleshRingEditorViewportClient::InvalidateAndDraw()
{
	Invalidate();

	// Force viewport rendering even when dropdown is open
	if (Viewport)
	{
		Viewport->Draw();
	}
}

void FFleshRingEditorViewportClient::ToggleShowSkeletalMesh()
{
	bShowSkeletalMesh = !bShowSkeletalMesh;

	if (PreviewScene)
	{
		if (USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent())
		{
			SkelMeshComp->SetVisibility(bShowSkeletalMesh);
		}
	}

	InvalidateAndDraw();
}

void FFleshRingEditorViewportClient::ToggleShowRingMeshes()
{
	bShowRingMeshes = !bShowRingMeshes;

	// Toggle Ring mesh component Visibility
	if (PreviewScene)
	{
		PreviewScene->SetRingMeshesVisible(bShowRingMeshes);
	}

	InvalidateAndDraw();
}

void FFleshRingEditorViewportClient::ToggleShowGrid()
{
	// Toggle Grid setting of current profile in UAssetViewerSettings
	UAssetViewerSettings* Settings = UAssetViewerSettings::Get();
	if (!Settings || !PreviewScene)
	{
		return;
	}

	int32 ProfileIndex = PreviewScene->GetCurrentProfileIndex();
	if (!Settings->Profiles.IsValidIndex(ProfileIndex))
	{
		return;
	}

	// Toggle profile's bShowGrid
	Settings->Profiles[ProfileIndex].bShowGrid = !Settings->Profiles[ProfileIndex].bShowGrid;

	// Notify change (to other listeners)
	Settings->OnAssetViewerSettingsChanged().Broadcast(GET_MEMBER_NAME_CHECKED(FPreviewSceneProfile, bShowGrid));

	// Apply immediately
	ApplyPreviewSceneShowFlags();
	Invalidate();
}

bool FFleshRingEditorViewportClient::ShouldShowGrid() const
{
	// Return Grid setting of current profile from UAssetViewerSettings
	UAssetViewerSettings* Settings = UAssetViewerSettings::Get();
	if (!Settings || !PreviewScene)
	{
		return false;
	}

	int32 ProfileIndex = PreviewScene->GetCurrentProfileIndex();
	if (!Settings->Profiles.IsValidIndex(ProfileIndex))
	{
		return false;
	}

	return Settings->Profiles[ProfileIndex].bShowGrid;
}

void FFleshRingEditorViewportClient::ApplyShowFlagsToScene()
{
	if (PreviewScene)
	{
		// Apply skeletal mesh visibility
		if (USkeletalMeshComponent* SkelMeshComp = PreviewScene->GetSkeletalMeshComponent())
		{
			SkelMeshComp->SetVisibility(bShowSkeletalMesh);
		}

		// Apply Ring mesh visibility
		PreviewScene->SetRingMeshesVisible(bShowRingMeshes);
	}
}

void FFleshRingEditorViewportClient::OnAssetViewerSettingsChanged(const FName& PropertyName)
{
	// Apply ShowFlags and refresh viewport when Preview Scene Settings changes
	ApplyPreviewSceneShowFlags();
	Invalidate();
}

void FFleshRingEditorViewportClient::ApplyPreviewSceneShowFlags()
{
	if (!PreviewScene)
	{
		return;
	}

	// Get current profile settings from UAssetViewerSettings
	UAssetViewerSettings* Settings = UAssetViewerSettings::Get();
	if (!Settings)
	{
		return;
	}

	int32 ProfileIndex = PreviewScene->GetCurrentProfileIndex();
	if (!Settings->Profiles.IsValidIndex(ProfileIndex))
	{
		return;
	}

	const FPreviewSceneProfile& Profile = Settings->Profiles[ProfileIndex];

	// Apply PostProcessing (must process first - DisableAdvancedFeatures resets other flags)
	if (Profile.bPostProcessingEnabled)
	{
		EngineShowFlags.EnableAdvancedFeatures();
		EngineShowFlags.SetBloom(true);
	}
	else
	{
		EngineShowFlags.DisableAdvancedFeatures();
		EngineShowFlags.SetBloom(false);
	}

	// Apply ShowFlags
	EngineShowFlags.SetGrid(Profile.bShowGrid);
	EngineShowFlags.SetMeshEdges(Profile.bShowMeshEdges);
	EngineShowFlags.SetTonemapper(Profile.bEnableToneMapping);

	// Sync DrawHelper's Grid as well
	DrawHelper.bDrawGrid = Profile.bShowGrid;
}

FString FFleshRingEditorViewportClient::GetConfigSectionName() const
{
	if (EditingAsset.IsValid())
	{
		// Include asset path in section name (e.g., FleshRingEditorViewport:/Game/FleshRings/MyAsset)
		return FString::Printf(TEXT("%s:%s"), *FleshRingViewportConfigSectionBase, *EditingAsset->GetPathName());
	}
	return FleshRingViewportConfigSectionBase;
}

void FFleshRingEditorViewportClient::SaveSettings()
{
	const FString SectionName = GetConfigSectionName();

	// === Save viewport type ===
	GConfig->SetInt(*SectionName, TEXT("ViewportType"), static_cast<int32>(GetViewportType()), GEditorPerProjectIni);

	// === Save perspective camera settings ===
	GConfig->SetVector(*SectionName, TEXT("PerspectiveViewLocation"), ViewTransformPerspective.GetLocation(), GEditorPerProjectIni);
	GConfig->SetRotator(*SectionName, TEXT("PerspectiveViewRotation"), ViewTransformPerspective.GetRotation(), GEditorPerProjectIni);

	// === Save orthographic camera settings ===
	GConfig->SetVector(*SectionName, TEXT("OrthographicViewLocation"), ViewTransformOrthographic.GetLocation(), GEditorPerProjectIni);
	GConfig->SetRotator(*SectionName, TEXT("OrthographicViewRotation"), ViewTransformOrthographic.GetRotation(), GEditorPerProjectIni);
	GConfig->SetFloat(*SectionName, TEXT("OrthoZoom"), ViewTransformOrthographic.GetOrthoZoom(), GEditorPerProjectIni);

	// === Save camera speed ===
	GConfig->SetFloat(*SectionName, TEXT("CameraSpeed"), GetCameraSpeedSettings().GetCurrentSpeed(), GEditorPerProjectIni);

	// === Save FOV ===
	GConfig->SetFloat(*SectionName, TEXT("ViewFOV"), ViewFOV, GEditorPerProjectIni);

	// === Save clipping planes ===
	GConfig->SetFloat(*SectionName, TEXT("NearClipPlane"), GetNearClipPlane(), GEditorPerProjectIni);
	GConfig->SetFloat(*SectionName, TEXT("FarClipPlane"), GetFarClipPlaneOverride(), GEditorPerProjectIni);

	// Orthographic clipping planes
	TOptional<double> OrthoNear = GetOrthographicNearPlaneOverride();
	TOptional<double> OrthoFar = GetOrthographicFarPlaneOverride();
	GConfig->SetBool(*SectionName, TEXT("HasOrthoNearClip"), OrthoNear.IsSet(), GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("HasOrthoFarClip"), OrthoFar.IsSet(), GEditorPerProjectIni);
	if (OrthoNear.IsSet())
	{
		GConfig->SetDouble(*SectionName, TEXT("OrthoNearClipPlane"), OrthoNear.GetValue(), GEditorPerProjectIni);
	}
	if (OrthoFar.IsSet())
	{
		GConfig->SetDouble(*SectionName, TEXT("OrthoFarClipPlane"), OrthoFar.GetValue(), GEditorPerProjectIni);
	}

	// === Save exposure settings ===
	GConfig->SetFloat(*SectionName, TEXT("ExposureFixedEV100"), ExposureSettings.FixedEV100, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ExposureBFixed"), ExposureSettings.bFixed, GEditorPerProjectIni);

	// === Save view mode (Lit, Unlit, Wireframe, etc.) ===
	GConfig->SetInt(*SectionName, TEXT("ViewMode"), static_cast<int32>(GetViewMode()), GEditorPerProjectIni);

	// === Save custom Show flags ===
	GConfig->SetBool(*SectionName, TEXT("ShowSkeletalMesh"), bShowSkeletalMesh, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowRingGizmos"), bShowRingGizmos, GEditorPerProjectIni);
	GConfig->SetFloat(*SectionName, TEXT("RingGizmoThickness"), RingGizmoThickness, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowRingMeshes"), bShowRingMeshes, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowBones"), bShowBones, GEditorPerProjectIni);

	// Save bone draw options
	GConfig->SetBool(*SectionName, TEXT("ShowBoneNames"), bShowBoneNames, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowMultiColorBones"), bShowMultiColorBones, GEditorPerProjectIni);
	GConfig->SetFloat(*SectionName, TEXT("BoneDrawSize"), BoneDrawSize, GEditorPerProjectIni);
	GConfig->SetInt(*SectionName, TEXT("BoneDrawMode"), static_cast<int32>(BoneDrawMode), GEditorPerProjectIni);

	// Save debug visualization options
	GConfig->SetBool(*SectionName, TEXT("ShowDebugVisualization"), bCachedShowDebugVisualization, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowSdfVolume"), bCachedShowSdfVolume, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowAffectedVertices"), bCachedShowAffectedVertices, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowSDFSlice"), bCachedShowSDFSlice, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowBulgeHeatmap"), bCachedShowBulgeHeatmap, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowBulgeArrows"), bCachedShowBulgeArrows, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowBulgeRange"), bCachedShowBulgeRange, GEditorPerProjectIni);
	GConfig->SetBool(*SectionName, TEXT("ShowRingSkinSamplingRadius"), bShowRingSkinSamplingRadius, GEditorPerProjectIni);
	GConfig->SetInt(*SectionName, TEXT("DebugSliceZ"), CachedDebugSliceZ, GEditorPerProjectIni);

	// Save to config file immediately
	GConfig->Flush(false, GEditorPerProjectIni);
}

void FFleshRingEditorViewportClient::LoadSettings()
{
	const FString SectionName = GetConfigSectionName();

	// === Load viewport type (first - set type before applying camera position) ===
	int32 SavedViewportType = static_cast<int32>(ELevelViewportType::LVT_Perspective);
	if (GConfig->GetInt(*SectionName, TEXT("ViewportType"), SavedViewportType, GEditorPerProjectIni))
	{
		SetViewportType(static_cast<ELevelViewportType>(SavedViewportType));
	}

	// === Load perspective camera settings ===
	bool bHasSavedPerspectiveLocation = false;
	FVector SavedPerspectiveLocation;
	if (GConfig->GetVector(*SectionName, TEXT("PerspectiveViewLocation"), SavedPerspectiveLocation, GEditorPerProjectIni))
	{
		ViewTransformPerspective.SetLocation(SavedPerspectiveLocation);
		bHasSavedPerspectiveLocation = true;
	}

	bool bHasSavedPerspectiveRotation = false;
	FRotator SavedPerspectiveRotation;
	if (GConfig->GetRotator(*SectionName, TEXT("PerspectiveViewRotation"), SavedPerspectiveRotation, GEditorPerProjectIni))
	{
		ViewTransformPerspective.SetRotation(SavedPerspectiveRotation);
		bHasSavedPerspectiveRotation = true;
	}

	// === Load orthographic camera settings ===
	bool bHasSavedOrthographicLocation = false;
	FVector SavedOrthographicLocation;
	if (GConfig->GetVector(*SectionName, TEXT("OrthographicViewLocation"), SavedOrthographicLocation, GEditorPerProjectIni))
	{
		ViewTransformOrthographic.SetLocation(SavedOrthographicLocation);
		bHasSavedOrthographicLocation = true;
	}

	bool bHasSavedOrthographicRotation = false;
	FRotator SavedOrthographicRotation;
	if (GConfig->GetRotator(*SectionName, TEXT("OrthographicViewRotation"), SavedOrthographicRotation, GEditorPerProjectIni))
	{
		ViewTransformOrthographic.SetRotation(SavedOrthographicRotation);
		bHasSavedOrthographicRotation = true;
	}

	float SavedOrthoZoom = DEFAULT_ORTHOZOOM;
	if (GConfig->GetFloat(*SectionName, TEXT("OrthoZoom"), SavedOrthoZoom, GEditorPerProjectIni))
	{
		ViewTransformOrthographic.SetOrthoZoom(SavedOrthoZoom);
	}

	// Apply saved camera position to actual view (restore camera on engine restart)
	// Stop camera interpolation started by FocusOnMesh() (saved position takes priority)
	bool bHasSavedCameraSettings = false;
	if (GetViewportType() == LVT_Perspective)
	{
		if (bHasSavedPerspectiveLocation)
		{
			SetViewLocation(SavedPerspectiveLocation);
			bHasSavedCameraSettings = true;
		}
		if (bHasSavedPerspectiveRotation)
		{
			SetViewRotation(SavedPerspectiveRotation);
			bHasSavedCameraSettings = true;
		}
	}
	else
	{
		if (bHasSavedOrthographicLocation)
		{
			SetViewLocation(SavedOrthographicLocation);
			bHasSavedCameraSettings = true;
		}
		if (bHasSavedOrthographicRotation)
		{
			SetViewRotation(SavedOrthographicRotation);
			bHasSavedCameraSettings = true;
		}
	}

	// Stop FocusOnMesh() interpolation if saved settings exist
	if (bHasSavedCameraSettings)
	{
		bIsCameraInterpolating = false;
	}

	// === Load camera speed ===
	float SavedCameraSpeed = 1.0f;
	if (GConfig->GetFloat(*SectionName, TEXT("CameraSpeed"), SavedCameraSpeed, GEditorPerProjectIni))
	{
		FEditorViewportCameraSpeedSettings SpeedSettings = GetCameraSpeedSettings();
		SpeedSettings.SetCurrentSpeed(SavedCameraSpeed);
		SetCameraSpeedSettings(SpeedSettings);
	}

	// === Load FOV ===
	float SavedFOV = 90.0f;
	if (GConfig->GetFloat(*SectionName, TEXT("ViewFOV"), SavedFOV, GEditorPerProjectIni))
	{
		ViewFOV = SavedFOV;
	}

	// === Load clipping planes ===
	float SavedNearClip = 1.0f;
	if (GConfig->GetFloat(*SectionName, TEXT("NearClipPlane"), SavedNearClip, GEditorPerProjectIni))
	{
		OverrideNearClipPlane(SavedNearClip);
	}

	float SavedFarClip = 0.0f;
	if (GConfig->GetFloat(*SectionName, TEXT("FarClipPlane"), SavedFarClip, GEditorPerProjectIni))
	{
		if (SavedFarClip > 0.0f)
		{
			OverrideFarClipPlane(SavedFarClip);
		}
	}

	// Load orthographic clipping planes
	bool bHasOrthoNearClip = false;
	bool bHasOrthoFarClip = false;
	GConfig->GetBool(*SectionName, TEXT("HasOrthoNearClip"), bHasOrthoNearClip, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("HasOrthoFarClip"), bHasOrthoFarClip, GEditorPerProjectIni);

	if (bHasOrthoNearClip)
	{
		double OrthoNearValue = 0.0;
		if (GConfig->GetDouble(*SectionName, TEXT("OrthoNearClipPlane"), OrthoNearValue, GEditorPerProjectIni))
		{
			SetOrthographicNearPlaneOverride(OrthoNearValue);
		}
	}

	if (bHasOrthoFarClip)
	{
		double OrthoFarValue = 0.0;
		if (GConfig->GetDouble(*SectionName, TEXT("OrthoFarClipPlane"), OrthoFarValue, GEditorPerProjectIni))
		{
			SetOrthographicFarPlaneOverride(OrthoFarValue);
		}
	}

	// === Load exposure settings ===
	GConfig->GetFloat(*SectionName, TEXT("ExposureFixedEV100"), ExposureSettings.FixedEV100, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ExposureBFixed"), ExposureSettings.bFixed, GEditorPerProjectIni);

	// === Load view mode (Lit, Unlit, Wireframe, etc.) ===
	// Note: ApplyPreviewSceneShowFlags() overwrites view mode, so reapply later
	EViewModeIndex LoadedViewMode = VMI_Lit;
	bool bHasSavedViewMode = false;
	int32 SavedViewMode = static_cast<int32>(VMI_Lit);
	if (GConfig->GetInt(*SectionName, TEXT("ViewMode"), SavedViewMode, GEditorPerProjectIni))
	{
		LoadedViewMode = static_cast<EViewModeIndex>(SavedViewMode);
		bHasSavedViewMode = true;
	}

	// === Load custom Show flags ===
	GConfig->GetBool(*SectionName, TEXT("ShowSkeletalMesh"), bShowSkeletalMesh, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowRingGizmos"), bShowRingGizmos, GEditorPerProjectIni);
	GConfig->GetFloat(*SectionName, TEXT("RingGizmoThickness"), RingGizmoThickness, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowRingMeshes"), bShowRingMeshes, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowBones"), bShowBones, GEditorPerProjectIni);

	// Load bone draw options
	GConfig->GetBool(*SectionName, TEXT("ShowBoneNames"), bShowBoneNames, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowMultiColorBones"), bShowMultiColorBones, GEditorPerProjectIni);
	GConfig->GetFloat(*SectionName, TEXT("BoneDrawSize"), BoneDrawSize, GEditorPerProjectIni);
	int32 BoneDrawModeInt = static_cast<int32>(EFleshRingBoneDrawMode::All);
	GConfig->GetInt(*SectionName, TEXT("BoneDrawMode"), BoneDrawModeInt, GEditorPerProjectIni);
	BoneDrawMode = static_cast<EFleshRingBoneDrawMode::Type>(FMath::Clamp(BoneDrawModeInt, 0, 5));

	// Load debug visualization options
	GConfig->GetBool(*SectionName, TEXT("ShowDebugVisualization"), bCachedShowDebugVisualization, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowSdfVolume"), bCachedShowSdfVolume, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowAffectedVertices"), bCachedShowAffectedVertices, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowSDFSlice"), bCachedShowSDFSlice, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowBulgeHeatmap"), bCachedShowBulgeHeatmap, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowBulgeArrows"), bCachedShowBulgeArrows, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowBulgeRange"), bCachedShowBulgeRange, GEditorPerProjectIni);
	GConfig->GetBool(*SectionName, TEXT("ShowRingSkinSamplingRadius"), bShowRingSkinSamplingRadius, GEditorPerProjectIni);
	GConfig->GetInt(*SectionName, TEXT("DebugSliceZ"), CachedDebugSliceZ, GEditorPerProjectIni);

	// Apply cached values to FleshRingComponent
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowDebugVisualization = bCachedShowDebugVisualization;
			Comp->bShowSdfVolume = bCachedShowSdfVolume;
			Comp->bShowAffectedVertices = bCachedShowAffectedVertices;
			Comp->bShowSDFSlice = bCachedShowSDFSlice;
			Comp->bShowBulgeHeatmap = bCachedShowBulgeHeatmap;
			Comp->bShowBulgeArrows = bCachedShowBulgeArrows;
			Comp->bShowBulgeRange = bCachedShowBulgeRange;
			Comp->DebugSliceZ = CachedDebugSliceZ;

			// Apply SDFSlice plane visibility
			Comp->SetDebugSlicePlanesVisible(Comp->bShowSDFSlice && Comp->bShowDebugVisualization);
		}
	}

	// Apply loaded Show Flags to PreviewScene
	ApplyShowFlagsToScene();

	// Apply Preview Scene Settings as well
	ApplyPreviewSceneShowFlags();

	// Reapply view mode (since ApplyPreviewSceneShowFlags() overwrites ShowFlags)
	if (bHasSavedViewMode)
	{
		SetViewMode(LoadedViewMode);
	}
}

void FFleshRingEditorViewportClient::ToggleShowDebugVisualization()
{
	bCachedShowDebugVisualization = !bCachedShowDebugVisualization;
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowDebugVisualization = bCachedShowDebugVisualization;
			// Hide/show plane actors immediately
			Comp->SetDebugSlicePlanesVisible(Comp->bShowSDFSlice && Comp->bShowDebugVisualization);
		}
	}
	InvalidateAndDraw();
}

void FFleshRingEditorViewportClient::ToggleShowSdfVolume()
{
	bCachedShowSdfVolume = !bCachedShowSdfVolume;
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowSdfVolume = bCachedShowSdfVolume;
		}
	}
	InvalidateAndDraw();
}

void FFleshRingEditorViewportClient::ToggleShowAffectedVertices()
{
	bCachedShowAffectedVertices = !bCachedShowAffectedVertices;
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowAffectedVertices = bCachedShowAffectedVertices;
		}
	}
	InvalidateAndDraw();
}

bool FFleshRingEditorViewportClient::ShouldShowDebugVisualization() const
{
	return bCachedShowDebugVisualization;
}

bool FFleshRingEditorViewportClient::ShouldShowSdfVolume() const
{
	return bCachedShowSdfVolume;
}

bool FFleshRingEditorViewportClient::ShouldShowAffectedVertices() const
{
	return bCachedShowAffectedVertices;
}

void FFleshRingEditorViewportClient::ToggleShowSDFSlice()
{
	bCachedShowSDFSlice = !bCachedShowSDFSlice;
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowSDFSlice = bCachedShowSDFSlice;
			// Hide/show plane actors immediately
			Comp->SetDebugSlicePlanesVisible(Comp->bShowSDFSlice && Comp->bShowDebugVisualization);
		}
	}
	InvalidateAndDraw();
}

bool FFleshRingEditorViewportClient::ShouldShowSDFSlice() const
{
	return bCachedShowSDFSlice;
}

int32 FFleshRingEditorViewportClient::GetDebugSliceZ() const
{
	return CachedDebugSliceZ;
}

void FFleshRingEditorViewportClient::SetDebugSliceZ(int32 NewValue)
{
	CachedDebugSliceZ = NewValue;
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->DebugSliceZ = NewValue;
		}
	}
	Invalidate();
}

void FFleshRingEditorViewportClient::ToggleShowBulgeHeatmap()
{
	bCachedShowBulgeHeatmap = !bCachedShowBulgeHeatmap;
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowBulgeHeatmap = bCachedShowBulgeHeatmap;
		}
	}
	InvalidateAndDraw();
}

bool FFleshRingEditorViewportClient::ShouldShowBulgeHeatmap() const
{
	return bCachedShowBulgeHeatmap;
}

void FFleshRingEditorViewportClient::ToggleShowBulgeArrows()
{
	bCachedShowBulgeArrows = !bCachedShowBulgeArrows;
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowBulgeArrows = bCachedShowBulgeArrows;
		}
	}
	InvalidateAndDraw();
}

bool FFleshRingEditorViewportClient::ShouldShowBulgeArrows() const
{
	return bCachedShowBulgeArrows;
}

void FFleshRingEditorViewportClient::ToggleShowBulgeRange()
{
	bCachedShowBulgeRange = !bCachedShowBulgeRange;
	if (PreviewScene)
	{
		if (UFleshRingComponent* Comp = PreviewScene->GetFleshRingComponent())
		{
			Comp->bShowBulgeRange = bCachedShowBulgeRange;
		}
	}
	InvalidateAndDraw();
}

bool FFleshRingEditorViewportClient::ShouldShowBulgeRange() const
{
	return bCachedShowBulgeRange;
}

void FFleshRingEditorViewportClient::SetBoneDrawMode(EFleshRingBoneDrawMode::Type InMode)
{
	BoneDrawMode = InMode;
	UpdateBonesToDraw();
	Invalidate();
}

void FFleshRingEditorViewportClient::UpdateBonesToDraw()
{
	if (!PreviewScene)
	{
		return;
	}

	UDebugSkelMeshComponent* MeshComponent = PreviewScene->GetSkeletalMeshComponent();
	if (!MeshComponent || !MeshComponent->GetSkeletalMeshAsset())
	{
		return;
	}

	const FReferenceSkeleton& RefSkeleton = MeshComponent->GetReferenceSkeleton();
	const int32 NumBones = RefSkeleton.GetNum();

	if (NumBones == 0)
	{
		BonesToDraw.Empty();
		return;
	}

	// Create parent indices array
	TArray<int32> ParentIndices;
	ParentIndices.SetNum(NumBones);
	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		ParentIndices[BoneIndex] = RefSkeleton.GetParentIndex(BoneIndex);
	}

	// Create selected bones array
	TArray<int32> SelectedBones;
	if (!SelectedBoneName.IsNone())
	{
		int32 SelectedBoneIndex = RefSkeleton.FindBoneIndex(SelectedBoneName);
		if (SelectedBoneIndex != INDEX_NONE)
		{
			SelectedBones.Add(SelectedBoneIndex);
		}
	}

	// Convert EFleshRingBoneDrawMode to EBoneDrawMode
	EBoneDrawMode::Type EngineBoneDrawMode = EBoneDrawMode::All;
	switch (BoneDrawMode)
	{
	case EFleshRingBoneDrawMode::None:
		EngineBoneDrawMode = EBoneDrawMode::None;
		break;
	case EFleshRingBoneDrawMode::Selected:
		EngineBoneDrawMode = EBoneDrawMode::Selected;
		break;
	case EFleshRingBoneDrawMode::SelectedAndParents:
		EngineBoneDrawMode = EBoneDrawMode::SelectedAndParents;
		break;
	case EFleshRingBoneDrawMode::SelectedAndChildren:
		EngineBoneDrawMode = EBoneDrawMode::SelectedAndChildren;
		break;
	case EFleshRingBoneDrawMode::SelectedAndParentsAndChildren:
		EngineBoneDrawMode = EBoneDrawMode::SelectedAndParentsAndChildren;
		break;
	case EFleshRingBoneDrawMode::All:
	default:
		EngineBoneDrawMode = EBoneDrawMode::All;
		break;
	}

	// Use SkeletalDebugRendering function to calculate bones to draw
	SkeletalDebugRendering::CalculateBonesToDraw(
		ParentIndices,
		SelectedBones,
		EngineBoneDrawMode,
		BonesToDraw);
}

// ============================================================================
// Right-click context menu related functions
// ============================================================================

void FFleshRingEditorViewportClient::ShowBoneContextMenu(FName BoneName, const FVector2D& ScreenPos)
{
	// Build weighted bone cache if empty
	if (WeightedBoneIndices.Num() == 0)
	{
		BuildWeightedBoneCache();
	}

	// Get index of selected bone
	int32 BoneIndex = INDEX_NONE;
	if (PreviewScene)
	{
		if (UDebugSkelMeshComponent* SkelComp = PreviewScene->GetSkeletalMeshComponent())
		{
			if (USkeletalMesh* SkelMesh = SkelComp->GetSkeletalMeshAsset())
			{
				BoneIndex = SkelMesh->GetRefSkeleton().FindBoneIndex(BoneName);
			}
		}
	}

	// Ring add condition: can add if self or any descendant has weights
	// (same logic as skeleton tree's bIsMeshBone = HasWeightedDescendant())
	bool bCanAddRing = (BoneIndex != INDEX_NONE) && HasWeightedDescendant(BoneIndex);

	FMenuBuilder MenuBuilder(true, nullptr);

	MenuBuilder.BeginSection("BoneActions", NSLOCTEXT("FleshRingEditor", "BoneActionsSection", "Bone"));
	{
		// Add Ring menu - enabled only for mesh bones (self or descendants have weights)
		MenuBuilder.AddMenuEntry(
			NSLOCTEXT("FleshRingEditor", "AddRingAtPosition", "Add Ring Here..."),
			bCanAddRing
				? NSLOCTEXT("FleshRingEditor", "AddRingAtPositionTooltip", "Select a mesh and add a ring at clicked position")
				: NSLOCTEXT("FleshRingEditor", "AddRingAtPositionDisabledTooltip", "Cannot add ring: This bone has no weighted vertices"),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"),
			FUIAction(
				FExecuteAction::CreateRaw(this, &FFleshRingEditorViewportClient::OnContextMenu_AddRing),
				FCanExecuteAction::CreateLambda([bCanAddRing]() { return bCanAddRing; })
			)
		);

		// Copy Bone Name menu
		MenuBuilder.AddMenuEntry(
			NSLOCTEXT("FleshRingEditor", "CopyBoneName", "Copy Bone Name"),
			FText::GetEmpty(),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
			FUIAction(
				FExecuteAction::CreateLambda([BoneName]()
				{
					FPlatformApplicationMisc::ClipboardCopy(*BoneName.ToString());
				})
			)
		);
	}
	MenuBuilder.EndSection();

	// Display menu - show at cursor position (ScreenPos is viewport local coordinates so use cursor position)
	TSharedPtr<SWidget> MenuWidget = MenuBuilder.MakeWidget();

	if (TSharedPtr<SFleshRingEditorViewport> ViewportPtr = ViewportWidget.Pin())
	{
		FSlateApplication::Get().PushMenu(
			ViewportPtr.ToSharedRef(),
			FWidgetPath(),
			MenuWidget.ToSharedRef(),
			FSlateApplication::Get().GetCursorPos(),
			FPopupTransitionEffect::ContextMenu
		);
	}
}

void FFleshRingEditorViewportClient::OnContextMenu_AddRing()
{
	if (PendingRingAddBoneName.IsNone())
	{
		return;
	}

	// Asset picker config
	FAssetPickerConfig AssetPickerConfig;
	AssetPickerConfig.Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
	AssetPickerConfig.Filter.bRecursiveClasses = true;
	AssetPickerConfig.SelectionMode = ESelectionMode::Single;
	AssetPickerConfig.bAllowNullSelection = false;  // Disabled as handled by button
	AssetPickerConfig.bFocusSearchBoxWhenOpened = true;
	AssetPickerConfig.InitialAssetViewType = EAssetViewType::List;

	// Callback when mesh is selected - use lambda to capture member variables
	FName CapturedBoneName = PendingRingAddBoneName;
	FVector2D CapturedScreenPos = PendingRingAddScreenPos;

	AssetPickerConfig.OnAssetSelected = FOnAssetSelected::CreateLambda(
		[this, CapturedBoneName, CapturedScreenPos](const FAssetData& AssetData)
		{
			// Close popup
			FSlateApplication::Get().DismissAllMenus();

			UStaticMesh* SelectedMesh = nullptr;
			if (AssetData.IsValid())
			{
				SelectedMesh = Cast<UStaticMesh>(AssetData.GetAsset());
			}

			// Calculate bone local offset and rotation (projected position on bone axis line, green line direction)
			FRotator LocalRotation;
			FVector LocalOffset = CalculateBoneLocalOffsetFromScreenPos(CapturedScreenPos, CapturedBoneName, &LocalRotation);

			// Request Ring add
			OnAddRingAtPositionRequested.ExecuteIfBound(
				CapturedBoneName,
				LocalOffset,
				LocalRotation,
				SelectedMesh
			);
		}
	);

	// Reset state (before opening asset picker)
	PendingRingAddBoneName = NAME_None;

	// Display asset picker popup
	FContentBrowserModule& ContentBrowserModule =
		FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

	TSharedRef<SWidget> AssetPickerWidget = ContentBrowserModule.Get().CreateAssetPicker(AssetPickerConfig);

	// Display as popup menu
	if (TSharedPtr<SFleshRingEditorViewport> ViewportPtr = ViewportWidget.Pin())
	{
		// Popup with bottom button bar (dialog style)
		FSlateApplication::Get().PushMenu(
			ViewportPtr.ToSharedRef(),
			FWidgetPath(),
			SNew(SBox)
				.WidthOverride(400.0f)
				.HeightOverride(500.0f)
				[
					SNew(SVerticalBox)
					// Asset picker (top)
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					[
						AssetPickerWidget
					]
					// Separator
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 4.0f)
					[
						SNew(SSeparator)
					]
					// Button bar (bottom)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(8.0f, 4.0f, 8.0f, 8.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						// Left margin
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 4.0f, 0.0f)
						[
							SNew(SButton)
							.Text(NSLOCTEXT("FleshRingEditor", "SkipMesh", "Skip Mesh"))
							.ToolTipText(NSLOCTEXT("FleshRingEditor", "SkipMeshTooltip", "Add ring without mesh"))
							.OnClicked_Lambda([this, CapturedBoneName, CapturedScreenPos]()
							{
								FSlateApplication::Get().DismissAllMenus();

								// Calculate bone local offset and rotation
								FRotator LocalRotation;
								FVector LocalOffset = CalculateBoneLocalOffsetFromScreenPos(CapturedScreenPos, CapturedBoneName, &LocalRotation);

								// Request Ring add without mesh
								OnAddRingAtPositionRequested.ExecuteIfBound(
									CapturedBoneName,
									LocalOffset,
									LocalRotation,
									nullptr
								);
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							SNew(SButton)
							.Text(NSLOCTEXT("FleshRingEditor", "Cancel", "Cancel"))
							.OnClicked_Lambda([]()
							{
								FSlateApplication::Get().DismissAllMenus();
								return FReply::Handled();
							})
						]
					]
				],
			FSlateApplication::Get().GetCursorPos(),
			FPopupTransitionEffect::ContextMenu
		);
	}
}

FVector FFleshRingEditorViewportClient::CalculateBoneLocalOffsetFromScreenPos(const FVector2D& ScreenPos, FName BoneName, FRotator* OutLocalRotation)
{
	if (!PreviewScene || !EditingAsset.IsValid())
	{
		return FVector::ZeroVector;
	}

	// Get bone transform
	UDebugSkelMeshComponent* SkeletalMeshComponent = PreviewScene->GetSkeletalMeshComponent();
	if (!SkeletalMeshComponent)
	{
		return FVector::ZeroVector;
	}

	const FReferenceSkeleton& RefSkeleton = SkeletalMeshComponent->GetSkeletalMeshAsset()->GetRefSkeleton();
	int32 BoneIndex = RefSkeleton.FindBoneIndex(BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return FVector::ZeroVector;
	}

	FTransform BoneTransform = SkeletalMeshComponent->GetBoneTransform(BoneIndex);
	FVector BoneOrigin = BoneTransform.GetLocation();

	// Build weighted bone cache if empty
	if (WeightedBoneIndices.Num() == 0)
	{
		const_cast<FFleshRingEditorViewportClient*>(this)->BuildWeightedBoneCache();
	}

	// Calculate bone axis direction: use direction from current bone toward weighted child bone
	FVector BoneAxisDir;

	// Check weighted child bone count
	int32 WeightedChildCount = CountWeightedChildBones(BoneIndex);

	if (WeightedChildCount == 1)
	{
		// When only one weighted child bone exists: automatic direction calculation
		int32 WeightedChildIndex = FindWeightedChildBone(BoneIndex);
		FVector ChildLocation = SkeletalMeshComponent->GetBoneTransform(WeightedChildIndex).GetLocation();
		BoneAxisDir = (ChildLocation - BoneOrigin).GetSafeNormal();
	}
	else if (WeightedChildCount >= 2)
	{
		// Multiple weighted child bones: use default rotation (direction is ambiguous)
		if (OutLocalRotation)
		{
			*OutLocalRotation = FRotator(-90.0f, 0.0f, 0.0f);
		}
		return FVector::ZeroVector;
	}
	else
	{
		// End bone (no children): use default rotation
		if (OutLocalRotation)
		{
			*OutLocalRotation = FRotator(-90.0f, 0.0f, 0.0f);
		}
		return FVector::ZeroVector;
	}

	// Screen coordinates → world ray conversion
	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		Viewport,
		GetScene(),
		EngineShowFlags)
		.SetTime(FGameTime::GetTimeSinceAppStart()));

	FSceneView* View = CalcSceneView(&ViewFamily);
	if (!View)
	{
		return FVector::ZeroVector;
	}

	FVector RayOrigin, RayDirection;
	View->DeprojectFVector2D(ScreenPos, RayOrigin, RayDirection);

	// Calculate closest point between ray and bone axis
	// Formula for closest point between two lines
	FVector W0 = BoneOrigin - RayOrigin;
	float A = FVector::DotProduct(BoneAxisDir, BoneAxisDir);  // Always 1 (normalized)
	float B = FVector::DotProduct(BoneAxisDir, RayDirection);
	float C = FVector::DotProduct(RayDirection, RayDirection); // Always 1 (normalized)
	float D = FVector::DotProduct(BoneAxisDir, W0);
	float E = FVector::DotProduct(RayDirection, W0);

	float Denom = A * C - B * B;

	float T_BoneAxis;
	if (FMath::Abs(Denom) < SMALL_NUMBER)
	{
		// Lines are parallel - use bone origin
		T_BoneAxis = 0.0f;
	}
	else
	{
		// Closest point parameter on bone axis
		T_BoneAxis = (B * E - C * D) / Denom;
	}

	// Calculate world offset along green line direction
	// BoneAxisDir = current→child bone direction (green line)
	FVector WorldOffset = BoneAxisDir * T_BoneAxis;

	// Convert world offset to bone local coordinate system
	// This ensures RingOffset aligns exactly on the green line
	FVector LocalOffset = BoneTransform.GetRotation().UnrotateVector(WorldOffset);

	// Rotation calculation: make green line direction become Ring's Z axis
	if (OutLocalRotation)
	{
		// Convert green line direction to bone local space
		FVector LocalAxisDir = BoneTransform.GetRotation().UnrotateVector(BoneAxisDir);

		// Calculate quaternion that rotates from Z axis (0,0,1) to green line direction
		FQuat RotationQuat = FQuat::FindBetweenNormals(FVector::UpVector, LocalAxisDir);
		*OutLocalRotation = RotationQuat.Rotator();
	}

	return LocalOffset;
}

FRotator FFleshRingEditorViewportClient::CalculateDefaultRingRotationForBone(FName BoneName)
{
	const FRotator DefaultRotation(-90.0f, 0.0f, 0.0f);

	if (!PreviewScene || BoneName.IsNone())
	{
		return DefaultRotation;
	}

	UDebugSkelMeshComponent* SkeletalMeshComponent = PreviewScene->GetSkeletalMeshComponent();
	if (!SkeletalMeshComponent)
	{
		return DefaultRotation;
	}

	USkeletalMesh* SkelMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
	if (!SkelMesh)
	{
		return DefaultRotation;
	}

	int32 BoneIndex = SkelMesh->GetRefSkeleton().FindBoneIndex(BoneName);
	if (BoneIndex == INDEX_NONE)
	{
		return DefaultRotation;
	}

	// Build weighted bone cache if empty
	if (WeightedBoneIndices.Num() == 0)
	{
		const_cast<FFleshRingEditorViewportClient*>(this)->BuildWeightedBoneCache();
	}

	// Check weighted child bone count
	int32 WeightedChildCount = CountWeightedChildBones(BoneIndex);

	if (WeightedChildCount == 1)
	{
		// When only one weighted child bone exists: automatic direction calculation
		int32 WeightedChildIndex = FindWeightedChildBone(BoneIndex);
		FTransform BoneTransform = SkeletalMeshComponent->GetBoneTransform(BoneIndex);
		FVector BoneOrigin = BoneTransform.GetLocation();
		FVector ChildLocation = SkeletalMeshComponent->GetBoneTransform(WeightedChildIndex).GetLocation();
		FVector BoneAxisDir = (ChildLocation - BoneOrigin).GetSafeNormal();

		// Convert green line direction to bone local space
		FVector LocalAxisDir = BoneTransform.GetRotation().UnrotateVector(BoneAxisDir);

		// Calculate quaternion that rotates from Z axis (0,0,1) to green line direction
		FQuat RotationQuat = FQuat::FindBetweenNormals(FVector::UpVector, LocalAxisDir);
		return RotationQuat.Rotator();
	}

	// No weighted child bones or 2+ weighted child bones: use default
	return DefaultRotation;
}

void FFleshRingEditorViewportClient::BuildWeightedBoneCache()
{
	WeightedBoneIndices.Empty();

	if (!PreviewScene)
	{
		return;
	}

	UDebugSkelMeshComponent* SkeletalMeshComponent = PreviewScene->GetSkeletalMeshComponent();
	if (!SkeletalMeshComponent)
	{
		return;
	}

	USkeletalMesh* SkelMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
	if (!SkelMesh)
	{
		return;
	}

	// Find weighted bones from LOD 0 render data
	FSkeletalMeshRenderData* RenderData = SkelMesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		return;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];

	// Bones in each section's BoneMap are the weighted bones
	for (const FSkelMeshRenderSection& Section : LODData.RenderSections)
	{
		for (FBoneIndexType BoneIndex : Section.BoneMap)
		{
			WeightedBoneIndices.Add(BoneIndex);
		}
	}
}

bool FFleshRingEditorViewportClient::IsBoneWeighted(int32 BoneIndex) const
{
	return WeightedBoneIndices.Contains(BoneIndex);
}

int32 FFleshRingEditorViewportClient::FindWeightedChildBone(int32 ParentBoneIndex) const
{
	if (!PreviewScene)
	{
		return INDEX_NONE;
	}

	UDebugSkelMeshComponent* SkeletalMeshComponent = PreviewScene->GetSkeletalMeshComponent();
	if (!SkeletalMeshComponent)
	{
		return INDEX_NONE;
	}

	USkeletalMesh* SkelMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
	if (!SkelMesh)
	{
		return INDEX_NONE;
	}

	const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();

	// Find weighted bone among child bones
	for (int32 i = 0; i < RefSkeleton.GetNum(); ++i)
	{
		if (RefSkeleton.GetParentIndex(i) == ParentBoneIndex)
		{
			if (IsBoneWeighted(i))
			{
				return i;
			}
		}
	}

	return INDEX_NONE;
}

bool FFleshRingEditorViewportClient::HasWeightedDescendant(int32 BoneIndex) const
{
	// True if self has weights
	if (IsBoneWeighted(BoneIndex))
	{
		return true;
	}

	if (!PreviewScene)
	{
		return false;
	}

	UDebugSkelMeshComponent* SkeletalMeshComponent = PreviewScene->GetSkeletalMeshComponent();
	if (!SkeletalMeshComponent)
	{
		return false;
	}

	USkeletalMesh* SkelMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
	if (!SkelMesh)
	{
		return false;
	}

	const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
	const int32 NumBones = RefSkeleton.GetNum();

	// Recursively check descendant bones
	for (int32 ChildIndex = 0; ChildIndex < NumBones; ++ChildIndex)
	{
		if (RefSkeleton.GetParentIndex(ChildIndex) == BoneIndex)
		{
			if (HasWeightedDescendant(ChildIndex))
			{
				return true;
			}
		}
	}

	return false;
}

int32 FFleshRingEditorViewportClient::CountWeightedChildBones(int32 ParentBoneIndex) const
{
	if (!PreviewScene)
	{
		return 0;
	}

	UDebugSkelMeshComponent* SkeletalMeshComponent = PreviewScene->GetSkeletalMeshComponent();
	if (!SkeletalMeshComponent)
	{
		return 0;
	}

	USkeletalMesh* SkelMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
	if (!SkelMesh)
	{
		return 0;
	}

	const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();

	int32 Count = 0;
	for (int32 i = 0; i < RefSkeleton.GetNum(); ++i)
	{
		if (RefSkeleton.GetParentIndex(i) == ParentBoneIndex)
		{
			if (IsBoneWeighted(i))
			{
				++Count;
			}
		}
	}

	return Count;
}
