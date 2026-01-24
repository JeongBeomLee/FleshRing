// Copyright 2026 LgThx. All Rights Reserved.

#include "FMaterialLayerMappingCustomization.h"
#include "DetailWidgetRow.h"
#include "DetailLayoutBuilder.h"
#include "IDetailChildrenBuilder.h"
#include "PropertyCustomizationHelpers.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "AssetThumbnail.h"
#include "FleshRingAsset.h"
#include "FleshRingTypes.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"

#define LOCTEXT_NAMESPACE "FMaterialLayerMappingCustomization"

// 썸네일 크기 (픽셀)
static constexpr int32 ThumbnailSize = 64;

FMaterialLayerMappingCustomization::FMaterialLayerMappingCustomization()
{
	// 썸네일 풀 생성 (최대 64개 썸네일 캐싱)
	ThumbnailPool = MakeShared<FAssetThumbnailPool>(64);
}

FMaterialLayerMappingCustomization::~FMaterialLayerMappingCustomization()
{
	// 에셋 변경 델리게이트 해제
	if (UFleshRingAsset* Asset = CachedAsset.Get())
	{
		if (AssetChangedDelegateHandle.IsValid())
		{
			Asset->OnAssetChanged.Remove(AssetChangedDelegateHandle);
			AssetChangedDelegateHandle.Reset();
		}
	}
}

TSharedRef<IPropertyTypeCustomization> FMaterialLayerMappingCustomization::MakeInstance()
{
	return MakeShared<FMaterialLayerMappingCustomization>();
}

void FMaterialLayerMappingCustomization::CustomizeHeader(
	TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// 프로퍼티 핸들 캐싱
	MainPropertyHandle = PropertyHandle;
	CachedSlotIndexHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FMaterialLayerMapping, MaterialSlotIndex));
	TSharedPtr<IPropertyHandle> SlotNameHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FMaterialLayerMapping, MaterialSlotName));

	// 에셋 변경 델리게이트 구독 (AddRaw 사용 - 소멸자에서 해제됨)
	if (UFleshRingAsset* Asset = GetOuterAsset())
	{
		if (!AssetChangedDelegateHandle.IsValid())
		{
			CachedAsset = Asset;
			AssetChangedDelegateHandle = Asset->OnAssetChanged.AddRaw(
				this, &FMaterialLayerMappingCustomization::OnAssetChanged);
		}
	}

	// 동적 텍스트 바인딩: 프로퍼티 값이 변경되면 자동으로 업데이트됨
	TSharedPtr<IPropertyHandle> LocalSlotIndexHandle = CachedSlotIndexHandle;
	auto GetSlotIndexText = [LocalSlotIndexHandle]() -> FText
	{
		int32 SlotIndex = 0;
		if (LocalSlotIndexHandle.IsValid())
		{
			LocalSlotIndexHandle->GetValue(SlotIndex);
		}
		return FText::Format(LOCTEXT("SlotIndexFormat", "[{0}]"), FText::AsNumber(SlotIndex));
	};

	auto GetSlotNameText = [SlotNameHandle]() -> FText
	{
		FName SlotName = NAME_None;
		if (SlotNameHandle.IsValid())
		{
			SlotNameHandle->GetValue(SlotName);
		}
		return FText::FromName(SlotName);
	};

	// 썸네일 컨테이너 생성 (멤버 변수로 저장하여 나중에 업데이트 가능)
	ThumbnailContainer = SNew(SBox)
		.WidthOverride(ThumbnailSize)
		.HeightOverride(ThumbnailSize)
		.Padding(2.0f);

	// 초기 썸네일 설정
	UpdateThumbnailContent();

	// 헤더 행 구성: [썸네일] [인덱스] [슬롯 이름]
	HeaderRow
		.NameContent()
		[
			SNew(SHorizontalBox)
			// 슬롯 1: 썸네일
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				ThumbnailContainer.ToSharedRef()
			]
			// 슬롯 2: 인덱스 (동적 바인딩)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda(GetSlotIndexText)
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			// 슬롯 3: 슬롯 이름 (동적 바인딩)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text_Lambda(GetSlotNameText)
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		.ValueContent()
		.MinDesiredWidth(200.0f)
		[
			PropertyHandle->CreatePropertyValueWidget()
		];
}

void FMaterialLayerMappingCustomization::CustomizeChildren(
	TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// LayerType만 표시 (MaterialSlotIndex, MaterialSlotName은 헤더에서 이미 표시)
	TSharedPtr<IPropertyHandle> LayerTypeHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FMaterialLayerMapping, LayerType));

	if (LayerTypeHandle.IsValid())
	{
		ChildBuilder.AddProperty(LayerTypeHandle.ToSharedRef());
	}
}

void FMaterialLayerMappingCustomization::UpdateThumbnailContent()
{
	if (!ThumbnailContainer.IsValid())
	{
		return;
	}

	int32 SlotIndex = 0;
	if (CachedSlotIndexHandle.IsValid())
	{
		CachedSlotIndexHandle->GetValue(SlotIndex);
	}

	UMaterialInterface* Material = GetMaterialForSlot(SlotIndex);

	TSharedPtr<SWidget> ContentWidget;
	if (Material && ThumbnailPool.IsValid())
	{
		TSharedPtr<FAssetThumbnail> Thumbnail = MakeShared<FAssetThumbnail>(
			Material, ThumbnailSize, ThumbnailSize, ThumbnailPool.ToSharedRef());
		ContentWidget = Thumbnail->MakeThumbnailWidget();
	}
	else
	{
		ContentWidget = SNew(STextBlock)
			.Text(LOCTEXT("NoMaterial", "?"))
			.Justification(ETextJustify::Center);
	}

	ThumbnailContainer->SetContent(ContentWidget.ToSharedRef());
}

void FMaterialLayerMappingCustomization::OnAssetChanged(UFleshRingAsset* ChangedAsset)
{
	// 에셋이 변경되면 썸네일 업데이트
	UpdateThumbnailContent();
}

UFleshRingAsset* FMaterialLayerMappingCustomization::GetOuterAsset() const
{
	if (!MainPropertyHandle.IsValid())
	{
		return nullptr;
	}

	TArray<UObject*> OuterObjects;
	MainPropertyHandle->GetOuterObjects(OuterObjects);

	for (UObject* Obj : OuterObjects)
	{
		if (UFleshRingAsset* Asset = Cast<UFleshRingAsset>(Obj))
		{
			return Asset;
		}
	}

	return nullptr;
}

UMaterialInterface* FMaterialLayerMappingCustomization::GetMaterialForSlot(int32 SlotIndex) const
{
	// 부모 FleshRingAsset 찾기
	UFleshRingAsset* Asset = GetOuterAsset();
	if (!Asset)
	{
		return nullptr;
	}

	// TSoftObjectPtr에서 SkeletalMesh 로드
	if (Asset->TargetSkeletalMesh.IsNull())
	{
		return nullptr;
	}

	USkeletalMesh* Mesh = Asset->TargetSkeletalMesh.LoadSynchronous();
	if (!Mesh)
	{
		return nullptr;
	}

	// 머티리얼 목록 가져오기
	const TArray<FSkeletalMaterial>& Materials = Mesh->GetMaterials();
	if (!Materials.IsValidIndex(SlotIndex))
	{
		return nullptr;
	}

	// 해당 슬롯의 머티리얼 반환
	return Materials[SlotIndex].MaterialInterface;
}

#undef LOCTEXT_NAMESPACE
