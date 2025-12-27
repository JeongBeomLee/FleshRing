// Copyright Epic Games, Inc. All Rights Reserved.

#include "SFleshRingEditorViewportToolbar.h"
#include "SFleshRingEditorViewport.h"
#include "FleshRingEditorViewportClient.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Input/SCheckBox.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "EditorViewportCommands.h"

#define LOCTEXT_NAMESPACE "FleshRingEditorViewportToolbar"

void SFleshRingEditorViewportToolbar::Construct(const FArguments& InArgs, TSharedPtr<SFleshRingEditorViewport> InViewport)
{
	Viewport = InViewport;

	SCommonEditorViewportToolbarBase::Construct(SCommonEditorViewportToolbarBase::FArguments(), InViewport);
}

TSharedRef<SWidget> SFleshRingEditorViewportToolbar::GenerateShowMenu() const
{
	// 뷰포트 클라이언트 가져오기
	TSharedPtr<SFleshRingEditorViewport> ViewportPtr = Viewport.Pin();
	if (!ViewportPtr.IsValid())
	{
		return SNullWidget::NullWidget;
	}

	TSharedPtr<FFleshRingEditorViewportClient> ViewportClient = ViewportPtr->GetViewportClient();
	if (!ViewportClient.IsValid())
	{
		return SNullWidget::NullWidget;
	}

	// WeakPtr로 캡처하여 수명 문제 방지
	TWeakPtr<FFleshRingEditorViewportClient> WeakViewportClient = ViewportClient;

	FMenuBuilder MenuBuilder(true, ViewportPtr->GetCommandList());

	// FleshRing 전용 Show 옵션
	MenuBuilder.BeginSection("FleshRingShow", LOCTEXT("FleshRingShowHeader", "FleshRing"));
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("ShowRingGizmos", "Ring Gizmos"),
			LOCTEXT("ShowRingGizmosTooltip", "Show/Hide ring gizmos"),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						Client->ToggleShowRingGizmos();
					}
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						return Client->ShouldShowRingGizmos();
					}
					return false;
				})
			),
			NAME_None,
			EUserInterfaceActionType::ToggleButton
		);

		MenuBuilder.AddMenuEntry(
			LOCTEXT("ShowRingMeshes", "Ring Meshes"),
			LOCTEXT("ShowRingMeshesTooltip", "Show/Hide ring meshes"),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						Client->ToggleShowRingMeshes();
					}
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						return Client->ShouldShowRingMeshes();
					}
					return false;
				})
			),
			NAME_None,
			EUserInterfaceActionType::ToggleButton
		);

		MenuBuilder.AddMenuEntry(
			LOCTEXT("ShowBones", "Bones"),
			LOCTEXT("ShowBonesTooltip", "Show/Hide skeleton bones"),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						Client->ToggleShowBones();
					}
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						return Client->ShouldShowBones();
					}
					return false;
				})
			),
			NAME_None,
			EUserInterfaceActionType::ToggleButton
		);
	}
	MenuBuilder.EndSection();

	return MenuBuilder.MakeWidget();
}

#undef LOCTEXT_NAMESPACE
