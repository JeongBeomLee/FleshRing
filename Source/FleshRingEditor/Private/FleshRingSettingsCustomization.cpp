// Copyright Epic Games, Inc. All Rights Reserved.

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
 * Ring 이름 인라인 편집 위젯
 * - 싱글 클릭: Ring 선택
 * - 더블 클릭: 이름 편집 모드
 * - 중복 이름 검증 (느낌표 아이콘 + 오류 툴팁)
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

		// 초기 텍스트 저장 (바인딩 아님 - 고정값)
		CurrentText = InArgs._InitialText;

		// 에셋 변경 델리게이트 구독 (스켈레톤 트리에서 이름 변경 시 업데이트)
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

		// 자식 위젯이 직접 마우스 이벤트를 받지 못하도록 설정
		// (편집 모드 진입 시에만 다시 활성화)
		if (InlineTextBlock.IsValid())
		{
			InlineTextBlock->SetVisibility(EVisibility::HitTestInvisible);
		}
	}

	~SRingNameWidget()
	{
		// 델리게이트 해제
		if (Asset)
		{
			Asset->OnAssetChanged.RemoveAll(this);
		}
	}

	/** 텍스트 업데이트 (외부에서 호출) */
	void SetText(const FText& NewText)
	{
		CurrentText = NewText;
		if (InlineTextBlock.IsValid())
		{
			InlineTextBlock->SetText(NewText);
		}
	}

	/** 에셋 변경 핸들러 (스켈레톤 트리에서 이름 변경 시) */
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
			// 좌클릭 눌림 상태 추적
			bIsLeftMouseButtonDown = true;
			// 싱글 클릭: Ring 선택 + 포커스 설정 (F2 키 처리를 위해)
			OnClickedDelegate.ExecuteIfBound();
			return FReply::Handled().SetUserFocus(AsShared(), EFocusCause::Mouse);
		}
		else if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
		{
			// 좌클릭 눌린 상태면 우클릭 무시 (동시 클릭 방지)
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
			// 우클릭이 동시에 눌려있으면 더블클릭 무시
			if (MouseEvent.IsMouseButtonDown(EKeys::RightMouseButton))
			{
				return FReply::Handled();
			}
			// 더블클릭: 편집 모드 진입
			EnterEditingMode();
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			// 좌클릭 해제
			bIsLeftMouseButtonDown = false;
			return FReply::Handled();
		}
		else if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
		{
			// 좌클릭이 눌려있으면 컨텍스트 메뉴 표시 안 함
			if (bIsLeftMouseButtonDown)
			{
				return FReply::Handled();
			}

			// 우클릭: 컨텍스트 메뉴 표시
			ShowContextMenu(MouseEvent.GetScreenSpacePosition());
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	/** 컨텍스트 메뉴 표시 */
	void ShowContextMenu(const FVector2D& ScreenPosition)
	{
		FMenuBuilder MenuBuilder(true, nullptr);

		// Rename Ring 메뉴 항목
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
		// F2 키: 이름 편집 모드 진입
		if (InKeyEvent.GetKey() == EKeys::F2)
		{
			EnterEditingMode();
			return FReply::Handled();
		}

		// Delete 키: Ring 삭제
		if (InKeyEvent.GetKey() == EKeys::Delete)
		{
			OnDeleteRequestedDelegate.ExecuteIfBound();
			return FReply::Handled();
		}

		// F 키: 카메라 포커스 (선택된 Ring에)
		if (InKeyEvent.GetKey() == EKeys::F)
		{
			FocusCameraOnRing();
			return FReply::Handled();
		}

		return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
	}

	/** 편집 모드 진입 */
	void EnterEditingMode()
	{
		// 편집 시작 시 원본 텍스트 저장 (검증 실패 시 복원용)
		OriginalText = CurrentText;
		bIsEnterPressed = false;

		if (InlineTextBlock.IsValid())
		{
			// 편집 중에는 마우스 이벤트를 받을 수 있도록 활성화
			InlineTextBlock->SetVisibility(EVisibility::Visible);
			InlineTextBlock->EnterEditingMode();
		}
	}

	virtual FReply OnPreviewKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override
	{
		// Enter 키 감지 (OnVerifyNameChanged에서 이전 이름으로 되돌리기 위해)
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

	/** 카메라를 선택된 Ring에 포커스 */
	void FocusCameraOnRing()
	{
		if (!Asset)
		{
			return;
		}

		// 현재 에셋을 편집 중인 뷰포트 클라이언트 찾기
		for (FFleshRingEditorViewportClient* ViewportClient : FFleshRingEditorViewportClient::GetAllInstances())
		{
			if (ViewportClient && ViewportClient->GetEditingAsset() == Asset)
			{
				ViewportClient->FocusOnMesh();
				break;
			}
		}
	}

	/** 이름 검증 (빈 이름/중복 체크) */
	bool OnVerifyNameChanged(const FText& NewText, FText& OutErrorMessage)
	{
		if (!Asset)
		{
			bIsEnterPressed = false;
			return true;
		}

		FName NewName = FName(*NewText.ToString());
		bool bIsValid = true;

		// 빈 이름 체크
		if (NewName.IsNone())
		{
			OutErrorMessage = LOCTEXT("EmptyNameError", "Name cannot be empty.");
			bIsValid = false;
		}
		// 중복 이름 체크
		else if (!Asset->IsRingNameUnique(NewName, RingIndex))
		{
			OutErrorMessage = LOCTEXT("DuplicateNameError", "This name is already in use. Please choose a different name.");
			bIsValid = false;
		}

		if (!bIsValid)
		{
			// Enter 시에만 이전 이름으로 되돌림
			if (bIsEnterPressed && InlineTextBlock.IsValid())
			{
				InlineTextBlock->SetText(OriginalText);
			}
			bIsEnterPressed = false;
			return false;  // 편집 모드 유지
		}

		bIsEnterPressed = false;
		return true;
	}

	/** 이름 커밋 */
	void OnNameCommitted(const FText& NewText, ETextCommit::Type CommitType)
	{
		if (CommitType == ETextCommit::OnEnter || CommitType == ETextCommit::OnUserMovedFocus)
		{
			// OnVerifyTextChanged에서 false 반환 시 여기까지 오지 않음
			// 여기 도달했다면 유효한 이름
			CurrentText = NewText;
			if (InlineTextBlock.IsValid())
			{
				InlineTextBlock->SetText(NewText);
			}
			OnTextCommittedDelegate.ExecuteIfBound(NewText, CommitType);
		}

		// 편집 종료 후 다시 마우스 이벤트 차단
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
	FText OriginalText;				// 편집 시작 시 원본 텍스트 (검증 실패 시 복원용)
	bool bIsEnterPressed = false;		// Enter 키 감지 플래그
	bool bIsLeftMouseButtonDown = false;	// 좌클릭 눌림 상태 (동시 클릭 방지용)
};

/**
 * 클릭/더블클릭 가능한 Row 버튼 위젯
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
			// 싱글 클릭
			OnClickedDelegate.ExecuteIfBound();
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	virtual FReply OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
	{
		if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
		{
			// 더블클릭
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
 * Bone 드롭다운용 트리 행 위젯 (SExpanderArrow + Wires 지원)
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

		// 아이콘과 색상 결정
		const FSlateBrush* IconBrush = nullptr;
		FSlateColor TextColor = FSlateColor::UseForeground();
		FSlateColor IconColor = FSlateColor::UseForeground();

		if (Item->bIsMeshBone)
		{
			IconBrush = FAppStyle::GetBrush("SkeletonTree.Bone");
		}
		else
		{
			// 웨이팅 안 된 본 (검색 시에만 표시됨)
			IconBrush = FAppStyle::GetBrush("SkeletonTree.BoneNonWeighted");
			TextColor = FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f));
			IconColor = FSlateColor(FLinearColor(0.4f, 0.4f, 0.4f));
		}

		STableRow<TSharedPtr<FBoneDropdownItem>>::Construct(
			STableRow<TSharedPtr<FBoneDropdownItem>>::FArguments()
			.Padding(FMargin(0, 0)),
			InOwnerTable
		);

		// SExpanderArrow로 트리 연결선 표시
		ChildSlot
		[
			SNew(SHorizontalBox)
			// Expander Arrow (트리 연결선)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Fill)
			[
				SNew(SExpanderArrow, SharedThis(this))
				.ShouldDrawWires(true)
			]
			// 아이콘 + 텍스트
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.Padding(0, 2)
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
					.DesiredSizeOverride(FVector2D(16, 16))
				]
				// 본 이름
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

// 각도 표시용 TypeInterface (숫자 옆에 ° 표시)
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
	// 메인 프로퍼티 핸들 캐싱 (Asset 접근용)
	MainPropertyHandle = PropertyHandle;

	// 배열 인덱스 캐싱 (클릭 선택 및 이름 표시용)
	CachedArrayIndex = PropertyHandle->GetIndexInArray();

	// BoneName 핸들 미리 가져오기 (헤더 미리보기용)
	BoneNameHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BoneName));

	// 헤더: 전체 row 클릭 가능 (클릭=선택, 더블클릭=이름 편집)
	FText TooltipText = LOCTEXT("RingHeaderTooltip", "Ring Name\nClick to select, Double-click to rename");

	// 배열 컨트롤용 핸들
	TSharedRef<IPropertyHandle> PropHandleRef = PropertyHandle;

	HeaderRow.WholeRowContent()
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
			// 좌측 열: Ring 이름 (35%, 클리핑 적용)
			+ SHorizontalBox::Slot()
			.FillWidth(0.35f)
			.VAlign(VAlign_Center)
			.Padding(0, 0, 16, 0)  // Ring 이름과 Bone 이름 사이 간격
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
			// 우측 열: Bone 이름 + 버튼 (65%)
			+ SHorizontalBox::Slot()
			.FillWidth(0.65f)
			.VAlign(VAlign_Center)
			[
				SNew(SHorizontalBox)
				// Bone 이름
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(this, &FFleshRingSettingsCustomization::GetCurrentBoneName)
					.Font(IDetailLayoutBuilder::GetDetailFont())
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				// 삽입 버튼
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
				// 복제 버튼
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
				// 삭제 버튼
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
	];
}

void FFleshRingSettingsCustomization::CustomizeChildren(
	TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// BoneName 핸들은 CustomizeHeader에서 이미 설정됨
	// Bone 트리 빌드
	BuildBoneTree();

	// BoneName을 검색 가능한 드롭다운으로 커스터마이징
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

	// Rotation 핸들 캐싱
	RingRotationHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingRotation));
	MeshRotationHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshRotation));

	// InfluenceMode 핸들 가져오기
	TSharedPtr<IPropertyHandle> InfluenceModeHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, InfluenceMode));

	// 현재 InfluenceMode가 Manual인지 확인 (초기 상태용)
	bool bIsManualMode = false;
	if (InfluenceModeHandle.IsValid())
	{
		uint8 ModeValue = 0;
		InfluenceModeHandle->GetValue(ModeValue);
		bIsManualMode = (static_cast<EFleshRingInfluenceMode>(ModeValue) == EFleshRingInfluenceMode::Manual);
	}

	// Manual 모드 동적 체크용 TAttribute (Ring Transform에 사용)
	TAttribute<bool> IsManualModeAttr = TAttribute<bool>::Create([InfluenceModeHandle]() -> bool
	{
		if (!InfluenceModeHandle.IsValid())
		{
			return true;
		}
		uint8 ModeValue = 0;
		InfluenceModeHandle->GetValue(ModeValue);
		return static_cast<EFleshRingInfluenceMode>(ModeValue) == EFleshRingInfluenceMode::Manual;
	});

	// SDF 모드 동적 체크용 TAttribute (SDF Settings에 사용 - Manual이 아닐 때 활성화)
	TAttribute<bool> IsSdfModeAttr = TAttribute<bool>::Create([InfluenceModeHandle]() -> bool
	{
		if (!InfluenceModeHandle.IsValid())
		{
			return true;
		}
		uint8 ModeValue = 0;
		InfluenceModeHandle->GetValue(ModeValue);
		return static_cast<EFleshRingInfluenceMode>(ModeValue) != EFleshRingInfluenceMode::Manual;
	});

	// VirtualBand 모드 동적 체크용 TAttribute
	TAttribute<bool> IsProceduralBandModeAttr = TAttribute<bool>::Create([InfluenceModeHandle]() -> bool
	{
		if (!InfluenceModeHandle.IsValid())
		{
			return false;
		}
		uint8 ModeValue = 0;
		InfluenceModeHandle->GetValue(ModeValue);
		return static_cast<EFleshRingInfluenceMode>(ModeValue) == EFleshRingInfluenceMode::ProceduralBand;
	});

	// Ring Transform 그룹에 넣을 프로퍼티들 수집
	TSharedPtr<IPropertyHandle> RingRadiusHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingRadius));
	TSharedPtr<IPropertyHandle> RingThicknessHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingThickness));
	TSharedPtr<IPropertyHandle> RingWidthHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingWidth));
	TSharedPtr<IPropertyHandle> RingOffsetHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingOffset));
	TSharedPtr<IPropertyHandle> RingEulerHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingEulerRotation));

	// Ring Transform 그룹에 들어갈 프로퍼티 이름들
	TSet<FName> RingGroupProperties;
	RingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingRadius));
	RingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingThickness));
	RingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingWidth));
	RingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingOffset));
	RingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingEulerRotation));

	// Smoothing 프로퍼티 핸들 개별 획득
	TSharedPtr<IPropertyHandle> bEnableRadialSmoothingHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableRadialSmoothing));
	TSharedPtr<IPropertyHandle> bEnableLaplacianSmoothingHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableLaplacianSmoothing));
	TSharedPtr<IPropertyHandle> bUseTaubinSmoothingHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bUseTaubinSmoothing));
	TSharedPtr<IPropertyHandle> SmoothingLambdaHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingLambda));
	TSharedPtr<IPropertyHandle> TaubinMuHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, TaubinMu));
	TSharedPtr<IPropertyHandle> SmoothingIterationsHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingIterations));
	TSharedPtr<IPropertyHandle> VolumePreservationHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, VolumePreservation));
	TSharedPtr<IPropertyHandle> bUseHopBasedSmoothingHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bUseHopBasedSmoothing));
	TSharedPtr<IPropertyHandle> MaxSmoothingHopsHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MaxSmoothingHops));
	TSharedPtr<IPropertyHandle> HopFalloffRatioHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, HopFalloffRatio));
	TSharedPtr<IPropertyHandle> HopFalloffTypeHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, HopFalloffType));
	TSharedPtr<IPropertyHandle> PostHopLaplacianIterationsHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, PostHopLaplacianIterations));
	TSharedPtr<IPropertyHandle> PostHopLaplacianLambdaHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, PostHopLaplacianLambda));
	TSharedPtr<IPropertyHandle> SeedBlendCountHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SeedBlendCount));
	TSharedPtr<IPropertyHandle> SeedBlendWeightTypeHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SeedBlendWeightType));
	TSharedPtr<IPropertyHandle> SeedBlendGaussianSigmaHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SeedBlendGaussianSigma));
	TSharedPtr<IPropertyHandle> DeformPropagationModeHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, DeformPropagationMode));
	TSharedPtr<IPropertyHandle> HeatDiffusionIterationsHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, HeatDiffusionIterations));
	TSharedPtr<IPropertyHandle> HeatDiffusionLambdaHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, HeatDiffusionLambda));
	TSharedPtr<IPropertyHandle> SmoothingBoundsZTopHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingBoundsZTop));
	TSharedPtr<IPropertyHandle> SmoothingBoundsZBottomHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingBoundsZBottom));

	// Smoothing 그룹에 들어갈 프로퍼티 이름들
	TSet<FName> SmoothingGroupProperties;
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableRadialSmoothing));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnableLaplacianSmoothing));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bUseTaubinSmoothing));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingLambda));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, TaubinMu));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingIterations));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, VolumePreservation));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bUseHopBasedSmoothing));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MaxSmoothingHops));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, HopFalloffRatio));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, HopFalloffType));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, PostHopLaplacianIterations));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, PostHopLaplacianLambda));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SeedBlendCount));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SeedBlendWeightType));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SeedBlendGaussianSigma));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, DeformPropagationMode));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, HeatDiffusionIterations));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, HeatDiffusionLambda));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingBoundsZTop));
	SmoothingGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, SmoothingBoundsZBottom));

	// PBD 프로퍼티 핸들 개별 획득
	TSharedPtr<IPropertyHandle> bEnablePBDEdgeConstraintHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnablePBDEdgeConstraint));
	TSharedPtr<IPropertyHandle> PBDStiffnessHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, PBDStiffness));
	TSharedPtr<IPropertyHandle> PBDIterationsHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, PBDIterations));
	TSharedPtr<IPropertyHandle> bPBDUseDeformAmountWeightHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bPBDUseDeformAmountWeight));

	// PBD 그룹에 들어갈 프로퍼티 이름들
	TSet<FName> PBDGroupProperties;
	PBDGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bEnablePBDEdgeConstraint));
	PBDGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, PBDStiffness));
	PBDGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, PBDIterations));
	PBDGroupProperties.Add(GET_MEMBER_NAME_CHECKED(FFleshRingSettings, bPBDUseDeformAmountWeight));

	// 나머지 프로퍼티들 먼저 표시 (Ring 그룹 제외)
	uint32 NumChildren;
	PropertyHandle->GetNumChildren(NumChildren);

	for (uint32 ChildIndex = 0; ChildIndex < NumChildren; ++ChildIndex)
	{
		TSharedRef<IPropertyHandle> ChildHandle = PropertyHandle->GetChildHandle(ChildIndex).ToSharedRef();
		FName PropertyName = ChildHandle->GetProperty()->GetFName();

		// BoneName은 이미 커스터마이징했으므로 스킵
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BoneName))
		{
			continue;
		}

		// RingName은 헤더에서 인라인 편집 가능하므로 스킵
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingName))
		{
			continue;
		}

		// FQuat은 UI에서 숨김 (EulerRotation만 표시)
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingRotation) ||
			PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshRotation))
		{
			continue;
		}

		// Ring 그룹에 넣을 프로퍼티는 여기서 스킵
		if (RingGroupProperties.Contains(PropertyName))
		{
			continue;
		}

		// Smoothing 그룹에 넣을 프로퍼티는 여기서 스킵
		if (SmoothingGroupProperties.Contains(PropertyName))
		{
			continue;
		}

		// PBD 그룹에 넣을 프로퍼티는 여기서 스킵
		if (PBDGroupProperties.Contains(PropertyName))
		{
			continue;
		}

		// Transform 프로퍼티들은 선형 드래그 감도 적용 + 기본 리셋 화살표
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshOffset))
		{
			ChildBuilder.AddProperty(ChildHandle)
				.CustomWidget()
				.NameContent()
				[
					ChildHandle->CreatePropertyNameWidget()
				]
				.ValueContent()
				.MinDesiredWidth(300.0f)
				[
					CreateLinearVectorWidget(ChildHandle, 0.1f)
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
			continue;
		}
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshEulerRotation))
		{
			ChildBuilder.AddProperty(ChildHandle)
				.CustomWidget()
				.NameContent()
				[
					ChildHandle->CreatePropertyNameWidget()
				]
				.ValueContent()
				.MinDesiredWidth(300.0f)
				[
					CreateLinearRotatorWidget(ChildHandle, 1.0f)
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
			continue;
		}
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, MeshScale))
		{
			ChildBuilder.AddProperty(ChildHandle)
				.CustomWidget()
				.NameContent()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						ChildHandle->CreatePropertyNameWidget()
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
								? LOCTEXT("UnlockScale", "Unlock Scale (비율 유지 해제)")
								: LOCTEXT("LockScale", "Lock Scale (비율 유지)");
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
					CreateMeshScaleWidget(ChildHandle, 0.0025f)
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
			continue;
		}

		// InfluenceMode - 기본값: Auto
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, InfluenceMode))
		{
			ChildBuilder.AddProperty(ChildHandle)
				.OverrideResetToDefault(
					FResetToDefaultOverride::Create(
						FIsResetToDefaultVisible::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							uint8 Value;
							Handle->GetValue(Value);
							return Value != static_cast<uint8>(EFleshRingInfluenceMode::Auto);
						}),
						FResetToDefaultHandler::CreateLambda([](TSharedPtr<IPropertyHandle> Handle) {
							Handle->SetValue(static_cast<uint8>(EFleshRingInfluenceMode::Auto));
						})
					)
				);
			continue;
		}

		// BulgeIntensity - 기본값: 1.0
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeIntensity))
		{
			ChildBuilder.AddProperty(ChildHandle)
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
			continue;
		}

		// BulgeAxialRange - 기본값: 5.0
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeAxialRange))
		{
			ChildBuilder.AddProperty(ChildHandle)
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
			continue;
		}

		// BulgeRadialRange - 기본값: 1.0
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BulgeRadialRange))
		{
			ChildBuilder.AddProperty(ChildHandle)
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
			continue;
		}

		// TightnessStrength - 기본값: 1.0
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, TightnessStrength))
		{
			ChildBuilder.AddProperty(ChildHandle)
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
			continue;
		}

		// FalloffType - 기본값: Linear
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, FalloffType))
		{
			ChildBuilder.AddProperty(ChildHandle)
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
			continue;
		}

		// VirtualBand - InfluenceMode가 VirtualBand일 때만 활성화
		if (PropertyName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, ProceduralBand))
		{
			ChildBuilder.AddProperty(ChildHandle)
				.IsEnabled(IsProceduralBandModeAttr);
			continue;
		}

		// 나머지는 기본 위젯 사용
		ChildBuilder.AddProperty(ChildHandle);
	}

	// Ring Transform 그룹 생성 (Auto 모드일 때 헤더 텍스트도 어둡게)
	IDetailGroup& RingGroup = ChildBuilder.AddGroup(TEXT("RingTransform"), LOCTEXT("RingTransformGroup", "Ring Transform"));
	RingGroup.HeaderRow()
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("RingTransformHeader", "Ring Transform"))
			.Font(IDetailLayoutBuilder::GetDetailFontBold())
			.ColorAndOpacity_Lambda([IsManualModeAttr]() -> FSlateColor
			{
				return IsManualModeAttr.Get() ? FSlateColor::UseForeground() : FSlateColor::UseSubduedForeground();
			})
		];

	// Ring 그룹에 프로퍼티 추가
	if (RingRadiusHandle.IsValid())
	{
		RingGroup.AddPropertyRow(RingRadiusHandle.ToSharedRef())
			.IsEnabled(IsManualModeAttr)
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
		RingGroup.AddPropertyRow(RingThicknessHandle.ToSharedRef())
			.IsEnabled(IsManualModeAttr)
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
	if (RingWidthHandle.IsValid())
	{
		RingGroup.AddPropertyRow(RingWidthHandle.ToSharedRef())
			.IsEnabled(IsManualModeAttr)
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
		RingGroup.AddPropertyRow(RingOffsetHandle.ToSharedRef())
			.IsEnabled(IsManualModeAttr)
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
		RingGroup.AddPropertyRow(RingEulerHandle.ToSharedRef())
			.IsEnabled(IsManualModeAttr)
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

	// Smoothing 그룹 생성 (Ring Transform 그룹 뒤)
	IDetailGroup& SmoothingGroup = ChildBuilder.AddGroup(TEXT("Smoothing"), LOCTEXT("SmoothingGroup", "Smoothing"));
	SmoothingGroup.HeaderRow()
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("SmoothingHeader", "Smoothing"))
			.Font(IDetailLayoutBuilder::GetDetailFontBold())
		];

	// Smoothing 프로퍼티 명시적 순서로 추가
	if (bEnableRadialSmoothingHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(bEnableRadialSmoothingHandle.ToSharedRef());
	}
	if (bEnableLaplacianSmoothingHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(bEnableLaplacianSmoothingHandle.ToSharedRef());
	}
	if (bUseTaubinSmoothingHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(bUseTaubinSmoothingHandle.ToSharedRef());
	}
	if (SmoothingLambdaHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(SmoothingLambdaHandle.ToSharedRef());
	}
	if (TaubinMuHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(TaubinMuHandle.ToSharedRef());
	}
	if (SmoothingIterationsHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(SmoothingIterationsHandle.ToSharedRef());
	}
	if (VolumePreservationHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(VolumePreservationHandle.ToSharedRef());
	}
	if (bUseHopBasedSmoothingHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(bUseHopBasedSmoothingHandle.ToSharedRef());
	}
	if (MaxSmoothingHopsHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(MaxSmoothingHopsHandle.ToSharedRef());
	}
	if (HopFalloffRatioHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(HopFalloffRatioHandle.ToSharedRef());
	}
	if (HopFalloffTypeHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(HopFalloffTypeHandle.ToSharedRef());
	}
	if (PostHopLaplacianIterationsHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(PostHopLaplacianIterationsHandle.ToSharedRef());
	}
	if (PostHopLaplacianLambdaHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(PostHopLaplacianLambdaHandle.ToSharedRef());
	}
	if (SeedBlendCountHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(SeedBlendCountHandle.ToSharedRef());
	}
	if (SeedBlendWeightTypeHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(SeedBlendWeightTypeHandle.ToSharedRef());
	}
	if (SeedBlendGaussianSigmaHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(SeedBlendGaussianSigmaHandle.ToSharedRef());
	}
	if (DeformPropagationModeHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(DeformPropagationModeHandle.ToSharedRef());
	}
	if (HeatDiffusionIterationsHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(HeatDiffusionIterationsHandle.ToSharedRef());
	}
	if (HeatDiffusionLambdaHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(HeatDiffusionLambdaHandle.ToSharedRef());
	}
	if (SmoothingBoundsZTopHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(SmoothingBoundsZTopHandle.ToSharedRef());
	}
	if (SmoothingBoundsZBottomHandle.IsValid())
	{
		SmoothingGroup.AddPropertyRow(SmoothingBoundsZBottomHandle.ToSharedRef());
	}

	// PBD 그룹 생성 (Smoothing 그룹 뒤)
	IDetailGroup& PBDGroup = ChildBuilder.AddGroup(TEXT("PBDEdgeConstraint"), LOCTEXT("PBDGroup", "PBD Edge Constraint"));
	PBDGroup.HeaderRow()
		.NameContent()
		[
			SNew(STextBlock)
			.Text(LOCTEXT("PBDHeader", "PBD Edge Constraint"))
			.Font(IDetailLayoutBuilder::GetDetailFontBold())
		];

	// PBD 프로퍼티 명시적 순서로 추가
	if (bEnablePBDEdgeConstraintHandle.IsValid())
	{
		PBDGroup.AddPropertyRow(bEnablePBDEdgeConstraintHandle.ToSharedRef());
	}
	if (PBDStiffnessHandle.IsValid())
	{
		PBDGroup.AddPropertyRow(PBDStiffnessHandle.ToSharedRef());
	}
	if (PBDIterationsHandle.IsValid())
	{
		PBDGroup.AddPropertyRow(PBDIterationsHandle.ToSharedRef());
	}
	if (bPBDUseDeformAmountWeightHandle.IsValid())
	{
		PBDGroup.AddPropertyRow(bPBDUseDeformAmountWeightHandle.ToSharedRef());
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

	// PropertyHandle 체인을 따라 올라가서 UFleshRingAsset 찾기
	// FFleshRingSettings -> Rings 배열 -> UFleshRingAsset
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
	// Asset의 SetEditorSelectedRingIndex 호출 (델리게이트 브로드캐스트됨)
	if (UFleshRingAsset* Asset = GetOuterAsset())
	{
		FScopedTransaction Transaction(LOCTEXT("SelectRingFromDetails", "Select Ring"));
		Asset->Modify();
		Asset->SetEditorSelectedRingIndex(RingIndex, EFleshRingSelectionType::Mesh);
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
				// 위젯에서 이미 검증되었으므로 바로 적용
				FScopedTransaction Transaction(LOCTEXT("RenameRing", "Rename Ring"));
				Asset->Modify();
				Asset->Rings[CachedArrayIndex].RingName = FName(*NewText.ToString());
				Asset->PostEditChange();

				// 스켈레톤 트리 등 다른 UI 갱신
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

	// 웨이팅된 본 캐시 빌드
	BuildWeightedBoneCache(SkeletalMesh);

	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
	const int32 NumBones = RefSkeleton.GetNum();

	// 자손 중 웨이팅된 본이 있는지 체크하는 재귀 함수
	TFunction<bool(int32)> HasWeightedDescendant = [&](int32 BoneIndex) -> bool
	{
		if (IsBoneWeighted(BoneIndex))
		{
			return true;
		}

		// 자식 본들 체크
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

	// 모든 본에 대해 아이템 생성
	AllBoneItems.SetNum(NumBones);
	for (int32 BoneIdx = 0; BoneIdx < NumBones; ++BoneIdx)
	{
		FName BoneName = RefSkeleton.GetBoneName(BoneIdx);
		bool bIsMeshBone = HasWeightedDescendant(BoneIdx);
		AllBoneItems[BoneIdx] = FBoneDropdownItem::Create(BoneName, BoneIdx, bIsMeshBone);
	}

	// 부모-자식 관계 설정
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
			// 루트 본
			BoneTreeRoots.Add(AllBoneItems[BoneIdx]);
		}
	}

	// 초기 필터링 적용
	ApplySearchFilter();
}

void FFleshRingSettingsCustomization::BuildWeightedBoneCache(USkeletalMesh* SkelMesh)
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

bool FFleshRingSettingsCustomization::IsBoneWeighted(int32 BoneIndex) const
{
	return WeightedBoneIndices.Contains(BoneIndex);
}

TSharedRef<SWidget> FFleshRingSettingsCustomization::CreateSearchableBoneDropdown()
{
	return SAssignNew(BoneComboButton, SComboButton)
		.OnGetMenuContent_Lambda([this]() -> TSharedRef<SWidget>
		{
			// 드롭다운 열릴 때 본 트리 재빌드
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

			// 트리 생성 후 모든 아이템 확장
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
		// RebuildList로 행 완전 재생성 (하이라이트 업데이트)
		BoneTreeView->RebuildList();

		// 모든 아이템 확장
		ExpandAllBoneTreeItems();
	}
}

void FFleshRingSettingsCustomization::ApplySearchFilter()
{
	FilteredBoneTreeRoots.Empty();

	// 검색어가 없으면 웨이팅된 본만 필터링
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
		// 검색어가 있어도 웨이팅된 본만 표시
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
				// 자식 중 검색어에 맞는 웨이팅된 본이 있으면 부모도 표시
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

	// 재귀적으로 모든 아이템 확장
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

	// 검색어가 없으면 웨이팅된 본만 표시
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
		// 검색어가 있어도 웨이팅된 본만 표시
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
		// 웨이팅된 본만 선택 가능
		if (NewSelection->bIsMeshBone)
		{
			BoneNameHandle->SetValue(NewSelection->BoneName);

			// 드롭다운 닫기
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

	// None은 경고 아님 (아직 선택 안 한 상태)
	if (CurrentValue == NAME_None)
	{
		return false;
	}

	// SkeletalMesh에서 본 찾기
	USkeletalMesh* SkeletalMesh = const_cast<FFleshRingSettingsCustomization*>(this)->GetTargetSkeletalMesh();
	if (!SkeletalMesh)
	{
		// SkeletalMesh 없으면 경고
		return true;
	}

	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
	int32 BoneIndex = RefSkeleton.FindBoneIndex(CurrentValue);

	if (BoneIndex == INDEX_NONE)
	{
		return true;
	}

	// 웨이팅되지 않은 본도 경고 (AllBoneItems가 비어있으면 체크 안 함)
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

		// 현재 선택된 본이 SkeletalMesh에 있는지 확인
		USkeletalMesh* SkeletalMesh = const_cast<FFleshRingSettingsCustomization*>(this)->GetTargetSkeletalMesh();
		if (SkeletalMesh)
		{
			const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
			int32 BoneIndex = RefSkeleton.FindBoneIndex(CurrentValue);

			if (BoneIndex == INDEX_NONE)
			{
				// 본이 없으면 경고 표시
				return FText::Format(
					LOCTEXT("BoneNotFound", "{0} (Not Found)"),
					FText::FromName(CurrentValue));
			}

			// 웨이팅되지 않은 본 경고 (AllBoneItems가 비어있으면 체크 안 함)
			if (AllBoneItems.IsValidIndex(BoneIndex) && !AllBoneItems[BoneIndex]->bIsMeshBone)
			{
				return FText::Format(
					LOCTEXT("BoneNotWeighted", "{0} (No Weight)"),
					FText::FromName(CurrentValue));
			}
		}
		else
		{
			// SkeletalMesh가 설정되지 않은 경우
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

	// Euler 읽기
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

	// Quat에 쓰기
	FQuat Quat = Euler.Quaternion();
	QuatHandle->EnumerateRawData([&Quat](void* RawData, const int32 DataIndex, const int32 NumDatas)
	{
		if (RawData)
		{
			*static_cast<FQuat*>(RawData) = Quat;
		}
		return true;
	});

	// 변경 알림 (프리뷰 갱신 트리거)
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

	// EnumerateRawData로 FVector 직접 읽기
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

	// EnumerateRawData로 FVector 직접 쓰기
	// NotifyPreChange는 호출자가 직접 관리 (슬라이더: OnBeginSliderMovement, 텍스트: OnValueCommitted에서)
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
					// 드래그 시작 시 Undo 포인트 생성
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
					// 드래그 종료 시 최종 값으로 커밋 (Undo 포인트 확정)
					FVector Vec = GetVector();
					Vec.X = FinalValue;
					SetVector(Vec, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([VecHandlePtr, GetVector, SetVector](double NewValue, ETextCommit::Type)
				{
					// 텍스트 입력 시 Undo 포인트 생성 후 값 설정
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

	// EnumerateRawData로 FRotator 직접 읽기
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

	// EnumerateRawData로 FRotator 직접 쓰기
	// NotifyPreChange는 호출자가 직접 관리 (슬라이더: OnBeginSliderMovement, 텍스트: OnValueCommitted에서)
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

	// 각도 표시용 TypeInterface
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
					// 드래그 종료 시 최종 값으로 커밋 (Undo 포인트 확정)
					FRotator Rot = GetRotator();
					Rot.Roll = FinalValue;
					SetRotator(Rot, EPropertyChangeType::ValueSet);
				})
				.OnValueCommitted_Lambda([RotHandlePtr, GetRotator, SetRotator](double NewValue, ETextCommit::Type)
				{
					// 텍스트 입력 시 Undo 포인트 생성 후 값 설정
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

	// 드래그 트랜잭션 관리용 (TSharedPtr로 감싸서 람다에서 안전하게 사용)
	TSharedPtr<TUniquePtr<FScopedTransaction>> TransactionHolder = MakeShared<TUniquePtr<FScopedTransaction>>();

	// EnumerateRawData로 실시간 메모리 값 읽기
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

	// 드래그 중 빠른 업데이트용
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

	// 드래그 시작: 트랜잭션 시작 + Modify 호출
	auto BeginTransaction = [VecHandlePtr, TransactionHolder]()
	{
		if (VecHandlePtr.IsValid())
		{
			// 새 트랜잭션 시작
			*TransactionHolder = MakeUnique<FScopedTransaction>(LOCTEXT("DragVector", "Drag Vector Value"));

			// UObject의 Modify() 호출 - 이 시점의 상태가 Undo로 복원됨
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

	// 드래그 종료: 트랜잭션 커밋
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
					// NotifyPostChange(ValueSet)으로 변경 완료 알림
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

	// 드래그 트랜잭션 관리용
	TSharedPtr<TUniquePtr<FScopedTransaction>> TransactionHolder = MakeShared<TUniquePtr<FScopedTransaction>>();

	// EnumerateRawData로 실시간 메모리 값 읽기
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

	// 드래그 중 빠른 업데이트용
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

	// 드래그 시작: 트랜잭션 시작 + Modify 호출
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

	// 드래그 종료: 트랜잭션 커밋
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
	// MeshScaleHandle 캐싱 (비율 계산용)
	MeshScaleHandle = VectorHandle.ToSharedPtr();
	TSharedPtr<IPropertyHandle> VecHandlePtr = MeshScaleHandle;

	// 드래그 트랜잭션 관리용
	TSharedPtr<TUniquePtr<FScopedTransaction>> TransactionHolder = MakeShared<TUniquePtr<FScopedTransaction>>();

	// EnumerateRawData로 실시간 메모리 값 읽기
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

	// 드래그 중 빠른 업데이트용
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

	// 드래그 시작: 트랜잭션 시작 + Modify 호출
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

	// 드래그 종료: 트랜잭션 커밋
	auto EndTransaction = [TransactionHolder]()
	{
		TransactionHolder->Reset();
	};

	// 비율 유지 스케일링 (X 축 변경 시)
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

	// 비율 유지 스케일링 (Y 축 변경 시)
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

	// 비율 유지 스케일링 (Z 축 변경 시)
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

	// 커밋 시 비율 유지 (X)
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

	// 커밋 시 비율 유지 (Y)
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

	// 커밋 시 비율 유지 (Z)
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

	// NotifyPreChange는 호출자가 직접 관리 (슬라이더: OnBeginSliderMovement, 텍스트/버튼: 호출 직전)
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

	// NotifyPreChange는 호출자가 직접 관리 (슬라이더: OnBeginSliderMovement, 텍스트/버튼: 호출 직전)
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

	// 버튼 클릭용 SetVector - NotifyPreChange는 호출자가 관리
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

	// 버튼 클릭용 SetRotator - NotifyPreChange는 호출자가 관리
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

	// NotifyPreChange는 호출자가 직접 관리 (슬라이더: OnBeginSliderMovement, 텍스트/버튼: 호출 직전)
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
		// Reset Button (우측 끝)
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

	// NotifyPreChange는 호출자가 직접 관리 (슬라이더: OnBeginSliderMovement, 텍스트/버튼: 호출 직전)
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
		// Reset Button (우측 끝)
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
