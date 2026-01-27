// Copyright 2026 LgThx. All Rights Reserved.

#include "FleshRingEditor.h"
#include "FleshRingAssetTypeActions.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "FleshRingDetailCustomization.h"
#include "FleshRingAssetDetailCustomization.h"
#include "FleshRingSettingsCustomization.h"
#include "FSubdivisionSettingsCustomization.h"
#include "FMaterialLayerMappingCustomization.h"
#include "FleshRingComponent.h"
#include "FleshRingAsset.h"
#include "PropertyEditorModule.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "FleshRingAssetThumbnailRenderer.h"
#include "ToolMenus.h"
#include "SEditorViewport.h"
#include "SCommonEditorViewportToolbarBase.h"
#include "ViewportToolbar/UnrealEdViewportToolbarContext.h"
#include "FleshRingEditorViewportClient.h"
#include "Styling/AppStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "ToolMenuContext.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FFleshRingEditorModule"

void FFleshRingEditorModule::StartupModule()
{
	// Register custom style set
	StyleSet = MakeShareable(new FSlateStyleSet(GetStyleSetName()));
	StyleSet->SetContentRoot(FPaths::EngineContentDir() / TEXT("Editor/Slate"));
	StyleSet->Set("FleshRing.RingIcon", new FSlateVectorImageBrush(
		StyleSet->RootToContentDir(TEXT("Icons/AssetIcons/ModelingTorus_16"), TEXT(".svg")),
		FVector2D(16.0f, 16.0f)));

	// Register FleshRingAsset class icon
	const FString PluginResourcesPath = FPaths::ProjectPluginsDir() / TEXT("FleshRingPlugin/Resources");
	const FString ClassIconPath = PluginResourcesPath / TEXT("FleshRingAssetIcon.png");
	StyleSet->Set("ClassIcon.FleshRingAsset", new FSlateImageBrush(ClassIconPath, FVector2D(16.0f, 16.0f)));

	// Register FleshRingComponent class icon
	const FString ComponentIconPath = PluginResourcesPath / TEXT("FleshRingAssetComponentIcon.png");
	StyleSet->Set("ClassIcon.FleshRingComponent", new FSlateImageBrush(ComponentIconPath, FVector2D(16.0f, 16.0f)));
	StyleSet->Set("ClassThumbnail.FleshRingComponent", new FSlateImageBrush(ComponentIconPath, FVector2D(64.0f, 64.0f)));

	FSlateStyleRegistry::RegisterSlateStyle(*StyleSet);

	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	// Register asset type actions
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();

	// Register FleshRing Asset type actions
	FleshRingAssetTypeActions = MakeShared<FFleshRingAssetTypeActions>();
	AssetTools.RegisterAssetTypeActions(FleshRingAssetTypeActions.ToSharedRef());

	// Get PropertyEditor module
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	// Register UFleshRingComponentDetail Customization
	PropertyModule.RegisterCustomClassLayout(
		UFleshRingComponent::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FFleshRingDetailCustomization::MakeInstance)
	);

	// Register UFleshRingAsset Detail Customization
	PropertyModule.RegisterCustomClassLayout(
		UFleshRingAsset::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FFleshRingAssetDetailCustomization::MakeInstance)
	);

	// Register FFleshRingSettings Property Type Customization
	PropertyModule.RegisterCustomPropertyTypeLayout(
		FFleshRingSettings::StaticStruct()->GetFName(),
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FFleshRingSettingsCustomization::MakeInstance)
	);

	// Register FSubdivisionSettings Property Type Customization
	PropertyModule.RegisterCustomPropertyTypeLayout(
		FSubdivisionSettings::StaticStruct()->GetFName(),
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FSubdivisionSettingsCustomization::MakeInstance)
	);

	// Register FMaterialLayerMapping Property Type Customization (display material thumbnail)
	PropertyModule.RegisterCustomPropertyTypeLayout(
		FMaterialLayerMapping::StaticStruct()->GetFName(),
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FMaterialLayerMappingCustomization::MakeInstance)
	);

	// Refresh registered Detail View
	PropertyModule.NotifyCustomizationModuleChanged();

	// Register FleshRingAsset thumbnail renderer
	UThumbnailManager::Get().RegisterCustomRenderer(UFleshRingAsset::StaticClass(), UFleshRingAssetThumbnailRenderer::StaticClass());

	// ToolMenus extension - Add custom coordinate system button only to FleshRing viewport
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([]()
	{
		// Add coordinate system button to TransformTools section of Transform submenu
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

				// Set ToolBarData same as existing Transform tools
				FToolMenuEntryToolBarData ToolBarData;
				ToolBarData.StyleNameOverride = "ViewportToolbar.TransformTools";

				// Find TransformTools section
				FToolMenuSection& TransformToolsSection = InMenu->FindOrAddSection("TransformTools");

				// Add separator
				TransformToolsSection.AddSeparator("FleshRingCoordSeparator");

				// Coordinate system button using submenu
				FToolMenuEntry& CoordSystemSubmenu = TransformToolsSection.AddSubMenu(
					"FleshRingCoordSystem",
					LOCTEXT("FleshRingCoordSystemLabel", "Coordinate System"),
					LOCTEXT("FleshRingCoordSystemTooltip", "Select coordinate system (Ctrl+`)"),
					FNewToolMenuDelegate::CreateLambda([WeakClient](UToolMenu* InSubmenu)
					{
						FToolMenuSection& Section = InSubmenu->FindOrAddSection(NAME_None);

						// Local option
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

						// World option
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

				// Dynamic icon setup
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

				// Toggle action when main button is clicked
				FToolUIAction ToggleAction;
				ToggleAction.ExecuteAction = FToolMenuExecuteAction::CreateLambda([WeakClient](const FToolMenuContext&)
				{
					if (TSharedPtr<FEditorViewportClient> C = WeakClient.Pin())
					{
						FFleshRingEditorViewportClient* FC = static_cast<FFleshRingEditorViewportClient*>(C.Get());
						FC->ToggleLocalCoordSystem();
					}
				});

				// Apply same style as existing Transform tools
				CoordSystemSubmenu.ToolBarData = ToolBarData;
				CoordSystemSubmenu.ToolBarData.LabelOverride = FText::GetEmpty();
				CoordSystemSubmenu.ToolBarData.ActionOverride = ToggleAction;
				CoordSystemSubmenu.SetShowInToolbarTopLevel(true);
			}));
		}

		// Hide default coordinate system controls when in FleshRing viewport from Transform menu
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

				// Hide default coordinate system controls (use our custom button)
				Context->bShowCoordinateSystemControls = false;
			}));
		}
	}));
}

void FFleshRingEditorModule::ShutdownModule()
{
	// Unregister custom style set
	if (StyleSet.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);
		StyleSet.Reset();
	}

	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
	// Unregister asset type actions
	if (FModuleManager::Get().IsModuleLoaded("AssetTools"))
	{
		IAssetTools& AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get();
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
		PropertyModule.UnregisterCustomPropertyTypeLayout(FSubdivisionSettings::StaticStruct()->GetFName());
	}

	// Unregister FleshRingAsset thumbnail renderer
	if (UObjectInitialized())
	{
		UThumbnailManager::Get().UnregisterCustomRenderer(UFleshRingAsset::StaticClass());
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFleshRingEditorModule, FleshRingEditor)
