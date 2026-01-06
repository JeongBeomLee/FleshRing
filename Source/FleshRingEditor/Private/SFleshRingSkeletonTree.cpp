// Copyright Epic Games, Inc. All Rights Reserved.

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
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Widgets/Views/SExpanderArrow.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "FleshRingSkeletonTree"

/** Ring 이름 변경 델리게이트 */
DECLARE_DELEGATE_TwoParams(FOnRingRenamed, int32 /*RingIndex*/, const FString& /*NewName*/);

/**
 * FleshRing 트리 행 위젯 (SExpanderArrow + Wires 지원)
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
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
	{
		Item = InArgs._Item;
		HighlightText = InArgs._HighlightText;
		RowIndex = InArgs._RowIndex;
		Asset = InArgs._Asset;
		OnRingRenamed = InArgs._OnRingRenamed;

		// 원본 이름 저장 (검증 실패 시 복원용)
		if (Item.IsValid())
		{
			OriginalName = Item->GetDisplayName().ToString();
		}

		// 아이콘과 색상, 툴팁 결정
		const FSlateBrush* IconBrush = nullptr;
		FSlateColor TextColor = FSlateColor::UseForeground();
		FSlateColor IconColor = FSlateColor::UseForeground();
		FText TooltipText;
		bool bIsRing = (Item->ItemType == EFleshRingTreeItemType::Ring);

		if (bIsRing)
		{
			IconBrush = FAppStyle::GetBrush("Icons.FilledCircle");
			IconColor = FSlateColor(FLinearColor(1.0f, 0.3f, 0.3f));
			TextColor = FSlateColor(FLinearColor(1.0f, 0.6f, 0.2f));
			TooltipText = FText::Format(LOCTEXT("RingTooltip", "Ring attached to bone: {0}\nDouble-click to rename"), FText::FromName(Item->BoneName));
		}
		else
		{
			if (Item->bIsMeshBone)
			{
				// 실제 메시 본: 채워진 뼈 아이콘
				IconBrush = FAppStyle::GetBrush("SkeletonTree.Bone");
				TooltipText = LOCTEXT("WeightedBoneTooltip", "This bone or one of its descendants has vertices weighted to it.\nRight-click to add a Ring.");
			}
			else
			{
				// 웨이팅 안 된 본: 비활성화 스타일 (빈 뼈 아이콘)
				IconBrush = FAppStyle::GetBrush("SkeletonTree.BoneNonWeighted");
				TextColor = FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f));
				IconColor = FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f));
				TooltipText = LOCTEXT("NonWeightedBoneTooltip", "This bone has no vertices weighted to it or its descendants.\nCannot add Ring to this bone.");
			}
		}

		// 홀수/짝수 행 배경색 (Persona 스타일)
		FLinearColor RowBgColor = (RowIndex % 2 == 0)
			? FLinearColor(0.0f, 0.0f, 0.0f, 0.0f)      // 짝수: 투명
			: FLinearColor(1.0f, 1.0f, 1.0f, 0.03f);    // 홀수: 살짝 밝게

		// STableRow 구성 (Content 없이 - ConstructChildren에서 직접 처리)
		STableRow<TSharedPtr<FFleshRingTreeItem>>::Construct(
			STableRow<TSharedPtr<FFleshRingTreeItem>>::FArguments()
			.Padding(FMargin(0, 0)),
			InOwnerTable
		);

		// 이름 위젯 생성 (Ring은 인라인 편집 가능)
		TSharedRef<SWidget> NameWidget = bIsRing
			? CreateRingNameWidget(TextColor, TooltipText)
			: CreateBoneNameWidget(TextColor, TooltipText);

		// 기본 expander 대신 우리 Content 직접 설정
		// SExpanderArrow를 최외곽에 배치해서 전체 행 높이에 와이어가 그려지게 함
		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
			.BorderBackgroundColor(RowBgColor)
			.Padding(0)
			[
				SNew(SHorizontalBox)
				// Expander Arrow (행 전체 높이 차지)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Fill)
				[
					SNew(SExpanderArrow, SharedThis(this))
					.ShouldDrawWires(true)
				]
				// 아이콘 + 텍스트 (패딩 적용)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.Padding(0, 2)  // 세로 패딩
				[
					SNew(SHorizontalBox)
					// 아이콘
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
					// 이름
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						NameWidget
					]
				]
			]
		];
	}

	/** 편집 모드 진입 */
	void EnterEditingMode()
	{
		if (InlineTextBlock.IsValid())
		{
			InlineTextBlock->EnterEditingMode();
		}
	}

