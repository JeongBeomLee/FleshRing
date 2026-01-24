// Copyright 2026 LgThx. All Rights Reserved.

#include "SFleshRingEditorViewport.h"
#include "SFleshRingEditorViewportToolbar.h"
#include "FleshRingPreviewScene.h"
#include "FleshRingEditorViewportClient.h"
#include "FleshRingEdMode.h"
#include "FleshRingAsset.h"
#include "FleshRingComponent.h"
#include "Engine/SkeletalMesh.h"
#include "RenderingThread.h"  // FlushRenderingCommands용
#include "Slate/SceneViewport.h"
#include "EditorModeRegistry.h"
#include "BufferVisualizationMenuCommands.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Styling/AppStyle.h"
#include "Viewports.h"

#define LOCTEXT_NAMESPACE "FleshRingEditorViewport"

void SFleshRingEditorViewport::Construct(const FArguments& InArgs)
{
	EditingAsset = InArgs._Asset;

	// EdMode를 글로벌 레지스트리에 등록 (아직 안 되어 있으면)
	if (!FEditorModeRegistry::Get().GetFactoryMap().Contains(FFleshRingEdMode::EM_FleshRingEdModeId))
	{
		FEditorModeRegistry::Get().RegisterMode<FFleshRingEdMode>(FFleshRingEdMode::EM_FleshRingEdModeId);
	}

	// ModeTools 생성 및 EdMode 활성화
	ModeTools = MakeShared<FEditorModeTools>();
	ModeTools->SetDefaultMode(FFleshRingEdMode::EM_FleshRingEdModeId);
	ModeTools->ActivateDefaultMode();

	// EdMode 캐싱 (static 인스턴스 사용)
	FleshRingEdMode = FFleshRingEdMode::CurrentInstance;

	// 기본 위젯 모드 설정
	ModeTools->SetWidgetMode(UE::Widget::WM_Translate);

	// 프리뷰 씬 생성
	FAdvancedPreviewScene::ConstructionValues CVS;
	CVS.bCreatePhysicsScene = false;
	CVS.LightBrightness = 3.0f;
	CVS.SkyBrightness = 1.0f;
	PreviewScene = MakeShared<FFleshRingPreviewScene>(CVS);

	// 부모 클래스 구성
	SEditorViewport::Construct(SEditorViewport::FArguments());

	// Asset 설정
	if (EditingAsset.IsValid())
	{
		SetAsset(EditingAsset.Get());
	}
}

SFleshRingEditorViewport::~SFleshRingEditorViewport()
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->Viewport = nullptr;
	}

	// ModeTools 정리
	if (ModeTools.IsValid())
	{
		ModeTools->DeactivateAllModes();
		ModeTools.Reset();
	}
}

void SFleshRingEditorViewport::SetAsset(UFleshRingAsset* InAsset)
{
	EditingAsset = InAsset;

	if (ViewportClient.IsValid())
	{
		ViewportClient->SetAsset(InAsset);
	}
}

