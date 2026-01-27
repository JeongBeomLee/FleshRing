// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingSettingsCustomization.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "IDetailGroup.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "FleshRingTypes.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STreeView.h"
#include "Widgets/Views/SExpanderArrow.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Styling/AppStyle.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"
#include "Misc/DefaultValueHelper.h"
#include "ScopedTransaction.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Application/SlateApplication.h"
#include "FleshRingEditorViewportClient.h"

#define LOCTEXT_NAMESPACE "FleshRingSettingsCustomization"

/**
 * Ring name inline edit widget
 * - Single click: Select Ring
 * - Double click: Enter name edit mode
 * - Duplicate name validation (exclamation icon + error tooltip)
 */
class SRingNameWidget : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SRingNameWidget) {}
		SLATE_ARGUMENT(FText, InitialText)
		SLATE_ATTRIBUTE(bool, IsSelected)
		SLATE_ARGUMENT(UFleshRingAsset*, Asset)
		SLATE_ARGUMENT(int32, RingIndex)
		SLATE_EVENT(FSimpleDelegate, OnClicked)
		SLATE_EVENT(FOnTextCommitted, OnTextCommitted)
		SLATE_EVENT(FSimpleDelegate, OnDeleteRequested)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OnClickedDelegate = InArgs._OnClicked;
		OnTextCommittedDelegate = InArgs._OnTextCommitted;
		OnDeleteRequestedDelegate = InArgs._OnDeleteRequested;
		IsSelectedAttr = InArgs._IsSelected;
		Asset = InArgs._Asset;
		RingIndex = InArgs._RingIndex;

		// Store initial text (not a binding - fixed value)
		CurrentText = InArgs._InitialText;

		// Subscribe to asset change delegate (update when name changes from skeleton tree)
		if (Asset)
		{
			Asset->OnAssetChanged.AddSP(this, &SRingNameWidget::OnAssetChangedHandler);
		}

		ChildSlot
		[
			SAssignNew(InlineTextBlock, SInlineEditableTextBlock)
			.Text(CurrentText)
			.IsSelected(this, &SRingNameWidget::IsSelected)
			.OnVerifyTextChanged(this, &SRingNameWidget::OnVerifyNameChanged)
			.OnTextCommitted(this, &SRingNameWidget::OnNameCommitted)
			.Font(IDetailLayoutBuilder::GetDetailFont())
		];

		// Prevent child widget from receiving mouse events directly
		// (re-enable only when entering edit mode)
		if (InlineTextBlock.IsValid())
		{
			InlineTextBlock->SetVisibility(EVisibility::HitTestInvisible);
		}
	}

	~SRingNameWidget()
	{
		// Unbind delegate
		if (Asset)
		{
			Asset->OnAssetChanged.RemoveAll(this);
		}
	}

	/** Update text (called externally) */
	void SetText(const FText& NewText)
	{
		CurrentText = NewText;
		if (InlineTextBlock.IsValid())
		{
			InlineTextBlock->SetText(NewText);
		}
	}

	/** Asset change handler (when name changes from skeleton tree) */
	void OnAssetChangedHandler(UFleshRingAsset* ChangedAsset)
	{
		if (Asset && Asset->Rings.IsValidIndex(RingIndex))
		{
			FText NewText = FText::FromString(Asset->Rings[RingIndex].GetDisplayName(RingIndex));
			CurrentText = NewText;
			if (InlineTextBlock.IsValid())
			{
				InlineTextBlock->SetText(NewText);
			}
		}
	}

	virtual bool SupportsKeyboardFocus() const override
	{
		return true;
	}

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			// Track left click pressed state
			bIsLeftMouseButtonDown = true;
			// Single click: Select Ring + set focus (for F2 key handling)
			OnClickedDelegate.ExecuteIfBound();
			return FReply::Handled().SetUserFocus(AsShared(), EFocusCause::Mouse);
		}
		else if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
		{
			// Ignore right click if left click is pressed (prevent simultaneous clicks)
			if (bIsLeftMouseButtonDown)
			{
				return FReply::Handled();
			}
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	virtual FReply OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			// Ignore double click if right click is also pressed
			if (MouseEvent.IsMouseButtonDown(EKeys::RightMouseButton))
			{
				return FReply::Handled();
			}
			// Double click: Enter edit mode
			EnterEditingMode();
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			// Left click released
			bIsLeftMouseButtonDown = false;
			return FReply::Handled();
		}
		else if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
		{
			// Don't show context menu if left click is pressed
			if (bIsLeftMouseButtonDown)
			{
				return FReply::Handled();
			}

			// Right click: Show context menu
			ShowContextMenu(MouseEvent.GetScreenSpacePosition());
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	/** Show context menu */
	void ShowContextMenu(const FVector2D& ScreenPosition)
	{
		FMenuBuilder MenuBuilder(true, nullptr);

		// Rename Ring menu item
		FMenuEntryParams RenameParams;
		RenameParams.LabelOverride = LOCTEXT("RenameRingName", "Rename Ring");
		RenameParams.ToolTipOverride = LOCTEXT("RenameRingNameTooltip", "Rename this ring");
		RenameParams.IconOverride = FSlateIcon(FAppStyle::GetAppStyleSetName(), "GenericCommands.Rename");
		RenameParams.DirectActions = FUIAction(
			FExecuteAction::CreateSP(this, &SRingNameWidget::EnterEditingMode)
		);
		RenameParams.InputBindingOverride = FText::FromString(TEXT("F2"));
		MenuBuilder.AddMenuEntry(RenameParams);

		FWidgetPath WidgetPath;
		FSlateApplication::Get().GeneratePathToWidgetChecked(AsShared(), WidgetPath);
		FSlateApplication::Get().PushMenu(
			AsShared(),
			WidgetPath,
			MenuBuilder.MakeWidget(),
			ScreenPosition,
			FPopupTransitionEffect::ContextMenu
		);
	}

	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override
	{
		// F2 key: Enter name edit mode
		if (InKeyEvent.GetKey() == EKeys::F2)
		{
			EnterEditingMode();
			return FReply::Handled();
		}

		// Delete key: Delete Ring
		if (InKeyEvent.GetKey() == EKeys::Delete)
		{
			OnDeleteRequestedDelegate.ExecuteIfBound();
			return FReply::Handled();
		}

		// F key: Camera focus (on selected Ring)
		if (InKeyEvent.GetKey() == EKeys::F)
		{
			FocusCameraOnRing();
			return FReply::Handled();
		}

		return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
	}

	/** Enter edit mode */
	void EnterEditingMode()
	{
		// Save original text when starting edit (for restoration on validation failure)
		OriginalText = CurrentText;
		bIsEnterPressed = false;

		if (InlineTextBlock.IsValid())
		{
			// Enable receiving mouse events while editing
			InlineTextBlock->SetVisibility(EVisibility::Visible);
			InlineTextBlock->EnterEditingMode();
		}
	}

	virtual FReply OnPreviewKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override
	{
		// Detect Enter key (to revert to previous name in OnVerifyNameChanged)
		if (InKeyEvent.GetKey() == EKeys::Enter)
		{
			bIsEnterPressed = true;
		}
		return SCompoundWidget::OnPreviewKeyDown(MyGeometry, InKeyEvent);
	}

private:
	bool IsSelected() const
	{
		return IsSelectedAttr.Get(false);
	}

	/** Focus camera on the selected Ring */
	void FocusCameraOnRing()
	{
		if (!Asset)
		{
			return;
		}

		// Find the viewport client currently editing this asset
		for (FFleshRingEditorViewportClient* ViewportClient : FFleshRingEditorViewportClient::GetAllInstances())
		{
			if (ViewportClient && ViewportClient->GetEditingAsset() == Asset)
			{
				ViewportClient->FocusOnMesh();
				break;
			}
		}
	}

	/** Name validation (empty name/duplicate check) */
	bool OnVerifyNameChanged(const FText& NewText, FText& OutErrorMessage)
	{
		if (!Asset)
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
		else if (!Asset->IsRingNameUnique(NewName, RingIndex))
		{
			OutErrorMessage = LOCTEXT("DuplicateNameError", "This name is already in use. Please choose a different name.");
			bIsValid = false;
		}

		if (!bIsValid)
		{
			// Revert to previous name only on Enter
			if (bIsEnterPressed && InlineTextBlock.IsValid())
			{
				InlineTextBlock->SetText(OriginalText);
			}
			bIsEnterPressed = false;
			return false;  // Keep edit mode
		}

		bIsEnterPressed = false;
		return true;
	}

	/** Commit name */
	void OnNameCommitted(const FText& NewText, ETextCommit::Type CommitType)
	{
		if (CommitType == ETextCommit::OnEnter || CommitType == ETextCommit::OnUserMovedFocus)
		{
			// If OnVerifyTextChanged returns false, this won't be reached
			// If we reach here, the name is valid
			CurrentText = NewText;
			if (InlineTextBlock.IsValid())
			{
				InlineTextBlock->SetText(NewText);
			}
			OnTextCommittedDelegate.ExecuteIfBound(NewText, CommitType);
		}

		// Block mouse events again after editing ends
		if (InlineTextBlock.IsValid())
		{
			InlineTextBlock->SetVisibility(EVisibility::HitTestInvisible);
		}
	}

	TSharedPtr<SInlineEditableTextBlock> InlineTextBlock;
	FSimpleDelegate OnClickedDelegate;
	FOnTextCommitted OnTextCommittedDelegate;
	FSimpleDelegate OnDeleteRequestedDelegate;
	TAttribute<bool> IsSelectedAttr;
	UFleshRingAsset* Asset = nullptr;
	int32 RingIndex = INDEX_NONE;
	FText CurrentText;
	FText OriginalText;				// Original text at edit start (for restoration on validation failure)
	bool bIsEnterPressed = false;		// Enter key detection flag
	bool bIsLeftMouseButtonDown = false;	// Left click pressed state (prevents simultaneous clicks)
};

/**
 * Clickable/Double-clickable Row button widget
 */
class SClickableRowButton : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SClickableRowButton) {}
		SLATE_DEFAULT_SLOT(FArguments, Content)
		SLATE_EVENT(FSimpleDelegate, OnClicked)
		SLATE_EVENT(FSimpleDelegate, OnDoubleClicked)
		SLATE_ATTRIBUTE(FText, ToolTipText)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		OnClickedDelegate = InArgs._OnClicked;
		OnDoubleClickedDelegate = InArgs._OnDoubleClicked;

		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("NoBorder"))
			.Padding(FMargin(4, 2))
			.ToolTipText(InArgs._ToolTipText)
			[
				InArgs._Content.Widget
			]
		];
	}

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			// Single click
			OnClickedDelegate.ExecuteIfBound();
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	virtual FReply OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			// Double click
			OnDoubleClickedDelegate.ExecuteIfBound();
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

private:
	FSimpleDelegate OnClickedDelegate;
	FSimpleDelegate OnDoubleClickedDelegate;
};

/**
 * Tree row widget for Bone dropdown (SExpanderArrow + Wires support)
 */
class SBoneDropdownTreeRow : public STableRow<TSharedPtr<FBoneDropdownItem>>
{
public:
	SLATE_BEGIN_ARGS(SBoneDropdownTreeRow) {}
		SLATE_ARGUMENT(TSharedPtr<FBoneDropdownItem>, Item)
		SLATE_ARGUMENT(FText, HighlightText)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
	{
		Item = InArgs._Item;
		HighlightText = InArgs._HighlightText;

		// Determine icon and color
		const FSlateBrush* IconBrush = nullptr;
		FSlateColor TextColor = FSlateColor::UseForeground();
		FSlateColor IconColor = FSlateColor::UseForeground();

		if (Item->bIsMeshBone)
		{
			IconBrush = FAppStyle::GetBrush("SkeletonTree.Bone");
		}
		else
		{
			// Non-weighted bone (only shown during search)
			IconBrush = FAppStyle::GetBrush("SkeletonTree.BoneNonWeighted");
			TextColor = FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f));
			IconColor = FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f));
		}

		STableRow<TSharedPtr<FBoneDropdownItem>>::Construct(
			STableRow<TSharedPtr<FBoneDropdownItem>>::FArguments()
			.Padding(FMargin(0, 0)),
			InOwnerTable
		);

		// Display tree connection lines with SExpanderArrow
		ChildSlot
		[
			SNew(SHorizontalBox)
			// Expander Arrow (tree connection lines)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			[
				SNew(SExpanderArrow, SharedThis(this))
				.ShouldDrawWires(true)
			]
			// Icon + Text
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(0, 2)
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
					.DesiredSizeOverride(FVector2D(16, 16))
				]
				// Bone name
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(FText::FromName(Item->BoneName))
					.ColorAndOpacity(TextColor)
					.Font(IDetailLayoutBuilder::GetDetailFont())
					.HighlightText(HighlightText)
				]
			]
		];
	}

private:
	TSharedPtr<FBoneDropdownItem> Item;
	FText HighlightText;
};

// TypeInterface for angle display (shows ° next to the number)
class FDegreeTypeInterface : public TDefaultNumericTypeInterface<double>
{
public:
	virtual FString ToString(const double& Value) const override
	{
		return FString::Printf(TEXT("%.2f\u00B0"), Value);
	}

	virtual TOptional<double> FromString(const FString& InString, const double& ExistingValue) override
	{
		FString CleanString = InString.Replace(TEXT("\u00B0"), TEXT("")).TrimStartAndEnd();
		double Result = 0.0;
		if (LexTryParseString(Result, *CleanString))
		{
			return Result;
		}
		return TOptional<double>();
	}
};

TSharedRef<IPropertyTypeCustomization> FFleshRingSettingsCustomization::MakeInstance()
{
	return MakeShareable(new FFleshRingSettingsCustomization);
}

