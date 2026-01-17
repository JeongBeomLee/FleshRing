// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingAssetEditor.h"
#include "FleshRingAssetEditorToolkit.h"
#include "FleshRingAsset.h"
#include "FleshRingComponent.h"
#include "FleshRingTypes.h"
#include "SFleshRingSkeletonTree.h"
#include "SFleshRingEditorViewport.h"
#include "FleshRingEditorViewportClient.h"
#include "FleshRingPreviewScene.h"
#include "SAdvancedPreviewDetailsTab.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/SBoxPanel.h"
#include "PropertyEditorModule.h"
#include "IDetailsView.h"
#include "PropertyPath.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"
#include "Editor.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Images/SThrobber.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "FleshRingAssetEditor"

FFleshRingAssetEditor::FFleshRingAssetEditor()
{
}

FFleshRingAssetEditor::~FFleshRingAssetEditor()
{
	// Ring 선택 변경 델리게이트 해제
	if (EditingAsset)
	{
		EditingAsset->OnRingSelectionChanged.RemoveAll(this);
	}

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
				// 우측: Details + Preview Settings (탭으로 전환)
				FTabManager::NewStack()
				->SetSizeCoefficient(0.3f)
				->AddTab(FleshRingAssetEditorToolkit::DetailsTabId, ETabState::OpenedTab)
				->AddTab(FleshRingAssetEditorToolkit::PreviewSettingsTabId, ETabState::OpenedTab)
				->SetForegroundTab(FleshRingAssetEditorToolkit::DetailsTabId)
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

	// Ring 선택 변경 델리게이트 구독 (디테일 패널 → 뷰포트/트리 동기화)
	if (EditingAsset)
	{
		EditingAsset->OnRingSelectionChanged.AddRaw(
			this, &FFleshRingAssetEditor::OnRingSelectionChangedFromDetails);
	}
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

	// Preview Scene Settings 탭 등록
	InTabManager->RegisterTabSpawner(
		FleshRingAssetEditorToolkit::PreviewSettingsTabId,
		FOnSpawnTab::CreateSP(this, &FFleshRingAssetEditor::SpawnTab_PreviewSettings))
		.SetDisplayName(LOCTEXT("PreviewSettingsTab", "Preview Scene Settings"))
		.SetGroup(WorkspaceMenuCategory.ToSharedRef())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "LevelEditor.Tabs.Details"));
}

void FFleshRingAssetEditor::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);

	InTabManager->UnregisterTabSpawner(FleshRingAssetEditorToolkit::SkeletonTreeTabId);
	InTabManager->UnregisterTabSpawner(FleshRingAssetEditorToolkit::ViewportTabId);
	InTabManager->UnregisterTabSpawner(FleshRingAssetEditorToolkit::DetailsTabId);
	InTabManager->UnregisterTabSpawner(FleshRingAssetEditorToolkit::PreviewSettingsTabId);
}

