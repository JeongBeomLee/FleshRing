// Copyright 2026 LgThx. All Rights Reserved.

#include "SFleshRingSkeletonTree.h"
#include "FleshRingAsset.h"
#include "FleshRingTypes.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Widgets/Views/STableRow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Styling/SlateStyleRegistry.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Engine/StaticMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Views/SExpanderArrow.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "FleshRingSkeletonTree"

/** Ring rename delegate */
DECLARE_DELEGATE_TwoParams(FOnRingRenamed, int32 /*RingIndex*/, FName /*NewName*/);

/** Ring move delegate (preserve world position on Shift drag) */
DECLARE_DELEGATE_ThreeParams(FOnRingMoved, int32 /*RingIndex*/, FName /*NewBoneName*/, bool /*bPreserveWorldPosition*/);

/** Ring duplicate delegate (Alt drag) */
DECLARE_DELEGATE_TwoParams(FOnRingDuplicated, int32 /*SourceRingIndex*/, FName /*TargetBoneName*/);

/**
 * FleshRing tree row widget (SExpanderArrow + Wires support)
 */
class SFleshRingTreeRow : public STableRow<TSharedPtr<FFleshRingTreeItem>>
{
public:
	SLATE_BEGIN_ARGS(SFleshRingTreeRow) {}
		SLATE_ARGUMENT(TSharedPtr<FFleshRingTreeItem>, Item)
		SLATE_ARGUMENT(FText, HighlightText)
		SLATE_ARGUMENT(int32, RowIndex)
		SLATE_ARGUMENT(UFleshRingAsset*, Asset)
		SLATE_EVENT(FOnRingRenamed, OnRingRenamed)
		SLATE_EVENT(FOnRingMoved, OnRingMoved)
		SLATE_EVENT(FOnRingDuplicated, OnRingDuplicated)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
	{
		Item = InArgs._Item;
		HighlightText = InArgs._HighlightText;
		RowIndex = InArgs._RowIndex;
		Asset = InArgs._Asset;
		OnRingRenamed = InArgs._OnRingRenamed;
		OnRingMoved = InArgs._OnRingMoved;
		OnRingDuplicated = InArgs._OnRingDuplicated;

		// Save original name (for restoration on validation failure)
		if (Item.IsValid())
		{
			OriginalName = Item->GetDisplayName().ToString();
		}

		// Determine icon, color, and tooltip
		const FSlateBrush* IconBrush = nullptr;
		FSlateColor TextColor = FSlateColor::UseForeground();
		FSlateColor IconColor = FSlateColor::UseForeground();
		FText TooltipText;
		bool bIsRing = (Item->ItemType == EFleshRingTreeItemType::Ring);

		if (bIsRing)
		{
			IconBrush = FSlateStyleRegistry::FindSlateStyle("FleshRingStyle")->GetBrush("FleshRing.RingIcon");
			IconColor = FSlateColor(FLinearColor(1.0f, 0.3f, 0.3f));
			TextColor = FSlateColor(FLinearColor(1.0f, 0.6f, 0.2f));
			TooltipText = FText::Format(
				LOCTEXT("RingTooltip",
					"Ring attached to bone: {0}\nDouble-click to rename\n\n"
					"Hold Alt while dragging to duplicate the Ring.\n"
					"Hold Shift while dragging to preserve absolute position."),
				FText::FromName(Item->BoneName));
		}
		else
		{
			if (Item->bIsMeshBone)
			{
				// Actual mesh bone: filled bone icon
				IconBrush = FAppStyle::GetBrush("SkeletonTree.Bone");
				TooltipText = LOCTEXT("WeightedBoneTooltip", "This bone or one of its descendants has vertices weighted to it.\nRight-click to add a Ring.");
			}
			else
			{
				// Non-weighted bone: disabled style (empty bone icon)
				IconBrush = FAppStyle::GetBrush("SkeletonTree.BoneNonWeighted");
				TextColor = FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f));
				IconColor = FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f));
				TooltipText = LOCTEXT("NonWeightedBoneTooltip", "This bone has no vertices weighted to it or its descendants.\nCannot add Ring to this bone.");
			}
		}

		// Odd/even row background color (Persona style)
		FLinearColor RowBgColor = (RowIndex % 2 == 0)
			? FLinearColor(0.0f, 0.0f, 0.0f, 0.0f)      // Even: transparent
			: FLinearColor(1.0f, 1.0f, 1.0f, 0.03f);    // Odd: slightly brighter

		// Configure STableRow (without Content - handle directly in ConstructChildren)
		STableRow<TSharedPtr<FFleshRingTreeItem>>::Construct(
			STableRow<TSharedPtr<FFleshRingTreeItem>>::FArguments()
			.Padding(FMargin(0, 0)),
			InOwnerTable
		);

		// Create name widget (Ring supports inline editing)
		TSharedRef<SWidget> NameWidget = bIsRing
			? CreateRingNameWidget(TextColor, TooltipText)
			: CreateBoneNameWidget(TextColor, TooltipText);

		// Set our Content directly instead of default expander
		// Place SExpanderArrow at outermost position so wires are drawn at full row height
		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(RowBgColor)
			.Padding(0)
			[
				SNew(SHorizontalBox)
				// Expander Arrow (takes full row height)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Fill)
				[
					SNew(SExpanderArrow, SharedThis(this))
					.ShouldDrawWires(true)
				]
				// Icon + Text (with padding)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(0, 2)  // Vertical padding
				[
					SNew(SHorizontalBox)
					// Icon
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0, 0, 6, 0)
					.VAlign(VAlign_Center)
					[
						SNew(SImage)
						.Image(IconBrush)
						.ColorAndOpacity(IconColor)
						.DesiredSizeOverride(bIsRing ? FVector2D(12, 12) : FVector2D(18, 18))
					]
					// Name
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						NameWidget
					]
					// Eye icon (visibility toggle) - only shown for Ring
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(4, 0, 0, 0)
					.VAlign(VAlign_Center)
					[
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "NoBorder")
						.ContentPadding(FMargin(2))
						.OnClicked(this, &SFleshRingTreeRow::OnVisibilityToggleClicked)
						.ToolTipText(LOCTEXT("ToggleVisibility", "Toggle ring visibility"))
						.Visibility(bIsRing ? EVisibility::Visible : EVisibility::Collapsed)
						[
							SNew(SImage)
							.Image(this, &SFleshRingTreeRow::GetVisibilityIcon)
							.DesiredSizeOverride(FVector2D(14, 14))
						]
					]
				]
			]
		];
	}

	/** Enter editing mode */
	void EnterEditingMode()
	{
		// Save original name at edit start (for restoration on validation failure)
		if (Item.IsValid())
		{
			OriginalName = Item->GetDisplayName().ToString();
		}
		bIsEnterPressed = false;

		if (InlineTextBlock.IsValid())
		{
			InlineTextBlock->EnterEditingMode();
		}
	}

	virtual FReply OnPreviewKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override
	{
		// Detect Enter key (to revert to previous name in OnVerifyRingNameChanged)
		if (InKeyEvent.GetKey() == EKeys::Enter)
		{
			bIsEnterPressed = true;
		}
		return STableRow<TSharedPtr<FFleshRingTreeItem>>::OnPreviewKeyDown(MyGeometry, InKeyEvent);
	}

	// === Drag and Drop ===

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		// Call parent class first (handle selection)
		FReply Reply = STableRow<TSharedPtr<FFleshRingTreeItem>>::OnMouseButtonDown(MyGeometry, MouseEvent);

		// Prepare drag detection on left button click for Ring item (after selection)
		if (Item.IsValid() && Item->ItemType == EFleshRingTreeItemType::Ring && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			Reply = Reply.DetectDrag(SharedThis(this), EKeys::LeftMouseButton);
		}
		return Reply;
	}

	virtual FReply OnDragDetected(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		// Start Ring item drag
		if (Item.IsValid() && Item->ItemType == EFleshRingTreeItemType::Ring)
		{
			FString RingName = Item->GetDisplayName().ToString();
			TSharedRef<FFleshRingDragDropOp> DragOp = FFleshRingDragDropOp::New(
				Item->RingIndex,
				RingName,
				Item->BoneName,
				Item->EditingAsset.Get(),
				MouseEvent.GetModifierKeys()  // Capture modifier key state
			);
			return FReply::Handled().BeginDragDrop(DragOp);
		}
		return FReply::Unhandled();
	}

	virtual void OnDragEnter(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override
	{
		TSharedPtr<FFleshRingDragDropOp> DragOp = DragDropEvent.GetOperationAs<FFleshRingDragDropOp>();
		if (DragOp.IsValid() && Item.IsValid() && Item->ItemType == EFleshRingTreeItemType::Bone)
		{
			bool bIsDifferentBone = (Item->BoneName != DragOp->SourceBoneName);

			// Drop conditions:
			// 1. bIsMeshBone = this or a descendant has weighted vertices
			// 2. Different bone, or Alt drag (duplicate) allows same bone
			bool bCanDrop = Item->bIsMeshBone && (bIsDifferentBone || DragOp->IsAltDrag());

			DragOp->bCanDrop = bCanDrop;
			DragOp->SetIcon(FAppStyle::GetBrush(bCanDrop
				? TEXT("Graph.ConnectorFeedback.OK")
				: TEXT("Graph.ConnectorFeedback.Error")));
		}
	}

	virtual void OnDragLeave(const FDragDropEvent& DragDropEvent) override
	{
		TSharedPtr<FFleshRingDragDropOp> DragOp = DragDropEvent.GetOperationAs<FFleshRingDragDropOp>();
		if (DragOp.IsValid())
		{
			DragOp->bCanDrop = false;
			DragOp->SetIcon(FAppStyle::GetBrush(TEXT("Graph.ConnectorFeedback.Error")));
		}
	}

	virtual FReply OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override
	{
		TSharedPtr<FFleshRingDragDropOp> DragOp = DragDropEvent.GetOperationAs<FFleshRingDragDropOp>();
		if (DragOp.IsValid() && Item.IsValid() && Item->ItemType == EFleshRingTreeItemType::Bone)
		{
			if (DragOp->bCanDrop)
			{
				return FReply::Handled();
			}
		}
		return FReply::Unhandled();
	}

	virtual FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override
	{
		TSharedPtr<FFleshRingDragDropOp> DragOp = DragDropEvent.GetOperationAs<FFleshRingDragDropOp>();
		if (DragOp.IsValid() && DragOp->bCanDrop && Item.IsValid() && Item->ItemType == EFleshRingTreeItemType::Bone)
		{
			if (DragOp->IsAltDrag())
			{
				// Alt+drag: Ring duplicate
				if (OnRingDuplicated.IsBound())
				{
					OnRingDuplicated.Execute(DragOp->RingIndex, Item->BoneName);
				}
			}
			else
			{
				// Normal/Shift+drag: Ring move
				if (OnRingMoved.IsBound())
				{
					bool bPreserveWorldPosition = DragOp->IsShiftDrag();
					OnRingMoved.Execute(DragOp->RingIndex, Item->BoneName, bPreserveWorldPosition);
				}
			}
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

private:
	/** Create Ring name widget (inline editing with validation) */
	TSharedRef<SWidget> CreateRingNameWidget(FSlateColor TextColor, FText TooltipText)
	{
		return SAssignNew(ValidationBorder, SBorder)
			.BorderImage(FAppStyle::GetBrush("NoBorder"))
			.Padding(0)
			[
				SAssignNew(InlineTextBlock, SInlineEditableTextBlock)
				.Text(Item->GetDisplayName())
				.ColorAndOpacity(TextColor)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
				.HighlightText(HighlightText)
				.ToolTipText(TooltipText)
				.IsSelected(this, &SFleshRingTreeRow::IsSelectedExclusively)
				.OnVerifyTextChanged(this, &SFleshRingTreeRow::OnVerifyRingNameChanged)
				.OnTextCommitted(this, &SFleshRingTreeRow::OnRingNameCommitted)
			];
	}

	/** Create Bone name widget (read-only) */
	TSharedRef<SWidget> CreateBoneNameWidget(FSlateColor TextColor, FText TooltipText)
	{
		return SNew(STextBlock)
			.Text(Item->GetDisplayName())
			.ColorAndOpacity(TextColor)
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
			.HighlightText(HighlightText)
			.ToolTipText(TooltipText);
	}

	/** Ring name validation (empty name/duplicate check) */
	bool OnVerifyRingNameChanged(const FText& NewText, FText& OutErrorMessage)
	{
		if (!Asset || !Item.IsValid())
		{
			bIsEnterPressed = false;
			return true;
		}

		FName NewName = FName(*NewText.ToString());
		bool bIsValid = true;

		// Empty name check
		if (NewName.IsNone())
		{
			OutErrorMessage = LOCTEXT("EmptyNameError", "Name cannot be empty.");
			bIsValid = false;
		}
		// Duplicate name check
		else if (!Asset->IsRingNameUnique(NewName, Item->RingIndex))
		{
			OutErrorMessage = LOCTEXT("DuplicateNameError", "This name is already in use. Please choose a different name.");
			bIsValid = false;
		}

		if (!bIsValid)
		{
			bIsNameValid = false;

			// Revert to previous name only on Enter
			if (bIsEnterPressed && InlineTextBlock.IsValid())
			{
				InlineTextBlock->SetText(FText::FromString(OriginalName));
			}
			bIsEnterPressed = false;
			return false;  // Stay in edit mode
		}

		bIsNameValid = true;
		bIsEnterPressed = false;
		return true;
	}

	/** Ring name commit */
	void OnRingNameCommitted(const FText& NewText, ETextCommit::Type CommitType)
	{
		// Reset validation border
		if (ValidationBorder.IsValid())
		{
			ValidationBorder->SetBorderImage(FAppStyle::GetBrush("NoBorder"));
		}

		if (CommitType == ETextCommit::OnEnter)
		{
			// Confirm with Enter: apply only valid names
			if (bIsNameValid && Item.IsValid() && OnRingRenamed.IsBound())
			{
				OnRingRenamed.Execute(Item->RingIndex, FName(*NewText.ToString()));
			}
		}
		else if (CommitType == ETextCommit::OnUserMovedFocus)
		{
			// Focus moved: apply if valid, restore original name if invalid
			if (bIsNameValid && Item.IsValid() && OnRingRenamed.IsBound())
			{
				OnRingRenamed.Execute(Item->RingIndex, FName(*NewText.ToString()));
			}
			// If invalid, InlineTextBlock auto-restores to original text
		}

		bIsNameValid = true;  // Reset state
	}

	/** Return visibility icon (based on bEditorVisible state) */
	const FSlateBrush* GetVisibilityIcon() const
	{
		if (Asset && Item.IsValid() && Asset->Rings.IsValidIndex(Item->RingIndex))
		{
			bool bVisible = Asset->Rings[Item->RingIndex].bEditorVisible;
			return FAppStyle::GetBrush(bVisible ? "Icons.Visible" : "Icons.Hidden");
		}
		return FAppStyle::GetBrush("Icons.Visible");
	}

	/** Visibility toggle button click */
	FReply OnVisibilityToggleClicked()
	{
		if (Asset && Item.IsValid() && Asset->Rings.IsValidIndex(Item->RingIndex))
		{
			FScopedTransaction Transaction(LOCTEXT("ToggleRingVisibility", "Toggle Ring Visibility"));
			Asset->Modify();

			Asset->Rings[Item->RingIndex].bEditorVisible = !Asset->Rings[Item->RingIndex].bEditorVisible;

			// Notify Asset change (for editor viewport refresh)
			Asset->OnAssetChanged.Broadcast(Asset);
		}
		return FReply::Handled();
	}

	TSharedPtr<FFleshRingTreeItem> Item;
	FText HighlightText;
	int32 RowIndex = 0;
	UFleshRingAsset* Asset = nullptr;
	FOnRingRenamed OnRingRenamed;
	FOnRingMoved OnRingMoved;
	FOnRingDuplicated OnRingDuplicated;
	TSharedPtr<SInlineEditableTextBlock> InlineTextBlock;
	TSharedPtr<SBorder> ValidationBorder;
	FString OriginalName;
	bool bIsNameValid = true;
	bool bIsEnterPressed = false;	// Enter key detection flag
};

#undef LOCTEXT_NAMESPACE
#define LOCTEXT_NAMESPACE "FleshRingSkeletonTree"

//////////////////////////////////////////////////////////////////////////
// FFleshRingTreeItem

FText FFleshRingTreeItem::GetDisplayName() const
{
	if (ItemType == EFleshRingTreeItemType::Ring)
	{
		// Custom Ring name or default name (FleshRing_index)
		if (UFleshRingAsset* Asset = EditingAsset.Get())
		{
			if (Asset->Rings.IsValidIndex(RingIndex))
			{
				return FText::FromString(Asset->Rings[RingIndex].GetDisplayName(RingIndex));
			}
		}
		return FText::Format(LOCTEXT("RingDisplayName", "FleshRing_{0}"), FText::AsNumber(RingIndex));
	}
	return FText::FromName(BoneName);
}

TSharedPtr<FFleshRingTreeItem> FFleshRingTreeItem::CreateBone(FName InBoneName, int32 InBoneIndex)
{
	TSharedPtr<FFleshRingTreeItem> Item = MakeShared<FFleshRingTreeItem>();
	Item->ItemType = EFleshRingTreeItemType::Bone;
	Item->BoneName = InBoneName;
	Item->BoneIndex = InBoneIndex;
	return Item;
}

TSharedPtr<FFleshRingTreeItem> FFleshRingTreeItem::CreateRing(FName InBoneName, int32 InRingIndex, UFleshRingAsset* InAsset)
{
	TSharedPtr<FFleshRingTreeItem> Item = MakeShared<FFleshRingTreeItem>();
	Item->ItemType = EFleshRingTreeItemType::Ring;
	Item->BoneName = InBoneName;
	Item->RingIndex = InRingIndex;
	Item->EditingAsset = InAsset;
	return Item;
}

//////////////////////////////////////////////////////////////////////////
// FFleshRingDragDropOp

TSharedRef<FFleshRingDragDropOp> FFleshRingDragDropOp::New(int32 InRingIndex, const FString& InRingName, FName InBoneName, UFleshRingAsset* InAsset, FModifierKeysState InModifierKeys)
{
	TSharedRef<FFleshRingDragDropOp> Operation = MakeShareable(new FFleshRingDragDropOp());
	Operation->RingIndex = InRingIndex;
	Operation->RingName = InRingName;
	Operation->SourceBoneName = InBoneName;
	Operation->Asset = InAsset;
	Operation->bCanDrop = false;
	Operation->ModifierKeysState = InModifierKeys;
	// Default icon: red (drop not allowed)
	Operation->SetIcon(FAppStyle::GetBrush(TEXT("Graph.ConnectorFeedback.Error")));
	Operation->Construct();
	return Operation;
}

TSharedPtr<SWidget> FFleshRingDragDropOp::GetDefaultDecorator() const
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Graph.ConnectorFeedback.Border"))
		.Content()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SImage)
				.Image(this, &FFleshRingDragDropOp::GetIcon)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(STextBlock)
				.Text(FText::Format(LOCTEXT("DragRingLabel", "FleshRing {0}"), FText::FromString(RingName)))
			]
		];
}

