// Copyright Epic Games, Inc. All Rights Reserved.

#include "SFleshRingEditorViewportToolbar.h"
#include "SFleshRingEditorViewport.h"
#include "FleshRingEditorViewportClient.h"
#include "FleshRingPreviewScene.h"
#include "FleshRingComponent.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Text/STextBlock.h"

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

		// 본 그리기 서브메뉴 (Bones 체크박스 제거, BoneDrawMode로 통합)
		MenuBuilder.AddSubMenu(
			LOCTEXT("BoneDrawing", "Bone Drawing"),
			LOCTEXT("BoneDrawingTooltip", "Bone drawing options"),
			FNewMenuDelegate::CreateLambda([WeakViewportClient](FMenuBuilder& SubMenuBuilder)
			{
				// 본 그리기 모드 라디오 버튼들 (먼저 배치)
				SubMenuBuilder.AddMenuEntry(
					LOCTEXT("BoneDrawModeAll", "All Hierarchy"),
					LOCTEXT("BoneDrawModeAllTooltip", "Draw all bones"),
					FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								Client->SetBoneDrawMode(EFleshRingBoneDrawMode::All);
							}
						}),
						FCanExecuteAction(),
						FIsActionChecked::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								return Client->IsBoneDrawModeSet(EFleshRingBoneDrawMode::All);
							}
							return false;
						})
					),
					NAME_None,
					EUserInterfaceActionType::RadioButton
				);

				SubMenuBuilder.AddMenuEntry(
					LOCTEXT("BoneDrawModeSelected", "Selected Only"),
					LOCTEXT("BoneDrawModeSelectedTooltip", "Draw only selected bone"),
					FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								Client->SetBoneDrawMode(EFleshRingBoneDrawMode::Selected);
							}
						}),
						FCanExecuteAction(),
						FIsActionChecked::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								return Client->IsBoneDrawModeSet(EFleshRingBoneDrawMode::Selected);
							}
							return false;
						})
					),
					NAME_None,
					EUserInterfaceActionType::RadioButton
				);

				SubMenuBuilder.AddMenuEntry(
					LOCTEXT("BoneDrawModeSelectedAndParents", "Selected and Parents"),
					LOCTEXT("BoneDrawModeSelectedAndParentsTooltip", "Draw selected bone and its parent bones"),
					FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								Client->SetBoneDrawMode(EFleshRingBoneDrawMode::SelectedAndParents);
							}
						}),
						FCanExecuteAction(),
						FIsActionChecked::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								return Client->IsBoneDrawModeSet(EFleshRingBoneDrawMode::SelectedAndParents);
							}
							return false;
						})
					),
					NAME_None,
					EUserInterfaceActionType::RadioButton
				);

				SubMenuBuilder.AddMenuEntry(
					LOCTEXT("BoneDrawModeSelectedAndChildren", "Selected and Children"),
					LOCTEXT("BoneDrawModeSelectedAndChildrenTooltip", "Draw selected bone and its child bones"),
					FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								Client->SetBoneDrawMode(EFleshRingBoneDrawMode::SelectedAndChildren);
							}
						}),
						FCanExecuteAction(),
						FIsActionChecked::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								return Client->IsBoneDrawModeSet(EFleshRingBoneDrawMode::SelectedAndChildren);
							}
							return false;
						})
					),
					NAME_None,
					EUserInterfaceActionType::RadioButton
				);

				SubMenuBuilder.AddMenuEntry(
					LOCTEXT("BoneDrawModeSelectedAndParentsAndChildren", "Selected, Parents, and Children"),
					LOCTEXT("BoneDrawModeSelectedAndParentsAndChildrenTooltip", "Draw selected bone with all parent and child bones"),
					FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								Client->SetBoneDrawMode(EFleshRingBoneDrawMode::SelectedAndParentsAndChildren);
							}
						}),
						FCanExecuteAction(),
						FIsActionChecked::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								return Client->IsBoneDrawModeSet(EFleshRingBoneDrawMode::SelectedAndParentsAndChildren);
							}
							return false;
						})
					),
					NAME_None,
					EUserInterfaceActionType::RadioButton
				);

				SubMenuBuilder.AddMenuEntry(
					LOCTEXT("BoneDrawModeNone", "None"),
					LOCTEXT("BoneDrawModeNoneTooltip", "Hide all bones"),
					FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								Client->SetBoneDrawMode(EFleshRingBoneDrawMode::None);
							}
						}),
						FCanExecuteAction(),
						FIsActionChecked::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								return Client->IsBoneDrawModeSet(EFleshRingBoneDrawMode::None);
							}
							return false;
						})
					),
					NAME_None,
					EUserInterfaceActionType::RadioButton
				);

				SubMenuBuilder.AddSeparator();

				// 본 이름 표시
				SubMenuBuilder.AddMenuEntry(
					LOCTEXT("ShowBoneNames", "Bone Names"),
					LOCTEXT("ShowBoneNamesTooltip", "Show/Hide bone names"),
					FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								Client->ToggleShowBoneNames();
							}
						}),
						FCanExecuteAction::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								return !Client->IsBoneDrawModeSet(EFleshRingBoneDrawMode::None);
							}
							return false;
						}),
						FIsActionChecked::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								return Client->ShouldShowBoneNames();
							}
							return false;
						})
					),
					NAME_None,
					EUserInterfaceActionType::ToggleButton
				);

				// 다중 컬러 본
				SubMenuBuilder.AddMenuEntry(
					LOCTEXT("ShowMultiColorBones", "Multi-Color Bones"),
					LOCTEXT("ShowMultiColorBonesTooltip", "Show bones with multiple colors based on hierarchy"),
					FSlateIcon(),
					FUIAction(
						FExecuteAction::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								Client->ToggleShowMultiColorBones();
							}
						}),
						FCanExecuteAction::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								return !Client->IsBoneDrawModeSet(EFleshRingBoneDrawMode::None);
							}
							return false;
						}),
						FIsActionChecked::CreateLambda([WeakViewportClient]()
						{
							if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
							{
								return Client->ShouldShowMultiColorBones();
							}
							return false;
						})
					),
					NAME_None,
					EUserInterfaceActionType::ToggleButton
				);

				SubMenuBuilder.AddSeparator();

				// 본 그리기 크기 슬라이더
				SubMenuBuilder.AddWidget(
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(FMargin(4.0f, 0.0f))
					[
						SNew(STextBlock)
						.Text(LOCTEXT("BoneDrawSize", "Bone Size"))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(FMargin(4.0f, 0.0f))
					[
						SNew(SBox)
						.WidthOverride(80.0f)
						[
							SNew(SSpinBox<float>)
							.MinValue(0.1f)
							.MaxValue(5.0f)
							.MinSliderValue(0.1f)
							.MaxSliderValue(5.0f)
							.Delta(0.1f)
							.Value_Lambda([WeakViewportClient]()
							{
								if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
								{
									return Client->GetBoneDrawSize();
								}
								return 1.0f;
							})
							.OnValueChanged_Lambda([WeakViewportClient](float NewValue)
							{
								if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
								{
									Client->SetBoneDrawSize(NewValue);
								}
							})
							.IsEnabled_Lambda([WeakViewportClient]()
							{
								if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
								{
									return !Client->IsBoneDrawModeSet(EFleshRingBoneDrawMode::None);
								}
								return false;
							})
						]
					],
					FText::GetEmpty()
				);
			})
		);
	}
	MenuBuilder.EndSection();

	// Debug / Visualization 섹션
	MenuBuilder.BeginSection("DebugVisualization", LOCTEXT("DebugVisualizationHeader", "Debug / Visualization"));
	{
		// Show Debug Visualization (마스터 스위치)
		MenuBuilder.AddMenuEntry(
			LOCTEXT("ShowDebugVisualization", "Show Debug Visualization"),
			LOCTEXT("ShowDebugVisualizationTooltip", "Enable/Disable all debug visualization"),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						Client->ToggleShowDebugVisualization();
					}
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						return Client->ShouldShowDebugVisualization();
					}
					return false;
				})
			),
			NAME_None,
			EUserInterfaceActionType::ToggleButton
		);

		MenuBuilder.AddSeparator();

		// Show SDF Volume
		MenuBuilder.AddMenuEntry(
			LOCTEXT("ShowSdfVolume", "Show SDF Volume"),
			LOCTEXT("ShowSdfVolumeTooltip", "Show/Hide SDF volume bounding box"),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						Client->ToggleShowSdfVolume();
					}
				}),
				FCanExecuteAction::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						return Client->ShouldShowDebugVisualization();
					}
					return false;
				}),
				FIsActionChecked::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						return Client->ShouldShowSdfVolume();
					}
					return false;
				})
			),
			NAME_None,
			EUserInterfaceActionType::ToggleButton
		);

		// Show Affected Vertices
		MenuBuilder.AddMenuEntry(
			LOCTEXT("ShowAffectedVertices", "Show Affected Vertices"),
			LOCTEXT("ShowAffectedVerticesTooltip", "Show/Hide affected vertices (color = influence strength)"),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						Client->ToggleShowAffectedVertices();
					}
				}),
				FCanExecuteAction::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						return Client->ShouldShowDebugVisualization();
					}
					return false;
				}),
				FIsActionChecked::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						return Client->ShouldShowAffectedVertices();
					}
					return false;
				})
			),
			NAME_None,
			EUserInterfaceActionType::ToggleButton
		);

		// Show SDF Slice
		MenuBuilder.AddMenuEntry(
			LOCTEXT("ShowSDFSlice", "Show SDF Slice"),
			LOCTEXT("ShowSDFSliceTooltip", "Show/Hide SDF slice plane"),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						Client->ToggleShowSDFSlice();
					}
				}),
				FCanExecuteAction::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						return Client->ShouldShowDebugVisualization();
					}
					return false;
				}),
				FIsActionChecked::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						return Client->ShouldShowSDFSlice();
					}
					return false;
				})
			),
			NAME_None,
			EUserInterfaceActionType::ToggleButton
		);

		// Debug Slice Z (SpinBox 위젯)
		MenuBuilder.AddWidget(
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(4.0f, 0.0f))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("DebugSliceZ", "Debug Slice Z"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(4.0f, 0.0f))
			[
				SNew(SBox)
				.WidthOverride(60.0f)
				[
					SNew(SSpinBox<int32>)
					.MinValue(0)
					.MaxValue(63)
					.Value_Lambda([WeakViewportClient]()
					{
						if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
						{
							return Client->GetDebugSliceZ();
						}
						return 32;
					})
					.OnValueChanged_Lambda([WeakViewportClient](int32 NewValue)
					{
						if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
						{
							Client->SetDebugSliceZ(NewValue);
						}
					})
					.IsEnabled_Lambda([WeakViewportClient]()
					{
						if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
						{
							return Client->ShouldShowDebugVisualization() && Client->ShouldShowSDFSlice();
						}
						return false;
					})
				]
			],
			FText::GetEmpty()
		);

		MenuBuilder.AddSeparator();

		// Show Bulge Heatmap
		MenuBuilder.AddMenuEntry(
			LOCTEXT("ShowBulgeHeatmap", "Show Bulge Heatmap"),
			LOCTEXT("ShowBulgeHeatmapTooltip", "Show/Hide bulge heatmap visualization"),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						Client->ToggleShowBulgeHeatmap();
					}
				}),
				FCanExecuteAction::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						return Client->ShouldShowDebugVisualization();
					}
					return false;
				}),
				FIsActionChecked::CreateLambda([WeakViewportClient]()
				{
					if (TSharedPtr<FFleshRingEditorViewportClient> Client = WeakViewportClient.Pin())
					{
						return Client->ShouldShowBulgeHeatmap();
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
