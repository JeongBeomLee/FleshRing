// Copyright Epic Games, Inc. All Rights Reserved.

#include "FSubdivisionSettingsCustomization.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "IDetailGroup.h"
#include "FleshRingAsset.h"
#include "FleshRingComponent.h"
#include "FleshRingTypes.h"
#include "FleshRingAssetEditor.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"

#define LOCTEXT_NAMESPACE "SubdivisionSettingsCustomization"

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

	// 헤더 숨기기 - 카테고리 이름만 표시되도록
	// (구조체 이름 "Subdivision Settings" 중복 방지)
}

void FSubdivisionSettingsCustomization::CustomizeChildren(
	TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// 자식 프로퍼티 핸들 가져오기
	TSharedPtr<IPropertyHandle> bEnableSubdivisionHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, bEnableSubdivision));
	TSharedPtr<IPropertyHandle> MinEdgeLengthHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, MinEdgeLength));
	TSharedPtr<IPropertyHandle> PreviewSubdivisionLevelHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, PreviewSubdivisionLevel));
	TSharedPtr<IPropertyHandle> PreviewBoneHopCountHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, PreviewBoneHopCount));
	TSharedPtr<IPropertyHandle> PreviewBoneWeightThresholdHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, PreviewBoneWeightThreshold));
	TSharedPtr<IPropertyHandle> MaxSubdivisionLevelHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, MaxSubdivisionLevel));
	TSharedPtr<IPropertyHandle> SubdividedMeshHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, SubdividedMesh));

	// =====================================
	// 공통 설정 (최상위)
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
	// Editor Preview 서브그룹
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

	// Refresh Preview 버튼
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
	// Runtime Settings 서브그룹
	// =====================================
	IDetailGroup& RuntimeGroup = ChildBuilder.AddGroup(
		TEXT("RuntimeSettings"),
		LOCTEXT("RuntimeSettingsGroup", "Runtime Settings")
	);

	if (MaxSubdivisionLevelHandle.IsValid())
	{
		RuntimeGroup.AddPropertyRow(MaxSubdivisionLevelHandle.ToSharedRef());
	}

	// Generate + Clear 버튼
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
					.IsEnabled(this, &FSubdivisionSettingsCustomization::IsSubdivisionEnabled)
					.OnClicked(this, &FSubdivisionSettingsCustomization::OnGenerateRuntimeMeshClicked)
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
					.IsEnabled(this, &FSubdivisionSettingsCustomization::IsSubdivisionEnabled)
					.OnClicked(this, &FSubdivisionSettingsCustomization::OnClearRuntimeMeshClicked)
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
	if (SubdividedMeshHandle.IsValid())
	{
		RuntimeGroup.AddPropertyRow(SubdividedMeshHandle.ToSharedRef());
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

FReply FSubdivisionSettingsCustomization::OnRefreshPreviewClicked()
{
	UFleshRingAsset* Asset = GetOuterAsset();
	if (Asset)
	{
		Asset->GeneratePreviewMesh();
		Asset->OnAssetChanged.Broadcast(Asset);
	}
	return FReply::Handled();
}

FReply FSubdivisionSettingsCustomization::OnGenerateRuntimeMeshClicked()
{
	UFleshRingAsset* Asset = GetOuterAsset();
	if (Asset)
	{
		// UAssetEditorSubsystem을 통해 열린 에디터에서 PreviewComponent 가져오기
		UFleshRingComponent* PreviewComponent = nullptr;

		if (GEditor)
		{
			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (AssetEditorSubsystem)
			{
				TArray<IAssetEditorInstance*> Editors = AssetEditorSubsystem->FindEditorsForAsset(Asset);
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

		Asset->GenerateSubdividedMesh(PreviewComponent);
	}
	return FReply::Handled();
}

FReply FSubdivisionSettingsCustomization::OnClearRuntimeMeshClicked()
{
	UFleshRingAsset* Asset = GetOuterAsset();
	if (Asset)
	{
		Asset->ClearSubdividedMesh();
	}
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