//////////////////////////////////////////////////////////////////////////
// SFleshRingSkeletonTree

void SFleshRingSkeletonTree::Construct(const FArguments& InArgs)
{
	EditingAsset = InArgs._Asset;
	OnBoneSelected = InArgs._OnBoneSelected;
	OnRingSelected = InArgs._OnRingSelected;
	OnAddRingRequested = InArgs._OnAddRingRequested;
	OnFocusCameraRequested = InArgs._OnFocusCameraRequested;
	OnRingDeleted = InArgs._OnRingDeleted;

	// Subscribe to asset change delegate (refresh tree when name changes in detail panel)
	if (UFleshRingAsset* Asset = EditingAsset.Get())
	{
		Asset->OnAssetChanged.AddSP(this, &SFleshRingSkeletonTree::OnAssetChangedHandler);
	}

	BuildTree();

	ChildSlot
	[
		SNew(SVerticalBox)
		// Top toolbar
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4)
		[
			CreateToolbar()
		]
		// Separator
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SSeparator)
		]
		// Tree view
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(TreeView, STreeView<TSharedPtr<FFleshRingTreeItem>>)
			.TreeItemsSource(&FilteredRootItems)
			.OnGenerateRow(this, &SFleshRingSkeletonTree::GenerateTreeRow)
			.OnGetChildren(this, &SFleshRingSkeletonTree::GetChildrenForTree)
			.OnSelectionChanged(this, &SFleshRingSkeletonTree::OnTreeSelectionChanged)
			.OnMouseButtonDoubleClick(this, &SFleshRingSkeletonTree::OnTreeDoubleClick)
			.OnContextMenuOpening(this, &SFleshRingSkeletonTree::CreateContextMenu)
			.OnExpansionChanged(this, &SFleshRingSkeletonTree::OnTreeExpansionChanged)
			.SelectionMode(ESelectionMode::Single)
		]
	];

	ApplyFilter();
}

