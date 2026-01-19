// Copyright Epic Games, Inc. All Rights Reserved.

#include "FSubdivisionSettingsCustomization.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailChildrenBuilder.h"
#include "IDetailGroup.h"
#include "FleshRingAsset.h"
#include "FleshRingComponent.h"
#include "FleshRingTypes.h"
#include "FleshRingAssetEditor.h"
#include "FleshRingDeformerInstance.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Editor.h"
#include "RenderingThread.h"
#include "Misc/ScopedSlowTask.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"
#include "UObject/UObjectGlobals.h"  // CollectGarbage용
#include "FileHelpers.h"  // FEditorFileUtils::PromptForCheckoutAndSave용

#define LOCTEXT_NAMESPACE "SubdivisionSettingsCustomization"

FSubdivisionSettingsCustomization::FSubdivisionSettingsCustomization()
{
}

FSubdivisionSettingsCustomization::~FSubdivisionSettingsCustomization()
{
	// 진행 중인 비동기 베이크 정리
	if (bAsyncBakeInProgress)
	{
		CleanupAsyncBake(true);
	}
}

TSharedRef<IPropertyTypeCustomization> FSubdivisionSettingsCustomization::MakeInstance()
{
	return MakeShareable(new FSubdivisionSettingsCustomization);
}

void FSubdivisionSettingsCustomization::CustomizeHeader(
	TSharedRef<IPropertyHandle> PropertyHandle,
	FDetailWidgetRow& HeaderRow,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	MainPropertyHandle = PropertyHandle;

	// 헤더 숨기기 - 카테고리 이름만 표시되도록
	// (구조체 이름 "Subdivision Settings" 중복 방지)
}

