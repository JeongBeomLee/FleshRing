// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class IAssetTypeActions;
class FSlateStyleSet;

class FFleshRingEditorModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** FleshRing 스타일 셋 이름 */
	static FName GetStyleSetName() { return TEXT("FleshRingStyle"); }

private:
	/** 커스텀 스타일 셋 */
	TSharedPtr<FSlateStyleSet> StyleSet;

	TSharedPtr<IAssetTypeActions> FleshRingDeformerAssetTypeActions;
	TSharedPtr<IAssetTypeActions> FleshRingAssetTypeActions;
};