void SFleshRingEditorViewport::RefreshPreview()
{
	if (PreviewScene.IsValid() && EditingAsset.IsValid())
	{
		// Asset 전체 갱신 (메시 + 컴포넌트 + Ring 시각화)
		PreviewScene->SetFleshRingAsset(EditingAsset.Get());
	}

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

void SFleshRingEditorViewport::UpdateRingTransformsOnly(int32 DirtyRingIndex)
{
	if (PreviewScene.IsValid())
	{
		// FleshRingComponent의 트랜스폼만 업데이트 (Deformer 유지, 깜빡임 방지)
		// DirtyRingIndex를 전달하여 해당 Ring만 처리
		UFleshRingComponent* FleshRingComp = PreviewScene->GetFleshRingComponent();
		if (FleshRingComp)
		{
			FleshRingComp->UpdateRingTransforms(DirtyRingIndex);
		}
	}

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

void SFleshRingEditorViewport::RefreshSDFOnly()
{
	if (PreviewScene.IsValid())
	{
		UFleshRingComponent* FleshRingComp = PreviewScene->GetFleshRingComponent();
		if (FleshRingComp)
		{
			// 1. SDF 재생성 (VirtualBand 파라미터 기반)
			FleshRingComp->RefreshSDF();
			FlushRenderingCommands();  // GPU 작업 완료 대기

			// 2. 트랜스폼 업데이트 + 캐시 무효화 (변형 재계산 트리거)
			FleshRingComp->UpdateRingTransforms();
		}
	}

	if (ViewportClient.IsValid())
	{
		ViewportClient->Invalidate();
	}
}

TSharedRef<SEditorViewport> SFleshRingEditorViewport::GetViewportWidget()
{
	return SharedThis(this);
}

TSharedPtr<FExtender> SFleshRingEditorViewport::GetExtenders() const
{
	TSharedPtr<FExtender> Extender = MakeShared<FExtender>();

	// 뷰 모드 메뉴에 버퍼 시각화 서브메뉴 추가
	TWeakPtr<const SFleshRingEditorViewport> WeakViewport = SharedThis(this);
	Extender->AddMenuExtension(
		TEXT("ViewMode"),
		EExtensionHook::After,
		GetCommandList(),
		FMenuExtensionDelegate::CreateLambda(
			[WeakViewport](FMenuBuilder& InMenuBuilder)
			{
				InMenuBuilder.AddSubMenu(
					LOCTEXT("VisualizeBufferViewModeDisplayName", "Buffer Visualization"),
					LOCTEXT("BufferVisualizationMenu_ToolTip", "Select a mode for buffer visualization"),
					FNewMenuDelegate::CreateStatic(&FBufferVisualizationMenuCommands::BuildVisualisationSubMenu),
					FUIAction(
						FExecuteAction(),
						FCanExecuteAction(),
						FIsActionChecked::CreateLambda(
							[WeakViewport]()
							{
								if (TSharedPtr<const SFleshRingEditorViewport> ViewportPtr = WeakViewport.Pin())
								{
									if (ViewportPtr->GetViewportClient().IsValid())
									{
										return ViewportPtr->GetViewportClient()->IsViewModeEnabled(VMI_VisualizeBuffer);
									}
								}
								return false;
							}
						)
					),
					NAME_None,  // InExtensionHook
					EUserInterfaceActionType::RadioButton,
					/* bInOpenSubMenuOnClick = */ false,
					FSlateIcon(FAppStyle::GetAppStyleSetName(), "EditorViewport.VisualizeBufferMode")
				);
			}
		)
	);

	return Extender;
}

void SFleshRingEditorViewport::OnFloatingButtonClicked()
{
	// 필요시 플로팅 버튼 클릭 처리
}

TSharedRef<FEditorViewportClient> SFleshRingEditorViewport::MakeEditorViewportClient()
{
	ViewportClient = MakeShared<FFleshRingEditorViewportClient>(
		ModeTools.Get(),
		PreviewScene.Get(),
		SharedThis(this));

	// EdMode에 ViewportClient 연결
	if (FleshRingEdMode)
	{
		FleshRingEdMode->SetViewportClient(ViewportClient.Get());
	}

	if (EditingAsset.IsValid())
	{
		ViewportClient->SetAsset(EditingAsset.Get());
	}

	return ViewportClient.ToSharedRef();
}

void SFleshRingEditorViewport::OnFocusViewportToSelection()
{
	if (ViewportClient.IsValid())
	{
		ViewportClient->FocusOnMesh();
	}
}

void SFleshRingEditorViewport::PopulateViewportOverlays(TSharedRef<SOverlay> Overlay)
{
	// 기본 오버레이 구성
	SEditorViewport::PopulateViewportOverlays(Overlay);
}

TSharedRef<SWidget> SFleshRingEditorViewport::MakeToolbar()
{
	return SNew(SFleshRingEditorViewportToolbar, SharedThis(this));
}

void SFleshRingEditorViewport::BindCommands()
{
	// 부모 클래스 커맨드 바인딩 (뷰 모드, 카메라 등)
	SEditorViewport::BindCommands();

	// 버퍼 시각화 커맨드 바인딩
	FBufferVisualizationMenuCommands::Get().BindCommands(*CommandList, Client);
}

#undef LOCTEXT_NAMESPACE
