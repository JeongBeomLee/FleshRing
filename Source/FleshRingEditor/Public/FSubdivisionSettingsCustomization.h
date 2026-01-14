// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"
#include "PropertyHandle.h"

class IDetailChildrenBuilder;
class UFleshRingAsset;

/**
 * FSubdivisionSettings 구조체의 프로퍼티 타입 커스터마이저
 * Editor Preview / Runtime Settings 서브그룹으로 정리
 */
class FSubdivisionSettingsCustomization : public IPropertyTypeCustomization
{
public:
	/** 커스터마이저 인스턴스 생성 (팩토리 함수) */
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	/** IPropertyTypeCustomization 인터페이스 구현 */

	// Header row customization (collapsed view)
	virtual void CustomizeHeader(
		TSharedRef<IPropertyHandle> PropertyHandle,
		FDetailWidgetRow& HeaderRow,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;

	// Children customization (expanded view)
	virtual void CustomizeChildren(
		TSharedRef<IPropertyHandle> PropertyHandle,
		IDetailChildrenBuilder& ChildBuilder,
		IPropertyTypeCustomizationUtils& CustomizationUtils) override;

private:
	/** 상위 Asset 가져오기 */
	UFleshRingAsset* GetOuterAsset() const;

	/** Subdivision 활성화 여부 */
	bool IsSubdivisionEnabled() const;

	/** Refresh Preview 버튼 클릭 */
	FReply OnRefreshPreviewClicked();

	/** Generate 버튼 클릭 */
	FReply OnGenerateRuntimeMeshClicked();

	/** Clear 버튼 클릭 */
	FReply OnClearRuntimeMeshClicked();

	/** 메인 프로퍼티 핸들 캐싱 */
	TSharedPtr<IPropertyHandle> MainPropertyHandle;
};
