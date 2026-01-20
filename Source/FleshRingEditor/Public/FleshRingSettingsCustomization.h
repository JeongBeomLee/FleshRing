// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IPropertyTypeCustomization.h"
#include "PropertyHandle.h"
#include "Widgets/SWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STreeView.h"

class IDetailChildrenBuilder;
class SComboButton;
class SInlineEditableTextBlock;

/**
 * Bone 드롭다운용 트리 아이템
 */
struct FBoneDropdownItem : public TSharedFromThis<FBoneDropdownItem>
{
	FName BoneName;
	int32 BoneIndex = INDEX_NONE;
	bool bIsMeshBone = false;  // 웨이팅된 본 (자손 포함)
	TArray<TSharedPtr<FBoneDropdownItem>> Children;
	TWeakPtr<FBoneDropdownItem> ParentItem;

	static TSharedPtr<FBoneDropdownItem> Create(FName InBoneName, int32 InBoneIndex, bool bInIsMeshBone)
	{
		TSharedPtr<FBoneDropdownItem> Item = MakeShared<FBoneDropdownItem>();
		Item->BoneName = InBoneName;
		Item->BoneIndex = InBoneIndex;
		Item->bIsMeshBone = bInIsMeshBone;
		return Item;
	}
};

/**
 * FFleshRingSettings 구조체의 프로퍼티 타입 커스터마이저
 * Bone 이름을 드롭다운으로 선택할 수 있게 함
 */