void FFleshRingSettingsCustomization::CustomizeHeader(
	TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// Cache main property handle (for asset access)
	MainPropertyHandle = PropertyHandle;

	// Cache array index (for click selection and name display)
	CachedArrayIndex = PropertyHandle->GetIndexInArray();

	// Pre-fetch BoneName handle (for header preview)
	BoneNameHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BoneName));

	// Header: entire row is clickable (click=select, double-click=rename)
	FText TooltipText = LOCTEXT("RingHeaderTooltip", "Ring Name\nClick to select, Double-click to rename");

	// Handle for array control
	TSharedRef<IPropertyHandle> PropHandleRef = PropertyHandle;

	HeaderRow.WholeRowContent()
	[
		// Background color highlight based on selection state (replaces HighlightProperty)
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
		.BorderBackgroundColor(this, &FFleshRingSettingsCustomization::GetHeaderBackgroundColor)
		.Padding(0)
		[
			SNew(SClickableRowButton)
			.OnClicked(FSimpleDelegate::CreateRaw(this, &FFleshRingSettingsCustomization::OnHeaderClickedVoid))
			.OnDoubleClicked(FSimpleDelegate::CreateLambda([this]() {
				if (RingNameWidget.IsValid())
				{
					RingNameWidget->EnterEditingMode();
				}
			}))
			.ToolTipText(TooltipText)
			[
				SNew(SHorizontalBox)
			// Left column: Ring name (35%, with clipping)
			+ SHorizontalBox::Slot()
			.FillWidth(0.35f)
			.VAlign(VAlign_Center)
			.Padding(0, 0, 16, 0)  // Spacing between Ring name and Bone name
			[
				SNew(SBox)
				.Clipping(EWidgetClipping::ClipToBounds)
				[
					SAssignNew(RingNameWidget, SRingNameWidget)
					.InitialText(GetDisplayRingName(CachedArrayIndex))
					.IsSelected(this, &FFleshRingSettingsCustomization::IsThisRingSelected)
					.Asset(GetOuterAsset())
					.RingIndex(CachedArrayIndex)
					.OnClicked(FSimpleDelegate::CreateRaw(this, &FFleshRingSettingsCustomization::OnHeaderClickedVoid))
					.OnTextCommitted(this, &FFleshRingSettingsCustomization::OnRingNameCommitted)
					.OnDeleteRequested_Lambda([PropHandleRef]() {
						if (TSharedPtr<IPropertyHandleArray> ArrayHandle = PropHandleRef->GetParentHandle()->AsArray())
						{
							int32 Index = PropHandleRef->GetIndexInArray();
							ArrayHandle->DeleteItem(Index);
						}
					})
				]
			]
			// Right column: Bone name + buttons (65%)
			+ SHorizontalBox::Slot()
			.FillWidth(0.65f)
			.VAlign(VAlign_Center)
			[
				SNew(SHorizontalBox)
				// Bone name
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &FFleshRingSettingsCustomization::GetCurrentBoneName)
					.Font(IDetailLayoutBuilder::GetDetailFont())
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				// Visibility toggle button (eye icon)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(2, 0)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.OnClicked(this, &FFleshRingSettingsCustomization::OnVisibilityToggleClicked)
					.ToolTipText(LOCTEXT("ToggleVisibilityTooltip", "Toggle ring visibility"))
					.ContentPadding(2)
					[
						SNew(SImage)
						.Image(this, &FFleshRingSettingsCustomization::GetVisibilityIcon)
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
				// Insert button
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(2, 0)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.OnClicked_Lambda([PropHandleRef]() -> FReply {
						if (TSharedPtr<IPropertyHandleArray> ArrayHandle = PropHandleRef->GetParentHandle()->AsArray())
						{
							int32 Index = PropHandleRef->GetIndexInArray();
							ArrayHandle->Insert(Index);
						}
						return FReply::Handled();
					})
					.ToolTipText(LOCTEXT("InsertTooltip", "Insert"))
					.ContentPadding(2)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.PlusCircle"))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
				// Duplicate button
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(2, 0)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.OnClicked_Lambda([PropHandleRef]() -> FReply {
						if (TSharedPtr<IPropertyHandleArray> ArrayHandle = PropHandleRef->GetParentHandle()->AsArray())
						{
							int32 Index = PropHandleRef->GetIndexInArray();
							ArrayHandle->DuplicateItem(Index);
						}
						return FReply::Handled();
					})
					.ToolTipText(LOCTEXT("DuplicateTooltip", "Duplicate"))
					.ContentPadding(2)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.Duplicate"))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
				// Delete button
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(2, 0)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.OnClicked_Lambda([PropHandleRef]() -> FReply {
						if (TSharedPtr<IPropertyHandleArray> ArrayHandle = PropHandleRef->GetParentHandle()->AsArray())
						{
							int32 Index = PropHandleRef->GetIndexInArray();
							ArrayHandle->DeleteItem(Index);
						}
						return FReply::Handled();
					})
					.ToolTipText(LOCTEXT("DeleteTooltip", "Delete"))
					.ContentPadding(2)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.Delete"))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
			]
		]
		]  // Close SBorder
	];
}

