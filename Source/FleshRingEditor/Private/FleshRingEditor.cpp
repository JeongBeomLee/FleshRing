// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingEditor.h"
#include "FleshRingDeformerAssetTypeActions.h"
#include "FleshRingAssetTypeActions.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "FleshRingDetailCustomization.h"
#include "FleshRingAssetDetailCustomization.h"
#include "FleshRingSettingsCustomization.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "PropertyEditorModule.h"
#include "ToolMenus.h"
#include "SEditorViewport.h"
#include "SCommonEditorViewportToolbarBase.h"
#include "ViewportToolbar/UnrealEdViewportToolbarContext.h"
#include "FleshRingEditorViewportClient.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "ToolMenuContext.h"

#define LOCTEXT_NAMESPACE "FFleshRingEditorModule"

void FFleshRingEditorModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	// Register asset type actions
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	FleshRingDeformerAssetTypeActions = MakeShared<FFleshRingDeformerAssetTypeActions>();
	AssetTools.RegisterAssetTypeActions(FleshRingDeformerAssetTypeActions.ToSharedRef());

	// FleshRing Asset type actions 등록
	FleshRingAssetTypeActions = MakeShared<FFleshRingAssetTypeActions>();
	AssetTools.RegisterAssetTypeActions(FleshRingAssetTypeActions.ToSharedRef());

	// PropertyEditor 모듈 가져오기
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	// UFleshRingComponentDetail Customization 등록
	PropertyModule.RegisterCustomClassLayout(
		UFleshRingComponent::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FFleshRingDetailCustomization::MakeInstance)
	);

	// UFleshRingAsset Detail Customization 등록
	PropertyModule.RegisterCustomClassLayout(
		UFleshRingAsset::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FFleshRingAssetDetailCustomization::MakeInstance)
	);

	// FFleshRingSettings Property Type Customization 등록
	PropertyModule.RegisterCustomPropertyTypeLayout(
		FFleshRingSettings::StaticStruct()->GetFName(),
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FFleshRingSettingsCustomization::MakeInstance)
	);

	// 등록 Detail View 갱신
	PropertyModule.NotifyCustomizationModuleChanged();

	// ToolMenus 확장 - FleshRing 뷰포트에만 커스텀 좌표계 버튼 추가
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([]()
	{
		// Transform 서브메뉴의 TransformTools 섹션에 좌표계 버튼 추가
		UToolMenu* TransformSubmenu = UToolMenus::Get()->ExtendMenu("UnrealEd.ViewportToolbar.Transform");
		if (TransformSubmenu)
		{
			TransformSubmenu->AddDynamicSection("FleshRingCoordSystemToolbar", FNewToolMenuDelegate::CreateLambda([](UToolMenu* InMenu)
			{
				UUnrealEdViewportToolbarContext* Context = InMenu->FindContext<UUnrealEdViewportToolbarContext>();
				if (!Context)
				{
					return;
				}

				TSharedPtr<SEditorViewport> Viewport = Context->Viewport.Pin();
				if (!Viewport.IsValid())
				{
					return;
				}

				TSharedPtr<FEditorViewportClient> BaseClient = Viewport->GetViewportClient();
				if (!BaseClient.IsValid() || !FFleshRingEditorViewportClient::IsFleshRingViewportClient(BaseClient.Get()))
				{
					return;
				}

				TWeakPtr<FEditorViewportClient> WeakClient = BaseClient;

				// 기존 Transform 툴들과 동일한 ToolBarData 설정
				FToolMenuEntryToolBarData ToolBarData;
				ToolBarData.StyleNameOverride = "ViewportToolbar.TransformTools";

				// TransformTools 섹션 찾기
				FToolMenuSection& TransformToolsSection = InMenu->FindOrAddSection("TransformTools");

				// 구분선 추가
				TransformToolsSection.AddSeparator("FleshRingCoordSeparator");

				// 서브메뉴를 사용한 좌표계 버튼
				FToolMenuEntry& CoordSystemSubmenu = TransformToolsSection.AddSubMenu(
					"FleshRingCoordSystem",
					LOCTEXT("FleshRingCoordSystemLabel", "Coordinate System"),
					LOCTEXT("FleshRingCoordSystemTooltip", "Select coordinate system (Ctrl+`)"),
					FNewToolMenuDelegate::CreateLambda([WeakClient](UToolMenu* InSubmenu)
					{
						FToolMenuSection& Section = InSubmenu->FindOrAddSection(NAME_None);

						// Local 옵션
						Section.AddMenuEntry(
							"FleshRingCoordLocal",
							LOCTEXT("FleshRingCoordLocalMenu", "Local"),
							LOCTEXT("FleshRingCoordLocalTooltipMenu", "Use local coordinate system"),
							FSlateIcon(FAppStyle::GetAppStyleSetName(), "EditorViewport.RelativeCoordinateSystem_Local"),
							FUIAction(
								FExecuteAction::CreateLambda([WeakClient]()
								{
									if (TSharedPtr<FEditorViewportClient> C = WeakClient.Pin())
									{
										FFleshRingEditorViewportClient* FC = static_cast<FFleshRingEditorViewportClient*>(C.Get());
										FC->SetLocalCoordSystem(true);
									}
								}),
								FCanExecuteAction(),
								FIsActionChecked::CreateLambda([WeakClient]()
								{
									if (TSharedPtr<FEditorViewportClient> C = WeakClient.Pin())
									{
										FFleshRingEditorViewportClient* FC = static_cast<FFleshRingEditorViewportClient*>(C.Get());
										return FC->IsUsingLocalCoordSystem();
									}
									return false;
								})
							),
							EUserInterfaceActionType::RadioButton
						);

						// World 옵션
						Section.AddMenuEntry(
							"FleshRingCoordWorld",
							LOCTEXT("FleshRingCoordWorldMenu", "World"),
							LOCTEXT("FleshRingCoordWorldTooltipMenu", "Use world coordinate system"),
							FSlateIcon(FAppStyle::GetAppStyleSetName(), "EditorViewport.RelativeCoordinateSystem_World"),
							FUIAction(
								FExecuteAction::CreateLambda([WeakClient]()
								{
									if (TSharedPtr<FEditorViewportClient> C = WeakClient.Pin())
									{
										FFleshRingEditorViewportClient* FC = static_cast<FFleshRingEditorViewportClient*>(C.Get());
										FC->SetLocalCoordSystem(false);
									}
								}),
								FCanExecuteAction(),
								FIsActionChecked::CreateLambda([WeakClient]()
								{
									if (TSharedPtr<FEditorViewportClient> C = WeakClient.Pin())
									{
										FFleshRingEditorViewportClient* FC = static_cast<FFleshRingEditorViewportClient*>(C.Get());
										return !FC->IsUsingLocalCoordSystem();
									}
									return false;
								})
							),
							EUserInterfaceActionType::RadioButton
						);
					})
				);

				// 동적 아이콘 설정
				CoordSystemSubmenu.Icon = TAttribute<FSlateIcon>::CreateLambda([WeakClient]() -> FSlateIcon
				{
					if (TSharedPtr<FEditorViewportClient> C = WeakClient.Pin())
					{
						FFleshRingEditorViewportClient* FC = static_cast<FFleshRingEditorViewportClient*>(C.Get());
						if (FC->IsUsingLocalCoordSystem())
						{
							return FSlateIcon(FAppStyle::GetAppStyleSetName(), "EditorViewport.RelativeCoordinateSystem_Local");
						}
					}
					return FSlateIcon(FAppStyle::GetAppStyleSetName(), "EditorViewport.RelativeCoordinateSystem_World");
				});

				// 메인 버튼 클릭 시 토글 액션
				FToolUIAction ToggleAction;
				ToggleAction.ExecuteAction = FToolMenuExecuteAction::CreateLambda([WeakClient](const FToolMenuContext&)
				{
					if (TSharedPtr<FEditorViewportClient> C = WeakClient.Pin())
					{
						FFleshRingEditorViewportClient* FC = static_cast<FFleshRingEditorViewportClient*>(C.Get());
						FC->ToggleLocalCoordSystem();
					}
				});

				// 기존 Transform 툴들과 동일한 스타일 적용
				CoordSystemSubmenu.ToolBarData = ToolBarData;
				CoordSystemSubmenu.ToolBarData.LabelOverride = FText::GetEmpty();
				CoordSystemSubmenu.ToolBarData.ActionOverride = ToggleAction;
				CoordSystemSubmenu.SetShowInToolbarTopLevel(true);
			}));
		}

		// Transform 메뉴에서 FleshRing 뷰포트일 때 기본 좌표계 컨트롤 숨기기
		UToolMenu* TransformMenu = UToolMenus::Get()->ExtendMenu("UnrealEd.ViewportToolbar.Transform");
		if (TransformMenu)
		{
			TransformMenu->AddDynamicSection("FleshRingHideDefaultCoordSystem", FNewToolMenuDelegate::CreateLambda([](UToolMenu* InMenu)
			{
				UUnrealEdViewportToolbarContext* Context = InMenu->FindContext<UUnrealEdViewportToolbarContext>();
				if (!Context)
				{
					return;
				}

				TSharedPtr<SEditorViewport> Viewport = Context->Viewport.Pin();
				if (!Viewport.IsValid())
				{
					return;
				}

				TSharedPtr<FEditorViewportClient> BaseClient = Viewport->GetViewportClient();
				if (!BaseClient.IsValid() || !FFleshRingEditorViewportClient::IsFleshRingViewportClient(BaseClient.Get()))
				{
					return;
				}

				// 기본 좌표계 컨트롤 숨기기 (우리 커스텀 버튼 사용)
				Context->bShowCoordinateSystemControls = false;
			}));
		}
	}));
} 

void FFleshRingEditorModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
	// Unregister asset type actions
	if (FModuleManager::Get().IsModuleLoaded("AssetTools"))
	{
		IAssetTools& AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get();
		if (FleshRingDeformerAssetTypeActions.IsValid())
		{
			AssetTools.UnregisterAssetTypeActions(FleshRingDeformerAssetTypeActions.ToSharedRef());
		}
		if (FleshRingAssetTypeActions.IsValid())
		{
			AssetTools.UnregisterAssetTypeActions(FleshRingAssetTypeActions.ToSharedRef());
		}
	}

	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.UnregisterCustomClassLayout(UFleshRingComponent::StaticClass()->GetFName());
		PropertyModule.UnregisterCustomClassLayout(UFleshRingAsset::StaticClass()->GetFName());
		PropertyModule.UnregisterCustomPropertyTypeLayout(FFleshRingSettings::StaticStruct()->GetFName());
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFleshRingEditorModule, FleshRingEditor)
