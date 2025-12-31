// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"

class UFleshRingAsset;
class USkeletalMesh;
class SSearchBox;
class SComboButton;
struct FFleshRingSettings;

/**
 * 트리 아이템 타입
 */
enum class EFleshRingTreeItemType : uint8
{
	Bone,
	Ring
};

/**
 * 트리 아이템 구조체 (Persona 스타일)
 */
struct FFleshRingTreeItem : public TSharedFromThis<FFleshRingTreeItem>
{
	/** 아이템 타입 */
	EFleshRingTreeItemType ItemType = EFleshRingTreeItemType::Bone;

	/** 본 이름 (Bone 타입) 또는 부착된 본 이름 (Ring 타입) */
	FName BoneName;

	/** 본 인덱스 (Bone 타입) */
	int32 BoneIndex = INDEX_NONE;

	/** Ring 인덱스 (Ring 타입) */
	int32 RingIndex = INDEX_NONE;

	/** 자식 노드들 */
	TArray<TSharedPtr<FFleshRingTreeItem>> Children;

	/** 부모 노드 */
	TWeakPtr<FFleshRingTreeItem> Parent;

	/** 메시 본인지 (IK/가상 본이 아닌) */
	bool bIsMeshBone = true;

	/** 필터링으로 숨겨졌는지 */
	bool bIsFiltered = false;

	/** 마지막 자식인지 (트리 라인용) */
	bool bIsLastChild = false;

	/** 깊이 레벨 */
	int32 Depth = 0;

	/** 표시 이름 */
	FText GetDisplayName() const;

	/** Bone 생성자 */
	static TSharedPtr<FFleshRingTreeItem> CreateBone(FName InBoneName, int32 InBoneIndex);

	/** Ring 생성자 */
	static TSharedPtr<FFleshRingTreeItem> CreateRing(FName InBoneName, int32 InRingIndex);
};

/** 본 선택 델리게이트 */
DECLARE_DELEGATE_OneParam(FOnBoneSelected, FName /*BoneName*/);

/** Ring 선택 델리게이트 */
DECLARE_DELEGATE_OneParam(FOnRingSelected, int32 /*RingIndex*/);

/** Ring 추가 요청 델리게이트 */
DECLARE_DELEGATE_OneParam(FOnAddRingRequested, FName /*BoneName*/);

/** 카메라 포커스 요청 델리게이트 */
DECLARE_DELEGATE(FOnFocusCameraRequested);

/**
 * FleshRing 에디터용 스켈레톤 트리 위젯 (Persona 스타일)
 */
class SFleshRingSkeletonTree : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFleshRingSkeletonTree) {}
		SLATE_ARGUMENT(UFleshRingAsset*, Asset)
		SLATE_EVENT(FOnBoneSelected, OnBoneSelected)
		SLATE_EVENT(FOnRingSelected, OnRingSelected)
		SLATE_EVENT(FOnAddRingRequested, OnAddRingRequested)
		SLATE_EVENT(FOnFocusCameraRequested, OnFocusCameraRequested)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Asset 설정 */
	void SetAsset(UFleshRingAsset* InAsset);

	/** 트리 갱신 */
	void RefreshTree();

	/** 특정 본 선택 */
	void SelectBone(FName BoneName);

	/** 현재 선택된 본 이름 반환 */
	FName GetSelectedBoneName() const;

	/** 선택 해제 */
	void ClearSelection();

	/** Ring 인덱스로 선택 (뷰포트에서 호출) */
	void SelectRingByIndex(int32 RingIndex);

	// SWidget interface
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