void FFleshRingSettingsCustomization::CustomizeChildren(
	TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// BoneName handle is already set in CustomizeHeader
	// Build bone tree
	BuildBoneTree();

	// Get InfluenceMode handle (processed first to place at top)
	TSharedPtr<IPropertyHandle> InfluenceModeHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, InfluenceMode));

	// ----- Effect Range Mode (topmost, above BoneName) -----
	if (InfluenceModeHandle.IsValid())
	{
		ChildBuilder.AddProperty(InfluenceModeHandle.ToSharedRef());
	}

	// Customize BoneName as searchable dropdown
	if (BoneNameHandle.IsValid())
	{
		ChildBuilder.AddCustomRow(LOCTEXT("BoneNameRow", "Bone Name"))
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("BoneNameLabel", "Bone Name"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		]
		.ValueContent()
		.MinDesiredWidth(200.0f)
		[
			CreateSearchableBoneDropdown()
		];
	}

	// Cache Rotation handles
	RingRotationHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingRotation));
	MeshRotationHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshRotation));

	// Check if current InfluenceMode is VirtualRing (for initial state)
	bool bIsVirtualRingMode = false;
	if (InfluenceModeHandle.IsValid())
	{
		uint8 ModeValue = 0;
		InfluenceModeHandle->GetValue(ModeValue);
		bIsVirtualRingMode = (static_cast<EFleshRingInfluenceMode>(ModeValue) == EFleshRingInfluenceMode::VirtualRing);
	}

	// TAttribute for dynamic VirtualRing mode check (used in Ring Transform)
	TAttribute<bool> IsVirtualRingModeAttr = TAttribute<bool>::Create([InfluenceModeHandle]() -> bool
	{
		if (!InfluenceModeHandle.IsValid())
		{
			return true;
		}
		uint8 ModeValue = 0;
		InfluenceModeHandle->GetValue(ModeValue);
		return static_cast<EFleshRingInfluenceMode>(ModeValue) == EFleshRingInfluenceMode::VirtualRing;
	});

	// TAttribute for dynamic Auto(SDF) mode check (used in SDF Bounds Expand)
	TAttribute<bool> IsAutoModeAttr = TAttribute<bool>::Create([InfluenceModeHandle]() -> bool
	{
		if (!InfluenceModeHandle.IsValid())
		{
			return true;
		}
		uint8 ModeValue = 0;
		InfluenceModeHandle->GetValue(ModeValue);
		return static_cast<EFleshRingInfluenceMode>(ModeValue) == EFleshRingInfluenceMode::Auto;
	});

	// TAttribute for dynamic VirtualBand mode check
	TAttribute<bool> IsVirtualBandModeAttr = TAttribute<bool>::Create([InfluenceModeHandle]() -> bool
	{
		if (!InfluenceModeHandle.IsValid())
		{
			return false;
		}
		uint8 ModeValue = 0;
		InfluenceModeHandle->GetValue(ModeValue);
		return static_cast<EFleshRingInfluenceMode>(ModeValue) == EFleshRingInfluenceMode::VirtualBand;
	});

	// TAttribute for dynamic non-VirtualBand mode check (used in BulgeRadialTaper, etc.)
	TAttribute<bool> IsNotVirtualBandModeAttr = TAttribute<bool>::Create([InfluenceModeHandle]() -> bool
	{
		if (!InfluenceModeHandle.IsValid())
		{
			return true;
		}
		uint8 ModeValue = 0;
		InfluenceModeHandle->GetValue(ModeValue);
		return static_cast<EFleshRingInfluenceMode>(ModeValue) != EFleshRingInfluenceMode::VirtualBand;
	});

	// Collect properties to add to Ring group
	TSharedPtr<IPropertyHandle> RingMeshHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingMesh));
	TSharedPtr<IPropertyHandle> RingRadiusHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingRadius));
	TSharedPtr<IPropertyHandle> RingThicknessHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingThickness));
	TSharedPtr<IPropertyHandle> RingHeightHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingHeight));
	TSharedPtr<IPropertyHandle> RingOffsetHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingOffset));
	TSharedPtr<IPropertyHandle> RingEulerHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingEulerRotation));

	// Property names to go into Ring group
	TSet<FName> RingGroupProperties;
	RingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingMesh));
	RingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingRadius));
	RingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingThickness));
	RingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingHeight));
	RingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingOffset));
	RingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingEulerRotation));

	// SDF group property handles
	TSharedPtr<IPropertyHandle> SDFBoundsExpandXHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SDFBoundsExpandX));
	TSharedPtr<IPropertyHandle> SDFBoundsExpandYHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SDFBoundsExpandY));

	// Property names to go into SDF group
	TSet<FName> SDFGroupProperties;
	SDFGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SDFBoundsExpandX));
	SDFGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SDFBoundsExpandY));

	// Mesh Transform group property handles
	TSharedPtr<IPropertyHandle> MeshOffsetHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshOffset));
	TSharedPtr<IPropertyHandle> MeshEulerRotationHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshEulerRotation));
	// MeshScaleHandle already exists as a class member (for scale lock feature)
	MeshScaleHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshScale));

	// Property names to go into Mesh Transform group
	TSet<FName> MeshTransformGroupProperties;
	MeshTransformGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshOffset));
	MeshTransformGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshEulerRotation));
	MeshTransformGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshScale));

	// Virtual Band group property handles
	TSharedPtr<IPropertyHandle> VirtualBandHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, VirtualBand));

	// Property names to go into Virtual Band group
	TSet<FName> VirtualBandGroupProperties;
	VirtualBandGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, VirtualBand));

	// Get Smoothing property handles individually
	// Post Process master toggle
	TSharedPtr<IPropertyHandle> bEnablePostProcessHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnablePostProcess));

	TSharedPtr<IPropertyHandle> bEnableSmoothingHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableSmoothing));
	TSharedPtr<IPropertyHandle> bEnableRadialSmoothingHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableRadialSmoothing));
	TSharedPtr<IPropertyHandle> RadialBlendStrengthHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RadialBlendStrength));
	TSharedPtr<IPropertyHandle> RadialSliceHeightHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RadialSliceHeight));
	TSharedPtr<IPropertyHandle> bEnableLaplacianSmoothingHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableLaplacianSmoothing));
	TSharedPtr<IPropertyHandle> LaplacianSmoothingTypeHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, LaplacianSmoothingType));
	TSharedPtr<IPropertyHandle> SmoothingLambdaHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingLambda));
	TSharedPtr<IPropertyHandle> TaubinMuHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, TaubinMu));
	TSharedPtr<IPropertyHandle> SmoothingIterationsHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingIterations));
	TSharedPtr<IPropertyHandle> bAnchorDeformedVerticesHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bAnchorDeformedVertices));
	TSharedPtr<IPropertyHandle> SmoothingVolumeModeHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingVolumeMode));
	TSharedPtr<IPropertyHandle> MaxSmoothingHopsHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MaxSmoothingHops));
	TSharedPtr<IPropertyHandle> HopFalloffTypeHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, HopFalloffType));
	TSharedPtr<IPropertyHandle> SmoothingBoundsZTopHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingBoundsZTop));
	TSharedPtr<IPropertyHandle> SmoothingBoundsZBottomHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingBoundsZBottom));
	// Heat Propagation handles
	TSharedPtr<IPropertyHandle> bEnableHeatPropagationHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableHeatPropagation));
	TSharedPtr<IPropertyHandle> HeatPropagationIterationsHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, HeatPropagationIterations));
	TSharedPtr<IPropertyHandle> HeatPropagationLambdaHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, HeatPropagationLambda));
	TSharedPtr<IPropertyHandle> bIncludeBulgeVerticesAsSeedsHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bIncludeBulgeVerticesAsSeeds));

	// Property names to go into Post Process group
	TSet<FName> SmoothingGroupProperties;
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnablePostProcess));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableSmoothing));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableRadialSmoothing));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RadialBlendStrength));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RadialSliceHeight));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableLaplacianSmoothing));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, LaplacianSmoothingType));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingLambda));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, TaubinMu));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingIterations));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bAnchorDeformedVertices));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingVolumeMode));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MaxSmoothingHops));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, HopFalloffType));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingBoundsZTop));
	// Heat Propagation properties
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableHeatPropagation));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, HeatPropagationIterations));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, HeatPropagationLambda));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingBoundsZBottom));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bIncludeBulgeVerticesAsSeeds));

	// Get PBD property handles individually
	TSharedPtr<IPropertyHandle> bEnablePBDEdgeConstraintHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnablePBDEdgeConstraint));
	TSharedPtr<IPropertyHandle> bPBDAnchorAffectedVerticesHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bPBDAnchorAffectedVertices));
	TSharedPtr<IPropertyHandle> PBDStiffnessHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, PBDStiffness));
	TSharedPtr<IPropertyHandle> PBDIterationsHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, PBDIterations));
	TSharedPtr<IPropertyHandle> PBDToleranceHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, PBDTolerance));

	// Property names to go into PBD group
	TSet<FName> PBDGroupProperties;
	PBDGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnablePBDEdgeConstraint));
	PBDGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bPBDAnchorAffectedVertices));
	PBDGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, PBDStiffness));
	PBDGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, PBDIterations));
	PBDGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, PBDTolerance));

	// Get Deformation (Tightness + Bulge) property handles
	TSharedPtr<IPropertyHandle> TightnessStrengthHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, TightnessStrength));
	TSharedPtr<IPropertyHandle> FalloffTypeHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, FalloffType));
	TSharedPtr<IPropertyHandle> bEnableBulgeHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableBulge));
	TSharedPtr<IPropertyHandle> BulgeDirectionHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeDirection));
	TSharedPtr<IPropertyHandle> BulgeFalloffHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeFalloff));
	TSharedPtr<IPropertyHandle> BulgeIntensityHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeIntensity));
	TSharedPtr<IPropertyHandle> BulgeAxialRangeHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeAxialRange));
	TSharedPtr<IPropertyHandle> BulgeRadialRangeHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeRadialRange));
	TSharedPtr<IPropertyHandle> BulgeRadialTaperHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeRadialTaper));
	TSharedPtr<IPropertyHandle> UpperBulgeStrengthHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, UpperBulgeStrength));
	TSharedPtr<IPropertyHandle> LowerBulgeStrengthHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, LowerBulgeStrength));
	TSharedPtr<IPropertyHandle> BulgeRadialRatioHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeRadialRatio));

	// Property names to go into Deformation group
	TSet<FName> DeformationGroupProperties;
	// Tightness
	DeformationGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, TightnessStrength));
	DeformationGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, FalloffType));
	// Bulge
	DeformationGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableBulge));
	DeformationGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeDirection));
	DeformationGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeFalloff));
	DeformationGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeIntensity));
	DeformationGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeAxialRange));
	DeformationGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeRadialRange));
	DeformationGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeRadialTaper));
	DeformationGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, UpperBulgeStrength));
	DeformationGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, LowerBulgeStrength));
	DeformationGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeRadialRatio));

	// Display remaining properties first (excluding Ring group)
	uint32 NumChildren;
	PropertyHandle->GetNumChildren(NumChildren);

	for (uint32 ChildIndex = 0; ChildIndex < NumChildren; ++ChildIndex)
	{
		TSharedRef<IPropertyHandle> ChildHandle = PropertyHandle->GetChildHandle(ChildIndex).ToSharedRef();
		FName PropertyName = ChildHandle->GetProperty()->GetFName();

		// Skip BoneName as it's already customized
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BoneName))
		{
			continue;
		}

		// Skip RingName as it's inline editable in the header
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingName))
		{
			continue;
		}

		// Skip InfluenceMode as it's already added at the top
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, InfluenceMode))
		{
			continue;
		}

		// Hide FQuat in UI (only show EulerRotation)
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingRotation) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshRotation))
		{
			continue;
		}

		// Skip properties that go into Ring group
		if (RingGroupProperties.Contains(PropertyName))
		{
			continue;
		}

		// Skip properties that go into Smoothing group
		if (SmoothingGroupProperties.Contains(PropertyName))
		{
			continue;
		}

		// Skip properties that go into PBD group
		if (PBDGroupProperties.Contains(PropertyName))
		{
			continue;
		}

		// Skip properties that go into Deformation group
		if (DeformationGroupProperties.Contains(PropertyName))
		{
			continue;
		}

		// Skip properties that go into SDF group
		if (SDFGroupProperties.Contains(PropertyName))
		{
			continue;
		}

		// Skip properties that go into Mesh Transform group
		if (MeshTransformGroupProperties.Contains(PropertyName))
		{
			continue;
		}

		// Skip properties that go into Virtual Band group
		if (VirtualBandGroupProperties.Contains(PropertyName))
		{
			continue;
		}

		// Use default widget for the rest
		ChildBuilder.AddProperty(ChildHandle);
	}

	// =====================================
	// Ring group
	// =====================================
	IDetailGroup& RingDefinitionGroup = ChildBuilder.AddGroup(TEXT("RingDefinition"), LOCTEXT("RingDefinitionGroup", "Ring"));

	if (RingMeshHandle.IsValid())
	{
		RingDefinitionGroup.AddPropertyRow(RingMeshHandle.ToSharedRef());
	}

	// ----- Mesh Transform subgroup (between RingMesh and InfluenceMode) -----
	IDetailGroup& MeshTransformSubGroup = RingDefinitionGroup.AddGroup(TEXT("MeshTransform"), LOCTEXT("MeshTransformSubGroup", "Mesh Transform"));

	if (MeshOffsetHandle.IsValid())
	{
		MeshTransformSubGroup.AddPropertyRow(MeshOffsetHandle.ToSharedRef())
			.CustomWidget()
			.NameContent()
			[
				MeshOffsetHandle->CreatePropertyNameWidget()
			]
			.ValueContent()
			.MinDesiredWidth(300.0f)
			[
				CreateLinearVectorWidget(MeshOffsetHandle.ToSharedRef(), 0.1f)
			]
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						FVector Value;
						Handle->GetValue(Value);
						return !Value.IsNearlyZero();
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(FVector::ZeroVector);
					})
				)
			);
	}
	if (MeshEulerRotationHandle.IsValid())
	{
		MeshTransformSubGroup.AddPropertyRow(MeshEulerRotationHandle.ToSharedRef())
			.CustomWidget()
			.NameContent()
			[
				MeshEulerRotationHandle->CreatePropertyNameWidget()
			]
			.ValueContent()
			.MinDesiredWidth(300.0f)
			[
				CreateLinearRotatorWidget(MeshEulerRotationHandle.ToSharedRef(), 1.0f)
			]
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						FRotator Value;
						Handle->GetValue(Value);
						return !Value.Equals(FRotator(-90.0f, 0.0f, 0.0f), 0.01f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(FRotator(-90.0f, 0.0f, 0.0f));
					})
				)
			);
	}
	if (MeshScaleHandle.IsValid())
	{
		MeshTransformSubGroup.AddPropertyRow(MeshScaleHandle.ToSharedRef())
			.CustomWidget()
			.NameContent()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					MeshScaleHandle->CreatePropertyNameWidget()
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(4, 0, 0, 0)
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "NoBorder")
					.OnClicked(this, &FFleshRingSettingsCustomization::OnMeshScaleLockClicked)
					.ToolTipText_Lambda([this]()
					{
						return bMeshScaleLocked
							? LOCTEXT("UnlockScale", "Unlock Scale (Disable Proportional Scaling)")
							: LOCTEXT("LockScale", "Lock Scale (Maintain Proportions)");
					})
					.ContentPadding(FMargin(2.0f))
					[
						SNew(SImage)
						.Image_Lambda([this]()
						{
							return bMeshScaleLocked
								? FAppStyle::GetBrush("Icons.Lock")
								: FAppStyle::GetBrush("Icons.Unlock");
						})
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
			]
			.ValueContent()
			.MinDesiredWidth(300.0f)
			[
				CreateMeshScaleWidget(MeshScaleHandle.ToSharedRef(), 0.0025f)
			]
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						FVector Value;
						Handle->GetValue(Value);
						return !Value.Equals(FVector::OneVector, 0.0001f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(FVector::OneVector);
					})
				)
			);
	}

	// ----- Ring Transform subgroup (for VirtualRing mode) -----
	IDetailGroup& RingTransformSubGroup = RingDefinitionGroup.AddGroup(TEXT("RingTransform"), LOCTEXT("RingTransformSubGroup", "Ring Transform"));
	RingTransformSubGroup.HeaderRow()
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("RingTransformSubHeader", "Ring Transform"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ColorAndOpacity_Lambda([IsVirtualRingModeAttr]() -> FSlateColor
			{
				return IsVirtualRingModeAttr.Get() ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground();
			})
		];

	if (RingRadiusHandle.IsValid())
	{
		RingTransformSubGroup.AddPropertyRow(RingRadiusHandle.ToSharedRef())
			.IsEnabled(IsVirtualRingModeAttr)
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 5.0f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(5.0f);
					})
				)
			);
	}
	if (RingThicknessHandle.IsValid())
	{
		RingTransformSubGroup.AddPropertyRow(RingThicknessHandle.ToSharedRef())
			.IsEnabled(IsVirtualRingModeAttr)
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 1.0f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(1.0f);
					})
				)
			);
	}
	if (RingHeightHandle.IsValid())
	{
		RingTransformSubGroup.AddPropertyRow(RingHeightHandle.ToSharedRef())
			.IsEnabled(IsVirtualRingModeAttr)
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 2.0f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(2.0f);
					})
				)
			);
	}
	if (RingOffsetHandle.IsValid())
	{
		RingTransformSubGroup.AddPropertyRow(RingOffsetHandle.ToSharedRef())
			.CustomWidget()
			.NameContent()
			[
				RingOffsetHandle->CreatePropertyNameWidget()
			]
			.ValueContent()
			.MinDesiredWidth(300.0f)
			[
				CreateLinearVectorWidget(RingOffsetHandle.ToSharedRef(), 0.1f)
			]
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						FVector Value;
						Handle->GetValue(Value);
						return !Value.IsNearlyZero();
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(FVector::ZeroVector);
					})
				)
			);
	}
	if (RingEulerHandle.IsValid())
	{
		RingTransformSubGroup.AddPropertyRow(RingEulerHandle.ToSharedRef())
			.CustomWidget()
			.NameContent()
			[
				RingEulerHandle->CreatePropertyNameWidget()
			]
			.ValueContent()
			.MinDesiredWidth(300.0f)
			[
				CreateLinearRotatorWidget(RingEulerHandle.ToSharedRef(), 1.0f)
			]
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						FRotator Value;
						Handle->GetValue(Value);
						return !Value.Equals(FRotator(-90.0f, 0.0f, 0.0f), 0.01f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(FRotator(-90.0f, 0.0f, 0.0f));
					})
				)
			);
	}

	// ----- Virtual Band subgroup (for VirtualBand mode) -----
	IDetailGroup& VirtualBandSubGroup = RingDefinitionGroup.AddGroup(TEXT("VirtualBand"), LOCTEXT("VirtualBandSubGroup", "Virtual Band"));
	VirtualBandSubGroup.HeaderRow()
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("VirtualBandSubHeader", "Virtual Band"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
			.ColorAndOpacity_Lambda([IsVirtualBandModeAttr]() -> FSlateColor
			{
				return IsVirtualBandModeAttr.Get() ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground();
			})
		];

	// Add VirtualBand struct's child properties by group
	if (VirtualBandHandle.IsValid())
	{
		// ----- Transform properties (band position/rotation) -----
		TSharedPtr<IPropertyHandle> BandOffsetHandle = VirtualBandHandle->GetChildHandle(TEXT("BandOffset"));
		TSharedPtr<IPropertyHandle> BandEulerRotationHandle = VirtualBandHandle->GetChildHandle(TEXT("BandEulerRotation"));

		if (BandOffsetHandle.IsValid())
		{
			VirtualBandSubGroup.AddPropertyRow(BandOffsetHandle.ToSharedRef())
				.IsEnabled(IsVirtualBandModeAttr)
				.CustomWidget()
				.NameContent()
				[
					BandOffsetHandle->CreatePropertyNameWidget()
				]
				.ValueContent()
				.MinDesiredWidth(300.0f)
				[
					CreateLinearVectorWidget(BandOffsetHandle.ToSharedRef(), 0.1f)
				]
				.OverrideResetToDefault(
					FResetToDefaultOverride::Create(
						FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							FVector Value;
							Handle->GetValue(Value);
							return !Value.IsNearlyZero();
						}),
						FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							Handle->SetValue(FVector::ZeroVector);
						})
					)
				);
		}
		if (BandEulerRotationHandle.IsValid())
		{
			VirtualBandSubGroup.AddPropertyRow(BandEulerRotationHandle.ToSharedRef())
				.IsEnabled(IsVirtualBandModeAttr)
				.CustomWidget()
				.NameContent()
				[
					BandEulerRotationHandle->CreatePropertyNameWidget()
				]
				.ValueContent()
				.MinDesiredWidth(300.0f)
				[
					CreateLinearRotatorWidget(BandEulerRotationHandle.ToSharedRef(), 1.0f)
				]
				.OverrideResetToDefault(
					FResetToDefaultOverride::Create(
						FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							FRotator Value;
							Handle->GetValue(Value);
							return !Value.Equals(FRotator(-90.0f, 0.0f, 0.0f), 0.01f);
						}),
						FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							Handle->SetValue(FRotator(-90.0f, 0.0f, 0.0f));
						})
					)
				);
		}

		// ----- Common properties (applied globally) -----
		TSharedPtr<IPropertyHandle> BandThicknessHandle = VirtualBandHandle->GetChildHandle(TEXT("BandThickness"));

		if (BandThicknessHandle.IsValid())
		{
			VirtualBandSubGroup.AddPropertyRow(BandThicknessHandle.ToSharedRef())
				.IsEnabled(IsVirtualBandModeAttr);
		}

		// Mid Band subgroup
		IDetailGroup& MidBandGroup = VirtualBandSubGroup.AddGroup(TEXT("MidBand"), LOCTEXT("MidBandGroup", "Mid Band"));
		MidBandGroup.HeaderRow()
			.NameContent()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("MidBandHeader", "Mid Band"))
				.Font(IDetailLayoutBuilder::GetDetailFont())
				.ColorAndOpacity_Lambda([IsVirtualBandModeAttr]() -> FSlateColor
				{
					return IsVirtualBandModeAttr.Get() ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground();
				})
			];

		TSharedPtr<IPropertyHandle> MidUpperRadiusHandle = VirtualBandHandle->GetChildHandle(TEXT("MidUpperRadius"));
		TSharedPtr<IPropertyHandle> MidLowerRadiusHandle = VirtualBandHandle->GetChildHandle(TEXT("MidLowerRadius"));
		TSharedPtr<IPropertyHandle> BandHeightHandle = VirtualBandHandle->GetChildHandle(TEXT("BandHeight"));

		if (MidUpperRadiusHandle.IsValid())
		{
			MidBandGroup.AddPropertyRow(MidUpperRadiusHandle.ToSharedRef())
				.IsEnabled(IsVirtualBandModeAttr);
		}
		if (MidLowerRadiusHandle.IsValid())
		{
			MidBandGroup.AddPropertyRow(MidLowerRadiusHandle.ToSharedRef())
				.IsEnabled(IsVirtualBandModeAttr);
		}
		if (BandHeightHandle.IsValid())
		{
			MidBandGroup.AddPropertyRow(BandHeightHandle.ToSharedRef())
				.IsEnabled(IsVirtualBandModeAttr);
		}

		// Upper/Lower subgroups (add structs as-is)
		TSharedPtr<IPropertyHandle> UpperHandle = VirtualBandHandle->GetChildHandle(TEXT("Upper"));
		TSharedPtr<IPropertyHandle> LowerHandle = VirtualBandHandle->GetChildHandle(TEXT("Lower"));

		if (UpperHandle.IsValid())
		{
			VirtualBandSubGroup.AddPropertyRow(UpperHandle.ToSharedRef())
				.IsEnabled(IsVirtualBandModeAttr);
		}
		if (LowerHandle.IsValid())
		{
			VirtualBandSubGroup.AddPropertyRow(LowerHandle.ToSharedRef())
				.IsEnabled(IsVirtualBandModeAttr);
		}
	}

	// =====================================
	// Deformation group (Tightness + Bulge)
	// =====================================
	IDetailGroup& DeformationGroup = ChildBuilder.AddGroup(TEXT("Deformation"), LOCTEXT("DeformationGroup", "Deformation"));

	// ----- Tightness subgroup -----
	IDetailGroup& TightnessGroup = DeformationGroup.AddGroup(TEXT("Tightness"), LOCTEXT("TightnessGroup", "Tightness"));

	if (TightnessStrengthHandle.IsValid())
	{
		TightnessGroup.AddPropertyRow(TightnessStrengthHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 1.0f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(1.0f);
					})
				)
			);
	}
	if (FalloffTypeHandle.IsValid())
	{
		TightnessGroup.AddPropertyRow(FalloffTypeHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						uint8 Value;
						Handle->GetValue(Value);
						return Value != static_cast<uint8>(EFalloffType::Linear);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(static_cast<uint8>(EFalloffType::Linear));
					})
				)
			);
	}
	// SDF Bounds Expand (only shown in Auto mode)
	if (SDFBoundsExpandXHandle.IsValid())
	{
		TightnessGroup.AddPropertyRow(SDFBoundsExpandXHandle.ToSharedRef())
			.IsEnabled(IsAutoModeAttr)
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyZero(Value);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(0.0f);
					})
				)
			);
	}
	if (SDFBoundsExpandYHandle.IsValid())
	{
		TightnessGroup.AddPropertyRow(SDFBoundsExpandYHandle.ToSharedRef())
			.IsEnabled(IsAutoModeAttr)
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyZero(Value);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(0.0f);
					})
				)
			);
	}

	// ----- Bulge subgroup -----
	IDetailGroup& BulgeGroup = DeformationGroup.AddGroup(TEXT("Bulge"), LOCTEXT("BulgeGroup", "Bulge"));

	if (bEnableBulgeHandle.IsValid())
	{
		BulgeGroup.AddPropertyRow(bEnableBulgeHandle.ToSharedRef());
	}
	if (BulgeDirectionHandle.IsValid())
	{
		BulgeGroup.AddPropertyRow(BulgeDirectionHandle.ToSharedRef());
	}
	if (BulgeFalloffHandle.IsValid())
	{
		BulgeGroup.AddPropertyRow(BulgeFalloffHandle.ToSharedRef());
	}
	if (BulgeIntensityHandle.IsValid())
	{
		BulgeGroup.AddPropertyRow(BulgeIntensityHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 1.0f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(1.0f);
					})
				)
			);
	}
	if (BulgeAxialRangeHandle.IsValid())
	{
		BulgeGroup.AddPropertyRow(BulgeAxialRangeHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 5.0f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(5.0f);
					})
				)
			);
	}
	if (BulgeRadialRangeHandle.IsValid())
	{
		BulgeGroup.AddPropertyRow(BulgeRadialRangeHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 1.0f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(1.0f);
					})
				)
			);
	}
	if (BulgeRadialTaperHandle.IsValid())
	{
		BulgeGroup.AddPropertyRow(BulgeRadialTaperHandle.ToSharedRef())
			.IsEnabled(IsNotVirtualBandModeAttr)
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 0.5f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(0.5f);
					})
				)
			);
	}
	if (UpperBulgeStrengthHandle.IsValid())
	{
		BulgeGroup.AddPropertyRow(UpperBulgeStrengthHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 1.0f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(1.0f);
					})
				)
			);
	}
	if (LowerBulgeStrengthHandle.IsValid())
	{
		BulgeGroup.AddPropertyRow(LowerBulgeStrengthHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 1.0f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(1.0f);
					})
				)
			);
	}
	if (BulgeRadialRatioHandle.IsValid())
	{
		BulgeGroup.AddPropertyRow(BulgeRadialRatioHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 0.7f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(0.7f);
					})
				)
			);
	}

	// ===== Post Process group (topmost) =====
	IDetailGroup& PostProcessGroup = ChildBuilder.AddGroup(TEXT("PostProcess"), LOCTEXT("PostProcessGroup", "Post Process"));
	PostProcessGroup.HeaderRow()
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PostProcessHeader", "Post Process"))
			.Font(IDetailLayoutBuilder::GetDetailFontBold())
		];

	// Post Process master toggle
	if (bEnablePostProcessHandle.IsValid())
	{
		PostProcessGroup.AddPropertyRow(bEnablePostProcessHandle.ToSharedRef());
	}

	// ===== Smoothing Volume subgroup =====
	IDetailGroup& SmoothingVolumeGroup = PostProcessGroup.AddGroup(TEXT("SmoothingVolume"), LOCTEXT("SmoothingVolumeGroup", "Smoothing Volume"));

	// Volume mode selection
	if (SmoothingVolumeModeHandle.IsValid())
	{
		SmoothingVolumeGroup.AddPropertyRow(SmoothingVolumeModeHandle.ToSharedRef());
	}
	// HopBased settings (only shown when SmoothingVolumeMode == HopBased)
	if (MaxSmoothingHopsHandle.IsValid())
	{
		SmoothingVolumeGroup.AddPropertyRow(MaxSmoothingHopsHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						int32 Value;
						Handle->GetValue(Value);
						return Value != 5;
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(5);
					})
				)
			);
	}
	// BoundsExpand settings (only shown when SmoothingVolumeMode == BoundsExpand)
	if (SmoothingBoundsZTopHandle.IsValid())
	{
		SmoothingVolumeGroup.AddPropertyRow(SmoothingBoundsZTopHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 5.0f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(5.0f);
					})
				)
			);
	}
	if (SmoothingBoundsZBottomHandle.IsValid())
	{
		SmoothingVolumeGroup.AddPropertyRow(SmoothingBoundsZBottomHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 0.0f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(0.0f);
					})
				)
			);
	}
	// Advanced (only shown in HopBased mode)
	TAttribute<EVisibility> SmoothingVolumeAdvancedVisibility = TAttribute<EVisibility>::Create([SmoothingVolumeModeHandle]() -> EVisibility
	{
		if (!SmoothingVolumeModeHandle.IsValid()) return EVisibility::Collapsed;
		uint8 ModeValue;
		SmoothingVolumeModeHandle->GetValue(ModeValue);
		return static_cast<ESmoothingVolumeMode>(ModeValue) == ESmoothingVolumeMode::HopBased
			? EVisibility::Visible : EVisibility::Collapsed;
	});
	IDetailGroup& SmoothingVolumeAdvancedGroup = SmoothingVolumeGroup.AddGroup(TEXT("SmoothingVolumeAdvanced"), LOCTEXT("SmoothingVolumeAdvancedGroup", "Advanced"));
	SmoothingVolumeAdvancedGroup.HeaderRow()
		.Visibility(SmoothingVolumeAdvancedVisibility)
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("SmoothingVolumeAdvanced", "Advanced"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		];
	if (HopFalloffTypeHandle.IsValid())
	{
		SmoothingVolumeAdvancedGroup.AddPropertyRow(HopFalloffTypeHandle.ToSharedRef())
			.Visibility(SmoothingVolumeAdvancedVisibility)
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						uint8 Value;
						Handle->GetValue(Value);
						return Value != static_cast<uint8>(EFalloffType::Hermite);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(static_cast<uint8>(EFalloffType::Hermite));
					})
				)
			);
	}

	// ===== Smoothing subgroup =====
	IDetailGroup& SmoothingGroup = PostProcessGroup.AddGroup(TEXT("Smoothing"), LOCTEXT("SmoothingGroup", "Smoothing"));

	// Smoothing master toggle
	if (bEnableSmoothingHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(bEnableSmoothingHandle.ToSharedRef());
	}

	// ===== Deformation Spread subgroup (executes before Radial) =====
	IDetailGroup& HeatPropagationGroup = SmoothingGroup.AddGroup(TEXT("DeformationSpread"), LOCTEXT("DeformationSpreadGroup", "Deformation Spread"));
	if (bEnableHeatPropagationHandle.IsValid())
	{
		HeatPropagationGroup.AddPropertyRow(bEnableHeatPropagationHandle.ToSharedRef());
	}
	if (HeatPropagationIterationsHandle.IsValid())
	{
		HeatPropagationGroup.AddPropertyRow(HeatPropagationIterationsHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						int32 Value;
						Handle->GetValue(Value);
						return Value != 10;
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(10);
					})
				)
			);
	}
	if (HeatPropagationLambdaHandle.IsValid())
	{
		HeatPropagationGroup.AddPropertyRow(HeatPropagationLambdaHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 0.5f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(0.5f);
					})
				)
			);
	}
	if (bIncludeBulgeVerticesAsSeedsHandle.IsValid())
	{
		HeatPropagationGroup.AddPropertyRow(bIncludeBulgeVerticesAsSeedsHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						bool Value;
						Handle->GetValue(Value);
						return Value != true;
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(true);
					})
				)
			);
	}

	// ===== Radial subgroup =====
	IDetailGroup& RadialGroup = SmoothingGroup.AddGroup(TEXT("Radial"), LOCTEXT("RadialGroup", "Radial"));
	if (bEnableRadialSmoothingHandle.IsValid())
	{
		RadialGroup.AddPropertyRow(bEnableRadialSmoothingHandle.ToSharedRef());
	}
	if (RadialBlendStrengthHandle.IsValid())
	{
		RadialGroup.AddPropertyRow(RadialBlendStrengthHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 1.0f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(1.0f);
					})
				)
			);
	}
	// Advanced (only shown when Radial Smoothing is enabled)
	TAttribute<EVisibility> RadialAdvancedVisibility = TAttribute<EVisibility>::Create([bEnableRadialSmoothingHandle]() -> EVisibility
	{
		if (!bEnableRadialSmoothingHandle.IsValid()) return EVisibility::Collapsed;
		bool bEnabled = false;
		bEnableRadialSmoothingHandle->GetValue(bEnabled);
		return bEnabled ? EVisibility::Visible : EVisibility::Collapsed;
	});
	IDetailGroup& RadialAdvancedGroup = RadialGroup.AddGroup(TEXT("RadialAdvanced"), LOCTEXT("RadialAdvancedGroup", "Advanced"));
	RadialAdvancedGroup.HeaderRow()
		.Visibility(RadialAdvancedVisibility)
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("RadialAdvanced", "Advanced"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		];
	if (RadialSliceHeightHandle.IsValid())
	{
		RadialAdvancedGroup.AddPropertyRow(RadialSliceHeightHandle.ToSharedRef())
			.Visibility(RadialAdvancedVisibility)
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 1.0f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(1.0f);
					})
				)
			);
	}

	// ===== Surface Smoothing subgroup =====
	IDetailGroup& LaplacianGroup = SmoothingGroup.AddGroup(TEXT("SurfaceSmoothing"), LOCTEXT("SurfaceSmoothingGroup", "Surface Smoothing"));
	if (bEnableLaplacianSmoothingHandle.IsValid())
	{
		LaplacianGroup.AddPropertyRow(bEnableLaplacianSmoothingHandle.ToSharedRef());
	}
	// Common parameters (Iterations → Lambda order)
	if (SmoothingIterationsHandle.IsValid())
	{
		LaplacianGroup.AddPropertyRow(SmoothingIterationsHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						int32 Value;
						Handle->GetValue(Value);
						return Value != 2;
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(2);
					})
				)
			);
	}
	if (SmoothingLambdaHandle.IsValid())
	{
		LaplacianGroup.AddPropertyRow(SmoothingLambdaHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 0.5f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(0.5f);
					})
				)
			);
	}
	// Algorithm selection
	if (LaplacianSmoothingTypeHandle.IsValid())
	{
		LaplacianGroup.AddPropertyRow(LaplacianSmoothingTypeHandle.ToSharedRef());
	}
	// Anchor mode (pin directly deformed vertices)
	if (bAnchorDeformedVerticesHandle.IsValid())
	{
		LaplacianGroup.AddPropertyRow(bAnchorDeformedVerticesHandle.ToSharedRef());
	}
	// Advanced (only shown when Surface Smoothing + Taubin type)
	TAttribute<EVisibility> LaplacianAdvancedVisibility = TAttribute<EVisibility>::Create([bEnableLaplacianSmoothingHandle, LaplacianSmoothingTypeHandle]() -> EVisibility
	{
		if (!bEnableLaplacianSmoothingHandle.IsValid() || !LaplacianSmoothingTypeHandle.IsValid())
			return EVisibility::Collapsed;
		bool bEnabled = false;
		bEnableLaplacianSmoothingHandle->GetValue(bEnabled);
		if (!bEnabled) return EVisibility::Collapsed;
		uint8 TypeValue;
		LaplacianSmoothingTypeHandle->GetValue(TypeValue);
		return static_cast<ELaplacianSmoothingType>(TypeValue) == ELaplacianSmoothingType::Taubin
			? EVisibility::Visible : EVisibility::Collapsed;
	});
	IDetailGroup& LaplacianAdvancedGroup = LaplacianGroup.AddGroup(TEXT("SurfaceSmoothingAdvanced"), LOCTEXT("SurfaceSmoothingAdvancedGroup", "Advanced"));
	LaplacianAdvancedGroup.HeaderRow()
		.Visibility(LaplacianAdvancedVisibility)
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("LaplacianAdvanced", "Advanced"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		];
	if (TaubinMuHandle.IsValid())
	{
		LaplacianAdvancedGroup.AddPropertyRow(TaubinMuHandle.ToSharedRef())
			.Visibility(LaplacianAdvancedVisibility)
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, -0.53f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(-0.53f);
					})
				)
			);
	}

	// ===== Edge Length Preservation subgroup =====
	IDetailGroup& PBDGroup = PostProcessGroup.AddGroup(TEXT("EdgeLengthPreservation"), LOCTEXT("EdgeLengthPreservationGroup", "Edge Length Preservation"));

	if (bEnablePBDEdgeConstraintHandle.IsValid())
	{
		PBDGroup.AddPropertyRow(bEnablePBDEdgeConstraintHandle.ToSharedRef());
	}
	if (bPBDAnchorAffectedVerticesHandle.IsValid())
	{
		PBDGroup.AddPropertyRow(bPBDAnchorAffectedVerticesHandle.ToSharedRef());
	}
	if (PBDStiffnessHandle.IsValid())
	{
		PBDGroup.AddPropertyRow(PBDStiffnessHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 0.8f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(0.8f);
					})
				)
			);
	}
	if (PBDIterationsHandle.IsValid())
	{
		PBDGroup.AddPropertyRow(PBDIterationsHandle.ToSharedRef())
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						int32 Value;
						Handle->GetValue(Value);
						return Value != 5;
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(5);
					})
				)
			);
	}
	// Advanced (only shown when Edge Length Preservation is enabled)
	TAttribute<EVisibility> PBDAdvancedVisibility = TAttribute<EVisibility>::Create([bEnablePBDEdgeConstraintHandle]() -> EVisibility
	{
		if (!bEnablePBDEdgeConstraintHandle.IsValid()) return EVisibility::Collapsed;
		bool bEnabled = false;
		bEnablePBDEdgeConstraintHandle->GetValue(bEnabled);
		return bEnabled ? EVisibility::Visible : EVisibility::Collapsed;
	});
	IDetailGroup& PBDAdvancedGroup = PBDGroup.AddGroup(TEXT("EdgeLengthPreservationAdvanced"), LOCTEXT("EdgeLengthPreservationAdvancedGroup", "Advanced"));
	PBDAdvancedGroup.HeaderRow()
		.Visibility(PBDAdvancedVisibility)
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PBDAdvanced", "Advanced"))
			.Font(IDetailLayoutBuilder::GetDetailFont())
		];
	if (PBDToleranceHandle.IsValid())
	{
		PBDAdvancedGroup.AddPropertyRow(PBDToleranceHandle.ToSharedRef())
			.Visibility(PBDAdvancedVisibility)
			.OverrideResetToDefault(
				FResetToDefaultOverride::Create(
					FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						float Value;
						Handle->GetValue(Value);
						return !FMath::IsNearlyEqual(Value, 0.2f);
					}),
					FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
						Handle->SetValue(0.2f);
					})
				)
			);
	}
}

