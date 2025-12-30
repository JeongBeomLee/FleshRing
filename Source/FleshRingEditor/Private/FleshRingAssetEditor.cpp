// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingAssetEditor.h"
#include "FleshRingAssetEditorToolkit.h"
#include "FleshRingAsset.h"
#include "FleshRingTypes.h"
#include "SFleshRingEditorViewport.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SBoxPanel.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "FleshRingAssetEditor"

FFleshRingAssetEditor::FFleshRingAssetEditor()
{
}

FFleshRingAssetEditor::~FFleshRingAssetEditor()
{
	// 프로퍼티 변경 델리게이트 해제
	if (OnPropertyChangedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(OnPropertyChangedHandle);
	}

	// Undo/Redo 델리게이트 해제
	if (OnObjectTransactedHandle.IsValid())
	{
		FCoreUObjectDelegates::OnObjectTransacted.Remove(OnObjectTransactedHandle);
	}
}

void FFleshRingAssetEditor::InitFleshRingAssetEditor(
	const EToolkitMode::Type Mode,
	const TSharedPtr<IToolkitHost>& InitToolkitHost,
	UFleshRingAsset* InAsset)
{
	EditingAsset = InAsset;

	// 에디터 레이아웃 정의
	const TSharedRef<FTabManager::FLayout> StandaloneDefaultLayout =
		FTabManager::NewLayout(FleshRingAssetEditorToolkit::LayoutName)
		->AddArea
		(
			FTabManager::NewPrimaryArea()
			->SetOrientation(Orient_Horizontal)
			->Split
			(
				// 좌측: 뷰포트
				FTabManager::NewStack()
				->SetSizeCoefficient(0.7f)
				->AddTab(FleshRingAssetEditorToolkit::ViewportTabId, ETabState::OpenedTab)
			)
			->Split
			(
				// 우측: Details
				FTabManager::NewStack()
				->SetSizeCoefficient(0.3f)
				->AddTab(FleshRingAssetEditorToolkit::DetailsTabId, ETabState::OpenedTab)
			)
		);

	// Asset 객체 배열 준비
	TArray<UObject*> ObjectsToEdit;
	ObjectsToEdit.Add(InAsset);

	// 에디터 초기화
	InitAssetEditor(
		Mode,
		InitToolkitHost,
		FleshRingAssetEditorToolkit::AppIdentifier,
		StandaloneDefaultLayout,
		true,  // bCreateDefaultStandaloneMenu
		true,  // bCreateDefaultToolbar
		ObjectsToEdit);

	// 프로퍼티 변경 델리게이트 구독
	OnPropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(
		this, &FFleshRingAssetEditor::OnObjectPropertyChanged);

	// Undo/Redo 델리게이트 구독
	OnObjectTransactedHandle = FCoreUObjectDelegates::OnObjectTransacted.AddRaw(
		this, &FFleshRingAssetEditor::OnObjectTransacted);
}


FName FFleshRingAssetEditor::GetToolkitFName() const
{
	return FName("FleshRingAssetEditor");
}

FText FFleshRingAssetEditor::GetBaseToolkitName() const
{
	return LOCTEXT("AppLabel", "FleshRing Asset Editor");
}

FText FFleshRingAssetEditor::GetToolkitName() const
{
	if (EditingAsset)
	{
		return FText::FromString(EditingAsset->GetName());
	}
	return GetBaseToolkitName();
}

FText FFleshRingAssetEditor::GetToolkitToolTipText() const
{
	if (EditingAsset)
	{
		return FText::FromString(EditingAsset->GetPathName());
	}
	return GetBaseToolkitName();
}

FLinearColor FFleshRingAssetEditor::GetWorldCentricTabColorScale() const
{
	// FleshRing 테마 색상 (분홍/살색)
	return FLinearColor(1.0f, 0.5f, 0.5f);
}

FString FFleshRingAssetEditor::GetWorldCentricTabPrefix() const
{
	return LOCTEXT("WorldCentricTabPrefix", "FleshRing ").ToString();
}

FString FFleshRingAssetEditor::GetDocumentationLink() const
{
	return FString();
}

void FFleshRingAssetEditor::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	// 부모 등록
	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	// 작업 영역 카테고리 추가
	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(
		LOCTEXT("WorkspaceMenu_FleshRingAssetEditor", "FleshRing Asset Editor"));

	// Viewport 탭 등록
	InTabManager->RegisterTabSpawner(
		FleshRingAssetEditorToolkit::ViewportTabId,
		FOnSpawnTab::CreateSP(this, &FFleshRingAssetEditor::SpawnTab_Viewport))
		.SetDisplayName(LOCTEXT("ViewportTab", "Viewport"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Viewports"));

	// Details 탭 등록
	InTabManager->RegisterTabSpawner(
		FleshRingAssetEditorToolkit::DetailsTabId,
		FOnSpawnTab::CreateSP(this, &FFleshRingAssetEditor::SpawnTab_Details))
		.SetDisplayName(LOCTEXT("DetailsTab", "Details"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));
}

void FFleshRingAssetEditor::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);

	InTabManager->UnregisterTabSpawner(FleshRingAssetEditorToolkit::ViewportTabId);
	InTabManager->UnregisterTabSpawner(FleshRingAssetEditorToolkit::DetailsTabId);
}

