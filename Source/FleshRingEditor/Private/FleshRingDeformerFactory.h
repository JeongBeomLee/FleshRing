// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "FleshRingDeformerFactory.generated.h"

UCLASS()
class UFleshRingDeformerFactory : public UFactory
{
	GENERATED_BODY()

public:
	UFleshRingDeformerFactory();

	// UFactory interface
	virtual bool CanCreateNew() const override { return true; }
	virtual bool ShouldShowInNewMenu() const override { return true; }
	virtual UObject* FactoryCreateNew(UClass* Class, UObject* InParent, FName Name, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn) override;
	virtual FText GetDisplayName() const override;
	virtual uint32 GetMenuCategories() const override;
	virtual FString GetDefaultNewAssetName() const override { return TEXT("NewFleshRingDeformer"); }
};
