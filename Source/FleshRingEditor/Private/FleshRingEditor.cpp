// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingEditor.h"
#include "FleshRingDeformerAssetTypeActions.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "FleshRingDetailCustomization.h"
#include "FleshRingSettingsCustomization.h"
#include "FleshRingComponent.h"
#include "PropertyEditorModule.h"

#define LOCTEXT_NAMESPACE "FFleshRingEditorModule"

void FFleshRingEditorModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	// Register asset type actions
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	FleshRingDeformerAssetTypeActions = MakeShared<FFleshRingDeformerAssetTypeActions>();
	AssetTools.RegisterAssetTypeActions(FleshRingDeformerAssetTypeActions.ToSharedRef());
	// PropertyEditor 모듈 가져오기
	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");

	// UFleshRingComponent에 대한 Detail Customization 등록
	PropertyModule.RegisterCustomClassLayout(
		UFleshRingComponent::StaticClass()->GetFName(),
		FOnGetDetailCustomizationInstance::CreateStatic(&FFleshRingDetailCustomization::MakeInstance)
	);

	// FFleshRingSettings 구조체에 대한 Property Type Customization 등록
	PropertyModule.RegisterCustomPropertyTypeLayout(
		FFleshRingSettings::StaticStruct()->GetFName(),
		FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FFleshRingSettingsCustomization::MakeInstance)
	);

	// 등록 후 Detail View 갱신
	PropertyModule.NotifyCustomizationModuleChanged();
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
	}
	// 모듈 언로드 시 등록 해제
	if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.UnregisterCustomClassLayout(UFleshRingComponent::StaticClass()->GetFName());
		PropertyModule.UnregisterCustomPropertyTypeLayout(FFleshRingSettings::StaticStruct()->GetFName());
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFleshRingEditorModule, FleshRingEditor)