TSharedRef<SWidget> SFleshRingSkeletonTree::CreateToolbar()
{
	return SNew(SHorizontalBox)
		// + button (Add Ring)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(0, 0, 4, 0)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.OnClicked(this, &SFleshRingSkeletonTree::OnAddButtonClicked)
			.ToolTipText(LOCTEXT("AddRingTooltip", "Add Ring to selected bone"))
			.IsEnabled(this, &SFleshRingSkeletonTree::CanAddRing)
			.ContentPadding(FMargin(2))
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Plus"))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		]
		// Search box
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 4, 0)
		[
			SAssignNew(SearchBox, SSearchBox)
			.HintText(LOCTEXT("SearchHint", "Search skeleton tree..."))
			.OnTextChanged(this, &SFleshRingSkeletonTree::OnSearchTextChanged)
		]
		// Filter button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		[
			SNew(SComboButton)
			.HasDownArrow(false)
			.ContentPadding(FMargin(2))
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.OnGetMenuContent(this, &SFleshRingSkeletonTree::CreateFilterMenu)
			.ButtonContent()
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Filter"))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
}

TSharedRef<SWidget> SFleshRingSkeletonTree::CreateFilterMenu()
{
	FMenuBuilder MenuBuilder(true, nullptr);

	MenuBuilder.BeginSection("BoneFilter", LOCTEXT("BoneFilterSection", "Bone Filter"));
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("ShowAllBones", "Show All Bones"),
			LOCTEXT("ShowAllBonesTooltip", "Show all bones in the skeleton"),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &SFleshRingSkeletonTree::OnShowAllBones),
				FCanExecuteAction(),
				FIsActionChecked::CreateSP(this, &SFleshRingSkeletonTree::IsShowAllBonesChecked)
			),
			NAME_None,
			EUserInterfaceActionType::RadioButton
		);

		MenuBuilder.AddMenuEntry(
			LOCTEXT("ShowMeshBonesOnly", "Mesh Bones Only"),
			LOCTEXT("ShowMeshBonesOnlyTooltip", "Hide IK and virtual bones"),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &SFleshRingSkeletonTree::OnShowMeshBonesOnly),
				FCanExecuteAction(),
				FIsActionChecked::CreateSP(this, &SFleshRingSkeletonTree::IsShowMeshBonesOnlyChecked)
			),
			NAME_None,
			EUserInterfaceActionType::RadioButton
		);

		MenuBuilder.AddMenuEntry(
			LOCTEXT("ShowBonesWithRings", "Bones with Rings Only"),
			LOCTEXT("ShowBonesWithRingsTooltip", "Show only bones that have rings attached"),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &SFleshRingSkeletonTree::OnShowBonesWithRingsOnly),
				FCanExecuteAction(),
				FIsActionChecked::CreateSP(this, &SFleshRingSkeletonTree::IsShowBonesWithRingsOnlyChecked)
			),
			NAME_None,
			EUserInterfaceActionType::RadioButton
		);
	}
	MenuBuilder.EndSection();

	return MenuBuilder.MakeWidget();
}

