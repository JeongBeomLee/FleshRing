// Copyright 2026 LgThx. All Rights Reserved.

#include "SFleshRingEditorViewport.h"
#include "SFleshRingEditorViewportToolbar.h"
#include "FleshRingPreviewScene.h"
#include "FleshRingEditorViewportClient.h"
#include "FleshRingEdMode.h"
#include "FleshRingAsset.h"
#include "FleshRingComponent.h"
#include "Engine/SkeletalMesh.h"
#include "RenderingThread.h"  // For FlushRenderingCommands
#include "Slate/SceneViewport.h"
#include "EditorModeRegistry.h"
#include "BufferVisualizationMenuCommands.h"
#include "EditorViewportCommands.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/AppStyle.h"
#include "Viewports.h"

#define LOCTEXT_NAMESPACE "FleshRingEditorViewport"

void SFleshRingEditorViewport::Construct(const FArguments& InArgs)
{
	EditingAsset = InArgs._Asset;
	ModeTools = InArgs._ModeTools;

	// Create preview scene
	FAdvancedPreviewScene::ConstructionValues CVS;
	CVS.bCreatePhysicsScene = false;
	CVS.LightBrightness = 3.0f;
	CVS.SkyBrightness = 1.0f;
	PreviewScene = MakeShared<FFleshRingPreviewScene>(CVS);

	// Construct parent class
	SEditorViewport::Construct(SEditorViewport::FArguments());

	// Set Asset
	if (EditingAsset.IsValid())
	{
		SetAsset(EditingAsset.Get());
	}
}

SFleshRingEditorViewport::~SFleshRingEditorViewport()
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->Viewport = nullptr;
	}

	// ModeTools is owned by FAssetEditorToolkit, no cleanup needed here
}

void SFleshRingEditorViewport::SetAsset(UFleshRingAsset* InAsset)
{
	EditingAsset = InAsset;

	if (ViewportClient.IsValid())
	{
		ViewportClient->SetAsset(InAsset);
	}
}