void FSubdivisionSettingsCustomization::CustomizeChildren(
	TSharedRef<IPropertyHandle> PropertyHandle,
	IDetailChildrenBuilder& ChildBuilder,
	IPropertyTypeCustomizationUtils& CustomizationUtils)
{
	// 자식 프로퍼티 핸들 가져오기
	TSharedPtr<IPropertyHandle> bEnableSubdivisionHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, bEnableSubdivision));
	TSharedPtr<IPropertyHandle> MinEdgeLengthHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, MinEdgeLength));
	TSharedPtr<IPropertyHandle> PreviewSubdivisionLevelHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, PreviewSubdivisionLevel));
	TSharedPtr<IPropertyHandle> PreviewBoneHopCountHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, PreviewBoneHopCount));
	TSharedPtr<IPropertyHandle> PreviewBoneWeightThresholdHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, PreviewBoneWeightThreshold));
	TSharedPtr<IPropertyHandle> MaxSubdivisionLevelHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, MaxSubdivisionLevel));
	TSharedPtr<IPropertyHandle> SubdividedMeshHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, SubdividedMesh));
	TSharedPtr<IPropertyHandle> BakedMeshHandle = PropertyHandle->GetChildHandle(GET_MEMBER_NAME_CHECKED(FSubdivisionSettings, BakedMesh));

	// =====================================
	// 공통 설정 (최상위)
	// =====================================
	if (bEnableSubdivisionHandle.IsValid())
	{
		ChildBuilder.AddProperty(bEnableSubdivisionHandle.ToSharedRef());
	}
	if (MinEdgeLengthHandle.IsValid())
	{
		ChildBuilder.AddProperty(MinEdgeLengthHandle.ToSharedRef());
	}

	// =====================================
	// Editor Preview 서브그룹
	// =====================================
	IDetailGroup& EditorPreviewGroup = ChildBuilder.AddGroup(
		TEXT("EditorPreview"),
		LOCTEXT("EditorPreviewGroup", "Editor Preview")
	);

	if (PreviewSubdivisionLevelHandle.IsValid())
	{
		EditorPreviewGroup.AddPropertyRow(PreviewSubdivisionLevelHandle.ToSharedRef());
	}
	if (PreviewBoneHopCountHandle.IsValid())
	{
		EditorPreviewGroup.AddPropertyRow(PreviewBoneHopCountHandle.ToSharedRef());
	}
	if (PreviewBoneWeightThresholdHandle.IsValid())
	{
		EditorPreviewGroup.AddPropertyRow(PreviewBoneWeightThresholdHandle.ToSharedRef());
	}

	// Refresh Preview 버튼
	EditorPreviewGroup.AddWidgetRow()
	.WholeRowContent()
	[
		SNew(SBox)
		.Padding(FMargin(0, 2, 0, 2))
		.HAlign(HAlign_Center)
		[
			SNew(SBox)
			.MinDesiredWidth(360.0f)
			[
				SNew(SButton)
				.OnClicked(this, &FSubdivisionSettingsCustomization::OnRefreshPreviewClicked)
				[
					SNew(SBox)
					.HAlign(HAlign_Center)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0, 0, 8, 0)
						[
							SNew(SImage)
							.Image(FAppStyle::GetBrush("Icons.Refresh"))
							.ColorAndOpacity(FSlateColor(FLinearColor(0.3f, 0.7f, 1.0f)))
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text(LOCTEXT("RefreshPreview", "Refresh Preview Mesh"))
						]
					]
				]
			]
		]
	];

	// =====================================
	// Baked Mesh 서브그룹 (런타임용, 변형 적용 완료)
	// =====================================
	IDetailGroup& BakedMeshGroup = ChildBuilder.AddGroup(
		TEXT("BakedMesh"),
		LOCTEXT("BakedMeshGroup", "Baked Mesh")
	);

	if (MaxSubdivisionLevelHandle.IsValid())
	{
		BakedMeshGroup.AddPropertyRow(MaxSubdivisionLevelHandle.ToSharedRef());
	}

	// Bake + Clear 버튼
	BakedMeshGroup.AddWidgetRow()
	.WholeRowContent()
	[
		SNew(SBox)
		.Padding(FMargin(0, 2, 0, 2))
		.HAlign(HAlign_Center)
		[
			SNew(SHorizontalBox)
			// Bake 버튼 (녹색)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(0, 0, 4, 0)
			[
				SNew(SBox)
				.MinDesiredWidth(180.0f)
				[
					SNew(SButton)
					.OnClicked(this, &FSubdivisionSettingsCustomization::OnBakeMeshClicked)
					[
						SNew(SBox)
						.HAlign(HAlign_Center)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(0, 0, 4, 0)
							[
								SNew(SImage)
								.Image(FAppStyle::GetBrush("Icons.Plus"))
								.ColorAndOpacity(FSlateColor(FLinearColor(0.3f, 0.9f, 0.3f)))
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("BakeMesh", "Bake"))
							]
						]
					]
				]
			]
			// Clear 버튼 (빨간색)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(4, 0, 0, 0)
			[
				SNew(SBox)
				.MinDesiredWidth(180.0f)
				[
					SNew(SButton)
					.OnClicked(this, &FSubdivisionSettingsCustomization::OnClearBakedMeshClicked)
					[
						SNew(SBox)
						.HAlign(HAlign_Center)
						[
							SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							.Padding(0, 0, 4, 0)
							[
								SNew(SImage)
								.Image(FAppStyle::GetBrush("Icons.X"))
								.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.3f, 0.3f)))
							]
							+ SHorizontalBox::Slot()
							.AutoWidth()
							.VAlign(VAlign_Center)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("ClearMesh", "Clear"))
							]
						]
					]
				]
			]
		]
	];

	// Baked Mesh 프로퍼티 (읽기 전용)
	if (BakedMeshHandle.IsValid())
	{
		BakedMeshGroup.AddPropertyRow(BakedMeshHandle.ToSharedRef());
	}
}

UFleshRingAsset* FSubdivisionSettingsCustomization::GetOuterAsset() const
{
	if (!MainPropertyHandle.IsValid())
	{
		return nullptr;
	}

	TArray<UObject*> OuterObjects;
	MainPropertyHandle->GetOuterObjects(OuterObjects);

	if (OuterObjects.Num() > 0)
	{
		return Cast<UFleshRingAsset>(OuterObjects[0]);
	}
	return nullptr;
}

bool FSubdivisionSettingsCustomization::IsSubdivisionEnabled() const
{
	UFleshRingAsset* Asset = GetOuterAsset();
	return Asset && Asset->SubdivisionSettings.bEnableSubdivision;
}

void FSubdivisionSettingsCustomization::SaveAsset(UFleshRingAsset* Asset)
{
	if (!Asset)
	{
		return;
	}

	UPackage* Package = Asset->GetOutermost();
	if (Package && Package->IsDirty())
	{
		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Package);
		FEditorFileUtils::PromptForCheckoutAndSave(PackagesToSave, false, false);
	}
}

FReply FSubdivisionSettingsCustomization::OnRefreshPreviewClicked()
{
	UFleshRingAsset* Asset = GetOuterAsset();
	if (Asset && GEditor)
	{
		// 에디터 찾아서 PreviewScene의 메시 강제 재생성
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (AssetEditorSubsystem)
		{
			TArray<IAssetEditorInstance*> Editors = AssetEditorSubsystem->FindEditorsForAsset(Asset);
			for (IAssetEditorInstance* Editor : Editors)
			{
				FFleshRingAssetEditor* FleshRingEditor = static_cast<FFleshRingAssetEditor*>(Editor);
				if (FleshRingEditor)
				{
					FleshRingEditor->ForceRefreshPreviewMesh();
					break;
				}
			}
		}
	}
	return FReply::Handled();
}