TSharedPtr<SWidget> SFleshRingSkeletonTree::CreateContextMenu()
{
	FMenuBuilder MenuBuilder(true, nullptr);

	// Get currently selected item directly from TreeView (resolves timing issue)
	// OnContextMenuOpening may be called before OnSelectionChanged, so
	// get directly from TreeView instead of using SelectedItem member variable
	TArray<TSharedPtr<FFleshRingTreeItem>> SelectedItems = TreeView->GetSelectedItems();
	TSharedPtr<FFleshRingTreeItem> CurrentItem = SelectedItems.Num() > 0 ? SelectedItems[0] : nullptr;

	// When bone is selected
	if (CurrentItem.IsValid() && CurrentItem->ItemType == EFleshRingTreeItemType::Bone)
	{
		// Sync SelectedItem (used in CanAddRing() etc.)
		SelectedItem = CurrentItem;

		MenuBuilder.BeginSection("BoneActions", LOCTEXT("BoneActionsSection", "Bone"));
		{
			MenuBuilder.AddMenuEntry(
				LOCTEXT("AddRing", "Add Ring"),
				LOCTEXT("AddRingTooltip", "Add a ring to this bone"),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Plus"),
				FUIAction(
					FExecuteAction::CreateSP(this, &SFleshRingSkeletonTree::OnContextMenuAddRing),
					FCanExecuteAction::CreateSP(this, &SFleshRingSkeletonTree::CanAddRing)
				)
			);

			MenuBuilder.AddMenuEntry(
				LOCTEXT("CopyBoneName", "Copy Bone Name"),
				LOCTEXT("CopyBoneNameTooltip", "Copy the bone name to clipboard"),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy"),
				FUIAction(
					FExecuteAction::CreateSP(this, &SFleshRingSkeletonTree::OnContextMenuCopyBoneName)
				)
			);

			// Paste Ring (only when a copied Ring exists)
			if (CanPasteRing())
			{
				MenuBuilder.AddSeparator();

				// Paste ring (to original bone)
				FMenuEntryParams PasteParams;
				PasteParams.LabelOverride = LOCTEXT("PasteRing", "Paste Ring");
				PasteParams.ToolTipOverride = LOCTEXT("PasteRingTooltip", "Paste ring to original bone");
				PasteParams.IconOverride = FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Paste");
				PasteParams.DirectActions = FUIAction(
					FExecuteAction::CreateSP(this, &SFleshRingSkeletonTree::OnContextMenuPasteRing)
				);
				PasteParams.InputBindingOverride = FText::FromString(TEXT("Ctrl+V"));
				MenuBuilder.AddMenuEntry(PasteParams);

				// Paste ring to selected bone (only available for mesh bones)
				FMenuEntryParams PasteToSelectedParams;
				PasteToSelectedParams.LabelOverride = LOCTEXT("PasteRingToSelectedBone", "Paste Ring to Selected Bone");
				PasteToSelectedParams.ToolTipOverride = LOCTEXT("PasteRingToSelectedBoneTooltip", "Paste ring to currently selected bone");
				PasteToSelectedParams.IconOverride = FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Paste");
				PasteToSelectedParams.DirectActions = FUIAction(
					FExecuteAction::CreateSP(this, &SFleshRingSkeletonTree::OnContextMenuPasteRingToSelectedBone),
					FCanExecuteAction::CreateSP(this, &SFleshRingSkeletonTree::CanPasteRingToSelectedBone)
				);
				PasteToSelectedParams.InputBindingOverride = FText::FromString(TEXT("Ctrl+Shift+V"));
				MenuBuilder.AddMenuEntry(PasteToSelectedParams);
			}
		}
		MenuBuilder.EndSection();
	}
	// When Ring is selected
	else if (CurrentItem.IsValid() && CurrentItem->ItemType == EFleshRingTreeItemType::Ring)
	{
		SelectedItem = CurrentItem;

		MenuBuilder.BeginSection("RingActions", LOCTEXT("RingActionsSection", "Ring"));
		{
			// Copy Ring
			FMenuEntryParams CopyParams;
			CopyParams.LabelOverride = LOCTEXT("CopyRing", "Copy Ring");
			CopyParams.ToolTipOverride = LOCTEXT("CopyRingTooltip", "Copy this ring");
			CopyParams.IconOverride = FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Copy");
			CopyParams.DirectActions = FUIAction(
				FExecuteAction::CreateSP(this, &SFleshRingSkeletonTree::OnContextMenuCopyRing),
				FCanExecuteAction::CreateSP(this, &SFleshRingSkeletonTree::CanCopyRing)
			);
			CopyParams.InputBindingOverride = FText::FromString(TEXT("Ctrl+C"));
			MenuBuilder.AddMenuEntry(CopyParams);

			// Rename Ring (icon + shortcut hint)
			FMenuEntryParams RenameParams;
			RenameParams.LabelOverride = LOCTEXT("RenameRing", "Rename Ring");
			RenameParams.ToolTipOverride = LOCTEXT("RenameRingTooltip", "Rename this ring");
			RenameParams.IconOverride = FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Rename");
			RenameParams.DirectActions = FUIAction(
				FExecuteAction::CreateSP(this, &SFleshRingSkeletonTree::OnContextMenuRenameRing)
			);
			RenameParams.InputBindingOverride = FText::FromString(TEXT("F2"));
			MenuBuilder.AddMenuEntry(RenameParams);

			// Delete Ring
			MenuBuilder.AddMenuEntry(
				LOCTEXT("DeleteRing", "Delete Ring"),
				LOCTEXT("DeleteRingTooltip", "Delete this ring"),
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Delete"),
				FUIAction(
					FExecuteAction::CreateSP(this, &SFleshRingSkeletonTree::OnContextMenuDeleteRing),
					FCanExecuteAction::CreateSP(this, &SFleshRingSkeletonTree::CanDeleteRing)
				)
			);
		}
		MenuBuilder.EndSection();
	}

	return MenuBuilder.MakeWidget();
}

FReply SFleshRingSkeletonTree::OnAddButtonClicked()
{
	OnContextMenuAddRing();
	return FReply::Handled();
}

void SFleshRingSkeletonTree::OnSearchTextChanged(const FText& NewText)
{
	SearchText = NewText.ToString();
	ApplyFilter();
}

void SFleshRingSkeletonTree::ApplyFilter()
{
	FilteredRootItems.Empty();
	RowIndexCounter = 0;  // Reset row index counter

	// Apply filter recursively
	TFunction<bool(TSharedPtr<FFleshRingTreeItem>)> FilterItem = [&](TSharedPtr<FFleshRingTreeItem> Item) -> bool
	{
		if (!Item.IsValid())
		{
			return false;
		}

		bool bPassesFilter = true;

		if (Item->ItemType == EFleshRingTreeItemType::Bone)
		{
			// Check bone filter mode
			switch (BoneFilterMode)
			{
			case EBoneFilterMode::ShowMeshBonesOnly:
				bPassesFilter = Item->bIsMeshBone;
				break;
			case EBoneFilterMode::ShowBonesWithRingsOnly:
				{
					// Check if this bone has a Ring
					bool bHasRing = false;
					for (const auto& Child : Item->Children)
					{
						if (Child->ItemType == EFleshRingTreeItemType::Ring)
						{
							bHasRing = true;
							break;
						}
					}
					bPassesFilter = bHasRing;
				}
				break;
			default:
				break;
			}

			// Check search text
			if (bPassesFilter && !SearchText.IsEmpty())
			{
				bPassesFilter = Item->BoneName.ToString().Contains(SearchText);
			}
		}
		else if (Item->ItemType == EFleshRingTreeItemType::Ring)
		{
			// Ring search: search by display name ("Ring [X]") or attached bone name
			if (!SearchText.IsEmpty())
			{
				FString DisplayName = Item->GetDisplayName().ToString();
				bPassesFilter = DisplayName.Contains(SearchText) || Item->BoneName.ToString().Contains(SearchText);
			}
		}

		// If any child passes the filter, show the parent as well
		bool bChildPasses = false;
		for (const auto& Child : Item->Children)
		{
			if (FilterItem(Child))
			{
				bChildPasses = true;
			}
		}

		Item->bIsFiltered = !(bPassesFilter || bChildPasses);
		return bPassesFilter || bChildPasses;
	};

	for (const auto& RootItem : RootItems)
	{
		if (FilterItem(RootItem))
		{
			FilteredRootItems.Add(RootItem);
		}
	}

	if (TreeView.IsValid())
	{
		// Rebuild rows completely with RebuildList (update highlight)
		TreeView->RebuildList();

		// Expand all items
		TFunction<void(TSharedPtr<FFleshRingTreeItem>)> ExpandAll = [&](TSharedPtr<FFleshRingTreeItem> Item)
		{
			TreeView->SetItemExpansion(Item, true);
			for (const auto& Child : Item->Children)
			{
				if (!Child->bIsFiltered)
				{
					ExpandAll(Child);
				}
			}
		};

		for (const auto& Root : FilteredRootItems)
		{
			ExpandAll(Root);
		}
	}
}