private:
	/** Ring 이름 위젯 생성 (인라인 편집 가능, 검증 포함) */
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

	/** Bone 이름 위젯 생성 (읽기 전용) */
	TSharedRef<SWidget> CreateBoneNameWidget(FSlateColor TextColor, FText TooltipText)
	{
		return SNew(STextBlock)
			.Text(Item->GetDisplayName())
			.ColorAndOpacity(TextColor)
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
			.HighlightText(HighlightText)
			.ToolTipText(TooltipText);
	}

	/** Ring 이름 검증 (중복 체크) */
	bool OnVerifyRingNameChanged(const FText& NewText, FText& OutErrorMessage)
	{
		if (!Asset || !Item.IsValid())
		{
			return true;
		}

		FString NewName = NewText.ToString();

		// 중복 이름 체크
		if (!Asset->IsRingNameUnique(NewName, Item->RingIndex))
		{
			OutErrorMessage = LOCTEXT("DuplicateNameError", "This name is already in use. Please choose a different name.");
			bIsNameValid = false;

			// 빨간 테두리 표시
			if (ValidationBorder.IsValid())
			{
				ValidationBorder->SetBorderImage(FAppStyle::GetBrush("WhiteBrush"));
				ValidationBorder->SetBorderBackgroundColor(FLinearColor(0.8f, 0.2f, 0.2f, 0.5f));
			}

			return false;  // Enter 키로 확정 불가
		}

		bIsNameValid = true;

		// 정상 테두리로 복원
		if (ValidationBorder.IsValid())
		{
			ValidationBorder->SetBorderImage(FAppStyle::GetBrush("NoBorder"));
		}

		return true;
	}

	/** Ring 이름 커밋 */
	void OnRingNameCommitted(const FText& NewText, ETextCommit::Type CommitType)
	{
		// 검증 테두리 초기화
		if (ValidationBorder.IsValid())
		{
			ValidationBorder->SetBorderImage(FAppStyle::GetBrush("NoBorder"));
		}

		if (CommitType == ETextCommit::OnEnter)
		{
			// Enter로 확정: 유효한 이름만 적용
			if (bIsNameValid && Item.IsValid() && OnRingRenamed.IsBound())
			{
				OnRingRenamed.Execute(Item->RingIndex, NewText.ToString());
			}
		}
		else if (CommitType == ETextCommit::OnUserMovedFocus)
		{
			// 포커스 이동: 유효하면 적용, 유효하지 않으면 원래 이름으로 복원
			if (bIsNameValid && Item.IsValid() && OnRingRenamed.IsBound())
			{
				OnRingRenamed.Execute(Item->RingIndex, NewText.ToString());
			}
			// 유효하지 않으면 InlineTextBlock이 원래 텍스트로 자동 복원됨
		}

		bIsNameValid = true;  // 상태 초기화
	}

	TSharedPtr<FFleshRingTreeItem> Item;
	FText HighlightText;
	int32 RowIndex = 0;
	UFleshRingAsset* Asset = nullptr;
	FOnRingRenamed OnRingRenamed;
	TSharedPtr<SInlineEditableTextBlock> InlineTextBlock;
	TSharedPtr<SBorder> ValidationBorder;
	FString OriginalName;
	bool bIsNameValid = true;
};

