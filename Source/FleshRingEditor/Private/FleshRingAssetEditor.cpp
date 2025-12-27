// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingAssetEditor.h"
#include "FleshRingAssetEditorToolkit.h"
#include "FleshRingAsset.h"
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

	// 프로퍼티 변경 델리게이트 구독 (Undo/Redo 포함)
	OnPropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddRaw(
		this, &FFleshRingAssetEditor::OnObjectPropertyChanged);
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
		RefreshViewport();
	}
}

void FFleshRingAssetEditor::RefreshViewport()
{
	if (ViewportWidget.IsValid())
	{
		ViewportWidget->RefreshPreview();
	}
}

#undef LOCTEXT_NAMESPACE
