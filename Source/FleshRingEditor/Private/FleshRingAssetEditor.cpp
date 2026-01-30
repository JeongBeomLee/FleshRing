// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingAssetEditor.h"
#include "FleshRingAssetEditorToolkit.h"
#include "FleshRingAsset.h"
#include "FleshRingComponent.h"
#include "FleshRingTypes.h"
#include "SFleshRingSkeletonTree.h"
#include "SFleshRingEditorViewport.h"
#include "FleshRingEditorViewportClient.h"
#include "FleshRingPreviewScene.h"
#include "FleshRingEditorCommands.h"
#include "FleshRingEdMode.h"
#include "EditorModeRegistry.h"
#include "SAdvancedPreviewDetailsTab.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SBoxPanel.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "PropertyPath.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"
#include "Editor.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Images/SThrobber.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/AppStyle.h"
#include "EditorModeManager.h"

#define LOCTEXT_NAMESPACE "FleshRingAssetEditor"

FFleshRingAssetEditor::FFleshRingAssetEditor()
{
}

FFleshRingAssetEditor::~FFleshRingAssetEditor()
{
	// Unbind Ring selection changed delegate
	if (EditingAsset)
	{
		EditingAsset->OnRingSelectionChanged.RemoveAll(this);
	}

	// Unbind property changed delegate
	if (OnPropertyChangedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(OnPropertyChangedHandle);
	}

	// Unbind Undo/Redo delegate
	if (OnObjectTransactedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectTransacted.Remove(OnObjectTransactedHandle);
	}
}

void FFleshRingAssetEditor::InitFleshRingAssetEditor(
	const EToolkitMode::Type Mode,
	const TSharedPtr<IToolkitHost>& InitToolkitHost,
	UFleshRingAsset* InAsset)
{
	EditingAsset = InAsset;

	// Define editor layout (v2: Added Skeleton Tree)
	const TSharedRef<FTabManager::FLayout> StandaloneDefaultLayout =
		FTabManager::NewLayout(FleshRingAssetEditorToolkit::LayoutName)
		->AddArea
		(
			FTabManager::NewPrimaryArea()
			->SetOrientation(Orient_Horizontal)
			->Split
			(
				// Left: Skeleton Tree
				FTabManager::NewStack()
				->SetSizeCoefficient(0.15f)
				->AddTab(FleshRingAssetEditorToolkit::SkeletonTreeTabId, ETabState::OpenedTab)
			)
			->Split
			(
				// Center: Viewport
				FTabManager::NewStack()
				->SetSizeCoefficient(0.55f)
				->AddTab(FleshRingAssetEditorToolkit::ViewportTabId, ETabState::OpenedTab)
			)
			->Split
			(
				// Right: Details + Preview Settings (switchable tabs)
				FTabManager::NewStack()
				->SetSizeCoefficient(0.3f)
				->AddTab(FleshRingAssetEditorToolkit::DetailsTabId, ETabState::OpenedTab)
				->AddTab(FleshRingAssetEditorToolkit::PreviewSettingsTabId, ETabState::OpenedTab)
				->SetForegroundTab(FleshRingAssetEditorToolkit::DetailsTabId)
			)
		);

	// Prepare asset object array
	TArray<UObject*> ObjectsToEdit;
	ObjectsToEdit.Add(InAsset);

	// Create EditorModeManager BEFORE InitAssetEditor (tabs are spawned during InitAssetEditor)
	CreateEditorModeManager();

	// Initialize editor
	InitAssetEditor(
		Mode,
		InitToolkitHost,
		FleshRingAssetEditorToolkit::AppIdentifier,
		StandaloneDefaultLayout,
		true,  // bCreateDefaultStandaloneMenu
		true,  // bCreateDefaultToolbar
		ObjectsToEdit);

	// Subscribe to property changed delegate
	OnPropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(
		this, &FFleshRingAssetEditor::OnObjectPropertyChanged);

	// Subscribe to Undo/Redo delegate
	OnObjectTransactedHandle = FCoreUObjectDelegates::OnObjectTransacted.AddRaw(
		this, &FFleshRingAssetEditor::OnObjectTransacted);

	// Subscribe to Ring selection changed delegate (Details panel -> Viewport/Tree sync)
	if (EditingAsset)
	{
		EditingAsset->OnRingSelectionChanged.AddRaw(
			this, &FFleshRingAssetEditor::OnRingSelectionChangedFromDetails);
	}

	// Register and bind editor commands (QWER shortcuts)
	FFleshRingEditorCommands::Register();
	BindCommands();
}


FName FFleshRingAssetEditor::GetToolkitFName() const
{
	return FName("FleshRingAssetEditor");
}

FText FFleshRingAssetEditor::GetBaseToolkitName() const
{
	return LOCTEXT("AppLabel", "FleshRing Asset Editor");
}

FText FFleshRingAssetEditor::GetToolkitName() const
{
	if (EditingAsset)
	{
		return FText::FromString(EditingAsset->GetName());
	}
	return GetBaseToolkitName();
}

FText FFleshRingAssetEditor::GetToolkitToolTipText() const
{
	if (EditingAsset)
	{
		return FText::FromString(EditingAsset->GetPathName());
	}
	return GetBaseToolkitName();
}

FLinearColor FFleshRingAssetEditor::GetWorldCentricTabColorScale() const
{
	// FleshRing theme color (pink/flesh tone)
	return FLinearColor(1.0f, 0.5f, 0.5f);
}

FString FFleshRingAssetEditor::GetWorldCentricTabPrefix() const
{
	return LOCTEXT("WorldCentricTabPrefix", "FleshRing ").ToString();
}

