// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "FleshRingAssetFactory.generated.h"

/**
 * Factory for creating FleshRing Asset in Content Browser
 */
UCLASS()
class UFleshRingAssetFactory : public UFactory
{
	GENERATED_BODY()

public:
	UFleshRingAssetFactory();

	// UFactory interface
	virtual bool CanCreateNew() const override { return true; }
	virtual bool ShouldShowInNewMenu() const override { return true; }
	virtual UObject* FactoryCreateNew(UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn) override;
	virtual FText GetDisplayName() const override;
	virtual uint32 GetMenuCategories() const override;
	virtual FString GetDefaultNewAssetName() const override { return TEXT("NewFleshRingAsset"); }
};
