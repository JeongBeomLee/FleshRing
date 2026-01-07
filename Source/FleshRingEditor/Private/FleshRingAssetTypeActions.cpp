// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingAssetTypeActions.h"
#include "FleshRingAsset.h"
#include "FleshRingAssetEditor.h"

#define LOCTEXT_NAMESPACE "FleshRingAssetTypeActions"

FText FFleshRingAssetTypeActions::GetName() const
{
	return LOCTEXT("FleshRingAssetName", "FleshRing Asset");
}

FColor FFleshRingAssetTypeActions::GetTypeColor() const
{
	// 분홍/살색 계열 - FleshRing 테마에 맞게
	return FColor(255, 128, 128);
}

UClass* FFleshRingAssetTypeActions::GetSupportedClass() const
{
	return UFleshRingAsset::StaticClass();
}

uint32 FFleshRingAssetTypeActions::GetCategories()
{
	return EAssetTypeCategories::Misc;
}

void FFleshRingAssetTypeActions::OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<IToolkitHost> EditWithinLevelEditor)
{
	EToolkitMode::Type Mode = EditWithinLevelEditor.IsValid()
		? EToolkitMode::WorldCentric
		: EToolkitMode::Standalone;

	for (UObject* Obj : InObjects)
	{
		UFleshRingAsset* Asset = Cast<UFleshRingAsset>(Obj);
		if (Asset)
		{
			TSharedRef<FFleshRingAssetEditor> NewEditor = MakeShared<FFleshRingAssetEditor>();
			NewEditor->InitFleshRingAssetEditor(Mode, EditWithinLevelEditor, Asset);
		}
	}
}

#undef LOCTEXT_NAMESPACE
