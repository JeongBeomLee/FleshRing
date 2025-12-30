// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingSettingsCustomization.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "IDetailGroup.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "FleshRingTypes.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Views/SListView.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "Styling/AppStyle.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Misc/DefaultValueHelper.h"

#define LOCTEXT_NAMESPACE "FleshRingSettingsCustomization"

// 각도 표시용 TypeInterface (숫자 옆에 ° 표시)
class FDegreeTypeInterface : public TDefaultNumericTypeInterface<double>
{
public:
	virtual FString ToString(const double& Value) const override
	{
		return FString::Printf(TEXT("%.2f\u00B0"), Value);
	}

	virtual TOptional<double> FromString(const FString& InString, const double& ExistingValue) override
	{
		FString CleanString = InString.Replace(TEXT("\u00B0"), TEXT("")).TrimStartAndEnd();
		double Result = 0.0;
		if (LexTryParseString(Result, *CleanString))
		{
			return Result;
		}
		return TOptional<double>();
	}
};

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

	// BoneName을 검색 가능한 드롭다운으로 커스터마이징
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
			CreateSearchableBoneDropdown()
		];
	}

	// Rotation 핸들 캐싱
	RingRotationHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingRotation));
	MeshRotationHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshRotation));

	// InfluenceMode 핸들 가져오기
	TSharedPtr<IPropertyHandle> InfluenceModeHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, InfluenceMode));

	// 현재 InfluenceMode가 Manual인지 확인 (초기 상태용)
	bool bIsManualMode = false;
	if (InfluenceModeHandle.IsValid())
	{
		uint8 ModeValue = 0;
		InfluenceModeHandle->GetValue(ModeValue);
		bIsManualMode = (static_cast<EFleshRingInfluenceMode>(ModeValue) == EFleshRingInfluenceMode::Manual);
	}

	// Manual 모드 동적 체크용 TAttribute (Ring Transform에 사용)
	TAttribute<bool> IsManualModeAttr = TAttribute<bool>::Create([InfluenceModeHandle]() -> bool
	{
		if (!InfluenceModeHandle.IsValid())
		{
			return true;
		}
		uint8 ModeValue = 0;
		InfluenceModeHandle->GetValue(ModeValue);
		return static_cast<EFleshRingInfluenceMode>(ModeValue) == EFleshRingInfluenceMode::Manual;
	});

	// SDF 모드 동적 체크용 TAttribute (SDF Settings에 사용 - Manual이 아닐 때 활성화)
	TAttribute<bool> IsSdfModeAttr = TAttribute<bool>::Create([InfluenceModeHandle]() -> bool
	{
		if (!InfluenceModeHandle.IsValid())
		{
			return true;
		}
		uint8 ModeValue = 0;
		InfluenceModeHandle->GetValue(ModeValue);
		return static_cast<EFleshRingInfluenceMode>(ModeValue) != EFleshRingInfluenceMode::Manual;
	});

	// Ring Transform 그룹에 넣을 프로퍼티들 수집
	TSharedPtr<IPropertyHandle> RingRadiusHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingRadius));
	TSharedPtr<IPropertyHandle> RingThicknessHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingThickness));
	TSharedPtr<IPropertyHandle> RingWidthHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingWidth));
	TSharedPtr<IPropertyHandle> RingOffsetHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingOffset));
	TSharedPtr<IPropertyHandle> RingEulerHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingEulerRotation));

	// Ring Transform 그룹에 들어갈 프로퍼티 이름들
	TSet<FName> RingGroupProperties;
	RingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingRadius));
	RingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingThickness));
	RingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingWidth));
	RingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingOffset));
	RingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingEulerRotation));

	// 나머지 프로퍼티들 먼저 표시 (Ring 그룹 제외)
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

		// FQuat은 UI에서 숨김 (EulerRotation만 표시)
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingRotation) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshRotation))
		{
			continue;
		}

		// Ring 그룹에 넣을 프로퍼티는 여기서 스킵
		if (RingGroupProperties.Contains(PropertyName))
		{
			continue;
		}

		// Transform 프로퍼티들은 선형 드래그 감도 적용 + 기본 리셋 화살표
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshOffset))
		{
			ChildBuilder.AddProperty(ChildHandle)
				.CustomWidget()
				.NameContent()
				[
					ChildHandle->CreatePropertyNameWidget()
				]
				.ValueContent()
				.MinDesiredWidth(300.0f)
				[
					CreateLinearVectorWidget(ChildHandle, 1.0f)
				]
				.OverrideResetToDefault(
					FResetToDefaultOverride::Create(
						FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							FVector Value;
							Handle->GetValue(Value);
							return !Value.IsNearlyZero();
						}),
						FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							Handle->SetValue(FVector::ZeroVector);
						})
					)
				);
			continue;
		}
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshEulerRotation))
		{
			ChildBuilder.AddProperty(ChildHandle)
				.CustomWidget()
				.NameContent()
				[
					ChildHandle->CreatePropertyNameWidget()
				]
				.ValueContent()
				.MinDesiredWidth(300.0f)
				[
					CreateLinearRotatorWidget(ChildHandle, 1.0f)
				]
				.OverrideResetToDefault(
					FResetToDefaultOverride::Create(
						FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							FRotator Value;
							Handle->GetValue(Value);
							return !Value.Equals(FRotator(-90.0f, 0.0f, 0.0f), 0.01f);
						}),
						FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							Handle->SetValue(FRotator(-90.0f, 0.0f, 0.0f));
						})
					)
				);
			continue;
		}
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshScale))
		{
			ChildBuilder.AddProperty(ChildHandle)
				.CustomWidget()
				.NameContent()
				[
					ChildHandle->CreatePropertyNameWidget()
				]
				.ValueContent()
				.MinDesiredWidth(300.0f)
				[
					CreateLinearVectorWidget(ChildHandle, 0.0025f)
				]
				.OverrideResetToDefault(
					FResetToDefaultOverride::Create(
						FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							FVector Value;
							Handle->GetValue(Value);
							return !Value.Equals(FVector::OneVector, 0.0001f);
						}),
						FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							Handle->SetValue(FVector::OneVector);
						})
					)
				);
			continue;
		}

		// SDF Settings는 Manual 모드일 때 비활성화 + 자식 프로퍼티 리셋 오버라이드
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SdfSettings))
		{
			// SDF Settings 그룹 생성
			IDetailGroup& SdfGroup = ChildBuilder.AddGroup(TEXT("SdfSettings"), LOCTEXT("SdfSettingsGroup", "SDF Settings"));

			// 자식 프로퍼티들 가져오기
			TSharedPtr<IPropertyHandle> ResolutionHandle = ChildHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSdfSettings, Resolution));
			TSharedPtr<IPropertyHandle> JfaIterationsHandle = ChildHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSdfSettings, JfaIterations));
			TSharedPtr<IPropertyHandle> UpdateModeHandle = ChildHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSdfSettings, UpdateMode));

			// Resolution - 기본값: 64
			if (ResolutionHandle.IsValid())
			{
				SdfGroup.AddPropertyRow(ResolutionHandle.ToSharedRef())
					.IsEnabled(IsSdfModeAttr)
					.OverrideResetToDefault(
						FResetToDefaultOverride::Create(
							FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
								int32 Value;
								Handle->GetValue(Value);
								return Value != 64;
							}),
							FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
								Handle->SetValue(64);
							})
						)
					);
			}

			// JfaIterations - 기본값: 8
			if (JfaIterationsHandle.IsValid())
			{
				SdfGroup.AddPropertyRow(JfaIterationsHandle.ToSharedRef())
					.IsEnabled(IsSdfModeAttr)
					.OverrideResetToDefault(
						FResetToDefaultOverride::Create(
							FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
								int32 Value;
								Handle->GetValue(Value);
								return Value != 8;
							}),
							FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
								Handle->SetValue(8);
							})
						)
					);
			}

			// UpdateMode - 기본값: OnChange
			if (UpdateModeHandle.IsValid())
			{
				SdfGroup.AddPropertyRow(UpdateModeHandle.ToSharedRef())
					.IsEnabled(IsSdfModeAttr)
					.OverrideResetToDefault(
						FResetToDefaultOverride::Create(
							FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
								uint8 Value;
								Handle->GetValue(Value);
								return Value != static_cast<uint8>(EFleshRingSdfUpdateMode::OnChange);
							}),
							FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
								Handle->SetValue(static_cast<uint8>(EFleshRingSdfUpdateMode::OnChange));
							})
						)
					);
			}

			continue;
		}

		// InfluenceMode - 기본값: Auto
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, InfluenceMode))
		{
			ChildBuilder.AddProperty(ChildHandle)
				.OverrideResetToDefault(
					FResetToDefaultOverride::Create(
						FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							uint8 Value;
							Handle->GetValue(Value);
							return Value != static_cast<uint8>(EFleshRingInfluenceMode::Auto);
						}),
						FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							Handle->SetValue(static_cast<uint8>(EFleshRingInfluenceMode::Auto));
						})
					)
				);
			continue;
		}

		// BulgeIntensity - 기본값: 0.5
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeIntensity))
		{
			ChildBuilder.AddProperty(ChildHandle)
				.OverrideResetToDefault(
					FResetToDefaultOverride::Create(
						FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							float Value;
							Handle->GetValue(Value);
							return !FMath::IsNearlyEqual(Value, 0.5f);
						}),
						FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							Handle->SetValue(0.5f);
						})
					)
				);
			continue;
		}

		// TightnessStrength - 기본값: 1.0
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, TightnessStrength))
		{
			ChildBuilder.AddProperty(ChildHandle)
				.OverrideResetToDefault(
					FResetToDefaultOverride::Create(
						FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							float Value;
							Handle->GetValue(Value);
							return !FMath::IsNearlyEqual(Value, 1.0f);
						}),
						FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							Handle->SetValue(1.0f);
						})
					)
				);
			continue;
		}

		// FalloffType - 기본값: Linear
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, FalloffType))
		{
			ChildBuilder.AddProperty(ChildHandle)
				.OverrideResetToDefault(
					FResetToDefaultOverride::Create(
						FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							uint8 Value;
							Handle->GetValue(Value);
							return Value != static_cast<uint8>(EFalloffType::Linear);
						}),
						FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							Handle->SetValue(static_cast<uint8>(EFalloffType::Linear));
						})
					)
				);
			continue;
		}

		// 나머지는 기본 위젯 사용
		ChildBuilder.AddProperty(ChildHandle);
	}

	// Ring Transform 그룹 생성 (Auto 모드일 때 헤더 텍스트도 어둡게)
	IDetailGroup& RingGroup = ChildBuilder.AddGroup(TEXT("RingTransform"), LOCTEXT("RingTransformGroup", "Ring Transform"));
	RingGroup.HeaderRow()
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("RingTransformHeader", "Ring Transform"))
			.Font(IDetailLayoutBuilder::GetDetailFontBold())
			.ColorAndOpacity_Lambda([IsManualModeAttr]() -> FSlateColor
			{
				return IsManualModeAttr.Get() ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground();
			})
		];

	// Ring 그룹에 프로퍼티 추가
	if (RingRadiusHandle.IsValid())
	{
		RingGroup.AddPropertyRow(RingRadiusHandle.ToSharedRef())
			.IsEnabled(IsManualModeAttr)
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 5.0f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(5.0f);
					})
				)
			);
	}
	if (RingThicknessHandle.IsValid())
	{
		RingGroup.AddPropertyRow(RingThicknessHandle.ToSharedRef())
			.IsEnabled(IsManualModeAttr)
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 1.0f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(1.0f);
					})
				)
			);
	}
	if (RingWidthHandle.IsValid())
	{
		RingGroup.AddPropertyRow(RingWidthHandle.ToSharedRef())
			.IsEnabled(IsManualModeAttr)
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 2.0f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(2.0f);
					})
				)
			);
	}
	if (RingOffsetHandle.IsValid())
	{
		RingGroup.AddPropertyRow(RingOffsetHandle.ToSharedRef())
			.IsEnabled(IsManualModeAttr)
			.CustomWidget()
			.NameContent()
			[
				RingOffsetHandle->CreatePropertyNameWidget()
			]
			.ValueContent()
			.MinDesiredWidth(300.0f)
			[
				CreateLinearVectorWidget(RingOffsetHandle.ToSharedRef(), 1.0f)
			]
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						FVector Value;
						Handle->GetValue(Value);
						return !Value.IsNearlyZero();
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(FVector::ZeroVector);
					})
				)
			);
	}
	if (RingEulerHandle.IsValid())
	{
		RingGroup.AddPropertyRow(RingEulerHandle.ToSharedRef())
			.IsEnabled(IsManualModeAttr)
			.CustomWidget()
			.NameContent()
			[
				RingEulerHandle->CreatePropertyNameWidget()
			]
			.ValueContent()
			.MinDesiredWidth(300.0f)
			[
				CreateLinearRotatorWidget(RingEulerHandle.ToSharedRef(), 1.0f)
			]
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						FRotator Value;
						Handle->GetValue(Value);
						return !Value.Equals(FRotator(-90.0f, 0.0f, 0.0f), 0.01f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(FRotator(-90.0f, 0.0f, 0.0f));
					})
				)
			);
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

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateSearchableBoneDropdown()
{
	// 필터링된 목록 초기화
	UpdateFilteredBoneList();

	return SAssignNew(BoneComboButton, SComboButton)
		.OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
		{
			// 드롭다운 열릴 때 본 목록 갱신
			UpdateBoneNameList();
			BoneSearchText.Empty();
			UpdateFilteredBoneList();

			return SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.0f)
				[
					SNew(SSearchBox)
					.HintText(LOCTEXT("SearchBoneHint", "Search Bone..."))
					.OnTextChanged(this, &FFleshRingSettingsCustomization::OnBoneSearchTextChanged)
				]
				+ SVerticalBox::Slot()
				.MaxHeight(300.0f)
				[
					SAssignNew(BoneListView, SListView<TSharedPtr<FName>>)
					.ListItemsSource(&FilteredBoneNameList)
					.OnGenerateRow(this, &FFleshRingSettingsCustomization::GenerateBoneRow)
					.OnSelectionChanged(this, &FFleshRingSettingsCustomization::OnBoneListSelectionChanged)
					.SelectionMode(ESelectionMode::Single)
				];
		})
		.ButtonContent()
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
		];
}

