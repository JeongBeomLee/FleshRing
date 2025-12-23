// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetTypeActions_Base.h"
#include "FleshRingDeformer.h"

class FFleshRingDeformerAssetTypeActions : public FAssetTypeActions_Base
{
public:
	// FAssetTypeActions_Base interface
	virtual FText GetName() const override { return NSLOCTEXT("AssetTypeActions", "FleshRingDeformer", "Flesh Ring Deformer"); }
	virtual FColor GetTypeColor() const override { return FColor(255, 128, 64); }
	virtual UClass* GetSupportedClass() const override { return UFleshRingDeformer::StaticClass(); }
	virtual uint32 GetCategories() override { return EAssetTypeCategories::Misc; }
};
