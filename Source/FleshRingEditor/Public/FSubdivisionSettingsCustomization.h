// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"
#include "PropertyHandle.h"
#include "Containers/Ticker.h"

class IDetailChildrenBuilder;
class UFleshRingAsset;
class UFleshRingComponent;
class USkeletalMesh;

/**
 * FSubdivisionSettings 구조체의 프로퍼티 타입 커스터마이저
 * Editor Preview / Runtime Settings 서브그룹으로 정리
 */
class FSubdivisionSettingsCustomization : public IPropertyTypeCustomization
{
public:
	FSubdivisionSettingsCustomization();
	virtual ~FSubdivisionSettingsCustomization();

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

	/** Generate Subdivided Mesh 버튼 클릭 */
	FReply OnGenerateSubdividedMeshClicked();

	/** Clear Subdivided Mesh 버튼 클릭 */
	FReply OnClearSubdividedMeshClicked();

	/** Bake 버튼 클릭 */
	FReply OnBakeMeshClicked();

	/** Clear Baked Mesh 버튼 클릭 */
	FReply OnClearBakedMeshClicked();

	/** 에셋 저장 (Perforce 체크아웃 프롬프트 포함) */
	void SaveAsset(UFleshRingAsset* Asset);

	// ===== 비동기 베이킹 관련 =====

	/** 비동기 베이크 틱 콜백 */
	bool OnAsyncBakeTick(float DeltaTime);

	/** 비동기 베이크 정리 (성공/실패 시 호출) */
	void CleanupAsyncBake(bool bRestorePreviewMesh);

	/** 원본 프리뷰 메시 복원 (동기 베이크용) */
	void RestoreOriginalPreviewMesh(UFleshRingComponent* PreviewComponent);

	/** 메인 프로퍼티 핸들 캐싱 */
	TSharedPtr<IPropertyHandle> MainPropertyHandle;

	// ===== 비동기 베이킹 상태 =====

	/** 비동기 베이크 진행 중 여부 */
	bool bAsyncBakeInProgress = false;

	/** 비동기 베이크 프레임 카운터 */
	int32 AsyncBakeFrameCount = 0;

	/** 캐시가 유효해진 후 추가 대기 프레임 카운터 */
	int32 PostCacheValidFrameCount = 0;

	/** 비동기 베이크 최대 대기 프레임 */
	static constexpr int32 MaxAsyncBakeFrames = 30;

	/** 캐시 유효화 후 대기 프레임 수 (GPU 연산 완료 보장) */
	static constexpr int32 PostCacheValidWaitFrames = 3;

	/** 비동기 베이크용 에셋 (약한 참조) */
	TWeakObjectPtr<UFleshRingAsset> AsyncBakeAsset;

	/** 비동기 베이크용 컴포넌트 (약한 참조) */
	TWeakObjectPtr<UFleshRingComponent> AsyncBakeComponent;

	/** 원본 프리뷰 메시 (복원용, 약한 참조) */
	TWeakObjectPtr<USkeletalMesh> OriginalPreviewMesh;

	/** 틱커 델리게이트 핸들 */
	FTSTicker::FDelegateHandle TickerHandle;
};
