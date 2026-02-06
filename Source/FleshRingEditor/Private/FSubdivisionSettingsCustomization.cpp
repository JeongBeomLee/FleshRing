// Copyright 2026 LgThx. All Rights Reserved.

#include "FSubdivisionSettingsCustomization.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "IDetailGroup.h"
#include "FleshRingAsset.h"
#include "FleshRingComponent.h"
#include "FleshRingTypes.h"
#include "FleshRingAssetEditor.h"
#include "FleshRingDeformerInstance.h"
#include "SFleshRingEditorViewport.h"
#include "FleshRingPreviewScene.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"
#include "RenderingThread.h"
#include "Misc/ScopedSlowTask.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"
#include "UObject/UObjectGlobals.h"  // For CollectGarbage
#include "FileHelpers.h"  // For FEditorFileUtils::PromptForCheckoutAndSave

#define LOCTEXT_NAMESPACE "SubdivisionSettingsCustomization"

FSubdivisionSettingsCustomization::FSubdivisionSettingsCustomization()
{
}

FSubdivisionSettingsCustomization::~FSubdivisionSettingsCustomization()
{
	// Clean up any in-progress async bake
	if (bAsyncBakeInProgress)
	{
		CleanupAsyncBake(true);
	}
}

TSharedRef<IPropertyTypeCustomization> FSubdivisionSettingsCustomization::MakeInstance()
{
	return MakeShareable(new FSubdivisionSettingsCustomization);
}

void FSubdivisionSettingsCustomization::CustomizeHeader(
	TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	MainPropertyHandle = PropertyHandle;

	// Hide header - only show category name
	// (Prevents duplicate display of struct name "Subdivision Settings")
}