FReply FSubdivisionSettingsCustomization::OnGenerateSubdividedMeshClicked()
{
	UFleshRingAsset* Asset = GetOuterAsset();
	if (Asset)
	{
		// UAssetEditorSubsystem을 통해 열린 에디터에서 PreviewComponent 가져오기
		UFleshRingComponent* PreviewComponent = nullptr;

		if (GEditor)
		{
			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (AssetEditorSubsystem)
			{
				TArray<IAssetEditorInstance*> Editors = AssetEditorSubsystem->FindEditorsForAsset(Asset);
				for (IAssetEditorInstance* Editor : Editors)
				{
					// FFleshRingAssetEditor는 FAssetEditorToolkit을 상속
					FFleshRingAssetEditor* FleshRingEditor = static_cast<FFleshRingAssetEditor*>(Editor);
					if (FleshRingEditor)
					{
						PreviewComponent = FleshRingEditor->GetPreviewFleshRingComponent();
						if (PreviewComponent)
						{
							break;
						}
					}
				}
			}
		}

		Asset->GenerateSubdividedMesh(PreviewComponent);
	}
	return FReply::Handled();
}

FReply FSubdivisionSettingsCustomization::OnClearSubdividedMeshClicked()
{
	UFleshRingAsset* Asset = GetOuterAsset();
	if (Asset)
	{
		Asset->ClearSubdividedMesh();
		// ★ 메모리 누수 방지: 버튼 클릭 시 즉시 GC 실행
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}
	return FReply::Handled();
}

FReply FSubdivisionSettingsCustomization::OnBakeMeshClicked()
{
	// 이미 베이크 진행 중이면 무시
	if (bAsyncBakeInProgress)
	{
		UE_LOG(LogTemp, Warning, TEXT("Bake already in progress. Please wait..."));
		return FReply::Handled();
	}

	UFleshRingAsset* Asset = GetOuterAsset();
	if (!Asset)
	{
		return FReply::Handled();
	}

	// ★ 기존 BakedMesh가 다른 에디터에서 열려있으면 먼저 닫기 (크래시 방지)
	if (Asset->HasBakedMesh())
	{
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (AssetEditorSubsystem)
		{
			USkeletalMesh* ExistingBakedMesh = Asset->SubdivisionSettings.BakedMesh;
			if (ExistingBakedMesh)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(ExistingBakedMesh);
			}
		}
	}

	// UAssetEditorSubsystem을 통해 열린 에디터와 PreviewComponent 가져오기
	FFleshRingAssetEditor* FleshRingEditor = nullptr;
	UFleshRingComponent* PreviewComponent = nullptr;

	if (GEditor)
	{
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (AssetEditorSubsystem)
		{
			TArray<IAssetEditorInstance*> Editors = AssetEditorSubsystem->FindEditorsForAsset(Asset);
			for (IAssetEditorInstance* Editor : Editors)
			{
				FleshRingEditor = static_cast<FFleshRingAssetEditor*>(Editor);
				if (FleshRingEditor)
				{
					PreviewComponent = FleshRingEditor->GetPreviewFleshRingComponent();
					if (PreviewComponent)
					{
						break;
					}
				}
			}
		}
	}

	if (!PreviewComponent || !FleshRingEditor)
	{
		UE_LOG(LogTemp, Warning, TEXT("Cannot bake mesh: PreviewComponent or Editor not found. Please ensure the asset editor is open."));
		return FReply::Handled();
	}

	// ★ Deformer가 없으면 강제 초기화 (서브디비전 OFF 상태에서도 베이크 가능하도록)
	if (!PreviewComponent->GetDeformer())
	{
		UE_LOG(LogTemp, Log, TEXT("Bake: Deformer not found, force initializing for editor preview..."));
		PreviewComponent->ForceInitializeForEditorPreview();
		FlushRenderingCommands();

		// 초기화 후에도 Deformer가 없으면 에러
		if (!PreviewComponent->GetDeformer())
		{
			UE_LOG(LogTemp, Warning, TEXT("Cannot bake mesh: Failed to initialize Deformer. Please check Ring settings (need valid SDF or Manual mode)."));
			return FReply::Handled();
		}
	}

	// 현재 프리뷰 메시 저장 (나중에 복원용)
	USkeletalMeshComponent* SkelMeshComp = PreviewComponent->GetResolvedTargetMesh();
	if (SkelMeshComp)
	{
		OriginalPreviewMesh = SkelMeshComp->GetSkeletalMeshAsset();
	}

	// ★ 비동기 베이크 시작 (오버레이 + FTSTicker)
	bAsyncBakeInProgress = true;
	AsyncBakeFrameCount = 0;
	PostCacheValidFrameCount = 0;
	AsyncBakeAsset = Asset;
	AsyncBakeComponent = PreviewComponent;

	// 오버레이 표시 (입력 차단)
	FleshRingEditor->ShowBakeOverlay(true, LOCTEXT("BakingMeshOverlay", "Baking mesh...\nPlease wait."));

	// 메시 스왑 시작 (캐시 무효화)
	FlushRenderingCommands();
	bool bImmediateSuccess = Asset->GenerateBakedMesh(PreviewComponent);

	if (bImmediateSuccess)
	{
		// 즉시 성공 (이미 캐시가 있었던 경우)
		UE_LOG(LogTemp, Log, TEXT("Bake completed immediately."));
		FleshRingEditor->ShowBakeOverlay(false);
		bAsyncBakeInProgress = false;
		RestoreOriginalPreviewMesh(PreviewComponent);

		// ★ 메모리 누수 방지: 즉시 성공 경로에서도 GC 호출
		FlushRenderingCommands();
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

		// ★ 자동 저장 (Perforce 체크아웃 프롬프트 포함)
		SaveAsset(Asset);

		return FReply::Handled();
	}

	// 비동기 틱커 시작 (렌더링 계속되면서 GPU 작업 완료 대기)
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateSP(this, &FSubdivisionSettingsCustomization::OnAsyncBakeTick),
		0.016f  // ~60fps
	);

	return FReply::Handled();
}

