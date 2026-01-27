// Copyright 2026 LgThx. All Rights Reserved.

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
 * Tree item for bone dropdown
 */
struct FBoneDropdownItem : public TSharedFromThis<FBoneDropdownItem>
{
	FName BoneName;
	int32 BoneIndex = INDEX_NONE;
	bool bIsMeshBone = false;  // Weighted bone (including descendants)
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
 * Property type customizer for FFleshRingSettings struct
 * Allows selecting bone names from a dropdown
 */
class FFleshRingSettingsCustomization : public IPropertyTypeCustomization
{
public:
	/** Create customizer instance (factory function) */
	static TSharedRef<IPropertyTypeCustomization> MakeInstance();

	/** IPropertyTypeCustomization interface implementation */

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
	/** Build bone tree structure */
	void BuildBoneTree();

	/** Build weighted bone cache */
	void BuildWeightedBoneCache(class USkeletalMesh* SkelMesh);

	/** Check if bone is weighted */
	bool IsBoneWeighted(int32 BoneIndex) const;

	/** Create searchable bone dropdown widget */
	TSharedRef<SWidget> CreateSearchableBoneDropdown();

	/** Called when search text changes */
	void OnBoneSearchTextChanged(const FText& NewText);

	/** Apply search filter */
	void ApplySearchFilter();

	/** Generate TreeView row */
	TSharedRef<ITableRow> GenerateBoneTreeRow(TSharedPtr<FBoneDropdownItem> InItem, const TSharedRef<STableViewBase>& OwnerTable);

	/** Get TreeView children */
	void GetBoneTreeChildren(TSharedPtr<FBoneDropdownItem> Item, TArray<TSharedPtr<FBoneDropdownItem>>& OutChildren);

	/** Called when selection changes in TreeView */
	void OnBoneTreeSelectionChanged(TSharedPtr<FBoneDropdownItem> NewSelection, ESelectInfo::Type SelectInfo);

	/** Expand all tree items */
	void ExpandAllBoneTreeItems();

	/** Get currently selected bone name */
	FText GetCurrentBoneName() const;

	/** Check if current bone is invalid (for warning icon display) */
	bool IsBoneInvalid() const;

	/** Sync from EulerRotation to FQuat */
	void SyncQuatFromEuler(
		TSharedPtr<IPropertyHandle> EulerHandle,
		TSharedPtr<IPropertyHandle> QuatHandle);

	/** Read Euler angles from FQuat handle */
	FRotator GetQuatAsEuler(TSharedPtr<IPropertyHandle> QuatHandle) const;

	/** Add linear drag row for FVector */
	void AddLinearVectorRow(
		IDetailChildrenBuilder& ChildBuilder,
		TSharedRef<IPropertyHandle> VectorHandle,
		const FText& DisplayName,
		float Delta,
		TAttribute<bool> IsEnabled = true);

	/** Add linear drag row for FRotator */
	void AddLinearRotatorRow(
		IDetailChildrenBuilder& ChildBuilder,
		TSharedRef<IPropertyHandle> RotatorHandle,
		const FText& DisplayName,
		float Delta,
		TAttribute<bool> IsEnabled = true);

	/** Write Euler angles to FQuat handle */
	void SetEulerToQuat(TSharedPtr<IPropertyHandle> QuatHandle, const FRotator& Euler);

	/** Create linear drag widget for FVector (for groups) */
	TSharedRef<SWidget> CreateLinearVectorWidget(
		TSharedRef<IPropertyHandle> VectorHandle,
		float Delta);

	/** Create linear drag widget for FRotator (for groups) */
	TSharedRef<SWidget> CreateLinearRotatorWidget(
		TSharedRef<IPropertyHandle> RotatorHandle,
		float Delta);

	/** Add linear drag row with reset button for FVector */
	void AddLinearVectorRowWithReset(
		IDetailChildrenBuilder& ChildBuilder,
		TSharedRef<IPropertyHandle> VectorHandle,
		const FText& DisplayName,
		float Delta,
		const FVector& DefaultValue,
		TAttribute<bool> IsEnabled = true);

	/** Add linear drag row with reset button for FRotator */
	void AddLinearRotatorRowWithReset(
		IDetailChildrenBuilder& ChildBuilder,
		TSharedRef<IPropertyHandle> RotatorHandle,
		const FText& DisplayName,
		float Delta,
		const FRotator& DefaultValue,
		TAttribute<bool> IsEnabled = true);

