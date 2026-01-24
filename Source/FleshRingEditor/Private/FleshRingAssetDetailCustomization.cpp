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
	// 편집 중인 Asset 캐싱
	TArray<TWeakObjectPtr<UObject>> Objects;
	DetailBuilder.GetObjectsBeingCustomized(Objects);
	if (Objects.Num() > 0)
	{
		CachedAsset = Cast<UFleshRingAsset>(Objects[0].Get());
	}

	// =====================================
	// 카테고리 순서 정의 (Important 우선순위, 호출 순서대로 배치)
	// =====================================

	// 1. Target - 맨 위 (타겟 메시 설정)
	DetailBuilder.EditCategory(
		TEXT("Target"),
		LOCTEXT("TargetCategory", "Target"),
		ECategoryPriority::Important
	);

	// 2. Subdivision Settings - FSubdivisionSettings 구조체로 대체됨
	// FSubdivisionSettingsCustomization에서 서브그룹 처리
	DetailBuilder.EditCategory(
		TEXT("Subdivision Settings"),
		LOCTEXT("SubdivisionSettingsCategory", "Subdivision Settings"),
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
		// 에디터 찾아서 PreviewScene의 메시 강제 재생성
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
		// UAssetEditorSubsystem을 통해 열린 에디터에서 PreviewComponent 가져오기
		UFleshRingComponent* PreviewComponent = nullptr;

		if (GEditor)
		{
			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (AssetEditorSubsystem)
			{
				TArray<IAssetEditorInstance*> Editors = AssetEditorSubsystem->FindEditorsForAsset(CachedAsset.Get());
				for (IAssetEditorInstance* Editor : Editors)
				{
					// FFleshRingAssetEditor는 FAssetEditorToolkit을 상속
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