FReply FSubdivisionSettingsCustomization::OnClearBakedMeshClicked()
{
	UFleshRingAsset* Asset = GetOuterAsset();
	if (Asset)
	{
		// ★ 기존 BakedMesh가 다른 에디터에서 열려있으면 먼저 닫기 (크래시 방지)
		if (Asset->HasBakedMesh())
		{
			UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (AssetEditorSubsystem)
			{
				USkeletalMesh* ExistingBakedMesh = Asset->SubdivisionSettings.BakedMesh;
				if (ExistingBakedMesh)
				{
					AssetEditorSubsystem->CloseAllEditorsForAsset(ExistingBakedMesh);
				}
			}
		}

		Asset->ClearBakedMesh();
		// ★ 메모리 누수 방지: 버튼 클릭 시 즉시 GC 실행
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

		// ★ 자동 저장 (Perforce 체크아웃 프롬프트 포함)
		SaveAsset(Asset);
	}
	return FReply::Handled();
}

bool FSubdivisionSettingsCustomization::OnAsyncBakeTick(float DeltaTime)
{
	// 유효성 검사
	if (!AsyncBakeAsset.IsValid() || !AsyncBakeComponent.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Async bake failed: Asset or Component became invalid."));
		CleanupAsyncBake(true);
		return false;  // 틱커 중지
	}

	++AsyncBakeFrameCount;

	// Deformer 캐시 상태 확인
	UFleshRingDeformer* Deformer = AsyncBakeComponent->GetDeformer();
	if (Deformer)
	{
		UFleshRingDeformerInstance* Instance = Deformer->GetActiveInstance();
		if (Instance && Instance->HasCachedDeformedGeometry(0))
		{
			// 캐시가 유효해짐 - 추가 프레임 대기 (GPU 연산 완료 보장)
			++PostCacheValidFrameCount;

			if (PostCacheValidFrameCount < PostCacheValidWaitFrames)
			{
				UE_LOG(LogTemp, Log, TEXT("Cache valid, waiting additional frame %d/%d for GPU completion..."),
					PostCacheValidFrameCount, PostCacheValidWaitFrames);
				return true;  // 계속 대기
			}

			// 충분히 대기했으므로 GPU 명령 플러시 후 베이킹 시도
			FlushRenderingCommands();

			bool bSuccess = AsyncBakeAsset->GenerateBakedMesh(AsyncBakeComponent.Get());

			if (bSuccess)
			{
				UE_LOG(LogTemp, Log, TEXT("Async bake completed successfully after %d frames (%d post-cache frames)."),
					AsyncBakeFrameCount, PostCacheValidFrameCount);
				CleanupAsyncBake(true);
				return false;  // 틱커 중지
			}
			else
			{
				// 캐시가 유효한데 실패? 뭔가 문제가 있음
				UE_LOG(LogTemp, Warning, TEXT("Async bake failed even with valid cache. Retrying..."));
			}
		}
	}

	// 최대 프레임 초과 확인
	if (AsyncBakeFrameCount >= MaxAsyncBakeFrames)
	{
		UE_LOG(LogTemp, Warning, TEXT("Async bake timed out after %d frames. Deformer may not have processed the mesh."), MaxAsyncBakeFrames);
		CleanupAsyncBake(true);
		return false;  // 틱커 중지
	}

	// 계속 대기
	return true;  // 틱커 계속
}

