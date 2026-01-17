// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"
#include "PropertyHandle.h"

class IDetailChildrenBuilder;
class UFleshRingAsset;
class FAssetThumbnailPool;

/**
 * FMaterialLayerMapping 구조체의 프로퍼티 타입 커스터마이저
 * 머티리얼 썸네일을 표시하여 시각적 식별 용이하게 함
 */
class FMaterialLayerMappingCustomization : public IPropertyTypeCustomization
{
public:
	FMaterialLayerMappingCustomization();
	virtual ~FMaterialLayerMappingCustomization();

	/** 커스터마이저 인스턴스 생성 (팩토리 함수) */
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	/** IPropertyTypeCustomization 인터페이스 구현 */

	// Header row customization (collapsed view - 썸네일 + 이름 표시)
	virtual void CustomizeHeader(
		TSharedRef<IPropertyHandle> PropertyHandle,
		FDetailWidgetRow& HeaderRow,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;

	// Children customization (expanded view - LayerType만 표시)
	virtual void CustomizeChildren(
		TSharedRef<IPropertyHandle> PropertyHandle,
		IDetailChildrenBuilder& ChildBuilder,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;

private:
	/** 상위 FleshRingAsset 가져오기 */
	UFleshRingAsset* GetOuterAsset() const;

	/** MaterialSlotIndex로 머티리얼 가져오기 */
	UMaterialInterface* GetMaterialForSlot(int32 SlotIndex) const;

	/** 메인 프로퍼티 핸들 캐싱 */
	TSharedPtr<IPropertyHandle> MainPropertyHandle;

	/** 썸네일 풀 (에디터에서 썸네일 렌더링용) */
	TSharedPtr<FAssetThumbnailPool> ThumbnailPool;
};
