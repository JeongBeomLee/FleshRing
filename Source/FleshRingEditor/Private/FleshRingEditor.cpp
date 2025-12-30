// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingEditor.h"
#include "FleshRingDeformerAssetTypeActions.h"
#include "FleshRingAssetTypeActions.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "FleshRingDetailCustomization.h"
#include "FleshRingSettingsCustomization.h"
#include "FleshRingComponent.h"
#include "PropertyEditorModule.h"
#include "ToolMenus.h"
#include "SEditorViewport.h"
#include "SCommonEditorViewportToolbarBase.h"
#include "ViewportToolbar/UnrealEdViewportToolbarContext.h"
#include "FleshRingEditorViewportClient.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"

#define LOCTEXT_NAMESPACE "FFleshRingEditorModule"

void FFleshRingEditorModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	// Register asset type actions
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	FleshRingDeformerAssetTypeActions = MakeShared<FFleshRingDeformerAssetTypeActions>();
	AssetTools.RegisterAssetTypeActions(FleshRingDeformerAssetTypeActions.ToSharedRef());

	// FleshRing Asset type actions 깅줉
	FleshRingAssetTypeActions = MakeShared<FFleshRingAssetTypeActions>();
	AssetTools.RegisterAssetTypeActions(FleshRingAssetTypeActions.ToSharedRef());

	// PropertyEditor 紐⑤뱢 媛몄삤湲
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	// UFleshRingComponentDetail Customization 깅줉
	PropertyModule.RegisterCustomClassLayout(
		UFleshRingComponent::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FFleshRingDetailCustomization::MakeInstance)
	);

	// FFleshRingSettings 援ъ“泥댁뿉 Property Type Customization 깅줉
	PropertyModule.RegisterCustomPropertyTypeLayout(
		FFleshRingSettings::StaticStruct()->GetFName(),
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FFleshRingSettingsCustomization::MakeInstance)
	);

	// 등록 Detail View 갱신
	PropertyModule.NotifyCustomizationModuleChanged();

	// ToolMenus 확장 - FleshRing 뷰포트에만 커스텀 좌표계 버튼 추가
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([]()
	{
		// 툴바 자체에 버튼 추가 (드롭다운 메뉴가 아닌 상단 바)
		UToolMenu* ViewportToolbar = UToolMenus::Get()->ExtendMenu("UnrealEd.ViewportToolbar");
		if (ViewportToolbar)
		{
			FToolMenuSection& Section = ViewportToolbar->FindOrAddSection("FleshRingCoordSystem");
			Section.InsertPosition = FToolMenuInsert(NAME_None, EToolMenuInsertType::First);
			Section.AddDynamicEntry("FleshRingCoordSystemButton", FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
			{
				// Context에서 뷰포트 확인
				if (UCommonViewportToolbarBaseMenuContext* Context = InSection.FindContext<UCommonViewportToolbarBaseMenuContext>())
				{
					TSharedPtr<SEditorViewport> Viewport = Context->Viewport.Pin();
					if (!Viewport.IsValid())
					{
						return;
					}

					TSharedPtr<FEditorViewportClient> BaseClient = Viewport->GetViewportClient();
					if (!BaseClient.IsValid())
					{
						return;
					}

					// 정적 인스턴스 레지스트리로 FleshRing ViewportClient인지 확인 (크래시 방지)
					if (!FFleshRingEditorViewportClient::IsFleshRingViewportClient(BaseClient.Get()))
					{
						return;
					}

					TWeakPtr<FEditorViewportClient> WeakClient = BaseClient;

					InSection.AddEntry(FToolMenuEntry::InitWidget(
						"FleshRingCoordSystem",
						SNew(SButton)
						.ButtonStyle(FAppStyle::Get(), "EditorViewportToolBar.Button")
						.ToolTipText(LOCTEXT("FleshRingCoordTooltip", "Toggle Local/World (Ctrl+`)"))
						.OnClicked_Lambda([WeakClient]() -> FReply
						{
							if (TSharedPtr<FEditorViewportClient> C = WeakClient.Pin())
							{
								FFleshRingEditorViewportClient* FC = static_cast<FFleshRingEditorViewportClient*>(C.Get());
								FC->ToggleLocalCoordSystem();
							}
							return FReply::Handled();
						})
						.Content()
						[
							SNew(SImage)
							.Image_Lambda([WeakClient]() -> const FSlateBrush*
							{
								if (TSharedPtr<FEditorViewportClient> C = WeakClient.Pin())
								{
									FFleshRingEditorViewportClient* FC = static_cast<FFleshRingEditorViewportClient*>(C.Get());
									if (FC->IsUsingLocalCoordSystem())
									{
										return FAppStyle::GetBrush("EditorViewport.RelativeCoordinateSystem_Local");
									}
								}
								return FAppStyle::GetBrush("EditorViewport.RelativeCoordinateSystem_World");
							})
						],
						FText::GetEmpty(),
						true // bNoIndent
					));
				}
			}));
		}

		// Transform 메뉴에서 FleshRing 뷰포트일 때 좌표계 컨트롤 숨기기
		UToolMenu* TransformMenu = UToolMenus::Get()->ExtendMenu("UnrealEd.ViewportToolbar.Transform");
		if (TransformMenu)
		{
			TransformMenu->AddDynamicSection("FleshRingHideCoordSystem", FNewToolMenuDelegate::CreateLambda([](UToolMenu* InMenu)
			{
				// UUnrealEdViewportToolbarContext를 찾아서 bShowCoordinateSystemControls를 false로 설정
				if (UUnrealEdViewportToolbarContext* Context = InMenu->FindContext<UUnrealEdViewportToolbarContext>())
				{
					TSharedPtr<SEditorViewport> Viewport = Context->Viewport.Pin();
					if (Viewport.IsValid())
					{
						TSharedPtr<FEditorViewportClient> BaseClient = Viewport->GetViewportClient();
						if (BaseClient.IsValid() && FFleshRingEditorViewportClient::IsFleshRingViewportClient(BaseClient.Get()))
						{
							// 좌표계 컨트롤 숨기기
							Context->bShowCoordinateSystemControls = false;
						}
					}
				}
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
	// 紐⑤뱢 몃줈깅줉 댁젣
	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.UnregisterCustomClassLayout(UFleshRingComponent::StaticClass()->GetFName());
		PropertyModule.UnregisterCustomPropertyTypeLayout(FFleshRingSettings::StaticStruct()->GetFName());
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFleshRingEditorModule, FleshRingEditor)