void FSubdivisionSettingsCustomization::CustomizeChildren(
	TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// Get child property handles
	TSharedPtr<IPropertyHandle> bEnableSubdivisionHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, bEnableSubdivision));
	TSharedPtr<IPropertyHandle> MinEdgeLengthHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, MinEdgeLength));
	TSharedPtr<IPropertyHandle> PreviewSubdivisionLevelHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, PreviewSubdivisionLevel));
	TSharedPtr<IPropertyHandle> PreviewBoneHopCountHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, PreviewBoneHopCount));
	TSharedPtr<IPropertyHandle> PreviewBoneWeightThresholdHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, PreviewBoneWeightThreshold));
	TSharedPtr<IPropertyHandle> MaxSubdivisionLevelHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, MaxSubdivisionLevel));
	TSharedPtr<IPropertyHandle> SubdividedMeshHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, SubdividedMesh));
	TSharedPtr<IPropertyHandle> BakedMeshHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, BakedMesh));

	// =====================================
	// Common Settings (Top-level)
	// =====================================
	if (bEnableSubdivisionHandle.IsValid())
	{
		ChildBuilder.AddProperty(bEnableSubdivisionHandle.ToSharedRef());
	}
	if (MinEdgeLengthHandle.IsValid())
	{
		ChildBuilder.AddProperty(MinEdgeLengthHandle.ToSharedRef());
	}

	// =====================================
	// Editor Preview Subgroup
	// =====================================
	IDetailGroup& EditorPreviewGroup = ChildBuilder.AddGroup(
		TEXT("EditorPreview"),
		LOCTEXT("EditorPreviewGroup", "Editor Preview")
	);

	if (PreviewSubdivisionLevelHandle.IsValid())
	{
		EditorPreviewGroup.AddPropertyRow(PreviewSubdivisionLevelHandle.ToSharedRef());
	}
	if (PreviewBoneHopCountHandle.IsValid())
	{
		EditorPreviewGroup.AddPropertyRow(PreviewBoneHopCountHandle.ToSharedRef());
	}
	if (PreviewBoneWeightThresholdHandle.IsValid())
	{
		EditorPreviewGroup.AddPropertyRow(PreviewBoneWeightThresholdHandle.ToSharedRef());
	}

	// Refresh Preview button
	EditorPreviewGroup.AddWidgetRow()
	.WholeRowContent()
	[
		SNew(SBox)
		.Padding(FMargin(0, 2, 0, 2))
		.HAlign(HAlign_Center)
		[
			SNew(SBox)
			.MinDesiredWidth(360.0f)
			[
				SNew(SButton)
				.OnClicked(this, &FSubdivisionSettingsCustomization::OnRefreshPreviewClicked)
				[
					SNew(SBox)
					.HAlign(HAlign_Center)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0, 0, 8, 0)
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush("Icons.Refresh"))
							.ColorAndOpacity(FSlateColor(FLinearColor(0.3f, 0.7f, 1.0f)))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("RefreshPreview", "Refresh Preview Mesh"))
						]
					]
				]
			]
		]
	];

	// =====================================
	// Baked Mesh Subgroup (For runtime, deformation applied)
	// =====================================
	IDetailGroup& BakedMeshGroup = ChildBuilder.AddGroup(
		TEXT("BakedMesh"),
		LOCTEXT("BakedMeshGroup", "Baked Mesh")
	);

	if (MaxSubdivisionLevelHandle.IsValid())
	{
		BakedMeshGroup.AddPropertyRow(MaxSubdivisionLevelHandle.ToSharedRef());
	}

	// Bake + Clear buttons
	BakedMeshGroup.AddWidgetRow()
	.WholeRowContent()
	[
		SNew(SBox)
		.Padding(FMargin(0, 2, 0, 2))
		.HAlign(HAlign_Center)
		[
			SNew(SHorizontalBox)
			// Bake button (green)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0, 0, 4, 0)
			[
				SNew(SBox)
				.MinDesiredWidth(180.0f)
				[
					SNew(SButton)
					.OnClicked(this, &FSubdivisionSettingsCustomization::OnBakeMeshClicked)
					[
						SNew(SBox)
						.HAlign(HAlign_Center)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(0, 0, 4, 0)
							[
								SNew(SImage)
								.Image(FAppStyle::GetBrush("Icons.Plus"))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.3f, 0.9f, 0.3f)))
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("BakeMesh", "Bake"))
							]
						]
					]
				]
			]
			// Clear button (red)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4, 0, 0, 0)
			[
				SNew(SBox)
				.MinDesiredWidth(180.0f)
				[
					SNew(SButton)
					.OnClicked(this, &FSubdivisionSettingsCustomization::OnClearBakedMeshClicked)
					[
						SNew(SBox)
						.HAlign(HAlign_Center)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(0, 0, 4, 0)
							[
								SNew(SImage)
								.Image(FAppStyle::GetBrush("Icons.X"))
								.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.3f, 0.3f)))
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("ClearMesh", "Clear"))
							]
						]
					]
				]
			]
		]
	];

	// Baked Mesh property (read-only)
	if (BakedMeshHandle.IsValid())
	{
		BakedMeshGroup.AddPropertyRow(BakedMeshHandle.ToSharedRef());
	}
}

UFleshRingAsset* FSubdivisionSettingsCustomization::GetOuterAsset() const
{
	if (!MainPropertyHandle.IsValid())
	{
		return nullptr;
	}

	TArray<UObject*> OuterObjects;
	MainPropertyHandle->GetOuterObjects(OuterObjects);

	if (OuterObjects.Num() > 0)
	{
		return Cast<UFleshRingAsset>(OuterObjects[0]);
	}
	return nullptr;
}

bool FSubdivisionSettingsCustomization::IsSubdivisionEnabled() const
{
	UFleshRingAsset* Asset = GetOuterAsset();
	return Asset && Asset->SubdivisionSettings.bEnableSubdivision;
}