void SFleshRingEditorViewport::RefreshPreview()
{
	if (PreviewScene.IsValid() && EditingAsset.IsValid())
	{
		// Full Asset refresh (mesh + component + Ring visualization)
		PreviewScene->SetFleshRingAsset(EditingAsset.Get());
	}

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

void SFleshRingEditorViewport::UpdateRingTransformsOnly(int32 DirtyRingIndex)
{
	if (PreviewScene.IsValid())
	{
		// Update only FleshRingComponent transforms (keep Deformer, prevent flickering)
		// Pass DirtyRingIndex to process only that Ring
		UFleshRingComponent* FleshRingComp = PreviewScene->GetFleshRingComponent();
		if (FleshRingComp)
		{
			FleshRingComp->UpdateRingTransforms(DirtyRingIndex);
		}
	}

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

void SFleshRingEditorViewport::RefreshSDFOnly()
{
	if (PreviewScene.IsValid())
	{
		UFleshRingComponent* FleshRingComp = PreviewScene->GetFleshRingComponent();
		if (FleshRingComp)
		{
			// 1. Regenerate SDF (based on VirtualBand parameters)
			FleshRingComp->RefreshSDF();
			FlushRenderingCommands();  // Wait for GPU work to complete

			// 2. Update transforms + invalidate cache (trigger deformation recalculation)
			FleshRingComp->UpdateRingTransforms();
		}
	}

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

TSharedRef<SEditorViewport> SFleshRingEditorViewport::GetViewportWidget()
{
	return SharedThis(this);
}

TSharedPtr<FExtender> SFleshRingEditorViewport::GetExtenders() const
{
	TSharedPtr<FExtender> Extender = MakeShared<FExtender>();

	// Add buffer visualization submenu to view mode menu
	TWeakPtr<const SFleshRingEditorViewport> WeakViewport = SharedThis(this);
	Extender->AddMenuExtension(
		TEXT("ViewMode"),
		EExtensionHook::After,
		GetCommandList(),
		FMenuExtensionDelegate::CreateLambda(
			[WeakViewport](FMenuBuilder& InMenuBuilder)
			{
				InMenuBuilder.AddSubMenu(
					LOCTEXT("VisualizeBufferViewModeDisplayName", "Buffer Visualization"),
					LOCTEXT("BufferVisualizationMenu_ToolTip", "Select a mode for buffer visualization"),
					FNewMenuDelegate::CreateStatic(&FBufferVisualizationMenuCommands::BuildVisualisationSubMenu),
					FUIAction(
						FExecuteAction(),
						FCanExecuteAction(),
						FIsActionChecked::CreateLambda(
							[WeakViewport]()
							{
								if (TSharedPtr<const SFleshRingEditorViewport> ViewportPtr = WeakViewport.Pin())
								{
									if (ViewportPtr->GetViewportClient().IsValid())
									{
										return ViewportPtr->GetViewportClient()->IsViewModeEnabled(VMI_VisualizeBuffer);
									}
								}
								return false;
							}
						)
					),
					NAME_None,  // InExtensionHook
					EUserInterfaceActionType::RadioButton,
					/* bInOpenSubMenuOnClick = */ false,
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "EditorViewport.VisualizeBufferMode")
				);
			}
		)
	);

	return Extender;
}

void SFleshRingEditorViewport::OnFloatingButtonClicked()
{
	// Handle floating button click if needed
}

TSharedRef<FEditorViewportClient> SFleshRingEditorViewport::MakeEditorViewportClient()
{
	ViewportClient = MakeShared<FFleshRingEditorViewportClient>(
		ModeTools,
		PreviewScene.Get(),
		SharedThis(this));

	// Connect ViewportClient to EdMode (get from ModeTools, not static instance)
	if (ModeTools)
	{
		FEdMode* ActiveMode = ModeTools->GetActiveMode(FFleshRingEdMode::EM_FleshRingEdModeId);
		FleshRingEdMode = static_cast<FFleshRingEdMode*>(ActiveMode);
		if (FleshRingEdMode)
		{
			FleshRingEdMode->SetViewportClient(ViewportClient.Get());
		}
	}

	if (EditingAsset.IsValid())
	{
		ViewportClient->SetAsset(EditingAsset.Get());
	}

	return ViewportClient.ToSharedRef();
}

void SFleshRingEditorViewport::OnFocusViewportToSelection()
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->FocusOnMesh();
	}
}

void SFleshRingEditorViewport::PopulateViewportOverlays(TSharedRef<SOverlay> Overlay)
{
	// Construct default overlay
	SEditorViewport::PopulateViewportOverlays(Overlay);
}

TSharedRef<SWidget> SFleshRingEditorViewport::MakeToolbar()
{
	return SNew(SFleshRingEditorViewportToolbar, SharedThis(this));
}

void SFleshRingEditorViewport::BindCommands()
{
	// Parent class command binding (view mode, camera, etc.)
	SEditorViewport::BindCommands();

	// Unbind engine's CycleTransformGizmoCoordSystem (Ctrl+`) from viewport
	// Our GetWidgetCoordSystemSpace() always returns COORD_World, breaking the engine's cycle logic
	// Instead, we use FFleshRingAssetEditor's ToggleCoordSystem via ToolkitCommands
	const FEditorViewportCommands& ViewportCommands = FEditorViewportCommands::Get();
	CommandList->UnmapAction(ViewportCommands.CycleTransformGizmoCoordSystem);

	// Buffer visualization command binding
	FBufferVisualizationMenuCommands::Get().BindCommands(*CommandList, Client);

	// NOTE: FleshRing editor commands (QWER, Ctrl+`, number keys, etc.)
	// are NOT bound here - they are bound in FFleshRingAssetEditor::BindCommands()
	// Binding in both places would cause double-execution (double-toggle for Ctrl+`)
}

void SFleshRingEditorViewport::OnCycleCoordinateSystem()
{
	// This override is kept for safety but shouldn't be called
	// since we unbound CycleTransformGizmoCoordSystem in BindCommands()
	if (ViewportClient.IsValid())
	{
		ViewportClient->ToggleLocalCoordSystem();
	}
}

#undef LOCTEXT_NAMESPACE