FString FFleshRingAssetEditor::GetDocumentationLink() const
{
	return FString();
}

void FFleshRingAssetEditor::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	// Register parent
	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	// Add workspace category
	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(
		LOCTEXT("WorkspaceMenu_FleshRingAssetEditor", "FleshRing Asset Editor"));

	// Register Skeleton Tree tab
	InTabManager->RegisterTabSpawner(
		FleshRingAssetEditorToolkit::SkeletonTreeTabId,
		FOnSpawnTab::CreateSP(this, &FFleshRingAssetEditor::SpawnTab_SkeletonTree))
		.SetDisplayName(LOCTEXT("SkeletonTreeTab", "Skeleton"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Persona.Tabs.SkeletonTree"));

	// Register Viewport tab
	InTabManager->RegisterTabSpawner(
		FleshRingAssetEditorToolkit::ViewportTabId,
		FOnSpawnTab::CreateSP(this, &FFleshRingAssetEditor::SpawnTab_Viewport))
		.SetDisplayName(LOCTEXT("ViewportTab", "Viewport"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"));

	// Register Details tab
	InTabManager->RegisterTabSpawner(
		FleshRingAssetEditorToolkit::DetailsTabId,
		FOnSpawnTab::CreateSP(this, &FFleshRingAssetEditor::SpawnTab_Details))
		.SetDisplayName(LOCTEXT("DetailsTab", "Details"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));

	// Register Preview Scene Settings tab
	InTabManager->RegisterTabSpawner(
		FleshRingAssetEditorToolkit::PreviewSettingsTabId,
		FOnSpawnTab::CreateSP(this, &FFleshRingAssetEditor::SpawnTab_PreviewSettings))
		.SetDisplayName(LOCTEXT("PreviewSettingsTab", "Preview Scene Settings"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));
}

void FFleshRingAssetEditor::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);

	InTabManager->UnregisterTabSpawner(FleshRingAssetEditorToolkit::SkeletonTreeTabId);
	InTabManager->UnregisterTabSpawner(FleshRingAssetEditorToolkit::ViewportTabId);
	InTabManager->UnregisterTabSpawner(FleshRingAssetEditorToolkit::DetailsTabId);
	InTabManager->UnregisterTabSpawner(FleshRingAssetEditorToolkit::PreviewSettingsTabId);
}

TSharedRef<SDockTab> FFleshRingAssetEditor::SpawnTab_SkeletonTree(const FSpawnTabArgs& Args)
{
	// Create Skeleton Tree widget
	SkeletonTreeWidget = SNew(SFleshRingSkeletonTree)
		.Asset(EditingAsset)
		.OnBoneSelected(FOnBoneSelected::CreateSP(this, &FFleshRingAssetEditor::OnBoneSelected))
		.OnRingSelected(FOnRingSelected::CreateSP(this, &FFleshRingAssetEditor::OnRingSelected))
		.OnAddRingRequested(FOnAddRingRequested::CreateSP(this, &FFleshRingAssetEditor::OnAddRingRequested))
		.OnFocusCameraRequested(FOnFocusCameraRequested::CreateSP(this, &FFleshRingAssetEditor::OnFocusCameraRequested))
		.OnRingDeleted(FOnRingDeletedFromTree::CreateSP(this, &FFleshRingAssetEditor::HandleRingDeleted));

	return SNew(SDockTab)
		.Label(LOCTEXT("SkeletonTreeTabLabel", "Skeleton"))
		[
			SkeletonTreeWidget.ToSharedRef()
		];
}

TSharedRef<SDockTab> FFleshRingAssetEditor::SpawnTab_Viewport(const FSpawnTabArgs& Args)
{
	// Create viewport widget (pass ModeTools from toolkit)
	ViewportWidget = SNew(SFleshRingEditorViewport)
		.Asset(EditingAsset)
		.ModeTools(&GetEditorModeManager());

	// Clear skeleton tree selection when bone selection is cleared in viewport
	if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
	{
		ViewportClient->SetOnBoneSelectionCleared(
			FOnBoneSelectionCleared::CreateSP(this, &FFleshRingAssetEditor::OnBoneSelectionCleared));

		// Sync tree/details panel when Ring is picked in viewport
		ViewportClient->SetOnRingSelectedInViewport(
			FOnRingSelectedInViewport::CreateSP(this, &FFleshRingAssetEditor::OnRingSelectedInViewport));

		// Common handling when Ring is deleted in viewport
		ViewportClient->SetOnRingDeletedInViewport(
			FOnRingDeletedInViewport::CreateSP(this, &FFleshRingAssetEditor::HandleRingDeleted));

		// Sync skeleton tree when bone is picked in viewport
		ViewportClient->SetOnBoneSelectedInViewport(
			FOnBoneSelectedInViewport::CreateSP(this, &FFleshRingAssetEditor::OnBoneSelectedInViewport));

		// Callback when Ring add is requested in viewport (right-click menu)
		ViewportClient->SetOnAddRingAtPositionRequested(
			FOnAddRingAtPositionRequested::CreateSP(this, &FFleshRingAssetEditor::OnAddRingAtPositionRequested));
	}

	return SNew(SDockTab)
		.Label(LOCTEXT("ViewportTabLabel", "Viewport"))
		.ContentPadding(0)
		[
			SNew(SVerticalBox)
			// Toolbar (top, auto height)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				ViewportWidget->MakeToolbar()
			]
			// Viewport (remaining space)
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				ViewportWidget.ToSharedRef()
			]
		];
}

TSharedRef<SDockTab> FFleshRingAssetEditor::SpawnTab_Details(const FSpawnTabArgs& Args)
{
	// Create Details View
	CreateDetailsView();

	return SNew(SDockTab)
		.Label(LOCTEXT("DetailsTabLabel", "Details"))
		[
			DetailsView.ToSharedRef()
		];
}