void FSubdivisionSettingsCustomization::SaveAsset(UFleshRingAsset* Asset)
{
	if (!Asset)
	{
		return;
	}

	UPackage* Package = Asset->GetOutermost();
	if (Package && Package->IsDirty())
	{
		// Fully flush rendering before checkout dialog
		// Ensures render resources for newly created meshes like BakedMesh are fully initialized
		// before the checkout dialog appears (render thread synchronization)
		FlushRenderingCommands();

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Package);
		FEditorFileUtils::PromptForCheckoutAndSave(PackagesToSave, false, false);
	}
}

FReply FSubdivisionSettingsCustomization::OnRefreshPreviewClicked()
{
	UFleshRingAsset* Asset = GetOuterAsset();
	if (Asset && GEditor)
	{
		// Find editor and force regenerate PreviewScene mesh
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (AssetEditorSubsystem)
		{
			TArray<IAssetEditorInstance*> Editors = AssetEditorSubsystem->FindEditorsForAsset(Asset);
			for (IAssetEditorInstance* Editor : Editors)
			{
				FFleshRingAssetEditor* FleshRingEditor = static_cast<FFleshRingAssetEditor*>(Editor);
				if (FleshRingEditor)
				{
					FleshRingEditor->ForceRefreshPreviewMesh();
					break;
				}
			}
		}
	}
	return FReply::Handled();
}

FReply FSubdivisionSettingsCustomization::OnGenerateSubdividedMeshClicked()
{
	UFleshRingAsset* Asset = GetOuterAsset();
	if (Asset)
	{
		// Get PreviewComponent from the editor opened via UAssetEditorSubsystem
		UFleshRingComponent* PreviewComponent = nullptr;

		if (GEditor)
		{
			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (AssetEditorSubsystem)
			{
				TArray<IAssetEditorInstance*> Editors = AssetEditorSubsystem->FindEditorsForAsset(Asset);
				for (IAssetEditorInstance* Editor : Editors)
				{
					// FFleshRingAssetEditor inherits from FAssetEditorToolkit
					FFleshRingAssetEditor* FleshRingEditor = static_cast<FFleshRingAssetEditor*>(Editor);
					if (FleshRingEditor)
					{
						PreviewComponent = FleshRingEditor->GetPreviewFleshRingComponent();
						if (PreviewComponent)
						{
							break;
						}
					}
				}
			}
		}

		Asset->GenerateSubdividedMesh(PreviewComponent);
	}
	return FReply::Handled();
}