USkeletalMesh* FFleshRingSettingsCustomization::GetTargetSkeletalMesh() const
{
	UFleshRingAsset* Asset = GetOuterAsset();
	if (Asset)
	{
		return Asset->TargetSkeletalMesh.LoadSynchronous();
	}
	return nullptr;
}

UFleshRingAsset* FFleshRingSettingsCustomization::GetOuterAsset() const
{
	if (!MainPropertyHandle.IsValid())
	{
		return nullptr;
	}

	// Walk up PropertyHandle chain to find UFleshRingAsset
	// FFleshRingSettings -> Rings array -> UFleshRingAsset
	TArray<UObject*> OuterObjects;
	MainPropertyHandle->GetOuterObjects(OuterObjects);

	for (UObject* Obj : OuterObjects)
	{
		if (UFleshRingAsset* Asset = Cast<UFleshRingAsset>(Obj))
		{
			return Asset;
		}
	}

	return nullptr;
}

FReply FFleshRingSettingsCustomization::OnHeaderClicked(int32 RingIndex)
{
	// Call Asset's SetEditorSelectedRingIndex (broadcasts delegate)
	if (UFleshRingAsset* Asset = GetOuterAsset())
	{
		// Determine SelectionType based on RingMesh presence
		// No RingMesh = Gizmo selection (virtual ring/band), with RingMesh = Mesh selection
		EFleshRingSelectionType SelectionType = EFleshRingSelectionType::Mesh;
		if (Asset->Rings.IsValidIndex(RingIndex) && Asset->Rings[RingIndex].RingMesh.IsNull())
		{
			SelectionType = EFleshRingSelectionType::Gizmo;
		}

		FScopedTransaction Transaction(LOCTEXT("SelectRingFromDetails", "Select Ring"));
		Asset->Modify();
		Asset->SetEditorSelectedRingIndex(RingIndex, SelectionType);
	}
	return FReply::Handled();
}