TSharedRef<SDockTab> FFleshRingAssetEditor::SpawnTab_PreviewSettings(const FSpawnTabArgs& Args)
{
	TSharedPtr<FFleshRingPreviewScene> PreviewScene;
	if (ViewportWidget.IsValid())
	{
		PreviewScene = ViewportWidget->GetPreviewScene();
	}

	TSharedRef<SWidget> Content = SNullWidget::NullWidget;
	if (PreviewScene.IsValid())
	{
		Content = SNew(SAdvancedPreviewDetailsTab, PreviewScene.ToSharedRef());
	}

	return SNew(SDockTab)
		.Label(LOCTEXT("PreviewSettingsTabLabel", "Preview Scene Settings"))
		[
			Content
		];
}

void FFleshRingAssetEditor::CreateDetailsView()
{
	FPropertyEditorModule& PropertyEditorModule =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.bAllowSearch = true;
	DetailsViewArgs.bHideSelectionTip = true;
	DetailsViewArgs.bShowActorLabel = false;
	DetailsViewArgs.bShowOptions = true;

	DetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);

	if (EditingAsset)
	{
		DetailsView->SetObject(EditingAsset);
	}
}

void FFleshRingAssetEditor::OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent)
{
	// Only update when EditingAsset has changed
	if (Object == EditingAsset)
	{
		// Detect structural changes (Ring add/remove, RingMesh change)
		bool bNeedsFullRefresh = false;

		if (PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayAdd ||
			PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayRemove ||
			PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayClear ||
			PropertyChangedEvent.ChangeType == EPropertyChangeType::Duplicate ||
			PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayMove)
		{
			// Full refresh needed when Ring array structure changes (add/remove/clear/duplicate/move)
			bNeedsFullRefresh = true;

			// Reset selection state on array remove/clear (selected Ring may have been deleted)
			if (PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayRemove ||
				PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayClear)
			{
				EditingAsset->EditorSelectedRingIndex = -1;
				EditingAsset->EditorSelectionType = EFleshRingSelectionType::None;
			}
		}

		if (PropertyChangedEvent.Property)
		{
			FName PropName = PropertyChangedEvent.Property->GetFName();

			// Need to replace skeletal mesh when TargetSkeletalMesh changes
			if (PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, TargetSkeletalMesh))
			{
				bNeedsFullRefresh = true;

				// Reset bone selection state when skeletal mesh changes
				if (ViewportWidget.IsValid())
				{
					TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
					if (ViewportClient.IsValid())
					{
						ViewportClient->ClearSelectedBone();
					}
				}
			}
			// Need to regenerate SDF when RingMesh changes
			else if (PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingMesh))
			{
				bNeedsFullRefresh = true;
			}
			// Ring attachment position changes when BoneName changes -> recalculate SDF/AffectedVertices
			else if (PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BoneName))
			{
				bNeedsFullRefresh = true;
			}
			// Recalculate AffectedVertices when InfluenceMode changes
			else if (PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, InfluenceMode))
			{
				bNeedsFullRefresh = true;
			}
			// Transform-related properties don't need full refresh (handled by lightweight update)
			// RingOffset, MeshOffset, RingRotation, MeshRotation, RingEulerRotation, MeshEulerRotation,
			// MeshScale, RingRadius, Strength, Falloff, etc. only need lightweight update
		}

		// If not a Rings array structure change (full refresh only when entire array changes via copy/paste, etc.)
		// Individual property changes are handled above
		if (!bNeedsFullRefresh && PropertyChangedEvent.MemberProperty)
		{
			FName MemberName = PropertyChangedEvent.MemberProperty->GetFName();
			if (MemberName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, Rings))
			{
				// When only Rings array changed without property info (copy/paste, etc.)
				// Interactive changes (during drag) are handled by lightweight update
				if (!PropertyChangedEvent.Property &&
					PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
				{
					bNeedsFullRefresh = true;
				}
			}
		}

		if (bNeedsFullRefresh)
		{
			RefreshViewport();
		}
		else
		{
			// Extract Ring index: Check which element of the Rings array was changed
			int32 ChangedRingIndex = INDEX_NONE;

			// Extract Ring index: Use the currently selected Ring from the asset
			// Use EditorSelectedRingIndex stored in asset (the Ring is selected when modifying properties)
			if (PropertyChangedEvent.MemberProperty)
			{
				FName MemberName = PropertyChangedEvent.MemberProperty->GetFName();
				if (MemberName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, Rings))
				{
					// Use the selected Ring index from the asset
					ChangedRingIndex = EditingAsset->EditorSelectedRingIndex;
				}
			}

			// Transform/parameter change: Lightweight update (prevents flickering)
			// Pass specific Ring index to process only that Ring
			UpdateRingTransformsOnly(ChangedRingIndex);
		}
	}
}

void FFleshRingAssetEditor::RefreshViewport()
{
	// Prevent SelectRing() calls during tree refresh
	// (Prevents Undo history corruption from new transaction creation)
	bSyncingFromViewport = true;

	// Perform refresh
	if (ViewportWidget.IsValid())
	{
		ViewportWidget->RefreshPreview();

		// Apply Show Flags to newly created components
		if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
		{
			ViewportClient->ApplyShowFlagsToScene();
		}
	}

	// Also refresh Skeleton Tree (update Ring markers)
	if (SkeletonTreeWidget.IsValid())
	{
		SkeletonTreeWidget->RefreshTree();
	}

	// Apply selection state from asset to viewport (asset is the source of truth for selection)
	ApplySelectionFromAsset();

	bSyncingFromViewport = false;
}

