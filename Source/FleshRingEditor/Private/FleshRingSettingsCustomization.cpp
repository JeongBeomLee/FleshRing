// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingSettingsCustomization.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SBoxPanel.h"
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

	// Rotation 핸들 캐싱
	RingRotationHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingRotation));
	MeshRotationHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshRotation));

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

		// FQuat은 UI에서 숨김 (EulerRotation만 표시)
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingRotation) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshRotation))
		{
			continue;
		}

		// Transform 프로퍼티들은 선형 드래그 감도 적용
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingOffset))
		{
			AddLinearVectorRow(ChildBuilder, ChildHandle, LOCTEXT("RingOffset", "Ring Offset"), 1.0f);
			continue;
		}
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingEulerRotation))
		{
			AddLinearRotatorRow(ChildBuilder, ChildHandle, LOCTEXT("RingRotation", "Ring Rotation"), 1.0f);
			continue;
		}
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshOffset))
		{
			AddLinearVectorRow(ChildBuilder, ChildHandle, LOCTEXT("MeshOffset", "Mesh Offset"), 1.0f);
			continue;
		}
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshEulerRotation))
		{
			AddLinearRotatorRow(ChildBuilder, ChildHandle, LOCTEXT("MeshRotation", "Mesh Rotation"), 1.0f);
			continue;
		}
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshScale))
		{
			AddLinearVectorRow(ChildBuilder, ChildHandle, LOCTEXT("MeshScale", "Mesh Scale"), 0.0025f);
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
	float Delta)
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
	float Delta)
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

#undef LOCTEXT_NAMESPACE
