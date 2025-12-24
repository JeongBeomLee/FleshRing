// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingDetailCustomization.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "IDetailPropertyRow.h"
#include "PropertyCustomizationHelpers.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"

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

	// FleshRing Asset 카테고리 (맨 위)
	IDetailCategoryBuilder& AssetCategory = DetailBuilder.EditCategory(
		TEXT("FleshRing Asset"),
		LOCTEXT("FleshRingAssetCategory", "FleshRing Asset"),
		ECategoryPriority::Important
	);

	// General 카테고리
	IDetailCategoryBuilder& GeneralCategory = DetailBuilder.EditCategory(
		TEXT("General"),
		LOCTEXT("GeneralCategory", "General"),
		ECategoryPriority::Default
	);

	// Target Settings 카테고리
	IDetailCategoryBuilder& TargetCategory = DetailBuilder.EditCategory(
		TEXT("Target Settings"),
		LOCTEXT("TargetSettingsCategory", "Target Settings"),
		ECategoryPriority::Default
	);

	// Debug 카테고리 (에디터 전용)
	IDetailCategoryBuilder& DebugCategory = DetailBuilder.EditCategory(
		TEXT("Debug"),
		LOCTEXT("DebugCategory", "Debug / Visualization"),
		ECategoryPriority::Default
	);

	// =====================================
	// FleshRingAsset 프로퍼티 필터링 적용
	// =====================================

	TSharedRef<IPropertyHandle> AssetPropertyHandle = DetailBuilder.GetProperty(
		GET_MEMBER_NAME_CHECKED(UFleshRingComponent, FleshRingAsset));

	// 기존 프로퍼티 숨기고 필터링된 버전으로 교체
	DetailBuilder.HideProperty(AssetPropertyHandle);

	AssetCategory.AddCustomRow(LOCTEXT("FleshRingAssetRow", "FleshRing Asset"))
		.NameContent()
		[
			AssetPropertyHandle->CreatePropertyNameWidget()
		]
		.ValueContent()
		.MinDesiredWidth(250.0f)
		[
			SNew(SObjectPropertyEntryBox)
				.PropertyHandle(AssetPropertyHandle)
				.AllowedClass(UFleshRingAsset::StaticClass())
				.OnShouldFilterAsset(this, &FFleshRingDetailCustomization::OnShouldFilterAsset)
				.AllowClear(true)
		];

	// =====================================
	// 기본 카테고리 숨기기
	// =====================================

	DetailBuilder.HideCategory(TEXT("ComponentTick"));
	DetailBuilder.HideCategory(TEXT("Tags"));
	DetailBuilder.HideCategory(TEXT("AssetUserData"));
	DetailBuilder.HideCategory(TEXT("Collision"));
	DetailBuilder.HideCategory(TEXT("Cooking"));
	DetailBuilder.HideCategory(TEXT("ComponentReplication"));
}

UFleshRingComponent* FFleshRingDetailCustomization::GetFirstSelectedComponent() const
{
	for (const TWeakObjectPtr<UObject>& Obj : SelectedObjects)
	{
		if (UFleshRingComponent* Component = Cast<UFleshRingComponent>(Obj.Get()))
		{
			return Component;
		}
	}
	return nullptr;
}

USkeletalMesh* FFleshRingDetailCustomization::GetOwnerSkeletalMesh() const
{
	UFleshRingComponent* Component = GetFirstSelectedComponent();
	if (!Component)
	{
		return nullptr;
	}

	AActor* Owner = Component->GetOwner();
	if (!Owner)
	{
		return nullptr;
	}

	// Owner에서 SkeletalMeshComponent 찾기
	TArray<USkeletalMeshComponent*> SkelMeshComponents;
	Owner->GetComponents<USkeletalMeshComponent>(SkelMeshComponents);

	if (SkelMeshComponents.Num() > 0)
	{
		return SkelMeshComponents[0]->GetSkeletalMeshAsset();
	}

	return nullptr;
}

bool FFleshRingDetailCustomization::OnShouldFilterAsset(const FAssetData& AssetData) const
{
	// true 반환 = 필터링됨(숨김), false 반환 = 표시

	USkeletalMesh* OwnerMesh = GetOwnerSkeletalMesh();

	// Owner에 SkeletalMesh가 없으면 모든 Asset 표시
	if (!OwnerMesh)
	{
		return false;
	}

	// Asset 로드해서 TargetSkeletalMesh 확인
	UFleshRingAsset* Asset = Cast<UFleshRingAsset>(AssetData.GetAsset());
	if (!Asset)
	{
		return false;  // 로드 실패 시 표시
	}

	// TargetSkeletalMesh가 설정되지 않은 Asset은 항상 표시
	if (Asset->TargetSkeletalMesh.IsNull())
	{
		return false;
	}

	// TargetSkeletalMesh와 Owner의 SkeletalMesh 비교
	USkeletalMesh* AssetTargetMesh = Asset->TargetSkeletalMesh.LoadSynchronous();
	if (!AssetTargetMesh)
	{
		return false;  // 로드 실패 시 표시
	}

	// 일치하면 표시(false), 불일치하면 숨김(true)
	return AssetTargetMesh != OwnerMesh;
}

#undef LOCTEXT_NAMESPACE
