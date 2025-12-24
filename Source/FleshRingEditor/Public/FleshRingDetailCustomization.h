// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class IDetailLayoutBuilder;
class IPropertyHandle;
class UFleshRingComponent;
class USkeletalMesh;
struct FAssetData;

/**
 * UFleshRingComponent의 Detail Panel 커스터마이저
 * 프로퍼티 그룹핑, 카테고리 정리, 커스텀 위젯 등 담당
 */
class FFleshRingDetailCustomization : public IDetailCustomization
{
public:
	/** DetailCustomization 인스턴스 생성 (팩토리 함수) */
	static TSharedRef<IDetailCustomization> MakeInstance();

	/** IDetailCustomization 인터페이스 구현 */
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	/** 선택된 컴포넌트들 캐싱 */
	TArray<TWeakObjectPtr<UObject>> SelectedObjects;

	/** FleshRingAsset 필터링: Owner의 SkeletalMesh와 일치하는 Asset만 표시 */
	bool OnShouldFilterAsset(const FAssetData& AssetData) const;

	/** Owner의 SkeletalMesh 가져오기 */
	USkeletalMesh* GetOwnerSkeletalMesh() const;

	/** 첫 번째 선택된 FleshRingComponent 가져오기 */
	UFleshRingComponent* GetFirstSelectedComponent() const;
};
