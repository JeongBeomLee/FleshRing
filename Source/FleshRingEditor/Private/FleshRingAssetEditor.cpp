// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingAssetEditor.h"
#include "FleshRingAssetEditorToolkit.h"
#include "FleshRingAsset.h"
#include "FleshRingTypes.h"
#include "SFleshRingSkeletonTree.h"
#include "SFleshRingEditorViewport.h"
#include "FleshRingEditorViewportClient.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SBoxPanel.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "PropertyPath.h"
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

	// 에디터 레이아웃 정의 (v2: Skeleton Tree 추가)
	const TSharedRef<FTabManager::FLayout> StandaloneDefaultLayout =
		FTabManager::NewLayout(FleshRingAssetEditorToolkit::LayoutName)
		->AddArea
		(
			FTabManager::NewPrimaryArea()
			->SetOrientation(Orient_Horizontal)
			->Split
			(
				// 좌측: Skeleton Tree
				FTabManager::NewStack()
				->SetSizeCoefficient(0.15f)
				->AddTab(FleshRingAssetEditorToolkit::SkeletonTreeTabId, ETabState::OpenedTab)
			)
			->Split
			(
				// 중앙: 뷰포트
				FTabManager::NewStack()
				->SetSizeCoefficient(0.55f)
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

	// Skeleton Tree 탭 등록
	InTabManager->RegisterTabSpawner(
		FleshRingAssetEditorToolkit::SkeletonTreeTabId,
		FOnSpawnTab::CreateSP(this, &FFleshRingAssetEditor::SpawnTab_SkeletonTree))
		.SetDisplayName(LOCTEXT("SkeletonTreeTab", "Skeleton"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Persona.Tabs.SkeletonTree"));

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

	InTabManager->UnregisterTabSpawner(FleshRingAssetEditorToolkit::SkeletonTreeTabId);
	InTabManager->UnregisterTabSpawner(FleshRingAssetEditorToolkit::ViewportTabId);
	InTabManager->UnregisterTabSpawner(FleshRingAssetEditorToolkit::DetailsTabId);
}

TSharedRef<SDockTab> FFleshRingAssetEditor::SpawnTab_SkeletonTree(const FSpawnTabArgs& Args)
{
	// Skeleton Tree 위젯 생성
	SkeletonTreeWidget = SNew(SFleshRingSkeletonTree)
		.Asset(EditingAsset)
		.OnBoneSelected(FOnBoneSelected::CreateSP(this, &FFleshRingAssetEditor::OnBoneSelected))
		.OnRingSelected(FOnRingSelected::CreateSP(this, &FFleshRingAssetEditor::OnRingSelected))
		.OnAddRingRequested(FOnAddRingRequested::CreateSP(this, &FFleshRingAssetEditor::OnAddRingRequested))
		.OnFocusCameraRequested(FOnFocusCameraRequested::CreateSP(this, &FFleshRingAssetEditor::OnFocusCameraRequested));

	return SNew(SDockTab)
		.Label(LOCTEXT("SkeletonTreeTabLabel", "Skeleton"))
		[
			SkeletonTreeWidget.ToSharedRef()
		];
}

TSharedRef<SDockTab> FFleshRingAssetEditor::SpawnTab_Viewport(const FSpawnTabArgs& Args)
{
	// 뷰포트 위젯 생성
	ViewportWidget = SNew(SFleshRingEditorViewport)
		.Asset(EditingAsset);

	// 뷰포트에서 본 선택 해제 시 스켈레톤 트리도 해제
	if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
	{
		ViewportClient->SetOnBoneSelectionCleared(
			FOnBoneSelectionCleared::CreateSP(this, &FFleshRingAssetEditor::OnBoneSelectionCleared));

		// 뷰포트에서 Ring 피킹 시 트리/디테일 패널 동기화
		ViewportClient->SetOnRingSelectedInViewport(
			FOnRingSelectedInViewport::CreateSP(this, &FFleshRingAssetEditor::OnRingSelectedInViewport));
	}

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
			PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayClear ||
			PropertyChangedEvent.ChangeType == EPropertyChangeType::Duplicate ||
			PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayMove)
		{
			// Ring 배열 구조 변경 시 전체 갱신 필요 (추가/삭제/클리어/복제/이동)
			bNeedsFullRefresh = true;
		}

		// Rings 배열 또는 그 하위 프로퍼티 변경 체크 (복사/붙여넣기 등)
		// MemberProperty는 배열 요소 변경 시 배열 자체를 가리킴
		if (PropertyChangedEvent.MemberProperty)
		{
			FName MemberName = PropertyChangedEvent.MemberProperty->GetFName();
			if (MemberName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, Rings))
			{
				// Rings 배열 내 어떤 변경이든 전체 갱신
				bNeedsFullRefresh = true;
			}
		}

		if (PropertyChangedEvent.Property)
		{
			FName PropName = PropertyChangedEvent.Property->GetFName();

			// TargetSkeletalMesh 변경 시 스켈레탈 메시 교체 필요
			if (PropName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, TargetSkeletalMesh))
			{
				bNeedsFullRefresh = true;

				// 스켈레탈 메시 변경 시 본 선택 상태 초기화
				if (ViewportWidget.IsValid())
				{
					TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
					if (ViewportClient.IsValid())
					{
						ViewportClient->ClearSelectedBone();
					}
				}
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

	// Skeleton Tree도 갱신 (Ring 마커 업데이트)
	if (SkeletonTreeWidget.IsValid())
	{
		SkeletonTreeWidget->RefreshTree();
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

void FFleshRingAssetEditor::OnBoneSelected(FName BoneName)
{
	// Skeleton Tree에서 본 선택 시 뷰포트에서 해당 본 하이라이트
	if (ViewportWidget.IsValid())
	{
		TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
		if (ViewportClient.IsValid())
		{
			ViewportClient->SetSelectedBone(BoneName);
		}
	}
}

void FFleshRingAssetEditor::OnBoneSelectionCleared()
{
	// 뷰포트에서 본 선택 해제 시 스켈레톤 트리도 해제
	if (SkeletonTreeWidget.IsValid())
	{
		SkeletonTreeWidget->ClearSelection();
	}
}

void FFleshRingAssetEditor::OnRingSelected(int32 RingIndex)
{
	// 뷰포트에서 동기화 중이면 뷰포트 업데이트 스킵 (순환 호출 방지)
	if (!bSyncingFromViewport)
	{
		// Ring의 부착 본 이름 가져오기
		FName AttachedBoneName = NAME_None;
		if (EditingAsset && EditingAsset->Rings.IsValidIndex(RingIndex))
		{
			AttachedBoneName = EditingAsset->Rings[RingIndex].BoneName;
		}

		// 뷰포트에서 Ring 선택 (부착된 본도 하이라이트)
		if (ViewportWidget.IsValid())
		{
			TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
			if (ViewportClient.IsValid())
			{
				ViewportClient->SelectRing(RingIndex, AttachedBoneName);
			}
		}
	}

	// Details 패널 하이라이트 처리 (항상 수행)
	if (DetailsView.IsValid())
	{
		if (RingIndex >= 0 && EditingAsset)
		{
			// Rings 배열 프로퍼티 찾기
			FProperty* RingsProperty = UFleshRingAsset::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UFleshRingAsset, Rings));
			if (RingsProperty)
			{
				// FPropertyPath 구성: Rings[RingIndex]
				TSharedRef<FPropertyPath> PropertyPath = FPropertyPath::CreateEmpty();
				PropertyPath->AddProperty(FPropertyInfo(RingsProperty, RingIndex));

				DetailsView->HighlightProperty(*PropertyPath);
			}
		}
		else
		{
			// Ring 선택 해제 시 하이라이트 해제
			DetailsView->HighlightProperty(*FPropertyPath::CreateEmpty());
		}
	}
}

void FFleshRingAssetEditor::OnRingSelectedInViewport(int32 RingIndex, EFleshRingSelectionType SelectionType)
{
	// 순환 호출 방지 플래그 설정
	bSyncingFromViewport = true;

	// 뷰포트에서 부착된 본 하이라이트 설정
	if (ViewportWidget.IsValid() && EditingAsset && EditingAsset->Rings.IsValidIndex(RingIndex))
	{
		FName AttachedBoneName = EditingAsset->Rings[RingIndex].BoneName;
		TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
		if (ViewportClient.IsValid())
		{
			ViewportClient->SetSelectedBone(AttachedBoneName);
		}
	}

	// 트리에서 해당 Ring 선택 (이 과정에서 OnRingSelected가 호출됨 -> Details 패널 하이라이트)
	if (SkeletonTreeWidget.IsValid())
	{
		SkeletonTreeWidget->SelectRingByIndex(RingIndex);
	}

	bSyncingFromViewport = false;
}

void FFleshRingAssetEditor::OnAddRingRequested(FName BoneName)
{
	// 선택한 본에 Ring 추가
	if (EditingAsset && !BoneName.IsNone())
	{
		// Undo/Redo 지원
		FScopedTransaction Transaction(FText::FromString(TEXT("Add Ring")));
		EditingAsset->Modify();

		// 새 Ring 추가
		FFleshRingSettings NewRing;
		NewRing.BoneName = BoneName;
		EditingAsset->Rings.Add(NewRing);

		// 뷰포트 갱신
		RefreshViewport();

		// Details 패널 갱신
		if (DetailsView.IsValid())
		{
			DetailsView->ForceRefresh();
		}
	}
}

void FFleshRingAssetEditor::OnFocusCameraRequested()
{
	// 뷰포트에 카메라 포커스 요청
	if (ViewportWidget.IsValid())
	{
		TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
		if (ViewportClient.IsValid())
		{
			ViewportClient->FocusOnMesh();
		}
	}
}

#undef LOCTEXT_NAMESPACE
