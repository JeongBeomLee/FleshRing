// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingAssetFactory.h"
#include "FleshRingAsset.h"
#include "AssetTypeCategories.h"

#define LOCTEXT_NAMESPACE "FleshRingAssetFactory"

UFleshRingAssetFactory::UFleshRingAssetFactory()
{
	SupportedClass = UFleshRingAsset::StaticClass();
	bCreateNew = true;
	bEditAfterNew = true;
}

UObject* UFleshRingAssetFactory::FactoryCreateNew(UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
	return NewObject<UFleshRingAsset>(InParent, Class, Name, Flags);
}

FText UFleshRingAssetFactory::GetDisplayName() const
{
	return LOCTEXT("FleshRingAssetFactoryDisplayName", "FleshRing Asset");
}

uint32 UFleshRingAssetFactory::GetMenuCategories() const
{
	return EAssetTypeCategories::Misc;
}

#undef LOCTEXT_NAMESPACE