FText FFleshRingSettingsCustomization::GetDisplayRingName(int32 Index) const
{
	if (UFleshRingAsset* Asset = GetOuterAsset())
	{
		if (Asset->Rings.IsValidIndex(Index))
		{
			return FText::FromString(Asset->Rings[Index].GetDisplayName(Index));
		}
	}
	return FText::Format(LOCTEXT("DefaultRingName", "FleshRing_{0}"), FText::AsNumber(Index));
}

void FFleshRingSettingsCustomization::OnHeaderClickedVoid()
{
	OnHeaderClicked(CachedArrayIndex);
}

void FFleshRingSettingsCustomization::OnRingNameCommitted(const FText& NewText, ETextCommit::Type CommitType)
{
	if (CommitType == ETextCommit::OnEnter || CommitType == ETextCommit::OnUserMovedFocus)
	{
		if (UFleshRingAsset* Asset = GetOuterAsset())
		{
			if (Asset->Rings.IsValidIndex(CachedArrayIndex))
			{
				// Already validated in widget, so apply directly
				FScopedTransaction Transaction(LOCTEXT("RenameRing", "Rename Ring"));
				Asset->Modify();
				Asset->Rings[CachedArrayIndex].RingName = FName(*NewText.ToString());
				Asset->PostEditChange();

				// Update other UI like skeleton tree
				Asset->OnAssetChanged.Broadcast(Asset);
			}
		}
	}
}

bool FFleshRingSettingsCustomization::IsThisRingSelected() const
{
	if (UFleshRingAsset* Asset = GetOuterAsset())
	{
		return Asset->EditorSelectedRingIndex == CachedArrayIndex;
	}
	return false;
}

FSlateColor FFleshRingSettingsCustomization::GetHeaderBackgroundColor() const
{
	if (IsThisRingSelected())
	{
		// Highlight color when selected (similar to Unreal Editor's selection color)
		return FLinearColor(0.1f, 0.4f, 0.7f, 0.3f);
	}
	return FLinearColor::Transparent;
}

const FSlateBrush* FFleshRingSettingsCustomization::GetVisibilityIcon() const
{
	UFleshRingAsset* Asset = GetOuterAsset();
	if (Asset && Asset->Rings.IsValidIndex(CachedArrayIndex))
	{
		bool bVisible = Asset->Rings[CachedArrayIndex].bEditorVisible;
		return FAppStyle::GetBrush(bVisible ? "Icons.Visible" : "Icons.Hidden");
	}
	return FAppStyle::GetBrush("Icons.Visible");
}

FReply FFleshRingSettingsCustomization::OnVisibilityToggleClicked()
{
	UFleshRingAsset* Asset = GetOuterAsset();
	if (Asset && Asset->Rings.IsValidIndex(CachedArrayIndex))
	{
		FScopedTransaction Transaction(LOCTEXT("ToggleRingVisibility", "Toggle Ring Visibility"));
		Asset->Modify();

		Asset->Rings[CachedArrayIndex].bEditorVisible = !Asset->Rings[CachedArrayIndex].bEditorVisible;

		// Asset change notification (for editor viewport update)
		Asset->OnAssetChanged.Broadcast(Asset);
	}
	return FReply::Handled();
}

void FFleshRingSettingsCustomization::BuildBoneTree()
{
	BoneTreeRoots.Empty();
	AllBoneItems.Empty();
	FilteredBoneTreeRoots.Empty();

	USkeletalMesh* SkeletalMesh = GetTargetSkeletalMesh();
	if (!SkeletalMesh)
	{
		return;
	}

	// Build weighted bone cache
	BuildWeightedBoneCache(SkeletalMesh);

	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
	const int32 NumBones = RefSkeleton.GetNum();

	// Recursive function to check if any descendant is weighted
	TFunction<bool(int32)> HasWeightedDescendant = [&](int32 BoneIndex) -> bool
	{
		if (IsBoneWeighted(BoneIndex))
		{
			return true;
		}

		// Check child bones
		for (int32 ChildIdx = 0; ChildIdx < NumBones; ++ChildIdx)
		{
			if (RefSkeleton.GetParentIndex(ChildIdx) == BoneIndex)
			{
				if (HasWeightedDescendant(ChildIdx))
				{
					return true;
				}
			}
		}
		return false;
	};

	// Create items for all bones
	AllBoneItems.SetNum(NumBones);
	for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
	{
		FName BoneName = RefSkeleton.GetBoneName(BoneIdx);
		bool bIsMeshBone = HasWeightedDescendant(BoneIdx);
		AllBoneItems[BoneIdx] = FBoneDropdownItem::Create(BoneName, BoneIdx, bIsMeshBone);
	}

	// Set up parent-child relationships
	for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
	{
		int32 ParentIdx = RefSkeleton.GetParentIndex(BoneIdx);
		if (ParentIdx != INDEX_NONE && AllBoneItems.IsValidIndex(ParentIdx))
		{
			AllBoneItems[ParentIdx]->Children.Add(AllBoneItems[BoneIdx]);
			AllBoneItems[BoneIdx]->ParentItem = AllBoneItems[ParentIdx];
		}
		else
		{
			// Root bone
			BoneTreeRoots.Add(AllBoneItems[BoneIdx]);
		}
	}

	// Apply initial filtering
	ApplySearchFilter();
}

void FFleshRingSettingsCustomization::BuildWeightedBoneCache(USkeletalMesh* SkelMesh)
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

bool FFleshRingSettingsCustomization::IsBoneWeighted(int32 BoneIndex) const
{
	return WeightedBoneIndices.Contains(BoneIndex);
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateSearchableBoneDropdown()
{
	return SAssignNew(BoneComboButton, SComboButton)
		.OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
		{
			// Rebuild bone tree when dropdown opens
			BuildBoneTree();
			BoneSearchText.Empty();

			TSharedRef<SWidget> MenuContent = SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(4.0f)
				[
					SNew(SSearchBox)
					.HintText(LOCTEXT("SearchBoneHint", "Search Bone..."))
					.OnTextChanged(this, &FFleshRingSettingsCustomization::OnBoneSearchTextChanged)
				]
				+ SVerticalBox::Slot()
				.MaxHeight(400.0f)
				[
					SAssignNew(BoneTreeView, STreeView<TSharedPtr<FBoneDropdownItem>>)
					.TreeItemsSource(&FilteredBoneTreeRoots)
					.OnGenerateRow(this, &FFleshRingSettingsCustomization::GenerateBoneTreeRow)
					.OnGetChildren(this, &FFleshRingSettingsCustomization::GetBoneTreeChildren)
					.OnSelectionChanged(this, &FFleshRingSettingsCustomization::OnBoneTreeSelectionChanged)
					.SelectionMode(ESelectionMode::Single)
				];

			// Expand all items after tree creation
			ExpandAllBoneTreeItems();

			return MenuContent;
		})
		.ButtonContent()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0, 0, 4, 0)
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.Warning"))
				.Visibility_Lambda([this]()
				{
					return IsBoneInvalid() ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.ColorAndOpacity(FLinearColor::Yellow)
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(this, &FFleshRingSettingsCustomization::GetCurrentBoneName)
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		];
}

void FFleshRingSettingsCustomization::OnBoneSearchTextChanged(const FText& NewText)
{
	BoneSearchText = NewText.ToString();
	ApplySearchFilter();

	if (BoneTreeView.IsValid())
	{
		// Fully regenerate rows with RebuildList (highlight update)
		BoneTreeView->RebuildList();

		// Expand all items
		ExpandAllBoneTreeItems();
	}
}

void FFleshRingSettingsCustomization::ApplySearchFilter()
{
	FilteredBoneTreeRoots.Empty();

	// If no search text, filter to show only weighted bones
	if (BoneSearchText.IsEmpty())
	{
		for (const auto& Root : BoneTreeRoots)
		{
			if (Root->bIsMeshBone)
			{
				FilteredBoneTreeRoots.Add(Root);
			}
		}
	}
	else
	{
		// Even with search text, show only weighted bones
		for (const auto& Root : BoneTreeRoots)
		{
			if (!Root->bIsMeshBone)
			{
				continue;
			}

			if (Root->BoneName.ToString().Contains(BoneSearchText, ESearchCase::IgnoreCase))
			{
				FilteredBoneTreeRoots.Add(Root);
			}
			else
			{
				// If any child has a matching weighted bone, show parent too
				TFunction<bool(const TSharedPtr<FBoneDropdownItem>&)> HasMatchingChild;
				HasMatchingChild = [&](const TSharedPtr<FBoneDropdownItem>& Item) -> bool
				{
					for (const auto& Child : Item->Children)
					{
						if (!Child->bIsMeshBone)
						{
							continue;
						}
						if (Child->BoneName.ToString().Contains(BoneSearchText, ESearchCase::IgnoreCase))
						{
							return true;
						}
						if (HasMatchingChild(Child))
						{
							return true;
						}
					}
					return false;
				};

				if (HasMatchingChild(Root))
				{
					FilteredBoneTreeRoots.Add(Root);
				}
			}
		}
	}
}

TSharedRef<ITableRow> FFleshRingSettingsCustomization::GenerateBoneTreeRow(TSharedPtr<FBoneDropdownItem> InItem, const TSharedRef<STableViewBase>& OwnerTable)
{
	return SNew(SBoneDropdownTreeRow, OwnerTable)
		.Item(InItem)
		.HighlightText(FText::FromString(BoneSearchText));
}

void FFleshRingSettingsCustomization::ExpandAllBoneTreeItems()
{
	if (!BoneTreeView.IsValid())
	{
		return;
	}

	// Recursively expand all items
	TFunction<void(const TSharedPtr<FBoneDropdownItem>&)> ExpandRecursive;
	ExpandRecursive = [&](const TSharedPtr<FBoneDropdownItem>& Item)
	{
		if (!Item.IsValid())
		{
			return;
		}
		BoneTreeView->SetItemExpansion(Item, true);
		for (const auto& Child : Item->Children)
		{
			if (Child->bIsMeshBone || !BoneSearchText.IsEmpty())
			{
				ExpandRecursive(Child);
			}
		}
	};

	for (const auto& Root : FilteredBoneTreeRoots)
	{
		ExpandRecursive(Root);
	}
}

void FFleshRingSettingsCustomization::GetBoneTreeChildren(TSharedPtr<FBoneDropdownItem> Item, TArray<TSharedPtr<FBoneDropdownItem>>& OutChildren)
{
	if (!Item.IsValid())
	{
		return;
	}

	// If no search text, show only weighted bones
	if (BoneSearchText.IsEmpty())
	{
		for (const auto& Child : Item->Children)
		{
			if (Child->bIsMeshBone)
			{
				OutChildren.Add(Child);
			}
		}
	}
	else
	{
		// Even with search text, show only weighted bones
		TFunction<bool(const TSharedPtr<FBoneDropdownItem>&)> HasMatchingDescendant;
		HasMatchingDescendant = [&](const TSharedPtr<FBoneDropdownItem>& CheckItem) -> bool
		{
			if (!CheckItem->bIsMeshBone)
			{
				return false;
			}
			if (CheckItem->BoneName.ToString().Contains(BoneSearchText, ESearchCase::IgnoreCase))
			{
				return true;
			}
			for (const auto& Child : CheckItem->Children)
			{
				if (HasMatchingDescendant(Child))
				{
					return true;
				}
			}
			return false;
		};

		for (const auto& Child : Item->Children)
		{
			if (Child->bIsMeshBone && HasMatchingDescendant(Child))
			{
				OutChildren.Add(Child);
			}
		}
	}
}

void FFleshRingSettingsCustomization::OnBoneTreeSelectionChanged(TSharedPtr<FBoneDropdownItem> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (BoneNameHandle.IsValid() && NewSelection.IsValid())
	{
		// Only weighted bones can be selected
		if (NewSelection->bIsMeshBone)
		{
			BoneNameHandle->SetValue(NewSelection->BoneName);

			// Close dropdown
			if (BoneComboButton.IsValid())
			{
				BoneComboButton->SetIsOpen(false);
			}
		}
	}
}

bool FFleshRingSettingsCustomization::IsBoneInvalid() const
{
	if (!BoneNameHandle.IsValid())
	{
		return false;
	}

	FName CurrentValue;
	BoneNameHandle->GetValue(CurrentValue);

	// None is not a warning (not yet selected)
	if (CurrentValue == NAME_None)
	{
		return false;
	}

	// Find bone in SkeletalMesh
	USkeletalMesh* SkeletalMesh = const_cast<FFleshRingSettingsCustomization*>(this)->GetTargetSkeletalMesh();
	if (!SkeletalMesh)
	{
		// Warn if no SkeletalMesh
		return true;
	}

	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
	int32 BoneIndex = RefSkeleton.FindBoneIndex(CurrentValue);

	if (BoneIndex == INDEX_NONE)
	{
		return true;
	}

	// Also warn for non-weighted bones (skip check if AllBoneItems is empty)
	if (AllBoneItems.IsValidIndex(BoneIndex))
	{
		return !AllBoneItems[BoneIndex]->bIsMeshBone;
	}

	return false;
}

FText FFleshRingSettingsCustomization::GetCurrentBoneName() const
{
	if (BoneNameHandle.IsValid())
	{
		FName CurrentValue;
		BoneNameHandle->GetValue(CurrentValue);

		if (CurrentValue == NAME_None)
		{
			return LOCTEXT("SelectBone", "Select Bone...");
		}

		// Check if currently selected bone exists in SkeletalMesh
		USkeletalMesh* SkeletalMesh = const_cast<FFleshRingSettingsCustomization*>(this)->GetTargetSkeletalMesh();
		if (SkeletalMesh)
		{
			const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
			int32 BoneIndex = RefSkeleton.FindBoneIndex(CurrentValue);

			if (BoneIndex == INDEX_NONE)
			{
				// Show warning if bone doesn't exist
				return FText::Format(
					LOCTEXT("BoneNotFound", "{0} (Not Found)"),
					FText::FromName(CurrentValue));
			}

			// Warn for non-weighted bones (skip check if AllBoneItems is empty)
			if (AllBoneItems.IsValidIndex(BoneIndex) && !AllBoneItems[BoneIndex]->bIsMeshBone)
			{
				return FText::Format(
					LOCTEXT("BoneNotWeighted", "{0} (No Weight)"),
					FText::FromName(CurrentValue));
			}
		}
		else
		{
			// If SkeletalMesh is not set
			return FText::Format(
				LOCTEXT("NoSkeletalMesh", "{0} (No Mesh)"),
				FText::FromName(CurrentValue));
		}

		return FText::FromName(CurrentValue);
	}
	return LOCTEXT("InvalidBone", "Invalid");
}

