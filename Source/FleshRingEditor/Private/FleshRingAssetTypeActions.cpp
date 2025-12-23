// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingAssetTypeActions.h"
#include "FleshRingAsset.h"

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
	// TODO: Week 4에서 FFleshRingAssetEditor 구현 후 교체
	// 현재는 기본 에디터(Property Editor)를 사용
	FAssetTypeActions_Base::OpenAssetEditor(InObjects, EditWithinLevelEditor);
}

#undef LOCTEXT_NAMESPACE