void FFleshRingSettingsCustomization::OnBoneSearchTextChanged(const FText& NewText)
{
	BoneSearchText = NewText.ToString();
	UpdateFilteredBoneList();

	if (BoneListView.IsValid())
	{
		BoneListView->RequestListRefresh();
	}
}

void FFleshRingSettingsCustomization::UpdateFilteredBoneList()
{
	FilteredBoneNameList.Empty();

	for (const TSharedPtr<FName>& BoneName : BoneNameList)
	{
		if (BoneSearchText.IsEmpty() || BoneName->ToString().Contains(BoneSearchText, ESearchCase::IgnoreCase))
		{
			FilteredBoneNameList.Add(BoneName);
		}
	}
}

TSharedRef<ITableRow> FFleshRingSettingsCustomization::GenerateBoneRow(TSharedPtr<FName> InItem, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(STableRow<TSharedPtr<FName>>, OwnerTable)
		.Padding(FMargin(4.0f, 2.0f))
		[
			SNew(STextBlock)
			.Text(FText::FromName(*InItem))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		];
}

void FFleshRingSettingsCustomization::OnBoneListSelectionChanged(TSharedPtr<FName> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (BoneNameHandle.IsValid() && NewSelection.IsValid())
	{
		BoneNameHandle->SetValue(*NewSelection);

		// 드롭다운 닫기
		if (BoneComboButton.IsValid())
		{
			BoneComboButton->SetIsOpen(false);
		}
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

void FFleshRingSettingsCustomization::SyncQuatFromEuler(
	TSharedPtr<IPropertyHandle> EulerHandle,
	TSharedPtr<IPropertyHandle> QuatHandle)
{
	if (!EulerHandle.IsValid() || !QuatHandle.IsValid())
	{
		return;
	}

	// Euler 읽기
	FRotator Euler;
	EulerHandle->EnumerateRawData([&Euler](void* RawData, const int32 DataIndex, const int32 NumDatas)
	{
		if (RawData)
		{
			Euler = *static_cast<FRotator*>(RawData);
			return false;
		}
		return true;
	});

	// Quat에 쓰기
	FQuat Quat = Euler.Quaternion();
	QuatHandle->EnumerateRawData([&Quat](void* RawData, const int32 DataIndex, const int32 NumDatas)
	{
		if (RawData)
		{
			*static_cast<FQuat*>(RawData) = Quat;
		}
		return true;
	});

	// 변경 알림 (프리뷰 갱신 트리거)
	QuatHandle->NotifyPostChange(EPropertyChangeType::ValueSet);
}

FRotator FFleshRingSettingsCustomization::GetQuatAsEuler(TSharedPtr<IPropertyHandle> QuatHandle) const
{
	if (!QuatHandle.IsValid())
	{
		return FRotator::ZeroRotator;
	}

	void* Data = nullptr;
	if (QuatHandle->GetValueData(Data) == FPropertyAccess::Success && Data)
	{
		FQuat Quat = *static_cast<FQuat*>(Data);
		return Quat.Rotator();
	}

	return FRotator::ZeroRotator;
}

void FFleshRingSettingsCustomization::SetEulerToQuat(TSharedPtr<IPropertyHandle> QuatHandle, const FRotator& Euler)
{
	if (!QuatHandle.IsValid())
	{
		return;
	}

	void* Data = nullptr;
	if (QuatHandle->GetValueData(Data) == FPropertyAccess::Success && Data)
	{
		FQuat& Quat = *static_cast<FQuat*>(Data);
		Quat = Euler.Quaternion();
		QuatHandle->NotifyPostChange(EPropertyChangeType::ValueSet);
	}
}

void FFleshRingSettingsCustomization::AddLinearVectorRow(
	IDetailChildrenBuilder& ChildBuilder,
	TSharedRef<IPropertyHandle> VectorHandle,
	const FText& DisplayName,
	float Delta,
	TAttribute<bool> IsEnabled)
{
	TSharedPtr<IPropertyHandle> VecHandlePtr = VectorHandle.ToSharedPtr();

	// EnumerateRawData로 FVector 직접 읽기
	auto GetVector = [VecHandlePtr]() -> FVector
	{
		FVector Result = FVector::ZeroVector;
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FVector*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	// EnumerateRawData로 FVector 직접 쓰기
	auto SetVector = [VecHandlePtr](const FVector& NewValue)
	{
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FVector*>(RawData) = NewValue;
				}
				return true;
			});
			VecHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
		}
	};

	ChildBuilder.AddCustomRow(DisplayName)
	.IsEnabled(IsEnabled)
	.NameContent()
	[
		SNew(STextBlock)
		.Text(DisplayName)
		.Font(IDetailLayoutBuilder::GetDetailFont())
	]
	.ValueContent()
	.MinDesiredWidth(300.0f)
	[
		SNew(SHorizontalBox)
		// X
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double
				{
					return GetVector().X;
				})
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.X = NewValue;
					SetVector(Vec);
				})
				.OnValueCommitted_Lambda([GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					FVector Vec = GetVector();
					Vec.X = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Y
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double
				{
					return GetVector().Y;
				})
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.Y = NewValue;
					SetVector(Vec);
				})
				.OnValueCommitted_Lambda([GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					FVector Vec = GetVector();
					Vec.Y = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Z
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double
				{
					return GetVector().Z;
				})
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.Z = NewValue;
					SetVector(Vec);
				})
				.OnValueCommitted_Lambda([GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					FVector Vec = GetVector();
					Vec.Z = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
	];
}