void FFleshRingSettingsCustomization::SyncQuatFromEuler(
	TSharedPtr<IPropertyHandle> EulerHandle,
	TSharedPtr<IPropertyHandle> QuatHandle)
{
	if (!EulerHandle.IsValid() || !QuatHandle.IsValid())
	{
		return;
	}

	// Read Euler
	FRotator Euler;
	EulerHandle->EnumerateRawData([&Euler](void* RawData, const int32 DataIndex, const int32 NumDatas)
	{
		if (RawData)
		{
			Euler = *static_cast<FRotator*>(RawData);
			return false;
		}
		return true;
	});

	// Write to Quat
	FQuat Quat = Euler.Quaternion();
	QuatHandle->EnumerateRawData([&Quat](void* RawData, const int32 DataIndex, const int32 NumDatas)
	{
		if (RawData)
		{
			*static_cast<FQuat*>(RawData) = Quat;
		}
		return true;
	});

	// Change notification (triggers preview update)
	QuatHandle->NotifyPostChange(EPropertyChangeType::ValueSet);
}

FRotator FFleshRingSettingsCustomization::GetQuatAsEuler(TSharedPtr<IPropertyHandle> QuatHandle) const
{
	if (!QuatHandle.IsValid())
	{
		return FRotator::ZeroRotator;
	}

	void* Data = nullptr;
	if (QuatHandle->GetValueData(Data) == FPropertyAccess::Success && Data)
	{
		FQuat Quat = *static_cast<FQuat*>(Data);
		return Quat.Rotator();
	}

	return FRotator::ZeroRotator;
}

void FFleshRingSettingsCustomization::SetEulerToQuat(TSharedPtr<IPropertyHandle> QuatHandle, const FRotator& Euler)
{
	if (!QuatHandle.IsValid())
	{
		return;
	}

	void* Data = nullptr;
	if (QuatHandle->GetValueData(Data) == FPropertyAccess::Success && Data)
	{
		FQuat& Quat = *static_cast<FQuat*>(Data);
		Quat = Euler.Quaternion();
		QuatHandle->NotifyPostChange(EPropertyChangeType::ValueSet);
	}
}

