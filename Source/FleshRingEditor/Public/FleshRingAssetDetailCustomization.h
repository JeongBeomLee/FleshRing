// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"

class IDetailLayoutBuilder;
class UFleshRingAsset;

/**
 * UFleshRingAsset의 Detail Panel 커스터마이저
 * Subdivision Settings 카테고리의 그룹핑 및 버튼 배치 담당
 */
class FFleshRingAssetDetailCustomization : public IDetailCustomization
{
public:
	/** DetailCustomization 인스턴스 생성 (팩토리 함수) */
	static TSharedRef<IDetailCustomization> MakeInstance();

	/** IDetailCustomization 인터페이스 구현 */
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

private:
	/** 편집 중인 Asset 캐싱 */
	TWeakObjectPtr<UFleshRingAsset> CachedAsset;

	/** bEnableSubdivision 체크 (버튼 활성화용) */
	bool IsSubdivisionEnabled() const;

	/** 버튼 클릭 핸들러 */
	FReply OnRefreshPreviewClicked();
	FReply OnGenerateRuntimeMeshClicked();
	FReply OnClearRuntimeMeshClicked();
};
