// Copyright Epic Games, Inc. All Rights Reserved.

#include "FleshRingEditor.h"
#include "FleshRingDeformerAssetTypeActions.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"

#define LOCTEXT_NAMESPACE "FFleshRingEditorModule"

void FFleshRingEditorModule::StartupModule()
{
	// Register asset type actions
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	FleshRingDeformerAssetTypeActions = MakeShared<FFleshRingDeformerAssetTypeActions>();
	AssetTools.RegisterAssetTypeActions(FleshRingDeformerAssetTypeActions.ToSharedRef());
}

void FFleshRingEditorModule::ShutdownModule()
{
	// Unregister asset type actions
	if (FModuleManager::Get().IsModuleLoaded("AssetTools"))
	{
		IAssetTools& AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get();
		if (FleshRingDeformerAssetTypeActions.IsValid())
		{
			AssetTools.UnregisterAssetTypeActions(FleshRingDeformerAssetTypeActions.ToSharedRef());
		}
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FFleshRingEditorModule, FleshRingEditor)
