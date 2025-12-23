// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingDetailCustomization.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "FleshRingComponent.h"

#define LOCTEXT_NAMESPACE "FleshRingDetailCustomization"

TSharedRef<IDetailCustomization> FFleshRingDetailCustomization::MakeInstance()
{
	return MakeShareable(new FFleshRingDetailCustomization);
}

void FFleshRingDetailCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	// 선택된 오브젝트 캐싱
	DetailBuilder.GetObjectsBeingCustomized(SelectedObjects);

	// =====================================
	// 카테고리 순서 정의
	// =====================================

	// General 카테고리를 맨 위로
	IDetailCategoryBuilder& GeneralCategory = DetailBuilder.EditCategory(
		TEXT("General"),
		LOCTEXT("GeneralCategory", "General"),
		ECategoryPriority::Important  // 높은 우선순위 = 위쪽에 표시
	);

	// Ring Settings 카테고리
	IDetailCategoryBuilder& RingCategory = DetailBuilder.EditCategory(
		TEXT("Ring Settings"),
		LOCTEXT("RingSettingsCategory", "Ring Settings"),
		ECategoryPriority::Default
	);

	// SDF Settings 카테고리
	IDetailCategoryBuilder& SdfCategory = DetailBuilder.EditCategory(
		TEXT("SDF Settings"),
		LOCTEXT("SdfSettingsCategory", "SDF Settings"),
		ECategoryPriority::Default
	);

	// Debug 카테고리 (에디터 전용)
	IDetailCategoryBuilder& DebugCategory = DetailBuilder.EditCategory(
		TEXT("Debug"),
		LOCTEXT("DebugCategory", "Debug / Visualization"),
		ECategoryPriority::Default
	);

	// =====================================
	// 기본 카테고리 숨기기 (필요시)
	// =====================================

	// ActorComponent 기본 카테고리 숨기기
	DetailBuilder.HideCategory(TEXT("ComponentTick"));
	DetailBuilder.HideCategory(TEXT("Tags"));
	DetailBuilder.HideCategory(TEXT("AssetUserData"));
	DetailBuilder.HideCategory(TEXT("Collision"));
	DetailBuilder.HideCategory(TEXT("Cooking"));
	DetailBuilder.HideCategory(TEXT("ComponentReplication"));

	// =====================================
	// 커스텀 위젯 추가 (추후 확장)
	// =====================================

	// TODO: Ring 배열에 대한 커스텀 위젯
	// TODO: Bone 선택 드롭다운
	// TODO: 시각화 토글 버튼
}

#undef LOCTEXT_NAMESPACE