void FSubdivisionSettingsCustomization::CleanupAsyncBake(bool bRestorePreviewMesh)
{
	// 틱커 제거
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	// 오버레이 숨김
	if (AsyncBakeAsset.IsValid() && GEditor)
	{
		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (AssetEditorSubsystem)
		{
			TArray<IAssetEditorInstance*> Editors = AssetEditorSubsystem->FindEditorsForAsset(AsyncBakeAsset.Get());
			for (IAssetEditorInstance* Editor : Editors)
			{
				FFleshRingAssetEditor* FleshRingEditor = static_cast<FFleshRingAssetEditor*>(Editor);
				if (FleshRingEditor)
				{
					FleshRingEditor->ShowBakeOverlay(false);
					break;
				}
			}
		}
	}

	// 원본 프리뷰 메시 복원
	if (bRestorePreviewMesh && AsyncBakeComponent.IsValid() && OriginalPreviewMesh.IsValid())
	{
		USkeletalMeshComponent* SkelMeshComp = AsyncBakeComponent->GetResolvedTargetMesh();
		if (SkelMeshComp && SkelMeshComp->GetSkeletalMeshAsset() != OriginalPreviewMesh.Get())
		{
			UE_LOG(LogTemp, Log, TEXT("Restoring original preview mesh..."));

			// 기존 버퍼 해제
			if (UFleshRingDeformer* Deformer = AsyncBakeComponent->GetDeformer())
			{
				if (UFleshRingDeformerInstance* Instance = Deformer->GetActiveInstance())
				{
					Instance->ReleaseResources();
				}
			}
			FlushRenderingCommands();

			// 원본 메시로 복원
			SkelMeshComp->SetSkeletalMeshAsset(OriginalPreviewMesh.Get());
			SkelMeshComp->MarkRenderStateDirty();
			SkelMeshComp->MarkRenderDynamicDataDirty();
			FlushRenderingCommands();
		}
	}

	// ★ 메모리 누수 방지: 원본 메시 복원 후 GC 실행
	// 이 시점에서야 SubdividedMesh/BakedMesh에 대한 참조가 모두 해제됨
	// Bake 중 오버레이로 대기 중이므로 동기 GC 비용 허용 가능
	FlushRenderingCommands();  // 렌더 스레드 완료 대기 (조건문 미진입 시에도 필요)
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	// ★ 자동 저장 (Perforce 체크아웃 프롬프트 포함)
	SaveAsset(AsyncBakeAsset.Get());

	// 상태 초기화
	bAsyncBakeInProgress = false;
	AsyncBakeFrameCount = 0;
	PostCacheValidFrameCount = 0;
	AsyncBakeAsset.Reset();
	AsyncBakeComponent.Reset();
	OriginalPreviewMesh.Reset();
}

void FSubdivisionSettingsCustomization::RestoreOriginalPreviewMesh(UFleshRingComponent* PreviewComponent)
{
	if (!PreviewComponent || !OriginalPreviewMesh.IsValid())
	{
		return;
	}

	USkeletalMeshComponent* SkelMeshComp = PreviewComponent->GetResolvedTargetMesh();
	if (SkelMeshComp && SkelMeshComp->GetSkeletalMeshAsset() != OriginalPreviewMesh.Get())
	{
		UE_LOG(LogTemp, Log, TEXT("Restoring original preview mesh..."));

		// 기존 버퍼 해제
		if (UFleshRingDeformer* Deformer = PreviewComponent->GetDeformer())
		{
			if (UFleshRingDeformerInstance* Instance = Deformer->GetActiveInstance())
			{
				Instance->ReleaseResources();
			}
		}
		FlushRenderingCommands();

		// 원본 메시로 복원
		SkelMeshComp->SetSkeletalMeshAsset(OriginalPreviewMesh.Get());
		SkelMeshComp->MarkRenderStateDirty();
		SkelMeshComp->MarkRenderDynamicDataDirty();
		FlushRenderingCommands();
	}

	OriginalPreviewMesh.Reset();
}

#undef LOCTEXT_NAMESPACE