void SFleshRingSkeletonTree::OnShowAllBones()
{
	BoneFilterMode = EBoneFilterMode::ShowAll;
	ApplyFilter();
}

bool SFleshRingSkeletonTree::IsShowAllBonesChecked() const
{
	return BoneFilterMode == EBoneFilterMode::ShowAll;
}

void SFleshRingSkeletonTree::OnShowMeshBonesOnly()
{
	BoneFilterMode = EBoneFilterMode::ShowMeshBonesOnly;
	ApplyFilter();
}

bool SFleshRingSkeletonTree::IsShowMeshBonesOnlyChecked() const
{
	return BoneFilterMode == EBoneFilterMode::ShowMeshBonesOnly;
}

void SFleshRingSkeletonTree::OnShowBonesWithRingsOnly()
{
	BoneFilterMode = EBoneFilterMode::ShowBonesWithRingsOnly;
	ApplyFilter();
}

bool SFleshRingSkeletonTree::IsShowBonesWithRingsOnlyChecked() const
{
	return BoneFilterMode == EBoneFilterMode::ShowBonesWithRingsOnly;
}

void SFleshRingSkeletonTree::SetAsset(UFleshRingAsset* InAsset)
{
	// Unsubscribe from existing delegate
	if (UFleshRingAsset* OldAsset = EditingAsset.Get())
	{
		OldAsset->OnAssetChanged.RemoveAll(this);
	}

	EditingAsset = InAsset;

	// Subscribe to new asset's delegate (refresh tree when name changes in detail panel)
	if (InAsset)
	{
		InAsset->OnAssetChanged.AddSP(this, &SFleshRingSkeletonTree::OnAssetChangedHandler);
	}

	RefreshTree();
}

void SFleshRingSkeletonTree::OnAssetChangedHandler(UFleshRingAsset* Asset)
{
	// Refresh tree when Ring name is changed in detail panel
	if (TreeView.IsValid())
	{
		TreeView->RebuildList();
	}
}

void SFleshRingSkeletonTree::RefreshTree()
{
	// Save current expansion state
	SaveExpansionState();

	// Rebuild tree
	BuildTree();
	ApplyFilter();

	// Restore expansion state
	RestoreExpansionState();
}

void SFleshRingSkeletonTree::SaveExpansionState()
{
	if (!TreeView.IsValid())
	{
		return;
	}

	ExpandedBoneNames.Empty();

	TFunction<void(TSharedPtr<FFleshRingTreeItem>)> SaveRecursive = [&](TSharedPtr<FFleshRingTreeItem> Item)
	{
		if (Item.IsValid() && Item->ItemType == EFleshRingTreeItemType::Bone)
		{
			if (TreeView->IsItemExpanded(Item))
			{
				ExpandedBoneNames.Add(Item->BoneName);
			}
			for (const auto& Child : Item->Children)
			{
				SaveRecursive(Child);
			}
		}
	};

	for (const auto& Root : RootItems)
	{
		SaveRecursive(Root);
	}
}

void SFleshRingSkeletonTree::RestoreExpansionState()
{
	if (!TreeView.IsValid())
	{
		return;
	}

	TFunction<void(TSharedPtr<FFleshRingTreeItem>)> RestoreRecursive = [&](TSharedPtr<FFleshRingTreeItem> Item)
	{
		if (Item.IsValid() && Item->ItemType == EFleshRingTreeItemType::Bone)
		{
			bool bShouldExpand = ExpandedBoneNames.Contains(Item->BoneName);
			TreeView->SetItemExpansion(Item, bShouldExpand);

			for (const auto& Child : Item->Children)
			{
				RestoreRecursive(Child);
			}
		}
	};

	for (const auto& Root : FilteredRootItems)
	{
		RestoreRecursive(Root);
	}
}

void SFleshRingSkeletonTree::OnTreeExpansionChanged(TSharedPtr<FFleshRingTreeItem> Item, bool bIsExpanded)
{
	// Save expansion state immediately
	if (Item.IsValid() && Item->ItemType == EFleshRingTreeItemType::Bone)
	{
		if (bIsExpanded)
		{
			ExpandedBoneNames.Add(Item->BoneName);
		}
		else
		{
			ExpandedBoneNames.Remove(Item->BoneName);
		}
	}
}

void SFleshRingSkeletonTree::SelectBone(FName BoneName)
{
	if (TSharedPtr<FFleshRingTreeItem>* FoundItem = BoneItemMap.Find(BoneName))
	{
		SelectedItem = *FoundItem;

		if (TreeView.IsValid())
		{
			// Expand parent nodes
			TSharedPtr<FFleshRingTreeItem> Current = SelectedItem;
			while (Current.IsValid())
			{
				TreeView->SetItemExpansion(Current, true);
				Current = Current->Parent.Pin();
			}

			TreeView->SetSelection(SelectedItem);
			TreeView->RequestScrollIntoView(SelectedItem);
		}
	}
}

FName SFleshRingSkeletonTree::GetSelectedBoneName() const
{
	if (SelectedItem.IsValid())
	{
		return SelectedItem->BoneName;
	}
	return NAME_None;
}

void SFleshRingSkeletonTree::ClearSelection()
{
	SelectedItem = nullptr;
	if (TreeView.IsValid())
	{
		TreeView->ClearSelection();
	}
}

void SFleshRingSkeletonTree::SelectRingByIndex(int32 RingIndex)
{
	if (RingIndex < 0)
	{
		ClearSelection();
		return;
	}

	// Find Ring item
	TSharedPtr<FFleshRingTreeItem> FoundRingItem;
	TFunction<void(TSharedPtr<FFleshRingTreeItem>)> FindRingRecursive = [&](TSharedPtr<FFleshRingTreeItem> Item)
	{
		if (!Item.IsValid() || FoundRingItem.IsValid())
		{
			return;
		}

		if (Item->ItemType == EFleshRingTreeItemType::Ring && Item->RingIndex == RingIndex)
		{
			FoundRingItem = Item;
			return;
		}

		for (const auto& Child : Item->Children)
		{
			FindRingRecursive(Child);
		}
	};

	for (const auto& Root : RootItems)
	{
		FindRingRecursive(Root);
		if (FoundRingItem.IsValid())
		{
			break;
		}
	}

	if (FoundRingItem.IsValid())
	{
		SelectedItem = FoundRingItem;

		if (TreeView.IsValid())
		{
			// Expand parent nodes
			TSharedPtr<FFleshRingTreeItem> Current = FoundRingItem->Parent.Pin();
			while (Current.IsValid())
			{
				TreeView->SetItemExpansion(Current, true);
				Current = Current->Parent.Pin();
			}

			TreeView->SetSelection(FoundRingItem, ESelectInfo::Direct);
			TreeView->RequestScrollIntoView(FoundRingItem);
		}
	}
}