void FFleshRingAssetEditor::UpdateRingTransformsOnly(int32 DirtyRingIndex)
{
	if (ViewportWidget.IsValid())
	{
		ViewportWidget->UpdateRingTransformsOnly(DirtyRingIndex);
	}
}

void FFleshRingAssetEditor::RefreshSDFOnly()
{
	if (ViewportWidget.IsValid())
	{
		ViewportWidget->RefreshSDFOnly();
	}
}

UFleshRingComponent* FFleshRingAssetEditor::GetPreviewFleshRingComponent() const
{
	if (ViewportWidget.IsValid())
	{
		TSharedPtr<FFleshRingPreviewScene> PreviewScene = ViewportWidget->GetPreviewScene();
		if (PreviewScene.IsValid())
		{
			return PreviewScene->GetFleshRingComponent();
		}
	}
	return nullptr;
}

void FFleshRingAssetEditor::ForceRefreshPreviewMesh()
{
	if (ViewportWidget.IsValid())
	{
		TSharedPtr<FFleshRingPreviewScene> PreviewScene = ViewportWidget->GetPreviewScene();
		if (PreviewScene.IsValid())
		{
			// Invalidate cache and force regeneration
			PreviewScene->InvalidatePreviewMeshCache();
			PreviewScene->GeneratePreviewMesh();

			// Broadcast asset change to update UI
			if (EditingAsset)
			{
				EditingAsset->OnAssetChanged.Broadcast(EditingAsset);
			}
		}
	}
}

void FFleshRingAssetEditor::TickPreviewScene(float DeltaTime)
{
	if (ViewportWidget.IsValid())
	{
		// PreviewScene tick (update world and components)
		TSharedPtr<FFleshRingPreviewScene> PreviewScene = ViewportWidget->GetPreviewScene();
		if (PreviewScene.IsValid())
		{
			PreviewScene->GetWorld()->Tick(LEVELTICK_All, DeltaTime);
		}

		// Viewport client tick (trigger rendering)
		TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
		if (ViewportClient.IsValid())
		{
			ViewportClient->Tick(DeltaTime);
			ViewportClient->Invalidate();
		}
	}
}

void FFleshRingAssetEditor::OnObjectTransacted(UObject* Object, const FTransactionObjectEvent& TransactionEvent)
{
	// Refresh viewport when EditingAsset changes during Undo/Redo
	if (Object == EditingAsset)
	{
		// Refresh on UndoRedo event (Ctrl+Z / Ctrl+Y)
		if (TransactionEvent.GetEventType() == ETransactionObjectEventType::UndoRedo)
		{
			// Safely refresh on next tick after Undo transaction completes
			// (Calling RefreshViewport during transaction causes crash due to incomplete SkeletalMesh bone data)
			if (GEditor)
			{
				// Skip selection validation during Undo/Redo (prevent deselection in Tick)
				// Save view mode (for restoration after Undo/Redo)
				EViewModeIndex SavedViewMode = VMI_Lit;
				if (ViewportWidget.IsValid())
				{
					TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
					if (ViewportClient.IsValid())
					{
						ViewportClient->SetSkipSelectionValidation(true);
						SavedViewMode = ViewportClient->GetViewMode();
					}
				}

				GEditor->GetTimerManager()->SetTimerForNextTick(
					[WeakThis = TWeakPtr<FFleshRingAssetEditor>(SharedThis(this)), SavedViewMode]()
					{
						if (TSharedPtr<FFleshRingAssetEditor> This = WeakThis.Pin())
						{
							// Prevent SelectRing() calls during tree refresh in Undo/Redo
							// (Prevents Undo history corruption from new transaction creation)
							This->bSyncingFromViewport = true;

							// Refresh viewport
							if (This->ViewportWidget.IsValid())
							{
								This->ViewportWidget->RefreshPreview();

								// Restore view mode (prevent reset from Undo/Redo)
								if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = This->ViewportWidget->GetViewportClient())
								{
									ViewportClient->SetViewMode(SavedViewMode);
								}
							}
							if (This->SkeletonTreeWidget.IsValid())
							{
								This->SkeletonTreeWidget->RefreshTree();
							}

							// Don't call ForceRefresh() during Undo/Redo
							// (To maintain scroll position - Unreal Engine automatically refreshes property values)
							// May cause UI mismatch on array size changes (Ring add/remove) - needs testing

							// Save previous selection index (before ApplySelectionFromAsset)
							int32 PreviousSelectedRingIndex = INDEX_NONE;
							if (This->ViewportWidget.IsValid())
							{
								TSharedPtr<FFleshRingPreviewScene> PreviewScene = This->ViewportWidget->GetPreviewScene();
								if (PreviewScene.IsValid())
								{
									PreviousSelectedRingIndex = PreviewScene->GetSelectedRingIndex();
								}
							}

							// Apply selection state from asset to viewport (values restored by Undo/Redo)
							// (ApplySelectionFromAsset also sets bSyncingFromViewport internally,
							//  but it's already true so redundant setting is fine)
							This->ApplySelectionFromAsset();

							This->bSyncingFromViewport = false;

							// Only scroll details panel when selected Ring changes
							// (Maintain scroll position for Undo/Redo that only changes property values)
							int32 NewSelectedRingIndex = This->EditingAsset ? This->EditingAsset->EditorSelectedRingIndex : INDEX_NONE;
							if (NewSelectedRingIndex != PreviousSelectedRingIndex && NewSelectedRingIndex >= 0)
							{
								if (This->DetailsView.IsValid() && This->EditingAsset)
								{
									FProperty* RingsProperty = UFleshRingAsset::StaticClass()->FindPropertyByName(
										GET_MEMBER_NAME_CHECKED(UFleshRingAsset, Rings));
									if (RingsProperty)
									{
										TSharedRef<FPropertyPath> PropertyPath = FPropertyPath::CreateEmpty();
										PropertyPath->AddProperty(FPropertyInfo(RingsProperty, NewSelectedRingIndex));
										This->DetailsView->ScrollPropertyIntoView(*PropertyPath, false);
									}
								}
							}

							// Re-enable selection validation after 0.2 seconds
							// (Must release later than SetFleshRingAsset's 0.1 second Deformer initialization timer)
							if (This->ViewportWidget.IsValid() && GEditor)
							{
								TWeakPtr<FFleshRingEditorViewportClient> WeakClient = This->ViewportWidget->GetViewportClient();
								FTimerHandle TimerHandle;
								GEditor->GetTimerManager()->SetTimer(
									TimerHandle,
									[WeakClient]()
									{
										if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakClient.Pin())
										{
											Client->SetSkipSelectionValidation(false);
										}
									},
									0.2f,  // Longer than SetFleshRingAsset's 0.1 second
									false
								);
							}
						}
					});
			}
		}
	}
}