	/** Create linear drag widget with reset button for FVector (for groups) */
	TSharedRef<SWidget> CreateLinearVectorWidgetWithReset(
		TSharedRef<IPropertyHandle> VectorHandle,
		float Delta,
		const FVector& DefaultValue);

	/** Create linear drag widget with reset button for FRotator (for groups) */
	TSharedRef<SWidget> CreateLinearRotatorWidgetWithReset(
		TSharedRef<IPropertyHandle> RotatorHandle,
		float Delta,
		const FRotator& DefaultValue);

	/** Create reset button for FVector */
	TSharedRef<SWidget> CreateResetButton(
		TSharedRef<IPropertyHandle> VectorHandle,
		const FVector& DefaultValue);

	/** Create reset button for FRotator */
	TSharedRef<SWidget> CreateResetButton(
		TSharedRef<IPropertyHandle> RotatorHandle,
		const FRotator& DefaultValue);

	/** FVector widget with reset button (right-aligned) */
	TSharedRef<SWidget> CreateVectorWidgetWithResetButton(
		TSharedRef<IPropertyHandle> VectorHandle,
		float Delta,
		const FVector& DefaultValue);

	/** FRotator widget with reset button (right-aligned) */
	TSharedRef<SWidget> CreateRotatorWidgetWithResetButton(
		TSharedRef<IPropertyHandle> RotatorHandle,
		float Delta,
		const FRotator& DefaultValue);

	/** Cached property handle */
	TSharedPtr<IPropertyHandle> BoneNameHandle;

	/** Main property handle (for Asset access) */
	TSharedPtr<IPropertyHandle> MainPropertyHandle;

	/** Cached FQuat property handle (for Euler display) */
	TSharedPtr<IPropertyHandle> RingRotationHandle;
	TSharedPtr<IPropertyHandle> MeshRotationHandle;

	/** Bone tree root items */
	TArray<TSharedPtr<FBoneDropdownItem>> BoneTreeRoots;

	/** All bone items (for fast index-based access) */
	TArray<TSharedPtr<FBoneDropdownItem>> AllBoneItems;

	/** Filtered bone tree root items */
	TArray<TSharedPtr<FBoneDropdownItem>> FilteredBoneTreeRoots;

	/** Weighted bone indices cache */
	TSet<int32> WeightedBoneIndices;

	/** Search text */
	FString BoneSearchText;

	/** Bone TreeView widget reference (for refresh) */
	TSharedPtr<STreeView<TSharedPtr<FBoneDropdownItem>>> BoneTreeView;

	/** ComboButton widget reference (for closing) */
	TSharedPtr<SComboButton> BoneComboButton;

	/** Get TargetSkeletalMesh from Asset */
	class USkeletalMesh* GetTargetSkeletalMesh() const;

	/** Get outer Asset */
	class UFleshRingAsset* GetOuterAsset() const;

	/** Select Ring when header clicked */
	FReply OnHeaderClicked(int32 RingIndex);

	/** Select Ring when header clicked (void version, for FSimpleDelegate) */
	void OnHeaderClickedVoid();

	/** Get Ring name for display */
	FText GetDisplayRingName(int32 Index) const;

	/** Called when Ring name is committed */
	void OnRingNameCommitted(const FText& NewText, ETextCommit::Type CommitType);

	/** Check if this Ring is currently selected */
	bool IsThisRingSelected() const;

	/** Return header background color (highlighted based on selection state) */
	FSlateColor GetHeaderBackgroundColor() const;

	/** Return visibility icon (based on bEditorVisible state) */
	const FSlateBrush* GetVisibilityIcon() const;

	/** Visibility toggle button click handler */
	FReply OnVisibilityToggleClicked();

	/** Cached array index */
	int32 CachedArrayIndex = INDEX_NONE;

	/** Ring name inline edit widget reference */
	TSharedPtr<class SRingNameWidget> RingNameWidget;

	// === MeshScale ratio lock feature ===

	/** Create MeshScale ratio lock widget (lock button + vector widget) */
	TSharedRef<SWidget> CreateMeshScaleWidget(TSharedRef<IPropertyHandle> VectorHandle, float Delta);

	/** MeshScale lock button click handler */
	FReply OnMeshScaleLockClicked();

	/** MeshScale ratio lock state (true: maintain ratio) */
	bool bMeshScaleLocked = false;

	/** Cached MeshScale property handle (for ratio calculation) */
	TSharedPtr<IPropertyHandle> MeshScaleHandle;
};