void SFleshRingSkeletonTree::BuildTree()
{
	RootItems.Empty();
	FilteredRootItems.Empty();
	BoneItemMap.Empty();

	UFleshRingAsset* Asset = EditingAsset.Get();
	if (!Asset)
	{
		return;
	}

	USkeletalMesh* SkelMesh = Asset->TargetSkeletalMesh.LoadSynchronous();
	if (!SkelMesh)
	{
		return;
	}

	const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
	const int32 NumBones = RefSkeleton.GetNum();

	if (NumBones == 0)
	{
		return;
	}

	// Build weighted bone cache
	BuildWeightedBoneCache(SkelMesh);

	// Recursive function to check if any descendant has weighted bones
	TFunction<bool(int32)> HasWeightedDescendant = [&](int32 BoneIndex) -> bool
	{
		if (IsBoneWeighted(BoneIndex))
		{
			return true;
		}
		// Check descendant bones
		for (int32 ChildIndex = 0; ChildIndex < NumBones; ++ChildIndex)
		{
			if (RefSkeleton.GetParentIndex(ChildIndex) == BoneIndex)
			{
				if (HasWeightedDescendant(ChildIndex))
				{
					return true;
				}
			}
		}
		return false;
	};

	// Create all bone items
	TArray<TSharedPtr<FFleshRingTreeItem>> AllBoneItems;
	AllBoneItems.SetNum(NumBones);

	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
		TSharedPtr<FFleshRingTreeItem> BoneItem = FFleshRingTreeItem::CreateBone(BoneName, BoneIndex);
		// Mark as mesh bone if self or any descendant has weighted bones
		BoneItem->bIsMeshBone = HasWeightedDescendant(BoneIndex);
		AllBoneItems[BoneIndex] = BoneItem;
		BoneItemMap.Add(BoneName, BoneItem);
	}

	// Set parent-child relationships
	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
		TSharedPtr<FFleshRingTreeItem> BoneItem = AllBoneItems[BoneIndex];

		if (ParentIndex == INDEX_NONE)
		{
			RootItems.Add(BoneItem);
		}
		else
		{
			TSharedPtr<FFleshRingTreeItem> ParentItem = AllBoneItems[ParentIndex];
			ParentItem->Children.Add(BoneItem);
			BoneItem->Parent = ParentItem;
		}
	}

	// Add Ring items
	UpdateRingItems();

	// Set depth and last child flags
	TFunction<void(TSharedPtr<FFleshRingTreeItem>, int32)> SetDepthRecursive = [&](TSharedPtr<FFleshRingTreeItem> Item, int32 CurrentDepth)
	{
		Item->Depth = CurrentDepth;
		for (int32 i = 0; i < Item->Children.Num(); ++i)
		{
			Item->Children[i]->bIsLastChild = (i == Item->Children.Num() - 1);
			SetDepthRecursive(Item->Children[i], CurrentDepth + 1);
		}
	};

	for (int32 i = 0; i < RootItems.Num(); ++i)
	{
		RootItems[i]->bIsLastChild = (i == RootItems.Num() - 1);
		SetDepthRecursive(RootItems[i], 0);
	}
}

void SFleshRingSkeletonTree::UpdateRingItems()
{
	UFleshRingAsset* Asset = EditingAsset.Get();
	if (!Asset)
	{
		return;
	}

	// Remove existing Ring items (from all bones)
	for (auto& Pair : BoneItemMap)
	{
		TSharedPtr<FFleshRingTreeItem> BoneItem = Pair.Value;
		BoneItem->Children.RemoveAll([](const TSharedPtr<FFleshRingTreeItem>& Child)
		{
			return Child->ItemType == EFleshRingTreeItemType::Ring;
		});
	}

	// Add Ring items
	for (int32 RingIndex = 0; RingIndex < Asset->Rings.Num(); ++RingIndex)
	{
		const FFleshRingSettings& Ring = Asset->Rings[RingIndex];

		if (TSharedPtr<FFleshRingTreeItem>* FoundBone = BoneItemMap.Find(Ring.BoneName))
		{
			TSharedPtr<FFleshRingTreeItem> RingItem = FFleshRingTreeItem::CreateRing(Ring.BoneName, RingIndex, Asset);
			RingItem->Parent = *FoundBone;

			// Add Ring before bone's children (at the front)
			(*FoundBone)->Children.Insert(RingItem, 0);
		}
	}
}

bool SFleshRingSkeletonTree::IsBoneWeighted(int32 BoneIndex) const
{
	return WeightedBoneIndices.Contains(BoneIndex);
}

void SFleshRingSkeletonTree::BuildWeightedBoneCache(USkeletalMesh* SkelMesh)
{
	WeightedBoneIndices.Empty();

	if (!SkelMesh)
	{
		return;
	}

	// Find weighted bones from LOD 0 render data
	FSkeletalMeshRenderData* RenderData = SkelMesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		return;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];

	// Bones in each section's BoneMap are the weighted bones
	for (const FSkelMeshRenderSection& Section : LODData.RenderSections)
	{
		for (FBoneIndexType BoneIndex : Section.BoneMap)
		{
			WeightedBoneIndices.Add(BoneIndex);
		}
	}
}

TSharedRef<ITableRow> SFleshRingSkeletonTree::GenerateTreeRow(TSharedPtr<FFleshRingTreeItem> Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	// Use SFleshRingTreeRow (supports SExpanderArrow + Wires)
	return SNew(SFleshRingTreeRow, OwnerTable)
		.Item(Item)
		.HighlightText(FText::FromString(SearchText))
		.RowIndex(RowIndexCounter++)
		.Asset(EditingAsset.Get())
		.OnRingRenamed(this, &SFleshRingSkeletonTree::HandleRingRenamed)
		.OnRingMoved(this, &SFleshRingSkeletonTree::MoveRingToBone)
		.OnRingDuplicated(this, &SFleshRingSkeletonTree::DuplicateRingToBone);
}

void SFleshRingSkeletonTree::GetChildrenForTree(TSharedPtr<FFleshRingTreeItem> Item, TArray<TSharedPtr<FFleshRingTreeItem>>& OutChildren)
{
	if (Item.IsValid())
	{
		for (const auto& Child : Item->Children)
		{
			if (!Child->bIsFiltered)
			{
				OutChildren.Add(Child);
			}
		}
	}
}

void SFleshRingSkeletonTree::OnTreeSelectionChanged(TSharedPtr<FFleshRingTreeItem> Item, ESelectInfo::Type SelectInfo)
{
	SelectedItem = Item;

	if (!Item.IsValid())
	{
		// Selection cleared
		if (OnBoneSelected.IsBound())
		{
			OnBoneSelected.Execute(NAME_None);
		}
		if (OnRingSelected.IsBound())
		{
			OnRingSelected.Execute(INDEX_NONE);
		}
		return;
	}

	if (Item->ItemType == EFleshRingTreeItemType::Ring)
	{
		// Ring selected (bone highlighting is handled inside OnRingSelected)
		if (OnRingSelected.IsBound())
		{
			OnRingSelected.Execute(Item->RingIndex);
		}
		// Don't call bone delegate (attached bone is auto-highlighted when Ring is selected)
	}
	else
	{
		// Bone selected
		if (OnBoneSelected.IsBound())
		{
			OnBoneSelected.Execute(Item->BoneName);
		}
		// Deselect Ring
		if (OnRingSelected.IsBound())
		{
			OnRingSelected.Execute(INDEX_NONE);
		}
	}
}

void SFleshRingSkeletonTree::OnTreeDoubleClick(TSharedPtr<FFleshRingTreeItem> Item)
{
	if (Item.IsValid() && TreeView.IsValid())
	{
		if (Item->ItemType == EFleshRingTreeItemType::Bone)
		{
			// Bone double-click: toggle expand/collapse
			bool bIsExpanded = TreeView->IsItemExpanded(Item);
			TreeView->SetItemExpansion(Item, !bIsExpanded);
		}
		else if (Item->ItemType == EFleshRingTreeItemType::Ring)
		{
			// Ring double-click: enter name editing mode
			TSharedPtr<ITableRow> RowWidget = TreeView->WidgetFromItem(Item);
			if (RowWidget.IsValid())
			{
				TSharedPtr<SFleshRingTreeRow> TreeRow = StaticCastSharedPtr<SFleshRingTreeRow>(RowWidget);
				if (TreeRow.IsValid())
				{
					TreeRow->EnterEditingMode();
				}
			}
		}
	}
}

void SFleshRingSkeletonTree::HandleRingRenamed(int32 RingIndex, FName NewName)
{
	if (UFleshRingAsset* Asset = EditingAsset.Get())
	{
		if (Asset->Rings.IsValidIndex(RingIndex))
		{
			// Apply directly since already validated in Row
			FScopedTransaction Transaction(LOCTEXT("RenameRingFromTree", "Rename Ring"));
			Asset->Modify();
			Asset->Rings[RingIndex].RingName = NewName;
			Asset->PostEditChange();

			// Update other UI like detail panel
			Asset->OnAssetChanged.Broadcast(Asset);

			// Refresh tree (rebuild rows with RebuildList to update names)
			if (TreeView.IsValid())
			{
				TreeView->RebuildList();
			}
		}
	}
}