TSharedRef<SDockTab> FFleshRingAssetEditor::SpawnTab_SkeletonTree(const FSpawnTabArgs& Args)
{
	// Skeleton Tree 위젯 생성
	SkeletonTreeWidget = SNew(SFleshRingSkeletonTree)
		.Asset(EditingAsset)
		.OnBoneSelected(FOnBoneSelected::CreateSP(this, &FFleshRingAssetEditor::OnBoneSelected))
		.OnRingSelected(FOnRingSelected::CreateSP(this, &FFleshRingAssetEditor::OnRingSelected))
		.OnAddRingRequested(FOnAddRingRequested::CreateSP(this, &FFleshRingAssetEditor::OnAddRingRequested))
		.OnFocusCameraRequested(FOnFocusCameraRequested::CreateSP(this, &FFleshRingAssetEditor::OnFocusCameraRequested))
		.OnRingDeleted(FOnRingDeletedFromTree::CreateSP(this, &FFleshRingAssetEditor::HandleRingDeleted));

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

		// 뷰포트에서 Ring 삭제 시 공통 처리
		ViewportClient->SetOnRingDeletedInViewport(
			FOnRingDeletedInViewport::CreateSP(this, &FFleshRingAssetEditor::HandleRingDeleted));

		// 뷰포트에서 본 피킹 시 스켈레톤 트리 동기화
		ViewportClient->SetOnBoneSelectedInViewport(
			FOnBoneSelectedInViewport::CreateSP(this, &FFleshRingAssetEditor::OnBoneSelectedInViewport));

		// 뷰포트에서 Ring 추가 요청 시 콜백 (우클릭 메뉴)
		ViewportClient->SetOnAddRingAtPositionRequested(
			FOnAddRingAtPositionRequested::CreateSP(this, &FFleshRingAssetEditor::OnAddRingAtPositionRequested));
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

TSharedRef<SDockTab> FFleshRingAssetEditor::SpawnTab_PreviewSettings(const FSpawnTabArgs& Args)
{
	TSharedPtr<FFleshRingPreviewScene> PreviewScene;
	if (ViewportWidget.IsValid())
	{
		PreviewScene = ViewportWidget->GetPreviewScene();
	}

	TSharedRef<SWidget> Content = SNullWidget::NullWidget;
	if (PreviewScene.IsValid())
	{
		Content = SNew(SAdvancedPreviewDetailsTab, PreviewScene.ToSharedRef());
	}

	return SNew(SDockTab)
		.Label(LOCTEXT("PreviewSettingsTabLabel", "Preview Scene Settings"))
		[
			Content
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

			// 배열 삭제/클리어 시 선택 상태 초기화 (선택된 Ring이 삭제되었을 수 있음)
			if (PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayRemove ||
				PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayClear)
			{
				EditingAsset->EditorSelectedRingIndex = -1;
				EditingAsset->EditorSelectionType = EFleshRingSelectionType::None;
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
			// 트랜스폼 관련 프로퍼티는 전체 갱신 불필요 (경량 업데이트로 처리)
			// RingOffset, MeshOffset, RingRotation, MeshRotation, RingEulerRotation, MeshEulerRotation,
			// MeshScale, RingRadius, Strength, Falloff 등은 경량 업데이트만 필요
		}

		// Rings 배열의 구조 변경이 아닌 경우 (복사/붙여넣기 등으로 배열 전체가 바뀐 경우만 전체 갱신)
		// 개별 프로퍼티 변경은 위에서 처리됨
		if (!bNeedsFullRefresh && PropertyChangedEvent.MemberProperty)
		{
			FName MemberName = PropertyChangedEvent.MemberProperty->GetFName();
			if (MemberName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, Rings))
			{
				// 프로퍼티 정보 없이 Rings 배열만 변경된 경우 (복사/붙여넣기 등)
				// Interactive 변경(드래그 중)은 경량 업데이트로 처리
				if (!PropertyChangedEvent.Property &&
					PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
				{
					bNeedsFullRefresh = true;
				}
			}
		}

		if (bNeedsFullRefresh)
		{
			RefreshViewport();
		}
		else
		{
			// Ring 인덱스 추출: Rings 배열의 어떤 요소가 변경되었는지 확인
			int32 ChangedRingIndex = INDEX_NONE;

			// Ring 인덱스 추출: 에셋의 현재 선택된 Ring 사용
			// 에셋에 저장된 EditorSelectedRingIndex를 사용 (프로퍼티 수정 시 해당 Ring이 선택되어 있음)
			if (PropertyChangedEvent.MemberProperty)
			{
				FName MemberName = PropertyChangedEvent.MemberProperty->GetFName();
				if (MemberName == GET_MEMBER_NAME_CHECKED(UFleshRingAsset, Rings))
				{
					// 에셋의 선택된 Ring 인덱스 사용
					ChangedRingIndex = EditingAsset->EditorSelectedRingIndex;
				}
			}

			// 트랜스폼/파라미터 변경: 경량 업데이트 (깜빡임 방지)
			// 특정 Ring 인덱스를 전달하여 해당 Ring만 처리
			UpdateRingTransformsOnly(ChangedRingIndex);
		}
	}
}

void FFleshRingAssetEditor::RefreshViewport()
{
	// 트리 갱신 중 SelectRing() 호출 방지
	// (새 트랜잭션 생성으로 Undo 히스토리 꼬임 방지)
	bSyncingFromViewport = true;

	// 리프레시 수행
	if (ViewportWidget.IsValid())
	{
		ViewportWidget->RefreshPreview();

		// Show Flag를 새로 생성된 컴포넌트들에 적용
		if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
		{
			ViewportClient->ApplyShowFlagsToScene();
		}
	}

	// Skeleton Tree도 갱신 (Ring 마커 업데이트)
	if (SkeletonTreeWidget.IsValid())
	{
		SkeletonTreeWidget->RefreshTree();
	}

	// 에셋의 선택 상태를 뷰포트에 적용 (에셋이 선택 상태의 진실 소스)
	ApplySelectionFromAsset();

	bSyncingFromViewport = false;
}

void FFleshRingAssetEditor::UpdateRingTransformsOnly(int32 DirtyRingIndex)
{
	if (ViewportWidget.IsValid())
	{
		ViewportWidget->UpdateRingTransformsOnly(DirtyRingIndex);
	}
}

void FFleshRingAssetEditor::RefreshSDFOnly()
{
	if (ViewportWidget.IsValid())
	{
		ViewportWidget->RefreshSDFOnly();
	}
}

UFleshRingComponent* FFleshRingAssetEditor::GetPreviewFleshRingComponent() const
{
	if (ViewportWidget.IsValid())
	{
		TSharedPtr<FFleshRingPreviewScene> PreviewScene = ViewportWidget->GetPreviewScene();
		if (PreviewScene.IsValid())
		{
			return PreviewScene->GetFleshRingComponent();
		}
	}
	return nullptr;
}

void FFleshRingAssetEditor::TickPreviewScene(float DeltaTime)
{
	if (ViewportWidget.IsValid())
	{
		// PreviewScene 틱 (월드 및 컴포넌트 업데이트)
		TSharedPtr<FFleshRingPreviewScene> PreviewScene = ViewportWidget->GetPreviewScene();
		if (PreviewScene.IsValid())
		{
			PreviewScene->GetWorld()->Tick(LEVELTICK_All, DeltaTime);
		}

		// 뷰포트 클라이언트 틱 (렌더링 트리거)
		TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
		if (ViewportClient.IsValid())
		{
			ViewportClient->Tick(DeltaTime);
			ViewportClient->Invalidate();
		}
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
			// Undo 트랜잭션 완료 후 다음 틱에서 안전하게 갱신
			// (트랜잭션 중 RefreshViewport 호출 시 SkeletalMesh 본 데이터 불완전으로 크래시 발생)
			if (GEditor)
			{
				// Undo/Redo 중 선택 검증 스킵 (Tick에서 선택 해제 방지)
				// 뷰모드 저장 (Undo/Redo 후 복원용)
				EViewModeIndex SavedViewMode = VMI_Lit;
				if (ViewportWidget.IsValid())
				{
					TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
					if (ViewportClient.IsValid())
					{
						ViewportClient->SetSkipSelectionValidation(true);
						SavedViewMode = ViewportClient->GetViewMode();
					}
				}

				GEditor->GetTimerManager()->SetTimerForNextTick(
					[WeakThis = TWeakPtr<FFleshRingAssetEditor>(SharedThis(this)), SavedViewMode]()
					{
						if (TSharedPtr<FFleshRingAssetEditor> This = WeakThis.Pin())
						{
							// Undo/Redo 중 트리 갱신 시 SelectRing() 호출 방지
							// (새 트랜잭션 생성으로 Undo 히스토리 꼬임 방지)
							This->bSyncingFromViewport = true;

							// 뷰포트 갱신
							if (This->ViewportWidget.IsValid())
							{
								This->ViewportWidget->RefreshPreview();

								// 뷰모드 복원 (Undo/Redo로 인한 초기화 방지)
								if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = This->ViewportWidget->GetViewportClient())
								{
									ViewportClient->SetViewMode(SavedViewMode);
								}
							}
							if (This->SkeletonTreeWidget.IsValid())
							{
								This->SkeletonTreeWidget->RefreshTree();
							}

							// DetailsView 강제 갱신 (Undo/Redo 후 배열 상태 반영)
							if (This->DetailsView.IsValid())
							{
								This->DetailsView->ForceRefresh();
							}

							// 에셋의 선택 상태를 뷰포트에 반영 (Undo/Redo로 복원된 값)
							// (ApplySelectionFromAsset 내부에서도 bSyncingFromViewport 설정하지만
							//  이미 true이므로 중복 설정해도 무방)
							This->ApplySelectionFromAsset();

							This->bSyncingFromViewport = false;

							// DetailsView 하이라이트를 다음 틱에서 다시 적용
							// (ForceRefresh() 후 즉시 HighlightProperty가 작동하지 않을 수 있음)
							if (This->EditingAsset && GEditor)
							{
								int32 RingIndex = This->EditingAsset->EditorSelectedRingIndex;
								if (RingIndex >= 0 && This->EditingAsset->Rings.IsValidIndex(RingIndex))
								{
									GEditor->GetTimerManager()->SetTimerForNextTick(
										[WeakThis, RingIndex]()
										{
											if (TSharedPtr<FFleshRingAssetEditor> InnerThis = WeakThis.Pin())
											{
												if (InnerThis->DetailsView.IsValid() && InnerThis->EditingAsset &&
													InnerThis->EditingAsset->Rings.IsValidIndex(RingIndex))
												{
													FProperty* RingsProperty = UFleshRingAsset::StaticClass()->FindPropertyByName(
														GET_MEMBER_NAME_CHECKED(UFleshRingAsset, Rings));
													if (RingsProperty)
													{
														TSharedRef<FPropertyPath> PropertyPath = FPropertyPath::CreateEmpty();
														PropertyPath->AddProperty(FPropertyInfo(RingsProperty, RingIndex));
														InnerThis->DetailsView->HighlightProperty(*PropertyPath);
													}
												}
											}
										});
								}
							}

							// 선택 검증 다시 활성화 - 0.2초 후
							// (SetFleshRingAsset의 0.1초 Deformer 초기화 타이머보다 늦게 해제해야 함)
							if (This->ViewportWidget.IsValid() && GEditor)
							{
								TWeakPtr<FFleshRingEditorViewportClient> WeakClient = This->ViewportWidget->GetViewportClient();
								FTimerHandle TimerHandle;
								GEditor->GetTimerManager()->SetTimer(
									TimerHandle,
									[WeakClient]()
									{
										if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakClient.Pin())
										{
											Client->SetSkipSelectionValidation(false);
										}
									},
									0.2f,  // SetFleshRingAsset의 0.1초보다 길게
									false
								);
							}
						}
					});
			}
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

void FFleshRingAssetEditor::OnBoneSelectedInViewport(FName BoneName)
{
	// 뷰포트에서 본 피킹 시 스켈레톤 트리에서 해당 본 선택
	if (SkeletonTreeWidget.IsValid())
	{
		SkeletonTreeWidget->SelectBone(BoneName);
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

void FFleshRingAssetEditor::OnAddRingRequested(FName BoneName, UStaticMesh* SelectedMesh)
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

		// 선택된 메쉬 설정
		if (SelectedMesh)
		{
			NewRing.RingMesh = SelectedMesh;
		}

		// 본의 가중치 자식 기반으로 기본 회전 계산
		if (ViewportWidget.IsValid())
		{
			if (TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient())
			{
				FRotator DefaultRotation = ViewportClient->CalculateDefaultRingRotationForBone(BoneName);

				// Ring 회전 설정
				NewRing.RingEulerRotation = DefaultRotation;
				NewRing.RingRotation = DefaultRotation.Quaternion();

				// Mesh 회전도 같이 설정 (Auto 모드에서는 MeshRotation 사용)
				NewRing.MeshEulerRotation = DefaultRotation;
				NewRing.MeshRotation = DefaultRotation.Quaternion();
			}
		}

		EditingAsset->Rings.Add(NewRing);

		// 새로 추가된 Ring 선택
		int32 NewRingIndex = EditingAsset->Rings.Num() - 1;
		EditingAsset->EditorSelectedRingIndex = NewRingIndex;

		// 뷰포트 갱신
		RefreshViewport();

		// 스켈레톤 트리 갱신 및 새 Ring 선택
		if (SkeletonTreeWidget.IsValid())
		{
			SkeletonTreeWidget->RefreshTree();
			SkeletonTreeWidget->SelectRingByIndex(NewRingIndex);
		}

		// Details 패널 갱신 후 새 Ring 선택 (ForceRefresh가 하이라이트를 초기화하므로)
		if (DetailsView.IsValid())
		{
			DetailsView->ForceRefresh();
		}

		// 새로 추가된 Ring 선택 (디테일 패널 하이라이트 포함)
		OnRingSelected(NewRingIndex);
	}
}

void FFleshRingAssetEditor::OnAddRingAtPositionRequested(FName BoneName, const FVector& LocalOffset, const FRotator& LocalRotation, UStaticMesh* SelectedMesh)
{
	// 뷰포트 우클릭에서 Ring 추가 (위치 및 메쉬 포함, 메쉬는 선택사항)
	if (!EditingAsset || BoneName.IsNone())
	{
		return;
	}

	// Undo/Redo 지원
	FScopedTransaction Transaction(NSLOCTEXT("FleshRingEditor", "AddRingAtPosition", "Add Ring at Position"));
	EditingAsset->Modify();

	// 새 Ring 생성
	FFleshRingSettings NewRing;
	NewRing.BoneName = BoneName;

	// RingOffset과 MeshOffset 둘 다 같은 위치에 설정 (본 로컬 공간)
	NewRing.RingOffset = LocalOffset;
	NewRing.MeshOffset = LocalOffset;

	// Ring 회전 설정 (녹색 라인 방향이 Z축이 되도록)
	NewRing.RingEulerRotation = LocalRotation;
	NewRing.RingRotation = LocalRotation.Quaternion();

	// Mesh 회전도 같이 설정 (Auto 모드에서는 MeshRotation 사용)
	NewRing.MeshEulerRotation = LocalRotation;
	NewRing.MeshRotation = LocalRotation.Quaternion();

	// 선택된 메쉬 설정 (nullptr이면 메쉬 없이 추가)
	if (SelectedMesh)
	{
		NewRing.RingMesh = SelectedMesh;
	}

	EditingAsset->Rings.Add(NewRing);

	// 새로 추가된 Ring 선택
	int32 NewRingIndex = EditingAsset->Rings.Num() - 1;
	EditingAsset->EditorSelectedRingIndex = NewRingIndex;

	// 뷰포트 갱신
	RefreshViewport();

	// 스켈레톤 트리 갱신 및 새 Ring 선택
	if (SkeletonTreeWidget.IsValid())
	{
		SkeletonTreeWidget->RefreshTree();
		SkeletonTreeWidget->SelectRingByIndex(NewRingIndex);
	}

	// Details 패널 갱신 후 새 Ring 선택 (ForceRefresh가 하이라이트를 초기화하므로)
	if (DetailsView.IsValid())
	{
		DetailsView->ForceRefresh();
	}

	// 새로 추가된 Ring 선택 (디테일 패널 하이라이트 포함)
	OnRingSelected(NewRingIndex);
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

void FFleshRingAssetEditor::HandleRingDeleted()
{
	// Ring 삭제 공통 처리 (뷰포트/트리/디테일 어디서든 호출됨)
	bSyncingFromViewport = true;

	// 뷰포트 갱신 (Ring 메시 컴포넌트 재생성)
	if (ViewportWidget.IsValid())
	{
		ViewportWidget->RefreshPreview();

		// ViewportClient 선택 상태 갱신
		TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
		if (ViewportClient.IsValid())
		{
			ViewportClient->SetSelectionType(EFleshRingSelectionType::None);
		}
	}

	// 트리 갱신
	if (SkeletonTreeWidget.IsValid())
	{
		SkeletonTreeWidget->RefreshTree();
	}

	bSyncingFromViewport = false;
}

void FFleshRingAssetEditor::ApplySelectionFromAsset()
{
	if (!EditingAsset || !ViewportWidget.IsValid())
	{
		return;
	}

	// 에셋에서 선택 상태 읽기
	int32 RingIndex = EditingAsset->EditorSelectedRingIndex;
	EFleshRingSelectionType SelectionType = EditingAsset->EditorSelectionType;

	// PreviewScene에 선택 인덱스 설정
	TSharedPtr<FFleshRingPreviewScene> PreviewScene = ViewportWidget->GetPreviewScene();
	if (PreviewScene.IsValid())
	{
		PreviewScene->SetSelectedRingIndex(RingIndex);
	}

	// ViewportClient에 선택 타입 설정
	TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportWidget->GetViewportClient();
	if (ViewportClient.IsValid())
	{
		ViewportClient->SetSelectionType(SelectionType);

		// 선택된 Ring이 있으면 부착된 본도 하이라이트
		if (RingIndex >= 0 && EditingAsset->Rings.IsValidIndex(RingIndex))
		{
			FName AttachedBoneName = EditingAsset->Rings[RingIndex].BoneName;
			ViewportClient->SetSelectedBone(AttachedBoneName);
		}
		else
		{
			// 선택 해제 시 본 하이라이트도 해제
			ViewportClient->ClearSelectedBone();
		}
	}

	// 트리에서 해당 Ring 선택
	// bSyncingFromViewport = true로 설정하여 OnRingSelected에서 SelectRing() 호출 방지
	// (SelectRing()은 새 트랜잭션을 생성하므로 Undo 히스토리가 꼬임)
	if (SkeletonTreeWidget.IsValid())
	{
		bSyncingFromViewport = true;
		SkeletonTreeWidget->SelectRingByIndex(RingIndex);
		bSyncingFromViewport = false;
	}

	// Details 패널 하이라이트 (Undo/Redo 시 확실하게 복원)
	if (DetailsView.IsValid())
	{
		if (RingIndex >= 0 && EditingAsset->Rings.IsValidIndex(RingIndex))
		{
			FProperty* RingsProperty = UFleshRingAsset::StaticClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UFleshRingAsset, Rings));
			if (RingsProperty)
			{
				TSharedRef<FPropertyPath> PropertyPath = FPropertyPath::CreateEmpty();
				PropertyPath->AddProperty(FPropertyInfo(RingsProperty, RingIndex));
				DetailsView->HighlightProperty(*PropertyPath);
			}
		}
		else
		{
			DetailsView->HighlightProperty(*FPropertyPath::CreateEmpty());
		}
	}
}