void FFleshRingSettingsCustomization::AddLinearVectorRow(
	IDetailChildrenBuilder& ChildBuilder,
	TSharedRef<IPropertyHandle> VectorHandle,
	const FText& DisplayName,
	float Delta,
	TAttribute<bool> IsEnabled)
{
	TSharedPtr<IPropertyHandle> VecHandlePtr = VectorHandle.ToSharedPtr();

	// Read FVector directly with EnumerateRawData
	auto GetVector = [VecHandlePtr]() -> FVector
	{
		FVector Result = FVector::ZeroVector;
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FVector*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	// Write FVector directly with EnumerateRawData
	// NotifyPreChange is managed by caller (slider: OnBeginSliderMovement, text: OnValueCommitted)
	auto SetVector = [VecHandlePtr](const FVector& NewValue, EPropertyChangeType::Type ChangeType = EPropertyChangeType::ValueSet)
	{
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FVector*>(RawData) = NewValue;
				}
				return true;
			});
			VecHandlePtr->NotifyPostChange(ChangeType);
		}
	};

	ChildBuilder.AddCustomRow(DisplayName)
	.IsEnabled(IsEnabled)
	.NameContent()
	[
		SNew(STextBlock)
		.Text(DisplayName)
		.Font(IDetailLayoutBuilder::GetDetailFont())
	]
	.ValueContent()
	.MinDesiredWidth(300.0f)
	[
		SNew(SHorizontalBox)
		// X
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double
				{
					return GetVector().X;
				})
				.OnBeginSliderMovement_Lambda([VecHandlePtr]()
				{
					// Create Undo point at drag start
					if (VecHandlePtr.IsValid())
					{
						VecHandlePtr->NotifyPreChange();
					}
				})
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.X = NewValue;
					SetVector(Vec, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetVector, SetVector](double FinalValue)
				{
					// Commit with final value at drag end (finalize Undo point)
					FVector Vec = GetVector();
					Vec.X = FinalValue;
					SetVector(Vec, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([VecHandlePtr, GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					// Create Undo point and set value on text input
					if (VecHandlePtr.IsValid())
					{
						VecHandlePtr->NotifyPreChange();
					}
					FVector Vec = GetVector();
					Vec.X = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Y
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double
				{
					return GetVector().Y;
				})
				.OnBeginSliderMovement_Lambda([VecHandlePtr]()
				{
					if (VecHandlePtr.IsValid())
					{
						VecHandlePtr->NotifyPreChange();
					}
				})
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.Y = NewValue;
					SetVector(Vec, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetVector, SetVector](double FinalValue)
				{
					FVector Vec = GetVector();
					Vec.Y = FinalValue;
					SetVector(Vec, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([VecHandlePtr, GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					if (VecHandlePtr.IsValid())
					{
						VecHandlePtr->NotifyPreChange();
					}
					FVector Vec = GetVector();
					Vec.Y = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Z
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double
				{
					return GetVector().Z;
				})
				.OnBeginSliderMovement_Lambda([VecHandlePtr]()
				{
					if (VecHandlePtr.IsValid())
					{
						VecHandlePtr->NotifyPreChange();
					}
				})
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.Z = NewValue;
					SetVector(Vec, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetVector, SetVector](double FinalValue)
				{
					FVector Vec = GetVector();
					Vec.Z = FinalValue;
					SetVector(Vec, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([VecHandlePtr, GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					if (VecHandlePtr.IsValid())
					{
						VecHandlePtr->NotifyPreChange();
					}
					FVector Vec = GetVector();
					Vec.Z = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
	];
}

void FFleshRingSettingsCustomization::AddLinearRotatorRow(
	IDetailChildrenBuilder& ChildBuilder,
	TSharedRef<IPropertyHandle> RotatorHandle,
	const FText& DisplayName,
	float Delta,
	TAttribute<bool> IsEnabled)
{
	TSharedPtr<IPropertyHandle> RotHandlePtr = RotatorHandle.ToSharedPtr();

	// Read FRotator directly with EnumerateRawData
	auto GetRotator = [RotHandlePtr]() -> FRotator
	{
		FRotator Result = FRotator::ZeroRotator;
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FRotator*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	// Write FRotator directly with EnumerateRawData
	// NotifyPreChange is managed by caller (slider: OnBeginSliderMovement, text: OnValueCommitted)
	auto SetRotator = [RotHandlePtr](const FRotator& NewValue, EPropertyChangeType::Type ChangeType = EPropertyChangeType::ValueSet)
	{
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FRotator*>(RawData) = NewValue;
				}
				return true;
			});
			RotHandlePtr->NotifyPostChange(ChangeType);
		}
	};

	// TypeInterface for angle display
	auto DegreeInterface = MakeShared<FDegreeTypeInterface>();

	ChildBuilder.AddCustomRow(DisplayName)
	.IsEnabled(IsEnabled)
	.NameContent()
	[
		SNew(STextBlock)
		.Text(DisplayName)
		.Font(IDetailLayoutBuilder::GetDetailFont())
	]
	.ValueContent()
	.MinDesiredWidth(300.0f)
	[
		SNew(SHorizontalBox)
		// Roll (X)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double
				{
					return GetRotator().Roll;
				})
				.OnBeginSliderMovement_Lambda([RotHandlePtr]()
				{
					if (RotHandlePtr.IsValid())
					{
						RotHandlePtr->NotifyPreChange();
					}
				})
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Roll = NewValue;
					SetRotator(Rot, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetRotator, SetRotator](double FinalValue)
				{
					// Commit with final value at drag end (finalize Undo point)
					FRotator Rot = GetRotator();
					Rot.Roll = FinalValue;
					SetRotator(Rot, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([RotHandlePtr, GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					// Create Undo point and set value on text input
					if (RotHandlePtr.IsValid())
					{
						RotHandlePtr->NotifyPreChange();
					}
					FRotator Rot = GetRotator();
					Rot.Roll = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Pitch (Y)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double
				{
					return GetRotator().Pitch;
				})
				.OnBeginSliderMovement_Lambda([RotHandlePtr]()
				{
					if (RotHandlePtr.IsValid())
					{
						RotHandlePtr->NotifyPreChange();
					}
				})
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Pitch = NewValue;
					SetRotator(Rot, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetRotator, SetRotator](double FinalValue)
				{
					FRotator Rot = GetRotator();
					Rot.Pitch = FinalValue;
					SetRotator(Rot, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([RotHandlePtr, GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					if (RotHandlePtr.IsValid())
					{
						RotHandlePtr->NotifyPreChange();
					}
					FRotator Rot = GetRotator();
					Rot.Pitch = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Yaw (Z)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double
				{
					return GetRotator().Yaw;
				})
				.OnBeginSliderMovement_Lambda([RotHandlePtr]()
				{
					if (RotHandlePtr.IsValid())
					{
						RotHandlePtr->NotifyPreChange();
					}
				})
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Yaw = NewValue;
					SetRotator(Rot, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetRotator, SetRotator](double FinalValue)
				{
					FRotator Rot = GetRotator();
					Rot.Yaw = FinalValue;
					SetRotator(Rot, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([RotHandlePtr, GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					if (RotHandlePtr.IsValid())
					{
						RotHandlePtr->NotifyPreChange();
					}
					FRotator Rot = GetRotator();
					Rot.Yaw = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
	];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateLinearVectorWidget(
	TSharedRef<IPropertyHandle> VectorHandle,
	float Delta)
{
	TSharedPtr<IPropertyHandle> VecHandlePtr = VectorHandle.ToSharedPtr();

	// For drag transaction management (wrapped in TSharedPtr for safe lambda use)
	TSharedPtr<TUniquePtr<FScopedTransaction>> TransactionHolder = MakeShared<TUniquePtr<FScopedTransaction>>();

	// Read real-time memory value with EnumerateRawData
	auto GetVector = [VecHandlePtr]() -> FVector
	{
		FVector Result = FVector::ZeroVector;
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FVector*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	// For fast updates during drag
	auto SetVectorInteractive = [VecHandlePtr](const FVector& NewValue)
	{
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FVector*>(RawData) = NewValue;
				}
				return true;
			});
			VecHandlePtr->NotifyPostChange(EPropertyChangeType::Interactive);
		}
	};

	// Drag start: Begin transaction + call Modify
	auto BeginTransaction = [VecHandlePtr, TransactionHolder]()
	{
		if (VecHandlePtr.IsValid())
		{
			// Start new transaction
			*TransactionHolder = MakeUnique<FScopedTransaction>(LOCTEXT("DragVector", "Drag Vector Value"));

			// Call UObject's Modify() - this state will be restored on Undo
			TArray<UObject*> OuterObjects;
			VecHandlePtr->GetOuterObjects(OuterObjects);
			for (UObject* Obj : OuterObjects)
			{
				if (Obj)
				{
					Obj->Modify();
				}
			}
		}
	};

	// Drag end: Commit transaction
	auto EndTransaction = [TransactionHolder]()
	{
		TransactionHolder->Reset();
	};

	return SNew(SHorizontalBox)
		// X
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.MinFractionalDigits(1)
				.MaxFractionalDigits(6)
				.Value_Lambda([GetVector]() -> double { return GetVector().X; })
				.OnBeginSliderMovement_Lambda([BeginTransaction]()
				{
					BeginTransaction();
				})
				.OnValueChanged_Lambda([GetVector, SetVectorInteractive](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.X = NewValue;
					SetVectorInteractive(Vec);
				})
				.OnEndSliderMovement_Lambda([VecHandlePtr, EndTransaction](double FinalValue)
				{
					// Notify change completion with NotifyPostChange(ValueSet)
					if (VecHandlePtr.IsValid())
					{
						VecHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
					}
					EndTransaction();
				})
				.OnValueCommitted_Lambda([VecHandlePtr, GetVector](double NewValue, ETextCommit::Type)
				{
					if (VecHandlePtr.IsValid())
					{
						FVector Vec = GetVector();
						Vec.X = NewValue;
						VecHandlePtr->SetValue(Vec);
					}
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Y
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.MinFractionalDigits(1)
				.MaxFractionalDigits(6)
				.Value_Lambda([GetVector]() -> double { return GetVector().Y; })
				.OnBeginSliderMovement_Lambda([BeginTransaction]()
				{
					BeginTransaction();
				})
				.OnValueChanged_Lambda([GetVector, SetVectorInteractive](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.Y = NewValue;
					SetVectorInteractive(Vec);
				})
				.OnEndSliderMovement_Lambda([VecHandlePtr, EndTransaction](double FinalValue)
				{
					if (VecHandlePtr.IsValid())
					{
						VecHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
					}
					EndTransaction();
				})
				.OnValueCommitted_Lambda([VecHandlePtr, GetVector](double NewValue, ETextCommit::Type)
				{
					if (VecHandlePtr.IsValid())
					{
						FVector Vec = GetVector();
						Vec.Y = NewValue;
						VecHandlePtr->SetValue(Vec);
					}
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Z
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.MinFractionalDigits(1)
				.MaxFractionalDigits(6)
				.Value_Lambda([GetVector]() -> double { return GetVector().Z; })
				.OnBeginSliderMovement_Lambda([BeginTransaction]()
				{
					BeginTransaction();
				})
				.OnValueChanged_Lambda([GetVector, SetVectorInteractive](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.Z = NewValue;
					SetVectorInteractive(Vec);
				})
				.OnEndSliderMovement_Lambda([VecHandlePtr, EndTransaction](double FinalValue)
				{
					if (VecHandlePtr.IsValid())
					{
						VecHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
					}
					EndTransaction();
				})
				.OnValueCommitted_Lambda([VecHandlePtr, GetVector](double NewValue, ETextCommit::Type)
				{
					if (VecHandlePtr.IsValid())
					{
						FVector Vec = GetVector();
						Vec.Z = NewValue;
						VecHandlePtr->SetValue(Vec);
					}
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateLinearRotatorWidget(
	TSharedRef<IPropertyHandle> RotatorHandle,
	float Delta)
{
	TSharedPtr<IPropertyHandle> RotHandlePtr = RotatorHandle.ToSharedPtr();

	// For drag transaction management
	TSharedPtr<TUniquePtr<FScopedTransaction>> TransactionHolder = MakeShared<TUniquePtr<FScopedTransaction>>();

	// Read real-time memory value with EnumerateRawData
	auto GetRotator = [RotHandlePtr]() -> FRotator
	{
		FRotator Result = FRotator::ZeroRotator;
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FRotator*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	// For fast updates during drag
	auto SetRotatorInteractive = [RotHandlePtr](const FRotator& NewValue)
	{
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FRotator*>(RawData) = NewValue;
				}
				return true;
			});
			RotHandlePtr->NotifyPostChange(EPropertyChangeType::Interactive);
		}
	};

	// Drag start: Begin transaction + call Modify
	auto BeginTransaction = [RotHandlePtr, TransactionHolder]()
	{
		if (RotHandlePtr.IsValid())
		{
			*TransactionHolder = MakeUnique<FScopedTransaction>(LOCTEXT("DragRotator", "Drag Rotator Value"));

			TArray<UObject*> OuterObjects;
			RotHandlePtr->GetOuterObjects(OuterObjects);
			for (UObject* Obj : OuterObjects)
			{
				if (Obj)
				{
					Obj->Modify();
				}
			}
		}
	};

	// Drag end: Commit transaction
	auto EndTransaction = [TransactionHolder]()
	{
		TransactionHolder->Reset();
	};

	auto DegreeInterface = MakeShared<FDegreeTypeInterface>();

	return SNew(SHorizontalBox)
		// Roll (X)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Roll; })
				.OnBeginSliderMovement_Lambda([BeginTransaction]()
				{
					BeginTransaction();
				})
				.OnValueChanged_Lambda([GetRotator, SetRotatorInteractive](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Roll = NewValue;
					SetRotatorInteractive(Rot);
				})
				.OnEndSliderMovement_Lambda([RotHandlePtr, EndTransaction](double FinalValue)
				{
					if (RotHandlePtr.IsValid())
					{
						RotHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
					}
					EndTransaction();
				})
				.OnValueCommitted_Lambda([RotHandlePtr, GetRotator](double NewValue, ETextCommit::Type)
				{
					if (RotHandlePtr.IsValid())
					{
						FRotator Rot = GetRotator();
						Rot.Roll = NewValue;
						RotHandlePtr->SetValue(Rot);
					}
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Pitch (Y)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Pitch; })
				.OnBeginSliderMovement_Lambda([BeginTransaction]()
				{
					BeginTransaction();
				})
				.OnValueChanged_Lambda([GetRotator, SetRotatorInteractive](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Pitch = NewValue;
					SetRotatorInteractive(Rot);
				})
				.OnEndSliderMovement_Lambda([RotHandlePtr, EndTransaction](double FinalValue)
				{
					if (RotHandlePtr.IsValid())
					{
						RotHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
					}
					EndTransaction();
				})
				.OnValueCommitted_Lambda([RotHandlePtr, GetRotator](double NewValue, ETextCommit::Type)
				{
					if (RotHandlePtr.IsValid())
					{
						FRotator Rot = GetRotator();
						Rot.Pitch = NewValue;
						RotHandlePtr->SetValue(Rot);
					}
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Yaw (Z)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Yaw; })
				.OnBeginSliderMovement_Lambda([BeginTransaction]()
				{
					BeginTransaction();
				})
				.OnValueChanged_Lambda([GetRotator, SetRotatorInteractive](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Yaw = NewValue;
					SetRotatorInteractive(Rot);
				})
				.OnEndSliderMovement_Lambda([RotHandlePtr, EndTransaction](double FinalValue)
				{
					if (RotHandlePtr.IsValid())
					{
						RotHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
					}
					EndTransaction();
				})
				.OnValueCommitted_Lambda([RotHandlePtr, GetRotator](double NewValue, ETextCommit::Type)
				{
					if (RotHandlePtr.IsValid())
					{
						FRotator Rot = GetRotator();
						Rot.Yaw = NewValue;
						RotHandlePtr->SetValue(Rot);
					}
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateMeshScaleWidget(
	TSharedRef<IPropertyHandle> VectorHandle,
	float Delta)
{
	// Cache MeshScaleHandle (for ratio calculation)
	MeshScaleHandle = VectorHandle.ToSharedPtr();
	TSharedPtr<IPropertyHandle> VecHandlePtr = MeshScaleHandle;

	// For drag transaction management
	TSharedPtr<TUniquePtr<FScopedTransaction>> TransactionHolder = MakeShared<TUniquePtr<FScopedTransaction>>();

	// Read real-time memory value with EnumerateRawData
	auto GetVector = [VecHandlePtr]() -> FVector
	{
		FVector Result = FVector::ZeroVector;
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FVector*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	// For fast updates during drag
	auto SetVectorInteractive = [VecHandlePtr](const FVector& NewValue)
	{
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FVector*>(RawData) = NewValue;
				}
				return true;
			});
			VecHandlePtr->NotifyPostChange(EPropertyChangeType::Interactive);
		}
	};

	// Drag start: Begin transaction + call Modify
	auto BeginTransaction = [VecHandlePtr, TransactionHolder]()
	{
		if (VecHandlePtr.IsValid())
		{
			*TransactionHolder = MakeUnique<FScopedTransaction>(LOCTEXT("DragMeshScale", "Drag Mesh Scale"));

			TArray<UObject*> OuterObjects;
			VecHandlePtr->GetOuterObjects(OuterObjects);
			for (UObject* Obj : OuterObjects)
			{
				if (Obj)
				{
					Obj->Modify();
				}
			}
		}
	};

	// Drag end: Commit transaction
	auto EndTransaction = [TransactionHolder]()
	{
		TransactionHolder->Reset();
	};

	// Proportional scaling (when X axis changes)
	auto ApplyScaleLockX = [this, GetVector, SetVectorInteractive](double NewValue)
	{
		FVector OldVec = GetVector();
		if (bMeshScaleLocked && !FMath::IsNearlyZero(OldVec.X))
		{
			double Ratio = NewValue / OldVec.X;
			FVector NewVec(NewValue, OldVec.Y * Ratio, OldVec.Z * Ratio);
			SetVectorInteractive(NewVec);
		}
		else
		{
			FVector NewVec = OldVec;
			NewVec.X = NewValue;
			SetVectorInteractive(NewVec);
		}
	};

	// Proportional scaling (when Y axis changes)
	auto ApplyScaleLockY = [this, GetVector, SetVectorInteractive](double NewValue)
	{
		FVector OldVec = GetVector();
		if (bMeshScaleLocked && !FMath::IsNearlyZero(OldVec.Y))
		{
			double Ratio = NewValue / OldVec.Y;
			FVector NewVec(OldVec.X * Ratio, NewValue, OldVec.Z * Ratio);
			SetVectorInteractive(NewVec);
		}
		else
		{
			FVector NewVec = OldVec;
			NewVec.Y = NewValue;
			SetVectorInteractive(NewVec);
		}
	};

	// Proportional scaling (when Z axis changes)
	auto ApplyScaleLockZ = [this, GetVector, SetVectorInteractive](double NewValue)
	{
		FVector OldVec = GetVector();
		if (bMeshScaleLocked && !FMath::IsNearlyZero(OldVec.Z))
		{
			double Ratio = NewValue / OldVec.Z;
			FVector NewVec(OldVec.X * Ratio, OldVec.Y * Ratio, NewValue);
			SetVectorInteractive(NewVec);
		}
		else
		{
			FVector NewVec = OldVec;
			NewVec.Z = NewValue;
			SetVectorInteractive(NewVec);
		}
	};

	// Maintain proportions on commit (X)
	auto CommitWithLockX = [this, VecHandlePtr, GetVector](double NewValue)
	{
		if (VecHandlePtr.IsValid())
		{
			FVector OldVec = GetVector();
			if (bMeshScaleLocked && !FMath::IsNearlyZero(OldVec.X))
			{
				double Ratio = NewValue / OldVec.X;
				FVector NewVec(NewValue, OldVec.Y * Ratio, OldVec.Z * Ratio);
				VecHandlePtr->SetValue(NewVec);
			}
			else
			{
				FVector NewVec = OldVec;
				NewVec.X = NewValue;
				VecHandlePtr->SetValue(NewVec);
			}
		}
	};

	// Maintain proportions on commit (Y)
	auto CommitWithLockY = [this, VecHandlePtr, GetVector](double NewValue)
	{
		if (VecHandlePtr.IsValid())
		{
			FVector OldVec = GetVector();
			if (bMeshScaleLocked && !FMath::IsNearlyZero(OldVec.Y))
			{
				double Ratio = NewValue / OldVec.Y;
				FVector NewVec(OldVec.X * Ratio, NewValue, OldVec.Z * Ratio);
				VecHandlePtr->SetValue(NewVec);
			}
			else
			{
				FVector NewVec = OldVec;
				NewVec.Y = NewValue;
				VecHandlePtr->SetValue(NewVec);
			}
		}
	};

	// Maintain proportions on commit (Z)
	auto CommitWithLockZ = [this, VecHandlePtr, GetVector](double NewValue)
	{
		if (VecHandlePtr.IsValid())
		{
			FVector OldVec = GetVector();
			if (bMeshScaleLocked && !FMath::IsNearlyZero(OldVec.Z))
			{
				double Ratio = NewValue / OldVec.Z;
				FVector NewVec(OldVec.X * Ratio, OldVec.Y * Ratio, NewValue);
				VecHandlePtr->SetValue(NewVec);
			}
			else
			{
				FVector NewVec = OldVec;
				NewVec.Z = NewValue;
				VecHandlePtr->SetValue(NewVec);
			}
		}
	};

	return SNew(SHorizontalBox)
		// X
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.MinFractionalDigits(1)
				.MaxFractionalDigits(6)
				.Value_Lambda([GetVector]() -> double { return GetVector().X; })
				.OnBeginSliderMovement_Lambda([BeginTransaction]()
				{
					BeginTransaction();
				})
				.OnValueChanged_Lambda([ApplyScaleLockX](double NewValue)
				{
					ApplyScaleLockX(NewValue);
				})
				.OnEndSliderMovement_Lambda([VecHandlePtr, EndTransaction](double FinalValue)
				{
					if (VecHandlePtr.IsValid())
					{
						VecHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
					}
					EndTransaction();
				})
				.OnValueCommitted_Lambda([CommitWithLockX](double NewValue, ETextCommit::Type)
				{
					CommitWithLockX(NewValue);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Y
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.MinFractionalDigits(1)
				.MaxFractionalDigits(6)
				.Value_Lambda([GetVector]() -> double { return GetVector().Y; })
				.OnBeginSliderMovement_Lambda([BeginTransaction]()
				{
					BeginTransaction();
				})
				.OnValueChanged_Lambda([ApplyScaleLockY](double NewValue)
				{
					ApplyScaleLockY(NewValue);
				})
				.OnEndSliderMovement_Lambda([VecHandlePtr, EndTransaction](double FinalValue)
				{
					if (VecHandlePtr.IsValid())
					{
						VecHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
					}
					EndTransaction();
				})
				.OnValueCommitted_Lambda([CommitWithLockY](double NewValue, ETextCommit::Type)
				{
					CommitWithLockY(NewValue);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Z
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.MinFractionalDigits(1)
				.MaxFractionalDigits(6)
				.Value_Lambda([GetVector]() -> double { return GetVector().Z; })
				.OnBeginSliderMovement_Lambda([BeginTransaction]()
				{
					BeginTransaction();
				})
				.OnValueChanged_Lambda([ApplyScaleLockZ](double NewValue)
				{
					ApplyScaleLockZ(NewValue);
				})
				.OnEndSliderMovement_Lambda([VecHandlePtr, EndTransaction](double FinalValue)
				{
					if (VecHandlePtr.IsValid())
					{
						VecHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
					}
					EndTransaction();
				})
				.OnValueCommitted_Lambda([CommitWithLockZ](double NewValue, ETextCommit::Type)
				{
					CommitWithLockZ(NewValue);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		];
}

FReply FFleshRingSettingsCustomization::OnMeshScaleLockClicked()
{
	bMeshScaleLocked = !bMeshScaleLocked;
	return FReply::Handled();
}

void FFleshRingSettingsCustomization::AddLinearVectorRowWithReset(
	IDetailChildrenBuilder& ChildBuilder,
	TSharedRef<IPropertyHandle> VectorHandle,
	const FText& DisplayName,
	float Delta,
	const FVector& DefaultValue,
	TAttribute<bool> IsEnabled)
{
	ChildBuilder.AddCustomRow(DisplayName)
	.IsEnabled(IsEnabled)
	.NameContent()
	[
		SNew(STextBlock)
		.Text(DisplayName)
		.Font(IDetailLayoutBuilder::GetDetailFont())
	]
	.ValueContent()
	.MinDesiredWidth(300.0f)
	[
		CreateLinearVectorWidgetWithReset(VectorHandle, Delta, DefaultValue)
	];
}

void FFleshRingSettingsCustomization::AddLinearRotatorRowWithReset(
	IDetailChildrenBuilder& ChildBuilder,
	TSharedRef<IPropertyHandle> RotatorHandle,
	const FText& DisplayName,
	float Delta,
	const FRotator& DefaultValue,
	TAttribute<bool> IsEnabled)
{
	ChildBuilder.AddCustomRow(DisplayName)
	.IsEnabled(IsEnabled)
	.NameContent()
	[
		SNew(STextBlock)
		.Text(DisplayName)
		.Font(IDetailLayoutBuilder::GetDetailFont())
	]
	.ValueContent()
	.MinDesiredWidth(300.0f)
	[
		CreateLinearRotatorWidgetWithReset(RotatorHandle, Delta, DefaultValue)
	];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateLinearVectorWidgetWithReset(
	TSharedRef<IPropertyHandle> VectorHandle,
	float Delta,
	const FVector& DefaultValue)
{
	TSharedPtr<IPropertyHandle> VecHandlePtr = VectorHandle.ToSharedPtr();

	auto GetVector = [VecHandlePtr]() -> FVector
	{
		FVector Result = FVector::ZeroVector;
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FVector*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	// NotifyPreChange is managed by caller (slider: OnBeginSliderMovement, text/button: just before call)
	auto SetVector = [VecHandlePtr](const FVector& NewValue, EPropertyChangeType::Type ChangeType = EPropertyChangeType::ValueSet)
	{
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FVector*>(RawData) = NewValue;
				}
				return true;
			});
			VecHandlePtr->NotifyPostChange(ChangeType);
		}
	};

	return SNew(SHorizontalBox)
		// X
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double { return GetVector().X; })
				.OnBeginSliderMovement_Lambda([VecHandlePtr]()
				{
					if (VecHandlePtr.IsValid()) { VecHandlePtr->NotifyPreChange(); }
				})
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.X = NewValue;
					SetVector(Vec, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetVector, SetVector](double FinalValue)
				{
					FVector Vec = GetVector();
					Vec.X = FinalValue;
					SetVector(Vec, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([VecHandlePtr, GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					if (VecHandlePtr.IsValid()) { VecHandlePtr->NotifyPreChange(); }
					FVector Vec = GetVector();
					Vec.X = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Y
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double { return GetVector().Y; })
				.OnBeginSliderMovement_Lambda([VecHandlePtr]()
				{
					if (VecHandlePtr.IsValid()) { VecHandlePtr->NotifyPreChange(); }
				})
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.Y = NewValue;
					SetVector(Vec, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetVector, SetVector](double FinalValue)
				{
					FVector Vec = GetVector();
					Vec.Y = FinalValue;
					SetVector(Vec, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([VecHandlePtr, GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					if (VecHandlePtr.IsValid()) { VecHandlePtr->NotifyPreChange(); }
					FVector Vec = GetVector();
					Vec.Y = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Z
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double { return GetVector().Z; })
				.OnBeginSliderMovement_Lambda([VecHandlePtr]()
				{
					if (VecHandlePtr.IsValid()) { VecHandlePtr->NotifyPreChange(); }
				})
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.Z = NewValue;
					SetVector(Vec, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetVector, SetVector](double FinalValue)
				{
					FVector Vec = GetVector();
					Vec.Z = FinalValue;
					SetVector(Vec, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([VecHandlePtr, GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					if (VecHandlePtr.IsValid()) { VecHandlePtr->NotifyPreChange(); }
					FVector Vec = GetVector();
					Vec.Z = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Reset Button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(4, 0, 0, 0)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.OnClicked_Lambda([VecHandlePtr, SetVector, DefaultValue]() -> FReply
			{
				if (VecHandlePtr.IsValid()) { VecHandlePtr->NotifyPreChange(); }
				SetVector(DefaultValue);
				return FReply::Handled();
			})
			.ContentPadding(FMargin(1, 0))
			.ToolTipText(LOCTEXT("ResetToDefault", "Reset to Default"))
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("PropertyWindow.DiffersFromDefault"))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateLinearRotatorWidgetWithReset(
	TSharedRef<IPropertyHandle> RotatorHandle,
	float Delta,
	const FRotator& DefaultValue)
{
	TSharedPtr<IPropertyHandle> RotHandlePtr = RotatorHandle.ToSharedPtr();

	auto GetRotator = [RotHandlePtr]() -> FRotator
	{
		FRotator Result = FRotator::ZeroRotator;
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FRotator*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	// NotifyPreChange is managed by caller (slider: OnBeginSliderMovement, text/button: just before call)
	auto SetRotator = [RotHandlePtr](const FRotator& NewValue, EPropertyChangeType::Type ChangeType = EPropertyChangeType::ValueSet)
	{
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FRotator*>(RawData) = NewValue;
				}
				return true;
			});
			RotHandlePtr->NotifyPostChange(ChangeType);
		}
	};

	auto DegreeInterface = MakeShared<FDegreeTypeInterface>();

	return SNew(SHorizontalBox)
		// Roll (X)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Roll; })
				.OnBeginSliderMovement_Lambda([RotHandlePtr]()
				{
					if (RotHandlePtr.IsValid()) { RotHandlePtr->NotifyPreChange(); }
				})
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Roll = NewValue;
					SetRotator(Rot, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetRotator, SetRotator](double FinalValue)
				{
					FRotator Rot = GetRotator();
					Rot.Roll = FinalValue;
					SetRotator(Rot, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([RotHandlePtr, GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					if (RotHandlePtr.IsValid()) { RotHandlePtr->NotifyPreChange(); }
					FRotator Rot = GetRotator();
					Rot.Roll = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Pitch (Y)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Pitch; })
				.OnBeginSliderMovement_Lambda([RotHandlePtr]()
				{
					if (RotHandlePtr.IsValid()) { RotHandlePtr->NotifyPreChange(); }
				})
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Pitch = NewValue;
					SetRotator(Rot, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetRotator, SetRotator](double FinalValue)
				{
					FRotator Rot = GetRotator();
					Rot.Pitch = FinalValue;
					SetRotator(Rot, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([RotHandlePtr, GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					if (RotHandlePtr.IsValid()) { RotHandlePtr->NotifyPreChange(); }
					FRotator Rot = GetRotator();
					Rot.Pitch = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Yaw (Z)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Yaw; })
				.OnBeginSliderMovement_Lambda([RotHandlePtr]()
				{
					if (RotHandlePtr.IsValid()) { RotHandlePtr->NotifyPreChange(); }
				})
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Yaw = NewValue;
					SetRotator(Rot, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetRotator, SetRotator](double FinalValue)
				{
					FRotator Rot = GetRotator();
					Rot.Yaw = FinalValue;
					SetRotator(Rot, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([RotHandlePtr, GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					if (RotHandlePtr.IsValid()) { RotHandlePtr->NotifyPreChange(); }
					FRotator Rot = GetRotator();
					Rot.Yaw = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Reset Button
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(4, 0, 0, 0)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.OnClicked_Lambda([RotHandlePtr, SetRotator, DefaultValue]() -> FReply
			{
				if (RotHandlePtr.IsValid()) { RotHandlePtr->NotifyPreChange(); }
				SetRotator(DefaultValue);
				return FReply::Handled();
			})
			.ContentPadding(FMargin(1, 0))
			.ToolTipText(LOCTEXT("ResetToDefault", "Reset to Default"))
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("PropertyWindow.DiffersFromDefault"))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateResetButton(
	TSharedRef<IPropertyHandle> VectorHandle,
	const FVector& DefaultValue)
{
	TSharedPtr<IPropertyHandle> VecHandlePtr = VectorHandle.ToSharedPtr();

	// SetVector for button click - NotifyPreChange is managed by caller
	auto SetVector = [VecHandlePtr](const FVector& NewValue)
	{
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FVector*>(RawData) = NewValue;
				}
				return true;
			});
			VecHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
		}
	};

	return SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.OnClicked_Lambda([VecHandlePtr, SetVector, DefaultValue]() -> FReply
		{
			if (VecHandlePtr.IsValid()) { VecHandlePtr->NotifyPreChange(); }
			SetVector(DefaultValue);
			return FReply::Handled();
		})
		.ContentPadding(FMargin(1, 0))
		.ToolTipText(LOCTEXT("ResetToDefaultVector", "Reset to Default"))
		[
			SNew(SImage)
			.Image(FAppStyle::GetBrush("PropertyWindow.DiffersFromDefault"))
			.ColorAndOpacity(FSlateColor::UseForeground())
		];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateResetButton(
	TSharedRef<IPropertyHandle> RotatorHandle,
	const FRotator& DefaultValue)
{
	TSharedPtr<IPropertyHandle> RotHandlePtr = RotatorHandle.ToSharedPtr();

	// SetRotator for button click - NotifyPreChange is managed by caller
	auto SetRotator = [RotHandlePtr](const FRotator& NewValue)
	{
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FRotator*>(RawData) = NewValue;
				}
				return true;
			});
			RotHandlePtr->NotifyPostChange(EPropertyChangeType::ValueSet);
		}
	};

	return SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "SimpleButton")
		.OnClicked_Lambda([RotHandlePtr, SetRotator, DefaultValue]() -> FReply
		{
			if (RotHandlePtr.IsValid()) { RotHandlePtr->NotifyPreChange(); }
			SetRotator(DefaultValue);
			return FReply::Handled();
		})
		.ContentPadding(FMargin(1, 0))
		.ToolTipText(LOCTEXT("ResetToDefaultRotator", "Reset to Default"))
		[
			SNew(SImage)
			.Image(FAppStyle::GetBrush("PropertyWindow.DiffersFromDefault"))
			.ColorAndOpacity(FSlateColor::UseForeground())
		];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateVectorWidgetWithResetButton(
	TSharedRef<IPropertyHandle> VectorHandle,
	float Delta,
	const FVector& DefaultValue)
{
	TSharedPtr<IPropertyHandle> VecHandlePtr = VectorHandle.ToSharedPtr();

	auto GetVector = [VecHandlePtr]() -> FVector
	{
		FVector Result = FVector::ZeroVector;
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FVector*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	// NotifyPreChange is managed by caller (slider: OnBeginSliderMovement, text/button: just before call)
	auto SetVector = [VecHandlePtr](const FVector& NewValue, EPropertyChangeType::Type ChangeType = EPropertyChangeType::ValueSet)
	{
		if (VecHandlePtr.IsValid())
		{
			VecHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FVector*>(RawData) = NewValue;
				}
				return true;
			});
			VecHandlePtr->NotifyPostChange(ChangeType);
			if (ChangeType == EPropertyChangeType::ValueSet)
			{
				VecHandlePtr->NotifyFinishedChangingProperties();
			}
		}
	};

	return SNew(SHorizontalBox)
		// X
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double { return GetVector().X; })
				.OnBeginSliderMovement_Lambda([VecHandlePtr]()
				{
					if (VecHandlePtr.IsValid()) { VecHandlePtr->NotifyPreChange(); }
				})
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.X = NewValue;
					SetVector(Vec, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetVector, SetVector](double FinalValue)
				{
					FVector Vec = GetVector();
					Vec.X = FinalValue;
					SetVector(Vec, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([VecHandlePtr, GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					if (VecHandlePtr.IsValid()) { VecHandlePtr->NotifyPreChange(); }
					FVector Vec = GetVector();
					Vec.X = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Y
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double { return GetVector().Y; })
				.OnBeginSliderMovement_Lambda([VecHandlePtr]()
				{
					if (VecHandlePtr.IsValid()) { VecHandlePtr->NotifyPreChange(); }
				})
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.Y = NewValue;
					SetVector(Vec, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetVector, SetVector](double FinalValue)
				{
					FVector Vec = GetVector();
					Vec.Y = FinalValue;
					SetVector(Vec, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([VecHandlePtr, GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					if (VecHandlePtr.IsValid()) { VecHandlePtr->NotifyPreChange(); }
					FVector Vec = GetVector();
					Vec.Y = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Z
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetVector]() -> double { return GetVector().Z; })
				.OnBeginSliderMovement_Lambda([VecHandlePtr]()
				{
					if (VecHandlePtr.IsValid()) { VecHandlePtr->NotifyPreChange(); }
				})
				.OnValueChanged_Lambda([GetVector, SetVector](double NewValue)
				{
					FVector Vec = GetVector();
					Vec.Z = NewValue;
					SetVector(Vec, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetVector, SetVector](double FinalValue)
				{
					FVector Vec = GetVector();
					Vec.Z = FinalValue;
					SetVector(Vec, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([VecHandlePtr, GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					if (VecHandlePtr.IsValid()) { VecHandlePtr->NotifyPreChange(); }
					FVector Vec = GetVector();
					Vec.Z = NewValue;
					SetVector(Vec);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Reset Button (right end)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(8, 0, 0, 0)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.OnClicked_Lambda([VecHandlePtr, SetVector, DefaultValue]() -> FReply
			{
				if (VecHandlePtr.IsValid()) { VecHandlePtr->NotifyPreChange(); }
				SetVector(DefaultValue);
				return FReply::Handled();
			})
			.ContentPadding(FMargin(1, 0))
			.ToolTipText(LOCTEXT("ResetVectorToDefault", "Reset to Default"))
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("PropertyWindow.DiffersFromDefault"))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateRotatorWidgetWithResetButton(
	TSharedRef<IPropertyHandle> RotatorHandle,
	float Delta,
	const FRotator& DefaultValue)
{
	TSharedPtr<IPropertyHandle> RotHandlePtr = RotatorHandle.ToSharedPtr();

	auto GetRotator = [RotHandlePtr]() -> FRotator
	{
		FRotator Result = FRotator::ZeroRotator;
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&Result](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					Result = *static_cast<FRotator*>(RawData);
					return false;
				}
				return true;
			});
		}
		return Result;
	};

	// NotifyPreChange is managed by caller (slider: OnBeginSliderMovement, text/button: just before call)
	auto SetRotator = [RotHandlePtr](const FRotator& NewValue, EPropertyChangeType::Type ChangeType = EPropertyChangeType::ValueSet)
	{
		if (RotHandlePtr.IsValid())
		{
			RotHandlePtr->EnumerateRawData([&NewValue](void* RawData, const int32 DataIndex, const int32 NumDatas)
			{
				if (RawData)
				{
					*static_cast<FRotator*>(RawData) = NewValue;
				}
				return true;
			});
			RotHandlePtr->NotifyPostChange(ChangeType);
			if (ChangeType == EPropertyChangeType::ValueSet)
			{
				RotHandlePtr->NotifyFinishedChangingProperties();
			}
		}
	};

	auto DegreeInterface = MakeShared<FDegreeTypeInterface>();

	return SNew(SHorizontalBox)
		// Roll (X)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.594f, 0.0197f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Roll; })
				.OnBeginSliderMovement_Lambda([RotHandlePtr]()
				{
					if (RotHandlePtr.IsValid()) { RotHandlePtr->NotifyPreChange(); }
				})
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Roll = NewValue;
					SetRotator(Rot, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetRotator, SetRotator](double FinalValue)
				{
					FRotator Rot = GetRotator();
					Rot.Roll = FinalValue;
					SetRotator(Rot, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([RotHandlePtr, GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					if (RotHandlePtr.IsValid()) { RotHandlePtr->NotifyPreChange(); }
					FRotator Rot = GetRotator();
					Rot.Roll = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Pitch (Y)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 2, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.1144f, 0.4456f, 0.0f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Pitch; })
				.OnBeginSliderMovement_Lambda([RotHandlePtr]()
				{
					if (RotHandlePtr.IsValid()) { RotHandlePtr->NotifyPreChange(); }
				})
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Pitch = NewValue;
					SetRotator(Rot, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetRotator, SetRotator](double FinalValue)
				{
					FRotator Rot = GetRotator();
					Rot.Pitch = FinalValue;
					SetRotator(Rot, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([RotHandlePtr, GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					if (RotHandlePtr.IsValid()) { RotHandlePtr->NotifyPreChange(); }
					FRotator Rot = GetRotator();
					Rot.Pitch = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Yaw (Z)
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(2, 0, 0, 0)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			.Padding(0, 1, -4, 1)
			[
				SNew(SColorBlock)
				.Color(FLinearColor(0.0251f, 0.207f, 0.85f))
				.Size(FVector2D(4.0f, 0.0f))
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SSpinBox<double>)
				.TypeInterface(DegreeInterface)
				.Delta(Delta)
				.LinearDeltaSensitivity(1)
				.Value_Lambda([GetRotator]() -> double { return GetRotator().Yaw; })
				.OnBeginSliderMovement_Lambda([RotHandlePtr]()
				{
					if (RotHandlePtr.IsValid()) { RotHandlePtr->NotifyPreChange(); }
				})
				.OnValueChanged_Lambda([GetRotator, SetRotator](double NewValue)
				{
					FRotator Rot = GetRotator();
					Rot.Yaw = NewValue;
					SetRotator(Rot, EPropertyChangeType::Interactive);
				})
				.OnEndSliderMovement_Lambda([GetRotator, SetRotator](double FinalValue)
				{
					FRotator Rot = GetRotator();
					Rot.Yaw = FinalValue;
					SetRotator(Rot, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([RotHandlePtr, GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					if (RotHandlePtr.IsValid()) { RotHandlePtr->NotifyPreChange(); }
					FRotator Rot = GetRotator();
					Rot.Yaw = NewValue;
					SetRotator(Rot);
				})
				.Font(IDetailLayoutBuilder::GetDetailFont())
			]
		]
		// Reset Button (right end)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(8, 0, 0, 0)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.OnClicked_Lambda([RotHandlePtr, SetRotator, DefaultValue]() -> FReply
			{
				if (RotHandlePtr.IsValid()) { RotHandlePtr->NotifyPreChange(); }
				SetRotator(DefaultValue);
				return FReply::Handled();
			})
			.ContentPadding(FMargin(1, 0))
			.ToolTipText(LOCTEXT("ResetRotatorToDefault", "Reset to Default"))
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("PropertyWindow.DiffersFromDefault"))
				.ColorAndOpacity(FSlateColor::UseForeground())
			]
		];
}

#undef LOCTEXT_NAMESPACE