void FFleshRingAssetEditor::OnBoneSelected(FName BoneName)
{
	// Highlight the bone in viewport when bone is selected in Skeleton Tree
	if (ViewportWidget.IsValid())
	{
		TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
		if (ViewportClient.IsValid())
		{
			ViewportClient->SetSelectedBone(BoneName);
		}
	}
}

void FFleshRingAssetEditor::OnBoneSelectionCleared()
{
	// Clear skeleton tree selection when bone selection is cleared in viewport
	if (SkeletonTreeWidget.IsValid())
	{
		SkeletonTreeWidget->ClearSelection();
	}
}

void FFleshRingAssetEditor::OnBoneSelectedInViewport(FName BoneName)
{
	// Select the bone in skeleton tree when bone is picked in viewport
	if (SkeletonTreeWidget.IsValid())
	{
		SkeletonTreeWidget->SelectBone(BoneName);
	}
}

void FFleshRingAssetEditor::OnRingSelected(int32 RingIndex)
{
	// Skip viewport update if syncing from viewport (prevent circular calls)
	if (!bSyncingFromViewport)
	{
		// Get the Ring's attached bone name
		FName AttachedBoneName = NAME_None;
		if (EditingAsset && EditingAsset->Rings.IsValidIndex(RingIndex))
		{
			AttachedBoneName = EditingAsset->Rings[RingIndex].BoneName;
		}

		// Select Ring in viewport (also highlight attached bone)
		if (ViewportWidget.IsValid())
		{
			TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
			if (ViewportClient.IsValid())
			{
				ViewportClient->SelectRing(RingIndex, AttachedBoneName);
			}
		}
	}

	// Scroll details panel when selected in skeleton tree
	// (bSyncingFromViewport is false = directly selected in skeleton tree)
	// (Scroll is handled in OnRingSelectedInViewport when selected from viewport)
	if (!bSyncingFromViewport && DetailsView.IsValid() && EditingAsset && RingIndex >= 0)
	{
		FProperty* RingsProperty = UFleshRingAsset::StaticClass()->FindPropertyByName(
			GET_MEMBER_NAME_CHECKED(UFleshRingAsset, Rings));
		if (RingsProperty)
		{
			TSharedRef<FPropertyPath> PropertyPath = FPropertyPath::CreateEmpty();
			PropertyPath->AddProperty(FPropertyInfo(RingsProperty, RingIndex));
			DetailsView->ScrollPropertyIntoView(*PropertyPath, false);
		}
	}
}

void FFleshRingAssetEditor::OnRingSelectedInViewport(int32 RingIndex, EFleshRingSelectionType SelectionType)
{
	// Set flag to prevent circular calls
	bSyncingFromViewport = true;

	// Set attached bone highlight in viewport
	if (ViewportWidget.IsValid() && EditingAsset && EditingAsset->Rings.IsValidIndex(RingIndex))
	{
		FName AttachedBoneName = EditingAsset->Rings[RingIndex].BoneName;
		TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
		if (ViewportClient.IsValid())
		{
			ViewportClient->SetSelectedBone(AttachedBoneName);
		}
	}

	// Select Ring in tree (OnRingSelected is called in this process -> Details panel highlight)
	if (SkeletonTreeWidget.IsValid())
	{
		SkeletonTreeWidget->SelectRingByIndex(RingIndex);
	}

	// Scroll details panel when selected in viewport (replaces HighlightProperty)
	if (DetailsView.IsValid() && EditingAsset && RingIndex >= 0)
	{
		FProperty* RingsProperty = UFleshRingAsset::StaticClass()->FindPropertyByName(
			GET_MEMBER_NAME_CHECKED(UFleshRingAsset, Rings));
		if (RingsProperty)
		{
			TSharedRef<FPropertyPath> PropertyPath = FPropertyPath::CreateEmpty();
			PropertyPath->AddProperty(FPropertyInfo(RingsProperty, RingIndex));
			DetailsView->ScrollPropertyIntoView(*PropertyPath, false);
		}
	}

	bSyncingFromViewport = false;
}