void FFleshRingAssetEditor::OnRingSelectionChangedFromDetails(int32 RingIndex)
{
	// 순환 호출 방지 (뷰포트/트리에서 이미 선택 중인 경우)
	if (bSyncingFromViewport)
	{
		return;
	}

	// 디테일 패널에서 Ring 클릭 시 뷰포트/트리 동기화
	bSyncingFromViewport = true;
	ApplySelectionFromAsset();
	bSyncingFromViewport = false;
}

void FFleshRingAssetEditor::ShowBakeOverlay(bool bShow, const FText& Message)
{
	if (bShow && !bBakeOverlayVisible)
	{
		// 오버레이 윈도우 생성
		TSharedPtr<SWindow> ParentWindow = FSlateApplication::Get().FindWidgetWindow(ViewportWidget.ToSharedRef());
		if (!ParentWindow.IsValid())
		{
			// 폴백: 활성 윈도우 사용
			ParentWindow = FSlateApplication::Get().GetActiveTopLevelWindow();
		}

		if (ParentWindow.IsValid())
		{
			FText DisplayMessage = Message.IsEmpty() ? LOCTEXT("BakingOverlay", "Baking mesh...\nPlease wait.") : Message;

			BakeOverlayWindow = SNew(SWindow)
				.Type(EWindowType::Normal)
				.Style(&FAppStyle::Get().GetWidgetStyle<FWindowStyle>("Window"))
				.Title(LOCTEXT("BakeOverlayTitle", "Baking"))
				.SizingRule(ESizingRule::FixedSize)
				.ClientSize(FVector2D(300, 100))
				.SupportsMaximize(false)
				.SupportsMinimize(false)
				.HasCloseButton(false)
				.CreateTitleBar(true)
				.IsTopmostWindow(true)
				.FocusWhenFirstShown(true)
				[
					SNew(SBox)
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Padding(20.f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.HAlign(HAlign_Center)
						[
							SNew(SCircularThrobber)
							.Radius(16.f)
						]
						+ SVerticalBox::Slot()
						.AutoHeight()
						.HAlign(HAlign_Center)
						.Padding(0, 10, 0, 0)
						[
							SNew(STextBlock)
							.Text(DisplayMessage)
							.Justification(ETextJustify::Center)
						]
					]
				];

			FSlateApplication::Get().AddWindowAsNativeChild(BakeOverlayWindow.ToSharedRef(), ParentWindow.ToSharedRef(), true);

			// 부모 윈도우 중앙에 배치
			FVector2D ParentSize = ParentWindow->GetClientSizeInScreen();
			FVector2D ParentPos = ParentWindow->GetPositionInScreen();
			FVector2D OverlaySize = BakeOverlayWindow->GetClientSizeInScreen();
			FVector2D CenteredPos = ParentPos + (ParentSize - OverlaySize) * 0.5f;
			BakeOverlayWindow->MoveWindowTo(CenteredPos);
		}

		bBakeOverlayVisible = true;
	}
	else if (!bShow && bBakeOverlayVisible)
	{
		// 오버레이 윈도우 제거
		if (BakeOverlayWindow.IsValid())
		{
			BakeOverlayWindow->RequestDestroyWindow();
			BakeOverlayWindow.Reset();
		}

		bBakeOverlayVisible = false;
	}
}

#undef LOCTEXT_NAMESPACE
