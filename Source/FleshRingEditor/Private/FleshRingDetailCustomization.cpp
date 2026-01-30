// Copyright 2026 LgThx. All Rights Reserved.

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
#include "AssetThumbnail.h"

#define LOCTEXT_NAMESPACE "FleshRingDetailCustomization"

TSharedRef<IDetailCustomization> FFleshRingDetailCustomization::MakeInstance()
{
	return MakeShareable(new FFleshRingDetailCustomization);
}

void FFleshRingDetailCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	// Cache selected objects
	DetailBuilder.GetObjectsBeingCustomized(SelectedObjects);

	// =====================================
	// Define category order
	// =====================================

	// FleshRing Asset category (top)
	IDetailCategoryBuilder& AssetCategory = DetailBuilder.EditCategory(
		TEXT("FleshRing Asset"),
		LOCTEXT("FleshRingAssetCategory", "FleshRing Asset"),
		ECategoryPriority::Important
	);

	// General category
	IDetailCategoryBuilder& GeneralCategory = DetailBuilder.EditCategory(
		TEXT("General"),
		LOCTEXT("GeneralCategory", "General"),
		ECategoryPriority::Default
	);

	// Target Settings category
	IDetailCategoryBuilder& TargetCategory = DetailBuilder.EditCategory(
		TEXT("Target Settings"),
		LOCTEXT("TargetSettingsCategory", "Target Settings"),
		ECategoryPriority::Default
	);

	// Debug category (editor only)
	IDetailCategoryBuilder& DebugCategory = DetailBuilder.EditCategory(
		TEXT("Debug"),
		LOCTEXT("DebugCategory", "Debug / Visualization"),
		ECategoryPriority::Default
	);

	// =====================================
	// Apply FleshRingAsset property filtering
	// =====================================

	TSharedRef<IPropertyHandle> AssetPropertyHandle = DetailBuilder.GetProperty(
		GET_MEMBER_NAME_CHECKED(UFleshRingComponent, FleshRingAsset));

	// Hide existing property and replace with filtered version
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
				.DisplayThumbnail(true)
				.ThumbnailPool(DetailBuilder.GetThumbnailPool())
		];

	// =====================================
	// Hide default categories
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

	// Find SkeletalMeshComponent from Owner
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
	// return true = filtered (hidden), return false = shown

	USkeletalMesh* OwnerMesh = GetOwnerSkeletalMesh();

	// If Owner has no SkeletalMesh, show all Assets
	if (!OwnerMesh)
	{
		return false;
	}

	// Load Asset to check TargetSkeletalMesh
	UFleshRingAsset* Asset = Cast<UFleshRingAsset>(AssetData.GetAsset());
	if (!Asset)
	{
		return false;  // Show on load failure
	}

	// Always show Assets with no TargetSkeletalMesh set
	if (Asset->TargetSkeletalMesh.IsNull())
	{
		return false;
	}

	// Compare TargetSkeletalMesh with Owner's SkeletalMesh
	USkeletalMesh* AssetTargetMesh = Asset->TargetSkeletalMesh.LoadSynchronous();
	if (!AssetTargetMesh)
	{
		return false;  // Show on load failure
	}

	// Match = show (false), mismatch = hide (true)
	return AssetTargetMesh != OwnerMesh;
}

#undef LOCTEXT_NAMESPACE