private:
	/** 트리 데이터 빌드 */
	void BuildTree();

	/** TreeView 행 생성 */
	TSharedRef<ITableRow> GenerateTreeRow(TSharedPtr<FFleshRingTreeItem> Item, const TSharedRef<STableViewBase>& OwnerTable);

	/** TreeView 자식 노드 */
	void GetChildrenForTree(TSharedPtr<FFleshRingTreeItem> Item, TArray<TSharedPtr<FFleshRingTreeItem>>& OutChildren);

	/** TreeView 선택 변경 */
	void OnTreeSelectionChanged(TSharedPtr<FFleshRingTreeItem> Item, ESelectInfo::Type SelectInfo);

	/** TreeView 더블클릭 */
	void OnTreeDoubleClick(TSharedPtr<FFleshRingTreeItem> Item);

	/** TreeView 확장 변경 */
	void OnTreeExpansionChanged(TSharedPtr<FFleshRingTreeItem> Item, bool bIsExpanded);

	/** 확장 상태 저장 */
	void SaveExpansionState();

	/** 확장 상태 복원 */
	void RestoreExpansionState();

	/** 컨텍스트 메뉴 생성 */
	TSharedPtr<SWidget> CreateContextMenu();

	/** 특정 아이템 찾기 */
	TSharedPtr<FFleshRingTreeItem> FindItem(FName BoneName, const TArray<TSharedPtr<FFleshRingTreeItem>>& Items);

	/** Ring 데이터 업데이트 */
	void UpdateRingItems();

	/** 버텍스 웨이팅된 본인지 체크 */
	bool IsBoneWeighted(int32 BoneIndex) const;

	/** 웨이팅된 본 인덱스 캐시 빌드 */
	void BuildWeightedBoneCache(USkeletalMesh* SkelMesh);

	// === 툴바 ===

	/** 상단 툴바 생성 */
	TSharedRef<SWidget> CreateToolbar();

	/** + 버튼 클릭 */
	FReply OnAddButtonClicked();

	/** 검색 텍스트 변경 */
	void OnSearchTextChanged(const FText& NewText);

	/** 필터 적용 */
	void ApplyFilter();

	/** 설정 메뉴 생성 */
	TSharedRef<SWidget> CreateFilterMenu();

	// === 필터 옵션 ===

	void OnShowAllBones();
	bool IsShowAllBonesChecked() const;

	void OnShowMeshBonesOnly();
	bool IsShowMeshBonesOnlyChecked() const;

	void OnShowBonesWithRingsOnly();
	bool IsShowBonesWithRingsOnlyChecked() const;

	// === 컨텍스트 메뉴 액션 ===

	/** Ring 추가 */
	void OnContextMenuAddRing();
	bool CanAddRing() const;

	/** Ring 삭제 */
	void OnContextMenuDeleteRing();
	bool CanDeleteRing() const;

	/** 본 이름 복사 */
	void OnContextMenuCopyBoneName();

private:
	/** 편집 중인 Asset */
	TWeakObjectPtr<UFleshRingAsset> EditingAsset;

	/** 본 선택 델리게이트 */
	FOnBoneSelected OnBoneSelected;

	/** Ring 선택 델리게이트 */
	FOnRingSelected OnRingSelected;

	/** Ring 추가 요청 델리게이트 */
	FOnAddRingRequested OnAddRingRequested;

	/** 카메라 포커스 요청 델리게이트 */
	FOnFocusCameraRequested OnFocusCameraRequested;

	/** 루트 아이템들 */
	TArray<TSharedPtr<FFleshRingTreeItem>> RootItems;

	/** 필터링된 루트 아이템들 */
	TArray<TSharedPtr<FFleshRingTreeItem>> FilteredRootItems;

	/** 모든 아이템 맵 (본 이름 → 아이템) */
	TMap<FName, TSharedPtr<FFleshRingTreeItem>> BoneItemMap;

	/** TreeView 위젯 */
	TSharedPtr<STreeView<TSharedPtr<FFleshRingTreeItem>>> TreeView;

	/** 검색 박스 */
	TSharedPtr<SSearchBox> SearchBox;

	/** 현재 선택된 아이템 */
	TSharedPtr<FFleshRingTreeItem> SelectedItem;

	/** 검색 텍스트 */
	FString SearchText;

	/** 확장된 아이템들 (본 이름) */
	TSet<FName> ExpandedBoneNames;

	/** 본 필터 모드 */
	enum class EBoneFilterMode : uint8
	{
		ShowAll,
		ShowMeshBonesOnly,
		ShowBonesWithRingsOnly
	};
	EBoneFilterMode BoneFilterMode = EBoneFilterMode::ShowAll;

	/** 행 인덱스 카운터 (줄무늬 배경용) */
	mutable int32 RowIndexCounter = 0;

	/** 웨이팅된 본 인덱스 캐시 */
	TSet<int32> WeightedBoneIndices;
};