TSharedRef<SDockTab> FFleshRingAssetEditor::SpawnTab_Viewport(const FSpawnTabArgs& Args)
{
	// 뷰포트 위젯 생성
	ViewportWidget = SNew(SFleshRingEditorViewport)
		.Asset(EditingAsset);

	return SNew(SDockTab)
		.Label(LOCTEXT("ViewportTabLabel", "Viewport"))
		.ContentPadding(0)
		[
			SNew(SVerticalBox)
			// 툴바 (상단, 자동 높이)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				ViewportWidget->MakeToolbar()
			]
			// 뷰포트 (나머지 공간)
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				ViewportWidget.ToSharedRef()
			]
		];
}

TSharedRef<SDockTab> FFleshRingAssetEditor::SpawnTab_Details(const FSpawnTabArgs& Args)
{
	// Details View 생성
	CreateDetailsView();

	return SNew(SDockTab)
		.Label(LOCTEXT("DetailsTabLabel", "Details"))
		[
			DetailsView.ToSharedRef()
		];
}

void FFleshRingAssetEditor::CreateDetailsView()
{
	FPropertyEditorModule& PropertyEditorModule =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.bAllowSearch = true;
	DetailsViewArgs.bHideSelectionTip = true;
	DetailsViewArgs.bShowActorLabel = false;
	DetailsViewArgs.bShowOptions = true;

	DetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);

	if (EditingAsset)
	{
		DetailsView->SetObject(EditingAsset);
	}
}

void FFleshRingAssetEditor::OnObjectPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent)
{
	// EditingAsset이 변경되었을 때만 갱신
	if (Object == EditingAsset)
	{
		// 구조적 변경 (Ring 추가/삭제, RingMesh 변경) 감지
		bool bNeedsFullRefresh = false;
		
		if (PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayAdd ||
			PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayRemove ||
			PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayClear)
		{
			// Ring 배열 구조 변경 시 전체 갱신 필요
			bNeedsFullRefresh = true;
		}
		else if (PropertyChangedEvent.Property)
		{
			FName PropName = PropertyChangedEvent.Property->GetFName();

			// TargetSkeletalMesh 변경 시 스켈레탈 메시 교체 필요
			if (PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, TargetSkeletalMesh))
			{
				bNeedsFullRefresh = true;
			}
			// RingMesh 변경 시 SDF 재생성 필요
			else if (PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, RingMesh))
			{
				bNeedsFullRefresh = true;
			}
			// BoneName 변경 시 Ring 부착 위치 변경 → SDF/AffectedVertices 재계산
			else if (PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, BoneName))
			{
				bNeedsFullRefresh = true;
			}
			// InfluenceMode 변경 시 AffectedVertices 재계산
			else if (PropName == GET_MEMBER_NAME_CHECKED(FFleshRingSettings, InfluenceMode))
			{
				bNeedsFullRefresh = true;
			}
		}

		if (bNeedsFullRefresh)
		{
			RefreshViewport();
		}
		else
		{
			// 트랜스폼/파라미터 변경: 경량 업데이트 (깜빡임 방지)
			UpdateRingTransformsOnly();
		}
	}
}

void FFleshRingAssetEditor::RefreshViewport()
{
	if (ViewportWidget.IsValid())
	{
		ViewportWidget->RefreshPreview();
	}
}

void FFleshRingAssetEditor::UpdateRingTransformsOnly()
{
	if (ViewportWidget.IsValid())
	{
		ViewportWidget->UpdateRingTransformsOnly();
	}
}

void FFleshRingAssetEditor::OnObjectTransacted(UObject* Object, const FTransactionObjectEvent& TransactionEvent)
{
	// Undo/Redo 시 EditingAsset이 변경되었으면 뷰포트 갱신
	if (Object == EditingAsset)
	{
		// UndoRedo 이벤트일 때 갱신 (Ctrl+Z / Ctrl+Y)
		if (TransactionEvent.GetEventType() == ETransactionObjectEventType::UndoRedo)
		{
			// Undo/Redo는 어떤 프로퍼티가 변경되었는지 알 수 없으므로 전체 갱신
			RefreshViewport();
		}
	}
}

#undef LOCTEXT_NAMESPACE
