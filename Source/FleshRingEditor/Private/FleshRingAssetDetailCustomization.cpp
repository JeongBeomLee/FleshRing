// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingAssetDetailCustomization.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailGroup.h"
#include "FleshRingAsset.h"
#include "FleshRingComponent.h"
#include "FleshRingAssetEditor.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateTypes.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "FleshRingAssetDetailCustomization"

TSharedRef<IDetailCustomization> FFleshRingAssetDetailCustomization::MakeInstance()
{
	return MakeShareable(new FFleshRingAssetDetailCustomization);
}

void FFleshRingAssetDetailCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	// Cache Asset being edited
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	if (Objects.Num() > 0)
	{
		CachedAsset = Cast<UFleshRingAsset>(Objects[0].Get());
	}

	// =====================================
	// Category order definition (arranged by call order)
	// =====================================

	// 1. Target
	DetailBuilder.EditCategory(
		TEXT("Target"),
		LOCTEXT("TargetCategory", "Target"),
		ECategoryPriority::Important
	);

	// 2. Skeletal Mesh Detail Settings
	DetailBuilder.EditCategory(
		TEXT("Skeletal Mesh Detail Settings"),
		LOCTEXT("SkeletalMeshDetailSettingsCategory", "Skeletal Mesh Detail Settings"),
		ECategoryPriority::Important
	);

	// 3. Ring Settings
	DetailBuilder.EditCategory(
		TEXT("Ring Settings"),
		LOCTEXT("RingSettingsCategory", "Ring Settings"),
		ECategoryPriority::Important
	);

	// 4. Material Layer Settings
	DetailBuilder.EditCategory(
		TEXT("Material Layer Settings"),
		LOCTEXT("MaterialLayerSettingsCategory", "Material Layer Settings"),
		ECategoryPriority::Important
	);

	// 5. Normals - Explicit English name to prevent UE localization
	DetailBuilder.EditCategory(
		TEXT("Normals"),
		LOCTEXT("NormalsCategory", "Normals"),
		ECategoryPriority::Important
	);
}

bool FFleshRingAssetDetailCustomization::IsSubdivisionEnabled() const
{
	return CachedAsset.IsValid() && CachedAsset->SubdivisionSettings.bEnableSubdivision;
}

FReply FFleshRingAssetDetailCustomization::OnRefreshPreviewClicked()
{
	if (CachedAsset.IsValid() && GEditor)
	{
		// Find editor and force regenerate mesh in PreviewScene
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (AssetEditorSubsystem)
		{
			TArray<IAssetEditorInstance*> Editors = AssetEditorSubsystem->FindEditorsForAsset(CachedAsset.Get());
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

FReply FFleshRingAssetDetailCustomization::OnGenerateRuntimeMeshClicked()
{
	if (CachedAsset.IsValid())
	{
		// Get PreviewComponent from editor opened via UAssetEditorSubsystem
		UFleshRingComponent* PreviewComponent = nullptr;

		if (GEditor)
		{
			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (AssetEditorSubsystem)
			{
				TArray<IAssetEditorInstance*> Editors = AssetEditorSubsystem->FindEditorsForAsset(CachedAsset.Get());
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

		CachedAsset->GenerateSubdividedMesh(PreviewComponent);
	}
	return FReply::Handled();
}

FReply FFleshRingAssetDetailCustomization::OnClearRuntimeMeshClicked()
{
	if (CachedAsset.IsValid())
	{
		CachedAsset->ClearSubdividedMesh();
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