FReply FSubdivisionSettingsCustomization::OnClearSubdividedMeshClicked()
{
	UFleshRingAsset* Asset = GetOuterAsset();
	if (Asset)
	{
		Asset->ClearSubdividedMesh();
		// Prevent memory leak: Execute GC immediately on button click
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	return FReply::Handled();
}

FReply FSubdivisionSettingsCustomization::OnBakeMeshClicked()
{
	// Ignore if bake is already in progress
	if (bAsyncBakeInProgress)
	{
		return FReply::Handled();
	}

	UFleshRingAsset* Asset = GetOuterAsset();
	if (!Asset)
	{
		return FReply::Handled();
	}

	// Order matters: Find FleshRingEditor first before calling CloseAllEditorsForAsset
	// (CloseAllEditorsForAsset triggers editor close events which can corrupt PreviewSubdividedMesh)

	// Get editor and PreviewComponent through UAssetEditorSubsystem
	FFleshRingAssetEditor* FleshRingEditor = nullptr;
	UFleshRingComponent* PreviewComponent = nullptr;

	if (GEditor)
	{
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (AssetEditorSubsystem)
		{
			TArray<IAssetEditorInstance*> Editors = AssetEditorSubsystem->FindEditorsForAsset(Asset);
			for (IAssetEditorInstance* Editor : Editors)
			{
				FleshRingEditor = static_cast<FFleshRingAssetEditor*>(Editor);
				if (FleshRingEditor)
				{
					PreviewComponent = FleshRingEditor->GetPreviewFleshRingComponent();
					if (PreviewComponent)
					{
						break;
					}
				}
			}
		}
	}

	if (!PreviewComponent || !FleshRingEditor)
	{
		return FReply::Handled();
	}

	// Clean up PreviewScene's PreviewSubdividedMesh before calling CloseAllEditorsForAsset
	// (Prevents editor close events from corrupting render resources)
	TSharedPtr<SFleshRingEditorViewport> ViewportWidget = FleshRingEditor->GetViewportWidget();
	if (ViewportWidget.IsValid())
	{
		TSharedPtr<FFleshRingPreviewScene> PreviewScene = ViewportWidget->GetPreviewScene();
		if (PreviewScene.IsValid() && PreviewScene->HasValidPreviewMesh())
		{
			PreviewScene->ClearPreviewMesh();
			FlushRenderingCommands();
		}
	}

	// Close existing BakedMesh if it's open in another editor (crash prevention)
	if (Asset->HasBakedMesh())
	{
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (AssetEditorSubsystem)
		{
			USkeletalMesh* ExistingBakedMesh = Asset->SubdivisionSettings.BakedMesh;
			if (ExistingBakedMesh)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(ExistingBakedMesh);
			}
		}
	}

	// Force initialize if Deformer doesn't exist (allows baking even with subdivision OFF)
	if (!PreviewComponent->GetDeformer())
	{
		PreviewComponent->ForceInitializeForEditorPreview();
		FlushRenderingCommands();

		// Error if Deformer still doesn't exist after initialization
		if (!PreviewComponent->GetDeformer())
		{
			return FReply::Handled();
		}
	}

	// Store current preview mesh (for later restoration)
	USkeletalMeshComponent* SkelMeshComp = PreviewComponent->GetResolvedTargetSkeletalMeshComponent();
	if (SkelMeshComp)
	{
		OriginalPreviewMesh = SkelMeshComp->GetSkeletalMeshAsset();
	}

	// Start async bake (overlay + FTSTicker)
	bAsyncBakeInProgress = true;
	AsyncBakeFrameCount = 0;
	PostCacheValidFrameCount = 0;
	AsyncBakeAsset = Asset;
	AsyncBakeComponent = PreviewComponent;

	// Show overlay (block input)
	FleshRingEditor->ShowBakeOverlay(true, LOCTEXT("BakingMeshOverlay", "Baking mesh...\nPlease wait."));

	// Start mesh swap (invalidate cache)
	FlushRenderingCommands();
	bool bImmediateSuccess = Asset->GenerateBakedMesh(PreviewComponent);

	if (bImmediateSuccess)
	{
		// Immediate success (cache already existed)
		FleshRingEditor->ShowBakeOverlay(false);
		bAsyncBakeInProgress = false;
		RestoreOriginalPreviewMesh(PreviewComponent);

		// Prevent memory leak: Call GC even in immediate success path
		FlushRenderingCommands();
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

		// Auto-save (includes Perforce checkout prompt)
		SaveAsset(Asset);

		return FReply::Handled();
	}

	// Start async ticker (continue rendering while waiting for GPU work to complete)
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateSP(this, &FSubdivisionSettingsCustomization::OnAsyncBakeTick),
		0.016f  // ~60fps
	);

	return FReply::Handled();
}

FReply FSubdivisionSettingsCustomization::OnClearBakedMeshClicked()
{
	UFleshRingAsset* Asset = GetOuterAsset();
	if (Asset)
	{
		// Close existing BakedMesh if it's open in another editor (crash prevention)
		if (Asset->HasBakedMesh())
		{
			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (AssetEditorSubsystem)
			{
				USkeletalMesh* ExistingBakedMesh = Asset->SubdivisionSettings.BakedMesh;
				if (ExistingBakedMesh)
				{
					AssetEditorSubsystem->CloseAllEditorsForAsset(ExistingBakedMesh);
				}
			}
		}

		Asset->ClearBakedMesh();
		// Prevent memory leak: Execute GC immediately on button click
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

		// Auto-save (includes Perforce checkout prompt)
		SaveAsset(Asset);
	}
	return FReply::Handled();
}

