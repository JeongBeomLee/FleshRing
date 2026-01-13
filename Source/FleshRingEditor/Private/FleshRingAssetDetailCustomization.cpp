// Copyright Epic Games, Inc. All Rights Reserved.

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

	// Subdivision Settings 카테고리 맨 위로 보내기
	IDetailCategoryBuilder& SubdivisionCategory = DetailBuilder.EditCategory(
		TEXT("Subdivision Settings"),
		LOCTEXT("SubdivisionSettingsCategory", "Subdivision Settings"),
		ECategoryPriority::Important
	);

	// 기존 자동 생성된 프로퍼티들 숨기기
	DetailBuilder.HideProperty(GET_MEMBER_NAME_CHECKED(UFleshRingAsset, bEnableSubdivision));
	DetailBuilder.HideProperty(GET_MEMBER_NAME_CHECKED(UFleshRingAsset, MaxSubdivisionLevel));
	DetailBuilder.HideProperty(GET_MEMBER_NAME_CHECKED(UFleshRingAsset, MinEdgeLength));
	DetailBuilder.HideProperty(GET_MEMBER_NAME_CHECKED(UFleshRingAsset, InfluenceRadiusMultiplier));
	DetailBuilder.HideProperty(GET_MEMBER_NAME_CHECKED(UFleshRingAsset, SubdividedMesh));
	DetailBuilder.HideProperty(GET_MEMBER_NAME_CHECKED(UFleshRingAsset, PreviewSubdivisionLevel));

	// Enable Subdivision (맨 위)
	SubdivisionCategory.AddProperty(
		DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UFleshRingAsset, bEnableSubdivision))
	);

	// Min Edge Length (공통 설정 - Preview/Runtime 둘 다 사용)
	SubdivisionCategory.AddProperty(
		DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UFleshRingAsset, MinEdgeLength))
	);

	// Editor Preview 그룹
	IDetailGroup& PreviewGroup = SubdivisionCategory.AddGroup(
		TEXT("EditorPreview"),
		LOCTEXT("EditorPreviewGroup", "Editor Preview")
	);

	PreviewGroup.AddPropertyRow(
		DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UFleshRingAsset, PreviewSubdivisionLevel))
	);

	// Refresh Preview 버튼 (가운데 정렬, 최소 너비 지정)
	PreviewGroup.AddWidgetRow()
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
				.OnClicked(this, &FFleshRingAssetDetailCustomization::OnRefreshPreviewClicked)
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
	// Runtime Settings 그룹
	// =====================================
	IDetailGroup& RuntimeGroup = SubdivisionCategory.AddGroup(
		TEXT("RuntimeSettings"),
		LOCTEXT("RuntimeSettingsGroup", "Runtime Settings")
	);

	RuntimeGroup.AddPropertyRow(
		DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UFleshRingAsset, MaxSubdivisionLevel))
	);

	RuntimeGroup.AddPropertyRow(
		DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UFleshRingAsset, InfluenceRadiusMultiplier))
	);

	// Generate + Clear 버튼 (가운데 정렬, 최소 너비 지정)
	RuntimeGroup.AddWidgetRow()
	.WholeRowContent()
	[
		SNew(SBox)
		.Padding(FMargin(0, 2, 0, 2))
		.HAlign(HAlign_Center)
		[
			SNew(SHorizontalBox)
			// Generate 버튼 (녹색)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0, 0, 4, 0)
			[
				SNew(SBox)
				.MinDesiredWidth(180.0f)
				[
					SNew(SButton)
					.IsEnabled(this, &FFleshRingAssetDetailCustomization::IsSubdivisionEnabled)
					.OnClicked(this, &FFleshRingAssetDetailCustomization::OnGenerateRuntimeMeshClicked)
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
								.Text(LOCTEXT("GenerateMesh", "Generate"))
							]
						]
					]
				]
			]
			// Clear 버튼 (빨간색)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4, 0, 0, 0)
			[
				SNew(SBox)
				.MinDesiredWidth(180.0f)
				[
					SNew(SButton)
					.IsEnabled(this, &FFleshRingAssetDetailCustomization::IsSubdivisionEnabled)
					.OnClicked(this, &FFleshRingAssetDetailCustomization::OnClearRuntimeMeshClicked)
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

	// Subdivided Mesh 프로퍼티 (읽기 전용)
	RuntimeGroup.AddPropertyRow(
		DetailBuilder.GetProperty(GET_MEMBER_NAME_CHECKED(UFleshRingAsset, SubdividedMesh))
	);
}

bool FFleshRingAssetDetailCustomization::IsSubdivisionEnabled() const
{
	return CachedAsset.IsValid() && CachedAsset->bEnableSubdivision;
}

FReply FFleshRingAssetDetailCustomization::OnRefreshPreviewClicked()
{
	if (CachedAsset.IsValid())
	{
		CachedAsset->GeneratePreviewMesh();
		CachedAsset->OnAssetChanged.Broadcast(CachedAsset.Get());
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
