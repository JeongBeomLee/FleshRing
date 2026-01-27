// Copyright 2026 LgThx. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/STreeView.h"
#include "Input/DragAndDrop.h"
#include "FleshRingTypes.h"

class UFleshRingAsset;
class USkeletalMesh;
class UStaticMesh;
class SSearchBox;
class SComboButton;
struct FFleshRingSettings;

/**
 * Tree item type
 */
enum class EFleshRingTreeItemType : uint8
{
	Bone,
	Ring
};

/**
 * Tree item struct (Persona style)
 */
struct FFleshRingTreeItem : public TSharedFromThis<FFleshRingTreeItem>
{
	/** Item type */
	EFleshRingTreeItemType ItemType = EFleshRingTreeItemType::Bone;

	/** Bone name (Bone type) or attached bone name (Ring type) */
	FName BoneName;

	/** Bone index (Bone type) */
	int32 BoneIndex = INDEX_NONE;

	/** Ring index (Ring type) */
	int32 RingIndex = INDEX_NONE;

	/** Asset being edited (for Ring name display) */
	TWeakObjectPtr<UFleshRingAsset> EditingAsset;

	/** Child nodes */
	TArray<TSharedPtr<FFleshRingTreeItem>> Children;

	/** Parent node */
	TWeakPtr<FFleshRingTreeItem> Parent;

	/** Is mesh bone (not IK/virtual bone) */
	bool bIsMeshBone = true;

	/** Is hidden by filtering */
	bool bIsFiltered = false;

	/** Is last child (for tree lines) */
	bool bIsLastChild = false;

	/** Depth level */
	int32 Depth = 0;

	/** Display name */
	FText GetDisplayName() const;

	/** Bone constructor */
	static TSharedPtr<FFleshRingTreeItem> CreateBone(FName InBoneName, int32 InBoneIndex);

	/** Ring constructor */
	static TSharedPtr<FFleshRingTreeItem> CreateRing(FName InBoneName, int32 InRingIndex, UFleshRingAsset* InAsset);
};

/**
 * Ring drag and drop Operation
 */
class FFleshRingDragDropOp : public FDragDropOperation
{
public:
	DRAG_DROP_OPERATOR_TYPE(FFleshRingDragDropOp, FDragDropOperation)

	/** Ring index being dragged */
	int32 RingIndex = INDEX_NONE;

	/** Ring name (for drag visual) */
	FString RingName;

	/** Originally attached bone */
	FName SourceBoneName;

	/** Asset being edited */
	TWeakObjectPtr<UFleshRingAsset> Asset;

	/** Can drop */
	bool bCanDrop = false;

	/** Modifier key state at drag start */
	FModifierKeysState ModifierKeysState;

	/** Is Alt drag (duplicate) */
	bool IsAltDrag() const { return ModifierKeysState.IsAltDown(); }

	/** Is Shift drag (preserve world position) */
	bool IsShiftDrag() const { return ModifierKeysState.IsShiftDown(); }

	/** Factory function */
	static TSharedRef<FFleshRingDragDropOp> New(int32 InRingIndex, const FString& InRingName, FName InBoneName, UFleshRingAsset* InAsset, FModifierKeysState InModifierKeys);

	/** Drag visual */
	virtual TSharedPtr<SWidget> GetDefaultDecorator() const override;

	/** Icon getter (for Slate binding) */
	const FSlateBrush* GetIcon() const { return CurrentIconBrush; }

	/** Icon setter (changes based on drop validity) */
	void SetIcon(const FSlateBrush* InIcon) { CurrentIconBrush = InIcon; }

private:
	/** Currently displayed icon */
	const FSlateBrush* CurrentIconBrush = nullptr;
};

/** Bone selected delegate */
DECLARE_DELEGATE_OneParam(FOnBoneSelected, FName /*BoneName*/);

/** Ring selected delegate */
DECLARE_DELEGATE_OneParam(FOnRingSelected, int32 /*RingIndex*/);

/** Ring add request delegate (includes mesh selection) */
DECLARE_DELEGATE_TwoParams(FOnAddRingRequested, FName /*BoneName*/, UStaticMesh* /*SelectedMesh*/);

/** Camera focus request delegate */
DECLARE_DELEGATE(FOnFocusCameraRequested);

/** Ring deleted delegate (for viewport refresh when deleted from tree) */
DECLARE_DELEGATE(FOnRingDeletedFromTree);

/**
 * Skeleton tree widget for FleshRing editor (Persona style)
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
		SLATE_EVENT(FOnRingDeletedFromTree, OnRingDeleted)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Set Asset */
	void SetAsset(UFleshRingAsset* InAsset);

	/** Refresh tree */
	void RefreshTree();

	/** Select specific bone */
	void SelectBone(FName BoneName);

	/** Get currently selected bone name */
	FName GetSelectedBoneName() const;

	/** Clear selection */
	void ClearSelection();

	/** Select by Ring index (called from viewport) */
	void SelectRingByIndex(int32 RingIndex);

	// SWidget interface
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;