void FFleshRingAssetEditor::OnAddRingRequested(FName BoneName, UStaticMesh* SelectedMesh)
{
	// Add Ring to selected bone
	if (EditingAsset && !BoneName.IsNone())
	{
		// Undo/Redo support
		FScopedTransaction Transaction(FText::FromString(TEXT("Add Ring")));
		EditingAsset->Modify();

		// Add new Ring
		FFleshRingSettings NewRing;
		NewRing.BoneName = BoneName;

		// Generate unique RingName automatically
		NewRing.RingName = EditingAsset->MakeUniqueRingName(FName(TEXT("FleshRing")));

		// Set selected mesh
		if (SelectedMesh)
		{
			NewRing.RingMesh = SelectedMesh;
		}

		// Calculate default rotation based on bone's weighted children
		if (ViewportWidget.IsValid())
		{
			if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
			{
				FRotator DefaultRotation = ViewportClient->CalculateDefaultRingRotationForBone(BoneName);

				// Set Ring rotation
				NewRing.RingEulerRotation = DefaultRotation;
				NewRing.RingRotation = DefaultRotation.Quaternion();

				// Also set Mesh rotation (MeshRotation is used in Auto mode)
				NewRing.MeshEulerRotation = DefaultRotation;
				NewRing.MeshRotation = DefaultRotation.Quaternion();
			}
		}

		EditingAsset->Rings.Add(NewRing);

		// Select newly added Ring
		int32 NewRingIndex = EditingAsset->Rings.Num() - 1;
		EditingAsset->EditorSelectedRingIndex = NewRingIndex;

		// Refresh viewport
		RefreshViewport();

		// Refresh skeleton tree and select new Ring
		if (SkeletonTreeWidget.IsValid())
		{
			SkeletonTreeWidget->RefreshTree();
			SkeletonTreeWidget->SelectRingByIndex(NewRingIndex);
		}

		// Select new Ring after refreshing Details panel (ForceRefresh resets highlight)
		if (DetailsView.IsValid())
		{
			DetailsView->ForceRefresh();
		}

		// Select newly added Ring (including details panel highlight)
		OnRingSelected(NewRingIndex);
	}
}

void FFleshRingAssetEditor::OnAddRingAtPositionRequested(FName BoneName, const FVector& LocalOffset, const FRotator& LocalRotation, UStaticMesh* SelectedMesh)
{
	// Add Ring from viewport right-click (with position and mesh, mesh is optional)
	if (!EditingAsset || BoneName.IsNone())
	{
		return;
	}

	// Undo/Redo support
	FScopedTransaction Transaction(NSLOCTEXT("FleshRingEditor", "AddRingAtPosition", "Add Ring at Position"));
	EditingAsset->Modify();

	// Create new Ring
	FFleshRingSettings NewRing;
	NewRing.BoneName = BoneName;

	// Generate unique RingName automatically
	NewRing.RingName = EditingAsset->MakeUniqueRingName(FName(TEXT("FleshRing")));

	// Set both RingOffset and MeshOffset to the same position (bone local space)
	NewRing.RingOffset = LocalOffset;
	NewRing.MeshOffset = LocalOffset;

	// Set Ring rotation (so green line direction becomes Z axis)
	NewRing.RingEulerRotation = LocalRotation;
	NewRing.RingRotation = LocalRotation.Quaternion();

	// Also set Mesh rotation (MeshRotation is used in Auto mode)
	NewRing.MeshEulerRotation = LocalRotation;
	NewRing.MeshRotation = LocalRotation.Quaternion();

	// Set selected mesh (add without mesh if nullptr)
	if (SelectedMesh)
	{
		NewRing.RingMesh = SelectedMesh;
	}

	EditingAsset->Rings.Add(NewRing);

	// Select newly added Ring
	int32 NewRingIndex = EditingAsset->Rings.Num() - 1;
	EditingAsset->EditorSelectedRingIndex = NewRingIndex;

	// Refresh viewport
	RefreshViewport();

	// Refresh skeleton tree and select new Ring
	if (SkeletonTreeWidget.IsValid())
	{
		SkeletonTreeWidget->RefreshTree();
		SkeletonTreeWidget->SelectRingByIndex(NewRingIndex);
	}

	// Select new Ring after refreshing Details panel (ForceRefresh resets highlight)
	if (DetailsView.IsValid())
	{
		DetailsView->ForceRefresh();
	}

	// Select newly added Ring (including details panel highlight)
	OnRingSelected(NewRingIndex);
}

void FFleshRingAssetEditor::OnFocusCameraRequested()
{
	// Request camera focus to viewport
	if (ViewportWidget.IsValid())
	{
		TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
		if (ViewportClient.IsValid())
		{
			ViewportClient->FocusOnMesh();
		}
	}
}

void FFleshRingAssetEditor::HandleRingDeleted()
{
	// Common Ring deletion handling (called from viewport/tree/details)
	bSyncingFromViewport = true;

	// Refresh viewport (regenerate Ring mesh components)
	if (ViewportWidget.IsValid())
	{
		ViewportWidget->RefreshPreview();

		// Update ViewportClient selection state
		TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
		if (ViewportClient.IsValid())
		{
			ViewportClient->SetSelectionType(EFleshRingSelectionType::None);
		}
	}

	// Refresh tree
	if (SkeletonTreeWidget.IsValid())
	{
		SkeletonTreeWidget->RefreshTree();
	}

	bSyncingFromViewport = false;
}

