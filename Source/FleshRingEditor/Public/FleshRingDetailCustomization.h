// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class IDetailLayoutBuilder;

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
};