TSharedPtr<FFleshRingTreeItem> SFleshRingSkeletonTree::FindItem(FName BoneName, const TArray<TSharedPtr<FFleshRingTreeItem>>& Items)
{
	for (const auto& Item : Items)
	{
		if (Item->BoneName == BoneName)
		{
			return Item;
		}

		TSharedPtr<FFleshRingTreeItem> Found = FindItem(BoneName, Item->Children);
		if (Found.IsValid())
		{
			return Found;
		}
	}

	return nullptr;
}

// === Context Menu Actions ===

void SFleshRingSkeletonTree::OnContextMenuAddRing()
{
	if (!CanAddRing() || !OnAddRingRequested.IsBound())
	{
		return;
	}

	// Save selected bone name (used in lambda)
	FName BoneNameToAdd = SelectedItem->BoneName;

	// Asset picker configuration
	FAssetPickerConfig AssetPickerConfig;
	AssetPickerConfig.Filter.ClassPaths.Add(UStaticMesh::StaticClass()->GetClassPathName());
	AssetPickerConfig.Filter.bRecursiveClasses = true;
	AssetPickerConfig.SelectionMode = ESelectionMode::Single;
	AssetPickerConfig.bAllowNullSelection = false;  // Disabled since we handle this with button
	AssetPickerConfig.bFocusSearchBoxWhenOpened = true;
	AssetPickerConfig.InitialAssetViewType = EAssetViewType::List;

	// Callback when mesh is selected
	AssetPickerConfig.OnAssetSelected = FOnAssetSelected::CreateLambda(
		[this, BoneNameToAdd](const FAssetData& AssetData)
		{
			// Close popup
			FSlateApplication::Get().DismissAllMenus();

			UStaticMesh* SelectedMesh = nullptr;
			if (AssetData.IsValid())
			{
				SelectedMesh = Cast<UStaticMesh>(AssetData.GetAsset());
			}

			// Request Ring addition
			OnAddRingRequested.ExecuteIfBound(BoneNameToAdd, SelectedMesh);
		}
	);

	// Show asset picker popup
	FContentBrowserModule& ContentBrowserModule =
		FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

	TSharedRef<SWidget> AssetPickerWidget = ContentBrowserModule.Get().CreateAssetPicker(AssetPickerConfig);

	// Popup with bottom button bar (dialog style)
	FSlateApplication::Get().PushMenu(
		AsShared(),
		FWidgetPath(),
		SNew(SBox)
			.WidthOverride(400.0f)
			.HeightOverride(500.0f)
			[
				SNew(SVerticalBox)
				// Asset picker (top)
				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				[
					AssetPickerWidget
				]
				// Separator
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.0f, 4.0f)
				[
					SNew(SSeparator)
				]
				// Button bar (bottom)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(8.0f, 4.0f, 8.0f, 8.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					// Left margin
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(0.0f, 0.0f, 4.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("SkipMesh", "Skip Mesh"))
						.ToolTipText(LOCTEXT("SkipMeshTooltip", "Add ring without mesh"))
						.OnClicked_Lambda([this, BoneNameToAdd]()
						{
							FSlateApplication::Get().DismissAllMenus();
							OnAddRingRequested.ExecuteIfBound(BoneNameToAdd, nullptr);
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("Cancel", "Cancel"))
						.OnClicked_Lambda([]()
						{
							FSlateApplication::Get().DismissAllMenus();
							return FReply::Handled();
						})
					]
				]
			],
		FSlateApplication::Get().GetCursorPos(),
		FPopupTransitionEffect::ContextMenu
	);
}

bool SFleshRingSkeletonTree::CanAddRing() const
{
	// Can only add Ring to mesh bones (excludes IK/virtual bones)
	return SelectedItem.IsValid()
		&& SelectedItem->ItemType == EFleshRingTreeItemType::Bone
		&& SelectedItem->bIsMeshBone;
}

void SFleshRingSkeletonTree::OnContextMenuDeleteRing()
{
	if (!CanDeleteRing())
	{
		return;
	}

	UFleshRingAsset* Asset = EditingAsset.Get();
	if (!Asset)
	{
		return;
	}

	int32 RingIndex = SelectedItem->RingIndex;
	if (Asset->Rings.IsValidIndex(RingIndex))
	{
		// Undo/Redo support
		// Limit transaction scope so RefreshPreview() is called outside the transaction
		// (Prevents Undo crash when creating PreviewSubdividedMesh during transaction)
		{
			FScopedTransaction Transaction(FText::FromString(TEXT("Delete Ring")));
			Asset->Modify();

			Asset->Rings.RemoveAt(RingIndex);

			// Selection cleared (properly restored on Undo)
			Asset->EditorSelectedRingIndex = -1;
			Asset->EditorSelectionType = EFleshRingSelectionType::None;
		}

		// Call delegate (HandleRingDeleted handles RefreshPreview + RefreshTree)
		// Called after transaction ends so mesh generation is not included in Undo history
		OnRingDeleted.ExecuteIfBound();
	}
}

bool SFleshRingSkeletonTree::CanDeleteRing() const
{
	return SelectedItem.IsValid() && SelectedItem->ItemType == EFleshRingTreeItemType::Ring;
}

void SFleshRingSkeletonTree::OnContextMenuRenameRing()
{
	if (SelectedItem.IsValid() && SelectedItem->ItemType == EFleshRingTreeItemType::Ring)
	{
		TSharedPtr<ITableRow> RowWidget = TreeView->WidgetFromItem(SelectedItem);
		if (RowWidget.IsValid())
		{
			TSharedPtr<SFleshRingTreeRow> TreeRow = StaticCastSharedPtr<SFleshRingTreeRow>(RowWidget);
			if (TreeRow.IsValid())
			{
				TreeRow->EnterEditingMode();
			}
		}
	}
}

void SFleshRingSkeletonTree::OnContextMenuCopyBoneName()
{
	if (SelectedItem.IsValid())
	{
		FPlatformApplicationMisc::ClipboardCopy(*SelectedItem->BoneName.ToString());
	}
}

void SFleshRingSkeletonTree::OnContextMenuCopyRing()
{
	if (!CanCopyRing())
	{
		return;
	}

	UFleshRingAsset* Asset = EditingAsset.Get();
	int32 RingIndex = SelectedItem->RingIndex;

	if (Asset && Asset->Rings.IsValidIndex(RingIndex))
	{
		CopiedRingSettings = Asset->Rings[RingIndex];
		CopiedRingSourceBone = Asset->Rings[RingIndex].BoneName;
	}
}

bool SFleshRingSkeletonTree::CanCopyRing() const
{
	return SelectedItem.IsValid() && SelectedItem->ItemType == EFleshRingTreeItemType::Ring;
}

void SFleshRingSkeletonTree::OnContextMenuPasteRing()
{
	if (!CanPasteRing())
	{
		return;
	}

	PasteRingToBone(CopiedRingSourceBone);
}

void SFleshRingSkeletonTree::OnContextMenuPasteRingToSelectedBone()
{
	if (!CanPasteRing() || !SelectedItem.IsValid())
	{
		return;
	}

	PasteRingToBone(SelectedItem->BoneName);
}

bool SFleshRingSkeletonTree::CanPasteRing() const
{
	// Cannot paste if no Ring has been copied
	if (!CopiedRingSettings.IsSet())
	{
		return false;
	}

	// Cannot paste if Ring is selected (same behavior as sockets)
	if (SelectedItem.IsValid() && SelectedItem->ItemType == EFleshRingTreeItemType::Ring)
	{
		return false;
	}

	return true;
}

bool SFleshRingSkeletonTree::CanPasteRingToSelectedBone() const
{
	// Basic paste conditions + check if selected bone is a mesh bone
	// Cannot add Ring to IK/virtual bones (same conditions as CanAddRing)
	return CanPasteRing()
		&& SelectedItem.IsValid()
		&& SelectedItem->ItemType == EFleshRingTreeItemType::Bone
		&& SelectedItem->bIsMeshBone;
}