void FFleshRingAssetEditor::ApplySelectionFromAsset()
{
	if (!EditingAsset || !ViewportWidget.IsValid())
	{
		return;
	}

	// Read selection state from asset
	int32 RingIndex = EditingAsset->EditorSelectedRingIndex;
	EFleshRingSelectionType SelectionType = EditingAsset->EditorSelectionType;

	// Set selection index in PreviewScene
	TSharedPtr<FFleshRingPreviewScene> PreviewScene = ViewportWidget->GetPreviewScene();
	if (PreviewScene.IsValid())
	{
		PreviewScene->SetSelectedRingIndex(RingIndex);
	}

	// Set selection type in ViewportClient
	TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
	if (ViewportClient.IsValid())
	{
		ViewportClient->SetSelectionType(SelectionType);

		// Also highlight attached bone if Ring is selected
		if (RingIndex >= 0 && EditingAsset->Rings.IsValidIndex(RingIndex))
		{
			FName AttachedBoneName = EditingAsset->Rings[RingIndex].BoneName;
			ViewportClient->SetSelectedBone(AttachedBoneName);
		}
		else
		{
			// Clear bone highlight when deselected
			ViewportClient->ClearSelectedBone();
		}
	}

	// Select Ring in tree
	// Set bSyncingFromViewport = true to prevent SelectRing() call in OnRingSelected
	// (SelectRing() creates a new transaction which corrupts Undo history)
	if (SkeletonTreeWidget.IsValid())
	{
		bSyncingFromViewport = true;
		SkeletonTreeWidget->SelectRingByIndex(RingIndex);
		bSyncingFromViewport = false;
	}
}

void FFleshRingAssetEditor::OnRingSelectionChangedFromDetails(int32 RingIndex)
{
	// Prevent circular calls (when already selecting from viewport/tree)
	if (bSyncingFromViewport)
	{
		return;
	}

	// Sync viewport/tree when Ring is clicked in details panel
	bSyncingFromViewport = true;
	ApplySelectionFromAsset();
	bSyncingFromViewport = false;
}