private:
	/** Build tree data */
	void BuildTree();

	/** Generate TreeView row */
	TSharedRef<ITableRow> GenerateTreeRow(TSharedPtr<FFleshRingTreeItem> Item, const TSharedRef<STableViewBase>& OwnerTable);

	/** TreeView children */
	void GetChildrenForTree(TSharedPtr<FFleshRingTreeItem> Item, TArray<TSharedPtr<FFleshRingTreeItem>>& OutChildren);

	/** TreeView selection changed */
	void OnTreeSelectionChanged(TSharedPtr<FFleshRingTreeItem> Item, ESelectInfo::Type SelectInfo);

	/** TreeView double click */
	void OnTreeDoubleClick(TSharedPtr<FFleshRingTreeItem> Item);

	/** Handle Ring renamed */
	void HandleRingRenamed(int32 RingIndex, FName NewName);

	/** Asset changed delegate handler (when name changed from detail panel) */
	void OnAssetChangedHandler(UFleshRingAsset* Asset);

	/** TreeView expansion changed */
	void OnTreeExpansionChanged(TSharedPtr<FFleshRingTreeItem> Item, bool bIsExpanded);

	/** Save expansion state */
	void SaveExpansionState();

	/** Restore expansion state */
	void RestoreExpansionState();

	/** Create context menu */
	TSharedPtr<SWidget> CreateContextMenu();

	/** Find specific item */
	TSharedPtr<FFleshRingTreeItem> FindItem(FName BoneName, const TArray<TSharedPtr<FFleshRingTreeItem>>& Items);

	/** Update Ring items */
	void UpdateRingItems();

	/** Check if bone is vertex weighted */
	bool IsBoneWeighted(int32 BoneIndex) const;

	/** Build weighted bone indices cache */
	void BuildWeightedBoneCache(USkeletalMesh* SkelMesh);

	// === Toolbar ===

	/** Create top toolbar */
	TSharedRef<SWidget> CreateToolbar();

	/** + button clicked */
	FReply OnAddButtonClicked();

	/** Search text changed */
	void OnSearchTextChanged(const FText& NewText);

	/** Apply filter */
	void ApplyFilter();

	/** Create filter menu */
	TSharedRef<SWidget> CreateFilterMenu();

	// === Filter options ===

	void OnShowAllBones();
	bool IsShowAllBonesChecked() const;

	void OnShowMeshBonesOnly();
	bool IsShowMeshBonesOnlyChecked() const;

	void OnShowBonesWithRingsOnly();
	bool IsShowBonesWithRingsOnlyChecked() const;

	// === Context menu actions ===

	/** Add Ring */
	void OnContextMenuAddRing();
	bool CanAddRing() const;

	/** Delete Ring */
	void OnContextMenuDeleteRing();
	bool CanDeleteRing() const;

	/** Rename Ring */
	void OnContextMenuRenameRing();

	/** Copy bone name */
	void OnContextMenuCopyBoneName();

	/** Copy Ring */
	void OnContextMenuCopyRing();
	bool CanCopyRing() const;

	/** Paste Ring (to original bone) */
	void OnContextMenuPasteRing();
	bool CanPasteRing() const;

	/** Paste Ring (to selected bone) */
	void OnContextMenuPasteRingToSelectedBone();
	bool CanPasteRingToSelectedBone() const;

	/** Paste Ring to specific bone (common logic) */
	void PasteRingToBone(FName TargetBoneName);

	// === Drag and drop ===

	/** Move Ring to another bone */
	void MoveRingToBone(int32 RingIndex, FName NewBoneName, bool bPreserveWorldPosition = false);

	/** Duplicate Ring to another bone (Alt+drag) */
	void DuplicateRingToBone(int32 SourceRingIndex, FName TargetBoneName);

private:
	/** Asset being edited */
	TWeakObjectPtr<UFleshRingAsset> EditingAsset;

	/** Bone selected delegate */
	FOnBoneSelected OnBoneSelected;

	/** Ring selected delegate */
	FOnRingSelected OnRingSelected;

	/** Ring add request delegate */
	FOnAddRingRequested OnAddRingRequested;

	/** Camera focus request delegate */
	FOnFocusCameraRequested OnFocusCameraRequested;

	/** Ring deleted delegate */
	FOnRingDeletedFromTree OnRingDeleted;

	/** Root items */
	TArray<TSharedPtr<FFleshRingTreeItem>> RootItems;

	/** Filtered root items */
	TArray<TSharedPtr<FFleshRingTreeItem>> FilteredRootItems;

	/** All items map (bone name -> item) */
	TMap<FName, TSharedPtr<FFleshRingTreeItem>> BoneItemMap;

	/** TreeView widget */
	TSharedPtr<STreeView<TSharedPtr<FFleshRingTreeItem>>> TreeView;

	/** Search box */
	TSharedPtr<SSearchBox> SearchBox;

	/** Currently selected item */
	TSharedPtr<FFleshRingTreeItem> SelectedItem;

	/** Search text */
	FString SearchText;

	/** Expanded items (bone names) */
	TSet<FName> ExpandedBoneNames;

	/** Bone filter mode */
	enum class EBoneFilterMode : uint8
	{
		ShowAll,
		ShowMeshBonesOnly,
		ShowBonesWithRingsOnly
	};
	EBoneFilterMode BoneFilterMode = EBoneFilterMode::ShowAll;

	/** Row index counter (for striped background) */
	mutable int32 RowIndexCounter = 0;

	/** Weighted bone indices cache */
	TSet<int32> WeightedBoneIndices;

	/** Copied Ring settings (acts as clipboard) */
	TOptional<FFleshRingSettings> CopiedRingSettings;

	/** Original bone name of copied Ring */
	FName CopiedRingSourceBone;
};
