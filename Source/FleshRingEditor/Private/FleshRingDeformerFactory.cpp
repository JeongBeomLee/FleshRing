// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingDeformerFactory.h"
#include "FleshRingDeformer.h"
#include "AssetTypeCategories.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(FleshRingDeformerFactory)

UFleshRingDeformerFactory::UFleshRingDeformerFactory()
{
	bCreateNew = true;
	bEditAfterNew = true;
	SupportedClass = UFleshRingDeformer::StaticClass();
}

UObject* UFleshRingDeformerFactory::FactoryCreateNew(UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
	return NewObject<UFleshRingDeformer>(InParent, Class, Name, Flags);
}

FText UFleshRingDeformerFactory::GetDisplayName() const
{
	return FText::FromString(TEXT("Flesh Ring Deformer"));
}

uint32 UFleshRingDeformerFactory::GetMenuCategories() const
{
	return EAssetTypeCategories::Misc;
}