bool FSubdivisionSettingsCustomization::OnAsyncBakeTick(float DeltaTime)
{
	// Validity check
	if (!AsyncBakeAsset.IsValid() || !AsyncBakeComponent.IsValid())
	{
		CleanupAsyncBake(true);
		return false;  // Stop ticker
	}

	++AsyncBakeFrameCount;

	// Check Deformer cache status
	UFleshRingDeformer* Deformer = AsyncBakeComponent->GetDeformer();
	if (Deformer)
	{
		UFleshRingDeformerInstance* Instance = Deformer->GetActiveInstance();
		if (Instance && Instance->HasCachedDeformedGeometry(0))
		{
			// Cache is now valid - wait additional frames (ensure GPU computation is complete)
			++PostCacheValidFrameCount;

			if (PostCacheValidFrameCount < PostCacheValidWaitFrames)
			{
				return true;  // Continue waiting
			}

			// Waited long enough, flush GPU commands and attempt baking
			FlushRenderingCommands();

			bool bSuccess = AsyncBakeAsset->GenerateBakedMesh(AsyncBakeComponent.Get());

			if (bSuccess)
			{
				CleanupAsyncBake(true);
				return false;  // Stop ticker
			}
			else
			{
				// Cache is valid but failed? Something is wrong - retry
			}
		}
	}

	// Check if maximum frames exceeded
	if (AsyncBakeFrameCount >= MaxAsyncBakeFrames)
	{
		CleanupAsyncBake(true);
		return false;  // Stop ticker
	}

	// Continue waiting
	return true;  // Continue ticker
}