#undef LOCTEXT_NAMESPACE
#define LOCTEXT_NAMESPACE "FleshRingSkeletonTree"

//////////////////////////////////////////////////////////////////////////
// FFleshRingTreeItem

FText FFleshRingTreeItem::GetDisplayName() const
{
	if (ItemType == EFleshRingTreeItemType::Ring)
	{
		// 커스텀 Ring 이름 또는 기본 이름 (FleshRing_인덱스)
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
// SFleshRingSkeletonTree

void SFleshRingSkeletonTree::Construct(const FArguments& InArgs)
{
	EditingAsset = InArgs._Asset;
	OnBoneSelected = InArgs._OnBoneSelected;
	OnRingSelected = InArgs._OnRingSelected;
	OnAddRingRequested = InArgs._OnAddRingRequested;
	OnFocusCameraRequested = InArgs._OnFocusCameraRequested;
	OnRingDeleted = InArgs._OnRingDeleted;

	// 에셋 변경 델리게이트 구독 (디테일 패널에서 이름 변경 시 트리 갱신)
	if (UFleshRingAsset* Asset = EditingAsset.Get())
	{
		Asset->OnAssetChanged.AddSP(this, &SFleshRingSkeletonTree::OnAssetChangedHandler);
	}

	BuildTree();

	ChildSlot
	[
		SNew(SVerticalBox)
		// 상단 툴바
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4)
		[
			CreateToolbar()
		]
		// 구분선
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SSeparator)
		]
		// 트리 뷰
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
		// + 버튼 (Ring 추가)
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
		// 검색 박스
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.Padding(0, 0, 4, 0)
		[
			SAssignNew(SearchBox, SSearchBox)
			.HintText(LOCTEXT("SearchHint", "Search skeleton tree..."))
			.OnTextChanged(this, &SFleshRingSkeletonTree::OnSearchTextChanged)
		]
		// 필터 버튼
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

	// 본 선택 시
	if (SelectedItem.IsValid() && SelectedItem->ItemType == EFleshRingTreeItemType::Bone)
	{
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
		}
		MenuBuilder.EndSection();
	}
	// Ring 선택 시
	else if (SelectedItem.IsValid() && SelectedItem->ItemType == EFleshRingTreeItemType::Ring)
	{
		MenuBuilder.BeginSection("RingActions", LOCTEXT("RingActionsSection", "Ring"));
		{
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
	RowIndexCounter = 0;  // 행 인덱스 카운터 리셋

	// 재귀적으로 필터 적용
	TFunction<bool(TSharedPtr<FFleshRingTreeItem>)> FilterItem = [&](TSharedPtr<FFleshRingTreeItem> Item) -> bool
	{
		if (!Item.IsValid())
		{
			return false;
		}

		bool bPassesFilter = true;

		if (Item->ItemType == EFleshRingTreeItemType::Bone)
		{
			// 본 필터 모드 확인
			switch (BoneFilterMode)
			{
			case EBoneFilterMode::ShowMeshBonesOnly:
				bPassesFilter = Item->bIsMeshBone;
				break;
			case EBoneFilterMode::ShowBonesWithRingsOnly:
				{
					// 이 본에 Ring이 있는지 확인
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

			// 검색 텍스트 확인
			if (bPassesFilter && !SearchText.IsEmpty())
			{
				bPassesFilter = Item->BoneName.ToString().Contains(SearchText);
			}
		}
		else if (Item->ItemType == EFleshRingTreeItemType::Ring)
		{
			// Ring 검색: 표시 이름("Ring [X]") 또는 부착된 본 이름으로 검색
			if (!SearchText.IsEmpty())
			{
				FString DisplayName = Item->GetDisplayName().ToString();
				bPassesFilter = DisplayName.Contains(SearchText) || Item->BoneName.ToString().Contains(SearchText);
			}
		}

		// 자식 중 하나라도 필터 통과하면 부모도 표시
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
		// RebuildList로 행 완전 재생성 (하이라이트 업데이트)
		TreeView->RebuildList();

		// 모든 아이템 확장
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
	// 기존 델리게이트 해제
	if (UFleshRingAsset* OldAsset = EditingAsset.Get())
	{
		OldAsset->OnAssetChanged.RemoveAll(this);
	}

	EditingAsset = InAsset;

	// 새 에셋의 델리게이트 구독 (디테일 패널에서 이름 변경 시 트리 갱신)
	if (InAsset)
	{
		InAsset->OnAssetChanged.AddSP(this, &SFleshRingSkeletonTree::OnAssetChangedHandler);
	}

	RefreshTree();
}

void SFleshRingSkeletonTree::OnAssetChangedHandler(UFleshRingAsset* Asset)
{
	// 디테일 패널에서 Ring 이름 변경 시 트리 갱신
	if (TreeView.IsValid())
	{
		TreeView->RebuildList();
	}
}

void SFleshRingSkeletonTree::RefreshTree()
{
	// 현재 확장 상태 저장
	SaveExpansionState();

	// 트리 재빌드
	BuildTree();
	ApplyFilter();

	// 확장 상태 복원
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
	// 확장 상태 즉시 저장
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
			// 부모 노드들 확장
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

	// Ring 아이템 찾기
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
			// 부모 노드들 확장
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

	// 웨이팅된 본 캐시 빌드
	BuildWeightedBoneCache(SkelMesh);

	// 자손 중 웨이팅된 본이 있는지 체크하는 재귀 함수
	TFunction<bool(int32)> HasWeightedDescendant = [&](int32 BoneIndex) -> bool
	{
		if (IsBoneWeighted(BoneIndex))
		{
			return true;
		}
		// 자손 본 체크
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

	// 모든 본 아이템 생성
	TArray<TSharedPtr<FFleshRingTreeItem>> AllBoneItems;
	AllBoneItems.SetNum(NumBones);

	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		FName BoneName = RefSkeleton.GetBoneName(BoneIndex);
		TSharedPtr<FFleshRingTreeItem> BoneItem = FFleshRingTreeItem::CreateBone(BoneName, BoneIndex);
		// 자신 또는 자손 중 웨이팅된 본이 있으면 메시 본으로 표시
		BoneItem->bIsMeshBone = HasWeightedDescendant(BoneIndex);
		AllBoneItems[BoneIndex] = BoneItem;
		BoneItemMap.Add(BoneName, BoneItem);
	}

	// 부모-자식 관계 설정
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

	// Ring 아이템 추가
	UpdateRingItems();

	// 깊이 및 마지막 자식 플래그 설정
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

	// 기존 Ring 아이템 제거 (모든 본에서)
	for (auto& Pair : BoneItemMap)
	{
		TSharedPtr<FFleshRingTreeItem> BoneItem = Pair.Value;
		BoneItem->Children.RemoveAll([](const TSharedPtr<FFleshRingTreeItem>& Child)
		{
			return Child->ItemType == EFleshRingTreeItemType::Ring;
		});
	}

	// Ring 아이템 추가
	for (int32 RingIndex = 0; RingIndex < Asset->Rings.Num(); ++RingIndex)
	{
		const FFleshRingSettings& Ring = Asset->Rings[RingIndex];

		if (TSharedPtr<FFleshRingTreeItem>* FoundBone = BoneItemMap.Find(Ring.BoneName))
		{
			TSharedPtr<FFleshRingTreeItem> RingItem = FFleshRingTreeItem::CreateRing(Ring.BoneName, RingIndex, Asset);
			RingItem->Parent = *FoundBone;

			// Ring은 본의 자식들 앞에 추가 (맨 앞)
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

	// LOD 0의 렌더 데이터에서 웨이팅된 본 찾기
	FSkeletalMeshRenderData* RenderData = SkelMesh->GetResourceForRendering();
	if (!RenderData || RenderData->LODRenderData.Num() == 0)
	{
		return;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];

	// 각 섹션의 BoneMap에 있는 본들이 웨이팅된 본들
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
	// SFleshRingTreeRow 사용 (SExpanderArrow + Wires 지원)
	return SNew(SFleshRingTreeRow, OwnerTable)
		.Item(Item)
		.HighlightText(FText::FromString(SearchText))
		.RowIndex(RowIndexCounter++)
		.Asset(EditingAsset.Get())
		.OnRingRenamed(this, &SFleshRingSkeletonTree::HandleRingRenamed);
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
		// 선택 해제
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
		// Ring 선택 (본 하이라이트는 OnRingSelected 내부에서 처리)
		if (OnRingSelected.IsBound())
		{
			OnRingSelected.Execute(Item->RingIndex);
		}
		// 본 델리게이트는 호출하지 않음 (Ring 선택 시 부착 본은 자동 하이라이트)
	}
	else
	{
		// 본 선택
		if (OnBoneSelected.IsBound())
		{
			OnBoneSelected.Execute(Item->BoneName);
		}
		// Ring 선택 해제
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
			// 본 더블클릭: 확장/축소 토글
			bool bIsExpanded = TreeView->IsItemExpanded(Item);
			TreeView->SetItemExpansion(Item, !bIsExpanded);
		}
		else if (Item->ItemType == EFleshRingTreeItemType::Ring)
		{
			// Ring 더블클릭: 이름 편집 모드
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

void SFleshRingSkeletonTree::HandleRingRenamed(int32 RingIndex, const FString& NewName)
{
	if (UFleshRingAsset* Asset = EditingAsset.Get())
	{
		if (Asset->Rings.IsValidIndex(RingIndex))
		{
			// Row에서 이미 검증했으므로 바로 적용
			FScopedTransaction Transaction(LOCTEXT("RenameRingFromTree", "Rename Ring"));
			Asset->Modify();
			Asset->Rings[RingIndex].RingName = NewName;
			Asset->PostEditChange();

			// 디테일 패널 등 다른 UI 갱신
			Asset->OnAssetChanged.Broadcast(Asset);

			// 트리 갱신 (RebuildList로 행 재생성하여 이름 업데이트)
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

// === 컨텍스트 메뉴 액션 ===

void SFleshRingSkeletonTree::OnContextMenuAddRing()
{
	if (CanAddRing() && OnAddRingRequested.IsBound())
	{
		OnAddRingRequested.Execute(SelectedItem->BoneName);
	}
}

bool SFleshRingSkeletonTree::CanAddRing() const
{
	// 메시 본에만 Ring 추가 가능 (IK/가상 본 제외)
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
		// Undo/Redo 지원
		FScopedTransaction Transaction(FText::FromString(TEXT("Delete Ring")));
		Asset->Modify();

		Asset->Rings.RemoveAt(RingIndex);

		// 선택 해제 (Undo 시 정상 복원됨)
		Asset->EditorSelectedRingIndex = -1;
		Asset->EditorSelectionType = EFleshRingSelectionType::None;

		// 델리게이트 호출 (HandleRingDeleted에서 RefreshPreview + RefreshTree 처리)
		OnRingDeleted.ExecuteIfBound();
	}
}

bool SFleshRingSkeletonTree::CanDeleteRing() const
{
	return SelectedItem.IsValid() && SelectedItem->ItemType == EFleshRingTreeItemType::Ring;
}

void SFleshRingSkeletonTree::OnContextMenuCopyBoneName()
{
	if (SelectedItem.IsValid())
	{
		FPlatformApplicationMisc::ClipboardCopy(*SelectedItem->BoneName.ToString());
	}
}

FReply SFleshRingSkeletonTree::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	// F 키: 카메라 포커스
	if (InKeyEvent.GetKey() == EKeys::F)
	{
		OnFocusCameraRequested.ExecuteIfBound();
		return FReply::Handled();
	}

	// Delete 키: 선택된 Ring 삭제
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

#undef LOCTEXT_NAMESPACE
