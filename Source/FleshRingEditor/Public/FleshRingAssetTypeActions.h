// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetTypeActions_Base.h"

class UFleshRingAsset;

/**
 * FleshRing Asset의 Content Browser 통합
 * - 더블클릭 시 전용 에디터 열기
 * - 아이콘/색상 설정
 * - 우클릭 메뉴 커스터마이징
 */
class FFleshRingAssetTypeActions : public FAssetTypeActions_Base
{
public:
	// FAssetTypeActions_Base interface
	virtual FText GetName() const override;
	virtual FColor GetTypeColor() const override;
	virtual UClass* GetSupportedClass() const override;
	virtual uint32 GetCategories() override;

	/** 더블클릭 시 에디터 열기 */
	virtual void OpenAssetEditor(const TArray<UObject*>& InObjects, TSharedPtr<class IToolkitHost> EditWithinLevelEditor = TSharedPtr<IToolkitHost>()) override;
};