class FFleshRingSettingsCustomization : public IPropertyTypeCustomization
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
	/** Bone 트리 구조 빌드 */
	void BuildBoneTree();

	/** 웨이팅된 본 캐시 빌드 */
	void BuildWeightedBoneCache(class USkeletalMesh* SkelMesh);

	/** 본이 웨이팅되어 있는지 확인 */
	bool IsBoneWeighted(int32 BoneIndex) const;

	/** 검색 가능한 Bone 드롭다운 위젯 생성 */
	TSharedRef<SWidget> CreateSearchableBoneDropdown();

	/** 검색 텍스트 변경 시 호출 */
	void OnBoneSearchTextChanged(const FText& NewText);

	/** 검색 필터 적용 */
	void ApplySearchFilter();

	/** TreeView 행 생성 */
	TSharedRef<ITableRow> GenerateBoneTreeRow(TSharedPtr<FBoneDropdownItem> InItem, const TSharedRef<STableViewBase>& OwnerTable);

	/** TreeView 자식 가져오기 */
	void GetBoneTreeChildren(TSharedPtr<FBoneDropdownItem> Item, TArray<TSharedPtr<FBoneDropdownItem>>& OutChildren);

	/** TreeView에서 선택 시 호출 */
	void OnBoneTreeSelectionChanged(TSharedPtr<FBoneDropdownItem> NewSelection, ESelectInfo::Type SelectInfo);

	/** 모든 트리 아이템 확장 */
	void ExpandAllBoneTreeItems();

	/** 현재 선택된 Bone 이름 가져오기 */
	FText GetCurrentBoneName() const;

	/** 현재 본이 유효하지 않은지 확인 (경고 아이콘 표시용) */
	bool IsBoneInvalid() const;

	/** EulerRotation에서 FQuat으로 동기화 */
	void SyncQuatFromEuler(
		TSharedPtr<IPropertyHandle> EulerHandle,
		TSharedPtr<IPropertyHandle> QuatHandle);

	/** FQuat 핸들에서 Euler 각도 읽기 */
	FRotator GetQuatAsEuler(TSharedPtr<IPropertyHandle> QuatHandle) const;

	/** FVector용 선형 드래그 Row 추가 */
	void AddLinearVectorRow(
		IDetailChildrenBuilder& ChildBuilder,
		TSharedRef<IPropertyHandle> VectorHandle,
		const FText& DisplayName,
		float Delta,
		TAttribute<bool> IsEnabled = true);

	/** FRotator용 선형 드래그 Row 추가 */
	void AddLinearRotatorRow(
		IDetailChildrenBuilder& ChildBuilder,
		TSharedRef<IPropertyHandle> RotatorHandle,
		const FText& DisplayName,
		float Delta,
		TAttribute<bool> IsEnabled = true);

	/** Euler 각도를 FQuat 핸들에 쓰기 */
	void SetEulerToQuat(TSharedPtr<IPropertyHandle> QuatHandle, const FRotator& Euler);

	/** FVector용 선형 드래그 위젯 생성 (그룹용) */
	TSharedRef<SWidget> CreateLinearVectorWidget(
		TSharedRef<IPropertyHandle> VectorHandle,
		float Delta);

	/** FRotator용 선형 드래그 위젯 생성 (그룹용) */
	TSharedRef<SWidget> CreateLinearRotatorWidget(
		TSharedRef<IPropertyHandle> RotatorHandle,
		float Delta);

	/** FVector용 선형 드래그 Row + Reset 버튼 추가 */
	void AddLinearVectorRowWithReset(
		IDetailChildrenBuilder& ChildBuilder,
		TSharedRef<IPropertyHandle> VectorHandle,
		const FText& DisplayName,
		float Delta,
		const FVector& DefaultValue,
		TAttribute<bool> IsEnabled = true);

	/** FRotator용 선형 드래그 Row + Reset 버튼 추가 */
	void AddLinearRotatorRowWithReset(
		IDetailChildrenBuilder& ChildBuilder,
		TSharedRef<IPropertyHandle> RotatorHandle,
		const FText& DisplayName,
		float Delta,
		const FRotator& DefaultValue,
		TAttribute<bool> IsEnabled = true);

	/** FVector용 선형 드래그 위젯 + Reset 버튼 생성 (그룹용) */
	TSharedRef<SWidget> CreateLinearVectorWidgetWithReset(
		TSharedRef<IPropertyHandle> VectorHandle,
		float Delta,
		const FVector& DefaultValue);

	/** FRotator용 선형 드래그 위젯 + Reset 버튼 생성 (그룹용) */
	TSharedRef<SWidget> CreateLinearRotatorWidgetWithReset(
		TSharedRef<IPropertyHandle> RotatorHandle,
		float Delta,
		const FRotator& DefaultValue);

	/** FVector용 Reset 버튼 생성 */
	TSharedRef<SWidget> CreateResetButton(
		TSharedRef<IPropertyHandle> VectorHandle,
		const FVector& DefaultValue);

	/** FRotator용 Reset 버튼 생성 */
	TSharedRef<SWidget> CreateResetButton(
		TSharedRef<IPropertyHandle> RotatorHandle,
		const FRotator& DefaultValue);

	/** FVector 위젯 + Reset 버튼 (우측 끝 배치) */
	TSharedRef<SWidget> CreateVectorWidgetWithResetButton(
		TSharedRef<IPropertyHandle> VectorHandle,
		float Delta,
		const FVector& DefaultValue);

	/** FRotator 위젯 + Reset 버튼 (우측 끝 배치) */
	TSharedRef<SWidget> CreateRotatorWidgetWithResetButton(
		TSharedRef<IPropertyHandle> RotatorHandle,
		float Delta,
		const FRotator& DefaultValue);

	/** 프로퍼티 핸들 캐싱 */
	TSharedPtr<IPropertyHandle> BoneNameHandle;

	/** 메인 프로퍼티 핸들 (Asset 접근용) */
	TSharedPtr<IPropertyHandle> MainPropertyHandle;

	/** FQuat 프로퍼티 핸들 캐싱 (Euler 표시용) */
	TSharedPtr<IPropertyHandle> RingRotationHandle;
	TSharedPtr<IPropertyHandle> MeshRotationHandle;

	/** 본 트리 루트 아이템 */
	TArray<TSharedPtr<FBoneDropdownItem>> BoneTreeRoots;

	/** 모든 본 아이템 (인덱스로 빠른 접근용) */
	TArray<TSharedPtr<FBoneDropdownItem>> AllBoneItems;

	/** 필터링된 본 트리 루트 아이템 */
	TArray<TSharedPtr<FBoneDropdownItem>> FilteredBoneTreeRoots;

	/** 웨이팅된 본 인덱스 캐시 */
	TSet<int32> WeightedBoneIndices;

	/** 검색 텍스트 */
	FString BoneSearchText;

	/** Bone TreeView 위젯 참조 (갱신용) */
	TSharedPtr<STreeView<TSharedPtr<FBoneDropdownItem>>> BoneTreeView;

	/** ComboButton 위젯 참조 (닫기용) */
	TSharedPtr<SComboButton> BoneComboButton;

	/** Asset에서 TargetSkeletalMesh 가져오기 */
	class USkeletalMesh* GetTargetSkeletalMesh() const;

	/** 상위 Asset 가져오기 */
	class UFleshRingAsset* GetOuterAsset() const;

	/** 헤더 클릭 시 Ring 선택 */
	FReply OnHeaderClicked(int32 RingIndex);

	/** 헤더 클릭 시 Ring 선택 (void 버전, FSimpleDelegate용) */
	void OnHeaderClickedVoid();

	/** 표시용 Ring 이름 가져오기 */
	FText GetDisplayRingName(int32 Index) const;

	/** Ring 이름 커밋 시 호출 */
	void OnRingNameCommitted(const FText& NewText, ETextCommit::Type CommitType);

	/** 이 Ring이 현재 선택되어 있는지 확인 */
	bool IsThisRingSelected() const;

	/** 헤더 배경색 반환 (선택 상태에 따라 하이라이트) */
	FSlateColor GetHeaderBackgroundColor() const;

	/** 배열 인덱스 캐시 */
	int32 CachedArrayIndex = INDEX_NONE;

	/** Ring 이름 인라인 편집 위젯 참조 */
	TSharedPtr<class SRingNameWidget> RingNameWidget;

	// === MeshScale 비율 잠금 기능 ===

	/** MeshScale 비율 잠금 위젯 생성 (잠금 버튼 + 벡터 위젯) */
	TSharedRef<SWidget> CreateMeshScaleWidget(TSharedRef<IPropertyHandle> VectorHandle, float Delta);

	/** MeshScale 잠금 버튼 클릭 핸들러 */
	FReply OnMeshScaleLockClicked();

	/** MeshScale 비율 잠금 상태 (true: 비율 유지) */
	bool bMeshScaleLocked = false;

	/** MeshScale 프로퍼티 핸들 캐싱 (비율 계산용) */
	TSharedPtr<IPropertyHandle> MeshScaleHandle;
};
