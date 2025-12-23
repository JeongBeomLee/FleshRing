// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingSettingsCustomization.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "FleshRingComponent.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

#define LOCTEXT_NAMESPACE "FleshRingSettingsCustomization"

TSharedRef<IPropertyTypeCustomization> FFleshRingSettingsCustomization::MakeInstance()
{
	return MakeShareable(new FFleshRingSettingsCustomization);
}

void FFleshRingSettingsCustomization::CustomizeHeader(
	TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// BoneName 핸들 미리 가져오기 (헤더 미리보기용)
	BoneNameHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BoneName));

	// 헤더: 이름 + 본 이름 미리보기
	HeaderRow.NameContent()
	[
		PropertyHandle->CreatePropertyNameWidget()
	]
	.ValueContent()
	[
		SNew(STextBlock)
		.Text(this, &FFleshRingSettingsCustomization::GetCurrentBoneName)
		.Font(IDetailLayoutBuilder::GetDetailFont())
	];
}

void FFleshRingSettingsCustomization::CustomizeChildren(
	TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// BoneName 핸들은 CustomizeHeader에서 이미 설정됨
	// Bone 목록 업데이트
	UpdateBoneNameList();

	// BoneName을 드롭다운으로 커스터마이징
	if (BoneNameHandle.IsValid())
	{
		ChildBuilder.AddCustomRow(LOCTEXT("BoneNameRow", "Bone Name"))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("BoneNameLabel", "Bone Name"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.ValueContent()
		.MinDesiredWidth(200.0f)
		[
			SNew(SComboBox<TSharedPtr<FName>>)
			.OptionsSource(&BoneNameList)
			.OnSelectionChanged(this, &FFleshRingSettingsCustomization::OnBoneNameSelected)
			.OnGenerateWidget_Lambda([](TSharedPtr<FName> InItem)
			{
				return SNew(STextBlock)
					.Text(FText::FromName(*InItem))
					.Font(IDetailLayoutBuilder::GetDetailFont());
			})
			.Content()
			[
				SNew(STextBlock)
				.Text(this, &FFleshRingSettingsCustomization::GetCurrentBoneName)
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		];
	}

	// 나머지 프로퍼티들은 기본 표시
	uint32 NumChildren;
	PropertyHandle->GetNumChildren(NumChildren);

	for (uint32 ChildIndex = 0; ChildIndex < NumChildren; ++ChildIndex)
	{
		TSharedRef<IPropertyHandle> ChildHandle = PropertyHandle->GetChildHandle(ChildIndex).ToSharedRef();
		FName PropertyName = ChildHandle->GetProperty()->GetFName();

		// BoneName은 이미 커스터마이징했으므로 스킵
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BoneName))
		{
			continue;
		}

		// 나머지는 기본 위젯 사용
		ChildBuilder.AddProperty(ChildHandle);
	}
}

void FFleshRingSettingsCustomization::UpdateBoneNameList()
{
	BoneNameList.Empty();

	// 기본값 추가
	BoneNameList.Add(MakeShareable(new FName(NAME_None)));

	// TODO: 실제 SkeletalMesh에서 Bone 목록 가져오기
	// 현재는 더미 데이터
	BoneNameList.Add(MakeShareable(new FName(TEXT("root"))));
	BoneNameList.Add(MakeShareable(new FName(TEXT("pelvis"))));
	BoneNameList.Add(MakeShareable(new FName(TEXT("spine_01"))));
	BoneNameList.Add(MakeShareable(new FName(TEXT("spine_02"))));
	BoneNameList.Add(MakeShareable(new FName(TEXT("spine_03"))));
	BoneNameList.Add(MakeShareable(new FName(TEXT("thigh_l"))));
	BoneNameList.Add(MakeShareable(new FName(TEXT("thigh_r"))));
	BoneNameList.Add(MakeShareable(new FName(TEXT("calf_l"))));
	BoneNameList.Add(MakeShareable(new FName(TEXT("calf_r"))));
	BoneNameList.Add(MakeShareable(new FName(TEXT("upperarm_l"))));
	BoneNameList.Add(MakeShareable(new FName(TEXT("upperarm_r"))));
}

void FFleshRingSettingsCustomization::OnBoneNameSelected(TSharedPtr<FName> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (BoneNameHandle.IsValid() && NewSelection.IsValid())
	{
		BoneNameHandle->SetValue(*NewSelection);
	}
}

FText FFleshRingSettingsCustomization::GetCurrentBoneName() const
{
	if (BoneNameHandle.IsValid())
	{
		FName CurrentValue;
		BoneNameHandle->GetValue(CurrentValue);

		if (CurrentValue == NAME_None)
		{
			return LOCTEXT("SelectBone", "Select Bone...");
		}
		return FText::FromName(CurrentValue);
	}
	return LOCTEXT("InvalidBone", "Invalid");
}

#undef LOCTEXT_NAMESPACE