void FFleshRingSettingsCustomization::AddLinearRotatorRow(
	IDetailChildrenBuilder& ChildBuilder,
	TSharedRef<IPropertyHandle> RotatorHandle,
	const FText& DisplayName,
	float Delta,
	TAttribute<bool> IsEnabled)
{
	TSharedPtr<IPropertyHandle> RotHandlePtr = RotatorHandle.ToSharedPtr();

	// EnumerateRawData로 FRotator 직접 읽기
	auto GetRotator = [RotHandlePtr]() -> FRotator
	{
		FRotator Result = FRotator::ZeroRotator;
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FRotator*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	// EnumerateRawData로 FRotator 직접 쓰기
	auto SetRotator = [RotHandlePtr](const FRotator& NewValue)
	{
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FRotator*>(RawData) = NewValue;
				}
				return true;
			});
			RotHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
		}
	};

	// 각도 표시용 TypeInterface
	auto DegreeInterface = MakeShared<FDegreeTypeInterface>();

	ChildBuilder.AddCustomRow(DisplayName)
	.IsEnabled(IsEnabled)
	.NameContent()
	[
		SNew(STextBlock)
		.Text(DisplayName)
		.Font(IDetailLayoutBuilder::GetDetailFont())
	]
	.ValueContent()
	.MinDesiredWidth(300.0f)
	[
		SNew(SHorizontalBox)
		// Roll (X)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double
				{
					return GetRotator().Roll;
				})
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Roll = NewValue;
					SetRotator(Rot);
				})
				.OnValueCommitted_Lambda([GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					FRotator Rot = GetRotator();
					Rot.Roll = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Pitch (Y)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double
				{
					return GetRotator().Pitch;
				})
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Pitch = NewValue;
					SetRotator(Rot);
				})
				.OnValueCommitted_Lambda([GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					FRotator Rot = GetRotator();
					Rot.Pitch = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Yaw (Z)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double
				{
					return GetRotator().Yaw;
				})
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Yaw = NewValue;
					SetRotator(Rot);
				})
				.OnValueCommitted_Lambda([GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					FRotator Rot = GetRotator();
					Rot.Yaw = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
	];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateLinearVectorWidget(
	TSharedRef<IPropertyHandle> VectorHandle,
	float Delta)
{
	TSharedPtr<IPropertyHandle> VecHandlePtr = VectorHandle.ToSharedPtr();

	auto GetVector = [VecHandlePtr]() -> FVector
	{
		FVector Result = FVector::ZeroVector;
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FVector*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	auto SetVector = [VecHandlePtr](const FVector& NewValue)
	{
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FVector*>(RawData) = NewValue;
				}
				return true;
			});
			VecHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
		}
	};

	return SNew(SHorizontalBox)
		// X
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double { return GetVector().X; })
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.X = NewValue;
					SetVector(Vec);
				})
				.OnValueCommitted_Lambda([GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					FVector Vec = GetVector();
					Vec.X = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Y
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double { return GetVector().Y; })
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.Y = NewValue;
					SetVector(Vec);
				})
				.OnValueCommitted_Lambda([GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					FVector Vec = GetVector();
					Vec.Y = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Z
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double { return GetVector().Z; })
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.Z = NewValue;
					SetVector(Vec);
				})
				.OnValueCommitted_Lambda([GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					FVector Vec = GetVector();
					Vec.Z = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateLinearRotatorWidget(
	TSharedRef<IPropertyHandle> RotatorHandle,
	float Delta)
{
	TSharedPtr<IPropertyHandle> RotHandlePtr = RotatorHandle.ToSharedPtr();

	auto GetRotator = [RotHandlePtr]() -> FRotator
	{
		FRotator Result = FRotator::ZeroRotator;
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FRotator*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	auto SetRotator = [RotHandlePtr](const FRotator& NewValue)
	{
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FRotator*>(RawData) = NewValue;
				}
				return true;
			});
			RotHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
		}
	};

	auto DegreeInterface = MakeShared<FDegreeTypeInterface>();

	return SNew(SHorizontalBox)
		// Roll (X)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Roll; })
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Roll = NewValue;
					SetRotator(Rot);
				})
				.OnValueCommitted_Lambda([GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					FRotator Rot = GetRotator();
					Rot.Roll = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Pitch (Y)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Pitch; })
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Pitch = NewValue;
					SetRotator(Rot);
				})
				.OnValueCommitted_Lambda([GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					FRotator Rot = GetRotator();
					Rot.Pitch = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Yaw (Z)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Yaw; })
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Yaw = NewValue;
					SetRotator(Rot);
				})
				.OnValueCommitted_Lambda([GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					FRotator Rot = GetRotator();
					Rot.Yaw = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		];
}