void FFleshRingAssetEditor::ShowBakeOverlay(bool bShow, const FText& Message)
{
	if (bShow && !bBakeOverlayVisible)
	{
		// Create overlay window
		TSharedPtr<SWindow> ParentWindow = FSlateApplication::Get().FindWidgetWindow(ViewportWidget.ToSharedRef());
		if (!ParentWindow.IsValid())
		{
			// Fallback: use active window
			ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
		}

		if (ParentWindow.IsValid())
		{
			FText DisplayMessage = Message.IsEmpty() ? LOCTEXT("BakingOverlay", "Baking mesh...\nPlease wait.") : Message;

			BakeOverlayWindow = SNew(SWindow)
				.Type(EWindowType::Normal)
				.Style(&FAppStyle::Get().GetWidgetStyle<FWindowStyle>("Window"))
				.Title(LOCTEXT("BakeOverlayTitle", "Baking"))
				.SizingRule(ESizingRule::FixedSize)
				.ClientSize(FVector2D(300, 100))
				.SupportsMaximize(false)
				.SupportsMinimize(false)
				.HasCloseButton(false)
				.CreateTitleBar(true)
				.IsTopmostWindow(true)
				.FocusWhenFirstShown(true)
				[
					SNew(SBox)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Padding(20.f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.HAlign(HAlign_Center)
						[
							SNew(SCircularThrobber)
							.Radius(16.f)
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.HAlign(HAlign_Center)
						.Padding(0, 10, 0, 0)
						[
							SNew(STextBlock)
							.Text(DisplayMessage)
							.Justification(ETextJustify::Center)
						]
					]
				];

			FSlateApplication::Get().AddWindowAsNativeChild(BakeOverlayWindow.ToSharedRef(), ParentWindow.ToSharedRef(), true);

			// Position at center of parent window
			FVector2D ParentSize = ParentWindow->GetClientSizeInScreen();
			FVector2D ParentPos = ParentWindow->GetPositionInScreen();
			FVector2D OverlaySize = BakeOverlayWindow->GetClientSizeInScreen();
			FVector2D CenteredPos = ParentPos + (ParentSize - OverlaySize) * 0.5f;
			BakeOverlayWindow->MoveWindowTo(CenteredPos);
		}

		bBakeOverlayVisible = true;
	}
	else if (!bShow && bBakeOverlayVisible)
	{
		// Remove overlay window
		if (BakeOverlayWindow.IsValid())
		{
			BakeOverlayWindow->RequestDestroyWindow();
			BakeOverlayWindow.Reset();
		}

		bBakeOverlayVisible = false;
	}
}

void FFleshRingAssetEditor::CreateEditorModeManager()
{
	// Call parent to create EditorModeManager
	FAssetEditorToolkit::CreateEditorModeManager();

	// Register EdMode to global registry (if not already registered)
	if (!FEditorModeRegistry::Get().GetFactoryMap().Contains(FFleshRingEdMode::EM_FleshRingEdModeId))
	{
		FEditorModeRegistry::Get().RegisterMode<FFleshRingEdMode>(FFleshRingEdMode::EM_FleshRingEdModeId);
	}

	// Setup EditorModeManager with FleshRing EdMode
	GetEditorModeManager().SetDefaultMode(FFleshRingEdMode::EM_FleshRingEdModeId);
	GetEditorModeManager().ActivateDefaultMode();
	GetEditorModeManager().SetWidgetMode(UE::Widget::WM_Translate);
}

void FFleshRingAssetEditor::BindCommands()
{
	const FFleshRingEditorCommands& Commands = FFleshRingEditorCommands::Get();

	ToolkitCommands->MapAction(
		Commands.SetWidgetModeNone,
		FExecuteAction::CreateSP(this, &FFleshRingAssetEditor::SetWidgetMode, UE::Widget::WM_None));

	ToolkitCommands->MapAction(
		Commands.SetWidgetModeTranslate,
		FExecuteAction::CreateSP(this, &FFleshRingAssetEditor::SetWidgetMode, UE::Widget::WM_Translate));

	ToolkitCommands->MapAction(
		Commands.SetWidgetModeRotate,
		FExecuteAction::CreateSP(this, &FFleshRingAssetEditor::SetWidgetMode, UE::Widget::WM_Rotate));

	ToolkitCommands->MapAction(
		Commands.SetWidgetModeScale,
		FExecuteAction::CreateSP(this, &FFleshRingAssetEditor::SetWidgetMode, UE::Widget::WM_Scale));

	ToolkitCommands->MapAction(
		Commands.ToggleCoordSystem,
		FExecuteAction::CreateSP(this, &FFleshRingAssetEditor::ToggleCoordSystem));

	// Debug Visualization (number keys)
	ToolkitCommands->MapAction(Commands.ToggleDebugVisualization,
		FExecuteAction::CreateSP(this, &FFleshRingAssetEditor::OnToggleDebugVisualization));
	ToolkitCommands->MapAction(Commands.ToggleSdfVolume,
		FExecuteAction::CreateSP(this, &FFleshRingAssetEditor::OnToggleSdfVolume));
	ToolkitCommands->MapAction(Commands.ToggleAffectedVertices,
		FExecuteAction::CreateSP(this, &FFleshRingAssetEditor::OnToggleAffectedVertices));
	ToolkitCommands->MapAction(Commands.ToggleBulgeHeatmap,
		FExecuteAction::CreateSP(this, &FFleshRingAssetEditor::OnToggleBulgeHeatmap));

	// Show toggles (Shift+number)
	ToolkitCommands->MapAction(Commands.ToggleSkeletalMesh,
		FExecuteAction::CreateSP(this, &FFleshRingAssetEditor::OnToggleSkeletalMesh));
	ToolkitCommands->MapAction(Commands.ToggleRingGizmos,
		FExecuteAction::CreateSP(this, &FFleshRingAssetEditor::OnToggleRingGizmos));
	ToolkitCommands->MapAction(Commands.ToggleRingMeshes,
		FExecuteAction::CreateSP(this, &FFleshRingAssetEditor::OnToggleRingMeshes));
	ToolkitCommands->MapAction(Commands.ToggleBulgeRange,
		FExecuteAction::CreateSP(this, &FFleshRingAssetEditor::OnToggleBulgeRange));

	// Debug options (Ctrl+number)
	ToolkitCommands->MapAction(Commands.ToggleSDFSlice,
		FExecuteAction::CreateSP(this, &FFleshRingAssetEditor::OnToggleSDFSlice));
	ToolkitCommands->MapAction(Commands.ToggleBulgeArrows,
		FExecuteAction::CreateSP(this, &FFleshRingAssetEditor::OnToggleBulgeArrows));
}

void FFleshRingAssetEditor::SetWidgetMode(UE::Widget::EWidgetMode Mode)
{
	// Use toolkit's EditorModeManager directly (shared with viewport)
	GetEditorModeManager().SetWidgetMode(Mode);

	// Invalidate viewport to reflect change
	if (ViewportWidget.IsValid())
	{
		if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
		{
			ViewportClient->Invalidate();
		}
	}
}

void FFleshRingAssetEditor::ToggleCoordSystem()
{
	if (ViewportWidget.IsValid())
	{
		if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
		{
			ViewportClient->ToggleLocalCoordSystem();
		}
	}
}

void FFleshRingAssetEditor::OnToggleDebugVisualization()
{
	if (ViewportWidget.IsValid())
	{
		if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
		{
			ViewportClient->ToggleShowDebugVisualization();
			ViewportClient->Invalidate();
		}
	}
}

void FFleshRingAssetEditor::OnToggleSdfVolume()
{
	if (ViewportWidget.IsValid())
	{
		if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
		{
			ViewportClient->ToggleShowSdfVolume();
			ViewportClient->Invalidate();
		}
	}
}

void FFleshRingAssetEditor::OnToggleAffectedVertices()
{
	if (ViewportWidget.IsValid())
	{
		if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
		{
			ViewportClient->ToggleShowAffectedVertices();
			ViewportClient->Invalidate();
		}
	}
}

void FFleshRingAssetEditor::OnToggleBulgeHeatmap()
{
	if (ViewportWidget.IsValid())
	{
		if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
		{
			ViewportClient->ToggleShowBulgeHeatmap();
			ViewportClient->Invalidate();
		}
	}
}

void FFleshRingAssetEditor::OnToggleSkeletalMesh()
{
	if (ViewportWidget.IsValid())
	{
		if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
		{
			ViewportClient->ToggleShowSkeletalMesh();
			ViewportClient->Invalidate();
		}
	}
}

void FFleshRingAssetEditor::OnToggleRingGizmos()
{
	if (ViewportWidget.IsValid())
	{
		if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
		{
			ViewportClient->ToggleShowRingGizmos();
			ViewportClient->Invalidate();
		}
	}
}

void FFleshRingAssetEditor::OnToggleRingMeshes()
{
	if (ViewportWidget.IsValid())
	{
		if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
		{
			ViewportClient->ToggleShowRingMeshes();
			ViewportClient->Invalidate();
		}
	}
}

void FFleshRingAssetEditor::OnToggleBulgeRange()
{
	if (ViewportWidget.IsValid())
	{
		if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
		{
			ViewportClient->ToggleShowBulgeRange();
			ViewportClient->Invalidate();
		}
	}
}

void FFleshRingAssetEditor::OnToggleSDFSlice()
{
	if (ViewportWidget.IsValid())
	{
		if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
		{
			ViewportClient->ToggleShowSDFSlice();
			ViewportClient->Invalidate();
		}
	}
}

void FFleshRingAssetEditor::OnToggleBulgeArrows()
{
	if (ViewportWidget.IsValid())
	{
		if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
		{
			ViewportClient->ToggleShowBulgeArrows();
			ViewportClient->Invalidate();
		}
	}
}

#undef LOCTEXT_NAMESPACE
