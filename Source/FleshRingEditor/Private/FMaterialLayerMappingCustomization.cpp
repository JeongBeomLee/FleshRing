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
}

TSharedRef<IPropertyTypeCustomization> FMaterialLayerMappingCustomization::MakeInstance()
{
	return MakeShared<FMaterialLayerMappingCustomization>();
}

void FMaterialLayerMappingCustomization::CustomizeHeader(
	TSharedRef<IPropertyHandle> PropertyHandle,				// FMaterialLayerMapping 하나에 대한 핸들
	FDetailWidgetRow& HeaderRow,							// 이 행의 UI를 여기에 구성
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// 나중에 GetOuterAsset() 에서 사용
	MainPropertyHandle = PropertyHandle;


	// 자식 프로퍼티 핸들 가져오기
	TSharedPtr<IPropertyHandle> SlotIndexHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FMaterialLayerMapping, MaterialSlotIndex));
	TSharedPtr<IPropertyHandle> SlotNameHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FMaterialLayerMapping, MaterialSlotName));

	// 값 추출
	int32 SlotIndex = 0;
	if (SlotIndexHandle.IsValid())
	{
		SlotIndexHandle->GetValue(SlotIndex);
	}

	FName SlotName = NAME_None;
	if (SlotNameHandle.IsValid())
	{
		SlotNameHandle->GetValue(SlotName);
	}

	// 머티리얼 가져오기
	UMaterialInterface* Material = GetMaterialForSlot(SlotIndex);

	// 썸네일 위젯 생성
	// FAssetThumbnail: 에셋의 미리보기 이미지 렌더링하는 UE 클래스
	TSharedPtr<FAssetThumbnail> Thumbnail;
	TSharedRef<SWidget> ThumbnailWidget = SNullWidget::NullWidget;

	if (Material && ThumbnailPool.IsValid())
	{
		Thumbnail = MakeShared<FAssetThumbnail>(Material, ThumbnailSize, ThumbnailSize, ThumbnailPool.ToSharedRef());

		ThumbnailWidget = SNew(SBox)
			.WidthOverride(ThumbnailSize)
			.HeightOverride(ThumbnailSize)
			.Padding(2.0f)
			[
				Thumbnail->MakeThumbnailWidget() // 실제 썸네일 이미지 위젯
			];
	}
	else
	{
		// 머티리얼이 없으면 빈 박스 표시
		ThumbnailWidget = SNew(SBox)
			.WidthOverride(ThumbnailSize)
			.HeightOverride(ThumbnailSize)
			.Padding(2.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("NoMaterial", "?"))
				.Justification(ETextJustify::Center)
			];
	}

	// 헤더 행 구성: [썸네일] [인덱스] [슬롯 이름]
	HeaderRow
		.NameContent()
		[
			// 슬롯 1: 썸네일
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				ThumbnailWidget
			]
			// 슬롯 2: 인덱스
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("SlotIndexFormat", "[{0}]"), FText::AsNumber(SlotIndex)))
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			// 슬롯 3: 슬롯 이름
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(4.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromName(SlotName))
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		.ValueContent()
		.MinDesiredWidth(200.0f)
		[
			// 기본 값 위젯 (확장 화살표)
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