void SFleshRingSkeletonTree::PasteRingToBone(FName TargetBoneName)
{
	UFleshRingAsset* Asset = EditingAsset.Get();
	if (!Asset || !CopiedRingSettings.IsSet())
	{
		return;
	}

	// Save current selection state (maintain selection like sockets)
	FName SelectedBoneName = SelectedItem.IsValid() ? SelectedItem->BoneName : NAME_None;

	FScopedTransaction Transaction(LOCTEXT("PasteRing", "Paste Ring"));
	Asset->Modify();

	FFleshRingSettings NewRing = CopiedRingSettings.GetValue();
	NewRing.BoneName = TargetBoneName;
	// Use Asset's existing MakeUniqueRingName
	NewRing.RingName = Asset->MakeUniqueRingName(CopiedRingSettings->RingName);

	Asset->Rings.Add(NewRing);

	// Notify asset change (don't change selection - same behavior as sockets)
	Asset->OnAssetChanged.Broadcast(Asset);

	// Refresh tree
	RefreshTree();

	// Restore selection state
	if (!SelectedBoneName.IsNone())
	{
		SelectBone(SelectedBoneName);
	}
}

FReply SFleshRingSkeletonTree::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	// Ctrl+C: Copy Ring
	if (InKeyEvent.IsControlDown() && InKeyEvent.GetKey() == EKeys::C)
	{
		if (CanCopyRing())
		{
			OnContextMenuCopyRing();
			return FReply::Handled();
		}
	}

	// Ctrl+Shift+V: Paste to selected bone (check before Ctrl+V, only available for mesh bones)
	if (InKeyEvent.IsControlDown() && InKeyEvent.IsShiftDown() && InKeyEvent.GetKey() == EKeys::V)
	{
		if (CanPasteRingToSelectedBone())
		{
			OnContextMenuPasteRingToSelectedBone();
			return FReply::Handled();
		}
	}

	// Ctrl+V: Paste to original bone
	if (InKeyEvent.IsControlDown() && !InKeyEvent.IsShiftDown() && InKeyEvent.GetKey() == EKeys::V)
	{
		if (CanPasteRing())
		{
			OnContextMenuPasteRing();
			return FReply::Handled();
		}
	}

	// F2 key: Rename Ring
	if (InKeyEvent.GetKey() == EKeys::F2)
	{
		if (SelectedItem.IsValid() && SelectedItem->ItemType == EFleshRingTreeItemType::Ring)
		{
			OnContextMenuRenameRing();
			return FReply::Handled();
		}
	}

	// F key: Focus camera
	if (InKeyEvent.GetKey() == EKeys::F)
	{
		OnFocusCameraRequested.ExecuteIfBound();
		return FReply::Handled();
	}

	// Delete key: Delete selected Ring
	if (InKeyEvent.GetKey() == EKeys::Delete)
	{
		if (CanDeleteRing())
		{
			OnContextMenuDeleteRing();
			return FReply::Handled();
		}
	}

	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

void SFleshRingSkeletonTree::MoveRingToBone(int32 RingIndex, FName NewBoneName, bool bPreserveWorldPosition)
{
	UFleshRingAsset* Asset = EditingAsset.Get();
	if (!Asset || !Asset->Rings.IsValidIndex(RingIndex))
	{
		return;
	}

	// Ignore if same bone (for move, not duplicate)
	if (Asset->Rings[RingIndex].BoneName == NewBoneName)
	{
		return;
	}

	// Undo/Redo support
	FScopedTransaction Transaction(LOCTEXT("MoveRingToBone", "Move Ring to Bone"));
	Asset->Modify();

	FFleshRingSettings& Ring = Asset->Rings[RingIndex];

	// Shift+drag: Preserve world (bind pose) position
	USkeletalMesh* SkelMesh = Asset->TargetSkeletalMesh.LoadSynchronous();
	if (bPreserveWorldPosition && SkelMesh)
	{
		const FReferenceSkeleton& RefSkeleton = SkelMesh->GetRefSkeleton();
		int32 OldBoneIndex = RefSkeleton.FindBoneIndex(Ring.BoneName);
		int32 NewBoneIndex = RefSkeleton.FindBoneIndex(NewBoneName);

		if (OldBoneIndex != INDEX_NONE && NewBoneIndex != INDEX_NONE)
		{
			// Calculate bone transform based on bind pose (using existing code pattern)
			auto GetBindPoseTransform = [&RefSkeleton](int32 BoneIndex) -> FTransform
			{
				FTransform BindPoseBoneTransform = FTransform::Identity;
				int32 CurrentBoneIdx = BoneIndex;
				while (CurrentBoneIdx != INDEX_NONE)
				{
					BindPoseBoneTransform = BindPoseBoneTransform * RefSkeleton.GetRefBonePose()[CurrentBoneIdx];
					CurrentBoneIdx = RefSkeleton.GetParentIndex(CurrentBoneIdx);
				}
				return BindPoseBoneTransform;
			};

			FTransform OldBoneAbsolute = GetBindPoseTransform(OldBoneIndex);
			FTransform NewBoneAbsolute = GetBindPoseTransform(NewBoneIndex);

			// Always transform MeshOffset/MeshRotation (ring mesh is available in all modes)
			FVector OldWorldMeshOffset = OldBoneAbsolute.TransformPosition(Ring.MeshOffset);
			Ring.MeshOffset = NewBoneAbsolute.InverseTransformPosition(OldWorldMeshOffset);

			FQuat OldWorldMeshRotation = OldBoneAbsolute.GetRotation() * Ring.MeshRotation;
			Ring.MeshRotation = NewBoneAbsolute.GetRotation().Inverse() * OldWorldMeshRotation;

			// Additionally transform mode-specific offsets
			if (Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualRing)
			{
				// VirtualRing: Transform RingOffset and RingRotation
				FVector OldWorldOffset = OldBoneAbsolute.TransformPosition(Ring.RingOffset);
				Ring.RingOffset = NewBoneAbsolute.InverseTransformPosition(OldWorldOffset);

				FQuat OldWorldRotation = OldBoneAbsolute.GetRotation() * Ring.RingRotation;
				Ring.RingRotation = NewBoneAbsolute.GetRotation().Inverse() * OldWorldRotation;
				Ring.RingEulerRotation = Ring.RingRotation.Rotator();
			}
			else if (Ring.InfluenceMode == EFleshRingInfluenceMode::VirtualBand)
			{
				// VirtualBand: Transform BandOffset and BandRotation
				FVector OldWorldOffset = OldBoneAbsolute.TransformPosition(Ring.VirtualBand.BandOffset);
				Ring.VirtualBand.BandOffset = NewBoneAbsolute.InverseTransformPosition(OldWorldOffset);

				FQuat OldWorldRotation = OldBoneAbsolute.GetRotation() * Ring.VirtualBand.BandRotation;
				Ring.VirtualBand.BandRotation = NewBoneAbsolute.GetRotation().Inverse() * OldWorldRotation;
				Ring.VirtualBand.BandEulerRotation = Ring.VirtualBand.BandRotation.Rotator();
			}
		}
	}

	// Change BoneName
	Ring.BoneName = NewBoneName;

	// Notify asset change
	Asset->OnAssetChanged.Broadcast(Asset);

	// Refresh tree
	RefreshTree();

	// Select moved Ring
	SelectRingByIndex(RingIndex);
}

void SFleshRingSkeletonTree::DuplicateRingToBone(int32 SourceRingIndex, FName TargetBoneName)
{
	UFleshRingAsset* Asset = EditingAsset.Get();
	if (!Asset || !Asset->Rings.IsValidIndex(SourceRingIndex))
	{
		return;
	}

	FScopedTransaction Transaction(LOCTEXT("DuplicateRing", "Duplicate Ring"));
	Asset->Modify();

	// Duplicate Ring
	FFleshRingSettings NewRing = Asset->Rings[SourceRingIndex];
	NewRing.BoneName = TargetBoneName;

	// Use existing UFleshRingAsset::MakeUniqueRingName() (same FName numbering as Unreal sockets)
	NewRing.RingName = Asset->MakeUniqueRingName(NewRing.RingName);

	// Add to array
	int32 NewIndex = Asset->Rings.Add(NewRing);

	// Notify asset change
	Asset->OnAssetChanged.Broadcast(Asset);

	// Refresh tree
	RefreshTree();

	// Select duplicated Ring
	SelectRingByIndex(NewIndex);
}

#undef LOCTEXT_NAMESPACE
