// Copyright Epic Games, Inc. All Rights Reserved.

#include "SFleshRingEditorViewport.h"
#include "FleshRingPreviewScene.h"
#include "FleshRingEditorViewportClient.h"
#include "FleshRingAsset.h"
#include "Engine/SkeletalMesh.h"
#include "Slate/SceneViewport.h"

void SFleshRingEditorViewport::Construct(const FArguments& InArgs)
{
	EditingAsset = InArgs._Asset;

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
		// 스켈레탈 메시 갱신
		USkeletalMesh* SkelMesh = EditingAsset->TargetSkeletalMesh.LoadSynchronous();
		PreviewScene->SetSkeletalMesh(SkelMesh);

		// Ring 메시 갱신
		PreviewScene->RefreshRings(EditingAsset->Rings);
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
		PreviewScene.Get(),
		SharedThis(this));

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
