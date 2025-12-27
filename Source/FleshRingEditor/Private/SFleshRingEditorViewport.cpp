// Copyright Epic Games, Inc. All Rights Reserved.

#include "SFleshRingEditorViewport.h"
#include "SFleshRingEditorViewportToolbar.h"
#include "FleshRingPreviewScene.h"
#include "FleshRingEditorViewportClient.h"
#include "FleshRingEdMode.h"
#include "FleshRingAsset.h"
#include "Engine/SkeletalMesh.h"
#include "Slate/SceneViewport.h"
#include "EditorModeRegistry.h"
#include "Styling/AppStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"

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

TSharedRef<SEditorViewport> SFleshRingEditorViewport::GetViewportWidget()
{
	return SharedThis(this);
}

TSharedPtr<FExtender> SFleshRingEditorViewport::GetExtenders() const
{
	return MakeShared<FExtender>();
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
	// 오버레이에 툴바 추가하지 않음 - Asset Editor에서 별도로 배치
	SEditorViewport::PopulateViewportOverlays(Overlay);
}

TSharedRef<SWidget> SFleshRingEditorViewport::MakeToolbar()
{
	return SNew(SFleshRingEditorViewportToolbar, SharedThis(this));
}