void FFleshRingSettingsCustomization::AddLinearVectorRowWithReset(
	IDetailChildrenBuilder& ChildBuilder,
	TSharedRef<IPropertyHandle> VectorHandle,
	const FText& DisplayName,
	float Delta,
	const FVector& DefaultValue,
	TAttribute<bool> IsEnabled)
{
	ChildBuilder.AddCustomRow(DisplayName)
	.IsEnabled(IsEnabled)
	.NameContent()
	[
		SNew(STextBlock)
		.Text(DisplayName)
		.Font(IDetailLayoutBuilder::GetDetailFont())
	]
	.ValueContent()
	.MinDesiredWidth(300.0f)
	[
		CreateLinearVectorWidgetWithReset(VectorHandle, Delta, DefaultValue)
	];
}

void FFleshRingSettingsCustomization::AddLinearRotatorRowWithReset(
	IDetailChildrenBuilder& ChildBuilder,
	TSharedRef<IPropertyHandle> RotatorHandle,
	const FText& DisplayName,
	float Delta,
	const FRotator& DefaultValue,
	TAttribute<bool> IsEnabled)
{
	ChildBuilder.AddCustomRow(DisplayName)
	.IsEnabled(IsEnabled)
	.NameContent()
	[
		SNew(STextBlock)
		.Text(DisplayName)
		.Font(IDetailLayoutBuilder::GetDetailFont())
	]
	.ValueContent()
	.MinDesiredWidth(300.0f)
	[
		CreateLinearRotatorWidgetWithReset(RotatorHandle, Delta, DefaultValue)
	];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateLinearVectorWidgetWithReset(
	TSharedRef<IPropertyHandle> VectorHandle,
	float Delta,
	const FVector& DefaultValue)
{
	TSharedPtr<IPropertyHandle> VecHandlePtr = VectorHandle.ToSharedPtr();

	auto GetVector = [VecHandlePtr]() -> FVector
	{
		FVector Result = FVector::ZeroVector;
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FVector*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	auto SetVector = [VecHandlePtr](const FVector& NewValue)
	{
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FVector*>(RawData) = NewValue;
				}
				return true;
			});
			VecHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
		}
	};

	return SNew(SHorizontalBox)
		// X
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double { return GetVector().X; })
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.X = NewValue;
					SetVector(Vec);
				})
				.OnValueCommitted_Lambda([GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					FVector Vec = GetVector();
					Vec.X = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Y
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double { return GetVector().Y; })
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.Y = NewValue;
					SetVector(Vec);
				})
				.OnValueCommitted_Lambda([GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					FVector Vec = GetVector();
					Vec.Y = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Z
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double { return GetVector().Z; })
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.Z = NewValue;
					SetVector(Vec);
				})
				.OnValueCommitted_Lambda([GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					FVector Vec = GetVector();
					Vec.Z = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Reset Button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(4, 0, 0, 0)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.OnClicked_Lambda([SetVector, DefaultValue]() -> FReply
			{
				SetVector(DefaultValue);
				return FReply::Handled();
			})
			.ContentPadding(FMargin(1, 0))
			.ToolTipText(LOCTEXT("ResetToDefault", "Reset to Default"))
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("PropertyWindow.DiffersFromDefault"))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateLinearRotatorWidgetWithReset(
	TSharedRef<IPropertyHandle> RotatorHandle,
	float Delta,
	const FRotator& DefaultValue)
{
	TSharedPtr<IPropertyHandle> RotHandlePtr = RotatorHandle.ToSharedPtr();

	auto GetRotator = [RotHandlePtr]() -> FRotator
	{
		FRotator Result = FRotator::ZeroRotator;
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FRotator*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	auto SetRotator = [RotHandlePtr](const FRotator& NewValue)
	{
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FRotator*>(RawData) = NewValue;
				}
				return true;
			});
			RotHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
		}
	};

	auto DegreeInterface = MakeShared<FDegreeTypeInterface>();

	return SNew(SHorizontalBox)
		// Roll (X)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Roll; })
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Roll = NewValue;
					SetRotator(Rot);
				})
				.OnValueCommitted_Lambda([GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					FRotator Rot = GetRotator();
					Rot.Roll = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Pitch (Y)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Pitch; })
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Pitch = NewValue;
					SetRotator(Rot);
				})
				.OnValueCommitted_Lambda([GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					FRotator Rot = GetRotator();
					Rot.Pitch = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Yaw (Z)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Yaw; })
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Yaw = NewValue;
					SetRotator(Rot);
				})
				.OnValueCommitted_Lambda([GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					FRotator Rot = GetRotator();
					Rot.Yaw = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Reset Button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(4, 0, 0, 0)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.OnClicked_Lambda([SetRotator, DefaultValue]() -> FReply
			{
				SetRotator(DefaultValue);
				return FReply::Handled();
			})
			.ContentPadding(FMargin(1, 0))
			.ToolTipText(LOCTEXT("ResetToDefault", "Reset to Default"))
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("PropertyWindow.DiffersFromDefault"))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateResetButton(
	TSharedRef<IPropertyHandle> VectorHandle,
	const FVector& DefaultValue)
{
	TSharedPtr<IPropertyHandle> VecHandlePtr = VectorHandle.ToSharedPtr();

	auto SetVector = [VecHandlePtr](const FVector& NewValue)
	{
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FVector*>(RawData) = NewValue;
				}
				return true;
			});
			VecHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
		}
	};

	return SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.OnClicked_Lambda([SetVector, DefaultValue]() -> FReply
		{
			SetVector(DefaultValue);
			return FReply::Handled();
		})
		.ContentPadding(FMargin(1, 0))
		.ToolTipText(LOCTEXT("ResetToDefaultVector", "Reset to Default"))
		[
			SNew(SImage)
			.Image(FAppStyle::GetBrush("PropertyWindow.DiffersFromDefault"))
			.ColorAndOpacity(FSlateColor::UseForeground())
		];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateResetButton(
	TSharedRef<IPropertyHandle> RotatorHandle,
	const FRotator& DefaultValue)
{
	TSharedPtr<IPropertyHandle> RotHandlePtr = RotatorHandle.ToSharedPtr();

	auto SetRotator = [RotHandlePtr](const FRotator& NewValue)
	{
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FRotator*>(RawData) = NewValue;
				}
				return true;
			});
			RotHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
		}
	};

	return SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.OnClicked_Lambda([SetRotator, DefaultValue]() -> FReply
		{
			SetRotator(DefaultValue);
			return FReply::Handled();
		})
		.ContentPadding(FMargin(1, 0))
		.ToolTipText(LOCTEXT("ResetToDefaultRotator", "Reset to Default"))
		[
			SNew(SImage)
			.Image(FAppStyle::GetBrush("PropertyWindow.DiffersFromDefault"))
			.ColorAndOpacity(FSlateColor::UseForeground())
		];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateVectorWidgetWithResetButton(
	TSharedRef<IPropertyHandle> VectorHandle,
	float Delta,
	const FVector& DefaultValue)
{
	TSharedPtr<IPropertyHandle> VecHandlePtr = VectorHandle.ToSharedPtr();

	auto GetVector = [VecHandlePtr]() -> FVector
	{
		FVector Result = FVector::ZeroVector;
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FVector*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	auto SetVector = [VecHandlePtr](const FVector& NewValue)
	{
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->NotifyPreChange();
			VecHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FVector*>(RawData) = NewValue;
				}
				return true;
			});
			VecHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
			VecHandlePtr->NotifyFinishedChangingProperties();
		}
	};

	return SNew(SHorizontalBox)
		// X
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double { return GetVector().X; })
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.X = NewValue;
					SetVector(Vec);
				})
				.OnValueCommitted_Lambda([GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					FVector Vec = GetVector();
					Vec.X = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Y
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double { return GetVector().Y; })
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.Y = NewValue;
					SetVector(Vec);
				})
				.OnValueCommitted_Lambda([GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					FVector Vec = GetVector();
					Vec.Y = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Z
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double { return GetVector().Z; })
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.Z = NewValue;
					SetVector(Vec);
				})
				.OnValueCommitted_Lambda([GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					FVector Vec = GetVector();
					Vec.Z = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Reset Button (우측 끝)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(8, 0, 0, 0)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.OnClicked_Lambda([SetVector, DefaultValue]() -> FReply
			{
				SetVector(DefaultValue);
				return FReply::Handled();
			})
			.ContentPadding(FMargin(1, 0))
			.ToolTipText(LOCTEXT("ResetVectorToDefault", "Reset to Default"))
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("PropertyWindow.DiffersFromDefault"))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateRotatorWidgetWithResetButton(
	TSharedRef<IPropertyHandle> RotatorHandle,
	float Delta,
	const FRotator& DefaultValue)
{
	TSharedPtr<IPropertyHandle> RotHandlePtr = RotatorHandle.ToSharedPtr();

	auto GetRotator = [RotHandlePtr]() -> FRotator
	{
		FRotator Result = FRotator::ZeroRotator;
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FRotator*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	auto SetRotator = [RotHandlePtr](const FRotator& NewValue)
	{
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->NotifyPreChange();
			RotHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FRotator*>(RawData) = NewValue;
				}
				return true;
			});
			RotHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
			RotHandlePtr->NotifyFinishedChangingProperties();
		}
	};

	auto DegreeInterface = MakeShared<FDegreeTypeInterface>();

	return SNew(SHorizontalBox)
		// Roll (X)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Roll; })
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Roll = NewValue;
					SetRotator(Rot);
				})
				.OnValueCommitted_Lambda([GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					FRotator Rot = GetRotator();
					Rot.Roll = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Pitch (Y)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Pitch; })
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Pitch = NewValue;
					SetRotator(Rot);
				})
				.OnValueCommitted_Lambda([GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					FRotator Rot = GetRotator();
					Rot.Pitch = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Yaw (Z)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Yaw; })
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Yaw = NewValue;
					SetRotator(Rot);
				})
				.OnValueCommitted_Lambda([GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					FRotator Rot = GetRotator();
					Rot.Yaw = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Reset Button (우측 끝)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(8, 0, 0, 0)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.OnClicked_Lambda([SetRotator, DefaultValue]() -> FReply
			{
				SetRotator(DefaultValue);
				return FReply::Handled();
			})
			.ContentPadding(FMargin(1, 0))
			.ToolTipText(LOCTEXT("ResetRotatorToDefault", "Reset to Default"))
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("PropertyWindow.DiffersFromDefault"))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
}

#undef LOCTEXT_NAMESPACE
