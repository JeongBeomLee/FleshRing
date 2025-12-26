// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingSettingsCustomization.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SBoxPanel.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "Styling/AppStyle.h"

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
	// 메인 프로퍼티 핸들 캐싱 (Asset 접근용)
	MainPropertyHandle = PropertyHandle;

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
			.OnComboBoxOpening(this, &FFleshRingSettingsCustomization::OnComboBoxOpening)
			.OnSelectionChanged(this, &FFleshRingSettingsCustomization::OnBoneNameSelected)
			.OnGenerateWidget_Lambda([](TSharedPtr<FName> InItem)
			{
				return SNew(STextBlock)
					.Text(FText::FromName(*InItem))
					.Font(IDetailLayoutBuilder::GetDetailFont());
			})
			.Content()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0, 0, 4, 0)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("Icons.Warning"))
					.Visibility_Lambda([this]()
					{
						return IsBoneInvalid() ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.ColorAndOpacity(FLinearColor::Yellow)
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &FFleshRingSettingsCustomization::GetCurrentBoneName)
					.Font(IDetailLayoutBuilder::GetDetailFont())
				]
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

USkeletalMesh* FFleshRingSettingsCustomization::GetTargetSkeletalMesh() const
{
	if (!MainPropertyHandle.IsValid())
	{
		return nullptr;
	}

	// PropertyHandle 체인을 따라 올라가서 UFleshRingAsset 찾기
	// FFleshRingSettings -> Rings 배열 -> UFleshRingAsset
	TArray<UObject*> OuterObjects;
	MainPropertyHandle->GetOuterObjects(OuterObjects);

	for (UObject* Obj : OuterObjects)
	{
		if (UFleshRingAsset* Asset = Cast<UFleshRingAsset>(Obj))
		{
			return Asset->TargetSkeletalMesh.LoadSynchronous();
		}
	}

	return nullptr;
}

void FFleshRingSettingsCustomization::UpdateBoneNameList()
{
	BoneNameList.Empty();

	// 기본값 추가 (선택 안함)
	BoneNameList.Add(MakeShareable(new FName(NAME_None)));

	// Asset의 TargetSkeletalMesh에서 본 목록 가져오기
	USkeletalMesh* SkeletalMesh = GetTargetSkeletalMesh();
	if (SkeletalMesh)
	{
		const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
		const int32 NumBones = RefSkeleton.GetNum();

		for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
		{
			FName BoneName = RefSkeleton.GetBoneName(BoneIdx);
			BoneNameList.Add(MakeShareable(new FName(BoneName)));
		}
	}
	else
	{
		// TargetSkeletalMesh가 설정되지 않은 경우 안내 메시지
		// (드롭다운에 None만 표시됨)
	}
}

void FFleshRingSettingsCustomization::OnComboBoxOpening()
{
	// 드롭다운 열릴 때마다 본 목록 갱신
	// TargetSkeletalMesh가 변경되었을 경우 최신 목록 반영
	UpdateBoneNameList();
}

void FFleshRingSettingsCustomization::OnBoneNameSelected(TSharedPtr<FName> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (BoneNameHandle.IsValid() && NewSelection.IsValid())
	{
		BoneNameHandle->SetValue(*NewSelection);
	}
}

bool FFleshRingSettingsCustomization::IsBoneInvalid() const
{
	if (!BoneNameHandle.IsValid())
	{
		return false;
	}

	FName CurrentValue;
	BoneNameHandle->GetValue(CurrentValue);

	// None은 경고 아님 (아직 선택 안 한 상태)
	if (CurrentValue == NAME_None)
	{
		return false;
	}

	// SkeletalMesh에서 본 찾기
	USkeletalMesh* SkeletalMesh = const_cast<FFleshRingSettingsCustomization*>(this)->GetTargetSkeletalMesh();
	if (!SkeletalMesh)
	{
		// SkeletalMesh 없으면 경고
		return true;
	}

	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
	int32 BoneIndex = RefSkeleton.FindBoneIndex(CurrentValue);

	return BoneIndex == INDEX_NONE;
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

		// 현재 선택된 본이 SkeletalMesh에 있는지 확인
		USkeletalMesh* SkeletalMesh = const_cast<FFleshRingSettingsCustomization*>(this)->GetTargetSkeletalMesh();
		if (SkeletalMesh)
		{
			const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
			int32 BoneIndex = RefSkeleton.FindBoneIndex(CurrentValue);

			if (BoneIndex == INDEX_NONE)
			{
				// 본이 없으면 경고 표시
				return FText::Format(
					LOCTEXT("BoneNotFound", "{0} (Not Found)"),
					FText::FromName(CurrentValue));
			}
		}
		else
		{
			// SkeletalMesh가 설정되지 않은 경우
			return FText::Format(
				LOCTEXT("NoSkeletalMesh", "{0} (No Mesh)"),
				FText::FromName(CurrentValue));
		}

		return FText::FromName(CurrentValue);
	}
	return LOCTEXT("InvalidBone", "Invalid");
}

#undef LOCTEXT_NAMESPACE