void FSubdivisionSettingsCustomization::CleanupAsyncBake(bool bRestorePreviewMesh)
{
	// Remove ticker
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	// NOTE: Overlay is hidden AFTER SaveAsset completes (keep input blocked until save finishes)

	// Restore original preview mesh
	if (bRestorePreviewMesh && AsyncBakeComponent.IsValid() && OriginalPreviewMesh.IsValid())
	{
		USkeletalMeshComponent* SkelMeshComp = AsyncBakeComponent->GetResolvedTargetSkeletalMeshComponent();
		if (SkelMeshComp && SkelMeshComp->GetSkeletalMeshAsset() != OriginalPreviewMesh.Get())
		{
			// Release existing buffers
			if (UFleshRingDeformer* Deformer = AsyncBakeComponent->GetDeformer())
			{
				if (UFleshRingDeformerInstance* Instance = Deformer->GetActiveInstance())
				{
					Instance->ReleaseResources();
				}
			}
			FlushRenderingCommands();

			// Restore to original mesh
			SkelMeshComp->SetSkeletalMeshAsset(OriginalPreviewMesh.Get());
			SkelMeshComp->MarkRenderStateDirty();
			SkelMeshComp->MarkRenderDynamicDataDirty();
			FlushRenderingCommands();
		}
	}

	// Clean up SubdividedMesh after restoring original mesh (safe timing)
	if (AsyncBakeAsset.IsValid() && AsyncBakeAsset->SubdivisionSettings.SubdividedMesh)
	{
		USkeletalMesh* SubdividedMesh = AsyncBakeAsset->SubdivisionSettings.SubdividedMesh;

		// Switch to another mesh first if preview component is still using SubdividedMesh
		if (AsyncBakeComponent.IsValid())
		{
			USkeletalMeshComponent* SkelMeshComp = AsyncBakeComponent->GetResolvedTargetSkeletalMeshComponent();
			if (SkelMeshComp && SkelMeshComp->GetSkeletalMeshAsset() == SubdividedMesh)
			{
				// Release Deformer buffers
				if (UFleshRingDeformer* Deformer = AsyncBakeComponent->GetDeformer())
				{
					if (UFleshRingDeformerInstance* Instance = Deformer->GetActiveInstance())
					{
						Instance->ReleaseResources();
					}
				}
				FlushRenderingCommands();

				// Switch to original mesh or TargetSkeletalMesh
				USkeletalMesh* FallbackMesh = OriginalPreviewMesh.IsValid()
					? OriginalPreviewMesh.Get()
					: AsyncBakeAsset->TargetSkeletalMesh.Get();
				if (FallbackMesh)
				{
					SkelMeshComp->SetSkeletalMeshAsset(FallbackMesh);
					SkelMeshComp->MarkRenderStateDirty();
					SkelMeshComp->MarkRenderDynamicDataDirty();
					FlushRenderingCommands();
				}
			}
		}

		// Release pointer (break UPROPERTY reference)
		AsyncBakeAsset->SubdivisionSettings.SubdividedMesh = nullptr;

		// Fully release render resources
		SubdividedMesh->ReleaseResources();
		SubdividedMesh->ReleaseResourcesFence.Wait();
		FlushRenderingCommands();

		// Change Outer to TransientPackage (detach from Asset subobject)
		SubdividedMesh->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);

		// Remove RF_Transactional flag - prevent Undo/Redo system from referencing
		SubdividedMesh->ClearFlags(RF_Public | RF_Standalone | RF_Transactional);
		SubdividedMesh->SetFlags(RF_Transient);

		// Mark for garbage collection
		SubdividedMesh->MarkAsGarbage();
	}

	// Prevent memory leak: Execute GC after restoring original mesh
	// At this point all references to SubdividedMesh/BakedMesh are released
	// Synchronous GC cost is acceptable since user is waiting with overlay during bake
	FlushRenderingCommands();  // Wait for render thread completion (needed even if condition not entered)
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	// Auto-save (includes Perforce checkout prompt)
	SaveAsset(AsyncBakeAsset.Get());

	// Hide overlay AFTER save completes (prevent user interaction during cleanup/save)
	if (AsyncBakeAsset.IsValid() && GEditor)
	{
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (AssetEditorSubsystem)
		{
			TArray<IAssetEditorInstance*> Editors = AssetEditorSubsystem->FindEditorsForAsset(AsyncBakeAsset.Get());
			for (IAssetEditorInstance* Editor : Editors)
			{
				// Verify editor type before casting (static_cast doesn't return nullptr for wrong types)
				if (Editor && Editor->GetEditorName() == FName("FleshRingAssetEditor"))
				{
					FFleshRingAssetEditor* FleshRingEditor = static_cast<FFleshRingAssetEditor*>(Editor);
					FleshRingEditor->ShowBakeOverlay(false);
					break;
				}
			}
		}
	}

	// Reset state
	bAsyncBakeInProgress = false;
	AsyncBakeFrameCount = 0;
	PostCacheValidFrameCount = 0;
	AsyncBakeAsset.Reset();
	AsyncBakeComponent.Reset();
	OriginalPreviewMesh.Reset();
}

void FSubdivisionSettingsCustomization::RestoreOriginalPreviewMesh(UFleshRingComponent* PreviewComponent)
{
	if (!PreviewComponent || !OriginalPreviewMesh.IsValid())
	{
		return;
	}

	USkeletalMeshComponent* SkelMeshComp = PreviewComponent->GetResolvedTargetSkeletalMeshComponent();
	if (SkelMeshComp && SkelMeshComp->GetSkeletalMeshAsset() != OriginalPreviewMesh.Get())
	{
		// Release existing buffers
		if (UFleshRingDeformer* Deformer = PreviewComponent->GetDeformer())
		{
			if (UFleshRingDeformerInstance* Instance = Deformer->GetActiveInstance())
			{
				Instance->ReleaseResources();
			}
		}
		FlushRenderingCommands();

		// Restore to original mesh
		SkelMeshComp->SetSkeletalMeshAsset(OriginalPreviewMesh.Get());
		SkelMeshComp->MarkRenderStateDirty();
		SkelMeshComp->MarkRenderDynamicDataDirty();
		FlushRenderingCommands();
	}

	OriginalPreviewMesh.Reset();
}

#undef LOCTEXT_NAMESPACE
